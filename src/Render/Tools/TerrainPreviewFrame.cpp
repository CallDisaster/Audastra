#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Render/TerrainCliffs.h"
#include "War3MapCore/Render/TerrainMaterialPreview.h"
#include "War3MapCore/Render/TerrainPreviewMesh.h"
#include "War3MapCore/RenderPluginV2.h"
#include "War3MapCore/Resources/ResourceResolver.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
constexpr std::uint64_t kMaximumArchiveBytes = 256ull * 1024 * 1024;
constexpr std::uint32_t kMinimumImageSize = 64, kMaximumImageSize = 2048;
constexpr std::uint64_t kMaximumRgbaBytes = 16ull * 1024 * 1024;
constexpr std::size_t kMaximumAssetBytes = 16 * 1024 * 1024;

struct Options {
    fs::path map, output, plugin, gameRoot;
    bool hasPlugin = false, hasGameRoot = false, groundSurface = false, includeCliffs = false;
    std::optional<std::uint32_t> waterFrameIndex;
    std::string sourceSha256, terrainSha256;
    War3RenderCameraV2 camera{sizeof(War3RenderCameraV2)};
    std::uint32_t width = 0, height = 0;
};

[[noreturn]] void Fail(std::string_view message) { throw std::runtime_error(std::string(message)); }
void Require(bool condition, std::string_view message) { if (!condition) Fail(message); }

std::string Utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string Ascii(std::wstring_view source, std::string_view name) {
    Require(!source.empty(), std::string(name) + " requires a value");
    std::string result;
    result.reserve(source.size());
    for (const auto character : source) {
        Require(static_cast<unsigned int>(character) < 128, std::string(name) + " must be ASCII");
        result.push_back(static_cast<char>(character));
    }
    return result;
}

bool LowerSha256(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char digit) {
        return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
    });
}

template <typename T>
T ParseUnsigned(std::wstring_view source, std::string_view name) {
    const auto text = Ascii(source, name);
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
        std::string(name) + " must be an unsigned decimal integer");
    return value;
}

float ParseFloat(std::wstring_view source, std::string_view name) {
    const auto text = Ascii(source, name);
    float value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    Require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && std::isfinite(value),
        std::string(name) + " must be a finite decimal number");
    return value;
}

Options ParseOptions(int argc, wchar_t** argv) {
    Options options;
    constexpr std::array<std::wstring_view, 17> names{L"--map", L"--expected-source-sha256", L"--expected-terrain-sha256", L"--output",
        L"--target-x", L"--target-y", L"--target-z", L"--distance", L"--yaw", L"--pitch", L"--width", L"--height", L"--plugin",
        L"--surface", L"--game-root", L"--water-frame", L"--cliffs"};
    std::array<bool, names.size()> seen{};
    for (int index = 1; index < argc; index += 2) {
        Require(index + 1 < argc, "every option requires a value");
        const std::wstring_view flag(argv[index]);
        const auto found = std::find(names.begin(), names.end(), flag);
        Require(found != names.end(), "unknown option");
        const auto slot = static_cast<std::size_t>(found - names.begin());
        Require(!seen[slot], "duplicate option");
        seen[slot] = true;
        const std::wstring_view value(argv[index + 1]);
        switch (slot) {
        case 0: options.map = value; break;
        case 1: options.sourceSha256 = Ascii(value, "--expected-source-sha256"); break;
        case 2: options.terrainSha256 = Ascii(value, "--expected-terrain-sha256"); break;
        case 3: options.output = value; break;
        case 4: options.camera.targetX = ParseFloat(value, "--target-x"); break;
        case 5: options.camera.targetY = ParseFloat(value, "--target-y"); break;
        case 6: options.camera.targetZ = ParseFloat(value, "--target-z"); break;
        case 7: options.camera.distance = ParseFloat(value, "--distance"); break;
        case 8: options.camera.yawRadians = ParseFloat(value, "--yaw"); break;
        case 9: options.camera.pitchRadians = ParseFloat(value, "--pitch"); break;
        case 10: options.width = ParseUnsigned<std::uint32_t>(value, "--width"); break;
        case 11: options.height = ParseUnsigned<std::uint32_t>(value, "--height"); break;
        case 12: options.plugin = value; options.hasPlugin = true; break;
        case 13:
            Require(value == L"ground" || value == L"height", "--surface must be ground or height");
            options.groundSurface = value == L"ground";
            break;
        case 14: options.gameRoot = value; options.hasGameRoot = true; break;
        case 15: options.waterFrameIndex = ParseUnsigned<std::uint32_t>(value, "--water-frame"); break;
        case 16: Require(value == L"1", "--cliffs requires 1"); options.includeCliffs = true; break;
        default: Fail("unreachable option");
        }
    }
    Require(std::all_of(seen.begin(), seen.begin() + 12, [](bool present) { return present; }),
        "usage: war3-terrain-preview --map <source.w3x> --expected-source-sha256 <sha> "
        "--expected-terrain-sha256 <sha> --output <new.bmp> "
        "--target-x <x> --target-y <y> --target-z <z> --distance <units> "
        "--yaw <radians> --pitch <radians> --width <pixels> --height <pixels> [--plugin <dll>] "
        "[--surface ground --game-root <absolute-game-root> [--water-frame <zero-based-index>] [--cliffs 1]]");
    Require(LowerSha256(options.sourceSha256) && LowerSha256(options.terrainSha256),
        "expected SHA-256 values must be 64 lowercase hexadecimal digits");
    Require(options.map.is_absolute() && options.output.is_absolute() &&
        (!options.hasPlugin || options.plugin.is_absolute()) &&
        (!options.hasGameRoot || options.gameRoot.is_absolute()), "paths must be absolute");
    Require(options.groundSurface == options.hasGameRoot,
        "--surface ground requires --game-root; --game-root is only valid with --surface ground");
    Require((!options.waterFrameIndex && !options.includeCliffs) || options.groundSurface,
        "--water-frame and --cliffs are only valid with --surface ground");
    Require(options.output.extension() == L".bmp", "output path must end in .bmp");
    Require(options.width >= kMinimumImageSize && options.width <= kMaximumImageSize &&
        options.height >= kMinimumImageSize && options.height <= kMaximumImageSize &&
        static_cast<std::uint64_t>(options.width) * options.height * 4 <= kMaximumRgbaBytes,
        "capture dimensions exceed 64..2048 pixels or 16 MiB RGBA budget");
    const auto coordinate = [](float value) { return std::isfinite(value) && std::abs(value) <= 10000000.0f; };
    Require(coordinate(options.camera.targetX) && coordinate(options.camera.targetY) &&
        coordinate(options.camera.targetZ) && coordinate(options.camera.yawRadians) &&
        options.camera.distance >= 16 && options.camera.distance <= 400000 &&
        options.camera.pitchRadians >= 0.05f && options.camera.pitchRadians <= 1.5f,
        "camera is outside the renderer bounds");
    return options;
}

