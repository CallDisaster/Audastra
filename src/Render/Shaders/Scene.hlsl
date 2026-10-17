cbuffer Camera : register(b0) { row_major float4x4 viewProjection; };
struct VertexInput { float3 position : POSITION; float3 color : COLOR; };
struct VertexOutput { float4 position : SV_POSITION; float3 color : COLOR; };
VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.color = input.color;
    return output;
}
float4 PSMain(VertexOutput input) : SV_TARGET { return float4(input.color, 1.0); }

// 场景环境（setEnvironment）：模型光照、距离雾、地表静态阴影。未设置时宿主写入中性默认值。
cbuffer Environment : register(b3) {
    float3 modelTowardLight; // 已归一化
    float fogMode;           // 0 无、1 线性、2 指数、3 指数平方
    float3 modelDiffuse;
    float fogStart;
    float3 modelAmbient;
    float fogEnd;
    float3 fogColor;
    float fogDensity;
    float2 shadowOrigin;     // 世界 xy
    float2 shadowScale;      // 1 / (像素数 × 像素世界尺寸)
    float shadowDarkness;    // 0 = 无阴影图
    float3 environmentPadding;
};
Texture2D<float> shadowTexture : register(t1);
SamplerState shadowSampler : register(s2);

// 雾按视深（裁剪空间 w）计算；返回场景颜色保留的比例（1 = 无雾）。
float FogKeep(float depth) {
    if (fogMode < 0.5) return 1.0;
    if (fogMode < 1.5) return saturate((fogEnd - depth) / max(fogEnd - fogStart, 1e-3));
    float d = depth * fogDensity;
    return fogMode < 2.5 ? saturate(exp(-d)) : saturate(exp(-d * d));
}

