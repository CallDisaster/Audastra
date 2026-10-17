#include "War3MapCore/Render/WorldModelScene.h"
#include "War3MapCore/Render/ModelPicking.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/RenderPluginStaticSamplers.h"
#include "MdxTestModel.h"
#include "fixtures/TinyJpegFixture.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace War3Map;
using namespace War3Map::Render;

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::string Key(std::string_view path) {
    std::string key(path);
    for (auto& c : key) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return key;
}

std::vector<std::byte> Blp(std::uint32_t width, std::uint32_t height, std::uint8_t red) {
    Image::DecodedImage image{width, height, std::vector<std::byte>(static_cast<std::size_t>(width) * height * 4)};
    for (std::size_t i = 0; i < image.rgba.size(); i += 4) { image.rgba[i] = std::byte{red}; image.rgba[i + 3] = std::byte{255}; }
    auto encoded = Image::EncodeBlpPaletted(image);
    Require(encoded.Succeeded(), "合成 BLP 编码失败");
    return std::move(*encoded.value);
}

struct Files {
    std::map<std::string, std::vector<std::byte>> files;
    std::map<std::string, int> reads;
    ModelFileReader Reader() {
        return [this](std::string_view path) {
            ++reads[Key(path)];
            const auto found = files.find(Key(path));
            if (found == files.end()) return Result<std::vector<std::byte>>::Failure("Test", "MISSING", "缺少文件");
            return Result<std::vector<std::byte>>::Success(found->second);
        };
    }
};

ModelInstanceRequest At(std::string path, std::uint32_t team, float x) {
    ModelInstanceRequest request;
    request.modelPath = std::move(path); request.teamColor = team; request.position = {x, 0, 0};
    return request;
}

bool HasWarning(const WorldModelScene& scene, std::string_view code) {
    return std::any_of(scene.warnings.begin(), scene.warnings.end(), [&](const Diagnostic& d) { return d.code == code; });
}

std::vector<std::byte> RigidTestModel(bool multiNodeGroup = false) {
    // 共享夹具动作按时间顺序：Stand=0 [0,1000]、Death=1 [2000,3000]。
    auto bytes = MdxTest::BuildTestMdx({.animatedHelper = true});
    const auto tag = [&](std::array<char, 4> name) {
        const auto at = std::search(bytes.begin(), bytes.end(), name.begin(), name.end(),
            [](std::byte left, char right) { return left == static_cast<std::byte>(right); });
        Require(at != bytes.end(), "合成 MDX 缺少预期块");
        return static_cast<std::size_t>(at - bytes.begin());
    };
    // GEOA 的 Stand 隐藏组对任意动作都无法可靠判定可见度；成功样本移除该块。
    const auto geoa = tag({'G', 'E', 'O', 'A'});
    std::uint32_t length = 0;
    std::memcpy(&length, bytes.data() + geoa + 4, 4);
    bytes.erase(bytes.begin() + geoa, bytes.begin() + geoa + 8 + length);
    // 第二个 geoset 故意含尚未渲染的三角带；本窄能力只验首个完整三角形 geoset。
    const auto geos = tag({'G', 'E', 'O', 'S'});
    std::uint32_t firstSize = 0, secondSize = 0;
    std::memcpy(&firstSize, bytes.data() + geos + 8, 4);
    const auto second = geos + 8 + firstSize;
    std::memcpy(&secondSize, bytes.data() + second, 4);
    bytes.erase(bytes.begin() + second, bytes.begin() + second + secondSize);
    std::memcpy(&length, bytes.data() + geos + 4, 4); length -= secondSize;
    std::memcpy(bytes.data() + geos + 4, &length, 4);
    if (multiNodeGroup) {
        const auto mtgc = tag({'M', 'T', 'G', 'C'});
        const auto mats = tag({'M', 'A', 'T', 'S'});
        const std::uint32_t two = 2;
        std::memcpy(bytes.data() + mtgc + 8, &two, 4); // 首个组引用两个合法节点 ID
        std::memcpy(bytes.data() + mats + 4, &two, 4);
        bytes.insert(bytes.begin() + mats + 12, 4, std::byte{0});
        std::memcpy(&length, bytes.data() + geos + 4, 4); length += 4;
        std::memcpy(bytes.data() + geos + 4, &length, 4);
        std::memcpy(&length, bytes.data() + geos + 8, 4); length += 4;
        std::memcpy(bytes.data() + geos + 8, &length, 4);
    }
    return bytes;
}

// 沿现有原创夹具移除控制器及一个矩阵组，材质、贴图、动作与静态几何全部保留。
std::vector<std::byte> NoControllerStaticTestModel() {
    const auto original = RigidTestModel();
    MdxTest::Writer output; output.Tag("MDLX");
    for (std::size_t at = 4; at < original.size();) {
        const std::string tag(reinterpret_cast<const char*>(original.data() + at), 4);
        std::uint32_t size = 0; std::memcpy(&size, original.data() + at + 4, 4);
        if (tag != "BONE" && tag != "HELP" && tag != "PIVT" && tag != "UNKN") {
            std::vector<std::byte> chunk(original.begin() + at, original.begin() + at + 8 + size);
            if (tag == "GEOS") {
                const auto find = [&](const std::array<char, 4>& name) {
                    const auto found = std::search(chunk.begin(), chunk.end(), name.begin(), name.end(),
                        [](std::byte left, char right) { return left == static_cast<std::byte>(right); });
                    Require(found != chunk.end(), "原创空矩阵组夹具需要字段");
                    return static_cast<std::size_t>(found - chunk.begin());
                };
                for (const auto name : {std::array{'M', 'A', 'T', 'S'}, std::array{'M', 'T', 'G', 'C'}}) {
                    const auto field = find(name); const std::uint32_t zero = 0;
                    std::memcpy(chunk.data() + field + 4, &zero, 4);
                    chunk.erase(chunk.begin() + field + 8, chunk.begin() + field + 12);
                }
                std::uint32_t geosetBytes = 0; std::memcpy(&geosetBytes, chunk.data() + 8, 4); geosetBytes -= 8;
                size -= 8; std::memcpy(chunk.data() + 4, &size, 4); std::memcpy(chunk.data() + 8, &geosetBytes, 4);
            }
            output.Raw(chunk.data(), chunk.size());
        }
        std::uint32_t originalSize = 0; std::memcpy(&originalSize, original.data() + at + 4, 4); at += 8 + originalSize;
    }
    return std::move(output.bytes);
}

void TestNoControllerStaticPreview() {
    Files files;
    const auto rigidBytes = NoControllerStaticTestModel();
    const auto strictModel = ParseMdx(rigidBytes);
    Require(!strictModel.Succeeded() && strictModel.diagnostics.front().code == "MDX_VALUE_INVALID", "原空矩阵组仍严格拒绝");
    files.files[Key("Preview\\Static.mdx")] = rigidBytes;
    files.files[Key("Units\\Normal.mdx")] = RigidTestModel();
    files.files[Key(TeamColorTexture(0))] = Blp(2, 2, 64);
    files.files[Key("Textures\\Crate.blp")] = Blp(2, 2, 96);
    const std::array requests{At("Preview\\Static.mdx", 0, 100), At("Preview\\Static.mdx", 0, 200),
        At("Units\\Normal.mdx", 0, 300)};
    const auto strict = BuildWorldModelScene(requests, files.Reader());
    Require(strict.Succeeded() && strict.value->instances.size() == 1 && strict.value->skippedInstances == 2
        && strict.value->rigidStaticPreviewAssets == 0 && HasWarning(*strict.value, "WORLD_MODEL_UNAVAILABLE"),
        "默认场景拒绝静态近似，正常资产继续显示");
    WorldModelLimits limits; limits.allowRigidStaticPreview = true; files.reads.clear();
    const auto built = BuildWorldModelScene(requests, files.Reader(), limits);
    Require(built.Succeeded(), "显式静态预览构建");
    const auto& scene = *built.value;
    Require(scene.instances.size() == 3 && scene.models.size() == 2 && scene.meshes.size() == 2
        && scene.vertices.size() == 8 && scene.indices.size() == 12 && scene.skippedInstances == 0
        && scene.rigidStaticPreviewAssets == 1 && scene.rigidStaticPreviewInstances == 2, "完整几何、资产与实例计数");
    Require(scene.instances[0].model == scene.instances[1].model && scene.instances[0].transform[3] == 100
        && scene.instances[1].transform[3] == 200 && files.reads[Key("Preview\\Static.mdx")] == 1, "两个实例共享资产且变换独立");
    const auto& mesh = scene.meshes[scene.models[scene.instances[0].model].firstMesh];
    Require(mesh.layerCount == 2 && scene.layers[mesh.firstLayer].filterMode == 0
        && scene.layers[mesh.firstLayer + 1].filterMode == 1 && scene.layers[mesh.firstLayer + 1].alpha == 0.5f
        && scene.textures.size() == 2, "空矩阵组预览没有丢掉材料、层、透明度与真实贴图");
    const auto warning = std::find_if(scene.warnings.begin(), scene.warnings.end(), [](const Diagnostic& d) {
        return d.code == "WORLD_MODEL_RIGID_STATIC_PREVIEW";
    });
    Require(warning != scene.warnings.end() && warning->origin && warning->origin->offset > 0
        && warning->message.find("预览近似") != std::string::npos && warning->message.find("count=0") != std::string::npos
        && warning->message.find("Preview\\Static.mdx") != std::string::npos
        && std::count_if(scene.warnings.begin(), scene.warnings.end(), [](const Diagnostic& d) {
            return d.code == "WORLD_MODEL_RIGID_STATIC_PREVIEW";
        }) == 1, "字段偏移与资格警告按资产汇总而非逐实例重复");
    const auto posed = BuildWorldModelScene(std::span(requests).first(2), files.Reader(), limits, {},
        ModelSceneRigidPoseRequest{0, 500, 0});
    Require(posed.Succeeded() && posed.value->instances.size() == 2 && posed.value->vertices.size() == 4
        && posed.value->vertices[0].x == scene.vertices[0].x && posed.value->vertices[3].z == scene.vertices[3].z,
        "显式Stand请求通过专用identity参考采样，位置不重复应用实例变换");
    Require(files.files[Key("Preview\\Static.mdx")] == rigidBytes, "原MDX字节未改写");
    auto parsed = ParseMdxRigidStaticPreview(rigidBytes); Require(parsed.Succeeded(), "原创静态来源解析");
    const auto asset = std::make_shared<const MdxModel>(std::move(*parsed.value));
    const auto providerStrict = BuildAnimatedModelGeometry(std::span(requests).first(2), files.Reader(),
        [&](std::string_view) { return ModelGeometryAsset{asset, true}; });
    Require(providerStrict.Succeeded() && providerStrict.value->instances.empty()
        && HasWarning(*providerStrict.value, "WORLD_MODEL_RIGID_PREVIEW_POLICY_REQUIRED"),
        "外部已解析资产不能绕过场景默认策略");
}

