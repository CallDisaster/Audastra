#include "WorldPlacementPending.h"
#include "War3MapCore/Hash.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <set>
#include <unordered_map>

namespace War3Map::Modules {
namespace {
Status Failure(const char* code, const char* text) { return Status::Failure("World.Placement", code, text); }
bool SameSource(const Services::WorldInstanceBinding& a, const Services::WorldInstanceBinding& b) {
    return a.sourceGeneration == b.sourceGeneration && a.editSessionIdentity == b.editSessionIdentity
        && a.sourceIdentity == b.sourceIdentity;
}
bool Coordinates(const std::array<float, 3>& values) {
    return std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v) && std::abs(v) <= 1000000; });
}
MapDataKind Kind(MapData::PlacementFile file) {
    return file == MapData::PlacementFile::Units ? MapDataKind::Units : MapDataKind::Doodads;
}
bool Drawn(const WorldPlacementPending::Entry& entry, const Services::WorldInstanceBinding& binding,
    const Render::AnimatedModelScene* scene, const Render::AnimationFrame* frame) {
    if (!entry.renderIdentity || !scene || !frame || scene->sourceIdentity != binding.sourceIdentity
        || scene->generation != binding.sourceGeneration || scene->sceneVersion != frame->sceneVersion
        || scene->documentRevision != frame->documentRevision || frame->documentRevision > binding.mapRevision) return false;
    for (const auto& pose : frame->poses) {
        if (pose.identity != *entry.renderIdentity || pose.hidden || pose.outsideView || pose.noVisibleGeometry
            || pose.renderedInstance >= frame->instances.size() || pose.requestIndex >= scene->specs.size()) continue;
        if (scene->specs[pose.requestIndex].identity == pose.identity) return true;
    }
    return false;
}
}

