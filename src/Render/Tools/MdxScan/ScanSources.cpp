#include "ScanTypes.h"
#include <algorithm>
#include <fstream>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include "War3MapCore/Game/GameDataSource.h"
#endif
namespace War3Map::Render::Scan {
namespace {
bool MdxName(const std::string& name) {
    if (name.size() < 4) return false;
    const auto tail = name.substr(name.size()-4);
    return tail[0] == '.' && (tail[1] == 'm' || tail[1] == 'M') && (tail[2] == 'd' || tail[2] == 'D') && (tail[3] == 'x' || tail[3] == 'X');
}
std::string Key(std::string text) {
    for (auto& c : text) { if (c == '\\') c = '/'; if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A')); }
    return text;
}
void SafeName(const std::string& name) {
    if (name.empty() || name.size() > 8192 || name.find_first_of("\r\n\t") != std::string::npos || name.find('\0') != std::string::npos)
        throw std::runtime_error("SCAN_MODEL_PATH_INVALID");
}
}
void Enumerate(const Options& options) {
    std::vector<std::string> names, warnings;
    if (!options.list.empty()) {
        auto input = ReadLines(options.list,16ull*1024*1024,100000);
        if (!input.empty() && input[0].starts_with("\xef\xbb\xbf")) input[0].erase(0,3);
        for (auto& name : input) {
            if (name.empty() || name.starts_with('#')) continue;
            SafeName(name);
            if (!MdxName(name)) { warnings.push_back("SCAN_NON_MDX_LIST_ENTRY"); continue; }
            if (options.gameRoot.empty()) {
                auto path = FromUtf8(name);
                if (path.is_relative()) path = options.list.parent_path() / path;
                name = Utf8(std::filesystem::absolute(path).lexically_normal());
            }
            names.push_back(std::move(name));
        }
    } else {
#ifdef _WIN32
        auto source = Game::GameDataSource::MountStrict(options.gameRoot);
        if (!source.Succeeded()) throw std::runtime_error(source.diagnostics.front().code);
        auto files = source.value->ListFilesWithSource();
        if (!files.Succeeded()) throw std::runtime_error(files.diagnostics.front().code);
        for (const auto& d : files.diagnostics) warnings.push_back(d.code);
        for (const auto& file : *files.value) if (MdxName(file.path)) { SafeName(file.path); names.push_back(file.path); }
        warnings.push_back("SCAN_MPQ_LISTFILE_COVERAGE_ONLY");
#else
        throw std::runtime_error("SCAN_GAME_ROOT_REQUIRES_WINDOWS");
#endif
    }
    std::set<std::string> seen;
    std::vector<std::string> unique;
    for (auto& name : names) {
        // 本地清单区分 Linux 大小写；游戏归档按现有 MPQ 名字规则去重。
        const auto identity = options.gameRoot.empty() ? name : Key(name);
        if (seen.insert(identity).second) unique.push_back(std::move(name));
    }
    const auto discovered = unique.size();
    if (unique.size() > options.limits.maxModels) { unique.resize(options.limits.maxModels); warnings.push_back("SCAN_MODEL_COUNT_LIMIT"); }
    std::ofstream out(options.tsv,std::ios::binary | std::ios::trunc);
    for (const auto& name : unique) out << name << '\n';
    out.flush(); if (!out) throw std::runtime_error("SCAN_ENUMERATE_WRITE_FAILED");
    auto infoPath = options.tsv; infoPath += ".info";
    std::ofstream info(infoPath,std::ios::binary | std::ios::trunc);
    info << "discovered=" << discovered << "\nselected=" << unique.size() << '\n';
    for (const auto& warning : warnings) info << warning << '\n';
    info.flush(); if (!info) throw std::runtime_error("SCAN_ENUMERATE_WRITE_FAILED");
}
std::vector<std::byte> ReadModel(const Options& options, std::string& layer) {
    SafeName(options.model);
    const auto maxBytes = static_cast<std::uint64_t>(options.limits.fileMiB) * 1024 * 1024;
    if (!options.gameRoot.empty()) {
#ifdef _WIN32
        auto source = Game::GameDataSource::MountStrict(options.gameRoot);
        if (!source.Succeeded()) throw std::runtime_error(source.diagnostics.front().code);
        // 指定 f977ff4 的来源绑定读取，损坏高优先级归档不能回落成低层“成功”。
        auto bytes = source.value->ReadFileBoundedWithSource(options.model,static_cast<std::size_t>(maxBytes));
        if (!bytes.Succeeded()) throw std::runtime_error(bytes.diagnostics.front().code);
        layer = bytes.value->archiveName;
        return std::move(bytes.value->bytes);
#else
        throw std::runtime_error("SCAN_GAME_ROOT_REQUIRES_WINDOWS");
#endif
    }
    const auto path = FromUtf8(options.model);
    std::ifstream in(path,std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("SCAN_MODEL_OPEN_FAILED");
    const auto end = in.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > maxBytes) throw std::runtime_error("SCAN_MODEL_FILE_LIMIT");
    std::vector<std::byte> bytes(static_cast<std::size_t>(end)); in.seekg(0);
    if (!bytes.empty()) in.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if (!in || in.peek() != std::ifstream::traits_type::eof()) throw std::runtime_error("SCAN_MODEL_CHANGED_OR_READ_FAILED");
    layer = "file-list"; return bytes;
}
}
