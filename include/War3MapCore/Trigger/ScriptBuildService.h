#pragma once

#include "War3MapCore/Trigger/JassCompiler.h"

namespace War3Map { class MapDocument; }

namespace War3Map::Trigger {

enum class ScriptBackend { Jass, VJass, Lua };

struct ScriptBuildCheck {
    ScriptBackend backend = ScriptBackend::Jass;
    JassCompileResult triggerFragment;
    std::vector<Diagnostic> diagnostics;
    bool triggerFragmentAvailable = false;
    bool mapSourceDataAvailable = false;
    bool mapInitializationAvailable = false;
    bool externalCompilerValidated = false;

    [[nodiscard]] bool CanBuildGameMap() const noexcept;
};

// 仅检查生成能力，不修改文档、不启动外部进程、不产出地图。
class ScriptBuildService {
public:
    [[nodiscard]] static ScriptBuildCheck CheckGameBuild(
        const TriggerDocument& document,
        const UiCatalog& catalog,
        ScriptBackend backend = ScriptBackend::Jass,
        const MapDocument* mapDocument = nullptr);
};

} // namespace War3Map::Trigger
