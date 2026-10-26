package com.hikari.remoteplay.net

import com.hikari.remoteplay.proto.ByteReader
import com.hikari.remoteplay.proto.ByteWriter
import com.hikari.remoteplay.proto.Hello
import com.hikari.remoteplay.proto.HikariCrypto
import com.hikari.remoteplay.proto.PacketType
import com.hikari.remoteplay.proto.Preset
import com.hikari.remoteplay.proto.Proto
import com.hikari.remoteplay.proto.RejectReason
import com.hikari.remoteplay.proto.Welcome
import com.hikari.remoteplay.proto.readHeader
import com.hikari.remoteplay.proto.splitHostPort
import com.hikari.remoteplay.proto.writeAuth
import com.hikari.remoteplay.proto.writeHeader
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.SocketTimeoutException

// Verifies a pairing without opening a session (client/src/host_probe.cpp):
// Hello with kHelloProbe, and the host answers with a session-less Welcome.
sealed class ProbeResult {
    data class Ok(val hostName: String, val address: String) : ProbeResult()
    data object BadCode : ProbeResult()
    data object BadVersion : ProbeResult()
    data object NoAnswer : ProbeResult()
}

object HostProbe {
    fun probe(candidates: List<String>, pairingCode: String, clientName: String, timeoutMs: Int = 5000): ProbeResult {
        val psk = HikariCrypto.pskFromCode(pairingCode)
        val targets = candidates.filter { it.isNotEmpty() }.distinct().mapNotNull { c ->
            val (h, port) = splitHostPort(c)
            runCatching { InetSocketAddress(InetAddress.getByName(h), port) to c }.getOrNull()
        }
        if (targets.isEmpty()) return ProbeResult.NoAnswer

        DatagramSocket().use { sock ->
            sock.soTimeout = 150
            val nonce = HikariCrypto.randomBytes(Proto.NONCE_SIZE)
            val hostNonces = HashMap<InetSocketAddress, ByteArray>()
            val deadline = System.currentTimeMillis() + timeoutMs
            var nextSend = 0L
            val rx = ByteArray(2048)
            var rejected: ProbeResult? = null
            while (System.currentTimeMillis() < deadline) {
                val t = System.currentTimeMillis()
                if (t >= nextSend) {
                    nextSend = t + 300
                    for ((a, _) in targets) {
                        val buf = ByteArray(256)
                        val w = ByteWriter(buf)
                        val hn = hostNonces[a]
                        if (hn == null) {
                            writeHeader(w, PacketType.HELLO, 0)
                            Hello(nonce, clientName, Preset.Balanced, 0, 1, 1, 60, Proto.HELLO_PROBE).write(w)
                        } else {
                            writeHeader(w, PacketType.AUTH, 0)
                            writeAuth(w, nonce, hn, HikariCrypto.authMac(psk, nonce, hn))
                        }
                        runCatching { sock.send(DatagramPacket(buf, w.size, a)) }
                    }
                }
                val p = DatagramPacket(rx, rx.size)
                try {
                    sock.receive(p)
                } catch (e: SocketTimeoutException) {
                    continue
                }
                val from = p.socketAddress as InetSocketAddress
                val target = targets.firstOrNull { it.first == from } ?: continue
                val r = ByteReader(rx, 0, p.length)
                val h = readHeader(r) ?: continue
                when (h.type) {
                    PacketType.CHALLENGE -> {
                        val hn = r.bytes(Proto.NONCE_SIZE)
                        if (r.ok && hostNonces[from] == null) {
                            hostNonces[from] = hn
                            nextSend = 0  // answer immediately
                        }
                    }
                    PacketType.WELCOME -> {
                        val hn = hostNonces[from] ?: continue
                        val wel = Welcome.read(r) ?: continue
                        val want = HikariCrypto.welcomeMac(psk, hn, nonce)
                        if (HikariCrypto.constantTimeEqual(want, 0, wel.mac, 0, Proto.MAC_SIZE))
                            return ProbeResult.Ok(wel.hostName, target.second)
                    }
                    PacketType.REJECT -> {
                        rejected = if (r.u8() == RejectReason.BAD_VERSION) ProbeResult.BadVersion else ProbeResult.BadCode
                        // Another candidate may still be the right host; give it a moment.
                        if (targets.size == 1) return rejected
                    }
                }
            }
            return rejected ?: ProbeResult.NoAnswer
        }
    }
}
