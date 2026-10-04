// SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-Commercial
// Copyright (c) 2026 Venn Audio Ltd.
#include "io/preview.hpp"

#include "common/json_writer.hpp"
#include "io/zip/reader.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <set>
#include <string_view>

namespace zp
{

namespace
{

bool ends_with_flac(std::string_view name)
{
    if (name.size() < 5)
        return false;
    auto ext = std::string(name.substr(name.size() - 5));
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    return ext == ".flac";
}

std::string container_of(const std::vector<std::uint8_t>& first)
{
    const auto starts = [&](std::string_view id) { return first.size() >= id.size() && std::equal(id.begin(), id.end(), first.begin()); };
    if (starts("RF64") || starts("BW64"))
        return "RF64";
    if (starts("RIFF"))
        return "WAV";
    if (starts("FORM"))
        return first.size() >= 12 && first[8] == 'A' && first[11] == 'C' ? "AIFF-C" : "AIFF";
    if (starts("caff"))
        return "CAF";
    if (starts("riff"))
        return "Wave64";
    return "unknown";
}

std::string human_size(std::uint64_t bytes)
{
    constexpr std::array<const char*, 5> units{ "B", "KB", "MB", "GB", "TB" };
    auto v = static_cast<double>(bytes);
    std::size_t u = 0;
    while (v >= 1000.0 && u + 1 < units.size())
    {
        v /= 1000.0;
        ++u;
    }
    return u == 0 ? std::format("{} B", bytes) : std::format("{:.1f} {}", v, units[u]);
}

std::string html_escape(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (const char c : s)
    {
        switch (c)
        {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

std::string duration(std::uint64_t samples, std::uint32_t rate)
{
    if (rate == 0)
        return {};
    const auto ms = samples * 1000 / rate;
    const auto s = ms / 1000;
    if (s >= 3600)
        return std::format("{}:{:02}:{:02}.{:03}", s / 3600, (s / 60) % 60, s % 60, ms % 1000);
    return std::format("{}:{:02}.{:03}", s / 60, s % 60, ms % 1000);
}

std::string percent_smaller(std::uint64_t size, std::uint64_t packed)
{
    if (size == 0)
        return "0%";
    const auto saved = 100.0 * (1.0 - static_cast<double>(packed) / static_cast<double>(size));
    return std::format("{:.0f}% {}", std::abs(saved), saved >= 0 ? "smaller" : "larger");
}

void write_flac_json(JsonWriter& j, const Preview::Flac& f)
{
    j.begin_object();
    j.field("sample_rate", static_cast<std::uint64_t>(f.sample_rate)).field("bits_per_sample", static_cast<std::uint64_t>(f.bits_per_sample));
    j.field("channels", static_cast<std::uint64_t>(f.channels)).field("total_samples", f.total_samples);
    j.field("original_name", f.original_name).field("container", f.container).field("layout", f.layout);
    j.field("channel_index", static_cast<std::uint64_t>(f.channel_index)).field("channel_count", static_cast<std::uint64_t>(f.channel_count));
    j.key("chunks").begin_array();
    for (const auto& c : f.chunks)
        j.begin_object().field("id", c.id).field("size", c.size).field("audio", c.audio).end_object();
    j.end_array().key("tags").begin_array();
    for (const auto& [k, v] : f.tags)
        j.begin_array().value(k).value(v).end_array();
    j.end_array().end_object();
}

std::string_view style()
{
    return R"(<style>
:root { --bg:#ffffff; --text:#1d2330; --muted:#5b6475; --line:#dde1e8; --accent:#2457d6; color-scheme: light dark; }
@media (prefers-color-scheme: dark) { :root { --bg:#1d2128; --text:#e7eaf0; --muted:#9aa3b2; --line:#303641; --accent:#6e95ff; } }
body { margin:0; padding:16px; background:var(--bg); color:var(--text); font:13px/1.45 -apple-system, "Segoe UI", system-ui, sans-serif; }
h1 { font-size:17px; margin:0 0 4px; word-break:break-all; }
.summary { color:var(--muted); margin:0 0 12px; }
.badge { color:var(--accent); font-weight:600; }
table { border-collapse:collapse; width:100%; }
th, td { text-align:left; padding:3px 8px 3px 0; border-bottom:1px solid var(--line); vertical-align:top; }
th { color:var(--muted); font-weight:500; }
td.num { text-align:right; font-variant-numeric:tabular-nums; white-space:nowrap; }
td.name { word-break:break-all; }
.more { color:var(--muted); margin-top:8px; }
dl { display:grid; grid-template-columns:max-content 1fr; gap:4px 12px; margin:0 0 12px; }
dt { color:var(--muted); }
dd { margin:0; word-break:break-word; }
h2 { font-size:14px; margin:16px 0 6px; }
</style>)";
}

void html_flac(std::string& out, const Preview::Flac& f)
{
    out += "<dl>";
    out += std::format("<dt>Audio</dt><dd>{} Hz · {}-bit · {} channel{} · {}</dd>", f.sample_rate, f.bits_per_sample, f.channels, f.channels == 1 ? "" : "s", duration(f.total_samples, f.sample_rate));
    if (!f.original_name.empty())
        out += std::format("<dt>Original file</dt><dd>{}{}</dd>", html_escape(f.original_name), f.container.empty() ? "" : " (" + html_escape(f.container) + ")");
    else if (!f.container.empty())
        out += std::format("<dt>Original format</dt><dd>{}</dd>", html_escape(f.container));
    if (f.layout == "multi_mono")
        out += std::format("<dt>Multi-mono</dt><dd>channel {} of {}</dd>", f.channel_index, f.channel_count);
    else if (f.layout == "standard")
        out += "<dt>Restore</dt><dd>also with <code>flac -d --keep-foreign-metadata</code></dd>";
    out += "</dl>";
    if (!f.tags.empty())
    {
        out += "<h2>Tags</h2><table>";
        for (const auto& [k, v] : f.tags)
            out += std::format("<tr><th>{}</th><td>{}</td></tr>", html_escape(k), html_escape(v));
        out += "</table>";
    }
    if (!f.chunks.empty())
    {
        out += "<h2>Original chunks</h2><table><tr><th>ID</th><th class='num'>Size</th></tr>";
        for (const auto& c : f.chunks)
            out += std::format("<tr><td>{}{}</td><td class='num'>{}</td></tr>", html_escape(c.id), c.audio ? " (audio)" : "", c.audio ? "" : human_size(c.size));
        out += "</table>";
    }
}

}

Preview::Flac describe_flac(const flac::Header& header)
{
    Preview::Flac f;
    const auto& si = header.stream_info;
    f.sample_rate = si.sample_rate;
    f.bits_per_sample = si.bits_per_sample;
    f.channels = si.channels;
    f.total_samples = si.total_samples;
    std::vector<flac::ForeignRecord> records = header.foreign;
    if (header.project)
    {
        const auto& p = *header.project;
        f.layout = p.layout == flac::Layout::Standard ? "standard" : p.layout == flac::Layout::MultiMonoMember ? "multi_mono"
                                                                                                               : "private";
        f.original_name = p.original_name;
        f.channel_index = p.channel_index;
        f.channel_count = p.channel_count;
        if (p.layout != flac::Layout::Standard)
        {
            if (auto unpacked = flac::unpack_private(p.private_data))
                records = std::move(*unpacked);
        }
    }
    if (!records.empty())
    {
        f.container = container_of(records.front().bytes);
        for (std::size_t r = 1; r < records.size(); ++r)
        {
            const auto& b = records[r].bytes;
            Preview::Chunk c;
            c.id = b.size() >= 4 ? std::string(reinterpret_cast<const char*>(b.data()), 4) : std::string{};
            c.size = b.size();
            c.audio = c.id == "data" || c.id == "SSND";
            f.chunks.push_back(std::move(c));
        }
    }
    if (header.comments)
        f.tags = header.comments->fields;
    return f;
}

Result<Preview> make_preview(IChunkedStream& input, std::string file_name, const PreviewOptions& options)
{
    Preview p;
    p.file_name = std::move(file_name);
    std::array<std::uint8_t, 4> magic{};
    if (auto r = input.seek(0); !r)
        return std::unexpected(r.error());
    std::size_t got = 0;
    while (got < magic.size())
    {
        auto n = input.read(magic.data() + got, magic.size() - got);
        if (!n)
            return std::unexpected(n.error());
        if (*n == 0)
            break;
        got += *n;
    }
    if (got == 4 && std::string_view(reinterpret_cast<const char*>(magic.data()), 4) == "fLaC")
    {
        if (auto r = input.seek(0); !r)
            return std::unexpected(r.error());
        auto header = flac::read_header(input);
        if (!header)
            return std::unexpected(header.error());
        p.kind = Preview::Kind::Flac;
        p.flac = describe_flac(*header);
        return p;
    }

    auto reader = ArchiveReader::open(input);
    if (!reader)
        return std::unexpected(reader.error());
    auto& r = **reader;
    p.kind = Preview::Kind::Zip;
    if (r.metadata())
        p.created_by = r.metadata()->app_version;
    p.zip64 = r.zip64();
    p.entry_count = r.entries().size();
    std::set<std::array<std::uint8_t, 16>> groups;
    std::size_t probed = 0;
    for (std::size_t i = 0; i < r.entries().size(); ++i)
    {
        std::optional<flac::Header> header;
        if (r.entries()[i].kind == ItemKind::File && ends_with_flac(r.entries()[i].name))
        {
            if (probed < options.max_probed)
            {
                ++probed;
                header = r.flac_header(i);
            }
            else
                p.restorable_audio_partial = true;
        }
        const auto& entry = r.entries()[i];
        if (entry.kind != ItemKind::Directory)
        {
            ++p.file_count;
            p.total_size += entry.uncompressed_size;
            p.packed_size += entry.compressed_size;
        }
        if (entry.flac_restorable && (!entry.flac_group || groups.insert(entry.flac_group->group_id).second))
            ++p.restorable_audio;
        if (p.entries.size() >= options.max_entries)
        {
            p.truncated = true;
            continue;
        }
        Preview::Entry pe;
        pe.name = entry.name;
        pe.directory = entry.kind == ItemKind::Directory;
        pe.size = entry.uncompressed_size;
        pe.packed = entry.compressed_size;
        if (header)
            pe.method = "FLAC";
        else if (entry.raw_method == 0)
            pe.method = "Stored";
        else if (entry.raw_method == 8)
            pe.method = "Deflate";
        else
            pe.method = std::format("Method {}", entry.raw_method);
        if (entry.flac_restorable && header && header->project)
            pe.restores_to = header->project->original_name;
        p.entries.push_back(std::move(pe));
    }
    return p;
}

std::string preview_json(const Preview& p)
{
    JsonWriter j;
    j.begin_object().field("kind", p.kind == Preview::Kind::Zip ? "zip" : "flac").field("file_name", p.file_name);
    if (p.kind == Preview::Kind::Flac && p.flac)
    {
        j.key("flac");
        write_flac_json(j, *p.flac);
        j.end_object();
        return j.str();
    }
    j.key("created_by");
    if (p.created_by)
        j.value(*p.created_by);
    else
        j.null();
    j.field("zip64", p.zip64).field("entry_count", p.entry_count).field("file_count", p.file_count);
    j.field("total_size", p.total_size).field("packed_size", p.packed_size);
    j.field("restorable_audio", p.restorable_audio).field("restorable_audio_partial", p.restorable_audio_partial);
    j.field("truncated", p.truncated).key("entries").begin_array();
    for (const auto& e : p.entries)
    {
        j.begin_object().field("name", e.name).field("directory", e.directory).field("size", e.size).field("packed", e.packed).field("method", e.method);
        if (!e.restores_to.empty())
            j.field("restores_to", e.restores_to);
        j.end_object();
    }
    j.end_array().end_object();
    return j.str();
}

std::string preview_html(const Preview& p)
{
    std::string out = "<!doctype html><html><head><meta charset='utf-8'><title>";
    out += html_escape(p.file_name);
    out += "</title>";
    out += style();
    out += "</head><body>";
    out += std::format("<h1>{}</h1>", html_escape(p.file_name));
    if (p.kind == Preview::Kind::Flac && p.flac)
    {
        out += p.flac->layout.empty() ? "<p class='summary'>FLAC audio</p>" : "<p class='summary'>FLAC audio · <span class='badge'>restores to the original file</span></p>";
        html_flac(out, *p.flac);
        out += "</body></html>";
        return out;
    }
    std::string summary = std::format("{} file{} · {} → {} ({})", p.file_count, p.file_count == 1 ? "" : "s", human_size(p.total_size), human_size(p.packed_size), percent_smaller(p.total_size, p.packed_size));
    if (p.restorable_audio > 0)
        summary += std::format(" · <span class='badge'>{}{} audio file{} restorable</span>", p.restorable_audio_partial ? "at least " : "", p.restorable_audio, p.restorable_audio == 1 ? "" : "s");
    if (p.created_by)
        summary += " · created with " + html_escape(*p.created_by);
    out += std::format("<p class='summary'>{}</p>", summary);
    out += "<table><tr><th>Name</th><th class='num'>Size</th><th class='num'>Packed</th><th>Method</th></tr>";
    for (const auto& e : p.entries)
    {
        const auto name = html_escape(e.name) + (e.restores_to.empty() ? "" : " → " + html_escape(e.restores_to));
        if (e.directory)
            out += std::format("<tr><td class='name'>{}</td><td></td><td></td><td></td></tr>", name);
        else
            out += std::format("<tr><td class='name'>{}</td><td class='num'>{}</td><td class='num'>{}</td><td>{}</td></tr>", name, human_size(e.size), human_size(e.packed), html_escape(e.method));
    }
    out += "</table>";
    if (p.truncated)
        out += std::format("<p class='more'>… and {} more entries</p>", p.entry_count - p.entries.size());
    out += "</body></html>";
    return out;
}

}
