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
    // 原版材质 mode 1：8 位 alpha GE 192 的 alpha test，并保留源 alpha 参与混合。按 8 位取整比较（阈值 191.5/255）：
    // 顶点 alpha 正好 192/255 时，部分显卡的插值会落到该值下方 1 ULP，直接比 192/255 会被误丢弃。
    if (layerFilter == 1 && texel.a * 255.0 < 191.5) discard;
    if (layerFilter == 0) texel.a = 1.0;
    float keep = (layerFlags & 2) != 0 ? 1.0 : FogKeep(input.depth);
    if (layerFilter == 3 || layerFilter == 4) texel.rgb *= keep;
    else if (layerFilter == 5) texel.rgb = lerp(float3(1, 1, 1), texel.rgb, keep);
    else if (layerFilter == 6) texel.rgb = lerp(float3(0.5, 0.5, 0.5), texel.rgb, keep);
    else texel.rgb = lerp(fogColor, texel.rgb, keep);
    return texel;
}
