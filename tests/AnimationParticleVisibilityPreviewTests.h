#pragma once
#include "War3MapCore/Render/ModelAnimationCapture.h"
#include "AnimationParticleContinuityTests.h"
#include "War3MapCore/Hash.h"
#include <map>

namespace AnimationParticleVisibilityPreviewTest {
using namespace War3Map;
using namespace War3Map::Render;
using AnimationParticleContinuityTest::Check;
using AnimationParticleContinuityTest::Good;
inline constexpr std::string_view RecoveryCode="PARTICLE_VISIBILITY_DUPLICATE_PREVIEW";
struct Fixture {
    std::vector<std::byte> bytes;
    std::size_t trackOffset=0;
};
inline Fixture Model(bool invalidRecoveredTexture=false,bool gameOrder=false) {
    const auto source=AnimationTest::Bytes();MdxTest::Writer writer;writer.Tag("MDLX");
    bool pivots=false;
    for(std::size_t at=4;at+8<=source.size();) {
        std::uint32_t size=0;std::memcpy(&size,source.data()+at+4,4);
        Check(size<=source.size()-at-8,"host visibility fixture chunk is truncated");
        if(!std::memcmp(source.data()+at,"PIVT",4)) {
            writer.Tag("PIVT");writer.U32(size+24);writer.Raw(source.data()+at+8,size);
            for(unsigned i=0;i<2;++i) { writer.F32(static_cast<float>(i)*10);writer.F32(0);writer.F32(0); }
            pivots=true;
        } else writer.Raw(source.data()+at,size+8);
        at+=size+8;
    }
    Check(pivots,"host visibility fixture has no original pivots");
    Fixture fixture;writer.Tag("PRE2");const auto chunk=writer.Begin();
    for(unsigned i=0;i<2;++i) {
        const auto object=writer.Begin(),node=writer.Begin();
        writer.Fixed("Hosted visibility recovery",80);writer.U32(2+i);writer.U32(0);writer.U32(0x1000);writer.EndInclusive(node);
        for(const float value:{0.f,0.f,0.f,0.f,.4f,100.f,0.f,0.f})writer.F32(value);
        writer.U32(1);writer.U32(1);writer.U32(1);writer.U32(0);writer.F32(0);writer.F32(.5f);
        for(unsigned color=0;color<9;++color)writer.F32(1);
        const unsigned char alpha[]{255,255,255};writer.Raw(alpha,3);
        for(unsigned scale=0;scale<3;++scale)writer.F32(1);
        for(unsigned interval=0;interval<4;++interval) { writer.U32(0);writer.U32(0);writer.U32(1); }
        writer.U32(invalidRecoveredTexture && i==0?99u:0u);writer.U32(0);writer.U32(0);writer.U32(0);
        if(i==0) {
            std::vector<std::uint32_t> frames{0,100,100,500,1000};
            if(gameOrder)frames={0,100,100,600,400,1000,2000,3000};
            fixture.trackOffset=writer.bytes.size();writer.Tag("KP2V");
            writer.U32(static_cast<std::uint32_t>(frames.size()));writer.U32(0);writer.U32(UINT32_MAX);
            for(const auto frame:frames) { writer.U32(frame);writer.F32(frame==500 || frame==600?0.f:1.f); }
        }
        writer.EndInclusive(object);
    }
    writer.EndChunk(chunk);fixture.bytes=std::move(writer.bytes);return fixture;
}
inline std::size_t Recoveries(std::span<const Diagnostic> diagnostics) {
    return static_cast<std::size_t>(std::count_if(diagnostics.begin(),diagnostics.end(),[](const auto& value) {
        return value.code==RecoveryCode;
    }));
}
inline void InteractiveWithoutIsolation() {
    const auto fixture=Model(false,true),invalid=Model(true,true);
    const auto texture=AnimationParticleContinuityTest::Texture();
    const auto read=[&](std::string_view path,std::uint64_t) {
        const auto& bytes=path=="ordered.mdx"?fixture.bytes:path=="invalid.mdx"?invalid.bytes:texture;
        return Result<AnimationResource>::Success({bytes,"original-game-order-layer",Good(Sha256(bytes))});
    };
    AnimationInstanceSpec spec;spec.identity={1,7};spec.hostKey="ordered-preview";spec.model.modelPath="ordered.mdx";
    spec.sequenceIndex=0;spec.elapsedMs=0;spec.globalMs=0;spec.playing=true;
    auto limits=InteractiveAnimationPreviewLimits();limits.enableParticles=true;
    Check(!limits.retainUnsupportedStatic,"interactive preview unexpectedly enables invalid-emitter isolation");
    const auto service=Good(ModelAnimationService::Create({7,1,"original-game-order"},std::span(&spec,1),read,limits));
    const auto scene=service->Scene();
    Check(scene->particleInstanceCount==1 && scene->particleCapabilities.size()==1 &&
        scene->particleCapabilities[0].available && scene->particleCapabilities[0].particle2Count==2 &&
        (!scene->particleCapabilities[0].skippedEmitters || scene->particleCapabilities[0].skippedEmitters->empty()),
        "interactive model service lost game-order particles or silently skipped an emitter");
    const auto notice=std::find_if(scene->geometry.warnings.begin(),scene->geometry.warnings.end(),[](const auto& item) {
        return item.code=="PARTICLE_KEYS_GAME_ORDER_PREVIEW";
    });
    Check(notice!=scene->geometry.warnings.end() && notice->origin && notice->origin->offset==fixture.trackOffset,
        "interactive model service lost game-order key provenance");
    AnimationAdvance advance;advance.worldCamera=War3RenderCore::LocalCamera{{0,-100,100},{0,0,1}};
    auto frame=Good(service->Prepare(nullptr,advance));advance.deltaMs=33;
    frame=Good(service->Prepare(frame.get(),advance));
    Check(frame->states[0].elapsedMs==33 && !frame->poses[0].hidden && frame->particleLinks.size()==1 && frame->particleLinks[0].simulated &&
        !frame->particleLinks[0].partial && !frame->particleVertices.empty() &&
        frame->particleCheckpoint->instances[0]->state.emitters.size()==2 &&
        frame->particleCheckpoint->instances[0]->state.emitters[0].spawned>0,
        "interactive service could not advance original game-order particles without isolation");
    spec.model.modelPath="invalid.mdx";
    const auto rejected=ModelAnimationService::Create({7,2,"original-game-order"},std::span(&spec,1),read,limits);
    Check(!rejected.Succeeded() && !rejected.value && !rejected.diagnostics.empty() &&
        rejected.diagnostics.front().code=="PARTICLE_TEXTURE_INVALID",
        "game-order preview weakened material validation or retained only the valid sibling emitter");
    std::cout<<"interactive_game_order_service=1 invalid_material_still_rejected=1 actual_particles_advanced=1 CPU_service_only=1\n";
}
inline void Run() {
    InteractiveWithoutIsolation();
    const auto fixture=Model(),invalidTexture=Model(true);
    const auto digest=Good(Sha256(fixture.bytes)),invalidDigest=Good(Sha256(invalidTexture.bytes));
    const auto image=AnimationParticleContinuityTest::Texture();
    auto truncated=fixture.bytes;truncated.pop_back();
    std::map<std::string,unsigned> reads;
    const auto read=[&](std::string_view path,std::uint64_t) {
        ++reads[std::string(path)];
        const auto& bytes=path=="visibility.mdx"?fixture.bytes:path=="material-invalid.mdx"?invalidTexture.bytes:
            path=="truncated.mdx"?truncated:image;
        return Result<AnimationResource>::Success({bytes,"original-visibility-layer",Good(Sha256(bytes))});
    };
    std::array<AnimationInstanceSpec,2> specs;
    for(std::size_t i=0;i<specs.size();++i) {
        specs[i].identity={i+1,7};specs[i].hostKey="visibility:"+std::to_string(i);
        specs[i].model.modelPath="visibility.mdx";specs[i].model.position[0]=static_cast<float>(i)*200;
        specs[i].sequenceIndex=0;specs[i].elapsedMs=0;specs[i].globalMs=0;
    }
    AnimationLimits limits;limits.enableParticles=true;
    const auto strict=ModelAnimationService::Create({7,1,"visibility-source"},specs,read,limits);
    Check(!strict.Succeeded() && !strict.diagnostics.empty() && strict.diagnostics.front().code=="PARTICLE_KEYS_INVALID",
        "formal strict service accepted the repeated visibility keys");
    limits.retainUnsupportedStatic=true;reads.clear();
    const auto service=Good(ModelAnimationService::Create({7,1,"visibility-source"},specs,read,limits));
    const auto scene=service->Scene();
    Check(scene->models.size()==1 && scene->particleInstanceCount==2 && reads["visibility.mdx"]==1 &&
        Recoveries(scene->geometry.warnings)==1,"shared asset duplicated the recovery warning or did not retain both particle instances");
    const auto diagnostic=std::find_if(scene->geometry.warnings.begin(),scene->geometry.warnings.end(),[](const auto& value) {
        return value.code==RecoveryCode;
    });
    Check(diagnostic!=scene->geometry.warnings.end() && diagnostic->severity==DiagnosticSeverity::Warning && diagnostic->origin &&
        diagnostic->origin->source==std::filesystem::path("visibility.mdx") && diagnostic->origin->fingerprint==digest &&
        diagnostic->origin->offset==fixture.trackOffset && diagnostic->origin->length==4 &&
        diagnostic->message.find("visibility-source|original-visibility-layer")!=std::string::npos &&
        !std::memcmp(fixture.bytes.data()+diagnostic->origin->offset,"KP2V",4),
        "formal recovery diagnostic lost actual model path, SHA, source layer or KP2V byte offset");
    for(const auto& capability:scene->particleCapabilities)Check(capability.available && capability.particle2Count==2 &&
        (!capability.skippedEmitters || capability.skippedEmitters->empty()),"recovery was counted as a skipped emitter");
    const auto warningCount=scene->geometry.warnings.size();
    const auto readSnapshot=reads;
    const auto prepare=[&](const AnimationFrame* previous,const AnimationAdvance& advance) {
        auto result=service->Prepare(previous,advance);
        Check(Recoveries(result.diagnostics)==0 && scene->geometry.warnings.size()==warningCount &&
            Recoveries(scene->geometry.warnings)==1,"Prepare regenerated an asset recovery diagnostic");
        return Good(std::move(result));
    };
    AnimationAdvance command;command.worldCamera=War3RenderCore::LocalCamera{{0,-100,100},{0,0,1}};
    auto frame=prepare(nullptr,command);Check(frame->particleVertices.empty(),"zero-time visibility fixture already emitted");
    const auto captureJson=AnimationReceiptJson(*scene,*frame,{},"visibility",std::string(64,'a'));
    Check(captureJson.find("\"source\":\"visibility.mdx\"")!=std::string::npos &&
        captureJson.find("\"offset\":\""+std::to_string(fixture.trackOffset)+"\"")!=std::string::npos &&
        captureJson.find("\"length\":\"4\"")!=std::string::npos &&
        captureJson.find("\"fingerprint\":\""+digest+"\"")!=std::string::npos &&
        captureJson.find("visibility-source|original-visibility-layer")!=std::string::npos,
        "completed capture diagnostic serialization lost the actual restored source byte span or SHA");
    AnimationEdit firstEdit;firstEdit.identity=specs[0].identity;firstEdit.elapsedMs=150;firstEdit.globalMs=150;
    AnimationEdit secondEdit;secondEdit.identity=specs[1].identity;secondEdit.elapsedMs=200;secondEdit.globalMs=200;
    command.edits={firstEdit,secondEdit};frame=prepare(frame.get(),command);
    Check(frame->particleLinks.size()==2 && frame->particleLinks[0].simulated && frame->particleLinks[1].simulated &&
        frame->particleLinks[0].particleCount>0 && frame->particleLinks[1].particleCount>0 && !frame->particleVertices.empty() &&
        frame->states[0].elapsedMs==150 && frame->states[1].elapsedMs==200 &&
        frame->particleCheckpoint->instances[0]->state.emitters.size()==2 &&
        frame->particleCheckpoint->instances[1]->state.emitters.size()==2 &&
        frame->particleCheckpoint->instances[0]->state.emitters[0].spawned>0 &&
        frame->particleCheckpoint->instances[1]->state.emitters[0].spawned>0,
        "restored source emitter did not reach real particle simulation for both independent clocks");
    const auto paused=prepare(frame.get(),{});
    Check(paused->particleLinks[0].stateHash==frame->particleLinks[0].stateHash &&
        paused->particleLinks[1].stateHash==frame->particleLinks[1].stateHash &&
        paused->particleLinks[0].geometryHash==frame->particleLinks[0].geometryHash,
        "pausing restored visibility changed particle state or geometry");
    firstEdit.elapsedMs=170;firstEdit.globalMs=170;command.edits={firstEdit};
    const auto advanced=prepare(paused.get(),command);
    Check(advanced->states[0].elapsedMs==170 && advanced->states[1].elapsedMs==200 &&
        advanced->particleLinks[1].stateHash==paused->particleLinks[1].stateHash &&
        advanced->particleLinks[1].geometryHash==paused->particleLinks[1].geometryHash && reads==readSnapshot,
        "restored emitter advancement coupled instances or reread immutable assets");

    auto isolatedSpecs=specs;for(auto& spec:isolatedSpecs)spec.model.modelPath="material-invalid.mdx";
    const auto isolated=Good(ModelAnimationService::Create({7,2,"visibility-source"},isolatedSpecs,read,limits));
    const auto isolatedScene=isolated->Scene();
    Check(Recoveries(isolatedScene->geometry.warnings)==0,"material-rejected emitter kept its visibility recovery claim");
    const auto& issues=isolatedScene->particleCapabilities[0].skippedEmitters;
    Check(issues && issues->size()==1 && issues==isolatedScene->particleCapabilities[1].skippedEmitters &&
        issues->front().sourceIndex==0 && issues->front().nodeId==2u && issues->front().code=="PARTICLE_TEXTURE_INVALID",
        "material isolation lost the exact recovered emitter source identity");
    command.edits={firstEdit,secondEdit};const auto remaining=Good(isolated->Prepare(nullptr,command));
    for(std::size_t i=0;i<specs.size();++i)Check(remaining->particleLinks[i].simulated && remaining->particleLinks[i].partial &&
        remaining->particleLinks[i].particleCount>0 && remaining->particleCheckpoint->instances[i]->state.emitters.size()==1 &&
        remaining->particleCheckpoint->instances[i]->state.emitters[0].spawned>0,
        "material isolation hid the valid sibling or kept the invalid recovered emitter in simulation");
    Check(!remaining->particleVertices.empty() && Good(Sha256(fixture.bytes))==digest &&
        Good(Sha256(invalidTexture.bytes))==invalidDigest,"host recovery changed immutable model bytes or omitted surviving geometry");
    auto brokenSpecs=specs;for(auto& spec:brokenSpecs)spec.model.modelPath="truncated.mdx";
    const auto broken=ModelAnimationService::Create({7,3,"visibility-source"},brokenSpecs,read,limits);
    Check(!broken.Succeeded() && !broken.value,"structural truncation became a recovered formal model");
    std::cout<<"host_visibility_preview_shared_warning=1 source_sha_offset_bound=1 restored_emitter_simulates=1 "
        "independent_clocks_pause=1 material_rejection_revokes_recovery=1 CPU_service_only=1\n";
}
} // namespace AnimationParticleVisibilityPreviewTest
