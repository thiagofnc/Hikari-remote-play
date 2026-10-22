#include "hikari/crypto.h"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <random>
#endif

namespace hikari::crypto {

namespace {
constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

constexpr char kAlphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";  // Crockford base32
}  // namespace

void Sha256::reset()
{
    static constexpr uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(h_, init, sizeof(h_));
    buf_len_ = 0;
    total_ = 0;
}

void Sha256::block(const uint8_t* p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) |
               uint32_t(p[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + mj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const void* data, size_t len)
{
    const auto* p = static_cast<const uint8_t*>(data);
    total_ += len;
    if (buf_len_) {
        const size_t take = std::min(len, sizeof(buf_) - buf_len_);
        std::memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ == 64) {
            block(buf_);
            buf_len_ = 0;
        }
    }
    while (len >= 64) {
        block(p);
        p += 64;
        len -= 64;
    }
    if (len) {
        std::memcpy(buf_, p, len);
        buf_len_ = len;
    }
}

Digest Sha256::finish()
{
    const uint64_t bits = total_ * 8;
    const uint8_t pad = 0x80;
    update(&pad, 1);
    const uint8_t zero = 0;
    while (buf_len_ != 56) update(&zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; ++i) len_be[i] = uint8_t(bits >> (56 - 8 * i));
    update(len_be, 8);
    Digest out;
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = uint8_t(h_[i] >> 24);
        out[4 * i + 1] = uint8_t(h_[i] >> 16);
        out[4 * i + 2] = uint8_t(h_[i] >> 8);
        out[4 * i + 3] = uint8_t(h_[i]);
    }
    return out;
}

Digest sha256(const void* data, size_t len)
{
    Sha256 s;
    s.update(data, len);
    return s.finish();
}

void Hmac::set_key(const uint8_t* key, size_t len)
{
    uint8_t k[64] = {};
    if (len > 64) {
        const Digest d = sha256(key, len);
        std::memcpy(k, d.data(), d.size());
    } else if (len) {
        std::memcpy(k, key, len);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    inner_.reset();
    inner_.update(ipad, 64);
    outer_.reset();
    outer_.update(opad, 64);
}

Digest Hmac::mac2(const void* a, size_t alen, const void* b, size_t blen) const
{
    Sha256 in = inner_;
    in.update(a, alen);
    if (blen) in.update(b, blen);
    const Digest ih = in.finish();
    Sha256 out = outer_;
    out.update(ih.data(), ih.size());
    return out.finish();
}

Digest Hmac::mac(const void* data, size_t len) const { return mac2(data, len, nullptr, 0); }

void Hmac::tag(const void* data, size_t len, uint8_t out[8]) const
{
    const Digest d = mac(data, len);
    std::memcpy(out, d.data(), 8);
}

bool Hmac::verify_tag(const void* data, size_t len, const uint8_t t[8]) const
{
    uint8_t want[8];
    tag(data, len, want);
    return constant_time_equal(want, t, 8);
}

bool constant_time_equal(const uint8_t* a, const uint8_t* b, size_t n)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= uint8_t(a[i] ^ b[i]);
    return diff == 0;
}

void random_bytes(void* out, size_t n)
{
#ifdef _WIN32
    BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    std::random_device rd;
    auto* p = static_cast<uint8_t*>(out);
    for (size_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(rd());
#endif
}

uint32_t random_u32()
{
    uint32_t v;
    random_bytes(&v, sizeof(v));
    return v;
}

std::string generate_pairing_code()
{
    uint8_t r[12];
    random_bytes(r, sizeof(r));
    std::string s;
    for (int i = 0; i < 12; ++i) {
        if (i && i % 4 == 0) s.push_back('-');
        s.push_back(kAlphabet[r[i] & 31]);
    }
    return s;
}

std::string normalize_pairing_code(const std::string& code)
{
    std::string s;
    for (char c : code) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c == 'O') c = '0';
        if (c == 'I' || c == 'L') c = '1';
        if (c == 'U') c = 'V';
        if (std::strchr(kAlphabet, c) && c != 0) s.push_back(c);
    }
    return s;
}

Digest psk_from_code(const std::string& code)
{
    const std::string n = "hikari-pair-v1:" + normalize_pairing_code(code);
    // Light key stretching: makes offline guessing of a captured handshake
    // meaningfully slower without a noticeable connect delay.
    Digest d = sha256(n.data(), n.size());
    for (int i = 0; i < 20000; ++i) {
        Sha256 s;
        s.update(d.data(), d.size());
        s.update(n.data(), n.size());
        d = s.finish();
    }
    return d;
}

namespace {
Digest derive(const Digest& psk, const char* label, const uint8_t* a, const uint8_t* b)
{
    Hmac h(psk.data(), psk.size());
    uint8_t msg[16 + 32];
    const size_t ll = std::strlen(label);
    std::memcpy(msg, label, ll);
    std::memcpy(msg + ll, a, 16);
    std::memcpy(msg + ll + 16, b, 16);
    return h.mac(msg, ll + 32);
}
}  // namespace

Digest auth_mac(const Digest& psk, const uint8_t* cn, const uint8_t* hn) { return derive(psk, "auth", cn, hn); }
Digest welcome_mac(const Digest& psk, const uint8_t* hn, const uint8_t* cn) { return derive(psk, "welcome", hn, cn); }
Digest session_key(const Digest& psk, const uint8_t* cn, const uint8_t* hn) { return derive(psk, "session", cn, hn); }

}  // namespace hikari::crypto
