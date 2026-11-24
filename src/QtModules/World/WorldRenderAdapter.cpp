#include "WorldRenderAdapter.h"

#include "War3MapCore/Render/RenderAdapters.h"

#include <QCoreApplication>
#include <QLibrary>
#include <QMutex>
#include <QSettings>

#include <optional>

namespace War3Map::Modules {
namespace {
QMutex overrideMutex;
std::optional<QString> overrideText;

QString Unavailable(const RenderAdapterPreference& preference) {
    return QCoreApplication::translate("WorldRenderAdapter", "所选显卡“%1”不可用，已使用系统默认显卡")
        .arg(preference.name.isEmpty() ? preference.text : preference.name);
}
} // namespace

void SetRenderAdapterOverride(const QString& text) {
    QMutexLocker lock(&overrideMutex);
    overrideText = text.trimmed();
}

RenderAdapterPreference ReadRenderAdapterPreference() {
    {
        QMutexLocker lock(&overrideMutex);
        if (overrideText) {
            if (overrideText->compare(QStringLiteral("default"), Qt::CaseInsensitive) == 0) return {};
            return {*overrideText, *overrideText};
        }
    }
    const QSettings settings;
    return {settings.value(QString::fromLatin1(kRenderAdapterSettingsKey)).toString(),
        settings.value(QString::fromLatin1(kRenderAdapterNameSettingsKey)).toString()};
}

QString ApplyRenderAdapterPreference(QLibrary& library) {
    const auto preference = ReadRenderAdapterPreference();
    const auto set = reinterpret_cast<Render::War3SetRenderAdapterV1>(library.resolve(Render::kSetRenderAdapterExportV1));
    if (!set) {
        return preference.text.isEmpty() ? QString()
            : QCoreApplication::translate("WorldRenderAdapter", "渲染插件不支持选择显卡，已使用系统默认显卡");
    }
    std::uint64_t luid = 0;
    bool missing = false;
    if (!preference.text.isEmpty()) {
        const auto adapters = Render::EnumerateRenderAdapters();
        const auto* found = adapters.Succeeded() ? Render::FindRenderAdapter(*adapters.value, preference.text.toStdString()) : nullptr;
        if (found) luid = found->luid;
        else missing = true;
    }
    if (set(luid) != 0) missing = true;
    return missing ? Unavailable(preference) : QString();
}

} // namespace War3Map::Modules
