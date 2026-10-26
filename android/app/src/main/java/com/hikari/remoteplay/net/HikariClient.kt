package com.hikari.remoteplay.net

import android.util.Log
import com.hikari.remoteplay.proto.AudioPacket
import com.hikari.remoteplay.proto.ByeReason
import com.hikari.remoteplay.proto.ByteReader
import com.hikari.remoteplay.proto.ByteWriter
import com.hikari.remoteplay.proto.DiscoverReply
import com.hikari.remoteplay.proto.Hello
import com.hikari.remoteplay.proto.HikariCrypto
import com.hikari.remoteplay.proto.HostStats
import com.hikari.remoteplay.proto.HostStatus
import com.hikari.remoteplay.proto.Input
import com.hikari.remoteplay.proto.PacketSealer
import com.hikari.remoteplay.proto.PacketType
import com.hikari.remoteplay.proto.Preset
import com.hikari.remoteplay.proto.Proto
import com.hikari.remoteplay.proto.ReceiverReport
import com.hikari.remoteplay.proto.RejectReason
import com.hikari.remoteplay.proto.StreamConfig
import com.hikari.remoteplay.proto.VideoFragment
import com.hikari.remoteplay.proto.Welcome
import com.hikari.remoteplay.proto.isTailscaleIpv4
import com.hikari.remoteplay.proto.readHeader
import com.hikari.remoteplay.proto.splitHostPort
import com.hikari.remoteplay.proto.writeAuth
import com.hikari.remoteplay.proto.writeHeader
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.StandardSocketOptions
import java.nio.ByteBuffer
import java.nio.channels.DatagramChannel
import java.nio.channels.SelectionKey
import java.nio.channels.Selector
import java.util.concurrent.ArrayBlockingQueue
import kotlin.math.abs

// Client network endpoint, a port of client/src/net_client.cpp.
//
// One RX thread does everything time-critical on the receive side: pairing
// handshake, frame reassembly (FEC, deadlines), keyframe requests, receiver
// reports, clock sync and LAN discovery. Complete frames go to the decoder
// through a small bounded queue; input is sent directly from the input thread.

enum class ConnState(val label: String) {
    Idle("Not connected"),
    Connecting("Connecting"),
    Connected("Connected"),
    Reconnecting("Reconnecting"),
    Rejected("Pairing code rejected"),
    Unreachable("Host unreachable"),
    Replaced("Another device took over"),
}

data class DiscoveredHost(val name: String, val address: String, val tailscale: Boolean, val hostId: String, val lastSeenUs: Long)

data class ClientNetStats(
    val rttMs: Double = 0.0,
    val jitterMs: Double = 0.0,
    val lossPct: Double = 0.0,
    val mbps: Double = 0.0,
    val queueDelayMs: Double = 0.0,
    val framesComplete: Long = 0,
    val framesDropped: Long = 0,
    val framesFecRecovered: Long = 0,
    val keyframeRequests: Long = 0,
    val clockSynced: Boolean = false,
)

interface ClientNetListener {
    fun onState(state: ConnState, detail: String) {}
    fun onStreamConfig(config: StreamConfig) {}
    fun onHostStatus(status: HostStatus) {}
    fun onHostStats(stats: HostStats) {}
    fun onAudio(packet: AudioPacket) {}  // RX thread
}

class HikariClient(private val clientName: String, private val displayHz: Int, private val audioCodecs: Int) {

    @Volatile var listener: ClientNetListener? = null

    private var channel: DatagramChannel? = null
    private var selector: Selector? = null
    private var rx: Thread? = null
    @Volatile private var running = false

    @Volatile var state = ConnState.Idle
        private set

    // Connection target + session (guarded by lock).
    private val lock = Any()
    private var host = ""
    private var fallback = ""
    private var useFallback = false
    @Volatile var hostName = ""
        private set
    private var addr: InetSocketAddress? = null
    private var psk = ByteArray(32)
    private var preset = Preset.Balanced
    private var maxKbps = 0
    private var wantConnect = false
    private var targetChanged = false
    private var session = 0
    private var sealer: PacketSealer? = null
    private var clientNonce = ByteArray(Proto.NONCE_SIZE)
    private var hostNonce = ByteArray(Proto.NONCE_SIZE)
    private var haveChallenge = false
    private var handshakeStarted = 0L
    private var lastHello = 0L
    private var lastRx = 0L
    private var nextResolve = 0L

