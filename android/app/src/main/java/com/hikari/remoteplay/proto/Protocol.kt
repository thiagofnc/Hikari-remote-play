package com.hikari.remoteplay.proto

// Hikari client <-> host UDP protocol. Byte-for-byte port of
// shared/protocol/hikari/net_protocol.h; see docs/protocol.md.

object Proto {
    const val DEFAULT_PORT = 47800
    const val MAGIC = 0xB7
    const val VERSION = 1
    const val MAX_DATAGRAM = 1200
    const val HEADER_SIZE = 6
    const val TAG_SIZE = 8
    const val NONCE_SIZE = 16
    const val MAC_SIZE = 32
    const val VIDEO_HEADER_SIZE = 4 + 2 + 2 + 1 + 1 + 4 + 8 + 2 + 2
    const val VIDEO_PAYLOAD = MAX_DATAGRAM - HEADER_SIZE - VIDEO_HEADER_SIZE

    const val HELLO_PROBE = 1

    const val INPUT_CONNECTED = 1 shl 0
    const val INPUT_LATENCY_MARK = 1 shl 1
    const val INPUT_REDUNDANT = 1 shl 2
    const val INPUT_HOST_CLOCK = 1 shl 3

    const val VIDEO_KEYFRAME = 1 shl 0
    const val VIDEO_RECOVERY = 1 shl 1
}

object PacketType {
    const val HELLO = 1
    const val CHALLENGE = 2
    const val AUTH = 3
    const val WELCOME = 4
    const val REJECT = 5
    const val BYE = 6
    const val INPUT = 10
    const val VIDEO = 20
    const val VIDEO_PARITY = 21
    const val STREAM_CONFIG = 22
    const val AUDIO = 30
    const val PING = 40
    const val PONG = 41
    const val RECEIVER_REPORT = 42
    const val KEYFRAME_REQUEST = 43
    const val HOST_STATS = 44
    const val SET_PRESET = 45
    const val HOST_STATUS = 48
    const val PLAY = 49
    const val STOP = 50
    const val POWER = 51
    const val DISCOVER = 60
    const val DISCOVER_REPLY = 61

    fun isMedia(t: Int) = t == VIDEO || t == VIDEO_PARITY || t == AUDIO
}

// Button bits (HK_BTN_*, Nintendo naming).
object Btn {
    const val Y = 1 shl 0
    const val B = 1 shl 1
    const val A = 1 shl 2
    const val X = 1 shl 3
    const val L = 1 shl 4
    const val R = 1 shl 5
    const val ZL = 1 shl 6
    const val ZR = 1 shl 7
    const val MINUS = 1 shl 8
    const val PLUS = 1 shl 9
    const val LSTICK = 1 shl 10
    const val RSTICK = 1 shl 11
    const val HOME = 1 shl 12
    const val CAPTURE = 1 shl 13
    const val DPAD_UP = 1 shl 16
    const val DPAD_DOWN = 1 shl 17
    const val DPAD_LEFT = 1 shl 18
    const val DPAD_RIGHT = 1 shl 19
}

enum class ConsoleState { Unknown, Off, Asleep, Waking, AwakeNoVideo, Ready;
    companion object { fun of(v: Int) = entries.getOrElse(v) { Unknown } } }

enum class SessionPhase { Lobby, Waking, WaitingForVideo, WaitingForController, StartingStream, Streaming, Failed;
    companion object { fun of(v: Int) = entries.getOrElse(v) { Lobby } } }

enum class HostIssue { None, NoBridge, NoCapture, NoSignal, NoEncoder, WakeFailed, ControllerNotAccepted;
    companion object { fun of(v: Int) = entries.getOrElse(v) { None } } }

enum class Preset(val label: String) { MinimumLatency("Minimum latency"), Balanced("Balanced"), Quality("Quality") }

object VideoCodec { const val H264 = 1; const val HEVC = 2 }
object AudioCodec { const val NONE = 0; const val PCM_S16 = 1; const val OPUS = 2 }
object RejectReason { const val BAD_VERSION = 1; const val BAD_AUTH = 2; const val BUSY = 3 }
object ByeReason { const val CLOSED = 0; const val REPLACED = 1; const val TIMEOUT = 2 }
object PowerAction { const val WAKE = 1; const val SLEEP = 2 }

fun writeHeader(w: ByteWriter, type: Int, session: Int) {
    w.u8(Proto.MAGIC)
    w.u8(type)
    w.u32(session)
}

class Header(val type: Int, val session: Int)

fun readHeader(r: ByteReader): Header? {
    if (r.u8() != Proto.MAGIC) return null
    val t = r.u8()
    val s = r.u32()
    return if (r.ok) Header(t, s) else null
}

// ---- handshake ---------------------------------------------------------------

