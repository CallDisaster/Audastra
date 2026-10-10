#include "War3MapCore/MapData/MapAuxiliary.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace War3Map::MapData;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// 原创微型夹具；只记录公开格式的字段顺序，不使用真实地图或第三方测试数据。
struct Bytes {
    std::vector<std::byte> data;
    void U32(std::uint32_t value) {
        for (unsigned int shift = 0; shift < 32; shift += 8) {
            data.push_back(static_cast<std::byte>((value >> shift) & 0xff));
        }
    }
    void I32(std::int32_t value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void Real(float value) { U32(std::bit_cast<std::uint32_t>(value)); }
    void Raw(std::string_view value) {
        for (const auto character : value) data.push_back(static_cast<std::byte>(character));
    }
    void String(std::string_view value) {
        Raw(value);
        data.push_back(std::byte{0});
    }
};

Bytes RegionFixture() {
    Bytes result;
    result.U32(5);
    result.U32(2);
    for (int i = 0; i < 2; ++i) {
        result.Real(-256.5f + i);
        result.Real(-128.25f);
        result.Real(384.5f);
        result.Real(512.25f);
        result.String(i == 0 ? "TRIGSTR_12" : "第二个区域");
        result.I32(42 + i);
        result.Raw("RAhr");
        result.String(i == 0 ? "gg_snd_Ambient" : "");
        result.Raw(std::string_view("\x12\x34\x56\x78", 4));
    }
    return result;
}

Bytes CameraFixture() {
    Bytes result;
    result.U32(0);
    result.U32(2);
    for (int i = 0; i < 2; ++i) {
        for (float field : {10.0f + i, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f,
            70.0f, 80.0f, 90.0f, 100.0f}) result.Real(field);
        result.String(i == 0 ? "TRIGSTR_20" : "第二镜头");
    }
    return result;
}

Bytes ExtendedCameraFixture(bool ambiguous = false) {
    Bytes result;
    result.U32(0); result.U32(1);
    for (float field : {10.f, 20.f, 30.f, 40.f, 50.f, 60.f, 70.f, 80.f, 90.f, 100.f}) result.Real(field);
    for (int i = 0; i < 3; ++i) {
        result.Real(ambiguous ? std::bit_cast<float>(0x3f010101u) : static_cast<float>(i + 1));
    }
    result.String("ExtendedCamera");
    return result;
}

Bytes SoundFixture() {
    Bytes result;
    result.U32(1);
    result.U32(2);
    for (int i = 0; i < 2; ++i) {
        result.String(i == 0 ? "gg_snd_Ambient" : "gg_snd_OneShot");
        result.String("Sound\\Ambient\\Original.wav");
        result.String("DefaultEAXON");
        result.U32(i == 0 ? 7 : 8);
        result.I32(11);
        result.I32(22);
        result.I32(-1);
        result.Real(1.25f);
        result.Real(0.125f);
        result.I32(8);
        result.I32(10);
        result.Real(100.0f);
        result.Real(2000.0f);
        result.Real(2500.0f);
        result.Real(90.0f);
        result.Real(180.0f);
        result.I32(67);
        result.Real(0.5f);
        result.Real(-0.25f);
        result.Real(0.75f);
    }
    return result;
}

Bytes ExtendedSoundFixture(std::uint32_t version, bool mismatchedAlias = false) {
    Bytes result;
    result.U32(version);
    result.U32(2);
    for (int i = 0; i < 2; ++i) {
        const std::string name = "gg_snd_Extended" + std::to_string(i);
        const std::string path = "Sound\\Extended" + std::to_string(i) + ".wav";
        result.String(name); result.String(path); result.String("DefaultEAXON");
        result.U32(0); result.I32(10); result.I32(20); result.I32(80);
        result.Real(1.0f); result.Real(0.0f); result.I32(1); result.I32(0);
        for (int field = 0; field < 5; ++field) result.Real(100.0f + field);
        result.I32(-1);
        for (int axis = 0; axis < 3; ++axis) result.Real(0.0f);
        result.String(mismatchedAlias && i == 1 ? "old_variable" : name);
        result.String("sound label");
        result.String(path);
        result.I32(-17); result.String("opaque2"); result.I32(-23);
        result.I32(i);
        if (i != 0) result.String("conditional value");
        result.String("opaque6"); result.String("opaque7");
        result.String("Face"); result.String("Group"); result.String("Animation\\Face.fdf");
        if (version == 3) result.I32(-31 - i);
    }
    return result;
}

void RegionsAreComplete() {
    const auto fixture = RegionFixture();
    const auto parsed = ParseRegions(fixture.data);
    Require(parsed.Succeeded() && parsed.value->entries.size() == 2, "region records not parsed");
    Require(parsed.value->version == 5 && parsed.value->originalBytes == fixture.data, "region baseline must be exact");
    const auto& first = parsed.value->entries[0];
    Require(first.left == -256.5f && first.bottom == -128.25f && first.right == 384.5f && first.top == 512.25f,
        "region coordinates are not in file order");
    Require(first.name == "TRIGSTR_12" && first.creationNumber == 42 && first.ambientSound == "gg_snd_Ambient",
        "region symbols must be preserved without fabricated resolution");
    Require(first.weatherId == std::array<std::byte, 4>{std::byte{'R'}, std::byte{'A'}, std::byte{'h'}, std::byte{'r'}},
        "weather id must preserve byte order");
    Require(first.colorBgra == std::array<std::uint8_t, 4>{0x12, 0x34, 0x56, 0x78},
        "color reserved byte must not become forced alpha");
    const auto reopened = ParseRegions(parsed.value->originalBytes);
    Require(reopened.Succeeded() && reopened.value->entries[1].name == "第二个区域", "region original-byte roundtrip failed");
}

void CamerasAreComplete() {
    const auto fixture = CameraFixture();
    const auto parsed = ParseCameras(fixture.data);
    Require(parsed.Succeeded() && parsed.value->entries.size() == 2
        && parsed.value->layout == CameraLayout::Classic, "classic camera records not parsed");
    const auto& first = parsed.value->entries[0];
    Require(first.targetX == 10 && first.targetY == 20 && first.zOffset == 30 && first.rotation == 40 &&
        first.angleOfAttack == 50 && first.targetDistance == 60 && first.roll == 70 &&
        first.fieldOfView == 80 && first.farClippingPlane == 90 && first.nearClippingPlane == 100,
        "all classic camera parameters must retain correct position");
    Require(first.name == "TRIGSTR_20" && parsed.value->originalBytes == fixture.data, "camera names or source bytes lost");
    const auto reopened = ParseCameras(parsed.value->originalBytes);
    Require(reopened.Succeeded() && reopened.value->entries[1].targetX == 11, "camera original-byte roundtrip failed");

    const auto extendedBytes = ExtendedCameraFixture();
    const auto extended = ReadCamerasVersioned(extendedBytes.data);
    Require(extended.Succeeded() && extended.value->layout == CameraLayout::Extended
        && extended.value->entries[0].localRotation == std::array<float, 3>{1.f, 2.f, 3.f}
        && extended.value->entries[0].name == "ExtendedCamera"
        && extended.value->originalBytes == extendedBytes.data
        && !extended.value->Serialize().Succeeded(), "extended v0 camera is fully readable but never writable");
    const auto ambiguousBytes = ExtendedCameraFixture(true);
    const auto ambiguous = ReadCamerasVersioned(ambiguousBytes.data);
    Require(ParseCameras(ambiguousBytes.data).Succeeded(), "trusted classic camera names are not ambiguous");
    Require(!ambiguous.Succeeded() && std::any_of(ambiguous.diagnostics.begin(), ambiguous.diagnostics.end(),
        [](const auto& diagnostic) { return diagnostic.code == "W3C_LAYOUT_AMBIGUOUS"; }),
        "two fully consumed v0 camera layouts must not select one by heuristic");
    Bytes empty; empty.U32(0); empty.U32(0);
    const auto noCameras = ReadCamerasVersioned(empty.data);
    Require(noCameras.Succeeded() && noCameras.value->layout == CameraLayout::UnknownEmpty
        && noCameras.value->Serialize().value == empty.data, "empty camera member preserves bytes without guessing record layout");
    auto changed = *noCameras.value;
    changed.entries.push_back(first);
    Require(!changed.Serialize().Succeeded(), "unknown empty camera layout cannot synthesize new records");
    changed = *extended.value; changed.layout = CameraLayout::Classic;
    Require(!changed.Serialize().Succeeded(), "classic layout flag cannot silently discard extended camera fields");
}

void SoundsRetainDefinitionsAndSpatialParameters() {
    const auto fixture = SoundFixture();
    const auto parsed = ParseSounds(fixture.data);
    Require(parsed.Succeeded() && parsed.value->entries.size() == 2, "multiple sound definitions must parse");
    const auto& first = parsed.value->entries[0];
    const auto& second = parsed.value->entries[1];
    Require(first.filePath == second.filePath && first.name != second.name, "same file must support independent definitions");
    Require(first.IsLooping() && first.Is3D() && first.StopsOutOfRange() && !first.IsMusic(), "sound flags shifted");
    Require(second.IsMusic() && !second.Is3D(), "2D/music flags shifted");
    Require(first.fadeInRate == 11 && first.fadeOutRate == 22 && first.volume == -1 && first.pitch == 1.25f &&
        first.pitchVariance == 0.125f && first.priority == 8 && first.channel == 10, "sound scalar fields lost");
    Require(first.minDistance == 100 && first.maxDistance == 2000 && first.distanceCutoff == 2500 &&
        first.coneAngleInside == 90 && first.coneAngleOutside == 180 && first.coneOutsideVolume == 67 &&
        first.coneOrientation == std::array<float, 3>{0.5f, -0.25f, 0.75f}, "sound spatial fields lost");
    Require(parsed.value->originalBytes == fixture.data && ParseSounds(parsed.value->originalBytes).Succeeded(),
        "sound source roundtrip failed");
}

void ExtendedSoundsAreReadOnlyAndKeepConditionalFields() {
    for (const std::uint32_t version : {2u, 3u}) {
        const auto bytes = ExtendedSoundFixture(version);
        const auto parsed = ParseSounds(bytes.data);
        Require(parsed.Succeeded() && parsed.value && parsed.value->version == version
            && parsed.value->entries.size() == 2 && parsed.value->originalBytes == bytes.data,
            "v2/v3 sound records must parse without losing source bytes");
        const auto& first = *parsed.value->entries[0].extended;
        const auto& second = *parsed.value->entries[1].extended;
        Require(first.repeatVariableName == "gg_snd_Extended0" && first.dialogueTextKey == -17
            && !first.unknown5 && first.facialAnimationLabel == "Face"
            && second.unknown5 == "conditional value" && second.dialogueSpeakerNameKey == -23
            && second.unknown11.has_value() == (version == 3),
            "v2/v3 per-record extension, conditional field and v3 tail shifted");
        Require(!parsed.value->Serialize().Succeeded(), "extended W3S writeback must stay rejected");
        auto forgedClassic = *parsed.value; forgedClassic.version = 1;
        Require(!forgedClassic.Serialize().Succeeded(), "v1 flag cannot discard retained sound extensions");
        auto damaged = bytes.data;
        damaged.pop_back();
        Require(!ParseSounds(damaged).Succeeded(), "truncated extended sound fails closed");
    }
    const auto mismatch = ParseSounds(ExtendedSoundFixture(2, true).data);
    Require(mismatch.Succeeded() && mismatch.value->entries[1].extended->repeatVariableName == "old_variable"
        && std::any_of(mismatch.diagnostics.begin(), mismatch.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "W3S_REPEAT_MISMATCH";
        }), "repeated variable mismatch remains visible and unmodified");
}

