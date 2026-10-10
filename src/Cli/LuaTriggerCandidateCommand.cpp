#include "LuaTriggerCandidateCommand.h"
#include "War3MapCore/Services/LuaTriggerCandidate.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <iostream>
#include <charconv>
#include <cstdio>
#include <fcntl.h>
#include <io.h>

namespace {
using namespace War3Map;
void Errors(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
struct Source {
    std::filesystem::path path;
    std::vector<std::byte> bytes;
    std::string sha;
};
std::optional<Source> Read(const std::filesystem::path& path, std::size_t limit) {
    auto bytes = IO::ReadFileBytes(path, limit);
    if (!bytes.Succeeded()) { Errors(bytes.diagnostics); return {}; }
    auto sha = Sha256(*bytes.value);
    if (!sha.Succeeded()) { Errors(sha.diagnostics); return {}; }
    return Source{path, std::move(*bytes.value), std::move(*sha.value)};
}
bool Same(const Source& source) {
    const auto sha = Sha256File(source.path);
    return sha.Succeeded() && *sha.value == source.sha;
}

std::string_view Kind(Script::LuaSourceSpanKind kind) {
    using enum Script::LuaSourceSpanKind;
    switch (kind) {
    case Variable: return "variable";
    case Trigger: return "trigger";
    case Event: return "event";
    case Statement: return "statement";
    case Callback: return "callback";
    }
    return "unknown";
}
void Span(const Script::LuaSourceSpan& span) {
    std::cout << span.byteOffset << '\t' << span.byteLength << '\t' << span.triggerId << '\t'
        << span.nodeId << '\t' << Kind(span.kind) << '\n';
}
void SourcePage(const Services::LuaTriggerSourcePage& page) {
    std::cout << "schema\tforge-lua-source-map-v1\nscope\ttrigger-unit\ncomplete_map\tfalse\nruntime_verified\tfalse\n"
        << "granularity\tstatement-or-trigger\ninput_revision\t" << page.inputRevision
        << "\nscript_sha256\t" << page.scriptSha256 << "\nsource_map_sha256\t" << page.sourceMapSha256 << "\nline\t" << page.line
        << "\nline_byte_offset\t" << page.lineByteOffset << "\nline_byte_length\t" << page.lineByteLength
        << "\noffset\t" << page.offset << "\ntotal_spans\t" << page.totalSpans << "\ntotal_matches\t" << page.totalMatches
        << "\nreturned\t" << page.spans.size() << '\n';
    if (page.innermost) { std::cout << "innermost\t"; Span(*page.innermost); }
    else std::cout << "innermost\tunmapped\n";
    for (const auto& span : page.spans) { std::cout << "span\t"; Span(span); }
}
}

int RunLuaTriggerCandidate(const std::filesystem::path& wtgPath, const std::filesystem::path& wctPath,
    const std::filesystem::path& uiRoot, const std::filesystem::path& profilePath, std::string_view revisionOrInspect,
    std::span<const std::string> options) {
    using namespace War3Map::Services;
    std::size_t line = 0, offset = 0, limit = 0;
    if (!options.empty()) {
        const auto number = [](const std::string& text, std::size_t& value) {
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size();
        };
        if (options.size() != 5 || options[0] != "--sources" || revisionOrInspect == "inspect"
            || !number(options[2], line) || !number(options[3], offset) || !number(options[4], limit)) {
            std::cerr << "LUA_CANDIDATE_ARGUMENTS: source query requires revision, script SHA, line, offset and limit\n";
            return 2;
        }
    }
    const auto wtg = Read(wtgPath, 32 * 1024 * 1024), wct = Read(wctPath, 32 * 1024 * 1024);
    const auto profileBytes = Read(profilePath, 1024 * 1024);
    if (!wtg || !wct || !profileBytes) return 1;
    const auto profile = ParseLuaCandidateProfile(std::string_view(
        reinterpret_cast<const char*>(profileBytes->bytes.data()), profileBytes->bytes.size()));
    if (!profile.Succeeded()) { Errors(profile.diagnostics); return 1; }
    const auto catalog = Trigger::UiCatalog::Load(uiRoot);
    if (!catalog.Succeeded()) { Errors(catalog.diagnostics); return 1; }
    const auto document = Trigger::WtgWctCodec{}.Parse(wtg->bytes, wct->bytes, *catalog.value);
    if (!document.Succeeded()) { Errors(document.diagnostics); return 1; }
    const auto prepared = PrepareLuaTriggerInput(*document.value, *catalog.value, *profile.value);
    if (!prepared.Succeeded()) { Errors(prepared.diagnostics); return 1; }
    const auto identity = prepared.value->Identity();
    std::optional<LuaTriggerCandidate> candidate;
    if (revisionOrInspect != "inspect") {
        auto compiled = CompileLuaTriggerCandidate(*prepared.value, revisionOrInspect);
        if (!compiled.Succeeded()) {
            if (compiled.sourceDiagnostics.empty()) Errors(compiled.diagnostics);
            else for (const auto& item : compiled.sourceDiagnostics)
                std::cerr << item.diagnostic.code << " [trigger=" << item.triggerId << ", node=" << item.nodeId
                    << "]: " << item.diagnostic.message << '\n';
            return 1;
        }
        candidate = std::move(*compiled.value);
    }
    // 完整生成后才输出；复核磁盘输入和重载 Catalog，不能返回半脚本或旧修订结果。
    const auto currentCatalog = Trigger::UiCatalog::Load(uiRoot);
    if (!Same(*wtg) || !Same(*wct) || !Same(*profileBytes) || !currentCatalog.Succeeded()
        || currentCatalog.value->Sha256() != identity->catalogSha256) {
        std::cerr << "LUA_INPUT_STALE: sources changed while compiling\n"; return 1;
    }
    if (!candidate) {
        std::cout << "schema\tforge-lua-trigger-input-v1\nscope\ttrigger-unit\ncomplete_map\tfalse\nruntime_verified\tfalse\n"
            << "input_revision\t" << identity->revision << "\ncurrent_wtg_sha256\t" << identity->wtgSha256
            << "\ncurrent_wct_sha256\t" << identity->wctSha256 << "\nsource_wtg_sha256\t" << identity->sourceWtgSha256
            << "\nsource_wct_sha256\t" << identity->sourceWctSha256 << "\ncatalog_sha256\t" << identity->catalogSha256
            << "\ntarget_request_sha256\t" << identity->targetRequestSha256 << "\ndisposition\tprepared-uncompiled\n";
    } else if (!options.empty()) {
        const auto page = QueryLuaTriggerSources(*candidate, identity->revision, options[1], line, offset, limit);
        if (!page.Succeeded()) { Errors(page.diagnostics); return 1; }
        SourcePage(*page.value);
    } else {
        // 以二进制块输出，Windows CRT 不可改写换行后仍报告原脚本 SHA。
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
            std::cerr << "LUA_OUTPUT_MODE: cannot enable binary stdout\n"; return 1;
        }
        const auto unit = candidate->Unit();
        std::cout.write(unit->text.data(), static_cast<std::streamsize>(unit->text.size()));
        std::cerr << "scope=trigger-unit complete_map=false runtime_verified=false input_revision=" << identity->revision
            << " script_sha256=" << unit->scriptSha256 << '\n';
    }
    return std::cout ? 0 : 1;
}
