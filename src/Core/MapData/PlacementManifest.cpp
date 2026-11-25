#include "War3MapCore/MapData/PlacementManifest.h"
#include "War3MapCore/IO/FileBytes.h"
#include "../Serialization/StrictJson.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

namespace War3Map::MapData {
namespace {
constexpr std::string_view kDomain = "Placement.Manifest";

struct Failure { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Fail(const char* code, std::string message) {
    throw Failure{Status::Failure(std::string(kDomain), code, std::move(message)).diagnostics};
}
template<class T, class Function> Result<T> Guard(Function function) {
    try { return Result<T>::Success(function()); }
    catch (Failure& failed) { return {{}, std::move(failed.diagnostics)}; }
    catch (const std::bad_alloc&) {
        return Result<T>::Failure(std::string(kDomain), "PLACEMENT_MANIFEST_MEMORY", "摆放清单内存不足，地图未修改。");
    } catch (const std::length_error&) {
        return Result<T>::Failure(std::string(kDomain), "PLACEMENT_MANIFEST_MEMORY", "摆放清单容量超限，地图未修改。");
    }
}

using Json = nlohmann::json;

std::string Lower(std::string_view text) {
    std::string lower(text);
    for (auto& c : lower) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return lower;
}

bool PrintableAscii(std::string_view text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= 0x20 && c <= 0x7e; });
}

void OnlyKeys(const Json& object, std::initializer_list<std::string_view> allowed, const std::string& where) {
    if (!object.is_object()) Fail("PLACEMENT_MANIFEST_SHAPE", where + " 必须是 JSON 对象。");
    for (const auto& [key, value] : object.items()) {
        (void)value;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            Fail("PLACEMENT_MANIFEST_SHAPE", where + " 含未知字段：" + key);
    }
}

const Json& Required(const Json& object, const char* key, const std::string& where) {
    const auto found = object.find(key);
    if (found == object.end()) Fail("PLACEMENT_MANIFEST_SHAPE", where + " 缺少 " + key + "。");
    return *found;
}

std::string Text(const Json& value, const std::string& where, std::size_t maximum) {
    if (!value.is_string()) Fail("PLACEMENT_MANIFEST_SHAPE", where + " 必须是字符串。");
    auto text = value.get<std::string>();
    if (text.empty() || text.size() > maximum)
        Fail("PLACEMENT_MANIFEST_SHAPE", where + " 不能为空，最长 " + std::to_string(maximum) + " 字节。");
    return text;
}

float Number(const Json& value, const std::string& where, double limit) {
    if (!value.is_number()) Fail("PLACEMENT_MANIFEST_NUMBER", where + " 必须是数值。");
    const auto number = value.get<double>();
    if (!std::isfinite(number) || std::abs(number) > limit)
        Fail("PLACEMENT_MANIFEST_NUMBER", where + " 必须是有限数值，绝对值不超过 " + std::to_string(std::int64_t(limit)) + "。");
    return float(number);
}

// 包内路径：Sonne\ 开头、可打印 ASCII、反斜杠分隔、无空段及“.”“..”段。
bool ValidArchivePath(std::string_view path) {
    if (path.size() <= PlacementManifestArchivePrefix.size() || path.size() > 260 || !PrintableAscii(path)) return false;
    if (Lower(path.substr(0, PlacementManifestArchivePrefix.size())) != Lower(PlacementManifestArchivePrefix)) return false;
    if (path.find('/') != std::string_view::npos || path.find(':') != std::string_view::npos) return false;
    std::size_t start = 0;
    while (true) {
        const auto end = path.find('\\', start);
        const auto part = path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (part.empty() || part == "." || part == ".." || part.front() == ' ' || part.back() == ' ') return false;
        if (end == std::string_view::npos) return true;
        start = end + 1;
    }
}

bool ValidKey(std::string_view key) {
    return !key.empty() && key.size() <= 32 && std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    });
}

bool ValidRawcode(std::string_view code) {
    return code.size() == 4 && std::all_of(code.begin(), code.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    });
}
} // namespace

