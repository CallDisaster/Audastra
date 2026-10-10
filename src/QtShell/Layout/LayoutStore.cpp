#include "Layout/LayoutStore.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>

#include <cmath>

namespace War3Map::Shell {
namespace {

QString Text(const ModuleId& id) {
    return QString::fromStdString(id);
}

ModuleId Id(const QJsonValue& value) {
    return value.toString().toStdString();
}

// JSON 数字必须是有限整数范围内的值；否则视为非法。
std::optional<int> Int(const QJsonValue& value) {
    if (!value.isDouble()) {
        return std::nullopt;
    }
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::abs(number) > 1'000'000.0) {
        return std::nullopt;
    }
    return static_cast<int>(std::lround(number));
}

} // namespace

QJsonObject LayoutToJson(const DockLayout& layout) {
    QJsonArray side;
    for (const auto& id : layout.side) {
        side.append(Text(id));
    }
    QJsonArray floats;
    for (const auto& window : layout.floats) {
        floats.append(QJsonObject{
            {"id", Text(window.id)}, {"x", window.x}, {"y", window.y},
            {"w", window.w}, {"h", window.h},
            {"maximized", window.maximized}, {"collapsed", window.collapsed},
        });
    }
    QJsonObject sizes;
    for (const auto& [id, size] : layout.sizes) {
        sizes.insert(Text(id), QJsonObject{{"w", size.w}, {"h", size.h}});
    }
    QJsonObject json{
        {"mainActive", Text(layout.mainActive)},
        {"side", side},
        {"sideWidth", layout.sideWidth},
        {"floats", floats},
        {"sizes", sizes},
    };
    if (layout.sideActive) {
        json.insert("sideActive", Text(*layout.sideActive));
    }
    return json;
}

DockLayout LayoutFromJson(const QJsonValue& value, const std::vector<ModuleId>& known) {
    if (!value.isObject()) {
        return DefaultLayout();
    }
    const QJsonObject raw = value.toObject();
    DockLayout layout;
    layout.mainActive = Id(raw.value("mainActive"));
    for (const auto& item : raw.value("side").toArray()) {
        layout.side.push_back(Id(item));
    }
    if (raw.value("sideActive").isString()) {
        layout.sideActive = Id(raw.value("sideActive"));
    }
    layout.sideWidth = Int(raw.value("sideWidth")).value_or(DefaultLayout().sideWidth);
    for (const auto& item : raw.value("floats").toArray()) {
        const QJsonObject object = item.toObject();
        const auto x = Int(object.value("x"));
        const auto y = Int(object.value("y"));
        const auto w = Int(object.value("w"));
        const auto h = Int(object.value("h"));
        if (!x || !y || !w || !h) {
            continue;
        }
        layout.floats.push_back({Id(object.value("id")), *x, *y, *w, *h,
            object.value("maximized").toBool(), object.value("collapsed").toBool()});
    }
    const QJsonObject sizes = raw.value("sizes").toObject();
    for (auto it = sizes.begin(); it != sizes.end(); ++it) {
        const QJsonObject size = it.value().toObject();
        const auto w = Int(size.value("w"));
        const auto h = Int(size.value("h"));
        if (w && h) {
            layout.sizes[it.key().toStdString()] = {*w, *h};
        }
    }
    return Sanitize(layout, known);
}

DockLayout LoadLayout(const QSettings& settings, const std::vector<ModuleId>& known) {
    const QByteArray bytes = settings.value(kLayoutSettingsKey).toByteArray();
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        return Sanitize(DefaultLayout(), known);
    }
    return LayoutFromJson(document.object(), known);
}

void SaveLayout(QSettings& settings, const DockLayout& layout) {
    settings.setValue(kLayoutSettingsKey, QJsonDocument(LayoutToJson(layout)).toJson(QJsonDocument::Compact));
}

} // namespace War3Map::Shell
