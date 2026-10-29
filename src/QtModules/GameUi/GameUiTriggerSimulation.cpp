#include "GameUiModule.h"

#include "GameUiAuthoringJson.h"
#include "GameUiTables.h"
#include "GameUiTriggerSimulation.h"
#include "EditorProject.h"
#include "War3MapCore/GameUi/UiTriggers.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <QJsonObject>
#include <QLabel>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTabWidget>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <limits>

// 界面模拟：描述里的模拟计时器按周期到期，执行作者用“界面模拟计时器到期”订阅它的触发器（TriggerSimulation.h），
// 再把这份界面的数值与状态变量读回预览。整图脚本与 F9 检查同一条生成链，按当前（含未保存的）触发器在后台生成。
namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;
using namespace GameUiTables;

QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

constexpr Column kTimerColumns[] = {{"id", QT_TR_NOOP("编号")}, {"label", QT_TR_NOOP("显示名")}, {"period", QT_TR_NOOP("周期（秒）")},
    {"repeat", QT_TR_NOOP("重复")}, {"triggers", QT_TR_NOOP("触发器")}};
constexpr Column kStartColumns[] = {{"value", QT_TR_NOOP("数值")}, {"start", QT_TR_NOOP("起始值")}};

constexpr int kClockMilliseconds = 16;
constexpr double kMaxFrameSeconds = 0.25;  // 卡顿后一帧最多补这么久
constexpr int kMaxFiresPerFrame = 256;
constexpr std::size_t kMaxProblems = 50;

// 每个数值占的起始值键：单值为编号，数组为 编号[下标]。
std::vector<std::pair<std::string, const UiValue*>> StartKeys(const UiDescription& description) {
    std::vector<std::pair<std::string, const UiValue*>> keys;
    for (const auto& value : description.values) {
        if (value.size == 0) keys.emplace_back(value.id, &value);
        for (std::uint32_t i = 0; i < value.size; ++i) keys.emplace_back(value.id + "[" + std::to_string(i) + "]", &value);
    }
    return keys;
}

// 起始值单元写回 simulation.values：空为删除；数值须能读成数。
bool PutStart(QJsonObject& values, const QString& key, const UiValue& value, const QTableWidgetItem& item, QString* error) {
    if (value.type == UiValueType::Boolean) {
        values.insert(key, item.checkState() == Qt::Checked);
        return true;
    }
    const QString text = item.text().trimmed();
    if (text.isEmpty()) {
        values.remove(key);
        return true;
    }
    if (value.type == UiValueType::String) {
        values.insert(key, text);
        return true;
    }
    bool ok = false;
    const double number = text.toDouble(&ok);
    if (!ok) {
        *error = QObject::tr("%1 不是数值").arg(text);
        return false;
    }
    values.insert(key, number);
    return true;
}

void AddProblem(std::vector<Diagnostic>& problems, Diagnostic diagnostic) {
    const bool seen = std::any_of(problems.begin(), problems.end(),
        [&](const Diagnostic& old) { return old.code == diagnostic.code && old.message == diagnostic.message; });
    if (!seen && problems.size() < kMaxProblems) problems.push_back(std::move(diagnostic));
}

Diagnostic Problem(DiagnosticSeverity severity, const char* code, const QString& message) {
    return Diagnostic{severity, "GameUi", code, message.toStdString(), std::nullopt};
}

// 问题里的“触发器 gg_trg_…”换成作者在触发器编辑器里看到的名字。
Diagnostic Named(const GameUiTriggerSimulation& sim, Diagnostic diagnostic) {
    const std::string lead = "触发器 ";
    const std::size_t start = diagnostic.message.find(lead + "gg_trg_");
    if (start == std::string::npos) return diagnostic;
    const std::size_t from = start + lead.size();
    const std::size_t end = diagnostic.message.find("：", from);
    if (end == std::string::npos) return diagnostic;
    const auto name = sim.names.find(std::string_view(diagnostic.message).substr(from, end - from));
    if (name != sim.names.end()) diagnostic.message.replace(from - 1, end - from + 1, "“" + name->second + "”");
    return diagnostic;
}

} // namespace

