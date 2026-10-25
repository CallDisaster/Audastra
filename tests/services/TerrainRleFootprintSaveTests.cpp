#include "War3MapCore/Hash.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/Services/TerrainPathingSave.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using War3Map::Services::TerrainPathingSave;
using Bytes = std::vector<std::byte>;
constexpr std::size_t kBudget = 8u * 1024u * 1024u;
constexpr std::string_view kTexture = "PathTextures/rle-footprint.tga";

void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
void Check(const Status& result) {
    if (result.Succeeded()) return;
    std::string message;
    for (const auto& diagnostic : result.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
    throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    Check({result.diagnostics}); Require(result.value.has_value(), "missing result"); return std::move(*result.value);
}
template<class T> bool HasCode(const T& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
Bytes Text(std::string_view value) {
    return {reinterpret_cast<const std::byte*>(value.data()), reinterpret_cast<const std::byte*>(value.data()+value.size())};
}
Bytes Slk(const std::vector<std::string>& columns, std::initializer_list<std::vector<std::string>> rows) {
    std::string text = "ID;PWXL;N;E\n";
    const auto append = [&](const auto& cells, std::size_t row) {
        for (std::size_t x=0; x<cells.size(); ++x)
            text += "C;X" + std::to_string(x+1) + ";Y" + std::to_string(row) + ";K\"" + cells[x] + "\"\n";
    };
    append(columns,1); std::size_t row=2;
    for (const auto& cells : rows) append(cells,row++);
    return Text(text + "E\n");
}
Bytes Texture(bool alpha, bool top, bool rle = true) {
    Bytes bytes(18); bytes[2] = rle ? std::byte{10} : std::byte{2};
    bytes[12] = bytes[14] = std::byte{4}; bytes[16] = alpha ? std::byte{32} : std::byte{24};
    bytes[17] = static_cast<std::byte>((top ? 32 : 0) | (alpha ? 8 : 0));
    if (rle) bytes.push_back(std::byte{0x8f}); // 单个16像素重复包跨越全部四行。
    for (unsigned pixel=0; pixel<(rle ? 1u : 16u); ++pixel) {
        bytes.insert(bytes.end(),{std::byte{255},std::byte{},std::byte{255}});
        if (alpha) bytes.push_back(std::byte{});
    }
    return bytes;
}

struct Fixture {
    std::filesystem::path root, game;
    TerrainGridSnapshot original;
    std::vector<MapArchiveMember> members;
    std::unique_ptr<MapArchiveSession> source;
    std::unique_ptr<MapDataEditSession> edits;
    unsigned sequence = 0;

    Fixture(bool alpha, bool top) {
        static std::atomic<unsigned> next{0};
        root = std::filesystem::temp_directory_path() / ("Audastra-rle-footprint-" + std::to_string(GetCurrentProcessId())
            + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(next++));
        Require(std::filesystem::create_directory(root),"private RLE fixture directory");
        game = root / "game"; Require(std::filesystem::create_directory(game),"private RLE game archive directory");
        Take(WriteVerifiedArchive(game / "war3.mpq", std::vector<MapArchiveMember>{{"fixture.txt",Text("private RLE fixture")}}));
        original.header.width = original.header.height = 9; original.header.mainTileset = 'L';
        original.header.groundTilesets = {"Tg01"}; original.header.cliffTilesets = {"Cl01"};
        original.vertices.resize(81);
        for (auto& vertex : original.vertices) {
            vertex.groundHeightRaw = 8192; vertex.waterHeightAndFlagsRaw = 8192 + 48 * 4;
            vertex.textureAndFlagsRaw = 0x40; vertex.cliffAndLayerRaw = 2;
        }
        RefreshTerrainDerived(original,0,0,9,9);
        Bytes pathing = Text("MP3W");
        for (const auto value : {0u,32u,32u}) for (unsigned shift=0; shift<32; shift+=8)
            pathing.push_back(static_cast<std::byte>((value >> shift) & 255));
        pathing.resize(16+32*32,std::byte{0x19});
        // 独立原版字面预期：浅水19、作者保护11；4x4紫色内部35编码ca，叠成db。
        for (std::size_t y=14; y<18; ++y) for (std::size_t x=14; x<18; ++x)
            pathing[16+y*32+x] = std::byte{0xdb};
        MapSettings settings; settings.formatVersion=25; settings.custom.emplace();
        settings.tileset='L'; settings.playableWidth=settings.playableHeight=8; settings.flags|=0x400;
        DoodadPlacements doodads;
        DoodadPlacement placement; placement.rawcode="Doo0"; placement.position={512,512,0};
        placement.scale={1,1,1}; placement.flags=2; placement.creationId=1; doodads.doodads.push_back(placement);
        members = {{"war3map.w3e",Take(SerializeMapTerrain(original))}, {"war3map.wpm",std::move(pathing)},
            {"war3map.w3i",Take(settings.Serialize())}, {"war3map.doo",Take(doodads.Serialize())},
            {"war3mapUnits.doo",Take(UnitPlacements{}.Serialize())},
            {"TerrainArt/Terrain.slk",Slk({"tileID","walkable","flyable","buildable"},{{"Tg01","1","1","1"}})},
            {"TerrainArt/Water.slk",Slk({"waterID","height","impassable"},{{"LSha","0","0"}})},
            {"UI/MiscData.txt",Text("[Terrain]\nMaxHeight=1536\n[Water]\nDeepLevelPathing=64\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")},
            {"UI/WorldEditData.txt",Text("[Other]\nValue=1\n")},
            {"Units/DestructableData.slk",Slk({"destructableID","code","walkable"},{{"Dst0","Dst0","0"}})},
            {"Units/DestructableMetaData.slk",Slk({"ID","field","slk","type","repeat","data","index"},
                {{"dwlk","walkable","DestructableData","bool","0","0","-1"}})},
            {"Doodads/Doodads.slk",Slk({"doodID","code","pathTex","category","fixedRot"},
                {{"Doo0","Doo0",std::string(kTexture),"E","-1"}})},
            {std::string(kTexture),Texture(alpha,top)}, {"war3map.j",Text("// original RLE fixture script\n")},
            {"war3mapMap.tga",Text("preserved fixture minimap")}};
        Open();
    }
    ~Fixture() {
        edits.reset(); source.reset(); std::error_code error; std::filesystem::remove_all(root,error);
    }
    Bytes& Member(std::string_view name) {
        const auto found=std::find_if(members.begin(),members.end(),[&](const auto& value){return value.name==name;});
        Require(found!=members.end(),"fixture member exists"); return found->data;
    }
    void Open() {
        source=std::make_unique<MapArchiveSession>(Take(MapArchiveSession::Open(
            Take(WriteVerifiedArchive(root/("source-"+std::to_string(sequence++)+".w3x"),members)).path)));
        edits=std::make_unique<MapDataEditSession>(MapDocument::FromMembers(members));
        Check(edits->AttachTerrain(Member("war3map.w3e"))); Check(edits->AttachPathing(Member("war3map.wpm")));
    }
    void Paint() {
        TerrainBrush brush; brush.kind=TerrainBrushKind::WaterHeight; brush.shape=TerrainBrushShape::Square;
        brush.radius=8; brush.waterHeight=80; const TerrainStrokePoint point{512,512};
        Take(edits->EditTerrain(brush,std::span(&point,1),false));
    }
};

void OrdinaryRleUsesTheNormalSaveIdentityAndReadback() {
    for (const bool alpha : {false,true}) for (const bool top : {false,true}) {
        Fixture f(alpha,top); f.Paint();
        const auto sourceSha=Take(Sha256File(f.source->SourcePath()));
        const auto owner=Take(TerrainPathingSave::CaptureNative(*f.source,f.original,*f.edits->Terrain(),f.game,kBudget));
        Take(TerrainPathingSave::CaptureNative(*f.source,f.original,*f.edits->Terrain(),f.game,owner.CaptureBudgetBytes()));
        Require(!TerrainPathingSave::CaptureNative(*f.source,f.original,*f.edits->Terrain(),f.game,owner.CaptureBudgetBytes()-1).Succeeded(),
            "ordinary source, parsed work, static layer and decoded RLE share one exact capture budget");
        Require(Take(Sha256File(f.source->SourcePath()))==sourceSha && !std::filesystem::exists(f.root/"candidate.w3x"),
            "a rejected tight budget changes neither source nor candidate publication");
        const auto plan=Take(TerrainPathingSave::PrepareForSave(*f.source,*f.edits,f.game));
        Check(plan.SaveGate()); Require(plan.RebuiltPathing(),"RLE participates in native source-WPM reproduction");
        auto output=f.edits->Replacements(); Check(plan.InstallPathingOutput(*f.edits,output));
        Check(plan.VerifyForSave(*f.edits,f.source->SourceSha256(),output));
        auto saved=Take(MapArchiveSession::Open(Take(f.source->SaveCandidate(f.root/"candidate.w3x",output)).path));
        Check(plan.VerifyReadback(saved)); Check(owner.VerifyReadback(saved,kBudget));
        auto expected=f.Member("war3map.wpm");
        std::fill(expected.begin()+16,expected.end(),std::byte{0x1b});
        for (std::size_t y=14; y<18; ++y) for (std::size_t x=14; x<18; ++x) expected[16+y*32+x]=std::byte{0xdb};
        Require(Take(saved.ReadMember("war3map.wpm"))==expected,"full independent deep-water and footprint WPM bytes");
        auto terrain=f.original; for (auto& vertex : terrain.vertices) vertex.waterHeightAndFlagsRaw=8192+80*4;
        Require(Take(saved.ReadMember("war3map.w3e"))==Take(SerializeMapTerrain(terrain)),"full independent edited W3E bytes");
        for (const auto& member : f.members) if (member.name!="war3map.w3e" && member.name!="war3map.wpm")
            Require(Take(saved.ReadMember(member.name))==member.data,"compressed TGA, DOO and unrelated members stay byte-identical");
        Require(Take(Sha256File(f.source->SourcePath()))==sourceSha,"successful candidate save preserves the source archive");
        // 相同像素改写为type2仍是不同wire来源，不能绕过冻结或独立回读。
        const MemberReplacement equivalent{std::string(kTexture),Texture(alpha,top,false)};
        Require(!owner.VerifyFrozenOutput(std::span(&equivalent,1),{}).Succeeded(),"equivalent re-encoding cannot replace frozen RLE bytes");
        auto changed=Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root/"reencoded.w3x",std::span(&equivalent,1))).path));
        Require(!plan.VerifyReadback(changed).Succeeded(),"readback rejects equivalent pixels with different TGA source bytes");
    }
}

