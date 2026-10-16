#pragma once

// 原创 S6 保存输入和独立回读断言；Qt 与 MCP 仍分别执行自己的公共命令和保存链。
#include "MapDataFixtures.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <bit>
#include <cctype>
#include <fstream>
#include <initializer_list>
#include <stdexcept>

namespace AuxiliarySaveFixtures {
using namespace War3Map;

inline void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
template<class T> T Take(Result<T> result) {
    std::string diagnostics;
    for (const auto& row : result.diagnostics) diagnostics += row.code + ": " + row.message + "\n";
    Require(result.Succeeded() && result.value.has_value(), diagnostics);
    return std::move(*result.value);
}
inline std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}
struct Declarations { std::string profile, common, blizzard; };

// 声明来自本测试原创的小型 API 清单，不复制游戏或 YDWE 基础库。
inline Declarations OriginalDeclarations() {
    Declarations result;
    auto& [profile, common, blizzard] = result;
    profile = "forge-lua-target-v1\nidentity\toriginal-s6-classic-lua-save\n";
    for (const auto* type : {"placement", "player", "trigger", "boolexpr", "alliancetype", "rect", "camerasetup", "camerafield"}) {
        profile += std::string("type\t") + type + "\thandle\n";
        common += std::string("type ") + type + " extends handle\n";
    }
    const auto api = [&](const char* name, const char* returns, std::initializer_list<const char*> parameters, bool bj = false) {
        profile += std::string("api\t") + name + '\t' + returns;
        std::string declaration = std::string(bj ? "function " : "native ") + name + " takes ";
        std::size_t index = 0;
        if (parameters.size() == 0) declaration += "nothing";
        for (const auto* parameter : parameters) {
            profile += std::string("\t") + parameter;
            if (index) declaration += ", ";
            declaration += std::string(parameter) + " p" + std::to_string(index++);
        }
        profile += '\n'; declaration += std::string(" returns ") + returns + '\n';
        if (bj) blizzard += declaration + "endfunction\n";
        else common += declaration;
    };
    common += "native ConvertPlacement takes integer i returns placement\n"
        "native ConvertAllianceType takes integer i returns alliancetype\n"
        "native ConvertCameraField takes integer i returns camerafield\n"
        "native Cheat takes string text returns nothing\n";
    api("GetCameraMargin", "real", {"integer"});
    api("SetCameraBounds", "nothing", {"real", "real", "real", "real", "real", "real", "real", "real"});
    api("SetDayNightModels", "nothing", {"string", "string"});
    api("NewSoundEnvironment", "nothing", {"string"});
    api("SetMapName", "nothing", {"string"}); api("SetMapDescription", "nothing", {"string"});
    api("SetPlayers", "nothing", {"integer"}); api("SetTeams", "nothing", {"integer"});
    api("SetGamePlacement", "nothing", {"placement"}); api("SetMapMusic", "nothing", {"string", "boolean", "integer"});
    api("InitGenericPlayerSlots", "nothing", {}, true); api("InitBlizzard", "nothing", {}, true);
    api("SetAmbientDaySound", "nothing", {"string"}, true); api("SetAmbientNightSound", "nothing", {"string"}, true);
    api("TriggerEvaluate", "boolean", {"trigger"}); api("TriggerExecute", "nothing", {"trigger"});
    api("DestroyTrigger", "nothing", {"trigger"}); api("DestroyBoolExpr", "nothing", {"boolexpr"});
    api("DisableTrigger", "nothing", {"trigger"}); api("SetPlayerAlliance", "nothing", {"player", "player", "alliancetype", "boolean"});
    api("Rect", "rect", {"real", "real", "real", "real"}); api("CreateCameraSetup", "camerasetup", {});
    api("CameraSetupSetField", "nothing", {"camerasetup", "camerafield", "real", "real"});
    api("CameraSetupSetDestPosition", "nothing", {"camerasetup", "real", "real", "real"});
    common += "globals\n";
    const auto constant = [&](const char* name, const char* type, std::string value) {
        profile += std::string("constant\t") + name + '\t' + type + '\n';
        common += std::string("constant ") + type + ' ' + name + " = " + value + '\n';
    };
    constant("MAP_PLACEMENT_USE_MAP_SETTINGS", "placement", "ConvertPlacement(0)");
    for (const auto* name : {"CAMERA_MARGIN_LEFT", "CAMERA_MARGIN_RIGHT", "CAMERA_MARGIN_BOTTOM", "CAMERA_MARGIN_TOP"})
        constant(name, "integer", "0");
    for (const auto* name : {"ALLIANCE_PASSIVE", "ALLIANCE_HELP_REQUEST", "ALLIANCE_HELP_RESPONSE", "ALLIANCE_SHARED_XP",
            "ALLIANCE_SHARED_SPELLS", "ALLIANCE_SHARED_VISION", "ALLIANCE_SHARED_CONTROL", "ALLIANCE_SHARED_ADVANCED_CONTROL"})
        constant(name, "alliancetype", "ConvertAllianceType(0)");
    int cameraField = 0;
    for (const auto* name : {"CAMERA_FIELD_ZOFFSET", "CAMERA_FIELD_ROTATION", "CAMERA_FIELD_ANGLE_OF_ATTACK",
            "CAMERA_FIELD_TARGET_DISTANCE", "CAMERA_FIELD_ROLL", "CAMERA_FIELD_FIELD_OF_VIEW", "CAMERA_FIELD_FARZ", "CAMERA_FIELD_NEARZ"})
        constant(name, "camerafield", "ConvertCameraField(" + std::to_string(cameraField++) + ")");
    common += "endglobals\n";
    return result;
}