void BinaryWriterRoundTripsAndRejectsInvalidChanges() {
    auto regions = ParseRegions(RegionFixture().data);
    auto cameras = ParseCameras(CameraFixture().data);
    auto sounds = ParseSounds(SoundFixture().data);
    Require(regions.Succeeded() && cameras.Succeeded() && sounds.Succeeded(), "writer fixtures must parse");
    Require(regions.value->Serialize().value == regions.value->originalBytes, "unchanged regions must preserve bytes");
    Require(cameras.value->Serialize().value == cameras.value->originalBytes, "unchanged cameras must preserve bytes");
    Require(sounds.value->Serialize().value == sounds.value->originalBytes, "unchanged sounds must preserve bytes");

    regions.value->entries[0].left = -100.25f;
    regions.value->entries[0].ambientSound = "gg_snd_Edited";
    const auto changedRegions = regions.value->Serialize();
    Require(changedRegions.Succeeded(), "edited regions should serialize");
    const auto parsedRegions = ParseRegions(*changedRegions.value);
    Require(parsedRegions.Succeeded() && parsedRegions.value->entries[0].left == -100.25f
        && parsedRegions.value->entries[0].ambientSound == "gg_snd_Edited"
        && parsedRegions.value->entries[1].name == "第二个区域", "region edit roundtrip must preserve other entries");

    cameras.value->entries[1].fieldOfView = 73.5f;
    const auto changedCameras = cameras.value->Serialize();
    Require(changedCameras.Succeeded(), "edited cameras should serialize");
    const auto parsedCameras = ParseCameras(*changedCameras.value);
    Require(parsedCameras.Succeeded() && parsedCameras.value->entries[1].fieldOfView == 73.5f
        && parsedCameras.value->entries[0].name == "TRIGSTR_20", "camera edit roundtrip must preserve other entries");

    sounds.value->entries[0].volume = 42;
    sounds.value->entries[0].coneOrientation[1] = 0.8f;
    const auto changedSounds = sounds.value->Serialize();
    Require(changedSounds.Succeeded(), "edited sounds should serialize");
    const auto parsedSounds = ParseSounds(*changedSounds.value);
    Require(parsedSounds.Succeeded() && parsedSounds.value->entries[0].volume == 42
        && parsedSounds.value->entries[0].coneOrientation[1] == 0.8f
        && parsedSounds.value->entries[1].volume == -1, "sound edit roundtrip must preserve other definition");

    cameras.value->version = 99;
    Require(!cameras.value->Serialize().Succeeded(), "unsupported camera version must reject writing");
    sounds.value->entries[0].pitch = std::numeric_limits<float>::quiet_NaN();
    Require(!sounds.value->Serialize().Succeeded(), "nonfinite sound pitch must reject writing");
    regions.value->entries[0].name.push_back('\0');
    Require(!regions.value->Serialize().Succeeded(), "embedded NUL must reject writing");
}