std::vector<std::byte> PartialSplineTestModel(bool bothGeosetsUseSpline) {
    auto bytes = MdxTest::BuildTestMdx({.animatedHelper = true});
    const auto tag = [&](std::array<char, 4> name, std::size_t start = 0, std::size_t end = 0) {
        if (end == 0) end = bytes.size();
        const auto found = std::search(bytes.begin() + start, bytes.begin() + end, name.begin(), name.end(),
            [](std::byte left, char right) { return left == static_cast<std::byte>(right); });
        Require(found != bytes.begin() + end, "合成 MDX 缺少预期块");
        return static_cast<std::size_t>(found - bytes.begin());
    };
    const auto geos = tag({'G', 'E', 'O', 'S'});
    std::uint32_t firstSize = 0, secondSize = 0;
    std::memcpy(&firstSize, bytes.data() + geos + 8, 4);
    const auto second = geos + 8 + firstSize;
    std::memcpy(&secondSize, bytes.data() + second, 4);
    const auto ptyp = tag({'P', 'T', 'Y', 'P'}, second, second + secondSize);
    const std::uint32_t triangle = 4, nodeId = bothGeosetsUseSpline ? 0 : 1;
    std::memcpy(bytes.data() + ptyp + 12, &triangle, 4); // 第二组改为完整三角形。
    const auto mats = tag({'M', 'A', 'T', 'S'}, second, second + secondSize);
    std::memcpy(bytes.data() + mats + 8, &nodeId, 4);
    const auto kgao = tag({'K', 'G', 'A', 'O'});
    const float visible = 1;
    std::memcpy(bytes.data() + kgao + 20, &visible, 4); // 两块 geoset 在 Stand 中均可见。

    MdxTest::Writer rotation;
    rotation.Tag("KGRT"); rotation.U32(2); rotation.U32(2); rotation.U32(UINT32_MAX);
    for (const auto frame : {0u, 1000u}) {
        rotation.U32(frame);
        for (int tangent = 0; tangent < 3; ++tangent) {
            rotation.F32(0); rotation.F32(0); rotation.F32(0); rotation.F32(1);
        }
    }
    const auto bone = tag({'B', 'O', 'N', 'E'});
    const auto node = bone + 8;
    std::uint32_t nodeSize = 0, boneSize = 0;
    std::memcpy(&nodeSize, bytes.data() + node, 4);
    std::memcpy(&boneSize, bytes.data() + bone + 4, 4);
    bytes.insert(bytes.begin() + node + nodeSize, rotation.bytes.begin(), rotation.bytes.end());
    nodeSize += static_cast<std::uint32_t>(rotation.bytes.size());
    boneSize += static_cast<std::uint32_t>(rotation.bytes.size());
    std::memcpy(bytes.data() + node, &nodeSize, 4);
    std::memcpy(bytes.data() + bone + 4, &boneSize, 4);
    return bytes;
}

void TestRigidPoseScene() {
    Files files;
    files.files[Key("Units\\Rigid.mdx")] = RigidTestModel();
    const std::vector<ModelInstanceRequest> requests{At("Units\\Rigid.mdx", 0, 100), At("Units\\Rigid.mdx", 0, 200)};
    const auto staticScene = BuildWorldModelScene(requests, files.Reader());
    const auto posed = BuildWorldModelScene(requests, files.Reader(), {}, {}, ModelSceneRigidPoseRequest{0, 500, 0});
    Require(staticScene.Succeeded() && posed.Succeeded(), "单节点组的静态及显式姿态场景构建");
    Require(staticScene.value->vertices[0].x == 0 && std::abs(posed.value->vertices[0].x - 5) < 1e-4f
        && posed.value->models.size() == 1 && posed.value->instances.size() == 2
        && posed.value->modelBounds[0][0] == 5 && posed.value->rigidPose->sequenceElapsedMs == 500,
        "姿态几何、变体复用、包围盒和时间身份");
    const auto invalid = BuildWorldModelScene(requests, files.Reader(), {}, {}, ModelSceneRigidPoseRequest{99, 0, 0});
    Require(!invalid.Succeeded() && invalid.diagnostics.front().code == "WORLD_MODEL_POSE_SEQUENCE_INVALID",
        "无效动作显式拒绝");
    files.files[Key("Units\\Rigid.mdx")] = RigidTestModel(true);
    const auto multi = BuildWorldModelScene(requests, files.Reader(), {}, {}, ModelSceneRigidPoseRequest{0, 500, 0});
    Require(multi.Succeeded() && multi.value && multi.value->vertices.size() == posed.value->vertices.size()
        && std::abs(multi.value->vertices[0].x - posed.value->vertices[0].x) < 1e-4f,
        "相同节点的两元素矩阵组与单节点姿态相同");
    files.files[Key("Units\\Rigid.mdx")] = MdxTest::BuildTestMdx({.animatedHelper = true});
    const auto stand = BuildWorldModelScene(requests, files.Reader(), {}, {}, ModelSceneRigidPoseRequest{0, 500, 0});
    Require(stand.Succeeded() && stand.value->meshes.size() == 1,
        "Stand 帧 KGAO=0 的隐藏 geoset 不阻断完整可见姿态");
    const auto death = BuildWorldModelScene(requests, files.Reader(), {}, {}, ModelSceneRigidPoseRequest{1, 0, 0});
    Require(!death.Succeeded() && death.diagnostics.front().code == "WORLD_MODEL_POSE_PRIMITIVE_UNSUPPORTED",
        "Death 帧 KGAO=1 的 geoset 必须接受图元完整性门禁");

    files.files[Key("Units\\Rigid.mdx")] = PartialSplineTestModel(false);
    const auto partial = BuildWorldModelScene(requests, files.Reader(), {}, {}, ModelSceneRigidPoseRequest{0, 500, 0});
    Require(partial.Succeeded() && partial.value->meshes.size() == 2 && partial.value->vertices.size() > 4
        && partial.value->instances.size() == 2 && HasWarning(*partial.value, "MDX_POSE_GENERIC_ROTATION_PREVIEW"),
        "KGRT 样条的通用预览须显示两块 geoset 并标明语义边界");
    Require(std::any_of(partial.value->warnings.begin(), partial.value->warnings.end(), [](const Diagnostic& d) {
        return d.code == "MDX_POSE_GENERIC_ROTATION_PREVIEW" && d.message.find("Units\\Rigid.mdx") != std::string::npos;
    }), "通用预览警告保留模型身份");
    files.files[Key("Units\\Rigid.mdx")] = PartialSplineTestModel(true);
    const auto allUnsupported = BuildWorldModelScene(requests, files.Reader(), {}, {}, ModelSceneRigidPoseRequest{0, 500, 0});
    Require(allUnsupported.Succeeded() && allUnsupported.value->meshes.size() == 2
        && HasWarning(*allUnsupported.value, "MDX_POSE_GENERIC_ROTATION_PREVIEW"),
        "全部可见 geoset 使用 KGRT 样条时仍须发布带警告的通用预览");
    auto cameraPose = ModelSceneRigidPoseRequest{0, 500, 0};
    cameraPose.camera = MdxPoseCamera{{0, -800, 300}, {0, 0, 1}};
    const auto sharedCamera = BuildWorldModelScene(requests, files.Reader(), {}, {}, cameraPose);
    Require(!sharedCamera.Succeeded() && sharedCamera.diagnostics.front().code == "WORLD_MODEL_POSE_CAMERA_SHARED",
        "不同摆放实例不能共享模型局部相机视点");
    const auto singleCamera = BuildWorldModelScene(std::span(requests).first(1), files.Reader(), {}, {}, cameraPose);
    Require(singleCamera.Succeeded() && singleCamera.value->rigidPose->camera.has_value(),
        "单实例相机姿态请求须透传到模型预览场景");
}

