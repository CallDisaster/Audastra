#include "EditorProject.h"
#include "TriggerModule.h"
#include "TriggerDialogs.h"
#include "TriggerValuePanel.h"
#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"
#include "Icons/SvgIcons.h"

#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerEditing.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"

#include <QApplication>
#include <QAbstractAnimation>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QComboBox>
#include <QDir>
#include <QFontMetrics>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QScreen>
#include <QScopeGuard>
#include <QSettings>
#include <QStyle>
#include <QLineEdit>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QUndoStack>

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>

// 取值面板对 YDWE 函数的支持：局部变量（loc_）、Null 跟随 typename、AnyReturnType、scriptcode，
// 以及把函数提供的参数换成其他来源（先摘除原函数，一次撤销还原）。夹具为原创合成数据。
using namespace War3Map;

namespace {

class Bytes {
public:
    void I32(std::int32_t value) {
        for (int shift = 0; shift < 32; shift += 8) data.push_back(static_cast<std::byte>((static_cast<std::uint32_t>(value) >> shift) & 0xffU));
    }
    void Text(std::string_view value) {
        for (const char c : value) data.push_back(static_cast<std::byte>(c));
        data.push_back(std::byte{0});
    }
    void Scalar(std::int32_t type, std::string_view value) {
        I32(type); Text(value); I32(0); I32(0);
    }
    std::vector<std::byte> data;
};

void WriteUtf8(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

// 触发器 Fx：
//   0 [特效] loc_fx = 最后创建的特效
//   1 删除 loc_fx
//   2 注释 hello
std::vector<std::byte> Wtg() {
    Bytes w;
    for (const char c : std::string_view("WTG!")) w.data.push_back(static_cast<std::byte>(c));
    w.I32(7);
    w.I32(1);
    w.I32(1); w.Text("Main"); w.I32(0);
    w.I32(2);
    w.I32(0);
    w.I32(1);
    w.Text("Fx"); w.Text(""); w.I32(0); w.I32(1); w.I32(0); w.I32(0); w.I32(0); w.I32(1);
    w.I32(3);
    const auto call = [&w](std::string_view name, const auto& arguments) {
        w.I32(2); w.Text(""); w.I32(1);
        w.I32(3); w.Text(name); w.I32(1);
        arguments();
        w.I32(0);
        w.I32(0);
    };
    w.I32(2); w.Text("YDWESetAnyTypeLocalVariable"); w.I32(1);
    w.Scalar(0, "typename20_effect"); w.Scalar(3, "fx");
    call("GetLastCreatedEffectBJ", []() {});
    w.I32(0);
    w.I32(2); w.Text("DestroyEffect"); w.I32(1);
    call("YDWEGetAnyTypeLocalVariable", [&w]() { w.Scalar(3, "fx"); });
    w.I32(0);
    w.I32(2); w.Text("CommentString"); w.I32(1);
    w.Scalar(3, "hello");
    w.I32(0);
    return std::move(w.data);
}

std::vector<std::byte> Wct() {
    Bytes w;
    w.I32(1); w.Text(""); w.I32(0); w.I32(1); w.I32(0);
    return std::move(w.data);
}

// 原创成员经真实 MPQ、native 和放置编解码器进入 Capture，绝不伪造密封引用名单。
Status MakePreplacedProject(QtProject::EditorProject& project, const std::filesystem::path& root) {
    using namespace ObjectData;
    using Editor = Trigger::TriggerDocumentEditor;
    project.war3GameRoot_.clear();
    project.projectRoot_ = root;
    project.sourceArchivePath_ = root / "preplaced.w3x";
    std::vector<Mpq::MpqBuildFile> files;
    for (const auto& [type, rawcode] : {std::pair{ObjectType::Unit, "hfoo"}, std::pair{ObjectType::Item, "ratf"}}) {
        auto native = NativeObjectDocument::CreateEmpty(type);
        if (!native.Succeeded()) return {native.diagnostics};
        const auto added = native.value->AddOriginalObject(rawcode);
        if (!added.Succeeded()) return added;
        auto bytes = NativeObjectCodec(type).Serialize(*native.value);
        if (!bytes.Succeeded()) return {bytes.diagnostics};
        auto parsed = NativeObjectCodec(type).Parse(*bytes.value);
        if (!parsed.Succeeded()) return {parsed.diagnostics};
        const auto index = static_cast<std::size_t>(type);
        project.nativeObjectDocuments_[index] = std::make_unique<NativeObjectDocument>(
            *dynamic_cast<NativeObjectDocument*>(parsed.document.get()));
        auto converted = ConvertNativeObjectDataToLni(type, *bytes.value);
        if (!converted.Succeeded()) return {converted.diagnostics};
        auto lni = LniObjectCodec(type).Parse(*converted.value);
        if (!lni.Succeeded()) return {lni.diagnostics};
        project.objectDocuments_[index] = std::make_unique<LniObjectDocument>(
            *dynamic_cast<LniObjectDocument*>(lni.document.get()));
        files.push_back({std::string(NativeFileName(type)), std::move(*bytes.value), false});
    }
    MapData::UnitPlacements units;
    for (const auto& [rawcode, creation] : {std::pair{"hfoo", 7}, std::pair{"ratf", 8}, std::pair{"u001", 9}}) {
        MapData::UnitPlacement unit;
        unit.rawcode = rawcode;
        unit.scale = {1.f, 1.f, 1.f};
        unit.flags = 2;
        unit.creationId = creation;
        units.units.push_back(std::move(unit));
    }
    auto encoded = units.Serialize();
    if (!encoded.Succeeded()) return {encoded.diagnostics};
    files.push_back({"war3mapUnits.doo", std::move(*encoded.value), false});
    const auto written = Mpq::WriteMpqArchive(project.sourceArchivePath_, files);
    if (!written.Succeeded()) return written;
    auto archive = MapArchiveSession::Open(project.sourceArchivePath_);
    if (!archive.Succeeded()) return {archive.diagnostics};
    project.archiveSession_ = std::make_unique<MapArchiveSession>(std::move(*archive.value));
    project.mapDocument_ = std::make_unique<MapDocument>(MapDocument::FromArchive(*project.archiveSession_));
    project.mapEdits_ = std::make_unique<MapData::MapDataEditSession>(*project.mapDocument_);

    WriteUtf8(root / "ui/config", "base\n");
    WriteUtf8(root / "ui/base/define.txt",
        "[TriggerTypes]\nunit=0,1,1,单位\nitem=0,1,1,物品\nwidget=0,0,0,实体\nhandle=0,0,0,句柄\n"
        "destructable=0,1,1,可破坏物\ninteger=0,1,1,整数\nAnyGlobal=0,0,0,变量\nNull=0,0,0,值\n");
    WriteUtf8(root / "ui/base/action.txt",
        "[ReadObjects]\ndescription=\"${单位} ${物品} ${实体} ${句柄} ${可破坏物}\"\n"
        "[[.args]]\ntype=unit\n[[.args]]\ntype=item\n[[.args]]\ntype=widget\n[[.args]]\ntype=handle\n[[.args]]\ntype=destructable\n"
        "[SetVariable]\ndescription=\"设置 ${变量} 为 ${值}\"\n[[.args]]\ntype=AnyGlobal\n[[.args]]\ntype=Null\n");
    auto catalog = Trigger::UiCatalog::Load(root);
    if (!catalog.Succeeded()) return {catalog.diagnostics};
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    auto empty = Trigger::WtgWctCodec{}.CreateEmpty(*project.uiCatalog_);
    if (!empty.Succeeded()) return {empty.diagnostics};
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*empty.value));
    auto& document = *project.triggerDocument_;
    const auto category = Trigger::TriggerDirectoryEditor::AddCategory(document, "Main");
    if (!category.Succeeded()) return {category.diagnostics};
    const auto trigger = Editor::AddTrigger(document, *category.value, "ReadPlaced");
    if (!trigger.Succeeded()) return {trigger.diagnostics};
    for (const auto& [name, type] : {std::pair{"PickedUnit", "unit"}, std::pair{"PickedItem", "item"},
             std::pair{"PickedWidget", "widget"}, std::pair{"PickedHandle", "handle"}, std::pair{"PickedDest", "destructable"}}) {
        const auto added = Editor::AddVariable(document, name, type);
        if (!added.Succeeded()) return added;
    }
    for (const auto name : {"ReadObjects", "SetVariable"}) {
        const auto appended = Editor::AppendRootEca(document, *project.uiCatalog_, *trigger.value,
            Trigger::UiDefinitionKind::Action, name);
        if (!appended.Succeeded()) return {appended.diagnostics};
        const auto* eca = Editor::FindEca(document, appended.value->rootEcaId);
        const std::array variables{"PickedUnit", "PickedItem", "PickedWidget", "PickedHandle", "PickedDest"};
        for (std::size_t i = 0; i < eca->arguments.size(); ++i) {
            const auto changed = Editor::SetScalarArgument(document, document.argumentNodes[eca->arguments[i]].id,
                1, std::string_view(name) == "SetVariable" ? "PickedUnit" : variables[i]);
            if (!changed.Succeeded()) return changed;
        }
    }
    project.triggerUndoStack_ = new QUndoStack(&project);
    return Status::Ok();
}