template <typename Parse>
void RejectBinaryDamage(const Bytes& fixture, Parse parser) {
    for (std::size_t length = 0; length < fixture.data.size(); ++length) {
        const auto result = parser(std::span<const std::byte>(fixture.data).first(length));
        Require(!result.Succeeded() && !result.value.has_value(), "truncation must not produce a partial document");
        Require(!result.diagnostics.empty() && result.diagnostics[0].origin.has_value(), "failure needs byte origin");
    }
    auto version = fixture.data;
    version[0] = std::byte{99};
    Require(!parser(version).Succeeded(), "unsupported version must fail");
    auto count = fixture.data;
    for (std::size_t i = 4; i < 8; ++i) count[i] = std::byte{0xff};
    Require(!parser(count).Succeeded(), "hostile count must fail before allocation");
    auto trailing = fixture.data;
    trailing.push_back(std::byte{0});
    Require(!parser(trailing).Succeeded(), "unknown trailing data must not silently pass");
}

void InvalidNumbersAndEmptyDocuments() {
    auto nonFinite = RegionFixture();
    const auto nanBits = std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN());
    for (unsigned int i = 0; i < 4; ++i) nonFinite.data[8 + i] = static_cast<std::byte>((nanBits >> (8 * i)) & 0xff);
    Require(!ParseRegions(nonFinite.data).Succeeded(), "nonfinite coordinates cannot become valid source data");
    Bytes regions;
    regions.U32(5);
    regions.U32(0);
    Bytes cameras;
    cameras.U32(0);
    cameras.U32(0);
    Bytes sounds;
    sounds.U32(1);
    sounds.U32(0);
    Require(ParseRegions(regions.data).Succeeded() && ParseCameras(cameras.data).Succeeded() && ParseSounds(sounds.data).Succeeded(),
        "valid zero-count documents are distinct from missing or broken members");
}

