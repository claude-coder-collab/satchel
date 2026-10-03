// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "codecs/codec.hpp"

#include "codecs/deflate.hpp"

#include <zlib-ng.h>

#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace zp
{

std::uint32_t crc32_update(std::uint32_t crc, std::span<const std::uint8_t> data)
{
    return zng_crc32_z(crc, data.data(), data.size());
}

std::uint32_t crc32_combine(std::uint32_t crc1, std::uint32_t crc2, std::uint64_t len2)
{
    return zng_crc32_combine(crc1, crc2, static_cast<z_off64_t>(len2));
}

namespace
{

class StoreEncoder final : public SegmentEncoder
{
public:
    [[nodiscard]] ZipMethod method() const override { return ZipMethod::Store; }
    [[nodiscard]] std::size_t segment_size() const override { return CodecRegistry::store_segment_size; }
    [[nodiscard]] std::size_t history_size() const override { return 0; }
    Result<std::vector<std::uint8_t>> encode(std::span<const std::uint8_t>, std::vector<std::uint8_t>&& input, bool) const override
    {
        return std::move(input);
    }
};

}

std::unique_ptr<SegmentEncoder> CodecRegistry::make_encoder(ZipMethod method, int level)
{
    switch (method)
    {
        case ZipMethod::Store:
            return std::make_unique<StoreEncoder>();
        case ZipMethod::Deflate:
            return std::make_unique<DeflateSegmentEncoder>(level);
    }
    return nullptr;
}

bool CodecRegistry::can_decode(std::uint16_t method)
{
    return method == static_cast<std::uint16_t>(ZipMethod::Store) || method == static_cast<std::uint16_t>(ZipMethod::Deflate);
}

Result<std::unique_ptr<IChunkedStream>> CodecRegistry::make_decoder(std::uint16_t method, std::unique_ptr<IChunkedStream> raw)
{
    if (method == static_cast<std::uint16_t>(ZipMethod::Store))
        return raw;
    if (method == static_cast<std::uint16_t>(ZipMethod::Deflate))
        return InflateStream::create(std::move(raw));
    return fail(Status::UnsupportedMethod, "unsupported compression method " + std::to_string(method));
}

}
