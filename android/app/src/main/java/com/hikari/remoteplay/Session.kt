package com.hikari.remoteplay

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import com.hikari.remoteplay.input.GamepadMapper
import com.hikari.remoteplay.input.InputSender
import com.hikari.remoteplay.media.AudioPlayer
import com.hikari.remoteplay.media.VideoDecoder
import com.hikari.remoteplay.net.ClientNetListener
import com.hikari.remoteplay.net.ConnState
import com.hikari.remoteplay.net.HikariClient
import com.hikari.remoteplay.net.HostProbe
import com.hikari.remoteplay.net.ProbeResult
import com.hikari.remoteplay.proto.AudioCodec
import com.hikari.remoteplay.proto.AudioPacket
import com.hikari.remoteplay.proto.Btn
import com.hikari.remoteplay.proto.HostStats
import com.hikari.remoteplay.proto.HostStatus
import com.hikari.remoteplay.proto.Invite
import com.hikari.remoteplay.proto.PadState
import com.hikari.remoteplay.proto.PowerAction
import com.hikari.remoteplay.proto.Preset
import com.hikari.remoteplay.proto.SessionPhase
import com.hikari.remoteplay.proto.StreamConfig
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import java.util.concurrent.Executors

enum class Screen { Setup, Lobby, Stream }

// Owns the connection and media pipeline for the app's lifetime and exposes
// what the UI needs as StateFlows (the role of client_app.cpp on desktop).
class Session(private val context: Context) : ClientNetListener {
    val prefs = Prefs(context)
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()

    val deviceName: String = Settings.Global.getString(context.contentResolver, "device_name")?.takeIf { it.isNotBlank() }
        ?: "${Build.MANUFACTURER} ${Build.MODEL}"

    private val displayHz = runCatching {
        @Suppress("DEPRECATION")
        (context.getSystemService(Context.WINDOW_SERVICE) as android.view.WindowManager).defaultDisplay.refreshRate.toInt()
    }.getOrDefault(60).coerceIn(30, 240)

    val client = HikariClient(deviceName, displayHz, if (AudioPlayer.opusSupported) 3 else 1)
    val decoder = VideoDecoder(client)
    val audio = AudioPlayer()
    val input = InputSender(client)
    val gamepad = GamepadMapper()

    private val _screen = MutableStateFlow(if (prefs.invite == null) Screen.Setup else Screen.Lobby)
    val screen: StateFlow<Screen> = _screen
    private val _conn = MutableStateFlow(ConnState.Idle to "")
    val conn: StateFlow<Pair<ConnState, String>> = _conn
    private val _status = MutableStateFlow<HostStatus?>(null)
    val status: StateFlow<HostStatus?> = _status
    private val _hostStats = MutableStateFlow<HostStats?>(null)
    val hostStats: StateFlow<HostStats?> = _hostStats
    private val _config = MutableStateFlow(StreamConfig())
    val config: StateFlow<StreamConfig> = _config
    private val _toast = MutableStateFlow<String?>(null)
    val toast: StateFlow<String?> = _toast
    private val _menuOpen = MutableStateFlow(false)
    val menuOpen: StateFlow<Boolean> = _menuOpen
    private val _gamepadActive = MutableStateFlow(false)
    val gamepadActive: StateFlow<Boolean> = _gamepadActive
    private val _preset = MutableStateFlow(prefs.preset)
    val preset: StateFlow<Preset> = _preset
    private val _hostName = MutableStateFlow(prefs.hostName)
    val hostName: StateFlow<String> = _hostName
    // The user pressed Stop: the lobby offers to resume instead of "ready".
    private val _endedByUser = MutableStateFlow(false)
    val endedByUser: StateFlow<Boolean> = _endedByUser

    private var wifiLock: WifiManager.WifiLock? = null
    private var foreground = false

    init {
        client.listener = this
        decoder.audioBufferMs = { audio.bufferedMs }
        client.start()
        decoder.start()
        input.start()
    }

    // ---- lifecycle ---------------------------------------------------------------

    fun onForeground() {
        foreground = true
        if (_screen.value == Screen.Setup) client.discovery = true else connect()
    }

    fun onBackground() {
        foreground = false
        if (_screen.value == Screen.Stream) stopStream()
        client.discovery = false
        client.disconnect()
    }

    fun connect() {
        val inv = prefs.invite ?: return
        val preset = prefs.preset
        worker.execute { client.connect(inv.host, inv.code, preset, 0, inv.fallback) }
    }

    // ---- setup -------------------------------------------------------------------

    fun openSetup() {
        if (_screen.value == Screen.Stream) stopStream()
        client.disconnect()
        _status.value = null
        _screen.value = Screen.Setup
        client.discovery = true
    }

    fun cancelSetup() {
        if (prefs.invite == null) return
        client.discovery = false
        _screen.value = Screen.Lobby
        connect()
    }

    fun probe(invite: Invite, done: (ProbeResult) -> Unit) {
        worker.execute {
            val r = HostProbe.probe(listOf(invite.host, invite.fallback), invite.code, deviceName)
            main.post {
                if (r is ProbeResult.Ok) {
                    prefs.invite = invite
                    prefs.hostName = r.hostName
                    _hostName.value = r.hostName
                    client.discovery = false
                    _screen.value = Screen.Lobby
                    if (foreground) connect()
                }
                done(r)
            }
        }
    }

    fun discovered() = client.discovered()

    // ---- lobby actions ---------------------------------------------------------------

