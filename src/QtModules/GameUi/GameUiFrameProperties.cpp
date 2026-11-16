#include "GameUiModule.h"

#include "GameUiAuthoringJson.h"
#include "GameUiChoice.h"
#include "GameUiNodeTree.h"
#include "War3MapCore/GameUi/UiFrameKinds.h"
#include "War3MapCore/GameUi/UiDescriptionEdit.h"
#include "War3MapCore/GameUi/UiStyles.h"
#include "Modules/ModuleHost.h"
#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "War3MapCore/GameUi/ScriptFrames.h"
#include "Widgets/TablePopupAnchor.h"

#include <QCheckBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QColor>
#include <QColorDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QPixmap>
#include <QScreen>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <set>

// 界面工作区属性树里自己控件的可改字段，分组为 基本 / 位置与大小 / 外观 / 事件 / 阵列。选择类字段用列表选取，
// 贴图用资源选择器，长度可按像素输入；改动经 UiDescriptionEdit 检查后放进待保存修改。右键“设置…”的卡片用同一套字段与修改。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

QString Text(std::string_view text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

constexpr int kFieldRole = Qt::UserRole + 40;
constexpr int kResourceRole = Qt::UserRole + 41;  // 贴图、模型字段：双击打开资源选择器；值为资源类型加 1
constexpr int kColorRole = Qt::UserRole + 42;     // 颜色字段：双击或右键打开取色器
constexpr int kGroupRole = Qt::UserRole + 43;     // 分组行：基本、位置与大小……（记住折叠）


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
// 取色器（带透明度）：选定后写回这一行，经同一属性修改检查写入。
void PickColor(QWidget* parent, QTreeWidgetItem* item) {
    const QColor current = QColor::fromString(item->text(1));
    const QColor color = QColorDialog::getColor(current.isValid() ? current : QColor(Qt::white), parent, QObject::tr("颜色"),
        QColorDialog::ShowAlphaChannel);
    if (color.isValid()) item->setText(1, color.name(QColor::HexArgb).toUpper());
}

QJsonValue BindingValue(const QJsonValue& previous, const QString& reference) {
    if (!previous.isObject()) return reference;
    QJsonObject binding = previous.toObject();
    binding.insert(QStringLiteral("value"), reference);
    return binding;
}

} // namespace

