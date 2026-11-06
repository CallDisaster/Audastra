#include "GameUiModule.h"

#include "GameUiAuthoringJson.h"
#include "GameUiChoice.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"
#include "War3MapCore/GameUi/UiOrigins.h"
#include "War3MapCore/GameUi/UiFrameKinds.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QTreeWidget>

#include <cmath>

namespace War3Map::Modules {
using namespace GameUi;
namespace {
constexpr int kFieldRole = Qt::UserRole + 40;
QString Text(std::string_view value) { return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size())); }
QJsonObject OriginJson(const UiOriginAdjustment& origin) {
    UiDescription temporary; temporary.id = "origin"; temporary.origins.push_back(origin);
    return QJsonDocument::fromJson(QByteArray::fromStdString(SerializeUiDescription(temporary))).object()["origins"].toArray().first().toObject();
}
}

QString GameUiModule::OriginNodeName(std::uint32_t node) const {
    if (!tree_ || node == kScreenNode || node >= tree_->Nodes().size()) return {};
    const auto& frame = tree_->Nodes()[node];
    if (frame.uiFrame || frame.scriptFrame) return {};
    const auto* part = FindUiOriginPart(frame.name);
    return part ? Text(part->name) : QString();
}

QString GameUiModule::OriginDescriptionId(const QString& name) const {
    for (const auto& file : Descriptions()) {
        for (const auto& origin : file.description.origins) if (Text(origin.name) == name) return Text(file.description.id);
    }
    for (const auto& file : Descriptions()) if (!file.description.origins.empty()) return Text(file.description.id);
    return Description(QStringLiteral("OriginalUi")) ? QStringLiteral("OriginalUi") : QString();
}

bool GameUiModule::EditOriginNode(const QString& name, const QJsonObject& fields, QString* error) {
    const auto* part = FindUiOriginPart(name.toStdString());
    if (!part) { if (error) *error = tr("不是可调整的原版节点"); return false; }
    QString id = OriginDescriptionId(name);
    const auto* description = Description(id);
    UiDescription base;
    if (description) base = *description;
    else { base.id = "OriginalUi"; base.label = "原版界面"; id = QStringLiteral("OriginalUi"); }
    QJsonObject origin{{"name", Text(part->name)}};
    for (const auto& change : base.origins) if (change.name == part->name) origin = OriginJson(change);
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        if (it.value().isNull()) origin.remove(it.key()); else origin[it.key()] = it.value();
    }
    const QJsonObject edit = origin.size() == 1 ? QJsonObject{{"op", "remove_origin"}, {"name", name}} : QJsonObject{{"op", "set_origin"}, {"origin", origin}};
    auto changed = ApplyUiEdits(base, CompactEdits({edit}).toStdString());
    if (!changed.value) { if (error) *error = Text(changed.diagnostics.front().message); return false; }
    if (*changed.value == base) return true;
    return StageDescription(std::move(*changed.value), !description, tr("调整原版界面 %1").arg(Text(part->label)), error);
}

bool GameUiModule::RestoreOriginNode(const QString& name, QString* error) {
    const auto id = OriginDescriptionId(name);
    if (id.isEmpty()) return true;
    return EditDescription(id, CompactEdits({QJsonObject{{"op", "remove_origin"}, {"name", name}}}), tr("恢复原版界面 %1").arg(name), error);
}

bool GameUiModule::MoveOriginNode(std::uint32_t node, double dx, double dy, QString* error) {
    const auto name = OriginNodeName(node);
    if (name.isEmpty() || tree_->Nodes()[node].layout != LayoutState::Placed) { if (error) *error = tr("此节点没有可调整的位置"); return false; }
    const auto rect = tree_->Nodes()[node].rect;
    return EditOriginNode(name, {{"position", QJsonObject{{"point", "BOTTOMLEFT"}, {"x", std::round((rect.left + dx) * 10000) / 10000},
        {"y", std::round((rect.bottom + dy) * 10000) / 10000}}}}, error);
}

bool GameUiModule::CopyOriginTemplate(std::uint32_t nodeId, QString* error) {
    if (!tree_ || nodeId >= tree_->Nodes().size() || !definitions_ || !definitions_->library) return false;
    const auto& node = tree_->Nodes()[nodeId];
    if (node.uiFrame || node.scriptFrame || !node.codeLabel.empty() || !definitions_->library->FindTemplate(node.name)) {
        if (error) *error = tr("此节点由游戏代码创建，没有可复制的模板"); return false;
    }
    std::string id = "CopiedUi";
    for (std::size_t n = 1; Description(Text(id)); ++n) id = "CopiedUi" + std::to_string(n);
    UiDescription description; description.id = id; description.label = "复制 " + node.name;
    const auto* kind = FindUiFrameKind("template");
    if (!kind) return false;
    auto frame = DefaultUiFrame(*kind, "Template1", node.name);
    if (node.layout == LayoutState::Placed) {
        frame.anchors = {UiAnchor{FramePoint::BottomLeft, UiAnchorTarget::Screen, {}, FramePoint::BottomLeft, node.rect.left, node.rect.bottom}};
        frame.width = node.rect.Width(); frame.height = node.rect.Height();
    }
    description.frames.push_back(std::move(frame));
    if (!StageDescription(std::move(description), true, tr("复制为自己的界面"), error)) return false;
    SelectFrameWhenReady(QStringLiteral("Template1"));
    return true;
}

