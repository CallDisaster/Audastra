#include "McpWorkspace.h"
#include "McpWorkspaceObjectsState.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Services/ObjectTransactionSnapshot.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace War3Map::Mcp {
namespace {
using namespace ObjectData;
constexpr std::size_t kMemberBudget = 64U * 1024U * 1024U;
constexpr std::size_t kMapBudget = 256U * 1024U * 1024U;
constexpr std::size_t kGameBudget = 128U * 1024U * 1024U;

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}
std::string Utf8(const std::filesystem::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
template<class T> Result<T> Failed(const auto& value) { return {{}, value.diagnostics}; }
Status Failure(std::string code, std::string message) {
    return Status::Failure("MCP.Object", std::move(code), std::move(message));
}
Result<std::shared_ptr<const LniObjectDocument>> ParseLni(ObjectType type, std::span<const std::byte> bytes) {
    auto parsed = LniObjectCodec(type).Parse(bytes);
    const auto* document = dynamic_cast<const LniObjectDocument*>(parsed.document.get());
    if (!parsed.Succeeded() || !document || !document->IsStructurallySafe()) {
        if (!parsed.diagnostics.empty()) return Failed<std::shared_ptr<const LniObjectDocument>>(parsed);
        return {{}, Failure("OBJECT_SOURCE_INVALID", "物编来源不能建立完整可编辑 LNI 文档。").diagnostics};
    }
    return Result<std::shared_ptr<const LniObjectDocument>>::Success(std::make_shared<const LniObjectDocument>(*document));
}
}

std::string McpWorkspace::ObjectRevision() const {
    const auto revision = session_.Revision();
    return std::to_string(revision.generation) + ':' + std::to_string(ActiveTransactionId()) + ':' +
        std::to_string(activeEdit_ ? activeEdit_->generation : 0) + ':' + std::to_string(toolEpoch_) + ':' +
        revision.sourceSha256 + ':' + revision.uiCatalogSha256;
}

