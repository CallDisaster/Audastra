#include "War3MapCore/Render/BuildingSplatPreview.h"
#include "War3MapCore/Render/TerrainPicking.h"
#include "War3RenderCore/Legacy/ModelSkinInput.h"

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
void CheckProjection(const BuildingSplatPreviewFrame& frame,
    std::span<const BuildingSplatPreviewInstance> inputs) {
    AnimatedSceneDescriptor descriptor; descriptor.Refresh(*frame.scene,*frame.frame);
    ModelScene accepted; std::uint64_t bytes=0;
    Require(CopySkinnedModelInput(descriptor.scene,accepted,bytes),"actual renderer rejected projection descriptor");
    for (const auto& owner:frame.owners) {
        const auto& mesh=frame.scene->geometry.meshes[owner.renderedInstance];
        const auto& footprint=*inputs[owner.inputIndex].row.footprint;
        double area=0;
        for (std::uint32_t i=0;i<mesh.indexCount;i+=3) {
            std::array<War3RenderModelVertexV2,3> vertices;
            for (std::size_t c=0;c<3;++c) {
                vertices[c]=frame.scene->geometry.vertices[mesh.baseVertex+frame.scene->geometry.indices[mesh.firstIndex+i+c]];
                const auto& v=vertices[c];
                Require(v.x>=footprint.minimum[0] && v.x<=footprint.maximum[0]
                    && v.y>=footprint.minimum[1] && v.y<=footprint.maximum[1],"projected face exceeds footprint");
                Require(v.u>=0 && v.u<=1 && v.v>=0 && v.v<=1,"projected UV exceeds source");
            }
            // 不只检查顶点：折面内部及边上的采样也必须落在同一真实地形面。
            for (const auto weights:{std::array<double,3>{.2,.3,.5},std::array<double,3>{.5,.5,0},
                std::array<double,3>{0,.5,.5},std::array<double,3>{1,0,0}}) {
                double x=0,y=0,z=0;
                for (std::size_t c=0;c<3;++c) { x+=vertices[c].x*weights[c]; y+=vertices[c].y*weights[c]; z+=vertices[c].z*weights[c]; }
                const auto height=SampleTerrainHeight(*frame.terrain,x,y);
                Require(height && std::abs(z-*height)<.001,"triangle interior crosses terrain fold");
            }
            area+=((vertices[1].x-vertices[0].x)*(vertices[2].y-vertices[0].y)
                -(vertices[1].y-vertices[0].y)*(vertices[2].x-vertices[0].x))*.5;
        }
        Require(std::abs(area-footprint.fullWidth*footprint.fullHeight)<.05,"projection has gaps or overlapping faces");
    }
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
    auto mipTextures=textures; std::uint64_t mipStorageDelta=0;
    for (auto& texture:mipTextures) {
        auto image=std::make_shared<ModelTextureRgba>(*texture.image);
        Image::DecodedImage lower; lower.width=lower.height=1; lower.rgba.assign(4,std::byte{127});
        image->additionalMips.push_back(std::move(lower)); image->mipOrigin=ModelTextureMipOrigin::BlpSource;
        image->mipPolicyVersion=ModelTextureMipPolicyVersion; image->sourceWidth=image->sourceHeight=2;
        image->mipStopLevel=2;
        mipStorageDelta+=2*(ModelTextureStorageBytes(*image)-ModelTextureStorageBytes(*texture.image));
        texture.image=std::move(image);
    }
    const auto mipAssets=BuildBuildingSplatPreviewAssets(catalog,mipTextures,session);
    Require(mipAssets.Succeeded() && mipAssets.value->grids.size()==2 &&
        mipAssets.value->logicalBytes==loaded.value->logicalBytes+mipStorageDelta,
        "splat source and scene copies did not charge the complete owned mip chain");
    for (const auto& image:mipAssets.value->scene->geometry.textures)
        Require(image.additionalMips.size()==1 && image.additionalMips[0].rgba[0]==std::byte{127},
            "splat asset copy lost its original lower source mip");
    BuildingSplatPreviewLimits exactMipBudget; exactMipBudget.maxAssetBytes=mipAssets.value->logicalBytes;
    const auto exactMipAssets=BuildBuildingSplatPreviewAssets(catalog,mipTextures,session,exactMipBudget);
    Require(exactMipAssets.Succeeded() && exactMipAssets.value->grids.size()==2,"exact full-chain splat budget refused");
    --exactMipBudget.maxAssetBytes;
    const auto shortMipAssets=BuildBuildingSplatPreviewAssets(catalog,mipTextures,session,exactMipBudget);
    Require(shortMipAssets.Succeeded() && shortMipAssets.value->grids.size()==1 && !shortMipAssets.value->issues.empty(),
        "one-byte-short splat budget omitted no full-chain asset");
    auto brokenMipTextures=mipTextures;
    auto brokenMip=std::make_shared<ModelTextureRgba>(*mipTextures[0].image); brokenMip->additionalMips[0].width=2;
    brokenMipTextures[0].image=brokenMip;
    const auto brokenMipAssets=BuildBuildingSplatPreviewAssets(catalog,brokenMipTextures,session);
    Require(brokenMipAssets.Succeeded() && brokenMipAssets.value->grids.size()==1 && !brokenMipAssets.value->issues.empty(),
        "splat asset preparation accepted malformed lower-level dimensions");
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
    Require(first.frame->groups.front().values[11]==0 && first.frame->instances[0].instance.transform[11]==0,"source dooZ used");
    CheckProjection(first,inputs);
    auto folded=std::make_shared<MapData::TerrainGridSnapshot>(*terrain);
    for (std::uint32_t y=0;y<9;++y) for (std::uint32_t x=0;x<9;++x) {
        const auto z=(x+y)%2?173.0f:0.0f;
        folded->vertices[y*9+x].position[2]=z;
        folded->vertices[y*9+x].groundHeightRaw=static_cast<std::uint16_t>(8192+z*4);
    }
    auto foldedFrame=BuildBuildingSplatPreviewFrame(assets,inputs,binding,folded,51);
    Require(foldedFrame.Succeeded() && foldedFrame.value->displayed==3,"folded terrain projection rejected");
    CheckProjection(*foldedFrame.value,inputs);
    auto refinedLimits=BuildingSplatPreviewLimits{}; refinedLimits.maximumCellSize=32;
    auto refinedAssets=BuildBuildingSplatPreviewAssets(catalog,textures,session,refinedLimits);
    Require(refinedAssets.Succeeded(),"refined assets failed");
    auto refined=BuildBuildingSplatPreviewFrame(*refinedAssets.value,inputs,binding,folded,52,refinedLimits);
    Require(refined.Succeeded() && refined.value->displayed==3,"refined terrain projection rejected");
    CheckProjection(*refined.value,inputs);
    auto duplicateTextures=textures; duplicateTextures.push_back(textures.front());
    Require(!BuildBuildingSplatPreviewAssets(catalog,duplicateTextures,session).Succeeded(),"conflicting source accepted");
    auto badTextures=textures; auto badImage=std::make_shared<ModelTextureRgba>(*textures[0].image); badImage->rgba.pop_back(); badTextures[0].image=badImage;
    auto badAssets=BuildBuildingSplatPreviewAssets(catalog,badTextures,session);
    Require(badAssets.Succeeded() && badAssets.value->grids.size()==1 && !badAssets.value->issues.empty(),"bad RGBA silently accepted or cleared other key");
    ++binding.version.document.revision;
    for (auto& input:inputs) input.row.version=binding.version;
    inputs[0].row.footprint->minimum[0]+=16; inputs[0].row.footprint->maximum[0]+=16; inputs[0].row.footprint->color[0]=91;
    auto moved=BuildBuildingSplatPreviewFrame(assets,inputs,binding,terrain,51,{},&first);
    Require(moved.Succeeded() && moved.value->scene!=first.scene && moved.value->frame->documentRevision==5
        && moved.value->binding.version.document.revision==8 && moved.value->frame->instances[0].instance.color[0]==91.0f/255,
        "move did not rebuild projected scene or mixed revision namespaces");
    CheckProjection(*moved.value,inputs);
    inputs[0].row.footprint->color[0]=42;
    auto recolored=BuildBuildingSplatPreviewFrame(assets,inputs,binding,terrain,52,{},&*moved.value);
    Require(recolored.Succeeded() && recolored.value->scene==moved.value->scene,"color-only edit rebuilt projected geometry");
    auto replacementTextures=textures;
    auto replacementImage=std::make_shared<ModelTextureRgba>(*textures[0].image);
    replacementImage->rgba[0]=17; replacementTextures[0].image=replacementImage; replacementTextures[0].digest="new-texture-sha";
    auto replacementAssets=BuildBuildingSplatPreviewAssets(catalog,replacementTextures,session);
    Require(replacementAssets.Succeeded(),"replacement texture assets failed");
    auto replaced=BuildBuildingSplatPreviewFrame(*replacementAssets.value,inputs,binding,terrain,53,{},&*recolored.value);
    Require(replaced.Succeeded() && replaced.value->scene!=recolored.value->scene,"old texture reused after source replacement");
    auto shiftedTerrain=std::make_shared<MapData::TerrainGridSnapshot>(*folded);
    shiftedTerrain->header.offset={-256.25f,-128.5f};
    shiftedTerrain->minimum[0]=-256.25f; shiftedTerrain->minimum[1]=-128.5f;
    shiftedTerrain->maximum[0]=767.75f; shiftedTerrain->maximum[1]=895.5f;
    for (auto& point:shiftedTerrain->vertices) {point.position[0]-=256.25f; point.position[1]-=128.5f;}
    auto shiftedInputs=inputs;
    for (auto& input:shiftedInputs) {input.row.footprint->minimum[0]-=256.25f;input.row.footprint->maximum[0]-=256.25f;
        input.row.footprint->minimum[1]-=128.5f;input.row.footprint->maximum[1]-=128.5f;}
    auto shifted=BuildBuildingSplatPreviewFrame(assets,shiftedInputs,binding,shiftedTerrain,54);
    Require(shifted.Succeeded() && shifted.value->displayed==3,"fractional terrain origin rejected");
    CheckProjection(*shifted.value,shiftedInputs);
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
    auto offsetLimits=BuildingSplatPreviewLimits{}; offsetLimits.previewHeightOffset=1;
    Require(!BuildBuildingSplatPreviewFrame(assets,inputs,binding,terrain,53,offsetLimits).Succeeded(),"blanket height offset accepted");
    Require(first.frame->groups.front().values[11]==0 && first.terrain==terrain,"old frame owner mutated");
    std::cout<<"building-splat-preview: three instances/two keys, independent tint, slope and source identity; unsupported surfaces omitted; PreviewApproximate\n";
    return 0;
} catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
