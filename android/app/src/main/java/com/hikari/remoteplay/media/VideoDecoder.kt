package com.hikari.remoteplay.media

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import com.hikari.remoteplay.net.AssembledFrame
import com.hikari.remoteplay.net.HikariClient
import com.hikari.remoteplay.net.nowUs
import com.hikari.remoteplay.proto.Proto
import com.hikari.remoteplay.proto.VideoCodec
import java.nio.ByteBuffer
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit

// Hardware decode straight to the SurfaceView's surface. Frames are decoded in
// arrival order and every output is released for rendering immediately: the
// newest picture always wins, nothing waits for a vsync-aligned schedule.
class VideoDecoder(private val net: HikariClient) {

    data class Stats(val decodeP50Ms: Double = 0.0, val decodeP95Ms: Double = 0.0, val presentFps: Double = 0.0, val endToEndMs: Double = 0.0, val decoderName: String = "")

    @Volatile private var surface: Surface? = null
    @Volatile var width = 1920
    @Volatile var height = 1080
    @Volatile var codecType = VideoCodec.H264
    @Volatile private var reconfigure = true

    private var codec: MediaCodec? = null
    private var decoderName = ""
    private val freeInputs = LinkedBlockingQueue<Int>()
    @Volatile private var failed = false
    private var needKeyframe = true

    private var thread: Thread? = null
    @Volatile private var running = false
    private val callbackThread = HandlerThread("hikari-video-cb").apply { start() }
    private val callbackHandler = Handler(callbackThread.looper)

    private val queuedAt = ConcurrentHashMap<Long, Long>()
    private val decodeSamples = LongArray(120)
    private var decodeCount = 0
    private val presentTimes = LongArray(60)
    private var presentCount = 0
    @Volatile private var lastE2eUs = 0L
    @Volatile var stats = Stats()
        private set
    @Volatile var framesShown = 0L
        private set
    var audioBufferMs: () -> Int = { 0 }

    fun setSurface(s: Surface?) {
        surface = s
        reconfigure = true
    }

    fun setStreamFormat(codec: Int, w: Int, h: Int) {
        if (codec != codecType || w != width || h != height) {
            codecType = codec
            width = w
            height = h
            reconfigure = true
        }
    }

    // A new stream needs an IDR before anything decodes.
    fun resetForNewStream() {
        reconfigure = true
    }

