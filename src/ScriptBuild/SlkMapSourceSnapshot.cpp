#include "War3MapCore/Script/SlkMapSourceSnapshot.h"
#include "War3MapCore/Script/SlkMapMemberProtocol.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <stdexcept>

namespace War3Map::Script {
namespace {
using namespace SlkMapMemberProtocol;
bool SameCanonical(std::string_view name, std::string_view uppercase) {
    if (name.size() != uppercase.size()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i] >= 'a' && name[i] <= 'z' ? static_cast<char>(name[i] - 'a' + 'A') : name[i];
        if (c != uppercase[i]) return false;
    }
    return true;
}

constexpr std::size_t FrontendSlotCount() {
    std::size_t count = (kMembers.size() + std::size(kMapSources)) * 2 + std::size(kLocales);
    for (const auto& table : kTableSources) count += table.second.empty() ? 1 : 2;
    return count;
}
struct Rejection { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Reject(std::string code, std::string message) {
    throw Rejection{{{DiagnosticSeverity::Error, "Script.SlkSources", std::move(code), std::move(message), {}}}};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) throw Rejection{std::move(result.diagnostics)};
    return std::move(*result.value);
}
void Spend(std::size_t& total, std::size_t count, std::size_t limit) {
    if (total > limit || count > limit - total) Reject("SLK_SOURCES_LIMIT", "最终来源快照超过预算。");
    total += count;
}
void Append(std::string& output, std::string_view value) {
    output += std::to_string(value.size()); output += ':'; output += value;
}
// 比固定 info.lua 更保守；未知资源不从高优先级回退到默认数据。
bool Unsupported(std::string_view name) {
    return name.ends_with(".SLK") || (name.starts_with("UNITS\\") && name.ends_with(".TXT")) ||
        name == "UI\\MISCDATA.TXT" || name == "WAR3MAPMISC.TXT" || name == "WAR3MAPSKIN.TXT" ||
        (name.starts_with("TABLE\\") && name.ends_with(".INI")) ||
        (name.starts_with("WAR3MAP.") && name.ends_with(".INI")) ||
        name.starts_with("W3X2LNI\\") || name.starts_with("MAP\\");
}
} // namespace

