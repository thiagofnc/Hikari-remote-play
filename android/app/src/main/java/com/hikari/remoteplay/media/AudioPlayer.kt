package com.hikari.remoteplay.media

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.media.MediaCodec
import android.media.MediaFormat
import android.util.Log
import com.hikari.remoteplay.proto.AudioCodec
import com.hikari.remoteplay.proto.AudioPacket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ArrayBlockingQueue
import java.util.concurrent.TimeUnit

// Low-latency playback of the host's 48 kHz stereo stream (PCM s16 or Opus,
// one 5 ms frame per packet). Latency is bounded by dropping packets once the
// track holds more than MAX_BUFFER_MS, instead of letting a backlog build.
class AudioPlayer {
    private val queue = ArrayBlockingQueue<AudioPacket>(64)
    private var thread: Thread? = null
    @Volatile private var running = false
    @Volatile var bufferedMs = 0
        private set
    @Volatile var muted = false

    private var track: AudioTrack? = null
    private var opus: MediaCodec? = null
    private var framesWritten = 0L
    private var started = false

    fun push(p: AudioPacket) {
        if (!running) return
        if (!queue.offer(p)) {
            queue.poll()
            queue.offer(p)
        }
    }

    fun start() {
        if (running) return
        running = true
        thread = Thread(::loop, "hikari-audio").apply { start() }
    }

    fun stop() {
        running = false
        thread?.interrupt()
        thread?.join(500)
        thread = null
        queue.clear()
    }

    private fun openTrack(): AudioTrack {
        val min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        val bytes = maxOf(min, RATE * 4 * MAX_BUFFER_MS / 1000 * 2)
        val t = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MOVIE)
                    .build()
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build()
            )
            .setBufferSizeInBytes(bytes)
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build()
        framesWritten = 0
        started = false
        return t
    }

    private fun openOpus(): MediaCodec? = try {
        // OpusHead (RFC 7845) + pre-skip and seek pre-roll in nanoseconds.
        val head = ByteBuffer.allocate(19).order(ByteOrder.LITTLE_ENDIAN)
        head.put("OpusHead".toByteArray(Charsets.US_ASCII))
        head.put(1.toByte())          // version
        head.put(2.toByte())          // channels
        head.putShort(0.toShort())    // pre-skip
        head.putInt(RATE)
        head.putShort(0.toShort())    // output gain
        head.put(0.toByte())          // mapping family
        head.flip()
        fun ns(v: Long) = ByteBuffer.allocate(8).order(ByteOrder.nativeOrder()).apply { putLong(v); flip() }
        val fmt = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_OPUS, RATE, 2)
        fmt.setByteBuffer("csd-0", head)
        fmt.setByteBuffer("csd-1", ns(0))
        fmt.setByteBuffer("csd-2", ns(80_000_000))
        MediaCodec.createDecoderByType(MediaFormat.MIMETYPE_AUDIO_OPUS).apply {
            configure(fmt, null, null, 0)
            start()
        }
    } catch (e: Exception) {
        Log.w(TAG, "Opus decoder unavailable", e)
        null
    }

    private fun write(t: AudioTrack, pcm: ByteArray, off: Int, len: Int) {
        val buffered = framesWritten - (t.playbackHeadPosition.toLong() and 0xFFFFFFFFL)
        bufferedMs = (buffered * 1000 / RATE).toInt().coerceAtLeast(0)
        if (bufferedMs > MAX_BUFFER_MS) return  // drift or a burst: drop to stay live
        if (muted) return
        val n = t.write(pcm, off, len, AudioTrack.WRITE_NON_BLOCKING)
        if (n > 0) framesWritten += n / 4
        // Build a small cushion once, then play.
        if (!started && framesWritten * 1000 / RATE >= START_CUSHION_MS) {
            t.play()
            started = true
        }
    }

    private fun loop() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
        val t = openTrack()
        track = t
        val info = MediaCodec.BufferInfo()
        var pcm = ByteArray(4096)
        try {
            while (running) {
                val p = try {
                    queue.poll(50, TimeUnit.MILLISECONDS)
                } catch (e: InterruptedException) {
                    null
                } ?: continue
                when (p.codec) {
                    AudioCodec.PCM_S16 -> write(t, p.payload, 0, p.payload.size)
                    AudioCodec.OPUS -> {
                        val c = opus ?: openOpus().also { opus = it } ?: continue
                        val inIdx = c.dequeueInputBuffer(5_000)
                        if (inIdx >= 0) {
                            c.getInputBuffer(inIdx)?.apply {
                                clear()
                                put(p.payload)
                            }
                            c.queueInputBuffer(inIdx, 0, p.payload.size, p.captureUs, 0)
                        }
                        while (true) {
                            val outIdx = c.dequeueOutputBuffer(info, 0)
                            if (outIdx < 0) break
                            val ob = c.getOutputBuffer(outIdx)
                            if (ob != null && info.size > 0) {
                                if (pcm.size < info.size) pcm = ByteArray(info.size)
                                ob.position(info.offset)
                                ob.get(pcm, 0, info.size)
                                write(t, pcm, 0, info.size)
                            }
                            c.releaseOutputBuffer(outIdx, false)
                        }
                    }
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "audio", e)
        } finally {
            runCatching { t.stop() }
            t.release()
            track = null
            opus?.let {
                runCatching { it.stop() }
                it.release()
            }
            opus = null
            bufferedMs = 0
        }
    }

    companion object {
        private const val TAG = "HikariAudio"
        private const val RATE = 48000
        private const val START_CUSHION_MS = 30
        private const val MAX_BUFFER_MS = 90

        val opusSupported: Boolean by lazy { VideoDecoder.hasDecoder(MediaFormat.MIMETYPE_AUDIO_OPUS) }
    }
}
