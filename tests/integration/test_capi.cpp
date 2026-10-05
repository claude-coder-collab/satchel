// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "pcm_fixtures.hpp"
#include "test_support.hpp"
#include "zp/zp.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <map>
#include <string>
#include <vector>

extern "C" int zp_c_header_check(void);

namespace
{

struct Ctx
{
    zp_context_t* ctx = zp_context_create(2, 0);
    Ctx() = default;
    Ctx(const Ctx&) = delete;
    Ctx& operator=(const Ctx&) = delete;
    Ctx(Ctx&&) = delete;
    Ctx& operator=(Ctx&&) = delete;
    ~Ctx() { zp_context_free(ctx); }
};

std::vector<std::uint8_t> memory_bytes(zp_stream_t* s)
{
    const std::uint8_t* data = nullptr;
    std::size_t len = 0;
    REQUIRE(zp_stream_memory_data(s, &data, &len) == ZP_OK);
    return { data, data + len };
}

}

TEST_CASE("C header compiles as C", "[capi]")
{
    CHECK(zp_c_header_check() == ZP_OK);
}

TEST_CASE("C API plan, resolve, build, read, extract", "[capi]")
{
    Ctx c;
    zp_input_t* in = zp_input_memory();
    const std::string a = "alpha";
    const std::string b = "beta";
    REQUIRE(zp_input_memory_add_directory(in, "dir", 1700000000, 0755) == ZP_OK);
    REQUIRE(zp_input_memory_add_file(in, "dir/a.txt", reinterpret_cast<const std::uint8_t*>(a.data()), a.size(), 1700000001, 0644) == ZP_OK);
    REQUIRE(zp_input_memory_add_file(in, "DIR/A.TXT", reinterpret_cast<const std::uint8_t*>(b.data()), b.size(), 1700000002, 0644) == ZP_OK);

    zp_plan_options_t opts;
    zp_plan_options_init(&opts);
    zp_plan_t* plan = zp_plan_create(c.ctx, in, &opts);
    REQUIRE(plan);
    CHECK(zp_plan_entry_count(plan) == 3);
    REQUIRE(zp_plan_conflict_count(plan) == 1);
    zp_conflict_t conflict{};
    REQUIRE(zp_plan_get_conflict(plan, 0, &conflict) == ZP_OK);
    CHECK(conflict.kind == ZP_CONFLICT_COLLISION);
    REQUIRE(conflict.entry_count == 2);
    CHECK_FALSE(zp_plan_executable(plan));

    zp_stream_t* out = zp_stream_memory();
    zp_build_result_t* result = nullptr;
    CHECK(zp_build(plan, out, nullptr, nullptr, nullptr, &result) == ZP_CONFLICTS_UNRESOLVED);
    CHECK(std::string(zp_last_error()).find("conflict") != std::string::npos);
    zp_build_result_free(result);

    const zp_resolution_t bad{ conflict.entries[1], ZP_RESOLVE_RENAME, "dir/A.txt" };
    CHECK(zp_plan_resolve(plan, &bad, 1) == ZP_NAME_COLLISION);
    const zp_resolution_t good{ conflict.entries[1], ZP_RESOLVE_RENAME, "dir/b.txt" };
    REQUIRE(zp_plan_resolve(plan, &good, 1) == ZP_OK);
    CHECK(zp_plan_executable(plan));

    int calls = 0;
    auto progress = [](void* user, std::uint64_t, std::uint64_t) -> int {
        ++*static_cast<int*>(user);
        return 0;
    };
    REQUIRE(zp_build(plan, out, nullptr, progress, &calls, &result) == ZP_OK);
    CHECK(calls > 0);
    REQUIRE(zp_build_result_entry_count(result) == 3);
    zp_entry_result_t er{};
    REQUIRE(zp_build_result_get_entry(result, 1, &er) == ZP_OK);
    CHECK(std::string(er.name) == "dir/a.txt");
    CHECK(er.uncompressed_size == a.size());
    zp_build_result_free(result);

    const auto bytes = memory_bytes(out);
    zp_stream_t* rin = zp_stream_memory_from(bytes.data(), bytes.size());
    zp_reader_t* reader = zp_reader_open(c.ctx, rin);
    REQUIRE(reader);
    REQUIRE(zp_reader_entry_count(reader) == 3);
    zp_entry_info_t info{};
    REQUIRE(zp_reader_get_entry(reader, 2, &info) == ZP_OK);
    CHECK(std::string(info.name) == "dir/b.txt");
    CHECK(info.kind == ZP_KIND_FILE);
    CHECK(info.mtime == 1700000002);
    std::array<char, 64> version{};
    REQUIRE(zp_reader_get_app_version(reader, version.data(), version.size()) == ZP_OK);
    CHECK(std::string(version.data()).starts_with("Satchel"));

    zp::test::TempDir dest;
    zp_sink_t* sink = zp_sink_filesystem(dest.path().string().c_str());
    zp_xplan_t* x = zp_extract_plan(reader, nullptr, 0, sink, nullptr);
    REQUIRE(x);
    CHECK(zp_xplan_item_count(x) == 3);
    CHECK(zp_xplan_issue_count(x) == 0);
    REQUIRE(zp_extract(x, nullptr, nullptr) == ZP_OK);
    CHECK(zp_xplan_outcome_count(x) == 2);
    CHECK(zp::test::read_file(dest.path() / "dir" / "b.txt") == zp::test::text_bytes("beta"));
    zp_xplan_free(x);

    zp_xplan_t* again = zp_extract_plan(reader, nullptr, 0, sink, nullptr);
    REQUIRE(again);
    bool exists = false;
    for (std::size_t i = 0; i < zp_xplan_issue_count(again); ++i)
    {
        zp_extract_issue_t issue{};
        REQUIRE(zp_xplan_get_issue(again, i, &issue) == ZP_OK);
        exists |= issue.kind == ZP_ISSUE_EXISTS_AT_DESTINATION;
    }
    CHECK(exists);
    CHECK(zp_extract(again, nullptr, nullptr) == ZP_DECISION_REQUIRED);
    zp_xplan_free(again);

    zp_sink_free(sink);
    zp_reader_free(reader);
    zp_stream_free(rin);
    zp_stream_free(out);
    zp_plan_free(plan);
    zp_input_free(in);
}

