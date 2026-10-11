#include "Dock/DockHost.h"
#include "Dock/FloatFrame.h"
#include "Dock/ModuleSwitcher.h"
#include "Dock/SideGroup.h"
#include "Layout/DockLayout.h"
#include "Layout/LayoutStore.h"
#include "Modules/ModuleHost.h"
#include "Modules/ModuleRegistry.h"
#include "Shell/HeaderBar.h"
#include "Shell/ShellWindow.h"

#include <QAction>
#include <QApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QPointer>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QUndoGroup>
#include <QUndoStack>
#include <QVBoxLayout>
#include <QtTest>

#include <algorithm>
#include <set>

using namespace War3Map::Shell;

namespace {

const std::vector<ModuleId> kAll{"triggers", "objects", "variables", "sounds", "assets", "manager", "campaign", "ai", "world"};

// 合成模块：记录创建次数与定位请求；控件含一个输入框用来验证状态随控件保留。
class FakeModule final : public EditorModule {
public:
    FakeModule(ModuleId id, QString label, bool available = true, bool showInSwitcher = true)
        : id_(std::move(id)), label_(std::move(label)), available_(available),
          showInSwitcher_(showInSwitcher) {}
    ~FakeModule() override { delete widget_; }

    ModuleInfo Info() const override {
        ModuleInfo info;
        info.id = id_;
        info.label = label_;
        info.icon = QStringLiteral("lucide/box");
        info.available = available_;
        info.unavailableReason = QStringLiteral("未接通");
        info.showInSwitcher = showInSwitcher_;
        return info;
    }
    QWidget* Widget() override {
        if (widget_ == nullptr) {
            ++created;
            widget_ = new QWidget;
            widget_->setObjectName(QString::fromStdString(id_));
            auto* layout = new QVBoxLayout(widget_);
            edit = new QLineEdit(widget_);
            layout->addWidget(edit);
        }
        return widget_;
    }
    void AttachHost(ModuleHost* value) override {
        host = value;
        if (onAttachHost) onAttachHost(value);
    }
    bool Reveal(const NavigationTarget& target) override {
        if (target.id == QStringLiteral("missing")) return false;
        revealed.push_back(target.id);
        position = target.id;
        scroll = 0;
        return true;
    }
    std::optional<QJsonObject> CaptureNavigationBookmark() const override {
        if (loading) return std::nullopt;
        return QJsonObject{{"position", position}, {"scroll", scroll}};
    }
    NavigationRestoreResult RestoreNavigationBookmark(const QJsonObject& state) override {
        if (blocked) return NavigationRestoreResult::Blocked;
        if (invalid.contains(state.value("position").toString())) return NavigationRestoreResult::Invalid;
        position = state.value("position").toString();
        scroll = state.value("scroll").toInt();
        return NavigationRestoreResult::Restored;
    }
    bool PrepareNavigationLeave() override { return !blocked; }
    void CancelNavigationRestoration() override { ++cancelled; }
    QUndoStack* UndoStack() override { return &stack; }
    QJsonObject SaveViewState() const override {
        return edit ? QJsonObject{{"text", edit->text()}} : QJsonObject{};
    }
    void RestoreViewState(const QJsonObject& state) override { restoredState = state; }

    int created = 0;
    QString position;
    int scroll = 0, cancelled = 0;
    bool blocked = false;
    bool loading = false;
    QStringList invalid;
    QPointer<QLineEdit> edit;
    ModuleHost* host = nullptr;
    std::function<void(ModuleHost*)> onAttachHost;
    QStringList revealed;
    QJsonObject restoredState;
    QUndoStack stack;
    QPointer<QWidget> widget_;

private:
    ModuleId id_;
    QString label_;
    bool available_;
    bool showInSwitcher_;
};

struct Fixture {
    ModuleRegistry registry;
    FakeModule* triggers = nullptr;
    FakeModule* objects = nullptr;
    FakeModule* manager = nullptr;
    FakeModule* campaign = nullptr;

    Fixture() {
        auto add = [this](ModuleId id, QString label, bool available = true) {
            auto module = std::make_unique<FakeModule>(std::move(id), std::move(label), available);
            FakeModule* raw = module.get();
            registry.Register(std::move(module));
            return raw;
        };
        triggers = add("triggers", QStringLiteral("触发器"));
        objects = add("objects", QStringLiteral("物体"));
        manager = add("manager", QStringLiteral("物体管理"));
        campaign = add("campaign", QStringLiteral("战役"), false);
    }
};

bool Contains(QWidget* ancestor, QWidget* widget) {
    return widget != nullptr && (ancestor == widget || ancestor->isAncestorOf(widget));
}

} // namespace