void TestBuild() {
    Files files;
    files.files[Key("Units\\Crate.mdx")] = MdxTest::BuildTestMdx();
    files.files[Key("Textures\\Crate.blp")] = Blp(1024, 4, 200);
    files.files[Key(TeamColorTexture(1))] = Blp(2, 2, 255);
    auto rotated = At("units/crate.mdx", 1, 200);
    rotated.angle = 1.5707963f; rotated.scale = {2, 2, 3}; rotated.tint = {0.5f, 1, 1};
    const std::vector<ModelInstanceRequest> requests{At("Units\\Crate.mdx", 1, 100), At("Units\\Crate.mdx", 0, 300),
        At("Units\\Missing.mdx", 0, 0), rotated};
    const auto result = BuildWorldModelScene(requests, files.Reader());
    Require(result.Succeeded(), "场景构建失败");
    const auto& scene = *result.value;
    Require(files.reads[Key("Units\\Crate.mdx")] == 1 && files.reads[Key("Textures\\Crate.blp")] == 1, "同一模型/贴图只读取一次");
    // 队伍色不同 → 两个模型变体；Stand 起点隐藏的 geoset 不进入网格。
    Require(scene.models.size() == 2 && scene.meshes.size() == 2 && scene.layers.size() == 4, "模型变体/网格/层");
    Require(scene.vertices.size() == 8 && scene.indices.size() == 12 && scene.meshes[1].baseVertex == 4, "几何复制");
    Require(scene.requestedInstances == 4 && scene.instances.size() == 3 && scene.skippedInstances == 1, "实例计数");
    Require(std::is_sorted(scene.instances.begin(), scene.instances.end(),
        [](const auto& a, const auto& b) { return a.model < b.model; }), "实例按模型排序");
    Require(scene.instances[0].model == 0 && scene.instances[1].model == 0 && scene.instances[2].model == 1, "排序稳定");
    Require(scene.instanceRequests == std::vector<std::uint32_t>{0, 3, 1}, "实例对应的请求序号随排序重排");
    Require(scene.modelBounds.size() == 2 && scene.modelBounds[0][0] == 0 && scene.modelBounds[0][3] == 10
        && scene.modelBounds[0][1] == 0 && scene.modelBounds[0][4] == 10, "模型局部包围盒");
    const auto& turned = scene.instances[1].transform;
    Require(std::abs(turned[0]) < 1e-5f && std::abs(turned[1] + 2) < 1e-5f && turned[3] == 200
        && std::abs(turned[4] - 2) < 1e-5f && turned[10] == 3 && scene.instances[1].color[0] == 0.5f, "T·Rz·S 变换与颜色");
    // 第一层是队伍色（替换贴图 1），第二层是透明测试的普通贴图。
    Require(scene.textures.size() == 2, "队伍色 01 与普通贴图各一张");
    const auto& layers = scene.layers;
    Require(layers[0].filterMode == 0 && layers[0].flags == 0 && layers[1].filterMode == 1 && layers[1].alpha == 0.5f, "层属性");
    Require(scene.textures[layers[0].textureIndex].path == TeamColorTexture(1), "替换贴图 1 使用队伍色");
    const auto& crate = scene.textures[layers[1].textureIndex];
    Require(crate.width == 512 && crate.height == 2 && crate.rgba[0] == 200, "大贴图缩小到 512");
    Require(layers[2].textureIndex == WAR3_RENDER_MODEL_NO_TEXTURE && scene.untexturedLayers == 1, "缺队伍色 00 按白色");
    Require(HasWarning(scene, "WORLD_MODEL_UNAVAILABLE") && HasWarning(scene, "WORLD_MODEL_TEXTURE_UNAVAILABLE"), "警告汇总");

    const auto descriptor = DescribeModelScene(scene);
    Require(descriptor.scene.structSize == sizeof(War3RenderModelSceneV2) && descriptor.scene.instanceCount == 3
        && descriptor.scene.textureCount == 2 && descriptor.scene.textures[1].mips->width == crate.width
        && descriptor.scene.textures[1].mips->rgba == crate.rgba.data(), "ABI 描述符");
}

void TestGhostUsesSharedAssetAndBlendedVariant() {
    Files files;
    files.files[Key("Units\\Crate.mdx")] = MdxTest::BuildTestMdx();
    files.files[Key("Textures\\Crate.blp")] = Blp(4, 4, 200);
    files.files[Key(TeamColorTexture(1))] = Blp(2, 2, 255);
    auto normal = At("Units\\Crate.mdx", 1, 0);
    auto ghost = normal; ghost.previewGhost = true; ghost.position[0] = 100;
    const std::vector<ModelInstanceRequest> requests{normal, ghost};
    auto built = BuildWorldModelScene(requests, files.Reader());
    Require(built.Succeeded() && built.value && built.value->models.size() == 2
        && built.value->instances.size() == 2, "半透明预览须是独立材质变体");
    auto& scene = *built.value;
    Require(files.reads[Key("Units\\Crate.mdx")] == 1
        && files.reads[Key("Textures\\Crate.blp")] == 1, "预览复用已读取的模型和贴图");
    const auto& realLayer = scene.layers[scene.meshes[scene.models[scene.instances[0].model].firstMesh].firstLayer];
    const auto& ghostLayer = scene.layers[scene.meshes[scene.models[scene.instances[1].model].firstMesh].firstLayer];
    Require(realLayer.filterMode == 0 && realLayer.alpha == 1.0f
        && ghostLayer.filterMode == 2 && ghostLayer.alpha == 0.45f,
        "仅预览实例使用 alpha 混合，已摆放模型保持不变");
    auto moved = requests; moved[1].position[0] = 200;
    const auto geometryCount = scene.vertices.size();
    Require(ReplaceModelInstances(scene, requests, moved) && scene.vertices.size() == geometryCount
        && scene.instances[1].transform[3] == 200,
        "鼠标移动只更新预览实例，不重建静态几何");
}

void TestMaterialUnfoggedFlags() {
    Files files;
    files.files[Key("Textures\\Crate.blp")] = Blp(4, 4, 200);
    files.files[Key(TeamColorTexture(0))] = Blp(2, 2, 255);
    const std::array<std::pair<std::uint32_t, std::uint32_t>, 7> cases{{
        {0, 0}, {0x1, 1}, {0x2, 0}, {0x20, WAR3_RENDER_MODEL_UNFOGGED}, {0x80, 4}, {0xa1, 7}, {0x10, 0}}};
    for (const auto [sourceFlags, expectedFlags] : cases) {
        auto bytes = RigidTestModel();
        constexpr std::array<char, 4> layersTag{'L', 'A', 'Y', 'S'};
        const auto layers = std::search(bytes.begin(), bytes.end(), layersTag.begin(), layersTag.end(),
            [](std::byte left, char right) { return left == static_cast<std::byte>(right); });
        Require(layers != bytes.end(), "原创材质需要 LAYS");
        // LAYS + count + inclusiveSize + filterMode 后为第一层的原始 shadingFlags。
        const auto flagsAt = static_cast<std::size_t>(layers - bytes.begin()) + 16;
        std::memcpy(bytes.data() + flagsAt, &sourceFlags, sizeof(sourceFlags));
        const auto parsed = ParseMdx(bytes);
        Require(parsed.Succeeded() && parsed.value->materials[0].layers[0].shadingFlags == sourceFlags,
            "原创 MDX 标志夹具错误");
        files.files[Key("Original\\FogLayer.mdx")] = bytes;
        auto ghost = At("Original\\FogLayer.mdx", 0, 200); ghost.previewGhost = true;
        const std::array requests{At("Original\\FogLayer.mdx", 0, 0), ghost};
        const auto built = BuildWorldModelScene(requests, files.Reader());
        const auto posed = BuildWorldModelScene(std::span(requests).first(1), files.Reader(), {}, {},
            ModelSceneRigidPoseRequest{0, 500, 0});
        Require(built.Succeeded() && built.value->models.size() == 2 && posed.Succeeded(), "材质标志场景构建失败");
        for (const auto& scene : {&*built.value, &*posed.value}) {
            for (const auto& model : scene->models) {
                const auto& mesh = scene->meshes[model.firstMesh];
                Require(scene->layers[mesh.firstLayer].flags == expectedFlags
                    && scene->layers[mesh.firstLayer + 1].flags == 0,
                    "静态、显式姿态及预览层须保留各自 Unfogged，且不改变旧光照/深度标志");
            }
        }
        Require(files.files.at(Key("Original\\FogLayer.mdx")) == bytes, "渲染不得回写来源模型");
    }
}

void TestLimitsAndInvalid() {
    Files files;
    files.files[Key("Units\\Crate.mdx")] = MdxTest::BuildTestMdx();
    WorldModelLimits limits;
    limits.maxModels = 1;
    const std::vector<ModelInstanceRequest> requests{At("Units\\Crate.mdx", 1, 0), At("Units\\Crate.mdx", 2, 0)};
    const auto result = BuildWorldModelScene(requests, files.Reader(), limits);
    Require(result.Succeeded() && result.value->models.size() == 1 && result.value->skippedInstances == 1
        && HasWarning(*result.value, "WORLD_MODEL_BUDGET"), "预算不足只跳过相关实例");
    auto invalid = At("Units\\Crate.mdx", 0, 0);
    invalid.position[2] = std::nanf("");
    Require(!BuildWorldModelScene(std::span(&invalid, 1), files.Reader()).Succeeded(), "非有限坐标拒绝");
    Require(!BuildWorldModelScene(requests, {}).Succeeded(), "缺读取器拒绝");
    const auto empty = BuildWorldModelScene({}, files.Reader());
    Require(empty.Succeeded() && empty.value->instances.empty() && DescribeModelScene(*empty.value).scene.instanceCount == 0, "空场景");
}

