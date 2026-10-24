#pragma once

#include "WorldTerrainAssets.h"
#include "War3MapCore/GameUi/FrameLibrary.h"
#include "War3MapCore/GameUi/MapUiFiles.h"
#include "War3MapCore/GameUi/ScriptFrames.h"
#include "War3MapCore/GameUi/UiDescription.h"
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
    bool fromMap = false;  // 命中地图或会话导入（不是游戏包）
};

// 只在一个后台线程上使用；首次读取时才挂载游戏包或打开地图解析器。
class GameUiReader {
public:
    explicit GameUiReader(GameUiSources sources);
    ~GameUiReader();
    GameUiReader(const GameUiReader&) = delete;
    GameUiReader& operator=(const GameUiReader&) = delete;

    [[nodiscard]] Result<GameUiFile> Read(const std::string& path);
    // 地图归档里的地图数据成员（脚本等不属于资源层的文件）；没有地图层或不存在时失败。
    [[nodiscard]] Result<std::vector<std::byte>> ReadMapMember(const std::string& path, std::size_t maxBytes);
    [[nodiscard]] bool HasMapLayer() const noexcept { return sources_.map.has_value(); }
    // 地图层（含会话导入）的全部文件路径；没有地图层时为空。
    [[nodiscard]] std::vector<std::string> MapFiles() const;
    [[nodiscard]] const GameUiSources& Sources() const noexcept { return sources_; }

private:
    [[nodiscard]] Status Open();

    struct State;
    GameUiSources sources_;
    std::unique_ptr<State> state_;
};

// 地图导入的自有界面描述。
struct GameUiDescriptionFile {
    std::string path;
    GameUi::UiDescription description;
};

// 界面定义：FrameDef.toc 全部 fdf（地图导入的同名文件覆盖游戏文件），再读地图脚本加载的界面目录
// 与地图里的 .toc（来源记为 map）；游戏皮肤表与地图皮肤表。
struct GameUiDefinitions {
    std::shared_ptr<const GameUi::FrameLibrary> library;
    std::string gameSkins;
    std::string miscUi;  // UI\MiscUI.txt（字号表）
    std::string mapSkin;
    std::vector<GameUi::TeamColor> teamColors;  // 皮肤指定的队伍颜色贴图（排行榜的玩家颜色与图标）
    std::vector<GameUi::TocLoadCall> tocCalls;  // 地图脚本中加载界面目录的调用
    std::vector<std::string> mapTocs;           // 读入的地图界面目录，按读入顺序
    GameUi::ScriptFramePlan scriptFrames;       // 地图脚本创建的界面
    std::vector<GameUi::MapScriptText> scripts;  // 地图脚本原文（预览模拟用）
    std::vector<GameUiDescriptionFile> descriptions;  // AudastraUI\<编号>.json，按编号排序
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