struct ReadLease {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit ReadLease(const fs::path& path) : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {
        Require(handle != INVALID_HANDLE_VALUE, "could not acquire a read-only map lease");
        const auto checked = [this](bool valid, std::string_view reason) {
            if (valid) return;
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            Fail(reason);
        };
        LARGE_INTEGER size{};
        checked(GetFileSizeEx(handle, &size) && size.QuadPart >= 0 &&
            static_cast<std::uint64_t>(size.QuadPart) <= kMaximumArchiveBytes,
            "map archive exceeds the 256 MiB input budget");
        BY_HANDLE_FILE_INFORMATION information{};
        checked(GetFileInformationByHandle(handle, &information) &&
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0, "map source is not a regular file");
    }
    ~ReadLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    ReadLease(const ReadLease&) = delete;
    ReadLease& operator=(const ReadLease&) = delete;
};

struct GameArchiveLease {
    std::string name;
    fs::path path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    GameArchiveLease(std::string archiveName, fs::path archivePath)
        : name(std::move(archiveName)), path(std::move(archivePath)),
          handle(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {
        Require(handle != INVALID_HANDLE_VALUE, "could not lock game MPQ for a stable read-only preview");
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(handle, &information) ||
            (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            Fail("game MPQ is not a regular file");
        }
    }
    ~GameArchiveLease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    GameArchiveLease(const GameArchiveLease&) = delete;
    GameArchiveLease& operator=(const GameArchiveLease&) = delete;
};

std::vector<std::unique_ptr<GameArchiveLease>> LockGameArchives(const fs::path& root) {
    // 与挂载、校验共用归档域，避免新增可读取的归档却漏掉稳定来源租约。
    const auto known = War3Map::Game::GameDataSource::CoreArchiveNames();
    std::map<std::string, fs::path, std::less<>> paths;
    std::error_code error;
    for (fs::directory_iterator at(root, error), end; !error && at != end; at.increment(error)) {
        auto name = at->path().filename().string();
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        if (std::find(known.begin(), known.end(), name) == known.end()) continue;
        Require(paths.emplace(name, at->path()).second, "game root contains duplicate core MPQ names");
    }
    Require(!error, "could not enumerate game MPQ files");
    Require(paths.contains("war3.mpq") && paths.contains("war3x.mpq"),
        "game root must contain base and expansion MPQ files");
    std::vector<std::unique_ptr<GameArchiveLease>> leases;
    for (auto it = known.rbegin(); it != known.rend(); ++it)
        if (const auto found = paths.find(*it); found != paths.end())
            leases.push_back(std::make_unique<GameArchiveLease>(std::string(*it), found->second));
    return leases;
}

struct Module {
    HMODULE handle = nullptr;
    explicit Module(const fs::path& path) {
        handle = LoadLibraryExW(path.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(handle != nullptr, "D3D11 render plugin load failed");
    }
    ~Module() { if (handle) FreeLibrary(handle); }
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
};

struct Renderer {
    const War3RenderPluginApiV2* api = nullptr;
    War3RendererHandle handle = nullptr;
    Renderer(const War3RenderPluginApiV2* plugin, const War3RenderCreateInfoV2& info) : api(plugin) {
        handle = api->createRenderer(&info);
        Require(handle != nullptr, "D3D11 WARP renderer creation failed");
    }
    ~Renderer() { if (handle) api->destroyRenderer(handle); }
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
};

template <typename F>
bool HasApi(const War3RenderPluginApiV2* api, std::size_t offset, F War3RenderPluginApiV2::* member) {
    return api->structSize >= offset + sizeof(F) && (api->*member) != nullptr;
}

bool SameCamera(const War3RenderCameraV2& first, const War3RenderCameraV2& second) {
    return first.structSize == second.structSize && first.targetX == second.targetX && first.targetY == second.targetY &&
        first.targetZ == second.targetZ && first.distance == second.distance &&
        first.yawRadians == second.yawRadians && first.pitchRadians == second.pitchRadians;
}

void WriteAll(HANDLE file, const void* data, std::size_t bytes) {
    const auto* source = static_cast<const std::byte*>(data);
    while (bytes) {
        const auto amount = static_cast<DWORD>((std::min)(bytes, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        Require(WriteFile(file, source, amount, &written, nullptr) && written == amount, "BMP write failed");
        source += amount;
        bytes -= amount;
    }
}

void SaveBmpNew(const fs::path& output, std::span<const std::uint8_t> rgba,
    std::uint32_t width, std::uint32_t height) {
    std::error_code error;
    Require(!fs::exists(output, error) && !error, "output exists or cannot be checked");
    const auto pixelBytes = static_cast<std::size_t>(width) * height * 4;
    Require(rgba.size() == pixelBytes && pixelBytes <= std::numeric_limits<DWORD>::max() - 54,
        "BMP dimensions are invalid");
    fs::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
        temporary = output;
        temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        Require(GetLastError() == ERROR_FILE_EXISTS, "could not create a temporary BMP beside output");
    }
    Require(file != INVALID_HANDLE_VALUE, "temporary BMP names are exhausted");
    struct TemporaryFile {
        HANDLE file;
        fs::path path;
        bool published = false;
        ~TemporaryFile() {
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            if (!published) DeleteFileW(path.c_str());
        }
    } guard{file, temporary};
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = static_cast<DWORD>(sizeof(header) + sizeof(BITMAPINFOHEADER));
    header.bfSize = static_cast<DWORD>(header.bfOffBits + pixelBytes);
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height); // 顶行先存，与 captureFrameRgba 一致。
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = static_cast<DWORD>(pixelBytes);
    WriteAll(file, &header, sizeof(header));
    WriteAll(file, &info, sizeof(info));
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width) * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto* source = rgba.data() + static_cast<std::size_t>(y) * row.size();
        for (std::size_t x = 0; x < row.size(); x += 4) {
            row[x] = source[x + 2]; row[x + 1] = source[x + 1];
            row[x + 2] = source[x]; row[x + 3] = source[x + 3];
        }
        WriteAll(file, row.data(), row.size());
    }
    Require(FlushFileBuffers(file), "BMP flush failed");
    Require(CloseHandle(file), "BMP close failed");
    guard.file = INVALID_HANDLE_VALUE;
    Require(MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH),
        "BMP publish failed; output may already exist");
    guard.published = true;
}

