#include "EditorProject.h"
#include "MapSaveJob.h"

#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"
#include "War3MapCore/Text/SourceEncoding.h"
#include "War3MapCore/Trigger/TriggerDocument.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <utility>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

// 界面线程仅冻结当前工程文档；工具链、游戏数据与工作目录由无 Qt 的共用服务准备。
namespace War3Map::QtProject {
namespace {

// 与 MapCopySave.cpp 的 kObjectTypes 同序：单位、技能、物品、魔法效果、科技、装饰物、可破坏物。
constexpr std::size_t kUnitIndex = 0;
constexpr std::size_t kAbilityIndex = 1;
constexpr std::size_t kItemIndex = 2;

struct ScriptSourceDeclarations {
    std::vector<Script::ScriptProfileSourceEncoding> encodings;
    std::vector<Script::ScriptProfileIncludeAccess> access;
};
Result<ScriptSourceDeclarations> ReadSourceDeclarations(const QString& fileName) {
    using Output = Result<ScriptSourceDeclarations>;
    const auto path = std::filesystem::path(fileName.toStdWString());
    const auto fail = [&](std::string message) {
        auto result = Output::Failure("ScriptSave.Prepare", "SCRIPT_SOURCE_ENCODINGS_FILE_INVALID", std::move(message));
        result.diagnostics.back().origin = OriginSpan{path}; return result;
    };
    if (!path.is_absolute()) return fail("来源编码声明文件须为明确绝对路径。");
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) return fail("无法读取来源编码声明文件，未退回默认编码。");
    constexpr qint64 limit = 1024 * 1024;
    if (file.size() < 0 || file.size() > limit) return fail("来源编码声明文件超过1MiB预算。");
    const auto bytes = file.read(limit + 1);
    if (bytes.size() > limit || file.error() != QFileDevice::NoError) return fail("声明文件读取失败或超过预算。");
    const std::string_view text(bytes.constData(), static_cast<std::size_t>(bytes.size()));
    for (std::uint32_t at = 0; at < text.size();) {
        auto scalar = Text::ReadSourceScalar(text, at, Text::SourceEncoding::Utf8);
        if (!scalar.Succeeded() || !scalar.value) return fail("来源编码声明文件必须为有效UTF-8 JSON。");
        at += scalar.value->bytes;
    }
    QJsonParseError error;
    const auto json = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !json.isObject()) return fail("来源编码声明文件须为JSON对象。");
    const auto object = json.object();
    if (object.isEmpty() || object.size() != static_cast<int>(object.contains("source_encodings"))
        + static_cast<int>(object.contains("include_access"))
        || (object.contains("source_encodings") && !object.value("source_encodings").isArray())
        || (object.contains("include_access") && !object.value("include_access").isArray()))
        return fail("声明文件仅允许source_encodings与include_access数组，不允许工具、版本或UI覆盖字段。");
    const auto entries = object.value("source_encodings").toArray();
    if (entries.size() > 1024) return fail("来源编码声明超过1024项。");
    std::vector<Script::ScriptProfileSourceEncoding> declarations;
    for (const auto& entry : entries) {
        if (!entry.isObject()) return fail("来源编码声明项须为对象。");
        const auto item = entry.toObject();
        if (item.size() != 2 || !item.value("path").isString() || !item.value("encoding").isString())
            return fail("来源声明项只接受path与encoding字符串。");
        auto declaration = Services::ParseScriptSourceEncodingDeclaration(
            std::filesystem::path(item.value("path").toString().toStdWString()), item.value("encoding").toString().toStdString());
        if (!declaration.Succeeded() || !declaration.value) return {{}, std::move(declaration.diagnostics)};
        declarations.push_back(std::move(*declaration.value));
    }
    auto normalized = Services::NormalizeScriptSourceEncodingDeclarations(std::move(declarations));
    if (!normalized.Succeeded() || !normalized.value) return {{}, std::move(normalized.diagnostics)};
    const auto accessEntries = object.value("include_access").toArray();
    if (accessEntries.size() > 128) return fail("普通include范围声明超过128项。");
    std::vector<Script::ScriptProfileIncludeAccess> access;
    for (const auto& entry : accessEntries) {
        if (!entry.isObject()) return fail("include范围声明项须为对象。");
        const auto item = entry.toObject();
        if (item.size() != 3 + static_cast<int>(item.contains("required"))
            || !item.value("path").isString() || !item.value("kind").isString() || !item.value("encoding").isString()
            || (item.contains("required") && !item.value("required").isBool()))
            return fail("include范围项只接受path、kind、encoding字符串与可选required布尔值。");
        auto declaration = Services::ParseScriptIncludeAccessDeclaration(
            std::filesystem::path(item.value("path").toString().toStdWString()),
            item.value("kind").toString().toStdString(), item.value("encoding").toString().toStdString(),
            item.value("required").toBool(true));
        if (!declaration.Succeeded() || !declaration.value) return {{}, std::move(declaration.diagnostics)};
        access.push_back(std::move(*declaration.value));
    }
    auto normalizedAccess = Services::NormalizeScriptIncludeAccessDeclarations(std::move(access));
    if (!normalizedAccess.Succeeded() || !normalizedAccess.value) return {{}, std::move(normalizedAccess.diagnostics)};
    return Output::Success({std::move(*normalized.value), std::move(*normalizedAccess.value)});
}

} // namespace

