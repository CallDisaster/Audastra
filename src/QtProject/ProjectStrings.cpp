#include "EditorProject.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Game/SoundChannels.h"
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
    // 声音通道与 EAX 名称随同一次游戏挂载读取，名称用刚载入的字符串表；读不到时为空表，界面只显示原值。
    Game::SoundTables tables;
    if (game) {
        const auto text = [&](const char* path) -> std::optional<std::string> {
            const auto data = game->ReadFile(path);
            if (!data.Succeeded() || !data.value || data.value->size() > 2u * 1024u * 1024u) return std::nullopt;
            return std::string(reinterpret_cast<const char*>(data.value->data()), data.value->size());
        };
        if (const auto data = text("UI\\WorldEditData.txt")) tables.channels = Game::ParseSoundChannels(*data, worldEditStrings_.get());
        if (const auto data = text("UI\\SoundInfo\\EAXDefs.slk")) tables.eaxSettings = Game::ParseSoundEaxSettings(*data, worldEditStrings_.get());
    }
    soundTables_ = std::make_shared<const Game::SoundTables>(std::move(tables));
    worldEditStringsToolRoot_ = toolRoot;
    worldEditStringsGameRoot_ = war3GameRoot_;
    return worldEditStrings_;
}

std::shared_ptr<const Game::SoundTables> EditorProject::CurrentSoundTables() {
    CurrentWorldEditStrings();
    return soundTables_;
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
