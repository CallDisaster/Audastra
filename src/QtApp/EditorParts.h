#pragma once

#include "EditorProject.h"
#include "Modules/ModuleRegistry.h"

#include <QSettings>

#include <memory>

namespace War3Map::Modules {
class AssetsModule;
class CampaignModule;
class ManagerModule;
class MapSettingsModule;
class MapEntitiesModule;
class SoundsModule;
class ObjectModule;
class TriggerModule;
class VariablesModule;
class WorldModule;
} // namespace War3Map::Modules

// 组装顺序：工程 → 撤销栈 → 模块注册表，都必须先于主窗口外壳构造（外壳构造时读取注册表并载入布局）。
// 析构顺序相反：外壳先交还模块控件，然后模块释放控件，最后释放工程文档。
struct EditorParts {
    explicit EditorParts(bool persistentSettings);
    ~EditorParts();
    EditorParts(const EditorParts&) = delete;
    EditorParts& operator=(const EditorParts&) = delete;

    std::unique_ptr<War3Map::QtProject::EditorProject> project_;
    // 本机布局设置；启动探针与测试不读写用户设置。
    std::unique_ptr<QSettings> shellSettings_;
    War3Map::Shell::ModuleRegistry modules_;
    // 注册表拥有模块；这里只保留具体类型的访问入口。
    War3Map::Modules::TriggerModule* triggersModule_ = nullptr;
    War3Map::Modules::ObjectModule* objectsModule_ = nullptr;
    War3Map::Modules::VariablesModule* variablesModule_ = nullptr;
    War3Map::Modules::AssetsModule* assetsModule_ = nullptr;
    War3Map::Modules::CampaignModule* campaignModule_ = nullptr;
    War3Map::Modules::ManagerModule* managerModule_ = nullptr;
    War3Map::Modules::MapSettingsModule* settingsModule_ = nullptr;
    War3Map::Modules::MapEntitiesModule* entitiesModule_ = nullptr;
    War3Map::Modules::SoundsModule* soundsModule_ = nullptr;
    War3Map::Modules::WorldModule* worldModule_ = nullptr;
};
