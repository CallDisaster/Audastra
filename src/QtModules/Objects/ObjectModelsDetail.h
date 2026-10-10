#pragma once
#include "Commands/ObjectSnapshotCommand.h"

#include "ObjectModels.h"

#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QStringList>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QScreen>
#include <QShortcut>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>


// 物体列表 / 字段模型 / 委托共用的转换工具（仅模块内部使用）。
namespace ObjectModelsDetail {

using namespace War3Map::ObjectData;

inline QString ToQString(const std::string& value) {
    return QString::fromUtf8(value);
}

inline QString IndexText(const ModificationKey& key) {
    QStringList parts;
    if (key.level) {
        parts.append(QObject::tr("等级 %1").arg(*key.level));
    }
    if (key.dataIndex) {
        parts.append(QObject::tr("数据 %1").arg(*key.dataIndex));
    }
    if (key.variation) {
        parts.append(QObject::tr("变化 %1").arg(*key.variation));
    }
    return parts.isEmpty() ? QStringLiteral("—") : parts.join(QStringLiteral(" · "));
}

inline std::optional<Value> ConvertEditedValue(const Value& current, const QVariant& input) {
    const QString text = input.toString();
    return std::visit(
        [&text](const auto& value) -> std::optional<Value> {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::int32_t>) {
                bool valid = false;
                const qlonglong parsed = text.toLongLong(&valid);
                if (!valid || parsed < (std::numeric_limits<std::int32_t>::min)() ||
                    parsed > (std::numeric_limits<std::int32_t>::max)()) {
                    return std::nullopt;
                }
                return Value{static_cast<std::int32_t>(parsed)};
            } else if constexpr (std::is_same_v<T, float>) {
                bool valid = false;
                const float parsed = text.toFloat(&valid);
                return valid && std::isfinite(parsed)
                    ? std::optional<Value>{Value{parsed}}
                    : std::nullopt;
            } else if constexpr (std::is_same_v<T, bool>) {
                const QString lower = text.trimmed().toLower();
                if (lower == QStringLiteral("true") || lower == QStringLiteral("1") ||
                    lower == QStringLiteral("是")) {
                    return Value{true};
                }
                if (lower == QStringLiteral("false") || lower == QStringLiteral("0") ||
                    lower == QStringLiteral("否")) {
                    return Value{false};
                }
                return std::nullopt;
            } else if constexpr (std::is_same_v<T, std::string>) {
                return Value{text.toUtf8().toStdString()};
            } else if constexpr (std::is_same_v<T, RawcodeValue>) {
                if (std::any_of(text.begin(), text.end(), [](QChar c) { return c.unicode() < 0x20 || c.unicode() > 0x7e; })) return std::nullopt;
                const std::string rawcode = text.toLatin1().toStdString();
                return rawcode.size() == 4
                    ? std::optional<Value>{Value{RawcodeValue{rawcode}}}
                    : std::nullopt;
            } else if constexpr (std::is_same_v<T, EnumValue>) {
                return Value{EnumValue{text.toUtf8().toStdString()}};
            } else if constexpr (std::is_same_v<T, AssetPathValue>) {
                return Value{AssetPathValue{text.toUtf8().toStdString()}};
            } else if constexpr (std::is_same_v<T, LocalizedStringValue>) {
                return Value{LocalizedStringValue{text.toUtf8().toStdString()}};
            } else {
                // 列表与未知二进制值需要专用编辑器，当前保持只读。
                return std::nullopt;
            }
        },
        current);
}

} // namespace ObjectModelsDetail

// 字段模型与委托共用：多值拆分、引用类型与撤销快照。
namespace ObjectModelsDetail {
inline QStringList SplitComponents(const QString& source) {
    QStringList result;
    QString token;
    bool quoted = false;
    for (qsizetype i = 0; i < source.size(); ++i) {
        const auto c = source.at(i);
        if (c == QChar('"')) {
            if (quoted && i + 1 < source.size() && source.at(i + 1) == QChar('"')) {
                token += c;
                ++i;
            } else quoted = !quoted;
        } else if (c == QChar(',') && !quoted) {
            result.append(token);
            token.clear();
        } else token += c;
    }
    if (quoted) return {};
    result.append(token);
    return result;
} // namespace ObjectModelsDetail

inline bool IsBoolean(const ObjectFieldDefinition* field) {
    return field && (field->valueType == "bool" || field->valueType == "boolean");
} // namespace ObjectModelsDetail

// 原版 metadata 的扩展提示/说明字段使用多行编辑；其他字符串仍保持单行。
// 已有换行的可编辑字符串也使用同一控件，避免单行输入吞掉正文。
inline bool IsMultilineTextField(const ObjectFieldDefinition* field, const std::optional<Value>& value) {
    if (!field || field->nativeValueType != NativeValueType::String || field->isList ||
        !field->referenceTypes.empty() || !field->choices.empty()) return false;
    std::string source = field->sourceField;
    std::transform(source.begin(), source.end(), source.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (source == "ubertip" || source == "researchubertip" || source == "description" ||
        source == "extendedtooltip") return true;
    return value && std::holds_alternative<std::string>(*value) &&
        std::get<std::string>(*value).find_first_of("\r\n") != std::string::npos;
} // namespace ObjectModelsDetail

inline bool ValidRawcode(const QString& value) {
    if (value.size() != 4) return false;
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character.unicode() > 0x7e) return false;
    }
    return true;
} // namespace ObjectModelsDetail

inline std::optional<Value> SchemaValue(const ObjectFieldDefinition* field, const QVariant& input) {
    if (!field || !field->nativeValueType) return std::nullopt;
    const QString text = input.toString();
    if (IsBoolean(field)) {
        auto value = ConvertEditedValue(Value{false}, input);
        if (!value) return std::nullopt;
        return Value{std::int32_t{std::get<bool>(*value) ? 1 : 0}};
    }
    switch (*field->nativeValueType) {
    case NativeValueType::Int32: return ConvertEditedValue(Value{std::int32_t{0}}, input);
    case NativeValueType::Real:
    case NativeValueType::Unreal: return ConvertEditedValue(Value{0.0F}, input);
    case NativeValueType::String:
        return Value{text.toUtf8().toStdString()};
    }
    return std::nullopt;
} // namespace ObjectModelsDetail

inline QString ReferenceKind(ObjectType type) {
    switch (type) {
    case ObjectType::Unit: return QStringLiteral("unit");
    case ObjectType::Ability: return QStringLiteral("ability");
    case ObjectType::Item: return QStringLiteral("item");
    case ObjectType::Buff: return QStringLiteral("buff");
    case ObjectType::Upgrade: return QStringLiteral("upgrade");
    case ObjectType::Doodad: return QStringLiteral("doodad");
    case ObjectType::Destructable: return QStringLiteral("destructable");
    }
    return {};
} // namespace ObjectModelsDetail

inline QStringList ReferenceIds(const Value& value) {
    if (const auto* ids = std::get_if<RawcodeListValue>(&value)) {
        QStringList result;
        for (const auto& id : ids->values) result.append(ToQString(id));
        return result;
    }
    const QString raw = ToQString(ValueToDisplayString(value));
    if (raw.isEmpty() || raw == QStringLiteral("_")) return {};
    return raw.split(QChar(','), Qt::KeepEmptyParts);
} // namespace ObjectModelsDetail

using ObjectSnapshotCommand = War3Map::QtProject::ObjectSnapshotCommand;
} // namespace ObjectModelsDetail
