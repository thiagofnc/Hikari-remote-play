package com.hikari.remoteplay.net

import com.hikari.remoteplay.proto.Proto
import com.hikari.remoteplay.proto.VideoFragment
import com.hikari.remoteplay.proto.seqNewer
import java.util.concurrent.ConcurrentLinkedQueue

// Port of hikari::FrameAssembler (shared/protocol/video_transport.cpp): XOR-parity
// repair, in-order delivery and deadline-based abandonment. RX thread only, apart
// from recycle(), which the decode thread calls.

class AssembledFrame {
    var frameId = 0
    var flags = 0
    var captureUs = 0L
    var encodeUs = 0
    var hostUs = 0
    var firstPacketUs = 0L
    var completeUs = 0L
    var fecRecovered = false
    var afterLoss = false
    var data = ByteArray(0)
    var size = 0
}

class AssemblerStats {
    var packetsReceived = 0L
    var dataPacketsReceived = 0L
    var packetsExpected = 0L
    var bytesReceived = 0L
    var framesComplete = 0L
    var framesDropped = 0L
    var framesFecRecovered = 0L
    var latePackets = 0L
    var duplicatePackets = 0L

    fun copy() = AssemblerStats().also {
        it.packetsReceived = packetsReceived; it.dataPacketsReceived = dataPacketsReceived
        it.packetsExpected = packetsExpected; it.bytesReceived = bytesReceived
        it.framesComplete = framesComplete; it.framesDropped = framesDropped
        it.framesFecRecovered = framesFecRecovered; it.latePackets = latePackets
        it.duplicatePackets = duplicatePackets
    }
}

class FrameAssembler(var reorderWindowUs: Long = 3000) {
    private class Slot {
        var active = false
        var frameId = 0
        var dataCount = 0
        var received = 0
        var parityReceived = 0
        var fecGroup = 0
        var flags = 0
        var frameSize = 0
        var captureUs = 0L
        var encodeUs = 0
        var hostUs = 0
        var firstUs = 0L
        var lastUs = 0L
        var fecUsed = false
        var have = BooleanArray(0)
        var haveParity = BooleanArray(0)
        var data = ByteArray(0)
        var parity = ByteArray(0)
    }

    private val slots = Array(SLOTS) { Slot() }
    private var started = false
    private var nextFrame = 0
    var highestFrame = 0
        private set
    private var lossEvent = false
    private var pendingAfterLoss = false
    val stats = AssemblerStats()
    private val pool = ConcurrentLinkedQueue<AssembledFrame>()

    fun reset() {
        for (s in slots) s.active = false
        started = false
        pendingAfterLoss = false
        lossEvent = false
    }

    fun recycle(f: AssembledFrame) {
        if (pool.size < 2 * SLOTS) pool.add(f)
    }

    // True once since the last call if any frame was abandoned.
    fun takeLossEvent(): Boolean {
        val v = lossEvent
        lossEvent = false
        return v
    }

    private fun slotFor(id: Int) = slots[Math.floorMod(id, SLOTS)]

    private fun fragLen(frameSize: Int, index: Int): Int = minOf(Proto.VIDEO_PAYLOAD, frameSize - index * Proto.VIDEO_PAYLOAD)

    private fun begin(s: Slot, f: VideoFragment, now: Long) {
        s.active = true
        s.frameId = f.frameId
        s.dataCount = f.dataCount
        s.received = 0
        s.parityReceived = 0
        s.fecGroup = f.fecGroup
        s.flags = f.flags
        s.frameSize = f.frameSize
        s.captureUs = f.captureUs
        s.encodeUs = f.encodeUs
        s.hostUs = f.hostUs
        s.firstUs = now
        s.lastUs = now
        s.fecUsed = false
        s.have = if (s.have.size >= s.dataCount) s.have.also { it.fill(false, 0, s.dataCount) } else BooleanArray(s.dataCount)
        val groups = if (s.fecGroup != 0) (s.dataCount + s.fecGroup - 1) / s.fecGroup else 0
        s.haveParity = BooleanArray(groups)
        if (s.data.size < s.frameSize) s.data = ByteArray(s.frameSize)
        if (s.parity.size < groups * Proto.VIDEO_PAYLOAD) s.parity = ByteArray(groups * Proto.VIDEO_PAYLOAD)
    }

    private fun xorInto(dst: ByteArray, dstOff: Int, src: ByteArray, srcOff: Int, n: Int) {
        for (i in 0 until n) dst[dstOff + i] = (dst[dstOff + i].toInt() xor src[srcOff + i].toInt()).toByte()
    }

    private fun tryRecover(s: Slot, group: Int) {
        if (s.fecGroup == 0 || group >= s.haveParity.size || !s.haveParity[group]) return
        val first = group * s.fecGroup
        val last = minOf(first + s.fecGroup, s.dataCount)
        var missing = -1
        var nMissing = 0
        for (i in first until last) {
            if (!s.have[i]) {
                missing = i
                if (++nMissing > 1) return
            }
        }
        if (nMissing != 1) return
        val mlen = fragLen(s.frameSize, missing)
        val dst = missing * Proto.VIDEO_PAYLOAD
        System.arraycopy(s.parity, group * Proto.VIDEO_PAYLOAD, s.data, dst, mlen)
        for (i in first until last) {
            if (i == missing) continue
            xorInto(s.data, dst, s.data, i * Proto.VIDEO_PAYLOAD, minOf(mlen, fragLen(s.frameSize, i)))
        }
        s.have[missing] = true
        ++s.received
        s.fecUsed = true
    }