    fun play() {
        _endedByUser.value = false
        when (client.state) {
            ConnState.Connected -> {}
            ConnState.Rejected -> { openSetup(); return }
            else -> return
        }
        val s = _status.value
        if (s != null && s.phase != SessionPhase.Lobby && s.phase != SessionPhase.Failed && s.phase != SessionPhase.Streaming) return
        // Optimistic: show progress immediately.
        stopPending = false
        _status.value = (s ?: HostStatus()).copy(phase = SessionPhase.Waking, phaseElapsedMs = 0)
        decoder.resetForNewStream()
        client.play()
    }

    // Set by Stop until the host reports it left Streaming, so a status packet
    // already in flight does not pull the user straight back into the stream.
    private var stopPending = false

    fun stopStream() {
        client.stopStream()
        stopPending = true
        _endedByUser.value = true
        leaveStream()
    }

    fun takeOver() = connect()  // the host hands the session to whoever connects last

    fun wake() = client.power(PowerAction.WAKE)
    fun sleep() = client.power(PowerAction.SLEEP)

    fun setPreset(p: Preset) {
        prefs.preset = p
        _preset.value = p
        client.setPreset(p, 0)
    }

    // ---- stream ------------------------------------------------------------------------

    private fun enterStream() {
        if (_screen.value == Screen.Stream) return
        _screen.value = Screen.Stream
        _menuOpen.value = false
        audio.start()
        input.setSending(true)
        acquireWifiLock()
    }

    private fun leaveStream() {
        if (_screen.value != Screen.Stream) return
        input.setSending(false)
        audio.stop()
        releaseWifiLock()
        _menuOpen.value = false
        _screen.value = Screen.Lobby
    }

    fun openMenu() {
        _menuOpen.value = true
        input.setSending(false)  // nothing stays held on the console while the menu is up
    }

    fun closeMenu() {
        _menuOpen.value = false
        if (_screen.value == Screen.Stream) input.setSending(true)
    }

    fun onTouchPad(p: PadState) {
        if (_gamepadActive.value && p != PadState.NEUTRAL) _gamepadActive.value = false
        input.setTouch(p)
    }

    fun showTouchControls() {
        _gamepadActive.value = false
    }

    private var chordSince = 0L
    private var chordFired = false

    fun onPhysicalPad() {
        val p = gamepad.state()
        if (!_gamepadActive.value && p != PadState.NEUTRAL) _gamepadActive.value = true
        // Hold − and + for a second to open the menu, like the desktop client.
        val chord = Btn.MINUS or Btn.PLUS
        val now = android.os.SystemClock.uptimeMillis()
        if (p.buttons and chord == chord) {
            if (chordSince == 0L) chordSince = now
            else if (!chordFired && now - chordSince >= 1000) {
                chordFired = true
                main.post { if (_menuOpen.value) closeMenu() else openMenu() }
            }
        } else {
            chordSince = 0
            chordFired = false
        }
        input.setPhysical(p)  // not sent while the menu is open
    }

    fun showToast(msg: String) {
        _toast.value = msg
        main.removeCallbacksAndMessages(TOAST_TOKEN)
        main.postAtTime({ _toast.value = null }, TOAST_TOKEN, android.os.SystemClock.uptimeMillis() + 3500)
    }

    private fun acquireWifiLock() {
        val wm = context.applicationContext.getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return
        val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) WifiManager.WIFI_MODE_FULL_LOW_LATENCY
        else @Suppress("DEPRECATION") WifiManager.WIFI_MODE_FULL_HIGH_PERF
        wifiLock = wm.createWifiLock(mode, "hikari-stream").apply {
            setReferenceCounted(false)
            runCatching { acquire() }
        }
    }

    private fun releaseWifiLock() {
        wifiLock?.let { runCatching { it.release() } }
        wifiLock = null
    }

    // ---- network events (RX thread) -----------------------------------------------------

    override fun onState(state: ConnState, detail: String) {
        main.post {
            _conn.value = state to detail
            when (state) {
                ConnState.Replaced -> {
                    showToast("Another device took over your host")
                    leaveStream()
                }
                ConnState.Connected -> client.hostName.takeIf { it.isNotEmpty() }?.let {
                    _hostName.value = it
                    prefs.hostName = it
                }
                ConnState.Idle -> {}
                else -> if (_screen.value == Screen.Stream) showToast("Connection lost. Reconnecting…")
            }
            if (state != ConnState.Connected) _status.value = null
        }
    }

    override fun onStreamConfig(config: StreamConfig) {
        decoder.setStreamFormat(config.codec, config.width, config.height)
        main.post { _config.value = config }
    }

    override fun onHostStatus(status: HostStatus) {
        main.post {
            val prev = _status.value
            _status.value = status
            if (status.phase != SessionPhase.Streaming) stopPending = false
            if (status.phase == SessionPhase.Streaming && !stopPending) enterStream()
            // The host left the stream (Stop, failure): fall back to the lobby.
            else if (prev?.phase == SessionPhase.Streaming && status.phase != SessionPhase.Streaming) leaveStream()
        }
    }

    override fun onHostStats(stats: HostStats) {
        _hostStats.value = stats
    }

    override fun onAudio(packet: AudioPacket) {
        if (packet.codec != AudioCodec.NONE) audio.push(packet)
    }

    fun shutdown() {
        input.stop()
        decoder.shutdown()
        audio.stop()
        client.stop()
        releaseWifiLock()
        worker.shutdown()
    }

    companion object {
        private val TOAST_TOKEN = Any()
    }
}