Result<SlkMapSourceSnapshot> SlkMapSourceSnapshot::Capture(const MapArchiveSession& archive,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    std::uint64_t revision, const SlkMapSourceLimits& limits) {
    try {
        const SlkMapSourceLimits hard;
        if (limits.memberCount > hard.memberCount || limits.pathBytes > hard.pathBytes ||
            limits.totalPathBytes > hard.totalPathBytes || limits.retainedBytes > hard.retainedBytes ||
            limits.replacementBytes > hard.replacementBytes || limits.frontendMemberCount > hard.frontendMemberCount)
            Reject("SLK_SOURCES_LIMIT", "调用方只能收紧最终来源快照预算。");
        if (FrontendSlotCount() > limits.frontendMemberCount)
            Reject("SLK_SOURCES_LIMIT", "固定 frontend 来源槽位超过预算。");
        std::size_t paths = 0, edits = 0, retained = 0;
        const auto key = [&](std::string_view name) {
            if (name.size() > limits.pathBytes) Reject("SLK_SOURCES_LIMIT", "成员路径超过预算。");
            Spend(paths, name.size(), limits.totalPathBytes);
            auto canonical = Take(Resources::CanonicalImportPath(name));
            if (canonical == "(LISTFILE)" || canonical == "(ATTRIBUTES)" || canonical == "(SIGNATURE)")
                Reject("SLK_SOURCES_RESERVED", "物编来源快照不接受归档内部保留成员的编辑。");
            return canonical;
        };
        if (archive.Members().size() > limits.memberCount || replacements.size() > limits.memberCount ||
            removals.size() > limits.memberCount) Reject("SLK_SOURCES_LIMIT", "成员清单超过预算。");
        std::map<std::string, const MapArchiveMember*> named;
        for (const auto& member : archive.Members()) {
            if (!named.emplace(key(member.name), &member).second)
                Reject("SLK_SOURCES_CONFLICT", "源成员有规范化名称冲突。");
        }
        std::vector<std::string> domain;
        domain.reserve(FrontendSlotCount());
        const auto add = [&](std::string_view name) {
            (void)key(name);
            domain.emplace_back(name);
        };
        for (const auto name : kMembers) add(name);
        for (const auto name : kMapSources) add(name);
        for (const auto name : kMembers) add("map\\" + std::string(name));
        for (const auto name : kMapSources) add("map\\" + std::string(name));
        for (const auto& table : kTableSources) {
            add(table.first);
            if (!table.second.empty()) add(table.second);
        }
        for (const auto name : kLocales) add(name);
        // 同一来源锁内查询固定名字：不依赖 listfile，读取失败不能变为 Missing。
        ArchiveExactMemberLimits exactLimits;
        exactLimits.memberCount = limits.frontendMemberCount;
        exactLimits.pathBytes = limits.pathBytes;
        exactLimits.totalPathBytes = limits.totalPathBytes;
        exactLimits.memberBytes = limits.retainedBytes;
        exactLimits.totalBytes = limits.retainedBytes;
        auto exact = Take(archive.CaptureExactMembers(domain, exactLimits));
        if (exact.size() != domain.size()) Reject("SLK_SOURCES_CONFLICT", "固定来源捕获未返回全部槽位。");
        std::set<std::string> final;
        for (const auto& [name, member] : named) {
            (void)member;
            final.insert(name);
        }
        for (const auto& member : exact) {
            const auto normalized = Take(Resources::CanonicalImportPath(member.name));
            const auto found = named.find(normalized);
            if (found != named.end() && (!member.present || found->second->data != member.bytes))
                Reject("SLK_SOURCES_CONFLICT", "固定查询与会话已列名来源不一致：" + normalized);
            if (member.present) final.insert(normalized);
        }
        std::map<std::string, const MemberReplacement*> changed;
        for (const auto& member : replacements) {
            Spend(edits, member.data.size(), limits.replacementBytes);
            if (!changed.emplace(key(member.name), &member).second)
                Reject("SLK_SOURCES_CONFLICT", "待保存成员有多个版本。");
        }
        std::set<std::string> removed;
        for (const auto& name : removals) {
            const auto normalized = key(name);
            if (!removed.insert(normalized).second || changed.contains(normalized))
                Reject("SLK_SOURCES_CONFLICT", "成员被重复删除或同时替换和删除。");
            if (!final.erase(normalized)) Reject("SLK_SOURCES_REMOVAL_MISSING", "被删除成员不在源快照中。");
        }
        for (const auto& [name, member] : changed) {
            (void)member;
            final.insert(name);
        }
        if (final.size() > limits.memberCount) Reject("SLK_SOURCES_LIMIT", "最终成员清单超过预算。");

        SlkMapSourceSnapshot output;
        output.archiveSha256_ = archive.SourceSha256(); output.revision_ = revision;
        output.finalNamesComplete_ = !archive.HasOpaqueMembers();
        std::string manifest;
        Append(manifest, "forge-slk-map-sources-v3"); Append(manifest, kFrontendProfile);
        Append(manifest, output.archiveSha256_);
        Append(manifest, output.finalNamesComplete_ ? "complete" : "opaque");
        Append(manifest, std::to_string(revision)); Append(manifest, std::to_string(changed.size()));
        for (const auto& [name, member] : changed) {
            Append(manifest, name); Append(manifest, Take(Sha256(member->data)));
        }
        Append(manifest, std::to_string(removed.size()));
        for (const auto& name : removed) Append(manifest, name);
        for (const auto& name : final) {
            output.finalNames_.push_back(name);
            if (Unsupported(name)) output.unsupported_.push_back(name);
        }
        output.members_.reserve(FrontendSlotCount());
        output.originals_.reserve(kMembers.size());
        // 先计算最终实际保留总量，分配任何额外 native 副本前拒绝超限。
        std::vector<SlkSessionSourceIdentity> originalIdentities;
        originalIdentities.reserve(exact.size());
        for (std::size_t i = 0; i < exact.size(); ++i) {
            const auto& source = exact[i];
            const auto normalized = Take(Resources::CanonicalImportPath(source.name));
            originalIdentities.push_back({source.present ? SlkSessionSourcePresence::Present : SlkSessionSourcePresence::Missing,
                source.present ? Take(Sha256(source.bytes)) : std::string{}});
            if (i < kMembers.size()) Spend(retained, source.bytes.size(), limits.retainedBytes);
            if (removed.contains(normalized)) continue;
            const auto replacement = changed.find(normalized);
            Spend(retained, replacement == changed.end() ? source.bytes.size() : replacement->second->data.size(), limits.retainedBytes);
        }
        // 原始九槽位转移所有权；被覆盖的其它原始输入不再需要，先释放再分配最终副本。
        for (std::size_t i = 0; i < exact.size(); ++i) {
            auto& source = exact[i];
            if (i < kMembers.size()) {
                output.originals_.push_back({domain[i], originalIdentities[i], std::move(source.bytes)});
            } else {
                const auto normalized = Take(Resources::CanonicalImportPath(source.name));
                if (removed.contains(normalized) || changed.contains(normalized)) std::vector<std::byte>().swap(source.bytes);
            }
        }
        Append(manifest, std::to_string(exact.size()));
        for (std::size_t i = 0; i < exact.size(); ++i) {
            auto& source = exact[i];
            const auto normalized = Take(Resources::CanonicalImportPath(source.name));
            SlkMapMemberSnapshot member;
            member.name = domain[i];
            member.identity = originalIdentities[i];
            // 原始存在性与 SHA 单独绑定；最终 overlay 不能冒充三方合并祖先。
            Append(manifest, normalized);
            Append(manifest, source.present ? "original-present" : "original-missing");
            Append(manifest, member.identity.sha256);
            if (removed.contains(normalized)) {
                member.identity = {SlkSessionSourcePresence::Missing, {}};
            } else if (const auto replacement = changed.find(normalized); replacement != changed.end()) {
                member.identity = {SlkSessionSourcePresence::Present, Take(Sha256(replacement->second->data))};
                member.bytes = replacement->second->data;
            } else if (i < kMembers.size()) {
                member.bytes = output.originals_[i].bytes;
            } else {
                member.bytes = std::move(source.bytes);
            }
            // 其它原始 frontend 字节不另存；被替换/删除的临时读取立即释放。
            std::vector<std::byte>().swap(source.bytes);
            Append(manifest, member.identity.presence == SlkSessionSourcePresence::Present ? "final-present" : "final-missing");
            Append(manifest, member.identity.sha256);
            output.members_.push_back(std::move(member));
        }
        output.manifestSha256_ = Take(Sha256Text(manifest));
        auto result = Result<SlkMapSourceSnapshot>::Success(std::move(output));
        for (const auto& name : result.value->unsupported_) {
            result.diagnostics.push_back({DiagnosticSeverity::Warning, "Script.SlkSources", "SLK_SOURCES_OVERLAY_UNSUPPORTED",
                "存在尚未物化的地图来源：" + name, OriginSpan{name, 0, 0, result.value->manifestSha256_}});
        }
        return result;
    } catch (const Rejection& failure) {
        Result<SlkMapSourceSnapshot> result; result.diagnostics = failure.diagnostics; return result;
    } catch (const std::exception&) {
        return Result<SlkMapSourceSnapshot>::Failure("Script.SlkSources", "SLK_SOURCES_RESOURCE_FAILURE", "来源快照未完成，未返回部分结果。");
    }
}

