package com.hikari.remoteplay

import com.hikari.remoteplay.net.FrameAssembler
import com.hikari.remoteplay.proto.ByteReader
import com.hikari.remoteplay.proto.ByteWriter
import com.hikari.remoteplay.proto.HikariCrypto
import com.hikari.remoteplay.proto.Input
import com.hikari.remoteplay.proto.Invite
import com.hikari.remoteplay.proto.PacketSealer
import com.hikari.remoteplay.proto.PacketType
import com.hikari.remoteplay.proto.PadState
import com.hikari.remoteplay.proto.Proto
import com.hikari.remoteplay.proto.VideoFragment
import com.hikari.remoteplay.proto.readHeader
import com.hikari.remoteplay.proto.splitHostPort
import com.hikari.remoteplay.proto.writeHeader
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

// Vectors come from an independent implementation of docs/protocol.md, so a
// pass means this client and the C++ host agree byte for byte.
class ProtocolTest {
    private fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }
    private val cn = ByteArray(16) { it.toByte() }
    private val hn = ByteArray(16) { (it + 16).toByte() }

    @Test fun pairingCodeNormalizes() {
        assertEquals("ABCD011V", HikariCrypto.normalizeCode("ab-cd-oi-lu"))
        assertArrayEquals(HikariCrypto.pskFromCode("k7qm-2xpa-9trb"), HikariCrypto.pskFromCode("K7QM2XPA9TRB"))
    }

    @Test fun handshakeDerivationsMatchReference() {
        val psk = HikariCrypto.pskFromCode("K7QM-2XPA-9TRB")
        assertEquals("8685553c36906d79dee80994494f0898d7ffbd5fefe9def6d4568b0069018c62", hex(psk))
        assertEquals("8dbae1a2d1c0d808bca689b458835ef61186b5bea379e022a93ac34097052467", hex(HikariCrypto.authMac(psk, cn, hn)))
        assertEquals("69d65244d5b21f890d1fd034da0b37659d2563a972e391a96f323f893cf0ecdd", hex(HikariCrypto.welcomeMac(psk, hn, cn)))
        assertEquals("ab15c8f4484b48038af09a0164ff3af8ad4ce09d4ab0f26e48650e9916449299", hex(HikariCrypto.sessionKey(psk, cn, hn)))
    }

    @Test fun sealedInputMatchesReference() {
        val psk = HikariCrypto.pskFromCode("K7QM-2XPA-9TRB")
        val sealer = PacketSealer(HikariCrypto.sessionKey(psk, cn, hn))
        val buf = ByteArray(Proto.MAX_DATAGRAM)
        val w = ByteWriter(buf)
        writeHeader(w, PacketType.INPUT, 0x12345678)
        Input(7, 123456789, Proto.INPUT_CONNECTED or Proto.INPUT_HOST_CLOCK,
            PadState(buttons = 0x4 or 0x10000, lx = 1000, ly = -2000, rx = 32767, ry = -32768, lt = 255, rt = 0)).write(w)
        val n = sealer.seal(w)
        assertEquals(
            "b70a785634120700000015cd5b07000000000904000100e80330f8ff7f0080ff009bae153c1122ac3b",
            hex(buf.copyOf(n)),
        )
        // And it opens again; a flipped bit does not.
        assertEquals(n - Proto.TAG_SIZE, sealer.open(buf, n))
        buf[10] = (buf[10].toInt() xor 1).toByte()
        assertEquals(0, sealer.open(buf, n))
    }

    @Test fun headerRoundTrips() {
        val buf = ByteArray(16)
        writeHeader(ByteWriter(buf), PacketType.HOST_STATUS, -2)
        val h = readHeader(ByteReader(buf, 0, 6))!!
        assertEquals(PacketType.HOST_STATUS, h.type)
        assertEquals(-2, h.session)
        assertNull(readHeader(ByteReader(ByteArray(6), 0, 6)))
    }

    @Test fun invitesParseLikeTheDesktop() {
        val inv = Invite.parse("hikari://living-room.tail1234.ts.net/K7QM-2XPA-9TRB?ip=100.101.102.103")!!
        assertEquals("living-room.tail1234.ts.net", inv.host)
        assertEquals("100.101.102.103", inv.fallback)
        assertEquals("K7QM2XPA9TRB", inv.code)
        val q = Invite.parse("  \"HIKARI://100.64.0.7:47801/k7qm-2xpa-9trb\"\n")!!
        assertEquals("100.64.0.7:47801", q.host)
        assertTrue(q.fallback.isEmpty())
        assertNotNull(Invite.parse("myhost/K7QM2XPA9TRB"))
        assertNull(Invite.parse("hello world"))
        assertNull(Invite.parse("hikari://host/ABC"))
        assertNull(Invite.parse("hikari://bad host!/K7QM2XPA9TRB"))
        assertEquals(Invite.parse(inv.toUri()), inv)
        assertEquals("100.64.0.7" to 47801, splitHostPort("100.64.0.7:47801"))
        assertEquals("fd7a::1" to 47800, splitHostPort("[fd7a::1]"))
        assertEquals("living-room" to 47800, splitHostPort("living-room"))
    }

    // Packetize like VideoPacketizer: data fragments, a parity after each group of K.
    private fun fragments(frameId: Int, data: ByteArray, k: Int, flags: Int = 0): List<Pair<VideoFragment, Boolean>> {
        val p = Proto.VIDEO_PAYLOAD
        val n = (data.size + p - 1) / p
        val out = ArrayList<Pair<VideoFragment, Boolean>>()
        var parity = ByteArray(p)
        fun frag(index: Int, payload: ByteArray, len: Int) =
            VideoFragment(frameId, index, n, flags, k, data.size, 1000L, 0, 0, payload, 0, len)
        for (i in 0 until n) {
            val len = minOf(p, data.size - i * p)
            val chunk = data.copyOfRange(i * p, i * p + len)
            if (k > 0) {
                if (i % k == 0) parity = ByteArray(p)
                for (j in 0 until len) parity[j] = (parity[j].toInt() xor chunk[j].toInt()).toByte()
            }
            out += frag(i, chunk, len) to false
            if (k > 0 && (i % k == k - 1 || i == n - 1)) {
                val g = i / k
                val glen = minOf(p, data.size - g * k * p)
                out += frag(g, parity.copyOf(glen), glen) to true
            }
        }
        return out
    }

    @Test fun fecRepairsOneLossPerGroup() {
        val data = ByteArray(5000) { (it * 31 + 7).toByte() }
        val a = FrameAssembler()
        for ((f, parity) in fragments(1, data, 4)) {
            if (!parity && f.index == 2) continue  // lost on the wire
            a.push(f, parity, 10)
        }
        val out = a.pop(11)!!
        assertTrue(out.fecRecovered)
        assertArrayEquals(data, out.data.copyOf(out.size))
        assertFalse(a.takeLossEvent())
    }

    @Test fun twoLossesAbandonTheFrameAndRaiseLoss() {
        val data = ByteArray(5000) { it.toByte() }
        val a = FrameAssembler(reorderWindowUs = 3000)
        for ((f, parity) in fragments(1, data, 4)) {
            if (!parity && (f.index == 1 || f.index == 2)) continue
            a.push(f, parity, 0)
        }
        for ((f, parity) in fragments(2, data, 0)) a.push(f, parity, 100)
        assertNull(a.pop(1000))         // still inside the reorder window
        val next = a.pop(10_000)!!      // frame 1 abandoned, frame 2 delivered
        assertEquals(2, next.frameId)
        assertTrue(next.afterLoss)
        assertTrue(a.takeLossEvent())
    }
}