    private val assembler = FrameAssembler()
    val frames = ArrayBlockingQueue<AssembledFrame>(8)

    val clock = ClockSync()
    private var pingSeq = 0
    private var nextPing = 0L
    private var nextReport = 0L
    private var lastKeyframeReq = 0L
    private var awaitingKeyframe = false
    @Volatile private var keyframeRequests = 0L
    @Volatile private var highestFrame = 0

    @Volatile var stats = ClientNetStats()
        private set
    private var lastStats = AssemblerStats()
    private var lastReportUs = 0L
    private var jitterUs = 0.0
    private var prevTransit = 0L
    private var haveTransit = false
    private val minTransit = longArrayOf(Long.MAX_VALUE, Long.MAX_VALUE)
    private var minWindowStart = 0L
    private var lastQueueDelay = 0L
    @Volatile private var lossPct = 0.0
    @Volatile private var mbps = 0.0

    @Volatile private var fbQueue = 0
    @Volatile private var fbDecodeP95 = 0
    @Volatile private var fbPresentFpsX100 = 0
    @Volatile private var fbAudioMs = 0

    @Volatile var discovery = false
    private var nextDiscover = 0L
    private val discovered = HashMap<String, DiscoveredHost>()

    fun start(): Boolean {
        stop()
        return try {
            val ch = DatagramChannel.open()
            ch.setOption(StandardSocketOptions.SO_BROADCAST, true)
            ch.setOption(StandardSocketOptions.SO_RCVBUF, 4 shl 20)
            ch.setOption(StandardSocketOptions.SO_SNDBUF, 1 shl 20)
            ch.bind(InetSocketAddress(0))
            ch.configureBlocking(false)
            val sel = Selector.open()
            ch.register(sel, SelectionKey.OP_READ)
            channel = ch
            selector = sel
            running = true
            rx = Thread(::rxLoop, "hikari-net-rx").apply {
                priority = Thread.MAX_PRIORITY
                start()
            }
            true
        } catch (e: Exception) {
            Log.e(TAG, "socket", e)
            false
        }
    }

    fun stop() {
        if (!running) return
        disconnect()
        running = false
        selector?.wakeup()
        rx?.join(500)
        rx = null
        runCatching { selector?.close() }
        runCatching { channel?.close() }
        selector = null
        channel = null
    }

    private fun setState(s: ConnState, detail: String = "") {
        if (state == s) return
        state = s
        Log.i(TAG, "Connection: ${s.label}${if (detail.isEmpty()) "" else " - $detail"}")
        listener?.onState(s, detail)
    }

    // Connects (and keeps reconnecting) to `host` ("name", "ip", "ip:port").
    // `fallback` (usually the host's Tailscale IP) is tried whenever `host`
    // does not resolve or stops answering. Derives the PSK: call off the main thread.
    fun connect(host: String, pairingCode: String, preset: Preset, maxKbps: Int, fallback: String = "") {
        val key = HikariCrypto.pskFromCode(pairingCode)
        synchronized(lock) {
            this.host = host
            this.fallback = if (fallback == host) "" else fallback
            useFallback = false
            psk = key
            this.preset = preset
            this.maxKbps = maxKbps
            wantConnect = true
            targetChanged = true
            session = 0
            haveChallenge = false
            nextResolve = 0
        }
    }

    fun disconnect() {
        val had = synchronized(lock) { session != 0 }
        if (had) sendSealed(PacketType.BYE) {}
        synchronized(lock) {
            wantConnect = false
            session = 0
        }
        setState(ConnState.Idle)
    }

    private fun sendSealed(type: Int, body: (ByteWriter) -> Unit) {
        val ch = channel ?: return
        val buf = ByteArray(Proto.MAX_DATAGRAM)
        synchronized(lock) {
            val s = sealer ?: return
            val a = addr ?: return
            if (session == 0) return
            val w = ByteWriter(buf)
            writeHeader(w, type, session)
            body(w)
            val n = s.seal(w)
            if (n > 0) runCatching { ch.send(ByteBuffer.wrap(buf, 0, n), a) }
        }
    }