void JsonString(std::ostream& output, std::string_view value) {
    constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') output << '\\' << static_cast<char>(character);
        else if (character < 0x20) output << "\\u00" << hex[character >> 4] << hex[character & 15];
        else output << static_cast<char>(character);
    }
    output << '"';
}

std::uint64_t RenderRevision(std::string_view sourceSha256, std::string_view terrainSha256) {
    const auto identity = War3Map::Sha256Text(std::string(sourceSha256) + ":" + std::string(terrainSha256));
    Require(identity.Succeeded(), "could not derive render revision");
    std::uint64_t revision = 0;
    const auto parsed = std::from_chars(identity.value->data(), identity.value->data() + 16, revision, 16);
    Require(parsed.ec == std::errc{} && parsed.ptr == identity.value->data() + 16,
        "render revision digest is invalid");
    return revision ? revision : 1;
}

struct AssetRead {
    std::string layer, path, sha256;
};

struct GroundMaterialData {
    War3Map::Render::TerrainMaterialPreview preview;
    std::optional<War3Map::Render::WorldModelScene> cliffScene;
    std::vector<War3Map::Diagnostic> diagnostics;
    std::vector<AssetRead> assets;
    std::string assetFingerprint;
    std::string waterFrameArchive;
};

void PrintDiagnostics(std::string_view prefix, const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cerr << prefix << '=' << diagnostic.code << ' ' << diagnostic.message << '\n';
}

