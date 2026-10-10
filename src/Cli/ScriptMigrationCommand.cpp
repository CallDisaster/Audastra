#include "ScriptMigrationCommand.h"
#include "War3MapCore/Services/ScriptMigrationPreflight.h"
#include "War3MapCore/UiLibrary/UiLibrary.h"

#include <iostream>

namespace {
// TSV v1：反斜杠、TAB、CR、LF 转义；UTF-8 原样保留，不返回脚本内容。
std::string Cell(std::string_view value) {
    std::string result;
    for (const char c : value) {
        switch (c) {
        case '\\': result += "\\\\"; break;
        case '\t': result += "\\t"; break;
        case '\r': result += "\\r"; break;
        case '\n': result += "\\n"; break;
        default: result += c; break;
        }
    }
    return result;
}
void Errors(const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
}
}

int RunScriptMigrationCheck(const std::filesystem::path& map, std::string_view sourceSha256,
    std::string_view targetName, const std::optional<std::filesystem::path>& toolRoot) {
    using namespace War3Map;
    using namespace War3Map::Services;
    if (targetName != "classic-jass" && targetName != "lua") {
        std::cerr << "target must be classic-jass or lua\n"; return 1;
    }
    auto opened = MapArchiveSession::Open(map);
    if (!opened.Succeeded()) { Errors(opened.diagnostics); return 1; }
    std::optional<Trigger::UiCatalog> catalog;
    if (toolRoot) {
        auto packages = UiLibrary::DiscoverPackages(*toolRoot);
        if (!packages.Succeeded()) { Errors(packages.diagnostics); return 1; }
        const auto layers = UiLibrary::EffectiveLayers(UiLibrary::ReconcileGlobal({}, *packages.value), {}, *packages.value);
        auto loaded = Trigger::UiCatalog::LoadLayers(*toolRoot, layers);
        if (!loaded.Succeeded()) { Errors(loaded.diagnostics); return 1; }
        catalog = std::move(*loaded.value);
    }
    const auto inspected = InspectScriptMigration(*opened.value, sourceSha256,
        targetName == "lua" ? ScriptMigrationTarget::Lua : ScriptMigrationTarget::ClassicJass,
        catalog ? &*catalog : nullptr);
    if (!inspected.Succeeded()) { Errors(inspected.diagnostics); return 1; }
    const auto& report = *inspected.value;
    std::cout << "schema\tforge-script-migration-tsv-v1\n"
        << "scope\tarchive-baseline\nsource_sha256\t" << report.sourceSha256
        << "\nreport_identity\t" << report.reportIdentity << "\ncatalog_sha256\t" << report.catalogSha256
        << "\ntarget\t" << targetName << "\nsource_language\t"
        << (report.sourceLanguage == ScriptSourceLanguage::Jass ? "jass" : report.sourceLanguage == ScriptSourceLanguage::Lua ? "lua" : "unknown")
        << "\nsettings_version\t" << (report.settingsVersion ? std::to_string(*report.settingsVersion) : "unknown")
        << "\ntrigger_pair_parsed\t" << (report.triggerPairParsed ? "true" : "false")
        << "\ndisposition\t" << (report.Blocked() ? "blocked" : "requires-build-and-validation") << '\n';
    for (const auto& member : report.members)
        std::cout << "member\t" << Cell(member.name) << '\t' << (member.present ? "present" : "not-named")
            << '\t' << member.byteCount << '\t' << member.sha256 << '\n';
    for (const auto& finding : report.findings)
        std::cout << "finding\t" << (finding.severity == DiagnosticSeverity::Error ? "error" : finding.severity == DiagnosticSeverity::Warning ? "warning" : "info")
            << '\t' << Cell(finding.code) << '\t' << Cell(finding.message) << '\n';
    return report.Blocked() ? 2 : 0;
}