class ShellTests final : public QObject {
    Q_OBJECT

private slots:
    void layoutKeepsEachModuleInOnePlace();
    void layoutCollapseReadAndClamp();
    void layoutJsonRoundTripAndValidation();
    void registryRejectsDuplicates();
    void modulesAreCreatedLazily();
    void menuOnlyModuleDoesNotConsumeHeaderWidth();
    void movingModulesKeepsWidgetAndState();
    void dockTargetsForFloatingWindows();
    void undoFollowsCurrentModule();
    void layoutChangesRetargetUndo();
    void navigationRevealsAndGoesBack();
    void navigationRestoresSelectionScrollAndForward();
    void asynchronousArrivalRetainsOriginalBookmark();
    void blockedAndFailedNavigationKeepsBothDirections();
    void deletedTargetIsSkippedAndNewNavigationCutsForward();
    void acceptedWorkspaceResetClearsHistoryAndAdvancesEpoch();
    void synchronousNavigationCallbacksRejectDestroyedOrChangedWorkspace();
    void mouseSideButtonsRouteOnceOnlyToOwnedWindows();
    void explicitRevealIncludesRepeatedPlacement();
    void explicitRevealIncludesActivationNavigationAndBack();
    void restoreAndRejectedRequestsDoNotReveal();
    void unavailableModulesCannotBePlaced();
    void unavailableModulesCannotBeRestoredOrNavigated();
    void unavailableDefaultModuleIsNotCreated();
    void layoutPersistsAcrossWindows();
    void teardownReturnsWidgetsToModules();
    void headerFitsNarrowWindows();
    void viewStatesPersistAcrossWindows();
};

void ShellTests::layoutKeepsEachModuleInOnePlace() {
    DockLayout l = DefaultLayout();
    QCOMPARE(*MainShown(l, kAll), ModuleId("triggers"));
    l = Activate(l, "objects");
    QCOMPARE(*MainShown(l, kAll), ModuleId("objects"));
    l = Place(l, "objects", Placement::Float(10, 20, 700, 500));
    QVERIFY(WhereIs(l, "objects") == Area::Float);
    QCOMPARE(*MainShown(l, kAll), ModuleId("triggers"));
    l = Place(l, "manager", Placement::Side());
    l = Place(l, "variables", Placement::Side(0));
    QVERIFY((l.side == std::vector<ModuleId>{"variables", "manager"}));
    QCOMPARE(*l.sideActive, ModuleId("variables"));
    auto visible = VisibleModules(l, kAll);
    std::sort(visible.begin(), visible.end());
    QVERIFY((visible == std::vector<ModuleId>{"objects", "triggers", "variables"}));
    l = Place(l, "sounds", Placement::Float(30, 40));
    l = Activate(l, "objects");
    QCOMPARE(l.floats.back().id, ModuleId("objects"));
    l = Place(l, "variables", Placement::Main());
    QVERIFY((l.side == std::vector<ModuleId>{"manager"}));
    QCOMPARE(*l.sideActive, ModuleId("manager"));
    QCOMPARE(*MainShown(l, kAll), ModuleId("variables"));
    l = Place(l, "objects", Placement::Main());
    l = Place(l, "objects", Placement::Float(5, 5));
    const auto window = std::find_if(l.floats.begin(), l.floats.end(), [](const FloatWindow& f) { return f.id == "objects"; });
    QCOMPARE(window->w, 700);
    std::set<ModuleId> placed(l.side.begin(), l.side.end());
    for (const auto& f : l.floats) {
        QVERIFY(placed.insert(f.id).second);
    }
    QCOMPARE(MainModules(l, kAll).size() + placed.size(), kAll.size());
}

void ShellTests::layoutCollapseReadAndClamp() {
    DockLayout l = Place(DefaultLayout(), "ai", Placement::Float(0, 0, 500, 400));
    FloatWindow collapsed = l.floats.front();
    collapsed.collapsed = true;
    l = UpdateFloat(l, collapsed);
    QVERIFY(Activate(l, "ai").floats.front().collapsed);
    QVERIFY(!Activate(l, "ai", true).floats.front().collapsed);
    DockLayout wide = l;
    wide.sideWidth = 600;
    const auto preset = MakePreset("triggers-manager", wide);
    QVERIFY(preset.has_value());
    QCOMPARE(preset->mainActive, ModuleId("triggers"));
    QVERIFY((preset->side == std::vector<ModuleId>{"manager"}));
    QVERIFY(preset->floats.empty());
    QCOMPARE(preset->sideWidth, 600);
    QVERIFY(!MakePreset("no-such-preset", l).has_value());

    const FloatWindow clamped = ClampFloat({"ai", 5000, -50, 100, 9999}, {0, 0, 1200, 800});
    QCOMPARE(clamped.x, 1120);
    QCOMPARE(clamped.y, 0);
    QCOMPARE(clamped.w, 360);
    QCOMPARE(clamped.h, 800);
    // 副屏（坐标不从 0 开始）同样保证标题栏可见。
    const FloatWindow second = ClampFloat({"ai", -9000, 5000, 800, 600}, {1920, 0, 1920, 1040});
    QCOMPARE(second.x, 1920 + 80 - 800);
    QCOMPARE(second.y, 1040 - 36);
}