Trigger::EcaNode& RootEca(QtProject::EditorProject& project, std::size_t row) {
    auto& document = *project.triggerDocument_;
    return document.ecaNodes[document.triggers.front().ecas[row]];
}

Trigger::ArgumentNode& RootArgument(QtProject::EditorProject& project, std::size_t row, std::size_t position) {
    return project.triggerDocument_->argumentNodes[RootEca(project, row).arguments[position]];
}

QListWidget* Variables(TriggerValuePanel* panel) {
    auto* tabs = panel->findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabText(i) == QStringLiteral("变量")) tabs->setCurrentIndex(i);
    }
    return panel->findChild<QListWidget*>(QStringLiteral("valuePanelList"));
}

int FindValue(QListWidget* list, const QString& symbol) {
    for (int row = 0; row < list->count(); ++row) {
        if (list->item(row)->data(Qt::UserRole + 2).toString() == symbol) return row;
    }
    return -1;
}

} // namespace

class TriggerValueTests final : public QObject {
    Q_OBJECT
private slots:
    void CurrentValueStaysSeparateFromChoicesAndNotes();
    void AppliedVariableAndLiteralCurrentCardsPreserveDocument();
    void YdweLocalsAndReplacingCalls();
    void PreplacedReferencesUseCaptureTypesAndUndo();
    void PreplacedCaptureUsesCurrentOverlays();
    void MissingReferenceSourcesKeepOrdinaryParametersEditable();
};

void TriggerValueTests::CurrentValueStaysSeparateFromChoicesAndNotes() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui" / "config", "base\n");
    WriteUtf8(ui / "ui" / "base" / "define.txt", "[TriggerTypes]\ninteger=0,1,1,整数\n");
    WriteUtf8(ui / "ui" / "base" / "action.txt",
        "[UseInteger]\ndescription=\"使用 ${null}\"\n[[.args]]\ntype=integer\n");
    WriteUtf8(ui / "ui" / "base" / "call.txt",
        "[GetRandomInt]\ntitle=\"随机整数\"\ndescription=\"随机整数 ${最小值} 至 ${最大值}\"\nreturns=integer\n"
        "[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n");
    const auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    Trigger::TriggerDocument document;
    Trigger::EcaNode action;
    action.id = 1; action.sourceTriggerId = 3; action.type = 2; action.name.text = "UseInteger"; action.arguments = {0};
    Trigger::EcaNode call;
    call.id = 2; call.sourceTriggerId = 3; call.type = 3; call.name.text = "GetRandomInt"; call.arguments = {1, 2};
    document.ecaNodes = {action, call};
    Trigger::ArgumentNode value;
    value.id = 10; value.type = 2; value.insertCall = 1; value.callEca = 1; value.value.text = "GetRandomInt";
    Trigger::ArgumentNode low;
    low.id = 11; low.type = 3; low.value.text = "1";
    Trigger::ArgumentNode high;
    high.id = 12; high.type = 3; high.value.text = "100";
    document.argumentNodes = {value, low, high};
    Trigger::VariableRecord variable;
    variable.id = 20; variable.name.text = "测试整数"; variable.typeName.text = "integer";
    document.variables.push_back(variable);
    TriggerValuePanel panel(document, *catalog.value, 1, 0, {});
    panel.setAttribute(Qt::WA_DeleteOnClose, false);
    auto* card = panel.findChild<QFrame*>(QStringLiteral("valuePanelCurrentCard"));
    auto* summary = panel.findChild<QLabel*>(QStringLiteral("valuePanelCurrentSummary"));
    auto* currentIcon = panel.findChild<QLabel*>(QStringLiteral("valuePanelCurrentIcon"));
    auto* list = panel.findChild<QListWidget*>(QStringLiteral("valuePanelList"));
    auto* note = panel.findChild<QLabel*>(QStringLiteral("valuePanelNote"));
    auto* tabs = panel.findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
    auto* search = panel.findChild<QLineEdit*>(QStringLiteral("valuePanelSearch"));
    auto* edit = panel.findChild<QPushButton*>(QStringLiteral("valuePanelEditCall"));
    QVERIFY(card && summary && currentIcon && list && note && tabs && search && edit);
    QVERIFY(!currentIcon->pixmap().isNull()); // 无类别图标的真实空回调也必须有后备图标。
    // 三块都直接属于 panel；明确同一坐标系，不能用换坐标掩盖真实重叠。
    QCOMPARE(card->parentWidget(), static_cast<QWidget*>(&panel));
    QCOMPARE(list->parentWidget(), static_cast<QWidget*>(&panel));
    QCOMPARE(note->parentWidget(), static_cast<QWidget*>(&panel));
    bool editRequested = false;
    panel.SetEditCall([&](StableId id) { editRequested = id == 2; });
    const QString capture = qEnvironmentVariable("FORGE_TRIGGER_VALUE_CAPTURE_DIR");
    const auto captureLayout = [&](QSize requested, int page, bool withNote) {
        if (capture.isEmpty()) return true;
        if (!QDir().mkpath(capture)) return false;
        const QString stem = QStringLiteral("value-%1x%2-page-%3%4").arg(requested.width()).arg(requested.height())
            .arg(page).arg(withNote ? QStringLiteral("-note") : QString());
        const auto rect = [](QRect area) { return QJsonObject{{"x", area.x()}, {"y", area.y()},
            {"width", area.width()}, {"height", area.height()}}; };
        QJsonObject facts{{"fixture_only", true}, {"platform", QGuiApplication::platformName()},
            {"requested_size", rect(QRect(QPoint{}, requested))}, {"actual_size", rect(panel.rect())},
            {"actual_minimum_size", rect(QRect(QPoint{}, panel.minimumSize()))},
            {"current_icon_non_null", !currentIcon->pixmap().isNull()},
            {"card", rect(card->geometry())}, {"list", rect(list->geometry())},
            {"note", rect(note->geometry())}, {"note_visible", note->isVisible()}, {"dpr", panel.devicePixelRatioF()}};
        if (panel.screen()) facts.insert("available_screen", rect(panel.screen()->availableGeometry()));
        if (!panel.grab().save(QDir(capture).filePath(stem + QStringLiteral(".png")))) return false;
        QFile receipt(QDir(capture).filePath(stem + QStringLiteral(".json")));
        const auto bytes = QJsonDocument(facts).toJson(QJsonDocument::Indented);
        return receipt.open(QIODevice::WriteOnly) && receipt.write(bytes) == bytes.size();
    };
    for (const QSize size : {QSize(420, 320), QSize(540, 500)}) {
        panel.resize(size);
        panel.show();
        QCoreApplication::processEvents();
        QVERIFY(panel.width() >= size.width());
        for (int page = 0; page < tabs->count(); ++page) {
            tabs->setCurrentIndex(page);
            search->clear();
            QCoreApplication::processEvents();
            QVERIFY(card->isVisible());
            QVERIFY(list->isVisible());
            QVERIFY(!card->geometry().intersects(list->geometry()));
            QVERIFY(card->geometry().bottom() < list->geometry().top());
            QVERIFY(!note->isVisible());
            QVERIFY(summary->toolTip().contains(QStringLiteral("1")));
            QVERIFY(summary->toolTip().contains(QStringLiteral("100")));
            QVERIFY(captureLayout(size, page, false));
            search->setText(QStringLiteral("不存在的值"));
            QCoreApplication::processEvents();
            if (note->isVisible()) {
                const QString geometry = QStringLiteral("panel=%1x%2 list=(%3,%4,%5,%6) note=(%7,%8,%9,%10)")
                    .arg(panel.width()).arg(panel.height()).arg(list->x()).arg(list->y()).arg(list->width()).arg(list->height())
                    .arg(note->x()).arg(note->y()).arg(note->width()).arg(note->height());
                QVERIFY2(!note->geometry().intersects(list->geometry()), qPrintable(geometry));
                QVERIFY(note->geometry().top() > list->geometry().bottom());
                QVERIFY(panel.rect().contains(note->geometry()));
                QVERIFY(captureLayout(size, page, true));
            }
            search->clear();
        }
        QVERIFY(edit->isVisible());
        const QRect editInCard(edit->mapTo(card, QPoint{}), edit->size());
        QVERIFY(card->rect().contains(editInCard));
    }
    QTest::mouseClick(edit, Qt::LeftButton);
    QVERIFY(editRequested);
    panel.hide();
    document.argumentNodes[0].callEca.reset();
    document.argumentNodes[0].insertCall = 0;
    document.argumentNodes[0].value.text.clear();
    TriggerValuePanel unset(document, *catalog.value, 1, 0, {});
    unset.setAttribute(Qt::WA_DeleteOnClose, false);
    auto* title = unset.findChild<QLabel*>(QStringLiteral("valuePanelTitle"));
    QVERIFY(title);
    QCOMPARE(title->text(), QStringLiteral("未设置 · 整数"));
}

