#include "MapEditSaveCommand.h"

#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/Services/MapScriptTargetSelection.h"
#include "War3MapCore/Services/ScriptSavePreparation.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace War3Map;

std::string Utf8Argument(std::wstring_view text) {
    const auto converted = std::filesystem::path(text).u8string();
    return {converted.begin(), converted.end()};
}

void PrintDiagnostics(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.subsystem << ": " << diagnostic.code << ": " << diagnostic.message << '\n';
        if (diagnostic.origin && !diagnostic.origin->source.empty()) {
            const auto path = diagnostic.origin->source.u8string();
            std::cerr << "  " << std::string(path.begin(), path.end()) << ":" << diagnostic.origin->offset << '\n';
        }
    }
}

template<class Number>
std::optional<Number> ParseNumber(std::wstring_view argument) {
    const auto text = Utf8Argument(argument);
    Number value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

int Invalid(std::string_view detail) {
    std::cerr << "MapEdit.Save: SAVE_ARGUMENT_INVALID: " << detail << '\n';
    return 2;
}

std::optional<std::string> ReadUtf8Text(const std::filesystem::path& path, std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) { error = "cannot read UTF-8 text file"; return std::nullopt; }
    const auto length = input.tellg();
    if (length < 0 || length > 1024 * 1024) { error = "text file exceeds 1 MiB budget"; return std::nullopt; }
    std::string text(static_cast<std::size_t>(length), '\0');
    input.seekg(0);
    if (!text.empty()) input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!input || text.find('\0') != std::string::npos) { error = "text file is incomplete or contains NUL"; return std::nullopt; }
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    const auto valid = Trigger::RawText{text, Trigger::TextEncoding::Utf8, {}}.ToBytes();
    if (!valid.Succeeded()) { error = "text file must contain valid UTF-8"; return std::nullopt; }
    return text;
}

// first 指向 trigger ID；完整 roots CSV 为 '-' 时表示明确的空根列表。
std::optional<Trigger::TriggerEditTriggerTarget> ParseGuiTarget(wchar_t** argv, int first, std::string& error) {
    const auto trigger = ParseNumber<std::uint64_t>(argv[first]);
    if (!trigger || !*trigger) { error = "GUI target requires nonzero trigger ID"; return std::nullopt; }
    Trigger::TriggerEditTriggerTarget target{*trigger, {}, Utf8Argument(argv[first + 2])};
    const std::wstring_view csv(argv[first + 1]);
    if (csv != L"-") {
        std::size_t begin = 0;
        do {
            const auto end = csv.find(L',', begin);
            const auto id = ParseNumber<std::uint64_t>(csv.substr(begin, end == csv.npos ? csv.size() - begin : end - begin));
            if (!id || !*id || target.expectedRootIds.size() >= 4096) { error = "GUI roots need at most 4096 nonzero IDs"; return std::nullopt; }
            target.expectedRootIds.push_back(*id);
            if (end == csv.npos) break;
            begin = end + 1;
        } while (true);
    }
    return target;
}

std::optional<Trigger::TriggerTextExpectation> ParseTextExpectation(wchar_t** argv, int first, std::string& error) {
    const auto encoding = Trigger::ParseTriggerEditEncoding(Utf8Argument(argv[first]));
    if (!encoding) { error = "text expectation needs original utf8/utf8-bom/gb18030/latin1 encoding"; return std::nullopt; }
    return Trigger::TriggerTextExpectation{*encoding, Utf8Argument(argv[first + 1]), Utf8Argument(argv[first + 2])};
}