void ShellTests::layoutJsonRoundTripAndValidation() {
    DockLayout l = Place(DefaultLayout(), "objects", Placement::Float(10, 20, 700, 500));
    l = Place(l, "manager", Placement::Side());
    l = Detach(Place(l, "sounds", Placement::Float(1, 2, 640, 480)), "sounds");
    const DockLayout back = LayoutFromJson(LayoutToJson(l), kAll);
    QVERIFY(back == l);
    QVERIFY((back.sizes.at("sounds") == FloatSize{640, 480}));

    const std::vector<ModuleId> known{"triggers", "objects", "ai"};
    QJsonObject raw{
        {"mainActive", "bogus"},
        {"side", QJsonArray{"objects", "objects", "bogus"}},
        {"floats", QJsonArray{QJsonObject{{"id", "ai"}, {"x", 1}, {"y", 2}, {"w", 3}, {"h", "NaN"}},
                              QJsonObject{{"id", "objects"}, {"x", 0}, {"y", 0}, {"w", 400}, {"h", 300}}}},
        {"sideWidth", 10},
        {"sizes", QJsonObject{{"ai", QJsonObject{{"w", 500}, {"h", 300}}}, {"bogus", QJsonObject{{"w", 1}, {"h", 1}}}}},
    };
    const DockLayout read = LayoutFromJson(raw, known);
    QCOMPARE(read.mainActive, ModuleId("triggers"));
    QVERIFY((read.side == std::vector<ModuleId>{"objects"}));
    QVERIFY(read.floats.empty());
    QCOMPARE(read.sideWidth, kMinSideWidth);
    QCOMPARE(read.sizes.size(), std::size_t{1});
    QVERIFY(read.sizes.contains("ai"));
    QVERIFY(LayoutFromJson(QJsonValue(QStringLiteral("garbage")), known) == DefaultLayout());
}

void ShellTests::registryRejectsDuplicates() {
    ModuleRegistry registry;
    QVERIFY(registry.Register(std::make_unique<FakeModule>("triggers", QStringLiteral("触发器"))));
    QVERIFY(!registry.Register(std::make_unique<FakeModule>("triggers", QStringLiteral("重复"))));
    QVERIFY(!registry.Register(std::make_unique<FakeModule>("", QStringLiteral("空"))));
    QVERIFY(!registry.Register(nullptr));
    QCOMPARE(registry.Ids().size(), std::size_t{1});
}

void ShellTests::modulesAreCreatedLazily() {
    Fixture f;
    ShellWindow shell(f.registry, nullptr);
    QCOMPARE(f.triggers->created, 1);
    QCOMPARE(f.objects->created, 0);
    QCOMPARE(f.manager->created, 0);
    QVERIFY(f.triggers->host == &shell);
    QVERIFY(shell.Header()->Switcher()->Chip("triggers")->isChecked());
    QVERIFY(!shell.Header()->Switcher()->Chip("campaign")->isEnabled());

    shell.ActivateModule("objects");
    QCOMPARE(f.objects->created, 1);
    QCOMPARE(shell.Dock()->MainStack()->currentWidget(), f.objects->Widget());
    QVERIFY(shell.Header()->Switcher()->Chip("objects")->isChecked());
    QVERIFY(!shell.Header()->Switcher()->Chip("triggers")->isChecked());
    shell.ActivateModule("objects");
    QCOMPARE(f.objects->created, 1);
}

void ShellTests::menuOnlyModuleDoesNotConsumeHeaderWidth() {
    ModuleRegistry registry;
    QVERIFY(registry.Register(std::make_unique<FakeModule>("triggers", QStringLiteral("触发器"))));
    auto settings = std::make_unique<FakeModule>("map-settings", QStringLiteral("地图设置"), true, false);
    auto* settingsModule = settings.get();
    QVERIFY(registry.Register(std::move(settings)));
    ShellWindow shell(registry, nullptr);
    QVERIFY(shell.Header()->Switcher()->Chip("triggers") != nullptr);
    QVERIFY(shell.Header()->Switcher()->Chip("map-settings") == nullptr);
    QCOMPARE(settingsModule->created, 0);
    shell.ActivateModule("map-settings");
    QCOMPARE(settingsModule->created, 1);
    QCOMPARE(shell.Dock()->MainStack()->currentWidget(), settingsModule->Widget());
}

void ShellTests::movingModulesKeepsWidgetAndState() {
    Fixture f;
    ShellWindow shell(f.registry, nullptr);
    shell.show();
    shell.ActivateModule("objects");
    QWidget* widget = f.objects->Widget();
    f.objects->edit->setText(QStringLiteral("hfoo"));
    f.objects->edit->setCursorPosition(2);

    shell.PlaceModule("objects", Placement::Float(120, 140, 640, 420));
    FloatFrame* frame = shell.Dock()->Frame("objects");
    QVERIFY(frame != nullptr);
    QVERIFY(Contains(frame, widget));
    QVERIFY(frame->isVisible());
    QCOMPARE(shell.Dock()->MainStack()->currentWidget(), f.triggers->Widget());
    QCOMPARE(shell.Header()->Switcher()->Chip("objects")->property("placement").toString(), QStringLiteral("float"));

    shell.PlaceModule("objects", Placement::Side());
    QTest::qWait(0);
    QVERIFY(shell.Dock()->Frame("objects") == nullptr);
    QVERIFY(shell.Dock()->Side()->isVisible());
    QVERIFY(Contains(shell.Dock()->Side(), widget));
    QVERIFY(widget->isVisible());

    shell.PlaceModule("manager", Placement::Side(0));
    QVERIFY((shell.Layout().side == std::vector<ModuleId>{"manager", "objects"}));
    QVERIFY(!widget->isVisible());

    shell.PlaceModule("objects", Placement::Main());
    QVERIFY(Contains(shell.Dock()->MainStack(), widget));
    QCOMPARE(shell.Dock()->MainStack()->currentWidget(), widget);
    // 同一个控件实例一路移动，状态不丢。
    QCOMPARE(f.objects->created, 1);
    QCOMPARE(f.objects->edit->text(), QStringLiteral("hfoo"));
    QCOMPARE(f.objects->edit->cursorPosition(), 2);
    // 浮动尺寸被记住，再次浮动沿用。
    shell.PlaceModule("objects", Placement::Float(10, 10));
    QCOMPARE(shell.Layout().floats.back().w, 640);
}

