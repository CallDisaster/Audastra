#pragma once

#include "War3MapCore/Image/BlpJpegEncode.h"
#include "War3MapCore/MapData/TerrainRowScan.h"
#include "War3MapCore/Render/TerrainMinimapCandidate.h"
#include "War3MapCore/Render/TerrainMinimapPostprocess.h"
#include "War3MapCore/Render/TerrainMinimapRepresentativeColor.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace War3Map::Render {

// ParsedEmpty仅供sealed对象捕获入口，旧caller声明入口拒绝该值。
enum class ClassicMinimapMemberState { Unknown, Missing, Empty, ParsedEmpty, Parsed };

class ClassicMinimapObjectAssets;

// 旧纯算法入口只接收 caller 声明的 Missing/Empty 一致性字段。
// ParsedEmpty/Parsed 必须来自实际对象捕获；对象资源和当前修订由保存服务复核。
// 这些公开字段只属于caller入口，不是来源读取证明，也不构成 native/editor/game 资格。
// 真实archive/edits捕获由独立sealed typed入口消费，不能用公开声明代替。
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
    std::size_t maxTiles = 65536; // 旧FullW3e caller候选仍使用此总量帽；ClassicPlayable使用下列独立账。
    std::size_t maxRawRgbaBytes = 64u * 1024u * 1024u;
    std::size_t maxBlpBytes = 1024u * 1024u;
    std::size_t maxSourceTextBytes = 64u * 1024u;
    MapData::TerrainRowScanLimits terrainScan;
};

struct ClassicMinimapManifest {
    static constexpr std::uint32_t algorithmVersion = 2;
    std::string rangePolicy = "FullW3eAlgorithmCandidate-v1";
    static constexpr const char* groundPolicy = "sw-ground-atlas-representative-v1";
    static constexpr const char* boundaryPolicy = "boundary-a230-black-truncate-v1";
    static constexpr const char* objectDomainDeclaration = "caller-declared-v1/native=false";
    static constexpr const char* encodingPolicy = "blp1-jpeg-q40-single-mip-v1";
    std::string sourceSha256;
    std::string terrainSha256;
    std::string objectDomainSha256;
    std::string objectDomainPolicy = "caller-declared-v1/native=false";
    std::string objectCaptureIdentitySha256;
    std::string rawRgbaSha256;
    std::string preNoiseRgbaSha256;
    std::string finalRgbaSha256;
    std::string blpSha256;
    TerrainMinimapPostprocessFacts postprocessFacts;
    std::string noiseSamplesSha256;
    std::uint32_t sourceWidth = 0, sourceHeight = 0;
    std::size_t sourceTiles = 0;
    MapData::TerrainRowScanFacts terrainScan; // 完整输入、累计访问及最多七行真实驻留账。
    std::array<std::uint32_t, 4> sourceRectangle{};
    std::size_t boundaryTiles = 0;
    std::size_t cliffTiles = 0, shallowWaterTiles = 0, deepWaterTiles = 0, objectPixels = 0;
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

// 纯CPU算法：按SW顶点选地面/悬崖代表色，按水深逐次混色，再混边界；
// ClassicPlayable 消费同源W3I的可玩区，后处理和BLP1/JPEG q40单mip共用。
// 旧FullW3e无配置输入仍拒未知水/坡道；本入口不读写归档，不认证caller来源。
[[nodiscard]] Result<ClassicMinimapCandidate> BuildClassicMinimapCandidate(
    const ClassicMinimapCandidateInput& input,
    const ClassicMinimapCandidateLimits& limits = {});

// 借用真实捕获的Terrain和对象图层，按DOO顺序覆盖像素，不复制W3E/atlas/settings。
// 只消费冻结资产；不重新验证当前来源，不授予长期有效或正式保存资格。
[[nodiscard]] Result<ClassicMinimapCandidate> BuildClassicMinimapCandidate(
    const ClassicMinimapObjectAssets& assets,
    const TerrainMinimapPostprocessSettings& postprocess = {},
    const ClassicMinimapCandidateLimits& limits = {});

} // namespace War3Map::Render
