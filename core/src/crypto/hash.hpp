// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace zp
{

// FIPS 180-4 SHA-256.
class Sha256
{
public:
    using Digest = std::array<std::uint8_t, 32>;

    Sha256();
    void update(std::span<const std::uint8_t> data);
    Digest finish();

    static Digest of(std::span<const std::uint8_t> data);

    // One 64-byte block (used by the shared buffering helper).
    void block(const std::uint8_t* p);

private:
    std::array<std::uint32_t, 8> h_{};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

// RFC 1321 MD5 (FLAC STREAMINFO signature).
class Md5
{
public:
    using Digest = std::array<std::uint8_t, 16>;

    Md5();
    void update(std::span<const std::uint8_t> data);
    Digest finish();

    static Digest of(std::span<const std::uint8_t> data);

    // One 64-byte block (used by the shared buffering helper).
    void block(const std::uint8_t* p);

private:
    std::array<std::uint32_t, 4> h_{};
    std::array<std::uint8_t, 64> buf_{};
    std::size_t buffered_ = 0;
    std::uint64_t length_ = 0;
};

std::string to_hex(std::span<const std::uint8_t> bytes);

}