void ShellTests::dockTargetsForFloatingWindows() {
    Fixture f;
    ShellWindow shell(f.registry, nullptr);
    shell.resize(1200, 800);
    shell.show();
    QVERIFY(QTest::qWaitForWindowExposed(&shell));
    DockHost* dock = shell.Dock();
    const QPoint header = shell.Header()->mapToGlobal(shell.Header()->rect().center());
    QVERIFY(dock->DockTargetAt(header).has_value());
    QVERIFY(dock->DockTargetAt(header)->area == Area::Main);
    const QPoint rightEdge = dock->mapToGlobal(QPoint(dock->width() - 20, dock->height() / 2));
    QVERIFY(dock->DockTargetAt(rightEdge)->area == Area::Side);
    // 主编辑区中部不是停靠目标：浮动窗口经常被移到主窗口上方。
    const QPoint middle = dock->mapToGlobal(QPoint(dock->width() / 3, dock->height() / 2));
    QVERIFY(!dock->DockTargetAt(middle).has_value());
}

void ShellTests::undoFollowsCurrentModule() {
    Fixture f;
    ShellWindow shell(f.registry, nullptr);
    QCOMPARE(shell.UndoGroup()->activeStack(), &f.triggers->stack);
    shell.ActivateModule("objects");
    QCOMPARE(shell.UndoGroup()->activeStack(), &f.objects->stack);
    QVERIFY(!shell.UndoAction()->isEnabled());
    f.objects->stack.push(new QUndoCommand(QStringLiteral("修改生命值")));
    QVERIFY(shell.UndoAction()->isEnabled());
    shell.UndoAction()->trigger();
    QCOMPARE(f.objects->stack.index(), 0);
    shell.ActivateModule("triggers");
    QCOMPARE(shell.UndoGroup()->activeStack(), &f.triggers->stack);
    QVERIFY(!shell.RedoAction()->isEnabled());
}

void ShellTests::layoutChangesRetargetUndo() {
    Fixture f;
    ShellWindow shell(f.registry, nullptr);
    shell.ActivateModule("objects");
    f.objects->stack.push(new QUndoCommand(QStringLiteral("修改物体")));
    f.triggers->stack.push(new QUndoCommand(QStringLiteral("修改触发器")));
    // 无需显示窗口或等待焦点事件，预设本身必须确定撤销目标。
    QVERIFY(shell.ApplyPreset("triggers-manager"));
    QCOMPARE(*shell.CurrentModule(), ModuleId("triggers"));
    QCOMPARE(shell.UndoGroup()->activeStack(), &f.triggers->stack);
    shell.UndoAction()->trigger();
    QCOMPARE(f.triggers->stack.index(), 0);
    QCOMPARE(f.objects->stack.index(), 1);

    shell.PlaceModule("manager", Placement::Side());
    QCOMPARE(*shell.CurrentModule(), ModuleId("manager"));
    DockLayout layout = shell.Layout();
    layout.mainActive = "objects";
    shell.SetLayout(layout);
    QCOMPARE(*shell.CurrentModule(), ModuleId("objects"));
    shell.UndoAction()->trigger();
    QCOMPARE(f.objects->stack.index(), 0);
}

void ShellTests::navigationRevealsAndGoesBack() {
    Fixture f;
    ShellWindow shell(f.registry, nullptr);
    shell.ActivateModule("objects");
    QVERIFY(!shell.BackAction()->isEnabled());
    f.objects->host->Navigate({"manager", QStringLiteral("object"), QStringLiteral("hfoo"), {}});
    QCOMPARE(f.manager->revealed, QStringList{QStringLiteral("hfoo")});
    QCOMPARE(*shell.CurrentModule(), ModuleId("manager"));
    QVERIFY(shell.BackAction()->isEnabled());
    QVERIFY(shell.GoBack());
    QCOMPARE(*shell.CurrentModule(), ModuleId("objects"));
    QCOMPARE(shell.Dock()->MainStack()->currentWidget(), f.objects->Widget());
    QVERIFY(!shell.GoBack());
}