Status WorldPlacementPending::Remember(const Services::WorldInstanceBinding& binding,
    const MapData::MapDataEditSession::EditReceipt& receipt, const MapDocument& after,
    std::span<const MapData::PlacementId> rows, std::span<const Render::ModelInstanceRequest> requests) {
    return RememberImpl(binding, receipt, &after, rows, requests);
}
Status WorldPlacementPending::RememberCreated(const Services::WorldInstanceBinding& binding,
    const MapData::MapDataEditSession::EditReceipt& receipt, std::span<const MapData::PlacementId> rows,
    std::span<const Render::ModelInstanceRequest> requests) {
    return RememberImpl(binding, receipt, nullptr, rows, requests);
}
Status WorldPlacementPending::RememberImpl(const Services::WorldInstanceBinding& binding,
    const MapData::MapDataEditSession::EditReceipt& receipt, const MapDocument* after,
    std::span<const MapData::PlacementId> rows, std::span<const Render::ModelInstanceRequest> requests) {
    if (!binding.sourceGeneration || binding.sourceIdentity.empty() || !binding.editSessionIdentity
        || binding.editSessionIdentity != receipt.SessionIdentity() || !receipt.CommandIdentity()
        || binding.editRevision != receipt.AfterRevision() || !receipt.Forward() || receipt.Restoring()
        || !receipt.HasPlacementLineage() || receipt.AfterRevision() <= receipt.BeforeRevision()
        || receipt.AfterRevision() - receipt.BeforeRevision() != 1 || rows.empty() || rows.size() != requests.size()
        || (binding_ && !SameSource(*binding_, binding)))
        return Failure("WORLD_PENDING_CREATION_STALE", "临时占位没有同一正式新增命令的来源、修订与谱系。");
    // 只为显示等待记录保留少量元数据；预算约束实际存储，不限制地图实例总数。
    constexpr std::size_t budget = 64ull * 1024 * 1024;
    std::size_t bytes = 0;
    for (const auto& entry : entries_) {
        const auto cost = sizeof(Entry) + entry.memberSha.size() + entry.createdRow.rawcode.size()
            + entry.request.modelPath.size() + entry.request.replaceableTexture.size() + entry.request.animationProperties.size();
        if (cost > budget - bytes) return Failure("WORLD_PENDING_BUDGET", "等待呈现记录超过实际字节预算。");
        bytes += cost;
    }
    std::map<MapDataKind, std::string> hashes;
    std::set<std::pair<MapData::PlacementFile, std::uint32_t>> seen;
    std::vector<Entry> added;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i]; const auto kind = Kind(row.file);
        if ((row.file != MapData::PlacementFile::Units && row.file != MapData::PlacementFile::Doodads)
            || !seen.emplace(row.file, row.index).second || !Coordinates(requests[i].position)
            || row.rawcode.size() != 4 || (after && !MapData::FindPlacement(*after, row).Succeeded()))
            return Failure("WORLD_PENDING_ROW_STALE", "正式新增行重复、过期或显示坐标无效。");
        const auto lineage = receipt.PlacementLineage();
        const auto map = std::find_if(lineage.begin(), lineage.end(), [kind](const auto& value) { return value.kind == kind; });
        if (map == lineage.end() || map->beforeToAfter.size() != map->beforeCount || row.index >= map->afterCount
            || std::any_of(map->beforeToAfter.begin(), map->beforeToAfter.end(), [&](const auto& target) { return target == row.index; }))
            return Failure("WORLD_PENDING_NOT_ADDED", "占位只可绑定谱系中没有原行来源的正式新增记录。");
        if (std::any_of(entries_.begin(), entries_.end(), [&](const auto& entry) {
            return entry.command == receipt.CommandIdentity() && entry.member == kind && entry.createdRow == row;
        })) return Failure("WORLD_PENDING_DUPLICATE", "同一创建命令的占位已登记。");
        auto found = hashes.find(kind);
        if (found == hashes.end()) {
            const auto raw = receipt.PlacementMemberBytes(kind, true);
            if (!raw)
                return Failure("WORLD_PENDING_MEMBER_STALE", "新增占位缺少同一回执的原生成员来源。");
            const auto digest = Sha256(*raw);
            if (!digest.Succeeded()) return {digest.diagnostics};
            if (after) {
                const auto& member = after->Member(kind);
                if (member.state != MapDataState::Ready || member.sources.size() != 1)
                    return Failure("WORLD_PENDING_MEMBER_STALE", "新增占位缺少同一快照的原生成员来源。");
                const auto current = Sha256(member.sources[0].data);
                if (!current.Succeeded() || *digest.value != *current.value)
                    return Failure("WORLD_PENDING_MEMBER_STALE", "新增回执成员与正式地图快照不一致。");
            }
            found = hashes.emplace(kind, *digest.value).first;
        }
        const auto cost = sizeof(Entry) + found->second.size() + row.rawcode.size() + requests[i].modelPath.size()
            + requests[i].replaceableTexture.size() + requests[i].animationProperties.size();
        if (cost > budget - bytes) return Failure("WORLD_PENDING_BUDGET", "等待呈现记录超过实际字节预算。");
        bytes += cost;
        Entry entry; entry.editSession = receipt.SessionIdentity(); entry.command = receipt.CommandIdentity();
        entry.createdRevision = receipt.AfterRevision(); entry.member = kind; entry.memberSha = found->second;
        entry.timing.commandIdentity = entry.command; entry.timing.documentRevision = entry.createdRevision;
        entry.createdRow = row; entry.currentRow = row; entry.request = requests[i]; added.push_back(std::move(entry));
    }
    entries_.insert(entries_.end(), std::make_move_iterator(added.begin()), std::make_move_iterator(added.end()));
    binding_ = binding;
    return {};
}

