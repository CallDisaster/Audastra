#pragma once

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Services/WorldSelection.h"

#include <QJsonDocument>
#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <type_traits>

namespace War3Map::Modules::WorldNavigation {

// 历史只持有纯视图值；源行号仅为完整 typed 身份的一个字段，不能单独定位。
inline constexpr qsizetype MaximumBookmarkBytes = 512 * 1024;
inline constexpr qsizetype MaximumText = 2048;

[[nodiscard]] inline bool Bounded(const QJsonObject& state) {
    return QJsonDocument(state).toJson(QJsonDocument::Compact).size() <= MaximumBookmarkBytes;
}

[[nodiscard]] inline std::optional<std::int64_t> Integer(const QJsonValue& value,
    std::int64_t minimum = 0, std::int64_t maximum = std::numeric_limits<int>::max()) {
    if (!value.isDouble()) return std::nullopt;
    const auto number = value.toDouble();
    if (!std::isfinite(number) || std::trunc(number) != number
        || number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) return std::nullopt;
    return static_cast<std::int64_t>(number);
}

[[nodiscard]] inline QString Bytes(const std::string& bytes) {
    return QString::fromLatin1(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())).toHex());
}

[[nodiscard]] inline std::optional<std::string> Bytes(const QJsonValue& value, qsizetype maximumBytes) {
    if (!value.isString()) return std::nullopt;
    const auto text = value.toString();
    if (text.size() > maximumBytes * 2 || text.size() % 2 != 0
        || std::any_of(text.begin(), text.end(), [](QChar c) {
            return !((c >= QChar(u'0') && c <= QChar(u'9')) || (c >= QChar(u'a') && c <= QChar(u'f')));
        })) return std::nullopt;
    const auto decoded = QByteArray::fromHex(text.toLatin1());
    return std::string(decoded.constData(), static_cast<std::size_t>(decoded.size()));
}

[[nodiscard]] inline QJsonObject Encode(const Services::WorldObjectId& id) {
    return std::visit([](const auto& value) -> QJsonObject {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, MapData::PlacementId>) {
            return {{"kind", "placement"}, {"file", static_cast<int>(value.file)},
                {"index", static_cast<double>(value.index)}, {"rawcodeBytes", Bytes(value.rawcode)}, {"creation", value.creationId}};
        } else if constexpr (std::is_same_v<T, MapData::RegionId>) {
            return {{"kind", "region"}, {"index", static_cast<double>(value.index)},
                {"nameBytes", Bytes(value.name)}, {"creation", value.creationNumber}};
        } else {
            return {{"kind", "camera"}, {"index", static_cast<double>(value.index)}, {"nameBytes", Bytes(value.name)}};
        }
    }, id);
}

[[nodiscard]] inline std::optional<Services::WorldObjectId> Decode(const QJsonValue& value) {
    if (!value.isObject()) return std::nullopt;
    const auto object = value.toObject();
    const auto kind = object.value("kind");
    const auto index = Integer(object.value("index"), 0, std::numeric_limits<std::uint32_t>::max());
    if (!kind.isString() || !index) return std::nullopt;
    if (kind.toString() == QLatin1String("placement")) {
        const auto file = Integer(object.value("file"), 0, 1);
        const auto rawcode = Bytes(object.value("rawcodeBytes"), 4);
        const auto creation = Integer(object.value("creation"), std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
        if (!file || !rawcode || rawcode->size() != 4 || !creation) return std::nullopt;
        return MapData::PlacementId{static_cast<MapData::PlacementFile>(*file), static_cast<std::uint32_t>(*index),
            *rawcode, static_cast<std::int32_t>(*creation)};
    }
    const auto name = Bytes(object.value("nameBytes"), MaximumText);
    if (!name) return std::nullopt;
    if (kind.toString() == QLatin1String("region")) {
        const auto creation = Integer(object.value("creation"), std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max());
        if (!creation) return std::nullopt;
        return MapData::RegionId{static_cast<std::uint32_t>(*index), *name, static_cast<std::int32_t>(*creation)};
    }
    if (kind.toString() == QLatin1String("camera")) return MapData::CameraId{static_cast<std::uint32_t>(*index), *name};
    return std::nullopt;
}

[[nodiscard]] inline std::optional<Services::WorldObjectId> At(const MapDocument& document, int tab, std::uint32_t index) {
    if (tab == 0 && document.Units() && index < document.Units()->units.size())
        return MapData::MakePlacementId(MapData::PlacementFile::Units, index, document.Units()->units[index]);
    if (tab == 1 && document.Doodads() && index < document.Doodads()->doodads.size())
        return MapData::MakePlacementId(MapData::PlacementFile::Doodads, index, document.Doodads()->doodads[index]);
    if (tab == 2 && document.Regions() && index < document.Regions()->entries.size())
        return MapData::MakeRegionId(index, document.Regions()->entries[index]);
    if (tab == 3 && document.Cameras() && index < document.Cameras()->entries.size())
        return MapData::MakeCameraId(index, document.Cameras()->entries[index]);
    return std::nullopt;
}

[[nodiscard]] inline int Tab(const Services::WorldObjectId& id) {
    if (const auto* placement = std::get_if<MapData::PlacementId>(&id)) return static_cast<int>(placement->file);
    return std::holds_alternative<MapData::RegionId>(id) ? 2 : 3;
}

[[nodiscard]] inline std::uint32_t Index(const Services::WorldObjectId& id) {
    return std::visit([](const auto& value) { return value.index; }, id);
}

[[nodiscard]] inline bool Contains(const MapDocument& document, const Services::WorldObjectId& id) {
    const auto current = At(document, Tab(id), Index(id));
    return current && *current == id;
}

} // namespace War3Map::Modules::WorldNavigation
