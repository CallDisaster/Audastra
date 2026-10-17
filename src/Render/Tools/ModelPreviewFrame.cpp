#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/WorldModelScene.h"
#include "ModelPreviewReplacement.h"

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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using War3Map::Render::BuildWorldModelScene;
using War3Map::Render::CheckStaticModelSamplerSupport;
using War3Map::Render::DescribeModelScene;
using War3Map::Render::ModelSceneRigidPoseRequest;
using War3Map::Render::ModelPreview::RelativeAssetPath;
namespace fs = std::filesystem;

constexpr std::size_t kMaxAssetBytes = 16 * 1024 * 1024;
constexpr std::uint32_t kDefaultWidth = 640, kDefaultHeight = 480;

struct Options {
    fs::path model, output, assets, gameRoot, plugin;
    bool hasAssets = false, hasGameRoot = false, hasPlugin = false;
    std::optional<std::string> replaceableTexture;
    std::size_t sequence = 0;
    std::uint64_t elapsedMs = 0;
    float targetX = 0, targetY = 0, targetZ = 80;
    float yaw = -1.1f, pitch = 0.35f, distance = 450;
    std::uint32_t width = kDefaultWidth, height = kDefaultHeight;
};

[[noreturn]] void Fail(std::string_view message) { throw std::runtime_error(std::string(message)); }
void Require(bool condition, std::string_view message) { if (!condition) Fail(message); }

std::string Utf8(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

std::string AsciiNumber(std::wstring_view source, std::string_view name) {
    Require(!source.empty(), std::string(name) + " requires a value");
    std::string value;
    value.reserve(source.size());
    for (const auto character : source) {
        Require(static_cast<unsigned int>(character) < 128, std::string(name) + " must use ASCII digits");
        value.push_back(static_cast<char>(character));
    }
    return value;
}

template <typename T>
T ParseUnsigned(std::wstring_view source, std::string_view name) {
    const auto value = AsciiNumber(source, name);
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    Require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(),
        std::string(name) + " must be a non-negative decimal integer");
    return result;
}

float ParseFloat(std::wstring_view source, std::string_view name) {
    const auto value = AsciiNumber(source, name);
    float result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    Require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::isfinite(result),
        std::string(name) + " must be a finite decimal number");
    return result;
}

Options ParseOptions(int argc, wchar_t** argv) {
    Options options;
    bool hasModel = false, hasOutput = false, hasSequence = false, hasElapsed = false;
    for (int i = 1; i < argc; i += 2) {
        const std::wstring_view flag(argv[i]);
        Require(i + 1 < argc, "every option requires a value");
        const std::wstring_view value(argv[i + 1]);
        if (flag == L"--model") { Require(!hasModel, "duplicate --model"); options.model = value; hasModel = true; }
        else if (flag == L"--output") { Require(!hasOutput, "duplicate --output"); options.output = value; hasOutput = true; }
        else if (flag == L"--sequence") {
            Require(!hasSequence, "duplicate --sequence"); options.sequence = ParseUnsigned<std::size_t>(value, "--sequence"); hasSequence = true;
        } else if (flag == L"--elapsed-ms") {
            Require(!hasElapsed, "duplicate --elapsed-ms"); options.elapsedMs = ParseUnsigned<std::uint64_t>(value, "--elapsed-ms"); hasElapsed = true;
        } else if (flag == L"--assets") { options.assets = value; options.hasAssets = true; }
        else if (flag == L"--replaceable-texture")
            War3Map::Render::ModelPreview::SetReplacementTexture(options.replaceableTexture, Utf8(fs::path(value)));
        else if (flag == L"--game-root") { options.gameRoot = value; options.hasGameRoot = true; }
        else if (flag == L"--plugin") { options.plugin = value; options.hasPlugin = true; }
        else if (flag == L"--target-x") options.targetX = ParseFloat(value, "--target-x");
        else if (flag == L"--target-y") options.targetY = ParseFloat(value, "--target-y");
        else if (flag == L"--target-z") options.targetZ = ParseFloat(value, "--target-z");
        else if (flag == L"--yaw") options.yaw = ParseFloat(value, "--yaw");
        else if (flag == L"--pitch") options.pitch = ParseFloat(value, "--pitch");
        else if (flag == L"--distance") options.distance = ParseFloat(value, "--distance");
        else if (flag == L"--width") options.width = ParseUnsigned<std::uint32_t>(value, "--width");
        else if (flag == L"--height") options.height = ParseUnsigned<std::uint32_t>(value, "--height");
        else Fail("unknown option");
    }
    Require(hasModel && hasOutput && hasSequence && hasElapsed,
        "usage: war3-model-preview --model <file.mdx> --output <new.bmp> --sequence <index> --elapsed-ms <ms> "
        "[--game-root <Warcraft III>] [--assets <directory>] [--plugin <war3-render-d3d11.dll>] "
        "[--replaceable-texture <relative resource path>] "
        "[--target-x/y/z <number>] [--yaw <radians>] [--pitch <radians>] [--distance <units>] "
        "[--width <pixels>] [--height <pixels>]");
    Require(options.width >= 64 && options.width <= 2048 && options.height >= 64 && options.height <= 2048,
        "capture dimensions must be 64..2048 pixels");
    Require(options.distance >= 16 && options.distance <= 400000 && options.pitch >= 0.05f && options.pitch <= 1.5f,
        "camera distance/pitch outside renderer bounds");
    return options;
}