inline std::vector<Mpq::MpqBuildFile> GameFiles(const Declarations& input) {
    return {{"common.j", Bytes(input.common), false}, {"blizzard.j", Bytes(input.blizzard), false},
        {"Units/UnitData.slk", Bytes("ID;PWXL\nC;X1;Y1;K\"ID\"\nC;X2;Y1;K\"Name\"\nC;X1;Y2;K\"u001\"\nC;X2;Y2;K\"Original S6 unit\"\nE\n"), false}};
}

inline constexpr std::string_view TemplateTypes =
    "[TriggerTypes]\ninteger=0,1,1,Integer,integer\nstring=0,1,1,String,string\n[TriggerParams]\n";
inline constexpr std::string_view TemplateAction =
    "[S6TemplateMarker]\nscript_name=SetMapName\ndescription=Original template ${text}\n[[.args]]\ntype=string\n";

// 初始源图中的原创可编辑 T。模板在普通文字参数中，保存必须经实际模板阶段才能产生 42。
inline Trigger::EncodedTriggerDocument TemplatePair(const Trigger::UiCatalog& catalog) {
    auto document = Take(Trigger::WtgWctCodec{}.CreateEmpty(catalog));
    const auto category = Take(Trigger::TriggerDirectoryEditor::AddCategory(document, "Original S6", {}));
    const auto trigger = Take(Trigger::TriggerDocumentEditor::AddTrigger(document, category, "OriginalTemplate"));
    const auto action = Take(Trigger::TriggerDocumentEditor::AppendRootEca(document, catalog, trigger,
        Trigger::UiDefinitionKind::Action, "S6TemplateMarker"));
    Require(action.appendedArguments.size() == 1, "original T action has one real editable argument");
    Require(Trigger::TriggerDocumentEditor::SetScalarArgument(document, action.appendedArguments.front().id,
        3, "S6 template <?= math.floor(42.75) ?>").Succeeded(), "original T template argument");
    return Take(Trigger::WtgWctCodec{}.Serialize(document, catalog));
}

inline Declarations JassDeclarations() {
    auto result = OriginalDeclarations();
    result.common = "type triggeraction extends handle\ntype weathereffect extends handle\n"
        "type destructable extends handle\ntype unit extends handle\n" + result.common;
    result.common.insert(result.common.find("globals\n"),
        "native CreateTrigger takes nothing returns trigger\n"
        "native TriggerAddAction takes trigger whichTrigger, code actionFunc returns triggeraction\n");
    return result;
}

inline std::vector<MapArchiveMember> SourceMembers(const Trigger::EncodedTriggerDocument& pair) {
    MapData::MapSettings settings; settings.formatVersion = 25; settings.tileset = 'L';
    settings.mapName = "Original S6 structural save"; settings.custom.emplace();
    settings.cameraBounds = {-1024, -1024, 1024, 1024, -1024, 1024, 1024, -1024};
    return {{"war3map.w3i", Take(settings.Serialize())}, {"war3map.wtg", pair.wtg}, {"war3map.wct", pair.wct},
        {"war3mapUnits.doo", MapDataFixtures::EmptyUnitsV8Sub11()}, {"war3map.doo", MapDataFixtures::EmptyDoodadsV8Sub11()},
        {"war3map.j", Bytes("function config takes nothing returns nothing\nendfunction\nfunction main takes nothing returns nothing\nendfunction\n")},
        {"unchanged.bin", Bytes("Original S6 unrelated member")}};
}

struct OriginalInputs {
    std::filesystem::path root, ui, tool, game, source, workspace;
    Trigger::EncodedTriggerDocument pair;
    std::vector<MapArchiveMember> members;
};

inline void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(static_cast<bool>(stream), "cannot write new original S6 input");
}
inline void Write(const std::filesystem::path& path, std::string_view text) { Write(path, Bytes(text)); }

