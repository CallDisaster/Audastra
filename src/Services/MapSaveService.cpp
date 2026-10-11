#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/LuaTestReloadController.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/Services/ObjectTransactionSnapshot.h"
#include "War3MapCore/Services/ObjectTransactionSaveGuards.h"

#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/ObjectData/ObjectTypes.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Script/ScriptSaveBundle.h"
#include "LuaMapSaveTarget.h"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <sstream>
#include <utility>

namespace War3Map::Services {
namespace {

void Append(std::vector<Diagnostic>& target, const std::vector<Diagnostic>& source) {
    target.insert(target.end(), source.begin(), source.end());
}

bool HasError(const std::vector<Diagnostic>& diagnostics) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [](const Diagnostic& diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
    });
}

void Error(MapSaveTask& task, std::string code, std::string message, std::string subsystem = "地图") {
    task.diagnostics.push_back({DiagnosticSeverity::Error, std::move(subsystem), std::move(code), std::move(message)});
}

std::string PathUtf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {utf8.begin(), utf8.end()};
}

std::string Seconds(double value) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(1) << value;
    return text.str();
}

void Report(const MapSaveService::Progress& progress, std::string_view stage) {
    // 进度通知只用于展示。即使 UI 回调抛异常，也不能中断归档事务或掩盖写出结果。
    try {
        if (progress) progress(stage);
    } catch (...) {
    }
}

std::uint32_t HeaderVersion(std::span<const std::byte> bytes) {
    if (bytes.size() < 4) return 0;
    std::uint32_t version = 0;
    for (unsigned i = 0; i < 4; ++i)
        version |= std::uint32_t(std::to_integer<std::uint8_t>(bytes[i])) << (8 * i);
    return version;
}

bool MemberNameIs(std::string_view actual, std::string_view expected) {
    return actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin(),
        [](char left, char right) {
            const auto lower = [](char value) { return value >= 'A' && value <= 'Z' ? char(value + 'a' - 'A') : value; };
            return lower(left) == lower(right);
        });
}

bool RejectUnsupportedMapFormat(MapSaveTask& task) {
    const bool luaTarget = task.script && task.script->luaTarget.has_value();
    if (luaTarget) {
        const auto verified = LuaMapSave::VerifyTarget(task);
        Append(task.diagnostics, verified.diagnostics);
        if (!verified.Succeeded()) return true;
    }
    // 在脚本重建之前直接锁住地图版本。脚本门禁仍存在，但不可再依赖它间接阻止
    // 未来新增的无脚本保存路径写出尚未实现的 W3I / SkinId 布局。
    if (task.archive->HasMember("war3map.w3i")) {
        const auto settings = task.archive->ReadMember("war3map.w3i");
        if (!settings.Succeeded() || !settings.value || HeaderVersion(*settings.value) != (luaTarget ? 31u : 25u)) {
            Error(task, "SAVE_MAP_FORMAT_UNSUPPORTED", "源地图 W3I 不是已验证的经典 v25 布局，已拒绝写出。");
            return true;
        }
    }
    for (const auto& replacement : task.replacements) {
        if (!MemberNameIs(replacement.name, "war3map.w3i")) continue;
        if (HeaderVersion(replacement.data) != (luaTarget ? 31u : 25u)) {
            Error(task, "SAVE_MAP_FORMAT_UNSUPPORTED", "目标地图 W3I 不是已验证的经典 v25 布局，已拒绝写出。");
            return true;
        }
    }
    for (const auto& removal : task.removals) {
        if (!MemberNameIs(removal, "war3map.w3i")) continue;
        Error(task, "SAVE_MAP_FORMAT_UNSUPPORTED", "不能删除地图 W3I 成员。");
        return true;
    }
    for (const auto& name : {"war3mapUnits.doo", "war3map.doo"}) {
        const auto unit = std::string_view(name) == "war3mapUnits.doo";
        const auto unsupported = [unit](std::span<const std::byte> bytes) {
            if (unit) {
                const auto parsed = MapData::ReadUnitPlacementsVersioned(bytes);
                return (parsed.Succeeded() && parsed.value->recordLayout == MapData::PlacementRecordLayout::SkinId)
                    || (!parsed.Succeeded() && !parsed.diagnostics.empty()
                        && parsed.diagnostics.front().code == "WIDGET_LAYOUT_AMBIGUOUS");
            }
            const auto parsed = MapData::ReadDoodadPlacementsVersioned(bytes);
            return (parsed.Succeeded() && parsed.value->recordLayout == MapData::PlacementRecordLayout::SkinId)
                || (!parsed.Succeeded() && !parsed.diagnostics.empty()
                    && parsed.diagnostics.front().code == "WIDGET_LAYOUT_AMBIGUOUS");
        };
        if (task.archive->HasMember(name)) {
            const auto source = task.archive->ReadMember(name);
            if (!source.Succeeded() || !source.value || unsupported(*source.value)) {
                Error(task, "SAVE_PLACEMENT_LAYOUT_UNSUPPORTED",
                    std::string(name) + " 的源记录布局不可证明为经典格式，已拒绝写出。");
                return true;
            }
        }
        for (const auto& replacement : task.replacements) {
            if (!MemberNameIs(replacement.name, name)) continue;
            if (unsupported(replacement.data)) {
                Error(task, "SAVE_PLACEMENT_LAYOUT_UNSUPPORTED",
                    std::string(name) + " 的目标记录布局不可证明为经典格式，已拒绝写出。");
                return true;
            }
        }
    }
    return false;
}

