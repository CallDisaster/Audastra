#include "War3MapCore/Script/ScriptBaseInputs.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"

#include <algorithm>
#include <cctype>

namespace War3Map::Script {
namespace {

constexpr std::size_t kBaseBytes = 16 * 1024 * 1024;

bool SameMember(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a == '/' ? '\\' : a) == std::tolower(b == '/' ? '\\' : b);
        });
}

void Reject(std::vector<Diagnostic>& diagnostics, std::string code, std::string message) {
    diagnostics.push_back({DiagnosticSeverity::Error, "Script.Base", std::move(code), std::move(message), {}});
}

bool Freeze(ScriptBaseInput& output, std::span<const std::byte> bytes, std::vector<Diagnostic>& diagnostics) {
    if (bytes.size() > kBaseBytes) {
        Reject(diagnostics, "SCRIPT_BASE_LIMIT", "基础脚本超过每项 16 MiB 的读取预算");
        return false;
    }
    auto hash = Sha256(bytes);
    diagnostics.insert(diagnostics.end(), hash.diagnostics.begin(), hash.diagnostics.end());
    if (!hash.Succeeded()) return false;
    output.bytes.assign(bytes.begin(), bytes.end());
    output.sha256 = std::move(*hash.value);
    return true;
}

// false 表示失败；true + absent 只表示最终命名视图已确认不存在，可以继续下一层。
bool ReadMap(const ScriptBaseInputsRequest& request, std::string_view name, ScriptBaseInput& output,
    bool& absent, std::vector<Diagnostic>& diagnostics) {
    const MemberReplacement* replacement = nullptr;
    for (const auto& member : request.replacements) {
        if (!SameMember(member.name, name)) continue;
        if (replacement) {
            Reject(diagnostics, "SCRIPT_BASE_EDIT_CONFLICT", std::string(name) + " 有多个规范名相同的覆盖");
            return false;
        }
        replacement = &member;
    }
    const bool removed = std::any_of(request.removals.begin(), request.removals.end(),
        [&](const auto& member) { return SameMember(member, name); });
    if (removed && replacement) {
        Reject(diagnostics, "SCRIPT_BASE_EDIT_CONFLICT", std::string(name) + " 同时被修改和删除");
        return false;
    }
    absent = removed;
    if (removed) return true;
    const MapArchiveMember* source = nullptr;
    if (!replacement && request.archive) {
        for (const auto& member : request.archive->Members()) {
            if (!SameMember(member.name, name)) continue;
            if (source) {
                Reject(diagnostics, "SCRIPT_BASE_EDIT_CONFLICT", std::string(name) + " 有多个归档来源");
                return false;
            }
            source = &member;
        }
    }
    absent = !replacement && !source;
    if (absent) {
        if (request.archive && request.archive->HasOpaqueMembers()) {
            Reject(diagnostics, "SCRIPT_BASE_MAP_UNRESOLVED", std::string(name) + " 可能位于匿名成员，不能据命名清单回退基础库");
            return false;
        }
        return true;
    }
    output.origin = replacement ? ScriptBaseOrigin::MapReplacement : ScriptBaseOrigin::MapArchive;
    output.memberName = replacement ? replacement->name : source->name;
    if (request.archive) output.sourceRoot = request.archive->SourcePath();
    return Freeze(output, replacement ? replacement->data : source->data, diagnostics);
}