    fun sendInput(input: Input) {
        val inp = if (clock.valid)
            Input(input.seq, clock.toRemote(input.clientTimeUs), input.flags or Proto.INPUT_HOST_CLOCK, input.pad)
        else input
        sendSealed(PacketType.INPUT) { inp.write(it) }
    }

    fun play() = sendSealed(PacketType.PLAY) { it.u8(1) }
    fun stopStream() = sendSealed(PacketType.STOP) {}
    fun power(action: Int) = sendSealed(PacketType.POWER) { it.u8(action) }

    fun setPreset(p: Preset, kbps: Int) {
        synchronized(lock) {
            preset = p
            maxKbps = kbps
        }
        sendSealed(PacketType.SET_PRESET) {
            it.u8(p.ordinal)
            it.u32(kbps)
        }
    }

    fun requestKeyframe(reason: Int) {
        val last = highestFrame
        sendSealed(PacketType.KEYFRAME_REQUEST) {
            it.u32(last)
            it.u8(reason)
        }
        ++keyframeRequests
    }

    fun recycle(f: AssembledFrame) = assembler.recycle(f)

    fun setDecodeFeedback(queue: Int, decodeP95Us: Int, presentFps: Double, audioMs: Int) {
        fbQueue = queue
        fbDecodeP95 = decodeP95Us.coerceIn(0, 65535)
        fbPresentFpsX100 = (presentFps * 100).toInt().coerceIn(0, 65535)
        fbAudioMs = audioMs.coerceIn(0, 65535)
    }

    fun discovered(): List<DiscoveredHost> {
        val now = nowUs()
        synchronized(discovered) { return discovered.values.filter { now - it.lastSeenUs < 6_000_000 } }
    }

    // ---- RX thread ---------------------------------------------------------

    private fun resolve(target: String): InetSocketAddress? {
        val (h, port) = splitHostPort(target)
        return try {
            val all = InetAddress.getAllByName(h)
            // Prefer IPv4: Tailscale and home LANs both answer there.
            val a = all.firstOrNull { it.address.size == 4 } ?: all.first()
            InetSocketAddress(a, port)
        } catch (e: Exception) {
            null
        }
    }

    private fun handshakeStep(now: Long) {
        val ch = channel ?: return
        var target: String
        var primary: String
        synchronized(lock) {
            if (!wantConnect || session != 0) return
            if (!(targetChanged || addr == null)) {
                target = ""
                primary = ""
            } else {
                if (now < nextResolve) return
                primary = host
                target = if (useFallback && fallback.isNotEmpty()) fallback else host
            }
        }
        if (target.isNotEmpty()) {
            val a = resolve(target)  // DNS: may block briefly
            synchronized(lock) {
                if (!wantConnect || primary != host) return
                if (a == null) {
                    if (fallback.isNotEmpty()) useFallback = !useFallback  // try the other address next
                    nextResolve = now + if (fallback.isEmpty()) 2_000_000 else 200_000
                } else {
                    addr = a
                    targetChanged = false
                    handshakeStarted = now
                    haveChallenge = false
                    clientNonce = HikariCrypto.randomBytes(Proto.NONCE_SIZE)
                }
            }
            if (a == null) {
                setState(ConnState.Unreachable, "cannot resolve $target")
                return
            }
        }

        val timedOut: Boolean
        synchronized(lock) {
            if (now - lastHello < HELLO_INTERVAL) return
            lastHello = now
            val buf = ByteArray(256)
            val w = ByteWriter(buf)
            if (!haveChallenge) {
                writeHeader(w, PacketType.HELLO, 0)
                Hello(clientNonce, clientName, preset, maxKbps, 1, audioCodecs, displayHz).write(w)
            } else {
                writeHeader(w, PacketType.AUTH, 0)
                writeAuth(w, clientNonce, hostNonce, HikariCrypto.authMac(psk, clientNonce, hostNonce))
            }
            addr?.let { a -> runCatching { ch.send(ByteBuffer.wrap(buf, 0, w.size), a) } }
            timedOut = now - handshakeStarted > HANDSHAKE_TIMEOUT
            if (timedOut) {
                // Keep trying forever (host may be rebooting); restart the exchange.
                handshakeStarted = now
                haveChallenge = false
                clientNonce = HikariCrypto.randomBytes(Proto.NONCE_SIZE)
                nextResolve = 0
                targetChanged = true  // re-resolve: DHCP / Tailscale address may change
                if (fallback.isNotEmpty()) useFallback = !useFallback
            }
        }
        if (timedOut) setState(ConnState.Unreachable, "no answer from host")
        else if (state != ConnState.Reconnecting && state != ConnState.Unreachable) setState(ConnState.Connecting)
    }

