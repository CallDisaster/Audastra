#include "EditorProject.h"

#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

#include <algorithm>

// UI 函数库两层启用的工程侧状态（架构文档 §4.3）：全局设置按数据根保存在编辑器本机设置中，
// 从不修改数据根的 config；地图用到本机已关闭的包时只对当前地图启用。
namespace War3Map::QtProject {
namespace {

QString SettingsKey(const std::filesystem::path& uiRoot) {
    const QString normalized = QString::fromStdWString(
        Trigger::UiCatalog::ResolveUiRoot(uiRoot).lexically_normal().wstring()).toLower();
    return QStringLiteral("uiLibraries/v1/") +
        QString::fromLatin1(QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString Text(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

} // namespace

UiLibrary::GlobalSettings EditorProject::ReadUiLibrarySettings(const std::filesystem::path& uiRoot) {
    UiLibrary::GlobalSettings settings;
    const QJsonArray entries = QJsonDocument::fromJson(QSettings{}.value(SettingsKey(uiRoot)).toByteArray()).array();
    for (const auto& value : entries) {
        const QJsonObject entry = value.toObject();
        settings.packages.push_back({entry["id"].toString().toStdString(), entry["enabled"].toBool(),
            entry["fingerprint"].toString().toStdString()});
    }
    return settings;
}

void EditorProject::WriteUiLibrarySettings(const std::filesystem::path& uiRoot, const UiLibrary::GlobalSettings& settings) {
    QJsonArray entries;
    for (const auto& entry : settings.packages) {
        entries.push_back(QJsonObject{{"id", Text(entry.id)}, {"enabled", entry.enabled}, {"fingerprint", Text(entry.fingerprint)}});
    }
    QSettings{}.setValue(SettingsKey(uiRoot), QJsonDocument(entries).toJson(QJsonDocument::Compact));
}

Result<Trigger::UiCatalog> EditorProject::LoadEffectiveUiCatalog(const std::filesystem::path& uiRoot) {
    auto discovered = UiLibrary::DiscoverPackages(uiRoot);
    if (!discovered.value || discovered.value->empty()) {
        // 无法识别包结构时退回原有 config 载入，行为与未引入两层启用前一致。
        uiLibraryPackages_.clear();
        uiLibraryGlobal_ = {};
        return Trigger::UiCatalog::Load(uiRoot);
    }
    uiLibraryPackages_ = std::move(*discovered.value);
    uiLibraryGlobal_ = UiLibrary::ReconcileGlobal(ReadUiLibrarySettings(uiRoot), uiLibraryPackages_);
    const auto layers = UiLibrary::EffectiveLayers(uiLibraryGlobal_, uiLibraryMapOnly_, uiLibraryPackages_);
    auto catalog = Trigger::UiCatalog::LoadLayers(uiRoot, layers);
    catalog.diagnostics.insert(catalog.diagnostics.begin(), discovered.diagnostics.begin(), discovered.diagnostics.end());
    return catalog;
}

std::optional<EditorProject::ParsedTriggers> EditorProject::ParseWithLocallyDisabledLibraries(
    const std::filesystem::path& uiRoot, const std::vector<std::byte>& wtg, const std::vector<std::byte>& wct) {
    std::vector<std::string> disabled;
    for (const auto& entry : uiLibraryGlobal_.packages) {
        if (!entry.enabled) disabled.push_back(entry.id);
    }
    if (disabled.empty()) return std::nullopt;
    const auto attempt = [&](const std::vector<std::string>& mapOnly) -> std::optional<ParsedTriggers> {
        const auto layers = UiLibrary::EffectiveLayers(uiLibraryGlobal_, mapOnly, uiLibraryPackages_);
        auto catalog = Trigger::UiCatalog::LoadLayers(uiRoot, layers);
        if (!catalog.Succeeded()) return std::nullopt;
        auto parsed = Trigger::WtgWctCodec{}.Parse(wtg, wct, *catalog.value);
        if (!parsed.Succeeded()) return std::nullopt;
        return ParsedTriggers{std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value)),
            std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value))};
    };
    auto parsed = attempt(disabled);
    if (!parsed) return std::nullopt;
    // 只保留实际用到的已关闭包，避免未使用的包改变其它函数的生效来源。
    std::vector<std::string> used;
    for (const auto& entry : UiLibrary::ComputeMapUsage(*parsed->document, uiLibraryPackages_).packages) {
        if (std::find(disabled.begin(), disabled.end(), entry.id) != disabled.end()) used.push_back(entry.id);
    }
    if (used.size() != disabled.size()) {
        parsed = attempt(used);
        if (!parsed) return std::nullopt;
    }
    uiLibraryMapOnly_ = used;
    for (const auto& id : used) {
        AddDiagnostic(tr("警告"), tr("函数库"), tr("UI_LIBRARY_MAP_ONLY"),
            tr("本图使用了本机已关闭的函数库 %1，已仅对此地图启用；全局设置不变。").arg(Text(id)));
    }
    return parsed;
}

void EditorProject::RecordUiLibraryUsage() {
    uiLibraryUsage_.reset();
    if (!triggerDocument_ || uiLibraryPackages_.empty()) return;
    uiLibraryUsage_ = UiLibrary::ComputeMapUsage(*triggerDocument_, uiLibraryPackages_);
    for (const auto& check : UiLibrary::CheckMapUsage(*uiLibraryUsage_, uiLibraryGlobal_, uiLibraryPackages_)) {
        if (check.status == UiLibrary::PackageStatus::Missing) {
            AddDiagnostic(tr("警告"), tr("函数库"), tr("UI_LIBRARY_MISSING"),
                tr("本图引用的函数库 %1 在本机数据根中不存在。").arg(Text(check.id)));
        }
    }
}

QJsonArray EditorProject::UiLibraryUsageJson() const {
    QJsonArray packages;
    if (!uiLibraryUsage_) return packages;
    for (const auto& entry : uiLibraryUsage_->packages) {
        packages.push_back(QJsonObject{{"id", Text(entry.id)}, {"fingerprint", Text(entry.fingerprint)},
            {"functions", static_cast<qint64>(entry.functionCount)}});
    }
    return packages;
}

bool EditorProject::ApplyUiLibrarySettings(const UiLibrary::GlobalSettings& settings, std::vector<std::string> mapOnly) {
    const std::filesystem::path uiRoot = ResolveUiDataRoot();
    if (uiRoot.empty()) return false;
    WriteUiLibrarySettings(uiRoot, settings);
    uiLibraryMapOnly_ = std::move(mapOnly);
    return LoadTriggers();
}

} // namespace War3Map::QtProject
