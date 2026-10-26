package com.hikari.remoteplay.proto

import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

// Pairing and per-packet authentication (shared/common/crypto.cpp).

object HikariCrypto {
    private const val ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"  // Crockford base32
    private val rng = SecureRandom()

    fun randomBytes(n: Int): ByteArray = ByteArray(n).also { rng.nextBytes(it) }

    // Uppercases, strips separators, maps ambiguous characters (O->0, I/L->1, U->V).
    fun normalizeCode(code: String): String {
        val sb = StringBuilder()
        for (ch in code) {
            var c = ch
            if (c in 'a'..'z') c = c - 'a'.code + 'A'.code
            when (c) {
                'O' -> c = '0'
                'I', 'L' -> c = '1'
                'U' -> c = 'V'
            }
            if (ALPHABET.indexOf(c) >= 0) sb.append(c)
        }
        return sb.toString()
    }

    fun formatCode(normalized: String): String = normalized.chunked(4).joinToString("-")

    // Light key stretching: d0 = SHA256(n), d_i = SHA256(d_{i-1} || n), 20000 rounds.
    fun pskFromCode(code: String): ByteArray {
        val n = ("hikari-pair-v1:" + normalizeCode(code)).toByteArray(Charsets.US_ASCII)
        val md = MessageDigest.getInstance("SHA-256")
        var d = md.digest(n)
        repeat(20000) {
            md.update(d)
            md.update(n)
            d = md.digest()
        }
        return d
    }

    fun hmac(key: ByteArray, vararg parts: ByteArray): ByteArray {
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(key, "HmacSHA256"))
        for (p in parts) mac.update(p)
        return mac.doFinal()
    }

    private fun derive(psk: ByteArray, label: String, a: ByteArray, b: ByteArray) =
        hmac(psk, label.toByteArray(Charsets.US_ASCII), a, b)

    fun authMac(psk: ByteArray, clientNonce: ByteArray, hostNonce: ByteArray) = derive(psk, "auth", clientNonce, hostNonce)
    fun welcomeMac(psk: ByteArray, hostNonce: ByteArray, clientNonce: ByteArray) = derive(psk, "welcome", hostNonce, clientNonce)
    fun sessionKey(psk: ByteArray, clientNonce: ByteArray, hostNonce: ByteArray) = derive(psk, "session", clientNonce, hostNonce)

    fun constantTimeEqual(a: ByteArray, aOff: Int, b: ByteArray, bOff: Int, n: Int): Boolean {
        var diff = 0
        for (i in 0 until n) diff = diff or (a[aOff + i].toInt() xor b[bOff + i].toInt())
        return diff == 0
    }
}

// 8-byte truncated HMAC-SHA256 over header + body, keyed with the session key.
class PacketSealer(key: ByteArray) {
    private val mac = Mac.getInstance("HmacSHA256").apply { init(SecretKeySpec(key, "HmacSHA256")) }

    // Appends the tag. Returns the final datagram length (0 if it did not fit).
    @Synchronized
    fun seal(w: ByteWriter): Int {
        if (!w.ok || w.remaining() < Proto.TAG_SIZE) return 0
        mac.update(w.buf, 0, w.size)
        val d = mac.doFinal()
        w.bytes(d, 0, Proto.TAG_SIZE)
        return w.size
    }

    // Verifies the trailing tag. On success returns the length without the tag, else 0.
    @Synchronized
    fun open(buf: ByteArray, len: Int): Int {
        if (len < Proto.HEADER_SIZE + Proto.TAG_SIZE) return 0
        val body = len - Proto.TAG_SIZE
        mac.update(buf, 0, body)
        val d = mac.doFinal()
        return if (HikariCrypto.constantTimeEqual(d, 0, buf, body, Proto.TAG_SIZE)) body else 0
    }
}
