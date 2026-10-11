#include "War3MapCore/Script/MapTestToolchain.h"

#include "../Jass/JassLexer.h"
#include "../Jass/JassParser.h"
#include "../Core/Mpq/MpqCrypt.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/MapData/MapSettings.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <span>
#include <string_view>
#include <tuple>

#include <windows.h>

namespace War3Map::Script {
namespace {
constexpr std::size_t kScriptBudget = 16u * 1024u * 1024u;
constexpr std::size_t kTokenBudget = 2u * 1024u * 1024u;
constexpr std::size_t kPeBudget = 32u * 1024u * 1024u;
using LaunchPlan = Services::MapTestLaunchPlan;
using namespace Jass::detail;

template<class T> Result<T> Failure(std::string code, std::string message) {
    return Result<T>::Failure("Map.Test.Runtime", std::move(code), std::move(message));
}
std::string_view View(const std::vector<std::byte>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// 仅识别实际已编译的 JASS 结构；函数名来自声明/调用树，不按 JAPI 前缀猜测。
Result<ProgramFile> ParseScript(std::string_view text, std::string_view name) {
    if (text.size() > kScriptBudget || text.empty())
        return Failure<ProgramFile>("MAP_TEST_SCRIPT_BUDGET", std::string(name) + " 为空或超过 16 MiB 读取预算。");
    const auto bomBytes = text.starts_with("\xEF\xBB\xBF") ? 3u : 0u;
    text.remove_prefix(bomBytes);
    const auto malformed = [&](std::size_t offset, std::string detail) {
        const auto line = 1 + std::count(text.begin(), text.begin() + (std::min)(offset, text.size()), '\n');
        auto result = Failure<ProgramFile>("MAP_TEST_SCRIPT_MALFORMED", std::string(name) + ":"
            + std::to_string(line) + "（字节 " + std::to_string(offset + bomBytes) + "）" + std::move(detail));
        result.diagnostics.back().origin = OriginSpan{std::filesystem::path(name), offset + bomBytes, 1, {}};
        return result;
    };
    const auto control = text.find_first_of(std::string_view("\0\x1a", 2));
    if (control != std::string_view::npos)
        return malformed(control, "包含 NUL 或 Ctrl-Z 控制字节，不能截断脚本后启动。");
    // 这里检查的是保存后最终JASS，复用保存CheckJass的字节词法规则。
    // 历史自定义脚本的UTF-8/GBK注释、字符串和fourCC可混合；不猜整篇编码或改写字节。
    // 实际代码仍只接受共享lexer识别的记号，文本中的假调用不参与运行时分类。
    auto bounded = LexJassBounded(text, LexOptions{false, true}, kTokenBudget);
    if (!bounded.Succeeded()) {
        auto result = Failure<ProgramFile>("MAP_TEST_SCRIPT_BUDGET", std::string(name) + " 的词法记号超过有界检查预算，读取已停止。");
        for (auto& diagnostic : bounded.diagnostics) {
            if (diagnostic.origin) {
                diagnostic.origin->source = std::filesystem::path(name);
                diagnostic.origin->offset += bomBytes;
            }
        }
        result.diagnostics.insert(result.diagnostics.begin(), bounded.diagnostics.begin(), bounded.diagnostics.end());
        return result;
    }
    auto tokens = std::move(*bounded.value);
    std::size_t groups = 0, blocks = 0, prefixes = 0;
    for (const auto& token : tokens) {
        if (token.kind == TokenKind::String && !ValidStringEscapes(token.text))
            return malformed(token.offset, "字符串含无效转义。");
        if (token.kind == TokenKind::Rawcode) {
            std::size_t length = 0;
            for (std::size_t i = 1; i + 1 < token.text.size(); ++i) {
                if (token.text[i] == '\\') ++i;
                ++length;
            }
            if (length != 1 && length != 4)
                return malformed(token.offset, "rawcode 必须含 1 或 4 个原字节字符（转义计一个），与保存校验一致。");
        }
        if (token.kind == TokenKind::Invalid)
            return malformed(token.offset, "含无效代码记号或未闭合的字符串/rawcode。");
        if (token.kind == TokenKind::Pragma && token.text.starts_with("//!"))
            return malformed(token.offset, "仍含未展开的编译指令，请先重建地图脚本。");
        if (token.IsOperator("(") || token.IsOperator("[")) ++groups;
        if ((token.IsOperator(")") || token.IsOperator("]")) && groups) --groups;
        if (token.Is(Keyword::If) || token.Is(Keyword::Loop)) ++blocks;
        if ((token.Is(Keyword::EndIf) || token.Is(Keyword::EndLoop)) && blocks) --blocks;
        prefixes = token.Is(Keyword::Not) || token.IsOperator("+") || token.IsOperator("-") ? prefixes + 1 : 0;
        if (groups > 64 || blocks > 64 || prefixes > 64)
            return Failure<ProgramFile>("MAP_TEST_SCRIPT_BUDGET", std::string(name) + " 的语法嵌套超过 64 层。");
    }
    auto parsed = ParseProgram(std::move(tokens));
    if (!parsed.errors.empty())
        return Failure<ProgramFile>("MAP_TEST_SCRIPT_MALFORMED", std::string(name) + ":"
            + std::to_string(parsed.errors.front().line) + " " + parsed.errors.front().message.substr(0, 180));
    return Result<ProgramFile>::Success(std::move(parsed));
}

struct Symbols {
    std::set<std::string, std::less<>> declared, natives, functions, calls;
};
Status CollectSymbols(const ProgramFile& file, std::string_view name, Symbols& symbols) {
    const auto addCalls = [&](const SyntaxPool& pool) {
        for (const auto& expression : pool.expressions) {
            if (expression.kind == ExprKind::Call || expression.kind == ExprKind::FunctionRef)
                symbols.calls.emplace(expression.text);
        }
    };
    for (const auto& native : file.natives) {
        if (!symbols.declared.emplace(native.name).second)
            return Status::Failure("Map.Test.Runtime", "MAP_TEST_SCRIPT_MALFORMED", std::string(name) + " 含重复的 native 或函数声明。");
        symbols.natives.emplace(native.name);
    }
    for (const auto& function : file.functions) {
        if (!symbols.declared.emplace(function.name).second)
            return Status::Failure("Map.Test.Runtime", "MAP_TEST_SCRIPT_MALFORMED", std::string(name) + " 含重复的 native 或函数声明。");
        symbols.functions.emplace(function.name);
        const auto body = ParseFunctionBody(file.tokens, function);
        if (!body.errors.empty())
            return Status::Failure("Map.Test.Runtime", "MAP_TEST_SCRIPT_MALFORMED", std::string(name) + ":"
                + std::to_string(body.errors.front().line) + " " + body.errors.front().message.substr(0, 180));
        addCalls(body.pool);
    }
    addCalls(file.globalPool);
    return Status::Ok();
}

struct RuntimeRequirement { std::set<std::string, std::less<>> missing; };

// 持续只读租约绑定同一文件对象；一轮流式SHA后，只读取五个明确成员。
// 不建立编辑会话、不解码模型/贴图/音频，也不再把整张地图读入vector。
class RuntimeMapLease {
public:
    explicit RuntimeMapLease(const std::filesystem::path& path) : path_(path) {
        file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    }
    ~RuntimeMapLease() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    RuntimeMapLease(const RuntimeMapLease&) = delete;
    RuntimeMapLease& operator=(const RuntimeMapLease&) = delete;
    Status Verify(std::string_view expected) {
        if (file_ == INVALID_HANDLE_VALUE || GetFileType(file_) != FILE_TYPE_DISK
            || !GetFileInformationByHandle(file_, &before_)
            || before_.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT)
            || before_.nNumberOfLinks != 1)
            return Status::Failure("Map.Test.Runtime", "MAP_TEST_SCRIPT_FILE", "无法只读锁定普通测试地图文件；未读取脚本或启动游戏。");
        const auto size = (static_cast<std::uint64_t>(before_.nFileSizeHigh) << 32) | before_.nFileSizeLow;
        if (!size || size > 1024ull * 1024 * 1024)
            return Status::Failure("Map.Test.Runtime", "MAP_TEST_SCRIPT_BUDGET", "测试地图超过 1 GiB 流式核验预算或为空。");
        auto normalized = std::string(expected);
        for (auto& c : normalized) if (c >= 'A' && c <= 'F') c = static_cast<char>(c - 'A' + 'a');
        const auto sha = Sha256Handle(file_);
        if (!sha.Succeeded()) { Status result; result.diagnostics = sha.diagnostics; return result; }
        if (*sha.value != normalized)
            return Status::Failure("Map.Test.Runtime", "MAP_TEST_SCRIPT_STALE", "测试地图在核心预检后改变，不能用另一份脚本判断运行时需求。");
        return Unchanged();
    }
    Status Unchanged() const {
        const auto same = [&](const BY_HANDLE_FILE_INFORMATION& info) {
            return info.dwVolumeSerialNumber == before_.dwVolumeSerialNumber
                && info.nFileIndexHigh == before_.nFileIndexHigh && info.nFileIndexLow == before_.nFileIndexLow
                && info.nFileSizeHigh == before_.nFileSizeHigh && info.nFileSizeLow == before_.nFileSizeLow
                && CompareFileTime(&info.ftLastWriteTime, &before_.ftLastWriteTime) == 0
                && !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT))
                && info.nNumberOfLinks == 1;
        };
        BY_HANDLE_FILE_INFORMATION current{}, named{};
        const auto pathHandle = CreateFileW(path_.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const bool valid = GetFileInformationByHandle(file_, &current) && same(current)
            && pathHandle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(pathHandle, &named) && same(named);
        if (pathHandle != INVALID_HANDLE_VALUE) CloseHandle(pathHandle);
        return valid ? Status::Ok() : Status::Failure("Map.Test.Runtime", "MAP_TEST_SCRIPT_STALE",
            "测试地图路径或已锁定文件身份发生变化，预检已停止。");
    }
    HANDLE Get() const { return file_; }
    std::uint64_t Size() const { return (static_cast<std::uint64_t>(before_.nFileSizeHigh) << 32) | before_.nFileSizeLow; }
private:
    std::filesystem::path path_;
    HANDLE file_ = INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION before_{};
};

Result<RuntimeRequirement> InspectMapRuntime(const Services::MapTestArtifact& artifact,
    const Services::MapTestProgress& progress) {
    RuntimeMapLease lease(artifact.testMap);
    const auto verified = lease.Verify(artifact.testMapSha256);
    if (!verified.Succeeded()) return {std::nullopt, verified.diagnostics};
    Mpq::MpqOpenLimits openLimits;
    openLimits.maxArchivePrefixBytes = 16u * 1024u * 1024u;
    openLimits.maxDeclaredArchiveBytes = 1024ull * 1024 * 1024;
    openLimits.maxStrictMemberCount = 65536;
    auto map = Mpq::MpqArchive::OpenReadOnlyHandle(lease.Get(), artifact.testMap, openLimits);
    if (!map.Succeeded()) return {std::nullopt, std::move(map.diagnostics)};
    auto structure = map.value->ListFilesStrict();
    if (!structure.Succeeded() && std::any_of(structure.diagnostics.begin(), structure.diagnostics.end(), [](const auto& item) {
            return item.code == "ANONYMOUS_BLOCK_REJECTED";
        })) structure = map.value->ListFilesPreservingOpaque();
    if (!structure.Succeeded()) return {std::nullopt, structure.diagnostics};
    if (lease.Size() != map.value->ArchivePrefixSize() + map.value->DeclaredArchiveSize())
        return Failure<RuntimeRequirement>("ARCHIVE_TRAILING_DATA", "地图归档声明范围外存在未识别数据，测试预检已停止。");
    if (progress) progress("read-scripts");
    constexpr std::array<const char*, 5> names{"war3map.j", "scripts\\war3map.j", "war3map.lua", "scripts\\war3map.lua", "war3map.w3i"};
    // 匿名块未经过listfile的明确名字检查；复用CaptureExactMembers的hash/标志门禁，
    // 仅核对这五个成员，不枚举或解码其他资源。
    const auto hashes = map.value->HashTableEntries();
    const auto blocks = map.value->BlockTableEntries();
    const auto& crypt = Mpq::MpqCrypt::Instance();
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::size_t> queriedHashes;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!queriedHashes.emplace(std::pair{crypt.HashString(names[i], Mpq::MpqCrypt::HashType::NameA),
                crypt.HashString(names[i], Mpq::MpqCrypt::HashType::NameB)}, i).second)
            return Failure<RuntimeRequirement>("EXACT_MEMBER_HASH_CONFLICT", "不同明确来源名字具有相同的 MPQ hash 对。");
    }
    std::array<std::uint32_t, names.size()> queriedBlocks;
    queriedBlocks.fill(Mpq::kMpqHashEmpty);
    for (const auto& hash : hashes) {
        if (hash.blockIndex == Mpq::kMpqHashEmpty || hash.blockIndex == Mpq::kMpqHashDeleted) continue;
        const auto wanted = queriedHashes.find({hash.name1, hash.name2});
        if (wanted != queriedHashes.end()) queriedBlocks[wanted->second] = hash.blockIndex;
    }
    constexpr auto knownFlags = Mpq::kFileImplode | Mpq::kFileCompress | Mpq::kFileEncrypted |
        Mpq::kFileFixKey | Mpq::kFilePatch | Mpq::kFileSingleUnit | Mpq::kFileSectorCrc | Mpq::kFileExists;
    struct Member { bool present = false; std::vector<std::byte> bytes; };
    std::array<Member, 5> members;
    std::size_t remaining = kScriptBudget;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto blockIndex = queriedBlocks[i];
        const bool present = map.value->HasFile(names[i]);
        if (present != (blockIndex != Mpq::kMpqHashEmpty))
            return Failure<RuntimeRequirement>("EXACT_MEMBER_HASH_UNREACHABLE",
                std::string("明确来源的名字 hash 存在但桶链不可达：") + names[i]);
        if (!present) continue;
        const auto& block = blocks[blockIndex]; // 全局结构检查已验证每个有效hash的块边界。
        if ((block.flags & ~knownFlags) != 0 ||
            ((block.flags & Mpq::kFileFixKey) != 0 && (block.flags & Mpq::kFileEncrypted) == 0) ||
            ((block.flags & Mpq::kFileImplode) != 0 && (block.flags & Mpq::kFileCompress) != 0))
            return Failure<RuntimeRequirement>("FILE_FLAGS_UNSUPPORTED",
                std::string("明确来源块标志不受支持：") + names[i]);
        if ((block.flags & (Mpq::kFilePatch | Mpq::kFileSectorCrc)) != 0)
            return Failure<RuntimeRequirement>("EXACT_MEMBER_ENCODING_UNSUPPORTED",
                std::string("明确来源为未支持的增量补丁或扇区 CRC 块：") + names[i]);
        auto read = map.value->ReadFileBounded(names[i], remaining);
        if (!read.Succeeded()) return Failure<RuntimeRequirement>("MAP_TEST_SCRIPT_BUDGET",
            "地图脚本成员损坏、不可读或超过 16 MiB 总读取预算，测试已停止。");
        members[i].present = true; members[i].bytes = std::move(*read.value);
        remaining -= members[i].bytes.size();
    }
    int language = 0;
    if (members[4].present) {
        const auto settings = MapData::MapSettings::Parse(members[4].bytes);
        if (!settings.Succeeded())
            return Failure<RuntimeRequirement>("MAP_TEST_SETTINGS_MALFORMED", "地图信息损坏，无法确定测试脚本语言。");
        language = settings.value->scriptLanguage;
        if (language != 0 && language != 1)
            return Failure<RuntimeRequirement>("MAP_TEST_SCRIPT_LANGUAGE", "地图脚本语言未受支持，不能猜测启动运行时。");
    } else if (!members[0].present && !members[1].present && (members[2].present || members[3].present)) language = 1;
    const auto index = language == 1 ? (members[2].present ? 2u : 3u) : (members[0].present ? 0u : 1u);
    if (!members[index].present || members[index].bytes.empty())
        return Failure<RuntimeRequirement>("MAP_TEST_SCRIPT_MISSING", "测试地图缺少与地图信息一致的非空运行脚本，请先重建脚本。");
    RuntimeRequirement requirement;
    if (language == 1) {
        // 该经典独立档案没有原生 Lua 执行器。旧插件观察 JASS，并读取 script/war3map.lua；
        // 不能把根 war3map.lua 的现代原生格式误当作配置六个 DLL 后即可执行。
        if (std::find(members[index].bytes.begin(), members[index].bytes.end(), std::byte{0}) != members[index].bytes.end())
            return Failure<RuntimeRequirement>("MAP_TEST_SCRIPT_MALFORMED", "Lua 地图脚本包含 NUL，不能作为完整源码执行。");
        return Failure<RuntimeRequirement>("MAP_TEST_LUA_CLIENT_UNSUPPORTED",
            "当前经典客户端的独立测试支持 JASS/JAPI；此 Lua 地图需要支持原生 Lua 的客户端或专用测试适配，不能仅配置六个扩展 DLL。原图与当前编辑保持。");
    }
    auto game = Game::GameDataSource::MountStrict(artifact.gameRoot);
    if (!game.Succeeded()) return {std::nullopt, std::move(game.diagnostics)};
    const auto read = [&](const char* file, std::size_t remaining) {
        const std::string scripts = std::string("scripts\\") + file;
        return game.value->ReadFileBounded(game.value->HasFile(scripts) ? scripts : file, remaining);
    };
    if (remaining == 0)
        return Failure<RuntimeRequirement>("MAP_TEST_SCRIPT_BUDGET", "地图脚本和所选游戏基础脚本合计超过 16 MiB 预算。");
    auto common = read("common.j", remaining);
    if (!common.Succeeded())
        return Failure<RuntimeRequirement>("MAP_TEST_GAME_SCRIPTS", "所选游戏无法在剩余读取预算内完整读取 common.j，不能确定原生接口。");
    remaining -= common.value->size();
    if (remaining == 0)
        return Failure<RuntimeRequirement>("MAP_TEST_SCRIPT_BUDGET", "地图脚本和所选游戏基础脚本合计超过 16 MiB 预算。");
    auto blizzard = read("blizzard.j", remaining);
    if (!blizzard.Succeeded())
        return Failure<RuntimeRequirement>("MAP_TEST_GAME_SCRIPTS", "所选游戏无法在剩余读取预算内完整读取 blizzard.j，不能确定原生接口。");
    if (progress) progress("check-script");
    auto commonFile = ParseScript(View(*common.value), "game/common.j");
    auto blizzardFile = ParseScript(View(*blizzard.value), "game/blizzard.j");
    auto mapFile = ParseScript(View(members[index].bytes), names[index]);
    for (const auto* parsed : {&commonFile, &blizzardFile, &mapFile})
        if (!parsed->Succeeded()) return {std::nullopt, parsed->diagnostics};
    Symbols base, original, mapSymbols;
    for (const auto& [file, name, output] : {
            std::tuple{&*commonFile.value, "game/common.j", &base},
            std::tuple{&*blizzardFile.value, "game/blizzard.j", &original},
            std::tuple{&*mapFile.value, names[index], &mapSymbols}}) {
        const auto collected = CollectSymbols(*file, name, *output);
        if (!collected.Succeeded()) return {std::nullopt, collected.diagnostics};
    }
    if (!mapSymbols.functions.contains("main") || !mapSymbols.functions.contains("config"))
        return Failure<RuntimeRequirement>("MAP_TEST_SCRIPT_ENTRYPOINT", "测试地图缺少 main 或 config 入口，请先重建地图脚本。");
    for (const auto& name : mapSymbols.natives)
        if (!base.natives.contains(name)) requirement.missing.emplace(name);
    for (const auto& name : mapSymbols.calls)
        if (!mapSymbols.declared.contains(name) && !base.declared.contains(name) && !original.declared.contains(name))
            requirement.missing.emplace(name);
    const auto unchanged = lease.Unchanged();
    if (!unchanged.Succeeded()) return {std::nullopt, unchanged.diagnostics};
    return Result<RuntimeRequirement>::Success(std::move(requirement));
}

