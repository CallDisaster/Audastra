#pragma once
#include "../src/Render/Tools/ModelPreviewReplacement.h"
#include "AnimationParticleContinuityTests.h"
#include "War3MapCore/Hash.h"
#include <cctype>

namespace ModelPreviewReplacementTest {
using namespace War3Map;
using namespace War3Map::Render;
using AnimationParticleContinuityTest::Check;
using AnimationParticleContinuityTest::Good;

inline std::string Key(std::string_view path) {
    std::string result(path);
    for (auto& c : result) c = c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

// 只组合既有原创夹具：一个完整三角形geoset、一个明确层、一个TEXS；骨骼与动作原样保留。
inline std::vector<std::byte> Model(std::uint32_t replaceable, std::string_view texturePath) {
    const auto original = AnimationTest::Bytes();
    MdxTest::Writer result; result.Tag("MDLX");
    for (std::size_t at = 4; at + 8 <= original.size();) {
        std::uint32_t size = 0; std::memcpy(&size, original.data() + at + 4, 4);
        Check(size <= original.size() - at - 8, "replacement fixture chunk truncated");
        const std::string_view tag(reinterpret_cast<const char*>(original.data() + at), 4);
        if (tag == "TEXS") {
            result.Tag("TEXS"); const auto chunk = result.Begin();
            result.U32(replaceable); result.Fixed(std::string(texturePath), 260); result.U32(0); result.EndChunk(chunk);
        } else if (tag == "MTLS") {
            result.Tag("MTLS"); const auto chunk = result.Begin(), material = result.Begin();
            result.U32(0); result.U32(0); result.Tag("LAYS"); result.U32(1); const auto layer = result.Begin();
            result.U32(0); result.U32(0); result.U32(0); result.U32(UINT32_MAX); result.U32(0); result.F32(1);
            result.EndInclusive(layer); result.EndInclusive(material); result.EndChunk(chunk);
        } else if (tag == "GEOS") {
            std::uint32_t firstSize = 0; std::memcpy(&firstSize, original.data() + at + 8, 4);
            Check(firstSize <= size, "replacement fixture first geoset truncated");
            result.Tag("GEOS"); result.U32(firstSize); result.Raw(original.data() + at + 8, firstSize);
        } else if (tag != "GEOA") result.Raw(original.data() + at, size + 8);
        at += size + 8;
    }
    return std::move(result.bytes);
}

inline void Options() {
    std::optional<std::string> replacement;
    Check(ModelPreview::MakeRequest("original.mdx", replacement).replaceableTexture.empty(), "omitted replacement guessed a texture");
    ModelPreview::SetReplacementTexture(replacement, "Original\\Replacement.blp");
    Check(replacement == "Original/Replacement.blp", "replacement separators did not normalize");
    const auto retained = replacement;
    bool duplicateRejected = false;
    try { ModelPreview::SetReplacementTexture(replacement, "Other.blp"); } catch (const std::exception&) { duplicateRejected = true; }
    Check(duplicateRejected && replacement == retained, "duplicate replacement modified the prior request");
    for (const auto invalid : {"", "../outside.blp", "folder/../outside.blp", "/absolute.blp", "C:\\absolute.blp",
        "C:relative.blp", "\\\\host\\share.blp", "folder/", ".", "folder/.", "bad\nname.blp"}) {
        std::optional<std::string> value; bool rejected = false;
        try { ModelPreview::SetReplacementTexture(value, invalid); } catch (const std::exception&) { rejected = true; }
        Check(rejected && !value, "invalid replacement published a partial option");
    }
    std::optional<std::string> missing; bool missingRejected = false;
    try { ModelPreview::SetReplacementTexture(missing, std::nullopt); } catch (const std::exception&) { missingRejected = true; }
    Check(missingRejected && !missing, "missing replacement option accepted");
    const auto unicode = std::u8string(u8"原创/贴图.tga");
    const auto unicodeText = std::string(reinterpret_cast<const char*>(unicode.data()), unicode.size());
    std::optional<std::string> unicodeValue; ModelPreview::SetReplacementTexture(unicodeValue, unicodeText);
    Check(unicodeValue == unicodeText, "valid Unicode replacement changed bytes");
}

inline void SourceRouting() {
    const auto image = AnimationParticleContinuityTest::Texture();
    constexpr std::string_view explicitPath = "Original/Replacement.blp";
    const ModelSceneRigidPoseRequest pose{0, 0, 0};
    for (const auto replaceable : {11u, 31u, 0u, 1u, 2u}) {
        const auto bytes = Model(replaceable, replaceable == 0 ? "Original/Source.blp" : "");
        const auto before = Good(Sha256(bytes));
        const auto parsed = Good(ParseMdx(bytes));
        Check(parsed.textures.size() == 1 && parsed.textures[0].replaceableId == replaceable && parsed.textures[0].flags == 0 &&
            parsed.materials.size() == 1 && parsed.materials[0].layers.size() == 1 && parsed.geosets.size() == 1,
            "replacement fixture did not preserve its actual single source layer");
        const auto expected = replaceable >= 3 ? std::string(explicitPath) : replaceable == 0 ? "Original/Source.blp"
            : replaceable == 1 ? TeamColorTexture(0) : TeamGlowTexture(0);
        std::vector<std::string> textureReads;
        const auto reader = [&](std::string_view path) -> Result<std::vector<std::byte>> {
            if (path == "original.mdx") return Result<std::vector<std::byte>>::Success(bytes);
            textureReads.push_back(Key(path));
            if (Key(path) == Key(expected)) return Result<std::vector<std::byte>>::Success(image);
            return Result<std::vector<std::byte>>::Failure("Test", "MISSING", "unexpected replacement source");
        };
        std::optional<std::string> replacement;
        if (replaceable == 11) {
            const auto omitted = ModelPreview::MakeRequest("original.mdx", replacement);
            const auto scene = Good(BuildWorldModelScene(std::span(&omitted, 1), reader, {}, {}, pose));
            Check(scene.untexturedLayers == 1 && scene.textures.empty() && textureReads.empty(),
                "omitted replacement guessed a cliff path or read unrelated assets");
        }
        ModelPreview::SetReplacementTexture(replacement, explicitPath);
        const auto request = ModelPreview::MakeRequest("original.mdx", replacement);
        const auto scene = Good(BuildWorldModelScene(std::span(&request, 1), reader, {}, {}, pose));
        Check(request.modelPath == "original.mdx" && request.replaceableTexture == explicitPath &&
            scene.instances.size() == 1 && scene.models.size() == 1 && scene.meshes.size() == 1 && scene.layers.size() == 1 &&
            !scene.vertices.empty() && !scene.indices.empty() && scene.untexturedLayers == 0 && scene.textures.size() == 1 &&
            scene.layerSamplerFlags.size() == 1 && scene.layerSamplerFlags[0] == 0,
            "shared option mapper did not reach the actual replacement11 pose/texture/sampler scene");
        Check(textureReads == std::vector<std::string>{Key(expected)} && Key(scene.textures[0].path) == Key(expected) &&
            scene.textures[0].width == 1 && scene.textures[0].height == 1 && scene.textures[0].rgba.size() == 4 &&
            scene.textures[0].rgba[0] == 255 && scene.textures[0].rgba[3] == 255,
            "replacement option overrode a TEXS0/team source or did not decode the real original BLP");
        const auto descriptor = DescribeModelScene(scene);
        Check(descriptor.StaticScene() && descriptor.samplerFlags.size() == 1 && descriptor.samplerFlags[0] == 0 &&
            descriptor.chains.size() == 1 && descriptor.chains[0].mipCount == 1,
            "replacement source sampler or actual mip0 descriptor was lost");
        Check(Good(Sha256(bytes)) == before, "replacement mapper modified source MDX bytes");
    }
}

inline void Run() { Options(); SourceRouting(); }
} // namespace ModelPreviewReplacementTest