// --field <kind> <row> <row-key> <member-sha256> <field-id> <expected-raw> <new-value>
std::optional<Services::MapEditFieldOperation> ParseField(wchar_t** argv, int index, std::string& error) {
    const auto kind = MapData::ParseMapDataKindId(Utf8Argument(argv[index + 1]));
    const auto row = ParseNumber<std::uint64_t>(argv[index + 2]);
    const auto* field = kind ? MapData::FindMapField(*kind, Utf8Argument(argv[index + 5])) : nullptr;
    if (!kind || !row || !field) {
        error = "--field needs a known kind, a row index, and a field id from map-field-catalog";
        return std::nullopt;
    }
    auto expected = MapData::ParseMapFieldRaw(field->type, Utf8Argument(argv[index + 6]));
    auto value = MapData::ParseMapFieldInput(*field, Utf8Argument(argv[index + 7]));
    if (!expected.Succeeded() || !expected.value || !value.Succeeded() || !value.value) {
        PrintDiagnostics(expected.diagnostics);
        PrintDiagnostics(value.diagnostics);
        error = "--field expected value must use the exact raw form and the new value must match the field type";
        return std::nullopt;
    }
    Services::MapEditFieldOperation operation;
    operation.expectedMemberSha256 = Utf8Argument(argv[index + 4]);
    operation.edit.kind = *kind;
    operation.edit.row = static_cast<std::size_t>(*row);
    operation.edit.expectedRowKey = Utf8Argument(argv[index + 3]);
    operation.edit.field = std::string(field->id);
    operation.edit.expected = std::move(*expected.value);
    operation.edit.value = std::move(*value.value);
    return operation;
}

std::optional<Services::MapEditPlacementSource> ParsePlacementSource(wchar_t** argv, int index,
    std::string& error) {
    const std::wstring_view file(argv[index]);
    const auto row = ParseNumber<std::uint64_t>(argv[index + 1]);
    if ((file != L"units" && file != L"doodads") || !row || *row > UINT32_MAX) {
        error = "placement source needs units or doodads and an unsigned row index";
        return std::nullopt;
    }
    return Services::MapEditPlacementSource{
        file == L"units" ? MapData::PlacementFile::Units : MapData::PlacementFile::Doodads,
        static_cast<std::uint32_t>(*row), Utf8Argument(argv[index + 2]), Utf8Argument(argv[index + 3])};
}

std::optional<Trigger::TriggerEditEcaTarget> ParseTriggerEcaTarget(wchar_t** argv, int index, std::string& error) {
    const auto trigger = ParseNumber<StableId>(argv[index + 1]), eca = ParseNumber<StableId>(argv[index + 2]);
    const auto parent = ParseNumber<StableId>(argv[index + 3]);
    const auto kind = ParseNumber<std::int32_t>(argv[index + 4]), enabled = ParseNumber<std::int32_t>(argv[index + 6]);
    const auto arguments = ParseNumber<std::uint32_t>(argv[index + 8]), children = ParseNumber<std::uint32_t>(argv[index + 9]);
    if (!trigger || !eca || !parent || !kind || !enabled || !arguments || !children) {
        error = "trigger edit requires strict IDs, kind, enabled, argument/child counts"; return std::nullopt;
    }
    Trigger::TriggerEditEcaTarget target{*trigger, *eca, *parent, *kind, Utf8Argument(argv[index + 5]),
        *enabled, {}, *arguments, *children, Utf8Argument(argv[index + 10])};
    if (std::wstring_view(argv[index + 7]) != L"root") {
        const auto child = ParseNumber<std::int32_t>(argv[index + 7]);
        if (!child) { error = "child id must be an int32 or root"; return std::nullopt; }
        target.expectedChildId = *child;
    }
    return target;
}