Status McpWorkspace::CaptureObjectSources() {
    if (objectState_) return Status::Ok();
    if (gameRoot_.empty()) return Failure("OBJECT_GAME_ROOT_REQUIRED",
        "七类物编需要宿主实际配置 FORGE_MCP_WAR3_ROOT；不猜标准目录或字段定义。");
    const auto environment = CheckEnvironmentUnchanged();
    if (!environment.Succeeded()) return environment;
    auto game = Game::GameDataSource::MountStrict(gameRoot_);
    if (!game.Succeeded()) return {game.diagnostics};
    auto state = std::make_unique<McpObjectWorkspaceState>();
    std::size_t gameBytes = 0;
    Status gameRead = Status::Ok();
    const StandardObjectCatalog::FileReader reader = [&](const std::string& name) -> Result<std::vector<std::byte>> {
        auto existing = state->gameInputs.find(name);
        if (existing == state->gameInputs.end()) {
            McpObjectWorkspaceState::GameInput input;
            if (game.value->HasFile(name)) {
                auto value = game.value->ReadFileBoundedWithSource(name, CatalogLoadOptions{}.maximumFileBytes);
                if (!value.Succeeded()) { gameRead.diagnostics = value.diagnostics; return Failed<std::vector<std::byte>>(value); }
                if (value.value->bytes.size() > kGameBudget - gameBytes) {
                    gameRead = Failure("OBJECT_GAME_SNAPSHOT_BUDGET", "标准目录与字段元数据的实际读取超过 128 MiB 累积预算。");
                    return Failed<std::vector<std::byte>>(gameRead);
                }
                gameBytes += value.value->bytes.size();
                input.archive = std::move(value.value->archiveName);
                input.bytes = std::move(value.value->bytes);
            }
            existing = state->gameInputs.emplace(name, std::move(input)).first;
        }
        if (!existing->second.bytes) return Result<std::vector<std::byte>>::Failure("MCP.Object",
            "OBJECT_GAME_MEMBER_MISSING", "游戏来源明确缺失：" + name);
        return Result<std::vector<std::byte>>::Success(*existing->second.bytes);
    };
    auto standard = StandardObjectCatalog::Load(reader);
    if (!gameRead.Succeeded()) return gameRead;
    if (!standard.Succeeded()) return {standard.diagnostics};
    auto schema = ObjectFieldSchema::Load(reader);
    if (!gameRead.Succeeded()) return gameRead;
    if (!schema.Succeeded()) return {schema.diagnostics};
    state->standard = std::move(*standard.value);
    state->schema = std::move(*schema.value);

    auto directory = DirectoryWorkspace::Open(projectRoot_);
    if (!directory.Succeeded()) return {directory.diagnostics};
    if (directory.value->Snapshot().aggregateSha256 != session_.Revision().sourceSha256)
        return Failure("OBJECT_WORKSPACE_SOURCE_CHANGED", "物编捕获与当前 EditorSession 来源修订不一致。");
    std::size_t mapBytes = 0;
    for (std::size_t index = 0; index < state->mapInputs.size(); ++index) {
        const auto type = static_cast<ObjectType>(index);
        auto& input = state->mapInputs[index];
        std::vector<std::filesystem::path> lniPaths, nativePaths;
        for (const auto& file : directory.value->Snapshot().files) {
            const auto name = Lower(Utf8(file.relativePath));
            const auto lni = std::string(LniFileName(type));
            const auto native = std::string(NativeFileName(type));
            if (name == lni || name == "table/" + lni) lniPaths.push_back(file.relativePath);
            if (name == native || name == "map/" + native) nativePaths.push_back(file.relativePath);
        }
        if (lniPaths.size() > 1 || nativePaths.size() > 1)
            return Failure("OBJECT_SOURCE_AMBIGUOUS", "同类物编存在多个 root/map/table 来源：" + std::string(ToString(type)));
        if (!nativePaths.empty()) {
            auto bytes = IO::ReadFileBytes(projectRoot_ / nativePaths.front(), kMemberBudget);
            if (!bytes.Succeeded()) return {bytes.diagnostics};
            if (bytes.value->size() > kMapBudget - mapBytes) return Failure("OBJECT_MAP_SNAPSHOT_BUDGET", "地图物编超过 256 MiB 累积预算。");
            mapBytes += bytes.value->size();
            const auto parsed = NativeObjectCodec(type).Parse(*bytes.value);
            const auto* native = dynamic_cast<const NativeObjectDocument*>(parsed.document.get());
            if (!parsed.Succeeded() || !native || !native->IsStructurallySafe()) return {parsed.diagnostics};
            input.native = *native;
            input.nativeBytes = std::move(*bytes.value);
        }
        if (!lniPaths.empty()) {
            input.sourcePresent = true;
            input.relativePath = lniPaths.front();
            auto bytes = IO::ReadFileBytes(projectRoot_ / input.relativePath, kMemberBudget);
            if (!bytes.Succeeded()) return {bytes.diagnostics};
            if (bytes.value->size() > kMapBudget - mapBytes) return Failure("OBJECT_MAP_SNAPSHOT_BUDGET", "地图物编超过 256 MiB 累积预算。");
            mapBytes += bytes.value->size();
            auto parsed = ParseLni(type, *bytes.value);
            if (!parsed.Succeeded()) return {parsed.diagnostics};
            input.initial = *parsed.value;
            input.sourceBytes = std::move(*bytes.value);
            if (input.native) {
                auto projected = *input.native;
                const auto status = ApplyLniChangesToNative(projected, *input.initial, &state->schema, &state->standard);
                if (!status.Succeeded()) return status;
                const auto encoded = NativeObjectCodec(type).Serialize(projected);
                if (!encoded.Succeeded()) return {encoded.diagnostics};
                if (*encoded.value != *input.nativeBytes)
                    return Failure("OBJECT_SOURCE_PROJECTION_CONFLICT", "LNI 与保留的 native 来源不一致；请先明确生效来源：" + Utf8(input.relativePath));
            }
        } else if (input.native) {
            input.sourcePresent = true;
            input.relativePath = nativePaths.front();
            input.nativeSource = true;
            input.sourceBytes = *input.nativeBytes;
            auto bytes = ConvertNativeObjectDataToLni(type, *input.nativeBytes);
            if (!bytes.Succeeded()) return {bytes.diagnostics};
            auto parsed = ParseLni(type, *bytes.value);
            if (!parsed.Succeeded()) return {parsed.diagnostics};
            input.initial = *parsed.value;
        } else {
            input.relativePath = std::filesystem::path("table") / std::string(LniFileName(type));
            auto parsed = ParseLni(type, {});
            if (!parsed.Succeeded()) return {parsed.diagnostics};
            input.initial = *parsed.value;
        }
        input.committed = input.initial;
    }
    auto map = MapDocument::FromDirectory(*directory.value);
    if (!map.Succeeded()) return {map.diagnostics};
    state->map = std::make_unique<MapDocument>(std::move(*map.value));
    // 只读实际主脚本供删除提示；.ai 不读取、不检查。
    for (const auto& file : directory.value->Snapshot().files) {
        const auto name = Lower(Utf8(file.relativePath));
        if (name != "war3map.j" && name != "map/war3map.j" && name != "scripts/war3map.j" &&
            name != "war3map.lua" && name != "map/war3map.lua" && name != "scripts/war3map.lua") continue;
        auto bytes = IO::ReadFileBytes(projectRoot_ / file.relativePath, kMemberBudget);
        if (!bytes.Succeeded()) return {bytes.diagnostics};
        if (bytes.value->size() > kMapBudget - mapBytes) return Failure("OBJECT_MAP_SNAPSHOT_BUDGET", "地图与脚本提示超过 256 MiB 累积预算。");
        mapBytes += bytes.value->size();
        state->scriptNames.push_back(Utf8(file.relativePath));
        state->scriptBytes.push_back(std::move(*bytes.value));
    }
    const auto source = session_.VerifySourceUnchanged();
    if (!source.Succeeded()) return source;
    objectState_ = std::move(state);
    const auto verified = CheckObjectSourcesUnchanged();
    if (!verified.Succeeded()) { objectState_.reset(); return verified; }
    return Status::Ok();
}