void GameUiModule::AddOriginProperties() {
    const auto name = OriginNodeName(selected_);
    if (name.isEmpty()) return;
    const auto* description = Description(OriginDescriptionId(name));
    QJsonObject current;
    if (description) for (const auto& change : description->origins) if (Text(change.name) == name) current = OriginJson(change);
    auto* group = new QTreeWidgetItem(properties_, {tr("原版界面调整")}); group->setExpanded(true);
    if (const auto* part = FindUiOriginPart(name.toStdString()); part && !part->reason.empty()) {
        auto* support = new QTreeWidgetItem(group, {tr("支持范围"), Text(part->reason)});
        support->setToolTip(1, Text(part->reason));
    }
    const auto row = [&](const QString& label, const QString& key, const QString& value) {
        const auto options = PropertyChoices("origin:" + key);
        auto* item = new QTreeWidgetItem(group, {label, options.isEmpty() ? value : GameUiChoiceLabel(options, value)});
        item->setData(1, kFieldRole, "origin:" + key);
        if (!options.isEmpty()) item->setData(1, kGameUiChoiceRole, value);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    };
    const auto visible = current.contains("visible") ? (current["visible"].toBool() ? QStringLiteral("true") : QStringLiteral("false")) : QString();
    row(tr("显示"), "visible", visible);
    row(tr("缩放"), "scale", current.contains("scale") ? QString::number(current["scale"].toDouble(), 'g', 15) : QString());
    row(tr("透明度"), "alpha", current.contains("alpha") ? QString::number(current["alpha"].toInt()) : QString());
    const auto position = current["position"].toObject();
    row(tr("自身锚点"), "point", position["point"].toString());
    row(tr("屏幕锚点"), "toPoint", position["toPoint"].toString(position["point"].toString()));
    row(tr("水平偏移"), "x", position.isEmpty() ? QString() : QString::number(position["x"].toDouble(), 'g', 15));
    row(tr("垂直偏移"), "y", position.isEmpty() ? QString() : QString::number(position["y"].toDouble(), 'g', 15));
}

bool GameUiModule::OnOriginPropertyEdited(QTreeWidgetItem* item) {
    const auto key = item->data(1, kFieldRole).toString();
    if (!key.startsWith("origin:")) return false;
    const auto name = OriginNodeName(selected_);
    const auto chosen = item->data(1, kGameUiChoiceRole);
    const auto text = (chosen.isValid() ? chosen.toString() : item->text(1)).trimmed();
    const auto field = key.mid(7);
    QJsonValue value = QJsonValue::Null;
    QString error;
    if (!text.isEmpty()) {
        if (field == "visible") { if (text != "true" && text != "false") error = tr("显示应选显示、隐藏或原样"); else value = text == "true"; }
        else if (field == "point" || field == "toPoint") value = text;
        else { bool ok = false; const auto number = text.toDouble(&ok); if (!ok || !std::isfinite(number)) error = tr("调整值应为有限数值"); else value = number; }
    }
    QJsonObject fields;
    if (field == "point" || field == "toPoint" || field == "x" || field == "y") {
        if (value.isNull()) fields["position"] = QJsonValue::Null;
        else {
            QJsonObject position;
            const auto* d = Description(OriginDescriptionId(name));
            if (d) for (const auto& change : d->origins) if (Text(change.name) == name) position = OriginJson(change)["position"].toObject();
            if (position.isEmpty() && tree_ && selected_ < tree_->Nodes().size()) {
                const auto rect = tree_->Nodes()[selected_].rect;
                position = {{"point", "BOTTOMLEFT"}, {"x", rect.left}, {"y", rect.bottom}};
            }
            position[field] = value; fields["position"] = position;
        }
    } else fields[field] = value;
    QTimer::singleShot(0, this, [this, name, fields, error]() mutable {
        if (!error.isEmpty() || !EditOriginNode(name, fields, &error)) ReportAuthoring(error);
        UpdateProperties();
    });
    return true;
}
} // namespace War3Map::Modules