void TriggerValueTests::AppliedVariableAndLiteralCurrentCardsPreserveDocument() {
    using Editor = Trigger::TriggerDocumentEditor;
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui/config", "base\n");
    WriteUtf8(ui / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,整数\n");
    WriteUtf8(ui / "ui/base/action.txt", "[UseInteger]\ndescription=\"使用 ${整数}\"\n[[.args]]\ntype=integer\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    auto created = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value);
    QVERIFY(created.Succeeded());
    auto& seed = *created.value;
    const auto category = Trigger::TriggerDirectoryEditor::AddCategory(seed, "Main");
    QVERIFY(category.Succeeded());
    const auto trigger = Editor::AddTrigger(seed, *category.value, "CurrentValueEvidence");
    QVERIFY(trigger.Succeeded());
    QVERIFY(Editor::AddVariable(seed, "当前整数", "integer").Succeeded());
    const auto appended = Editor::AppendRootEca(seed, *catalog.value, *trigger.value, Trigger::UiDefinitionKind::Action, "UseInteger");
    QVERIFY(appended.Succeeded());
    const auto ecaId = appended.value->rootEcaId;
    const auto argumentId = seed.argumentNodes[Editor::FindEca(seed, ecaId)->arguments[0]].id;
    QVERIFY(Editor::SetScalarArgument(seed, argumentId, 3, "7").Succeeded());

    // 使用正式模块的参数命令和 Undo；只在独立原创文档中应用，不接触作者地图。
    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(seed));
    auto* originalDocument = project.triggerDocument_.get();
    auto* originalUndo = project.triggerUndoStack_;
    Modules::TriggerModule module(project);
    const auto baseline = Trigger::WtgWctCodec{}.Serialize(*originalDocument, *project.uiCatalog_);
    QVERIFY(baseline.Succeeded());
    const auto argument = [&]() -> const Trigger::ArgumentNode& {
        const auto* eca = Editor::FindEca(*project.triggerDocument_, ecaId);
        return project.triggerDocument_->argumentNodes[eca->arguments[0]];
    };
    const auto bytesMatchBaseline = [&]() {
        const auto wire = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
        return wire.Succeeded() && wire.value->wtg == baseline.value->wtg && wire.value->wct == baseline.value->wct;
    };
    // 面板关闭会记住大小；保存并恢复唯一设置键，不污染作者上次使用的尺寸。
    QSettings settings;
    const QString sizeKey = QStringLiteral("triggers/valuePanel/size");
    const bool hadSize = settings.contains(sizeKey);
    const QVariant priorSize = settings.value(sizeKey);
    const auto restore = qScopeGuard([&]() {
        while (originalUndo->index() > 0) originalUndo->undo();
        if (hadSize) settings.setValue(sizeKey, priorSize); else settings.remove(sizeKey);
        QVERIFY(bytesMatchBaseline());
    });
    const QString captureRoot = qEnvironmentVariable("FORGE_TRIGGER_VALUE_CAPTURE_DIR");
    const auto rectJson = [](QRect r) { return QJsonObject{{"x", r.x()}, {"y", r.y()}, {"width", r.width()}, {"height", r.height()}}; };
    const auto wireSha = [](const auto& bytes) {
        return QString::fromLatin1(QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(bytes.data()),
            static_cast<qsizetype>(bytes.size())), QCryptographicHash::Sha256).toHex());
    };
    QJsonArray captures;
    for (const bool variable : {true, false}) {
        const QString kind = variable ? QStringLiteral("variable") : QStringLiteral("literal");
        const QString expectedValue = variable ? QStringLiteral("当前整数") : QStringLiteral("42");
        const int expectedKind = variable ? 1 : 3;
        {
            TriggerValuePanel chooser(*project.triggerDocument_, *project.uiCatalog_, ecaId, 0, {});
            chooser.setAttribute(Qt::WA_DeleteOnClose, false);
            bool chosen = false;
            chooser.SetChosen([&](const TriggerValueChoice& choice) {
                chosen = module.ApplyValueChoice(ecaId, 0, choice);
            });
            chooser.show();
            auto* tabs = chooser.findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
            auto* search = chooser.findChild<QLineEdit*>(QStringLiteral("valuePanelSearch"));
            auto* list = chooser.findChild<QListWidget*>(QStringLiteral("valuePanelList"));
            QVERIFY(tabs && search && list);
            const QString targetPage = variable ? QStringLiteral("变量") : QStringLiteral("值");
            int targetIndex = -1;
            for (int i = 0; i < tabs->count(); ++i) if (tabs->tabText(i) == targetPage) targetIndex = i;
            QVERIFY(targetIndex >= 0);
            tabs->setCurrentIndex(targetIndex);
            search->setText(variable ? QString{} : expectedValue);
            const int row = FindValue(list, expectedValue);
            QVERIFY(row >= 0);
            QCOMPARE(list->item(row)->data(Qt::UserRole + 1).toInt(), expectedKind);
            list->setCurrentRow(row);
            chooser.Apply();
            QVERIFY(chosen);
        }
        QCOMPARE(argument().type, expectedKind);
        QCOMPARE(QString::fromStdString(argument().value.text), expectedValue);
        QVERIFY(!argument().callEca.has_value());
        QCOMPARE(originalUndo->index(), 1);
        QVERIFY(!bytesMatchBaseline());
        const auto appliedWire = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
        QVERIFY(appliedWire.Succeeded());
        for (const QSize requested : {QSize(420, 320), QSize(540, 500)}) {
            // 重新绑定已应用的文档当前值；不是只切换旧函数面板的页签。
            TriggerValuePanel panel(*project.triggerDocument_, *project.uiCatalog_, ecaId, 0, {});
            panel.setAttribute(Qt::WA_DeleteOnClose, false);
            panel.resize(requested);
            panel.show();
            auto* card = panel.findChild<QFrame*>(QStringLiteral("valuePanelCurrentCard"));
            auto* name = panel.findChild<QLabel*>(QStringLiteral("valuePanelCurrentName"));
            auto* summary = panel.findChild<QLabel*>(QStringLiteral("valuePanelCurrentSummary"));
            auto* icon = panel.findChild<QLabel*>(QStringLiteral("valuePanelCurrentIcon"));
            auto* list = panel.findChild<QListWidget*>(QStringLiteral("valuePanelList"));
            auto* note = panel.findChild<QLabel*>(QStringLiteral("valuePanelNote"));
            auto* search = panel.findChild<QLineEdit*>(QStringLiteral("valuePanelSearch"));
            auto* tabs = panel.findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
            auto* edit = panel.findChild<QPushButton*>(QStringLiteral("valuePanelEditCall"));
            QVERIFY(card && name && summary && icon && list && note && search && tabs && edit);
            QCOMPARE(name->text(), expectedValue);
            QCOMPARE(summary->text(), variable ? QStringLiteral("全局变量 · 整数") : QStringLiteral("整数"));
            QCOMPARE(tabs->tabText(tabs->currentIndex()), variable ? QStringLiteral("变量") : QStringLiteral("值"));
            QVERIFY(!edit->isVisible());
            QVERIFY(!icon->pixmap().isNull());
            const QString iconName = variable ? QStringLiteral("lucide/variable") : QStringLiteral("lucide/hash");
            QCOMPARE(icon->pixmap().toImage(), UiKit::SvgIcon(iconName).pixmap(16, 16).toImage());
            for (const bool withNote : {false, true}) {
                if (withNote) search->setText(variable ? QStringLiteral("不存在的变量") : QStringLiteral("2147483648"));
                list->setFocus(); // 避免输入光标闪烁干扰像素稳定采样，不关闭任何动画。
                const int styleDuration = panel.style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, &panel);
                const int minimumSettle = std::max(250, styleDuration);
                QElapsedTimer elapsed, stableWindow;
                elapsed.start();
                QImage previous, settledImage;
                QJsonArray previousGeometry;
                int stableSamples = 0;
                bool stable = false;
                while (elapsed.elapsed() < 5000) {
                    QTest::qWait(40);
                    if (elapsed.elapsed() < minimumSettle) continue;
                    bool animationRunning = false;
                    for (const auto* animation : panel.findChildren<QAbstractAnimation*>())
                        animationRunning |= animation->state() == QAbstractAnimation::Running;
                    const QJsonArray geometry{rectJson(panel.rect()), rectJson(card->geometry()), rectJson(list->geometry()),
                        rectJson(note->geometry()), note->isVisible()};
                    const QImage frame = panel.grab().toImage();
                    if (!animationRunning && !frame.isNull() && frame == previous && geometry == previousGeometry) {
                        ++stableSamples;
                        if (!stableWindow.isValid()) stableWindow.start();
                        if (stableSamples >= 5 && stableWindow.elapsed() >= 200) { stable = true; settledImage = frame; break; }
                    } else { stableSamples = 0; stableWindow.invalidate(); }
                    previous = frame;
                    previousGeometry = geometry;
                }
                const bool geometryValid = panel.rect().contains(card->geometry()) && panel.rect().contains(list->geometry()) &&
                    !card->geometry().intersects(list->geometry()) && (!withNote || (panel.rect().contains(note->geometry()) &&
                    !card->geometry().intersects(note->geometry()) && !list->geometry().intersects(note->geometry())));
                if (!stable || !geometryValid) {
                    // 失败先落原像素与几何；下方严格断言照常失败，不能留下只有前几张成功图的假完整批次。
                    const QString failedStem = QStringLiteral("current-%1-%2x%3%4.failure").arg(kind).arg(requested.width()).arg(requested.height())
                        .arg(withNote ? QStringLiteral("-note") : QString{});
                    const auto failureImage = settledImage.isNull() ? previous : settledImage;
                    const QJsonObject failure{{"source_kind", kind}, {"argument_kind", argument().type},
                        {"argument_value", QString::fromStdString(argument().value.text)}, {"requested_size", rectJson(QRect(QPoint{}, requested))},
                        {"actual_size", rectJson(panel.rect())}, {"actual_minimum_size", rectJson(QRect(QPoint{}, panel.minimumSize()))},
                        {"card", rectJson(card->geometry())}, {"list", rectJson(list->geometry())}, {"note", rectJson(note->geometry())},
                        {"note_visible", note->isVisible()}, {"note_text", note->text()}, {"with_note", withNote},
                        {"note_size_hint", rectJson(QRect(QPoint{}, note->sizeHint()))},
                        {"note_minimum_size_hint", rectJson(QRect(QPoint{}, note->minimumSizeHint()))},
                        {"note_height_for_width", note->heightForWidth(note->width())},
                        {"layout_minimum_size", rectJson(QRect(QPoint{}, panel.layout()->minimumSize()))},
                        {"layout_total_minimum_size", rectJson(QRect(QPoint{}, panel.layout()->totalMinimumSize()))},
                        {"layout_total_height_for_width", panel.layout()->totalHeightForWidth(panel.width())},
                        {"stable", stable}, {"stable_samples", stableSamples}, {"geometry_valid", geometryValid},
                        {"dpr", panel.devicePixelRatioF()}, {"png_width", failureImage.width()}, {"png_height", failureImage.height()},
                        {"platform", QGuiApplication::platformName()}, {"native_desktop_capture", false}, {"fixture_only", true}};
                    qWarning().noquote() << QJsonDocument(failure).toJson(QJsonDocument::Compact);
                    if (!captureRoot.isEmpty()) {
                        QVERIFY(QDir().mkpath(captureRoot));
                        const QString failedPath = QDir(captureRoot).filePath(failedStem);
                        QVERIFY(!QFile::exists(failedPath + QStringLiteral(".png")));
                        QVERIFY(failureImage.save(failedPath + QStringLiteral(".png")));
                        QFile failedReceipt(failedPath + QStringLiteral(".json"));
                        QVERIFY(!failedReceipt.exists());
                        QVERIFY(failedReceipt.open(QIODevice::WriteOnly));
                        const auto bytes = QJsonDocument(failure).toJson(QJsonDocument::Indented);
                        QCOMPARE(failedReceipt.write(bytes), bytes.size());
                    }
                }
                QVERIFY2(stable, "Current-value panel failed to reach stable pixels and geometry with animations preserved");
                QCOMPARE(note->isVisible(), withNote);
                QCOMPARE(card->parentWidget(), static_cast<QWidget*>(&panel));
                QCOMPARE(list->parentWidget(), static_cast<QWidget*>(&panel));
                QCOMPARE(note->parentWidget(), static_cast<QWidget*>(&panel));
                QVERIFY(panel.rect().contains(card->geometry()) && panel.rect().contains(list->geometry()));
                QVERIFY(card->rect().contains(icon->geometry()) && card->rect().contains(name->geometry()) && card->rect().contains(summary->geometry()));
                QVERIFY(!icon->geometry().intersects(name->geometry()));
                QVERIFY(!summary->geometry().intersects(icon->geometry()) && !summary->geometry().intersects(name->geometry()));
                QVERIFY(!card->geometry().intersects(list->geometry()));
                QVERIFY(card->geometry().bottom() < list->geometry().top());
                if (withNote) {
                    QVERIFY(panel.rect().contains(note->geometry()));
                    QVERIFY(!card->geometry().intersects(note->geometry()) && !list->geometry().intersects(note->geometry()));
                    QVERIFY(note->geometry().top() > list->geometry().bottom());
                    QVERIFY(!note->text().isEmpty());
                }
                QCOMPARE(name->text(), expectedValue);
                QCOMPARE(argument().type, expectedKind);
                QCOMPARE(QString::fromStdString(argument().value.text), expectedValue);
                QCOMPARE(originalUndo->index(), 1);
                const auto currentWire = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
                QVERIFY(currentWire.Succeeded());
                QVERIFY(currentWire.value->wtg == appliedWire.value->wtg && currentWire.value->wct == appliedWire.value->wct);
                const QString stem = QStringLiteral("current-%1-%2x%3%4").arg(kind).arg(requested.width()).arg(requested.height())
                    .arg(withNote ? QStringLiteral("-note") : QString{});
                QJsonObject facts{{"file", stem + QStringLiteral(".png")}, {"fixture_only", true}, {"platform", QGuiApplication::platformName()},
                    {"native_desktop_capture", false}, {"source_kind", kind}, {"argument_kind", expectedKind}, {"argument_value", expectedValue},
                    {"current_name", name->text()}, {"current_summary", summary->text()}, {"current_type", QString::fromStdString(panel.ExpectedType())},
                    {"current_icon", iconName}, {"current_icon_non_null", !icon->pixmap().isNull()}, {"edit_call_visible", edit->isVisible()},
                    {"selected_page", tabs->tabText(tabs->currentIndex())}, {"applied_through_panel_and_original_undo", true},
                    {"capture_phase", "applied-current-value"}, {"undo_index_at_capture", originalUndo->index()},
                    {"requested_size", rectJson(QRect(QPoint{}, requested))}, {"actual_size", rectJson(panel.rect())},
                    {"actual_minimum_size", rectJson(QRect(QPoint{}, panel.minimumSize()))}, {"dpr", panel.devicePixelRatioF()},
                    {"png_width", settledImage.width()}, {"png_height", settledImage.height()},
                    {"card", rectJson(card->geometry())}, {"list", rectJson(list->geometry())}, {"note", rectJson(note->geometry())},
                    {"icon_in_card", rectJson(icon->geometry())}, {"name_in_card", rectJson(name->geometry())}, {"summary_in_card", rectJson(summary->geometry())},
                    {"note_visible", note->isVisible()}, {"note_text", note->text()}, {"stable", stable}, {"stable_samples", stableSamples},
                    {"stable_window_ms", stableWindow.elapsed()}, {"actual_settle_ms", elapsed.elapsed()}, {"minimum_settle_ms", minimumSettle},
                    {"style_animation_duration_ms", styleDuration}, {"animations_disabled_by_test", false},
                    {"applied_wtg_sha256", wireSha(appliedWire.value->wtg)}, {"applied_wct_sha256", wireSha(appliedWire.value->wct)}};
                if (!captureRoot.isEmpty()) {
                    QVERIFY(QDir().mkpath(captureRoot));
                    const QString path = QDir(captureRoot).filePath(stem + QStringLiteral(".png"));
                    QVERIFY(!QFile::exists(path));
                    QVERIFY(settledImage.save(path));
                }
                captures.append(facts);
            }
        }
        originalUndo->undo();
        QCOMPARE(originalUndo->index(), 0);
        QVERIFY(bytesMatchBaseline());
        QCOMPARE(project.triggerDocument_.get(), originalDocument);
        QCOMPARE(project.triggerUndoStack_, originalUndo);
    }
    QCOMPARE(captures.size(), 8);
    QVERIFY(bytesMatchBaseline());
    const auto finalWire = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(finalWire.Succeeded());
    if (!captureRoot.isEmpty()) {
        for (const auto& capture : captures) {
            auto facts = capture.toObject();
            facts.insert("final_document_restored", true);
            facts.insert("final_undo_index", originalUndo->index());
            facts.insert("baseline_wtg_sha256", wireSha(baseline.value->wtg));
            facts.insert("baseline_wct_sha256", wireSha(baseline.value->wct));
            facts.insert("final_wtg_sha256", wireSha(finalWire.value->wtg));
            facts.insert("final_wct_sha256", wireSha(finalWire.value->wct));
            const QString stem = QFileInfo(facts.value("file").toString()).completeBaseName();
            QFile receipt(QDir(captureRoot).filePath(stem + QStringLiteral(".json")));
            QVERIFY(!receipt.exists());
            QVERIFY(receipt.open(QIODevice::WriteOnly));
            const auto bytes = QJsonDocument(facts).toJson(QJsonDocument::Indented);
            QCOMPARE(receipt.write(bytes), bytes.size());
        }
    }
}

