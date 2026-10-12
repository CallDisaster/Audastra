#include "TriggerBlueprint.h"

#include "TriggerModelsDetail.h"
#include "TriggerUiText.h"

#include "War3MapCore/Trigger/UiCatalog.h"

#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QLabel>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <map>

// 蓝图布局：按语句结构放置节点、端口与执行线（交互见 TriggerBlueprintInput.cpp）。
using namespace TriggerModelsDetail;
using namespace TriggerBlueprintDetail;

namespace {

constexpr double kPitch = 380;
constexpr double kGap = 34;
constexpr std::size_t kMaxDepth = 64;

QColor KindColor(std::int32_t type, bool block) {
    if (block) return QColor(0xff, 0xca, 0x80);
    switch (type) {
    case 0: return QColor(0x75, 0xd9, 0xff);
    case 1: return QColor(0xc6, 0xa8, 0xff);
    default: return QColor(0x7e, 0xe0, 0xbd);
    }
}

// 语句文字 → HTML：参数记号为 arg:N 链接（空值标红），注释语句整行绿色斜体。
QString SentenceHtml(const War3Map::Trigger::EcaPresentation& presentation, bool comment, const QColor& link,
    BlueprintNode* node, const std::function<QIcon(std::string_view)>& icons, bool rawValues, bool linked = true) {
    const std::string& text = presentation.text;
    const auto escape = [](std::string_view part) { return QString::fromUtf8(part.data(), static_cast<qsizetype>(part.size())).toHtmlEscaped(); };
    QString html;
    std::size_t cursor = 0;
    for (const auto& span : presentation.arguments) {
        if (span.begin < cursor || span.begin + span.length > text.size()) continue;
        html += escape(std::string_view(text).substr(cursor, span.begin - cursor));
        QString shown = span.length == 0 ? QStringLiteral("＿") : escape(std::string_view(text).substr(span.begin, span.length));
        if (!rawValues && icons && !span.iconPath.empty()) {
            const auto icon = icons(span.iconPath);
            if (!icon.isNull()) {
                const auto url = QUrl(QStringLiteral("audastra-icon:%1-%2-%3").arg(node->Id()).arg(span.position).arg(qHash(Utf8(span.iconPath))));
                node->AddImage(url, icon.pixmap(16, 16).toImage());
                shown.prepend(QStringLiteral("<img src=\"%1\" width=\"16\" height=\"16\"/> ").arg(url.toString()));
            }
        }
        if (!rawValues && !span.reference.empty()) shown += QStringLiteral(" <span style=\"font-size:small;\">▤</span>");
        QString tooltip = QObject::tr("原值：%1").arg(Utf8(span.rawValue));
        if (!span.reference.empty()) tooltip += QObject::tr("\n来自地图字符串表：%1").arg(Utf8(span.reference));
        if (!span.issue.empty()) tooltip += QLatin1Char('\n') + Utf8(span.issue);
        for (const auto& candidate : span.candidates) tooltip += QStringLiteral("\n• ") + Utf8(candidate);
        if (linked) {
            node->SetArgumentTooltip(span.position, tooltip);
            html += QStringLiteral("<a href=\"arg:%1\" style=\"color:%2;\">%3</a>")
                .arg(span.position).arg(span.empty ? QStringLiteral("#e5534b") : link.name(), shown);
        } else {
            html += shown;
            node->setToolTip(node->toolTip() + tooltip + QLatin1Char('\n'));
        }
        cursor = span.begin + span.length;
    }
    html += escape(std::string_view(text).substr(std::min(cursor, text.size())));
    return comment ? QStringLiteral("<i style=\"color:#5fbf77\">%1</i>").arg(html) : html;
}

} // namespace

