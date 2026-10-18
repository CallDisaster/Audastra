#include "War3MapCore/Hash.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Services/TerrainMinimapSave.h"

#include <Windows.h>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::Services;
using Bytes = std::vector<std::byte>;

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "导入小地图夹具失败";
        for (const auto& diagnostic : result.diagnostics)
            message += " | " + diagnostic.code + ":" + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        path_ = std::filesystem::temp_directory_path() / ("Audastra-imported-minimap-"
            + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64())
            + "-" + std::to_string(sequence.fetch_add(1)));
        Require(std::filesystem::create_directories(path_), "创建导入小地图临时目录");
    }
    ~TempDirectory() { std::error_code error; std::filesystem::remove_all(path_, error); }
    const std::filesystem::path& Path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
};

Bytes Text(std::string_view value) {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

Bytes ImportedMapBlpTable() {
    // classic war3map.imp v1: flags=13, complete map member path.
    Bytes bytes{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0},
        std::byte{0}, std::byte{0}};
    const auto path = Text("war3mapMap.blp");
    bytes.insert(bytes.end(), std::byte{13});
    bytes.insert(bytes.end(), path.begin(), path.end()); bytes.push_back(std::byte{0});
    return bytes;
}

Bytes BoundedBlp(std::byte marker) {
    return Bytes(32, marker);
}

struct Fixture {
    TempDirectory directory;
    std::filesystem::path source;
    std::filesystem::path target;
    Bytes imp = ImportedMapBlpTable();
    Bytes blp = BoundedBlp(std::byte{0x2a});
    Bytes w3u = Text("original-units");

    std::filesystem::path Write(bool imported, bool includeBlp) {
        std::vector<MapArchiveMember> members{
            {"war3map.w3e", Text("source-terrain")},
            {"war3map.wpm", Text("source-pathing")},
            {"war3map.w3u", w3u},
        };
        if (imported) members.push_back({"war3map.imp", imp});
        if (includeBlp) members.push_back({"war3mapMap.blp", blp});
        source = directory.Path() / (imported ? "imported.w3x" : includeBlp ? "ordinary.w3x" : "missing.w3x");
        Require(WriteVerifiedArchive(source, members).Succeeded(), "写入导入小地图源归档");
        return source;
    }

    void Freeze(MapSaveTask& task) const {
        auto opened = Take(MapArchiveSession::Open(source));
        Require(task.FreezeArchive(opened).Succeeded(), "冻结导入小地图源归档");
        auto name = source.filename(); name.replace_extension("-saved.w3x");
        task.target = directory.Path() / name;
    }
};

void ImportedExistingBlpSuppressesAutomaticGenerationAndPreservesBytes() {
    Fixture fixture; fixture.Write(true, true);
    MapSaveTask task; fixture.Freeze(task);
    const auto replacement = Text("changed-units");
    task.replacements.push_back({"war3map.w3u", replacement});
    Require(!Take(TerrainMinimapSave::RequiresAutomaticGeneration(task)),
        "导入清单声明的MapBLP在w3u净变化时仍应保护作者小地图");
    MapData::MapDataEditSession edits(MapDocument::FromMembers({}));
    const auto prepared = Take(TerrainMinimapSave::PrepareAutomaticOrPreserve(task, edits, {}));
    Require(!prepared.generated, "导入MapBLP保存不得启动自动生成器");

    MapSaveService::Run(task);
    Require(task.succeeded && task.nextSession, "导入MapBLP普通保存必须完成");
    const auto saved = Take(MapArchiveSession::Open(task.target));
    Require(Take(saved.ReadMember("war3mapMap.blp")) == fixture.blp,
        "导入MapBLP保存必须保留原小地图正文");
    Require(Take(saved.ReadMember("war3map.imp")) == fixture.imp,
        "导入清单未变化时必须保留原字节");
    Require(Take(saved.ReadMember("war3map.w3u")) == replacement,
        "普通物编替换仍应写出");
}

