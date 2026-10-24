#pragma once

#include "War3MapCore/GameUi/UiDescription.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

// 界面编写共用：描述与编辑在界面层用 QJson 组装，再交给 UiDescriptionEdit 按文件规则检查。
namespace War3Map::Modules {

[[nodiscard]] inline QString CompactEdits(const QJsonArray& edits) {
    return QString::fromUtf8(QJsonDocument(edits).toJson(QJsonDocument::Compact));
}

// 描述的规范 JSON（取出 values/states/events 整体改写用）。
[[nodiscard]] inline QJsonObject DescriptionObject(const GameUi::UiDescription& description) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(GameUi::SerializeUiDescription(description))).object();
}

// 单个框架（不含子框架）的规范 JSON。
[[nodiscard]] inline QJsonObject FrameObject(const GameUi::UiFrame& frame) {
    return QJsonDocument::fromJson(QByteArray::fromStdString(GameUi::SerializeUiFrame(frame))).object();
}

[[nodiscard]] inline QJsonArray AnchorsOf(const GameUi::UiFrame& frame) {
    return FrameObject(frame).value(QStringLiteral("anchors")).toArray();
}

} // namespace War3Map::Modules
