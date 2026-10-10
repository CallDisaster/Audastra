#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Game/SoundInfoTables.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Text/SlkCells.h"
#include "War3MapCore/Text/TextUtil.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <system_error>

namespace War3Map::Game {
namespace {

// 核心 MPQ，按加载优先级从低到高。查找时反向（patch 优先）。
constexpr std::array<const char*, 4> kCoreArchives{
    "war3.mpq",      // 基础
    "war3x.mpq",     // 资料片
    "war3xlocal.mpq",// 本地化
    "war3patch.mpq", // 补丁（最高优先级）
};

// 在目录中按大小写不敏感方式查找实际文件名（Windows 通常不敏感，但保持稳健）。
Result<std::filesystem::path> ResolveArchivePath(
    const std::filesystem::path& root, const char* archiveName, bool strict = false) {
    using PathResult = Result<std::filesystem::path>;
    const std::filesystem::path direct = root / archiveName;
    std::error_code ec;
    const auto directStatus = std::filesystem::status(direct, ec);
    if (std::filesystem::is_regular_file(directStatus)) {
        return PathResult::Success(direct);
    }
    if (strict && ec && ec != std::errc::no_such_file_or_directory) {
        return PathResult::Failure("Game.DataSource", "archive-path-error",
            "无法检查游戏归档: " + direct.string() + ": " + ec.message());
    }
    if (strict && std::filesystem::exists(directStatus)) {
        return PathResult::Failure("Game.DataSource", "archive-not-file",
            "游戏归档路径存在但不是普通文件: " + direct.string());
    }
    if (strict && !std::filesystem::exists(directStatus)) {
        // status 会把悬空链接当作不存在；严格来源仍须拒绝这个存在却不可读的入口。
        std::error_code linkError;
        const auto linkStatus = std::filesystem::symlink_status(direct, linkError);
        if (std::filesystem::exists(linkStatus) ||
            (linkError && linkError != std::errc::no_such_file_or_directory)) {
            return PathResult::Failure("Game.DataSource", "archive-path-error",
                "游戏归档入口存在但不可读取: " + direct.string());
        }
    }
    // 回退：遍历目录做大小写不敏感匹配。
    std::string target = archiveName;
    for (char& c : target) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    ec.clear();
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (name == target) {
            if (it->is_regular_file(ec)) return PathResult::Success(it->path());
            if (strict) {
                return PathResult::Failure("Game.DataSource", "archive-path-error",
                    "无法读取游戏归档文件: " + it->path().string() + ": " + ec.message());
            }
        }
    }
    if (strict && ec) {
        return PathResult::Failure("Game.DataSource", "archive-path-error",
            "无法枚举游戏归档目录: " + root.string() + ": " + ec.message());
    }
    return PathResult::Success({});
}

} // namespace

bool GameRootValidation::Usable() const noexcept {
    // war3.mpq 与 war3x.mpq 是图标资源的主要来源，二者可打开即可用。
    bool base = false;
    bool expansion = false;
    for (const auto& info : archives) {
        if (info.fileName == "war3.mpq" && info.opened) base = true;
        if (info.fileName == "war3x.mpq" && info.opened) expansion = true;
    }
    return base && expansion;
}

std::string GameRootValidation::Summary() const {
    std::string result;
    for (const auto& info : archives) {
        result += "  " + info.fileName + ": ";
        if (!info.present) {
            result += "缺失";
        } else if (!info.opened) {
            result += "存在但无法作为 MPQ 打开";
        } else {
            result += "OK (" + std::to_string(info.fileCount) + " 块)";
        }
        result += "\n";
    }
    return result;
}

GameRootValidation GameDataSource::Validate(const std::filesystem::path& war3Root) {
    GameRootValidation validation;
    validation.root = war3Root;

    std::error_code ec;
    if (!std::filesystem::is_directory(war3Root, ec)) {
        return validation;
    }

    for (const char* archiveName : kCoreArchives) {
        GameArchiveInfo info;
        info.fileName = archiveName;
        const auto resolved = ResolveArchivePath(war3Root, archiveName);
        const auto path = resolved.value.value_or(std::filesystem::path{});
        if (path.empty()) {
            validation.archives.push_back(std::move(info));
            continue;
        }
        info.present = true;
        auto opened = Mpq::MpqArchive::OpenReadOnly(path);
        if (opened.Succeeded() && opened.value.has_value()) {
            info.opened = true;
            info.fileCount = opened.value->FileCount();
        }
        validation.archives.push_back(std::move(info));
    }
    return validation;
}

Result<GameDataSource> GameDataSource::Mount(const std::filesystem::path& war3Root) {
    return MountImpl(war3Root, false);
}

