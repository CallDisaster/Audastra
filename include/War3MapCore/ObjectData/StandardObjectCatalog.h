#pragma once

#include "War3MapCore/ObjectData/ObjectTypes.h"

#include <functional>
#include <span>
#include <unordered_map>

namespace War3Map::Game {
class GameDataSource;
class WorldEditStrings;
}

namespace War3Map::ObjectData {

// 原版物体目录只读快照；不拥有游戏归档，也不包含解码后的图像资源。
struct StandardObjectEntry {
    ObjectKey key;
    std::string displayName;
    std::string race;
    std::string category;
    std::string iconPath;
    std::string nameSourceRawcode;
    std::string iconSourceRawcode;
    // SLK 列名与 profile 键名统一为 ASCII 小写，rawcode 值保持大小写。
    std::unordered_map<std::string, std::string> fields;
    std::vector<std::string> sourceFiles;
    bool nameResolved = false;
    bool editorVisible = true;

    [[nodiscard]] std::string_view Field(std::string_view name) const;
};

// 同类表按顺序覆盖字段。只有首个 SLK 定义成员集合，补充表/profile 不添加幽灵物体。
struct CatalogSourceSpec {
    ObjectType type = ObjectType::Unit;
    std::vector<std::string> slkFiles;
    std::vector<std::string> profileFiles;
    std::vector<std::string> nameFields{"name", "editorname", "bufftip"};
    std::vector<std::string> iconFields{"art", "researchart", "icon"};
    // 仅回退缺失的名称/图标，不把运行时代码类型当作完整物体字段继承。
    std::string presentationParentField;
};

struct CatalogLoadOptions {
    // 空列表使用七类经典 MPQ 数据表；调用方可提供其他版本的数据布局。
    std::vector<CatalogSourceSpec> sources;
    std::vector<std::string> stringFiles{"UI/WorldEditStrings.txt", "UI/WorldEditGameStrings.txt"};
    // 统一字符串表（YDWE 覆盖层 → 游戏 → 英文原版）；提供时优先于 stringFiles 解析 WESTRING_ 名称。
    const Game::WorldEditStrings* strings = nullptr;
    std::size_t maximumFileBytes = 32 * 1024 * 1024;
};

class StandardObjectCatalog {
public:
    using FileReader = std::function<Result<std::vector<std::byte>>(const std::string&)>;

    [[nodiscard]] static Result<StandardObjectCatalog> Load(
        const Game::GameDataSource& source, const CatalogLoadOptions& options = {});
    // 可注入读取器以复用目录/归档适配器，也便于使用原创合成数据测试。
    [[nodiscard]] static Result<StandardObjectCatalog> Load(
        const FileReader& reader, const CatalogLoadOptions& options = {});
    [[nodiscard]] static std::vector<CatalogSourceSpec> DefaultSources();

    [[nodiscard]] std::span<const StandardObjectEntry> Entries() const noexcept { return entries_; }
    [[nodiscard]] const StandardObjectEntry* Find(ObjectType type, std::string_view rawcode) const;
    [[nodiscard]] std::size_t Count(ObjectType type) const noexcept;
    [[nodiscard]] const std::vector<std::string>& LoadedFiles() const noexcept { return loadedFiles_; }

private:
    std::vector<StandardObjectEntry> entries_;
    std::unordered_map<std::string, std::size_t> index_;
    std::vector<std::string> loadedFiles_;
};

} // namespace War3Map::ObjectData
