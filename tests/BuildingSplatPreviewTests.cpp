#include "War3MapCore/Render/BuildingSplatPreview.h"
#include "War3MapCore/Render/TerrainPicking.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Render;
using namespace War3Map::Services;
namespace {
void Require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::shared_ptr<const BuildingSplatCatalog> Catalog() {
    const std::array<std::string_view, 20> names{"Name","Dir","file","BlendMode","Scale","BirthTime","PauseTime","Decay",
        "StartR","StartG","StartB","StartA","MiddleR","MiddleG","MiddleB","MiddleA","EndR","EndG","EndB","EndA"};
    std::string text = "ID;PWXL;N;E\n";
    for (std::size_t i = 0; i < names.size(); ++i) text += "C;X" + std::to_string(i+1) + ";Y1;K\"" + std::string(names[i]) + "\"\n";
    for (std::uint32_t y = 2; y <= 3; ++y) {
        const std::array<std::string_view,20> values{y==2?"HMED":"USMA","ReplaceableTextures/Splats",y==2?"HumanUberSplat":"UndeadUberSplat",
            "0",y==2?"190":"170","1","0","2","255","255","255","0","255","255","255","255","255","255","255","0"};
        for (std::size_t i=0;i<values.size();++i) text += "C;X"+std::to_string(i+1)+";Y"+std::to_string(y)+";K\""+std::string(values[i])+"\"\n";
    }
    text += "E\n"; const auto bytes = std::as_bytes(std::span(text.data(),text.size()));
    auto parsed=ParseBuildingSplatCatalog({{"Splats/UberSplatData.slk",Resources::ResourceLayer::Game,330,"fixture-patch","Splats/UberSplatData.slk"},{bytes.begin(),bytes.end()}});
    Require(parsed.Succeeded(),"fixture catalog invalid"); return *parsed.value;
}
std::shared_ptr<MapData::TerrainGridSnapshot> Terrain() {
    auto grid=std::make_shared<MapData::TerrainGridSnapshot>(); grid->header.width=grid->header.height=9;
    grid->minimum={0,0,0}; grid->maximum={1024,1024,384};
    for (std::uint32_t y=0;y<9;++y) for (std::uint32_t x=0;x<9;++x) {
        MapData::TerrainVertex vertex; vertex.groundHeightRaw=static_cast<std::uint16_t>(8192+128*x+64*y);
        vertex.cliffAndLayerRaw=2; vertex.position={static_cast<float>(128*x),static_cast<float>(128*y),static_cast<float>(32*x+16*y)};
        grid->vertices.push_back(vertex);
    }
    for (std::uint32_t y=0;y<8;++y) for (std::uint32_t x=0;x<8;++x) grid->tiles.push_back({{y*9+x,y*9+x+1,(y+1)*9+x,(y+1)*9+x+1}});
    return grid;
}
std::vector<BuildingSplatPreviewInstance> Inputs(const std::shared_ptr<const BuildingSplatCatalog>& catalog,
    const WorldSelectionVersion& version) {
    std::vector<BuildingSplatUnitInput> units(3);
    for (std::uint32_t i=0;i<units.size();++i) {
        auto& unit=units[i]; unit.version=version; unit.owner={MapData::PlacementFile::Units,i,"hars",static_cast<std::int32_t>(i+1)};
        unit.position={i==0?384.0f:640.0f,i==1?640.0f:384.0f,900}; unit.fields.complete=true;
        unit.fields.uubs=i==1?"USMA":"HMED"; unit.fields.tint={17,51,203};
        for (const auto field:{"uubs","uclr","uclg","uclb"}) unit.fields.origins.push_back({field,"hars","fixture-standard","Units/UnitUI.slk",{}});
    }
    auto page=QueryBuildingSplatFootprints(catalog,{version,"owned-fixture",units},version,catalog->identitySha256);
    Require(page.Succeeded(),"footprint query failed"); std::vector<BuildingSplatPreviewInstance> result;
    for (auto& row:page.value->rows) result.push_back({std::move(row),result.size()+100}); return result;
}
bool Has(const BuildingSplatPreviewFrame& frame, std::string_view code) {
    return std::any_of(frame.issues.begin(),frame.issues.end(),[&](const auto& issue){return issue.code==code;});
}
}
int main() try {
    const auto catalog=Catalog(); const WorldSelectionVersion version{{"fixture-map",11,7,"source-sha"},3,4};
    BuildingSplatPreviewBinding binding{version,catalog->identitySha256,"display-terrain-sha",1};
    std::vector<BuildingSplatPreviewTexture> textures;
    for (const auto& [key,definition]:catalog->definitions) {
        auto image=std::make_shared<ModelTextureRgba>(); image->path=definition.texturePath; image->width=image->height=2;
        image->rgba={255,255,255,0,255,255,255,64,255,255,255,128,255,255,255,255};
        textures.push_back({definition.texturePath,"fixture-patch",key+"-sha",std::move(image)});
    }
    const AnimationSession session{13,5,"owned-resource-source"};
    auto loaded=BuildBuildingSplatPreviewAssets(catalog,textures,session); Require(loaded.Succeeded(),"assets failed");
    const auto& assets=*loaded.value; Require(assets.grids.size()==2 && assets.grids[0].fullWidth==380
        && assets.grids[1].fullWidth==340 && assets.grids[0].vertexCount==16,"table width/grid spacing wrong");
    const auto terrain=Terrain(); auto inputs=Inputs(catalog,version);
    auto built=BuildBuildingSplatPreviewFrame(assets,inputs,binding,terrain,50); Require(built.Succeeded(),"frame failed");
    const auto& first=*built.value;
    Require(first.previewApproximate && first.displayed==3 && first.owners.size()==3 && first.scene->models.size()==3,"input order/run layout lost");
    Require(first.frame->instances[0].instance.model==0 && first.frame->instances[1].instance.model==1
        && first.frame->instances[2].instance.model==2 && first.owners[2].stablePlacementId==102,"owner identity mixed");
    Require(first.scene->geometry.layers[0].filterMode==2 && first.scene->geometry.layers[0].flags==5
        && first.frame->alphas[0]==1 && first.frame->instances[0].instance.color[2]==203.0f/255,"actual tint/alpha mode lost");
    const auto& vertex=assets.scene->vertices[assets.grids[0].firstVertex].vertex;
    const auto ground=SampleTerrainHeight(*terrain,384+vertex.x,384+vertex.y);
    Require(ground && first.frame->groups.front().values[11]==*ground && first.frame->instances[0].instance.transform[11]==0,"source dooZ used or slope lost");
    Require(vertex.u==0 && vertex.v==1 && first.scene->vertices.SharesStorageWith(assets.scene->vertices),"original UV grid not shared");
    auto duplicateTextures=textures; duplicateTextures.push_back(textures.front());
    Require(!BuildBuildingSplatPreviewAssets(catalog,duplicateTextures,session).Succeeded(),"conflicting source accepted");
    auto badTextures=textures; auto badImage=std::make_shared<ModelTextureRgba>(*textures[0].image); badImage->rgba.pop_back(); badTextures[0].image=badImage;
    auto badAssets=BuildBuildingSplatPreviewAssets(catalog,badTextures,session);
    Require(badAssets.Succeeded() && badAssets.value->grids.size()==1 && !badAssets.value->issues.empty(),"bad RGBA silently accepted or cleared other key");
    ++binding.version.document.revision;
    for (auto& input:inputs) input.row.version=binding.version;
    inputs[0].row.footprint->minimum[0]+=16; inputs[0].row.footprint->maximum[0]+=16; inputs[0].row.footprint->color[0]=91;
    auto moved=BuildBuildingSplatPreviewFrame(assets,inputs,binding,terrain,51,{},&first);
    Require(moved.Succeeded() && moved.value->scene==first.scene && moved.value->frame->documentRevision==5
        && moved.value->binding.version.document.revision==8 && moved.value->frame->groups.front().values[11]!=first.frame->groups.front().values[11]
        && moved.value->frame->instances[0].instance.color[0]==91.0f/255,"move reuploaded static scene or mixed revision namespaces");
    auto stale=inputs; stale[0].row.version=version;
    Require(!BuildBuildingSplatPreviewFrame(assets,stale,binding,terrain,52).Succeeded(),"stale row accepted");
    auto missingTint=inputs; missingTint[0].row.origins.pop_back();
    auto missing=BuildBuildingSplatPreviewFrame(assets,missingTint,binding,terrain,52);
    Require(missing.Succeeded() && missing.value->displayed==2 && Has(*missing.value,"BUILDING_SPLAT_DATA_UNAVAILABLE"),"missing tint was filled white");
    auto duplicated=inputs; duplicated[1]=duplicated[0];
    Require(!BuildBuildingSplatPreviewFrame(assets,duplicated,binding,terrain,52).Succeeded(),"duplicate owner accepted");
    const auto one=std::span<const BuildingSplatPreviewInstance>(inputs.data(),1);
    auto bad=std::make_shared<MapData::TerrainGridSnapshot>(*terrain); bad->vertices[20].textureAndFlagsRaw=0x40;
    auto omitted=BuildBuildingSplatPreviewFrame(assets,one,binding,bad,52);
    Require(omitted.Succeeded() && omitted.value->displayed==0 && Has(*omitted.value,"BUILDING_SPLAT_WATER_UNSUPPORTED"),"water silently flattened");
    bad=std::make_shared<MapData::TerrainGridSnapshot>(*terrain); bad->vertices[20].cliffAndLayerRaw=3;
    omitted=BuildBuildingSplatPreviewFrame(assets,one,binding,bad,52);
    Require(omitted.Succeeded() && Has(*omitted.value,"BUILDING_SPLAT_CLIFF_UNSUPPORTED"),"cliff silently flattened");
    bad=std::make_shared<MapData::TerrainGridSnapshot>(*terrain); bad->vertices[20].textureAndFlagsRaw=0x80;
    omitted=BuildBuildingSplatPreviewFrame(assets,one,binding,bad,52);
    Require(omitted.Succeeded() && Has(*omitted.value,"BUILDING_SPLAT_BOUNDARY_UNSUPPORTED"),"boundary silently clamped");
    bad=std::make_shared<MapData::TerrainGridSnapshot>(*terrain); bad->vertices[20].position[2]=std::numeric_limits<float>::quiet_NaN();
    omitted=BuildBuildingSplatPreviewFrame(assets,one,binding,bad,52);
    Require(omitted.Succeeded() && omitted.value->frame->groups.empty() && Has(*omitted.value,"BUILDING_SPLAT_TERRAIN_INVALID"),"invalid terrain produced Z0");
    auto outside=inputs; outside[0].row.footprint->minimum[0]-=400; outside[0].row.footprint->maximum[0]-=400;
    omitted=BuildBuildingSplatPreviewFrame(assets,outside,binding,terrain,52);
    Require(omitted.Succeeded() && omitted.value->displayed==2 && Has(*omitted.value,"BUILDING_SPLAT_OUTSIDE"),"one rejected splat cleared others");
    auto limits=BuildingSplatPreviewLimits{}; limits.maxFrameBytes=1;
    omitted=BuildBuildingSplatPreviewFrame(assets,inputs,binding,terrain,52,limits);
    Require(omitted.Succeeded() && omitted.value->omittedBudget==3 && omitted.value->scene->vertices.empty()
        && omitted.value->frame->instances.empty(),"budget omitted count or empty scene wrong");
    Require(first.frame->groups.front().values[11]==*ground && first.terrain==terrain,"old frame owner mutated");
    std::cout<<"building-splat-preview: three instances/two keys, independent tint, slope and source identity; unsupported surfaces omitted; PreviewApproximate\n";
    return 0;
} catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