class Hello(
    val clientNonce: ByteArray,
    val clientName: String,
    val preset: Preset,
    val maxBitrateKbps: Int,
    val videoCodecs: Int,
    val audioCodecs: Int,
    val displayHz: Int,
    val flags: Int = 0,
) {
    fun write(w: ByteWriter) {
        w.u16(Proto.VERSION)
        w.bytes(clientNonce)
        w.str(clientName, 32)
        w.u8(preset.ordinal)
        w.u32(maxBitrateKbps)
        w.u8(videoCodecs)
        w.u8(audioCodecs)
        w.u16(displayHz)
        if (flags != 0) w.u8(flags)
    }
}

fun writeAuth(w: ByteWriter, clientNonce: ByteArray, hostNonce: ByteArray, mac: ByteArray) {
    w.bytes(clientNonce)
    w.bytes(hostNonce)
    w.bytes(mac)
}

data class StreamConfig(
    val codec: Int = VideoCodec.H264,
    val width: Int = 1920,
    val height: Int = 1080,
    val fps: Int = 60,
    val bitrateKbps: Int = 12000,
    val audio: Int = AudioCodec.PCM_S16,
    val audioRate: Int = 48000,
    val audioChannels: Int = 2,
    val audioFrameSamples: Int = 240,
    val fecGroup: Int = 0,
    val encoderName: String = "",
) {
    companion object {
        fun read(r: ByteReader): StreamConfig? {
            val c = StreamConfig(
                codec = r.u8(),
                width = r.u16(),
                height = r.u16(),
                fps = r.u16(),
                bitrateKbps = r.u32(),
                audio = r.u8(),
                audioRate = r.u32(),
                audioChannels = r.u8(),
                audioFrameSamples = r.u16(),
                fecGroup = r.u8(),
                encoderName = r.str(48),
            )
            return if (r.ok) c else null
        }
    }
}

class Welcome(val sessionId: Int, val mac: ByteArray, val config: StreamConfig, val hostName: String) {
    companion object {
        fun read(r: ByteReader): Welcome? {
            val id = r.u32()
            val mac = r.bytes(Proto.MAC_SIZE)
            val cfg = StreamConfig.read(r) ?: return null
            val name = if (r.remaining() >= 32) r.str(32) else ""
            return if (r.ok) Welcome(id, mac, cfg, name) else null
        }
    }
}

// ---- input -------------------------------------------------------------------

// Canonical controller state: sticks signed 16-bit with +Y = up, triggers 0..255.
data class PadState(
    val buttons: Int = 0,
    val lx: Int = 0, val ly: Int = 0,
    val rx: Int = 0, val ry: Int = 0,
    val lt: Int = 0, val rt: Int = 0,
) {
    companion object { val NEUTRAL = PadState() }
}

class Input(val seq: Int, val clientTimeUs: Long, val flags: Int, val pad: PadState) {
    fun write(w: ByteWriter) {
        w.u32(seq)
        w.u64(clientTimeUs)
        w.u8(flags and 0x7F)
        w.u32(pad.buttons)
        w.i16(pad.lx)
        w.i16(pad.ly)
        w.i16(pad.rx)
        w.i16(pad.ry)
        w.u8(pad.lt)
        w.u8(pad.rt)
    }
}

// ---- video -------------------------------------------------------------------

class VideoFragment(
    val frameId: Int,
    val index: Int,
    val dataCount: Int,
    val flags: Int,
    val fecGroup: Int,
    val frameSize: Int,
    val captureUs: Long,
    val encodeUs: Int,
    val hostUs: Int,
    val payload: ByteArray,
    val payloadOff: Int,
    val payloadLen: Int,
) {
    companion object {
        fun read(r: ByteReader): VideoFragment? {
            val f = VideoFragment(
                frameId = r.u32(),
                index = r.u16(),
                dataCount = r.u16(),
                flags = r.u8(),
                fecGroup = r.u8(),
                frameSize = r.u32(),
                captureUs = r.u64(),
                encodeUs = r.u16(),
                hostUs = r.u16(),
                payload = r.buf,
                payloadOff = r.pos,
                payloadLen = r.remaining(),
            )
            if (!r.ok) return null
            return if (f.dataCount > 0 && f.payloadLen > 0 && f.payloadLen <= Proto.VIDEO_PAYLOAD) f else null
        }
    }
}

// ---- audio -------------------------------------------------------------------

class AudioPacket(val seq: Int, val captureUs: Long, val codec: Int, val channels: Int, val samples: Int, val payload: ByteArray) {
    companion object {
        fun read(r: ByteReader): AudioPacket? {
            val seq = r.u32()
            val cap = r.u64()
            val codec = r.u8()
            val ch = r.u8()
            val samples = r.u16()
            if (!r.ok) return null
            return AudioPacket(seq, cap, codec, ch, samples, r.bytes(r.remaining()))
        }
    }
}

// ---- control / feedback ----------------------------------------------------

