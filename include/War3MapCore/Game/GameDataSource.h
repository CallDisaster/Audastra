#pragma once

#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Types.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Game {

// 魔兽争霸3 安装目录下的核心 MPQ，按加载优先级从低到高排列。
// 仅描述经典 1.27a 归档层的 patch 至 base 顺序，不代表原版完整来源搜索。
struct GameArchiveInfo {
    std::string fileName;   // 如 "war3.mpq"
    bool present = false;   // 文件是否存在
    bool opened = false;    // 是否成功作为 MPQ 打开
    std::size_t fileCount = 0;
};

// 校验结果：报告每个已知 MPQ 的状态。
struct GameRootValidation {
    std::filesystem::path root;
    std::vector<GameArchiveInfo> archives;

    // 至少 war3.mpq 与 war3x.mpq（或等价）可打开才算可用。
    [[nodiscard]] bool Usable() const noexcept;
    [[nodiscard]] std::string Summary() const;
};

// 仅来自游戏 MPQ 可读 (listfile) 的名字；未列出的块不能凭空枚举。
// archiveName 为实际命中的最高优先级归档，可能不同于提供名字的归档。
struct GameResourceFile {
    std::string path;
    std::string archiveName;
};

// 一次有界读取的内容与实际命中的核心 MPQ 来源。
struct GameFileContents {
    std::vector<std::byte> bytes;
    std::string archiveName;
};

// 挂载魔兽争霸3 的多个 MPQ，提供统一的按名读取，供图标/资源提取使用。
class GameDataSource {
public:
    GameDataSource() = default;
    GameDataSource(GameDataSource&&) noexcept = default;
    GameDataSource& operator=(GameDataSource&&) noexcept = default;
    GameDataSource(const GameDataSource&) = delete;
    GameDataSource& operator=(const GameDataSource&) = delete;
    ~GameDataSource() = default;

    // 经典 1.27a 核心归档层，按查找优先级从低到高；归档可缺失。
    // 挂载、校验和预览来源租约共用此名单，不包含 loose 文件、Storm 前缀视图或地图覆盖。
    [[nodiscard]] static std::span<const std::string_view> CoreArchiveNames() noexcept;

    // 仅检查目录下的 MPQ 状态，不长期持有句柄。用于路径前置校验。
    [[nodiscard]] static GameRootValidation Validate(
        const std::filesystem::path& war3Root);

    // 挂载目录下所有可用的核心 MPQ。失败（无任何可用 MPQ）返回 Failure。
    [[nodiscard]] static Result<GameDataSource> Mount(
        const std::filesystem::path& war3Root);

    // 严格来源：允许核心包缺失；存在却不是普通文件、无法检查或无法打开则整体失败。
    // 用于不能把损坏高层归档悄悄当作缺失并显示低层资源的调用方。
    [[nodiscard]] static Result<GameDataSource> MountStrict(
        const std::filesystem::path& war3Root);

    // 世界资源专用的显式地形集上下文：最高优先级主包内的 <tileset>.mpq
    // 作为 owned 嵌套归档置于 patch 之下、local/expansion/base 之上；地图覆盖仍由上层 resolver 负责。
    // 缺少嵌套包可使用普通来源，嵌套包存在但损坏/超额时拒绝，不猜 <tileset>_ 文件名。
    [[nodiscard]] static Result<GameDataSource> MountStrict(
        const std::filesystem::path& war3Root, std::uint8_t mainTileset);

    // 是否存在指定归档内文件（不区分大小写，'/' 与 '\\' 等价）。
    [[nodiscard]] bool HasFile(const std::string& name) const;

    // 按归档优先级（patch>xlocal>local>x>base）读取文件的解压内容。
    [[nodiscard]] Result<std::vector<std::byte>> ReadFile(const std::string& name) const;
    // 同优先级完整读取，命中归档在分配/解压前检查存储与解压大小；超限或损坏不回退低层。
    [[nodiscard]] Result<std::vector<std::byte>> ReadFileBounded(
        const std::string& name, std::size_t maxBytes) const;
    // 同一次优先级查找返回解压内容和实际命中的核心 MPQ 名；失败不回退低层。
    [[nodiscard]] Result<GameFileContents> ReadFileBoundedWithSource(
        const std::string& name, std::size_t maxBytes) const;
    // 同优先级规则，只读取文件开头至多 maxBytes 字节。
    [[nodiscard]] Result<std::vector<std::byte>> ReadFileHead(const std::string& name, std::size_t maxBytes) const;

    // 只读枚举可信清单中可按名字访问的资源。缺失/损坏清单与同层大小写或分隔符冲突
    // 作为 Warning 返回；这些名字被跳过，其他可确认项仍可浏览。调用方应在后台线程执行。
    [[nodiscard]] Result<std::vector<GameResourceFile>> ListFilesWithSource() const;

    // 从 UI/SoundInfo 七张表提取 DirectoryBase + FileNames；每个候选必须通过实际
    // 游戏 MPQ HasFile 验证后才返回。可补充没有 (listfile) 的基础包，但并非全量枚举。
    [[nodiscard]] Result<std::vector<GameResourceFile>> DiscoverSoundFilesFromSoundInfo() const;

    [[nodiscard]] const std::filesystem::path& Root() const noexcept { return root_; }
    [[nodiscard]] std::size_t MountedCount() const noexcept { return archives_.size(); }

private:
    [[nodiscard]] static Result<GameDataSource> MountImpl(
        const std::filesystem::path& war3Root, bool strict);
    std::filesystem::path root_;
    // 按查找优先级从高到低排列（front 优先）。
    std::vector<std::unique_ptr<Mpq::MpqArchive>> archives_;
    std::vector<std::string> archiveNames_; // 与 archives_ 同下标、同优先级顺序。
};

} // namespace War3Map::Game