EditorProject::ScriptRebuild EditorProject::PrepareScriptRebuild(MapSaveJob& job) {
    Services::ScriptSaveInputSources sources;
    if (luaMapTarget_) {
        if (luaMapTargetSource_ != sourceArchivePath_) {
            AddDiagnostic(tr("错误"), tr("构建"), tr("LUA_MAP_TARGET_SOURCE_CHANGED"),
                tr("当前Lua档案属于另一张地图，请为当前地图重新选择或清除档案。"));
            return ScriptRebuild::Failed;
        }
        sources.luaTarget = luaMapTarget_;
    }
    sources.documentRevision = editRevision_.Revision();
    // 仅进程读取；文件是提交配置，任务拥有其typed声明，不声称编译期仍持有文件租约。
    const auto processFile = qEnvironmentVariable("FORGE_SCRIPT_SOURCE_ENCODINGS_FILE");
    const auto declarationFile = processFile.isEmpty()
        ? QSettings{}.value("script/sourceEncodingsFile").toString() : processFile;
    if (!declarationFile.isEmpty()) {
        auto declarations = ReadSourceDeclarations(declarationFile);
        AddDiagnostics(declarations.diagnostics);
        if (!declarations.Succeeded() || !declarations.value) return ScriptRebuild::Failed;
        scriptSourceEncodings_ = std::move(declarations.value->encodings);
        scriptSourceIncludeAccess_ = std::move(declarations.value->access);
        scriptSourceEncodingsFile_ = std::filesystem::path(declarationFile.toStdWString());
    } else if (!scriptSourceEncodingsFile_.empty()) {
        scriptSourceEncodings_.clear();
        scriptSourceIncludeAccess_.clear();
        scriptSourceEncodingsFile_.clear();
    }
    sources.sourceEncodings = scriptSourceEncodings_;
    sources.includeAccess = scriptSourceIncludeAccess_;
    if (triggerDocument_) sources.triggers = std::make_unique<Trigger::TriggerDocument>(*triggerDocument_);
    if (uiCatalog_) sources.catalog = std::make_unique<Trigger::UiCatalog>(*uiCatalog_);
    sources.documentRevision = editRevision_.Revision();
    if (sources.catalog) sources.layers = sources.catalog->Layers();
    if (!uiLibraryPackages_.empty()) {
        sources.expectedEffectiveLayers = UiLibrary::EffectiveLayers(
            uiLibraryGlobal_, uiLibraryMapOnly_, uiLibraryPackages_);
    }

    const auto records = [&job](std::size_t index) {
        std::vector<ObjectData::ObjectRecord> objects;
        if (job.nextBaselines[index]) {
            objects.assign(job.nextBaselines[index]->Objects().begin(), job.nextBaselines[index]->Objects().end());
        }
        return objects;
    };
    sources.units = records(kUnitIndex);
    sources.items = records(kItemIndex);
    sources.abilities = records(kAbilityIndex);
    for (std::size_t index = 0; index < kObjectTypeCount; ++index) {
        if (!objectDocuments_[index]) continue;
        // v1的已有字段可修改，不等于模板可以整类替换。脚本查询仍读取当前值，
        // 模板的完整基线资格单独检查，不能从一般编辑能力推定。
        if (job.nextBaselines[index]
            && !job.nextBaselines[index]->Supports(ObjectData::NativeObjectOperation::ReplaceFromTemplate)) continue;
        const auto type = static_cast<ObjectData::ObjectType>(index);
        if (!job.nextBaselines[index] && job.archive
            && job.archive->HasMember(ObjectData::NativeFileName(type))) continue;
        sources.editableInputs[index] = std::make_unique<ObjectData::LniObjectDocument>(*objectDocuments_[index]);
    }
    sources.toolRoot = compilerDataRoot_.empty() ? activeUiDataRoot_ : compilerDataRoot_;
    sources.uiRoot = activeUiDataRoot_;
    sources.gameRoot = war3GameRoot_;
    if (job.archive) sources.scriptMember = Script::MapScriptMemberName(*job.archive);
    // 触发器和地图初始化有修改时必须真正重建；不能把 WTG 已保存冒充游戏脚本已更新。
    sources.allowMissingToolchain = !triggerDirty_ && !(mapEdits_ && mapEdits_->IsDirty());

    auto prepared = Services::PrepareScriptSaveInputs(job, std::move(sources));
    AddDiagnostics(prepared.diagnostics);
    if (!prepared.Succeeded() || !prepared.value) return ScriptRebuild::Failed;
    return *prepared.value == Services::ScriptSavePreparationState::Skipped
        ? ScriptRebuild::Skipped : ScriptRebuild::Rebuilt;
}

} // namespace War3Map::QtProject