Status McpWorkspace::CheckObjectSourcesUnchanged() const {
    if (!objectState_) return Status::Ok();
    auto game = Game::GameDataSource::MountStrict(gameRoot_);
    if (!game.Succeeded()) return {game.diagnostics};
    for (const auto& [name, expected] : objectState_->gameInputs) {
        if (game.value->HasFile(name) != expected.bytes.has_value())
            return Failure("OBJECT_GAME_SOURCE_CHANGED", "标准物编或元数据来源的存在性变化：" + name);
        if (!expected.bytes) continue;
        const auto current = game.value->ReadFileBoundedWithSource(name, CatalogLoadOptions{}.maximumFileBytes);
        if (!current.Succeeded()) return {current.diagnostics};
        if (current.value->archiveName != expected.archive || current.value->bytes != *expected.bytes)
            return Failure("OBJECT_GAME_SOURCE_CHANGED", "标准物编、字段元数据或实际遮蔽归档已变化：" + name);
    }
    return Status::Ok();
}

Result<Services::ObjectCommandContext> McpWorkspace::ObjectContext(ObjectType type) {
    const auto captured = CaptureObjectSources();
    if (!captured.Succeeded()) return Failed<Services::ObjectCommandContext>(captured);
    const auto current = CheckEnvironmentUnchanged();
    if (!current.Succeeded()) return Failed<Services::ObjectCommandContext>(current);
    const auto index = static_cast<std::size_t>(type);
    if (index >= objectState_->mapInputs.size()) return Failed<Services::ObjectCommandContext>(Failure("OBJECT_KIND_INVALID", "物编类别无效。"));
    if (objectState_->standard.Count(type) == 0)
        return Failed<Services::ObjectCommandContext>(Failure("OBJECT_STANDARD_CATEGORY_MISSING", "实际标准目录没有此类对象：" + std::string(ToString(type))));
    Services::ObjectCommandContext context;
    context.type = type; context.standard = &objectState_->standard; context.schema = &objectState_->schema;
    for (std::size_t i = 0; i < context.documents.size(); ++i)
        context.documents[i] = activeEdit_ && activeEdit_->objects[i] ? activeEdit_->objects[i].get() : objectState_->mapInputs[i].committed.get();
    context.document = context.documents[index];
    context.nameMap = activeEdit_ && activeEdit_->mapData ? activeEdit_->mapData.get()
        : committedMapData_ ? committedMapData_.get() : objectState_->map.get();
    return Result<Services::ObjectCommandContext>::Success(context);
}

