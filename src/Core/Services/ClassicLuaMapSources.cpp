#include "War3MapCore/Services/ClassicLuaMapSources.h"

#include <algorithm>

namespace War3Map::Services {

Status VerifyClassicLuaGeneratedSources(const MapArchiveSession& archive,
    std::string_view expectedSourceSha256, ClassicLuaSourcePolicy policy) {
    const auto fail = [](std::string code, std::string message) {
        return Status::Failure("LuaMap.Source", std::move(code), std::move(message));
    };
    std::vector<std::string> names;
    for (const auto name : kClassicLuaGeneratedMembers) names.emplace_back(name);
    const auto source = archive.DescribeExactMemberIdentities(expectedSourceSha256, names);
    if (!source.Succeeded() || !source.value) return {source.diagnostics};
    const auto& identities = source.value->members;
    if (identities.size() != kClassicLuaGeneratedMembers.size())
        return fail("CLASSIC_LUA_SOURCE_IDENTITY", "经典Lua保留成员未取得完整来源身份。");
    if (!identities.back().present) {
        if (std::any_of(identities.begin() + 1, identities.end() - 1, [](const auto& m) { return m.present; }))
            return fail("CLASSIC_LUA_RESERVED_MEMBER_CONFLICT", "源地图已有同名Lua模块且无完整生成清单；保留原资源并拒绝覆盖。");
        if (policy == ClassicLuaSourcePolicy::AllowAbsentForFirstSave) return Status::Ok();
        return fail("CLASSIC_LUA_MANIFEST_REQUIRED", "地图缺少完整经典Lua生成清单；请使用经典1.27a + Lua档案重新保存后测试。");
    }
    std::string expected = "audastra-classic-lua-v1\n";
    for (std::size_t index = 0; index + 1 < identities.size(); ++index) {
        const auto& member = identities[index];
        if (!member.present || !member.sha256)
            return fail("CLASSIC_LUA_GENERATED_SOURCE_CHANGED", "上次生成的经典Lua入口或模块缺失，无法确认完整来源。");
        expected += std::string(kClassicLuaGeneratedMembers[index]) + "\t" + *member.sha256 + "\n";
    }
    ArchiveExactMemberLimits limits;
    limits.memberCount = 1; limits.memberBytes = 8192; limits.totalBytes = 8192;
    const std::array<std::string, 1> manifestName{std::string(kClassicLuaGeneratedMembers.back())};
    const auto manifest = archive.CaptureExactMembers(manifestName, limits);
    if (!manifest.Succeeded() || !manifest.value) return {manifest.diagnostics};
    if (manifest.value->size() != 1 || !manifest.value->front().present)
        return fail("CLASSIC_LUA_SOURCE_IDENTITY", "经典Lua生成清单未取得完整来源身份。");
    const auto& bytes = manifest.value->front().bytes;
    if (bytes.size() != expected.size() || !std::equal(bytes.begin(), bytes.end(),
            reinterpret_cast<const std::byte*>(expected.data())))
        return fail("CLASSIC_LUA_GENERATED_SOURCE_CHANGED", "生成的Lua入口、模块或清单被修改；拒绝使用未确认的内容。");
    return Status::Ok();
}

} // namespace War3Map::Services