    private fun handle(buf: ByteArray, len: Int, from: InetSocketAddress, now: Long) {
        val r = ByteReader(buf, 0, len)
        val h = readHeader(r) ?: return
        when (h.type) {
            PacketType.DISCOVER_REPLY -> {
                val d = DiscoverReply.read(r) ?: return
                if (d.version != Proto.VERSION) return
                val portSuffix = if (d.port == Proto.DEFAULT_PORT) "" else ":${d.port}"
                val ts = isTailscaleIpv4(d.tailscaleIpv4)
                val ip = from.address.hostAddress ?: return
                val host = DiscoveredHost(
                    name = d.hostName,
                    address = (if (ts) d.tailscaleIpv4 else ip) + portSuffix,
                    tailscale = ts,
                    hostId = d.hostId.joinToString("") { "%02x".format(it) },
                    lastSeenUs = now,
                )
                synchronized(discovered) { discovered[host.hostId] = host }
                return
            }
            PacketType.CHALLENGE -> {
                val nonce = r.bytes(Proto.NONCE_SIZE)
                if (!r.ok) return
                synchronized(lock) {
                    if (session != 0 || !wantConnect || from != addr) return
                    hostNonce = nonce
                    haveChallenge = true
                    lastHello = 0  // answer immediately
                }
                return
            }
            PacketType.REJECT -> {
                val reason = r.u8()
                if (!r.ok) return
                synchronized(lock) {
                    if (session != 0 || !wantConnect || from != addr) return
                    wantConnect = false
                }
                setState(ConnState.Rejected, if (reason == RejectReason.BAD_VERSION) "different Hikari version" else "wrong pairing code")
                return
            }
            PacketType.WELCOME -> {
                val wel = Welcome.read(r) ?: return
                synchronized(lock) {
                    if (session != 0 || !wantConnect || !haveChallenge || from != addr || wel.sessionId == 0) return
                    val want = HikariCrypto.welcomeMac(psk, hostNonce, clientNonce)
                    if (!HikariCrypto.constantTimeEqual(want, 0, wel.mac, 0, Proto.MAC_SIZE)) return  // impostor
                    session = wel.sessionId
                    if (wel.hostName.isNotEmpty()) hostName = wel.hostName
                    sealer = PacketSealer(HikariCrypto.sessionKey(psk, clientNonce, hostNonce))
                    lastRx = now
                }
                assembler.reset()
                drainFrames()
                lastStats = assembler.stats.copy()
                lastReportUs = 0
                nextReport = 0
                highestFrame = 0
                minTransit[0] = Long.MAX_VALUE
                minTransit[1] = Long.MAX_VALUE
                minWindowStart = now
                jitterUs = 0.0
                lastQueueDelay = 0
                awaitingKeyframe = false
                haveTransit = false
                nextPing = 0
                setState(ConnState.Connected)
                listener?.onStreamConfig(wel.config)
                return
            }
        }

        // Session traffic.
        var body = len
        synchronized(lock) {
            if (session == 0 || h.session != session || from != addr) return
            if (!PacketType.isMedia(h.type)) {
                body = sealer?.open(buf, len) ?: 0
                if (body == 0) return
            }
            lastRx = now
        }
        val b = ByteReader(buf, Proto.HEADER_SIZE, body)
        when (h.type) {
            PacketType.VIDEO, PacketType.VIDEO_PARITY -> {
                val f = VideoFragment.read(b) ?: return
                if (f.flags and Proto.VIDEO_KEYFRAME != 0) awaitingKeyframe = false
                assembler.push(f, h.type == PacketType.VIDEO_PARITY, now)
                while (true) onFrameReady(assembler.pop(now) ?: break, now)
            }
            PacketType.AUDIO -> AudioPacket.read(b)?.let { listener?.onAudio(it) }
            PacketType.PONG -> {
                b.u32()
                val echo = b.u64()
                val remote = b.u64()
                if (b.ok) clock.addSample(echo, remote, now)
            }
            PacketType.HOST_STATUS -> HostStatus.read(b)?.let { listener?.onHostStatus(it) }
            PacketType.HOST_STATS -> HostStats.read(b)?.let { listener?.onHostStats(it) }
            PacketType.STREAM_CONFIG -> StreamConfig.read(b)?.let { listener?.onStreamConfig(it) }
            PacketType.BYE -> {
                val reason = if (b.remaining() > 0) b.u8() else ByeReason.CLOSED
                val replaced = reason == ByeReason.REPLACED
                synchronized(lock) {
                    session = 0
                    haveChallenge = false
                    targetChanged = true
                    // Taken over by another device: stay off the host instead of
                    // grabbing it back automatically (two clients would ping-pong).
                    if (replaced) wantConnect = false
                }
                if (replaced) setState(ConnState.Replaced, "another device is now connected to the host")
                else setState(ConnState.Reconnecting, "host closed the session")
            }
        }
    }

