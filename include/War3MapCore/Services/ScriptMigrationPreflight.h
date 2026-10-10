#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Trigger/UiCatalog.h"

namespace War3Map::Services {

enum class ScriptMigrationTarget { ClassicJass, Lua };
enum class ScriptSourceLanguage { Unknown, Jass, Lua };

struct ScriptMigrationMember {
    std::string name;
    bool present = false; // 匿名成员存在时，未命名内容另由 coverage 阻断说明。
    std::size_t byteCount = 0;
    std::string sha256;
};

struct ScriptMigrationReport {
    // 仅已打开归档的基线；不包含未保存 overlay，不接受此报告充当保存许可。
    std::string sourceSha256;
    std::string catalogSha256;
    std::string reportIdentity;
    ScriptMigrationTarget target = ScriptMigrationTarget::ClassicJass;
    ScriptSourceLanguage sourceLanguage = ScriptSourceLanguage::Unknown;
    std::optional<std::int32_t> settingsVersion;
    std::vector<ScriptMigrationMember> members; // 固定白名单，不返回脚本文本或任意导入资源。
    bool triggerPairParsed = false;
    std::vector<Diagnostic> findings; // 上限 128；诊断原文上限 2048 字节。

    [[nodiscard]] bool Blocked() const noexcept;
};

// 纯只读预检：核对源 SHA，使用既有读取器；不执行代码、不创建输出、不改语言。
// 成功表示报告已完成；Blocked()==false 也必须经过正式构建/保存/原版/游戏门禁。
// catalog 可空，此时明确报告尚未检查 WTG/WCT，不因能读取脚本成员就当成可转换。
[[nodiscard]] Result<ScriptMigrationReport> InspectScriptMigration(
    const MapArchiveSession& archive, std::string_view expectedSourceSha256,
    ScriptMigrationTarget target, const Trigger::UiCatalog* catalog = nullptr);

} // namespace War3Map::Services