Status McpWorkspace::ApplyObjectOverlay(ObjectType type, const LniObjectDocument& document, TransactionId transactionId) {
    const auto index = static_cast<std::size_t>(type);
    const auto& input = objectState_->mapInputs[index];
    Result<ObjectPatch> patch;
    if (input.nativeSource) {
        auto candidate = *input.native;
        const auto projected = ApplyLniChangesToNative(candidate, document, &objectState_->schema, &objectState_->standard);
        if (!projected.Succeeded()) return projected;
        patch = NativeObjectCodec(type).BuildPatch(candidate, input.relativePath, "MCP 共用物编命令");
    } else {
        patch = LniObjectCodec(type).BuildPatch(document, input.relativePath, "MCP 共用物编命令");
    }
    if (!patch.Succeeded()) return {patch.diagnostics};
    // 七类缺失成员由 EditorSession 的类型化入口核验 codec 空源与路径；
    // 通用 ApplyObjectPatch 仍拒绝创建任意成员。
    return input.sourcePresent ? session_.ApplyObjectPatch(transactionId, std::move(*patch.value)) :
        session_.CreateObjectMember(transactionId, std::move(*patch.value));
}

Status McpWorkspace::ApplyObjectPlan(const Services::ObjectCommandPlan& plan) {
    if (publishing_) return Failure("MCP_PUBLISH_IN_PROGRESS", "归档候选正在发布；不能变更物编事务。");
    const auto context = ObjectContext(plan.subject.type);
    if (!context.Succeeded()) return {context.diagnostics};
    if (!context.value->document || context.value->document->DocumentFingerprint() != plan.before.DocumentFingerprint())
        return Failure("OBJECT_PLAN_STALE", "物编命令计划不属于当前可见文档。");
    const auto index = static_cast<std::size_t>(plan.subject.type);
    // 可抛出的文档分配在获取 writer lease/发布 overlay 前完成。
    auto candidate = std::make_shared<const LniObjectDocument>(plan.after);
    const bool startedHere = !activeEdit_;
    if (startedHere) {
        const auto started = BeginEdit(session_.Revision());
        if (!started.Succeeded()) return {started.diagnostics};
    }
    const auto applied = ApplyObjectOverlay(plan.subject.type, *candidate, activeEdit_->id);
    if (!applied.Succeeded()) {
        if (!startedHere) return applied;
        auto status = applied;
        const auto rolled = Rollback(activeEdit_->id, activeEdit_->baseRevision);
        status.diagnostics.insert(status.diagnostics.end(), rolled.diagnostics.begin(), rolled.diagnostics.end());
        return status;
    }
    activeEdit_->objects[index] = std::move(candidate);
    ++activeEdit_->generation; ++toolEpoch_;
    activeEdit_->previewedGeneration.reset(); activeEdit_->validatedGeneration.reset();
    return Status::Ok();
}

Services::ObjectDeletionAssessment McpWorkspace::AssessObjectDeletion(const ObjectKey& key) {
    std::vector<ObjectRecordSource> sources;
    std::vector<Services::ScriptSourceInput> scripts;
    for (std::size_t index = 0; index < objectState_->mapInputs.size(); ++index) {
        const auto& input = objectState_->mapInputs[index];
        const auto* document = activeEdit_ && activeEdit_->objects[index] ? activeEdit_->objects[index].get() : input.committed.get();
        sources.push_back({static_cast<ObjectType>(index), Utf8(input.relativePath),
            input.sourcePresent || (activeEdit_ && activeEdit_->objects[index]) ? ReferenceInputState::Ready : ReferenceInputState::Missing,
            document->Objects()});
    }
    for (std::size_t index = 0; index < objectState_->scriptNames.size(); ++index)
        scripts.push_back({objectState_->scriptNames[index], objectState_->scriptBytes[index]});
    return Services::AssessObjectDeletion({&VisibleDocument(), &catalog_, sources, objectState_->map.get(), &objectState_->schema}, key, scripts);
}

