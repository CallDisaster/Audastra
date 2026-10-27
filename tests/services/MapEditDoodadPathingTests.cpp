#include "../AuxiliarySaveFixtures.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapData/PlacementEditing.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include <Windows.h>
#include <algorithm>
#include <iostream>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
using namespace AuxiliarySaveFixtures;
using ByteBuffer = std::vector<std::byte>;

ByteBuffer Slk(const std::vector<std::string>& columns, std::initializer_list<std::vector<std::string>> rows) {
    std::string text="ID;PWXL;N;E\n";
    const auto append=[&](const auto& values, unsigned y) {
        for (unsigned x=0;x<values.size();++x)
            text+="C;X"+std::to_string(x+1)+";Y"+std::to_string(y)+";K\""+values[x]+"\"\n";
    };
    append(columns,1);unsigned y=2;for (const auto& row:rows) append(row,y++);
    return AuxiliarySaveFixtures::Bytes(text+"E\n");
}

void CheckCreate(bool footprint, bool corrupt) {
    const auto root=std::filesystem::temp_directory_path()/("Audastra-doodad-final-"+std::to_string(GetCurrentProcessId())
        +"-"+std::to_string(GetTickCount64())+"-"+std::to_string(footprint)+std::to_string(corrupt));
    Require(std::filesystem::create_directory(root),"own temporary fixture");
    auto inputs=CreateOriginalInputs(root);
    auto members=std::move(inputs.members);
    TerrainGridSnapshot grid;grid.header.width=grid.header.height=9;grid.header.mainTileset='L';
    grid.header.offset={-512,-512};grid.header.groundTilesets={"Tg01"};grid.header.cliffTilesets={"Cl01"};grid.vertices.resize(81);
    for (auto& v:grid.vertices) {v.groundHeightRaw=8192;v.waterHeightAndFlagsRaw=7936;v.cliffAndLayerRaw=2;}
    RefreshTerrainDerived(grid,0,0,9,9);
    MapSettings settings;settings.formatVersion=25;settings.tileset='L';settings.custom.emplace();
    settings.playableWidth=settings.playableHeight=8;settings.cameraBounds={-512,-512,512,512,-512,512,512,-512};
    const auto setting=std::find_if(members.begin(),members.end(),[](const auto& m){return m.name=="war3map.w3i";});
    setting->data=Take(settings.Serialize());
    ByteBuffer wpm=Bytes("MP3W");
    for (const unsigned v:{0u,32u,32u}) for (unsigned shift=0;shift<32;shift+=8) wpm.push_back(std::byte((v>>shift)&255));
    wpm.resize(16+32*32,std::byte{0x40});
    members.push_back({"war3map.w3e",Take(SerializeMapTerrain(grid))});members.push_back({"war3map.wpm",wpm});
    members.push_back({"TerrainArt/Terrain.slk",Slk({"tileID","walkable","flyable","buildable"},{{"Tg01","1","1","1"}})});
    members.push_back({"TerrainArt/Water.slk",Slk({"waterID","height","impassable"},{{"LSha","0","0"}})});
    members.push_back({"UI/MiscData.txt",Bytes("[Water]\nDeepLevelPathing=128\n[CameraMargins]\nLeft=0\nRight=0\nBottom=0\nTop=0\n")});
    members.push_back({"UI/WorldEditData.txt",Bytes("[Other]\nValue=1\n")});
    members.push_back({"Units/DestructableData.slk",Slk({"destructableID","code","walkable","pathTex","fixedRot"},{})});
    members.push_back({"Units/DestructableMetaData.slk",Slk({"ID","field","slk","type","repeat","data","index"},
        {{"bptx","pathTex","DestructableData","pathingTexture","0","0","0"}})});
    const std::string texture="PathTextures/owned-footprint.tga";
    members.push_back({"Doodads/Doodads.slk",Slk({"doodID","code","pathTex","category","fixedRot"},{{"Doo0","Doo0",footprint?texture:"","E","0"}})});
    members.push_back({"Doodads/DoodadMetaData.slk",Slk({"ID","field","slk","type","repeat","index"},{{"dpt0","pathTex","DoodadData","pathingTexture","0","0"}})});
    if (footprint) {
        ByteBuffer image(18+4*4*3);image[2]=std::byte{2};image[12]=image[14]=std::byte{4};image[16]=std::byte{24};
        for (unsigned i=18;i<image.size();i+=3) image[i]=image[i+2]=std::byte{255};
        members.push_back({texture,corrupt?Bytes("broken-present-texture"):image});
    }
    auto gameFiles=GameFiles(JassDeclarations());
    for (const auto& member:members) if (member.name.ends_with(".slk")) gameFiles.push_back({member.name,member.data,false});
    inputs.game=root/"native-game";
    Require(std::filesystem::create_directory(inputs.game),"separate synthetic native resource root");
    Require(Mpq::WriteMpqArchive(inputs.game/"war3.mpq",gameFiles).Succeeded(),"owned complete synthetic static metadata");
    const auto source=Take(WriteVerifiedArchive(root/"native-source.w3x",members));
    MapEditSaveRequest request;request.sourceMap=source.path;request.expectedSourceSha256=source.sha256;
    request.targetCandidate=root/"candidate.w3x";request.toolRoot=inputs.tool;request.gameRoot=inputs.game;
    request.minimapChoice=TerrainMinimapSaveChoice::Preserve;
    const auto opened=Take(MapArchiveSession::Open(source.path));
    MapEditPlacementCreate creation;creation.expectedMemberSha256=Take(Sha256(Take(opened.ReadMember("war3map.doo"))));
    DoodadPlacement placed;placed.rawcode="Doo0";placed.position={0,0,0};placed.scale={1,1,1};placed.flags=2;
    creation.records=std::vector{placed};request.placementEdit=std::move(creation);
    const auto saved=SaveMapEditCandidate(request);
    if (!saved.Succeeded()) {
        for (const auto& d:saved.diagnostics) std::cerr<<d.code<<": "<<d.message<<'\n';
    }
    Require(saved.Succeeded()!=corrupt,"known ordinary doodad reaches final native pathing; malformed footprint rejects");
    Require(std::filesystem::exists(request.targetCandidate)!=corrupt,"publish only qualified output");
    if (corrupt) Require(std::any_of(saved.diagnostics.begin(),saved.diagnostics.end(),[](const auto& d) {
        return d.code=="TERRAIN_UNIFORM_TGA_FORMAT";
    }),"malformed footprint is rejected by the final resource decoder, not the early elevation gate");
    if (!corrupt) {
        const auto candidate=Take(MapArchiveSession::Open(request.targetCandidate));
        const auto doodads=Take(ParseDoodadPlacements(Take(candidate.ReadMember("war3map.doo"))));
        Require(doodads.doodads.size()==1 && doodads.doodads[0].rawcode=="Doo0","real placed member survives independent readback");
        const auto pathing=Take(candidate.ReadMember("war3map.wpm"));
        Require(((std::to_integer<unsigned>(pathing[16+16*32+16])&2)!=0)==footprint,"final native footprint changes center walkability only when present");
        Require((std::to_integer<unsigned>(pathing[16+4*32+4])&2)==0,"distant pathing remains walkable");
        Require(Take(candidate.ReadMember("war3map.w3e"))==Take(opened.ReadMember("war3map.w3e")),"placement does not alter terrain");
    }
    Require(Take(Sha256File(source.path))==source.sha256,"source unchanged on success or failure");
    std::error_code error;std::filesystem::remove_all(root,error);
}
}
int main(int argc,char** argv) {
    try {
        const std::string_view mode=argc==2?argv[1]:"";
        Require(mode=="empty" || mode=="footprint" || mode=="corrupt","choose one bounded fixture");
        CheckCreate(mode!="empty",mode=="corrupt");return 0;
    }
    catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
