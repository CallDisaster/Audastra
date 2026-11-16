#include "GameUiModule.h"

#include "GameUiAuthoringJson.h"
#include "GameUiCanvas.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"

#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

// 自己控件的布局修改：换父控件、铺满父控件、按矩形摆放（改大小、对齐），以及长度的单位（魔兽界面单位或参考分辨率下的像素）。
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
    return PlaceDescriptionFrame(descriptionId, frameId, parentId, std::nullopt, error);
}

bool GameUiModule::PlaceDescriptionFrame(const QString& descriptionId, const QString& frameId, const QString& parentId,
    std::optional<std::size_t> index, QString* error) {
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
    if (oldParent == parentId) {
        // 同一父控件下只调整先后（后面的画在上面）。
        if (!index) return true;
        const auto position = UiFrameSiblingIndex(*description, frameId.toStdString());
        if (!position) return fail(tr("界面 %1 里没有控件 %2").arg(descriptionId, frameId));
        const std::size_t target = std::min(*index, position->second - 1);
        if (target == position->first) return true;
        QJsonObject move{{"op", "move_frame"}, {"frame", frameId}, {"index", static_cast<qint64>(target)}};
        if (!parentId.isEmpty()) move.insert("parent", parentId);
        return EditDescription(descriptionId, CompactEdits({move}), tr("界面 %1：调整 %2 的先后").arg(descriptionId, frameId), error);
    }

    QJsonObject move{{"op", "move_frame"}, {"frame", frameId}};
    if (!parentId.isEmpty()) move.insert("parent", parentId);
    if (index) {
        const UiFrame* parent = parentId.isEmpty() ? nullptr : FindUiFrame(*description, parentId.toStdString());
        const std::size_t count = parent ? parent->children.size() : description->frames.size();
        move.insert("index", static_cast<qint64>(std::min(*index, count)));
    }
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

bool GameUiModule::RenameDescriptionNode(const QString& descriptionId, const QString& frameId, const QString& label, QString* error) {
    const UiDescription* description = Description(descriptionId);
    if (!description) {
        if (error) *error = tr("地图里没有界面 %1").arg(descriptionId);
        return false;
    }
    const QString name = label.trimmed();
    if (frameId.isEmpty()) {
        if (Text(description->label) == name) return true;
        return EditDescription(descriptionId, CompactEdits({QJsonObject{{"op", "set_description"}, {"label", name}}}),
            tr("界面 %1：改名为 %2").arg(descriptionId, name), error);
    }
    const UiFrame* frame = FindUiFrame(*description, frameId.toStdString());
    if (!frame) {
        if (error) *error = tr("界面 %1 里没有控件 %2").arg(descriptionId, frameId);
        return false;
    }
    if (Text(frame->label) == name) return true;
    const QJsonObject edit{{"op", "set_frame"}, {"frame", frameId}, {"fields", QJsonObject{{"label", name}}}};
    return EditDescription(descriptionId, CompactEdits({edit}), tr("界面 %1：%2 改名为 %3").arg(descriptionId, frameId, name), error);
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

bool GameUiModule::SetDescriptionFrameRects(const QString& descriptionId, const QList<std::pair<QString, FrameRect>>& rects,
    const QString& undoLabel, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const UiDescription* description = Description(descriptionId);
    if (!description) return fail(tr("地图里没有界面 %1").arg(descriptionId));
    struct Change {
        const UiFrame* frame = nullptr;
        std::optional<FrameRect> from;
        FrameRect to;
    };
    std::map<std::string, Change, std::less<>> changes;
    for (const auto& [frameId, rect] : rects) {
        const UiFrame* frame = FindUiFrame(*description, frameId.toStdString());
        if (!frame) return fail(tr("界面 %1 里没有控件 %2").arg(descriptionId, frameId));
        const bool finite = std::isfinite(rect.left) && std::isfinite(rect.bottom) && std::isfinite(rect.right) && std::isfinite(rect.top);
        if (!finite || !(rect.Width() > 0.0f) || !(rect.Height() > 0.0f)) return fail(tr("控件 %1 的宽高必须大于 0").arg(frameId));
        Change change{frame, DescriptionFrameRect(descriptionId, frameId), rect};
        if (!change.from && !frame->anchors.empty())
            return fail(tr("控件 %1 在画面上还没排出位置（锚点目标未定位），不能按矩形摆放").arg(frameId));
        changes.insert_or_assign(frame->id, change);
    }
    // 目标控件在这一批里时它对应点的移动（目标随后也会移动，偏移要扣掉，否则移动两次）。
    const auto targetMove = [&](const std::string& target, FramePoint point) -> std::pair<double, double> {
        const auto found = target.empty() ? changes.end() : changes.find(target);
        if (found == changes.end() || !found->second.from) return {0.0, 0.0};
        const Change& change = found->second;
        return {FramePointX(change.to, point) - FramePointX(*change.from, point), FramePointY(change.to, point) - FramePointY(*change.from, point)};
    };
    const FrameRect unit{0.0f, 0.0f, 1.0f, 1.0f};
    QJsonArray edits;
    for (const auto& [id, change] : changes) {
        const UiFrame& frame = *change.frame;
        const FrameRect& to = change.to;
        QJsonObject fields;
        if (frame.anchors.empty()) {
            fields.insert("anchors", QJsonArray{QJsonObject{{"point", "BOTTOMLEFT"}, {"to", "screen"}, {"x", RoundUiUnits(to.left)},
                {"y", RoundUiUnits(to.bottom)}}});
            fields.insert("width", RoundUiUnits(to.Width()));
            fields.insert("height", RoundUiUnits(to.Height()));
        } else {
            const FrameRect& from = *change.from;
            QJsonArray anchors = AnchorsOf(frame);
            std::set<float> columns;
            std::set<float> rows;
            for (std::size_t i = 0; i < frame.anchors.size() && i < static_cast<std::size_t>(anchors.size()); ++i) {
                const UiAnchor& anchor = frame.anchors[i];
                columns.insert(FramePointX(unit, anchor.point));
                rows.insert(FramePointY(unit, anchor.point));
                const std::string target = anchor.target == UiAnchorTarget::Parent ? UiFrameParentId(*description, frame.id)
                    : anchor.target == UiAnchorTarget::Frame                       ? anchor.targetId
                                                                                   : std::string();
                const auto [targetX, targetY] = targetMove(target, anchor.targetPoint);
                const double dx = FramePointX(to, anchor.point) - FramePointX(from, anchor.point) - targetX;
                const double dy = FramePointY(to, anchor.point) - FramePointY(from, anchor.point) - targetY;
                QJsonObject object = anchors[static_cast<qsizetype>(i)].toObject();
                object.insert("x", RoundUiUnits(anchor.x + dx));
                object.insert("y", RoundUiUnits(anchor.y + dy));
                anchors[static_cast<qsizetype>(i)] = object;
            }
            fields.insert("anchors", anchors);
            // 锚点在某个方向上只定住一处（一个点或同一列/行）时，那个方向的长度由宽高决定。
            if (columns.size() < 2 && std::abs(to.Width() - from.Width()) > 1e-6f) fields.insert("width", RoundUiUnits(to.Width()));
            if (rows.size() < 2 && std::abs(to.Height() - from.Height()) > 1e-6f) fields.insert("height", RoundUiUnits(to.Height()));
        }
        edits.append(QJsonObject{{"op", "set_frame"}, {"frame", Text(id)}, {"fields", fields}});
    }
    if (edits.isEmpty()) return true;
    return EditDescription(descriptionId, CompactEdits(edits), undoLabel, error);
}

bool GameUiModule::ArrangeDescriptionFrames(const QString& descriptionId, const QStringList& frames, UiArrangeMode mode, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const UiDescription* description = Description(descriptionId);
    if (!description) return fail(tr("地图里没有界面 %1").arg(descriptionId));
    QStringList unique;
    for (const QString& frame : frames) {
        if (!unique.contains(frame)) unique.push_back(frame);
    }
    if (static_cast<std::size_t>(unique.size()) < UiArrangeMinimum(mode))
        return fail(tr("至少要选 %1 个控件").arg(UiArrangeMinimum(mode)));
    std::vector<FrameRect> rects;
    for (const QString& frame : unique) {
        if (!FindUiFrame(*description, frame.toStdString())) return fail(tr("界面 %1 里没有控件 %2").arg(descriptionId, frame));
        const auto rect = DescriptionFrameRect(descriptionId, frame);
        if (!rect) return fail(tr("控件 %1 在画面上还没排出位置").arg(frame));
        rects.push_back(*rect);
    }
    // 只有一个控件时对齐父控件（界面顶层的控件对齐整个画面）。
    std::optional<FrameRect> reference;
    if (unique.size() == 1) {
        reference = DescriptionFrameRect(descriptionId, Text(UiFrameParentId(*description, unique.front().toStdString())));
        if (!reference) reference = FrameRect{0.0f, 0.0f, static_cast<float>(kUiScreenWidth), static_cast<float>(kUiScreenHeight)};
    }
    const auto arranged = ArrangeUiRects(rects, mode, reference);
    QList<std::pair<QString, FrameRect>> targets;
    for (qsizetype i = 0; i < unique.size(); ++i) targets.push_back({unique[i], arranged[static_cast<std::size_t>(i)]});
    return SetDescriptionFrameRects(descriptionId, targets,
        tr("界面 %1：%2 个控件%3").arg(descriptionId).arg(unique.size()).arg(ArrangeLabel(mode)), error);
}

QString GameUiModule::ArrangeLabel(UiArrangeMode mode) {
    switch (mode) {
    case UiArrangeMode::AlignLeft: return tr("左对齐");
    case UiArrangeMode::AlignCenterX: return tr("水平居中");
    case UiArrangeMode::AlignRight: return tr("右对齐");
    case UiArrangeMode::AlignTop: return tr("顶对齐");
    case UiArrangeMode::AlignCenterY: return tr("垂直居中");
    case UiArrangeMode::AlignBottom: return tr("底对齐");
    case UiArrangeMode::DistributeX: return tr("横向等间距");
    case UiArrangeMode::DistributeY: return tr("纵向等间距");
    case UiArrangeMode::SameWidth: return tr("同宽");
    case UiArrangeMode::SameHeight: return tr("同高");
    }
    return {};
}

std::vector<std::uint32_t> GameUiModule::ExtraSelectedNodes() const {
    std::vector<std::uint32_t> nodes;
    if (!tree_ || extraKeys_.isEmpty()) return nodes;
    for (const auto& node : tree_->Nodes()) {
        if (node.id != selected_ && node.id != kScreenNode && extraKeys_.contains(NodeKey(node.id))) nodes.push_back(node.id);
    }
    return nodes;
}

QStringList GameUiModule::SelectedDescriptionFrames() const {
    QStringList frames;
    if (!tree_ || selected_ >= tree_->Nodes().size() || !tree_->Nodes()[selected_].uiFrame) return frames;
    const std::string& description = tree_->Nodes()[selected_].uiFrame->description;
    std::vector<std::uint32_t> nodes{selected_};
    for (const std::uint32_t node : ExtraSelectedNodes()) nodes.push_back(node);
    for (const std::uint32_t node : nodes) {
        const auto& link = tree_->Nodes()[node].uiFrame;
        if (!link || link->frame.empty() || link->description != description) continue;
        const QString frame = Text(link->frame);
        if (!frames.contains(frame)) frames.push_back(frame);
    }
    return frames;
}

bool GameUiModule::ResizeDescriptionFrame(std::uint32_t node, const FrameRect& rect, QString* error) {
    if (!tree_ || node >= tree_->Nodes().size() || !tree_->Nodes()[node].uiFrame || tree_->Nodes()[node].uiFrame->frame.empty()) {
        if (error) *error = tr("只能改自己界面里控件的大小");
        return false;
    }
    const FrameNode& target = tree_->Nodes()[node];
    const QString id = Text(target.uiFrame->description);
    const QString frameId = Text(target.uiFrame->frame);
    const auto first = DescriptionFrameRect(id, frameId);
    if (!first) {
        if (error) *error = tr("控件 %1 在画面上还没排出位置").arg(frameId);
        return false;
    }
    // 所拖实例各边的变化套到第一个实例上。
    const FrameRect current = target.rect;
    const FrameRect moved{first->left + (rect.left - current.left), first->bottom + (rect.bottom - current.bottom),
        first->right + (rect.right - current.right), first->top + (rect.top - current.top)};
    return SetDescriptionFrameRects(id, {{frameId, moved}}, tr("界面 %1：%2 改大小").arg(id, frameId), error);
}

void GameUiModule::SetPixelUnits(bool pixels) {
    if (pixelUnits_ == pixels) return;
    pixelUnits_ = pixels;
    if (canvas_) canvas_->SetMeasure(pixelUnits_, resolution_);
    UpdateProperties();
}

void GameUiModule::SetReferenceResolution(UiResolution resolution) {
    if (!resolution.Valid() || (resolution.width == resolution_.width && resolution.height == resolution_.height)) return;
    resolution_ = resolution;
    if (canvas_) canvas_->SetMeasure(pixelUnits_, resolution_);
    UpdateProperties();
}

void GameUiModule::SetReferenceImage(const QString& path, double opacity) {
    referenceOpacity_ = std::clamp(opacity, 0.05, 1.0);
    QImage image;
    if (!path.isEmpty() && !image.load(path)) {
        referencePath_.clear();
        if (canvas_) canvas_->SetReferenceImage({}, referenceOpacity_);
        if (referenceButton_) referenceButton_->setChecked(false);
        ReportAuthoring(tr("读不出参考图 %1").arg(QDir::toNativeSeparators(path)));
        return;
    }
    referencePath_ = path;
    if (canvas_) canvas_->SetReferenceImage(image, referenceOpacity_);
    if (referenceButton_) referenceButton_->setChecked(!image.isNull());
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