TEST_CASE("C API editor and atomic file output", "[capi]")
{
    Ctx c;
    zp::test::TempDir dir;
    const auto path = (dir.path() / "e.zip").string();

    zp_input_t* in = zp_input_memory();
    const std::string a = "keep me";
    REQUIRE(zp_input_memory_add_file(in, "a.txt", reinterpret_cast<const std::uint8_t*>(a.data()), a.size(), 0, 0644) == ZP_OK);
    zp_plan_t* plan = zp_plan_create(c.ctx, in, nullptr);
    zp_stream_t* file = zp_stream_create_file(path.c_str());
    REQUIRE(file);
    REQUIRE(zp_build(plan, file, nullptr, nullptr, nullptr, nullptr) == ZP_OK);
    REQUIRE(zp_stream_commit(file) == ZP_OK);
    zp_stream_free(file);
    zp_plan_free(plan);
    zp_input_free(in);

    zp_stream_t* src = zp_stream_open_file(path.c_str());
    REQUIRE(src);
    zp_editor_t* ed = zp_editor_open(c.ctx, src);
    REQUIRE(ed);
    REQUIRE(zp_editor_rename(ed, 0, "renamed.txt") == ZP_OK);
    CHECK(zp_editor_rename(ed, 5, "x") == ZP_INVALID_ARGUMENT);
    zp_plan_t* view = zp_editor_plan(ed);
    REQUIRE(zp_plan_entry_count(view) == 1);
    zp_plan_entry_t pe{};
    REQUIRE(zp_plan_get_entry(view, 0, &pe) == ZP_OK);
    CHECK(pe.codec == ZP_CODEC_KEPT);
    CHECK(std::string(pe.output_name) == "renamed.txt");
    zp_plan_free(view);

    zp_stream_t* dst = zp_stream_create_file(path.c_str());
    REQUIRE(zp_editor_commit(ed, dst, nullptr, nullptr) == ZP_OK);
    REQUIRE(zp_stream_commit(dst) == ZP_OK);
    zp_stream_free(dst);
    zp_editor_free(ed);
    zp_stream_free(src);

    auto x = zp::test::extract_all(zp::test::read_file(path));
    CHECK(x.files["renamed.txt"].data == zp::test::text_bytes("keep me"));
}