    fun start() {
        if (running) return
        running = true
        thread = Thread(::loop, "hikari-video").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun stop() {
        running = false
        thread?.interrupt()
        thread?.join(500)
        thread = null
        release()
    }

    fun shutdown() {
        stop()
        callbackThread.quitSafely()
    }

    private fun release() {
        codec?.let {
            runCatching { it.stop() }
            runCatching { it.release() }
        }
        codec = null
        freeInputs.clear()
        queuedAt.clear()
    }

    private fun mime() = if (codecType == VideoCodec.HEVC) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC

    private fun open(csd: List<ByteArray>): Boolean {
        release()
        val s = surface ?: return false
        return try {
            val c = MediaCodec.createDecoderByType(mime())
            decoderName = c.name
            val fmt = MediaFormat.createVideoFormat(mime(), width, height)
            csd.forEachIndexed { i, b -> fmt.setByteBuffer("csd-$i", ByteBuffer.wrap(b)) }
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) fmt.setInteger(MediaFormat.KEY_LOW_LATENCY, 1)
            fmt.setInteger(MediaFormat.KEY_PRIORITY, 0)  // realtime
            fmt.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, maxOf(width * height, 1 shl 20))
            // Vendor low-latency switches; unknown keys are ignored by other decoders.
            val n = decoderName.lowercase()
            if (n.contains("qcom") || n.contains("qti")) {
                fmt.setInteger("vendor.qti-ext-dec-picture-order.enable", 1)
                fmt.setInteger("vendor.qti-ext-dec-low-latency.enable", 1)
            } else if (n.contains("exynos")) {
                fmt.setInteger("vendor.rtc-ext-dec-low-latency.enable", 1)
            } else if (n.contains("mtk") || n.contains("mediatek")) {
                fmt.setInteger("vendor.mtk.vdec.lowlatency.mode", 1)
            }
            c.setCallback(object : MediaCodec.Callback() {
                override fun onInputBufferAvailable(codec: MediaCodec, index: Int) {
                    freeInputs.offer(index)
                }

                override fun onOutputBufferAvailable(codec: MediaCodec, index: Int, info: MediaCodec.BufferInfo) {
                    val now = nowUs()
                    runCatching { codec.releaseOutputBuffer(index, true) }
                    onPresented(info.presentationTimeUs, now)
                }

                override fun onError(codec: MediaCodec, e: MediaCodec.CodecException) {
                    Log.w(TAG, "decoder error", e)
                    failed = true
                }

                // Some decoders reset the scaling mode on format changes.
                override fun onOutputFormatChanged(codec: MediaCodec, format: MediaFormat) {
                    runCatching { codec.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT) }
                }
            }, callbackHandler)
            c.configure(fmt, s, null, 0)
            // Scale the whole picture to the surface; never crop its edges.
            c.setVideoScalingMode(MediaCodec.VIDEO_SCALING_MODE_SCALE_TO_FIT)
            c.start()
            codec = c
            failed = false
            Log.i(TAG, "Decoder $decoderName ${width}x$height")
            true
        } catch (e: Exception) {
            Log.e(TAG, "open decoder", e)
            release()
            false
        }
    }

    private fun onPresented(pts: Long, now: Long) {
        framesShown++
        queuedAt.remove(pts)?.let { q ->
            decodeSamples[decodeCount++ % decodeSamples.size] = now - q
        }
        presentTimes[presentCount++ % presentTimes.size] = now
        // pts carries the host capture time; with a synced clock this is capture -> display.
        if (net.clock.valid) lastE2eUs = now - net.clock.toLocal(pts)
    }

    private fun publishStats() {
        val n = minOf(decodeCount, decodeSamples.size)
        val sorted = decodeSamples.copyOf(n).also { it.sort() }
        val p50 = if (n > 0) sorted[n / 2] else 0
        val p95 = if (n > 0) sorted[minOf(n - 1, n * 95 / 100)] else 0
        val m = minOf(presentCount, presentTimes.size)
        var fps = 0.0
        if (m >= 2) {
            val newest = presentTimes[(presentCount - 1) % presentTimes.size]
            val oldest = presentTimes[(presentCount - m) % presentTimes.size]
            if (nowUs() - newest < 500_000 && newest > oldest) fps = (m - 1) * 1e6 / (newest - oldest)
        }
        stats = Stats(p50 / 1000.0, p95 / 1000.0, fps, lastE2eUs / 1000.0, decoderName)
        net.setDecodeFeedback(net.frames.size, p95.toInt(), fps, audioBufferMs())
    }

    private fun loop() {
        var nextStats = 0L
        while (running) {
            val f = try {
                net.frames.poll(20, TimeUnit.MILLISECONDS)
            } catch (e: InterruptedException) {
                null
            }
            val now = nowUs()
            if (now >= nextStats) {
                nextStats = now + 250_000
                publishStats()
            }
            if (f == null) continue
            try {
                decode(f)
            } catch (e: Exception) {
                Log.w(TAG, "decode", e)
                failed = true
            } finally {
                net.recycle(f)
            }
            if (failed) {
                Log.w(TAG, "Decoder error; resetting and requesting a keyframe")
                release()
                failed = false
                reconfigure = true
                net.requestKeyframe(2)
            }
        }
    }

    private fun decode(f: AssembledFrame) {
        val key = f.flags and Proto.VIDEO_KEYFRAME != 0
        if (reconfigure || codec == null) {
            if (surface == null) {
                release()
                return
            }
            if (!key) {
                needKeyframe = true
                askForKeyframe()
                return
            }
            if (!open(parameterSets(f.data, f.size))) return
            reconfigure = false
            needKeyframe = false
        }
        if (needKeyframe && !key) {
            askForKeyframe()
            return
        }
        if (key) needKeyframe = false
        val c = codec ?: return
        val idx = freeInputs.poll(30, TimeUnit.MILLISECONDS) ?: return  // decoder stalled: drop, a later IDR recovers
        val buf = c.getInputBuffer(idx) ?: return
        buf.clear()
        if (buf.capacity() < f.size) {
            c.queueInputBuffer(idx, 0, 0, f.captureUs, 0)
            return
        }
        buf.put(f.data, 0, f.size)
        queuedAt[f.captureUs] = nowUs()
        if (queuedAt.size > 64) queuedAt.clear()
        c.queueInputBuffer(idx, 0, f.size, f.captureUs, if (key) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0)
    }

    // Joined mid-stream (surface came up late, decoder reset): the next IDR may be far away.
    private var lastKeyframeAsk = 0L
    private fun askForKeyframe() {
        val now = nowUs()
        if (now - lastKeyframeAsk < 400_000) return
        lastKeyframeAsk = now
        net.requestKeyframe(3)
    }

    // Splits SPS/PPS (and VPS for HEVC) out of an Annex-B keyframe for csd-*.
    private fun parameterSets(data: ByteArray, size: Int): List<ByteArray> {
        val nals = ArrayList<IntArray>()  // [start-code offset, payload offset]
        var i = 0
        while (i + 3 <= size) {
            if (data[i].toInt() == 0 && data[i + 1].toInt() == 0) {
                if (data[i + 2].toInt() == 1) {
                    nals.add(intArrayOf(i, i + 3)); i += 3; continue
                }
                if (i + 4 <= size && data[i + 2].toInt() == 0 && data[i + 3].toInt() == 1) {
                    nals.add(intArrayOf(i, i + 4)); i += 4; continue
                }
            }
            i++
        }
        val hevc = codecType == VideoCodec.HEVC
        var sps: ByteArray? = null
        var pps: ByteArray? = null
        val joined = java.io.ByteArrayOutputStream()
        for ((k, nal) in nals.withIndex()) {
            val end = if (k + 1 < nals.size) nals[k + 1][0] else size
            if (nal[1] >= end) continue
            val hdr = data[nal[1]].toInt() and 0xFF
            val type = if (hevc) (hdr shr 1) and 0x3F else hdr and 0x1F
            val isParam = if (hevc) type in 32..34 else type == 7 || type == 8
            if (!isParam) continue
            val unit = ByteArray(4 + end - nal[1])
            unit[3] = 1
            System.arraycopy(data, nal[1], unit, 4, end - nal[1])
            when {
                hevc -> joined.write(unit)
                type == 7 -> if (sps == null) sps = unit
                else -> if (pps == null) pps = unit
            }
        }
        // H.264: csd-0 = SPS, csd-1 = PPS. HEVC: csd-0 = VPS+SPS+PPS.
        // Without them the decoder still picks up the in-band parameter sets.
        if (hevc) return if (joined.size() > 0) listOf(joined.toByteArray()) else emptyList()
        val s = sps
        val p = pps
        return if (s != null && p != null) listOf(s, p) else emptyList()
    }

    companion object {
        private const val TAG = "HikariVideo"

        fun hasDecoder(mime: String): Boolean = runCatching {
            android.media.MediaCodecList(android.media.MediaCodecList.REGULAR_CODECS).codecInfos.any { info: MediaCodecInfo ->
                !info.isEncoder && info.supportedTypes.any { it.equals(mime, ignoreCase = true) }
            }
        }.getOrDefault(false)
    }
}