Status WorldPlacementPending::Reconcile(std::shared_ptr<const Services::WorldInstanceRegistry> registry,
    const MapDocument* current) {
    if (!registry || !binding_ || !SameSource(*binding_, registry->Binding()))
        return Failure("WORLD_PENDING_REGISTRY_STALE", "等待呈现记录与注册器的地图来源/编辑会话不一致。");
    const auto& binding = registry->Binding();
    if (binding.editRevision < binding_->editRevision)
        return Failure("WORLD_PENDING_REGISTRY_STALE", "注册器仍是正式创建之前的修订。");
    auto updated = entries_;
    struct CurrentEntry {
        const Services::WorldRegisteredPlacement* row = nullptr;
        const Render::AnimationInstanceSpec* spec = nullptr;
    };
    // 注册表可以有十万行，等待呈现的新增行通常很少。只为待决身份建立索引，
    // 然后各扫一次已验证注册表；不为每次点击分配两份全图树节点。
    std::unordered_map<std::uint64_t, CurrentEntry> currentEntries;
    currentEntries.reserve(updated.size());
    for (auto& entry : updated) {
        if (!entry.renderIdentity) {
            if (binding.editRevision != entry.createdRevision)
                return Failure("WORLD_PENDING_REMAP_MISSING", "新增行尚未在其创建修订绑定，不能猜测后续物理行身份。");
            const auto identity = registry->Find(entry.createdRow);
            if (!identity) return Failure("WORLD_PENDING_REMAP_MISSING", "正式新增行未出现在同修订注册器中。");
            entry.renderIdentity = *identity;
        }
        currentEntries.try_emplace(entry.renderIdentity->value);
    }
    for (const auto file : {MapData::PlacementFile::Units, MapData::PlacementFile::Doodads})
        for (const auto& row : registry->Placements(file))
            if (const auto found = currentEntries.find(row.identity.value); found != currentEntries.end()) found->second.row = &row;
    for (const auto& spec : registry->CapturedSpecs())
        if (const auto found = currentEntries.find(spec.identity.value); found != currentEntries.end()) found->second.spec = &spec;
    for (auto& entry : updated) {
        const auto& found = currentEntries.at(entry.renderIdentity->value);
        if (!found.row) { entry.currentRow.reset(); continue; } // Undo/删除，身份没有替身。
        if (found.row->identity != *entry.renderIdentity)
            return Failure("WORLD_PENDING_REMAP_STALE", "注册器实例代次不一致。");
        entry.currentRow = found.row->row;
        if (found.spec && found.spec->identity == *entry.renderIdentity) entry.request = found.spec->model;
        else if (current) {
            // 模型字段为空的正式记录仍存在；只显示未知占地标记，不填假模型路径。
            const auto placement = MapData::FindPlacement(*current, *entry.currentRow);
            if (!placement.Succeeded()) return {placement.diagnostics};
            entry.request = {}; entry.request.position = placement.value->position;
            entry.request.angle = placement.value->rotation; entry.request.scale = placement.value->scale;
        } else if (!entry.request.modelPath.empty()) entry.currentRow.reset();
    }
    entries_ = std::move(updated); binding_ = binding;
    return {};
}

void WorldPlacementPending::AppendLines(Render::OverlayLines& lines, const Services::WorldInstanceBinding& current,
    const Render::AnimatedModelScene* completedScene, const Render::AnimationFrame* completed,
    const std::function<bool(const MapData::PlacementId&)>& visible) const {
    if (!binding_ || !SameSource(*binding_, current) || binding_->editRevision != current.editRevision) return;
    constexpr Render::OverlayColor color{0.95f, 0.78f, 0.32f};
    constexpr std::size_t lineBudget = 262144; // 既有Overlay实际载荷预算，不是实例数量门禁。
    for (const auto& entry : entries_) {
        // registry尚未准备时，正式创建回执已证明该行存在；只在这个精确修订临时显示。
        if (!entry.currentRow || (!entry.renderIdentity && entry.createdRevision != current.editRevision)
            || (visible && !visible(*entry.currentRow)) || Drawn(entry, current, completedScene, completed)) continue;
        if (lines.size() > lineBudget - 12) break;
        const auto& p = entry.request.position;
        if (!Coordinates(p)) continue;
        constexpr float half = 16, height = 32, lift = 4;
        const std::array<std::array<float, 3>, 8> box{{
            {p[0]-half,p[1]-half,p[2]+lift}, {p[0]+half,p[1]-half,p[2]+lift},
            {p[0]+half,p[1]+half,p[2]+lift}, {p[0]-half,p[1]+half,p[2]+lift},
            {p[0]-half,p[1]-half,p[2]+lift+height}, {p[0]+half,p[1]-half,p[2]+lift+height},
            {p[0]+half,p[1]+half,p[2]+lift+height}, {p[0]-half,p[1]+half,p[2]+lift+height}}};
        Render::AppendBoxLines(lines, box, color);
    }
}
void WorldPlacementPending::Clear() noexcept { entries_.clear(); binding_.reset(); }