// 调用者先独占创建根目录。这里仅生成原创输入，不编辑地图、绕过引用或写候选。
inline OriginalInputs CreateOriginalInputs(const std::filesystem::path& root) {
    OriginalInputs result; result.root = root; result.tool = root / "tool"; result.ui = result.tool / "ui";
    result.game = root / "game"; result.source = root / "source.w3x"; result.workspace = root / "workspace";
    Write(result.ui / "config", "base\n"); Write(result.ui / "base/define.txt", TemplateTypes);
    Write(result.ui / "base/action.txt", TemplateAction);
    for (const auto* name : {"call.txt", "condition.txt", "event.txt"}) Write(result.ui / "base" / name, "");
    std::filesystem::create_directories(result.tool / "compiler/include");
    for (const auto* name : {"YDTrigger.h", "Globals.h", "Function.h"})
        Write(result.tool / "compiler/include/YDTrigger" / name, "// Original S6 header: this fixture uses no storage or framework macros.\n");
    const auto declarations = JassDeclarations();
    Write(result.tool / "compiler/jass/24/common.j", declarations.common);
    Write(result.tool / "compiler/jass/24/blizzard.j", declarations.blizzard);
    std::filesystem::create_directory(result.game);
    Require(Mpq::WriteMpqArchive(result.game / "war3.mpq", GameFiles(declarations)).Succeeded(), "original game MPQ");
    const auto catalog = Take(Trigger::UiCatalog::Load(result.ui));
    result.pair = TemplatePair(catalog); result.members = SourceMembers(result.pair);
    (void)Take(WriteVerifiedArchive(result.source, result.members));
    for (const auto& member : result.members) Write(result.workspace / "map" / member.name, member.data);
    return result;
}

inline void VerifyNativeMembers(MapArchiveSession& reopened, const MapDocument& edited,
    std::uint8_t reservedColor = 177, std::uint32_t nearClipBits = 0x80000000u) {
    const auto regions = Take(reopened.ReadMember("war3map.w3r")), cameras = Take(reopened.ReadMember("war3map.w3c"));
    Require(regions == edited.Member(MapDataKind::Regions).sources.front().data
        && cameras == edited.Member(MapDataKind::Cameras).sources.front().data,
        "independent MPQ members differ from the actual command snapshot");
    const auto r = Take(MapData::ParseRegions(regions)); const auto c = Take(MapData::ParseCameras(cameras));
    Require(r.entries.size() == 1 && r.entries[0].name == "KeptRegion" && r.entries[0].top == 768
        && r.entries[0].creationNumber == 10 && r.entries[0].colorBgra[3] == reservedColor, "independent region fields");
    Require(c.entries.size() == 1 && c.entries[0].name == "KeptCamera" && c.entries[0].rotation == 45
        && c.entries[0].targetY == 96 && std::bit_cast<std::uint32_t>(c.entries[0].nearClippingPlane) == nearClipBits,
        "independent camera fields and reserved negative zero");
}

inline std::string VerifyJassInitialization(MapArchiveSession& reopened) {
    const auto bytes = Take(reopened.ReadMember("war3map.j"));
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto compact = [](std::string_view source) {
        std::string result; bool quoted = false, escaped = false;
        for (const char ch : source) {
            if (quoted) {
                result += ch;
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') quoted = false;
            } else if (ch == '"') { quoted = true; result += ch; }
            else if (!std::isspace(static_cast<unsigned char>(ch))) result += ch;
        }
        return result;
    };
    const auto normalized = compact(text);
    for (const auto* expected : {"function CreateRegions takes nothing returns nothing", "gg_rct_KeptRegion = Rect(-128.0, -64.0, 256.0, 768.0)",
            "function CreateCameras takes nothing returns nothing", "gg_cam_KeptCamera = CreateCameraSetup()",
            "CameraSetupSetField(gg_cam_KeptCamera, CAMERA_FIELD_ROTATION, 45.0, 0.0)",
            "CameraSetupSetDestPosition(gg_cam_KeptCamera, 16.0, 96.0, 0.0)", "call CreateRegions()", "call CreateCameras()",
            "call SetMapName(\"S6 template 42\")"})
        Require(normalized.find(compact(expected)) != std::string::npos, std::string("actual rebuilt JASS lacks: ") + expected);
    for (const auto* absent : {"DraftRegion", "RemovedRegion", "DraftCamera", "RemovedCamera"})
        Require(text.find(absent) == std::string::npos, "renamed or deleted S6 symbol remains in rebuilt JASS");
    Require(text.find("<?") == std::string::npos, "normal YDWE template stage left unevaluated source");
    return text;
}
} // namespace AuxiliarySaveFixtures
