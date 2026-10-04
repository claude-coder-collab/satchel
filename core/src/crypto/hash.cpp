// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "crypto/hash.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <string_view>
#include <utility>

#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2)
    #include <arm_neon.h>
#elif defined(__x86_64__) || defined(_M_X64)
    #include <immintrin.h>
    #if defined(_MSC_VER) && !defined(__clang__)
        #include <intrin.h>
        #define ZP_SHA256_TARGET
    #else
        #include <cpuid.h>
        #define ZP_SHA256_TARGET __attribute__((target("sha,sse4.1,ssse3")))
    #endif
#endif

namespace zp
{

namespace
{

constexpr std::array<std::uint32_t, 64> sha_k = {
    0x428a2f98,
    0x71374491,
    0xb5c0fbcf,
    0xe9b5dba5,
    0x3956c25b,
    0x59f111f1,
    0x923f82a4,
    0xab1c5ed5,
    0xd807aa98,
    0x12835b01,
    0x243185be,
    0x550c7dc3,
    0x72be5d74,
    0x80deb1fe,
    0x9bdc06a7,
    0xc19bf174,
    0xe49b69c1,
    0xefbe4786,
    0x0fc19dc6,
    0x240ca1cc,
    0x2de92c6f,
    0x4a7484aa,
    0x5cb0a9dc,
    0x76f988da,
    0x983e5152,
    0xa831c66d,
    0xb00327c8,
    0xbf597fc7,
    0xc6e00bf3,
    0xd5a79147,
    0x06ca6351,
    0x14292967,
    0x27b70a85,
    0x2e1b2138,
    0x4d2c6dfc,
    0x53380d13,
    0x650a7354,
    0x766a0abb,
    0x81c2c92e,
    0x92722c85,
    0xa2bfe8a1,
    0xa81a664b,
    0xc24b8b70,
    0xc76c51a3,
    0xd192e819,
    0xd6990624,
    0xf40e3585,
    0x106aa070,
    0x19a4c116,
    0x1e376c08,
    0x2748774c,
    0x34b0bcb5,
    0x391c0cb3,
    0x4ed8aa4a,
    0x5b9cca4f,
    0x682e6ff3,
    0x748f82ee,
    0x78a5636f,
    0x84c87814,
    0x8cc70208,
    0x90befffa,
    0xa4506ceb,
    0xbef9a3f7,
    0xc67178f2,
};

constexpr std::array<std::uint32_t, 64> md5_k = {
    0xd76aa478,
    0xe8c7b756,
    0x242070db,
    0xc1bdceee,
    0xf57c0faf,
    0x4787c62a,
    0xa8304613,
    0xfd469501,
    0x698098d8,
    0x8b44f7af,
    0xffff5bb1,
    0x895cd7be,
    0x6b901122,
    0xfd987193,
    0xa679438e,
    0x49b40821,
    0xf61e2562,
    0xc040b340,
    0x265e5a51,
    0xe9b6c7aa,
    0xd62f105d,
    0x02441453,
    0xd8a1e681,
    0xe7d3fbc8,
    0x21e1cde6,
    0xc33707d6,
    0xf4d50d87,
    0x455a14ed,
    0xa9e3e905,
    0xfcefa3f8,
    0x676f02d9,
    0x8d2a4c8a,
    0xfffa3942,
    0x8771f681,
    0x6d9d6122,
    0xfde5380c,
    0xa4beea44,
    0x4bdecfa9,
    0xf6bb4b60,
    0xbebfbc70,
    0x289b7ec6,
    0xeaa127fa,
    0xd4ef3085,
    0x04881d05,
    0xd9d4d039,
    0xe6db99e5,
    0x1fa27cf8,
    0xc4ac5665,
    0xf4292244,
    0x432aff97,
    0xab9423a7,
    0xfc93a039,
    0x655b59c3,
    0x8f0ccc92,
    0xffeff47d,
    0x85845dd1,
    0x6fa87e4f,
    0xfe2ce6e0,
    0xa3014314,
    0x4e0811a1,
    0xf7537e82,
    0xbd3af235,
    0x2ad7d2bb,
    0xeb86d391,
};

constexpr std::array<int, 64> md5_r = {
    7,
    12,
    17,
    22,
    7,
    12,
    17,
    22,
    7,
    12,
    17,
    22,
    7,
    12,
    17,
    22,
    5,
    9,
    14,
    20,
    5,
    9,
    14,
    20,
    5,
    9,
    14,
    20,
    5,
    9,
    14,
    20,
    4,
    11,
    16,
    23,
    4,
    11,
    16,
    23,
    4,
    11,
    16,
    23,
    4,
    11,
    16,
    23,
    6,
    10,
    15,
    21,
    6,
    10,
    15,
    21,
    6,
    10,
    15,
    21,
    6,
    10,
    15,
    21,
};

template <std::size_t I>
inline void md5_step(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d, const std::array<std::uint32_t, 16>& m)
{
    std::uint32_t f = 0;
    std::size_t g = 0;
    if constexpr (I < 16)
    {
        f = d ^ (b & (c ^ d));
        g = I;
    }
    else if constexpr (I < 32)
    {
        f = c ^ (d & (b ^ c));
        g = (5 * I + 1) % 16;
    }
    else if constexpr (I < 48)
    {
        f = b ^ c ^ d;
        g = (3 * I + 5) % 16;
    }
    else
    {
        f = c ^ (b | ~d);
        g = (7 * I) % 16;
    }
    const auto tmp = d;
    d = c;
    c = b;
    b = b + std::rotl(a + f + md5_k[I] + m[g], md5_r[I]);
    a = tmp;
}

template <std::size_t... I>
inline void md5_rounds(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c, std::uint32_t& d, const std::array<std::uint32_t, 16>& m, std::index_sequence<I...>)
{
    (md5_step<I>(a, b, c, d, m), ...);
}

std::uint32_t load_be32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) | (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::uint32_t load_le32(const std::uint8_t* p)
{
    return (static_cast<std::uint32_t>(p[3]) << 24) | (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[0];
}

template <typename Self>
void feed(Self& self, std::array<std::uint8_t, 64>& buf, std::size_t& buffered, std::uint64_t& length, std::span<const std::uint8_t> data)
{
    length += data.size();
    if (buffered > 0)
    {
        const auto n = std::min(data.size(), buf.size() - buffered);
        std::memcpy(buf.data() + buffered, data.data(), n);
        buffered += n;
        data = data.subspan(n);
        if (buffered < buf.size())
            return;
        self.blocks(buf.data(), 1);
        buffered = 0;
    }
    if (const auto count = data.size() / 64; count > 0)
    {
        self.blocks(data.data(), count);
        data = data.subspan(count * 64);
    }
    if (!data.empty())
    {
        std::memcpy(buf.data(), data.data(), data.size());
        buffered = data.size();
    }
}

void sha256_portable(std::array<std::uint32_t, 8>& state, const std::uint8_t* p)
{
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i)
        w[i] = load_be32(p + 4 * i);
    for (std::size_t i = 16; i < 64; ++i)
    {
        const auto s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const auto s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = state;
    for (std::size_t i = 0; i < 64; ++i)
    {
        const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto ch = (e & f) ^ (~e & g);
        const auto t1 = h + s1 + ch + sha_k[i] + w[i];
        const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto maj = (a & b) ^ (a & c) ^ (b & c);
        const auto t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

#if defined(__aarch64__) && defined(__ARM_FEATURE_SHA2)

void sha256_hardware(std::array<std::uint32_t, 8>& state, const std::uint8_t* p, std::size_t count)
{
    uint32x4_t state0 = vld1q_u32(state.data());
    uint32x4_t state1 = vld1q_u32(state.data() + 4);
    for (; count > 0; --count, p += 64)
    {
        const auto abef = state0;
        const auto cdgh = state1;
        std::array<uint32x4_t, 4> w{};
        for (std::size_t i = 0; i < 4; ++i)
            w[i] = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p + 16 * i)));
        for (std::size_t g = 0; g < 16; ++g)
        {
            const auto k = vaddq_u32(w[g % 4], vld1q_u32(sha_k.data() + 4 * g));
            if (g < 12)
                w[g % 4] = vsha256su0q_u32(w[g % 4], w[(g + 1) % 4]);
            const auto prev = state0;
            state0 = vsha256hq_u32(state0, state1, k);
            state1 = vsha256h2q_u32(state1, prev, k);
            if (g < 12)
                w[g % 4] = vsha256su1q_u32(w[g % 4], w[(g + 2) % 4], w[(g + 3) % 4]);
        }
        state0 = vaddq_u32(state0, abef);
        state1 = vaddq_u32(state1, cdgh);
    }
    vst1q_u32(state.data(), state0);
    vst1q_u32(state.data() + 4, state1);
}

bool cpu_has_sha256()
{
    return true;
}

#elif defined(__x86_64__) || defined(_M_X64)

// NOLINTBEGIN(portability-simd-intrinsics)
struct M128
{
    __m128i v;
};

ZP_SHA256_TARGET void sha256_hardware(std::array<std::uint32_t, 8>& state, const std::uint8_t* p, std::size_t count)
{
    const __m128i mask = _mm_set_epi64x(0x0c0d0e0f08090a0bLL, 0x0405060700010203LL);
    __m128i tmp = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state.data()));
    __m128i state1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(state.data() + 4));
    tmp = _mm_shuffle_epi32(tmp, 0xB1);
    state1 = _mm_shuffle_epi32(state1, 0x1B);
    __m128i state0 = _mm_alignr_epi8(tmp, state1, 8);
    state1 = _mm_blend_epi16(state1, tmp, 0xF0);
    for (; count > 0; --count, p += 64)
    {
        const auto abef = state0;
        const auto cdgh = state1;
        std::array<M128, 4> w{};
        for (std::size_t i = 0; i < 4; ++i)
            w[i].v = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * i)), mask);
        for (std::size_t g = 0; g < 16; ++g)
        {
            auto msg = _mm_add_epi32(w[g % 4].v, _mm_loadu_si128(reinterpret_cast<const __m128i*>(sha_k.data() + 4 * g)));
            state1 = _mm_sha256rnds2_epu32(state1, state0, msg);
            if (g >= 3 && g < 15)
            {
                const auto t = _mm_alignr_epi8(w[g % 4].v, w[(g + 3) % 4].v, 4);
                w[(g + 1) % 4].v = _mm_sha256msg2_epu32(_mm_add_epi32(w[(g + 1) % 4].v, t), w[g % 4].v);
            }
            msg = _mm_shuffle_epi32(msg, 0x0E);
            state0 = _mm_sha256rnds2_epu32(state0, state1, msg);
            if (g >= 1 && g < 13)
                w[(g + 3) % 4].v = _mm_sha256msg1_epu32(w[(g + 3) % 4].v, w[g % 4].v);
        }
        state0 = _mm_add_epi32(state0, abef);
        state1 = _mm_add_epi32(state1, cdgh);
    }
    tmp = _mm_shuffle_epi32(state0, 0x1B);
    state1 = _mm_shuffle_epi32(state1, 0xB1);
    state0 = _mm_blend_epi16(tmp, state1, 0xF0);
    state1 = _mm_alignr_epi8(state1, tmp, 8);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state.data()), state0);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state.data() + 4), state1);
}
// NOLINTEND(portability-simd-intrinsics)