void TriggerValueTests::YdweLocalsAndReplacingCalls() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const std::filesystem::path ui = root.path().toStdWString();
    WriteUtf8(ui / "ui" / "config", "base\n");
    WriteUtf8(ui / "ui" / "base" / "define.txt",
        "[TriggerTypes]\neffect=0,1,1,特效\nstring=0,1,1,字符串\ntypename=1,0,0,参数类型,string\n"
        "boolean=0,1,1,布尔\naliasbool=0,1,1,布尔别名,boolean\nAttrType=0,0,0,属性种类,boolean\ninteger=0,1,1,整数\nreal=0,1,1,实数\nbooleanoperator=0,0,0,布尔比较\n"
        "scriptcode=1,0,0,脚本代码,string\n"
        "[TriggerParams]\ntypename20_effect=0,typename,20,特效\ntypename04_string=0,typename,4,字符串\n"
        "LastFlag=0,boolean,bj_lastLoadPreloadSLResult,存档结果\nAttrType0=0,AttrType,false,基础\nAttrType1=0,AttrType,true,全部\nBoolEqual=0,booleanoperator,==,等于\n");
    WriteUtf8(ui / "ui" / "base" / "condition.txt",
        "[OperatorCompareBoolean]\ndescription=\"${表达式A} ${比较} ${表达式B}\"\n"
        "[[.args]]\ntype=boolean\ndefault=\"true\"\n[[.args]]\ntype=booleanoperator\ndefault=\"BoolEqual\"\n"
        "[[.args]]\ntype=boolean\ndefault=\"true\"\n");
    WriteUtf8(ui / "ui" / "base" / "action.txt",
        "[YDWESetAnyTypeLocalVariable]\ndescription=\"[${类型}] loc_${变量名} = ${值}\"\n"
        "[[.args]]\ntype=typename\n[[.args]]\ntype=scriptcode\n[[.args]]\ntype=Null\n"
        "[DestroyEffect]\ndescription=\"删除 ${特效}\"\n[[.args]]\ntype=effect\n"
        "[CommentString]\ndescription=\"-------- ${注释文本} --------\"\n[[.args]]\ntype=scriptcode\n"
        "[UseValues]\n[[.args]]\ntype=boolean\n[[.args]]\ntype=aliasbool\n[[.args]]\ntype=AttrType\n[[.args]]\ntype=integer\n[[.args]]\ntype=real\n[[.args]]\ntype=string\n");
    WriteUtf8(ui / "ui" / "base" / "call.txt",
        "[GetLastCreatedEffectBJ]\ntitle=\"最后创建的特效\"\ndescription=\"最后创建的特效\"\nreturns=effect\n"
        "[YDWEGetAnyTypeLocalVariable]\ntitle=\"变量 -- 读取\"\ndescription=\"loc_${变量名}\"\nreturns=AnyReturnType\n"
        "[[.args]]\ntype=scriptcode\n"
        "[GetUnitName]\ntitle=\"单位名字\"\nreturns=string\n");
    auto catalog = Trigger::UiCatalog::Load(ui);
    QVERIFY(catalog.Succeeded());
    auto parsed = Trigger::WtgWctCodec{}.Parse(Wtg(), Wct(), *catalog.value);
    QVERIFY2(parsed.Succeeded(), parsed.diagnostics.empty() ? "" : parsed.diagnostics.front().message.c_str());
    QVERIFY(Trigger::TriggerDocumentEditor::AppendRootEca(*parsed.value, *catalog.value,
        parsed.value->triggers.front().id, Trigger::UiDefinitionKind::Action, "UseValues").Succeeded());
    QVERIFY(Trigger::TriggerDocumentEditor::AppendRootEca(*parsed.value, *catalog.value,
        parsed.value->triggers.front().id, Trigger::UiDefinitionKind::Condition, "OperatorCompareBoolean").Succeeded());
    QtProject::EditorProject project;
    project.triggerUndoStack_ = new QUndoStack(&project);
    project.uiCatalog_ = std::make_unique<Trigger::UiCatalog>(std::move(*catalog.value));
    project.triggerDocument_ = std::make_unique<Trigger::TriggerDocument>(std::move(*parsed.value));
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    page->resize(1200, 700);
    page->show();
    QVERIFY(module.Reveal({"triggers", "trigger", "Fx", {}}));
    auto& document = *project.triggerDocument_;
    const auto rootEca = [&document](std::size_t row) -> const Trigger::EcaNode& {
        return document.ecaNodes[document.triggers.front().ecas[row]];
    };
    const auto argumentOf = [&](std::size_t row, std::size_t position) -> const Trigger::ArgumentNode& {
        return document.argumentNodes[rootEca(row).arguments[position]];
    };
    const auto callName = [&](std::size_t row, std::size_t position) {
        const auto& argument = argumentOf(row, position);
        return argument.callEca ? document.ecaNodes[*argument.callEca].name.text : std::string{};
    };
    const auto encode = [&]() { return Trigger::WtgWctCodec{}.Serialize(document, *project.uiCatalog_).value->wtg; };
    const auto open = [&](std::size_t row, std::size_t position) {
        module.OpenValuePanel(rootEca(row).id, position, QRect(page->mapToGlobal(QPoint(40, 40)), QSize(30, 16)));
        return module.ValuePanel();
    };
    const auto tabTexts = [](TriggerValuePanel* panel) {
        QStringList texts;
        auto* tabs = panel->findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
        for (int index = 0; index < tabs->count(); ++index) texts << tabs->tabText(index);
        return texts;
    };
    const auto rows = [](TriggerValuePanel* panel) {
        QStringList texts;
        auto* list = panel->findChild<QListWidget*>(QStringLiteral("valuePanelList"));
        for (int index = 0; index < list->count(); ++index) texts << list->item(index)->text();
        return texts;
    };
    const auto pristine = encode();

    // 真正的传统参数对话框：布尔比较 B 的直接值与预设共用 literal 请求，YDWE 预设保留原 kind。
    {
        TriggerParameterDialog dialog(document, *project.uiCatalog_, rootEca(4).id);
        const auto parameterRows = dialog.findChildren<QWidget*>(QStringLiteral("parameterRow"));
        QCOMPARE(parameterRows.size(), 3);
        QWidget* b = parameterRows[2];
        auto* kind = b->findChild<QComboBox*>(QStringLiteral("parameterValueKind"));
        auto* direct = b->findChild<QComboBox*>(QStringLiteral("parameterBooleanValue"));
        auto* presets = b->findChild<QComboBox*>(QStringLiteral("parameterPresetValue"));
        QVERIFY(kind && direct && presets);
        QCOMPARE(b->findChild<QStackedWidget*>()->currentWidget(), static_cast<QWidget*>(direct));
        QCOMPARE(direct->currentData().toString(), QStringLiteral("true"));
        direct->setCurrentIndex(direct->findData(QStringLiteral("false")));
        QCOMPARE(dialog.PendingEdits()[2].value, std::string("false"));
        QCOMPARE(dialog.PendingEdits()[2].valueKind, 3);
        kind->setCurrentIndex(kind->findData(0));
        QCOMPARE(presets->count(), 3); QVERIFY(presets->findData(QStringLiteral("true")) >= 0);
        presets->setCurrentIndex(presets->findData(QStringLiteral("false")));
        QCOMPARE(dialog.PendingEdits()[2].value, std::string("false")); QCOMPARE(dialog.PendingEdits()[2].valueKind, 3);
        presets->setCurrentIndex(presets->findData(QStringLiteral("LastFlag")));
        QCOMPARE(dialog.PendingEdits()[2].value, std::string("LastFlag")); QCOMPARE(dialog.PendingEdits()[2].valueKind, 0);
        QVERIFY(encode() == pristine);
    }

    // 当前是局部变量读取：默认在“变量”页，列出 loc_fx；函数页同时列出特效函数与 AnyReturnType 函数。
    TriggerValuePanel* panel = open(1, 0);
    QCOMPARE(panel->ExpectedType(), std::string("effect"));
    auto* tabs = panel->findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
    QCOMPARE(tabs->tabText(tabs->currentIndex()), QStringLiteral("变量"));
    QVERIFY2(rows(panel).join('|').contains(QStringLiteral("loc_fx")), qPrintable(rows(panel).join('|')));
    tabs->setCurrentIndex(tabTexts(panel).indexOf(QStringLiteral("函数")));
    const QStringList functions = rows(panel);
    QVERIFY2(functions.size() == 2 && functions[0].startsWith(QStringLiteral("最后创建的特效")) &&
            functions[1].startsWith(QStringLiteral("变量 -- 读取")), qPrintable(functions.join('|')));
    QVERIFY(!functions.join('|').contains(QStringLiteral("单位名字")));

    // 换成另一个函数：摘除原读取函数，挂接新函数；一次撤销逐字节还原。
    const int undoIndex = project.triggerUndoStack_->index();
    panel->findChild<QListWidget*>(QStringLiteral("valuePanelList"))->setCurrentRow(0);
    panel->Apply();
    QTRY_COMPARE(callName(1, 0), std::string("GetLastCreatedEffectBJ"));
    QCOMPARE(project.triggerUndoStack_->index(), undoIndex + 1);
    project.triggerUndoStack_->undo();
    QCOMPARE(callName(1, 0), std::string("YDWEGetAnyTypeLocalVariable"));
    QVERIFY(encode() == pristine);
    project.triggerUndoStack_->redo();
    QCOMPARE(callName(1, 0), std::string("GetLastCreatedEffectBJ"));

    // 再换回局部变量：生成读取函数并写入变量名。
    panel = open(1, 0);
    tabs = panel->findChild<QTabBar*>(QStringLiteral("valuePanelTabs"));
    tabs->setCurrentIndex(tabTexts(panel).indexOf(QStringLiteral("变量")));
    auto* list = panel->findChild<QListWidget*>(QStringLiteral("valuePanelList"));
    QCOMPARE(list->count(), 1);
    list->setCurrentRow(0);
    panel->Apply();
    QTRY_COMPARE(callName(1, 0), std::string("YDWEGetAnyTypeLocalVariable"));
    const auto& getter = document.ecaNodes[*argumentOf(1, 0).callEca];
    QCOMPARE(document.argumentNodes[getter.arguments[0]].value.text, std::string("fx"));

    // 设置局部变量的值：Null 跟随 typename（特效）。
    panel = open(0, 2);
    QCOMPARE(panel->ExpectedType(), std::string("effect"));
    QVERIFY(tabTexts(panel).contains(QStringLiteral("函数")));
    panel->close();

    // scriptcode：只有文本页，输入原样写入（不裁剪首尾空格）。
    panel = open(2, 0);
    QCOMPARE(tabTexts(panel), QStringList{QStringLiteral("文本")});
    panel->findChild<QLineEdit*>(QStringLiteral("valuePanelSearch"))->setText(QStringLiteral(" 愈战愈勇 "));
    panel->Apply();
    QTRY_COMPARE(argumentOf(2, 0).value.text, std::string(" 愈战愈勇 "));

    // 局部变量名（scriptcode）提示已有的变量名。
    panel = open(0, 1);
    QCOMPARE(tabTexts(panel), QStringList{QStringLiteral("文本")});
    panel->findChild<QLineEdit*>(QStringLiteral("valuePanelSearch"))->clear();
    QVERIFY2(rows(panel).join('|').contains(QStringLiteral("fx　局部")), qPrintable(rows(panel).join('|')));
    panel->close();

    const auto beforeBoolean = encode(); const int beforeUndo = project.triggerUndoStack_->index();
    panel = open(3, 0); QVERIFY(rows(panel).join('|').contains(QStringLiteral("真　true")));
    QVERIFY(rows(panel).join('|').contains(QStringLiteral("假　false"))); QVERIFY(rows(panel).join('|').contains(QStringLiteral("存档结果")));
    panel->findChild<QListWidget*>(QStringLiteral("valuePanelList"))->setCurrentRow(0); panel->Apply();
    QTRY_COMPARE(argumentOf(3, 0).value.text, std::string("true")); QCOMPARE(argumentOf(3, 0).type, 3);
    QCOMPARE(project.triggerUndoStack_->index(), beforeUndo + 1); project.triggerUndoStack_->undo(); QVERIFY(encode() == beforeBoolean);
    panel = open(3, 1); QCOMPARE(rows(panel).size(), 2); panel->close();
    panel = open(3, 2); QCOMPARE(rows(panel), (QStringList{QStringLiteral("基础　AttrType0"), QStringLiteral("全部　AttrType1")})); panel->close();
    for (const auto& sample : {std::pair{3, QStringLiteral("2147483648")}, std::pair{4, QStringLiteral("NaN")}}) {
        panel = open(3, static_cast<std::size_t>(sample.first));
        auto* input = panel->findChild<QLineEdit*>(QStringLiteral("valuePanelSearch")); input->setText(sample.second);
        QCOMPARE(rows(panel).size(), 0); panel->Apply(); QCOMPARE(project.triggerUndoStack_->index(), beforeUndo);
        input->setText(QStringLiteral("1")); panel->Apply(); QTRY_COMPARE(argumentOf(3, static_cast<std::size_t>(sample.first)).value.text, std::string("1"));
        project.triggerUndoStack_->undo(); QVERIFY(encode() == beforeBoolean);
    }
    panel = open(3, 5); QCOMPARE(rows(panel), QStringList{QStringLiteral("使用“”")}); panel->close();
}