// --stroke <kind> <circle|square> <vertex-x> <vertex-y> <radius-0..16> <value>
std::optional<Services::MapEditTerrainStroke> ParseStroke(wchar_t** argv, int index, std::string& error) {
    const auto x = ParseNumber<std::uint64_t>(argv[index + 3]);
    const auto y = ParseNumber<std::uint64_t>(argv[index + 4]);
    const auto radius = ParseNumber<std::uint64_t>(argv[index + 5]);
    if (!x || !y || *x > UINT32_MAX || *y > UINT32_MAX || !radius || *radius > 16) {
        error = "stroke x/y must be unsigned vertex indices and radius must be 0..16";
        return std::nullopt;
    }
    Services::MapEditTerrainStroke stroke;
    stroke.pointSpace = Services::MapEditPointSpace::TerrainVertex;
    stroke.points.push_back({static_cast<double>(*x), static_cast<double>(*y)});
    stroke.brush.radius = static_cast<std::uint32_t>(*radius);
    const std::wstring_view kind(argv[index + 1]), shape(argv[index + 2]);
    if (shape == L"circle") stroke.brush.shape = MapData::TerrainBrushShape::Circle;
    else if (shape == L"square") stroke.brush.shape = MapData::TerrainBrushShape::Square;
    else {
        error = "stroke shape must be circle or square";
        return std::nullopt;
    }
    if (kind == L"texture") {
        stroke.brush.kind = MapData::TerrainBrushKind::Texture;
        stroke.brush.groundTexture = Utf8Argument(argv[index + 6]);
        if (stroke.brush.groundTexture.size() != 4 || !std::all_of(stroke.brush.groundTexture.begin(),
            stroke.brush.groundTexture.end(), [](unsigned char c) { return c >= 33 && c <= 126; })) {
            error = "texture value must be a four-byte ASCII rawcode"; return std::nullopt;
        }
        return stroke;
    }
    if (kind == L"water-height" || kind == L"water_height") {
        const auto height = ParseNumber<float>(argv[index + 6]);
        if (!height || !std::isfinite(*height) || *height < -2048 || *height > 2047.75f) {
            error = "water height must be finite and within -2048..2047.75"; return std::nullopt;
        }
        stroke.brush.kind = MapData::TerrainBrushKind::WaterHeight; stroke.brush.waterHeight = *height;
        return stroke;
    }
    if (kind == L"ramp") {
        const std::wstring_view value(argv[index + 6]);
        if (shape != L"circle" || *radius != 0 || (value != L"true" && value != L"false")) {
            error = "ramp requires circle, radius 0 and a true or false value; its three-point dry step is checked by the shared planner";
            return std::nullopt;
        }
        stroke.brush.kind = MapData::TerrainBrushKind::Ramp;
        stroke.brush.value = value == L"true";
        return stroke;
    }
    if (kind != L"raise" && kind != L"lower" && kind != L"smooth") {
        error = "stroke kind must be raise, lower, smooth, texture, water-height, or ramp";
        return std::nullopt;
    }
    const auto strength = ParseNumber<float>(argv[index + 6]);
    const auto maximum = kind == L"smooth" ? 1.0f : 1024.0f;
    if (!strength || !std::isfinite(*strength) || *strength <= 0 || *strength > maximum) {
        error = "raise/lower strength must be >0..1024 and smooth strength >0..1";
        return std::nullopt;
    }
    using MapData::TerrainBrushKind;
    stroke.brush.kind = kind == L"raise" ? TerrainBrushKind::Raise
        : kind == L"lower" ? TerrainBrushKind::Lower : TerrainBrushKind::Smooth;
    stroke.brush.strength = *strength;
    return stroke;
}

// --water-stroke <shallow|deep> <circle|square> <vertex-x> <vertex-y>
//                <size-1..8> <cliff-slot-0..14> <seed-u32> <enforce-limits:true|false>
std::optional<Services::MapEditTerrainStroke> ParseWaterStroke(wchar_t** argv, int index, std::string& error) {
    MapData::TerrainWaterStrokeOptions water;
    const std::wstring_view kind(argv[index + 1]), shape(argv[index + 2]), enforce(argv[index + 8]);
    if (kind == L"shallow") water.kind = MapData::TerrainWaterBrushKind::Shallow;
    else if (kind == L"deep") water.kind = MapData::TerrainWaterBrushKind::Deep;
    else { error = "water stroke kind must be shallow or deep"; return std::nullopt; }
    if (shape == L"circle") water.shape = MapData::TerrainBrushShape::Circle;
    else if (shape == L"square") water.shape = MapData::TerrainBrushShape::Square;
    else { error = "water stroke shape must be circle or square"; return std::nullopt; }
    const auto x = ParseNumber<std::uint32_t>(argv[index + 3]);
    const auto y = ParseNumber<std::uint32_t>(argv[index + 4]);
    const auto size = ParseNumber<std::uint32_t>(argv[index + 5]);
    const auto cliff = ParseNumber<std::uint32_t>(argv[index + 6]);
    const auto seed = ParseNumber<std::uint32_t>(argv[index + 7]);
    if (!x || !y || !size || *size < 1 || *size > 8 || !cliff || *cliff > 14 || !seed) {
        error = "water stroke needs uint32 vertex indices and seed, size 1..8, and cliff slot 0..14";
        return std::nullopt;
    }
    if (enforce != L"true" && enforce != L"false") {
        error = "water stroke enforce-limits must be true or false"; return std::nullopt;
    }
    water.size = *size;
    water.cliffTexture = static_cast<std::uint8_t>(*cliff);
    water.randomSeed = *seed;
    water.enforceWaterLimits = enforce == L"true";
    Services::MapEditTerrainStroke stroke;
    stroke.waterBrush = water;
    stroke.pointSpace = Services::MapEditPointSpace::TerrainVertex;
    stroke.points.push_back({static_cast<double>(*x), static_cast<double>(*y)});
    return stroke;
}

} // namespace

