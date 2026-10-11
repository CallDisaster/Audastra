#include "EditorParts.h"

#include "AssetsModule.h"
#include "CampaignModule.h"
#include "ManagerModule.h"
#include "MapEntitiesModule.h"
#include "MapSettingsModule.h"
#include "SoundsModule.h"
#include "Modules/PlaceholderModule.h"
#include "ObjectModule.h"
#include "TriggerModule.h"
#include "VariablesModule.h"
#include "WorldModule.h"

#include <QCoreApplication>
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
    triggersModule_ = Add(modules_, std::make_unique<War3Map::Modules::TriggerModule>(*project_));
    objectsModule_ = Add(modules_, std::make_unique<War3Map::Modules::ObjectModule>(*project_));
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
}

EditorParts::~EditorParts() = default;
