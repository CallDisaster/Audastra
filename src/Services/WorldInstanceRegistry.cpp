#include "War3MapCore/Services/WorldInstanceRegistry.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace War3Map::Services {
namespace {
Status Failure(const char* code, const char* message) { return Status::Failure("World.Instances", code, message); }
MapDataKind Kind(std::size_t file) { return file == 0 ? MapDataKind::Units : MapDataKind::Doodads; }
MapData::PlacementFile File(std::size_t file) { return file == 0 ? MapData::PlacementFile::Units : MapData::PlacementFile::Doodads; }
// 重建/丢弃候选也不复用实例ID。分配只保留编号，不修改当前registry或地图状态。
std::atomic<std::uint64_t> nextInstanceIdentity{1};
std::optional<War3RenderCore::InstanceIdentity> AllocateIdentity(std::uint64_t generation) {
    auto value = nextInstanceIdentity.load(std::memory_order_relaxed);
    while (value && value != std::numeric_limits<std::uint64_t>::max()) {
        if (nextInstanceIdentity.compare_exchange_weak(value,value + 1,std::memory_order_relaxed)) return War3RenderCore::InstanceIdentity{value,generation};
    }
    return std::nullopt;
}
Result<std::array<std::string,2>> Hashes(const MapDocument& document) {
    std::array<std::string,2> output;
    for (std::size_t file = 0; file < 2; ++file) {
        const auto& member = document.Member(Kind(file));
        if (member.state == MapDataState::Missing && member.sources.empty()) { output[file] = "missing"; continue; }
        if (member.state != MapDataState::Ready || member.sources.size() != 1)
            return Result<std::array<std::string,2>>::Failure("World.Instances", "WORLD_INSTANCE_SOURCE_UNSAFE", "摆放来源缺失身份、歧义或不可编辑，不能建立增量身份。");
        auto hash = Sha256(member.sources.front().data);
        if (!hash.Succeeded()) return {std::nullopt, std::move(hash.diagnostics)};
        output[file] = std::move(*hash.value);
    }
    return Result<std::array<std::string,2>>::Success(std::move(output));
}
std::vector<WorldRegisteredPlacement> Rows(const MapDocument& document, std::size_t file) {
    std::vector<WorldRegisteredPlacement> result;
    if (file == 0 && document.Units()) {
        result.reserve(document.Units()->units.size());
        for (std::size_t row = 0; row < document.Units()->units.size(); ++row)
            result.push_back({MapData::MakePlacementId(File(file), static_cast<std::uint32_t>(row), document.Units()->units[row]), {}});
    } else if (file == 1 && document.Doodads()) {
        result.reserve(document.Doodads()->doodads.size());
        for (std::size_t row = 0; row < document.Doodads()->doodads.size(); ++row)
            result.push_back({MapData::MakePlacementId(File(file), static_cast<std::uint32_t>(row), document.Doodads()->doodads[row]), {}});
    }
    return result;
}
std::string HostKey(const MapData::PlacementId& row) {
    std::string result;
    result.reserve(31 + row.rawcode.size() * 2);
    result += row.file == MapData::PlacementFile::Units ? "units:" : "doodads:";
    const auto number = [&](auto value) {
        char digits[24];
        const auto converted = std::to_chars(digits, digits + sizeof(digits), value);
        result.append(digits, static_cast<std::size_t>(converted.ptr - digits));
    };
    number(row.index); result += ':';
    constexpr char hex[] = "0123456789abcdef";
    for (const auto character : row.rawcode) {
        const auto byte = static_cast<unsigned char>(character);
        result += hex[byte >> 4]; result += hex[byte & 15];
    }
    result += ':'; number(row.creationId);
    return result;
}
bool SameAsset(const Render::ModelInstanceRequest& a, const Render::ModelInstanceRequest& b) {
    return a.modelPath == b.modelPath && a.replaceableTexture == b.replaceableTexture && a.teamColor == b.teamColor &&
        a.previewGhost == b.previewGhost && a.animationProperties == b.animationProperties;
}
}

