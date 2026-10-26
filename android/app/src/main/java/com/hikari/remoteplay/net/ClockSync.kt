package com.hikari.remoteplay.net

// Offset from the minimum-RTT sample of the last 32 Ping/Pong exchanges
// (shared/common/hikari/sync.h): queueing only inflates RTT, so the fastest
// exchange is the most symmetric one.
class ClockSync {
    private class Sample(val rtt: Long, val offset: Long)

    private val samples = arrayOfNulls<Sample>(WINDOW)
    private var next = 0

    @Volatile var offset = 0L            // remote clock - local clock
        private set
    @Volatile var lastRtt = 0L
        private set
    @Volatile var valid = false
        private set

    @Synchronized
    fun addSample(localSend: Long, remote: Long, localRecv: Long) {
        if (localRecv < localSend) return
        val rtt = localRecv - localSend
        samples[next++ % WINDOW] = Sample(rtt, remote - (localSend + rtt / 2))
        val best = samples.filterNotNull().minBy { it.rtt }
        offset = best.offset
        lastRtt = rtt
        valid = true
    }

    @Synchronized
    fun reset() {
        samples.fill(null)
        next = 0
        valid = false
    }

    fun toLocal(remoteTime: Long) = remoteTime - offset
    fun toRemote(localTime: Long) = localTime + offset

    companion object { private const val WINDOW = 32 }
}

fun nowUs(): Long = System.nanoTime() / 1000
