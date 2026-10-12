#include "ObjectResourceQueryDetail.h"
#include "War3MapCore/Game/SoundInfoTables.h"
#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Image/TextureImage.h"
#include "War3MapCore/Text/SlkCells.h"

#include <algorithm>

namespace War3Map::Services {
using namespace ObjectResourceDetail;
namespace {
bool Missing(const std::vector<Diagnostic>& diagnostics) {
    return !diagnostics.empty() && std::all_of(diagnostics.begin(), diagnostics.end(), [](const auto& item) {
        return item.code == "RESOURCE_NOT_FOUND";
    });
}
Result<std::string> SoundLabelPath(const Resources::ResourceResolver& resolver, std::string_view label) {
    std::optional<std::string> result;
    for (const auto name : Game::kSoundInfoTables) {
        auto file = resolver.Read("UI/SoundInfo/" + std::string(name) + ".slk");
        if (!file.Succeeded()) {
            if (Missing(file.diagnostics)) continue;
            return {std::nullopt, std::move(file.diagnostics)};
        }
        if (file.value->bytes.size() > 4 * 1024 * 1024)
            return Result<std::string>::Failure("Object.Resource", "OBJECT_RESOURCE_SOUND_TABLE_LIMIT", "声音标签表超过4MiB读取预算。");
        auto table = Text::ParseSlkCells(std::string_view(reinterpret_cast<const char*>(file.value->bytes.data()),
            file.value->bytes.size()), Text::SlkCellMode::CatalogStrict);
        if (!table.Succeeded()) return {std::nullopt, std::move(table.diagnostics)};
        int filesColumn = 0, baseColumn = 0;
        const auto headers = table.value->find(1);
        if (headers == table.value->end()) return Result<std::string>::Failure("Object.Resource", "OBJECT_RESOURCE_SOUND_TABLE_HEADER", "声音标签表缺少表头。");
        for (const auto& [column, title] : headers->second) {
            if (Text::LowerAscii(title) == "filenames") filesColumn = column;
            if (Text::LowerAscii(title) == "directorybase") baseColumn = column;
        }
        for (const auto& [number, row] : *table.value) {
            if (number == 1) continue;
            const auto id = row.find(1);
            if (id == row.end() || id->second != label) continue;
            const auto files = row.find(filesColumn), base = row.find(baseColumn);
            if (files == row.end()) return Result<std::string>::Failure("Object.Resource", "OBJECT_RESOURCE_SOUND_FILES_MISSING", "声音标签没有 FileNames，不能猜测文件路径。");
            const auto values = Values(files->second, true);
            if (values.empty() || values.front().empty() || values.front() == "_")
                return Result<std::string>::Failure("Object.Resource", "OBJECT_RESOURCE_SOUND_FILES_MISSING", "声音标签没有可试听文件。");
            auto path = base == row.end() ? values.front() : JoinPath(base->second, values.front());
            if (result && Text::LowerAscii(Slash(*result)) != Text::LowerAscii(Slash(path)))
                return Result<std::string>::Failure("Object.Resource", "OBJECT_RESOURCE_SOUND_LABEL_AMBIGUOUS", "声音标签对应多个文件定义，不能选择其中一项。");
            result = std::move(path);
        }
    }
    if (!result) return Result<std::string>::Failure("Object.Resource", "OBJECT_RESOURCE_SOUND_LABEL_MISSING", "声音标签不在可读的原版声音表中。");
    return Result<std::string>::Success(std::move(*result));
}
}

Result<ObjectResourceInfo> InspectObjectResource(const Resources::ResourceResolver& resolver,
    std::string_view value, ObjectResourceKind kind, std::size_t maxBytes) {
    using Return = Result<ObjectResourceInfo>;
    if (value.empty() || value.size() > 4096 || maxBytes == 0 || maxBytes > 32 * 1024 * 1024
        || ObjectResourceKindName(kind) == "unknown")
        return Return::Failure("Object.Resource", "OBJECT_RESOURCE_INFO_LIMITS", "资源信息查询需要有效的路径、类型和读取上限。");
    ObjectResourceInfo result;
    result.requestedValue = value; result.kind = kind; result.resourcePath = value;
    if (kind == ObjectResourceKind::Model) result.resourcePath = Render::MdxModelFile(value, 0, 1);
    if (kind == ObjectResourceKind::SoundLabel) {
        auto path = SoundLabelPath(resolver, value);
        if (!path.Succeeded()) return {std::nullopt, std::move(path.diagnostics)};
        result.resourcePath = std::move(*path.value);
        result.diagnostics.push_back(Warning("OBJECT_RESOURCE_SOUND_LABEL_PREVIEW", "声音标签预览取 FileNames 的第一项；不改变游戏中的随机播放规则。"));
    }
    if (kind == ObjectResourceKind::UnitSoundSet)
        return Return::Failure("Object.Resource", "OBJECT_RESOURCE_UNIT_SOUND_SET", "单位声音组包含多个事件标签；保留组名，尚不把组名猜成单个声音文件。");
    auto resource = resolver.Read(result.resourcePath);
    if (!resource.Succeeded()) return {std::nullopt, std::move(resource.diagnostics)};
    if (resource.value->bytes.size() > maxBytes)
        return Return::Failure("Object.Resource", "OBJECT_RESOURCE_READ_LIMIT", "资源超过本次读取预算。");
    result.source = resource.value->location; result.bytes = resource.value->bytes.size();
    if (kind == ObjectResourceKind::Model) {
        Render::MdxLimits limits; limits.maxInputBytes = maxBytes;
        auto model = Render::ParseMdx(resource.value->bytes, limits);
        if (!model.Succeeded()) return {std::nullopt, std::move(model.diagnostics)};
        for (const auto& sequence : model.value->sequences)
            result.sequences.push_back({sequence.name, sequence.start, sequence.end, sequence.flags == 0, sequence.Usable()});
        result.diagnostics.insert(result.diagnostics.end(), model.diagnostics.begin(), model.diagnostics.end());
    } else if (kind == ObjectResourceKind::Icon || kind == ObjectResourceKind::Texture) {
        auto image = Image::DecodeTexture(result.resourcePath, resource.value->bytes);
        if (!image.Succeeded()) return {std::nullopt, std::move(image.diagnostics)};
        result.width = image.value->width; result.height = image.value->height;
    }
    return Return::Success(std::move(result));
}
} // namespace War3Map::Services
