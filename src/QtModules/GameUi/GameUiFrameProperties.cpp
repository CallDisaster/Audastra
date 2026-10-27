#include "GameUiModule.h"

#include "GameUiAuthoringJson.h"
#include "GameUiChoice.h"
#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "War3MapCore/GameUi/ScriptFrames.h"

#include <QCheckBox>
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QTimer>
#include <QTreeWidget>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>

// 界面工作区属性树里“编辑”一组：描述框架可直接改的字段（尺寸、贴图、文字、字体、颜色、对齐、进度、显示、
// 锚点、重复）。选择类字段用列表选取，改动整批经 UiDescriptionEdit 检查后放进待保存修改。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

constexpr int kFieldRole = Qt::UserRole + 40;

double Round4(double value) { return std::round(value * 10000.0) / 10000.0; }

GameUiOptions PointOptions(bool raw) {
    static const std::array<const char*, 9> labels{"左上", "上", "右上", "左", "中心", "右", "左下", "下", "右下"};
    GameUiOptions options;
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const QString name = Text(FramePointName(static_cast<FramePoint>(i)));
        options.push_back({raw ? name : QObject::tr(labels[i]), name});
    }
    return options;
}

GameUiOptions JustifyOptions(bool horizontal, bool raw) {
    static const std::array<std::pair<const char*, const char*>, 3> horizontalItems{{{"左", "left"}, {"居中", "center"}, {"右", "right"}}};
    static const std::array<std::pair<const char*, const char*>, 3> verticalItems{{{"上", "top"}, {"居中", "middle"}, {"下", "bottom"}}};
    GameUiOptions options;
    for (const auto& [label, value] : horizontal ? horizontalItems : verticalItems) {
        options.push_back({raw ? QString::fromLatin1(value) : QObject::tr(label), QString::fromLatin1(value)});
    }
    return options;
}

QString TargetValue(const UiAnchor& anchor) {
    switch (anchor.target) {
    case UiAnchorTarget::Screen: return QStringLiteral("screen");
    case UiAnchorTarget::Frame: return Text(anchor.targetId);
    case UiAnchorTarget::GameArea: return QStringLiteral("game:") + Text(anchor.targetId);
    default: return QStringLiteral("parent");
    }
}

// 颜色按 #AARRGGBB 显示（与属性树里只读的颜色一致），也接受 #RRGGBB。
QString ColorValue(const std::array<double, 4>& color) {
    const auto channel = [](double value) { return std::clamp(static_cast<int>(std::lround(value * 255.0)), 0, 255); };
    return QColor(channel(color[0]), channel(color[1]), channel(color[2]), channel(color[3])).name(QColor::HexArgb).toUpper();
}

void CollectIds(const UiFrame& frame, std::set<std::string>& ids) {
    ids.insert(frame.id);
    for (const auto& child : frame.children) CollectIds(child, ids);
}

void CollectFrames(const std::vector<UiFrame>& frames, std::vector<const UiFrame*>& out) {
    for (const auto& frame : frames) {
        out.push_back(&frame);
        CollectFrames(frame.children, out);
    }
}

// 值改为另一个取值时保留原来的换算（倍数、偏移、取反）。
QJsonValue BindingValue(const QJsonValue& previous, const QString& reference) {
    if (!previous.isObject()) return reference;
    QJsonObject binding = previous.toObject();
    binding.insert(QStringLiteral("value"), reference);
    return binding;
}

} // namespace