// 射线点选实例包围盒：按进入距离排序，小物件按最小半径放大；旋转缩放实例按局部盒判断。
void TestPicking() {
    Files files;
    files.files[Key("Units/Crate.mdx")] = MdxTest::BuildTestMdx();
    auto rotated = At("Units/Crate.mdx", 0, 200);
    rotated.angle = 1.5707963f; rotated.scale = {2, 2, 3};
    const std::vector<ModelInstanceRequest> requests{At("Units/Crate.mdx", 0, 100), rotated};
    const auto built = BuildWorldModelScene(requests, files.Reader());
    Require(built.Succeeded() && built.value->instances.size() == 2, "点选场景");
    const auto& scene = *built.value;
    const auto down = [](double x, double y) { return TerrainRay{{x, y, 1000}, {0, 0, -1}}; };
    auto hits = PickModelInstances(scene, down(105, 5), 5000, 0);
    Require(hits.size() == 1 && scene.instanceRequests[hits[0].instance] == 0, "点中第一个实例");
    Require(PickModelInstances(scene, down(115, 5), 5000, 0).empty(), "盒外未命中");
    Require(PickModelInstances(scene, down(115, 5), 5000, 24).size() == 1, "小物件按最小半径放大");
    hits = PickModelInstances(scene, down(190, 10), 5000, 0);
    Require(hits.size() == 1 && scene.instanceRequests[hits[0].instance] == 1, "旋转缩放实例的局部盒");
    Require(PickModelInstances(scene, down(210, 10), 5000, 0).empty(), "旋转后原方向的空处未命中");
    Require(PickModelInstances(scene, down(105, 5), 100, 0).empty(), "超出最大距离未命中");
    // 斜穿两个实例：先进入者在前。
    const TerrainRay across{{0, 5, 1}, {1, 0, 0}};
    const auto both = PickModelInstances(scene, across, 5000, 30);
    Require(both.size() == 2 && both[0].distance < both[1].distance
        && scene.instanceRequests[both[0].instance] == 0, "按进入距离排序");
    const auto box = ModelInstanceBox(scene, hits[0].instance, 0);
    Require(box && std::abs((*box)[0][0] - 200) < 1e-3f && std::abs((*box)[2][0] - 180) < 1e-3f
        && std::abs((*box)[2][1] - 20) < 1e-3f, "包围盒角点经实例变换");
    Require(!ModelInstanceBox(scene, 99, 0), "越界实例无包围盒");
}

BakedModelMesh Triangle(float x, std::string texture, std::uint32_t samplerFlags = 3) {
    BakedModelMesh mesh;
    for (int i = 0; i < 3; ++i) {
        War3RenderModelVertexV2 vertex{};
        vertex.x = x + static_cast<float>(i) * 10; vertex.y = static_cast<float>(i % 2) * 10; vertex.nz = 1;
        mesh.vertices.push_back(vertex);
    }
    mesh.indices = {0, 1, 2};
    mesh.texturePath = std::move(texture);
    mesh.samplerFlags = samplerFlags;
    return mesh;
}

std::vector<std::uint32_t> EffectiveSamplerFlags(const WorldModelScene& scene) {
    Require(scene.layerSamplerFlags.empty() || scene.layerSamplerFlags.size() == scene.layers.size(),
        "显式 sampler 数组必须与图层全长对齐");
    return scene.layerSamplerFlags.empty() ? std::vector<std::uint32_t>(scene.layers.size(), 3)
        : std::vector<std::uint32_t>(scene.layerSamplerFlags.cbegin(), scene.layerSamplerFlags.cend());
}

// 保存实际 CPU 场景的独立字节收据，拒绝路径不能靠同时改坏两个共享快照蒙混过关。
std::string BakedSceneReceipt(const WorldModelScene& scene) {
    std::string bytes;
    const auto array = [&](const auto& values) {
        const auto count = values.size();
        bytes.append(reinterpret_cast<const char*>(&count), sizeof(count));
        const auto raw = std::as_bytes(std::span(values.data(), values.size()));
        if (!raw.empty()) bytes.append(reinterpret_cast<const char*>(raw.data()), raw.size());
    };
    const auto text = [&](const std::string& value) { array(value); };
    array(scene.vertices); array(scene.indices); array(scene.layers); array(scene.layerSamplerFlags);
    array(scene.meshes); array(scene.models); array(scene.instances); array(scene.instanceRequests);
    array(scene.modelBounds); array(scene.vertexGroups); array(scene.meshGeosets);
    for (const auto& path : scene.modelPaths) text(path);
    for (const auto& key : scene.modelAssetKeys) text(key);
    for (const auto& texture : scene.textures) {
        text(texture.path); text(texture.ownedIdentity); array(texture.rgba);
        array(std::array{texture.width, texture.height});
    }
    for (const auto& bindings : scene.modelTextureBindings) {
        array(std::array{bindings ? bindings->size() : std::size_t{0}});
        if (bindings) for (const auto& binding : *bindings)
            array(std::array{binding.textureIndex, binding.samplerFlags, static_cast<std::uint32_t>(binding.reachable)});
    }
    array(std::array{scene.requestedInstances, scene.skippedInstances, scene.untexturedLayers,
        scene.rigidStaticPreviewAssets, scene.rigidStaticPreviewInstances});
    for (const auto& warning : scene.warnings) { text(warning.subsystem); text(warning.code); text(warning.message); }
    return bytes;
}

void RequireBakedReplaceRejected(WorldModelScene& scene, std::span<const BakedModelMesh> baked,
    const WorldModelLimits& limits = {}) {
    const auto receipt = BakedSceneReceipt(scene);
    const auto snapshot = scene;
    Require(!ReplaceBakedMeshes(scene, baked, limits) && BakedSceneReceipt(scene) == receipt,
        "失败替换必须保留几何、材质、sampler、实例、贴图和计数的原始数据");
    const WorldModelScene& after = scene;
    Require(after.vertices.SharesStorageWith(snapshot.vertices) && after.indices.SharesStorageWith(snapshot.indices)
        && after.layers.SharesStorageWith(snapshot.layers) && after.layerSamplerFlags.SharesStorageWith(snapshot.layerSamplerFlags)
        && after.meshes.SharesStorageWith(snapshot.meshes) && after.models.SharesStorageWith(snapshot.models)
        && after.modelBounds.SharesStorageWith(snapshot.modelBounds) && after.textures.SharesStorageWith(snapshot.textures)
        && after.vertexGroups.SharesStorageWith(snapshot.vertexGroups) && after.meshGeosets.SharesStorageWith(snapshot.meshGeosets)
        && after.modelPaths.SharesStorageWith(snapshot.modelPaths) && after.modelAssetKeys.SharesStorageWith(snapshot.modelAssetKeys)
        && after.modelTextureBindings.SharesStorageWith(snapshot.modelTextureBindings),
        "失败校验不得触发共享场景的隐式 COW 复制");
}

struct StaticSamplerBackend {
    War3RenderStaticSamplerCapabilitiesV2 response{sizeof(response), WAR3_RENDER_STATIC_LAYER_SAMPLERS_V1,
        WAR3_RENDER_STATIC_SAMPLER_FLAGS_MASK, 0};
    std::int32_t result = 0;
    unsigned calls = 0;
};

std::int32_t WAR3_RENDER_CALL StaticSamplerCapabilities(War3RendererHandle handle,
    War3RenderStaticSamplerCapabilitiesV2* capabilities) {
    auto& backend = *static_cast<StaticSamplerBackend*>(handle);
    ++backend.calls;
    Require(capabilities && capabilities->structSize == sizeof(*capabilities), "能力查询必须提供准确输出容量");
    *capabilities = backend.response;
    return backend.result;
}

void TestStaticSamplerDescriptorAndCapabilities(const WorldModelScene& scene) {
    auto source = scene;
    auto descriptor = DescribeModelScene(source);
    const auto& description = std::as_const(descriptor);
    Require(description.validStaticSamplers && description.StaticScene() == &description.samplerScene.base
        && description.samplerScene.base.structSize == sizeof(War3RenderStaticSamplerModelSceneV2)
        && description.samplerScene.samplerFormat == WAR3_RENDER_STATIC_LAYER_SAMPLERS_V1
        && description.samplerScene.samplerFlagCount == scene.layers.size()
        && description.samplerFlags == EffectiveSamplerFlags(scene)
        && description.samplerScene.samplerFlags == description.samplerFlags.data()
        && description.samplerFlags.SharesStorageWith(source.layerSamplerFlags), "静态 descriptor 必须共享全长 sampler 的不可写所有权");
    const auto* publishedFlags = description.samplerScene.samplerFlags;
    source.layerSamplerFlags.front() = 3;
    Require(description.samplerScene.samplerFlags == publishedFlags && description.samplerFlags == EffectiveSamplerFlags(scene)
        && !description.samplerFlags.SharesStorageWith(source.layerSamplerFlags),
        "来源场景可写访问必须 COW，已发布 descriptor 的拥有者、指针和 sampler 值保持不变");
    auto legacyBase = description.samplerScene.base; legacyBase.structSize = sizeof(legacyBase);
    Require(std::memcmp(&legacyBase, &description.scene, sizeof(legacyBase)) == 0,
        "扩展仅改变外部结构大小，旧 base 几何、图层、实例和贴图字段逐字节保持");
    auto moved = std::move(descriptor);
    const auto& movedView = std::as_const(moved);
    Require(movedView.StaticScene() == &movedView.samplerScene.base && movedView.samplerScene.samplerFlags == movedView.samplerFlags.data()
        && movedView.samplerFlags == EffectiveSamplerFlags(scene)
        && movedView.scene.textures == movedView.chains.data() && movedView.chains.front().mips == movedView.mips.data(),
        "移动 descriptor 后其 sampler、mip 与 chain 指针必须继续指向拥有的缓冲区");

    StaticSamplerBackend backend;
    War3RenderPluginApiV2 api{}; api.structSize = sizeof(api); api.apiVersion = WAR3_RENDER_PLUGIN_API_V2;
    api.getStaticModelSamplerCapabilities = StaticSamplerCapabilities;
    const auto unsupported = [&](const War3RenderPluginApiV2* candidate) {
        const auto status = CheckStaticModelSamplerSupport(candidate, &backend, scene);
        Require(!status.Succeeded() && !status.diagnostics.empty()
            && status.diagnostics.front().code == "WORLD_STATIC_SAMPLER_UNSUPPORTED",
            "非默认 sampler 必须拒绝缺失或不完整的后端能力，不能静默降级");
    };
    unsupported(nullptr);
    auto missing = api; missing.getStaticModelSamplerCapabilities = nullptr; unsupported(&missing);
    auto shortTable = api;
    shortTable.structSize = offsetof(War3RenderPluginApiV2, getStaticModelSamplerCapabilities)
        + sizeof(shortTable.getStaticModelSamplerCapabilities) - 1;
    unsupported(&shortTable);
    Require(backend.calls == 0, "旧表缺失尾部时不能访问新能力 getter");
    const auto good = backend.response;
    for (const auto response : {
        War3RenderStaticSamplerCapabilitiesV2{0, good.samplerFormat, good.supportedSamplerFlags, 0},
        War3RenderStaticSamplerCapabilitiesV2{sizeof(good) - 1, good.samplerFormat, good.supportedSamplerFlags, 0},
        War3RenderStaticSamplerCapabilitiesV2{sizeof(good), good.samplerFormat + 1, good.supportedSamplerFlags, 0},
        War3RenderStaticSamplerCapabilitiesV2{sizeof(good), good.samplerFormat, 1, 0},
        War3RenderStaticSamplerCapabilitiesV2{sizeof(good), good.samplerFormat, good.supportedSamplerFlags | 4, 0},
        War3RenderStaticSamplerCapabilitiesV2{sizeof(good), good.samplerFormat, good.supportedSamplerFlags, 1}}) {
        backend.response = response; unsupported(&api);
    }
    backend.response = good; backend.result = -1; unsupported(&api);
    backend.result = 0;
    Require(CheckStaticModelSamplerSupport(&api, &backend, scene).Succeeded() && backend.calls == 8,
        "准确能力声明才可放行非默认 sampler，查询本身不构成产品提交证明");

    auto legacy = scene;
    for (const bool explicitFlags : {false, true}) {
        if (explicitFlags) legacy.layerSamplerFlags.assign(legacy.layers.size(), 3);
        else legacy.layerSamplerFlags.clear();
        const auto description = DescribeModelScene(legacy);
        const auto calls = backend.calls;
        Require(description.validStaticSamplers && description.samplerFlags.empty()
            && description.StaticScene() == &description.scene && description.scene.structSize == sizeof(War3RenderModelSceneV2)
            && CheckStaticModelSamplerSupport(nullptr, nullptr, legacy).Succeeded()
            && CheckStaticModelSamplerSupport(&api, &backend, legacy).Succeeded() && backend.calls == calls,
            "空或全3的 legacy 场景必须保留旧 descriptor，无需新 API 能力");
    }
    for (const bool wrongLength : {false, true}) {
        auto invalid = scene;
        if (wrongLength) invalid.layerSamplerFlags.pop_back();
        else invalid.layerSamplerFlags.front() = 4;
        const auto description = DescribeModelScene(invalid);
        const auto status = CheckStaticModelSamplerSupport(&api, &backend, invalid);
        Require(!description.validStaticSamplers && description.StaticScene() == nullptr && !status.Succeeded()
            && !status.diagnostics.empty() && status.diagnostics.front().code == "WORLD_STATIC_SAMPLER_INVALID",
            "损坏的宿主 sampler 数组必须在 descriptor 和 capability 两条入口都明确拒绝");
    }
}