Texture2D<float4> terrainTexture : register(t0);
SamplerState terrainSampler : register(s0);
cbuffer TerrainTexture : register(b1) {
    float2 terrainDimensions;
    float2 terrainCells;
    float terrainMaxMip;
    float terrainAlphaBlend; // 1 = 过渡层，输出贴图 alpha 供混合
    float terrainIsTranslucent;
    float terrainTapLimit; // 宿主逐批次明确下发1/4；shader自身仍硬限4tap。
};
struct TerrainInput { float3 position : POSITION; float3 color : COLOR; float2 uv : TEXCOORD; };
struct TerrainOutput {
    float4 position : SV_POSITION; float3 color : COLOR; float2 uv : TEXCOORD0; float3 world : TEXCOORD1;
};
TerrainOutput VSTerrain(TerrainInput input) {
    TerrainOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.color = input.color;
    output.uv = input.uv;
    output.world = float3(input.position.xy, output.position.w);
    return output;
}
float4 SampleTerrainCell(float2 uv, float2 cell, float level) {
    uint width, height, mipCount;
    terrainTexture.GetDimensions((uint)level, width, height, mipCount);
    float2 dimensions = float2(width, height);
    float2 minimum = cell / terrainCells + 0.5 / dimensions;
    float2 maximum = (cell + 1.0) / terrainCells - 0.5 / dimensions;
    return terrainTexture.SampleLevel(terrainSampler, clamp(uv, minimum, maximum), level);
}
float4 SampleTerrainOriginal(float2 uv, float2 cell, float2 dx, float2 dy) {
    float lod = clamp(0.5 * log2(max(max(dot(dx, dx), dot(dy, dy)), 1e-16)), 0.0, terrainMaxMip);
    float lower = floor(lod), upper = min(lower + 1.0, terrainMaxMip);
    return lerp(SampleTerrainCell(uv, cell, lower), SampleTerrainCell(uv, cell, upper), lod - lower);
}
bool TryTerrainFootprint(float2 dx, float2 dy, out float lod, out float2 axis, out float major, out uint taps) {
    // 纯数学资格检查；所有out先赋值，纹理采样仅由调用者的互斥分支执行。
    lod = 0.0;
    axis = float2(1.0, 0.0);
    major = 0.0;
    taps = 1u;
    [branch] if (terrainTapLimit < 1.5 || terrainIsTranslucent > 0.5 || terrainMaxMip <= 0.0)
        return false;
    // 先归一化J再求协方差，避免合法大导数先平方再平方溢出；极端/放大仍走旧表达式。
    float scale = max(max(abs(dx.x), abs(dx.y)), max(abs(dy.x), abs(dy.y)));
    [branch] if (!all(isfinite(dx)) || !all(isfinite(dy)) || scale <= 0.5 || scale > 8.507059173023462e37)
        return false;
    float2 p = dx / scale, q = dy / scale;
    float a = p.x * p.x + q.x * q.x, b = p.x * p.y + q.x * q.y, c = p.y * p.y + q.y * q.y;
    float difference = a - c;
    float eigenvalue = 0.5 * (a + c + sqrt(max(difference * difference + 4.0 * b * b, 0.0)));
    float majorNorm = sqrt(eigenvalue);
    [branch] if (!isfinite(majorNorm) || majorNorm <= 0.0) return false;
    float minorNorm = abs(p.x * q.y - p.y * q.x) / majorNorm;
    [branch] if (!isfinite(minorNorm) || minorNorm <= majorNorm * 0.000244140625 || majorNorm / minorNorm <= 1.25)
        return false;
    float candidateMajor = scale * majorNorm, minor = scale * minorNorm;
    float width = max(max(minor, candidateMajor * 0.25), 1.0);
    [branch] if (!isfinite(candidateMajor) || !isfinite(minor) || !isfinite(width))
        return false;
    uint candidateTaps = min(4u, max(1u, (uint)ceil(candidateMajor / width)));
    [branch] if (candidateTaps == 1u) return false;
    float2 firstAxis = float2(b, eigenvalue - a), secondAxis = float2(eigenvalue - c, b);
    float2 candidateAxis = dot(firstAxis, firstAxis) >= dot(secondAxis, secondAxis) ? firstAxis : secondAxis;
    float axisLengthSquared = dot(candidateAxis, candidateAxis);
    [branch] if (!isfinite(axisLengthSquared) || axisLengthSquared <= 0.0)
        return false;
    axis = candidateAxis * rsqrt(axisLengthSquared);
    lod = clamp(log2(width), 0.0, terrainMaxMip);
    major = candidateMajor;
    taps = candidateTaps;
    return true;
}
float4 SampleTerrainBounded(float2 uv, float2 cell, float2 dx, float2 dy) {
    float lod = 0.0, major = 0.0;
    float2 axis = float2(1.0, 0.0);
    uint taps = 1u;
    bool bounded = TryTerrainFootprint(dx, dy, lod, axis, major, taps);
    float4 result = 0.0;
    // 单出口避免FXC合并早退采样路径；正常K4分支不预采原两fetch。
    [branch] if (bounded) {
        float lower = floor(lod), upper = min(lower + 1.0, terrainMaxMip);
        uint lowerWidth = 0u, lowerHeight = 0u, lowerCount = 0u, upperWidth = 0u, upperHeight = 0u, upperCount = 0u;
        terrainTexture.GetDimensions((uint)lower, lowerWidth, lowerHeight, lowerCount);
        terrainTexture.GetDimensions((uint)upper, upperWidth, upperHeight, upperCount);
        float2 lowerDimensions = float2(lowerWidth, lowerHeight), upperDimensions = float2(upperWidth, upperHeight);
        float2 lowerMinimum = cell / terrainCells + 0.5 / lowerDimensions;
        float2 lowerMaximum = (cell + 1.0) / terrainCells - 0.5 / lowerDimensions;
        float2 upperMinimum = cell / terrainCells + 0.5 / upperDimensions;
        float2 upperMaximum = (cell + 1.0) / terrainCells - 0.5 / upperDimensions;
        float4 accumulated = 0.0;
        [unroll] for (uint index = 0u; index < 4u; ++index) {
            if (index < taps) {
                float2 sampleUv = uv + axis * (((float(index) + 0.5) / float(taps) - 0.5) * major) / terrainDimensions;
                // 所有tap固定中心cell；每级分别夹取，不允许偏移后改选相邻mask/variation。
                float4 low = terrainTexture.SampleLevel(terrainSampler, clamp(sampleUv, lowerMinimum, lowerMaximum), lower);
                float4 high = terrainTexture.SampleLevel(terrainSampler, clamp(sampleUv, upperMinimum, upperMaximum), upper);
                accumulated += lerp(low, high, lod - lower);
            }
        }
        result = accumulated / float(taps);
    } else {
        result = SampleTerrainOriginal(uv, cell, dx, dy);
    }
    return result;
}
float4 PSTerrain(TerrainOutput input) : SV_TARGET {
    // 所有动态分支前取原UV导数，避免clamp或发散tap循环改变足迹。
    float2 dx = ddx(input.uv) * terrainDimensions;
    float2 dy = ddy(input.uv) * terrainDimensions;
    float2 cell = min(floor(saturate(input.uv) * terrainCells), terrainCells - 1.0);
    float4 color = SampleTerrainBounded(input.uv, cell, dx, dy);
    // 基础层不透明；过渡层输出遮罩 alpha，由混合状态叠在基础层上。
    // 顶点颜色是宿主给的光照（未提供光照的宿主为白色，结果不变）。
    float alpha = terrainAlphaBlend > 0.5 ? color.a : 1.0;
    float3 lit = saturate(color.rgb * input.color);
    if (shadowDarkness > 0.0) {
        float2 shadowUv = (input.world.xy - shadowOrigin) * shadowScale;
        // 纹理首行是南端：v 自下而上递增。
        float shade = shadowTexture.SampleLevel(shadowSampler, shadowUv, 0);
        lit *= 1.0 - shadowDarkness * shade;
    }
    return float4(lerp(fogColor, lit, FogKeep(input.world.z)), alpha);
}