War3Map::Result<std::vector<std::byte>> ReadLocalBounded(const fs::path& path, std::size_t maxBytes) {
    std::error_code error;
    const auto size = fs::file_size(path, error);
    if (error || size > maxBytes)
        return War3Map::Result<std::vector<std::byte>>::Failure("ModelPreview", "MODEL_PREVIEW_FILE",
            "File missing, unreadable or exceeds byte limit: " + Utf8(path));
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream || !stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        return War3Map::Result<std::vector<std::byte>>::Failure("ModelPreview", "MODEL_PREVIEW_READ",
            "Could not read file completely: " + Utf8(path));
    return War3Map::Result<std::vector<std::byte>>::Success(std::move(bytes));
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

bool EqualCamera(const War3RenderCameraV2& left, const War3RenderCameraV2& right) {
    return left.structSize == right.structSize && left.targetX == right.targetX && left.targetY == right.targetY &&
        left.targetZ == right.targetZ && left.distance == right.distance &&
        left.yawRadians == right.yawRadians && left.pitchRadians == right.pitchRadians;
}

void WriteAll(HANDLE file, const void* data, std::size_t bytes) {
    const auto* source = static_cast<const std::byte*>(data);
    while (bytes) {
        DWORD written = 0;
        const auto chunk = static_cast<DWORD>((std::min)(bytes, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
        Require(WriteFile(file, source, chunk, &written, nullptr) && written == chunk, "BMP write failed");
        source += chunk;
        bytes -= chunk;
    }
}

void SaveBmpNew(const fs::path& output, std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height) {
    std::error_code error;
    Require(!fs::exists(output, error) && !error, "output already exists or cannot be checked");
    const auto pixelBytes = static_cast<std::size_t>(width) * height * 4;
    Require(rgba.size() == pixelBytes && pixelBytes <= std::numeric_limits<DWORD>::max() - 54,
        "BMP size invalid");
    fs::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
        temporary = output;
        temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        Require(GetLastError() == ERROR_FILE_EXISTS, "cannot create temporary BMP beside output");
    }
    Require(file != INVALID_HANDLE_VALUE, "temporary BMP name collision");
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
    header.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER));
    header.bfSize = static_cast<DWORD>(header.bfOffBits + pixelBytes);
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = static_cast<LONG>(width);
    info.biHeight = -static_cast<LONG>(height); // top-down, same row order as captureFrameRgba
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    WriteAll(file, &header, sizeof(header));
    WriteAll(file, &info, sizeof(info));
    std::vector<std::uint8_t> bgra(rgba.begin(), rgba.end());
    for (std::size_t at = 0; at < bgra.size(); at += 4) std::swap(bgra[at], bgra[at + 2]);
    WriteAll(file, bgra.data(), bgra.size());
    Require(FlushFileBuffers(file), "BMP flush failed");
    Require(CloseHandle(file), "BMP close failed");
    guard.file = INVALID_HANDLE_VALUE;
    Require(MoveFileExW(temporary.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH),
        "BMP publish failed (output may already exist)");
    guard.published = true;
}