Result<PlacementManifest> ParsePlacementManifest(std::span<const std::byte> json) {
    return Guard<PlacementManifest>([&] {
        Serialization::StrictJsonLimits limits;
        limits.maxBytes = PlacementManifestMaximumBytes; limits.maxDepth = 5; limits.maxNodes = 4'000'000;
        limits.maxStringBytes = 1024;
        auto parsed = Serialization::ParseStrictJson(json, limits);
        if (!parsed.Succeeded() || !parsed.value) throw Failure{std::move(parsed.diagnostics)};
        const auto& root = *parsed.value;
        OnlyKeys(root, {"format", "files", "types", "instances"}, "摆放清单");
        const auto& format = Required(root, "format", "摆放清单");
        if (!format.is_string() || format.get<std::string>() != PlacementManifestFormatV1)
            Fail("PLACEMENT_MANIFEST_FORMAT", "摆放清单的 format 必须是 " + std::string(PlacementManifestFormatV1) + "。");
        PlacementManifest manifest;

        std::set<std::string> archiveKeys;
        if (const auto files = root.find("files"); files != root.end()) {
            if (!files->is_array() || files->size() > PlacementManifestMaximumFiles)
                Fail("PLACEMENT_MANIFEST_FILES", "files 须为最多 " + std::to_string(PlacementManifestMaximumFiles) + " 项的数组。");
            for (std::size_t index = 0; index < files->size(); ++index) {
                const auto where = "files[" + std::to_string(index) + "]";
                const auto& entry = (*files)[index];
                OnlyKeys(entry, {"source", "archivePath"}, where);
                PlacementManifestFile file;
                file.source = Text(Required(entry, "source", where), where + ".source", 260);
                if (!IO::IsSafeManifestRelativePath(file.source))
                    Fail("PLACEMENT_MANIFEST_SOURCE_PATH", where + ".source 必须是清单目录内的相对路径，不能是绝对路径或含“..”。");
                file.archivePath = Text(Required(entry, "archivePath", where), where + ".archivePath", 260);
                if (!ValidArchivePath(file.archivePath))
                    Fail("PLACEMENT_MANIFEST_ARCHIVE_PATH", where + ".archivePath 须以 Sonne\\ 开头，只用 ASCII 与反斜杠，不含空段或“..”：" + file.archivePath);
                if (!archiveKeys.insert(Lower(file.archivePath)).second)
                    Fail("PLACEMENT_MANIFEST_ARCHIVE_DUPLICATE", where + ".archivePath 重复（不分大小写）：" + file.archivePath);
                manifest.files.push_back(std::move(file));
            }
        }

        const auto& types = Required(root, "types", "摆放清单");
        if (!types.is_array() || types.empty() || types.size() > PlacementManifestMaximumTypes)
            Fail("PLACEMENT_MANIFEST_TYPES", "types 须为 1.." + std::to_string(PlacementManifestMaximumTypes) + " 项的数组。");
        std::map<std::string, std::uint32_t> keys;
        for (std::size_t index = 0; index < types.size(); ++index) {
            const auto where = "types[" + std::to_string(index) + "]";
            const auto& entry = types[index];
            OnlyKeys(entry, {"key", "kind", "model", "name", "base", "followGround"}, where);
            PlacementManifestType type;
            type.key = Text(Required(entry, "key", where), where + ".key", 32);
            if (!ValidKey(type.key)) Fail("PLACEMENT_MANIFEST_KEY", where + ".key 只能用字母、数字和下划线，最长 32：" + type.key);
            if (!keys.emplace(type.key, std::uint32_t(index)).second)
                Fail("PLACEMENT_MANIFEST_KEY", where + ".key 重复：" + type.key);
            const auto kind = Text(Required(entry, "kind", where), where + ".kind", 16);
            if (kind == "doodad") type.kind = PlacementManifestKind::Doodad;
            else if (kind == "unit") type.kind = PlacementManifestKind::Unit;
            else Fail("PLACEMENT_MANIFEST_KIND", where + ".kind 只能是 doodad 或 unit：" + kind);
            type.model = Text(Required(entry, "model", where), where + ".model", 260);
            if (!PrintableAscii(type.model) || type.model.find('/') != std::string::npos)
                Fail("PLACEMENT_MANIFEST_MODEL", where + ".model 须为可打印 ASCII 的包内路径，用反斜杠分隔：" + type.model);
            if (Lower(type.model).starts_with(Lower(PlacementManifestArchivePrefix)) && !archiveKeys.contains(Lower(type.model)))
                Fail("PLACEMENT_MANIFEST_MODEL", where + ".model 指向的 Sonne\\ 文件不在 files 里：" + type.model);
            if (const auto name = entry.find("name"); name != entry.end()) type.name = Text(*name, where + ".name", 256);
            else type.name = type.key;
            if (const auto base = entry.find("base"); base != entry.end()) {
                type.base = Text(*base, where + ".base", 4);
                if (!ValidRawcode(*type.base)) Fail("PLACEMENT_MANIFEST_BASE", where + ".base 须为四位字母或数字的 rawcode：" + *type.base);
            }
            if (type.kind == PlacementManifestKind::Unit && !type.base)
                Fail("PLACEMENT_MANIFEST_BASE", where + " 是单位类型，必须填写 base。");
            if (const auto follow = entry.find("followGround"); follow != entry.end()) {
                if (!follow->is_boolean()) Fail("PLACEMENT_MANIFEST_SHAPE", where + ".followGround 必须是 true 或 false。");
                if (type.kind != PlacementManifestKind::Doodad)
                    Fail("PLACEMENT_MANIFEST_SHAPE", where + ".followGround 只用于装饰物。");
                type.followGround = follow->get<bool>();
            }
            manifest.types.push_back(std::move(type));
        }

        const auto& instances = Required(root, "instances", "摆放清单");
        if (!instances.is_array() || instances.empty() || instances.size() > PlacementManifestMaximumInstances)
            Fail("PLACEMENT_MANIFEST_INSTANCES", "instances 须为 1.." + std::to_string(PlacementManifestMaximumInstances) + " 项的数组。");
        manifest.instances.reserve(instances.size());
        for (std::size_t index = 0; index < instances.size(); ++index) {
            const auto where = "instances[" + std::to_string(index) + "]";
            const auto& entry = instances[index];
            OnlyKeys(entry, {"type", "x", "y", "z", "facing", "scale", "variation"}, where);
            PlacementManifestInstance instance;
            const auto key = Text(Required(entry, "type", where), where + ".type", 32);
            const auto type = keys.find(key);
            if (type == keys.end()) Fail("PLACEMENT_MANIFEST_TYPE_REFERENCE", where + ".type 不在 types 里：" + key);
            instance.type = type->second;
            instance.position = {Number(Required(entry, "x", where), where + ".x", 1.0e6),
                Number(Required(entry, "y", where), where + ".y", 1.0e6), Number(Required(entry, "z", where), where + ".z", 1.0e6)};
            instance.facingDegrees = Number(Required(entry, "facing", where), where + ".facing", 1.0e6);
            const auto& scale = Required(entry, "scale", where);
            if (!scale.is_array() || scale.size() != 3) Fail("PLACEMENT_MANIFEST_SCALE", where + ".scale 须为三个数。");
            for (std::size_t axis = 0; axis < 3; ++axis) {
                instance.scale[axis] = Number(scale[axis], where + ".scale", 1000.0);
                if (!(instance.scale[axis] > 0.0f)) Fail("PLACEMENT_MANIFEST_SCALE", where + ".scale 必须为正数。");
            }
            if (const auto variation = entry.find("variation"); variation != entry.end()) {
                if (!variation->is_number_integer() || variation->get<std::int64_t>() < 0 || variation->get<std::int64_t>() > 1023)
                    Fail("PLACEMENT_MANIFEST_VARIATION", where + ".variation 须为 0..1023 的整数。");
                instance.variation = std::int32_t(variation->get<std::int64_t>());
            }
            manifest.instances.push_back(instance);
        }
        return manifest;
    });
}

Result<PlacementManifest> ReadPlacementManifest(const std::filesystem::path& manifestPath) {
    auto json = IO::ReadFileBytes(manifestPath, PlacementManifestMaximumBytes);
    if (!json.Succeeded() || !json.value) return {{}, std::move(json.diagnostics)};
    auto manifest = ParsePlacementManifest(*json.value);
    if (!manifest.Succeeded() || !manifest.value) return manifest;
    std::uint64_t total = 0;
    for (auto& file : manifest.value->files) {
        auto bytes = IO::ReadFileBytes(manifestPath.parent_path() / IO::PathFromUtf8(file.source), PlacementManifestMaximumFileBytes);
        if (!bytes.Succeeded() || !bytes.value) return {{}, std::move(bytes.diagnostics)};
        total += bytes.value->size();
        if (total > PlacementManifestMaximumTotalFileBytes)
            return Result<PlacementManifest>::Failure(std::string(kDomain), "PLACEMENT_MANIFEST_FILE_BUDGET",
                "导入文件合计超过 " + std::to_string(PlacementManifestMaximumTotalFileBytes / (1024 * 1024)) + " MiB。");
        file.bytes = std::move(*bytes.value);
    }
    return manifest;
}

} // namespace War3Map::MapData
