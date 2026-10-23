#pragma once

#include "WorldTerrainAssets.h"
#include "War3MapCore/GameUi/FrameLibrary.h"
#include "War3MapCore/Image/BlpImage.h"

#include <QString>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace War3Map::QtProject { class EditorProject; }
namespace War3Map::Modules {

// 界面工作区的资源来源：打开地图时按“会话导入 → 地图 → 游戏”解析（与世界、物编预览同一解析器）；
// 没有可验证的地图来源时只读游戏。在界面线程捕获，后台线程使用。
struct GameUiSources {
    std::filesystem::path gameRoot;
    std::optional<WorldTerrainSources> map;
    QString mapNote;  // 打开了地图但地图层不可用的原因
};
[[nodiscard]] GameUiSources CaptureGameUiSources(const QtProject::EditorProject& project);

struct GameUiFile {
    std::vector<std::byte> bytes;
    std::string origin;  // 实际命中的来源，如地图、会话导入或游戏包名
};

// 只在一个后台线程上使用；首次读取时才挂载游戏包或打开地图解析器。
class GameUiReader {
public:
    explicit GameUiReader(GameUiSources sources);
    ~GameUiReader();
    GameUiReader(const GameUiReader&) = delete;
    GameUiReader& operator=(const GameUiReader&) = delete;

    [[nodiscard]] Result<GameUiFile> Read(const std::string& path);
    [[nodiscard]] bool HasMapLayer() const noexcept { return sources_.map.has_value(); }
    [[nodiscard]] const GameUiSources& Sources() const noexcept { return sources_; }

private:
    [[nodiscard]] Status Open();

    struct State;
    GameUiSources sources_;
    std::unique_ptr<State> state_;
};

// 界面定义：FrameDef.toc 全部 fdf（地图导入的同名文件覆盖游戏文件）、游戏皮肤表与地图皮肤表。
struct GameUiDefinitions {
    std::shared_ptr<const GameUi::FrameLibrary> library;
    std::string gameSkins;
    std::string mapSkin;
    std::vector<Diagnostic> diagnostics;
};
[[nodiscard]] GameUiDefinitions LoadGameUiDefinitions(GameUiReader& reader);

struct GameUiAsset {
    std::string path;
    std::string origin;
    std::optional<Image::DecodedImage> image;  // 贴图
    std::vector<std::byte> fontBytes;          // 字体
    std::string error;
};
[[nodiscard]] std::vector<GameUiAsset> LoadGameUiAssets(GameUiReader& reader,
    const std::vector<std::string>& textures, const std::vector<std::string>& fonts);

// 贴图与字体缓存的键：小写、统一反斜杠。
[[nodiscard]] QString GameUiAssetKey(const QString& path);

} // namespace War3Map::Modules
