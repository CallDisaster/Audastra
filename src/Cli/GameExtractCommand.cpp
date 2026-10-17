#include "GameExtractCommand.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr std::size_t kMaxExtractBytes = 64u * 1024u * 1024u;

void PrintDiagnostics(const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
    }
}
} // namespace

int RunGameExtract(int argc, wchar_t* argv[]) {
    if (argc != 5 && (argc != 7 || std::wstring_view(argv[5]) != L"--tileset")) {
        std::cerr << "Game: ARGUMENTS_INVALID: game-extract <war3-root> <internal\\path> <out-file> [--tileset <A-Z>]\n";
        return 2;
    }
    std::optional<std::uint8_t> tileset;
    if (argc == 7) {
        const std::wstring_view value = argv[6];
        if (value.size() != 1 || value.front() > 0xFF) {
            std::cerr << "Game: TILESET_ARGUMENT_INVALID: expected one ASCII tileset byte\n";
            return 2;
        }
        // A-Z 的有效性统一由已有 MountStrict 规则判断，不截断 Unicode 或另建地形集表。
        tileset = static_cast<std::uint8_t>(value.front());
    }
    const std::filesystem::path root = argv[2];
    const auto utf8 = std::filesystem::path(argv[3]).u8string();
    const std::string name(utf8.begin(), utf8.end());
    if (!tileset) {
        // 保留原无 flag 的 best-effort 挂载、无界读取和文本输出合同。
        auto mounted = War3Map::Game::GameDataSource::Mount(root);
        if (!mounted.Succeeded() || !mounted.value.has_value()) {
            PrintDiagnostics(mounted.diagnostics);
            return 1;
        }
        auto data = mounted.value->ReadFile(name);
        if (!data.Succeeded() || !data.value.has_value()) {
            PrintDiagnostics(data.diagnostics);
            return 1;
        }
        std::ofstream out(argv[4], std::ios::binary);
        if (!out) {
            std::cerr << "Game: OUTPUT_OPEN_FAILED\n";
            return 1;
        }
        out.write(reinterpret_cast<const char*>(data.value->data()),
                  static_cast<std::streamsize>(data.value->size()));
        std::cout << "extracted=" << name << " bytes=" << data.value->size() << '\n';
        if (data.value->size() >= 4) {
            const auto* magic = reinterpret_cast<const char*>(data.value->data());
            std::cout << "magic=" << std::string(magic, 4) << '\n';
        }
        return 0;
    }

    auto mounted = War3Map::Game::GameDataSource::MountStrict(root, *tileset);
    PrintDiagnostics(mounted.diagnostics); // 确认缺少嵌套包的 Info 也须保留，不能伪称命中 Z。
    if (!mounted.Succeeded() || !mounted.value) return 1;
    auto data = mounted.value->ReadFileBoundedWithSource(name, kMaxExtractBytes);
    if (!data.Succeeded() || !data.value) {
        PrintDiagnostics(data.diagnostics);
        return 1;
    }
    const auto sha = War3Map::Sha256(data.value->bytes);
    if (!sha.Succeeded() || !sha.value) {
        PrintDiagnostics(sha.diagnostics);
        return 1;
    }
    const auto rootUtf8 = root.u8string();
    nlohmann::json response{{"ok", true}, {"scope", "game-only"},
        {"game_root", std::string(rootUtf8.begin(), rootUtf8.end())}, {"path", name},
        {"tileset", std::string(1, static_cast<char>(*tileset))}, {"bytes", data.value->bytes.size()},
        {"source_archive", data.value->archiveName}, {"member_sha256", *sha.value}, {"diagnostics", nlohmann::json::array()}};
    const auto appendDiagnostics = [&](const std::vector<War3Map::Diagnostic>& diagnostics) {
        for (const auto& diagnostic : diagnostics) {
            response["diagnostics"].push_back({{"severity", diagnostic.severity == War3Map::DiagnosticSeverity::Error ? "error"
                : diagnostic.severity == War3Map::DiagnosticSeverity::Warning ? "warning" : "info"},
                {"subsystem", diagnostic.subsystem}, {"code", diagnostic.code}, {"message", diagnostic.message}});
        }
    };
    appendDiagnostics(mounted.diagnostics);
    appendDiagnostics(data.diagnostics);
    appendDiagnostics(sha.diagnostics);
    std::string serialized;
    try {
        // 复用 CLI 既有 JSON 库处理全部路径、控制字符与诊断文本，先生成完整成功回执。
        serialized = response.dump();
    } catch (const nlohmann::json::exception&) {
        std::cerr << "Game: OUTPUT_JSON_FAILED\n";
        return 1;
    }
    // 全量读取及 SHA 成功前不创建/截断输出，坏高层和超限均沿原诊断失败。
    std::ofstream out(argv[4], std::ios::binary);
    if (!out) {
        std::cerr << "Game: OUTPUT_OPEN_FAILED\n";
        return 1;
    }
    out.write(reinterpret_cast<const char*>(data.value->bytes.data()),
        static_cast<std::streamsize>(data.value->bytes.size()));
    out.flush();
    out.close();
    if (!out) {
        std::cerr << "Game: OUTPUT_WRITE_FAILED\n";
        return 1;
    }
    std::cout << serialized << '\n';
    return 0;
}