Result<GameDataSource> GameDataSource::MountStrict(const std::filesystem::path& war3Root) {
    return MountImpl(war3Root, true);
}

Result<GameDataSource> GameDataSource::MountStrict(const std::filesystem::path& war3Root,
    std::uint8_t mainTileset) {
    if (mainTileset < 'A' || mainTileset > 'Z')
        return Result<GameDataSource>::Failure("Game.DataSource", "tileset-invalid",
            "地形集必须是 W3E 的大写 ASCII 地形集字节。");
    auto mounted = MountImpl(war3Root, true);
    if (!mounted.Succeeded()) return mounted;
    auto& source = *mounted.value;
    const auto name = std::string(1, static_cast<char>(mainTileset)) + ".mpq";
    if (!source.HasFile(name)) {
        mounted.diagnostics.push_back({DiagnosticSeverity::Info, "Game.DataSource", "tileset-archive-absent",
            "游戏归档没有 " + name + "，使用普通游戏资源（不猜地形集前缀别名）。", std::nullopt});
        return mounted;
    }
    constexpr std::size_t maxBytes = 64u * 1024u * 1024u;
    auto bytes = source.ReadFileBoundedWithSource(name, maxBytes);
    if (!bytes.Succeeded()) return {std::nullopt, std::move(bytes.diagnostics)};
    auto sha = Sha256(bytes.value->bytes);
    if (!sha.Succeeded()) return {std::nullopt, std::move(sha.diagnostics)};
    // 外层实际命中包、嵌套成员及完整原字节身份均进入来源名称，动画/截图可直接留证。
    auto identity = bytes.value->archiveName + "|" + name + "|sha256:" + *sha.value;
    Mpq::MpqOpenLimits limits;
    limits.maxArchivePrefixBytes = 4096;
    limits.maxDeclaredArchiveBytes = maxBytes;
    limits.maxHashTableEntries = 65536;
    limits.maxBlockTableEntries = 65536;
    auto nested = Mpq::MpqArchive::OpenReadOnlyBytes(std::move(bytes.value->bytes), name, limits);
    if (!nested.Succeeded()) return {std::nullopt, std::move(nested.diagnostics)};
    // 1.27a: 主包 0..3、tileset 6、War3Patch 8；地图来源仍由调用方置于游戏层之上。
    const std::size_t index = !source.archiveNames_.empty() && source.archiveNames_.front() == "war3patch.mpq" ? 1 : 0;
    source.archives_.insert(source.archives_.begin() + static_cast<std::ptrdiff_t>(index),
        std::make_unique<Mpq::MpqArchive>(std::move(*nested.value)));
    source.archiveNames_.insert(source.archiveNames_.begin() + static_cast<std::ptrdiff_t>(index), std::move(identity));
    return mounted;
}

Result<GameDataSource> GameDataSource::MountImpl(const std::filesystem::path& war3Root, bool strict) {
    std::error_code ec;
    if (!std::filesystem::is_directory(war3Root, ec)) {
        return Result<GameDataSource>::Failure(
            "Game.DataSource", "root-not-directory",
            "魔兽争霸3 路径不是有效目录: " + war3Root.string());
    }

    GameDataSource source;
    source.root_ = war3Root;

    // 按优先级从高到低挂载（patch 在前）。
    for (auto it = kCoreArchives.rbegin(); it != kCoreArchives.rend(); ++it) {
        auto resolved = ResolveArchivePath(war3Root, *it, strict);
        if (!resolved.Succeeded()) {
            Result<GameDataSource> failure;
            failure.diagnostics = std::move(resolved.diagnostics);
            return failure;
        }
        const auto& path = *resolved.value;
        if (path.empty()) continue;
        auto opened = Mpq::MpqArchive::OpenReadOnly(path);
        if (opened.Succeeded() && opened.value.has_value()) {
            source.archives_.push_back(
                std::make_unique<Mpq::MpqArchive>(std::move(*opened.value)));
            source.archiveNames_.emplace_back(*it);
        } else if (strict) {
            auto failure = Result<GameDataSource>::Failure(
                "Game.DataSource", "archive-open-failed", "无法打开存在的游戏归档: " + path.string());
            failure.diagnostics.insert(failure.diagnostics.end(),
                opened.diagnostics.begin(), opened.diagnostics.end());
            return failure;
        }
    }

    if (source.archives_.empty()) {
        return Result<GameDataSource>::Failure(
            "Game.DataSource", "no-archives",
            "在指定目录未能打开任何核心 MPQ。");
    }
    return Result<GameDataSource>::Success(std::move(source));
}

bool GameDataSource::HasFile(const std::string& name) const {
    for (const auto& archive : archives_) {
        if (archive->HasFile(name)) return true;
    }
    return false;
}