// 地形编辑后替换悬崖烘焙网格：其余模型整体平移，实例与请求对应关系不变；贴图须已载入。
void TestReplaceBaked() {
    Files files;
    files.files[Key("Units\\Crate.mdx")] = MdxTest::BuildTestMdx();
    files.files[Key("Textures\\Crate.blp")] = Blp(4, 4, 90);
    files.files[Key("Cliff\\A.blp")] = Blp(2, 2, 10);
    const std::vector<ModelInstanceRequest> requests{At("Units\\Crate.mdx", 0, 100)};
    const std::vector<BakedModelMesh> original{Triangle(0, "Cliff\\A.blp")};
    auto built = BuildWorldModelScene(requests, files.Reader(), {}, original);
    Require(built.Succeeded() && built.value->models.size() == 2 && built.value->instanceRequests[0] == kBakedInstance, "烘焙场景");
    auto scene = *built.value;
    const auto crateVertices = std::vector<War3RenderModelVertexV2>(scene.vertices.begin() + 3, scene.vertices.end());
    // 两块新网格（同一贴图、路径大小写不同）。
    const std::vector<BakedModelMesh> twice{Triangle(500, "cliff/a.BLP"), Triangle(600, "Cliff\\A.blp")};
    Require(ReplaceBakedMeshes(scene, twice), "替换为两块网格");
    Require(scene.models.size() == 2 && scene.models[0].meshCount == 2 && scene.vertices.size() == 6 + crateVertices.size()
        && scene.vertices[0].x == 500 && scene.meshes[2].baseVertex == 6, "新烘焙网格在前、其余平移");
    Require(std::equal(crateVertices.begin(), crateVertices.end(), scene.vertices.begin() + 6,
        [](const auto& a, const auto& b) { return a.x == b.x && a.z == b.z; }), "摆放模型几何不变");
    Require(scene.instanceRequests == std::vector<std::uint32_t>{kBakedInstance, 0} && scene.instances[1].model == 1, "实例对应不变");
    Require(scene.modelBounds[0][0] == 500 && scene.modelBounds[0][3] == 620, "烘焙包围盒更新");
    // 只读失败不能留下半次替换，也不能突破渲染器的网格、图层或实例预算。
    WorldModelLimits limited;
    limited.maxMeshes = 2;
    RequireBakedReplaceRejected(scene, twice, limited);
    limited = {};
    limited.maxLayers = 2;
    RequireBakedReplaceRejected(scene, twice, limited);
    limited = {};
    limited.maxInstances = 1;
    RequireBakedReplaceRejected(scene, twice, limited);
    auto malformed = scene;
    malformed.models[0].meshCount = static_cast<std::uint32_t>(malformed.meshes.size() + 1);
    RequireBakedReplaceRejected(malformed, original);
    malformed = scene;
    malformed.meshes[0].firstIndex = 1;
    RequireBakedReplaceRejected(malformed, original);
    // 缺贴图不修改；清空后烘焙模型移除，其余模型序号前移。
    RequireBakedReplaceRejected(scene, std::vector<BakedModelMesh>{Triangle(0, "Cliff\\Missing.blp")});
    Require(ReplaceBakedMeshes(scene, {}), "清空烘焙网格");
    Require(scene.models.size() == 1 && scene.instances.size() == 1 && scene.instances[0].model == 0
        && scene.instanceRequests[0] == 0 && scene.meshes[0].baseVertex == 0 && scene.vertices.size() == crateVertices.size(), "移除烘焙模型");
    // 原来没有烘焙模型时插入到最前。
    Require(ReplaceBakedMeshes(scene, original) && scene.models.size() == 2 && scene.instances[1].model == 1
        && scene.instanceRequests[0] == kBakedInstance, "插入烘焙模型");
    const auto originalUntextured = scene.untexturedLayers;
    Require(ReplaceBakedMeshes(scene, std::vector<BakedModelMesh>{Triangle(0, "")})
        && scene.untexturedLayers == originalUntextured + 1, "无贴图烘焙图层计数");
    Require(ReplaceBakedMeshes(scene, original) && scene.untexturedLayers == originalUntextured,
        "恢复贴图后图层计数恢复");
    Require(!DescribeModelScene(scene).mips.empty(), "替换后仍可描述");
}

