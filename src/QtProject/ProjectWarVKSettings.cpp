// WarVK 渲染设置（地图成员 audastra\warvk.ini）：查询、修改、远景装饰物标记与撤销记录。
// 设置作为编辑器自有的待保存导入发布；保存时脚本生成据此写出受管的 WarVK 初始化指令。
#include "EditorProject.h"

#include <QUndoCommand>
#include <QUndoStack>

#include <algorithm>
#include <memory>

namespace War3Map::QtProject {
namespace {
constexpr EditorProject::OwnedMemberSpec kWarVKSettings{MapData::kWarVKSettingsMember, MapData::kWarVKSettingsMaxBytes,
    "MapData.WarVK", "WARVK_SETTINGS_INVALID", "WarVK 设置"};

Status Fail(const char* code, std::string message) { return Status::Failure("MapData.WarVK", code, std::move(message)); }
} // namespace

// 一条 WarVK 设置修改。按设置值（不是整份资源状态）核对前值：其他资源改动或保存之后仍能撤销重做；
// 当前设置已被其他操作改掉时标为过期，不覆盖。
class WarVKSettingsCommand final : public QUndoCommand {
public:
    WarVKSettingsCommand(EditorProject& project, MapData::WarVKMapSettings before, MapData::WarVKMapSettings after,
        const QString& label)
        : QUndoCommand(label), project_(project), before_(std::move(before)), after_(std::move(after)) {}
    void undo() override { Apply(false); }
    void redo() override { if (published_) published_ = false; else Apply(true); }
    // 首次修改已在入栈前发布；入栈时的 redo 不再重复发布。
    void MarkPublished() noexcept { published_ = true; }

private:
    void Apply(bool forward) {
        const auto& expected = forward ? before_ : after_;
        const auto& wanted = forward ? after_ : before_;
        Status status;
        const auto current = project_.QueryWarVKMapSettings();
        if (!current.Succeeded() || !current.value) status.diagnostics = current.diagnostics;
        else if (*current.value != expected)
            status = Fail("WARVK_UNDO_STALE", "WarVK 设置已被其他操作改动；此次撤销或重做未修改设置。");
        else status = project_.PublishWarVKMapSettings(wanted, forward ? EditOperation::Redo : EditOperation::Undo);
        if (status.Succeeded()) return;
        setObsolete(true);
        for (const auto& diagnostic : status.diagnostics)
            project_.AddDiagnostic(QObject::tr("错误"), QObject::tr("地图设置"),
                QString::fromStdString(diagnostic.code), QString::fromStdString(diagnostic.message));
    }

    EditorProject& project_;
    MapData::WarVKMapSettings before_, after_;
    bool published_ = false;
};

Result<MapData::WarVKMapSettings> EditorProject::QueryWarVKMapSettings() const {
    const auto bytes = ReadOwnedMapMember(kWarVKSettings);
    if (!bytes.Succeeded() || !bytes.value) return {std::nullopt, bytes.diagnostics};
    return *bytes.value ? MapData::ParseWarVKMapSettings(**bytes.value) : Result<MapData::WarVKMapSettings>::Success({});
}

Status EditorProject::PublishWarVKMapSettings(const MapData::WarVKMapSettings& settings, EditOperation operation) {
    if (savingMap_) return Fail("WARVK_SETTINGS_BUSY", "地图正在保存，稍后再修改 WarVK 设置。");
    if (!editTrackingReady_ || editTrackingFailed_ || sourceChangeSuspected_ || sourceChangeConfirmed_)
        return Fail("WARVK_SETTINGS_STALE", "地图来源或编辑修订尚未核实，未修改 WarVK 设置。");
    const auto bytes = MapData::SerializeWarVKMapSettings(settings);
    if (!bytes.Succeeded() || !bytes.value) return {bytes.diagnostics};
    const auto base = ReadBaseOwnedMapMember(kWarVKSettings);
    if (!base.Succeeded() || !base.value) return {base.diagnostics};
    // 与地图里已有的内容相同（地图没有该文件时即为空设置）时不留待导入文件；否则以新内容覆盖。
    bool sameAsBase = settings == MapData::WarVKMapSettings{};
    if (*base.value) {
        const auto parsed = MapData::ParseWarVKMapSettings(**base.value);
        sameAsBase = parsed.Succeeded() && parsed.value && *parsed.value == settings;
    }
    const auto member = QString::fromUtf8(MapData::kWarVKSettingsMember.data(),
        static_cast<qsizetype>(MapData::kWarVKSettingsMember.size()));
    const auto key = AssetKey(member);
    auto candidate = CaptureAssets();
    std::erase_if(candidate.imports, [&](const PendingImport& item) { return AssetKey(item.archivePath) == key; });
    if (!sameAsBase) {
        std::vector<ImportPayload> payloads{{member, std::make_shared<const std::vector<std::byte>>(*bytes.value)}};
        const auto problem = PrepareImportPayloads(candidate, payloads, true);
        if (!problem.isEmpty()) return Fail("WARVK_SETTINGS_CONFLICT", problem.toStdString());
    }
    auto prepared = PrepareAssetStateEdit(std::move(candidate), editRevision_.Generation(), editRevision_.Revision(),
        "WarVK 设置", operation);
    if (!prepared.Succeeded() || !prepared.value) return {prepared.diagnostics};
    return PublishPreparedAssetState(**prepared.value);
}

Status EditorProject::EditWarVKMapSettings(const MapData::WarVKMapSettings& settings, const QString& undoLabel) {
    if (!mapSettingsUndoStack_) return Fail("WARVK_SETTINGS_UNDO", "地图设置撤销栈未就绪。");
    if (const auto valid = MapData::ValidateWarVKMapSettings(settings); !valid.Succeeded()) return valid;
    const auto before = QueryWarVKMapSettings();
    if (!before.Succeeded() || !before.value) return {before.diagnostics};
    if (*before.value == settings) return Status::Ok();
    if (const auto published = PublishWarVKMapSettings(settings, EditOperation::Edit); !published.Succeeded()) return published;
    auto command = std::make_unique<WarVKSettingsCommand>(*this, *before.value, settings, undoLabel);
    command->MarkPublished();
    mapSettingsUndoStack_->push(command.release());
    return Status::Ok();
}

Status EditorProject::SetWarVKFarDoodadType(std::string_view rawcode, bool far) {
    const auto current = QueryWarVKMapSettings();
    if (!current.Succeeded() || !current.value) return {current.diagnostics};
    auto types = MapData::SetWarVKFarDoodadType(current.value->farDoodadTypes, rawcode, far);
    if (!types.Succeeded() || !types.value) return {types.diagnostics};
    auto next = *current.value;
    next.farDoodadTypes = std::move(*types.value);
    const auto code = QString::fromLatin1(rawcode.data(), static_cast<qsizetype>(rawcode.size()));
    return EditWarVKMapSettings(next, far ? tr("标为远景 %1").arg(code) : tr("取消远景 %1").arg(code));
}

} // namespace War3Map::QtProject