bool OrdinaryAbsoluteFile(const std::filesystem::path& path) {
    if (!path.is_absolute()) return false;
    auto current = path;
    while (!current.empty()) {
        const auto attrs = GetFileAttributesW(current.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

// PE 仅按有界字节解析。预检不会 LoadLibrary、触发 DllMain 或执行任何插件。
class PeBytes {
public:
    explicit PeBytes(std::span<const std::byte> bytes) : bytes_(bytes) {}
    bool Fits(std::size_t offset, std::size_t count) const { return offset <= bytes_.size() && count <= bytes_.size() - offset; }
    std::uint16_t U16(std::size_t at) const {
        return static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes_[at]) | (std::to_integer<unsigned>(bytes_[at + 1]) << 8));
    }
    std::uint32_t U32(std::size_t at) const { return static_cast<std::uint32_t>(U16(at)) | (static_cast<std::uint32_t>(U16(at + 2)) << 16); }
    bool Header(bool dll) {
        if (!Fits(0, 64) || U16(0) != 0x5a4d) return false;
        const auto pe = static_cast<std::size_t>(U32(60));
        if (pe < 64 || !Fits(pe, 24) || U32(pe) != 0x00004550 || U16(pe + 4) != 0x014c) return false;
        const auto flags = U16(pe + 22);
        if (!(flags & 0x0002) || static_cast<bool>(flags & 0x2000) != dll) return false;
        sections_ = U16(pe + 6); optional_ = pe + 24; optionalBytes_ = U16(pe + 20);
        if (!sections_ || sections_ > 96 || optionalBytes_ < 104 || !Fits(optional_, optionalBytes_) || U16(optional_) != 0x010b) return false;
        sectionTable_ = optional_ + optionalBytes_;
        if (!Fits(sectionTable_, static_cast<std::size_t>(sections_) * 40)) return false;
        headers_ = U32(optional_ + 60);
        if (headers_ < sectionTable_ + static_cast<std::size_t>(sections_) * 40 || !Fits(0, headers_)) return false;
        for (unsigned i = 0; i < sections_; ++i) {
            const auto section = sectionTable_ + static_cast<std::size_t>(i) * 40;
            if (!Fits(U32(section + 20), U32(section + 16))) return false;
        }
        return true;
    }
    std::optional<std::size_t> Rva(std::uint32_t address, std::size_t count) const {
        if (address < headers_ && count <= headers_ - address && Fits(address, count)) return address;
        for (unsigned i = 0; i < sections_; ++i) {
            const auto section = sectionTable_ + static_cast<std::size_t>(i) * 40;
            const auto start = U32(section + 12), rawBytes = U32(section + 16), raw = U32(section + 20);
            if (address < start) continue;
            const auto delta = static_cast<std::uint64_t>(address) - start;
            if (delta <= rawBytes && count <= rawBytes - delta && delta + raw <= std::numeric_limits<std::size_t>::max()
                && Fits(static_cast<std::size_t>(delta + raw), count)) return static_cast<std::size_t>(delta + raw);
        }
        return {};
    }
    std::optional<std::string> Name(std::uint32_t address) const {
        std::string result;
        for (unsigned i = 0; i < 256; ++i) {
            if (address > std::numeric_limits<std::uint32_t>::max() - i) return {};
            const auto offset = Rva(address + i, 1); if (!offset) return {};
            const char c = static_cast<char>(std::to_integer<unsigned char>(bytes_[*offset]));
            if (!c) return result;
            if (static_cast<unsigned char>(c) < 32 || static_cast<unsigned char>(c) >= 127) return {};
            result.push_back(c);
        }
        return {};
    }
    bool ExecutableRva(std::uint32_t address) const {
        if (!Rva(address, 1)) return false;
        for (unsigned i = 0; i < sections_; ++i) {
            const auto section = sectionTable_ + static_cast<std::size_t>(i) * 40;
            const auto start = U32(section + 12), rawBytes = U32(section + 16);
            if (address >= start && static_cast<std::uint64_t>(address) - start < rawBytes
                && (U32(section + 36) & 0x20000000u)) return true;
        }
        return false;
    }
    bool InitializeExport() const {
        if (U32(optional_ + 92) < 1) return false;
        const auto address = U32(optional_ + 96), size = U32(optional_ + 100);
        if (size < 40) return false;
        const auto directory = Rva(address, 40); if (!directory) return false;
        const auto functions = U32(*directory + 20), names = U32(*directory + 24);
        if (!functions || functions > 65536 || !names || names > 4096) return false;
        const auto functionTable = Rva(U32(*directory + 28), static_cast<std::size_t>(functions) * 4);
        const auto nameTable = Rva(U32(*directory + 32), static_cast<std::size_t>(names) * 4);
        const auto ordinals = Rva(U32(*directory + 36), static_cast<std::size_t>(names) * 2);
        if (!functionTable || !nameTable || !ordinals) return false;
        for (std::size_t i = 0; i < names; ++i) {
            const auto name = Name(U32(*nameTable + i * 4)); if (!name) return false;
            if (*name != "Initialize") continue;
            const auto ordinal = U16(*ordinals + i * 2); if (ordinal >= functions) return false;
            const auto entry = U32(*functionTable + static_cast<std::size_t>(ordinal) * 4);
            // 不接受转发到另一未知 DLL 的 Initialize。
            return entry && !(entry >= address && static_cast<std::uint64_t>(entry) - address < size) && ExecutableRva(entry);
        }
        return false;
    }
    bool CompatibleLuaImports() const {
        if (U32(optional_ + 92) < 2) return true;
        if (optionalBytes_ < 112) return false;
        const auto address = U32(optional_ + 104), size = U32(optional_ + 108);
        if (!address && !size) return true;
        if (!address || size < 20 || size > 1024 * 1024) return false;
        for (std::uint32_t i = 0; i < size / 20 && i < 4096; ++i) {
            if (static_cast<std::uint64_t>(address) + static_cast<std::uint64_t>(i) * 20 > std::numeric_limits<std::uint32_t>::max()) return false;
            const auto offset = Rva(address + i * 20, 20); if (!offset) return false;
            if (!U32(*offset) && !U32(*offset + 4) && !U32(*offset + 8) && !U32(*offset + 12) && !U32(*offset + 16)) return true;
            auto name = Name(U32(*offset + 12)); if (!name || name->empty()) return false;
            for (auto& c : *name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (name->starts_with("lua") && name->ends_with(".dll") && name->size() > 3
                && (*name)[3] >= '0' && (*name)[3] <= '9' && *name != "lua53.dll") return false;
        }
        return false;
    }
private:
    std::span<const std::byte> bytes_;
    std::size_t optional_ = 0, optionalBytes_ = 0, sectionTable_ = 0;
    std::uint32_t headers_ = 0;
    unsigned sections_ = 0;
};

Status CheckRuntimePe(const std::filesystem::path& path, bool dll, bool initialize) {
    const auto name = path.filename().string();
    if (!OrdinaryAbsoluteFile(path))
        return Status::Failure("Map.Test.Runtime", "MAP_TEST_RUNTIME_MISSING", name + " 缺失、不是普通绝对文件或经重解析目录；检查路径：" + IO::PathUtf8(path));
    const auto bytes = IO::ReadFileBytes(path, kPeBudget);
    if (!bytes.Succeeded()) return Status::Failure("Map.Test.Runtime", "MAP_TEST_RUNTIME_PE", "独立运行时文件无法有界读取。");
    PeBytes image(*bytes.value);
    if (!image.Header(dll))
        return Status::Failure("Map.Test.Runtime", "MAP_TEST_RUNTIME_PE", "独立运行时当前支持经典 x86 PE / Lua 5.3 档案；文件架构或 PE 结构不匹配。");
    if (!image.CompatibleLuaImports())
        return Status::Failure("Map.Test.Runtime", "MAP_TEST_RUNTIME_PROFILE", "独立扩展运行时当前采用 x86 Lua 5.3 档案；插件依赖了其他 Lua ABI 或导入表损坏。");
    if (initialize && !image.InitializeExport())
        return Status::Failure("Map.Test.Runtime", "MAP_TEST_RUNTIME_EXPORT", "JASS / Lua 插件未提供有效的 Initialize 导出，不能声称已具备加载链。");
    return Status::Ok();
}
} // namespace

Result<Services::MapTestLaunchPlan> BuildIndependentMapTestLaunchPlan(const Services::MapTestArtifact& artifact,
    const Services::MapTestProgress& progress) {
    if (artifact.enableLuaHotReload && !artifact.useOwnedLuaRuntime)
        return Failure<LaunchPlan>("MAP_TEST_LUA_RELOAD_RUNTIME", "Lua 热重载不能附加到普通 JASS 或外部扩展运行时。");
    if (artifact.useOwnedLuaRuntime)
        return Failure<LaunchPlan>("MAP_TEST_OWN_LUA_RUNTIME_NOT_READY", "自有 Lua 的游戏 native 与主线程入口尚未接通，当前不能启动测试。");
    try {
        if (progress) progress("verify");
        auto requirements = InspectMapRuntime(artifact, progress);
        if (!requirements.Succeeded()) return {std::nullopt, std::move(requirements.diagnostics)};
        if (progress) progress("check-runtime");
        const bool required = !requirements.value->missing.empty();
        auto selected = artifact;
        if (required && selected.runtimeRoot.empty() && !selected.runtimeYdweRoot.empty()) {
            selected.runtimeRoot = Services::MapTestYdweInstallRoot(selected.runtimeYdweRoot);
            selected.runtimeLayout = Services::MapTestRuntimeLayout::YdweTree;
        }
        if (required && selected.runtimeRoot.empty()) {
            const auto reason = std::string("此地图使用所选原生游戏没有提供的接口：")
                + requirements.value->missing.begin()->substr(0, 80);
            return Failure<LaunchPlan>("MAP_TEST_RUNTIME_REQUIRED", reason
                + "。请配置本机 YDWE 目录，或在地图测试设置中选择包含六个 DLL 的独立运行时目录；无需启动 YDWE。原图与当前编辑保持。");
        }
        // 先按脚本所需接口选择目录，再只生成一次计划，避免额外整图读取。
        auto plan = Services::BuildMapTestLaunchPlan(selected);
        if (!plan.Succeeded()) return plan;
        if (!selected.runtimeRoot.empty()) {
            std::size_t index = 0;
            for (const auto& [name, initialize] : {std::pair{"ydbase.dll", false}, std::pair{"lua53.dll", false},
                    std::pair{"ydwar3.dll", false}, std::pair{"yd_loader.dll", false},
                    std::pair{"yd_jass_api.dll", true}, std::pair{"yd_lua_engine.dll", true}}) {
                auto path = selected.runtimeRoot;
                if (selected.runtimeLayout == Services::MapTestRuntimeLayout::YdweTree)
                    path /= index < 3 ? "bin" : "plugin/warcraft3";
                ++index;
                const auto status = CheckRuntimePe(path / name, true, initialize);
                if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
            }
            for (const auto& [file, dll] : {std::pair{selected.runtimeBridge, false}, std::pair{selected.runtimeBootstrap, true},
                    std::pair{selected.gameRoot / "war3.exe", false}, std::pair{selected.gameRoot / "Game.dll", true}}) {
                const auto status = CheckRuntimePe(file, dll, false);
                if (!status.Succeeded()) return {std::nullopt, status.diagnostics};
            }
            plan.diagnostics.push_back({DiagnosticSeverity::Info, "Map.Test.Runtime", "MAP_TEST_RUNTIME_PROFILE",
                "独立 x86 Lua 5.3 扩展运行时预检通过；实际 Game.dll 身份与插件初始化由独立启动器及 bootstrap 回执确认。", std::nullopt});
        }
        return plan;
    } catch (const std::bad_alloc&) {
        return Failure<LaunchPlan>("MAP_TEST_RUNTIME_BUDGET", "测试运行时检查无法取得预算内存，未启动游戏。");
    } catch (const std::exception&) {
        return Failure<LaunchPlan>("MAP_TEST_RUNTIME_PREFLIGHT", "测试运行时只读检查失败，未启动游戏或修改安装配置。");
    }
}
} // namespace War3Map::Script
