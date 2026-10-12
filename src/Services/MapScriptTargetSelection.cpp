#include "War3MapCore/Services/MapScriptTargetSelection.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Script/LuaAuthorCode.h"

#include <algorithm>
#include <new>
#include <stdexcept>

namespace War3Map::Services {

struct PreparedLuaMapTarget::State {
    MapScriptTargetIdentity identity;
    Script::LuaMapBuildProfile profile;
    std::vector<std::byte> selectedSettings;
};

namespace {

using PreparedResult = Result<PreparedLuaMapTarget>;

PreparedResult Fail(std::string code, std::string message) {
    return PreparedResult::Failure("Script.TargetSelection", std::move(code), std::move(message));
}

bool SameBytes(std::span<const std::byte> left, std::span<const std::byte> right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}

bool Sha(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

bool KnownLanguage(const MapData::MapSettings& settings) {
    return settings.scriptLanguage == 0 || settings.scriptLanguage == 1;
}

// 长度前缀区分路径、参数边界和空项；沿用输入层序，不把有序请求重新排序。
class RequestFingerprint {
public:
    void Add(std::string_view value) {
        constexpr std::size_t maximumBytes = 2u * 1024u * 1024u;
        if (value.size() > maximumBytes || text_.size() > maximumBytes - value.size()
            || maximumBytes - text_.size() - value.size() < 24)
            throw std::length_error("Lua map target identity budget");
        text_ += std::to_string(value.size()); text_ += ':'; text_ += value;
    }
    void Number(std::uint64_t value) { Add(std::to_string(value)); }
    void Path(const std::filesystem::path& path) {
        const auto bytes = path.generic_u8string();
        Add(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    Result<std::string> Hash() const { return Sha256Text(text_); }
private:
    std::string text_;
};

Result<std::string> RequestHash(std::string_view profileHash, const LuaMapTargetRequest& request,
    const Script::LuaSyntaxCheckerIdentity* checker) {
    RequestFingerprint identity;
    identity.Add("forge-lua-map-target-request-v1");
    identity.Add(profileHash);
    identity.Number(static_cast<std::uint8_t>(request.playerLayout));
    identity.Number(request.changeLanguageToLua ? 1 : 0);
    if (request.dialect == Script::LuaMapSaveDialect::Classic127YDWE) identity.Add("classic-127-ydwe-v1");
    identity.Number(request.profile.target.nativeAuthorCode ? 1 : 0);
    if (request.profile.target.nativeAuthorCode) {
        const auto& author = *request.profile.target.nativeAuthorCode->Identity();
        identity.Add(author.sourceSettingsSha256); identity.Add(author.graphSha256);
    }
    if (request.dialect == Script::LuaMapSaveDialect::Classic127YDWE) {
        identity.Add("embedded-lua-5.3.6-text-compilation-v1");
    } else {
        identity.Path(checker->executable);
        identity.Add(checker->sha256);
        identity.Number(checker->bytes);
        identity.Number(checker->runtimeDirectories.size());
        for (const auto& directory : checker->runtimeDirectories) identity.Path(directory);
    }
    return identity.Hash();
}

} // namespace

const MapScriptTargetIdentity* PreparedLuaMapTarget::Identity() const noexcept {
    return state_ ? &state_->identity : nullptr;
}

const Script::LuaMapBuildProfile* PreparedLuaMapTarget::Profile() const noexcept {
    return state_ ? &state_->profile : nullptr;
}

std::span<const std::byte> PreparedLuaMapTarget::SelectedSettings() const noexcept {
    return state_ ? std::span<const std::byte>(state_->selectedSettings) : std::span<const std::byte>{};
}

Result<PreparedLuaMapTarget> PrepareMapScriptTargetSelection(
    std::span<const std::byte> sourceSettings, std::span<const std::byte> finalSettings,
    const Trigger::TriggerDocument& triggers, const Trigger::UiCatalog& catalog,
    const LuaMapTargetRequest& request, std::uint64_t documentRevision) {
    try {
        const auto source = MapData::MapSettings::Parse(sourceSettings);
        if (!source.Succeeded() || !source.value) return {{}, source.diagnostics};
        const auto final = MapData::MapSettings::Parse(finalSettings);
        if (!final.Succeeded() || !final.value) return {{}, final.diagnostics};
        if (request.dialect != Script::LuaMapSaveDialect::NativeV31
            && request.dialect != Script::LuaMapSaveDialect::Classic127YDWE)
            return Fail("LUA_MAP_TARGET_DIALECT_UNKNOWN", "未知Lua保存档案。");
        const bool classic = request.dialect == Script::LuaMapSaveDialect::Classic127YDWE;
        const auto expectedVersion = classic ? 25 : 31;
        // 直接解析两份真实输入，不能借用调用方改过formatVersion的结构取得升级资格。
        if (source.value->formatVersion != expectedVersion || final.value->formatVersion != expectedVersion
            || source.value->SourceFormatVersion() != expectedVersion || final.value->SourceFormatVersion() != expectedVersion)
            return Fail("LUA_MAP_TARGET_VERSION_UNSUPPORTED",
                "源和最终W3I必须与明确选择的Lua档案版本一致；不能在保存时自动升级。");
        if (classic && (request.playerLayout != Script::MapInitializationPlayerLayout::Classic12
            || request.changeLanguageToLua || request.profile.target.nativeAuthorCode
            || source.value->scriptLanguage != 0 || final.value->scriptLanguage != 0))
            return Fail("CLASSIC_LUA_TARGET_INPUT_UNSUPPORTED",
                "经典Lua档案保留真实v25/JASS设置，仅支持纯T与12槽；不能混入原生Lua作者代码或改W3I语言。");
        if (!KnownLanguage(*source.value) || !KnownLanguage(*final.value))
            return Fail("LUA_MAP_TARGET_LANGUAGE_UNKNOWN", "源或最终W3I的脚本语言不是已知的JASS/Lua档位。");
        if (request.profile.target.nativeAuthorCode) {
            if (source.value->scriptLanguage != 1)
                return Fail("LUA_AUTHOR_SOURCE_LANGUAGE", "经典 JASS 源不能借用其他地图的原生 Lua 作者代码凭据。");
            const auto* author = request.profile.target.nativeAuthorCode->Identity();
            const auto hash = Sha256(sourceSettings);
            if (!author || !hash.Succeeded() || !hash.value || author->sourceSettingsSha256 != *hash.value)
                return Fail("LUA_AUTHOR_SOURCE_IDENTITY", "原生 Lua 作者凭据与实际源 W3I 字节不一致。");
            const auto verified = request.profile.target.nativeAuthorCode->Verify(triggers, catalog);
            if (!verified.Succeeded()) return {{}, verified.diagnostics};
        }
        if (!classic && !request.changeLanguageToLua
            && (source.value->scriptLanguage != 1 || final.value->scriptLanguage != 1))
            return Fail("LUA_MAP_TARGET_LANGUAGE_SELECTION_REQUIRED",
                "源和最终W3I未同时声明Lua；须显式选择v31内的语言切换，不能静默改脚本语言。");
        if (request.playerLayout != Script::MapInitializationPlayerLayout::Classic12
            && request.playerLayout != Script::MapInitializationPlayerLayout::Extended24)
            return Fail("LUA_MAP_TARGET_PLAYER_LAYOUT_UNKNOWN", "未知玩家槽位档位；目标选择不猜测或迁移玩家编号。");
        const auto* checker = request.checker.Identity();
        if (!classic && (!checker || checker->executable.empty() || checker->bytes == 0 || !Sha(checker->sha256)))
            return Fail("LUA_MAP_TARGET_CHECKER_UNCAPTURED", "Lua检查器尚未捕获完整文件身份，不能冻结保存目标。");
        if (request.profile.target.identity.empty())
            return Fail("LUA_MAP_TARGET_PROFILE_MISSING", "Lua目标档案没有明确身份。");

        // 复用已存在的档案/显式回调指纹和T输入seal检查，不另造第二套API清单散列。
        const auto triggerInput = PrepareLuaTriggerInput(triggers, catalog, request.profile);
        if (!triggerInput.Succeeded() || !triggerInput.value) return {{}, triggerInput.diagnostics};
        const auto* triggerIdentity = triggerInput.value->Identity();
        if (!triggerIdentity || !Sha(triggerIdentity->targetRequestSha256))
            return Fail("LUA_MAP_TARGET_PROFILE_IDENTITY", "无法取得完整Lua档案及回调请求身份。");
        const auto requestHash = RequestHash(triggerIdentity->targetRequestSha256, request, checker);
        if (!requestHash.Succeeded() || !requestHash.value) return {{}, requestHash.diagnostics};

        auto selected = *final.value;
        if (!classic) selected.scriptLanguage = 1;
        const auto serialized = selected.Serialize();
        if (!serialized.Succeeded() || !serialized.value) return {{}, serialized.diagnostics};
        const auto reparsed = MapData::MapSettings::Parse(*serialized.value);
        if (!reparsed.Succeeded() || !reparsed.value) return {{}, reparsed.diagnostics};
        if (reparsed.value->formatVersion != expectedVersion || reparsed.value->SourceFormatVersion() != expectedVersion
            || reparsed.value->scriptLanguage != (classic ? 0 : 1) || reparsed.value->UnknownTail() != final.value->UnknownTail())
            return Fail("LUA_MAP_TARGET_SETTINGS_ROUNDTRIP", "选中W3I回读的格式、语言或不透明尾部不一致。");

        // 回读后只还原语言，再与最终输入逐字节比较。这样覆盖全部其余字段，
        // 包括游戏版本、玩家编号、未知标志、随机表和不透明尾部，防止将来漏加字段比较。
        auto restored = *reparsed.value;
        restored.scriptLanguage = final.value->scriptLanguage;
        const auto restoredBytes = restored.Serialize();
        if (!restoredBytes.Succeeded() || !restoredBytes.value
            || !SameBytes(*restoredBytes.value, finalSettings))
            return Fail("LUA_MAP_TARGET_SETTINGS_CHANGED", "选择Lua时除脚本语言外的W3I字节发生变化；未返回目标快照。");
        if ((classic || final.value->scriptLanguage == 1) && !SameBytes(*serialized.value, finalSettings))
            return Fail("LUA_MAP_TARGET_SETTINGS_CHANGED", "已为Lua的设置未保持输入原字节，拒绝隐式重写。");

        const auto sourceHash = Sha256(sourceSettings), finalHash = Sha256(finalSettings), selectedHash = Sha256(*serialized.value);
        if (!sourceHash.Succeeded() || !finalHash.Succeeded() || !selectedHash.Succeeded()
            || !sourceHash.value || !finalHash.value || !selectedHash.value)
            return Fail("LUA_MAP_TARGET_HASH_FAILED", "无法计算源、最终输入或选中W3I的完整指纹。");
        PreparedLuaMapTarget::State state;
        state.identity = {documentRevision, *sourceHash.value, *finalHash.value, *selectedHash.value,
            *requestHash.value, static_cast<std::uint32_t>(source.value->scriptLanguage)};
        state.profile.target = request.profile.target;
        state.profile.callbacks = request.profile.callbacks;
        state.profile.initialization.playerLayout = request.playerLayout;
        state.profile.checker = request.checker;
        state.profile.requestSha256 = *requestHash.value;
        state.profile.dialect = request.dialect;
        state.selectedSettings = final.value->scriptLanguage == 1
            ? std::vector<std::byte>(finalSettings.begin(), finalSettings.end()) : *serialized.value;
        PreparedLuaMapTarget prepared;
        prepared.state_ = std::make_shared<const PreparedLuaMapTarget::State>(std::move(state));
        auto result = PreparedResult::Success(std::move(prepared));
        result.diagnostics = source.diagnostics;
        if (!SameBytes(sourceSettings, finalSettings))
            result.diagnostics.insert(result.diagnostics.end(), final.diagnostics.begin(), final.diagnostics.end());
        return result;
    } catch (const std::length_error&) {
        return Fail("LUA_MAP_TARGET_BUDGET", "目标身份或设置超出冻结预算；没有可用的部分目标。");
    } catch (const std::bad_alloc&) {
        return Fail("LUA_MAP_TARGET_ALLOCATION", "目标选择无法分配独立快照。");
    } catch (const std::exception&) {
        return Fail("LUA_MAP_TARGET_FAILED", "目标选择失败，未发布设置或脚本变更。");
    }
}

} // namespace War3Map::Services
