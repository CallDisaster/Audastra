#include "TerrainQueryCommand.h"
#include "MapDataFixtures.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Workspace.h"
#include "../src/Core/Mpq/MpqCrypt.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace War3Map;
using Bytes = std::vector<std::byte>;
using Json = nlohmann::json;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T>
T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "原创夹具被拒绝";
        for (const auto& note : result.diagnostics) message += " | " + note.code + ": " + note.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path() / ("Audastra-cli-terrain-query-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())
            + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directory(path_), "创建专用原创夹具目录");
    }
    ~TempDirectory() { std::error_code ignored; std::filesystem::remove_all(path_, ignored); }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

Bytes TerrainBytes(std::uint32_t version = 11) {
    // 19×19 原创平地，偏移不居中；只生成格式字节，解析完全复用生产 ParseMapTerrain。
    MapDataFixtures::Fixture fixture;
    fixture.Raw("W3E!"); fixture.UInt(version); fixture.Byte('L'); fixture.UInt(0);
    fixture.UInt(1); fixture.Raw("Ldrt"); fixture.UInt(1); fixture.Raw("Cl01");
    fixture.UInt(19); fixture.UInt(19); fixture.Float(-1000.5f); fixture.Float(256.25f);
    for (std::uint32_t index = 0; index < 19 * 19; ++index) {
        fixture.Byte(0); fixture.Byte(32); fixture.Byte(0); fixture.Byte(32);
        fixture.Byte(0);
        if (version == 12) fixture.Byte(0);
        fixture.Byte(0); fixture.Byte(2);
    }
    return std::move(fixture.bytes);
}

enum class Listfile { Complete, OmitsTerrain, Absent };

void Put32(Bytes& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.at(at + shift / 8) = static_cast<std::byte>((value >> shift) & 255u);
}

