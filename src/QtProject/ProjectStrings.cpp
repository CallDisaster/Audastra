#include "EditorProject.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Game/WorldEditStringLoader.h"
#include "War3MapCore/Trigger/UiCatalog.h"

// 显示文本字符串表：YDWE share 覆盖层 → 游戏 MPQ → 英文原版（见 WorldEditStringLoader.h）。
// 触发器目录与物编共用同一份；UI 数据根或游戏目录变化后按需重新载入。
namespace War3Map::QtProject {

std::filesystem::path EditorProject::UiToolRoot() const {
    // 触发器实际使用的数据根优先；尚未载入触发器时用持久化的 UI 根，最后用额外图标目录（通常就是 YDWE 根）。
    if (auto root = ResolveUiDataRoot(); !root.empty()) return root;
    if (!uiDataRoot_.empty()) return uiDataRoot_;
    return extraIconDir_;
}

std::shared_ptr<const Game::WorldEditStrings> EditorProject::CurrentWorldEditStrings() {
    const auto toolRoot = UiToolRoot();
    if (worldEditStrings_ && toolRoot == worldEditStringsToolRoot_ && war3GameRoot_ == worldEditStringsGameRoot_) {
        return worldEditStrings_;
    }
    Game::WorldEditStringSources sources;
    sources.toolRoot = toolRoot;
    std::optional<Game::GameDataSource> game;
    if (!war3GameRoot_.empty()) {
        auto mounted = Game::GameDataSource::Mount(war3GameRoot_);
        if (mounted.Succeeded() && mounted.value) game.emplace(std::move(*mounted.value));
    }
    sources.game = game ? &*game : nullptr;
    worldEditStrings_ = std::make_shared<const Game::WorldEditStrings>(Game::LoadWorldEditStrings(sources));
    worldEditStringsToolRoot_ = toolRoot;
    worldEditStringsGameRoot_ = war3GameRoot_;
    return worldEditStrings_;
}

void EditorProject::ApplyWorldEditStrings(Trigger::UiCatalog& catalog) {
    catalog.SetStrings(CurrentWorldEditStrings());
}

void EditorProject::RefreshWorldEditStrings() {
    const auto previous = worldEditStrings_;
    const auto current = CurrentWorldEditStrings();
    if (current == previous) return;
    if (uiCatalog_) uiCatalog_->SetStrings(current);
    emit WorldEditStringsChanged();
}

} // namespace War3Map::QtProject
