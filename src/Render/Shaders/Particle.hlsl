#include "Scene.hlsl"

// CPU 粒子服务已生成世界空间 billboard/ribbon；GPU 只做投影与明确的材质过滤。
struct ParticleInput { float3 position : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR; };
struct ParticleOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR; float depth : TEXCOORD1; };
ParticleOutput VSParticle(ParticleInput input) {
    ParticleOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.uv = input.uv; output.color = input.color; output.depth = output.position.w;
    return output;
}
float4 PSParticle(ParticleOutput input) : SV_TARGET {
    float4 texel = modelTexture.Sample(modelSampler, input.uv) * input.color;
    // 原版材质 mode 1：GE 192 的 alpha test，并保留源 alpha 参与混合。
    if (layerFilter == 1 && texel.a < (192.0 / 255.0)) discard;
    if (layerFilter == 0) texel.a = 1.0;
    float keep = (layerFlags & 2) != 0 ? 1.0 : FogKeep(input.depth);
    if (layerFilter == 3 || layerFilter == 4) texel.rgb *= keep;
    else if (layerFilter == 5) texel.rgb = lerp(float3(1, 1, 1), texel.rgb, keep);
    else if (layerFilter == 6) texel.rgb = lerp(float3(0.5, 0.5, 0.5), texel.rgb, keep);
    else texel.rgb = lerp(fogColor, texel.rgb, keep);
    return texel;
}