    fun push(f: VideoFragment, isParity: Boolean, now: Long) {
        val size = f.frameSize.toLong() and 0xFFFFFFFFL
        if (f.dataCount == 0 || size == 0L ||
            size > f.dataCount.toLong() * Proto.VIDEO_PAYLOAD ||
            size <= (f.dataCount - 1).toLong() * Proto.VIDEO_PAYLOAD
        ) return  // inconsistent header
        if (!started) {
            started = true
            nextFrame = f.frameId
            highestFrame = f.frameId
        }
        if (seqNewer(nextFrame, f.frameId)) {
            ++stats.latePackets
            return
        }
        // A frame far ahead of what we are waiting for: everything older is
        // hopeless, skip forward instead of stalling.
        while (f.frameId - nextFrame >= SLOTS) {
            val old = slotFor(nextFrame)
            if (old.active && old.frameId == nextFrame) {
                abandon(old)
            } else {
                ++stats.framesDropped
                lossEvent = true
                pendingAfterLoss = true
            }
            ++nextFrame
        }
        val s = slotFor(f.frameId)
        if (!s.active || s.frameId != f.frameId) {
            if (s.active) abandon(s)
            begin(s, f, now)
        }
        if (f.dataCount != s.dataCount || f.frameSize != s.frameSize) return
        if (seqNewer(f.frameId, highestFrame)) highestFrame = f.frameId
        ++stats.packetsReceived
        stats.bytesReceived += (f.payloadLen + Proto.HEADER_SIZE + Proto.VIDEO_HEADER_SIZE).toLong()
        s.lastUs = now

        if (isParity) {
            val g = f.index
            if (g >= s.haveParity.size) return
            if (s.haveParity[g]) {
                ++stats.duplicatePackets
                return
            }
            val glen = fragLen(s.frameSize, g * s.fecGroup)
            if (f.payloadLen != glen) return
            System.arraycopy(f.payload, f.payloadOff, s.parity, g * Proto.VIDEO_PAYLOAD, glen)
            s.haveParity[g] = true
            ++s.parityReceived
            tryRecover(s, g)
            return
        }
        val i = f.index
        if (i >= s.dataCount) return
        if (s.have[i]) {
            ++stats.duplicatePackets
            return
        }
        if (f.payloadLen != fragLen(s.frameSize, i)) return
        System.arraycopy(f.payload, f.payloadOff, s.data, i * Proto.VIDEO_PAYLOAD, f.payloadLen)
        s.have[i] = true
        ++s.received
        ++stats.dataPacketsReceived
        if (s.fecGroup != 0) tryRecover(s, i / s.fecGroup)
    }

    private fun abandon(s: Slot) {
        ++stats.framesDropped
        stats.packetsExpected += s.dataCount
        s.active = false
        lossEvent = true
        pendingAfterLoss = true
    }

    private fun take(s: Slot, now: Long): AssembledFrame {
        val out = pool.poll() ?: AssembledFrame()
        out.frameId = s.frameId
        out.flags = s.flags
        out.captureUs = s.captureUs
        out.encodeUs = s.encodeUs
        out.hostUs = s.hostUs
        out.firstPacketUs = s.firstUs
        out.completeUs = now
        out.fecRecovered = s.fecUsed
        out.afterLoss = pendingAfterLoss
        pendingAfterLoss = false
        // Zero-copy hand-off; the slot inherits the pooled buffer.
        val buf = s.data
        s.data = out.data
        out.data = buf
        out.size = s.frameSize
        ++stats.framesComplete
        stats.packetsExpected += s.dataCount
        if (s.fecUsed) ++stats.framesFecRecovered
        s.active = false
        ++nextFrame
        return out
    }

    // Returns the next frame ready for decode, in frame order, or null.
    fun pop(now: Long): AssembledFrame? {
        while (true) {
            if (!started) return null
            val s = slotFor(nextFrame)
            val here = s.active && s.frameId == nextFrame
            if (here && s.received == s.dataCount) return take(s, now)

            var newer = false
            var newerFirst = Long.MAX_VALUE
            for (o in slots) {
                if (o.active && seqNewer(o.frameId, nextFrame)) {
                    newer = true
                    newerFirst = minOf(newerFirst, o.firstUs)
                }
            }
            val giveUp = if (here) {
                val idle = now - s.lastUs
                (newer && idle > reorderWindowUs) || idle > STALL_DEADLINE_US
            } else {
                newer && now - newerFirst > reorderWindowUs  // whole frame never arrived
            }
            if (!giveUp) return null
            if (here) {
                abandon(s)
            } else {
                ++stats.framesDropped
                lossEvent = true
                pendingAfterLoss = true
            }
            ++nextFrame
        }
    }

    companion object {
        const val SLOTS = 16
        private const val STALL_DEADLINE_US = 60_000L  // tail of a stream lost entirely
    }
}