void ShellTests::navigationRestoresSelectionScrollAndForward() {
    Fixture f; ShellWindow shell(f.registry, nullptr);
    shell.ActivateModule("triggers");
    f.triggers->position = QStringLiteral("eca-1"); f.triggers->scroll = 137;
    QVERIFY(shell.TryNavigate({"objects", "object", "map-hfoo", {}}));
    f.objects->scroll = 64;
    QVERIFY(shell.GoBack());
    QCOMPARE(*shell.CurrentModule(), ModuleId("triggers"));
    QCOMPARE(f.triggers->position, QStringLiteral("eca-1")); QCOMPARE(f.triggers->scroll, 137);
    QVERIFY(shell.ForwardAction()->isEnabled());
    QVERIFY(shell.GoForward());
    QCOMPARE(*shell.CurrentModule(), ModuleId("objects")); QCOMPARE(f.objects->position, QStringLiteral("map-hfoo"));
    QCOMPARE(f.objects->scroll, 64);
    QVERIFY(shell.TryNavigate({"objects", "object", "map-hbar", {}}));
    QVERIFY(shell.GoBack()); QCOMPARE(f.objects->position, QStringLiteral("map-hfoo")); QCOMPARE(f.objects->scroll, 64);
    QVERIFY(shell.GoForward()); QCOMPARE(f.objects->position, QStringLiteral("map-hbar"));
}
void ShellTests::asynchronousArrivalRetainsOriginalBookmark() {
    Fixture f; ShellWindow shell(f.registry, nullptr);
    shell.ActivateModule("objects"); f.objects->position = "before-loading"; f.objects->scroll = 73;
    f.manager->loading = true;
    QVERIFY(shell.TryNavigate({"manager", "code", "async-code", {}}));
    QVERIFY(shell.BackAction()->isEnabled());
    QVERIFY(!shell.GoBack()); // 加载中尚不能捕获当前位置，历史保留。
    f.manager->loading = false;
    QVERIFY(shell.GoBack()); QCOMPARE(f.objects->position, QStringLiteral("before-loading")); QCOMPARE(f.objects->scroll, 73);
    QVERIFY(shell.GoForward()); QCOMPARE(f.manager->position, QStringLiteral("async-code"));
}
void ShellTests::blockedAndFailedNavigationKeepsBothDirections() {
    Fixture f; ShellWindow shell(f.registry, nullptr);
    shell.ActivateModule("objects"); f.objects->position = "A";
    QVERIFY(shell.TryNavigate({"manager", "object", "B", {}}));
    f.manager->blocked = true;
    QVERIFY(!shell.GoBack()); QVERIFY(shell.BackAction()->isEnabled()); QVERIFY(!shell.ForwardAction()->isEnabled());
    f.manager->blocked = false; f.objects->blocked = true;
    QVERIFY(!shell.GoBack()); QCOMPARE(*shell.CurrentModule(), ModuleId("manager"));
    f.objects->blocked = false;
    QVERIFY(shell.GoBack()); QVERIFY(shell.ForwardAction()->isEnabled());
    QVERIFY(!shell.TryNavigate({"manager", "object", "missing", {}}));
    QVERIFY(shell.ForwardAction()->isEnabled()); QCOMPARE(*shell.CurrentModule(), ModuleId("objects"));
    QVERIFY(shell.GoForward()); QCOMPARE(f.manager->position, QStringLiteral("B"));
}
void ShellTests::deletedTargetIsSkippedAndNewNavigationCutsForward() {
    Fixture f; ShellWindow shell(f.registry, nullptr);
    shell.ActivateModule("objects"); f.objects->position = "A";
    QVERIFY(shell.TryNavigate({"objects", "object", "deleted", {}}));
    QVERIFY(shell.TryNavigate({"manager", "object", "B", {}}));
    f.objects->invalid << "deleted";
    QVERIFY(shell.GoBack()); QCOMPARE(f.objects->position, QStringLiteral("A"));
    QVERIFY(shell.ForwardAction()->isEnabled());
    QVERIFY(shell.TryNavigate({"manager", "object", "C", {}}));
    QVERIFY(!shell.ForwardAction()->isEnabled());
    QVERIFY(shell.GoBack()); QCOMPARE(f.objects->position, QStringLiteral("A"));
    QVERIFY(!shell.GoBack());
}
void ShellTests::acceptedWorkspaceResetClearsHistoryAndAdvancesEpoch() {
    Fixture f; ShellWindow shell(f.registry, nullptr);
    shell.ActivateModule("objects");
    QVERIFY(shell.TryNavigate({"manager", "object", "B", {}}));
    const auto epoch = shell.NavigationEpoch(); const auto cancelCount = f.objects->cancelled;
    shell.ResetNavigationHistory();
    QCOMPARE(shell.NavigationEpoch(), epoch + 1);
    QVERIFY(f.objects->cancelled > cancelCount);
    QVERIFY(!shell.BackAction()->isEnabled()); QVERIFY(!shell.ForwardAction()->isEnabled());
    QVERIFY(!shell.GoBack()); QVERIFY(!shell.GoForward());
}
void ShellTests::synchronousNavigationCallbacksRejectDestroyedOrChangedWorkspace() {
    for (int mode = 0; mode < 3; ++mode) {
        Fixture f;
        QPointer<ShellWindow> shell = new ShellWindow(f.registry, nullptr);
        const auto cleanup = qScopeGuard([&]() { delete shell.data(); });
        shell->ActivateModule("objects");
        const auto epoch = shell->NavigationEpoch();
        if (mode == 2) {
            connect(shell, &ShellWindow::CurrentModuleChanged, shell, [shell](const ModuleId& id) {
                if (id == "manager") delete shell.data();
            });
        } else {
            connect(shell, &ShellWindow::ModuleRevealRequested, shell, [shell, mode](const ModuleId& id) {
                if (id != "manager") return;
                if (mode == 0) delete shell.data(); else shell->ResetNavigationHistory();
            });
        }
        const bool accepted = shell->TryNavigate({"manager", "object", "B", {}});
        QVERIFY(!accepted);
        if (mode == 1) {
            QVERIFY(shell);
            QCOMPARE(shell->NavigationEpoch(), epoch + 1);
            QCOMPARE(*shell->CurrentModule(), ModuleId("objects"));
            QVERIFY(!shell->BackAction()->isEnabled());
            QVERIFY(!shell->ForwardAction()->isEnabled());
        } else {
            QVERIFY(!shell);
        }
    }
}
void ShellTests::mouseSideButtonsRouteOnceOnlyToOwnedWindows() {
    Fixture f; ShellWindow shell(f.registry, nullptr);
    shell.show(); shell.ActivateModule("objects"); f.objects->position = "A";
    QVERIFY(shell.TryNavigate({"manager", "object", "B", {}}));
    QWidget foreign;
    QTest::mouseClick(&foreign, Qt::BackButton); QCOMPARE(*shell.CurrentModule(), ModuleId("manager"));
    QTest::mouseClick(f.manager->edit, Qt::BackButton); QCOMPARE(*shell.CurrentModule(), ModuleId("objects"));
    QVERIFY(!shell.BackAction()->isEnabled());
    QTest::mouseClick(f.objects->edit, Qt::ForwardButton); QCOMPARE(*shell.CurrentModule(), ModuleId("manager"));
    shell.PlaceModule("manager", Placement::Float(30, 40, 700, 500));
    QTest::mouseClick(f.manager->edit, Qt::BackButton); QCOMPARE(*shell.CurrentModule(), ModuleId("objects"));
}