void TriggerValueTests::PreplacedReferencesUseCaptureTypesAndUndo() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QtProject::EditorProject project;
    const auto initialized = MakePreplacedProject(project, temporary.path().toStdWString());
    QVERIFY2(initialized.Succeeded(), initialized.diagnostics.empty() ? "" : initialized.diagnostics.front().message.c_str());
    const auto initial = project.CaptureTriggerMapObjectReferences();
    QVERIFY2(initial.Succeeded(), initial.diagnostics.empty() ? "" : initial.diagnostics.front().message.c_str());
    QVERIFY((*initial.value)->Find("gg_unit_hfoo_0007"));
    QVERIFY((*initial.value)->Find("gg_item_ratf_0008"));
    QVERIFY(!(*initial.value)->Find("gg_unit_u001_0009"));
    const auto originalArchiveSha = project.archiveSession_->SourceSha256();
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    page->resize(1000, 650);
    page->show();
    QVERIFY(module.Reveal({"triggers", "trigger", "ReadPlaced", {}}));
    const auto open = [&](std::size_t row, std::size_t position) {
        module.OpenValuePanel(RootEca(project, row).id, position, QRect(page->mapToGlobal(QPoint(30, 30)), QSize(40, 20)));
        return module.ValuePanel();
    };
    // native handle 祖先按共用类型链过滤；单位与物品不互相冒充。
    for (std::size_t position = 0; position < 5; ++position) {
        auto* panel = open(0, position);
        auto* list = Variables(panel);
        QCOMPARE(FindValue(list, QStringLiteral("gg_unit_hfoo_0007")) >= 0, position == 0 || position == 2 || position == 3);
        QCOMPARE(FindValue(list, QStringLiteral("gg_item_ratf_0008")) >= 0, position == 1 || position == 2 || position == 3);
        QCOMPARE(FindValue(list, QStringLiteral("gg_unit_u001_0009")), -1);
        panel->close();
    }
    auto* targetPanel = open(1, 0);
    QCOMPARE(FindValue(Variables(targetPanel), QStringLiteral("gg_unit_hfoo_0007")), -1);
    QCOMPARE(FindValue(Variables(targetPanel), QStringLiteral("gg_item_ratf_0008")), -1);
    targetPanel->close();

    const auto pristine = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(pristine.Succeeded());
    auto* panel = open(0, 0);
    auto* list = Variables(panel);
    list->setCurrentRow(FindValue(list, QStringLiteral("gg_unit_hfoo_0007")));
    std::optional<TriggerValueChoice> selected;
    panel->SetChosen([&](const auto& value) { selected = value; });
    panel->Apply();
    QVERIFY(selected.has_value());
    QCOMPARE(selected->mapObjectSnapshotIdentity, (*initial.value)->Identity());
    QCOMPARE(selected->valueKind, 1);
    QVERIFY(module.ApplyValueChoice(RootEca(project, 0).id, 0, *selected));
    QCOMPARE(RootArgument(project, 0, 0).value.text, std::string("gg_unit_hfoo_0007"));
    QCOMPARE(project.triggerUndoStack_->index(), 1);
    project.triggerUndoStack_->undo();
    const auto restored = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
    QVERIFY(restored.Succeeded());
    QVERIFY(restored.value->wtg == pristine.value->wtg && restored.value->wct == pristine.value->wct);
    project.triggerUndoStack_->redo();
    QCOMPARE(RootArgument(project, 0, 0).value.text, std::string("gg_unit_hfoo_0007"));
    project.triggerUndoStack_->undo();

    // 物品同样经真实选择结果写入，且可向 widget/handle 父类型传递。
    panel = open(0, 1);
    list = Variables(panel);
    list->setCurrentRow(FindValue(list, QStringLiteral("gg_item_ratf_0008")));
    std::optional<TriggerValueChoice> itemChoice;
    panel->SetChosen([&](const auto& value) { itemChoice = value; });
    panel->Apply();
    QVERIFY(itemChoice.has_value());
    QCOMPARE(itemChoice->mapObjectSnapshotIdentity, (*initial.value)->Identity());
    for (const std::size_t position : {1U, 2U, 3U}) {
        QVERIFY(module.ApplyValueChoice(RootEca(project, 0).id, position, *itemChoice));
        QCOMPARE(RootArgument(project, 0, position).value.text, std::string("gg_item_ratf_0008"));
        project.triggerUndoStack_->undo();
    }

    const auto undoCount = project.triggerUndoStack_->count();
    const auto unchanged = [&] {
        const auto bytes = Trigger::WtgWctCodec{}.Serialize(*project.triggerDocument_, *project.uiCatalog_);
        return bytes.Succeeded() && bytes.value->wtg == pristine.value->wtg && bytes.value->wct == pristine.value->wct
            && project.triggerUndoStack_->index() == 0 && project.triggerUndoStack_->count() == undoCount;
    };
    QVERIFY(!module.ApplyValueChoice(RootEca(project, 0).id, 1, *selected)); // unit 不能读作 item。
    QVERIFY(unchanged());
    QVERIFY(!module.ApplyValueChoice(RootEca(project, 1).id, 0, *selected)); // SetVariable 左值。
    QVERIFY(unchanged());
    auto unsealed = *selected;
    unsealed.mapObjectSnapshotIdentity.clear();
    QVERIFY(!module.ApplyValueChoice(RootEca(project, 0).id, 0, unsealed));
    QVERIFY(unchanged());
    auto forged = *selected;
    forged.value = "gg_unit_hfoo_9999";
    QVERIFY(!module.ApplyValueChoice(RootEca(project, 0).id, 0, forged));
    QVERIFY(unchanged());
    // 已有带下标参数不能把相同来源借作数组（拒绝前不得改动参数池或 Undo）。
    auto& argument = RootArgument(project, 0, 0);
    const auto scalar = argument;
    argument.insertIndex = 1;
    argument.indexArgument = RootEca(project, 0).arguments[1];
    QVERIFY(!module.ApplyValueChoice(RootEca(project, 0).id, 0, *selected));
    QCOMPARE(argument.insertIndex, 1);
    QCOMPARE(argument.value.text, scalar.value.text);
    QCOMPARE(project.triggerUndoStack_->index(), 0);
    argument = scalar;
    QVERIFY(unchanged());

    // 弹层完成选择后，世界编辑已移动实例：同符号也必须重新捕获，不能重用旧快照。
    auto units = *project.mapDocument_->Units();
    units.units.front().position[0] = 64.f;
    QVERIFY(project.mapEdits_->EditUnits(units).Succeeded());
    const auto moved = project.CaptureTriggerMapObjectReferences();
    QVERIFY(moved.Succeeded());
    QVERIFY((*moved.value)->Identity() != selected->mapObjectSnapshotIdentity);
    QVERIFY(!module.ApplyValueChoice(RootEca(project, 0).id, 0, *selected));
    QVERIFY(unchanged());
    QVERIFY(project.mapEdits_->Undo());
    QVERIFY(project.archiveSession_->VerifySourceUnchanged().Succeeded());
    auto reopened = MapArchiveSession::Open(project.sourceArchivePath_);
    QVERIFY(reopened.Succeeded());
    QCOMPARE(reopened.value->SourceSha256(), originalArchiveSha);
}