TEST_CASE("C API rejects bad arguments without crashing", "[capi]")
{
    CHECK(zp_plan_create(nullptr, nullptr, nullptr) == nullptr);
    CHECK(std::string(zp_last_error()).find("invalid argument") != std::string::npos);
    CHECK(zp_build(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) == ZP_INVALID_ARGUMENT);
    CHECK(zp_stream_open_file("/definitely/not/here.zip") == nullptr);
    CHECK(std::string(zp_status_name(ZP_CRC_MISMATCH)) == "CRC_MISMATCH");
    CHECK(std::string(zp_status_name(999)) == "UNKNOWN");
    zp_plan_options_t opts;
    zp_plan_options_init(&opts);
    opts.deflate_level = 42;
    Ctx c;
    zp_input_t* in = zp_input_memory();
    CHECK(zp_plan_create(c.ctx, in, &opts) == nullptr);
    zp_input_free(in);
    const std::uint8_t junk[16]{};
    zp_stream_t* s = zp_stream_memory_from(junk, sizeof junk);
    CHECK(zp_reader_open(c.ctx, s) == nullptr);
    CHECK(zp_preview(s, "junk", ZP_PREVIEW_JSON) == nullptr);
    CHECK(zp_preview(s, "junk", 7) == nullptr);
    CHECK(zp_preview(nullptr, "junk", ZP_PREVIEW_HTML) == nullptr);
    zp_stream_free(s);
    CHECK(std::string(zp_version()).size() > 0);
}

