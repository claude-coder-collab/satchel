// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "common/bytes.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace zp;

TEST_CASE("base64 known vectors", "[bytes]")
{
    CHECK(base64_encode(as_bytes("")).empty());
    CHECK(base64_encode(as_bytes("f")) == "Zg==");
    CHECK(base64_encode(as_bytes("fo")) == "Zm8=");
    CHECK(base64_encode(as_bytes("foo")) == "Zm9v");
    CHECK(base64_encode(as_bytes("foobar")) == "Zm9vYmFy");
    for (std::string s : { "", "f", "fo", "foo", "foob", "fooba", "foobar" })
    {
        auto d = base64_decode(base64_encode(as_bytes(s)));
        REQUIRE(d);
        CHECK(std::string(d->begin(), d->end()) == s);
    }
    CHECK_FALSE(base64_decode("Zg="));
    CHECK_FALSE(base64_decode("Z=g="));
    CHECK_FALSE(base64_decode("Zm9*"));
}

TEST_CASE("byte reader and writer", "[bytes]")
{
    ByteWriter w;
    w.u16le(0x1234);
    w.u32le(0xDEADBEEF);
    w.u64le(0x0102030405060708ULL);
    w.u16be(0xABCD);
    w.u32be(0x01020304);
    const auto v = w.take();
    ByteReader r(v);
    CHECK(r.u16le() == 0x1234);
    CHECK(r.u32le() == 0xDEADBEEF);
    CHECK(r.u64le() == 0x0102030405060708ULL);
    CHECK(r.u16be() == 0xABCD);
    CHECK(r.u32be() == 0x01020304u);
    CHECK_FALSE(r.u8());
}