bool cpu_has_sha256()
{
    constexpr unsigned ssse3 = 1u << 9;
    constexpr unsigned sse41 = 1u << 19;
    constexpr unsigned sha = 1u << 29;
    #if defined(_MSC_VER) && !defined(__clang__)
    std::array<int, 4> r{};
    __cpuid(r.data(), 0);
    if (r[0] < 7)
        return false;
    __cpuid(r.data(), 1);
    const auto ecx1 = static_cast<unsigned>(r[2]);
    __cpuidex(r.data(), 7, 0);
    const auto ebx7 = static_cast<unsigned>(r[1]);
    #else
    unsigned a = 0;
    unsigned b = 0;
    unsigned c = 0;
    unsigned d = 0;
    if (__get_cpuid_max(0, nullptr) < 7 || !__get_cpuid(1, &a, &b, &c, &d))
        return false;
    const auto ecx1 = c;
    __cpuid_count(7, 0, a, b, c, d);
    const auto ebx7 = b;
    #endif
    return (ecx1 & ssse3) && (ecx1 & sse41) && (ebx7 & sha);
}

#else

void sha256_hardware(std::array<std::uint32_t, 8>&, const std::uint8_t*, std::size_t) {}

bool cpu_has_sha256()
{
    return false;
}

#endif

