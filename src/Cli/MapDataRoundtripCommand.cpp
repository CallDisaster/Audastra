#include "MapDataRoundtripCommand.h"

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapData/TerrainEditing.h"
#include "War3MapCore/MapData/MapAuxiliary.h"
#include "War3MapCore/MapData/MapPlacements.h"
#include "War3MapCore/MapData/MapSettings.h"

#include <array>
#include <iostream>

namespace {

template <typename T>
bool Check(const War3Map::MapMemberState& member, const std::optional<T>& typed) {
    if (member.state == War3Map::MapDataState::Missing) {
        std::cout << member.memberName << "=missing\n";
        return true;
    }
    if (!typed || member.sources.size() != 1) {
        std::cerr << member.memberName << "：来源不可唯一解析，无法验证写出\n";
        return false;
    }
    const auto serialized = typed->Serialize();
    if (!serialized.Succeeded() || !serialized.value) {
        for (const auto& diagnostic : serialized.diagnostics)
            std::cerr << member.memberName << ' ' << diagnostic.code << ": " << diagnostic.message << '\n';
        return false;
    }
    if (*serialized.value != member.sources.front().data) {
        std::cerr << member.memberName << "：未修改的类型化写出与源字节不同\n";
        return false;
    }
    std::cout << member.memberName << "=byte-identical bytes=" << serialized.value->size() << '\n';
    return true;
}

// 地形不在 MapDocument 中：经归档会话读取，解析后原样写出并逐字节比较。
bool CheckTerrain(const std::filesystem::path& archivePath) {
    auto session = War3Map::MapArchiveSession::Open(archivePath);
    if (!session.Succeeded() || !session.value) {
        std::cerr << "war3map.w3e：归档会话无法打开，无法验证写出\n";
        return false;
    }
    if (!session.value->HasMember("war3map.w3e")) {
        std::cout << "war3map.w3e=missing\n";
        return true;
    }
    const auto bytes = session.value->ReadMember("war3map.w3e");
    const auto parsed = bytes.Succeeded() && bytes.value ? War3Map::MapData::ParseMapTerrain(*bytes.value)
                                                          : War3Map::Result<War3Map::MapData::MapTerrain>{};
    if (!parsed.Succeeded() || !parsed.value) {
        for (const auto& diagnostic : parsed.diagnostics) std::cerr << "war3map.w3e " << diagnostic.code << ": " << diagnostic.message << '\n';
        if (parsed.diagnostics.empty()) std::cerr << "war3map.w3e：无法读取\n";
        return false;
    }
    const auto written = War3Map::MapData::SerializeMapTerrain(*parsed.value->Snapshot());
    if (!written.Succeeded() || !written.value || *written.value != *bytes.value) {
        std::cerr << "war3map.w3e：未修改的写出与源字节不同\n";
        return false;
    }
    std::cout << "war3map.w3e=byte-identical bytes=" << written.value->size() << '\n';
    return true;
}

} // namespace

int RunMapDataRoundtrip(const std::filesystem::path& archivePath) {
    const auto opened = War3Map::MapDocument::OpenReadOnlyArchive(archivePath);
    if (!opened.Succeeded() || !opened.value) {
        for (const auto& diagnostic : opened.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        return 1;
    }
    const auto& data = *opened.value;
    bool passed = true;
    passed = Check(data.Member(War3Map::MapDataKind::Settings), data.Settings()) && passed;
    passed = Check(data.Member(War3Map::MapDataKind::Units), data.Units()) && passed;
    passed = Check(data.Member(War3Map::MapDataKind::Doodads), data.Doodads()) && passed;
    passed = Check(data.Member(War3Map::MapDataKind::Regions), data.Regions()) && passed;
    passed = Check(data.Member(War3Map::MapDataKind::Cameras), data.Cameras()) && passed;
    passed = Check(data.Member(War3Map::MapDataKind::Sounds), data.Sounds()) && passed;
    passed = Check(data.Member(War3Map::MapDataKind::Strings), data.Strings()) && passed;
    passed = CheckTerrain(archivePath) && passed;
    std::cout << "originalEditorValidated=false\nruntimeValidated=false\n";
    return passed ? 0 : 2;
}