void ShellTests::explicitRevealIncludesRepeatedPlacement() {
    Fixture f;
    ShellWindow shell(f.registry, nullptr);
    QSignalSpy reveal(&shell, &ShellWindow::ModuleRevealRequested);
    QSignalSpy changed(&shell, &ShellWindow::CurrentModuleChanged);
    QVERIFY(reveal.isValid());
    shell.PlaceModule("objects", Placement::Main());
    QCOMPARE(reveal.count(), 1);
    QCOMPARE(reveal.at(0).at(0).value<ModuleId>(), ModuleId("objects"));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(*shell.CurrentModule(), ModuleId("objects"));
    const auto placed = shell.Layout();
    shell.PlaceModule("objects", Placement::Main());
    QCOMPARE(reveal.count(), 2);
    QCOMPARE(reveal.at(1).at(0).value<ModuleId>(), ModuleId("objects"));
    QCOMPARE(changed.count(), 1);
    QVERIFY(shell.Layout() == placed);

    // 顶栏和停靠区拖放的实际信号接线也经过同一通知入口。
    shell.Header()->ModuleDropped("objects");
    QCOMPARE(reveal.count(), 3);
    shell.Dock()->PlaceRequested("manager", Placement::Side());
    QCOMPARE(reveal.count(), 4);
    QCOMPARE(reveal.at(3).at(0).value<ModuleId>(), ModuleId("manager"));
    QCOMPARE(WhereIs(shell.Layout(), "manager"), Area::Side);
}

void ShellTests::explicitRevealIncludesActivationNavigationAndBack() {
    Fixture f;
    ShellWindow shell(f.registry, nullptr);
    QSignalSpy reveal(&shell, &ShellWindow::ModuleRevealRequested);
    shell.ActivateModule("objects");
    shell.ActivateModule("objects");
    QCOMPARE(reveal.count(), 2);
    QCOMPARE(reveal.at(0).at(0).value<ModuleId>(), ModuleId("objects"));
    QCOMPARE(reveal.at(1).at(0).value<ModuleId>(), ModuleId("objects"));
    shell.Navigate({"manager", QStringLiteral("object"), QStringLiteral("hfoo"), {}});
    QCOMPARE(reveal.count(), 3);
    QCOMPARE(reveal.at(2).at(0).value<ModuleId>(), ModuleId("manager"));
    QCOMPARE(f.manager->revealed, QStringList{QStringLiteral("hfoo")});
    QVERIFY(shell.GoBack());
    QCOMPARE(reveal.count(), 4);
    QCOMPARE(reveal.at(3).at(0).value<ModuleId>(), ModuleId("objects"));
    QVERIFY(!shell.GoBack());
    QCOMPARE(reveal.count(), 4);
    shell.Header()->Switcher()->ActivateRequested("objects");
    QCOMPARE(reveal.count(), 5);
    shell.Dock()->ActivateRequested("objects", false);
    QCOMPARE(reveal.count(), 5);
    shell.Dock()->ActivateRequested("objects", true);
    QCOMPARE(reveal.count(), 6);
}