std::atomic<bool>& sha256_hardware_enabled()
{
    static std::atomic<bool> enabled{ cpu_has_sha256() };
    return enabled;
}

}

Sha256::Sha256() :
    h_{ 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }
{
}

void Sha256::blocks(const std::uint8_t* p, std::size_t count)
{
    if (sha256_hardware_enabled().load(std::memory_order_relaxed))
    {
        sha256_hardware(h_, p, count);
        return;
    }
    for (; count > 0; --count, p += 64)
        sha256_portable(h_, p);
}

bool Sha256::hardware_accelerated() noexcept
{
    return sha256_hardware_enabled().load();
}

void Sha256::set_hardware_enabled(bool enabled) noexcept
{
    sha256_hardware_enabled().store(enabled && cpu_has_sha256());
}

void Sha256::update(std::span<const std::uint8_t> data)
{
    feed(*this, buf_, buffered_, length_, data);
}

Sha256::Digest Sha256::finish()
{
    const std::uint64_t bits = length_ * 8;
    std::array<std::uint8_t, 72> pad{};
    pad[0] = 0x80;
    const std::size_t pad_len = (buffered_ < 56 ? 56 - buffered_ : 120 - buffered_);
    update({ pad.data(), pad_len });
    std::array<std::uint8_t, 8> len{};
    for (std::size_t i = 0; i < 8; ++i)
        len[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(len);
    Digest out{};
    for (std::size_t i = 0; i < 8; ++i)
    {
        out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
    }
    return out;
}

Sha256::Digest Sha256::of(std::span<const std::uint8_t> data)
{
    Sha256 s;
    s.update(data);
    return s.finish();
}

Md5::Md5() :
    h_{ 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }
{
}

void Md5::blocks(const std::uint8_t* p, std::size_t count)
{
    for (; count > 0; --count, p += 64)
    {
        std::array<std::uint32_t, 16> m{};
        for (std::size_t i = 0; i < 16; ++i)
            m[i] = load_le32(p + 4 * i);
        auto [a, b, c, d] = h_;
        md5_rounds(a, b, c, d, m, std::make_index_sequence<64>{});
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
    }
}

void Md5::update(std::span<const std::uint8_t> data)
{
    feed(*this, buf_, buffered_, length_, data);
}

Md5::Digest Md5::finish()
{
    const std::uint64_t bits = length_ * 8;
    std::array<std::uint8_t, 72> pad{};
    pad[0] = 0x80;
    const std::size_t pad_len = (buffered_ < 56 ? 56 - buffered_ : 120 - buffered_);
    update({ pad.data(), pad_len });
    std::array<std::uint8_t, 8> len{};
    for (std::size_t i = 0; i < 8; ++i)
        len[i] = static_cast<std::uint8_t>(bits >> (8 * i));
    update(len);
    Digest out{};
    for (std::size_t i = 0; i < 4; ++i)
    {
        out[4 * i] = static_cast<std::uint8_t>(h_[i]);
        out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 8);
        out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 16);
        out[4 * i + 3] = static_cast<std::uint8_t>(h_[i] >> 24);
    }
    return out;
}

Md5::Digest Md5::of(std::span<const std::uint8_t> data)
{
    Md5 m;
    m.update(data);
    return m.finish();
}

std::string to_hex(std::span<const std::uint8_t> bytes)
{
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto b : bytes)
    {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

}