// 特殊地形对象与普通装饰物共用有界 RLE 解码（TerrainSpecialFootprintRleCases.h 覆盖保存与预算阴例）。
void SpecialFootprintsShareTheBoundedRleDecoder() {
    Fixture f(true,true);
    auto placements=Take(ParseDoodadPlacements(f.Member("war3map.doo")));
    placements.doodads.clear(); placements.specialDoodads.push_back({"Doo0",0,{3,3}});
    f.Member("war3map.doo")=Take(placements.Serialize()); f.Open();
    const auto captured=TerrainPathingSave::CaptureNative(*f.source,f.original,f.original,f.game,kBudget);
    const std::string message = "special terrain footprints decode the same bounded RLE TGA: "
        + (captured.diagnostics.empty() ? std::string() : captured.diagnostics.front().code);
    Require(captured.Succeeded() && captured.value, message.c_str());
    Require(!std::filesystem::exists(f.root/"candidate.w3x"),"capturing special RLE publishes no candidate");
}

Bytes BlpTexture(bool transparent) {
    Image::DecodedImage image; image.width = image.height = 4;
    image.rgba.resize(4 * 4 * 4);
    for (std::size_t at = 0; at < image.rgba.size(); at += 4) {
        image.rgba[at] = image.rgba[at + 2] = std::byte{255};
        image.rgba[at + 3] = transparent ? std::byte{} : std::byte{255};
    }
    return Take(Image::EncodeBlpPaletted(image));
}
void BlpFallbackKeepsTgaAbsenceAndExactWinner() {
    constexpr std::string_view blpName = "PathTextures/rle-footprint.blp";
    for (bool transparent : {false,true}) {
        Fixture f(true,true);
        std::erase_if(f.members, [](const auto& member) { return member.name == kTexture; });
        const auto texture = BlpTexture(transparent);
        f.members.push_back({std::string(blpName),texture}); f.Open(); f.Paint();
        const auto plan = Take(TerrainPathingSave::PrepareForSave(*f.source,*f.edits,f.game));
        Check(plan.SaveGate()); Require(plan.RebuiltPathing(), "missing TGA with actual BLP uses the normal native save plan");
        auto output = f.edits->Replacements(); Check(plan.InstallPathingOutput(*f.edits,output));
        Check(plan.VerifyForSave(*f.edits,f.source->SourceSha256(),output));
        auto saved = Take(MapArchiveSession::Open(Take(f.source->SaveCandidate(f.root/"blp.w3x",output)).path));
        Check(plan.VerifyReadback(saved));
        auto expected = f.Member("war3map.wpm"); std::fill(expected.begin()+16,expected.end(),std::byte{0x1b});
        for (std::size_t y=14; y<18; ++y) for (std::size_t x=14; x<18; ++x) expected[16+y*32+x]=std::byte{0xdb};
        Require(Take(saved.ReadMember("war3map.wpm")) == expected && Take(saved.ReadMember(blpName)) == texture,
            "literal complete deep-water WPM retains magenta RGB even when BLP alpha is zero");
        const MemberReplacement newlyPresent{std::string(kTexture),Texture(true,true,false)};
        auto samePixels = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root/"new-tga.w3x",std::span(&newlyPresent,1))).path));
        Require(!plan.VerifyReadback(samePixels).Succeeded(), "higher-priority TGA presence cannot reuse a BLP proof even with identical pixels");
        auto changedTexture = texture; changedTexture.back() ^= std::byte{1};
        const MemberReplacement changed{std::string(blpName),std::move(changedTexture)};
        auto changedArchive = Take(MapArchiveSession::Open(Take(saved.SaveCandidate(f.root/"changed-blp.w3x",std::span(&changed,1))).path));
        Require(!plan.VerifyReadback(changedArchive).Succeeded(), "BLP source wire participates in exact final readback");
        // TGA先在地图和游戏中完整查找，之后才考虑地图BLP，不把后缀顺序当来源顺序。
        Require(std::filesystem::remove(f.game/"war3.mpq"), "remove only this fixture's placeholder archive before replacing it");
        Take(WriteVerifiedArchive(f.game/"war3.mpq",std::vector<MapArchiveMember>{{std::string(kTexture),Texture(true,true,false)}}));
        Require(!plan.VerifyReadback(saved).Succeeded(), "new game TGA changes the winning extension despite the same map BLP");
    }
    Fixture malformed(true,true);
    malformed.members.push_back({std::string(blpName),BlpTexture(false)});
    malformed.Member(kTexture).resize(17); malformed.Open(); malformed.Paint();
    const auto rejected = TerrainPathingSave::PrepareForSave(*malformed.source,*malformed.edits,malformed.game);
    const auto rejectedGate = rejected.value ? rejected.value->SaveGate() : Status{rejected.diagnostics};
    Require(!rejectedGate.Succeeded() && HasCode(rejectedGate,"TERRAIN_UNIFORM_TGA_TRUNCATED"),
        "present malformed TGA is diagnosed by the save gate rather than silently interpreted as an absent extension");
    Fixture preference(false,true);
    preference.Member("Doodads/Doodads.slk") = Slk({"doodID","code","pathTex","category","fixedRot"},
        {{"Doo0","Doo0",std::string(blpName),"E","-1"}});
    std::erase_if(preference.members, [](const auto& member) { return member.name == kTexture; });
    preference.members.push_back({std::string(blpName),BlpTexture(false)});
    auto clearTexture = Texture(false,true,false);
    std::fill(clearTexture.begin()+18,clearTexture.end(),std::byte{});
    Require(std::filesystem::remove(preference.game/"war3.mpq"), "replace only this fixture's private base archive");
    Take(WriteVerifiedArchive(preference.game/"war3.mpq",std::vector<MapArchiveMember>{{std::string(kTexture),std::move(clearTexture)}}));
    auto& oldWpm = preference.Member("war3map.wpm");
    std::fill(oldWpm.begin()+16,oldWpm.end(),std::byte{0x19});
    preference.Open(); preference.Paint();
    const auto preferred = Take(TerrainPathingSave::PrepareForSave(*preference.source,*preference.edits,preference.game));
    auto output = preference.edits->Replacements(); Check(preferred.InstallPathingOutput(*preference.edits,output));
    auto expected = oldWpm; std::fill(expected.begin()+16,expected.end(),std::byte{0x1b});
    const auto result = std::find_if(output.begin(),output.end(),[](const auto& item) { return item.name == "war3map.wpm"; });
    Require(result != output.end() && result->data == expected,
        "even a BLP-declared path tries the complete game TGA winner before the magenta map BLP");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--blp") BlpFallbackKeepsTgaAbsenceAndExactWinner();
        else if (argc == 1) { OrdinaryRleUsesTheNormalSaveIdentityAndReadback(); SpecialFootprintsShareTheBoundedRleDecoder(); }
        else throw std::runtime_error("unknown footprint save test group");
        std::cout<<"Ordinary footprint save contracts passed\n"; return 0;
    } catch (const std::exception& exception) { std::cerr<<exception.what()<<'\n'; return 1; }
}