bool Pick(const ScriptBaseInputsRequest& request, std::string_view name, const std::filesystem::path& fallback,
    ScriptBaseInput& output, std::vector<Diagnostic>& diagnostics) {
    if (request.policy == ScriptBasePolicy::MapGameThenLayout || request.policy == ScriptBasePolicy::MapGameOnly) {
        for (const auto& member : {std::string(name), "scripts\\" + std::string(name)}) {
            bool absent = false;
            if (!ReadMap(request, member, output, absent, diagnostics)) return false;
            if (!absent) return true;
        }
        const bool compatibleGame = request.game && request.gameLibraryVersion &&
            (request.policy == ScriptBasePolicy::MapGameOnly ||
                (*request.gameLibraryVersion == 20) == request.layout->allowReturnBug);
        if (compatibleGame) {
            for (const auto& member : {std::string(name), "scripts\\" + std::string(name)}) {
                if (!request.game->HasFile(member)) continue;
                auto file = request.game->ReadFileBoundedWithSource(member, kBaseBytes);
                diagnostics.insert(diagnostics.end(), file.diagnostics.begin(), file.diagnostics.end());
                if (!file.Succeeded()) return false;
                output.origin = ScriptBaseOrigin::GameArchive;
                output.memberName = member;
                output.sourceRoot = request.game->Root();
                output.archiveName = std::move(file.value->archiveName);
                return Freeze(output, file.value->bytes, diagnostics);
            }
        }
    }
    if (request.policy == ScriptBasePolicy::MapGameOnly) {
        Reject(diagnostics, "SCRIPT_BASE_MAP_GAME_MISSING", "地图和已绑定的游戏来源中缺少 " + std::string(name));
        return false;
    }
    const auto& bindings = request.layout->bindingFiles;
    const auto binding = std::find_if(bindings.begin(), bindings.end(), [&](const auto& file) { return file.path == fallback; });
    if (binding == bindings.end() || binding->kind != ScriptToolchainBindingKind::RegularFile || binding->sha256.empty()) {
        Reject(diagnostics, "GAME_SCRIPTS_MISSING", "无法读取绑定的 " + std::string(name));
        return false;
    }
    auto file = ReadVerifiedFile(fallback, binding->sha256, std::min<std::uint64_t>(binding->maxBytes, kBaseBytes));
    diagnostics.insert(diagnostics.end(), file.diagnostics.begin(), file.diagnostics.end());
    if (!file.Succeeded()) return false;
    output.origin = ScriptBaseOrigin::Layout;
    output.memberName = std::string(name);
    output.sourceRoot = fallback;
    return Freeze(output, *file.value, diagnostics);
}

} // namespace

Result<ScriptBaseInputs> CaptureScriptBaseInputs(const ScriptBaseInputsRequest& request) {
    Result<ScriptBaseInputs> result;
    const bool mapGameOnly = request.policy == ScriptBasePolicy::MapGameOnly;
    if ((!mapGameOnly && !request.layout) || (request.policy != ScriptBasePolicy::BoundLayout &&
        request.policy != ScriptBasePolicy::MapGameThenLayout && !mapGameOnly)) {
        Reject(result.diagnostics, "SCRIPT_BASE_REQUEST_INVALID", "基础库捕获需要绑定的工具链与有效来源策略");
        return result;
    }
    if (request.gameLibraryVersion && *request.gameLibraryVersion != 20 && *request.gameLibraryVersion != 24) {
        Reject(result.diagnostics, "SCRIPT_BASE_GAME_POLICY_INVALID", "游戏基础库校验策略必须是 20 或 24");
        return result;
    }
    if (mapGameOnly && !request.gameLibraryVersion) {
        Reject(result.diagnostics, "SCRIPT_BASE_GAME_POLICY_UNBOUND", "仅地图和游戏来源策略必须显式绑定基础库校验版本");
        return result;
    }
    if (!mapGameOnly) {
        auto verified = VerifyScriptToolchainLayout(*request.layout);
        const bool layoutValid = verified.Succeeded();
        result.diagnostics = std::move(verified.diagnostics);
        if (!layoutValid) return result;
    }
    if (request.policy == ScriptBasePolicy::MapGameThenLayout && request.game) {
        if (!request.gameLibraryVersion || ((*request.gameLibraryVersion == 20) != request.layout->allowReturnBug)) {
            result.diagnostics.push_back({DiagnosticSeverity::Info, "Script.Base",
                request.gameLibraryVersion ? "GAME_BASE_POLICY_MISMATCH" : "GAME_BASE_POLICY_UNBOUND",
                "游戏基础库策略未绑定或与目标不一致，本次不读取游戏层", {}});
        }
    }
    if ((request.policy == ScriptBasePolicy::MapGameThenLayout || mapGameOnly) && request.archive) {
        auto source = request.archive->VerifySourceUnchanged();
        result.diagnostics.insert(result.diagnostics.end(), source.diagnostics.begin(), source.diagnostics.end());
        if (!source.Succeeded()) return result;
    }
    ScriptBaseInputs inputs;
    inputs.policy = request.policy;
    inputs.allowReturnBug = mapGameOnly ? *request.gameLibraryVersion == 20 : request.layout->allowReturnBug;
    inputs.documentRevision = request.documentRevision;
    if (request.archive) inputs.sourceMapSha256 = request.archive->SourceSha256();
    inputs.gameLibraryVersion = request.gameLibraryVersion;
    const auto commonFallback = mapGameOnly ? std::filesystem::path{} : request.layout->commonScript;
    const auto blizzardFallback = mapGameOnly ? std::filesystem::path{} : request.layout->blizzardScript;
    if (!Pick(request, "common.j", commonFallback, inputs.common, result.diagnostics) ||
        !Pick(request, "blizzard.j", blizzardFallback, inputs.blizzard, result.diagnostics)) return result;
    result.value = std::move(inputs);
    return result;
}