// 描述框架可直接改的字段：中文名默认，勾选“显示原始字段”时显示文件里的字段名。
void GameUiModule::AddEditableRows(QTreeWidgetItem* group, const UiFrame& frame) {
    const bool raw = rawFields_ && rawFields_->isChecked();
    const auto editable = [&](QTreeWidgetItem* parent, const QString& label, const QString& key, const QString& value) {
        const QString rawLabel = key.section(QLatin1Char(':'), -1);
        auto* item = new QTreeWidgetItem(parent, {raw ? rawLabel : label, value});
        item->setData(1, kFieldRole, key);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setToolTip(1, value);
        return item;
    };
    // 选择类字段：显示中文，取值另存。
    const auto choice = [&](QTreeWidgetItem* parent, const QString& label, const QString& key, const GameUiOptions& options,
                            const QString& value) {
        auto* item = editable(parent, label, key, GameUiChoiceLabel(options, value));
        item->setData(1, kGameUiChoiceRole, value);
        return item;
    };
    const auto number = [](const std::optional<double>& value) { return value ? QString::number(*value) : QString(); };
    const auto binding = [](const UiBinding& bound) {
        QString text = Text(bound.value);
        if (bound.perInstance) text += QStringLiteral("[]");
        else if (bound.index) text += QStringLiteral("[%1]").arg(*bound.index);
        return text;
    };
    editable(group, tr("编号"), QStringLiteral("id"), Text(frame.id));
    editable(group, tr("显示名"), QStringLiteral("label"), Text(frame.label));
    if (frame.type == UiFrameType::Template) {
        choice(group, tr("模板"), QStringLiteral("template"), PropertyChoices(QStringLiteral("template")), Text(frame.templateName));
    }
    editable(group, tr("宽"), QStringLiteral("width"), number(frame.width));
    editable(group, tr("高"), QStringLiteral("height"), number(frame.height));
    const bool image = frame.type == UiFrameType::Backdrop || frame.type == UiFrameType::Bar || frame.type == UiFrameType::Button;
    const bool text = frame.type == UiFrameType::Text || frame.type == UiFrameType::Button;
    if (image) editable(group, tr("贴图"), QStringLiteral("texture"), Text(frame.texture));
    if (frame.type == UiFrameType::Bar) editable(group, tr("底图"), QStringLiteral("background"), Text(frame.background));
    if (frame.type == UiFrameType::Button) editable(group, tr("按下贴图"), QStringLiteral("pushed"), Text(frame.pushed));
    if (text) {
        editable(group, tr("文字"), QStringLiteral("text"), Text(frame.text));
        editable(group, tr("字体"), QStringLiteral("font"), Text(frame.font));
        editable(group, tr("字号"), QStringLiteral("fontHeight"), QString::number(frame.fontHeight));
        auto* color = editable(group, tr("颜色"), QStringLiteral("color"), ColorValue(frame.color));
        color->setData(1, Qt::DecorationRole, QColor::fromRgbF(static_cast<float>(frame.color[0]),
            static_cast<float>(frame.color[1]), static_cast<float>(frame.color[2]), static_cast<float>(frame.color[3])));
        choice(group, tr("水平对齐"), QStringLiteral("justifyH"), JustifyOptions(true, raw),
            Text(std::string_view(frame.justifyH == JustifyH::Left ? "left" : frame.justifyH == JustifyH::Right ? "right" : "center")));
        choice(group, tr("竖直对齐"), QStringLiteral("justifyV"), JustifyOptions(false, raw),
            Text(std::string_view(frame.justifyV == JustifyV::Top ? "top" : frame.justifyV == JustifyV::Bottom ? "bottom" : "middle")));
    }
    if (frame.type == UiFrameType::Bar) {
        editable(group, tr("进度"), QStringLiteral("value"),
            frame.value.binding ? binding(*frame.value.binding) : QString::number(frame.value.literal));
    }
    const QString visible = frame.visible.binding ? binding(*frame.visible.binding)
                                                  : (frame.visible.literal ? QStringLiteral("true") : QStringLiteral("false"));
    choice(group, tr("显示"), QStringLiteral("visible"), PropertyChoices(QStringLiteral("visible")), visible);

    const GameUiOptions points = PointOptions(raw);
    for (std::size_t i = 0; i < frame.anchors.size(); ++i) {
        const UiAnchor& anchor = frame.anchors[i];
        const QString prefix = QStringLiteral("anchor:%1:").arg(i);
        const QString target = TargetValue(anchor);
        auto* item = new QTreeWidgetItem(group, {raw ? QStringLiteral("anchors[%1]").arg(i) : tr("锚点 %1").arg(i + 1),
            tr("%1 → %2 的 %3").arg(GameUiChoiceLabel(points, Text(FramePointName(anchor.point))),
                GameUiChoiceLabel(PropertyChoices(prefix + QStringLiteral("to")), target),
                GameUiChoiceLabel(points, Text(FramePointName(anchor.targetPoint))))});
        item->setData(0, kFieldRole, prefix);
        item->setExpanded(true);
        choice(item, tr("对齐点"), prefix + QStringLiteral("point"), points, Text(FramePointName(anchor.point)));
        choice(item, tr("目标"), prefix + QStringLiteral("to"), PropertyChoices(prefix + QStringLiteral("to")), target);
        choice(item, tr("目标点"), prefix + QStringLiteral("toPoint"), points, Text(FramePointName(anchor.targetPoint)));
        editable(item, tr("横向偏移"), prefix + QStringLiteral("x"), QString::number(anchor.x));
        editable(item, tr("纵向偏移"), prefix + QStringLiteral("y"), QString::number(anchor.y));
    }

    // 重复：次数留空即不重复；有重复时才列出间距。
    auto* repeat = new QTreeWidgetItem(group, {raw ? QStringLiteral("repeat") : tr("重复"),
        frame.repeat ? tr("%1 个").arg(frame.repeat->count) : tr("不重复")});
    repeat->setExpanded(true);
    editable(repeat, tr("次数"), QStringLiteral("repeat:count"), frame.repeat ? QString::number(frame.repeat->count) : QString());
    if (frame.repeat) {
        editable(repeat, tr("横向间距"), QStringLiteral("repeat:dx"), QString::number(frame.repeat->dx));
        editable(repeat, tr("纵向间距"), QStringLiteral("repeat:dy"), QString::number(frame.repeat->dy));
        editable(repeat, tr("每行个数"), QStringLiteral("repeat:columns"), frame.repeat->columns ? QString::number(frame.repeat->columns) : QString());
        editable(repeat, tr("换行横向"), QStringLiteral("repeat:rowDx"), QString::number(frame.repeat->rowDx));
        editable(repeat, tr("换行纵向"), QStringLiteral("repeat:rowDy"), QString::number(frame.repeat->rowDy));
    }
}