Result<std::vector<std::shared_ptr<const Services::ObjectTransactionSnapshot>>> McpWorkspace::FreezeObjectTransactions(
    const std::filesystem::path& sourceMap, const std::string& sourceSha256) const {
    using Frozen = std::vector<std::shared_ptr<const Services::ObjectTransactionSnapshot>>;
    Frozen result;
    if (!objectState_ || !activeEdit_) return Result<Frozen>::Success(std::move(result));
    const auto archive = Mpq::MpqArchive::OpenReadOnly(sourceMap);
    if (!archive.Succeeded()) return Failed<Frozen>(archive);
    for (std::size_t index = 0; index < activeEdit_->objects.size(); ++index) {
        if (!activeEdit_->objects[index]) continue;
        const auto& input = objectState_->mapInputs[index];
        const auto type = static_cast<ObjectType>(index);
        std::optional<std::vector<std::byte>> original;
        if (archive.value->HasFile(NativeFileName(type))) {
            const auto read = archive.value->ReadFileBounded(NativeFileName(type), kMemberBudget);
            if (!read.Succeeded()) return Failed<Frozen>(read);
            original = *read.value;
        }
        if (input.nativeBytes && original != input.nativeBytes)
            return Failed<Frozen>(Failure("OBJECT_ARCHIVE_BASELINE_MISMATCH", "来源归档与工作区的 native 物编基线不同：" + std::string(NativeFileName(type))));
        const auto nativeSpan = original ? std::optional<std::span<const std::byte>>{*original} : std::nullopt;
        auto baseline = Services::ObjectTransactionSnapshot::Freeze(*input.initial, nativeSpan, sourceSha256, &objectState_->schema, &objectState_->standard);
        if (!baseline.Succeeded()) return Failed<Frozen>(baseline);
        if ((original && !std::equal((*baseline.value)->EncodedNative().begin(), (*baseline.value)->EncodedNative().end(), original->begin(), original->end())) ||
            (!original && !input.initial->Objects().empty()))
            return Failed<Frozen>(Failure("OBJECT_ARCHIVE_BASELINE_MISMATCH", "工作区初始 LNI 不等于源图物编；不能把另一份地图的覆盖混入保存。"));
        auto frozen = Services::ObjectTransactionSnapshot::Freeze(*activeEdit_->objects[index], nativeSpan, sourceSha256, &objectState_->schema, &objectState_->standard);
        if (!frozen.Succeeded()) return Failed<Frozen>(frozen);
        result.push_back(std::move(*frozen.value));
    }
    return Result<Frozen>::Success(std::move(result));
}

Status McpWorkspace::CheckObjectOverlay(const SemanticDiff& preview,
    const std::vector<std::shared_ptr<const Services::ObjectTransactionSnapshot>>& objects) const {
    std::set<std::size_t> seen;
    for (const auto& change : preview.changes) {
        if (change.kind != "object") continue;
        if (!objectState_ || !activeEdit_) return Failure("MCP_OBJECT_OVERLAY_MISMATCH", "物编 overlay 没有对应的活动领域状态。");
        bool matched = false;
        for (std::size_t index = 0; index < activeEdit_->objects.size(); ++index) {
            if (!activeEdit_->objects[index] || change.path != objectState_->mapInputs[index].relativePath || seen.contains(index)) continue;
            const auto type = static_cast<ObjectType>(index);
            const auto snapshot = std::find_if(objects.begin(), objects.end(), [type](const auto& value) { return value->Type() == type; });
            if (snapshot == objects.end() || !DiffObjects((*snapshot)->EditedDocument().Objects(), activeEdit_->objects[index]->Objects()).Empty()) continue;
            const auto& input = objectState_->mapInputs[index];
            auto before = input.sourcePresent ? Sha256(input.sourceBytes) : Result<std::string>::Success({});
            Result<std::string> after;
            if (input.nativeSource) after = Sha256((*snapshot)->EncodedNative());
            else {
                const auto bytes = LniObjectCodec(type).Serialize(*activeEdit_->objects[index]);
                if (!bytes.Succeeded()) return {bytes.diagnostics};
                after = Sha256(*bytes.value);
            }
            if (!before.Succeeded()) return {before.diagnostics};
            if (!after.Succeeded()) return {after.diagnostics};
            if (change.beforeSha256 != *before.value || change.afterSha256 != *after.value) continue;
            seen.insert(index); matched = true; break;
        }
        if (!matched) return Failure("MCP_OBJECT_OVERLAY_MISMATCH", "物编 overlay 的领域、路径或前后 SHA 不属于冻结保存事务。");
    }
    for (const auto& snapshot : objects) {
        const auto index = static_cast<std::size_t>(snapshot->Type());
        const auto& input = objectState_->mapInputs[index];
        if (!DiffObjects(input.initial->Objects(), activeEdit_->objects[index]->Objects()).Empty() && !seen.contains(index))
            return Failure("MCP_OBJECT_OVERLAY_MISMATCH", "已改变的物编文档缺少对应 EditorSession overlay。");
    }
    return Status::Ok();
}

} // namespace War3Map::Mcp