void TriggerValueTests::PreplacedCaptureUsesCurrentOverlays() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QtProject::EditorProject project;
    const auto initialized = MakePreplacedProject(project, temporary.path().toStdWString());
    QVERIFY2(initialized.Succeeded(), initialized.diagnostics.empty() ? "" : initialized.diagnostics.front().message.c_str());
    const auto initial = project.CaptureTriggerMapObjectReferences();
    QVERIFY(initial.Succeeded());
    const auto originalMember = project.archiveSession_->ReadMember("war3map.w3u");
    QVERIFY(originalMember.Succeeded());
    const auto unitIndex = static_cast<std::size_t>(ObjectData::ObjectType::Unit);
    const auto itemIndex = static_cast<std::size_t>(ObjectData::ObjectType::Item);
    // 新自定义类别只存在于当前 LNI overlay，Capture 必须消费既有 native 回灌的结果。
    QVERIFY(project.objectDocuments_[unitIndex]->AddObject("u001", "hfoo").Succeeded());
    project.objectDirty_[unitIndex] = true;
    auto withUnit = project.CaptureTriggerMapObjectReferences();
    QVERIFY2(withUnit.Succeeded(), withUnit.diagnostics.empty() ? "" : withUnit.diagnostics.front().message.c_str());
    QVERIFY((*withUnit.value)->Find("gg_unit_u001_0009"));
    QVERIFY((*withUnit.value)->Identity() != (*initial.value)->Identity());
    const auto baselineBytes = ObjectData::NativeObjectCodec(ObjectData::ObjectType::Unit)
        .Serialize(*project.nativeObjectDocuments_[unitIndex]);
    QVERIFY(baselineBytes.Succeeded());
    QVERIFY(*baselineBytes.value == *originalMember.value); // 回灌只写隔离副本。

    QVERIFY(project.objectDocuments_[itemIndex]->AddObject("I001", "ratf").Succeeded());
    project.objectDirty_[itemIndex] = true;
    auto placements = *project.mapDocument_->Units();
    auto item = placements.units[1];
    item.rawcode = "I001";
    item.creationId = 10;
    placements.units.push_back(item);
    placements.units.front().creationId = 70;
    QVERIFY(project.mapEdits_->EditUnits(placements).Succeeded());
    const auto effective = project.CaptureTriggerMapObjectReferences();
    QVERIFY(effective.Succeeded());
    QVERIFY((*effective.value)->Find("gg_item_I001_0010"));
    QVERIFY((*effective.value)->Find("gg_unit_hfoo_0070"));
    QVERIFY(!(*effective.value)->Find("gg_unit_hfoo_0007"));
    QVERIFY((*effective.value)->Find("gg_unit_u001_0009"));
    placements.units.pop_back();
    QVERIFY(project.mapEdits_->EditUnits(placements).Succeeded());
    const auto removed = project.CaptureTriggerMapObjectReferences();
    QVERIFY(removed.Succeeded());
    QVERIFY(!(*removed.value)->Find("gg_item_I001_0010"));
    QVERIFY((*removed.value)->Find("gg_unit_hfoo_0070"));
    QVERIFY(project.mapEdits_->Undo());
    project.nativeObjectDocuments_[unitIndex].reset();
    const auto missingBaseline = project.CaptureTriggerMapObjectReferences();
    QVERIFY(!missingBaseline.Succeeded());
    QVERIFY(!missingBaseline.value);
    QVERIFY(project.archiveSession_->VerifySourceUnchanged().Succeeded());
}