War3Map::Result<StringTableDocument> ParseWts(std::string_view text) {
    Bytes bytes;
    bytes.Raw(text);
    return ParseStringTable(bytes.data);
}

void StringsPreserveTextAndResolveExplicitly() {
    const std::string source = "\xef\xbb\xbf// Original fixture\r\nSTRING 12\r\n// comment\r\n{\r\n  第一行\r\n\r\nlast { brace }\r\n}\r\n"
        "STRING 20\n{\n}\nSTRING 21\n{\nTRIGSTR_12\n}\n";
    const auto parsed = ParseWts(source);
    Require(parsed.Succeeded() && parsed.value->entries.size() == 3, "BOM, comments and multiline WTS must parse");
    Bytes expected;
    expected.Raw(source);
    Require(parsed.value->originalBytes == expected.data, "WTS byte source must keep BOM and line endings");
    Require(parsed.value->entries[0].value == "  第一行\r\n\r\nlast { brace }", "WTS internal whitespace must remain unchanged");
    const auto text = parsed.value->Resolve("TRIGSTR_00012");
    Require(text.Succeeded() && *text.value == parsed.value->entries[0].value, "TRIGSTR leading-zero id failed");
    const auto empty = parsed.value->Resolve("TRIGSTR_20");
    Require(empty.Succeeded() && empty.value->empty(), "empty string is a known value");
    const auto nonRecursive = parsed.value->Resolve("TRIGSTR_21");
    Require(nonRecursive.Succeeded() && *nonRecursive.value == "TRIGSTR_12", "WTS resolution must be a single explicit lookup");
    const auto literal = parsed.value->Resolve("prefix TRIGSTR_12");
    Require(literal.Succeeded() && *literal.value == "prefix TRIGSTR_12", "embedded substring must not be rewritten");
    for (const auto reference : {"TRIGSTR_", "TRIGSTR_-1", "TRIGSTR_12x", "TRIGSTR_4294967296", "TRIGSTR_30"}) {
        Require(!parsed.value->Resolve(reference).Succeeded(), "invalid or missing reference must not resolve to fabricated text");
    }
    Require(ParseStringTable(parsed.value->originalBytes).Succeeded(), "WTS preserved original must parse again");
}