GroundMaterialData BuildGroundMaterialData(const War3Map::MapData::TerrainGridSnapshot& terrain,
    const War3Map::Mpq::MpqArchive& mapArchive, const fs::path& gameRoot,
    std::optional<std::uint32_t> waterFrameIndex, bool includeCliffs) {
    using namespace War3Map;
    auto mounted = Game::GameDataSource::MountStrict(gameRoot, terrain.header.mainTileset);
    if (!mounted.Succeeded()) {
        PrintDiagnostics("game_mount_error", mounted.diagnostics);
        Fail("game MPQ sources could not be mounted strictly");
    }
    auto game = std::move(*mounted.value);
    Resources::ResourceResolver resolver;
    std::vector<AssetRead> assets;
    const auto trackedRead = [&](std::string_view layer, std::string_view path,
        Result<std::vector<std::byte>> read) -> Result<std::vector<std::byte>> {
        if (!read.Succeeded()) return read;
        const auto digest = Sha256(*read.value);
        if (!digest.Succeeded()) return {std::nullopt, digest.diagnostics};
        assets.push_back({std::string(layer), std::string(path), *digest.value});
        return read;
    };
    Resources::ResourceSource mapSource;
    mapSource.id = "terrain-preview-map";
    mapSource.layer = Resources::ResourceLayer::MapImport;
    mapSource.priority = Resources::ResourcePriority::MapArchive;
    mapSource.probe = [&mapArchive](std::string_view path) {
        return Result<Resources::ResourceProbe>::Success(mapArchive.HasFile(path)
            ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(path)}
            : Resources::ResourceProbe{});
    };
    mapSource.read = [&mapArchive, &trackedRead](std::string_view path) {
        return trackedRead("map", path, mapArchive.ReadFileBounded(path, kMaximumAssetBytes));
    };
    Require(resolver.AddSource(std::move(mapSource)).Succeeded(), "could not register map resource source");
    Resources::ResourceSource gameSource;
    gameSource.id = "terrain-preview-game";
    gameSource.layer = Resources::ResourceLayer::Game;
    gameSource.priority = Resources::ResourcePriority::GameBase;
    gameSource.probe = [&game](std::string_view path) {
        return Result<Resources::ResourceProbe>::Success(game.HasFile(std::string(path))
            ? Resources::ResourceProbe{Resources::ResourceProbeState::Found, std::string(path)}
            : Resources::ResourceProbe{});
    };
    gameSource.read = [&game, &trackedRead](std::string_view path) {
        // 同一次有界读取同时给出命中的 MPQ；高层损坏/超额不会转读低层。
        auto read = game.ReadFileBoundedWithSource(std::string(path), kMaximumAssetBytes);
        if (!read.Succeeded()) return Result<std::vector<std::byte>>{std::nullopt, std::move(read.diagnostics)};
        return trackedRead(read.value->archiveName, path,
            Result<std::vector<std::byte>>::Success(std::move(read.value->bytes)));
    };
    Require(resolver.AddSource(std::move(gameSource)).Succeeded(), "could not register game resource source");

    auto built = Render::BuildTerrainMaterialPreview(terrain, resolver, {}, {}, waterFrameIndex, includeCliffs);
    if (!built.Succeeded()) {
        PrintDiagnostics("ground_scene_error", built.diagnostics);
        Fail("ground material scene could not be built without invalid or unreported fallback");
    }
    Require(built.value->coverage.texturedTiles != 0 && !built.value->ground.textures.empty(),
        "ground material scene has no textured ground tiles");
    GroundMaterialData data;
    data.preview = std::move(*built.value);
    data.diagnostics = std::move(built.diagnostics);
    if (data.preview.cliffs && !data.preview.cliffs->geometry.meshes.empty()) {
        const Render::ModelFileReader reader = [&resolver](std::string_view path) -> Result<std::vector<std::byte>> {
            auto resolved = resolver.Read(path);
            if (!resolved.Succeeded()) return {std::nullopt, std::move(resolved.diagnostics)};
            return Result<std::vector<std::byte>>::Success(std::move(resolved.value->bytes));
        };
        auto scene = Render::BuildWorldModelScene({}, reader, {}, data.preview.cliffs->geometry.meshes);
        if (!scene.Succeeded()) {
            PrintDiagnostics("cliff_scene_error", scene.diagnostics);
            Fail("cliff model scene could not be built");
        }
        const auto meshCount = std::count_if(data.preview.cliffs->geometry.meshes.begin(),
            data.preview.cliffs->geometry.meshes.end(), [](const auto& mesh) { return !mesh.indices.empty(); });
        Require(scene.value->models.size() == 1 && scene.value->instances.size() == 1
            && scene.value->meshes.size() == static_cast<std::size_t>(meshCount)
            && !scene.value->vertices.empty() && scene.value->untexturedLayers == 0,
            "cliff geometry or textures were omitted by model scene budget/decoder");
        data.cliffScene = std::move(*scene.value);
    }
    data.assets = std::move(assets);
    if (data.preview.water && data.preview.water->style) {
        const auto& style = *data.preview.water->style;
        const auto source = std::find_if(data.assets.begin(), data.assets.end(), [&](const AssetRead& asset) {
            return asset.path == style.frameSourcePath && asset.sha256 == style.frameSourceSha256;
        });
        Require(source != data.assets.end(), "water frame does not match recorded source bytes");
        data.waterFrameArchive = source->layer;
    }
    std::string identity;
    const auto append = [&identity](std::string_view value) {
        identity += std::to_string(value.size()) + ':';
        identity.append(value);
    };
    append(Utf8(gameRoot));
    for (const auto& asset : data.assets) {
        append(asset.layer); append(asset.path); append(asset.sha256);
    }
    const auto fingerprint = Sha256Text(identity);
    Require(fingerprint.Succeeded(), "could not derive material asset fingerprint");
    data.assetFingerprint = *fingerprint.value;
    const auto& ground = data.preview.ground;
    if (ground.waterTextureIndex) {
        Require(data.preview.water && data.preview.water->style
            && !ground.waterTiles.empty() && !ground.waterOmitted
            && *ground.waterTextureIndex < ground.textures.size(),
            "water atlas has no matching completed water scene");
        const auto& frame = *data.preview.water->style;
        Require(frame.frameSourcePath == ground.waterFrameSourcePath
            && frame.frameSourceSha256 == ground.waterFrameSourceSha256
            && std::any_of(data.assets.begin(), data.assets.end(), [&](const AssetRead& asset) {
                return asset.path == frame.frameSourcePath && asset.sha256 == frame.frameSourceSha256;
            }), "water frame does not match recorded source bytes");
    }
    for (std::size_t index = 0; index < ground.textures.size(); ++index) {
        const auto& texture = ground.textures[index];
        if (ground.waterTextureIndex && index == *ground.waterTextureIndex) continue;
        Require(texture && std::any_of(data.assets.begin(), data.assets.end(), [&](const AssetRead& asset) {
            return asset.path == texture->source.canonicalPath && asset.sha256 == texture->sourceSha256;
        }), "ground texture does not match recorded source bytes");
    }
    if (data.preview.cliffs) {
        for (const auto& model : data.preview.cliffs->modelSources)
            Require(std::any_of(data.assets.begin(), data.assets.end(), [&](const AssetRead& asset) {
                return asset.path == model.resource.canonicalPath && asset.sha256 == model.sha256;
            }), "cliff MDX does not match recorded source bytes");
        if (data.cliffScene) for (const auto& texture : data.cliffScene->textures)
            Require(std::any_of(data.assets.begin(), data.assets.end(), [&](const AssetRead& asset) {
                return asset.path == texture.path;
            }), "cliff BLP does not match a recorded source path");
    }
    return data;
}

