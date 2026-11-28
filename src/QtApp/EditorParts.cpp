#include "EditorParts.h"

#include "AssetsModule.h"
#include "CampaignModule.h"
#include "GameUiModule.h"
#include "ManagerModule.h"
#include "MapEntitiesModule.h"
#include "MapSettingsModule.h"
#include "SoundsModule.h"
#include "Modules/PlaceholderModule.h"
#include "ObjectModule.h"
#include "ObjectResourcePickerAdapter.h"
#include "TriggerModule.h"
#include "VariablesModule.h"
#include "WorldModule.h"

#include <QCoreApplication>
#include <QTimer>
#include <QUndoStack>

namespace {

template <class Module>
Module* Add(War3Map::Shell::ModuleRegistry& registry, std::unique_ptr<Module> module) {
    Module* raw = module.get();
    registry.Register(std::move(module));
    return raw;
}

} // namespace

EditorParts::EditorParts(bool persistentSettings)
    : project_(std::make_unique<War3Map::QtProject::EditorProject>()) {
    // 撤销栈归工程所有；模块与外壳只借用。
    project_->triggerUndoStack_ = new QUndoStack(project_.get());
    project_->triggerUndoStack_->setUndoLimit(64);
    for (auto& stack : project_->objectUndoStacks_) {
        stack = new QUndoStack(project_.get());
        stack->setUndoLimit(64);
    }
    if (persistentSettings) {
        shellSettings_ = std::make_unique<QSettings>();
    }
    // 登记顺序即顶栏切换器顺序。
    using War3Map::Shell::PlaceholderModule;
    const QString pending = QCoreApplication::translate("EditorParts", "原生模块尚未接通");
    const auto key = [](Qt::Key k) { return QKeySequence(Qt::ALT | k); };
    // 资源选择器的索引全工程共用一份：物编、界面编辑与触发器参数打开的是同一个缓存。
    const auto resourcePickers = War3Map::QtApp::MakeObjectResourcePickerFactory(*project_);
    triggersModule_ = Add(modules_, std::make_unique<War3Map::Modules::TriggerModule>(*project_));
    triggersModule_->SetResourcePickerFactory(resourcePickers);
    objectsModule_ = Add(modules_, std::make_unique<War3Map::Modules::ObjectModule>(*project_));
    objectsModule_->SetResourcePickerFactory(resourcePickers);
    variablesModule_ = Add(modules_, std::make_unique<War3Map::Modules::VariablesModule>(*project_));
    soundsModule_ = Add(modules_, std::make_unique<War3Map::Modules::SoundsModule>(*project_));
    assetsModule_ = Add(modules_, std::make_unique<War3Map::Modules::AssetsModule>(*project_));
    managerModule_ = Add(modules_, std::make_unique<War3Map::Modules::ManagerModule>(*project_));
    settingsModule_ = Add(modules_, std::make_unique<War3Map::Modules::MapSettingsModule>(*project_));
    entitiesModule_ = Add(modules_, std::make_unique<War3Map::Modules::MapEntitiesModule>(*project_));
    campaignModule_ = Add(modules_, std::make_unique<War3Map::Modules::CampaignModule>(*project_));
    modules_.Register(std::make_unique<PlaceholderModule>("ai",
        QCoreApplication::translate("EditorParts", "AI"), "lucide/shield", key(Qt::Key_8), pending));
    worldModule_ = Add(modules_, std::make_unique<War3Map::Modules::WorldModule>(*project_));
    worldModule_->SetResourcePickerFactory(resourcePickers);
    gameUiModule_ = Add(modules_, std::make_unique<War3Map::Modules::GameUiModule>(*project_));
    gameUiModule_->SetResourcePickerFactory(resourcePickers);
    // 打开地图（编辑基线重置）后稍等界面安定，再在后台建资源索引；启动探针与测试不预热。
    if (persistentSettings) {
        auto* project = project_.get();
        auto* prewarmDelay = new QTimer(project);
        prewarmDelay->setSingleShot(true); prewarmDelay->setInterval(1500);
        QObject::connect(prewarmDelay, &QTimer::timeout, project,
            [project, prewarm = War3Map::QtApp::MakeObjectResourcePrewarm(resourcePickers, project)] {
                if (project->archiveSession_ || project->directorySnapshot_) prewarm();
            });
        QObject::connect(project, &War3Map::QtProject::EditorProject::EditTrackingBaselineChanged,
            prewarmDelay, qOverload<>(&QTimer::start));
    }
}

EditorParts::~EditorParts() = default;
