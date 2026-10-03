// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/zip/path_policy.hpp"

#include <catch2/catch_test_macros.hpp>

using zp::PathPolicy;
using zp::Status;

TEST_CASE("normalize composes to NFC and uses forward slashes", "[path]")
{
    const std::string nfd = "Cafe\xCC\x81/take1.wav";
    const std::string nfc = "Caf\xC3\xA9/take1.wav";
    CHECK(PathPolicy::normalize(nfd).value() == nfc);
    CHECK(PathPolicy::normalize(nfc).value() == nfc);
    CHECK(PathPolicy::normalize("a\\b\\c").value() == "a/b/c");
    CHECK(PathPolicy::normalize("./././x").value() == "x");
    CHECK(PathPolicy::normalize("dir/").value() == "dir");
}

TEST_CASE("normalize rejects invalid UTF-8 and NUL", "[path]")
{
    CHECK(PathPolicy::normalize("bad\xFF").error().status == Status::InvalidName);
    CHECK(PathPolicy::normalize(std::string("a\0b", 3)).error().status == Status::InvalidName);
}

TEST_CASE("collision keys fold case, including non-ASCII", "[path]")
{
    CHECK(PathPolicy::collision_key("Take1.WAV").value() == PathPolicy::collision_key("take1.wav").value());
    CHECK(PathPolicy::collision_key("\xC3\x84RGER.txt").value() == PathPolicy::collision_key("\xC3\xA4rger.txt").value());
    CHECK(PathPolicy::collision_key("Stra\xC3\x9F"
                                    "e")
              .value()
        == PathPolicy::collision_key("STRASSE").value());
    CHECK(PathPolicy::collision_key("Cafe\xCC\x81").value() == PathPolicy::collision_key("CAF\xC3\x89").value());
    CHECK(PathPolicy::collision_key("a.txt").value() != PathPolicy::collision_key("b.txt").value());
}

TEST_CASE("validate_for_archive", "[path]")
{
    CHECK(PathPolicy::validate_for_archive("a/b/c.txt"));
    CHECK(PathPolicy::validate_for_archive("x"));
    CHECK_FALSE(PathPolicy::validate_for_archive(""));
    CHECK_FALSE(PathPolicy::validate_for_archive("/etc/passwd"));
    CHECK_FALSE(PathPolicy::validate_for_archive("C:/x"));
    CHECK_FALSE(PathPolicy::validate_for_archive("c:x"));
    CHECK_FALSE(PathPolicy::validate_for_archive("a//b"));
    CHECK_FALSE(PathPolicy::validate_for_archive("a/../b"));
    CHECK_FALSE(PathPolicy::validate_for_archive(".."));
    CHECK_FALSE(PathPolicy::validate_for_archive("a/./b"));
    CHECK_FALSE(PathPolicy::validate_for_archive("a\\b"));
    CHECK_FALSE(PathPolicy::validate_for_archive("bad\xFF"));
}

TEST_CASE("sanitize_for_extraction", "[path]")
{
    CHECK(PathPolicy::sanitize_for_extraction("a/b.txt").value() == "a/b.txt");
    CHECK(PathPolicy::sanitize_for_extraction("./a//b/./c").value() == "a/b/c");
    CHECK(PathPolicy::sanitize_for_extraction("dir/").value() == "dir");
    CHECK(PathPolicy::sanitize_for_extraction("a\\b").value() == "a/b");
    CHECK(PathPolicy::sanitize_for_extraction("../evil").error().status == Status::UnsafePath);
    CHECK(PathPolicy::sanitize_for_extraction("a/../../evil").error().status == Status::UnsafePath);
    CHECK(PathPolicy::sanitize_for_extraction("..\\evil").error().status == Status::UnsafePath);
    CHECK(PathPolicy::sanitize_for_extraction("/etc/passwd").error().status == Status::UnsafePath);
    CHECK(PathPolicy::sanitize_for_extraction("\\\\server\\share\\x").error().status == Status::UnsafePath);
    CHECK(PathPolicy::sanitize_for_extraction("C:\\Windows\\x").error().status == Status::UnsafePath);
    CHECK(PathPolicy::sanitize_for_extraction("file.txt:stream").error().status == Status::UnsafePath);
    CHECK(PathPolicy::sanitize_for_extraction("./").error().status == Status::UnsafePath);
}

TEST_CASE("validate_for_extraction keeps paths under the destination", "[path]")
{
    CHECK(PathPolicy::validate_for_extraction("/tmp/dest", "a/b"));
    CHECK_FALSE(PathPolicy::validate_for_extraction("/tmp/dest", "../x"));
    CHECK_FALSE(PathPolicy::validate_for_extraction("/tmp/dest", "/abs"));
}
