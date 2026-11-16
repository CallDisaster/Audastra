#include "GameUiModule.h"

#include "GameUiAuthoringJson.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"

#include <QJsonArray>
#include <QJsonObject>

#include <cmath>

// 自己控件的布局修改：换父控件、铺满父控件，以及属性里长度的单位（魔兽界面单位或参考分辨率下的像素）。
// 界面与助手工具共用这些操作，修改都经 UiDescriptionEdit 检查后放进待保存修改。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

bool ParentAnchor(const UiAnchor& anchor, FramePoint point) {
    return anchor.point == point && anchor.target == UiAnchorTarget::Parent && anchor.targetPoint == point && anchor.x == 0.0 && anchor.y == 0.0;
}

} // namespace

std::optional<FrameRect> GameUiModule::DescriptionFrameRect(const QString& description, const QString& frame) const {
    if (!tree_) return std::nullopt;
    const std::string descriptionId = description.toStdString();
    const std::string frameId = frame.toStdString();
    for (const FrameNode& node : tree_->Nodes()) {
        if (!node.uiFrame || node.uiFrame->description != descriptionId || node.uiFrame->frame != frameId) continue;
        if (node.layout != LayoutState::Placed && node.layout != LayoutState::SizeFromContent) return std::nullopt;
        return node.rect;  // 节点按实例顺序排列，第一个即第一个实例
    }
    return std::nullopt;
}

bool GameUiModule::ReparentDescriptionFrame(const QString& descriptionId, const QString& frameId, const QString& parentId, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const UiDescription* description = Description(descriptionId);
    if (!description) return fail(tr("地图里没有界面 %1").arg(descriptionId));
    const UiFrame* frame = FindUiFrame(*description, frameId.toStdString());
    if (!frame) return fail(tr("界面 %1 里没有控件 %2").arg(descriptionId, frameId));
    if (!parentId.isEmpty() && !FindUiFrame(*description, parentId.toStdString()))
        return fail(tr("界面 %1 里没有控件 %2").arg(descriptionId, parentId));
    const QString oldParent = Text(UiFrameParentId(*description, frameId.toStdString()));
    if (oldParent == parentId) return true;

    QJsonObject move{{"op", "move_frame"}, {"frame", frameId}};
    if (!parentId.isEmpty()) move.insert("parent", parentId);
    QJsonArray edits{move};
    // 以父控件为目标的锚点：偏移加上旧父控件对应点与新父控件对应点之差。
    const auto from = DescriptionFrameRect(descriptionId, oldParent);
    const auto to = DescriptionFrameRect(descriptionId, parentId);
    if (from && to) {
        QJsonArray anchors = AnchorsOf(*frame);
        bool changed = false;
        for (std::size_t i = 0; i < frame->anchors.size() && i < static_cast<std::size_t>(anchors.size()); ++i) {
            const UiAnchor& anchor = frame->anchors[i];
            if (anchor.target != UiAnchorTarget::Parent) continue;
            const double dx = FramePointX(*from, anchor.targetPoint) - FramePointX(*to, anchor.targetPoint);
            const double dy = FramePointY(*from, anchor.targetPoint) - FramePointY(*to, anchor.targetPoint);
            if (dx == 0.0 && dy == 0.0) continue;
            QJsonObject object = anchors[static_cast<qsizetype>(i)].toObject();
            object.insert("x", RoundUiUnits(anchor.x + dx));
            object.insert("y", RoundUiUnits(anchor.y + dy));
            anchors[static_cast<qsizetype>(i)] = object;
            changed = true;
        }
        if (changed) edits.append(QJsonObject{{"op", "set_frame"}, {"frame", frameId}, {"fields", QJsonObject{{"anchors", anchors}}}});
    }
    const QString where = parentId.isEmpty() ? tr("界面顶层") : parentId;
    return EditDescription(descriptionId, CompactEdits(edits), tr("界面 %1：%2 移到 %3 下").arg(descriptionId, frameId, where), error);
}

bool GameUiModule::FillsParent(const UiFrame& frame) {
    return frame.anchors.size() == 2 && ParentAnchor(frame.anchors[0], FramePoint::TopLeft)
        && ParentAnchor(frame.anchors[1], FramePoint::BottomRight);
}

bool GameUiModule::SetDescriptionFrameFill(const QString& descriptionId, const QString& frameId, bool fill, QString* error) {
    const UiDescription* description = Description(descriptionId);
    const UiFrame* frame = description ? FindUiFrame(*description, frameId.toStdString()) : nullptr;
    if (!frame) {
        if (error) *error = tr("界面 %1 里没有控件 %2").arg(descriptionId, frameId);
        return false;
    }
    if (FillsParent(*frame) == fill) return true;
    QJsonObject fields;
    if (fill) {
        fields.insert("anchors", QJsonArray{QJsonObject{{"point", "TOPLEFT"}}, QJsonObject{{"point", "BOTTOMRIGHT"}}});
        fields.insert("width", QJsonValue::Null);
        fields.insert("height", QJsonValue::Null);
    } else {
        // 居中于父控件，宽高取当前大小：画面上不动。
        fields.insert("anchors", QJsonArray{QJsonObject{{"point", "CENTER"}}});
        if (const auto rect = DescriptionFrameRect(descriptionId, frameId)) {
            fields.insert("width", RoundUiUnits(rect->Width()));
            fields.insert("height", RoundUiUnits(rect->Height()));
        }
    }
    const QJsonObject edit{{"op", "set_frame"}, {"frame", frameId}, {"fields", fields}};
    return EditDescription(descriptionId, CompactEdits({edit}),
        (fill ? tr("界面 %1：%2 铺满父控件") : tr("界面 %1：%2 不再铺满父控件")).arg(descriptionId, frameId), error);
}

void GameUiModule::SetPixelUnits(bool pixels) {
    if (pixelUnits_ == pixels) return;
    pixelUnits_ = pixels;
    UpdateProperties();
}

void GameUiModule::SetReferenceResolution(UiResolution resolution) {
    if (!resolution.Valid() || (resolution.width == resolution_.width && resolution.height == resolution_.height)) return;
    resolution_ = resolution;
    UpdateProperties();
}

QString GameUiModule::LengthText(double units, bool horizontal) const {
    if (!pixelUnits_) return QString::number(units);
    const double pixels = horizontal ? UiPixelsFromUnitsX(units, resolution_) : UiPixelsFromUnitsY(units, resolution_);
    return QString::number(std::round(pixels * 100.0) / 100.0);
}

std::optional<double> GameUiModule::ParseLength(const QString& input, bool horizontal) const {
    QString text = input.trimmed();
    bool pixels = pixelUnits_;
    if (text.endsWith(QLatin1String("px"), Qt::CaseInsensitive)) {
        pixels = true;
        text.chop(2);
        text = text.trimmed();
    }
    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok || !std::isfinite(value)) return std::nullopt;
    if (!pixels) return value;
    return RoundUiUnits(horizontal ? UiUnitsFromPixelsX(value, resolution_) : UiUnitsFromPixelsY(value, resolution_));
}

} // namespace War3Map::Modules