std::string SubmitGroundScene(const War3RenderPluginApiV2* api, War3RendererHandle renderer,
    const War3Map::Render::TerrainGroundMesh& ground, std::uint64_t revision) {
    Require(HasApi(api, offsetof(War3RenderPluginApiV2, submitTerrainScene),
        &War3RenderPluginApiV2::submitTerrainScene), "D3D11 plugin lacks the ground terrain ABI");
    std::vector<War3RenderTerrainVertexV2> vertices;
    vertices.reserve(ground.vertices.size());
    for (const auto& v : ground.vertices)
        vertices.push_back({v.x, v.y, v.z, v.red, v.green, v.blue, v.u, v.v});
    std::vector<War3RenderTerrainBatchV2> batches;
    batches.reserve(ground.batches.size());
    for (const auto& b : ground.batches)
        batches.push_back({b.firstIndex, b.indexCount,
            b.textureIndex | (b.alphaBlend ? WAR3_RENDER_TERRAIN_ALPHA_BLEND : 0u)
                | (b.translucent ? WAR3_RENDER_TERRAIN_TRANSLUCENT : 0u)});
    if (HasApi(api, offsetof(War3RenderPluginApiV2, submitTerrainMipScene),
        &War3RenderPluginApiV2::submitTerrainMipScene)) {
        std::vector<std::vector<War3RenderMipRgbaV2>> mipLists(ground.textures.size());
        std::vector<War3RenderTextureMipChainV2> chains;
        chains.reserve(ground.textures.size());
        for (std::size_t index = 0; index < ground.textures.size(); ++index) {
            const auto& texture = *ground.textures[index];
            auto& mips = mipLists[index];
            const auto append = [&mips](const War3Map::Image::DecodedImage& mip) {
                mips.push_back({sizeof(War3RenderMipRgbaV2), mip.width, mip.height,
                    mip.rgba.size(), mip.rgba.data()});
            };
            append(texture.image);
            for (const auto& mip : texture.additionalMips) append(mip);
            std::uint32_t maximum = 0;
            for (std::size_t level = 1; level < mips.size(); ++level) {
                if (mips[level].width < texture.columns || mips[level].height < texture.rows
                    || mips[level].width % texture.columns != 0 || mips[level].height % texture.rows != 0) break;
                maximum = static_cast<std::uint32_t>(level);
            }
            chains.push_back({sizeof(War3RenderTextureMipChainV2), static_cast<std::uint32_t>(mips.size()),
                maximum, texture.columns, texture.rows, mips.data()});
        }
        const War3RenderTerrainMipSceneV2 scene{sizeof(scene), static_cast<std::uint32_t>(vertices.size()),
            static_cast<std::uint32_t>(ground.indices.size()), static_cast<std::uint32_t>(chains.size()),
            static_cast<std::uint32_t>(batches.size()), revision, vertices.data(), ground.indices.data(),
            chains.data(), batches.data()};
        Require(api->submitTerrainMipScene(renderer, &scene) == 0,
            "D3D11 plugin rejected the original-mip ground scene; no mip0 fallback is allowed");
        return "original-mip-chain";
    }
    std::vector<War3RenderTextureRgbaV2> textures;
    textures.reserve(ground.textures.size());
    for (const auto& texture : ground.textures)
        textures.push_back({sizeof(War3RenderTextureRgbaV2), texture->image.width, texture->image.height,
            texture->image.rgba.size(), texture->image.rgba.data()});
    const War3RenderTerrainSceneV2 scene{sizeof(scene), static_cast<std::uint32_t>(vertices.size()),
        static_cast<std::uint32_t>(ground.indices.size()), static_cast<std::uint32_t>(textures.size()),
        static_cast<std::uint32_t>(batches.size()), revision, vertices.data(), ground.indices.data(),
        textures.data(), batches.data()};
    Require(api->submitTerrainScene(renderer, &scene) == 0, "D3D11 plugin rejected the mip0 ground scene");
    return "mip0-only";
}