void RebuildScript(MapSaveTask& task, const MapSaveService::Progress& progress) {
    if (!task.script) return;
    auto& frozen = *task.script;
    if (!frozen.triggers || !frozen.catalog || !task.archive || frozen.scriptMember.empty()) {
        Error(task, "SCRIPT_INPUT_MISSING", "缺少冻结的归档、触发器、函数目录或脚本成员，无法重建游戏脚本。", "构建");
        return;
    }
    // 只在本次调用栈建立指针和最终成员 span；后台任务本身不储存自指引用。
    Script::MapScriptBuildRequest request;
    request.archive = task.archive.get();
    request.finalSaveMembers = Script::MapScriptFinalMembers{task.replacements, task.removals};
    request.documentRevision = frozen.documentRevision;
    request.triggers = frozen.triggers.get();
    request.catalog = frozen.catalog.get();
    request.standard = frozen.standard.get();
    request.game = frozen.game ? &*frozen.game : nullptr;
    request.units = frozen.units;
    request.items = frozen.items;
    request.abilities = frozen.abilities;
    request.layers = frozen.layers;
    request.toolchain = frozen.toolchain;
    request.luaTarget = frozen.luaTarget ? frozen.luaTarget->Profile() : nullptr;
    request.luaTestOptions = frozen.luaTestOptions;
    if (frozen.luaTarget) request.selectedLuaSettings = frozen.luaTarget->SelectedSettings();
    request.toolchain.preprocessCache = nullptr;
    request.toolchain.onStage = [&progress](std::string_view stage) { Report(progress, stage); };

    const auto started = std::chrono::steady_clock::now();
    const auto firstScriptDiagnostic = task.diagnostics.size();
    auto result = Script::BuildMapScript(request);
    // BuildMapScript 已消费最终成员视图。合并生成物会扩容 replacements，先清除旧 span。
    request.finalSaveMembers.reset();
    Append(task.diagnostics, result.diagnostics);
    Append(task.diagnostics, result.generated.diagnostics);
    if (result.pipeline) Append(task.diagnostics, result.pipeline->diagnostics);
    if (result.luaGenerated) Append(task.diagnostics, result.luaGenerated->diagnostics);
    if (result.luaPipeline) Append(task.diagnostics, result.luaPipeline->diagnostics);
    task.scriptSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const auto workDir = PathUtf8(request.toolchain.workDir);
    if (result.FinalScript() == nullptr || (!result.pipeline && !result.luaPipeline)) {
        // 写明实际失败原因（模板、预处理、编译或检查），不笼统说成语法检查。
        const auto failed = std::find_if(task.diagnostics.begin() + static_cast<std::ptrdiff_t>(firstScriptDiagnostic),
            task.diagnostics.end(), [](const auto& diagnostic) { return diagnostic.severity == DiagnosticSeverity::Error; });
        const auto reason = failed != task.diagnostics.end() ? failed->code + "：" + failed->message
                                                             : std::string("构建没有给出错误诊断");
        Error(task, "REPACK_SCRIPT_REJECTED",
            "游戏脚本重建失败（" + Seconds(task.scriptSeconds) + " 秒）：" + reason
                + "；已中止保存，各阶段文件在 " + workDir, "构建");
        return;
    }
    auto prepared = result.luaPipeline
        ? Script::PrepareScriptSaveBundle(*result.luaPipeline, frozen.scriptMember)
        : Script::PrepareScriptSaveBundle(*result.pipeline, frozen.scriptMember);
    Append(task.diagnostics, prepared.diagnostics);
    if (!prepared.Succeeded() || !prepared.value) return;
    struct OriginalNativeView { bool present; std::span<const std::byte> bytes; };
    const auto originalNative = [&](std::string_view name) -> Result<OriginalNativeView> {
        if (result.slkSources) {
            for (const auto& member : result.slkSources->OriginalMembers()) {
                if (MemberNameIs(member.name, name)) return Result<OriginalNativeView>::Success({
                    member.identity.presence == Script::SlkSessionSourcePresence::Present, member.bytes});
            }
            return Result<OriginalNativeView>::Failure("MapSaveService", "SAVE_NATIVE_BASELINE_UNCAPTURED",
                "模板执行的固定来源域没有原始物编成员：" + std::string(name));
        }
        for (const auto& member : task.archive->Members()) {
            if (MemberNameIs(member.name, name)) return Result<OriginalNativeView>::Success({true, member.data});
        }
        if (task.archive->HasOpaqueMembers()) return Result<OriginalNativeView>::Failure(
            "MapSaveService", "SAVE_NATIVE_BASELINE_UNCAPTURED",
            "物编成员未列名且没有原始来源捕获，不能当作空祖先：" + std::string(name));
        return Result<OriginalNativeView>::Success({false, {}});
    };
    std::array<const ObjectData::LniObjectDocument*, kMapSaveObjectTypeCount> editableInputs{};
    for (std::size_t index = 0; index < editableInputs.size(); ++index)
        editableInputs[index] = frozen.editableInputs[index].get();
    for (std::size_t index = 0; index < editableInputs.size(); ++index) {
        if (!prepared.value->nativeBaselines[index] || editableInputs[index]) continue;
        const auto name = ObjectData::NativeFileName(static_cast<ObjectData::ObjectType>(index));
        const auto original = originalNative(name);
        Append(task.diagnostics, original.diagnostics);
        if (!original.Succeeded() || !original.value) return;
        if (!original.value->present) continue;
        Error(task, "SAVE_GENERATED_NATIVE_BASELINE_UNAVAILABLE",
            "脚本模板将替换已有物编成员 " + std::string(name)
                + "，但本次保存缺少该类别的可编辑来源快照；无法证明整类替换会保留原有语义，已中止保存。", "构建");
        return;
    }
    if (task.requireGeneratedNativeByteEquality) {
        const auto guarded = ValidateGeneratedObjectMembersForSave(*task.archive,
            task.objectTransactions, prepared.value->members, frozen.scriptMember);
        Append(task.diagnostics, guarded.diagnostics);
        if (!guarded.Succeeded()) return;
    }
    // 模板输出是最终 native 成员；编辑器仍须保留当前工程的 LNI 原文与父链。
    // 以冻结源归档为共同祖先，先在隔离文档中合并全部生成类别，再提交任何成员。
    std::unique_ptr<ObjectData::ObjectFieldSchema> fieldSchema;
    std::array<std::unique_ptr<ObjectData::LniObjectDocument>, kMapSaveObjectTypeCount> mergedEditable;
    for (std::size_t index = 0; index < editableInputs.size(); ++index) {
        if (!prepared.value->nativeBaselines[index] || !editableInputs[index]) continue;
        const auto type = static_cast<ObjectData::ObjectType>(index);
        const auto memberName = ObjectData::NativeFileName(type);
        std::unique_ptr<ObjectData::NativeObjectDocument> baseline;
        const auto original = originalNative(memberName);
        Append(task.diagnostics, original.diagnostics);
        if (!original.Succeeded() || !original.value) return;
        if (original.value->present) {
            auto parsed = ObjectData::NativeObjectCodec(type).Parse(original.value->bytes);
            Append(task.diagnostics, parsed.diagnostics);
            auto* native = parsed.Succeeded() && parsed.document
                ? dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get()) : nullptr;
            if (!native || !native->Supports(ObjectData::NativeObjectOperation::ReplaceFromTemplate)
                || !native->IsStructurallySafe()) {
                Error(task, "SAVE_NATIVE_BASELINE_UNSAFE",
                    "冻结源地图物编成员不能作为三方合并祖先：" + std::string(memberName), "构建");
                return;
            }
            baseline.reset(static_cast<ObjectData::NativeObjectDocument*>(parsed.document.release()));
        } else {
            auto empty = ObjectData::NativeObjectDocument::CreateEmpty(type);
            Append(task.diagnostics, empty.diagnostics);
            if (!empty.Succeeded() || !empty.value) {
                Error(task, "SAVE_NATIVE_BASELINE_UNAVAILABLE",
                    "缺失类别不能建立空 native 合并祖先：" + std::string(memberName), "构建");
                return;
            }
            baseline = std::make_unique<ObjectData::NativeObjectDocument>(std::move(*empty.value));
        }
        if (!fieldSchema && frozen.game) {
            auto loaded = ObjectData::ObjectFieldSchema::Load(*frozen.game);
            Append(task.diagnostics, loaded.diagnostics);
            if (!loaded.Succeeded() || !loaded.value) {
                Error(task, "SAVE_OBJECT_SCHEMA_UNAVAILABLE",
                    "游戏物编字段元数据不可用，不能证明模板生成字段的完整存储类型。", "构建");
                return;
            }
            fieldSchema = std::make_unique<ObjectData::ObjectFieldSchema>(std::move(*loaded.value));
        }
        auto mergedLni = ObjectData::MergeGeneratedNativeWithLniEdits(
            *baseline, *editableInputs[index], *prepared.value->nativeBaselines[index],
            fieldSchema.get(), frozen.standard.get());
        Append(task.diagnostics, mergedLni.diagnostics);
        if (!mergedLni.Succeeded() || !mergedLni.value) {
            Error(task, "SAVE_LNI_MERGE_REJECTED",
                "生成 native 不能无损合入当前 LNI 编辑状态：" + std::string(memberName), "构建");
            return;
        }
        mergedEditable[index] = std::make_unique<ObjectData::LniObjectDocument>(std::move(*mergedLni.value));
    }
    const auto merged = frozen.luaTarget
        ? LuaMapSave::MergeCompletedScript(task, prepared.value->members)
        : Script::MergeScriptSaveMembers(task.replacements, task.removals, prepared.value->members);
    Append(task.diagnostics, merged.diagnostics);
    if (!merged.Succeeded()) return;
    for (std::size_t index = 0; index < kMapSaveObjectTypeCount; ++index) {
        if (!prepared.value->nativeBaselines[index]) continue;
        task.nextBaselines[index] = std::move(prepared.value->nativeBaselines[index]);
        task.generatedObjectBaselines[index] = mergedEditable[index]
            ? std::move(mergedEditable[index]) : std::move(prepared.value->editableBaselines[index]);
    }
    task.scriptRebuilt = true;
    const auto triggerCount = result.luaGenerated ? frozen.triggers->triggers.size() : result.generated.triggerCount;
    const auto& checkedBy = result.luaPipeline ? result.luaPipeline->checkedBy : result.pipeline->checkedBy;
    task.diagnostics.push_back({DiagnosticSeverity::Info, "构建", "SCRIPT_REBUILT",
        "游戏脚本已重建：" + std::to_string(triggerCount) + " 个触发器，检查方式："
            + checkedBy + "（" + Seconds(task.scriptSeconds) + " 秒）；各阶段文件在 " + workDir});
    if (frozen.luaTestOptions.testHotReload && result.luaGenerated)
        task.generatedLuaTestUnit = std::move(result.luaGenerated->triggers);
    task.script.reset();
}

} // namespace