QStringList GameUiModule::FrameTemplates() const {
    QStringList names = AvailableTemplates();
    names.removeAll(Text(kGameCodeRoot));
    names.removeAll(Text(kScriptRoot));
    return names;
}

const UiFrame* GameUiModule::SelectedDescriptionFrame(QString* descriptionId) const {
    if (!tree_ || selected_ >= tree_->Nodes().size()) return nullptr;
    const auto& link = tree_->Nodes()[selected_].uiFrame;
    if (!link || link->frame.empty()) return nullptr;
    const UiDescription* description = Description(Text(link->description));
    if (!description) return nullptr;
    if (descriptionId) *descriptionId = Text(description->id);
    return FindUiFrame(*description, link->frame);
}

QList<std::pair<QString, QString>> GameUiModule::PropertyChoices(const QString& key) const {
    const bool raw = rawFields_ && rawFields_->isChecked();
    QString id;
    const UiFrame* frame = SelectedDescriptionFrame(&id);
    const UiDescription* description = frame ? Description(id) : nullptr;
    if (!description) return {};
    const auto valueIds = [&](std::initializer_list<UiValueType> types) {
        GameUiOptions options;
        for (const auto& value : description->values) {
            if (std::find(types.begin(), types.end(), value.type) == types.end()) continue;
            const QString reference = Text(value.id) + (value.size > 0 ? QStringLiteral("[]") : QString());
            options.push_back({raw || value.label.empty() ? reference : QStringLiteral("%1  %2").arg(Text(value.label), reference),
                reference});
        }
        return options;
    };
    if (key == QLatin1String("justifyH")) return JustifyOptions(true, raw);
    if (key == QLatin1String("justifyV")) return JustifyOptions(false, raw);
    if (key == QLatin1String("visible")) {
        GameUiOptions options{{raw ? QStringLiteral("true") : tr("显示"), QStringLiteral("true")},
            {raw ? QStringLiteral("false") : tr("隐藏"), QStringLiteral("false")}};
        options += valueIds({UiValueType::Boolean, UiValueType::String});  // 文字数值非空为显示
        return options;
    }
    if (key == QLatin1String("value")) return valueIds({UiValueType::Real, UiValueType::Integer});
    if (key == QLatin1String("template")) {
        GameUiOptions options;
        for (const QString& name : FrameTemplates()) options.push_back({name, name});
        return options;
    }
    if (key.startsWith(QLatin1String("anchor:")) && (key.endsWith(QLatin1String(":point")) || key.endsWith(QLatin1String(":toPoint")))) {
        return PointOptions(raw);
    }
    if (key.startsWith(QLatin1String("anchor:")) && key.endsWith(QLatin1String(":to"))) {
        // 父框架、屏幕、同一描述里自身以外的框架（不含自己的子框架，免得成环）、游戏界面区域。
        GameUiOptions options{{raw ? QStringLiteral("parent") : tr("父框架"), QStringLiteral("parent")},
            {raw ? QStringLiteral("screen") : tr("屏幕"), QStringLiteral("screen")}};
        std::set<std::string> own;
        CollectIds(*frame, own);
        std::vector<const UiFrame*> frames;
        CollectFrames(description->frames, frames);
        for (const UiFrame* other : frames) {
            if (own.contains(other->id)) continue;
            options.push_back({raw || other->label.empty() ? Text(other->id) : QStringLiteral("%1  %2").arg(Text(other->label), Text(other->id)),
                Text(other->id)});
        }
        const QString gameRoot = QStringLiteral("game:") + Text(kGameCodeRoot);
        options.push_back({raw ? gameRoot : tr("游戏界面"), gameRoot});
        for (const auto& area : GameCodeAreas()) {
            const QString value = QStringLiteral("game:") + Text(area.id);
            options.push_back({raw ? value : tr("游戏界面 · %1").arg(Text(area.label)), value});
        }
        return options;
    }
    return {};
}

