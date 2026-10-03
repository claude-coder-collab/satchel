// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "common/bytes.hpp"
#include "crypto/hash.hpp"
#include "test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace zp;

TEST_CASE("SHA-256 known vectors", "[hash]")
{
    CHECK(to_hex(Sha256::of(as_bytes(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(to_hex(Sha256::of(as_bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(to_hex(Sha256::of(as_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")))
        == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string million(1000000, 'a');
    CHECK(to_hex(Sha256::of(as_bytes(million))) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 is independent of chunking", "[hash]")
{
    const auto data = test::random_bytes(100003, 1);
    Sha256 s;
    for (std::size_t off = 0, step = 1; off < data.size(); off += step, step = step * 3 % 977 + 1)
        s.update(std::span(data).subspan(off, std::min(step, data.size() - off)));
    CHECK(s.finish() == Sha256::of(data));
}

TEST_CASE("MD5 known vectors", "[hash]")
{
    CHECK(to_hex(Md5::of(as_bytes(""))) == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(to_hex(Md5::of(as_bytes("abc"))) == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(to_hex(Md5::of(as_bytes("12345678901234567890123456789012345678901234567890123456789012345678901234567890"))) == "57edf4a22be3c955ac49da2e2107b67a");
}
