#include "ScanPreview.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/Render/AnimationSourceGuard.h"
#include "War3MapCore/Resources/ImportTable.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iostream>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace War3Map::Render::Scan {
namespace {
template<class T> T Good(Result<T> result, std::string_view context = {}) {
    if (!result.Succeeded()) throw std::runtime_error(std::string(context) +
        (result.diagnostics.empty() ? "SCAN_READ_FAILED" : result.diagnostics.front().code));
    return std::move(*result.value);
}
void Good(Status status) {
    if (!status.Succeeded()) throw std::runtime_error(status.diagnostics.empty() ? "SCAN_SOURCE_FAILED" : status.diagnostics.front().code);
}
bool Mdx(std::string_view name) {
    if (name.size() < 4) return false;
    auto tail = std::string(name.substr(name.size() - 4));
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return tail == ".mdx";
}
void Safe(std::string_view path) {
    if (path.empty() || path.size() > 8192 || path.find_first_of("\0\t\r\n", 0, 4) != std::string_view::npos)
        throw std::runtime_error("SCAN_MODEL_PATH_INVALID");
}
std::string Key(std::string_view path) { return Good(Resources::CanonicalImportPath(path)); }
struct Sources {
    const Options& options;
    std::filesystem::path mapPath;
    AnimationSourceGuard guard;
    std::optional<Mpq::MpqArchive> map;
    std::optional<Game::GameDataSource> game;
    std::optional<std::uint8_t> mainTileset;
    std::vector<Diagnostic> gameDiagnostics;
    std::map<std::string, AnimationResource, std::less<>> cache;
    std::uint64_t cacheBytes = 0;
    explicit Sources(const Options& input, std::filesystem::path archive,
        std::optional<std::uint8_t> tileset) : options(input), mapPath(std::move(archive)), mainTileset(tileset) {
        if (!mapPath.empty()) {
            Good(guard.Watch(mapPath)); map.emplace(Good(Mpq::MpqArchive::OpenReadOnly(mapPath)));
            if (map->HasFile("war3map.w3e")) {
                // 与世界资源读取器使用同一已解析地形集，不从文件名猜嵌套包。
                const auto bytes = Good(map->ReadFileBounded("war3map.w3e", 64ull * 1024 * 1024), "SCAN_SOURCE_TERRAIN_");
                const auto terrain = Good(MapData::ParseMapTerrain(bytes), "SCAN_SOURCE_TERRAIN_");
                const auto declared = terrain.Snapshot()->header.mainTileset;
                if (mainTileset && *mainTileset != declared)
                    throw std::runtime_error("SCAN_SOURCE_TILESET_MAP_CONFLICT");
                mainTileset = declared;
            }
        }
        if (!options.gameRoot.empty()) {
            Good(guard.WatchGameRoot(options.gameRoot));
            auto mounted = mainTileset ? Game::GameDataSource::MountStrict(options.gameRoot, *mainTileset) :
                Game::GameDataSource::MountStrict(options.gameRoot);
            gameDiagnostics = mounted.diagnostics;
            game.emplace(Good(std::move(mounted)));
        }
    }
    AnimationResource Read(std::string_view path, std::uint64_t limit) {
        Safe(path); Good(guard.Verify());
        const bool diskModel = !options.list.empty() && mapPath.empty() && options.gameRoot.empty();
        const auto key = diskModel && path == options.model ? std::string(path) : Key(path);
        if (const auto found = cache.find(key); found != cache.end()) {
            if (found->second.bytes.size() > limit) throw std::runtime_error("SCAN_RESOURCE_LIMIT");
            return found->second;
        }
        AnimationResource resource;
        if (diskModel) {
            // 模型清单路径可以是绝对路径；模型内部资源只允许位于同一目录下的安全相对名。
            const auto model = FromUtf8(options.model);
            const auto disk = path == options.model ? model : model.parent_path() / FromUtf8(Key(path));
            // 记录缺失文件的身份，让 PREM 的同源 .mdl → .mdx 回退也受来源租约约束。
            Good(guard.Watch(disk, true));
            std::ifstream stream(disk, std::ios::binary | std::ios::ate);
            if (!stream) {
                std::error_code error;
                const bool exists = std::filesystem::exists(disk, error);
                if (!error && !exists) throw std::runtime_error("RESOURCE_NOT_FOUND");
                // 权限或 I/O 失败不是缺失，不能据此允许换扩展名读取。
                throw std::runtime_error("SCAN_READ_RESOURCE_FAILED");
            }
            const auto size = stream.tellg();
            if (size < 0 || static_cast<std::uint64_t>(size) > limit) throw std::runtime_error("SCAN_RESOURCE_LIMIT");
            resource.bytes.resize(static_cast<std::size_t>(size)); stream.seekg(0);
            if (!resource.bytes.empty()) stream.read(reinterpret_cast<char*>(resource.bytes.data()), static_cast<std::streamsize>(resource.bytes.size()));
            if (!stream || stream.peek() != std::ifstream::traits_type::eof()) throw std::runtime_error("SCAN_READ_CHANGED");
            resource.sourceLayer = "disk-list";
        } else if (map && map->HasFile(std::string(path))) {
            resource.bytes = Good(map->ReadFileBounded(path, static_cast<std::size_t>(limit)));
            resource.sourceLayer = "map:" + Utf8(mapPath.filename());
        } else if (game) {
            auto read = Good(game->ReadFileBoundedWithSource(std::string(path), static_cast<std::size_t>(limit)));
            resource.bytes = std::move(read.bytes); resource.sourceLayer = "game:" + read.archiveName;
        } else throw std::runtime_error("RESOURCE_NOT_FOUND");
        Good(guard.Verify()); resource.digest = Good(Sha256(resource.bytes));
        if (cache.size() >= 4096 || resource.bytes.size() > 256ull * 1024 * 1024 - cacheBytes)
            throw std::runtime_error("SCAN_RESOURCE_CACHE_LIMIT");
        cacheBytes += resource.bytes.size(); cache.emplace(key, resource); return resource;
    }
};
std::filesystem::path Companion(const std::filesystem::path& path, const char* suffix) {
    auto result = path; result += suffix; return result;
}
std::string DiagnosticHeader() {
    return "model\tsha256\tsource\tpolicy\tcategory\tcode\tseverity\toccurrences\tfirst_sequence\tfirst_elapsed_ms\tmessage";
}
std::string ModelHeader() { return "model\tsha256\tsource\tpolicy\tstate\tplanned\tsampled\tvisible\tanimated\tmoving_sequences"; }
void Write(const std::filesystem::path& path, const PreviewModelReport& model) {
    std::ofstream rows(path, std::ios::binary | std::ios::trunc);
    std::ofstream issues(Companion(path, ".diagnostics.tsv"), std::ios::binary | std::ios::trunc);
    for (const auto& policy : model.policies) {
        rows << Cell(model.path) << '\t' << Cell(model.sha256) << '\t' << Cell(model.source) << '\t'
            << policy.policy << '\t' << policy.state << '\t' << policy.planned << '\t' << policy.sampled << '\t'
            << policy.visible << '\t' << policy.animated << '\t' << policy.moving << '\n';
        for (const auto& [code, issue] : policy.issues)
            issues << Cell(model.path) << '\t' << Cell(model.sha256) << '\t' << Cell(model.source) << '\t'
                << policy.policy << '\t' << issue.category << '\t' << Cell(code) << '\t' << issue.severity << '\t'
                << issue.occurrences << '\t' << issue.firstSequence << '\t' << issue.firstElapsedMs << '\t'
                << Cell(issue.message.substr(0, 4096)) << '\n';
    }
    rows.flush(); issues.flush();
    if (!rows || !issues) throw std::runtime_error("SCAN_REPORT_WRITE_FAILED");
}
PreviewModelReport Failure(const Options& options, std::string code) {
    PreviewModelReport model; model.path = options.model;
    for (const auto* policy : {"strict", "interactive"}) {
        PreviewPolicyReport result; result.policy = policy; result.state = "uncovered";
        result.issues.emplace(code, PreviewIssue{std::string(PreviewDiagnosticCategory(code)), "error", code, 1});
        model.policies.push_back(std::move(result));
    }
    return model;
}
int One(const Options& options, const std::filesystem::path& mapPath, std::optional<std::uint8_t> tileset) {
    const auto start = Clock::now();
    try {
        Sources source(options, mapPath, tileset);
        auto model = source.Read(options.model, static_cast<std::uint64_t>(options.limits.fileMiB) * 1024 * 1024);
        const auto checkpoint = [&] {
            Good(source.guard.Verify());
            if (Clock::now() - start >= std::chrono::milliseconds(options.limits.modelMs)) throw Interrupted{"SCAN_MODEL_TIMEOUT"};
        };
        const auto reader = [&](std::string_view path, std::uint64_t maximum) {
            if (path == "scan-model.mdx") {
                if (model.bytes.size() > maximum) return Result<AnimationResource>::Failure("PreviewScan", "SCAN_RESOURCE_LIMIT", "模型超过资源预算。");
                return Result<AnimationResource>::Success(model);
            }
            try { return Result<AnimationResource>::Success(source.Read(path, maximum)); }
            catch (const std::exception& error) { return Result<AnimationResource>::Failure("PreviewScan", error.what(), "只读资源取得失败。"); }
        };
        // 服务内始终用合法的虚拟成员名，绝对磁盘路径只进入报告，不伪装成 MPQ 资源路径。
        auto report = EvaluatePreviewModel("scan-model.mdx", model, reader, options.limits, checkpoint);
        report.path = options.model;
        checkpoint(); Write(options.tsv, report);
    } catch (const Interrupted& error) { Write(options.tsv, Failure(options, error.code)); }
    catch (const std::bad_alloc&) { Write(options.tsv, Failure(options, "SCAN_MEMORY_LIMIT")); }
    catch (const std::exception& error) { Write(options.tsv, Failure(options, error.what())); }
    return 0;
}
int EnumeratePreview(const Options& options, const std::filesystem::path& mapPath,
    std::optional<std::uint8_t> tileset) {
    Sources source(options, mapPath, tileset);
    std::vector<std::string> names, scope;
    if (!options.list.empty()) {
        names = ReadLines(options.list, 16ull * 1024 * 1024, 100000);
        if (!names.empty() && names.front().starts_with("\xef\xbb\xbf")) names.front().erase(0, 3);
        names.erase(std::remove_if(names.begin(), names.end(), [](const auto& name) { return name.empty() || name.starts_with('#'); }), names.end());
        if (!source.map && !source.game) for (auto& name : names) {
            auto disk = FromUtf8(name); if (disk.is_relative()) disk = options.list.parent_path() / disk;
            name = Utf8(std::filesystem::absolute(disk).lexically_normal());
        }
        scope.push_back("scope=explicit-model-list");
    } else if (source.map) {
        names = source.map->ListFiles();
        scope.push_back("scope=map-readable-listfile-names-only");
        scope.push_back("archive=" + Cell(Utf8(mapPath)));
        scope.push_back("archive_sha256=" + Good(Sha256File(mapPath)));
        scope.push_back("archive_blocks=" + std::to_string(source.map->FileCount()));
        scope.push_back("named_members=" + std::to_string(names.size()));
        const auto complete = source.map->ListFilesStrict();
        scope.push_back(std::string("complete_member_enumeration=") + (complete.Succeeded() ? "1" : "0"));
        for (const auto& diagnostic : complete.diagnostics) scope.push_back("scope_notice=" + diagnostic.code);
    } else if (source.game) {
        auto files = source.game->ListFilesWithSource();
        for (const auto& diagnostic : files.diagnostics) scope.push_back("scope_notice=" + diagnostic.code);
        for (const auto& file : Good(std::move(files))) names.push_back(file.path);
        scope.push_back("scope=game-readable-listfile-names-only");
        scope.push_back("complete_member_enumeration=0");
        const auto archives = Game::GameDataSource::Validate(options.gameRoot);
        for (const auto& archive : archives.archives) {
            scope.push_back("archive=" + archive.fileName + " present=" + std::to_string(archive.present) +
                " opened=" + std::to_string(archive.opened) + " members=" + std::to_string(archive.fileCount));
            if (archive.present && archive.opened)
                scope.push_back("archive_sha256=" + archive.fileName + " " + Good(Sha256File(options.gameRoot / FromUtf8(archive.fileName))));
        }
    } else throw std::runtime_error("SCAN_REQUIRED_ARGUMENT_MISSING");
    std::set<std::string> seen;
    std::vector<std::string> selected;
    for (auto& name : names) {
        Safe(name); if (!Mdx(name)) continue;
        const auto key = source.map || source.game ? Key(name) : name;
        if (seen.insert(key).second) selected.push_back(std::move(name));
    }
    scope.push_back("discovered_models=" + std::to_string(selected.size()));
    if (selected.size() > options.limits.maxModels) {
        selected.resize(options.limits.maxModels); scope.push_back("scope_notice=SCAN_MODEL_COUNT_LIMIT");
    }
    scope.push_back("selected_models=" + std::to_string(selected.size()));
    if (source.mainTileset) {
        const auto letter = std::string(1, static_cast<char>(*source.mainTileset));
        scope.push_back("main_tileset=" + std::to_string(*source.mainTileset));
        scope.push_back("main_tileset_letter=" + letter);
        scope.push_back(std::string("tileset_selection=") + (tileset ? "explicit" : "map-terrain"));
        if (source.game) {
            scope.push_back("game_context=tileset:" + letter);
            scope.push_back("tileset_archive_member=" + letter + ".mpq");
            const bool absent = std::any_of(source.gameDiagnostics.begin(), source.gameDiagnostics.end(),
                [](const auto& diagnostic) { return diagnostic.code == "tileset-archive-absent"; });
            scope.push_back(std::string("tileset_archive_state=") + (absent ? "absent" : "mounted"));
        }
    } else if (source.game) scope.push_back("game_context=default");
    for (const auto& diagnostic : source.gameDiagnostics) scope.push_back("scope_notice=" + diagnostic.code);
    Good(source.guard.Verify());
    std::ofstream list(options.tsv, std::ios::binary | std::ios::trunc);
    std::ofstream info(Companion(options.tsv, ".scope.txt"), std::ios::binary | std::ios::trunc);
    for (const auto& name : selected) list << name << '\n';
    for (const auto& item : scope) info << item << '\n';
    list.flush(); info.flush(); if (!list || !info) throw std::runtime_error("SCAN_ENUMERATE_WRITE_FAILED");
    return 0;
}
struct Scratch {
    std::filesystem::path path;
    Scratch() {
        std::random_device random;
        for (int i = 0; i < 10; ++i) {
            auto candidate = std::filesystem::temp_directory_path() / ("audastra-model-scan-" + std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) { path = std::move(candidate); return; }
        }
        throw std::runtime_error("SCAN_TEMP_CREATE_FAILED");
    }
    ~Scratch() { std::error_code error; if (!path.empty()) std::filesystem::remove_all(path, error); }
};
std::vector<std::string> Cells(const std::string& line) {
    std::vector<std::string> cells; std::size_t at = 0;
    while (at <= line.size()) {
        const auto end = line.find('\t', at); cells.push_back(line.substr(at, end == std::string::npos ? end : end - at));
        if (end == std::string::npos) break; at = end + 1;
    }
    return cells;
}
}
int RunPreview(const Options& options, const std::filesystem::path& mapPath,
    std::optional<std::uint8_t> tileset) {
    if (tileset && (*tileset < 'A' || *tileset > 'Z')) throw std::runtime_error("SCAN_TILESET_INVALID");
    if (tileset && options.gameRoot.empty()) throw std::runtime_error("SCAN_TILESET_REQUIRES_GAME_ROOT");
    if (options.mode == "model") return One(options, mapPath, tileset);
    if (options.mode == "enumerate") {
        try { return EnumeratePreview(options, mapPath, tileset); }
        catch (const std::exception& error) {
            // 父进程只接收子进程退出码；单独保留有界来源错误，避免只剩 CHILD_EXIT_1。
            std::ofstream diagnostic(Companion(options.tsv, ".error.txt"), std::ios::binary | std::ios::trunc);
            diagnostic << Cell(std::string_view(error.what()).substr(0, 4096)) << '\n';
            throw;
        }
    }
    for (const auto* suffix : {"", ".diagnostics.tsv", ".categories.tsv", ".scope.txt"})
        if (std::filesystem::exists(Companion(options.tsv, suffix))) throw std::runtime_error("SCAN_OUTPUT_ALREADY_EXISTS");
    // 父进程持有整批源身份；子进程不能在两次扫描之间接受一份已变化的地图/补丁包。
    AnimationSourceGuard batchGuard;
    if (!mapPath.empty()) Good(batchGuard.Watch(mapPath));
    if (!options.gameRoot.empty()) Good(batchGuard.WatchGameRoot(options.gameRoot));
    if (!options.list.empty()) Good(batchGuard.Watch(options.list));
    const auto deadline = Clock::now() + std::chrono::seconds(options.limits.totalSeconds);
    const auto remaining = [&] {
        return static_cast<std::uint32_t>(std::max<std::int64_t>(0,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count()));
    };
    const auto arguments = [&](const char* mode, const std::filesystem::path& output) {
        auto args = LimitArguments(options.limits);
        args.insert(args.end(), {"--preview", "1", "--internal", mode, "--tsv", Utf8(output)});
        if (!options.gameRoot.empty()) args.insert(args.end(), {"--game-root", Utf8(options.gameRoot)});
        if (!mapPath.empty()) args.insert(args.end(), {"--map", Utf8(mapPath)});
        if (!options.list.empty()) args.insert(args.end(), {"--list", Utf8(options.list)});
        if (tileset) args.insert(args.end(), {"--tileset", std::string(1, static_cast<char>(*tileset))});
        return args;
    };
    Scratch scratch;
    const auto candidates = scratch.path / "models.txt";
    const auto enumerated = RunChild(options, arguments("enumerate", candidates), std::min(remaining(), 30000u));
    if (!enumerated.empty()) {
        const auto errorPath = Companion(candidates, ".error.txt");
        if (std::filesystem::exists(errorPath)) {
            const auto errors = ReadLines(errorPath, 8192, 2);
            if (!errors.empty()) throw std::runtime_error("SCAN_ENUMERATION_" + errors.front());
        }
        throw std::runtime_error("SCAN_ENUMERATION_" + enumerated);
    }
    Good(batchGuard.Verify());
    const auto names = ReadLines(candidates, 16ull * 1024 * 1024, options.limits.maxModels);
    const auto scope = ReadLines(Companion(candidates, ".scope.txt"), 16ull * 1024 * 1024, 100000);
    std::ofstream models(options.tsv, std::ios::binary | std::ios::trunc);
    std::ofstream diagnostics(Companion(options.tsv, ".diagnostics.tsv"), std::ios::binary | std::ios::trunc);
    models << ModelHeader() << '\n'; diagnostics << DiagnosticHeader() << '\n';
    struct Count { std::uint64_t models = 0, occurrences = 0; std::vector<std::string> examples; };
    std::map<std::array<std::string, 3>, Count> categories;
    std::uint64_t uncovered = 0, policyRows = 0;
    for (const auto& name : names) {
        Good(batchGuard.Verify());
        Options worker = options; worker.model = name; worker.tsv = scratch.path / "result.tsv";
        std::error_code error;
        std::filesystem::remove(worker.tsv, error); std::filesystem::remove(Companion(worker.tsv, ".diagnostics.tsv"), error);
        auto args = arguments("model", worker.tsv); args.insert(args.end(), {"--model", name});
        const auto failure = remaining() ? RunChild(options, args, std::min(remaining(), options.limits.modelMs + 250)) : "SCAN_TOTAL_TIME_LIMIT";
        Good(batchGuard.Verify());
        if (!failure.empty()) Write(worker.tsv, Failure(worker, failure));
        std::vector<std::string> rows, issues;
        try {
            rows = ReadLines(worker.tsv, 1024 * 1024, 16);
            issues = ReadLines(Companion(worker.tsv, ".diagnostics.tsv"), 16 * 1024 * 1024, 32768);
            if (rows.size() != 2) throw std::runtime_error("SCAN_CHILD_REPORT_INCOMPLETE");
        } catch (const std::exception&) {
            Write(worker.tsv, Failure(worker, "SCAN_CHILD_REPORT_INCOMPLETE"));
            rows = ReadLines(worker.tsv, 1024 * 1024, 16);
            issues = ReadLines(Companion(worker.tsv, ".diagnostics.tsv"), 16 * 1024 * 1024, 32768);
        }
        for (const auto& row : rows) {
            const auto cells = Cells(row); if (cells.size() != 10) throw std::runtime_error("SCAN_CHILD_REPORT_INVALID");
            ++policyRows; uncovered += cells[4] == "uncovered"; models << row << '\n';
        }
        for (const auto& issue : issues) {
            const auto cells = Cells(issue); if (cells.size() != 11) throw std::runtime_error("SCAN_CHILD_REPORT_INVALID");
            std::uint64_t count = 0; const auto number = std::from_chars(cells[7].data(), cells[7].data() + cells[7].size(), count);
            if (number.ec != std::errc{} || number.ptr != cells[7].data() + cells[7].size()) throw std::runtime_error("SCAN_CHILD_REPORT_INVALID");
            auto& category = categories[{cells[3], cells[4], cells[5]}];
            ++category.models; category.occurrences += count;
            if (category.examples.size() < 3) category.examples.push_back(cells[0]);
            diagnostics << issue << '\n';
        }
        models.flush(); diagnostics.flush(); if (!models || !diagnostics) throw std::runtime_error("SCAN_REPORT_WRITE_FAILED");
    }
    std::ofstream grouped(Companion(options.tsv, ".categories.tsv"), std::ios::binary | std::ios::trunc);
    grouped << "policy\tcategory\tcode\tmodels\tdiagnostic_occurrences\trepresentative_models\n";
    for (const auto& [key, value] : categories) {
        grouped << key[0] << '\t' << key[1] << '\t' << key[2] << '\t' << value.models << '\t' << value.occurrences << '\t';
        for (std::size_t i = 0; i < value.examples.size(); ++i) { if (i) grouped << " | "; grouped << value.examples[i]; }
        grouped << '\n';
    }
    std::ofstream info(Companion(options.tsv, ".scope.txt"), std::ios::binary | std::ios::trunc);
    bool partial = false;
    for (const auto& line : scope) {
        info << line << '\n';
        partial |= line.starts_with("scope_notice=") || line == "complete_member_enumeration=0";
    }
    info << "quality=PreviewApproximate\ngpu_evidence=0\ngame_evidence=0\nsource_readonly=1\n"
        << "samples=sequence-start-middle-end-with-explicit-camera\npolicies=strict,interactive\n"
        << "model_ms=" << options.limits.modelMs << "\ntotal_seconds=" << options.limits.totalSeconds
        << "\nchild_memory_mib=" << options.limits.memoryMiB << "\nfile_mib=" << options.limits.fileMiB
        << "\nmax_models=" << options.limits.maxModels << "\nmax_samples_per_policy=" << options.limits.maxSamples
        << "\nmax_nodes=" << options.limits.maxNodes << "\nmax_vertices=" << options.limits.maxVertices
        << "\nresource_cache_entries=4096\nresource_cache_mib=256\ntexture_dimension=256\ntexture_mib=32\n"
        << "uncovered_policy_rows=" << uncovered << "\npolicy_rows=" << policyRows
        << "\nnamed_samples_complete=" << (uncovered == 0) << "\nscope_partial=" << partial << '\n';
    grouped.flush(); info.flush(); if (!grouped || !info) throw std::runtime_error("SCAN_REPORT_WRITE_FAILED");
    std::cout << "PREVIEW_SCAN_SUMMARY models=" << names.size() << " policy_rows=" << policyRows
        << " uncovered=" << uncovered << " scope_partial=" << partial << " tsv=" << Cell(Utf8(options.tsv)) << '\n';
    return uncovered || partial ? 2 : 0;
}
}