void TriggerValueTests::MissingReferenceSourcesKeepOrdinaryParametersEditable() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QtProject::EditorProject project;
    const auto initialized = MakePreplacedProject(project, temporary.path().toStdWString());
    QVERIFY2(initialized.Succeeded(), initialized.diagnostics.empty() ? "" : initialized.diagnostics.front().message.c_str());
    const auto unitIndex = static_cast<std::size_t>(ObjectData::ObjectType::Unit);
    project.objectDirty_[unitIndex] = true;
    project.nativeObjectDocuments_[unitIndex].reset();
    QVERIFY(!project.CaptureTriggerMapObjectReferences().Succeeded());
    Modules::TriggerModule module(project);
    QWidget* page = module.Widget();
    page->resize(1000, 650);
    page->show();
    QVERIFY(module.Reveal({"triggers", "trigger", "ReadPlaced", {}}));
    module.OpenValuePanel(RootEca(project, 0).id, 0, QRect(page->mapToGlobal(QPoint(30, 30)), QSize(40, 20)));
    auto* panel = module.ValuePanel();
    auto* list = Variables(panel);
    QVERIFY(FindValue(list, QStringLiteral("PickedUnit")) >= 0);
    QCOMPARE(FindValue(list, QStringLiteral("gg_unit_hfoo_0007")), -1);
    QVERIFY(panel->findChild<QLabel*>(QStringLiteral("valuePanelNote"))->text().contains(QStringLiteral("预放对象不可用")));
    panel->close();
    TriggerValueChoice ordinary;
    ordinary.valueKind = 1;
    ordinary.value = "PickedUnit";
    QVERIFY(module.ApplyValueChoice(RootEca(project, 0).id, 2, ordinary));
    QCOMPARE(RootArgument(project, 0, 2).value.text, std::string("PickedUnit"));
    QCOMPARE(project.triggerUndoStack_->index(), 1);
    project.triggerUndoStack_->undo();
    QCOMPARE(RootArgument(project, 0, 2).value.text, std::string("PickedWidget"));
    ordinary.value = "gg_unit_hfoo_0007";
    QVERIFY(!module.ApplyValueChoice(RootEca(project, 0).id, 0, ordinary));
    QCOMPARE(project.triggerUndoStack_->index(), 0);

    // 目录 LNI 没有已支持的分类捕获，不降级到同目录 native 或标准表；普通变量仍可用。
    QTemporaryDir lniRoot;
    QVERIFY(lniRoot.isValid());
    project.archiveSession_.reset();
    project.mapEdits_.reset();
    project.objectDirty_.fill(false);
    project.projectRoot_ = lniRoot.path().toStdWString();
    WriteUtf8(project.projectRoot_ / "table/unit.ini", "[u001]\n_parent=\"hfoo\"\n");
    const auto lni = project.CaptureTriggerMapObjectReferences();
    QVERIFY(!lni.Succeeded());
    QVERIFY(std::any_of(lni.diagnostics.begin(), lni.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == "MAP_OBJECT_LNI_CLASSIFICATION_UNSUPPORTED";
    }));
    ordinary.value = "PickedUnit";
    QVERIFY(module.ApplyValueChoice(RootEca(project, 0).id, 2, ordinary));
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    War3Map::UiKit::ThemeManager theme;
    if (qEnvironmentVariableIsSet("FORGE_TRIGGER_VALUE_CAPTURE_DIR")) {
        const auto fonts = War3Map::UiKit::LoadFonts(War3Map::UiKit::DefaultFontDirectory());
        if (!fonts.bundledUi || !fonts.bundledCode || !fonts.failedFiles.isEmpty()) {
            qCritical("Value capture requires deployed bundled UI/code fonts; refusing unreadable evidence");
            return 2;
        }
        War3Map::UiKit::ApplyApplicationFont(fonts);
        const QFontMetrics metrics(QApplication::font());
        if (!metrics.inFont(QChar{u'地'}) || !metrics.inFont(QChar{u'A'})) return 2;
        theme.ApplyTheme(QStringLiteral("graphite"));
    }
    TriggerValueTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "TriggerValueTests.moc"