void StringsRejectAmbiguityAndDamage() {
    const auto duplicate = ParseWts("STRING 4\n{\none\n}\nSTRING 4\n{\ntwo\n}\n");
    Require(duplicate.Succeeded() && duplicate.value->entries.size() == 2 && !duplicate.diagnostics.empty(),
        "duplicate IDs must preserve both definitions and report ambiguity");
    Require(!duplicate.value->Resolve("TRIGSTR_4").Succeeded(), "duplicate lookup must not choose a record");
    for (const auto source : {"STRING 1\n{\nmissing", "STRING -1\n{\na\n}\n", "STRING 4294967296\n{\na\n}\n",
        "STRING 1\nvalue\n}\n", "STRING1\n{\na\n}\n", "STRING 1\n{\na\n}\nJUNK", "STRING 1\n{inline}\n"}) {
        const auto result = ParseWts(source);
        Require(!result.Succeeded() && !result.value, "malformed string table must fail without a partial table");
    }
    Require(!ParseWts(std::string_view("STRING 1\0", 9)).Succeeded(), "NUL must not truncate WTS silently");
    Require(ParseWts("").Succeeded() && ParseWts("\xef\xbb\xbf// empty\n").Succeeded(), "explicit empty WTS is valid");
    const auto twoBlanks = ParseWts("STRING 1\n{\n\n\n}\n");
    Require(twoBlanks.Succeeded() && twoBlanks.value->entries[0].value == "\n", "meaningful blank content line lost");
}