bool GameUiModule::StartTriggerSimulation(const QString& id, QString* error) {
    if (!BeginTriggerSimulation(id, error)) return false;
    auto& sim = *triggerSim_;
    sim.running = true;
    if (sim.simulator) {
        sim.wall.restart();
        sim.clock.start(kClockMilliseconds);
    }
    UpdateTriggerSimulationPage();
    emit TriggerSimulationChanged();
    return true;
}

void GameUiModule::PauseTriggerSimulation() {
    auto& sim = *triggerSim_;
    sim.running = false;
    sim.stepPending = false;
    sim.clock.stop();
    UpdateTriggerSimulationPage();
    emit TriggerSimulationChanged();
}

bool GameUiModule::StepTriggerSimulation(const QString& id, QString* error) {
    if (!BeginTriggerSimulation(id, error)) return false;
    auto& sim = *triggerSim_;
    sim.running = false;
    sim.clock.stop();
    if (sim.simulator) AdvanceTriggerSimulation(std::nullopt);
    else sim.stepPending = true;
    UpdateTriggerSimulationPage();
    emit TriggerSimulationChanged();
    return true;
}

void GameUiModule::ResetTriggerSimulation() {
    const QString id = StopTriggerSimulation();
    // 预览回到模拟的起始值与基础状态。
    if (!id.isEmpty()) ShowSimulationStart(id);
    UpdateProblems();
    UpdateTriggerSimulationPage(true);
    emit TriggerSimulationChanged();
}

QString GameUiModule::StopTriggerSimulation() {
    auto& sim = *triggerSim_;
    QString id = std::exchange(sim.description, QString());
    sim.clock.stop();
    ++sim.generation;
    sim.simulator.reset();
    sim.preparing = sim.running = sim.stepPending = false;
    sim.time = 0.0;
    sim.fired = 0;
    sim.due.clear();
    sim.finished.clear();
    sim.intercepted.clear();
    sim.diagnostics.clear();
    return id;
}

GameUiModule::TriggerSimulationStatus GameUiModule::TriggerSimulationState() const {
    const auto& sim = *triggerSim_;
    TriggerSimulationStatus status;
    status.description = sim.description;
    status.preparing = sim.preparing;
    status.running = sim.running;
    status.ready = sim.simulator.has_value();
    status.time = sim.time;
    status.fired = sim.fired;
    if (sim.simulator) status.subscriptions = sim.simulator->Subscriptions();
    return status;
}

const std::vector<Diagnostic>& GameUiModule::TriggerSimulationProblems() const { return triggerSim_->diagnostics; }

bool GameUiModule::BeginTriggerSimulation(const QString& id, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const UiDescription* description = Description(id);
    if (!description) return fail(tr("没有界面描述 %1").arg(id));
    if (description->simulation.timers.empty()) return fail(tr("界面 %1 没有模拟计时器").arg(id));
    auto& sim = *triggerSim_;
    if (sim.description != id) {
        ResetTriggerSimulation();
        sim.description = id;
        ShowSimulationStart(id);
    }
    if (sim.simulator || sim.preparing) return true;
    return PrepareTriggerSimulation(id, error);
}

void GameUiModule::ShowSimulationStart(const QString& id) {
    const UiDescription* description = Description(id);
    if (!description) return;
    UiInstanceState start;
    for (const auto& [key, text] : description->simulation.values) start.values[key] = text;
    descriptionStates_[id.toStdString()] = std::move(start);
    RebuildScene();
}