bool GameUiModule::PropertyFreeText(const QString& key) const {
    // 显示与进度可取数值，也可直接写字面值或 编号[下标]；模板名可写列表外的名字（例如地图界面文件里的）。
    return key == QLatin1String("visible") || key == QLatin1String("value") || key == QLatin1String("template");
}

void GameUiModule::OnPropertyEdited(QTreeWidgetItem* item, int column) {
    if (column != 1 || !item) return;
    const QString key = item->data(1, kFieldRole).toString();
    QString id;
    const UiFrame* frame = key.isEmpty() ? nullptr : SelectedDescriptionFrame(&id);
    if (!frame) return;
    const QVariant chosen = item->data(1, kGameUiChoiceRole);
    const QString text = (chosen.isValid() ? chosen.toString() : item->text(1)).trimmed();
    const QJsonObject current = FrameObject(*frame);
    QString error;
    QJsonObject fields;
    const auto parseNumber = [&](const QString& what, double* out) {
        bool ok = false;
        *out = text.toDouble(&ok);
        if (!ok) error = tr("%1 应为数值").arg(what);
        return ok;
    };
    if (key == QLatin1String("width") || key == QLatin1String("height") || key == QLatin1String("fontHeight")) {
        double number = 0.0;
        if (text.isEmpty()) fields.insert(key, QJsonValue::Null);
        else if (parseNumber(item->text(0), &number)) fields.insert(key, number);
    } else if (key == QLatin1String("value") || key == QLatin1String("visible")) {
        bool ok = false;
        const double number = text.toDouble(&ok);
        if (text.isEmpty()) fields.insert(key, QJsonValue::Null);
        else if (key == QLatin1String("visible") && (text == QLatin1String("true") || text == tr("显示"))) fields.insert(key, true);
        else if (key == QLatin1String("visible") && (text == QLatin1String("false") || text == tr("隐藏"))) fields.insert(key, false);
        else if (ok && key == QLatin1String("value")) fields.insert(key, number);
        else fields.insert(key, BindingValue(current.value(key), text));  // 取值：编号、编号[] 或 编号[下标]
    } else if (key == QLatin1String("color")) {
        const QColor color = QColor::fromString(text);
        if (text.isEmpty()) {
            fields.insert(key, QJsonValue::Null);
        } else if (!color.isValid() || !text.startsWith(QLatin1Char('#'))) {
            error = tr("颜色写作 #AARRGGBB 或 #RRGGBB");
        } else {
            fields.insert(key, QJsonArray{Round4(color.redF()), Round4(color.greenF()), Round4(color.blueF()), Round4(color.alphaF())});
        }
    } else if (key.startsWith(QLatin1String("repeat:"))) {
        const QString part = key.section(QLatin1Char(':'), 1);
        QJsonObject repeat = current.value(QStringLiteral("repeat")).toObject();
        if (part == QLatin1String("count")) {
            bool ok = false;
            const int count = text.toInt(&ok);
            if (text.isEmpty()) {
                fields.insert(QStringLiteral("repeat"), QJsonValue::Null);
            } else if (!ok || count < 1) {
                error = tr("重复次数应为正整数");
            } else {
                repeat.insert(QStringLiteral("count"), count);
                fields.insert(QStringLiteral("repeat"), repeat);
            }
        } else if (repeat.isEmpty()) {
            error = tr("先填写重复次数");
        } else if (part == QLatin1String("columns")) {
            // 空为不换行。
            bool ok = false;
            const int columns = text.toInt(&ok);
            if (text.isEmpty()) {
                repeat.remove(QStringLiteral("columns"));
                fields.insert(QStringLiteral("repeat"), repeat);
            } else if (!ok || columns < 1) {
                error = tr("每行个数应为正整数，空为不换行");
            } else {
                repeat.insert(QStringLiteral("columns"), columns);
                fields.insert(QStringLiteral("repeat"), repeat);
            }
        } else {
            double number = 0.0;
            if (parseNumber(item->text(0), &number)) {
                repeat.insert(part, number);
                fields.insert(QStringLiteral("repeat"), repeat);
            }
        }
    } else if (key.startsWith(QLatin1String("anchor:"))) {
        QJsonArray anchors = current.value(QStringLiteral("anchors")).toArray();
        const qsizetype index = key.section(QLatin1Char(':'), 1, 1).toInt();
        const QString part = key.section(QLatin1Char(':'), 2);
        if (index < 0 || index >= anchors.size()) return;
        QJsonObject anchor = anchors[index].toObject();
        double number = 0.0;
        if (part == QLatin1String("x") || part == QLatin1String("y")) {
            if (parseNumber(item->text(0), &number)) anchor.insert(part, number);
        } else if (part == QLatin1String("point")) {
            // 目标点未单独写出时跟着对齐点；改对齐点时先把原来的目标点写明，目标点不变。
            if (!anchor.contains(QStringLiteral("toPoint"))) anchor.insert(QStringLiteral("toPoint"), anchor.value(QStringLiteral("point")));
            anchor.insert(QStringLiteral("point"), text);
        } else if (part == QLatin1String("toPoint")) {
            anchor.insert(QStringLiteral("toPoint"), text);
        } else if (part == QLatin1String("to")) {
            if (text == QLatin1String("parent") || text.isEmpty()) anchor.remove(QStringLiteral("to"));
            else anchor.insert(QStringLiteral("to"), text);
        }
        anchors[index] = anchor;
        if (error.isEmpty()) fields.insert(QStringLiteral("anchors"), anchors);
    } else {
        fields.insert(key, text.isEmpty() && key != QLatin1String("id") ? QJsonValue(QJsonValue::Null) : QJsonValue(text));
    }
    if (!error.isEmpty()) {
        ReportAuthoring(error);
        QTimer::singleShot(0, this, &GameUiModule::UpdateProperties);  // 恢复成描述里的值
        return;
    }
    const QString frameId = Text(frame->id);
    const QJsonObject edit{{"op", "set_frame"}, {"frame", frameId}, {"fields", fields}};
    // 改编号或重复次数后原来的节点名不在了：仍选中这个框架（重复时选第一个实例）。
    const QString target = key == QLatin1String("id") ? text : frameId;
    auto after = [this, target] {
        if (const UiFrame* selected = SelectedDescriptionFrame(); selected && Text(selected->id) == target) return;
        if (!SelectFrame(target)) (void)SelectFrame(target + QStringLiteral("#0"));
    };
    CommitDescriptionEdits(id, {edit}, tr("界面 %1：改 %2 的 %3").arg(id, frameId, item->text(0)), std::move(after));
}

