#pragma once
#include "ModelAnimationMaterialTextures.h"
#include "War3MapCore/Hash.h"

namespace AnimationMaterialPrefetchTest {
using namespace War3Map;
using namespace War3Map::Render;
using AnimationParticleContinuityTest::Check;
using AnimationParticleContinuityTest::Good;

inline void SingleLegacyResidentTexture(std::vector<std::byte>& bytes) {
    bool textureFound=false,materialFound=false;
    const auto word=[&](std::size_t at) {
        std::uint32_t value=0; std::memcpy(&value,bytes.data()+at,4); return value;
    };
    const auto write=[&](std::size_t at,std::uint32_t value) { std::memcpy(bytes.data()+at,&value,4); };
    // 专门的提升用例需要一张常驻静态贴图。通用原创夹具还带第二张TeamColor TEXS，
    // 先验证并移除这个无关通道，保留本用例原有的贴图数量/索引及全部共享断言。
    for (std::size_t at=4;at+8<=bytes.size();) {
        auto size=word(at+4);
        Check(size<=bytes.size()-at-8,"prefetch fixture chunk is truncated");
        if (!std::memcmp(bytes.data()+at,"TEXS",4)) {
            Check(size==536 && word(at+8)==0 && word(at+8+268)==1,
                "prefetch fixture static/team TEXS layout changed");
            write(at+8+264,3); // 本用例专验legacy→material-state提升；源clamp首次提交另有独立测试。
            bytes.erase(bytes.begin()+static_cast<std::ptrdiff_t>(at+8+268),
                bytes.begin()+static_cast<std::ptrdiff_t>(at+8+size));
            size=268; write(at+4,size); textureFound=true;
        } else if (!std::memcmp(bytes.data()+at,"MTLS",4)) {
            const auto material=at+8,firstLayer=material+20,secondLayer=firstLayer+28;
            Check(size==76 && word(material)==76 && word(material+16)==2 &&
                word(firstLayer)==28 && word(secondLayer)==28 && word(firstLayer+12)==1 && word(secondLayer+12)==0,
                "prefetch fixture static material layout changed");
            write(firstLayer+12,0); materialFound=true;
        }
        at+=8+size;
    }
    Check(textureFound && materialFound,"prefetch fixture has no complete TEXS/MTLS chunks");
}

inline void Run() {
    auto residentModel=AnimationParticleContinuityTest::Model(); SingleLegacyResidentTexture(residentModel);
    const auto materialModel=AnimationMaterialTextureTest::Model();
    const auto residentImage=AnimationParticleContinuityTest::Texture();
    auto firstImage=residentImage,secondImage=residentImage;
    firstImage[158]=std::byte{80}; secondImage[158]=std::byte{160};
    std::string secondLayer="material-textures";
    std::map<std::string,unsigned> reads;
    const auto read=[&](std::string_view path,std::uint64_t) {
        ++reads[std::string(path)];
        const auto& bytes=path=="resident.mdx" ? residentModel : path=="material.mdx" ? materialModel :
            path=="first.tga" ? firstImage : path=="second.tga" ? secondImage : residentImage;
        return Result<AnimationResource>::Success({bytes,path=="second.tga" ? secondLayer : "material-prefetch-fixture",Good(Sha256(bytes))});
    };
    AnimationLimits limits; limits.enableParticles=true; limits.retainUnsupportedStatic=true;
    limits.playbackPolicy=AnimationPlaybackPolicy::InteractivePreview;
    AnimationInstanceSpec added; added.identity={3,7}; added.hostKey="material:formal";
    added.model.modelPath="material.mdx"; added.model.position[0]=200;
    added.sequenceIndex=1; added.elapsedMs=750; added.globalMs=750; added.playing=true;
    auto ghost=added; ghost.identity={77,99}; ghost.model.previewGhost=true;
    auto preview=Good(ModelAnimationService::Create({99,1,"material-prefetch-source"},std::span(&ghost,1),read,limits));
    const auto preloadedModel=preview->Scene()->pickModels.front().source;
    const auto preloaded=Good(preview->CaptureAssetSnapshot()); preview.reset();

    std::array<AnimationInstanceSpec,2> originals;
    for (std::size_t i=0;i<originals.size();++i) {
        auto& spec=originals[i]; spec.identity={i+1,7}; spec.hostKey="resident:"+std::to_string(i);
        spec.model.modelPath="resident.mdx"; spec.model.position[0]=static_cast<float>(i)*100;
        spec.sequenceIndex=1; spec.elapsedMs=300+i*300; spec.globalMs=*spec.elapsedMs; spec.playing=true;
    }
    auto resident=Good(ModelAnimationService::Create({7,1,"material-prefetch-source"},originals,read,limits));
    const auto original=resident->Scene();
    Check(!original->dynamicMaterialStates && original->geometry.models.size()==1 &&
        original->geometry.textures.size()==1 && original->pickModels.front().source->textures.size()==1 &&
        original->pickModels.front().source->textures.front().flags==3 && original->drawLayerBindings.front().staticSamplerFlags==3,
        "prefetch fixture did not start in the old fixed sampler mode");
    const auto originalVertices=original->geometry.vertices.data();
    const auto originalTextures=original->geometry.textures.data();
    auto view=AnimationParticleContinuityTest::View(0);
    auto latest=Good(resident->Prepare(nullptr,view)); view.deltaMs=33;
    latest=Good(resident->Prepare(latest.get(),view));
    Check(latest->particleCheckpoint->instances[0] && latest->particleCheckpoint->instances[1] &&
        latest->particleCheckpoint->instances[0]->state.emitters[0].random!=latest->particleCheckpoint->instances[1]->state.emitters[0].random,
        "material promotion fixture has no independent resident particle RNG");
    const auto catalog=Good(resident->CaptureInstanceCatalog());
    const auto packet=Good(catalog->PrepareStaticAssets(added.model,read,preloaded));
    Check(catalog->Scene()==original && !original->dynamicMaterialStates && original->specs.size()==2 &&
        original->geometry.vertices.data()==originalVertices && original->geometry.textures.data()==originalTextures &&
        original->drawLayerBindings.front().staticSamplerFlags==3,
        "material prefetch mutated resident mode, geometry or sampler metadata");

    AnimationInstanceDelta delta; delta.session={7,2,"material-prefetch-source"}; delta.additions={added};
    AnimationInstanceDeltaTimings timing;
    auto expectedReads=reads;
    const auto verifiedOnce=[&] {
        ++expectedReads["material.mdx"]; ++expectedReads["first.tga"]; ++expectedReads["second.tga"];
        Check(reads==expectedReads && timing.preparedAssetHit && timing.preparedAssetMissReason.empty(),
            "material packet missed, reread residents, or read a new source more than its one required verification");
    };
    const auto plan=Good(catalog->PrepareInstanceCatalog(delta,read,{},&timing,packet)); verifiedOnce();
    auto alternate=delta; alternate.session.documentRevision=3;
    alternate.additions[0].identity={30,7}; alternate.additions[0].hostKey="material:alternate";
    alternate.additions[0].elapsedMs=0; alternate.additions[0].globalMs=0;
    const auto alternatePlan=Good(catalog->PrepareInstanceCatalog(alternate,read,{},&timing,packet)); verifiedOnce();
    // 静态预取与catalog建立期间原世界继续前进；正式消费只能重基到最新接受帧。
    latest=Good(resident->Prepare(latest.get(),view)); view.deltaMs=0;
    const auto first=Good(resident->PrepareInstanceDelta(*latest,*plan,view));
    const auto sibling=Good(resident->PrepareInstanceDelta(*latest,*alternatePlan,view));
    const auto scene=first.service->Scene(),siblingScene=sibling.service->Scene();
    std::cerr << "material-prefetch reads=" << (reads==expectedReads)
        << " mode=" << scene->dynamicMaterialStates << " alphas=" << scene->dynamicMaterialAlphas
        << " models=" << scene->geometry.models.size() << " textures=" << scene->geometry.textures.size()
        << " parsed-old=" << (scene->pickModels[0].source==original->pickModels[0].source)
        << " parsed-new=" << (scene->pickModels[1].source==preloadedModel)
        << " shared-vertices=" << scene->geometry.vertices.SharesStorageWith(siblingScene->geometry.vertices)
        << " shared-textures=" << scene->geometry.textures.SharesStorageWith(siblingScene->geometry.textures)
        << " shared-layers=" << scene->drawLayerBindings.SharesStorageWith(siblingScene->drawLayerBindings)
        << " shared-texture-bindings=" << scene->geometry.modelTextureBindings.SharesStorageWith(siblingScene->geometry.modelTextureBindings)
        << '\n';
    Check(reads==expectedReads && scene->dynamicMaterialStates && scene->dynamicMaterialAlphas &&
        scene->geometry.models.size()==2 && scene->geometry.textures.size()==3 &&
        scene->pickModels[0].source==original->pickModels[0].source && scene->pickModels[1].source==preloadedModel &&
        scene->geometry.vertices.SharesStorageWith(siblingScene->geometry.vertices) &&
        scene->geometry.textures.SharesStorageWith(siblingScene->geometry.textures) &&
        scene->drawLayerBindings.SharesStorageWith(siblingScene->drawLayerBindings) &&
        scene->geometry.modelTextureBindings.SharesStorageWith(siblingScene->geometry.modelTextureBindings),
        "first KMTF append lost cached parsing or rebuilt merged immutable geometry per formal candidate");
    Check(scene->geometry.modelTextureBindings.size()==2 && scene->geometry.modelTextureBindings[0] &&
        scene->geometry.modelTextureBindings[1] && scene->geometry.modelTextureBindings[0]->size()==1 &&
        scene->geometry.modelTextureBindings[1]->size()==2,
        "mode promotion omitted the old or new TEXS-to-resident map");
    const auto& oldBinding=scene->geometry.modelTextureBindings[0]->at(0);
    const auto& newFirst=scene->geometry.modelTextureBindings[1]->at(0);
    const auto& newSecond=scene->geometry.modelTextureBindings[1]->at(1);
    Check(oldBinding.reachable && oldBinding.textureIndex==0 && oldBinding.samplerFlags==3 &&
        newFirst.reachable && newFirst.textureIndex==1 && newFirst.samplerFlags==1 &&
        newSecond.reachable && newSecond.textureIndex==2 && newSecond.samplerFlags==2 &&
        scene->drawLayerBindings.front().staticSamplerFlags==3 && original->drawLayerBindings.front().staticSamplerFlags==3,
        "material append guessed wrap flags, kept fragment-local texture IDs or changed the resident snapshot");
    for (std::size_t i=0;i<originals.size();++i) {
        const auto& pose=first.frame->poses[i]; const auto& checkpoint=latest->particleCheckpoint->instances[i];
        Check(pose.asset==War3RenderCore::AssetIdentity{1,7} && pose.materialStates.count>0 &&
            first.frame->materialStates[pose.materialStates.offset].textureIndex==0 &&
            first.frame->materialStates[pose.materialStates.offset].samplerFlags==3 &&
            first.frame->states[i].elapsedMs==latest->states[i].elapsedMs && first.frame->states[i].globalMs==latest->states[i].globalMs &&
            first.frame->particleCheckpoint->instances[i]->stateHash==checkpoint->stateHash &&
            first.frame->particleCheckpoint->instances[i]->state.emitters[0].random==checkpoint->state.emitters[0].random &&
            first.frame->particleLinks[i].resetReason.empty(),
            "first material mode assigned new compiled ownership or reset a surviving clock/RNG");
    }
    const auto& newPose=first.frame->poses.back(); const auto& siblingPose=sibling.frame->poses.back();
    Check(newPose.asset==War3RenderCore::AssetIdentity{2,7} && siblingPose.asset==newPose.asset &&
        newPose.materialStates.count>0 && siblingPose.materialStates.count==newPose.materialStates.count &&
        first.frame->states.back().elapsedMs==750 && sibling.frame->states.back().elapsedMs==0 &&
        first.frame->materialStates[newPose.materialStates.offset].textureIndex==2 &&
        first.frame->materialStates[newPose.materialStates.offset].samplerFlags==2 &&
        sibling.frame->materialStates[siblingPose.materialStates.offset].textureIndex==1 &&
        sibling.frame->materialStates[siblingPose.materialStates.offset].samplerFlags==1 &&
        newPose.materialStateHash!=siblingPose.materialStateHash,
        "prepared KMTF inherited ghost compiled generation, candidate clock or another instance's sampler");
    const auto beforeFlow=first.frame->particleCheckpoint->instances[0];
    view.deltaMs=33; const auto flowing=Good(first.service->Prepare(first.frame.get(),view));
    view.deltaMs=0; const auto unchangedSibling=Good(sibling.service->Prepare(sibling.frame.get(),view));
    const auto unchangedResident=Good(resident->Prepare(latest.get(),view));
    Check(flowing->states[0].elapsedMs==latest->states[0].elapsedMs+33 && flowing->states.back().elapsedMs==783 &&
        unchangedSibling->states[0].elapsedMs==latest->states[0].elapsedMs && unchangedSibling->states.back().elapsedMs==0 &&
        unchangedSibling->particleCheckpoint->instances[0]->state.emitters[0].random==beforeFlow->state.emitters[0].random &&
        unchangedResident->particleCheckpoint->instances[0]->state.emitters[0].random==beforeFlow->state.emitters[0].random &&
        first.frame->materialStates[newPose.materialStates.offset].textureIndex==2 && reads==expectedReads,
        "advancing one material candidate mutated a sibling/original RNG, clock, old frame or reread an asset");

    // second.tga在静态首键并不使用，仍属于KMTF可达闭包，必须复核其来源和原字节。
    for (const bool mutateBytes:{false,true}) {
        if (mutateBytes) secondImage[158]=std::byte{159}; else secondLayer="shadowing-material-layer";
        const auto rejected=catalog->PrepareInstanceCatalog(delta,read,{},&timing,packet);
        Check(!rejected.Succeeded() && !rejected.diagnostics.empty() &&
            rejected.diagnostics.front().code=="ANIMATION_PREPARED_SOURCE_CHANGED" && !timing.preparedAssetHit &&
            resident->Scene()==original && !original->dynamicMaterialStates,
            "future KMTF texture source/bytes changed without refusing the whole prepared packet");
        secondLayer="material-textures"; secondImage[158]=std::byte{160};
    }
    std::cout<<"material_prefetch_first_promotion=1 source_verify_once=1 parsed_and_static_storage_reused=1 "
        "resident_clock_rng_preserved=1 owned_sampler_asset_bindings=1 CPU_service_only=1\n";
}
} // namespace AnimationMaterialPrefetchTest