std::vector<std::string> GameUiModule::PlanTriggerSimulation(const UiDescription& description, const Trigger::TriggerDocument& document) {
    // 生成的界面触发器在模拟里不执行（它们调游戏的界面函数）：“创建”“更新”只读回数值，状态触发器切换预览状态。
    auto& sim = *triggerSim_;
    sim.intercepted.clear();
    sim.names.clear();
    for (const auto& record : document.triggers) sim.names[Trigger::TriggerDirectoryEditor::TriggerSymbol(record.name)] = record.name.text;
    const auto intercept = [&](const std::string& name, std::optional<std::string> state) {
        for (const auto& record : document.triggers) {
            if (record.name.text == name) sim.intercepted[Trigger::TriggerDirectoryEditor::TriggerSymbol(record.name)] = state;
        }
    };
    intercept(UiTriggerName(description, "创建", "Create"), std::nullopt);
    intercept(UiTriggerName(description, "更新", "Update"), std::nullopt);
    if (!description.states.empty()) intercept(UiTriggerName(description, "基础状态", "BaseState"), std::string());
    for (const auto& state : description.states) intercept(UiStateTriggerName(description, state), state.id);
    std::vector<std::string> names;
    for (const auto& entry : sim.intercepted) names.push_back(entry.first);
    return names;
}

void RunTriggerSimulationJob(GameUiTriggerSimulationJob& job, std::vector<MapScriptText> scripts, std::vector<std::string> intercepted) {
    auto prepared = TriggerSimulator::Prepare(std::move(scripts));
    job.diagnostics.insert(job.diagnostics.end(), prepared.diagnostics.begin(), prepared.diagnostics.end());
    if (!prepared.Succeeded() || !prepared.value) return;
    prepared.value->Intercept(std::move(intercepted));
    job.init = prepared.value->Initialize();
    job.simulator = std::move(prepared.value);
}

bool GameUiModule::PrepareTriggerSimulation(const QString& id, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    const UiDescription* description = Description(id);
    if (!description) return fail(tr("没有界面描述 %1").arg(id));
    if (!project_.triggerDocument_ || !project_.uiCatalog_) return fail(tr("当前地图没有可编辑的触发器"));
    auto snapshot = project_.CaptureCodeCheckSnapshot();
    if (!snapshot || !snapshot->catalog) return fail(tr("当前地图没有可生成脚本的完整来源"));
    auto document = std::make_shared<const Trigger::TriggerDocument>(*project_.triggerDocument_);
    auto names = PlanTriggerSimulation(*description, *document);

    auto& sim = *triggerSim_;
    auto job = std::make_shared<GameUiTriggerSimulationJob>();
    sim.preparing = true;
    const auto generation = ++sim.generation;
    auto* worker = QThread::create([snapshot, document, cache = project_.SharedCodeCheckCache(), names = std::move(names), job]() mutable {
        auto built = Services::BuildProjectJassScript(snapshot, *cache, document);
        job->diagnostics = built.diagnostics;
        if (!built.Succeeded() || !built.value) return;
        std::vector<MapScriptText> scripts;
        scripts.push_back({"common.j", ScriptLanguage::Jass, std::move(built.value->common)});
        scripts.push_back({"blizzard.j", ScriptLanguage::Jass, std::move(built.value->blizzard)});
        scripts.push_back({"war3map.j", ScriptLanguage::Jass, std::move(built.value->map)});
        RunTriggerSimulationJob(*job, std::move(scripts), std::move(names));
    });
    connect(worker, &QThread::finished, this, [this, generation, job] { FinishTriggerSimulation(generation, *job); });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
    UpdateTriggerSimulationPage();
    return true;
}

bool GameUiModule::PrepareTriggerSimulationForTesting(const QString& id, std::vector<MapScriptText> scripts, QString* error) {
    const UiDescription* description = Description(id);
    if (!description || description->simulation.timers.empty() || !project_.triggerDocument_) {
        if (error) *error = tr("界面 %1 不能模拟").arg(id);
        return false;
    }
    ResetTriggerSimulation();
    auto& sim = *triggerSim_;
    sim.description = id;
    ShowSimulationStart(id);
    GameUiTriggerSimulationJob job;
    RunTriggerSimulationJob(job, std::move(scripts), PlanTriggerSimulation(*description, *project_.triggerDocument_));
    FinishTriggerSimulation(++sim.generation, job);
    if (!sim.simulator && error) *error = sim.diagnostics.empty() ? tr("准备失败") : Text(sim.diagnostics.front().message);
    return sim.simulator.has_value();
}