int RunMapEditSave(int argc, wchar_t** argv) {
    if (argc < 9) return Invalid("expected a source, bound SHA-256 values, roots, candidate, and one edit operation");
    Services::MapEditSaveRequest request;
    request.sourceMap = argv[2];
    request.expectedSourceSha256 = Utf8Argument(argv[3]);
    request.expectedTerrainSha256 = Utf8Argument(argv[4]);
    if (request.expectedTerrainSha256 == "-") request.expectedTerrainSha256.clear();
    request.toolRoot = argv[5];
    request.gameRoot = argv[6];
    request.targetCandidate = argv[7];
    std::vector<Services::MapEditPlacementSource> copySources;
    std::optional<std::array<float, 3>> copyOffset;
    std::optional<Services::MapEditPlacementRotation> rotation;
    std::size_t retainedTriggerTextBytes = 0;
    for (int index = 8; index < argc;) {
        const std::wstring_view flag(argv[index]);
        std::string error;
        if (flag == L"--ui-root") {
            if (index + 1 >= argc || argv[index + 1][0] == L'\0' || !request.uiRoot.empty())
                return Invalid("--ui-root requires one nonempty UI data root");
            request.uiRoot = argv[index + 1];
            index += 2;
        } else if (flag == L"--lua-map-target") {
            if (index + 1 >= argc || argv[index + 1][0] == L'\0' || request.luaTarget)
                return Invalid("--lua-map-target requires one absolute configuration path and may appear only once");
            auto target = Services::LoadLuaMapTargetConfiguration(argv[index + 1]);
            if (!target.Succeeded() || !target.value) { PrintDiagnostics(target.diagnostics); return 2; }
            request.luaTarget = std::move(*target.value);
            index += 2;
        } else if (flag == L"--source-encoding") {
            if (index + 2 >= argc || request.sourceEncodings.size() >= 1024)
                return Invalid("--source-encoding requires an absolute source path and utf8|gbk; at most 1024 declarations");
            auto declaration = Services::ParseScriptSourceEncodingDeclaration(argv[index + 1], Utf8Argument(argv[index + 2]));
            if (!declaration.Succeeded() || !declaration.value) { PrintDiagnostics(declaration.diagnostics); return 2; }
            request.sourceEncodings.push_back(std::move(*declaration.value));
            index += 3;
        } else if (flag == L"--include-access") {
            if (index + 4 >= argc || request.includeAccess.size() >= 128)
                return Invalid("--include-access requires absolute path, directory|file, utf8|gbk|auto and required|optional; at most 128 declarations");
            const std::wstring_view requirement(argv[index + 4]);
            if (requirement != L"required" && requirement != L"optional")
                return Invalid("include access requirement must be required or optional");
            auto declaration = Services::ParseScriptIncludeAccessDeclaration(argv[index + 1],
                Utf8Argument(argv[index + 2]), Utf8Argument(argv[index + 3]), requirement == L"required");
            if (!declaration.Succeeded() || !declaration.value) { PrintDiagnostics(declaration.diagnostics); return 2; }
            request.includeAccess.push_back(std::move(*declaration.value));
            index += 5;
        } else if (flag == L"--layer") {
            if (index + 1 >= argc || argv[index + 1][0] == L'\0') return Invalid("--layer requires a nonempty id");
            if (!request.effectiveLayers) request.effectiveLayers.emplace();
            if (request.effectiveLayers->size() >= 128) return Invalid("at most 128 layers are allowed");
            request.effectiveLayers->push_back(Utf8Argument(argv[index + 1]));
            index += 2;
        } else if (flag == L"--trigger-identity") {
            if (index + 3 >= argc || (request.triggerEdits && !request.triggerEdits->expectedWtgSha256.empty()))
                return Invalid("--trigger-identity requires one WTG, WCT and Catalog SHA triple");
            if (!request.triggerEdits) request.triggerEdits.emplace();
            request.triggerEdits->expectedWtgSha256 = Utf8Argument(argv[index + 1]);
            request.triggerEdits->expectedWctSha256 = Utf8Argument(argv[index + 2]);
            request.triggerEdits->expectedCatalogSha256 = Utf8Argument(argv[index + 3]);
            index += 4;
        } else if (flag == L"--remove-eca" || flag == L"--set-eca-enabled") {
            const bool setEnabled = flag == L"--set-eca-enabled";
            if (index + (setEnabled ? 11 : 10) >= argc)
                return Invalid("ECA edit requires trigger/eca/parent IDs, kind/function/enabled/child, counts and span SHA");
            if (!request.triggerEdits) request.triggerEdits.emplace();
            if (request.triggerEdits->operations.size() >= Trigger::kMaxTriggerEditOperations)
                return Invalid("at most 256 trigger operations are allowed");
            auto target = ParseTriggerEcaTarget(argv, index, error);
            if (!target) return Invalid(error);
            if (setEnabled) {
                const auto value = ParseNumber<std::int32_t>(argv[index + 11]);
                if (!value || (*value != 0 && *value != 1)) return Invalid("new enabled must be 0 or 1");
                request.triggerEdits->operations.emplace_back(Trigger::TriggerSetEcaEnabledEdit{std::move(*target), *value != 0});
            } else request.triggerEdits->operations.emplace_back(Trigger::TriggerRemoveEcaEdit{std::move(*target)});
            index += setEnabled ? 12 : 11;
        } else if (flag == L"--set-argument") {
            if (index + 8 >= argc) return Invalid("--set-argument needs trigger/owner/argument IDs, old kind/value/type, new kind/value");
            const auto trigger = ParseNumber<std::uint64_t>(argv[index + 1]), owner = ParseNumber<std::uint64_t>(argv[index + 2]),
                argument = ParseNumber<std::uint64_t>(argv[index + 3]);
            const auto before = ParseNumber<std::int32_t>(argv[index + 4]), after = ParseNumber<std::int32_t>(argv[index + 7]);
            if (!trigger || !owner || !argument || !before || !after) return Invalid("invalid argument identities or kinds");
            if (!request.triggerEdits) request.triggerEdits.emplace();
            request.triggerEdits->operations.emplace_back(Trigger::TriggerSetArgumentEdit{*trigger, *owner, *argument, *before,
                Utf8Argument(argv[index + 5]), Utf8Argument(argv[index + 6]), *after, Utf8Argument(argv[index + 8])});
            index += 9;
        } else if (flag == L"--append-root") {
            if (index + 5 >= argc) return Invalid("--append-root needs trigger ID, complete roots CSV, span SHA, kind and function");
            auto target = ParseGuiTarget(argv, index + 1, error);
            if (!target) return Invalid(error);
            const std::wstring_view kindText(argv[index + 4]);
            const auto kind = kindText == L"event" ? Trigger::UiDefinitionKind::Event
                : kindText == L"condition" ? Trigger::UiDefinitionKind::Condition
                : kindText == L"action" ? Trigger::UiDefinitionKind::Action : Trigger::UiDefinitionKind::Unknown;
            if (kind == Trigger::UiDefinitionKind::Unknown) return Invalid("root kind must be event/condition/action");
            if (!request.triggerEdits) request.triggerEdits.emplace();
            request.triggerEdits->operations.emplace_back(Trigger::TriggerAppendRootEdit{std::move(*target), kind, Utf8Argument(argv[index + 5])});
            index += 6;
        } else if (flag == L"--set-global-code" || flag == L"--set-custom-text" || flag == L"--convert-custom-text") {
            const bool global = flag == L"--set-global-code", convert = flag == L"--convert-custom-text";
            const int argumentCount = global ? 4 : convert ? 7 : 5;
            if (index + argumentCount >= argc) return Invalid("text edit requires original encoding and UTF-8/encoded SHA plus UTF-8 file; GUI conversion also complete roots/span");
            const int expectationIndex = index + (global ? 1 : convert ? 4 : 2);
            const auto expected = ParseTextExpectation(argv, expectationIndex, error);
            const auto text = ReadUtf8Text(argv[expectationIndex + 3], error);
            if (!expected || !text) return Invalid(error);
            if (text->size() > 4 * 1024 * 1024 - retainedTriggerTextBytes) return Invalid("trigger text files exceed 4 MiB batch budget");
            retainedTriggerTextBytes += text->size();
            if (!request.triggerEdits) request.triggerEdits.emplace();
            if (global) request.triggerEdits->operations.emplace_back(Trigger::TriggerSetGlobalCodeEdit{*expected, *text});
            else if (convert) {
                auto target = ParseGuiTarget(argv, index + 1, error);
                if (!target) return Invalid(error);
                request.triggerEdits->operations.emplace_back(Trigger::TriggerConvertCustomTextEdit{std::move(*target), *expected, *text});
            } else {
                const auto trigger = ParseNumber<std::uint64_t>(argv[index + 1]);
                if (!trigger || !*trigger) return Invalid("custom text needs an existing custom trigger ID");
                request.triggerEdits->operations.emplace_back(Trigger::TriggerSetCustomTextEdit{*trigger, *expected, *text});
            }
            index += argumentCount + 1;
        } else if (flag == L"--field") {
            if (index + 7 >= argc) return Invalid("--field requires seven arguments");
            if (request.fieldEdits.size() >= MapData::kMaxMapFieldEdits) return Invalid("at most 32 field edits are allowed");
            auto operation = ParseField(argv, index, error);
            if (!operation) return Invalid(error);
            request.fieldEdits.push_back(std::move(*operation));
            index += 8;
        } else if (flag == L"--stroke") {
            if (index + 6 >= argc) return Invalid("--stroke requires six arguments");
            if (request.terrainStrokes.size() >= 128) return Invalid("at most 128 ordered strokes are allowed");
            auto stroke = ParseStroke(argv, index, error);
            if (!stroke) return Invalid(error);
            request.terrainStrokes.push_back(std::move(*stroke));
            index += 7;
        } else if (flag == L"--water-stroke") {
            if (index + 8 >= argc) return Invalid("--water-stroke requires eight arguments");
            if (request.terrainStrokes.size() >= 128) return Invalid("at most 128 ordered strokes are allowed");
            auto stroke = ParseWaterStroke(argv, index, error);
            if (!stroke) return Invalid(error);
            request.terrainStrokes.push_back(std::move(*stroke));
            index += 9;
        } else if (flag == L"--copy-source") {
            if (index + 4 >= argc) return Invalid("--copy-source requires file, row, row key, and member SHA");
            if (copySources.size() >= 4096) return Invalid("at most 4096 placements may be copied");
            auto source = ParsePlacementSource(argv, index + 1, error);
            if (!source) return Invalid(error);
            copySources.push_back(std::move(*source));
            index += 5;
        } else if (flag == L"--copy-offset") {
            if (index + 3 >= argc || copyOffset) return Invalid("--copy-offset requires one x/y/z triple");
            const auto x = ParseNumber<float>(argv[index + 1]);
            const auto y = ParseNumber<float>(argv[index + 2]);
            const auto z = ParseNumber<float>(argv[index + 3]);
            if (!x || !y || !z) return Invalid("--copy-offset needs three finite world-coordinate numbers");
            copyOffset = std::array<float, 3>{*x, *y, *z};
            index += 4;
        } else if (flag == L"--rotate") {
            if (index + 5 >= argc || rotation) return Invalid("--rotate requires one file, row, row key, member SHA, and degrees");
            auto source = ParsePlacementSource(argv, index + 1, error);
            const auto degrees = ParseNumber<float>(argv[index + 5]);
            if (!source || !degrees || !std::isfinite(*degrees) || *degrees < 0 || *degrees >= 360)
                return Invalid("--rotate needs a bound placement and finite degrees in [0, 360)");
            rotation = Services::MapEditPlacementRotation{
                std::move(*source), static_cast<float>(double(*degrees) * std::numbers::pi_v<double> / 180.0)};
            index += 6;
        } else {
            return Invalid("unknown map-edit-save option; use one published field, terrain, placement or trigger edit");
        }
        if (request.triggerEdits && request.triggerEdits->operations.size() > Trigger::kMaxTriggerEditOperations)
            return Invalid("at most 256 trigger operations are allowed");
    }
    if ((!copySources.empty() || copyOffset) && (!copySources.size() || !copyOffset || rotation))
        return Invalid("group copy requires --copy-source and --copy-offset and cannot mix with --rotate");
    if (rotation) request.placementEdit = std::move(*rotation);
    else if (copyOffset) request.placementEdit = Services::MapEditPlacementCopy{std::move(copySources), *copyOffset};
    if (request.terrainStrokes.empty() && request.fieldEdits.empty()
        && std::holds_alternative<std::monostate>(request.placementEdit) && !request.triggerEdits)
        return Invalid("at least one bounded edit operation is required");
    auto saved = Services::SaveMapEditCandidate(request);
    PrintDiagnostics(saved.diagnostics);
    if (!saved.Succeeded() || !saved.value) return 1;
    const auto& receipt = *saved.value;
    std::cout << "candidate=" << Utf8Argument(receipt.candidate.wstring())
              << "\nsource_sha256=" << receipt.sourceSha256
              << "\ncandidate_sha256=" << receipt.candidateSha256
              << "\noriginal_terrain_sha256=" << receipt.originalTerrainSha256
              << "\nsaved_terrain_sha256=" << receipt.savedTerrainSha256
              << "\nsaved_pathing_sha256=" << receipt.savedPathingSha256
              << "\nmember_count=" << receipt.memberCount
              << "\napplied_strokes=" << receipt.appliedStrokeCount
              << "\napplied_field_edits=" << receipt.appliedFieldEditCount
              << "\napplied_placements=" << receipt.appliedPlacementCount
              << "\nedited_member_count=" << receipt.editedMembers.size();
    for (std::size_t index = 0; index < receipt.editedMembers.size(); ++index) {
        const auto& member = receipt.editedMembers[index];
        std::cout << "\nedited_member_" << index << '=' << member.name
                  << "\nedited_member_" << index << "_original_sha256=" << member.originalSha256
                  << "\nedited_member_" << index << "_saved_sha256=" << member.savedSha256;
    }
    if (receipt.generatedMissingMinimap) {
        const auto& minimap = *receipt.generatedMissingMinimap;
        const bool declared = minimap.rangePolicy == Render::TerrainMinimapRangePolicy::DeclaredW3iMarginsExteriorMapEdge;
        std::cout << "\nminimap_action=" << (declared ? "generate_missing_declared_ground_only" : "generate_missing_ground_only")
                  << "\nminimap_range_policy=" << (declared ? "DeclaredW3iMarginsExteriorMapEdge" : "FullW3e")
                  << "\nminimap_blp_sha256=" << minimap.encodedBlpSha256
                  << "\nminimap_blp_bytes=" << minimap.encodedBlpBytes
                  << "\nminimap_decoded_mip0_rgba_sha256=" << minimap.decodedMip0RgbaSha256
                  << "\nminimap_width=" << minimap.width
                  << "\nminimap_height=" << minimap.height
                  << "\nminimap_mip_count=" << minimap.encodedMipCount
                  << "\nminimap_manifest_sha256=" << minimap.manifestSha256;
    }
    if (receipt.triggerEdits) std::cout << "\napplied_trigger_edits=" << receipt.triggerEdits->operationCount
        << "\nremoved_eca_count=" << receipt.triggerEdits->removedEcaCount
        << "\nremoved_argument_count=" << receipt.triggerEdits->removedArgumentCount
        << "\nenabled_change_count=" << receipt.triggerEdits->enabledChangeCount
        << "\nargument_change_count=" << receipt.triggerEdits->argumentChangeCount
        << "\nappended_root_count=" << receipt.triggerEdits->appendedRootCount
        << "\ntext_change_count=" << receipt.triggerEdits->textChangeCount
        << "\nconverted_gui_count=" << receipt.triggerEdits->convertedGuiCount;
    std::cout << "\nscript_seconds=" << receipt.scriptSeconds
              << "\nlayer_count=" << receipt.effectiveLayers.size();
    for (std::size_t layer = 0; layer < receipt.effectiveLayers.size(); ++layer)
        std::cout << "\nlayer_" << layer << '=' << receipt.effectiveLayers[layer];
    std::cout << "\noriginal_editor_validated=false\nruntime_validated=false\n";
    return 0;
}

