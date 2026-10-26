package com.hikari.remoteplay.proto

// Little-endian field-at-a-time serialization, mirroring shared/serialization/hikari/byte_io.h.
// u32 values travel as Int (raw bits) and u64 as Long; callers widen where they need unsigned math.

class ByteWriter(val buf: ByteArray, var pos: Int = 0) {
    var ok = true
        private set

    val size: Int get() = pos
    fun remaining(): Int = buf.size - pos

    fun u8(v: Int) {
        if (pos + 1 > buf.size) { ok = false; return }
        buf[pos++] = v.toByte()
    }
    fun u16(v: Int) { u8(v); u8(v ushr 8) }
    fun i16(v: Int) = u16(v)
    fun u32(v: Int) { u16(v); u16(v ushr 16) }
    fun i32(v: Int) = u32(v)
    fun u64(v: Long) { u32(v.toInt()); u32((v ushr 32).toInt()) }

    fun bytes(src: ByteArray, off: Int = 0, len: Int = src.size - off) {
        if (pos + len > buf.size) { ok = false; return }
        System.arraycopy(src, off, buf, pos, len)
        pos += len
    }

    // Fixed-width, zero-padded UTF-8 field; always leaves a terminating zero.
    fun str(s: String, width: Int) {
        val b = s.toByteArray(Charsets.UTF_8)
        val n = minOf(b.size, width - 1)
        bytes(b, 0, n)
        repeat(width - n) { u8(0) }
    }
}

class ByteReader(val buf: ByteArray, var pos: Int = 0, val end: Int = buf.size) {
    var ok = true
        private set

    fun remaining(): Int = if (ok) end - pos else 0

    fun u8(): Int {
        if (pos + 1 > end) { ok = false; return 0 }
        return buf[pos++].toInt() and 0xFF
    }
    fun u16(): Int = u8() or (u8() shl 8)
    fun i16(): Int = u16().toShort().toInt()
    fun u32(): Int = u16() or (u16() shl 16)
    fun i32(): Int = u32()
    fun u64(): Long = (u32().toLong() and 0xFFFFFFFFL) or (u32().toLong() shl 32)

    fun bytes(n: Int): ByteArray {
        if (pos + n > end) { ok = false; return ByteArray(n) }
        val out = buf.copyOfRange(pos, pos + n)
        pos += n
        return out
    }

    fun str(width: Int): String {
        if (pos + width > end) { ok = false; return "" }
        var len = 0
        while (len < width && buf[pos + len] != 0.toByte()) ++len
        val s = String(buf, pos, len, Charsets.UTF_8)
        pos += width
        return s
    }

    fun skip(n: Int) {
        if (pos + n > end) { ok = false; return }
        pos += n
    }
}

fun Int.u32ToLong(): Long = toLong() and 0xFFFFFFFFL

// Wrap-safe "a is newer than b" for 32-bit sequence numbers (hk_seq_newer).
fun seqNewer(a: Int, b: Int): Boolean = (a - b) > 0