void GameUiModule::FinishTriggerSimulation(std::uint64_t generation, GameUiTriggerSimulationJob& job) {
    auto& sim = *triggerSim_;
    if (generation != sim.generation) return;
    sim.preparing = false;
    for (auto& diagnostic : job.diagnostics) AddProblem(sim.diagnostics, std::move(diagnostic));
    // 初始化跳过的语句（多是模拟里不能执行的游戏函数）只记条数与第一条。
    if (!job.init.diagnostics.empty()) {
        AddProblem(sim.diagnostics, Problem(DiagnosticSeverity::Info, "AUI_SIM_INIT_SKIPPED",
            tr("模拟初始化跳过 %1 条语句，第一条：%2").arg(job.init.diagnostics.size()).arg(Text(job.init.diagnostics.front().message))));
    }
    const UiDescription* description = Description(sim.description);
    if (!job.simulator || !description) {
        sim.running = sim.stepPending = false;
        UpdateProblems();
        UpdateTriggerSimulationPage();
        emit TriggerSimulationChanged();
        return;
    }
    sim.simulator = std::move(job.simulator);
    for (const auto& [key, text] : description->simulation.values) {
        const auto open = key.find('[');
        const std::string name = "udg_" + UiValueVariableName(description->id, key.substr(0, open));
        const std::optional<std::uint32_t> index = open == std::string::npos ? std::nullopt
            : std::optional<std::uint32_t>(static_cast<std::uint32_t>(std::stoul(key.substr(open + 1))));
        (void)sim.simulator->SetGlobal(name, index, text);
    }
    if (!sim.simulator->Global("udg_" + UiCreatedVariableName(description->id))) {
        AddProblem(sim.diagnostics, Problem(DiagnosticSeverity::Warning, "AUI_SIM_NOT_GENERATED",
            tr("界面 %1 还没有生成触发器：模拟只执行触发器，读不回数值").arg(sim.description)));
    }
    const auto subscriptions = sim.simulator->Subscriptions();
    if (std::none_of(subscriptions.begin(), subscriptions.end(), [&](const auto& entry) { return entry.first == description->id; })) {
        AddProblem(sim.diagnostics, Problem(DiagnosticSeverity::Warning, "AUI_SIM_NO_SUBSCRIBER",
            tr("没有触发器用“界面模拟计时器到期”订阅界面 %1 的计时器").arg(sim.description)));
    }
    for (const auto& timer : description->simulation.timers) sim.due[timer.id] = timer.period;
    ApplyTriggerSimulationValues({});
    if (sim.stepPending) {
        sim.stepPending = false;
        AdvanceTriggerSimulation(std::nullopt);
    }
    if (sim.running) {
        sim.wall.restart();
        sim.clock.start(kClockMilliseconds);
    }
    UpdateProblems();
    UpdateTriggerSimulationPage(true);
    emit TriggerSimulationChanged();
}

void GameUiModule::AdvanceTriggerSimulation(std::optional<double> until) {
    auto& sim = *triggerSim_;
    const UiDescription* description = Description(sim.description);
    if (!sim.simulator || !description) return;
    const auto& timers = description->simulation.timers;
    // 计时器随描述增删：新加的从现在起计时，删掉的不再到期。
    std::erase_if(sim.due, [&](const auto& entry) {
        return std::none_of(timers.begin(), timers.end(), [&](const UiSimulationTimer& timer) { return timer.id == entry.first; });
    });
    for (const auto& timer : timers) {
        if (!sim.due.contains(timer.id) && !sim.finished.contains(timer.id)) sim.due[timer.id] = sim.time + timer.period;
    }
    std::vector<std::string> intercepted;
    std::optional<double> instant;  // 单步：只执行最早那一刻到期的
    bool stopped = false;
    int fires = 0;
    for (; fires < kMaxFiresPerFrame; ++fires) {
        const UiSimulationTimer* next = nullptr;
        double at = std::numeric_limits<double>::infinity();
        for (const auto& timer : timers) {
            const auto due = sim.due.find(timer.id);
            if (due != sim.due.end() && due->second < at) {
                at = due->second;
                next = &timer;
            }
        }
        if (!next || (until && at > *until)) break;
        if (!until) {
            if (instant && at > *instant + 1e-9) break;
            instant = at;
        }
        sim.time = at;
        auto run = sim.simulator->Fire(description->id, next->id);
        ++sim.fired;
        if (next->repeat) {
            sim.due[next->id] = at + next->period;
        } else {
            sim.due.erase(next->id);
            sim.finished.insert(next->id);
        }
        intercepted.insert(intercepted.end(), run.intercepted.begin(), run.intercepted.end());
        for (auto& diagnostic : run.diagnostics) AddProblem(sim.diagnostics, Named(sim, std::move(diagnostic)));
        if (!run.completed) {
            stopped = true;
            break;
        }
    }
    if (until && !stopped && fires < kMaxFiresPerFrame) sim.time = std::max(sim.time, *until);
    if (stopped) {
        // 触发器停下时暂停，作者看问题列表里的位置。
        sim.running = false;
        sim.clock.stop();
    }
    ApplyTriggerSimulationValues(intercepted);
    if (stopped) UpdateProblems();
}

