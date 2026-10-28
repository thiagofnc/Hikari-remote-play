package com.hikari.remoteplay

import com.hikari.remoteplay.net.ClientNetListener
import com.hikari.remoteplay.net.ConnState
import com.hikari.remoteplay.net.HikariClient
import com.hikari.remoteplay.proto.HostStatus
import com.hikari.remoteplay.proto.Invite
import com.hikari.remoteplay.proto.Preset
import org.junit.Assume.assumeTrue
import org.junit.Test

// Talks to a real host: HIKARI_LIVE_INVITE=hikari://... ./gradlew testDebugUnitTest --tests '*LiveHostTest*'
// HIKARI_LIVE_PLAY=1 also presses Play (wakes the console).
class LiveHostTest {
    @Test fun sessionWithRealHost() {
        val text = System.getenv("HIKARI_LIVE_INVITE")
        assumeTrue(text != null)
        val inv = Invite.parse(text!!)!!
        val client = HikariClient("Hikari live test", 60, 1, 1)
        val log = { s: String -> println("[%6.2f] %s".format((System.nanoTime() - t0) / 1e9, s)) }
        var last: HostStatus? = null
        client.listener = object : ClientNetListener {
            override fun onState(state: ConnState, detail: String) = log("state $state $detail")
            override fun onHostStatus(status: HostStatus) {
                if (last == null || last!!.phase != status.phase || last!!.message != status.message || last!!.console != status.console)
                    log("host: console=${status.console} phase=${status.phase} issue=${status.issue} '${status.message}'")
                last = status
            }
        }
        client.start()
        client.connect(inv.host, inv.code, Preset.Balanced, 0, inv.fallback)
        repeat(40) { Thread.sleep(250) }
        log("after 10 s: state=${client.state} clockSynced=${client.clock.valid} rtt=${client.clock.lastRtt / 1000.0} ms")
        if (System.getenv("HIKARI_LIVE_PLAY") == "1") {
            client.play()
            log("sent Play")
            repeat(60) { Thread.sleep(250) }
            log("after Play: frames queued=${client.frames.size} stats=${client.stats}")
            client.stopStream()
            Thread.sleep(500)
        }
        client.stop()
    }

    private val t0 = System.nanoTime()
}