void ShellTests::restoreAndRejectedRequestsDoNotReveal() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("reveal.ini")), QSettings::IniFormat);
    auto restored = DefaultLayout();
    restored.mainActive = "objects";
    SaveLayout(settings, restored);
    Fixture f;
    // 在构造期模块挂接时安装spy，覆盖随后真正执行的Commit/视图恢复。
    std::unique_ptr<QSignalSpy> reveal;
    f.triggers->onAttachHost = [&](ModuleHost* host) {
        if (host) reveal = std::make_unique<QSignalSpy>(
            static_cast<ShellWindow*>(host), &ShellWindow::ModuleRevealRequested);
    };
    ShellWindow shell(f.registry, &settings);
    QVERIFY(reveal);
    QVERIFY(reveal->isValid());
    QCOMPARE(*shell.CurrentModule(), ModuleId("objects"));
    QCOMPARE(reveal->count(), 0);
    restored.mainActive = "manager";
    shell.SetLayout(restored);
    QCOMPARE(*shell.CurrentModule(), ModuleId("manager"));
    QCOMPARE(reveal->count(), 0);
    QVERIFY(shell.ApplyPreset("triggers-manager"));
    QCOMPARE(reveal->count(), 0);
    shell.Dock()->SideWidthEdited(440);
    QCOMPARE(shell.Layout().sideWidth, 440);
    QCOMPARE(reveal->count(), 0);
    const auto before = shell.Layout();
    for (const ModuleId id : {"missing", "campaign"}) {
        shell.PlaceModule(id, Placement::Main());
        shell.ActivateModule(id);
        shell.Navigate({id, {}, {}, {}});
        shell.Dock()->ActivateRequested(id, true);
    }
    QCOMPARE(reveal->count(), 0);
    QVERIFY(shell.Layout() == before);
    QCOMPARE(f.campaign->created, 0);
    QVERIFY(f.campaign->revealed.empty());
}

void ShellTests::unavailableModulesCannotBePlaced() {
    Fixture f;
    ShellWindow shell(f.registry, nullptr);
    const DockLayout before = shell.Layout();
    shell.PlaceModule("campaign", Placement::Side());
    shell.ActivateModule("campaign");
    QVERIFY(shell.Layout() == before);
    QCOMPARE(f.campaign->created, 0);
}

void ShellTests::unavailableModulesCannotBeRestoredOrNavigated() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("shell.ini")), QSettings::IniFormat);
    DockLayout stale = DefaultLayout();
    stale.mainActive = "campaign";
    stale = Place(stale, "campaign", Placement::Float(50, 50, 500, 400));
    SaveLayout(settings, stale);
    Fixture f;
    ShellWindow shell(f.registry, &settings);
    QCOMPARE(f.campaign->created, 0);
    QVERIFY(shell.Dock()->Frame("campaign") == nullptr);
    QCOMPARE(*shell.CurrentModule(), ModuleId("triggers"));

    stale = Place(stale, "campaign", Placement::Side());
    shell.SetLayout(stale);
    QVERIFY(shell.Layout().side.empty());
    QCOMPARE(f.campaign->created, 0);
    shell.Navigate({"campaign", QStringLiteral("chapter"), QStringLiteral("1"), {}});
    QVERIFY(f.campaign->revealed.empty());
    QVERIFY(!shell.BackAction()->isEnabled());
    QCOMPARE(*shell.CurrentModule(), ModuleId("triggers"));

    // DockHost 的公开应用接口也不能绕过 Shell 的能力检查。
    shell.Dock()->Apply(stale);
    QCOMPARE(f.campaign->created, 0);
}

void ShellTests::unavailableDefaultModuleIsNotCreated() {
    ModuleRegistry registry;
    auto disabled = std::make_unique<FakeModule>("triggers", QStringLiteral("触发器"), false);
    FakeModule* disabledRaw = disabled.get();
    registry.Register(std::move(disabled));
    auto enabled = std::make_unique<FakeModule>("objects", QStringLiteral("物体"));
    FakeModule* enabledRaw = enabled.get();
    registry.Register(std::move(enabled));
    ShellWindow shell(registry, nullptr);
    QCOMPARE(disabledRaw->created, 0);
    QCOMPARE(*shell.CurrentModule(), ModuleId("objects"));
    QCOMPARE(shell.UndoGroup()->activeStack(), &enabledRaw->stack);
    QVERIFY(shell.ApplyPreset("triggers-variables"));
    QCOMPARE(disabledRaw->created, 0);
    QCOMPARE(*shell.CurrentModule(), ModuleId("objects"));
    // 唯一可用模块浮动后主区应为空，不得回退创建禁用的 triggers。
    shell.PlaceModule("objects", Placement::Float(50, 50, 500, 400));
    QCOMPARE(disabledRaw->created, 0);
    QCOMPARE(shell.Dock()->MainStack()->count(), 0);
}

void ShellTests::layoutPersistsAcrossWindows() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("shell.ini")), QSettings::IniFormat);
    {
        Fixture f;
        ShellWindow shell(f.registry, &settings);
        QVERIFY(shell.ApplyPreset("objects-manager"));
        shell.close();
    }
    Fixture f;
    ShellWindow shell(f.registry, &settings);
    QCOMPARE(shell.Layout().mainActive, ModuleId("objects"));
    QVERIFY((shell.Layout().side == std::vector<ModuleId>{"manager"}));
    QCOMPARE(f.manager->created, 1);
    QCOMPARE(shell.Dock()->MainStack()->currentWidget(), f.objects->Widget());
}