class ReceiverReport(
    val highestFrame: Int,
    val intervalUs: Int,
    val packetsReceived: Int,
    val packetsLost: Int,
    val bytesReceived: Int,
    val framesComplete: Int,
    val framesDropped: Int,
    val framesFecRecovered: Int,
    val rttUs: Int,
    val jitterUs: Int,
    val queueDelayUs: Int,
    val decodeQueue: Int,
    val decodeP95Us: Int,
    val presentFpsX100: Int,
    val audioBufferMs: Int,
) {
    fun write(w: ByteWriter) {
        w.u32(highestFrame); w.u32(intervalUs); w.u32(packetsReceived); w.u32(packetsLost)
        w.u32(bytesReceived); w.u32(framesComplete); w.u32(framesDropped); w.u32(framesFecRecovered)
        w.u32(rttUs); w.u32(jitterUs); w.i32(queueDelayUs)
        w.u16(decodeQueue); w.u16(decodeP95Us); w.u16(presentFpsX100); w.u16(audioBufferMs)
    }
}

data class HostStats(
    val captureFpsX100: Int = 0,
    val encodeFpsX100: Int = 0,
    val captureDrops: Int = 0,
    val encodeDrops: Int = 0,
    val bitrateKbps: Int = 0,
    val width: Int = 0, val height: Int = 0, val fps: Int = 0,
    val captureToSendP50: Int = 0, val captureToSendP95: Int = 0, val captureToSendP99: Int = 0,
    val encodeP50: Int = 0, val encodeP95: Int = 0, val encodeP99: Int = 0,
    val inputOwdP50: Int = 0, val inputOwdP95: Int = 0, val inputOwdP99: Int = 0,
    val inputHostP50: Int = 0, val inputHostP99: Int = 0,
    val espConnected: Int = 0, val espUsbMounted: Int = 0, val espFailsafe: Int = 0,
    val espReportsPerS: Int = 0,
    val espRxToUsbP50: Int = 0, val espRxToUsbP99: Int = 0,
    val espRxToConsoleP50: Int = 0, val espRxToConsoleP99: Int = 0,
    val espLinkRttP50: Int = 0,
    val inputRate: Int = 0,
    val encoderName: String = "",
) {
    companion object {
        fun read(r: ByteReader): HostStats? {
            val s = HostStats(
                r.u16(), r.u16(), r.u32(), r.u32(), r.u32(),
                r.u16(), r.u16(), r.u16(),
                r.u16(), r.u16(), r.u16(),
                r.u16(), r.u16(), r.u16(),
                r.i32(), r.i32(), r.i32(),
                r.u16(), r.u16(),
                r.u8(), r.u8(), r.u8(),
                r.u16(),
                r.u16(), r.u16(),
                r.u16(), r.u16(),
                r.u16(),
                r.u16(),
                r.str(48),
            )
            return if (r.ok) s else null
        }
    }
}

data class HostStatus(
    val console: ConsoleState = ConsoleState.Unknown,
    val phase: SessionPhase = SessionPhase.Lobby,
    val issue: HostIssue = HostIssue.None,
    val bridgeOk: Boolean = false,
    val captureOk: Boolean = false,
    val signalOk: Boolean = false,
    val encoderOk: Boolean = false,
    val wakeMethods: Int = 0,
    val remoteWakeup: Boolean = false,
    val phaseElapsedMs: Int = 0,
    val phaseProgress: Int = 0,
    val hostName: String = "",
    val message: String = "",
) {
    companion object {
        fun read(r: ByteReader): HostStatus? {
            val s = HostStatus(
                ConsoleState.of(r.u8()), SessionPhase.of(r.u8()), HostIssue.of(r.u8()),
                r.u8() != 0, r.u8() != 0, r.u8() != 0, r.u8() != 0,
                r.u8(), r.u8() != 0, r.u16(), r.u16(),
                r.str(32), r.str(80),
            )
            return if (r.ok) s else null
        }
    }
}

class DiscoverReply(val version: Int, val port: Int, val hostName: String, val hostId: ByteArray, val tailscaleIpv4: String) {
    companion object {
        fun read(r: ByteReader): DiscoverReply? {
            val v = r.u16()
            val port = r.u16()
            val name = r.str(32)
            val id = r.bytes(8)
            if (!r.ok) return null
            var ts = ""
            if (r.remaining() >= 4 + 16) {
                val tag = r.bytes(4)
                if (String(tag, Charsets.US_ASCII) == "HTS1") ts = r.str(16)
            }
            return DiscoverReply(v, port, name, id, ts)
        }
    }
}

// 100.64.0.0/10 (CGNAT range Tailscale assigns from).
fun isTailscaleIpv4(s: String): Boolean {
    val parts = s.split('.')
    if (parts.size != 4) return false
    val n = parts.map { it.toIntOrNull() ?: return false }
    if (n.any { it !in 0..255 }) return false
    return n[0] == 100 && n[1] in 64..127
}
