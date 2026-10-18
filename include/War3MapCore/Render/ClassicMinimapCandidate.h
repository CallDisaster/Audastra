#pragma once

#include "War3MapCore/Image/BlpJpegEncode.h"
#include "War3MapCore/Render/TerrainMinimapCandidate.h"
#include "War3MapCore/Render/TerrainMinimapPostprocess.h"
#include "War3MapCore/Render/TerrainMinimapRepresentativeColor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace War3Map::Render {

enum class ClassicMinimapMemberState { Unknown, Missing, Empty };

// 0x08 对应对象管理器尚未由独立原版对照确定。第一版只接收 caller 声明的
// Missing/Empty 一致性字段，不绘制对象；Unknown、非零成员或不一致身份 fail-closed。
// 这些字段不是来源读取证明，也不构成 native/editor/game 资格；真实 archive/edits
// 捕获工厂尚未接入，调用者不能以此宣称已证明对象层为空。
struct ClassicMinimapObjectDomain {
    static constexpr const char* declarationPolicy = "caller-declared-v1";
    std::string sourceSha256;
    std::string documentId;
    std::uint64_t documentGeneration = 0;
    std::uint64_t revision = 0;
    std::uint64_t terrainSourceRevision = 0;
    ClassicMinimapMemberState unitsState = ClassicMinimapMemberState::Unknown;
    ClassicMinimapMemberState doodadsState = ClassicMinimapMemberState::Unknown;
    std::string unitsMemberSha256; // Missing时必须为空；Empty时必须为SHA256(empty)
    std::string doodadsMemberSha256; // Missing时必须为空；Empty时必须为SHA256(empty)
    std::size_t unitsMemberBytes = 0;
    std::size_t doodadsMemberBytes = 0;
};

struct ClassicMinimapCandidateInput {
    // TerrainMinimapAssets 的同一次冻结输入；仅 FullW3eAlgorithmCandidate 消费。
    TerrainMinimapInput terrain;
    // 不允许从裸bool推断对象为空；必须带当前来源/会话/编辑代际及声明状态。
    ClassicMinimapObjectDomain objectDomain;
    // 当前 MapDataEditSession 的 source revision；必须与对象域相同。
    std::uint64_t terrainSourceRevision = 0;
    TerrainMinimapPostprocessSettings postprocess;
};

struct ClassicMinimapCandidateLimits {
    // 输入capacity、解析网格、原图copy、postprocess与JPEG工作缓冲的有界载荷估算。
    // 不含allocator元数据、shared_ptr控制块、CNG/provider内部系统内存或进程RSS。
    std::size_t maxWorkingBytes = 128u * 1024u * 1024u;
    std::size_t maxW3eBytes = 4u * 1024u * 1024u;
    std::size_t maxTiles = 65536;
    std::size_t maxRawRgbaBytes = 64u * 1024u * 1024u;
    std::size_t maxBlpBytes = 1024u * 1024u;
    std::size_t maxSourceTextBytes = 64u * 1024u;
};

struct ClassicMinimapManifest {
    static constexpr std::uint32_t algorithmVersion = 1;
    static constexpr const char* rangePolicy = "FullW3eAlgorithmCandidate-v1";
    static constexpr const char* groundPolicy = "sw-ground-atlas-representative-v1";
    static constexpr const char* boundaryPolicy = "boundary-a230-black-truncate-v1";
    static constexpr const char* objectDomainDeclaration = "caller-declared-v1/native=false";
    static constexpr const char* encodingPolicy = "blp1-jpeg-q40-single-mip-v1";
    std::string sourceSha256;
    std::string terrainSha256;
    std::string objectDomainSha256;
    std::string rawRgbaSha256;
    std::string preNoiseRgbaSha256;
    std::string finalRgbaSha256;
    std::string blpSha256;
    TerrainMinimapPostprocessFacts postprocessFacts;
    std::string noiseSamplesSha256;
    std::uint32_t sourceWidth = 0, sourceHeight = 0;
    std::size_t sourceTiles = 0;
    std::array<std::uint32_t, 4> sourceRectangle{};
    std::size_t boundaryTiles = 0;
    bool ignoredBlightSeen = false;
    bool ignoredVariationSeen = false;
    bool nativeEditorOrGameVerified = false;
    bool formalSaveAuthorized = false;
};

struct ClassicMinimapCandidate {
    Image::DecodedImage raw;
    Image::DecodedImage preNoise;
    Image::DecodedImage finalImage;
    Image::BlpJpegSingleMip encoded;
    ClassicMinimapManifest manifest;
};

// 纯离线、FullW3eAlgorithmCandidate：解析冻结 W3E，逐 tile 使用 SW 顶点和真实 ground
// atlas 代表色，处理已知 BoundaryTerrain=A230 黑色混合，随后调用纯 CPU postprocess 与
// BLP1/JPEG q40 单 mip 编码。保存层忽略 blight/variation 但把其出现记录到 manifest。
// 实际遇到水、悬崖层级/坡道、对象非空、声明W3I crop 或未知标志均拒绝；MapEdge
// 与已知 BoundaryTerrain 同样使用 A230 黑混合。对象域只是 caller 声明，未认证来源。
// 不读地图、不捕获对象、不写归档、不证明原版/native/game 输出等价。调用者提供的
// Ground rawcode/table winner、archive当前来源和对象为空均未被本纯算法入口独立认证。
[[nodiscard]] Result<ClassicMinimapCandidate> BuildClassicMinimapCandidate(
    const ClassicMinimapCandidateInput& input,
    const ClassicMinimapCandidateLimits& limits = {});

} // namespace War3Map::Render