TriggerBlueprint::TriggerBlueprint(QWidget* parent)
    : QGraphicsView(parent),
      scene_(new QGraphicsScene(this)) {
    setObjectName("triggerBlueprint");
    setScene(scene_);
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    setFocusPolicy(Qt::StrongFocus);
    // 缩放比例挂在视图本身而不是视口上：视口的子控件会随滚动一起移动。
    zoomLabel_ = new QLabel(this);
    zoomLabel_->setObjectName("blueprintZoom");
    connect(scene_, &QGraphicsScene::selectionChanged, this, [this]() {
        if (selecting_ || !callbacks_.selected) return;
        const auto items = scene_->selectedItems();
        if (!items.isEmpty()) {
            if (auto* node = dynamic_cast<Node*>(items.front()); node != nullptr && node->Id() != 0) callbacks_.selected(node->Id());
        }
    });
    UpdateZoomLabel();
}

TriggerBlueprint::~TriggerBlueprint() = default;

void TriggerBlueprint::SetSource(const War3Map::Trigger::TriggerDocument* document,
    const War3Map::Trigger::UiCatalog* catalog, War3Map::Trigger::PresentationOptions options) {
    options_ = std::move(options);
    if (document == document_ && catalog == catalog_) {
        return; // 同一文档：保留当前布局与视口
    }
    document_ = document;
    catalog_ = catalog;
    triggerId_ = 0;
    fitted_ = false;
    dragPin_ = nullptr;
    dragWire_ = nullptr;
    nodes_.clear();
    conditionNode_ = nullptr;
    startNode_ = nullptr;
    scene_->clear();
}

TriggerBlueprint::Node* TriggerBlueprint::AddNode(std::uint32_t nodeIndex, double x, double y) {
    const auto& eca = document_->ecaNodes[nodeIndex];
    const bool block = KnownBlock(eca.name.text, catalog_) != nullptr;
    QString title = Utf8(eca.name.text);
    if (catalog_ != nullptr) {
        if (const auto* definition = catalog_->Find(eca.type, eca.name.text)) title = Utf8(War3Map::Trigger::DefinitionTitle(*catalog_, *definition));
    }
    auto* node = new Node(eca.id, eca.type, title, EcaKindName(eca.type), KindColor(eca.type, block && eca.type == 2), eca.enabled != 0);
    QString html = Utf8(eca.name.text).toHtmlEscaped();
    if (catalog_ != nullptr) {
        html = SentenceHtml(War3Map::Trigger::PresentEca(*document_, nodeIndex, *catalog_, options_), IsCommentEca(eca),
            palette().color(QPalette::Link), node, iconResolver_, options_.showRawValues);
    }
    node->SetBody(html, palette().color(QPalette::Text));
    node->setPos(x, y);
    scene_->addItem(node);
    nodes_[eca.id] = node;
    return node;
}

// 触发条件节点：列出全部触发条件（全部成立才执行动作），点击进入传统视图编辑。
TriggerBlueprint::Node* TriggerBlueprint::AddConditionNode(const std::vector<std::uint32_t>& conditions, double x, double y) {
    auto* node = new Node(0, 1, QObject::tr("触发条件"), QObject::tr("%1 项").arg(conditions.size()), KindColor(1, false), true);
    QStringList lines;
    for (const std::uint32_t condition : conditions) {
        const auto& eca = document_->ecaNodes[condition];
        const QString text = catalog_ == nullptr ? Utf8(eca.name.text).toHtmlEscaped()
            : SentenceHtml(War3Map::Trigger::PresentEca(*document_, condition, *catalog_, options_), IsCommentEca(eca),
                palette().color(QPalette::Link), node, iconResolver_, options_.showRawValues, false);
        lines << (eca.enabled == 0 ? QStringLiteral("<s>%1</s>").arg(text) : text);
    }
    node->SetBody(QStringLiteral("<a href=\"cond\" style=\"color:#c6a8ff;\">%1</a><br/><span style=\"color:%2\">%3</span>")
        .arg(lines.join(QStringLiteral("<br/>")), palette().color(QPalette::PlaceholderText).name(),
            QObject::tr("全部成立才执行动作 · 点击在传统视图中编辑")), palette().color(QPalette::Text));
    node->setPos(x, y);
    scene_->addItem(node);
    conditionNode_ = node;
    return node;
}

