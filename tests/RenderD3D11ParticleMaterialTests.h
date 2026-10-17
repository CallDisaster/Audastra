#pragma once
#include "War3MapCore/RenderPluginParticleMaterial.h"

// 原创同帧双draw，唯一材质差是Unfogged；背景由实际模型明确给定。
void TestParticleUnfoggedMaterials(const War3RenderPluginApiV2* api, const War3RenderCreateInfoV2& info) {
    RendererOwner renderer{api,api->createRenderer(&info)};
    Require(renderer.handle != nullptr,"particle material renderer unavailable");
    War3RenderParticleMaterialCapabilitiesV2 capabilities{};
    auto& status = capabilities.base.base.base.base.base.base;
    status.structSize = sizeof(capabilities);
    Require(api->getSkinStatus(renderer.handle,&status) == 0
        && capabilities.particleMaterialFlagsFormat == WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1
        && capabilities.particleMaterialFlagsMask == WAR3_PARTICLE_UNFOGGED,
        "particle material capability not advertised");
    const War3RenderCameraV2 camera{sizeof(camera),0,0,0,3500,-1.5707963f,1.5f};
    Require(api->setCamera(renderer.handle,&camera) == 0,"particle material camera rejected");
    constexpr std::array<int,3> source{192,96,48}, background{11,15,19}, fog{32,64,128};
    ParticleGpuFixture fixture;
    fixture.skin.layer.flags = WAR3_RENDER_MODEL_UNSHADED | WAR3_RENDER_MODEL_UNFOGGED;
    for (auto& color : fixture.colors) for (std::size_t c=0;c<3;++c)
        color.rgb[c] = static_cast<float>(background[c])/255;
    fixture.Bind(1,1,true);
    Require(api->submitParticleModelScene(renderer.handle,&fixture.scene) == 0,"particle material background rejected");
    const auto backdrop = CaptureParticles(renderer,1,1);
    RequireWorldColor(backdrop.pixels,backdrop.receipt.models.frame.view,-500,0,background,1);
    RequireWorldColor(backdrop.pixels,backdrop.receipt.models.frame.view,500,0,background,1);
    std::array<War3RenderParticleVertexV2,12> vertices;
    for (std::size_t i=0;i<6;++i) {
        vertices[i] = vertices[i+6] = fixture.vertices[i];
        vertices[i+6].position[0] += 1000;
    }
    std::array<War3RenderParticleDrawV2,2> draws{{{0,6,0,0},{6,6,0,0}}};
    std::array<std::uint32_t,2> flags{0,WAR3_PARTICLE_UNFOGGED};
    War3RenderEnvironmentV2 environment{sizeof(environment),{0,0,1},{1,1,1},{1,1,1},
        WAR3_RENDER_FOG_NONE,100,200,.01f,{32.0f/255,64.0f/255,128.0f/255},0,0,0,0,0,0,nullptr};
    std::uint64_t content = 1;
    for (std::uint32_t test=0;test<9;++test) {
        const auto filter = test<7 ? test : static_cast<std::uint32_t>(WAR3_PARTICLE_ALPHAKEY);
        const auto alpha = test==7 ? 191.0f/255 : test==8 ? 192.0f/255
            : filter==WAR3_PARTICLE_ALPHAKEY ? 1.0f : .5f;
        for (auto& vertex : vertices) {
            for (std::size_t c=0;c<3;++c) vertex.color[c] = static_cast<float>(source[c])/255;
            vertex.color[3] = alpha;
        }
        for (auto& draw : draws) draw.filter = filter;
        fixture.Bind(1,++content,true);
        War3RenderParticleMaterialUpdateV2 update{fixture.frame,WAR3_RENDER_PARTICLE_MATERIAL_FLAGS_V1,2,flags.data()};
        update.base.structSize = sizeof(update); update.base.vertexCount = 12; update.base.drawCount = 2;
        update.base.vertices = vertices.data(); update.base.draws = draws.data();
        Require(api->updateParticleModelFrame(renderer.handle,&update.base) == 0,"particle material update rejected");
        flags[1] = 0; // 不能在完成帧继续借用宿主标志。
        const auto expected = [&](bool fogged) {
            if (test==7) return background;
            std::array<int,3> result{};
            for (std::size_t c=0;c<3;++c) {
                const auto value = !fogged ? static_cast<float>(source[c])
                    : filter==WAR3_PARTICLE_ADDITIVE || filter==WAR3_PARTICLE_ADDALPHA ? 0.0f
                    : filter==WAR3_PARTICLE_MODULATE ? 255.0f
                    : filter==WAR3_PARTICLE_MODULATE2X ? 127.5f : static_cast<float>(fog[c]);
                const auto mixed = filter==WAR3_PARTICLE_OPAQUE ? value
                    : filter==WAR3_PARTICLE_BLEND || filter==WAR3_PARTICLE_ALPHAKEY ? value*alpha+background[c]*(1-alpha)
                    : filter==WAR3_PARTICLE_ADDITIVE || filter==WAR3_PARTICLE_ADDALPHA ? value*alpha+background[c]
                    : value*background[c]/255*(filter==WAR3_PARTICLE_MODULATE2X ? 2 : 1);
                result[c] = static_cast<int>(std::lround(std::clamp(mixed,0.0f,255.0f)));
            }
            return result;
        };
        for (const auto mode : {WAR3_RENDER_FOG_NONE,WAR3_RENDER_FOG_LINEAR,WAR3_RENDER_FOG_EXP,WAR3_RENDER_FOG_EXP2}) {
            environment.fogMode = mode;
            Require(api->setEnvironment(renderer.handle,&environment) == 0,"particle material environment rejected");
            const auto captured = CaptureParticles(renderer,1,content);
            try {
                RequireWorldColor(captured.pixels,captured.receipt.models.frame.view,-500,0,expected(mode!=WAR3_RENDER_FOG_NONE),2);
                RequireWorldColor(captured.pixels,captured.receipt.models.frame.view,500,0,expected(false),2);
            } catch (...) {
                std::cerr << "particle Unfogged case=" << test << " filter=" << filter << " fog=" << mode << '\n';
                throw;
            }
            Require(captured.receipt.particles.textureUploadCount == 1
                && captured.receipt.particles.dynamicUploadCount == content,
                "particle material change reuploaded textures or changed publication count");
        }
        flags = {WAR3_PARTICLE_UNFOGGED,0};
        update.base.contentVersion = ++content; fixture.pose.base.contentVersion = content;
        Require(api->updateParticleModelFrame(renderer.handle,&update.base) == 0,"particle material swap rejected");
        const auto swapped = CaptureParticles(renderer,1,content);
        RequireWorldColor(swapped.pixels,swapped.receipt.models.frame.view,-500,0,expected(false),2);
        RequireWorldColor(swapped.pixels,swapped.receipt.models.frame.view,500,0,expected(true),2);
        flags[0] = 2;
        update.base.contentVersion = content+1; fixture.pose.base.contentVersion = content+1;
        Require(api->updateParticleModelFrame(renderer.handle,&update.base) != 0
            && CaptureParticles(renderer,1,content).pixels == swapped.pixels,
            "unknown particle material flag replaced completed model/particle pair");
        flags = {0,WAR3_PARTICLE_UNFOGGED};
        auto legacy = update.base; legacy.structSize = sizeof(legacy);
        legacy.contentVersion = ++content; fixture.pose.base.contentVersion = content;
        Require(api->updateParticleModelFrame(renderer.handle,&legacy) == 0,"legacy reset after extended frame rejected");
        const auto reset = CaptureParticles(renderer,1,content);
        RequireWorldColor(reset.pixels,reset.receipt.models.frame.view,-500,0,expected(true),2);
        RequireWorldColor(reset.pixels,reset.receipt.models.frame.view,500,0,expected(true),2);
    }
    std::cout << "particle Unfogged WARP: two independent draws, seven filters, AlphaKey191/192, three fog modes and rejected flags\n";
}
