#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Resources {

// layer 仅标注来源身份；实际覆盖顺序由各来源可注入的 priority 决定。
enum class ResourceLayer {
    SessionOverride,
    MapImport,
    Campaign,
    Game,
    ToolExtension,
    Other,
};

// 推荐值对应当前编辑器来源顺序；调用方可注册更多来源和优先级。
namespace ResourcePriority {
inline constexpr int SessionOverride = 600;
inline constexpr int MapArchive = 500;
inline constexpr int MapDirectory = 490;
inline constexpr int Campaign = 400;
inline constexpr int GamePatch = 330;
inline constexpr int GameLocal = 320;
inline constexpr int GameExpansion = 310;
inline constexpr int GameBase = 300;
inline constexpr int ToolExtension = 200;
} // namespace ResourcePriority

enum class ResourceProbeState {
    Missing,
    Found,
    Conflict,
};

struct ResourceProbe {
    ResourceProbeState state = ResourceProbeState::Missing;
    // Found 时为实际命中的归档成员或目录相对路径；Conflict 时可填冲突说明。
    std::string sourcePath;
};

using ResourceProbeCallback = std::function<Result<ResourceProbe>(std::string_view canonicalPath)>;
using ResourceReadCallback = std::function<Result<std::vector<std::byte>>(std::string_view sourcePath)>;

// 与成功读取的正文同次产生；不改变sourcePath的读取键或source的逻辑层/优先级。
// Synthetic仅供明确的生成者使用，不能从路径文字推断；nullopt表示尚未取证。
enum class ResourceReadOrigin { MapArchive, GameArchive, SessionOwned, SessionFile, Synthetic };

struct ResourceReadProvenance {
    ResourceReadOrigin origin = ResourceReadOrigin::MapArchive;
    // 实际容器标识：如地图路径、同次GameFileContents.archiveName、会话owner版本或生成者ID。
    // 游戏嵌套包保留完整opaque名称；不将gameRoot伪作winning archive或猜拼物理路径。
    std::string containerIdentity;
    std::string memberPath;
    // 解释容器所需的来源上下文，如地图SHA或gameRoot；不是独立的winner证明。
    std::string sourceContext;
    [[nodiscard]] bool operator==(const ResourceReadProvenance&) const = default;
};

struct ResourceReadResult {
    std::vector<std::byte> bytes;
    std::optional<ResourceReadProvenance> provenance;
};

using ResourceReadWithProvenanceCallback = std::function<Result<ResourceReadResult>(std::string_view sourcePath)>;

struct ResourceSource {
    std::string id;
    ResourceLayer layer = ResourceLayer::Game;
    // 全局优先级，数值越大越先查询；同级多个来源命中视为冲突。
    int priority = 0;
    ResourceProbeCallback probe;
    ResourceReadCallback read;
    // 两回调同时存在时只调用本项；本项失败/缺少有效provenance绝不回退read或低优先级来源。
    // 原read回调保持兼容，成功时provenance仍为空。追加字段保留旧aggregate初始化顺序。
    ResourceReadWithProvenanceCallback readWithProvenance;
};

struct ResourceLocation {
    std::string canonicalPath;
    ResourceLayer layer = ResourceLayer::Game;
    int priority = 0;
    std::string sourceId;
    std::string sourcePath;
    // Locate只查询来源，始终为空；Read验证rich结果后冻结单份，行级location复制不深拷贝字符串。
    // 回调只返回value，不能保留此const owner的可写shared alias；身份比较必须按内容而非地址。
    std::shared_ptr<const ResourceReadProvenance> readProvenance;
};

struct ResolvedResource {
    ResourceLocation location;
    std::vector<std::byte> bytes;
};

// 仅调度由调用方注入的只读来源；不持有归档句柄，也不缓存命中或文件内容。
// 来源自身负责保证 probe/read 之间的字节一致性（例如 VerifiedFile）。
class ResourceResolver {
public:
    [[nodiscard]] Status AddSource(ResourceSource source);
    [[nodiscard]] bool RemoveSource(std::string_view id);
    void ClearSources() noexcept;

    // 返回规范化虚拟路径和确切来源；失败不回退到低优先级来源。
    [[nodiscard]] Result<ResourceLocation> Locate(std::string_view virtualPath) const;
    [[nodiscard]] Result<ResolvedResource> Read(std::string_view virtualPath) const;

private:
    std::vector<ResourceSource> sources_;
};

} // namespace War3Map::Resources