// 开始节点：没有事件与触发条件的触发器（例如被其他触发器运行的）从这里开始；端口可以直接拉出事件 / 条件 / 动作。
TriggerBlueprint::Node* TriggerBlueprint::AddStartNode(const QString& name, const std::vector<EcaSlot>& roots, War3Map::StableId firstAction) {
    auto* node = new Node(0, -1, QObject::tr("开始"), QObject::tr("触发器"), palette().color(QPalette::Highlight), true);
    node->SetBody(QStringLiteral("%1<br/><span style=\"color:%2\">%3</span>").arg(name.toHtmlEscaped(),
        palette().color(QPalette::PlaceholderText).name(), QObject::tr("从端口拉线添加事件、条件或动作")), palette().color(QPalette::Text));
    for (const EcaSlot& slot : roots) {
        const PinTarget target{slot, slot.type == 2 ? firstAction : 0, slot.label};
        node->AddPin(slot.type == 2 ? PinKind::Out : PinKind::Branch, slot.label, KindColor(slot.type, false), target);
        node->AddMenuTarget({slot, 0, slot.label});
    }
    scene_->addItem(node);
    startNode_ = node;
    return node;
}

void TriggerBlueprint::AddWire(Pin* from, Pin* to, const QColor& color) {
    if (from == nullptr || to == nullptr) return;
    // 进入输入端口时向下；进入右侧端口时向右。
    const QPointF arrive = to->Kind() == PinKind::In ? QPointF(0, 1) : QPointF(1, 0);
    auto* wire = scene_->addPath(ExecWire(from->scenePos(), from->Direction(), to->scenePos(), arrive), QPen(color, 2));
    wire->setZValue(-1);
    from->SetConnected(true);
    to->SetConnected(true);
    ++wires_;
}

TriggerBlueprint::Placed TriggerBlueprint::LayoutChain(
    const std::vector<std::uint32_t>& chain, const EcaSlot& slot, int column, double top, std::size_t depth) {
    Placed placed;
    placed.bottom = top;
    if (depth > kMaxDepth) {
        return placed;
    }
    std::vector<std::uint32_t> valid;
    for (const std::uint32_t index : chain) {
        if (index < document_->ecaNodes.size()) valid.push_back(index);
    }
    double y = top;
    const QColor exec = palette().color(QPalette::PlaceholderText);
    const QColor kindColor = KindColor(slot.type, false);
    for (std::size_t position = 0; position < valid.size(); ++position) {
        const std::uint32_t index = valid[position];
        const auto& eca = document_->ecaNodes[index];
        const War3Map::StableId next = position + 1 < valid.size() ? document_->ecaNodes[valid[position + 1]].id : 0;
        Node* node = AddNode(index, column * kPitch, y);
        node->AddPin(PinKind::In, QObject::tr("输入"), kindColor, {slot, eca.id, slot.label});
        node->AddPin(PinKind::Next, QObject::tr("下一步"), slot.type == 2 ? exec : kindColor, {slot, next, slot.label});
        node->AddMenuTarget({slot, next, {}});

        // 分支：声明的分支（含空的“否则”）+ 实际出现的其他分支号。
        std::map<std::int32_t, std::pair<std::int32_t, std::vector<std::uint32_t>>> branches;
        for (const BranchSpec& spec : DeclaredBranches(eca.name.text, catalog_)) branches[spec.branch] = {spec.type, {}};
        for (const std::uint32_t child : eca.children) {
            if (child >= document_->ecaNodes.size()) continue;
            auto& branch = branches.try_emplace(document_->ecaNodes[child].childId.value_or(0),
                std::make_pair(document_->ecaNodes[child].type, std::vector<std::uint32_t>{})).first->second;
            branch.second.push_back(child);
        }
        std::vector<std::pair<Pin*, std::vector<std::uint32_t>>> branchPins;
        for (const auto& [branchId, entry] : branches) {
            const EcaSlot branchSlot{eca.id, branchId, entry.first, BranchLabel(eca.name.text, branchId, entry.first, catalog_)};
            const War3Map::StableId first = entry.second.empty() ? 0 : document_->ecaNodes[entry.second.front()].id;
            Pin* pin = node->AddPin(PinKind::Branch, branchSlot.label, KindColor(entry.first, entry.first == 2), {branchSlot, first, branchSlot.label});
            node->AddMenuTarget({branchSlot, 0, branchSlot.label});
            branchPins.emplace_back(pin, entry.second);
        }
        if (placed.last != nullptr) {
            AddWire(placed.last->FindPin(PinKind::Next), node->FindPin(PinKind::In), slot.type == 2 ? exec : kindColor);
        }
        if (placed.first == nullptr) placed.first = node;

        // 分支内的语句向右展开，首个语句略低于分支端口，执行线从端口向右再向下进入。
        double bottom = node->sceneBoundingRect().bottom();
        double branchTop = y;
        for (const auto& [pin, children] : branchPins) {
            if (children.empty()) continue;
            const double childTop = std::max(branchTop, pin->scenePos().y() + 18);
            const Placed sub = LayoutChain(children, pin->Target().slot, column + 1, childTop, depth + 1);
            if (sub.first != nullptr) AddWire(pin, sub.first->FindPin(PinKind::In), pin->Color());
            branchTop = sub.bottom + kGap;
            bottom = std::max(bottom, sub.bottom);
        }
        y = bottom + kGap;
        placed.last = node;
        placed.bottom = bottom;
    }
    return placed;
}