void FinalImportedBlpReplacementSuppressesGeneration() {
    Fixture fixture; fixture.Write(false, false);
    MapSaveTask task; fixture.Freeze(task);
    const auto finalImp = ImportedMapBlpTable();
    const auto finalBlp = BoundedBlp(std::byte{0x5c});
    task.replacements = {{"war3map.imp", finalImp}, {"war3mapMap.blp", finalBlp},
        {"war3map.w3u", Text("changed-units")}};
    Require(!Take(TerrainMinimapSave::RequiresAutomaticGeneration(task)),
        "本次最终列表同时提供导入清单和MapBLP时不得再生成小地图");
    MapData::MapDataEditSession edits(MapDocument::FromMembers({}));
    const auto prepared = Take(TerrainMinimapSave::PrepareAutomaticOrPreserve(task, edits, {}));
    Require(!prepared.generated, "最终导入MapBLP替换应直接沿用保存列表");
    MapSaveService::Run(task);
    Require(task.succeeded && task.nextSession, "本次导入MapBLP必须沿原保存链完成");
    const auto saved = Take(MapArchiveSession::Open(task.target));
    Require(Take(saved.ReadMember("war3map.imp")) == finalImp
        && Take(saved.ReadMember("war3mapMap.blp")) == finalBlp,
        "独立Open确认final imp和MapBLP替换正文没有被自动生成覆盖");
}

void OrdinaryExistingBlpWithoutImportStillRequiresGeneration() {
    Fixture fixture; fixture.Write(false, true);
    MapSaveTask task; fixture.Freeze(task);
    task.replacements.push_back({"war3map.w3u", Text("changed-units")});
    Require(Take(TerrainMinimapSave::RequiresAutomaticGeneration(task)),
        "没有导入声明时，已有MapBLP在w3u净变化后仍需自动重建判定");
}

void UnchangedMinimapDoesNotParseUnknownImportTable() {
    Fixture fixture;
    fixture.imp = Bytes{std::byte{99}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}; // 未支持版本，不能当作空清单。
    fixture.Write(true, true);
    MapSaveTask task; fixture.Freeze(task);
    task.replacements.push_back({"unrelated.bin", Text("unrelated edit")});
    Require(!Take(TerrainMinimapSave::RequiresAutomaticGeneration(task)),
        "已有BLP且没有相关净变化时，不得为无关编辑解析未知imp而拒绝保存");
    MapSaveService::Run(task);
    Require(task.succeeded && task.nextSession, "未知imp的无关编辑仍能沿原保存链完成");
    const auto saved = Take(MapArchiveSession::Open(task.target));
    Require(Take(saved.ReadMember("war3map.imp")) == fixture.imp
        && Take(saved.ReadMember("war3mapMap.blp")) == fixture.blp,
        "独立Open确认未知imp和已有BLP原字保留");
}

void FullPathImportFlagsProtectCaseVariants() {
    for (const auto spelling : {std::string_view{"WAR3MAPMAP.BLP"}, std::string_view{"War3MapMap.BlP"}}) {
        Fixture fixture;
        fixture.imp = Bytes{std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0},
            std::byte{1}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{13}};
        const auto path = Text(spelling);
        fixture.imp.insert(fixture.imp.end(), path.begin(), path.end()); fixture.imp.push_back(std::byte{0});
        fixture.Write(true, true);
        MapSaveTask task; fixture.Freeze(task);
        task.replacements.push_back({"war3map.w3u", Text("changed-units")});
        Require(!Take(TerrainMinimapSave::RequiresAutomaticGeneration(task)),
            "flags13完整路径的大小写变体必须指向同一受保护MapBLP");
        MapData::MapDataEditSession edits(MapDocument::FromMembers({}));
        Require(!Take(TerrainMinimapSave::Prepare(task, edits, {}, TerrainMinimapSaveChoice::Automatic)),
            "大小写变体导入声明不得启动自动小地图生成器");
    }
}

} // namespace

int main() {
    try {
        ImportedExistingBlpSuppressesAutomaticGenerationAndPreservesBytes();
        FinalImportedBlpReplacementSuppressesGeneration();
        OrdinaryExistingBlpWithoutImportStillRequiresGeneration();
        UnchangedMinimapDoesNotParseUnknownImportTable();
        FullPathImportFlagsProtectCaseVariants();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