int Run(const Options& options) {
    std::error_code error;
    Require(fs::is_regular_file(options.model, error) && !error, "model must be a regular local file");
    auto modelBytes = ReadLocalBounded(options.model, kMaxAssetBytes);
    Require(modelBytes.Succeeded(), "model read failed or exceeds 16 MiB");
    const auto sourceSha = War3Map::Sha256(*modelBytes.value);
    Require(sourceSha.Succeeded(), "could not hash model bytes");
    auto onDiskSha = War3Map::Sha256File(options.model);
    Require(onDiskSha.Succeeded() && *onDiskSha.value == *sourceSha.value, "model changed during read");

    std::optional<War3Map::Game::GameDataSource> game;
    if (options.hasGameRoot) {
        const auto validation = War3Map::Game::GameDataSource::Validate(options.gameRoot);
        Require(validation.Usable(), "supplied game root has no usable core MPQ set");
        auto mounted = War3Map::Game::GameDataSource::MountStrict(options.gameRoot);
        Require(mounted.Succeeded(), "could not strictly mount supplied game root");
        game.emplace(std::move(*mounted.value));
    }
    if (options.hasAssets) {
        error.clear();
        Require(fs::is_directory(options.assets, error) && !error, "--assets must be an existing directory");
    }

    const auto modelKey = Utf8(options.model);
    const auto reader = [&](std::string_view name) -> War3Map::Result<std::vector<std::byte>> {
        if (name == modelKey) return War3Map::Result<std::vector<std::byte>>::Success(*modelBytes.value);
        const auto relative = RelativeAssetPath(name);
        if (!relative)
            return War3Map::Result<std::vector<std::byte>>::Failure("ModelPreview", "MODEL_PREVIEW_ASSET_PATH",
                "Resource path is not relative to the asset root");
        if (options.hasAssets) {
            const auto candidate = options.assets / *relative;
            if (auto local = ReadLocalBounded(candidate, kMaxAssetBytes); local.Succeeded()) return local;
        }
        if (game) return game->ReadFileBounded(std::string(name), kMaxAssetBytes);
        return War3Map::Result<std::vector<std::byte>>::Failure("ModelPreview", "MODEL_PREVIEW_ASSET_MISSING",
            "Resource is not available in the selected read-only sources");
    };

    const float horizontal = options.distance * std::cos(options.pitch);
    ModelSceneRigidPoseRequest pose;
    pose.sequenceIndex = options.sequence;
    pose.sequenceElapsedMs = options.elapsedMs;
    pose.camera = War3Map::Render::MdxPoseCamera{{
        options.targetX + horizontal * std::cos(options.yaw),
        options.targetY + horizontal * std::sin(options.yaw),
        options.targetZ + options.distance * std::sin(options.pitch)}, {0, 0, 1}};
    const auto request = War3Map::Render::ModelPreview::MakeRequest(modelKey, options.replaceableTexture);
    auto scene = BuildWorldModelScene(std::span(&request, 1), reader, {}, {}, pose);
    if (!scene.Succeeded()) {
        for (const auto& diagnostic : scene.diagnostics)
            std::cerr << "scene_error=" << diagnostic.code << " " << diagnostic.message << '\n';
        Fail("model pose scene build failed");
    }
    Require(scene.value->requestedInstances == 1 && scene.value->skippedInstances == 0 &&
        scene.value->instances.size() == 1 && !scene.value->vertices.empty() && !scene.value->indices.empty(),
        "model was skipped or has no renderable triangles");
    for (const auto& warning : scene.value->warnings)
        std::cerr << "scene_warning=" << warning.code << " " << warning.message << '\n';
    auto descriptor = DescribeModelScene(*scene.value);

    wchar_t executable[MAX_PATH]{};
    const auto count = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    Require(count != 0 && count < MAX_PATH, "executable path unavailable");
    const auto plugin = options.hasPlugin ? options.plugin : fs::path(executable).parent_path() / L"war3-render-d3d11.dll";
    const Module module(plugin);
    const auto getApi = reinterpret_cast<War3GetRenderPluginApiV2>(
        GetProcAddress(module.handle, "War3MapEditor_GetRenderPluginApiV2"));
    Require(getApi != nullptr, "D3D11 render plugin has no V2 API export");
    const auto* api = getApi(WAR3_RENDER_PLUGIN_API_V2);
    Require(api != nullptr && api->apiVersion == WAR3_RENDER_PLUGIN_API_V2 &&
        HasApi(api, offsetof(War3RenderPluginApiV2, captureFrameRgba), &War3RenderPluginApiV2::captureFrameRgba) &&
        HasApi(api, offsetof(War3RenderPluginApiV2, submitModelScene), &War3RenderPluginApiV2::submitModelScene) &&
        api->createRenderer && api->destroyRenderer && api->submitMesh && api->setCamera && api->getStatus,
        "D3D11 plugin lacks required atomic model capture API");
    const War3RenderCreateInfoV2 create{sizeof(create), options.width, options.height, 1}; // WARP, no HWND
    const Renderer renderer(api, create);
    Require(CheckStaticModelSamplerSupport(api, renderer.handle, *scene.value).Succeeded(), "WORLD_STATIC_SAMPLER_UNSUPPORTED");
    std::uint64_t revision = 0;
    const auto parsed = std::from_chars(sourceSha.value->data(), sourceSha.value->data() + 16, revision, 16);
    Require(parsed.ec == std::errc{} && parsed.ptr == sourceSha.value->data() + 16, "invalid source SHA-256");
    if (!revision) revision = 1;
    const War3RenderMeshV2 emptyMesh{sizeof(emptyMesh), 0, 0, revision, nullptr, nullptr};
    const War3RenderCameraV2 camera{sizeof(camera), options.targetX, options.targetY, options.targetZ,
        options.distance, options.yaw, options.pitch};
    Require(api->submitMesh(renderer.handle, &emptyMesh) == 0 &&
        api->submitModelScene(renderer.handle, descriptor.StaticScene()) == 0 &&
        api->setCamera(renderer.handle, &camera) == 0,
        "D3D11 plugin rejected model scene or camera");
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(options.width) * options.height * 4);
    War3RenderCapturedFrameV2 receipt{sizeof(receipt)};
    const auto captureResult = api->captureFrameRgba(renderer.handle, rgba.data(), rgba.size(), &receipt);
    if (captureResult != 0) {
        War3RenderStatusV2 status{sizeof(status)};
        if (api->getStatus(renderer.handle, &status) == 0)
            std::cerr << "renderer_status=" << status.message << " result=" << status.result << '\n';
        Fail("finished-frame model capture failed or was superseded");
    }
    const auto& capture = receipt.capture;
    const auto& view = receipt.view;
    Require(receipt.structSize == sizeof(receipt) && capture.structSize == sizeof(capture) &&
        view.structSize == sizeof(view) && receipt.camera.structSize == sizeof(receipt.camera) &&
        capture.ticket != 0 && capture.frameCount != 0 && capture.width == options.width &&
        capture.height == options.height && capture.documentRevision == revision &&
        view.width == capture.width && view.height == capture.height &&
        view.frameCount == capture.frameCount && view.documentRevision == capture.documentRevision &&
        view.surfaceEpoch && view.sceneEpoch && view.viewEpoch && EqualCamera(receipt.camera, camera) &&
        std::all_of(std::begin(view.viewProjection), std::end(view.viewProjection),
            [](float value) { return std::isfinite(value); }),
        "atomic frame receipt does not identify the requested model/camera/revision");
    War3RenderStatusV2 status{sizeof(status)};
    Require(api->getStatus(renderer.handle, &status) == 0 && status.result == 0 &&
        status.deviceReady == 1 && status.software == 1 &&
        status.frameCount == capture.frameCount && status.documentRevision == capture.documentRevision &&
        status.width == capture.width && status.height == capture.height &&
        // 插件的模型上传失败保留旧模型且 status.result 仍为 0；只有成功状态行能证明本帧模型已上传。
        std::string_view(status.message, sizeof(status.message)).starts_with("D3D11 WARP / Feature Level 11.0 / "),
        "WARP model upload failed or frame status differs from the captured frame");
    onDiskSha = War3Map::Sha256File(options.model);
    Require(onDiskSha.Succeeded() && *onDiskSha.value == *sourceSha.value,
        "model source changed before screenshot publish");
    SaveBmpNew(options.output, rgba, options.width, options.height);
    const auto imageSha = War3Map::Sha256File(options.output);
    Require(imageSha.Succeeded(), "could not hash completed BMP");
    std::cout << "preview_label=通用算法预览\n"
        << "model=" << Utf8(options.model) << "\nsource_sha256=" << *sourceSha.value
        << "\noutput=" << Utf8(options.output) << "\nimage_sha256=" << *imageSha.value
        << "\nreplaceable_texture=" << options.replaceableTexture.value_or("")
        << "\nsequence=" << options.sequence << " elapsed_ms=" << options.elapsedMs
        << "\ncamera_target=" << camera.targetX << ',' << camera.targetY << ',' << camera.targetZ
        << " yaw=" << camera.yawRadians << " pitch=" << camera.pitchRadians << " distance=" << camera.distance
        << "\nframe=" << capture.frameCount << " ticket=" << capture.ticket
        << " revision=" << capture.documentRevision << " surface_epoch=" << view.surfaceEpoch
        << " scene_epoch=" << view.sceneEpoch << " view_epoch=" << view.viewEpoch
        << "\nsoftware=WARP model_upload=ok width=" << capture.width << " height=" << capture.height
        << " models=" << scene.value->models.size() << " meshes=" << scene.value->meshes.size()
        << " untextured_layers=" << scene.value->untexturedLayers << '\n';
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try { return Run(ParseOptions(argc, argv)); }
    catch (const std::exception& error) { std::cerr << "model_preview_error=" << error.what() << '\n'; return 1; }
}