void TriggerBlueprint::ShowTrigger(War3Map::StableId triggerId) {
    const bool sameTrigger = triggerId == triggerId_ && fitted_;
    const QPointF center = mapToScene(viewport()->rect().center());
    const War3Map::StableId selected = SelectedEca();
    selecting_ = true;
    dragPin_ = nullptr;
    dragWire_ = nullptr;
    scene_->clear();
    nodes_.clear();
    conditionNode_ = nullptr;
    startNode_ = nullptr;
    wires_ = 0;
    triggerId_ = triggerId;
    const War3Map::Trigger::TriggerRecord* trigger = nullptr;
    if (document_ != nullptr) {
        for (const auto& record : document_->triggers) {
            if (record.id == triggerId) trigger = &record;
        }
    }
    if (trigger != nullptr && trigger->customTextFlag == 0 && trigger->type == 0) {
        std::vector<std::uint32_t> events;
        std::vector<std::uint32_t> conditions;
        std::vector<std::uint32_t> actions;
        for (const std::uint32_t root : trigger->ecas) {
            if (root >= document_->ecaNodes.size()) continue;
            switch (document_->ecaNodes[root].type) {
            case 0: events.push_back(root); break;
            case 1: conditions.push_back(root); break;
            default: actions.push_back(root); break;
            }
        }
        const std::vector<EcaSlot> roots = {
            {0, std::nullopt, 0, QObject::tr("事件")}, {0, std::nullopt, 1, QObject::tr("触发条件")}, {0, std::nullopt, 2, QObject::tr("动作")}};
        const War3Map::StableId firstAction = actions.empty() ? 0 : document_->ecaNodes[actions.front()].id;
        const War3Map::StableId firstCondition = conditions.empty() ? 0 : document_->ecaNodes[conditions.front()].id;
        const PinTarget toActions{roots[2], firstAction, roots[2].label};
        // 执行顺序：事件 → 触发条件 → 动作链。事件与触发条件在第 0 列，动作从第 1 列开始。
        Pin* flow = nullptr;
        if (events.empty() && conditions.empty()) {
            flow = AddStartNode(Utf8(trigger->name.text), roots, firstAction)->FindPin(PinKind::Out);
        } else {
            const Placed placedEvents = LayoutChain(events, roots[0], 0, 0, 0);
            if (!conditions.empty()) {
                Node* gate = AddConditionNode(conditions, 0, events.empty() ? 0 : placedEvents.bottom + kGap);
                gate->AddPin(PinKind::In, QObject::tr("输入"), KindColor(1, false), {roots[1], firstCondition, roots[1].label});
                gate->AddPin(PinKind::Next, QObject::tr("下一步"), KindColor(1, false), {roots[1], 0, roots[1].label});
                gate->AddMenuTarget({roots[1], 0, roots[1].label});
                flow = gate->AddPin(PinKind::Out, roots[2].label, KindColor(2, false), toActions);
                if (placedEvents.last != nullptr) {
                    AddWire(placedEvents.last->FindPin(PinKind::Next), gate->FindPin(PinKind::In), KindColor(0, false));
                }
            } else if (placedEvents.last != nullptr) {
                flow = placedEvents.last->AddPin(PinKind::Out, roots[2].label, KindColor(2, false), toActions);
            }
        }
        // 动作链从通向它的端口略下方开始，执行线向右再向下进入第一个动作。
        const double actionsTop = flow == nullptr ? 0 : std::max(0.0, flow->scenePos().y() + 18);
        const Placed chain = LayoutChain(actions, roots[2], 1, actionsTop, 0);
        if (chain.first != nullptr) AddWire(flow, chain.first->FindPin(PinKind::In), KindColor(2, false));
    }
    scene_->setSceneRect(scene_->itemsBoundingRect().adjusted(-80, -80, 400, 300));
    selecting_ = false;
    if (sameTrigger) {
        centerOn(center);
    } else {
        Fit();
    }
    if (selected != 0) Select(selected, false);
}