Result<std::vector<std::byte>> GameDataSource::ReadFile(const std::string& name) const {
    // 按优先级顺序（archives_ 已是 patch 优先）查找第一个含该文件的归档。
    for (const auto& archive : archives_) {
        if (archive->HasFile(name)) {
            return archive->ReadFile(name);
        }
    }
    return Result<std::vector<std::byte>>::Failure(
        "Game.DataSource", "file-not-found",
        "在已挂载的游戏 MPQ 中未找到: " + name);
}

Result<std::vector<std::byte>> GameDataSource::ReadFileHead(const std::string& name, std::size_t maxBytes) const {
    for (const auto& archive : archives_) {
        if (archive->HasFile(name)) {
            return archive->ReadFileHead(name, maxBytes);
        }
    }
    return Result<std::vector<std::byte>>::Failure(
        "Game.DataSource", "file-not-found",
        "在已挂载的游戏 MPQ 中未找到: " + name);
}

Result<std::vector<std::byte>> GameDataSource::ReadFileBounded(
    const std::string& name, std::size_t maxBytes) const {
    auto sourced = ReadFileBoundedWithSource(name, maxBytes);
    Result<std::vector<std::byte>> result;
    result.diagnostics = std::move(sourced.diagnostics);
    if (sourced.value) result.value = std::move(sourced.value->bytes);
    return result;
}

Result<GameFileContents> GameDataSource::ReadFileBoundedWithSource(
    const std::string& name, std::size_t maxBytes) const {
    for (std::size_t index = 0; index < archives_.size(); ++index) {
        if (archives_[index]->HasFile(name)) {
            auto read = archives_[index]->ReadFileBounded(name, maxBytes);
            Result<GameFileContents> result;
            result.diagnostics = std::move(read.diagnostics);
            if (read.value) {
                result.value = GameFileContents{std::move(*read.value), archiveNames_[index]};
            }
            return result;
        }
    }
    return Result<GameFileContents>::Failure(
        "Game.DataSource", "file-not-found",
        "在已挂载的游戏 MPQ 中未找到: " + name);
}

Result<std::vector<GameResourceFile>> GameDataSource::ListFilesWithSource() const {
    Result<std::vector<GameResourceFile>> result;
    result.value.emplace();
    // 上层清单中的名字优先；同一层含规范化冲突的名字不能凭遍历顺序任取。
    std::map<std::string, std::string> named;
    std::set<std::string> blocked;
    const auto warn = [&](std::string code, std::string message) {
        result.diagnostics.push_back({DiagnosticSeverity::Warning, "Game.DataSource",
            std::move(code), std::move(message), std::nullopt});
    };
    for (std::size_t index = 0; index < archives_.size(); ++index) {
        const auto& archive = *archives_[index];
        const auto& archiveName = archiveNames_[index];
        if (!archive.HasFile("(listfile)")) {
            warn("GAME_LISTFILE_MISSING", archiveName + " 没有可枚举的 (listfile)");
            continue;
        }
        const auto& listed = archive.RawListfileNames();
        if (listed.empty()) {
            warn("GAME_LISTFILE_UNREADABLE", archiveName + " 的 (listfile) 无法读取或没有有效名称");
            continue;
        }
        std::map<std::string, std::string> local;
        std::set<std::string> localConflicts;
        for (const auto& path : listed) {
            auto canonical = Resources::CanonicalImportPath(path);
            if (!canonical.Succeeded() || !canonical.value) {
                warn("GAME_LISTFILE_PATH_UNSAFE", archiveName + " 清单含不安全名称，已跳过");
                continue;
            }
            const auto& key = *canonical.value;
            if (!archive.HasFile(path)) {
                warn("GAME_LISTFILE_NAME_UNRESOLVED", archiveName + " 清单名称无法读取：" + path);
                continue;
            }
            if (const auto [it, inserted] = local.emplace(key, path); !inserted) {
                localConflicts.insert(key);
            }
        }
        for (const auto& key : localConflicts) {
            warn("GAME_LISTFILE_NAME_CONFLICT", archiveName + " 清单有大小写或分隔符冲突：" + key);
            if (!named.contains(key)) blocked.insert(key);
        }
        for (const auto& [key, path] : local) {
            if (!localConflicts.contains(key) && !blocked.contains(key)) named.try_emplace(key, path);
        }
    }
    result.value->reserve(named.size());
    for (const auto& [key, path] : named) {
        if (blocked.contains(key)) continue;
        // 清单可来自下层，但命中来源必须再按实际游戏查找顺序确认。
        for (std::size_t index = 0; index < archives_.size(); ++index) {
            if (!archives_[index]->HasFile(path)) continue;
            result.value->push_back({path, archiveNames_[index]});
            break;
        }
    }
    return result;
}