std::size_t WorldInstanceRegistry::CommandMemory::Bytes() const noexcept {
    std::size_t bytes = sizeof(CommandMemory) + files.size() * sizeof(FileMemory);
    for (const auto& file : files) bytes += (file.removed.size() + file.added.size()) * sizeof(RememberedRow);
    return bytes;
}
const std::vector<WorldRegisteredPlacement>& WorldInstanceRegistry::Placements(MapData::PlacementFile file) const noexcept {
    static const std::vector<WorldRegisteredPlacement> empty;
    return file == MapData::PlacementFile::Units ? rows_[0] : file == MapData::PlacementFile::Doodads ? rows_[1] : empty;
}
std::optional<War3RenderCore::InstanceIdentity> WorldInstanceRegistry::Find(const MapData::PlacementId& row) const noexcept {
    const auto& file = Placements(row.file);
    return row.index < file.size() && file[row.index].row == row ? std::optional(file[row.index].identity) : std::nullopt;
}
Status WorldInstanceRegistry::Capture(std::span<const Render::ModelInstanceRequest> requests,
    std::span<const MapData::PlacementId> rows, const Render::AnimationInstanceSpec& defaults) {
    if (requests.size() != rows.size()) return Failure("WORLD_INSTANCE_CAPTURE_INVALID", "模型请求与摆放行数量不同。");
    // Find先核对typed行身份；按真实文件/行的位图查重，不为每个实例分配散列表节点。
    std::array<std::vector<bool>,2> seen;
    for (std::size_t file = 0; file < seen.size(); ++file) seen[file].resize(rows_[file].size());
    captured_.clear(); captured_.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto identity = Find(rows[i]);
        const auto file = rows[i].file == MapData::PlacementFile::Units ? 0u : 1u;
        if (!identity || seen[file][rows[i].index])
            return Failure("WORLD_INSTANCE_CAPTURE_STALE", "捕获模型行已过期或重复，不能绑定身份。");
        seen[file][rows[i].index] = true;
        auto spec = defaults; spec.identity = *identity; spec.hostKey = HostKey(rows[i]); spec.model = requests[i];
        captured_.push_back(std::move(spec));
    }
    return {};
}
Result<std::shared_ptr<const WorldInstanceRegistry>> WorldInstanceRegistry::Create(const MapDocument& document,
    WorldInstanceBinding binding, std::span<const Render::ModelInstanceRequest> requests,
    std::span<const MapData::PlacementId> rows, const Render::AnimationInstanceSpec& defaults, WorldInstanceHistoryLimits limits) {
    using Output = Result<std::shared_ptr<const WorldInstanceRegistry>>;
    if (!binding.sourceGeneration || !binding.editSessionIdentity || binding.sourceIdentity.empty())
        return Output::Failure("World.Instances", "WORLD_INSTANCE_BINDING_INVALID", "地图来源代次或编辑会话身份无效。");
    auto hashes = Hashes(document); if (!hashes.Succeeded()) return {std::nullopt, std::move(hashes.diagnostics)};
    auto output = std::shared_ptr<WorldInstanceRegistry>(new WorldInstanceRegistry);
    output->binding_ = std::move(binding); output->limits_ = limits; output->memberHashes_ = std::move(*hashes.value);
    for (std::size_t file = 0; file < 2; ++file) {
        output->rows_[file] = Rows(document,file);
        for (auto& row : output->rows_[file]) {
            const auto identity = AllocateIdentity(output->binding_.sourceGeneration);
            if (!identity) return Output::Failure("World.Instances", "WORLD_INSTANCE_ID_EXHAUSTED", "实例身份已用尽。");
            row.identity = *identity;
        }
    }
    if (const auto status = output->Capture(requests,rows,defaults); !status.Succeeded()) return {std::nullopt,status.diagnostics};
    return Output::Success(std::move(output));
}
Result<WorldInstanceRegistryChange> WorldInstanceRegistry::Prepare(const MapData::MapDataEditSession::EditReceipt& receipt,
    const WorldInstanceBinding& expected, std::uint64_t nextMapRevision, const MapDocument& nextDocument,
    std::span<const Render::ModelInstanceRequest> requests, std::span<const MapData::PlacementId> rows,
    const Render::AnimationInstanceSpec& defaults) const {
    using Output = Result<WorldInstanceRegistryChange>;
    const auto fail = [](const char* code, const char* message) { return Output::Failure("World.Instances",code,message); };
    if (expected != binding_ || nextMapRevision <= binding_.mapRevision || receipt.SessionIdentity() != binding_.editSessionIdentity ||
        !receipt.CommandIdentity() || receipt.BeforeRevision() != binding_.editRevision || receipt.AfterRevision() <= receipt.BeforeRevision() ||
        receipt.AfterRevision() - receipt.BeforeRevision() != 1)
        return fail("WORLD_INSTANCE_RECEIPT_STALE", "编辑回执、地图修订或来源代次不是当前绑定的下一次应用。");
    if (receipt.ChangesPlacements() && !receipt.HasPlacementLineage())
        return fail("WORLD_INSTANCE_LINEAGE_UNSUPPORTED", "整表编辑没有可信行谱系，必须完整重建世界。");
    auto hashes = Hashes(nextDocument); if (!hashes.Succeeded()) return {std::nullopt,std::move(hashes.diagnostics)};
    for (std::size_t file = 0; file < 2; ++file) {
        const auto before = receipt.PlacementMemberBytes(Kind(file),false), after = receipt.PlacementMemberBytes(Kind(file),true);
        if (before && after) {
            const auto beforeHash = Sha256(*before), afterHash = Sha256(*after);
            if (!beforeHash.Succeeded() || !afterHash.Succeeded() || *beforeHash.value != memberHashes_[file] || *afterHash.value != (*hashes.value)[file])
                return fail("WORLD_INSTANCE_MEMBER_STALE", "摆放字节与回执前后来源不符，未猜测存活对象。");
        } else if (before || after || (*hashes.value)[file] != memberHashes_[file])
            return fail("WORLD_INSTANCE_MEMBER_STALE", "未记录的摆放成员变化，必须完整重建世界。");
    }
    // 当前请求表随后由Capture完整构造；不先深拷贝上一版全部模型字符串再立即清空。
    // 同时只复制未被真实谱系覆盖的行表，原registry和历史命令均保持不可变。
    auto next = std::shared_ptr<WorldInstanceRegistry>(new WorldInstanceRegistry);
    next->binding_ = binding_; next->limits_ = limits_; next->history_ = history_;
    for (std::size_t file = 0; file < rows_.size(); ++file) {
        const auto changed = std::any_of(receipt.PlacementLineage().begin(), receipt.PlacementLineage().end(),
            [&](const auto& lineage) { return lineage.kind == Kind(file); });
        if (!changed) next->rows_[file] = rows_[file];
    }
    next->binding_.mapRevision = nextMapRevision; next->binding_.editRevision = receipt.AfterRevision(); next->memberHashes_ = std::move(*hashes.value);
    const auto found = std::find_if(history_.begin(),history_.end(),[&](const auto& record) { return record->identity == receipt.CommandIdentity(); });
    const CommandMemory* remembered = found == history_.end() ? nullptr : found->get();
    if (receipt.ChangesPlacements() && ((!remembered && (receipt.Restoring() || !receipt.Forward())) || (remembered && remembered->forward == receipt.Forward())))
        return fail("WORLD_INSTANCE_RESTORE_UNSUPPORTED", "撤销身份记录缺席或命令方向重复，必须完整重建世界。");
    auto memory = std::make_shared<CommandMemory>();
    if (remembered) *memory = *remembered;
    memory->identity = receipt.CommandIdentity(); memory->forward = receipt.Forward();
    const auto lookup = [](const std::vector<RememberedRow>& list, std::uint32_t row) -> std::optional<War3RenderCore::InstanceIdentity> {
        const auto item = std::lower_bound(list.begin(),list.end(),row,[](const auto& entry, auto index) { return entry.row < index; });
        return item != list.end() && item->row == row ? std::optional(item->identity) : std::nullopt;
    };
    for (const auto& map : receipt.PlacementLineage()) {
        if (map.kind != MapDataKind::Units && map.kind != MapDataKind::Doodads)
            return fail("WORLD_INSTANCE_LINEAGE_INVALID", "未知摆放文件谱系。");
        const std::size_t file = map.kind == MapDataKind::Units ? 0 : 1;
        const auto& oldRows = rows_[file]; auto newRows = Rows(nextDocument,file);
        const auto currentCount = receipt.Forward() ? map.beforeCount : map.afterCount;
        const auto newCount = receipt.Forward() ? map.afterCount : map.beforeCount;
        if (map.beforeToAfter.size() != map.beforeCount || currentCount != oldRows.size() || newCount != newRows.size())
            return fail("WORLD_INSTANCE_LINEAGE_INVALID", "谱系计数与当前完整表不符。");
        FileMemory newMemory{map.kind,{}, {}};
        const FileMemory* saved = nullptr;
        if (remembered) for (const auto& item : remembered->files) if (item.kind == map.kind) saved = &item;
        std::vector<bool> targets(map.afterCount,false);
        for (std::uint32_t beforeRow = 0; beforeRow < map.beforeCount; ++beforeRow) {
            if (const auto afterRow = map.beforeToAfter[beforeRow]) {
                if (*afterRow >= map.afterCount || targets[*afterRow]) return fail("WORLD_INSTANCE_LINEAGE_INVALID", "谱系目标越界或重复。");
                targets[*afterRow] = true;
                const auto oldRow = receipt.Forward() ? beforeRow : *afterRow, newRow = receipt.Forward() ? *afterRow : beforeRow;
                if (oldRows[oldRow].row.rawcode != newRows[newRow].row.rawcode || oldRows[oldRow].row.creationId != newRows[newRow].row.creationId)
                    return fail("WORLD_INSTANCE_LINEAGE_INVALID", "显式存活行的原始字段不一致。");
                newRows[newRow].identity = oldRows[oldRow].identity;
            } else if (receipt.Forward()) {
                if (saved) { const auto id = lookup(saved->removed,beforeRow); if (!id || *id != oldRows[beforeRow].identity) return fail("WORLD_INSTANCE_RESTORE_STALE", "删除行不属于原命令身份。"); }
                else newMemory.removed.push_back({beforeRow,oldRows[beforeRow].identity});
            } else {
                const auto id = saved ? lookup(saved->removed,beforeRow) : std::nullopt;
                if (!id) return fail("WORLD_INSTANCE_RESTORE_UNSUPPORTED", "被删除对象只能由原命令的保存回执恢复。");
                newRows[beforeRow].identity = *id;
            }
        }
        for (std::uint32_t afterRow = 0; afterRow < map.afterCount; ++afterRow) if (!targets[afterRow]) {
            if (receipt.Forward()) {
                if (saved) {
                    const auto id = lookup(saved->added,afterRow); if (!id) return fail("WORLD_INSTANCE_RESTORE_UNSUPPORTED", "新增对象身份记录缺席。");
                    newRows[afterRow].identity = *id;
                } else {
                    const auto id = AllocateIdentity(binding_.sourceGeneration);
                    if (!id) return fail("WORLD_INSTANCE_ID_EXHAUSTED", "实例身份已用尽。");
                    newRows[afterRow].identity = *id; newMemory.added.push_back({afterRow,*id});
                }
            } else {
                const auto id = saved ? lookup(saved->added,afterRow) : std::nullopt;
                if (!id || *id != oldRows[afterRow].identity) return fail("WORLD_INSTANCE_RESTORE_STALE", "撤销新增行不属于原命令身份。");
            }
        }
        next->rows_[file] = std::move(newRows);
        if (!remembered) memory->files.push_back(std::move(newMemory));
    }
    if (receipt.ChangesPlacements()) {
        if (remembered) next->history_.erase(std::find_if(next->history_.begin(),next->history_.end(),[&](const auto& entry) { return entry->identity == memory->identity; }));
        next->history_.push_back(std::move(memory));
        std::size_t bytes = 0; for (const auto& entry : next->history_) bytes += entry->Bytes();
        while (!next->history_.empty() && (next->history_.size() > limits_.maximumCommands || bytes > limits_.maximumBytes)) {
            bytes -= next->history_.front()->Bytes(); next->history_.pop_front();
        }
    }
    if (const auto status = next->Capture(requests,rows,defaults); !status.Succeeded()) return {std::nullopt,status.diagnostics};
    auto delta = DeltaTo(*next);
    if (!delta.Succeeded()) return {std::nullopt, std::move(delta.diagnostics)};
    WorldInstanceRegistryChange change; change.delta = std::move(*delta.value);
    next->previousSnapshotIdentity_ = snapshotIdentity_;
    next->previousDelta_ = std::make_shared<const Render::AnimationInstanceDelta>(change.delta);
    change.registry = std::move(next); return Output::Success(std::move(change));
}
Result<Render::AnimationInstanceDelta> WorldInstanceRegistry::DeltaTo(const WorldInstanceRegistry& next) const {
    using Output = Result<Render::AnimationInstanceDelta>;
    if (binding_.sourceGeneration != next.binding_.sourceGeneration || binding_.sourceIdentity != next.binding_.sourceIdentity
        || binding_.editSessionIdentity != next.binding_.editSessionIdentity || binding_.editRevision > next.binding_.editRevision)
        return Output::Failure("World.Instances", "WORLD_INSTANCE_DELTA_BINDING_STALE", "连续摆放快照不属于同一来源与编辑会话。");
    if (next.previousSnapshotIdentity_ == snapshotIdentity_ && next.previousDelta_)
        return Output::Success(*next.previousDelta_);
    Render::AnimationInstanceDelta delta;
    delta.session = {next.binding_.sourceGeneration, next.binding_.mapRevision, next.binding_.sourceIdentity};
    const auto compare = [&](const Render::AnimationInstanceSpec& old, const Render::AnimationInstanceSpec& spec) {
        if (old.identity != spec.identity || !SameAsset(old.model, spec.model))
            return Failure("WORLD_INSTANCE_ASSET_REBIND_UNSUPPORTED", "存活身份的资产改变，不能合并成普通摆放增量。");
        if (old.hostKey != spec.hostKey) delta.rows.push_back({spec.identity, spec.hostKey});
        Render::AnimationEdit edit; edit.identity = spec.identity;
        const auto finite = [](const auto& values) {
            return std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); });
        };
        if (old.model.position != spec.model.position || old.model.angle != spec.model.angle || old.model.scale != spec.model.scale
            || !std::isfinite(spec.model.angle) || !finite(spec.model.position) || !finite(spec.model.scale)) {
            const auto transform = Render::ModelInstanceTransform(spec.model);
            if (transform != Render::ModelInstanceTransform(old.model)) edit.transform = transform;
        }
        if (old.model.tint != spec.model.tint) edit.tint = spec.model.tint;
        if (edit.transform || edit.tint) delta.edits.push_back(std::move(edit));
        return Status{};
    };
    const auto common = std::min(captured_.size(), next.captured_.size());
    const bool samePrefix = std::equal(captured_.begin(), captured_.begin() + common, next.captured_.begin(),
        [](const auto& before, const auto& after) { return before.identity == after.identity; });
    if (samePrefix) {
        // 已验证快照中身份序列相同的前缀仍逐项核验资产/变换；仅省去两张全表identity索引。
        for (std::size_t i = 0; i < common; ++i)
            if (const auto status = compare(captured_[i], next.captured_[i]); !status.Succeeded())
                return {std::nullopt, status.diagnostics};
        for (std::size_t i = common; i < captured_.size(); ++i) delta.removals.push_back(captured_[i].identity);
        for (std::size_t i = common; i < next.captured_.size(); ++i) delta.additions.push_back(next.captured_[i]);
    } else {
        std::unordered_map<std::uint64_t, std::size_t> oldSpecs;
        oldSpecs.reserve(captured_.size());
        for (std::size_t i = 0; i < captured_.size(); ++i) oldSpecs.emplace(captured_[i].identity.value, i);
        std::vector<bool> retained(captured_.size());
        for (const auto& spec : next.captured_) {
            const auto previous = oldSpecs.find(spec.identity.value);
            if (previous == oldSpecs.end()) { delta.additions.push_back(spec); continue; }
            retained[previous->second] = true;
            if (const auto status = compare(captured_[previous->second], spec); !status.Succeeded())
                return {std::nullopt, status.diagnostics};
        }
        for (std::size_t i = 0; i < captured_.size(); ++i)
            if (!retained[i]) delta.removals.push_back(captured_[i].identity);
    }
    return Output::Success(std::move(delta));
}
} // namespace War3Map::Services