int Run(const Options& options) {
    ReadLease source(options.map);
    const auto sourceSha = War3Map::Sha256Handle(source.handle);
    Require(sourceSha.Succeeded() && *sourceSha.value == options.sourceSha256,
        "map source SHA-256 differs from expected value");
    War3Map::Mpq::MpqOpenLimits limits;
    limits.maxArchivePrefixBytes = 4ull * 1024 * 1024;
    limits.maxDeclaredArchiveBytes = kMaximumArchiveBytes;
    limits.maxHashTableEntries = 1u << 17;
    limits.maxBlockTableEntries = 1u << 17;
    limits.maxStrictMemberCount = 1u << 17;
    auto archive = War3Map::Mpq::MpqArchive::OpenReadOnlyHandle(source.handle, options.map, limits);
    Require(archive.Succeeded(), "could not open source map as a bounded read-only MPQ");
    auto bytes = archive.value->ReadFileBounded("war3map.w3e", War3Map::MapData::TerrainReadLimits{}.maxBytes);
    Require(bytes.Succeeded(), "war3map.w3e is missing, invalid or exceeds the 64 MiB member budget");
    const auto terrainSha = War3Map::Sha256(*bytes.value);
    Require(terrainSha.Succeeded() && *terrainSha.value == options.terrainSha256,
        "W3E member SHA-256 differs from expected value");
    auto parsed = War3Map::MapData::ParseMapTerrain(*bytes.value);
    Require(parsed.Succeeded(), "W3E terrain could not be parsed without semantic guesses");
    std::optional<War3Map::Render::TerrainPreviewMesh> heightMesh;
    std::optional<GroundMaterialData> material;
    std::vector<std::unique_ptr<GameArchiveLease>> gameLeases;
    fs::path gameRoot;
    std::size_t rampAdjusted = 0;
    if (options.groundSurface) {
        std::error_code error;
        gameRoot = fs::canonical(options.gameRoot, error);
        Require(!error && fs::is_directory(gameRoot), "--game-root must resolve to an existing game directory");
        gameLeases = LockGameArchives(gameRoot);
        material.emplace(BuildGroundMaterialData(*parsed.value->Snapshot(), *archive.value, gameRoot,
            options.waterFrameIndex, options.includeCliffs));
        rampAdjusted = material->preview.rampAdjustedVertices;
    } else {
        auto shown = *parsed.value->Snapshot();
        rampAdjusted = War3Map::Render::ApplyRampHeights(shown);
        auto mesh = War3Map::Render::BuildTerrainPreviewMesh(shown);
        Require(mesh.Succeeded() && !mesh.value->vertices.empty() && !mesh.value->indices.empty(),
            "height-color preview mesh could not be built within renderer limits");
        heightMesh = std::move(*mesh.value);
    }
    const auto revision = RenderRevision(options.sourceSha256,
        material ? options.terrainSha256 + ':' + material->assetFingerprint : options.terrainSha256);

    wchar_t executable[MAX_PATH]{};
    const auto count = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    Require(count != 0 && count < MAX_PATH, "executable path is unavailable");
    const auto pluginPath = options.hasPlugin ? options.plugin : fs::path(executable).parent_path() / L"war3-render-d3d11.dll";
    const Module module(pluginPath);
    const auto getApi = reinterpret_cast<War3GetRenderPluginApiV2>(
        GetProcAddress(module.handle, "War3MapEditor_GetRenderPluginApiV2"));
    Require(getApi != nullptr, "D3D11 render plugin lacks the V2 export");
    const auto* api = getApi(WAR3_RENDER_PLUGIN_API_V2);
    Require(api && api->apiVersion == WAR3_RENDER_PLUGIN_API_V2 &&
        HasApi(api, offsetof(War3RenderPluginApiV2, captureFrameRgba), &War3RenderPluginApiV2::captureFrameRgba) &&
        api->createRenderer && api->destroyRenderer && api->submitMesh && api->setCamera && api->getStatus,
        "D3D11 render plugin lacks the atomic frame capture API");
    const War3RenderCreateInfoV2 create{sizeof(create), options.width, options.height, 1}; // 无 HWND 的 WARP。
    const Renderer renderer(api, create);
    if (material && material->cliffScene)
        Require(War3Map::Render::CheckStaticModelSamplerSupport(api, renderer.handle, *material->cliffScene).Succeeded(),
            "WORLD_STATIC_SAMPLER_UNSUPPORTED: paired ground and cliff scene retained");
    std::string mipMode;
    if (material) mipMode = SubmitGroundScene(api, renderer.handle, material->preview.ground, revision);
    else {
        const War3RenderMeshV2 scene{sizeof(scene), static_cast<std::uint32_t>(heightMesh->vertices.size()),
            static_cast<std::uint32_t>(heightMesh->indices.size()), revision,
            heightMesh->vertices.data(), heightMesh->indices.data()};
        Require(api->submitMesh(renderer.handle, &scene) == 0, "D3D11 plugin rejected the height-color scene");
    }
    if (material && material->cliffScene) {
        Require(HasApi(api, offsetof(War3RenderPluginApiV2, submitModelScene),
            &War3RenderPluginApiV2::submitModelScene), "D3D11 plugin lacks the cliff model ABI");
        const auto descriptor = War3Map::Render::DescribeModelScene(*material->cliffScene);
        Require(api->submitModelScene(renderer.handle, descriptor.StaticScene()) == 0,
            "D3D11 plugin rejected the cliff model scene");
    }
    Require(api->setCamera(renderer.handle, &options.camera) == 0,
        "D3D11 plugin rejected the terrain camera");
    const auto rgbaBytes = static_cast<std::size_t>(options.width) * options.height * 4;
    std::vector<std::uint8_t> rgba(rgbaBytes);
    War3RenderCapturedFrameV2 receipt{sizeof(receipt)};
    const auto captured = api->captureFrameRgba(renderer.handle, rgba.data(), rgba.size(), &receipt);
    if (captured != 0) {
        War3RenderStatusV2 status{sizeof(status)};
        if (api->getStatus(renderer.handle, &status) == 0)
            std::cerr << "renderer_status=" << status.message << " result=" << status.result << '\n';
        Fail("real completed terrain frame was unavailable, failed or superseded");
    }
    const auto& frame = receipt.capture;
    const auto& view = receipt.view;
    Require(receipt.structSize == sizeof(receipt) && frame.structSize == sizeof(frame) &&
        view.structSize == sizeof(view) && receipt.camera.structSize == sizeof(receipt.camera) &&
        frame.ticket != 0 && frame.frameCount != 0 && frame.width == options.width &&
        frame.height == options.height && frame.documentRevision == revision &&
        view.width == frame.width && view.height == frame.height &&
        view.frameCount == frame.frameCount && view.documentRevision == frame.documentRevision &&
        view.surfaceEpoch != 0 && view.sceneEpoch != 0 && view.viewEpoch != 0 &&
        SameCamera(receipt.camera, options.camera) &&
        std::all_of(std::begin(view.viewProjection), std::end(view.viewProjection),
            [](float value) { return std::isfinite(value); }),
        "atomic RGBA receipt differs from the requested camera, dimensions or map revision");
    War3RenderStatusV2 status{sizeof(status)};
    Require(api->getStatus(renderer.handle, &status) == 0 && status.structSize == sizeof(status) &&
        status.result == 0 && status.deviceReady == 1 && status.software == 1 &&
        status.frameCount == frame.frameCount && status.documentRevision == revision &&
        status.width == frame.width && status.height == frame.height &&
        std::string_view(status.message, sizeof(status.message)).starts_with(material
            ? "D3D11 WARP / Feature Level 11.0 / terrain base layer"
            : "D3D11 WARP / Feature Level 11.0 / height colors"),
        "WARP terrain upload failed or renderer status no longer identifies the captured frame");
    const auto sourceAfter = War3Map::Sha256Handle(source.handle);
    Require(sourceAfter.Succeeded() && *sourceAfter.value == options.sourceSha256,
        "map source changed before screenshot publication");
    SaveBmpNew(options.output, rgba, options.width, options.height);
    const auto imageSha = War3Map::Sha256File(options.output);
    Require(imageSha.Succeeded(), "published BMP could not be hashed");

    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(9);
    std::cout << "{\"ok\":true,\"schema_version\":1,\"is_render_capture\":true,\"surface_semantics\":";
    const bool renderedWater = material && !material->preview.ground.waterTiles.empty()
        && !material->preview.ground.waterOmitted;
    const bool renderedCliffs = material && material->cliffScene
        && material->preview.cliffs && material->preview.coverage.hiddenCliffTiles != 0;
    JsonString(std::cout, renderedCliffs
        ? (renderedWater ? "ground-material-cliff-water-frame-preview" : "ground-material-cliff-preview")
        : renderedWater ? "ground-material-water-frame-preview"
        : material ? "ground-material-ramp-preview" : "height-color-ramp-preview");
    std::cout << ",\"includes_ground_textures\":" << (material ? "true" : "false")
        << ",\"includes_cliff_ramp_geometry\":" << (renderedCliffs ? "true" : "false")
        << ",\"includes_water\":" << (renderedWater ? "true" : "false")
        << ",\"includes_entities\":false"
        << ",\"ramp_height_adjusted_vertices\":" << rampAdjusted
        << ",\"terrain_binding\":{\"document_id\":";
    JsonString(std::cout, "archive-readonly:" + options.sourceSha256);
    std::cout << ",\"document_generation\":\"1\",\"revision\":\"0\",\"source_sha256\":";
    JsonString(std::cout, options.sourceSha256);
    std::cout << ",\"terrain_sha256\":";
    JsonString(std::cout, options.terrainSha256);
    std::cout << "},\"source_map\":";
    JsonString(std::cout, Utf8(options.map));
    std::cout << ",\"output\":";
    JsonString(std::cout, Utf8(options.output));
    std::cout << ",\"image_sha256\":";
    JsonString(std::cout, *imageSha.value);
    std::cout << ",\"format\":\"bmp-bgra8-top-down\",\"width\":" << frame.width
        << ",\"height\":" << frame.height << ",\"rgba_bytes\":" << rgbaBytes
        << ",\"source_vertex_width\":" << (material ? material->preview.ground.sourceWidth : heightMesh->sourceWidth)
        << ",\"source_vertex_height\":" << (material ? material->preview.ground.sourceHeight : heightMesh->sourceHeight)
        << ",\"preview_stride\":" << (material ? 1 : heightMesh->stride)
        << ",\"preview_vertex_count\":" << (material ? material->preview.ground.vertices.size()
            + (material->cliffScene ? material->cliffScene->vertices.size() : 0) : heightMesh->vertices.size())
        << ",\"preview_triangle_count\":" << (material ? material->preview.ground.indices.size()
            + (material->cliffScene ? material->cliffScene->indices.size() : 0) : heightMesh->indices.size()) / 3
        << ",\"render_revision\":\"" << revision
        << "\",\"frame_count\":\"" << frame.frameCount
        << "\",\"ticket\":\"" << frame.ticket
        << "\",\"surface_epoch\":\"" << view.surfaceEpoch
        << "\",\"scene_epoch\":\"" << view.sceneEpoch
        << "\",\"view_epoch\":\"" << view.viewEpoch << "\",\"camera\":{\"target\":["
        << receipt.camera.targetX << ',' << receipt.camera.targetY << ',' << receipt.camera.targetZ
        << "],\"distance\":" << receipt.camera.distance << ",\"yaw_radians\":" << receipt.camera.yawRadians
        << ",\"pitch_radians\":" << receipt.camera.pitchRadians << "},\"view_projection_row_major\":[";
    for (std::size_t index = 0; index < std::size(view.viewProjection); ++index) {
        if (index) std::cout << ',';
        std::cout << view.viewProjection[index];
    }
    std::cout << ']';
    if (material) {
        const auto& coverage = material->preview.coverage;
        const auto& fallback = coverage.fallback;
        const auto& waterBudget = material->preview.ground.budget.water;
        std::cout << ",\"same_frame_terrain_upload_verified\":true,\"terrain_upload\":";
        JsonString(std::cout, mipMode);
        std::cout << ",\"lighting\":\"default-neutral\",\"game_root\":";
        JsonString(std::cout, Utf8(gameRoot));
        std::cout << ",\"asset_fingerprint_sha256\":";
        JsonString(std::cout, material->assetFingerprint);
        std::cout << ",\"ground_texture_count\":" << material->preview.ground.textures.size()
            << ",\"ground_coverage\":{\"total_tiles\":" << coverage.totalTiles
            << ",\"textured_tiles\":" << coverage.texturedTiles
            << ",\"height_color_tiles\":" << coverage.heightColorTiles
            << ",\"hidden_cliff_tiles\":" << coverage.hiddenCliffTiles
            << ",\"texture_coverage_complete\":"
            << (coverage.heightColorTiles == 0 && coverage.unavailableAtlasSlots == 0 ? "true" : "false")
            << ",\"referenced_atlas_slots\":" << coverage.referencedAtlasSlots
            << ",\"loaded_atlas_slots\":" << coverage.loadedAtlasSlots
            << ",\"unavailable_atlas_slots\":" << coverage.unavailableAtlasSlots
            << ",\"water_flagged_without_water_surface\":"
            << coverage.waterFlaggedTiles - coverage.waterRenderedTiles
            << ",\"water_requested_tiles\":" << waterBudget.requested
            << ",\"water_no_contribution_tiles\":" << waterBudget.noContribution
            << ",\"water_rendered_tiles\":" << coverage.waterRenderedTiles
            << ",\"water_omitted_tiles\":" << waterBudget.omitted
            << ",\"water_omitted\":" << (coverage.waterOmitted ? "true" : "false")
            << ",\"blight_approximation_tiles\":" << coverage.blightApproximationTiles
            << ",\"fallback_reasons\":{\"blight_flagged_tiles\":" << fallback.blightTiles
            << ",\"mixed_cliff_layer_tiles\":" << fallback.mixedCliffLayerTiles
            << ",\"neighboring_cliff_tiles\":" << fallback.neighboringCliffTiles
            << ",\"invalid_ground_index_tiles\":" << fallback.invalidGroundIndexTiles
            << ",\"missing_atlas_tiles\":" << fallback.missingAtlasTiles
            << ",\"invalid_atlas_tiles\":" << fallback.invalidAtlasTiles << "}}";
        if (options.includeCliffs) {
            const auto& cliff = *material->preview.cliffs;
            std::cout << ",\"cliff_preview\":{\"candidate_tiles\":" << cliff.candidateTiles
                << ",\"cliff_tiles\":" << cliff.geometry.cliffTiles
                << ",\"placed_tiles\":" << cliff.geometry.placedTiles
                << ",\"ramp_pieces\":" << cliff.geometry.rampPieces
                << ",\"ramp_surface_tiles\":" << cliff.geometry.rampSurfaceTiles
                << ",\"unknown_type_tiles\":" << cliff.geometry.unknownTypeTiles
                << ",\"too_high_tiles\":" << cliff.geometry.tooHighTiles
                << ",\"missing_model_tiles\":" << cliff.geometry.missingModelTiles
                << ",\"hidden_ground_tiles\":" << coverage.hiddenCliffTiles
                << ",\"model_mesh_count\":" << (material->cliffScene ? material->cliffScene->meshes.size() : 0)
                << ",\"model_source_count\":" << cliff.modelSources.size()
                << ",\"texture_count\":" << (material->cliffScene ? material->cliffScene->textures.size() : 0)
                << '}';
        }
        if (options.waterFrameIndex) {
            std::cout << ",\"water_frame\":{\"requested_index\":" << *options.waterFrameIndex
                << ",\"selected\":" << (material->preview.water && material->preview.water->style ? "true" : "false")
                << ",\"identity_scope\":\"selected-source\""
                << ",\"rendered\":" << (renderedWater ? "true" : "false")
                << ",\"no_contribution_tiles\":" << waterBudget.noContribution
                << ",\"quality_omitted_tiles\":" << waterBudget.omitted
                << ",\"drawn_tiles\":" << coverage.waterRenderedTiles;
            if (material->preview.water && material->preview.water->style) {
                const auto& water = *material->preview.water;
                const auto& style = *water.style;
                std::cout << ",\"water_id\":";
                JsonString(std::cout, water.waterId);
                std::cout << ",\"frame_index\":" << style.frameIndex
                    << ",\"frame_count\":" << style.frameCount
                    << ",\"mixed_flag_tiles\":" << water.coverage.mixedWaterFlagTiles
                    << ",\"frame_source_path\":";
                JsonString(std::cout, water.frameSource.canonicalPath);
                std::cout << ",\"frame_source_sha256\":";
                JsonString(std::cout, style.frameSourceSha256);
                std::cout << ",\"frame_source_archive\":";
                JsonString(std::cout, material->waterFrameArchive == "map"
                    ? Utf8(options.map) : material->waterFrameArchive);
                std::cout << ",\"water_table_path\":";
                JsonString(std::cout, water.waterTableSource.canonicalPath);
                std::cout << ",\"water_table_row\":" << water.waterTableRow;
            }
            std::cout << '}';
        }
        std::cout << ",\"asset_reads\":[";
        for (std::size_t index = 0; index < material->assets.size(); ++index) {
            const auto& asset = material->assets[index];
            if (index) std::cout << ',';
            std::cout << "{\"source_archive\":";
            JsonString(std::cout, asset.layer == "map" ? Utf8(options.map) : asset.layer);
            std::cout << ",\"virtual_path\":";
            JsonString(std::cout, asset.path);
            std::cout << ",\"sha256\":";
            JsonString(std::cout, asset.sha256);
            std::cout << '}';
        }
        std::cout << "],\"material_diagnostics\":[";
        for (std::size_t index = 0; index < material->diagnostics.size(); ++index) {
            const auto& diagnostic = material->diagnostics[index];
            if (index) std::cout << ',';
            std::cout << "{\"severity\":";
            JsonString(std::cout, diagnostic.severity == War3Map::DiagnosticSeverity::Warning ? "warning" : "info");
            std::cout << ",\"code\":";
            JsonString(std::cout, diagnostic.code);
            std::cout << ",\"message\":";
            JsonString(std::cout, diagnostic.message);
            std::cout << '}';
        }
        std::cout << "],\"limitations\":[";
        if (renderedCliffs) std::cout << "\"cliff variants and texture filters are not original-validated\",";
        else std::cout << "\"no cliff or ramp model geometry\",";
        if (renderedWater) std::cout << "\"water shore geometry, UV and animation are not original-validated\",";
        else std::cout << "\"no water surface\",";
        std::cout << "\"no blight atlas\",\"default terrain lighting\",\"not original-equivalent rendering\"";
        if (mipMode == "mip0-only") std::cout << ",\"renderer samples only mip0\"";
        std::cout << ']';
    }
    std::cout << "}\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try { return Run(ParseOptions(argc, argv)); }
    catch (const std::exception& error) { std::cerr << "terrain_preview_error=" << error.what() << '\n'; return 1; }
}