void GameUiModule::ApplyTriggerSimulationValues(const std::vector<std::string>& intercepted) {
    auto& sim = *triggerSim_;
    const UiDescription* description = Description(sim.description);
    if (!sim.simulator || !description) return;
    const auto& preview = descriptionStates_[sim.description.toStdString()];
    std::map<std::string, std::string> values;
    for (const auto& [key, value] : StartKeys(*description)) {
        const auto open = key.find('[');
        const std::string name = "udg_" + UiValueVariableName(description->id, value->id);
        const auto text = open == std::string::npos ? sim.simulator->Global(name)
            : sim.simulator->Global(name, static_cast<std::uint32_t>(std::stoul(key.substr(open + 1))));
        if (!text) continue;
        const auto normalized = NormalizeUiValue(value->type, *text);
        if (!normalized) continue;
        const auto old = preview.values.find(key);
        if (old == preview.values.end() || old->second != *normalized) values[key] = *normalized;
    }
    std::optional<std::string> state;
    if (!description->states.empty()) state = sim.simulator->Global("udg_" + UiStateVariableName(description->id));
    for (const auto& name : intercepted) {
        const auto found = sim.intercepted.find(name);
        if (found != sim.intercepted.end() && found->second) state = found->second;
    }
    if (!values.empty()) (void)SetDescriptionValues(sim.description, values);
    if (state && *state != preview.state) {
        QString error;
        if (!SetDescriptionState(sim.description, Text(*state), &error)) {
            AddProblem(sim.diagnostics, Problem(DiagnosticSeverity::Warning, "AUI_SIM_STATE", error));
        }
    }
}

void GameUiModule::ChangeSimulation(const std::function<bool(QJsonObject&, QString*)>& change, const QString& what) {
    const QString id = descriptionPanelFor_;
    const UiDescription* description = Description(id);
    if (!description) return;
    QJsonObject simulation = DescriptionObject(*description).value(QStringLiteral("simulation")).toObject();
    const QJsonObject original = simulation;
    QString error;
    if (!change(simulation, &error)) {
        if (!error.isEmpty()) {
            ReportAuthoring(error);
            QTimer::singleShot(0, this, [this] { UpdateTriggerSimulationPage(true); });
        }
        return;
    }
    if (simulation == original) return;
    CommitDescriptionEdits(id, {QJsonObject{{"op", "set_simulation"}, {"simulation", simulation}}}, tr("界面 %1：%2").arg(id, what));
}