void WorldPlacementPending::RecordCreatedTiming(std::uint64_t command, std::uint64_t inputNs,
    std::uint64_t transactionNs) {
    for (auto& entry : entries_) if (entry.command == command) {
        entry.timing.commandIdentity = command; entry.timing.documentRevision = entry.createdRevision;
        entry.timing.inputStartedNs = inputNs; entry.timing.transactionCompletedNs = transactionNs;
    }
}
void WorldPlacementPending::RecordCaptureTiming(const WorldInstanceTiming& timing) {
    for (auto& entry : entries_) if (entry.command == timing.commandIdentity) {
        entry.timing.captureStartedNs = timing.captureStartedNs; entry.timing.captureNs = timing.captureNs;
        entry.timing.registryPrepareNs = timing.registryPrepareNs;
    }
}
std::vector<WorldPlacementCommandTiming> WorldPlacementPending::TimingCommands(
    std::uint64_t afterRevision, std::uint64_t throughRevision) const {
    std::vector<WorldPlacementCommandTiming> result;
    std::map<std::uint64_t, std::size_t> commands;
    for (const auto& entry : entries_) {
        if (entry.createdRevision <= afterRevision || entry.createdRevision > throughRevision) continue;
        const auto [found, added] = commands.emplace(entry.command, result.size());
        if (added) result.push_back(entry.timing);
        ++result[found->second].placements;
        if (entry.currentRow) ++result[found->second].retainedPlacements;
    }
    return result;
}

Result<std::shared_ptr<const Render::AnimationFrame>> TranslatePlacementPreviewFrame(
    const Render::AnimatedModelScene& scene, const Render::AnimationFrame& previous,
    std::span<const std::array<float, 12>> transforms, std::uint64_t contentVersion) {
    using Output = Result<std::shared_ptr<const Render::AnimationFrame>>;
    const auto reject = [] { return Output::Failure("World.Placement", "WORLD_PREVIEW_TRANSLATION_UNAVAILABLE",
        "缓存预览仅可平移同一静态姿势；粒子/旋转或身份变化须由后台实际求值。"); };
    if (scene.sceneVersion != previous.sceneVersion || scene.documentRevision != previous.documentRevision
        || previous.states.size() != scene.specs.size() || transforms.size() != scene.specs.size()
        || !contentVersion || contentVersion <= previous.contentVersion || previous.particleCheckpoint || !previous.particleVertices.empty()
        || !previous.particleDraws.empty() || previous.poses.size() != previous.instances.size()) return reject();
    std::vector<std::array<float, 3>> shifts; shifts.reserve(transforms.size());
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        const auto& old = previous.states[i]; const auto& next = transforms[i];
        if (old.identity != scene.specs[i].identity) return reject();
        for (std::size_t component = 0; component < next.size(); ++component) {
            if (!std::isfinite(next[component]) || std::abs(next[component]) > 1000000) return reject();
            if (component != 3 && component != 7 && component != 11 && next[component] != old.transform[component]) return reject();
        }
        shifts.push_back({next[3]-old.transform[3], next[7]-old.transform[7], next[11]-old.transform[11]});
    }
    std::vector<bool> seen(previous.instances.size());
    for (const auto& pose : previous.poses) {
        if (pose.requestIndex >= scene.specs.size() || pose.renderedInstance >= seen.size() || seen[pose.renderedInstance]
            || pose.identity != scene.specs[pose.requestIndex].identity) return reject();
        seen[pose.renderedInstance] = true;
    }
    auto result = std::make_shared<Render::AnimationFrame>(previous);
    result->contentVersion = contentVersion; result->timings = {}; // 没有发生新骨架/粒子求值。
    for (std::size_t i = 0; i < transforms.size(); ++i) result->states[i].transform = transforms[i];
    for (std::size_t i = 0; i < result->poses.size(); ++i) {
        const auto& pose = result->poses[i]; const auto& transform = transforms[pose.requestIndex];
        std::copy(transform.begin(), transform.end(), result->instances[pose.renderedInstance].instance.transform);
        if (i < result->pickBounds.size() && result->pickBounds[i].available)
            for (std::size_t axis = 0; axis < 3; ++axis) {
                result->pickBounds[i].box[axis] += shifts[pose.requestIndex][axis];
                result->pickBounds[i].box[axis+3] += shifts[pose.requestIndex][axis];
            }
    }
    return Output::Success(std::move(result));
}
} // namespace War3Map::Modules