Result<SlkMapSourceLookup> SlkMapSourceSnapshot::LookupMember(std::string_view name) const {
    if (name.size() > SlkMapSourceLimits{}.pathBytes)
        return Result<SlkMapSourceLookup>::Failure("Script.SlkSources", "SLK_SOURCES_QUERY_INVALID", "查询成员名超过硬限。");
    auto canonical = Resources::CanonicalImportPath(name);
    if (!canonical.Succeeded()) {
        return Result<SlkMapSourceLookup>::Failure("Script.SlkSources", "SLK_SOURCES_QUERY_INVALID", "查询成员名无效。");
    }
    SlkMapSourceLookup result;
    result.memberName = std::move(*canonical.value);
    if (result.memberName == "(LISTFILE)" || result.memberName == "(ATTRIBUTES)" || result.memberName == "(SIGNATURE)")
        return Result<SlkMapSourceLookup>::Failure("Script.SlkSources", "SLK_SOURCES_QUERY_INVALID", "归档保留成员不在来源查询域中。");
    for (const auto& member : members_) {
        if (!SameCanonical(member.name, result.memberName)) continue;
        if (member.identity.presence == SlkSessionSourcePresence::Present) {
            result.state = SlkMapSourceLookupState::Captured;
            result.member = &member;
        }
        return Result<SlkMapSourceLookup>::Success(std::move(result));
    }
    if (std::binary_search(finalNames_.begin(), finalNames_.end(), result.memberName))
        result.state = SlkMapSourceLookupState::PresentUncaptured;
    else if (!finalNamesComplete_)
        result.state = SlkMapSourceLookupState::Unknown;
    return Result<SlkMapSourceLookup>::Success(std::move(result));
}

Result<SlkMapSourceLookup> SlkMapSourceSnapshot::LoadFrontendMember(std::string_view group,
    std::string_view name, SlkFrontendInputMode mode) const {
    const auto invalid = [] {
        return Result<SlkMapSourceLookup>::Failure("Script.SlkSources", "SLK_SOURCES_QUERY_INVALID",
            "请求不在固定 frontend/proxy 的来源协议内；不能作为 Missing 回退。");
    };
    if ((mode != SlkFrontendInputMode::Archive && mode != SlkFrontendInputMode::Lni) ||
        name.size() > SlkMapSourceLimits{}.pathBytes) return invalid();
    const auto canonical = Resources::CanonicalImportPath(name);
    if (!canonical.Succeeded()) return invalid();
    if (group == "map") {
        const auto known = [&](std::string_view candidate) { return SameCanonical(candidate, *canonical.value); };
        if (!std::any_of(kMembers.begin(), kMembers.end(), known) &&
            !std::any_of(std::begin(kMapSources), std::end(kMapSources), known)) return invalid();
        if (mode == SlkFrontendInputMode::Lni) return LookupMember("MAP\\" + *canonical.value);
        return LookupMember(*canonical.value);
    }
    if (group == "table") {
        for (const auto& table : kTableSources) {
            if (table.name != name) continue;
            auto first = LookupMember(table.first);
            if (!first.Succeeded() || first.value->state != SlkMapSourceLookupState::Missing || table.second.empty()) return first;
            return LookupMember(table.second);
        }
        return invalid();
    }
    if (group == "w3x2lni") {
        if (*canonical.value != "LOCALE\\W3I.LNG" && *canonical.value != "LOCALE\\LML.LNG") return invalid();
        return LookupMember("W3X2LNI\\" + *canonical.value);
    }
    return invalid();
}
} // namespace War3Map::Script