bool GameUiModule::AddDescriptionAnchor(QString* error) {
    QString id;
    const UiFrame* frame = SelectedDescriptionFrame(&id);
    if (!frame) {
        if (error) *error = tr("先选中一个界面描述里的框架");
        return false;
    }
    QJsonArray anchors = AnchorsOf(*frame);
    // 新锚点取第一个还没用过的对齐点：无锚点时居中，其次左上、右下，再按游戏顺序。
    std::set<FramePoint> used;
    for (const auto& anchor : frame->anchors) used.insert(anchor.point);
    std::vector<FramePoint> order{FramePoint::Center, FramePoint::TopLeft, FramePoint::BottomRight};
    for (int i = 0; i < 9; ++i) order.push_back(static_cast<FramePoint>(i));
    const auto free = std::find_if(order.begin(), order.end(), [&](FramePoint point) { return !used.contains(point); });
    if (free == order.end()) {
        if (error) *error = tr("九个对齐点都已使用");
        return false;
    }
    anchors.append(QJsonObject{{"point", Text(FramePointName(*free))}});
    const QJsonObject edit{{"op", "set_frame"}, {"frame", Text(frame->id)}, {"fields", QJsonObject{{"anchors", anchors}}}};
    return EditDescription(id, CompactEdits({edit}), tr("界面 %1：%2 添加锚点").arg(id, Text(frame->id)), error);
}

