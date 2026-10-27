#pragma once

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/ObjectData/ObjectCategoryArt.h"

#include <QHash>
#include <QIcon>
#include <QString>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace War3Map {
namespace Trigger {
class UiCatalog;
}
} // namespace War3Map

namespace War3Map::QtApp {

// 触发器图标服务：从魔兽争霸3 的 MPQ 提取 BLP 图标，解码为 QIcon 并缓存。
//   * 纹理路径（如 ReplaceableTextures\\CommandButtons\\BTNPillage.blp）→ QIcon
//   * 触发器分类 id（如 TC_NOTHING）→ QIcon（经 define.txt 的 [TriggerCategories] 映射）
// 未配置游戏路径或解码失败时返回空 QIcon，由调用方回退到程序化图标。
class IconProvider {
public:
    IconProvider() = default;

    // 挂载魔兽争霸3 资源。成功返回 true。可重复调用以切换路径。
    bool SetGameRoot(const std::filesystem::path& war3Root);
    [[nodiscard]] bool IsReady() const noexcept { return source_ != nullptr; }

    // 按当前 UI Catalog 合并映射：各层 [TriggerCategories] 的 分类→纹理（上层覆盖下层）与
    // 函数定义 category 字段的 函数→分类。Catalog 是界面实际显示的定义，覆盖游戏 TriggerData 基线；
    // 没有 category 字段的函数保留基线映射。可重复调用。
    void MergeCatalogCategories(const War3Map::Trigger::UiCatalog& catalog);

    // UI 数据根（YDWE 根目录或其 ui 目录）。函数库分类图标可能写成游戏路径、相对 YDWE 根
    // （icons\\x.blp、ui\\WarVK\\icons\\x.blp）或相对所在层目录；据此依次查找
    // share/<语言>/mpq 覆盖层、YDWE 根与分类所在层目录。传空路径清除。
    void SetUiToolRoot(const std::filesystem::path& toolRoot);

    // 切换 UI Catalog 前清除上一个 Catalog 的函数/分类映射，
    // 然后恢复游戏 TriggerData 基线。纹理解码缓存不受影响。
    void ResetUiMappings();

    // 额外图标目录（如 YDWE 安装目录）：游戏 MPQ 未命中时按相对路径回退到
    // 磁盘查找 BLP（解决 MapUI 等地图/YDWE 自带纹理）。传空路径清除。
    void SetExtraIconDir(const std::filesystem::path& dir);
    // 地图层先于游戏层；切换项目必须清除旧来源及缓存。
    bool SetMapArchive(const std::filesystem::path& path);
    void SetMapResourceDirectory(const std::filesystem::path& path);

    // 游戏数据（不含地图层）中是否有该文件，例如原版自带的暂停图标。
    [[nodiscard]] bool HasGameFile(const QString& path) const;

    // 纹理路径 → QIcon（带缓存）。失败返回空 QIcon。
    [[nodiscard]] QIcon IconForTexture(const QString& texturePath);

    // 原版编辑器界面图标（UI\WorldEditData.txt [WorldEditArt] 的 SEIcon_*），运行时从本机游戏数据读取，
    // 不随仓库分发。16 像素点阵图按最近邻放大到 2 倍，高 DPI 下不发糊。游戏数据缺失时返回空 QIcon。
    enum class EditorIcon {
        Trigger,               // 普通触发器
        TriggerPartial,        // 含禁用语句
        TriggerDisabled,       // 未允许（红叉）
        TriggerUnused,         // 初始关闭（灰）
        TriggerPartialUnused,
        TriggerUnusedDisabled,
        Comment,               // 注释条目
        Category,
        CategoryOpen,
        Event,
        Condition,
        Action,
        Function,
        FunctionDisabled,
    };
    [[nodiscard]] QIcon IconForEditor(EditorIcon icon);

    // 装饰物/可破坏物的类别图标（UI\WorldEditData.txt 的 [DoodadCategories]/[DestructibleCategories]）。
    // 原版目录给没有自身图标的条目显示所在类别的图标；category 为物体的类别字段。只取游戏数据，
    // 没有对应类别或图标时返回空 QIcon。
    [[nodiscard]] QIcon IconForObjectCategory(War3Map::ObjectData::ObjectType type, const std::string& category);

    // 分类 id（TC_XXX）→ QIcon。未知分类返回空 QIcon。
    [[nodiscard]] QIcon IconForCategory(const QString& categoryId);

    // 函数名（如 CreateNUnitsAtLoc）→ QIcon，经 函数→分类→纹理 两级映射。
    [[nodiscard]] QIcon IconForFunction(const QString& functionName);

    // 函数当前映射到的分类 id（测试/诊断用；未映射返回空串）。
    [[nodiscard]] QString CategoryForFunction(const QString& functionName) const;
    // 分类当前映射到的纹理路径（测试/诊断用；未映射返回空串）。
    [[nodiscard]] QString TextureForCategory(const QString& categoryId) const;

    [[nodiscard]] int CategoryCount() const noexcept { return categoryToTexture_.size(); }
    [[nodiscard]] int FunctionCategoryCount() const noexcept {
        return functionToCategory_.size();
    }

private:
    // 从游戏 MPQ 的 UI\TriggerData.txt 解析分类与函数分类（SetGameRoot 内自动调用）。
    void LoadFromGameTriggerData();
    // 解析游戏 TriggerData 文本：填充 categoryToTexture_ 与 functionToCategory_。
    void ParseTriggerData(const std::string& content);
    // layer 非空时，其他来源都未命中再查该层目录（分类图标相对层目录的写法）。
    [[nodiscard]] QIcon IconForTexture(const QString& texturePath, const QString& layer);
    [[nodiscard]] std::vector<std::byte> ReadTexture(const QString& key, const QString& layer) const;
    // 只从游戏数据解码编辑器界面图标；16 像素点阵图另备最近邻放大的 2 倍图。
    [[nodiscard]] QIcon GameUiIcon(const QString& texturePath);

    std::unique_ptr<War3Map::Game::GameDataSource> source_;
    std::unique_ptr<War3Map::Mpq::MpqArchive> mapArchive_;
    std::filesystem::path mapResourceDirectory_;
    std::filesystem::path extraIconDir_;             // 额外磁盘图标目录（可为空）
    std::vector<std::filesystem::path> overlayRoots_; // YDWE share/<语言>/mpq（先于游戏）
    std::vector<std::filesystem::path> toolRoots_;    // YDWE 根目录（后于游戏）
    QHash<QString, QIcon> iconCache_;           // 归一化纹理路径 → QIcon
    QHash<QString, QString> categoryToTexture_; // TC_xxx → 纹理路径
    QHash<QString, QString> functionToCategory_; // 函数名 → TC_xxx
    QHash<QString, QString> categoryLayer_;      // TC_xxx → 提供该分类的 UI 层（游戏基线为空）
    std::optional<std::vector<War3Map::ObjectData::ObjectCategoryArt>> objectCategories_; // 首次使用时读取
};

} // namespace War3Map::QtApp