std::filesystem::path WriteArchive(const TempDirectory& directory, std::string_view filename,
    Bytes terrain, Listfile listfile, std::string terrainName = "war3map.w3e",
    std::optional<std::uint32_t> declaredTerrainBytes = std::nullopt) {
    // 沿用既有 MapDocument 无 listfile 夹具的 MPQ v0 布局和生产 MpqCrypt。
    // 不使用 writer 自动补出的 listfile，不复制归档或 W3E 解析实现。
    struct File { std::string name; Bytes bytes; };
    std::vector<File> files{{std::move(terrainName), std::move(terrain)}, {"marker.bin", {std::byte{7}}}};
    if (listfile != Listfile::Absent) {
        const std::string names = "marker.bin\r\n" + (listfile == Listfile::Complete ? files[0].name + "\r\n" : "");
        Bytes bytes;
        for (const unsigned char character : names) bytes.push_back(static_cast<std::byte>(character));
        files.push_back({"(listfile)", std::move(bytes)});
    }
    constexpr std::uint32_t hashCount = 8;
    const auto blockCount = static_cast<std::uint32_t>(files.size());
    std::uint32_t dataBytes = 0;
    for (const auto& file : files) dataBytes += static_cast<std::uint32_t>(file.bytes.size());
    const auto hashOffset = 32u + dataBytes, blockOffset = hashOffset + hashCount * 16;
    Bytes image(blockOffset + blockCount * 16);
    Put32(image, 0, 0x1a51504d); Put32(image, 4, 32); Put32(image, 8, static_cast<std::uint32_t>(image.size()));
    Put32(image, 12, 0x00030000); Put32(image, 16, hashOffset); Put32(image, 20, blockOffset);
    Put32(image, 24, hashCount); Put32(image, 28, blockCount);
    std::fill(image.begin() + hashOffset, image.begin() + blockOffset, std::byte{0xff});
    const auto& crypt = Mpq::MpqCrypt::Instance();
    std::uint32_t position = 32;
    for (std::uint32_t index = 0; index < blockCount; ++index) {
        const auto& file = files[index];
        std::copy(file.bytes.begin(), file.bytes.end(), image.begin() + position);
        auto bucket = crypt.HashString(file.name, Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        while (image[hashOffset + bucket * 16 + 12] != std::byte{0xff}) bucket = (bucket + 1) & (hashCount - 1);
        const auto entry = hashOffset + bucket * 16;
        Put32(image, entry, crypt.HashString(file.name, Mpq::MpqCrypt::HashType::NameA));
        Put32(image, entry + 4, crypt.HashString(file.name, Mpq::MpqCrypt::HashType::NameB));
        Put32(image, entry + 8, 0); Put32(image, entry + 12, index);
        const auto size = static_cast<std::uint32_t>(file.bytes.size());
        Put32(image, blockOffset + index * 16, position); Put32(image, blockOffset + index * 16 + 4, size);
        Put32(image, blockOffset + index * 16 + 8, index == 0 && declaredTerrainBytes ? *declaredTerrainBytes : size);
        Put32(image, blockOffset + index * 16 + 12, Mpq::kFileExists | Mpq::kFileSingleUnit
            | (index == 0 && declaredTerrainBytes ? Mpq::kFileCompress : 0u));
        position += size;
    }
    crypt.EncryptBlock(image.data() + hashOffset, hashCount * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(image.data() + blockOffset, blockCount * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    const auto path = directory.Path() / std::string(filename);
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
    Require(stream.good(), "写入原创小归档夹具");
    return path;
}

struct CommandResult { int exitCode; std::string output, error; };

class CapturedStreams {
public:
    CapturedStreams() : previousOut_(std::cout.rdbuf(out_.rdbuf())), previousErr_(std::cerr.rdbuf(err_.rdbuf())) {}
    ~CapturedStreams() { std::cout.rdbuf(previousOut_); std::cerr.rdbuf(previousErr_); }
    CapturedStreams(const CapturedStreams&) = delete;
    CapturedStreams& operator=(const CapturedStreams&) = delete;
    CommandResult Finish(int exitCode) const { return {exitCode, out_.str(), err_.str()}; }
private:
    std::ostringstream out_, err_;
    std::streambuf* previousOut_;
    std::streambuf* previousErr_;
};

template <typename F>
CommandResult Invoke(F&& command) {
    CapturedStreams captured;
    return captured.Finish(command());
}

Json Document(const CommandResult& result) {
    Require(result.exitCode == 0 && result.error.empty() && !result.output.empty(), "只读命令应完整成功且无错误输出");
    Require(result.output.size() <= 1024 * 1024, "CLI 完整 JSON 不能越过原 1MiB 门");
    return Json::parse(result.output);
}

void Failed(const CommandResult& result, int exitCode, std::string_view diagnostic) {
    Require(result.exitCode == exitCode && result.output.empty(), "失败不能发布部分 JSON 或遗留上次结果");
    Require(result.error.find(diagnostic) != std::string::npos, "失败须保留明确诊断类别");
}

void Common(const Json& document, const std::string& sourceSha, const std::string& terrainSha) {
    Require(document.at("ok") == true && document.at("schema_version") == 1
        && document.at("workspace_kind") == "map-archive-readonly" && document.at("content_scope") == "committed-source"
        && document.at("coordinate_system") == "southwest-origin-x-east-y-north-z-up"
        && document.at("surface_semantics") == "w3e-raw-layer-height-grid"
        && document.at("preview_diagonal") == "northwest-southeast"
        && document.at("is_render_capture") == false && document.at("includes_cliff_ramp_geometry") == false
        && document.at("includes_water_material_offset") == false && document.at("includes_entities") == false
        && document.at("includes_pathing") == false, "保留原始地形的只读身份和输出语义");
    const auto& binding = document.at("terrain_binding");
    Require(binding.at("document_id") == "archive-readonly:" + sourceSha
        && binding.at("document_generation") == "1" && binding.at("revision") == "0"
        && binding.at("source_sha256") == sourceSha && binding.at("terrain_sha256") == terrainSha,
        "每次结果都绑定同一源归档及真实 W3E 成员 SHA");
    const auto& header = document.at("header");
    Require(header.at("vertex_width") == 19 && header.at("vertex_height") == 19
        && header.at("tile_width") == 18 && header.at("tile_height") == 18
        && header.at("tile_size_world") == 128 && header.at("offset_world_xy") == Json::array({-1000.5, 256.25}),
        "origin 必须来自原始 W3E，不能按地图中心或相机推导");
}

void TestKnownMemberQueries() {
    TempDirectory directory;
    const auto terrain = TerrainBytes();
    const auto terrainSha = Take(Sha256(terrain));
    Json referenceHeader;
    for (const auto inventory : {Listfile::Complete, Listfile::OmitsTerrain, Listfile::Absent}) {
        const auto path = WriteArchive(directory, "known-" + std::to_string(static_cast<int>(inventory)) + ".w3x", terrain, inventory);
        const auto sourceSha = Take(Sha256File(path));
        const auto workspace = Take(MpqWorkspace::OpenReadOnly(path));
        Require(workspace.Archive().HasFile("war3map.w3e"), "W3E 必须按真实名字 hash 存在");
        if (inventory != Listfile::Complete)
            Require(std::none_of(workspace.KnownMembers().begin(), workspace.KnownMembers().end(),
                [](const auto& member) { return member.name == "war3map.w3e"; }), "夹具不能把 W3E 偷列进已知清单");
        if (inventory == Listfile::Absent) {
            Require(!workspace.Archive().HasFile("(listfile)"), "无清单反例应物理上没有 listfile");
            const auto editable = MapArchiveSession::Open(path);
            Require(!editable.Succeeded() && !editable.value
                && std::any_of(editable.diagnostics.begin(), editable.diagnostics.end(),
                    [](const auto& note) { return note.code == "LISTFILE_MISSING"; }),
                "已知成员只读成功不能绕过可编辑/保存 inventory 门");
        }
        const auto snapshot = Document(Invoke([&] { return RunTerrainSnapshot(path); }));
        Common(snapshot, sourceSha, terrainSha);
        Require(snapshot.at("header").at("version") == 11 && snapshot.at("header").at("vertex_record_bytes") == 7,
            "保留原 W3E 版本和记录尺寸");
        if (referenceHeader.is_null()) referenceHeader = snapshot.at("header");
        else Require(snapshot.at("header") == referenceHeader, "清单差异不能改变实际 W3E header");
        const auto block = Document(Invoke([&] { return RunTerrainBlock(path, sourceSha, terrainSha, "1", "2", "2", "3"); }));
        Common(block, sourceSha, terrainSha);
        Require(block.at("cells").size() == 6 && block.at("vertices").size() == 12
            && block.at("vertices")[0].at("source_index") == 39
            && block.at("vertices")[0].at("position_world") == Json::array({-872.5, 512.25, 0.0}),
            "区块返回真实原格和世界位置，不从 listfile 构造格子");
        Require(workspace.VerifyUnchanged().Succeeded() && Take(Sha256File(path)) == sourceSha,
            "snapshot/block 查询前后来源字节与真实文件身份保持不变");
    }
    const auto v12 = WriteArchive(directory, "v12.w3x", TerrainBytes(12), Listfile::Absent);
    const auto snapshot = Document(Invoke([&] { return RunTerrainSnapshot(v12); }));
    Common(snapshot, Take(Sha256File(v12)), Take(Sha256(TerrainBytes(12))));
    Require(snapshot.at("header").at("version") == 12 && snapshot.at("header").at("vertex_record_bytes") == 8,
        "已知的 W3E v12 仍按现有生产解析器读取");
}

void TestBindingAndBlockLimits() {
    TempDirectory directory;
    const auto terrain = TerrainBytes();
    const auto terrainSha = Take(Sha256(terrain));
    const auto path = WriteArchive(directory, "limits.w3x", terrain, Listfile::Absent);
    const auto sourceSha = Take(Sha256File(path));
    auto wrongSource = sourceSha, wrongTerrain = terrainSha;
    wrongSource[0] = wrongSource[0] == '0' ? '1' : '0';
    wrongTerrain[0] = wrongTerrain[0] == '0' ? '1' : '0';
    Failed(Invoke([&] { return RunTerrainBlock(path, wrongSource, terrainSha, "0", "0", "1", "1"); }), 1, "TERRAIN_INSPECT_STALE");
    Failed(Invoke([&] { return RunTerrainBlock(path, sourceSha, wrongTerrain, "0", "0", "1", "1"); }), 1, "TERRAIN_INSPECT_STALE");
    Failed(Invoke([&] { return RunTerrainBlock(path, "bad-sha", terrainSha, "0", "0", "1", "1"); }), 1, "TERRAIN_INSPECT_BINDING_INVALID");
    Failed(Invoke([&] { return RunTerrainBlock(path, sourceSha, terrainSha, "18", "0", "1", "1"); }), 1, "TERRAIN_INSPECT_BOUNDS");
    Failed(Invoke([&] { return RunTerrainBlock(path, sourceSha, terrainSha, "0", "0", "0", "1"); }), 1, "TERRAIN_INSPECT_BOUNDS");
    Failed(Invoke([&] { return RunTerrainBlock(path, sourceSha, terrainSha, "4294967295", "0", "1", "1"); }), 1, "TERRAIN_INSPECT_BOUNDS");
    Failed(Invoke([&] { return RunTerrainBlock(path, sourceSha, terrainSha, "-1", "0", "1", "1"); }), 2, "TERRAIN_RECTANGLE_INVALID");
    Failed(Invoke([&] { return RunTerrainBlock(path, sourceSha, terrainSha, "4294967296", "0", "1", "1"); }), 2, "TERRAIN_RECTANGLE_INVALID");
    const auto exact = Document(Invoke([&] { return RunTerrainBlock(path, sourceSha, terrainSha, "0", "0", "16", "16"); }));
    Common(exact, sourceSha, terrainSha);
    Require(exact.at("cells").size() == 256 && exact.at("vertices").size() == 289, "原 256 格门应恰好允许完整结果");
    Failed(Invoke([&] { return RunTerrainBlock(path, sourceSha, terrainSha, "0", "0", "17", "16"); }), 1, "TERRAIN_INSPECT_BUDGET");
    Require(Take(Sha256File(path)) == sourceSha, "失败与边界查询不得写回来源");
}

void TestRejectedMembers() {
    TempDirectory directory;
    const auto rejected = [&](const std::filesystem::path& path, std::string_view code) {
        const auto sourceSha = Take(Sha256File(path));
        Failed(Invoke([&] { return RunTerrainSnapshot(path); }), 1, code);
        Failed(Invoke([&] { return RunTerrainBlock(path, sourceSha, std::string(64, '0'), "0", "0", "1", "1"); }), 1, code);
        Require(Take(Sha256File(path)) == sourceSha, "读取/解析失败也不得写回来源");
    };
    rejected(WriteArchive(directory, "missing.w3x", TerrainBytes(), Listfile::Absent, "other.bin"), "TERRAIN_MEMBER_MISSING");
    rejected(WriteArchive(directory, "empty.w3x", {}, Listfile::Absent), "TERRAIN_TRUNCATED");
    auto badMagic = TerrainBytes(); badMagic[0] = std::byte{'!'};
    rejected(WriteArchive(directory, "bad-magic.w3x", std::move(badMagic), Listfile::Absent), "TERRAIN_MAGIC");
    auto truncated = TerrainBytes(); truncated.pop_back();
    rejected(WriteArchive(directory, "truncated.w3x", std::move(truncated), Listfile::Absent), "TERRAIN_TRUNCATED");
    rejected(WriteArchive(directory, "unknown-version.w3x", TerrainBytes(13), Listfile::Absent), "TERRAIN_VERSION_UNSUPPORTED");
    // 物理只有一字节，声明解压大小64MiB+1；必须在读取/解码前拒绝，不分配大载荷。
    constexpr std::uint32_t maximumTerrainBytes = 64u * 1024u * 1024u;
    Require(MapData::TerrainReadLimits{}.maxBytes == maximumTerrainBytes, "CLI 不得放宽原 64MiB W3E 上限");
    const auto oversized = WriteArchive(directory, "oversized.w3x", {std::byte{0}}, Listfile::Absent,
        "war3map.w3e", maximumTerrainBytes + 1);
    Require(std::filesystem::file_size(oversized) < 1024, "预算反例只用微型合成归档");
    rejected(oversized, "FILE_READ_BUDGET_EXCEEDED");
}
} // namespace

int main() {
    try {
        TestKnownMemberQueries();
        TestBindingAndBlockLimits();
        TestRejectedMembers();
        std::cout << "CLI terrain query: known-name readonly access, source/member binding and unchanged save gates passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "CliTerrainQueryTests: " << error.what() << '\n';
        return 1;
    }
}
