#pragma once
// Allocation-free little-endian reader/writer over caller-owned buffers.
// Every network packet is serialized explicitly field by field: no struct
// packing, no padding surprises, no endianness assumptions.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace hikari {

class ByteWriter {
public:
    ByteWriter(uint8_t* buf, size_t cap) : buf_(buf), cap_(cap) {}

    void u8(uint8_t v) { put(&v, 1); }
    void u16(uint16_t v)
    {
        uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        put(b, 2);
    }
    void u32(uint32_t v)
    {
        uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
        put(b, 4);
    }
    void u64(uint64_t v)
    {
        u32(uint32_t(v));
        u32(uint32_t(v >> 32));
    }
    void i16(int16_t v) { u16(uint16_t(v)); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void bytes(const void* p, size_t n) { put(p, n); }
    // Fixed-width, zero-padded string field.
    void str(const char* s, size_t width)
    {
        size_t n = s ? std::strlen(s) : 0;
        if (n > width) n = width;
        put(s, n);
        for (size_t i = n; i < width; ++i) u8(0);
    }

    size_t size() const { return pos_; }
    bool ok() const { return ok_; }
    uint8_t* data() { return buf_; }
    uint8_t* cursor() { return buf_ + pos_; }
    size_t remaining() const { return cap_ - pos_; }

private:
    void put(const void* p, size_t n)
    {
        if (!ok_ || pos_ + n > cap_) {
            ok_ = false;
            return;
        }
        std::memcpy(buf_ + pos_, p, n);
        pos_ += n;
    }

    uint8_t* buf_;
    size_t cap_;
    size_t pos_ = 0;
    bool ok_ = true;
};

class ByteReader {
public:
    ByteReader(const uint8_t* buf, size_t len) : buf_(buf), len_(len) {}

    uint8_t u8()
    {
        uint8_t v = 0;
        get(&v, 1);
        return v;
    }
    uint16_t u16()
    {
        uint8_t b[2] = {};
        get(b, 2);
        return uint16_t(b[0] | (b[1] << 8));
    }
    uint32_t u32()
    {
        uint8_t b[4] = {};
        get(b, 4);
        return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    }
    uint64_t u64()
    {
        uint64_t lo = u32();
        uint64_t hi = u32();
        return lo | (hi << 32);
    }
    int16_t i16() { return int16_t(u16()); }
    int32_t i32() { return int32_t(u32()); }
    void bytes(void* p, size_t n) { get(p, n); }
    void str(char* out, size_t width)  // out must hold width + 1
    {
        get(out, width);
        out[ok_ ? width : 0] = 0;
    }
    const uint8_t* cursor() const { return buf_ + pos_; }
    void skip(size_t n)
    {
        if (pos_ + n > len_)
            ok_ = false;
        else
            pos_ += n;
    }

    size_t remaining() const { return len_ - pos_; }
    size_t position() const { return pos_; }
    bool ok() const { return ok_; }

private:
    void get(void* p, size_t n)
    {
        if (!ok_ || pos_ + n > len_) {
            ok_ = false;
            std::memset(p, 0, n);
            return;
        }
        std::memcpy(p, buf_ + pos_, n);
        pos_ += n;
    }

    const uint8_t* buf_;
    size_t len_;
    size_t pos_ = 0;
    bool ok_ = true;
};

}  // namespace hikari