void GameUiModule::BuildTriggerSimulationPage(QTabWidget* tabs) {
    triggerSim_->clock.setTimerType(Qt::PreciseTimer);
    connect(&triggerSim_->clock, &QTimer::timeout, this, [this] {
        auto& sim = *triggerSim_;
        const double elapsed = std::min(static_cast<double>(sim.wall.restart()) / 1000.0, kMaxFrameSeconds);
        AdvanceTriggerSimulation(sim.time + elapsed);
        UpdateTriggerSimulationPage();
    });

    auto* page = new QWidget(tabs);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(4);
    auto* controls = new QHBoxLayout;
    simRun_ = Button(page, "gameUiSimulationRun", tr("开始"));
    simStep_ = Button(page, "gameUiSimulationStep", tr("单步"));
    simReset_ = Button(page, "gameUiSimulationReset", tr("重置"));
    simStatus_ = new QLabel(page);
    simStatus_->setObjectName(QStringLiteral("gameUiSimulationStatus"));
    for (auto* button : {simRun_, simStep_, simReset_}) controls->addWidget(button);
    controls->addStretch(1);
    controls->addWidget(simStatus_);
    layout->addLayout(controls);

    simTimers_ = MakeTable(page, "gameUiSimulationTimers", kTimerColumns);
    simStarts_ = MakeTable(page, "gameUiSimulationValues", kStartColumns);
    auto* addTimer = Button(page, "gameUiAddSimulationTimer", tr("添加计时器"));
    auto* removeTimer = Button(page, "gameUiRemoveSimulationTimer", tr("删除计时器"));
    auto* split = new QSplitter(Qt::Vertical, page);
    split->setChildrenCollapsible(false);
    split->addWidget(TablePage(split, simTimers_, {addTimer, removeTimer}));
    simStarts_->setParent(split);
    split->addWidget(simStarts_);
    layout->addWidget(split, 1);
    tabs->addTab(page, tr("模拟"));

    const auto report = [this](bool ok, const QString& error) {
        if (!ok) ReportAuthoring(error);
    };
    connect(simRun_, &QPushButton::clicked, this, [this, report] {
        if (triggerSim_->running && triggerSim_->description == descriptionPanelFor_) {
            PauseTriggerSimulation();
            return;
        }
        QString error;
        report(StartTriggerSimulation(descriptionPanelFor_, &error), error);
    });
    connect(simStep_, &QPushButton::clicked, this, [this, report] {
        QString error;
        report(StepTriggerSimulation(descriptionPanelFor_, &error), error);
    });
    connect(simReset_, &QPushButton::clicked, this, [this] { ResetTriggerSimulation(); });

    connect(addTimer, &QPushButton::clicked, this, [this] {
        ChangeSimulation([](QJsonObject& simulation, QString*) {
            QJsonArray timers = simulation.value("timers").toArray();
            QString id;
            for (int n = 1; id.isEmpty(); ++n) {
                const QString candidate = QStringLiteral("timer%1").arg(n);
                if (std::none_of(timers.begin(), timers.end(), [&](const QJsonValue& timer) { return timer.toObject().value("id").toString() == candidate; }))
                    id = candidate;
            }
            timers.append(QJsonObject{{"id", id}, {"period", 1}});
            simulation.insert("timers", timers);
            return true;
        }, tr("添加模拟计时器"));
    });
    connect(removeTimer, &QPushButton::clicked, this, [this] {
        const int row = simTimers_->currentRow();
        if (row < 0) return;
        ChangeSimulation([row](QJsonObject& simulation, QString*) {
            QJsonArray timers = simulation.value("timers").toArray();
            if (row >= timers.size()) return false;
            timers.removeAt(row);
            if (timers.isEmpty()) simulation.remove("timers");
            else simulation.insert("timers", timers);
            return true;
        }, tr("删除模拟计时器"));
    });
    connect(simTimers_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        const int row = item->row();
        const QString key = ColumnKey(kTimerColumns, item->column());
        const bool checked = item->checkState() == Qt::Checked;
        const QString text = item->text().trimmed();
        ChangeSimulation([row, key, checked, text](QJsonObject& simulation, QString* error) {
            QJsonArray timers = simulation.value("timers").toArray();
            if (row < 0 || row >= timers.size()) return false;
            QJsonObject timer = timers[row].toObject();
            if (key == QLatin1String("repeat")) {
                if (checked) timer.remove(key);
                else timer.insert(key, false);
            } else if (key == QLatin1String("period")) {
                bool ok = false;
                const double period = text.toDouble(&ok);
                if (!ok) {
                    *error = QObject::tr("%1 不是数值").arg(text);
                    return false;
                }
                timer.insert(key, period);
            } else if (text.isEmpty() && key == QLatin1String("label")) {
                timer.remove(key);
            } else {
                timer.insert(key, text);
            }
            timers[row] = timer;
            simulation.insert("timers", timers);
            return true;
        }, tr("改模拟计时器的%1").arg(ColumnLabel(kTimerColumns, item->column())));
    });
    connect(simStarts_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem* item) {
        const UiDescription* description = Description(descriptionPanelFor_);
        if (!description || item->column() != 1) return;
        const auto keys = StartKeys(*description);
        const int row = item->row();
        if (row < 0 || static_cast<std::size_t>(row) >= keys.size()) return;
        const QString key = Text(keys[static_cast<std::size_t>(row)].first);
        const UiValue& value = *keys[static_cast<std::size_t>(row)].second;
        ChangeSimulation([&, key](QJsonObject& simulation, QString* error) {
            QJsonObject values = simulation.value("values").toObject();
            if (!PutStart(values, key, value, *item, error)) return false;
            if (values.isEmpty()) simulation.remove("values");
            else simulation.insert("values", values);
            return true;
        }, tr("改 %1 的模拟起始值").arg(key));
    });
}