TEST_CASE("C API callback input and sink, and JSON descriptions", "[capi]")
{
    Ctx c;
    const std::string alpha = "alpha alpha alpha";
    const std::vector<std::uint8_t> wav = zp::test::make_wav({});
    struct Source
    {
        std::vector<std::vector<std::uint8_t>> data;
    } source{ { std::vector<std::uint8_t>(alpha.begin(), alpha.end()), wav } };
    const zp_input_item_t items[] = {
        { "dir", ZP_KIND_DIRECTORY, 0, 1700000000, 0755 },
        { "dir/a.txt", ZP_KIND_FILE, alpha.size(), 1700000000, 0644 },
        { "dir/take.wav", ZP_KIND_FILE, wav.size(), 1700000000, 0644 },
    };
    const auto read = [](void* user, size_t item, uint64_t offset, uint8_t* buf, size_t len) -> int64_t {
        auto& d = static_cast<Source*>(user)->data[item - 1];
        const auto n = std::min<std::uint64_t>(len, d.size() - offset);
        std::memcpy(buf, d.data() + offset, static_cast<std::size_t>(n));
        return static_cast<int64_t>(n);
    };
    zp_input_t* in = zp_input_from_callbacks(items, 3, read, &source);
    REQUIRE(in);
    zp_plan_t* plan = zp_plan_create(c.ctx, in, nullptr);
    REQUIRE(plan);
    char* json = zp_plan_describe(plan);
    REQUIRE(json);
    const std::string described(json);
    zp_free(json);
    CHECK(described.find("\"output_name\":\"dir/take.flac\"") != std::string::npos);
    CHECK(described.find("\"restored_name\":\"dir/take.wav\"") != std::string::npos);
    CHECK(described.find("\"executable\":true") != std::string::npos);

    zp_stream_t* out = zp_stream_memory();
    REQUIRE(zp_build(plan, out, nullptr, nullptr, nullptr, nullptr) == ZP_OK);
    const auto zip = memory_bytes(out);
    zp_stream_t* rin = zp_stream_memory_from(zip.data(), zip.size());
    zp_reader_t* reader = zp_reader_open(c.ctx, rin);
    REQUIRE(reader);
    json = zp_reader_describe(reader);
    const std::string listing(json);
    zp_free(json);
    CHECK(listing.find("\"restores_to\":\"take.wav\"") != std::string::npos);
    char* details = zp_reader_flac_describe(reader, 2);
    REQUIRE(details);
    const std::string flac_json(details);
    zp_free(details);
    CHECK(flac_json.find("\"container\":\"WAV\"") != std::string::npos);
    char* preview = zp_preview(rin, "a.zip", ZP_PREVIEW_HTML);
    REQUIRE(preview);
    CHECK(std::string(preview).find("<h1>a.zip</h1>") != std::string::npos);
    zp_free(preview);
    CHECK(flac_json.find("\"layout\":\"standard\"") != std::string::npos);
    CHECK(flac_json.find("\"id\":\"fmt \"") != std::string::npos);
    CHECK(flac_json.find("\"ENCODER\"") != std::string::npos);
    CHECK(zp_reader_flac_describe(reader, 1) == nullptr);
    std::uint64_t original_size = 0;
    REQUIRE(zp_reader_flac_original_size(reader, 2, &original_size) == ZP_OK);
    CHECK(original_size == wav.size());
    CHECK(zp_reader_flac_original_size(reader, 1, &original_size) == ZP_INVALID_ARGUMENT);
    CHECK(listing.find("\"original_size\":" + std::to_string(wav.size())) != std::string::npos);

    struct Sink
    {
        std::map<std::int64_t, std::pair<std::string, std::vector<std::uint8_t>>> open;
        std::map<std::string, std::vector<std::uint8_t>> done;
        std::int64_t next = 0;
    } sink;
    zp_sink_callbacks_t cb{};
    cb.user = &sink;
    cb.exists = [](void* u, const char* p) -> int { return static_cast<Sink*>(u)->done.contains(p) ? 1 : 0; };
    cb.make_directory = [](void*, const char*) -> int { return 0; };
    cb.open_file = [](void* u, const char* p, int) -> int64_t {
        auto& s = *static_cast<Sink*>(u);
        s.open[s.next] = { p, {} };
        return s.next++;
    };
    cb.write = [](void* u, int64_t f, const uint8_t* b, size_t n) -> int {
        auto& v = static_cast<Sink*>(u)->open[f].second;
        v.insert(v.end(), b, b + n);
        return 0;
    };
    cb.commit = [](void* u, int64_t f, int64_t, uint32_t, int) -> int {
        auto& s = *static_cast<Sink*>(u);
        s.done[s.open[f].first] = std::move(s.open[f].second);
        s.open.erase(f);
        return 0;
    };
    cb.discard = [](void* u, int64_t f) { static_cast<Sink*>(u)->open.erase(f); };
    zp_sink_t* zsink = zp_sink_from_callbacks(&cb);
    REQUIRE(zsink);
    zp_xplan_t* x = zp_extract_plan(reader, nullptr, 0, zsink, nullptr);
    REQUIRE(x);
    REQUIRE(zp_extract(x, nullptr, nullptr) == ZP_OK);
    json = zp_xplan_describe(x);
    CHECK(std::string(json).find("\"restore\":true") != std::string::npos);
    zp_free(json);
    CHECK(sink.done["dir/take.wav"] == wav);
    CHECK(sink.done["dir/a.txt"] == std::vector<std::uint8_t>(alpha.begin(), alpha.end()));

    zp_xplan_free(x);
    zp_sink_free(zsink);
    zp_reader_free(reader);
    zp_stream_free(rin);
    zp_stream_free(out);
    zp_plan_free(plan);
    zp_input_free(in);
}
