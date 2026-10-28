#include "War3MapCore/Services/AuxiliaryTransaction.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <algorithm>
#include <set>

namespace War3Map::Services {
namespace {
Status Bad(const char* code, std::string message) {
    return Status::Failure("MapData.AuxiliaryTransaction", code, std::move(message));
}
std::string Canonical(std::string value) {
    value = Text::LowerAscii(value);
    std::replace(value.begin(), value.end(), '\\', '/');
    if (value.starts_with("map/")) value.erase(0, 4);
    return value;
}
std::string ArchiveMemberKey(std::string value) {
    value = Text::LowerAscii(value);
    std::replace(value.begin(), value.end(), '\\', '/');
    return value; // 归档内 map/ 是实际目录，不能套用目录工作区的布局别名。
}
Result<std::shared_ptr<const MapDocument>> Replay(const MapDocument& original,
    std::span<const AuxiliaryEditRequest> commands, const AuxiliaryReferenceContext& references) {
    using R = Result<std::shared_ptr<const MapDocument>>;
    if (commands.empty() || commands.size() > 128)
        return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_COMMAND_BUDGET", "区域/镜头事务须包含 1 到 128 条命令。");
    MapData::MapDataEditSession replay(original);
    for (const auto& command : commands) {
        auto before = std::make_shared<const MapDocument>(replay.Snapshot());
        auto plan = AuxiliaryEditing::Prepare(std::move(before), command, references);
        if (!plan.Succeeded()) return {{}, plan.diagnostics};
        const auto applied = AuxiliaryEditing::Apply(replay, *plan.value, references);
        if (!applied.Succeeded()) return {{}, applied.diagnostics};
    }
    return R::Success(std::make_shared<const MapDocument>(replay.Snapshot()));
}
std::string LowerPath(const std::filesystem::path& path) {
    return Text::LowerAscii(path.generic_string());
}
bool IsPath(const std::filesystem::path& path, std::string_view member) {
    const auto value = LowerPath(path);
    return value == member || value == std::string("map/") + std::string(member);
}
} // namespace

void ObserveAuxiliaryScriptSource(AuxiliaryReferenceContext& context, std::string name) {
    auto lower = Text::LowerAscii(name);
    std::replace(lower.begin(), lower.end(), '\\', '/');
    if (lower.starts_with("map/")) lower.erase(0, 4);
    if ((lower.ends_with(".lua") || lower.ends_with(".j") || lower.ends_with(".ai"))
        && lower != "war3map.j" && lower != "scripts/war3map.j")
        context.opaqueScriptSources.push_back(std::move(name));
}

Status VerifyAuxiliarySaveOutput(std::span<const MapArchiveMember> expectedMembers,
    std::span<const MapArchiveMember> finalMembers, std::span<const std::string> removals) {
    const auto mismatch = [](std::string message) {
        return Status::Failure("MapData.AuxiliarySave", "SAVE_AUXILIARY_OUTPUT_MISMATCH", std::move(message));
    };
    if (expectedMembers.size() > 2) return mismatch("冻结的区域/镜头成员数量无效。");
    std::set<std::string> expectedNames;
    for (const auto& expected : expectedMembers) {
        const auto key = ArchiveMemberKey(expected.name);
        if ((key != "war3map.w3r" && key != "war3map.w3c") || !expectedNames.insert(key).second)
            return mismatch("冻结的区域/镜头成员名称无效或重复：" + expected.name);
        std::size_t count = 0;
        for (const auto& member : finalMembers) {
            if (ArchiveMemberKey(member.name) != key) continue;
            if (member.data != expected.data)
                return mismatch("保存过程改变了冻结的区域/镜头成员：" + expected.name);
            ++count;
        }
        if (count != 1 || std::any_of(removals.begin(), removals.end(), [&](const auto& name) {
            return ArchiveMemberKey(name) == key;
        })) return mismatch("冻结的区域/镜头成员缺失、重复或被删除：" + expected.name);
    }
    return Status::Ok();
}

Result<std::shared_ptr<const AuxiliaryTransactionSnapshot>> AuxiliaryTransactionSnapshot::Freeze(
    const MapDocument& original, const MapDocument& current, std::span<const AuxiliaryEditRequest> commands,
    const AuxiliaryReferenceContext& references) {
    using R = Result<std::shared_ptr<const AuxiliaryTransactionSnapshot>>;
    const auto replay = Replay(original, commands, references);
    if (!replay.Succeeded()) return {{}, replay.diagnostics};
    if (!references.triggers || !references.triggers->HasTrustedSourceSeal()
        || references.triggers->SourceWtgSha256().size() != 64 || references.triggers->SourceWctSha256().size() != 64
        || references.triggers->UiCatalogSha256().size() != 64)
        return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_TRIGGER_SOURCE_REQUIRED", "严格封印需要解析器建立的 WTG/WCT 与 Catalog 来源身份。");
    if (!AuxiliaryEditing::AreEquivalentMapSources(**replay.value, current))
        return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_REPLAY_MISMATCH", "当前文档不等于共用区域/镜头命令序列重放结果，不能密封额外字节。");
    std::shared_ptr<AuxiliaryTransactionSnapshot> snapshot(new AuxiliaryTransactionSnapshot);
    snapshot->original_ = std::make_shared<const MapDocument>(original);
    snapshot->document_ = *replay.value;
    snapshot->commands_.assign(commands.begin(), commands.end());
    snapshot->sourceWtgSha256_ = references.triggers->SourceWtgSha256();
    snapshot->sourceWctSha256_ = references.triggers->SourceWctSha256();
    snapshot->catalogSha256_ = references.triggers->UiCatalogSha256();
    std::set<MapDataKind> kinds;
    std::size_t totalBytes = 0;
    for (const auto& command : commands) kinds.insert(command.kind);
    for (const auto kind : kinds) {
        const auto& after = current.Member(kind);
        if (after.state != MapDataState::Ready || after.sources.size() != 1)
            return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_OUTPUT_UNSAFE", "重放输出没有唯一完整原生成员。");
        const auto& bytes = after.sources.front().data;
        if (bytes.size() > 64U * 1024U * 1024U - totalBytes)
            return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_BYTE_BUDGET", "区域/镜头事务输出超过 64 MiB。");
        totalBytes += bytes.size();
        snapshot->members_.push_back({after.memberName, bytes});
    }
    snapshot->changed_ = !AuxiliaryEditing::AreEquivalentMapSources(original, current);
    return R::Success(std::move(snapshot));
}

Status AuxiliaryTransactionSnapshot::VerifyAgainst(const MapDocument& original,
    const AuxiliaryReferenceContext& references) const {
    // 所有已知源成员都绑定，包含 W3I 布局、W3S 环境音和单位表传送门；不可只核对 W3R/W3C。
    if (!AuxiliaryEditing::AreEquivalentMapSources(original, *original_)) return Bad("AUX_TRANSACTION_SOURCE_STALE", "保存或发布源地图数据与原区域/镜头事务不同。");
    if (!references.triggers || !references.triggers->HasTrustedSourceSeal()
        || references.triggers->SourceWtgSha256() != sourceWtgSha256_ || references.triggers->SourceWctSha256() != sourceWctSha256_
        || references.triggers->UiCatalogSha256() != catalogSha256_)
        return Bad("AUX_TRANSACTION_TRIGGER_SOURCE_STALE", "WTG/WCT 或 Catalog 不是同一封印的来源，不能借用另一个地图的引用检查。");
    const auto replay = Replay(original, commands_, references);
    if (!replay.Succeeded()) return {replay.diagnostics};
    if (!AuxiliaryEditing::AreEquivalentMapSources(**replay.value, *document_)) return Bad("AUX_TRANSACTION_REPLAY_MISMATCH", "重放后的区域/镜头成员不等于密封候选。");
    return Status::Ok();
}

Result<AuxiliaryPatch> AuxiliaryTransactionSnapshot::Seal(const WorkspaceSnapshot& source) const {
    using R = Result<AuxiliaryPatch>;
    for (const auto& [name, sha] : {std::pair{"war3map.wtg", sourceWtgSha256_}, std::pair{"war3map.wct", sourceWctSha256_}}) {
        const WorkspaceFile* trigger = nullptr;
        for (const auto& file : source.files) if (IsPath(file.relativePath, name)) {
            if (trigger) return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_TRIGGER_SOURCE_STALE", "WTG/WCT 有多个目录来源。");
            trigger = &file;
        }
        if (!trigger || trigger->sha256 != sha)
            return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_TRIGGER_SOURCE_STALE", "触发器来源封印不属于同一个目录快照。");
    }
    // 不只绑定写出的两个成员。伪造一个缺少传送门/声音/地图设置的原文档，不能授权真实目录写入。
    for (const auto& original : original_->Members()) {
        std::vector<const WorkspaceFile*> bound;
        for (const auto& file : source.files) {
            const auto path = LowerPath(file.relativePath);
            if (IsPath(file.relativePath, original.kind == MapDataKind::Settings ? "war3map.w3i" : Canonical(original.memberName))
                || (original.kind == MapDataKind::Settings && path == "table/w3i.ini")) bound.push_back(&file);
        }
        if (bound.size() != original.sources.size())
            return R::Failure("MapData.AuxiliaryTransaction", bound.size() > 1 ? "AUX_TRANSACTION_SOURCE_AMBIGUOUS" : "AUX_TRANSACTION_SOURCE_STALE", "地图数据来源集合与目录快照不一致。");
        std::set<std::size_t> matched;
        for (const auto& input : original.sources) {
            const auto sha = Sha256(input.data);
            if (!sha.Succeeded()) return {{}, sha.diagnostics};
            const auto file = std::find_if(bound.begin(), bound.end(), [&](const auto* value) {
                const auto index = static_cast<std::size_t>(std::find(bound.begin(), bound.end(), value) - bound.begin());
                return !matched.contains(index) && Canonical(value->relativePath.generic_string()) == Canonical(input.name)
                    && value->size == input.data.size() && value->sha256 == *sha.value;
            });
            if (file == bound.end()) return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_SOURCE_STALE", "地图数据原字节不属于目录快照。");
            matched.insert(static_cast<std::size_t>(file - bound.begin()));
        }
    }
    std::vector<BinaryFilePatch> files;
    for (const auto& member : members_) {
        const auto kind = member.name == "war3map.w3r" ? MapDataKind::Regions : MapDataKind::Cameras;
        const auto& before = original_->Member(kind);
        const auto identity = AuxiliaryEditing::MemberIdentity(*original_, kind);
        if (!identity.Succeeded()) return {{}, identity.diagnostics};
        const WorkspaceFile* bound = nullptr;
        for (const auto& file : source.files) if (IsPath(file.relativePath, member.name)) {
            if (bound) return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_SOURCE_AMBIGUOUS", "目录中同一原生成员有多个来源。");
            bound = &file;
        }
        std::filesystem::path path;
        if (*identity.value == "missing") {
            if (bound) return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_SOURCE_STALE", "缺席成员在目录快照中已经存在。");
            const WorkspaceFile* settings = nullptr;
            for (const auto& file : source.files) if (IsPath(file.relativePath, "war3map.w3i")) {
                if (settings) return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_SOURCE_AMBIGUOUS", "缺席成员的地图设置目录布局不唯一。");
                settings = &file;
            }
            const auto& state = original_->Member(MapDataKind::Settings);
            if (!settings || state.sources.size() != 1)
                return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_SOURCE_STALE", "缺席成员没有同源的经典地图设置来源。");
            const auto sha = Sha256(state.sources.front().data);
            if (!sha.Succeeded()) return {{}, sha.diagnostics};
            if (*sha.value != settings->sha256 || settings->size != state.sources.front().data.size())
                return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_SOURCE_STALE", "地图设置与目录快照身份不同。");
            path = settings->relativePath.parent_path() / member.name;
        } else {
            if (!bound || bound->sha256 != *identity.value || before.sources.size() != 1
                || bound->size != before.sources.front().data.size())
                return R::Failure("MapData.AuxiliaryTransaction", "AUX_TRANSACTION_SOURCE_STALE", "原生成员不属于打开目录的精确快照。");
            path = bound->relativePath;
        }
        files.push_back({std::move(path), member.data, "共用区域/镜头命令序列", *identity.value});
    }
    return R::Success(AuxiliaryPatch(std::move(files)));
}

} // namespace War3Map::Services