void TestBakedSamplerFlagsAndSnapshotRefresh() {
    Files files;
    files.files[Key("Original\\Sampler.mdx")] = RigidTestModel();
    files.files[Key("Textures\\Crate.blp")] = Blp(4, 4, 90);
    files.files[Key(TeamColorTexture(0))] = Blp(2, 2, 10);
    const std::array requests{At("Original\\Sampler.mdx", 0, 100)};
    const std::vector<BakedModelMesh> original{Triangle(0, "Textures\\Crate.blp", 0),
        Triangle(20, "textures/crate.BLP", 1), Triangle(40, "Textures\\Crate.blp", 2),
        Triangle(60, "Textures\\Crate.blp", 3)};
    auto built = BuildWorldModelScene(requests, files.Reader(), {}, original);
    Require(built.Succeeded() && built.value->models.size() == 2 && built.value->models[0].meshCount == 4
        && built.value->layers.size() == 6 && !built.value->layerSamplerFlags.empty(), "sampler 烘焙场景构建");
    auto& scene = *built.value;
    const WorldModelScene& view = scene;
    Require(EffectiveSamplerFlags(view) == std::vector<std::uint32_t>{0, 1, 2, 3, 0, 3},
        "烘焙前缀与普通模型后缀必须分别保留实际来源 sampler");
    Require(view.textures.size() == 2 && files.reads[Key("Textures\\Crate.blp")] == 1,
        "不同 sampler 仍共用一次读取和一份真实 BLP，不能复制贴图或伪造替代纹理");
    TestStaticSamplerDescriptorAndCapabilities(view);
    for (std::size_t i = 0; i < original.size(); ++i)
        Require(view.layers[i].textureIndex == view.layers[5].textureIndex && view.layers[i].filterMode == 0
            && view.layers[i].flags == 0 && view.layers[i].alpha == 1,
            "sampler 元数据独立于旧图层 ABI 字段，同一贴图身份保持不变");

    // 合法非默认后缀模拟已发布静态快照，替换烘焙前缀时不得改写它。
    scene.layerSamplerFlags.back() = 1;
    const auto snapshot = scene;
    const auto snapshotReceipt = BakedSceneReceipt(snapshot);
    Require(view.layerSamplerFlags.SharesStorageWith(snapshot.layerSamplerFlags), "快照应共享只读 sampler 存储");
    const std::vector<BakedModelMesh> replacement{Triangle(500, "textures/crate.BLP", 2), Triangle(600, "Textures\\Crate.blp")};
    Require(ReplaceBakedMeshes(scene, replacement)
        && EffectiveSamplerFlags(view) == std::vector<std::uint32_t>{2, 3, 0, 1},
        "前缀缩短后 sampler 与图层同步平移，默认烘焙仍为3，显式后缀保持原值");
    Require(BakedSceneReceipt(snapshot) == snapshotReceipt
        && !view.layerSamplerFlags.SharesStorageWith(snapshot.layerSamplerFlags)
        && view.textures.SharesStorageWith(snapshot.textures), "成功刷新分离 sampler，旧快照和共享真实贴图保持不变");
    auto moved = requests; moved[0].position[0] = 200;
    const auto beforeMove = scene;
    Require(ReplaceModelInstances(scene, requests, moved) && view.instances.back().transform[3] == 200
        && view.layerSamplerFlags.SharesStorageWith(beforeMove.layerSamplerFlags)
        && EffectiveSamplerFlags(view) == std::vector<std::uint32_t>{2, 3, 0, 1},
        "仅刷新实例不能丢失、重建或改写 sampler 元数据");

    RequireBakedReplaceRejected(scene, std::vector<BakedModelMesh>{Triangle(700, "Textures\\Crate.blp", 0),
        Triangle(800, "Textures\\Crate.blp", 4)});
    auto malformed = scene; malformed.layerSamplerFlags.pop_back();
    RequireBakedReplaceRejected(malformed, replacement);
    malformed = scene; malformed.layerSamplerFlags.back() = 4;
    RequireBakedReplaceRejected(malformed, replacement);
    WorldModelLimits limited; limited.maxLayers = 3;
    RequireBakedReplaceRejected(scene, replacement, limited);
    limited = {}; limited.maxVertices = 9;
    RequireBakedReplaceRejected(scene, replacement, limited);

    // 空数组是明确的旧 repeat/repeat，而不是省略现有图层或猜测来源 TEXS。
    auto legacy = scene; legacy.layerSamplerFlags.clear();
    Require(ReplaceBakedMeshes(legacy, std::vector<BakedModelMesh>{Triangle(0, "Textures\\Crate.blp", 0)})
        && EffectiveSamplerFlags(legacy) == std::vector<std::uint32_t>{0, 3, 3}, "旧场景补齐后缀 sampler=3");
    Require(ReplaceBakedMeshes(legacy, {}) && EffectiveSamplerFlags(legacy) == std::vector<std::uint32_t>{3, 3},
        "清空烘焙前缀后 sampler 后缀仍与保留的普通图层对齐");
    Require(ReplaceBakedMeshes(legacy, std::vector<BakedModelMesh>{Triangle(0, "Textures\\Crate.blp")})
        && EffectiveSamplerFlags(legacy) == std::vector<std::uint32_t>{3, 3, 3}, "再次插入采用默认 sampler=3");

    files.reads.clear();
    const std::vector<BakedModelMesh> invalid{original[0], Triangle(0, "Textures\\Crate.blp", 4)};
    const auto rejected = BuildWorldModelScene(requests, files.Reader(), {}, invalid);
    Require(!rejected.Succeeded() && !rejected.value && !rejected.diagnostics.empty()
        && rejected.diagnostics.front().code == "WORLD_BAKED_MESH_INVALID" && files.reads.empty(),
        "未知烘焙 sampler 必须在所有模型/贴图读取前拒绝，不能发布部分场景");

    BakedModelMesh defaultMesh; Require(defaultMesh.samplerFlags == 3, "旧调用者默认保留 repeat/repeat 合同");
    const std::vector<BakedModelMesh> implicitDefault{Triangle(0, "Textures\\Crate.blp")};
    const std::vector<BakedModelMesh> explicitDefault{Triangle(0, "Textures\\Crate.blp", 3)};
    const auto implicit = BuildWorldModelScene(requests, files.Reader(), {}, implicitDefault);
    const auto explicitFlags = BuildWorldModelScene(requests, files.Reader(), {}, explicitDefault);
    Require(implicit.Succeeded() && explicitFlags.Succeeded()
        && BakedSceneReceipt(*implicit.value) == BakedSceneReceipt(*explicitFlags.value),
        "默认与显式3产生相同旧几何、图层字段、实例、贴图与 sampler 语义");
}

void TestStaticSamplerMetadataBudget() {
    Files files; files.files[Key("Original\\Tiny.blp")] = Blp(2, 2, 17);
    const std::vector<BakedModelMesh> baked{Triangle(0, "Original\\Tiny.blp", 0), Triangle(20, "Original\\Tiny.blp", 1),
        Triangle(40, "Original\\Tiny.blp", 2), Triangle(60, "Original\\Tiny.blp", 3), Triangle(80, "Original\\Tiny.blp", 0)};
    const auto probe = BuildWorldModelScene({}, files.Reader(), {}, baked);
    Require(probe.Succeeded() && probe.value->textures.size() == 1 && probe.value->textures[0].rgba.size() == 16,
        "紧预算夹具只允许一张真实2x2 RGBA贴图，不制造大纹理或虚拟内存");
    const auto& baseline = *probe.value;
    const auto tailBytes = sizeof(War3RenderStaticSamplerModelSceneV2) - sizeof(War3RenderModelSceneV2);
    const auto exact = ModelTextureRgbaBytes(baseline.textures[0])
        + baseline.layerSamplerFlags.capacity() * sizeof(std::uint32_t) + tailBytes;
    WorldModelLimits limits; limits.maxTotalTextureBytes = exact - 1;
    const auto rejected = BuildWorldModelScene({}, files.Reader(), limits, baked);
    Require(!rejected.Succeeded() && !rejected.value && !rejected.diagnostics.empty()
        && rejected.diagnostics.front().code == "WORLD_STATIC_SAMPLER_BUDGET",
        "真实像素可装入但 sampler 实际分配容量超出一字节时必须拒绝");
    limits.maxTotalTextureBytes = exact;
    const auto accepted = BuildWorldModelScene({}, files.Reader(), limits, baked);
    Require(accepted.Succeeded() && EffectiveSamplerFlags(*accepted.value) == std::vector<std::uint32_t>{0, 1, 2, 3, 0},
        "像素、sampler容量和描述符增量恰好达到原预算时应接纳");

    const std::vector<BakedModelMesh> replacement{Triangle(200, "Original\\Tiny.blp", 2)};
    auto measured = baseline;
    Require(ReplaceBakedMeshes(measured, replacement), "测量真实替换候选的 sampler 分配容量");
    const auto replaceExact = ModelTextureRgbaBytes(baseline.textures[0])
        + (baseline.layerSamplerFlags.capacity() + std::as_const(measured).layerSamplerFlags.capacity()) * sizeof(std::uint32_t)
        + tailBytes;
    auto candidate = baseline;
    limits.maxTotalTextureBytes = replaceExact - 1;
    RequireBakedReplaceRejected(candidate, replacement, limits);
    limits.maxTotalTextureBytes = replaceExact;
    Require(ReplaceBakedMeshes(candidate, replacement, limits)
        && EffectiveSamplerFlags(candidate) == std::vector<std::uint32_t>{2},
        "替换时旧快照和新候选 sampler 的实际容量必须一起计入原预算");
}

