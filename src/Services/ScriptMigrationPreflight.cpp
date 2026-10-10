#include "War3MapCore/Services/ScriptMigrationPreflight.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <algorithm>
#include <array>
#include <map>
#include <new>

namespace War3Map::Services {
namespace {
constexpr std::string_view kMembers[]{
    "war3map.w3i", "war3mapUnits.doo", "war3map.doo", "war3map.w3r", "war3map.w3c", "war3map.w3s", "war3map.wts",
    "war3map.wtg", "war3map.wct", "war3map.j", "scripts\\war3map.j", "war3map.lua", "scripts\\war3map.lua",
    "war3map.w3u", "war3map.w3a", "war3map.w3t", "war3map.w3h", "war3map.w3q", "war3map.w3d", "war3map.w3b",
    "war3mapSkin.w3u", "war3mapSkin.w3a", "war3mapSkin.w3t", "war3mapSkin.w3h", "war3mapSkin.w3q", "war3mapSkin.w3d", "war3mapSkin.w3b"};
constexpr ObjectData::ObjectType kObjectTypes[]{ObjectData::ObjectType::Unit, ObjectData::ObjectType::Ability,
    ObjectData::ObjectType::Item, ObjectData::ObjectType::Buff, ObjectData::ObjectType::Upgrade,
    ObjectData::ObjectType::Doodad, ObjectData::ObjectType::Destructable};
constexpr std::size_t kTotalBytes = 64 * 1024 * 1024;

bool Sha(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
std::string Bounded(std::string text) {
    if (text.size() <= 2048) return text;
    std::size_t end = 2045;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
    text.resize(end); return text + "...";
}
void Add(ScriptMigrationReport& report, Diagnostic diagnostic) {
    if (report.findings.size() < 127) {
        diagnostic.message = Bounded(std::move(diagnostic.message));
        report.findings.push_back(std::move(diagnostic));
    } else if (report.findings.size() == 127) {
        report.findings.push_back({DiagnosticSeverity::Error, "Script.Migration", "MIGRATION_FINDINGS_LIMIT",
            "诊断超过 127 项；本报告不完整，不可推断其余内容可转换。", {}});
    }
}
void Finding(ScriptMigrationReport& report, std::string code, std::string message,
    DiagnosticSeverity severity = DiagnosticSeverity::Error) {
    Add(report, {severity, "Script.Migration", std::move(code), std::move(message), {}});
}
void CopyFindings(ScriptMigrationReport& report, const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) Add(report, diagnostic);
}
}

bool ScriptMigrationReport::Blocked() const noexcept {
    return std::any_of(findings.begin(), findings.end(), [](const auto& item) {
        return item.severity == DiagnosticSeverity::Error;
    });
}

Result<ScriptMigrationReport> InspectScriptMigration(const MapArchiveSession& archive,
    std::string_view expectedSourceSha256, ScriptMigrationTarget target, const Trigger::UiCatalog* catalog) {
    const auto fail = [](std::string code, std::string message) {
        return Result<ScriptMigrationReport>::Failure("Script.Migration", std::move(code), std::move(message));
    };
    if (!Sha(expectedSourceSha256) || expectedSourceSha256 != archive.SourceSha256())
        return fail("MIGRATION_SOURCE_STALE", "请求未绑定当前归档的 SHA-256。");
    if (target != ScriptMigrationTarget::ClassicJass && target != ScriptMigrationTarget::Lua)
        return fail("MIGRATION_TARGET_INVALID", "未知迁移目标。");
    const auto verified = archive.VerifySourceUnchanged();
    if (!verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    try {
        ScriptMigrationReport report;
        report.sourceSha256 = archive.SourceSha256(); report.target = target;
        if (catalog) report.catalogSha256 = catalog->Sha256();
        // 仅读取固定候选成员；源身份与读取后的再核对共同约束本次报告。
        std::map<std::string, std::vector<std::byte>, std::less<>> bytes;
        std::size_t retained = 0;
        for (const auto name : kMembers) {
            ScriptMigrationMember member{std::string(name)};
            member.present = archive.HasMember(name);
            if (member.present) {
                const auto entry = std::find_if(archive.Members().begin(), archive.Members().end(),
                    [&](const auto& item) { return item.name.size() == name.size()
                        && std::equal(name.begin(), name.end(), item.name.begin(), [](char a, char b) {
                            const auto canonical = [](char c) { return c == '/' ? '\\' : c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
                            return canonical(a) == canonical(b);
                        }); });
                if (entry != archive.Members().end() && entry->data.size() > kTotalBytes - retained)
                    return fail("MIGRATION_SOURCE_BUDGET", "固定成员总量超过 64 MiB，未生成报告。");
                auto read = archive.ReadMember(name);
                if (!read.Succeeded()) { CopyFindings(report, read.diagnostics); report.members.push_back(std::move(member)); continue; }
                if (read.value->size() > kTotalBytes - retained)
                    return fail("MIGRATION_SOURCE_BUDGET", "固定成员总量超过 64 MiB，未生成报告。");
                retained += read.value->size(); member.byteCount = read.value->size();
                auto hash = Sha256(*read.value);
                if (!hash.Succeeded()) return {std::nullopt, std::move(hash.diagnostics)};
                member.sha256 = std::move(*hash.value);
                bytes.emplace(name, std::move(*read.value));
            }
            report.members.push_back(std::move(member));
        }
        if (archive.HasOpaqueMembers()) Finding(report, "MIGRATION_SOURCE_COVERAGE_UNKNOWN",
            "归档含匿名成员；命名白名单不是完整内容审计，不能据缺失推断所有编辑源不存在。");
        std::vector<MapArchiveMember> mapMembers;
        for (std::size_t i = 0; i < 7; ++i) {
            const auto& member = report.members[i];
            if (bytes.contains(member.name)) mapMembers.push_back({member.name, bytes.at(member.name)});
        }
        const auto map = MapDocument::FromMembers(mapMembers, archive.SourcePath());
        CopyFindings(report, map.InitializationSourceStatus().diagnostics);
        for (const auto& member : map.Members()) {
            if (member.state == MapDataState::Missing) continue;
            CopyFindings(report, member.diagnostics);
            if (member.state != MapDataState::Ready)
                Finding(report, "MIGRATION_MEMBER_INCOMPLETE", member.memberName + " 没有完整可解释的来源。");
        }
        if (!map.Settings()) Finding(report, "MIGRATION_SETTINGS_UNAVAILABLE", "没有可解析的 W3I，不能推断脚本语言。");
        else {
            const auto& settings = *map.Settings(); report.settingsVersion = settings.formatVersion;
            if (settings.formatVersion == 18 || settings.formatVersion == 25) report.sourceLanguage = ScriptSourceLanguage::Jass;
            else if (settings.Layout() && settings.Layout()->scriptLanguage
                && (settings.scriptLanguage == 0 || settings.scriptLanguage == 1))
                report.sourceLanguage = settings.scriptLanguage == 0 ? ScriptSourceLanguage::Jass : ScriptSourceLanguage::Lua;
            else Finding(report, "MIGRATION_LANGUAGE_UNKNOWN", "W3I 的脚本语言或版本不在已实现读取档案内。");
            if (target == ScriptMigrationTarget::ClassicJass && settings.formatVersion != 25)
                Finding(report, "MIGRATION_CLASSIC_FORMAT_REQUIRED", "现有经典整图构建仅接受 W3I v25；未实现新版降级转换。");
        }
        const auto present = [&](std::string_view name) { return archive.HasMember(name); };
        const int jass = static_cast<int>(present("war3map.j")) + static_cast<int>(present("scripts\\war3map.j"));
        const int lua = static_cast<int>(present("war3map.lua")) + static_cast<int>(present("scripts\\war3map.lua"));
        if (jass > 1 || lua > 1 || (jass && lua)) Finding(report, "MIGRATION_SCRIPT_MEMBERS_CONFLICT",
            "脚本成员有多个候选或两种语言并存；不能任取一个或删除另一个。");
        if ((report.sourceLanguage == ScriptSourceLanguage::Jass && lua)
            || (report.sourceLanguage == ScriptSourceLanguage::Lua && jass))
            Finding(report, "MIGRATION_LANGUAGE_MEMBER_MISMATCH", "W3I 语言与已有脚本成员冲突。");
        if (!jass && !lua) Finding(report, "MIGRATION_GAME_SCRIPT_MISSING", "没有已有游戏脚本；后续需要完整构建。", DiagnosticSeverity::Warning);
        if (target == ScriptMigrationTarget::Lua) {
            // 仅识别作者声明的精确导入对，不读取载荷，不按扩展名检查导入脚本。
            if (present("tiara.ai") && present("tiara.blp"))
                Finding(report, "MIGRATION_LUA_MEMHACK_UNSUPPORTED",
                    "地图携带 MemHack 导入 AI 与配对载荷 tiara.ai / tiara.blp；其运行依赖主 JASS 初始化/钩子与内存/DLL 语义，尚不能等价迁移到 Lua。组件存在不证明游戏中已初始化或功能生效。");
            Finding(report, "MIGRATION_LUA_MAP_BACKEND_UNAVAILABLE",
                "Lua 触发器单元不包含整图 config/main 和对象初始化，也没有原版/联机验证，不能作为地图迁移产物。");
        } else if (report.sourceLanguage == ScriptSourceLanguage::Lua)
            Finding(report, "MIGRATION_LUA_TO_JASS_UNAVAILABLE", "尚未实现 Lua 到经典 JASS 的语义转换。");
        if (target == ScriptMigrationTarget::ClassicJass) {
            if ((map.Units() && map.Units()->recordLayout != MapData::PlacementRecordLayout::Classic)
                || (map.Doodads() && map.Doodads()->recordLayout != MapData::PlacementRecordLayout::Classic))
                Finding(report, "MIGRATION_SKIN_PLACEMENT_UNSUPPORTED", "SkinId 实例仅只读，经典生成器不能忽略 skinId。");
            for (std::size_t i = 20; i < std::size(kMembers); ++i)
                if (present(kMembers[i])) Finding(report, "MIGRATION_SKIN_MEMBER_UNSUPPORTED",
                    std::string(kMembers[i]) + " 是独立成员，尚未实现经典降级规则。");
        }
        for (const auto type : kObjectTypes) {
            const std::string name(ObjectData::NativeFileName(type));
            if (!bytes.contains(name)) continue;
            const auto parsed = ObjectData::NativeObjectCodec(type).Parse(bytes.at(name));
            CopyFindings(report, parsed.diagnostics);
            if (!parsed.Succeeded()) Finding(report, "MIGRATION_OBJECT_SOURCE_UNPARSED", name + " 无法由当前物编读取器完整解析。");
            else if (const auto* native = dynamic_cast<const ObjectData::NativeObjectDocument*>(parsed.document.get());
                !native || !native->IsStructurallySafe() || !native->IsEditable())
                Finding(report, "MIGRATION_OBJECT_SOURCE_READONLY", name + " 只能保留读取，尚无可用迁移写入规则。");
        }
        if (!bytes.contains("war3map.wtg") || !bytes.contains("war3map.wct"))
            Finding(report, "MIGRATION_TRIGGER_PAIR_MISSING", "缺少可读 WTG/WCT 成对编辑源；游戏脚本不能替代 GUI 源。");
        else if (!catalog) Finding(report, "MIGRATION_UI_CATALOG_REQUIRED", "未提供函数库目录，WTG/WCT 尚未进行成对解析。");
        else {
            const auto pair = Trigger::WtgWctCodec{}.Parse(bytes.at("war3map.wtg"), bytes.at("war3map.wct"), *catalog);
            CopyFindings(report, pair.diagnostics); report.triggerPairParsed = pair.Succeeded();
            if (!pair.Succeeded()) Finding(report, "MIGRATION_TRIGGER_PAIR_UNPARSED", "当前读取器不能完整解释这份 WTG/WCT。");
            else if (target == ScriptMigrationTarget::Lua) {
                if (!pair.value->wct.customCode.text.empty()) Finding(report, "MIGRATION_CUSTOM_SOURCE_UNTRANSLATED", "地图自定义代码尚未进行语言转换。");
                for (const auto& trigger : pair.value->triggers)
                    if (trigger.customTextFlag || !trigger.customText.text.empty()) Add(report,
                        {DiagnosticSeverity::Error, "Script.Migration", "MIGRATION_CUSTOM_SOURCE_UNTRANSLATED",
                            "触发器 " + std::to_string(trigger.id) + " 含自定义代码（禁用源也保留）。", trigger.origin});
            }
        }
        Finding(report, "MIGRATION_VALIDATION_REQUIRED", "报告只覆盖归档基线；未保存编辑需重新捕获。预检无阻断也必须走正式构建/保存及目标验收。", DiagnosticSeverity::Info);
        const auto identity = Sha256Text("forge-script-migration-v2:" + report.sourceSha256 + ":"
            + std::to_string(static_cast<int>(target)) + ":" + (catalog ? "catalog:" : "no-catalog:") + report.catalogSha256);
        if (!identity.Succeeded()) return {std::nullopt, identity.diagnostics};
        report.reportIdentity = *identity.value;
        const auto after = archive.VerifySourceUnchanged();
        if (!after.Succeeded()) return {std::nullopt, after.diagnostics};
        return Result<ScriptMigrationReport>::Success(std::move(report));
    } catch (const std::bad_alloc&) { return fail("MIGRATION_ALLOCATION_FAILED", "迁移预检内存不足，未生成报告。"); }
}
} // namespace War3Map::Services
