#pragma once

#include "War3MapCore/Services/MapDataQuery.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>

#include <bit>
#include <cmath>
#include <type_traits>

// 已提交来源查询和当前实体查询只保留这一份无损行/字段 JSON 格式。
namespace War3Map::Mcp::MapDataJson {
using namespace Services;

inline QString MapKindName(MapDataKind kind) {
    return QString::fromLatin1(MapData::MapDataKindId(kind));
}

inline QString StateName(MapDataQueryState state) {
    switch (state) {
    case MapDataQueryState::Missing: return "missing";
    case MapDataQueryState::Ready: return "ready";
    case MapDataQueryState::Partial: return "partial";
    case MapDataQueryState::Invalid: return "invalid";
    case MapDataQueryState::Ambiguous: return "ambiguous";
    case MapDataQueryState::Unreadable: return "unreadable";
    case MapDataQueryState::OpaqueUnknown: return "opaque-unknown";
    }
    return "invalid";
}

inline QString TextStateName(MapDataQueryTextState state) {
    switch (state) {
    case MapDataQueryTextState::Literal: return "literal";
    case MapDataQueryTextState::Resolved: return "resolved";
    case MapDataQueryTextState::ReferenceInvalid: return "reference-invalid";
    case MapDataQueryTextState::ReferenceMissing: return "reference-missing";
    case MapDataQueryTextState::ReferenceAmbiguous: return "reference-ambiguous";
    case MapDataQueryTextState::StringTableUnavailable: return "string-table-unavailable";
    case MapDataQueryTextState::CatalogUnavailable: return "catalog-unavailable";
    }
    return "invalid";
}

inline QString Hex(std::string_view bytes) {
    if (bytes.empty()) return QStringLiteral("");
    return QString::fromLatin1(QByteArray(bytes.data(), static_cast<qsizetype>(bytes.size())).toHex());
}

inline QJsonValue Utf8OrNull(std::string_view bytes) {
    if (bytes.empty()) return QStringLiteral("");
    const QByteArray original(bytes.data(), static_cast<qsizetype>(bytes.size()));
    const QString converted = QString::fromUtf8(original.constData(), original.size());
    if (converted.toUtf8() != original) return QJsonValue(QJsonValue::Null);
    return converted;
}

inline QJsonValue TextBytesOrNull(const std::optional<std::string>& bytes, bool utf8) {
    if (!bytes) return QJsonValue(QJsonValue::Null);
    return utf8 ? Utf8OrNull(*bytes) : QJsonValue(Hex(*bytes));
}

inline QJsonObject BindingJson(const MapDataQueryBinding& binding) {
    return {{"document_id", QString::fromStdString(binding.documentId)},
        {"document_generation", QString::number(binding.generation)},
        {"revision", QString::number(binding.revision)},
        {"source_sha256", QString::fromStdString(binding.sourceSha256)}};
}

inline QJsonObject MemberJson(const MapDataQueryMember& member) {
    return {{"kind", MapKindName(member.kind)},
        {"member_name", QString::fromStdString(member.memberName)},
        {"state", StateName(member.state)},
        {"record_count", member.recordCount ? QJsonValue(QString::number(*member.recordCount)) : QJsonValue(QJsonValue::Null)},
        {"member_sha256", member.memberSha256 ? QJsonValue(QString::fromStdString(*member.memberSha256)) : QJsonValue(QJsonValue::Null)},
        {"snapshot_token", member.memberSha256 ? QString::fromStdString(*member.memberSha256) : QStringLiteral("missing")},
        {"diagnostic_count", QString::number(member.diagnosticCount)}};
}

inline QJsonObject TextJson(const MapDataQueryText& value) {
    return {{"raw_hex", Hex(value.rawBytes)}, {"raw_utf8", Utf8OrNull(value.rawBytes)},
        {"display_hex", TextBytesOrNull(value.displayBytes, false)},
        {"display_utf8", TextBytesOrNull(value.displayBytes, true)},
        {"state", TextStateName(value.state)}};
}

template <typename Number, std::size_t Count>
inline QJsonValue NumberArray(const std::optional<std::array<Number, Count>>& values) {
    if (!values) return QJsonValue(QJsonValue::Null);
    QJsonArray result;
    for (Number value : *values) {
        if constexpr (std::is_floating_point_v<Number>) {
            if (!std::isfinite(value)) return QJsonValue(QJsonValue::Null);
        }
        result.append(static_cast<double>(value));
    }
    return result;
}

// raw 是写入请求须回传的精确前值；value 是显示值（角度为度，字节为 UTF-8 或 null）。
inline QJsonObject FieldJson(const MapDataQueryField& field) {
    const bool isDefault = MapData::IsMapFieldDefault(*field.descriptor, field.value);
    QJsonValue value(QJsonValue::Null);
    if (!isDefault) {
        if (field.value.type == MapData::MapFieldType::Bytes) value = Utf8OrNull(field.value.bytes);
        else if (const auto number = MapData::MapFieldDisplayNumber(*field.descriptor, field.value)) value = *number;
    }
    return {{"id", QString::fromLatin1(field.descriptor->id)},
        {"type", QString::fromLatin1(MapData::MapFieldTypeId(field.value.type))},
        {"writable", field.writable},
        {"raw", QString::fromStdString(MapData::FormatMapFieldRaw(field.value))},
        {"is_default", isDefault}, {"value", value}};
}

inline QJsonArray FieldCatalogJson() {
    QJsonArray catalog;
    for (const auto& field : MapData::MapFieldCatalog()) {
        catalog.append(QJsonObject{{"kind", MapKindName(field.kind)}, {"id", QString::fromLatin1(field.id)},
            {"label", QString::fromUtf8(field.label.data(), static_cast<qsizetype>(field.label.size()))},
            {"type", QString::fromLatin1(MapData::MapFieldTypeId(field.type))}, {"writable", field.writable},
            {"constraint", QString::fromUtf8(field.constraint.data(), static_cast<qsizetype>(field.constraint.size()))}});
    }
    return catalog;
}

inline QJsonObject RowJson(const MapDataQueryRow& row) {
    QJsonArray fields;
    for (const auto& field : row.fields) fields.append(FieldJson(field));
    QJsonValue positionRaw(QJsonValue::Null);
    if (row.positionWorld) {
        QJsonArray raw;
        for (float value : *row.positionWorld) raw.append(static_cast<double>(std::bit_cast<std::uint32_t>(value)));
        positionRaw = raw;
    }
    QString form = "record";
    if (row.form == MapDataQueryRowForm::NormalDoodad) form = "normal-doodad";
    else if (row.form == MapDataQueryRowForm::SpecialDoodad) form = "special-doodad";
    return {{"source_index", QString::number(row.sourceIndex)}, {"locator_scope", "snapshot-revision"},
        {"form", form}, {"rawcode_hex", Hex(row.rawcodeBytes)}, {"rawcode_utf8", Utf8OrNull(row.rawcodeBytes)},
        {"creation_number", row.creationNumber ? QJsonValue(*row.creationNumber) : QJsonValue(QJsonValue::Null)},
        {"string_id", row.stringId ? QJsonValue(QString::number(*row.stringId)) : QJsonValue(QJsonValue::Null)},
        {"name", TextJson(row.name)},
        {"owner", row.owner ? QJsonValue(*row.owner) : QJsonValue(QJsonValue::Null)},
        {"position_world", NumberArray(row.positionWorld)}, {"position_world_raw_u32", positionRaw},
        {"grid_position", NumberArray(row.gridPosition)},
        {"resource_path_hex", TextBytesOrNull(row.resourcePathBytes, false)},
        {"resource_path_utf8", TextBytesOrNull(row.resourcePathBytes, true)},
        {"row_key", QString::fromStdString(row.rowKey)}, {"fields", fields}};
}

} // namespace War3Map::Mcp::MapDataJson