Status MapSaveTask::FreezeArchive(const MapArchiveSession& source) {
    archive.reset();
    sourcePath.clear();
    sourceSha256.clear();
    try {
        auto frozen = std::make_unique<MapArchiveSession>(source);
        std::filesystem::path path = frozen->SourcePath();
        std::string sha256 = frozen->SourceSha256();
        archive = std::move(frozen);
        sourcePath = std::move(path);
        sourceSha256 = std::move(sha256);
        return Status::Ok();
    } catch (...) {
        auto failed = Status::Failure("MapSaveService", "SAVE_ARCHIVE_FREEZE_FAILED",
            "无法冻结地图归档快照；保存未开始。");
        // 即使诊断列表扩容也失败，返回值仍携带错误，调用方必须检查它。
        try {
            Append(diagnostics, failed.diagnostics);
        } catch (...) {
        }
        return failed;
    }
}

namespace {

void ExecuteSaveTask(MapSaveTask& task, const MapSaveService::Progress& progress, bool preflight) {
    if (task.executed) {
        task.succeeded = false;
        Error(task, "SAVE_TASK_ALREADY_EXECUTED", "同一冻结保存任务不能再次执行。");
        return;
    }
    task.executed = true;
    task.succeeded = false;
    bool archiveWritten = false;
    try {
        if (!task.archive || task.sourcePath.empty() || task.sourceSha256.size() != 64 ||
            task.archive->SourcePath() != task.sourcePath || task.archive->SourceSha256() != task.sourceSha256) {
            Error(task, "SAVE_SOURCE_SNAPSHOT_INVALID", "冻结的源归档路径或 SHA-256 与保存任务不一致。");
            return;
        }
        if (task.target.empty() && !preflight) {
            Error(task, "SAVE_TARGET_INVALID", "地图保存目标路径为空。");
            return;
        }
        if (task.script && task.script->luaTestOptions.testHotReload) {
            std::error_code equivalentError;
            const bool same = !task.target.empty()
                && std::filesystem::equivalent(task.target, task.sourcePath, equivalentError);
            if (!task.testCopyOnly || task.inPlace || same || task.target.lexically_normal() == task.sourcePath.lexically_normal()
                || !task.script->luaTarget) {
                Error(task, "LUA_TEST_SAVE_REQUIRED", "热重载登记层只能用于明确的独立 Lua 测试副本，不能写回原图或正式保存。");
                return;
            }
        }
        if (task.inPlace && !task.target.empty()) {
            std::error_code pathError;
            if (!std::filesystem::equivalent(task.target, task.sourcePath, pathError) || pathError) {
                Error(task, "SAVE_SOURCE_MISMATCH", "原地保存目标与冻结的源归档不一致。");
                return;
            }
        }
        if (RejectUnsupportedMapFormat(task)) return;
        const auto verifyObjects = [&]() {
            if (!task.mapObjectReferences) return true;
            const auto current = CaptureArchiveMapObjectReferences(*task.archive, task.mapObjectReferenceGameRoot,
                task.replacements, task.removals);
            Append(task.diagnostics, current.diagnostics);
            if (!current.Succeeded() || !current.value) return false;
            const auto verified = VerifyMapObjectReferenceContents(*task.mapObjectReferences, **current.value);
            Append(task.diagnostics, verified.diagnostics);
            return verified.Succeeded();
        };
        if (!verifyObjects()) return;
        RebuildScript(task, progress);
        if (HasError(task.diagnostics)) return;
        // 模板产物可能替换分类成员；必须对最终成员再校验，不能只信编译前的类型。
        if (!verifyObjects()) return;
        if (task.generatedLuaTestUnit) {
            const auto binding = CaptureLuaTestMapBinding(*task.archive, task.replacements, task.removals, task.generatedScriptMembers);
            Append(task.diagnostics, binding.diagnostics);
            if (!binding.Succeeded() || !binding.value) return;
            task.luaTestMapBinding = *binding.value;
        }

        if (task.beforePublish) {
            const auto guard = task.beforePublish();
            Append(task.diagnostics, guard.diagnostics);
            if (!guard.Succeeded()) return;
        }

        if (preflight) {
            Report(progress, "archive-preflight");
            const auto checked = task.archive->PreflightSave(task.replacements, task.removals,
                task.inPlace ? std::filesystem::path{} : task.target);
            Append(task.diagnostics, checked.diagnostics);
            return;
        }

        ArchiveSaveResult written;
        if (task.inPlace) {
            Report(progress, "archive-in-place");
            auto saved = task.archive->SaveInPlace(task.replacements, task.removals, task.inPlaceOptions);
            if (!saved.Succeeded() || !saved.value) {
                Append(task.diagnostics, saved.diagnostics);
                return;
            }
            archiveWritten = true;
            Append(task.diagnostics, saved.diagnostics);
            written = saved.value->written;
            task.written = written.path;
            task.memberCount = written.memberCount;
            task.backup = saved.value->backup;
            task.prunedBackups = saved.value->prunedBackups.size();
        } else {
            Report(progress, "archive-candidate");
            auto candidate = task.archive->SaveCandidate(task.target, task.replacements, task.removals);
            if (!candidate.Succeeded() || !candidate.value) {
                Append(task.diagnostics, candidate.diagnostics);
                return;
            }
            archiveWritten = true;
            Append(task.diagnostics, candidate.diagnostics);
            written = *candidate.value;
            task.written = written.path;
            task.memberCount = written.memberCount;
        }
        Report(progress, "archive-reopen");
        auto next = task.archive->AfterSave(written, task.replacements, task.removals);
        Append(task.diagnostics, next.diagnostics);
        if (!next.Succeeded() || !next.value) {
            Error(task, "REPACK_REOPEN_FAILED", task.inPlace
                ? "地图已写回，但无法重新载入；请重新打开地图后再继续编辑。"
                : "地图已写出，但无法建立新会话；当前基线保持不变。");
            return;
        }
        task.nextSession = std::make_unique<MapArchiveSession>(std::move(*next.value));
        task.succeeded = true;
    } catch (const std::exception&) {
        Error(task, "SAVE_INTERNAL_CPP_EXCEPTION", archiveWritten
            ? "地图已写出，但保存后处理发生异常；当前工程基线未切换，请重新打开地图。"
            : "保存过程中发生异常；当前工程基线未切换，请核对目标文件状态。");
    } catch (...) {
        Error(task, "SAVE_INTERNAL_NONSTANDARD_EXCEPTION", archiveWritten
            ? "地图已写出，但保存后处理发生非标准 C++ 异常；当前工程基线未切换，请重新打开地图。"
            : "保存过程中发生非标准 C++ 异常；当前工程基线未切换，请核对目标文件状态。");
    }
}

} // namespace

void MapSaveService::Run(MapSaveTask& task, const Progress& progress) {
    ExecuteSaveTask(task, progress, false);
}

Status MapSaveService::Preflight(MapSaveTask& task, const Progress& progress) {
    ExecuteSaveTask(task, progress, true);
    return Status{task.diagnostics};
}

} // namespace War3Map::Services