    private fun drainFrames() {
        while (true) assembler.recycle(frames.poll() ?: break)
    }

    private fun onFrameReady(f: AssembledFrame, now: Long) {
        // Network one-way delay estimate for this frame, independent of frame
        // size: first packet arrival vs. when the host sent it (capture + host_us).
        val transit = f.firstPacketUs - (f.captureUs + f.hostUs)
        if (haveTransit) jitterUs += (abs((transit - prevTransit).toDouble()) - jitterUs) / 16.0
        prevTransit = transit
        haveTransit = true
        if (now - minWindowStart > 5_000_000) {  // two 5 s windows -> 5..10 s minimum
            minTransit[1] = minTransit[0]
            minTransit[0] = Long.MAX_VALUE
            minWindowStart = now
        }
        minTransit[0] = minOf(minTransit[0], transit)
        lastQueueDelay = transit - minOf(minTransit[0], minTransit[1])
        if (!frames.offer(f)) {
            // The decoder is not keeping up: everything queued is stale. Flush
            // and restart from a keyframe rather than build latency.
            assembler.recycle(f)
            drainFrames()
            awaitingKeyframe = true
            lastKeyframeReq = now
            requestKeyframe(2)
        }
    }

    private fun periodic(now: Long) {
        while (true) onFrameReady(assembler.pop(now) ?: break, now)
        highestFrame = assembler.highestFrame
        val a = assembler.stats

        var connected: Boolean
        synchronized(lock) {
            connected = session != 0
            if (connected && now - lastRx > LINK_TIMEOUT) {
                session = 0
                haveChallenge = false
                targetChanged = true
                connected = false
                lastRx = now
            }
        }
        if (!connected) {
            if (state == ConnState.Connected) setState(ConnState.Reconnecting, "connection lost")
            handshakeStep(now)
        } else {
            // Loss -> ask for a keyframe; repeat if it does not arrive promptly.
            if (assembler.takeLossEvent() && now - lastKeyframeReq > 120_000) {
                awaitingKeyframe = true
                lastKeyframeReq = now
                requestKeyframe(1)
            } else if (awaitingKeyframe && now - lastKeyframeReq > KEYFRAME_RETRY) {
                lastKeyframeReq = now
                requestKeyframe(1)
            }
            if (now >= nextPing) {
                val seq = ++pingSeq
                sendSealed(PacketType.PING) {
                    it.u32(seq)
                    it.u64(now)
                }
                // Fast pings until the clock has converged, then 4 Hz.
                nextPing = now + if (clock.valid && pingSeq > 20) 250_000 else 50_000
            }
            if (now >= nextReport) {
                nextReport = now + REPORT_INTERVAL
                val expected = a.packetsExpected - lastStats.packetsExpected
                val got = a.dataPacketsReceived - lastStats.dataPacketsReceived
                val rr = ReceiverReport(
                    highestFrame = assembler.highestFrame,
                    intervalUs = (if (lastReportUs != 0L) now - lastReportUs else REPORT_INTERVAL).toInt(),
                    packetsReceived = (a.packetsReceived - lastStats.packetsReceived).toInt(),
                    packetsLost = (if (expected > got) expected - got else 0).toInt(),
                    bytesReceived = (a.bytesReceived - lastStats.bytesReceived).toInt(),
                    framesComplete = (a.framesComplete - lastStats.framesComplete).toInt(),
                    framesDropped = (a.framesDropped - lastStats.framesDropped).toInt(),
                    framesFecRecovered = (a.framesFecRecovered - lastStats.framesFecRecovered).toInt(),
                    rttUs = clock.lastRtt.toInt(),
                    jitterUs = jitterUs.toInt(),
                    queueDelayUs = lastQueueDelay.toInt(),
                    decodeQueue = fbQueue,
                    decodeP95Us = fbDecodeP95,
                    presentFpsX100 = fbPresentFpsX100,
                    audioBufferMs = fbAudioMs,
                )
                sendSealed(PacketType.RECEIVER_REPORT) { rr.write(it) }
                val total = rr.packetsReceived.toDouble() + rr.packetsLost
                lossPct = if (total > 0) 100.0 * rr.packetsLost / total else 0.0
                mbps = if (rr.intervalUs != 0) rr.bytesReceived * 8.0 / rr.intervalUs else 0.0
                lastStats = a.copy()
                lastReportUs = now
            }
        }
        stats = ClientNetStats(
            rttMs = clock.lastRtt / 1000.0,
            jitterMs = jitterUs / 1000.0,
            lossPct = lossPct,
            mbps = mbps,
            queueDelayMs = lastQueueDelay / 1000.0,
            framesComplete = a.framesComplete,
            framesDropped = a.framesDropped,
            framesFecRecovered = a.framesFecRecovered,
            keyframeRequests = keyframeRequests,
            clockSynced = clock.valid,
        )

        if (discovery && now >= nextDiscover) {
            nextDiscover = now + 1_500_000
            val buf = ByteArray(Proto.HEADER_SIZE)
            writeHeader(ByteWriter(buf), PacketType.DISCOVER, 0)
            runCatching { channel?.send(ByteBuffer.wrap(buf), InetSocketAddress("255.255.255.255", Proto.DEFAULT_PORT)) }
        }
    }