/// 自己控件的可改字段，分组列出：中文名默认，勾选“显示原始字段”时显示文件里的字段名。
void GameUiModule::AddEditableRows(QTreeWidgetItem* root, const UiFrame& frame) {
    const bool raw = rawFields_ && rawFields_->isChecked();
    QString descriptionId;
    SelectedDescriptionFrame(&descriptionId);
    const auto group = [this](QTreeWidgetItem* parent, const QString& title, const QString& key) {
        auto* item = new QTreeWidgetItem(parent, {title});
        item->setFirstColumnSpanned(true);
        QFont font = item->font(0);
        font.setWeight(QFont::DemiBold);
        item->setFont(0, font);
        MarkPropertyGroup(item, key);
        return item;
    };
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
    // 长度：按当前单位显示，提示里给出另一种单位。
    const auto length = [&](QTreeWidgetItem* parent, const QString& label, const QString& key, std::optional<double> units, bool horizontal) {
        auto* item = editable(parent, label, key, units ? LengthText(*units, horizontal) : QString());
        if (units) {
            const double pixels = horizontal ? UiPixelsFromUnitsX(*units, resolution_) : UiPixelsFromUnitsY(*units, resolution_);
            item->setToolTip(1, pixelUnits_ ? QString::number(*units)
                                            : tr("%1 像素（%2×%3）").arg(std::round(pixels * 100.0) / 100.0).arg(resolution_.width).arg(resolution_.height));
        }
        return item;
    };
    const auto binding = [](const UiBinding& bound) {
        QString text = Text(bound.value);
        if (bound.perInstance) text += QStringLiteral("[]");
        else if (bound.index) text += QStringLiteral("[%1]").arg(*bound.index);
        return text;
    };

    // 基本
    auto* basic = group(root, tr("基本"), QStringLiteral("basic"));
    editable(basic, tr("名称"), QStringLiteral("label"), Text(frame.label));
    editable(basic, tr("编号"), QStringLiteral("id"), Text(frame.id));
    auto* typeRow = new QTreeWidgetItem(basic, {raw ? QStringLiteral("type") : tr("种类"), GameUiNodeTree::FrameKindLabel(frame.type)});
    typeRow->setToolTip(1, Text(UiFrameTypeName(frame.type)));
    if (frame.type == UiFrameType::Template) {
        choice(basic, tr("模板"), QStringLiteral("template"), PropertyChoices(QStringLiteral("template")), Text(frame.templateName));
    }
    if (const UiDescription* description = Description(descriptionId)) {
        const QString parent = Text(UiFrameParentId(*description, frame.id));
        choice(basic, tr("父控件"), QStringLiteral("parent"), PropertyChoices(QStringLiteral("parent")), parent);
    }
    const QString visible = frame.visible.binding ? binding(*frame.visible.binding)
                                                  : (frame.visible.literal ? QStringLiteral("true") : QStringLiteral("false"));
    choice(basic, tr("显示"), QStringLiteral("visible"), PropertyChoices(QStringLiteral("visible")), visible);
    editable(basic, tr("透明度"), QStringLiteral("alpha"), QStringLiteral("%1%").arg(std::round(frame.alpha * 1000.0) / 10.0))
        ->setToolTip(1, tr("100% 不透明，0% 全透明；子控件随之变淡"));

    // 位置与大小
    auto* place = group(root, tr("位置与大小"), QStringLiteral("place"));
    choice(place, tr("单位"), QStringLiteral("unit"), PropertyChoices(QStringLiteral("unit")),
        pixelUnits_ ? QStringLiteral("pixel") : QStringLiteral("war3"));
    choice(place, tr("参考分辨率"), QStringLiteral("resolution"), PropertyChoices(QStringLiteral("resolution")),
        QStringLiteral("%1×%2").arg(resolution_.width).arg(resolution_.height));
    const bool fill = FillsParent(frame);
    choice(place, tr("铺满父控件"), QStringLiteral("fill"), PropertyChoices(QStringLiteral("fill")), fill ? QStringLiteral("true") : QStringLiteral("false"));
    const GameUiOptions points = PointOptions(raw);
    if (!fill) {
        length(place, tr("宽"), QStringLiteral("width"), frame.width, true);
        length(place, tr("高"), QStringLiteral("height"), frame.height, false);
        const auto anchorRows = [&](QTreeWidgetItem* parent, std::size_t i) {
            const QString prefix = QStringLiteral("anchor:%1:").arg(i);
            if (i >= frame.anchors.size()) {
                choice(parent, tr("对齐点"), prefix + QStringLiteral("point"), points, QString())->setText(1, tr("（未定位）"));
                return;
            }
            const UiAnchor& anchor = frame.anchors[i];
            choice(parent, tr("对齐点"), prefix + QStringLiteral("point"), points, Text(FramePointName(anchor.point)));
            choice(parent, tr("对齐到"), prefix + QStringLiteral("to"), PropertyChoices(prefix + QStringLiteral("to")), TargetValue(anchor));
            choice(parent, tr("目标点"), prefix + QStringLiteral("toPoint"), points, Text(FramePointName(anchor.targetPoint)));
            length(parent, tr("X"), prefix + QStringLiteral("x"), anchor.x, true);
            length(parent, tr("Y"), prefix + QStringLiteral("y"), anchor.y, false)->setToolTip(0, tr("向上为正"));
        };
        // 第一个锚点直接列在组里，其余锚点各占一组。
        anchorRows(place, 0);
        for (std::size_t i = 1; i < frame.anchors.size(); ++i) {
            const UiAnchor& anchor = frame.anchors[i];
            const QString prefix = QStringLiteral("anchor:%1:").arg(i);
            auto* item = new QTreeWidgetItem(place, {raw ? QStringLiteral("anchors[%1]").arg(i) : tr("锚点 %1").arg(i + 1),
                tr("%1 → %2 的 %3").arg(GameUiChoiceLabel(points, Text(FramePointName(anchor.point))),
                    GameUiChoiceLabel(PropertyChoices(prefix + QStringLiteral("to")), TargetValue(anchor)),
                    GameUiChoiceLabel(points, Text(FramePointName(anchor.targetPoint))))});
            item->setData(0, kFieldRole, prefix);
            item->setExpanded(true);
            anchorRows(item, i);
        }
    }

    // 外观
    auto* look = group(root, tr("外观"), QStringLiteral("look"));
    // 贴图与模型：有资源选择器时双击打开它（自定义页可写路径或 {编号} 模板）；贴图行首显示已读到的贴图。
    const auto resource = [&](const QString& label, const QString& key, const std::string& value, Services::ObjectResourceKind kind) {
        auto* item = editable(look, label, key, Text(value));
        if (resourcePickerFactory_) {
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            item->setData(1, kResourceRole, static_cast<int>(kind) + 1);
        }
        return item;
    };
    const auto texture = [&](const QString& label, const QString& key, const std::string& value) {
        auto* item = resource(label, key, value, Services::ObjectResourceKind::Texture);
        const auto found = textures_.constFind(GameUiAssetKey(Text(EnsureTextureExtension(value))));
        if (found != textures_.constEnd() && !found->isNull()) {
            item->setData(1, Qt::DecorationRole, QPixmap::fromImage(found->scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        }
    };
    const bool image = frame.type == UiFrameType::Backdrop || frame.type == UiFrameType::Bar || frame.type == UiFrameType::Button;
    const bool text = frame.type == UiFrameType::Text || frame.type == UiFrameType::Button;
    if (image) texture(tr("贴图"), QStringLiteral("texture"), frame.texture);
    if (frame.type == UiFrameType::Bar) texture(tr("底图"), QStringLiteral("background"), frame.background);
    if (frame.type == UiFrameType::Button) texture(tr("按下贴图"), QStringLiteral("pushed"), frame.pushed);
    if (frame.type == UiFrameType::Model) resource(tr("模型"), QStringLiteral("model"), frame.model, Services::ObjectResourceKind::Model);
    if (text) {
        editable(look, tr("文字"), QStringLiteral("text"), Text(frame.text));
        editable(look, tr("字体"), QStringLiteral("font"), Text(frame.font));
        length(look, tr("字号"), QStringLiteral("fontHeight"), frame.fontHeight, false);
        auto* color = editable(look, tr("颜色"), QStringLiteral("color"), ColorValue(frame.color));
        color->setData(1, Qt::DecorationRole, QColor::fromRgbF(static_cast<float>(frame.color[0]),
            static_cast<float>(frame.color[1]), static_cast<float>(frame.color[2]), static_cast<float>(frame.color[3])));
        color->setFlags(color->flags() & ~Qt::ItemIsEditable);
        color->setData(1, kColorRole, true);
        choice(look, tr("水平对齐"), QStringLiteral("justifyH"), JustifyOptions(true, raw),
            Text(std::string_view(frame.justifyH == JustifyH::Left ? "left" : frame.justifyH == JustifyH::Right ? "right" : "center")));
        choice(look, tr("竖直对齐"), QStringLiteral("justifyV"), JustifyOptions(false, raw),
            Text(std::string_view(frame.justifyV == JustifyV::Top ? "top" : frame.justifyV == JustifyV::Bottom ? "bottom" : "middle")));
    }
    // 模板控件：模板能写文字（文字模板或带 ButtonText 的按钮）时可改文字，字体颜色仍用模板的。
    if (frame.type == UiFrameType::Template && definitions_ && definitions_->library
        && UiTemplateTextTarget(*definitions_->library, frame.templateName)) {
        editable(look, tr("文字"), QStringLiteral("text"), Text(frame.text));
    }
    if (frame.type == UiFrameType::Bar) {
        editable(look, tr("进度"), QStringLiteral("value"),
            frame.value.binding ? binding(*frame.value.binding) : QString::number(frame.value.literal));
    }
    if (look->childCount() == 0) delete look;

    // 事件
    AddEventRows(group(root, tr("事件"), QStringLiteral("events")), descriptionId, frame);

    // 阵列：数量留空即只有一个；有阵列时才列出间距。
    auto* repeat = group(root, raw ? QStringLiteral("repeat") : tr("阵列"), QStringLiteral("repeat"));
    editable(repeat, tr("数量"), QStringLiteral("repeat:count"), frame.repeat ? QString::number(frame.repeat->count) : QString());
    if (frame.repeat) {
        length(repeat, tr("横向间距"), QStringLiteral("repeat:dx"), frame.repeat->dx, true);
        length(repeat, tr("纵向间距"), QStringLiteral("repeat:dy"), frame.repeat->dy, false);
        editable(repeat, tr("每行个数"), QStringLiteral("repeat:columns"), frame.repeat->columns ? QString::number(frame.repeat->columns) : QString());
        length(repeat, tr("换行横向"), QStringLiteral("repeat:rowDx"), frame.repeat->rowDx, true);
        length(repeat, tr("换行纵向"), QStringLiteral("repeat:rowDy"), frame.repeat->rowDy, false);
    }
}

// 事件：点击、鼠标进入、鼠标离开各一行，值为订阅它的触发器；选“新建触发器”建好后跳过去。
void GameUiModule::AddEventRows(QTreeWidgetItem* group, const QString& description, const UiFrame& frame) {
    const bool raw = rawFields_ && rawFields_->isChecked();
    struct Kind { UiEventKind kind; const char* label; const char* on; };
    static const std::array<Kind, 3> kinds{{{UiEventKind::Click, "点击", "click"}, {UiEventKind::Enter, "鼠标进入", "enter"},
        {UiEventKind::Leave, "鼠标离开", "leave"}}};
    for (const Kind& kind : kinds) {
        const QString key = QStringLiteral("event:") + QLatin1String(kind.on);
        const FrameEventLink link = FrameEventTriggers(description, Text(frame.id), kind.kind);
        QStringList names;
        for (const auto& trigger : link.triggers) names.push_back(trigger.second);
        const QString value = link.triggers.isEmpty() ? QString() : QStringLiteral("trigger:%1").arg(link.triggers.front().first);
        const QString shown = !names.isEmpty() ? names.join(QStringLiteral("、")) : link.eventId.isEmpty() ? tr("（无）") : tr("（无触发器）");
        auto* item = new QTreeWidgetItem(group, {raw ? QString::fromLatin1(kind.on) : tr(kind.label), shown});
        item->setData(1, kFieldRole, key);
        item->setData(1, kGameUiChoiceRole, value);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        item->setToolTip(1, link.key.isEmpty() ? shown : tr("%1\n事件名 %2").arg(shown, link.key));
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
    if (key == "origin:visible") return {{tr("原样"), ""}, {tr("显示"), "true"}, {tr("隐藏"), "false"}};
    if (key == "origin:point" || key == "origin:toPoint") {
        auto result = PointOptions(false); result.prepend({tr("原样"), ""}); return result;
    }
    const bool raw = rawFields_ && rawFields_->isChecked();
    if (key == QLatin1String("unit")) return {{tr("魔兽单位"), QStringLiteral("war3")}, {tr("像素"), QStringLiteral("pixel")}};
    if (key == QLatin1String("resolution")) {
        GameUiOptions options;
        for (const char* size : {"1920×1080", "2560×1440", "3840×2160", "1600×900", "1366×768", "1280×720", "1280×1024", "1024×768"}) {
            options.push_back({QString::fromUtf8(size), QString::fromUtf8(size)});
        }
        return options;
    }
    if (key == QLatin1String("fill")) return {{tr("是"), QStringLiteral("true")}, {tr("否"), QStringLiteral("false")}};
    if (key.startsWith(QLatin1String("event:"))) {
        GameUiOptions options{{tr("（无）"), QString()}, {tr("新建触发器"), QStringLiteral("new")}};
        for (const auto& [trigger, name] : EventTriggerCandidates()) options.push_back({name, QStringLiteral("trigger:%1").arg(trigger)});
        return options;
    }
    QString id;
    const UiFrame* frame = SelectedDescriptionFrame(&id);
    const UiDescription* description = frame ? Description(id) : nullptr;
    if (!description) return {};
    if (key == QLatin1String("parent")) {
        // 界面顶层与同一界面里自身以外的控件（不含自己的子控件，免得成环）。
        GameUiOptions options{{tr("（界面顶层）"), QString()}};
        std::set<std::string> own;
        CollectIds(*frame, own);
        std::vector<const UiFrame*> frames;
        CollectFrames(description->frames, frames);
        for (const UiFrame* other : frames) {
            if (own.contains(other->id)) continue;
            options.push_back({raw || other->label.empty() ? Text(other->id) : QStringLiteral("%1  %2").arg(Text(other->label), Text(other->id)),
                Text(other->id)});
        }
        return options;
    }
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
        // 父控件、屏幕、同一界面里自身以外的控件（不含自己的子控件，免得成环）、游戏界面区域。
        GameUiOptions options{{raw ? QStringLiteral("parent") : tr("父控件"), QStringLiteral("parent")},
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
    // 参考分辨率可写列表外的 宽×高；父控件与事件的触发器可输入筛选。
    return key == QLatin1String("visible") || key == QLatin1String("value") || key == QLatin1String("template")
        || key == QLatin1String("resolution") || key == QLatin1String("parent") || key.startsWith(QLatin1String("event:"));
}

void GameUiModule::OnPropertyEdited(QTreeWidgetItem* item, int column) {
    if (column != 1 || !item) return;
    if (OnOriginPropertyEdited(item)) return;
    const QString key = item->data(1, kFieldRole).toString();
    QString id;
    const UiFrame* frame = key.isEmpty() ? nullptr : SelectedDescriptionFrame(&id);
    if (!frame) return;
    const QVariant chosen = item->data(1, kGameUiChoiceRole);
    const QString text = (chosen.isValid() ? chosen.toString() : item->text(1)).trimmed();
    const QString frameId = Text(frame->id);
    // 修改会重建属性树（删掉正在发出信号的行），放到下一轮事件里；失败时报告并恢复成描述里的值。
    const auto later = [this](std::function<bool(QString*)> apply) {
        QTimer::singleShot(0, this, [this, apply = std::move(apply)] {
            QString error;
            if (apply(&error)) return;
            if (!error.isEmpty()) ReportAuthoring(error);
            UpdateProperties();
        });
    };
    if (key == QLatin1String("unit")) {
        QTimer::singleShot(0, this, [this, pixels = text == QLatin1String("pixel")] { SetPixelUnits(pixels); });
        return;
    }
    if (key == QLatin1String("resolution")) {
        const QStringList parts = QString(text).replace(QChar(0x00D7), QLatin1Char('x')).replace(QLatin1Char('*'), QLatin1Char('x'))
                                      .split(QLatin1Char('x'), Qt::SkipEmptyParts);
        bool okWidth = false;
        bool okHeight = false;
        const UiResolution resolution{parts.size() == 2 ? parts[0].trimmed().toInt(&okWidth) : 0,
            parts.size() == 2 ? parts[1].trimmed().toInt(&okHeight) : 0};
        if (!okWidth || !okHeight || !resolution.Valid()) {
            ReportAuthoring(tr("参考分辨率写作 宽×高，例如 1920×1080"));
            QTimer::singleShot(0, this, &GameUiModule::UpdateProperties);
            return;
        }
        QTimer::singleShot(0, this, [this, resolution] { SetReferenceResolution(resolution); });
        return;
    }
    if (key == QLatin1String("parent")) {
        later([this, id, frameId, text](QString* error) {
            if (!ReparentDescriptionFrame(id, frameId, text, error)) return false;
            SelectFrameWhenReady(frameId);
            return true;
        });
        return;
    }
    if (key == QLatin1String("fill")) {
        later([this, id, frameId, fill = text == QLatin1String("true")](QString* error) {
            return SetDescriptionFrameFill(id, frameId, fill, error);
        });
        return;
    }
    if (key.startsWith(QLatin1String("event:"))) {
        const QString on = key.section(QLatin1Char(':'), 1);
        const UiEventKind kind = on == QLatin1String("enter") ? UiEventKind::Enter : on == QLatin1String("leave") ? UiEventKind::Leave : UiEventKind::Click;
        std::optional<StableId> trigger = StableId{0};
        if (text == QLatin1String("new")) {
            trigger = std::nullopt;
        } else if (text.startsWith(QLatin1String("trigger:"))) {
            trigger = text.section(QLatin1Char(':'), 1).toULongLong();
        } else if (!text.isEmpty() && text != tr("（无）")) {
            // 列表外的输入：按触发器名字找。
            const auto candidates = EventTriggerCandidates();
            const auto found = std::find_if(candidates.begin(), candidates.end(), [&](const auto& row) { return row.second == text; });
            if (found == candidates.end()) {
                ReportAuthoring(tr("没有可订阅界面事件的触发器 %1").arg(text));
                QTimer::singleShot(0, this, &GameUiModule::UpdateProperties);
                return;
            }
            trigger = found->first;
        }
        later([this, id, frameId, kind, trigger](QString* error) {
            const auto linked = LinkFrameEvent(id, frameId, kind, trigger, error);
            if (!linked) return false;
            if (*linked != 0) OfferMapRuntime();
            // 新建的触发器：跳过去写动作。
            if (!trigger && *linked != 0 && host_) host_->Navigate(Shell::NavigationTarget{"triggers", "trigger", QString::number(*linked), {}});
            return true;
        });
        return;
    }
    const QJsonObject current = FrameObject(*frame);
    QString error;
    QJsonObject fields;
    // 长度按当前单位换算，带 px 时按像素。
    const auto parseLength = [&](const QString& what, bool horizontal, double* out) {
        const auto value = ParseLength(text, horizontal);
        if (!value) error = tr("%1 应为数值（像素可写 64px）").arg(what);
        else *out = *value;
        return value.has_value();
    };
    if (key == QLatin1String("width") || key == QLatin1String("height") || key == QLatin1String("fontHeight")) {
        double number = 0.0;
        if (text.isEmpty()) fields.insert(key, QJsonValue::Null);
        else if (parseLength(item->text(0), key == QLatin1String("width"), &number)) fields.insert(key, number);
    } else if (key == QLatin1String("alpha")) {
        QString percent = text;
        if (percent.endsWith(QLatin1Char('%'))) percent.chop(1);
        bool ok = false;
        const double value = percent.trimmed().toDouble(&ok);
        if (text.isEmpty()) fields.insert(key, QJsonValue::Null);
        else if (!ok || value < 0.0 || value > 100.0) error = tr("透明度写 0 到 100 的百分数");
        else fields.insert(key, std::round(value * 10.0) / 1000.0);
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
                error = tr("阵列数量应为正整数");
            } else {
                repeat.insert(QStringLiteral("count"), count);
                fields.insert(QStringLiteral("repeat"), repeat);
            }
        } else if (repeat.isEmpty()) {
            error = tr("先填写阵列数量");
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
            if (parseLength(item->text(0), part == QLatin1String("dx") || part == QLatin1String("rowDx"), &number)) {
                repeat.insert(part, number);
                fields.insert(QStringLiteral("repeat"), repeat);
            }
        }
    } else if (key.startsWith(QLatin1String("anchor:"))) {
        QJsonArray anchors = current.value(QStringLiteral("anchors")).toArray();
        const qsizetype index = key.section(QLatin1Char(':'), 1, 1).toInt();
        const QString part = key.section(QLatin1Char(':'), 2);
        // 没有锚点时“对齐点”一行新建第一个锚点。
        if (index == anchors.size() && part == QLatin1String("point") && !text.isEmpty()) anchors.append(QJsonObject{{"point", text}});
        if (index < 0 || index >= anchors.size()) return;
        QJsonObject anchor = anchors[index].toObject();
        double number = 0.0;
        if (part == QLatin1String("x") || part == QLatin1String("y")) {
            if (parseLength(item->text(0), part == QLatin1String("x"), &number)) anchor.insert(part, number);
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
    const QJsonObject edit{{"op", "set_frame"}, {"frame", frameId}, {"fields", fields}};
    // 改编号或阵列数量后原来的节点名不在了：仍选中这个控件（阵列时选第一个）。
    const QString target = key == QLatin1String("id") ? text : frameId;
    auto after = [this, target] {
        if (const UiFrame* selected = SelectedDescriptionFrame(); selected && Text(selected->id) == target) return;
        SelectFrameWhenReady(target);
    };
    CommitDescriptionEdits(id, {edit}, tr("界面 %1：改 %2 的 %3").arg(id, frameId, item->text(0)), std::move(after));
}

bool GameUiModule::AddDescriptionAnchor(QString* error) {
    QString id;
    const UiFrame* frame = SelectedDescriptionFrame(&id);
    if (!frame) {
        if (error) *error = tr("先选中一个自己界面里的控件");
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

void GameUiModule::MarkPropertyGroup(QTreeWidgetItem* group, const QString& key, bool expandedByDefault) {
    group->setData(0, kGroupRole, key);
    group->setExpanded(groupExpanded_.value(key, expandedByDefault));
}

void GameUiModule::InstallPropertyEditors(QTreeWidget* tree) {
    const auto key = [](const QModelIndex& index) { return index.data(kFieldRole).toString(); };
    // 分组的展开、折叠按组记住，换选控件后照旧（填表时信号已屏蔽，不算作者的操作）。
    const auto remember = [this](QTreeWidgetItem* item, bool expanded) {
        const QString group = item ? item->data(0, kGroupRole).toString() : QString();
        if (!group.isEmpty()) groupExpanded_.insert(group, expanded);
    };
    connect(tree, &QTreeWidget::itemExpanded, this, [remember](QTreeWidgetItem* item) { remember(item, true); });
    connect(tree, &QTreeWidget::itemCollapsed, this, [remember](QTreeWidgetItem* item) { remember(item, false); });
    tree->setItemDelegateForColumn(1, new GameUiChoiceDelegate(
        [this, key](const QModelIndex& index) { return PropertyChoices(key(index)); }, tree,
        [this, key](const QModelIndex& index) { return PropertyFreeText(key(index)); }));
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree, &QTreeWidget::customContextMenuRequested, this, [this, tree](const QPoint& position) { ShowPropertyMenu(tree, position); });
    connect(tree, &QTreeWidget::itemChanged, this, &GameUiModule::OnPropertyEdited);
    connect(tree, &QTreeWidget::itemDoubleClicked, this, [this, tree](QTreeWidgetItem* item, int) {
        if (item && item->data(1, kResourceRole).toInt() > 0) OpenResourcePicker(tree, item);
        else if (item && item->data(1, kColorRole).toBool()) PickColor(tree, item);
    });
}

void GameUiModule::ShowPropertyMenu(QTreeWidget* tree, const QPoint& position) {
    if (!tree || !SelectedDescriptionFrame()) return;
    QMenu menu(tree);
    QTreeWidgetItem* item = tree->itemAt(position);
    if (item && item->data(1, kResourceRole).toInt() > 0) {
        const bool model = item->data(1, kResourceRole).toInt() == static_cast<int>(Services::ObjectResourceKind::Model) + 1;
        menu.addAction(model ? tr("选择模型…") : tr("选择贴图…"), this, [this, tree, item] { OpenResourcePicker(tree, item); });
        menu.addSeparator();
    } else if (item && item->data(1, kColorRole).toBool()) {
        menu.addAction(tr("选择颜色…"), this, [tree, item] { PickColor(tree, item); });
        menu.addSeparator();
    } else if (const QString key = item ? item->data(1, kFieldRole).toString() : QString(); key.startsWith(QLatin1String("event:"))) {
        // 事件：打开订阅它的触发器。
        QString id;
        const UiFrame* frame = SelectedDescriptionFrame(&id);
        const QString on = key.section(QLatin1Char(':'), 1);
        const UiEventKind kind = on == QLatin1String("enter") ? UiEventKind::Enter : on == QLatin1String("leave") ? UiEventKind::Leave : UiEventKind::Click;
        const FrameEventLink link = frame ? FrameEventTriggers(id, Text(frame->id), kind) : FrameEventLink{};
        for (const auto& [trigger, name] : link.triggers) {
            menu.addAction(tr("打开触发器 %1").arg(name), this, [this, trigger] {
                if (host_) host_->Navigate(Shell::NavigationTarget{"triggers", "trigger", QString::number(trigger), {}});
            });
        }
        if (!link.triggers.isEmpty()) menu.addSeparator();
    } else if (item && item->data(1, kFieldRole).toString() == QLatin1String("text")) {
        // 插入取值：把本界面的一个数值写成 {编号}（数组为 {编号[]}，按所在重复实例取格）接在文字后面。
        QString id;
        const UiFrame* frame = SelectedDescriptionFrame(&id);
        const UiDescription* description = frame ? Description(id) : nullptr;
        QMenu* insert = menu.addMenu(tr("插入取值"));
        for (const auto& value : description ? description->values : std::vector<UiValue>{}) {
            const QString reference = Text(value.id) + (value.size > 0 ? QStringLiteral("[]") : QString());
            const QString title = value.label.empty() ? reference : QStringLiteral("%1  %2").arg(Text(value.label), reference);
            insert->addAction(title, this, [item, reference] { item->setText(1, item->text(1) + QLatin1Char('{') + reference + QLatin1Char('}')); });
        }
        insert->setEnabled(!insert->isEmpty());
        menu.addSeparator();
    }
    menu.addAction(tr("添加锚点"), this, [this] {
        QString error;
        if (!AddDescriptionAnchor(&error)) ReportAuthoring(error);
    });
    // 点在锚点组或其下的行上时可删该锚点。
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
    menu.exec(tree->viewport()->mapToGlobal(position));
}

// 贴图、模型选择：物编的资源选择器，打开即按目录浏览游戏文件。与物编字段同一套表格浮层定位：
// 贴所在行、受屏幕工作区约束，Esc、点到别处、换行或表格重建时关闭；commit 返回假时选择器不关。
void GameUiModule::ShowResourcePicker(Services::ObjectResourceKind kind, const QString& current, QAbstractItemView* view,
    const QModelIndex& index, std::function<bool(const QString&)> commit) {
    if (!resourcePickerFactory_ || !view || !index.isValid()) return;
    auto* picker = new ObjectResourcePicker(Services::ObjectResourceField{kind, false, true}, current, resourcePickerFactory_(), view);
    picker->setObjectName(kind == Services::ObjectResourceKind::Model ? QStringLiteral("gameUiModelPicker") : QStringLiteral("gameUiTexturePicker"));
    picker->BrowseGameFiles();
    picker->SetCommit(std::move(commit));
    picker->show();
    UiKit::AnchorTablePopup(picker, view, index);
    picker->raise();
}

// 属性表里的贴图、模型：应用时作为一次框架修改写入。
void GameUiModule::OpenResourcePicker(QTreeWidget* tree, QTreeWidgetItem* item) {
    QString id;
    const UiFrame* frame = SelectedDescriptionFrame(&id);
    if (!tree || !item || !frame) return;
    const QString key = item->data(1, kFieldRole).toString();
    const QString frameId = Text(frame->id);
    const QString label = item->text(0);
    const auto kind = static_cast<Services::ObjectResourceKind>(item->data(1, kResourceRole).toInt() - 1);
    const QPointer<GameUiModule> self(this);
    ShowResourcePicker(kind, item->text(1), tree, tree->indexFromItem(item, 1), [self, id, frameId, key, label](const QString& value) {
            if (!self) return false;
            const QString path = value.trimmed();
            const QJsonObject edit{{"op", "set_frame"}, {"frame", frameId},
                {"fields", QJsonObject{{key, path.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(path)}}}};
            QString error;
            if (self->EditDescription(id, CompactEdits({edit}), tr("界面 %1：改 %2 的 %3").arg(id, frameId, label), &error)) return true;
            self->ReportAuthoring(error);
            return false;
        });
}

QPointer<QWidget> GameUiModule::OpenFrameSettings(std::uint32_t node, const QPoint& global) {
    if (!tree_ || !page_ || node >= tree_->Nodes().size() || !tree_->Nodes()[node].uiFrame || tree_->Nodes()[node].uiFrame->frame.empty()) {
        return nullptr;
    }
    SelectNode(node);
    if (!settingsCard_) {
        auto* card = new QFrame(page_, Qt::Tool | Qt::FramelessWindowHint);
        card->setObjectName(QStringLiteral("gameUiFrameSettings"));
        card->setAttribute(Qt::WA_DeleteOnClose);
        card->setFrameShape(QFrame::StyledPanel);
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(8, 6, 8, 8);
        layout->setSpacing(6);
        auto* header = new QHBoxLayout;
        auto* title = new QLabel(card);
        title->setObjectName(QStringLiteral("gameUiFrameSettingsTitle"));
        QFont bold = title->font();
        bold.setBold(true);
        title->setFont(bold);
        auto* close = new QToolButton(card);
        close->setText(QStringLiteral("×"));
        close->setAutoRaise(true);
        connect(close, &QToolButton::clicked, card, &QWidget::close);
        header->addWidget(title, 1);
        header->addWidget(close);
        layout->addLayout(header);
        auto* fields = new QTreeWidget(card);
        fields->setObjectName(QStringLiteral("gameUiFrameSettingsFields"));
        fields->setColumnCount(2);
        fields->setHeaderLabels({tr("属性"), tr("值")});
        fields->header()->setSectionResizeMode(QHeaderView::Interactive);
        fields->header()->resizeSection(0, 110);
        fields->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
        layout->addWidget(fields, 1);
        InstallPropertyEditors(fields);
        auto* escape = new QShortcut(QKeySequence(Qt::Key_Escape), card);
        connect(escape, &QShortcut::activated, card, &QWidget::close);
        card->resize(360, 440);
        settingsCard_ = card;
        settingsFields_ = fields;
        settingsTitle_ = title;
    }
    RefreshFrameSettings();
    if (!settingsCard_) return nullptr;
    // 放在右键位置右侧，受屏幕工作区约束。
    QPoint origin = global + QPoint(12, 0);
    if (const QScreen* screen = QGuiApplication::screenAt(global)) {
        const QRect available = screen->availableGeometry();
        const QSize size = settingsCard_->size();
        origin.setX(std::clamp(origin.x(), available.left(), std::max(available.left(), available.right() - size.width())));
        origin.setY(std::clamp(origin.y(), available.top(), std::max(available.top(), available.bottom() - size.height())));
    }
    settingsCard_->move(origin);
    settingsCard_->show();
    settingsCard_->raise();
    return settingsCard_.data();
}

// 卡片跟随选择：换选另一个描述框架时显示它，选中的不是描述框架时关闭。
void GameUiModule::RefreshFrameSettings() {
    if (!settingsCard_ || !settingsFields_ || !settingsTitle_) return;
    QString id;
    const UiFrame* frame = SelectedDescriptionFrame(&id);
    if (!frame) {
        settingsCard_->close();
        return;
    }
    QString kind = Text(UiFrameTypeName(frame->type));
    for (const auto& candidate : UiFrameKinds()) {
        if (candidate.type != frame->type) continue;
        kind = Text(candidate.label);
        break;
    }
    const QString name = frame->label.empty() ? Text(frame->id) : QStringLiteral("%1 · %2").arg(Text(frame->label), Text(frame->id));
    settingsTitle_->setText(QStringLiteral("%1  %2").arg(kind, name));
    const QSignalBlocker block(settingsFields_.data());  // 填表不算编辑
    settingsFields_->clear();
    AddEditableRows(settingsFields_->invisibleRootItem(), *frame);
}

} // namespace War3Map::Modules
