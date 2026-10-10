#include "Icons/IconProvider.h"

#include "War3MapCore/Game/WorldEditStringLoader.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/UiCatalog.h"
#include "War3MapCore/Text/TextUtil.h"

#include <QImage>
#include <QPixmap>
#include <QFileInfo>

#include <cstring>
#include <span>
#include <string>

namespace War3Map::QtApp {
namespace {

// 归一化纹理路径：统一用反斜杠、小写，补 .blp 扩展名（WorldEditUI 条目常缺扩展名）。
QString NormalizeTexturePath(const QString& raw) {
    QString path = raw;
    path.replace('/', '\\');
    // 去除首尾空白与逗号后可能的多余部分。
    path = path.trimmed();
    path = path.section(',', 0, 0).trimmed();
    if (path.isEmpty() || path.startsWith('\\') || path.contains(':') || path.contains(QChar(0))) return {};
    // 函数库常把分隔符写成双反斜杠（icons\\x.blp）：重复分隔符视为一个。
    QStringList segments = path.split('\\', Qt::SkipEmptyParts);
    for (const auto& segment : segments) {
        if (segment == "." || segment == "..") return {};
    }
    path = segments.join('\\');
    if (QFileInfo(path).suffix().isEmpty()) {
        path += ".blp";
    }
    return path.toLower();
}

// 从磁盘目录读取（与 YDWE 覆盖层同一套规则：逐段不区分大小写、不出根目录）。
std::vector<std::byte> ReadTextureInside(const std::filesystem::path& root, const QString& key) {
    if (root.empty() || key.isEmpty()) return {};
    const std::span<const std::filesystem::path> single(&root, 1);
    auto bytes = War3Map::Game::ReadOverlayFile(single, key.toStdString());
    return bytes ? std::move(*bytes) : std::vector<std::byte>{};
}

// 分类图标写 none 表示不显示图标。
bool IsNoTexture(const QString& path) {
    return path.trimmed().compare(QStringLiteral("none"), Qt::CaseInsensitive) == 0;
}

} // namespace

bool IconProvider::SetGameRoot(const std::filesystem::path& war3Root) {
    auto mounted = War3Map::Game::GameDataSource::Mount(war3Root);
    if (!mounted.Succeeded() || !mounted.value.has_value()) {
        return false;
    }
    source_ = std::make_unique<War3Map::Game::GameDataSource>(std::move(*mounted.value));
    iconCache_.clear();
    categoryToTexture_.clear();
    functionToCategory_.clear();
    // 以游戏自带的 UI\TriggerData.txt 为权威来源。
    LoadFromGameTriggerData();
    return true;
}

void IconProvider::LoadFromGameTriggerData() {
    if (source_ == nullptr) {
        return;
    }
    auto data = source_->ReadFile("UI\\TriggerData.txt");
    if (!data.Succeeded() || !data.value.has_value() || data.value->empty()) {
        return;
    }
    const auto& bytes = *data.value;
    std::string content(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    ParseTriggerData(content);
}

void IconProvider::MergeCatalogCategories(
    const War3Map::Trigger::UiCatalog& catalog) {
    using War3Map::Trigger::UiDefinitionKind;
    const War3Map::Trigger::TriggerCategoryTable categories(catalog);
    for (const auto& category : categories.Categories()) {
        const QString id = QString::fromStdString(category.id);
        categoryToTexture_.insert(id, QString::fromStdString(category.icon));
        categoryLayer_.insert(id, QString::fromStdString(category.layer));
    }
    constexpr UiDefinitionKind kKinds[] = {
        UiDefinitionKind::Event,
        UiDefinitionKind::Condition,
        UiDefinitionKind::Action,
        UiDefinitionKind::Call,
    };
    for (const UiDefinitionKind kind : kKinds) {
        for (const auto* definition : catalog.Definitions(kind)) {
            if (definition == nullptr || definition->name.empty()) {
                continue;
            }
            const auto category = categories.CanonicalId(War3Map::Trigger::TriggerCategoryTable::CategoryOf(*definition));
            if (!category.empty()) {
                functionToCategory_.insert(QString::fromStdString(definition->name), QString::fromStdString(category));
            }
        }
    }
}

void IconProvider::SetUiToolRoot(const std::filesystem::path& toolRoot) {
    auto overlays = War3Map::Game::YdweOverlayRoots(toolRoot);
    auto roots = War3Map::Game::YdweRootDirectories(toolRoot);
    if (overlays == overlayRoots_ && roots == toolRoots_) {
        return;
    }
    overlayRoots_ = std::move(overlays);
    toolRoots_ = std::move(roots);
    iconCache_.clear();
}

void IconProvider::ResetUiMappings() {
    categoryToTexture_.clear();
    functionToCategory_.clear();
    categoryLayer_.clear();
    // 游戏自带 TriggerData 是每个 Catalog 的共享基线；未挂载游戏根时保持空映射。
    LoadFromGameTriggerData();
}

// 游戏 TriggerData.txt 格式：
//   [TriggerCategories] 段：TC_XXX=显示文本,纹理路径[,显示标志]
//   任意段：_<函数名>_Category=TC_XXX（把函数归入分类）
void IconProvider::ParseTriggerData(const std::string& content) {
    std::string section;
    std::size_t pos = 0;
    while (pos < content.size()) {
        std::size_t lineEnd = content.find('\n', pos);
        if (lineEnd == std::string::npos) {
            lineEnd = content.size();
        }
        const std::string line = War3Map::Text::TrimLineWhitespaceCopy(content.substr(pos, lineEnd - pos));
        pos = lineEnd + 1;
        if (line.empty() || (line.size() >= 2 && line[0] == '/' && line[1] == '/')) {
            continue;
        }
        if (line.front() == '[') {
            const std::size_t rb = line.find(']');
            section = (rb == std::string::npos) ? line : line.substr(0, rb + 1);
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = War3Map::Text::TrimLineWhitespaceCopy(line.substr(0, eq));
        const std::string value = War3Map::Text::TrimLineWhitespaceCopy(line.substr(eq + 1));

        // 分类定义：TC_XXX=显示,纹理[,标志]。
        if (section == "[TriggerCategories]" && key.rfind("TC_", 0) == 0) {
            const std::size_t c1 = value.find(',');
            if (c1 == std::string::npos) {
                continue;
            }
            const std::size_t c2 = value.find(',', c1 + 1);
            std::string texture = (c2 == std::string::npos)
                ? value.substr(c1 + 1)
                : value.substr(c1 + 1, c2 - (c1 + 1));
            texture = War3Map::Text::TrimLineWhitespaceCopy(texture);
            if (!texture.empty()) {
                categoryToTexture_.insert(QString::fromStdString(key),
                                          QString::fromStdString(texture));
            }
            continue;
        }

        // 函数分类：_<函数名>_Category=TC_XXX（“_Category”共 9 个字符）。
        constexpr std::size_t kSuffixLen = 9; // "_Category"
        if (key.size() > kSuffixLen + 1 && key.front() == '_' &&
            key.compare(key.size() - kSuffixLen, kSuffixLen, "_Category") == 0 &&
            value.rfind("TC_", 0) == 0) {
            const std::string name = key.substr(1, key.size() - 1 - kSuffixLen);
            if (!name.empty()) {
                functionToCategory_.insert(QString::fromStdString(name),
                                           QString::fromStdString(value));
            }
        }
    }
}

QIcon IconProvider::IconForTexture(const QString& texturePath) {
    return IconForTexture(texturePath, {});
}

std::vector<std::byte> IconProvider::ReadTexture(const QString& key, const QString& layer) const {
    // 地图归档/展开资源先于原版游戏。同路径替换应与地图实际使用的资源一致。
    if (mapArchive_) {
        auto mapped = mapArchive_->ReadFile(key.toStdString());
        if (mapped.Succeeded() && mapped.value && !mapped.value->empty()) return std::move(*mapped.value);
    }
    if (auto bytes = ReadTextureInside(mapResourceDirectory_, key); !bytes.empty()) return bytes;
    // YDWE share 覆盖层整文件替代游戏同名文件。
    for (const auto& root : overlayRoots_) {
        if (auto bytes = ReadTextureInside(root, key); !bytes.empty()) return bytes;
    }
    if (source_ != nullptr) {
        auto blp = source_->ReadFile(key.toStdString());
        if (blp.Succeeded() && blp.value.has_value() && !blp.value->empty()) return std::move(*blp.value);
    }
    // 游戏未命中：相对 YDWE 根目录的函数库图标，再到分类所在层目录，最后是额外图标目录。
    for (const auto& root : toolRoots_) {
        if (auto bytes = ReadTextureInside(root, key); !bytes.empty()) return bytes;
    }
    if (!layer.isEmpty()) {
        const std::filesystem::path layerPath(layer.toStdWString());
        for (const auto& root : toolRoots_) {
            for (const auto& directory : {root / layerPath, root / "ui" / layerPath}) {
                if (auto bytes = ReadTextureInside(directory, key); !bytes.empty()) return bytes;
            }
        }
    }
    return ReadTextureInside(extraIconDir_, key);
}

QIcon IconProvider::IconForTexture(const QString& texturePath, const QString& layer) {
    if (texturePath.isEmpty() || IsNoTexture(texturePath)) {
        return {};
    }
    const QString key = NormalizeTexturePath(texturePath);
    if (key.isEmpty()) return {};
    // 层目录只作为最后的回退；同一路径在不同层可能是不同文件，缓存键带上层名。
    const QString cacheKey = layer.isEmpty() ? key : layer + QLatin1Char('|') + key;
    auto cached = iconCache_.find(cacheKey);
    if (cached != iconCache_.end()) {
        return cached.value();
    }
    const auto bytes = ReadTexture(key, layer);
    if (bytes.empty()) {
        iconCache_.insert(cacheKey, QIcon{}); // 缓存空结果，避免重复失败查找
        return {};
    }

    if (!key.endsWith(".blp")) {
        const auto qimg = QImage::fromData(reinterpret_cast<const uchar*>(bytes.data()), static_cast<int>(bytes.size()));
        const QIcon icon = qimg.isNull() ? QIcon{} : QIcon(QPixmap::fromImage(qimg));
        iconCache_.insert(cacheKey, icon);
        return icon;
    }
    auto decoded = War3Map::Image::DecodeBlp(bytes);
    if (!decoded.Succeeded() || !decoded.value.has_value() || !decoded.value->Valid()) {
        iconCache_.insert(cacheKey, QIcon{});
        return {};
    }

    const auto& img = *decoded.value;
    QImage qimg(static_cast<int>(img.width), static_cast<int>(img.height),
                QImage::Format_RGBA8888);
    for (std::uint32_t y = 0; y < img.height; ++y) {
        std::memcpy(qimg.scanLine(static_cast<int>(y)),
                    img.rgba.data() + static_cast<std::size_t>(y) * img.width * 4,
                    static_cast<std::size_t>(img.width) * 4);
    }
    QIcon icon(QPixmap::fromImage(qimg));
    iconCache_.insert(cacheKey, icon);
    return icon;
}

bool IconProvider::HasGameFile(const QString& path) const {
    return source_ != nullptr && source_->ReadFile(NormalizeTexturePath(path).toStdString()).Succeeded();
}

QIcon IconProvider::IconForEditor(EditorIcon icon) {
    static constexpr const char* kPaths[] = {
        "Editor-Trigger", "Editor-TriggerPartial", "Editor-TriggerDisabled", "Editor-TriggerUnused",
        "Editor-TriggerPartialUnused", "Editor-TriggerUnusedDisabled", "Editor-Toolbar-TriggerComment",
        "Editor-TriggerGroup", "Editor-TriggerGroup-Open", "Editor-TriggerEvent", "Editor-TriggerCondition",
        "Editor-TriggerAction", "Editor-TriggerFunction", "Editor-TriggerFunctionDisabled",
    };
    const QString key = QStringLiteral("editor:%1").arg(static_cast<int>(icon));
    if (const auto cached = iconCache_.find(key); cached != iconCache_.end()) {
        return cached.value();
    }
    QIcon result;
    if (source_ != nullptr) {
        // 与 IconForTexture 相同的归一化（反斜杠、小写）；编辑器界面图标只取原版游戏数据，地图不能替换。
        const std::string path = NormalizeTexturePath(
            QStringLiteral("ReplaceableTextures\\WorldEditUI\\") + QString::fromLatin1(kPaths[static_cast<int>(icon)])).toStdString();
        auto blp = source_->ReadFile(path);
        Result<War3Map::Image::DecodedImage> decoded;
        if (blp.Succeeded() && blp.value) decoded = War3Map::Image::DecodeBlp(*blp.value);
        if (decoded.Succeeded() && decoded.value && decoded.value->Valid()) {
            const auto& image = *decoded.value;
            QImage pixels(static_cast<int>(image.width), static_cast<int>(image.height), QImage::Format_RGBA8888);
            for (std::uint32_t y = 0; y < image.height; ++y) {
                std::memcpy(pixels.scanLine(static_cast<int>(y)), image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4,
                    static_cast<std::size_t>(image.width) * 4);
            }
            result.addPixmap(QPixmap::fromImage(pixels));
            result.addPixmap(QPixmap::fromImage(pixels.scaled(pixels.size() * 2, Qt::IgnoreAspectRatio, Qt::FastTransformation)));
        }
    }
    iconCache_.insert(key, result);
    return result;
}

void IconProvider::SetExtraIconDir(const std::filesystem::path& dir) {
    if (extraIconDir_ == dir) {
        return;
    }
    extraIconDir_ = dir;
    iconCache_.clear(); // 已缓存的空结果可能因新目录而变为可解析
}

bool IconProvider::SetMapArchive(const std::filesystem::path& path) {
    mapArchive_.reset();
    iconCache_.clear();
    if (path.empty()) return true;
    auto opened = War3Map::Mpq::MpqArchive::OpenReadOnly(path);
    if (!opened.Succeeded() || !opened.value) return false;
    mapArchive_ = std::make_unique<War3Map::Mpq::MpqArchive>(std::move(*opened.value));
    return true;
}

void IconProvider::SetMapResourceDirectory(const std::filesystem::path& path) {
    mapResourceDirectory_ = path;
    iconCache_.clear();
}

QIcon IconProvider::IconForCategory(const QString& categoryId) {
    auto it = categoryToTexture_.find(categoryId);
    if (it == categoryToTexture_.end()) {
        return {};
    }
    return IconForTexture(it.value(), categoryLayer_.value(categoryId));
}

QIcon IconProvider::IconForFunction(const QString& functionName) {
    if (functionName.isEmpty()) {
        return {};
    }
    auto it = functionToCategory_.find(functionName);
    if (it == functionToCategory_.end()) {
        return {};
    }
    return IconForCategory(it.value());
}

QString IconProvider::CategoryForFunction(const QString& functionName) const {
    return functionToCategory_.value(functionName);
}

QString IconProvider::TextureForCategory(const QString& categoryId) const {
    return categoryToTexture_.value(categoryId);
}

} // namespace War3Map::QtApp