void GameUiModule::UpdateTriggerSimulationPage(bool refill) {
    if (!simTimers_) return;
    const UiDescription* description = Description(descriptionPanelFor_);
    const auto& sim = *triggerSim_;
    const bool mine = description && sim.description == descriptionPanelFor_;
    simRun_->setText(mine && sim.running ? tr("暂停") : tr("开始"));
    const bool canRun = description && !description->simulation.timers.empty();
    simRun_->setEnabled(canRun);
    simStep_->setEnabled(canRun && !(mine && sim.preparing));
    simReset_->setEnabled(mine);
    if (!mine) simStatus_->clear();
    else if (sim.preparing) simStatus_->setText(tr("正在生成脚本"));
    else simStatus_->setText(tr("%1 秒 · 到期 %2 次").arg(QString::number(sim.time, 'f', 2)).arg(sim.fired));
    if (!description || !refill) return;
    const bool raw = descriptionRaw_;
    SetHeaders(simTimers_, kTimerColumns, raw);
    SetHeaders(simStarts_, kStartColumns, raw);
    const auto subscriptions = mine && sim.simulator ? sim.simulator->Subscriptions() : std::vector<std::pair<std::string, std::string>>{};
    {
        const QSignalBlocker block(simTimers_);
        simTimers_->setRowCount(0);
        for (const auto& timer : description->simulation.timers) {
            const int row = simTimers_->rowCount();
            simTimers_->insertRow(row);
            simTimers_->setItem(row, 0, Cell(Text(timer.id)));
            simTimers_->setItem(row, 1, Cell(Text(timer.label)));
            simTimers_->setItem(row, 2, Cell(QString::number(timer.period, 'g', 15)));
            simTimers_->setItem(row, 3, CheckCell(timer.repeat));
            const auto count = std::count_if(subscriptions.begin(), subscriptions.end(),
                [&](const auto& entry) { return entry.first == description->id && entry.second == timer.id; });
            simTimers_->setItem(row, 4, Cell(mine && sim.simulator ? QString::number(count) : QString(), false));
        }
        FitColumns(simTimers_);
    }
    {
        const QSignalBlocker block(simStarts_);
        simStarts_->setRowCount(0);
        for (const auto& [key, value] : StartKeys(*description)) {
            const int row = simStarts_->rowCount();
            simStarts_->insertRow(row);
            const QString label = raw || value->label.empty() ? Text(key) : QStringLiteral("%1  %2").arg(Text(value->label), Text(key));
            simStarts_->setItem(row, 0, Cell(label, false));
            const auto start = description->simulation.values.find(key);
            const bool has = start != description->simulation.values.end();
            if (value->type == UiValueType::Boolean) {
                simStarts_->setItem(row, 1, CheckCell((has ? start->second : value->defaultValue) == "true"));
            } else {
                simStarts_->setItem(row, 1, Cell(has ? Text(start->second) : QString()));
            }
        }
        FitColumns(simStarts_);
    }
}

} // namespace War3Map::Modules