void TriggerBlueprint::Select(War3Map::StableId ecaId, bool center) {
    selecting_ = true;
    scene_->clearSelection();
    if (const auto found = nodes_.find(ecaId); found != nodes_.end()) {
        found->second->setSelected(true);
        if (center) ensureVisible(found->second, 60, 60);
    }
    selecting_ = false;
}

War3Map::StableId TriggerBlueprint::SelectedEca() const {
    const auto items = scene_->selectedItems();
    if (items.isEmpty()) return 0;
    const auto* node = dynamic_cast<const Node*>(items.front());
    return node == nullptr ? 0 : node->Id();
}

void TriggerBlueprint::Fit() {
    fitted_ = true;
    const QRectF bounds = scene_->itemsBoundingRect();
    if (bounds.isEmpty()) {
        resetTransform();
        UpdateZoomLabel();
        return;
    }
    // 初始阅读比例：放得下时 100%，否则缩到能看全，最小 35%（原型 R02 的 100% 初始比例）。
    const double zoom = std::clamp(std::min((viewport()->width() - 60) / bounds.width(),
        (viewport()->height() - 60) / bounds.height()), 0.35, 1.0);
    resetTransform();
    scale(zoom, zoom);
    if (zoom >= 1.0 || bounds.height() * zoom > viewport()->height()) {
        centerOn(bounds.center().x(), bounds.top() + viewport()->height() / (2 * zoom) - 30);
    } else {
        centerOn(bounds.center());
    }
    UpdateZoomLabel();
}

void TriggerBlueprint::ZoomBy(double factor) {
    const double current = transform().m11();
    const double next = std::clamp(current * factor, 0.25, 1.7);
    scale(next / current, next / current);
    UpdateZoomLabel();
}

void TriggerBlueprint::UpdateZoomLabel() {
    zoomLabel_->setText(QStringLiteral("%1%").arg(qRound(transform().m11() * 100)));
    zoomLabel_->adjustSize();
    const QRect area = viewport()->geometry();
    zoomLabel_->move(area.right() - zoomLabel_->width() - 12, area.bottom() - zoomLabel_->height() - 10);
    zoomLabel_->raise();
}
