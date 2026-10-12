#include "War3MapCore/Services/ClassicLuaMapSources.h"
#include "War3MapCore/Hash.h"

#include <algorithm>

namespace War3Map::Services {

Status VerifyClassicLuaGeneratedIdentities(std::span<const ArchiveExactMemberIdentity> identities,
    std::span<const std::byte> manifestBytes, ClassicLuaSourcePolicy policy) {
    const auto fail = [](std::string code, std::string message) {
        return Status::Failure("LuaMap.Source", std::move(code), std::move(message));
    };
    if (identities.size() != kClassicLuaGeneratedMembers.size())
        return fail("CLASSIC_LUA_SOURCE_IDENTITY", "经典Lua保留成员未取得完整来源身份。");
    const auto validSha = [](const auto& sha) {
        return sha && sha->size() == 64 && std::all_of(sha->begin(), sha->end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    };
    for (std::size_t index = 0; index < identities.size(); ++index) {
        const auto& member = identities[index];
        if (member.name != kClassicLuaGeneratedMembers[index]
            || (member.present && (!member.byteSize || !validSha(member.sha256)))
            || (!member.present && (member.byteSize || member.sha256)))
            return fail("CLASSIC_LUA_SOURCE_IDENTITY", "经典Lua成员名称、顺序或实际字节身份不完整。");
    }
    if (!identities.back().present) {
        if (!manifestBytes.empty())
            return fail("CLASSIC_LUA_SOURCE_IDENTITY", "缺失清单的来源身份不能附带清单字节。");
        if (std::any_of(identities.begin() + 1, identities.end() - 1, [](const auto& m) { return m.present; }))
            return fail("CLASSIC_LUA_RESERVED_MEMBER_CONFLICT", "源地图已有同名Lua模块且无完整生成清单；保留原资源并拒绝覆盖。");
        if (policy == ClassicLuaSourcePolicy::AllowAbsentForFirstSave) return Status::Ok();
        return fail("CLASSIC_LUA_MANIFEST_REQUIRED", "地图缺少完整经典Lua生成清单；请使用经典1.27a + Lua档案重新保存后测试。");
    }
    if (manifestBytes.size() > 8192 || *identities.back().byteSize != manifestBytes.size())
        return fail("CLASSIC_LUA_SOURCE_IDENTITY", "经典Lua清单超过预算或与捕获的字节大小不符。");
    const auto manifestSha = Sha256(manifestBytes);
    if (!manifestSha.Succeeded() || !manifestSha.value) return {manifestSha.diagnostics};
    if (*manifestSha.value != *identities.back().sha256)
        return fail("CLASSIC_LUA_SOURCE_IDENTITY", "经典Lua清单字节与捕获的SHA不符。");
    std::string expected = "audastra-classic-lua-v1\n";
    for (std::size_t index = 0; index + 1 < identities.size(); ++index) {
        const auto& member = identities[index];
        if (!member.present || !member.sha256)
            return fail("CLASSIC_LUA_GENERATED_SOURCE_CHANGED", "上次生成的经典Lua入口或模块缺失，无法确认完整来源。");
        expected += std::string(kClassicLuaGeneratedMembers[index]) + "\t" + *member.sha256 + "\n";
    }
    if (manifestBytes.size() != expected.size() || !std::equal(manifestBytes.begin(), manifestBytes.end(),
            reinterpret_cast<const std::byte*>(expected.data())))
        return fail("CLASSIC_LUA_GENERATED_SOURCE_CHANGED", "生成的Lua入口、模块或清单被修改；拒绝使用未确认的内容。");
    return Status::Ok();
}

Status VerifyClassicLuaGeneratedSources(const MapArchiveSession& archive,
    std::string_view expectedSourceSha256, ClassicLuaSourcePolicy policy) {
    std::vector<std::string> names;
    for (const auto name : kClassicLuaGeneratedMembers) names.emplace_back(name);
    const auto source = archive.DescribeExactMemberIdentities(expectedSourceSha256, names);
    if (!source.Succeeded() || !source.value) return {source.diagnostics};
    if (source.value->members.size() != names.size() || !source.value->members.back().present)
        return VerifyClassicLuaGeneratedIdentities(source.value->members, {}, policy);
    ArchiveExactMemberLimits limits;
    limits.memberCount = 1; limits.memberBytes = 8192; limits.totalBytes = 8192;
    const std::array<std::string, 1> manifestName{std::string(kClassicLuaGeneratedMembers.back())};
    const auto manifest = archive.CaptureExactMembers(manifestName, limits);
    if (!manifest.Succeeded() || !manifest.value) return {manifest.diagnostics};
    if (manifest.value->size() != 1 || !manifest.value->front().present)
        return Status::Failure("LuaMap.Source", "CLASSIC_LUA_SOURCE_IDENTITY", "经典Lua生成清单未取得完整来源身份。");
    return VerifyClassicLuaGeneratedIdentities(source.value->members, manifest.value->front().bytes, policy);
}

} // namespace War3Map::Services