void ShellTests::teardownReturnsWidgetsToModules() {
    class ValueCommand final : public QUndoCommand {
    public:
        explicit ValueCommand(int& value) : value_(value) {}
        void redo() override { value_ = 42; }
        void undo() override { value_ = 0; }
    private:
        int& value_;
    };
    int editedValue = 0;
    Fixture f;
    QPointer<QWidget> floating;
    QPointer<QWidget> side;
    QPointer<QUndoGroup> previousGroup;
    {
        ShellWindow shell(f.registry, nullptr);
        shell.show();
        shell.PlaceModule("objects", Placement::Float(50, 50, 500, 400));
        shell.PlaceModule("manager", Placement::Side());
        floating = f.objects->Widget();
        side = f.manager->Widget();
        previousGroup = shell.UndoGroup();
        f.objects->stack.push(new ValueCommand(editedValue));
        QCOMPARE(editedValue, 42);
    }
    // 外壳析构后控件仍由模块持有，随后由模块释放（Fixture 析构时不会重复删除）。
    QVERIFY(!floating.isNull());
    QVERIFY(!side.isNull());
    QVERIFY(floating->parent() == nullptr);
    QVERIFY(side->parent() == nullptr);
    QVERIFY(f.triggers->host == nullptr);
    QVERIFY(f.objects->host == nullptr);
    QVERIFY(f.manager->host == nullptr);
    QVERIFY(f.campaign->host == nullptr);
    QVERIFY(previousGroup.isNull());
    // 旧宿主已释放后，模块持有的撤销栈和命令仍可独立操作真实数据。
    f.objects->stack.undo();
    QCOMPARE(editedValue, 0);
    f.objects->stack.redo();
    QCOMPARE(editedValue, 42);
    // 同一组模块可以安全附着到下一个外壳；已创建控件继续保留。
    ShellWindow next(f.registry, nullptr);
    QVERIFY(f.objects->host == &next);
    next.ActivateModule("objects");
    QCOMPARE(f.objects->created, 1);
    QCOMPARE(f.objects->Widget(), floating.data());
    QVERIFY(next.UndoGroup()->stacks().contains(&f.objects->stack));
    QCOMPARE(next.UndoGroup()->activeStack(), &f.objects->stack);
    QVERIFY(next.UndoAction()->isEnabled());
    next.UndoAction()->trigger();
    QCOMPARE(editedValue, 0);
    QVERIFY(next.RedoAction()->isEnabled());
    next.RedoAction()->trigger();
    QCOMPARE(editedValue, 42);
}

void ShellTests::viewStatesPersistAcrossWindows() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QSettings settings(dir.filePath(QStringLiteral("views.ini")), QSettings::IniFormat);
    {
        Fixture f;
        ShellWindow shell(f.registry, &settings);
        shell.ActivateModule("objects");
        f.objects->edit->setText(QStringLiteral("hpea"));
        shell.close();
    }
    Fixture f;
    ShellWindow shell(f.registry, &settings);
    QCOMPARE(f.objects->restoredState.value("text").toString(), QStringLiteral("hpea"));
    // 禁用模块不接收视图状态。
    QVERIFY(f.campaign->restoredState.isEmpty());
}

// 窄窗口下顶栏不能撑宽主窗口：切换器要么完整显示文字，要么整体切成只显示图标。
void ShellTests::headerFitsNarrowWindows() {
    ModuleRegistry registry;
    for (const auto& [id, label] : std::vector<std::pair<ModuleId, QString>>{
             {"triggers", QStringLiteral("触发器")}, {"objects", QStringLiteral("物体")},
             {"variables", QStringLiteral("变量")}, {"sounds", QStringLiteral("声音")},
             {"assets", QStringLiteral("资源")}, {"manager", QStringLiteral("物体管理")},
             {"campaign", QStringLiteral("战役")}, {"ai", QStringLiteral("AI")}, {"world", QStringLiteral("世界")}}) {
        registry.Register(std::make_unique<FakeModule>(id, label));
    }
    ShellWindow shell(registry, nullptr);
    auto* search = new QLineEdit;
    search->setFixedWidth(230);
    shell.Header()->AddWidget(search);
    for (int i = 0; i < 8; ++i) shell.Header()->AddAction(new QAction(QStringLiteral("A"), &shell));
    for (const int width : {1600, 1100}) {
        shell.resize(width, 800);
        shell.show();
        QTest::qWait(20);
        const QString diag = QStringLiteral("width %1: min %2, header %3")
            .arg(width).arg(shell.minimumSizeHint().width()).arg(shell.Header()->width());
        QVERIFY2(shell.Header()->width() <= shell.width(), qPrintable(diag));
        auto* switcher = shell.Header()->Switcher();
        const QString sw = QStringLiteral(" switcher %1 hint %2 min %3").arg(switcher->width()).arg(switcher->sizeHint().width()).arg(switcher->minimumSizeHint().width());
        const bool compact = switcher->Chip("triggers")->toolButtonStyle() == Qt::ToolButtonIconOnly;
        for (const auto& id : registry.Ids()) {
            auto* chip = switcher->Chip(id);
            QVERIFY2(compact || chip->width() >= chip->TextModeWidth(),
                qPrintable(diag + sw + QStringLiteral(" chip %1 %2 < %3").arg(QString::fromStdString(id)).arg(chip->width()).arg(chip->TextModeWidth())));
        }
    }
}

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
    QApplication application(argc, argv);
    ShellTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ShellTests.moc"
