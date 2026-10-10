#include "War3MapCore/ObjectData/SkinObjectCatalog.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <string_view>

namespace War3Map::ObjectData {
namespace {

constexpr std::array<ObjectType, 7> kTypes{
    ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
    ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};
constexpr std::size_t kMaximumPageRows = 128;
constexpr std::size_t kMaximumPageFields = 8192;

std::string Canonical(std::string_view value) {
    std::string result(value);
    for (auto& character : result) {
        if (character == '/') character = '\\';
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    return result;
}

} // namespace

std::string SkinObjectFileName(ObjectType type) {
    const auto ordinary = NativeFileName(type);
    return "war3mapSkin" + std::string(ordinary.substr(std::string_view("war3map").size()));
}

Result<SkinObjectCatalog> SkinObjectCatalog::Capture(
    std::span<const MapArchiveMember> members, std::string sourceSha256, bool hasOpaqueMembers) {
    if (sourceSha256.size() != 64 || !std::all_of(sourceSha256.begin(), sourceSha256.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        })) {
        return Result<SkinObjectCatalog>::Failure("ObjectData", "SKIN_SOURCE_ID_INVALID",
            "Skin 目录需要已验证的 64 位十六进制源地图 SHA-256。");
    }
    SkinObjectCatalog catalog;
    catalog.sourceSha256_ = std::move(sourceSha256);
    for (std::size_t index = 0; index < kTypes.size(); ++index) {
        auto& summary = catalog.summaries_[index];
        summary.type = kTypes[index];
        summary.memberName = SkinObjectFileName(summary.type);
        const auto name = Canonical(summary.memberName);
        const MapArchiveMember* matched = nullptr;
        for (const auto& member : members) {
            if (Canonical(member.name) != name) continue;
            if (matched) {
                summary.state = SkinMemberState::Ambiguous;
                summary.diagnostics.push_back({DiagnosticSeverity::Error, "ObjectData", "SKIN_MEMBER_AMBIGUOUS",
                    summary.memberName + " 有多个同名来源，禁止择一读取或写出。"});
                break;
            }
            matched = &member;
        }
        if (summary.state == SkinMemberState::Ambiguous) continue;
        if (!matched) {
            summary.state = hasOpaqueMembers ? SkinMemberState::OpaqueUnknown : SkinMemberState::Missing;
            continue;
        }
        auto hash = Sha256(matched->data);
        if (!hash.Succeeded()) {
            summary.state = SkinMemberState::Invalid;
            summary.diagnostics = std::move(hash.diagnostics);
            continue;
        }
        summary.memberSha256 = std::move(*hash.value);
        auto parsed = NativeObjectCodec(summary.type).Parse(matched->data);
        summary.diagnostics = std::move(parsed.diagnostics);
        auto* native = dynamic_cast<NativeObjectDocument*>(parsed.document.get());
        if (!native || !native->IsStructurallySafe()) {
            summary.state = SkinMemberState::Invalid;
            continue;
        }
        summary.version = native->Version();
        summary.originalCount = native->OriginalObjectCount();
        summary.customCount = native->CustomObjectCount();
        summary.state = summary.diagnostics.empty() ? SkinMemberState::Ready : SkinMemberState::Partial;
        catalog.documents_[index].reset(static_cast<NativeObjectDocument*>(parsed.document.release()));
    }
    return Result<SkinObjectCatalog>::Success(std::move(catalog));
}

Result<SkinObjectPage> SkinObjectCatalog::Query(ObjectType type, std::string_view expectedSourceSha256,
    std::size_t offset, std::size_t limit) const {
    if (expectedSourceSha256 != sourceSha256_) {
        return Result<SkinObjectPage>::Failure("ObjectData", "SKIN_SOURCE_STALE",
            "源地图 SHA-256 与 Skin 目录快照不一致，请重新读取。");
    }
    if (limit == 0 || limit > kMaximumPageRows) {
        return Result<SkinObjectPage>::Failure("ObjectData", "SKIN_PAGE_LIMIT_INVALID",
            "Skin 查询每页须请求 1..128 行。");
    }
    const auto index = static_cast<std::size_t>(type);
    if (index >= documents_.size() || !documents_[index]) {
        return Result<SkinObjectPage>::Failure("ObjectData", "SKIN_MEMBER_UNAVAILABLE",
            "该类 Skin 成员缺失、不透明、重复或不能安全解析；查看成员状态和诊断。");
    }
    const auto records = documents_[index]->Objects();
    if (offset > records.size()) {
        return Result<SkinObjectPage>::Failure("ObjectData", "SKIN_PAGE_OFFSET_INVALID",
            "Skin 查询起点超出对象数量。");
    }
    SkinObjectPage page;
    const auto end = offset + std::min(limit, records.size() - offset);
    std::size_t fieldCount = 0;
    for (auto row = offset; row < end; ++row) {
        const auto count = records[row].modifications.size();
        if (count > kMaximumPageFields - fieldCount) {
            return Result<SkinObjectPage>::Failure("ObjectData", "SKIN_PAGE_FIELD_BUDGET",
                "此页字段数超过 8192；请缩小查询页数。单行仍超出时保留源成员并拒绝此页。");
        }
        fieldCount += count;
        page.rows.push_back({row,
            row < summaries_[index].originalCount ? NativeObjectTable::Original : NativeObjectTable::Custom,
            records[row], false});
    }
    if (end < records.size()) page.nextOffset = end;
    return Result<SkinObjectPage>::Success(std::move(page));
}

} // namespace War3Map::ObjectData
