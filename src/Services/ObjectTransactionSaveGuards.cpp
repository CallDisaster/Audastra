#include "War3MapCore/Services/ObjectTransactionSaveGuards.h"

#include "War3MapCore/Services/ObjectTransactionSnapshot.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <array>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace War3Map::Services {
namespace {

constexpr std::array<std::string_view, 7> kNativeNames{
    "war3map.w3u", "war3map.w3a", "war3map.w3t", "war3map.w3h",
    "war3map.w3q", "war3map.w3d", "war3map.w3b"};
constexpr std::array<std::string_view, 7> kNativeKeys{
    "WAR3MAP.W3U", "WAR3MAP.W3A", "WAR3MAP.W3T", "WAR3MAP.W3H",
    "WAR3MAP.W3Q", "WAR3MAP.W3D", "WAR3MAP.W3B"};

Status Reject(std::string code, std::string message) {
    return Status::Failure("MapSaveService", std::move(code), std::move(message));
}

} // namespace

Status ValidateGeneratedObjectMembersForSave(const MapArchiveSession& source,
    std::span<const std::shared_ptr<const ObjectTransactionSnapshot>> snapshots,
    std::span<const MemberReplacement> generated, std::string_view scriptMember) {
    try {
        if (snapshots.size() > kNativeNames.size())
            return Reject("SAVE_OBJECT_TRANSACTION_COUNT", "封印物编类别不能超过七类。");
        if (generated.size() > kNativeNames.size() + 1)
            return Reject("SAVE_GENERATED_OBJECT_MEMBER_LIMIT", "模板提交只允许七类物编和一个已检查脚本。");
        if (scriptMember.size() > 1024)
            return Reject("SAVE_GENERATED_SCRIPT_MEMBER_INVALID", "脚本成员路径超过 1024 字节预算。");
        const auto script = Resources::CanonicalImportPath(scriptMember);
        if (!script.Succeeded() || !script.value || (*script.value != "WAR3MAP.J"
            && *script.value != "SCRIPTS\\WAR3MAP.J" && *script.value != "WAR3MAP.LUA"))
            return Reject("SAVE_GENERATED_SCRIPT_MEMBER_INVALID", "模板提交没有明确的受支持脚本成员身份。");
        std::map<std::size_t, const ObjectTransactionSnapshot*> sealed;
        std::set<std::size_t> requested;
        for (const auto& snapshot : snapshots) {
            if (!snapshot)
                return Reject("SAVE_OBJECT_TRANSACTION_INVALID", "物编封印为空，未核验模板提交。");
            const auto category = static_cast<std::size_t>(snapshot->Type());
            if (category >= kNativeNames.size())
                return Reject("SAVE_OBJECT_TRANSACTION_INVALID", "物编封印不属于七类 native 格式。");
            if (!sealed.emplace(category, snapshot.get()).second)
                return Reject("SAVE_OBJECT_TRANSACTION_DUPLICATE", "同一物编类别不能授予两个模板提交封印。");
            requested.insert(category);
        }
        std::map<std::size_t, const MemberReplacement*> generatedObjects;
        std::set<std::string> seen;
        std::size_t generatedBytes = 0;
        for (const auto& member : generated) {
            if (member.name.size() > 1024)
                return Reject("SAVE_GENERATED_OBJECT_MEMBER_LIMIT", "模板成员路径超过 1024 字节预算。");
            const auto key = Resources::CanonicalImportPath(member.name);
            if (!key.Succeeded() || !key.value)
                return Reject("SAVE_GENERATED_OBJECT_MEMBER_UNSUPPORTED", "模板生成成员路径不受支持：" + member.name);
            if (!seen.insert(*key.value).second)
                return Reject("SAVE_GENERATED_OBJECT_MEMBER_DUPLICATE", "模板重复生成同一归档成员：" + member.name);
            if (*key.value == *script.value) continue;
            const auto found = std::find(kNativeKeys.begin(), kNativeKeys.end(), *key.value);
            if (found == kNativeKeys.end())
                return Reject("SAVE_GENERATED_OBJECT_MEMBER_UNSUPPORTED", "模板输出超出本次脚本和七类物编域：" + member.name);
            if (member.data.size() > ObjectTransactionSnapshot::kMaximumMemberBytes - generatedBytes)
                return Reject("SAVE_GENERATED_OBJECT_MEMBER_LIMIT", "模板生成物编超过 64 MiB 累积预算。");
            generatedBytes += member.data.size();
            const auto category = static_cast<std::size_t>(found - kNativeKeys.begin());
            generatedObjects.emplace(category, &member);
            requested.insert(category);
        }
        // 一个明确名字集合由同一源租约捕获，匿名块也不能误当作 Missing。
        std::vector<std::string> names;
        std::vector<std::size_t> categories;
        for (const auto category : requested) {
            names.emplace_back(kNativeNames[category]);
            categories.push_back(category);
        }
        const auto captured = source.CaptureExactMembers(names);
        if (!captured.Succeeded() || !captured.value) return Status{captured.diagnostics};
        if (captured.value->size() != categories.size())
            return Reject("SAVE_GENERATED_OBJECT_SOURCE_INCOMPLETE", "明确物编来源捕获没有返回完整身份集合。");
        for (std::size_t index = 0; index < categories.size(); ++index) {
            const auto category = categories[index];
            const auto& original = (*captured.value)[index];
            const auto authorized = sealed.find(category);
            if (authorized != sealed.end()) {
                const auto& snapshot = *authorized->second;
                const auto verified = snapshot.VerifyAgainst(source.SourceSha256(), original.present
                    ? std::optional<std::span<const std::byte>>{original.bytes} : std::nullopt);
                if (!verified.Succeeded()) return verified;
                if (const auto output = generatedObjects.find(category); output != generatedObjects.end()
                    && !std::ranges::equal(snapshot.EncodedNative(), output->second->data))
                    return Reject("SAVE_GENERATED_OBJECT_TRANSACTION_CHANGED", "脚本模板物编输出超出了封印候选："
                        + names[index] + "；已在发布前拒绝。");
            } else {
                const auto& output = *generatedObjects.at(category);
                if (!original.present || !std::ranges::equal(original.bytes, output.data))
                    return Reject("SAVE_GENERATED_NATIVE_CHANGED", "脚本模板将新增或改变未获封印的物编成员："
                        + names[index] + "；已在发布前拒绝。");
            }
        }
        return Status{captured.diagnostics};
    } catch (const std::exception&) {
        return Reject("SAVE_GENERATED_OBJECT_GUARD_FAILED", "模板物编来源与封印核验未完成，未允许提交。");
    }
}

} // namespace War3Map::Services