void StringsEditPreservesTriviaAndAllocatesIds() {
    const std::string source = "\xef\xbb\xbf// note\r\nSTRING 7\r\n{\r\nfirst\r\n}\r\n"
        "// between\r\nSTRING 15\r\n{\r\nsecond\r\n}\r\n";
    auto parsed = ParseWts(source);
    Require(parsed.Succeeded(), "WTS edit fixture must parse");
    const auto unchanged = parsed.value->Serialize();
    Require(unchanged.Succeeded() && std::string_view(reinterpret_cast<const char*>(unchanged.value->data()),
        unchanged.value->size()) == source, "unchanged WTS must preserve every source byte");
    parsed.value->entries[0].value = "revised\r\nline";
    const auto added = parsed.value->AddString("third");
    Require(added.Succeeded() && *added.value == 16, "new WTS id must follow maximum, without reuse");
    const auto written = parsed.value->Serialize();
    Require(written.Succeeded(), "edited WTS should serialize");
    const std::string_view output(reinterpret_cast<const char*>(written.value->data()), written.value->size());
    Require(output.find("// note") != output.npos && output.find("// between") != output.npos,
        "editing WTS must preserve original comments");
    const auto roundtrip = ParseStringTable(*written.value);
    Require(roundtrip.Succeeded() && roundtrip.value->entries.size() == 3
        && roundtrip.value->entries[0].value == "revised\r\nline"
        && roundtrip.value->entries[1].value == "second"
        && roundtrip.value->entries[2].id == 16, "edited WTS must roundtrip exact entries");
    parsed.value->entries[0].id = 6;
    Require(!parsed.value->Serialize().Succeeded(), "renumbered old WTS id must reject writing");
    auto duplicates = ParseWts("STRING 4\n{\na\n}\nSTRING 4\n{\nb\n}\n");
    Require(duplicates.Succeeded(), "ambiguous WTS is still readable");
    duplicates.value->entries[0].value = "changed";
    Require(!duplicates.value->Serialize().Succeeded(), "ambiguous WTS must reject edits");
}

} // namespace

int main() {
    try {
        RegionsAreComplete();
        CamerasAreComplete();
        SoundsRetainDefinitionsAndSpatialParameters();
        ExtendedSoundsAreReadOnlyAndKeepConditionalFields();
        BinaryWriterRoundTripsAndRejectsInvalidChanges();
        RejectBinaryDamage(RegionFixture(), ParseRegions);
        RejectBinaryDamage(CameraFixture(), ParseCameras);
        RejectBinaryDamage(SoundFixture(), ParseSounds);
        InvalidNumbersAndEmptyDocuments();
        StringsPreserveTextAndResolveExplicitly();
        StringsRejectAmbiguityAndDamage();
        StringsEditPreservesTriviaAndAllocatesIds();
        std::cout << "Map auxiliary parser tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