bool GameUiModule::RemoveDescriptionAnchor(int index, QString* error) {
    QString id;
    const UiFrame* frame = SelectedDescriptionFrame(&id);
    if (!frame || index < 0 || static_cast<std::size_t>(index) >= frame->anchors.size()) {
        if (error) error->clear();
        return false;
    }
    QJsonArray anchors = AnchorsOf(*frame);
    anchors.removeAt(index);
    // 删到没有锚点时去掉该字段：与新建时不写锚点相同，框架在游戏中不显示。
    const QJsonValue value = anchors.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(anchors);
    const QJsonObject edit{{"op", "set_frame"}, {"frame", Text(frame->id)}, {"fields", QJsonObject{{"anchors", value}}}};
    return EditDescription(id, CompactEdits({edit}), tr("界面 %1：%2 删除锚点 %3").arg(id, Text(frame->id)).arg(index + 1), error);
}

void GameUiModule::InstallPropertyEditors() {
    const auto key = [](const QModelIndex& index) { return index.data(kFieldRole).toString(); };
    properties_->setItemDelegateForColumn(1, new GameUiChoiceDelegate(
        [this, key](const QModelIndex& index) { return PropertyChoices(key(index)); }, properties_,
        [this, key](const QModelIndex& index) { return PropertyFreeText(key(index)); }));
    properties_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(properties_, &QTreeWidget::customContextMenuRequested, this, &GameUiModule::ShowPropertyMenu);
    connect(properties_, &QTreeWidget::itemChanged, this, &GameUiModule::OnPropertyEdited);
}

void GameUiModule::ShowPropertyMenu(const QPoint& position) {
    if (!properties_ || !SelectedDescriptionFrame()) return;
    QMenu menu(properties_);
    menu.addAction(tr("添加锚点"), this, [this] {
        QString error;
        if (!AddDescriptionAnchor(&error)) ReportAuthoring(error);
    });
    // 点在锚点组或其下的行上时可删该锚点。
    QTreeWidgetItem* item = properties_->itemAt(position);
    QString prefix;
    for (QTreeWidgetItem* at = item; at && prefix.isEmpty(); at = at->parent()) {
        const QString key = at->data(0, kFieldRole).toString();
        if (key.startsWith(QLatin1String("anchor:"))) prefix = key;
    }
    if (!prefix.isEmpty()) {
        const int index = prefix.section(QLatin1Char(':'), 1, 1).toInt();
        menu.addAction(tr("删除锚点 %1").arg(index + 1), this, [this, index] {
            // 删除会重建属性树（菜单所在的行随之删除），放到下一轮事件里。
            QTimer::singleShot(0, this, [this, index] {
                QString error;
                if (!RemoveDescriptionAnchor(index, &error) && !error.isEmpty()) ReportAuthoring(error);
            });
        });
    }
    menu.exec(properties_->viewport()->mapToGlobal(position));
}

} // namespace War3Map::Modules