int RunMapTriggerTextQuery(int argc, wchar_t** argv) {
    if (argc < 9) return Invalid("map-trigger-text requires source map, source SHA, UI root, global/trigger slot, trigger ID or 0, offset and limit");
    Services::MapTriggerTextQueryRequest request;
    request.sourceMap = argv[2]; request.expectedSourceSha256 = Utf8Argument(argv[3]); request.uiRoot = argv[4];
    const std::wstring_view slot(argv[5]);
    if (slot == L"global") request.query.slot = Trigger::TriggerTextSlot::GlobalCode;
    else if (slot == L"trigger") request.query.slot = Trigger::TriggerTextSlot::TriggerCode;
    else return Invalid("text slot must be global or trigger");
    const auto trigger = ParseNumber<std::uint64_t>(argv[6]), offset = ParseNumber<std::uint64_t>(argv[7]),
        limit = ParseNumber<std::uint64_t>(argv[8]);
    if (!trigger || !offset || !limit || *offset > std::numeric_limits<std::size_t>::max()
        || *limit < 4 || *limit > 65536) return Invalid("invalid text identity, UTF-8 byte offset or 4..65536 byte limit");
    request.query.triggerId = *trigger; request.query.offset = static_cast<std::size_t>(*offset); request.query.limit = static_cast<std::size_t>(*limit);
    for (int index = 9; index < argc; index += 2) {
        if (index + 1 >= argc) return Invalid("text query option needs a value");
        const std::wstring_view flag(argv[index]);
        if (flag == L"--catalog-sha256" && !request.expectedCatalogSha256) request.expectedCatalogSha256 = Utf8Argument(argv[index + 1]);
        else if (flag == L"--layer") {
            if (!request.effectiveLayers) request.effectiveLayers.emplace();
            if (request.effectiveLayers->size() >= 128) return Invalid("at most 128 effective layers");
            request.effectiveLayers->push_back(Utf8Argument(argv[index + 1]));
        } else return Invalid("text query accepts only --catalog-sha256 or --layer");
    }
    const auto queried = Services::QueryMapTriggerText(request);
    PrintDiagnostics(queried.diagnostics);
    if (!queried.Succeeded() || !queried.value) return 1;
    const auto& receipt = *queried.value;
    const auto& page = receipt.page;
    std::cout << "source_sha256=" << receipt.sourceSha256 << "\nwtg_sha256=" << page.wtgSha256
        << "\nwct_sha256=" << page.wctSha256 << "\ncatalog_sha256=" << page.catalogSha256
        << "\nslot=" << (page.slot == Trigger::TriggerTextSlot::GlobalCode ? "global" : "trigger")
        << "\ntrigger_id=" << page.triggerId << "\nencoding=" << Trigger::TriggerEditEncodingName(page.encoding)
        << "\nutf8_sha256=" << page.utf8Sha256 << "\nencoded_sha256=" << page.encodedSha256
        << "\ncustom_text=" << (page.customText ? "true" : "false") << "\ntotal_bytes=" << page.totalBytes
        << "\noffset=" << page.offset << "\nnext_offset=" << page.nextOffset << "\ntext_utf8_hex=";
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char value : page.text) std::cout << hex[value >> 4] << hex[value & 15];
    std::cout << "\ngui_target_available=" << (receipt.guiTarget ? "true" : "false");
    if (receipt.guiTarget) {
        std::cout << "\nexpected_source_span_sha256=" << receipt.guiTarget->expectedSourceSpanSha256 << "\nexpected_root_ids=";
        for (std::size_t index = 0; index < receipt.guiTarget->expectedRootIds.size(); ++index) {
            if (index) std::cout << ',';
            std::cout << receipt.guiTarget->expectedRootIds[index];
        }
        if (receipt.guiTarget->expectedRootIds.empty()) std::cout << '-';
    }
    std::cout << "\nlayer_count=" << receipt.effectiveLayers.size();
    for (std::size_t index = 0; index < receipt.effectiveLayers.size(); ++index) std::cout << "\nlayer_" << index << '=' << receipt.effectiveLayers[index];
    std::cout << '\n';
    return 0;
}
