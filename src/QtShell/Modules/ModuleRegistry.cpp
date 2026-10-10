#include "Modules/ModuleRegistry.h"

namespace War3Map::Shell {

bool ModuleRegistry::Register(std::unique_ptr<EditorModule> module) {
    if (module == nullptr || module->Info().id.empty() || Find(module->Info().id) != nullptr) {
        return false;
    }
    modules_.push_back(std::move(module));
    return true;
}

EditorModule* ModuleRegistry::Find(const ModuleId& id) const {
    for (const auto& module : modules_) {
        if (module->Info().id == id) {
            return module.get();
        }
    }
    return nullptr;
}

std::vector<ModuleId> ModuleRegistry::Ids() const {
    std::vector<ModuleId> ids;
    ids.reserve(modules_.size());
    for (const auto& module : modules_) {
        ids.push_back(module->Info().id);
    }
    return ids;
}

std::vector<ModuleId> ModuleRegistry::AvailableIds() const {
    std::vector<ModuleId> ids;
    for (const auto& module : modules_) {
        const ModuleInfo info = module->Info();
        if (info.available) {
            ids.push_back(info.id);
        }
    }
    return ids;
}

} // namespace War3Map::Shell