Status VerifyScriptBaseInputs(const ScriptBaseInputs& inputs, const ScriptToolchainLayout& layout, ScriptBasePolicy policy) {
    Status result;
    bool valid = (policy == ScriptBasePolicy::BoundLayout || policy == ScriptBasePolicy::MapGameThenLayout) &&
        inputs.policy == policy && inputs.allowReturnBug == layout.allowReturnBug;
    for (const auto* input : {&inputs.common, &inputs.blizzard}) {
        if (input->bytes.size() > kBaseBytes) { valid = false; continue; }
        auto hash = Sha256(input->bytes);
        result.diagnostics.insert(result.diagnostics.end(), hash.diagnostics.begin(), hash.diagnostics.end());
        valid = valid && hash.Succeeded() && *hash.value == input->sha256;
        valid = valid && !input->memberName.empty() &&
            (input->origin == ScriptBaseOrigin::Layout || input->origin == ScriptBaseOrigin::MapArchive ||
                input->origin == ScriptBaseOrigin::MapReplacement || input->origin == ScriptBaseOrigin::GameArchive);
        if (input->origin == ScriptBaseOrigin::Layout) {
            const auto path = input == &inputs.common ? layout.commonScript : layout.blizzardScript;
            const auto binding = std::find_if(layout.bindingFiles.begin(), layout.bindingFiles.end(),
                [&](const auto& file) { return file.path == path; });
            valid = valid && input->sourceRoot == path && binding != layout.bindingFiles.end() && binding->sha256 == input->sha256;
        } else if (policy == ScriptBasePolicy::BoundLayout) valid = false;
    }
    if (!valid) Reject(result.diagnostics, "SCRIPT_BASE_SNAPSHOT_MISMATCH", "冻结的基础库字节、摘要或来源策略与本次工具链不一致");
    return result;
}

Status VerifyScriptBaseInputs(const ScriptBaseInputs& inputs, ScriptBasePolicy policy) {
    Status result;
    bool valid = policy == ScriptBasePolicy::MapGameOnly && inputs.policy == policy &&
        inputs.gameLibraryVersion && (*inputs.gameLibraryVersion == 20 || *inputs.gameLibraryVersion == 24) &&
        inputs.allowReturnBug == (*inputs.gameLibraryVersion == 20);
    for (const auto* input : {&inputs.common, &inputs.blizzard}) {
        if (input->bytes.size() > kBaseBytes) { valid = false; continue; }
        auto hash = Sha256(input->bytes);
        result.diagnostics.insert(result.diagnostics.end(), hash.diagnostics.begin(), hash.diagnostics.end());
        valid = valid && hash.Succeeded() && *hash.value == input->sha256;
        const std::string name = input == &inputs.common ? "common.j" : "blizzard.j";
        valid = valid && (SameMember(input->memberName, name) || SameMember(input->memberName, "scripts\\" + name));
        switch (input->origin) {
        case ScriptBaseOrigin::MapArchive:
            valid = valid && !input->sourceRoot.empty() && !inputs.sourceMapSha256.empty() && input->archiveName.empty();
            break;
        case ScriptBaseOrigin::MapReplacement:
            valid = valid && input->archiveName.empty();
            break;
        case ScriptBaseOrigin::GameArchive:
            valid = valid && !input->sourceRoot.empty() && !input->archiveName.empty();
            break;
        default:
            valid = false;
            break;
        }
    }
    if (!valid) Reject(result.diagnostics, "SCRIPT_BASE_SNAPSHOT_MISMATCH", "冻结的基础库字节、摘要或来源不符合仅地图和游戏来源策略");
    return result;
}

} // namespace War3Map::Script
