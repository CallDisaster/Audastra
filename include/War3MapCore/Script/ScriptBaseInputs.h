#pragma once

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Script/ScriptToolchainLayout.h"

#include <optional>
#include <span>

namespace War3Map::Game { class GameDataSource; }

namespace War3Map::Script {

// 默认保持现代工具链的既有来源。地图/游戏优先级必须由兼容 profile 显式选择。
enum class ScriptBasePolicy { BoundLayout, MapGameThenLayout };
enum class ScriptBaseOrigin { Layout, MapArchive, MapReplacement, GameArchive };

struct ScriptBaseInput {
    std::vector<std::byte> bytes;
    std::string sha256;
    ScriptBaseOrigin origin = ScriptBaseOrigin::Layout;
    std::string memberName;
    std::filesystem::path sourceRoot;
    std::string archiveName; // 游戏来源实际命中的最高优先级 MPQ
};

struct ScriptBaseInputs {
    ScriptBasePolicy policy = ScriptBasePolicy::BoundLayout;
    bool allowReturnBug = false;
    std::uint64_t documentRevision = 0;
    std::string sourceMapSha256;
    std::optional<int> gameLibraryVersion;
    ScriptBaseInput common;
    ScriptBaseInput blizzard;
};

struct ScriptBaseInputsRequest {
    const ScriptToolchainLayout* layout = nullptr;
    ScriptBasePolicy policy = ScriptBasePolicy::BoundLayout;
    const MapArchiveSession* archive = nullptr;
    std::span<const MemberReplacement> replacements;
    std::span<const std::string> removals;
    const Game::GameDataSource* game = nullptr;
    // 由调用者验证的游戏基础库策略：20=return bug，24=Post124；不从版本宏猜测。
    // 未提供时不读取游戏层，仍允许地图→绑定工具链。策略不同也跳过游戏层。
    std::optional<int> gameLibraryVersion;
    std::uint64_t documentRevision = 0;
};

// common 与 blizzard 独立取源，根成员优先 scripts\ 成员；修改/删除来自同一最终保存视图。
// 命中成员损坏、覆盖冲突、超限直接失败，不静默降级。每项完整冻结字节与来源摘要。
[[nodiscard]] Result<ScriptBaseInputs> CaptureScriptBaseInputs(const ScriptBaseInputsRequest& request);
[[nodiscard]] Status VerifyScriptBaseInputs(const ScriptBaseInputs& inputs,
    const ScriptToolchainLayout& layout, ScriptBasePolicy policy);

} // namespace War3Map::Script