Result<std::vector<GameResourceFile>> GameDataSource::DiscoverSoundFilesFromSoundInfo() const {
    Result<std::vector<GameResourceFile>> result;
    result.value.emplace();
    std::map<std::string, GameResourceFile> discovered;
    constexpr std::size_t kMaxTableBytes = 4u * 1024u * 1024u;
    const auto warn = [&](std::string code, std::string message) {
        result.diagnostics.push_back({DiagnosticSeverity::Warning, "Game.SoundInfo",
            std::move(code), std::move(message), std::nullopt});
    };
    for (const auto table : kSoundInfoTables) {
        const std::string member = "UI\\SoundInfo\\" + std::string(table) + ".slk";
        auto bytes = ReadFileHead(member, kMaxTableBytes + 1);
        if (!bytes.Succeeded() || !bytes.value) {
            warn("SOUNDINFO_TABLE_UNAVAILABLE", "无法读取声音目录表：" + member);
            continue;
        }
        if (bytes.value->size() > kMaxTableBytes) {
            warn("SOUNDINFO_TABLE_TOO_LARGE", "声音目录表超过读取预算：" + member);
            continue;
        }
        const std::string_view text(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size());
        auto parsed = Text::ParseSlkCells(text, Text::SlkCellMode::CatalogStrict);
        if (!parsed.Succeeded() || !parsed.value) {
            warn("SOUNDINFO_TABLE_INVALID", "声音目录表无法完整解析：" + member);
            continue;
        }
        const auto& headers = parsed.value->at(1);
        int filesColumn = 0, baseColumn = 0;
        for (const auto& [column, title] : headers) {
            const auto key = Text::LowerAscii(title);
            if (key == "filenames") filesColumn = column;
            else if (key == "directorybase") baseColumn = column;
        }
        if (filesColumn == 0) {
            warn("SOUNDINFO_FILES_COLUMN_MISSING", "声音目录表没有 FileNames 列：" + member);
            continue;
        }
        std::size_t unresolved = 0, unsafe = 0;
        std::string firstUnresolved;
        for (const auto& [rowNumber, row] : *parsed.value) {
            if (rowNumber == 1) continue;
            const auto files = row.find(filesColumn);
            if (files == row.end()) continue;
            const auto base = row.find(baseColumn);
            const std::string_view directory = base == row.end() ? std::string_view{}
                : Text::TrimCWhitespace(base->second);
            const std::string_view names = files->second;
            for (std::size_t start = 0; start <= names.size();) {
                const auto comma = names.find(',', start);
                const auto end = comma == std::string_view::npos ? names.size() : comma;
                const auto leaf = Text::TrimCWhitespace(names.substr(start, end - start));
                start = comma == std::string_view::npos ? names.size() + 1 : comma + 1;
                if (leaf.empty() || leaf == "_" || leaf == "-") continue;
                std::string candidate(directory);
                if (!candidate.empty() && candidate.back() != '\\' && candidate.back() != '/')
                    candidate.push_back('\\');
                if (!candidate.empty() && (candidate.back() == '\\' || candidate.back() == '/')
                    && (leaf.front() == '\\' || leaf.front() == '/'))
                    candidate.append(leaf.substr(1));
                else candidate.append(leaf);
                auto canonical = Resources::CanonicalImportPath(candidate);
                if (!canonical.Succeeded() || !canonical.value) { ++unsafe; continue; }
                const auto& key = *canonical.value;
                if (!key.ends_with(".WAV") && !key.ends_with(".MP3")) continue;
                if (discovered.contains(key)) continue;
                bool found = false;
                for (std::size_t index = 0; index < archives_.size(); ++index) {
                    if (!archives_[index]->HasFile(candidate)) continue;
                    discovered.emplace(key, GameResourceFile{candidate, archiveNames_[index]});
                    found = true;
                    break;
                }
                if (!found) {
                    ++unresolved;
                    if (firstUnresolved.empty()) firstUnresolved = candidate;
                }
            }
        }
        if (unsafe)
            warn("SOUNDINFO_PATH_UNSAFE", member + " 有 " + std::to_string(unsafe) + " 条不安全路径，已跳过");
        if (unresolved)
            warn("SOUNDINFO_PATH_NOT_FOUND", member + " 有 " + std::to_string(unresolved)
                + " 条路径没有实际 MPQ 成员；例如 " + firstUnresolved);
    }
    result.value->reserve(discovered.size());
    for (auto& [key, resource] : discovered) result.value->push_back(std::move(resource));
    return result;
}

} // namespace War3Map::Game
