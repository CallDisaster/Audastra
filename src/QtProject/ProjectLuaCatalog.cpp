#include "EditorProject.h"
#include "AssetOwnedPayload.h"
#include "ProjectLuaModules.h"

#include "War3MapCore/Lua/LuaApiCatalog.h"
#include "War3MapCore/Services/LuaRuntimeCatalog.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

namespace War3Map::QtProject {

std::shared_ptr<const Lua::ApiCatalog> EditorProject::LuaRuntimeCatalog() {
    if (luaRuntimeScriptsRoot_ != war3GameRoot_) {
        luaRuntimeScriptsRoot_ = war3GameRoot_;
        luaRuntimeScripts_.reset();
        luaRuntimeCatalog_.reset();
        if (!war3GameRoot_.empty()) {
            auto scripts = Services::ReadGameLuaRuntimeScripts(war3GameRoot_);
            if (scripts.Succeeded() && scripts.value)
                luaRuntimeScripts_ = std::make_shared<const Services::LuaRuntimeScripts>(std::move(*scripts.value));
        }
    }
    // 地图变量经 jass.globals 读取，名字是 udg_ 前缀的 JASS 全局。
    std::vector<std::pair<std::string, std::string>> globals;
    std::string key;
    if (triggerDocument_) {
        for (const auto& variable : triggerDocument_->variables) {
            auto type = variable.typeName.text + (variable.isArray ? " array" : "");
            key += variable.name.text + ':' + type + '\n';
            globals.emplace_back("udg_" + variable.name.text, std::move(type));
        }
    }
    if (!luaRuntimeCatalog_ || key != luaRuntimeCatalogKey_) {
        luaRuntimeCatalog_ = Services::BuildLuaRuntimeCatalog(luaRuntimeScripts_.get(), globals);
        luaRuntimeCatalogKey_ = std::move(key);
    }
    return luaRuntimeCatalog_;
}

std::string LuaModuleName(const QString& path) {
    if (!path.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive)) return {};
    auto name = path.left(path.size() - 4);
    name.replace('\\', '.').replace('/', '.');
    return name.toStdString();
}

std::shared_ptr<const LuaModuleTexts> CaptureLuaModules(const EditorProject& project, const QString& current) {
    constexpr std::size_t kTotalBytes = 16 * 1024 * 1024;
    auto modules = std::make_shared<LuaModuleTexts>();
    std::size_t total = 0;
    for (const auto& entry : project.AssetEntries()) {
        if (entry.ambiguous || EditorProject::AssetKey(entry.path) == EditorProject::AssetKey(current)) continue;
        auto name = LuaModuleName(entry.path);
        if (name.empty()) continue;
        const auto read = ReadOwnedAsset(project, entry.path);
        if (!read.Succeeded() || !read.value->bytes || total + read.value->bytes->size() > kTotalBytes) continue;
        total += read.value->bytes->size();
        const auto& bytes = *read.value->bytes;
        modules->emplace(std::move(name), std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    }
    return modules;
}

} // namespace War3Map::QtProject
