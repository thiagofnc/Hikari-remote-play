package com.hikari.remoteplay.input

import com.hikari.remoteplay.net.HikariClient
import com.hikari.remoteplay.net.nowUs
import com.hikari.remoteplay.proto.Input
import com.hikari.remoteplay.proto.PadState
import com.hikari.remoteplay.proto.Proto
import kotlin.math.abs
import kotlin.math.max

// Merges the on-screen controls and any physical gamepad into one pad and sends
// it like the desktop client does (client/src/gamepad_input.cpp): immediately on
// change, a refresh every 16 ms, and one redundant copy 2 ms after each change.
class InputSender(private val net: HikariClient) {
    private val lock = Object()
    private var touch = PadState.NEUTRAL
    private var physical = PadState.NEUTRAL
    private var sending = false
    private var flushNeutral = false
    private var seq = 0
    private var lastSent = PadState.NEUTRAL
    private var lastSendUs = 0L
    private var lastInput: Input? = null
    private var redundantAt = 0L
    @Volatile private var running = false
    private var thread: Thread? = null

    @Volatile var rate = 0
        private set
    private var rateCount = 0
    private var rateStart = 0L

    fun setTouch(p: PadState) = synchronized(lock) {
        touch = p
        lock.notifyAll()
    }

    fun setPhysical(p: PadState) = synchronized(lock) {
        physical = p
        lock.notifyAll()
    }

    // Lobby and in-stream menu: stop driving the console, releasing anything held.
    fun setSending(on: Boolean) = synchronized(lock) {
        if (sending && !on) flushNeutral = true
        sending = on
        lock.notifyAll()
    }

    fun start() {
        if (running) return
        running = true
        thread = Thread(::loop, "hikari-input").apply {
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    fun stop() {
        running = false
        synchronized(lock) { lock.notifyAll() }
        thread?.join(300)
        thread = null
    }

    private fun merged(): PadState {
        val a = touch
        val b = physical
        // Each stick comes from whichever source is deflected further.
        val touchL = abs(a.lx) + abs(a.ly) >= abs(b.lx) + abs(b.ly)
        val touchR = abs(a.rx) + abs(a.ry) >= abs(b.rx) + abs(b.ry)
        return PadState(
            buttons = a.buttons or b.buttons,
            lx = if (touchL) a.lx else b.lx, ly = if (touchL) a.ly else b.ly,
            rx = if (touchR) a.rx else b.rx, ry = if (touchR) a.ry else b.ry,
            lt = max(a.lt, b.lt), rt = max(a.rt, b.rt),
        )
    }

    private fun loop() {
        android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY)
        while (running) {
            var toSend: Input? = null
            synchronized(lock) {
                val now = nowUs()
                if (flushNeutral) {
                    flushNeutral = false
                    lastSent = PadState.NEUTRAL
                    toSend = Input(++seq, now, Proto.INPUT_CONNECTED, PadState.NEUTRAL)
                    redundantAt = 0
                } else if (sending) {
                    val pad = merged()
                    val changed = pad != lastSent
                    val refresh = now - lastSendUs >= REFRESH_US
                    if (changed || refresh) {
                        val inp = Input(++seq, now, Proto.INPUT_CONNECTED, pad)
                        lastSent = pad
                        lastSendUs = now
                        lastInput = inp
                        redundantAt = if (changed) now + REDUNDANT_DELAY_US else 0
                        toSend = inp
                    } else if (redundantAt != 0L && now >= redundantAt) {
                        // Same sequence number: the host ignores it if the original arrived.
                        lastInput?.let { toSend = Input(it.seq, it.clientTimeUs, it.flags or Proto.INPUT_REDUNDANT, it.pad) }
                        redundantAt = 0
                    }
                }
                if (toSend == null) {
                    val wakeAt = when {
                        !sending -> 0L
                        redundantAt != 0L -> minOf(redundantAt, lastSendUs + REFRESH_US)
                        else -> lastSendUs + REFRESH_US
                    }
                    val waitUs = if (wakeAt == 0L) 100_000 else (wakeAt - now).coerceIn(200, REFRESH_US)
                    lock.wait(waitUs / 1000, ((waitUs % 1000) * 1000).toInt())
                }
            }
            toSend?.let {
                net.sendInput(it)
                countRate()
            }
        }
    }

    private fun countRate() {
        val now = nowUs()
        if (rateStart == 0L) rateStart = now
        ++rateCount
        if (now - rateStart >= 1_000_000) {
            rate = rateCount
            rateCount = 0
            rateStart = now
        }
    }

    companion object {
        private const val REFRESH_US = 16_000L
        private const val REDUNDANT_DELAY_US = 2_000L
    }
}