void TestPreciseAnimatedPicking() {
    // 原创单三角形、同资产两实例；只测完成帧输入的 CPU 数学，不声称 GPU 或游戏已完成。
    AnimatedModelScene scene;
    scene.sceneVersion = 11; scene.generation = 12; scene.documentRevision = 13;
    scene.vertices = {{{0, 0, 0, 0, 0, 1, 0, 0}, 0}, {{2, 0, 0, 0, 0, 1, 0, 0}, 0}, {{0, 2, 0, 0, 0, 1, 0, 0}, 0}};
    scene.geometry.indices = {0, 1, 2};
    scene.geometry.layers = {{WAR3_RENDER_MODEL_NO_TEXTURE, 0, 0, 1}};
    scene.geometry.meshGeosets = {0};
    scene.meshes = {{{0, 3, 0, 0, 1}, 0, 1}};
    scene.models = {{{0, 1}, 1}};
    scene.pickMeshGroups = {{0, 1}}; scene.pickGroupBounds = {{0, {0, 0, 0, 2, 2, 0}}};
    auto source = std::make_shared<MdxModel>(); source->geosets.resize(1);
    scene.pickModels = {{source, std::make_shared<std::vector<std::uint32_t>>()}};
    scene.specs.resize(2);
    AnimationFrame frame;
    frame.sceneVersion = 11; frame.contentVersion = 14; frame.documentRevision = 13;
    frame.groups.resize(2); frame.alphas = {1, 1}; frame.states.resize(2);
    ModelPickBinding binding{13, 12, 11, 14, 15, 16, {17, 18, 19, 20}};
    ModelPickVisibilitySnapshot visibility{binding, {1, 1}, {1, 1}};
    for (std::uint32_t index = 0; index < 2; ++index) {
        scene.specs[index].identity = {index + 1, 7}; frame.states[index].identity = scene.specs[index].identity;
        War3RenderSkinnedModelInstanceV2 instance{};
        instance.instance.transform[0] = instance.instance.transform[5] = instance.instance.transform[10] = 1;
        std::fill(std::begin(instance.instance.color), std::end(instance.instance.color), 1.0f);
        instance.firstGroup = index; instance.firstAlpha = index;
        frame.instances.push_back(instance); scene.geometry.instances.push_back(instance.instance);
        scene.geometry.instanceRequests.push_back(index);
        frame.groups[index].values[0] = frame.groups[index].values[4] = frame.groups[index].values[8] = 1;
        frame.groups[index].values[9] = static_cast<float>(index * 5);
        AnimationPoseLink pose;
        pose.identity = scene.specs[index].identity; pose.asset = {1, 7}; pose.evaluationEpoch = 15; pose.poseVersion = 21 + index;
        pose.requestIndex = pose.renderedInstance = index; pose.firstGroup = pose.firstAlpha = index;
        pose.groupCount = pose.alphaCount = 1; pose.animated = true; frame.poses.push_back(pose);
        frame.pickBounds.push_back({{double(index * 5), 0, -1, double(index * 5 + 2), 2, 1}, true, false});
    }
    const auto query = [&](double x, double y, const ModelPickLimits& limits = {}) {
        auto result = PickAnimatedModelInstances(scene, frame, visibility, binding, {{x, y, 10}, {0, 0, -1}}, 20, limits);
        Require(result.Succeeded(), "原创已绑定姿势点选查询成功"); return std::move(*result.value);
    };
    Require(query(1.75, 1.75).hits.empty(), "AABB 相交但三角形外空白不命中");
    auto result = query(0.25, 0.25);
    Require(result.hits.size() == 1 && result.hits[0].identity == scene.specs[0].identity
        && result.hits[0].grade == 8 && result.hits[0].triangle == 0u, "真三角形精确命中并带身份");
    result = query(5.25, 0.25);
    Require(result.hits.size() == 1 && result.hits[0].identity == scene.specs[1].identity, "同一静态网格使用各实例当前组姿势");
    visibility.visible[0] = 0; Require(query(0.25, 0.25).hits.empty(), "编辑器隐藏不参与命中"); visibility.visible[0] = 1;
    visibility.selectable[0] = 0; Require(query(0.25, 0.25).hits.empty(), "锁定不可选不参与命中"); visibility.selectable[0] = 1;
    source->geosets[0].selectionFlags = 4; Require(query(0.25, 0.25).hits.empty(), "原网格不可选标志保持");
    source->geosets[0].selectionFlags = 0;

    MdxCollisionShape shape;
    shape.sourceIndex = 0; shape.sourceType = 0; shape.nodeId = 9; shape.vertexCount = 2;
    shape.vertices = {{{-1, -1, -1}, {1, 1, 1}}}; shape.layout = MdxCollisionLayout::BoxVertices24;
    shape.support = MdxCollisionSupport::Supported; source->collisionShapes = {shape};
    scene.pickModels[0].collisionNodes = std::make_shared<std::vector<std::uint32_t>>(1, 0);
    frame.collisionMatrices.resize(2);
    for (std::uint32_t index = 0; index < 2; ++index) {
        frame.poses[index].collisionMatrices = {index, 1};
        auto& matrix = frame.collisionMatrices[index]; matrix.available = true;
        // 节点绕 Z 90°，再各轴缩放 2/1/3；保持列式与实例行式变换区别。
        matrix.matrix.values[1] = 2; matrix.matrix.values[3] = -1; matrix.matrix.values[8] = 3;
        frame.pickBounds[index] = {{-1, -2, -3, 7, 2, 3}, true, false};
    }
    frame.alphas[1] = 0;
    result = query(0.25, 0.25);
    Require(result.hits.size() == 2 && result.hits[0].grade == 8 && result.hits[1].grade == 4
        && result.hits[0].distance > result.hits[1].distance, "网格 grade8 优先于更近的碰撞 grade4");
    visibility.visible[1] = 0; frame.alphas[0] = 0;
    result = query(0.5, 0);
    Require(result.hits.size() == 1 && result.hits[0].collisionShape == 0u && std::abs(result.hits[0].distance - 7) < 1e-9,
        "节点旋转和非均匀缩放后的真盒命中");
    auto& sphere = source->collisionShapes[0]; sphere.sourceType = 2; sphere.vertexCount = 1;
    sphere.vertices[0] = {0, 0, 0}; sphere.radius = 1; sphere.hasRadius = true; sphere.layout = MdxCollisionLayout::SphereCenterRadius16;
    result = query(0.5, 0);
    Require(result.hits.size() == 1 && std::abs(result.hits[0].distance - (10 - 3 * std::sqrt(0.75))) < 1e-9,
        "非均匀缩放球按局部真球求交而非世界包围球");
    Require(query(0.9, 1.9).hits.empty(), "椭球包围盒内的实际球外空白不命中");
    sphere.support = MdxCollisionSupport::LayoutAmbiguous; frame.alphas[0] = 1;
    for (const auto type : {1u, 3u}) {
        sphere.sourceType = type;
        sphere.layout = type == 1 ? MdxCollisionLayout::UnverifiedType1Vertices24 : MdxCollisionLayout::UnverifiedType3VerticesRadius28;
        result = query(0.25, 0.25);
        Require(result.hits.size() == 1 && result.hits[0].grade == 8 && !result.collisionComplete && !result.issues.empty()
            && result.issues[0].collisionShape == 0u, "未知碰撞保留来源诊断且不取消独立网格命中");
    }
    source->collisionShapes.clear(); frame.collisionMatrices.clear();
    for (auto& pose : frame.poses) pose.collisionMatrices = {};
    scene.geometry.indices = {0, 1, 2, 0, 1, 2}; scene.meshes[0].mesh.indexCount = 6;
    ModelPickLimits limits; limits.maxTriangles = 1;
    result = query(0.25, 0.25, limits);
    Require(result.budgetExceeded && !result.meshComplete, "三角形预算不足显式 incomplete");
    auto stale = binding; ++stale.view.frameCount;
    Require(!PickAnimatedModelInstances(scene, frame, visibility, stale, {{0.25, 0.25, 10}, {0, 0, -1}}, 20).Succeeded(),
        "旧完成帧身份不被重新使用");
    ++frame.contentVersion;
    Require(!PickAnimatedModelInstances(scene, frame, visibility, binding, {{0.25, 0.25, 10}, {0, 0, -1}}, 20).Succeeded(),
        "不同姿势内容版本拒绝查询");
}

void TestPreparedAnimatedPicking() {
    Files files; auto bytes = RigidTestModel();
    const std::array<char, 4> pivotTag{'P', 'I', 'V', 'T'};
    const auto pivot = std::search(bytes.begin(), bytes.end(), pivotTag.begin(), pivotTag.end(),
        [](std::byte left, char right) { return left == static_cast<std::byte>(right); });
    Require(pivot != bytes.end(), "沿已有原创模型扩展一个碰撞节点轴心");
    const auto offset = static_cast<std::size_t>(pivot - bytes.begin()); std::uint32_t length = 0;
    std::memcpy(&length, bytes.data() + offset + 4, 4);
    bytes.insert(bytes.begin() + offset + 8 + length, 12, std::byte{0}); length += 12;
    std::memcpy(bytes.data() + offset + 4, &length, 4);
    MdxTest::Writer w; w.bytes = std::move(bytes); w.Tag("CLID"); const auto chunk = w.Begin();
    const auto node = w.Begin(); w.Fixed("PickBox", 80); w.U32(2); w.U32(1); w.U32(0); w.EndInclusive(node);
    w.U32(0); for (const auto value : {0.0f, 0.0f, 0.0f, 10.0f, 10.0f, 5.0f}) w.F32(value); w.EndChunk(chunk);
    files.files[Key("Units\\Rigid.mdx")] = std::move(w.bytes);
    files.files[Key("ReplaceableTextures\\TeamColor\\TeamColor00.blp")] = Blp(2, 2, 64);
    files.files[Key("Textures\\Crate.blp")] = Blp(2, 2, 96);
    AnimationInstanceSpec spec; spec.identity = {1, 7}; spec.hostKey = "original-pick-row";
    spec.model = At("Units\\Rigid.mdx", 0, 0); spec.elapsedMs = 500;
    const auto created = ModelAnimationService::Create({7, 33, "original-pick-fixture"}, std::span(&spec, 1),
        [&](std::string_view path, std::uint64_t) {
            auto read = files.Reader()(path);
            if (!read.Succeeded()) { Result<AnimationResource> failed; failed.diagnostics = std::move(read.diagnostics); return failed; }
            return Result<AnimationResource>::Success({std::move(*read.value), "synthetic", std::string(64, 'a')});
        });
    if (!created.Succeeded()) throw std::runtime_error("实际动画服务加载原创模型：" + (created.diagnostics.empty()
        ? std::string("没有诊断") : created.diagnostics.front().code + ": " + created.diagnostics.front().message));
    const auto service = *created.value; const auto prepared = service->Prepare(nullptr);
    if (!prepared.Succeeded()) throw std::runtime_error("实际动画服务生成点选帧：" + (prepared.diagnostics.empty()
        ? std::string("没有诊断") : prepared.diagnostics.front().code + ": " + prepared.diagnostics.front().message));
    const auto scene = service->Scene(); const auto& frame = **prepared.value;
    ModelPickBinding binding{33, 7, scene->sceneVersion, frame.contentVersion, 15, 16, {17, 18, 19, 20}};
    const auto picked = PickAnimatedModelInstances(*scene, frame, {binding, {1}, {1}}, binding, {{6, 1, 20}, {0, 0, -1}}, 40);
    Require(picked.Succeeded() && picked.value->hits.size() == 1 && picked.value->hits[0].identity == spec.identity
        && picked.value->hits[0].grade == 8 && frame.pickBounds[0].available && frame.collisionMatrices.size() == 1
        && frame.collisionMatrices[0].available && std::abs(frame.collisionMatrices[0].matrix.values[9] - 5) < 1e-5,
        "Create→Prepare 生成实际 mesh 映射、当前盒与 owned 碰撞节点矩阵后精确命中");
}


std::vector<std::byte> SourceMipFixture(std::uint32_t width, std::uint32_t height, unsigned count=16) {
    std::vector<std::byte> bytes(1180);
    const auto put=[&](std::size_t at,std::uint32_t value) {
        for (unsigned i=0;i<4;++i) bytes[at+i]=static_cast<std::byte>((value>>(i*8))&255);
    };
    bytes[0]=std::byte{'B'};bytes[1]=std::byte{'L'};bytes[2]=std::byte{'P'};bytes[3]=std::byte{'1'};
    put(4,1);put(12,width);put(16,height);put(156,0x00ff0000);put(160,0x0000ff00);
    for (unsigned level=0;level<count;++level) {
        const auto w=std::max(1u,width>>level),h=std::max(1u,height>>level);
        put(28+4*level,static_cast<std::uint32_t>(bytes.size()));put(92+4*level,w*h);
        bytes.insert(bytes.end(),w*h,static_cast<std::byte>(level ? 1 : 0));
        if (w==1 && h==1) break;
    }
    return bytes;
}