// 模型（装饰物/单位静态姿态）：槽 0 每顶点，槽 1 每实例 3×4 变换与颜色。
// 光照 = 环境光 + 直射光 × max(法线·光向, 0)，逐分量夹到 1；来自 setEnvironment。
cbuffer ModelLayer : register(b2) {
    float layerAlpha;
    uint layerFilter; // MDX 过滤模式
    uint layerFlags;  // 1 = 无光照、2 = 不受雾影响
    uint layerTextured;
};
struct ModelInput {
    float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0;
    float4 row0 : TEXCOORD1; float4 row1 : TEXCOORD2; float4 row2 : TEXCOORD3; float3 tint : COLOR;
};
struct ModelOutput {
    float4 position : SV_POSITION; float3 color : COLOR; float2 uv : TEXCOORD0;
    float depth : TEXCOORD1; float opacity : TEXCOORD2; float resolvedLayerAlpha : TEXCOORD3;
    nointerpolation uint hasDynamicLayerAlpha : TEXCOORD4;
};
ModelOutput VSModel(ModelInput input) {
    ModelOutput output;
    float4 local = float4(input.position, 1.0);
    float3 world = float3(dot(input.row0, local), dot(input.row1, local), dot(input.row2, local));
    float3 normal = float3(dot(input.row0.xyz, input.normal), dot(input.row1.xyz, input.normal), dot(input.row2.xyz, input.normal));
    float lengthSquared = dot(normal, normal);
    normal = lengthSquared > 1e-12 ? normal * rsqrt(max(lengthSquared, 1e-12)) : float3(0, 0, 1);
    float3 light = saturate(modelAmbient + modelDiffuse * saturate(dot(normal, modelTowardLight)));
    if (layerFlags & 1) light = float3(1, 1, 1);
    output.position = mul(float4(world, 1.0), viewProjection);
    output.color = input.tint * light;
    output.uv = input.uv;
    output.depth = output.position.w;
    output.opacity = 1.0;
    output.resolvedLayerAlpha = layerAlpha;
    output.hasDynamicLayerAlpha = 0;
    return output;
}
Texture2D<float4> modelTexture : register(t0);
SamplerState modelSampler : register(s1);
float4 PSModel(ModelOutput input) : SV_TARGET {
    float4 texel = layerTextured != 0 ? modelTexture.Sample(modelSampler, input.uv) : float4(1, 1, 1, 1);
    if (input.opacity <= 0.0) discard;
    // 旧路径仍在PS取原uniform，避免把静态alpha经插值改成另一条算术路径。
    float resolvedLayerAlpha = input.hasDynamicLayerAlpha != 0 ? input.resolvedLayerAlpha : layerAlpha;
    // 显式材质预览：已饱和到0的层关闭此draw，旧opaque静态alpha规则不变。
    if (input.hasDynamicLayerAlpha != 0 && resolvedLayerAlpha == 0.0) discard;
    float alpha = texel.a * resolvedLayerAlpha * saturate(input.opacity);
    if (layerFilter == 1 && alpha < 0.75) discard;
    // 不透明与透明测试写入不透明 alpha；混合模式的目标 alpha 由混合状态保持。
    if (layerFilter <= 1) alpha = 1.0;
    float3 color = texel.rgb * input.color;
    // 雾：普通层混向雾色；叠加层淡到 0、调制层淡到中性值，避免雾色被叠加或相乘。
    float keep = (layerFlags & 2) != 0 ? 1.0 : FogKeep(input.depth);
    if (layerFilter == 3 || layerFilter == 4) color *= keep;
    else if (layerFilter == 5) color = lerp(float3(1, 1, 1), color, keep);
    else if (layerFilter == 6) color = lerp(float3(0.5, 0.5, 0.5), color, keep);
    else color = lerp(fogColor, color, keep);
    return float4(color, alpha);
}