    private fun rxLoop() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_AUDIO)
        val bb = ByteBuffer.allocate(2048)
        var lastPeriodic = 0L
        while (running) {
            val ch = channel ?: break
            val sel = selector ?: break
            // Short timeout: frame deadlines and keyframe retries are checked here.
            try {
                sel.select(1)
                sel.selectedKeys().clear()
            } catch (e: Exception) {
                if (!running) break
            }
            while (running) {
                bb.clear()
                val from = try { ch.receive(bb) as InetSocketAddress? } catch (e: Exception) { null } ?: break
                val now = nowUs()
                try {
                    handle(bb.array(), bb.position(), from, now)
                } catch (e: Exception) {
                    Log.w(TAG, "packet", e)
                }
                if (now - lastPeriodic >= 1000) {
                    lastPeriodic = now
                    periodic(now)
                }
            }
            val now = nowUs()
            if (now - lastPeriodic >= 1000) {
                lastPeriodic = now
                try {
                    periodic(now)
                } catch (e: Exception) {
                    Log.w(TAG, "periodic", e)
                }
            }
        }
    }

    companion object {
        private const val TAG = "HikariNet"
        private const val HELLO_INTERVAL = 300_000L
        private const val HANDSHAKE_TIMEOUT = 6_000_000L
        private const val LINK_TIMEOUT = 3_000_000L
        private const val REPORT_INTERVAL = 100_000L
        private const val KEYFRAME_RETRY = 400_000L
    }
}