void TestModelSourceMipsAndLegacyRoutes() {
    const auto source=SourceMipFixture(3,5);
    auto decoded=DecodeModelTexture("Original\\Source.blp",source,512,72);
    Require(decoded.Succeeded() && ModelTextureValid(*decoded.value,512) && ModelTextureRgbaBytes(*decoded.value)==72
        && decoded.value->additionalMips.size()==2 && decoded.value->rgba[0]==255
        && decoded.value->additionalMips[0].rgba[1]==std::byte{255}
        && decoded.value->mipOrigin==ModelTextureMipOrigin::BlpSource,"生产解码未保留故意不同的红top/绿原低级");
    Require(ModelTextureStorageBytes(*decoded.value)>ModelTextureRgbaBytes(*decoded.value),"实际owned计费遗漏lower容器/容量");
    Require(!DecodeModelTexture("Original\\Source.blp",source,512,71).Succeeded(),"预算不足发布了trimmed原链");
    auto topOnly=DecodeModelTexture("Original\\OnlyTop.blp",SourceMipFixture(3,5,1),512);
    Require(topOnly.Succeeded() && topOnly.value->additionalMips.empty()
        && topOnly.value->mipOrigin==ModelTextureMipOrigin::GeneratedFromTop
        && topOnly.value->mipStopReason==Image::BlpMipChainStopReason::UndeclaredMip,"明确无低级的源没有保留旧生成路径");
    const auto normalTail=DecodeModelTexture("Original\\NormalTail.blp",SourceMipFixture(3,5,2),512);
    Require(normalTail.Succeeded() && normalTail.value->additionalMips.size()==1
        && normalTail.value->mipStopReason==Image::BlpMipChainStopReason::UndeclaredMip
        && normalTail.value->mipDiagnostics.empty(),"正常缺失尾级被补齐或误判为坏素材");
    std::vector<std::byte> tga(18+2*2*3); tga[2]=std::byte{2}; tga[12]=tga[14]=std::byte{2};
    tga[16]=std::byte{24}; tga[17]=std::byte{0x20};
    for (std::size_t pixel=18;pixel<tga.size();pixel+=3) tga[pixel+2]=std::byte{255};
    const auto legacyTga=DecodeModelTexture("Original\\Legacy.tga",tga,512);
    Require(legacyTga.Succeeded() && legacyTga.value->additionalMips.empty() && legacyTga.value->rgba[0]==255,
        "普通TGA没有保持原DecodeTexture路径");
    for (const auto dimensions : {std::array<std::uint32_t,3>{4096,1,512},{128,1,32},{1024,1,2048}}) {
        const auto image=DecodeModelTexture("Original\\Derived.blp",SourceMipFixture(dimensions[0],dimensions[1]),dimensions[2]);
        Require(image.Succeeded() && image.value->additionalMips.empty()
            && image.value->width==std::min(dimensions[0],dimensions[2]) && image.value->rgba[0]==255
            && image.value->rgba[1]==0 && image.value->sourceWidth==dimensions[0],
            "新generic/512门退回旧大源、caller小上限或2048消费者能力");
    }
    const auto put=[](std::vector<std::byte>& bytes,std::size_t at,std::uint32_t value) {
        for (unsigned i=0;i<4;++i) bytes[at+i]=static_cast<std::byte>((value>>(i*8))&255);
    };
    auto damaged=source;put(damaged,32,static_cast<std::uint32_t>(damaged.size()+1));
    Require(!DecodeModelTexture("Original\\BadMip1.blp",damaged,512).Succeeded(),"只剩mip0的坏源偷走GenerateMips");
    damaged=source;put(damaged,32,0);put(damaged,96,0);
    Require(!DecodeModelTexture("Original\\Hole.blp",damaged,512).Succeeded(),"中间空洞误认为正常只有top");
    damaged=source;put(damaged,36,static_cast<std::uint32_t>(damaged.size()+1));
    auto prefix=DecodeModelTexture("Original\\BadMip2.blp",damaged,512);
    Require(prefix.Succeeded() && prefix.value->additionalMips.size()==1 && !prefix.value->mipDiagnostics.empty(),
        "明确坏范围的多级前缀未保留来源诊断");
    const auto top=TestFixtures::MakeTinyGrayJpeg(16,16),small=TestFixtures::MakeTinyGrayJpeg(8,8),last=TestFixtures::MakeTinyGrayJpeg(4,4);
    const auto jpeg=[&](std::vector<unsigned char> final) {
        std::vector<std::byte> bytes(160);bytes[0]=std::byte{'B'};bytes[1]=std::byte{'L'};bytes[2]=std::byte{'P'};bytes[3]=std::byte{'1'};
        put(bytes,12,16);put(bytes,16,16);
        const std::array<std::vector<unsigned char>,3> levels{top.bytes,small.bytes,std::move(final)};
        for (std::size_t i=0;i<levels.size();++i) {
            put(bytes,28+4*i,static_cast<std::uint32_t>(bytes.size()));put(bytes,92+4*i,static_cast<std::uint32_t>(levels[i].size()));
            for (const auto value:levels[i]) bytes.push_back(static_cast<std::byte>(value));
        }
        return bytes;
    };
    const auto formatPrefix=DecodeModelTexture("Original\\KnownFormat.blp",jpeg(small.bytes),512);
    Require(formatPrefix.Succeeded() && formatPrefix.value->additionalMips.size()==1,
        "可定位SOF尺寸损坏没有保留多级前缀");
    auto entropy=last.bytes;entropy.erase(entropy.begin()+static_cast<std::ptrdiff_t>(last.entropyOffset));
    Require(!DecodeModelTexture("Original\\CoreFailure.blp",jpeg(entropy),512).Succeeded(),
        "底层Core失败被按message猜成坏素材前缀发布");
    std::vector<ModelTextureRgba> textures{*decoded.value,*prefix.value,*topOnly.value};
    auto descriptors=DescribeModelTextures(textures);
    Require(descriptors.chains.size()==3 && descriptors.mips.size()==6 && descriptors.chains[0].mipCount==3
        && descriptors.chains[0].maxSampleMip==2 && descriptors.chains[0].mips==descriptors.mips.data()
        && descriptors.chains[1].mips==descriptors.mips.data()+3 && descriptors.chains[2].mips==descriptors.mips.data()+5,
        "多纹理描述符没有稳定指向各自完整链区间");
    auto moved=std::move(descriptors);
    Require(moved.chains[0].mips==moved.mips.data(),"描述符owner移动令mip指针悬空");
    Files files;files.files[Key("Original\\Source.blp")]=source;
    const std::vector<BakedModelMesh> baked{Triangle(0,"Original\\Source.blp")};
    auto scene=BuildWorldModelScene({},files.Reader(),{},baked);
    Require(scene.Succeeded() && scene.value->textures.size()==1 && scene.value->textures[0].additionalMips.size()==2,
        "实际baked模型Builder没有消费生产原始链");
    auto cached=std::make_shared<ModelTextureRgba>(*decoded.value); cached->rgba[0]=19;
    const ModelDecodedTextureProvider cache=[&](std::string_view path,std::span<const std::byte> bytes,std::uint32_t maximum) {
        Require(Key(path)==Key(cached->path) && maximum==512 && std::equal(bytes.begin(),bytes.end(),source.begin(),source.end()),
            "decoded cache bypassed the current actual source bytes or caller policy");
        return cached;
    };
    auto cachedScene=BuildAnimatedModelGeometry({},files.Reader(),{},{},baked,cache);
    Require(cachedScene.Succeeded() && cachedScene.value->textures[0].rgba[0]==19,
        "valid current-policy complete source chain was not reused");
    ++cached->mipPolicyVersion;
    cachedScene=BuildAnimatedModelGeometry({},files.Reader(),{},{},baked,cache);
    Require(cachedScene.Succeeded() && cachedScene.value->textures[0].rgba[0]==255,
        "changed mip policy reused an obsolete decoded cache entry");
    cached->mipPolicyVersion=ModelTextureMipPolicyVersion;
    cached->mipStopReason=Image::BlpMipChainStopReason::WorkingBudget;
    cachedScene=BuildAnimatedModelGeometry({},files.Reader(),{},{},baked,cache);
    Require(cachedScene.Succeeded() && cachedScene.value->textures[0].rgba[0]==255,
        "cached budget-trimmed chain bypassed full source-chain revalidation");
    cached->mipStopReason=Image::BlpMipChainStopReason::Complete;
    WorldModelLimits shortCacheQuota; shortCacheQuota.maxTotalTextureBytes=71;
    cachedScene=BuildAnimatedModelGeometry({},files.Reader(),{},shortCacheQuota,baked,cache);
    Require(cachedScene.Succeeded() && cachedScene.value->textures.empty() && HasWarning(*cachedScene.value,"WORLD_MODEL_TEXTURE_BUDGET"),
        "cache reused all mip levels without charging the current complete-chain quota");
}

} // namespace

int main() {
    try {
        TestBuild();
        TestModelSourceMipsAndLegacyRoutes();
        TestGhostUsesSharedAssetAndBlendedVariant();
        TestMaterialUnfoggedFlags();
        TestLimitsAndInvalid();
        TestPicking();
        TestPreciseAnimatedPicking();
        TestPreparedAnimatedPicking();
        TestReplaceBaked();
        TestBakedSamplerFlagsAndSnapshotRefresh();
        TestStaticSamplerMetadataBudget();
        TestRigidPoseScene();
        TestNoControllerStaticPreview();
        std::cout << "world model scene: variants, dedupe, transforms, textures, budgets passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
