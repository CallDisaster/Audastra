#include "Scene.hlsl"

// RenderCore 模型局部组矩阵：四列各float3、48字节，已平均，未乘世界变换。
struct SkinGroup { float3 x, y, z, translation; };
StructuredBuffer<SkinGroup> skinGroups : register(t2);
StructuredBuffer<float> skinAlphas : register(t3);
StructuredBuffer<float3> skinColors : register(t4); // 12字节RGB，与alpha的每实例网格索引相同
StructuredBuffer<float> skinLayerAlphas : register(t5); // 绝对材质层alpha，独立于GEOA。
struct MaterialState { uint textureIndex, samplerFlags; float3 uv0, uv1; };
StructuredBuffer<MaterialState> skinMaterialStates : register(t6);
cbuffer SkinMesh : register(b4) {
    uint meshFirstGroup, meshLocalIndex, hasDynamicColors, hasDynamicMaterialAlphas;
    float3 geosetColor; uint modelLocalLayerIndex;
    uint hasDynamicMaterialStates; uint3 materialReserved;
};
struct SkinInput {
    float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0;
    uint group : BLENDINDICES0;
    float4 row0 : TEXCOORD1; float4 row1 : TEXCOORD2; float4 row2 : TEXCOORD3; float3 tint : COLOR;
    uint firstGroup : TEXCOORD4; uint firstAlpha : TEXCOORD5;
    uint firstLayerAlpha : TEXCOORD6;
    uint firstMaterialState : TEXCOORD7;
};
ModelOutput VSSkinnedModel(SkinInput input) {
    SkinGroup group = skinGroups[input.firstGroup + meshFirstGroup + input.group];
    ModelInput base;
    base.position = group.x * input.position.x + group.y * input.position.y + group.z * input.position.z + group.translation;
    base.normal = group.x * input.normal.x + group.y * input.normal.y + group.z * input.normal.z;
    // 通用预览：组矩阵3x3后归一化，再沿用现有实例法线变换；不是逆转置或原版等价。
    float lengthSquared = dot(base.normal, base.normal);
    base.normal = lengthSquared > 1e-12 ? base.normal * rsqrt(max(lengthSquared, 1e-12)) : float3(0, 0, 1);
    base.uv = input.uv; base.row0 = input.row0; base.row1 = input.row1; base.row2 = input.row2; base.tint = input.tint * (hasDynamicColors != 0 ? skinColors[input.firstAlpha + meshLocalIndex] : geosetColor);
    // 动态RGB是绝对GEOA值，不能再乘静态颜色；归一化范围由CPU严格校验。
    ModelOutput output = VSModel(base);
    output.opacity = saturate(skinAlphas[input.firstAlpha + meshLocalIndex]);
    // 已求值绝对层值，不能再乘static layerAlpha；未启用时沿用VSModel的旧静态值。
    if (hasDynamicMaterialAlphas != 0) {
        output.resolvedLayerAlpha = saturate(skinLayerAlphas[input.firstLayerAlpha + modelLocalLayerIndex]);
        output.hasDynamicLayerAlpha = 1;
    }
    if (hasDynamicMaterialStates != 0) {
        MaterialState state = skinMaterialStates[input.firstMaterialState + modelLocalLayerIndex];
        float3 uv = float3(input.uv, 1.0);
        output.uv = float2(dot(state.uv0, uv), dot(state.uv1, uv));
    }
    return output;
}
