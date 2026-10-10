#include "War3MapCore/Trigger/UiCatalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using War3Map::Trigger::UiCatalog;
using War3Map::Trigger::UiDefinition;
using War3Map::Trigger::UiDefinitionKind;
using War3Map::Trigger::UiDefinitionRevision;

constexpr std::string_view kLocalWarning =
    "仅限本地视觉；本调用及其查询结果禁止用于多人同步玩法分支。";

struct ExpectedDefinition {
    UiDefinitionKind kind = UiDefinitionKind::Unknown;
    std::string name;
    std::string signature;
    std::vector<std::string> argumentTypes;
    std::vector<std::string> placeholders;
    std::string category;
    std::string title;
    std::string description;
    std::optional<std::string> returns;
};

struct ExpectedJassFunction {
    std::string name;
    std::string takes;
    std::string returns;
    std::string comment;
    std::vector<std::string> statements;
};

struct ParsedJassFunction {
    std::string name;
    std::string takes;
    std::string returns;
    std::vector<std::string> comments;
    std::vector<std::string> statements;
};

struct GenericDefinition {
    UiDefinitionKind kind = UiDefinitionKind::Unknown;
    std::string name;
    std::string signature;
    std::vector<std::string> argumentTypes;
    std::vector<std::string> argumentNames;
    std::string category;
    std::string title;
    std::string description;
    std::optional<std::string> returns;

    [[nodiscard]] bool operator==(const GenericDefinition&) const = default;
};

[[noreturn]] void Fail(const std::string& message) {
    throw std::runtime_error(message);
}

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        Fail(message);
    }
}

std::string Trim(std::string value) {
    const auto whitespace = [](unsigned char character) {
        return std::isspace(character) != 0;
    };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), whitespace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), whitespace).base(), value.end());
    return value;
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        Fail("cannot open generated JASS file: " + path.string());
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0 || size > 1024 * 1024) {
        Fail("generated JASS file has an invalid size");
    }
    stream.seekg(0, std::ios::beg);
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!result.empty()) {
        stream.read(result.data(), size);
        if (!stream) {
            Fail("cannot read complete generated JASS file");
        }
    }
    return result;
}

std::vector<ExpectedDefinition> ExpectedDefinitions() {
    const std::string warning(kLocalWarning);
    return {
        {
            UiDefinitionKind::Call,
            "WarVKGetVersion",
            "call:WarVKGetVersion(nothing)",
            {"nothing"},
            {},
            "TC_WARVK_SYSTEM",
            "获取版本",
            warning + "返回版本文本。",
            "string",
        },
        {
            UiDefinitionKind::Call,
            "WarVKGetProtocolVersion",
            "call:WarVKGetProtocolVersion(nothing)",
            {"nothing"},
            {},
            "TC_WARVK_SYSTEM",
            "获取协议版本",
            warning + "返回协议版本。",
            "integer",
        },
        {
            UiDefinitionKind::Call,
            "WarVKIsReady",
            "call:WarVKIsReady(nothing)",
            {"nothing"},
            {},
            "TC_WARVK_SYSTEM",
            "查询就绪",
            warning + "返回本地就绪状态。",
            "boolean",
        },
        {
            UiDefinitionKind::Call,
            "WarVKCreatePointLight",
            "call:WarVKCreatePointLight(real)",
            {"real"},
            {"radius"},
            "TC_WARVK_POINTLIGHT",
            "创建点光",
            warning + "以半径 ${radius} 创建本地点光。",
            "integer",
        },
        {
            UiDefinitionKind::Call,
            "WarVKIsPointLightAlive",
            "call:WarVKIsPointLightAlive(integer)",
            {"integer"},
            {"lightId"},
            "TC_WARVK_POINTLIGHT",
            "查询点光",
            warning + "查询点光 ${lightId}。",
            "boolean",
        },
        {
            UiDefinitionKind::Call,
            "WarVKGetMetric",
            "call:WarVKGetMetric(nothing)",
            {"nothing"},
            {},
            "TC_WARVK_SYSTEM",
            "查询指标",
            warning + "返回本地指标。",
            "real",
        },
        {
            UiDefinitionKind::Action,
            "WarVKConfigurePointLight",
            "action:WarVKConfigurePointLight(boolean,integer,integer,real)",
            {"boolean", "integer", "integer", "real"},
            {"enabled", "quality", "lightId", "radius"},
            "TC_WARVK_POINTLIGHT",
            "配置点光",
            warning + "设置开关 ${enabled}、质量 ${quality}、点光 ${lightId}、半径 ${radius}。",
            std::nullopt,
        },
        {
            UiDefinitionKind::Action,
            "WarVKClearState",
            "action:WarVKClearState(nothing)",
            {"nothing"},
            {},
            "TC_WARVK_SYSTEM",
            "清除状态",
            warning + "清除本地状态。",
            std::nullopt,
        },
    };
}

std::vector<ExpectedJassFunction> ExpectedJassFunctions() {
    const std::string warning(kLocalWarning);
    return {
        {
            "WarVKEncodeBool", "boolean value", "string",
            "Local-visual protocol encoding helper; do not use it for synchronized gameplay state.",
            {"if value then", "return \"b:1\"", "endif", "return \"b:0\""},
        },
        {
            "WarVKEncodeInteger", "integer value", "string",
            "Local-visual protocol encoding helper; do not use it for synchronized gameplay state.",
            {"return \"i:\" + I2S(value)"},
        },
        {
            "WarVKEncodeId", "integer value", "string",
            "Local-visual protocol encoding helper; only positive managed ids are accepted by the runtime.",
            {"return \"d:\" + I2S(value)"},
        },
        {
            "WarVKEncodeReal", "real value", "string",
            "Local-visual protocol encoding helper; non-finite or out-of-range values are rejected by the runtime.",
            {"return \"r:\" + R2S(value)"},
        },
        {
            "WarVKGetVersion", "nothing", "string",
            warning + "返回版本文本。",
            {"return GetLocalizedString(\"warvk:v1;system.version\")"},
        },
        {
            "WarVKGetProtocolVersion", "nothing", "integer",
            warning + "返回协议版本。",
            {"return GetLocalizedHotkey(\"warvk:v1;system.protocolVersion\")"},
        },
        {
            "WarVKIsReady", "nothing", "boolean",
            warning + "返回本地就绪状态。",
            {"return GetLocalizedHotkey(\"warvk:v1;system.ready\") != 0"},
        },
        {
            "WarVKCreatePointLight", "real radius", "integer",
            warning + "以半径 ${radius} 创建本地点光。",
            {"return GetLocalizedHotkey(\"warvk:v1;pointLight.create\" + \";\" + WarVKEncodeReal(radius))"},
        },
        {
            "WarVKIsPointLightAlive", "integer lightId", "boolean",
            warning + "查询点光 ${lightId}。",
            {"return GetLocalizedHotkey(\"warvk:v1;pointLight.alive\" + \";\" + WarVKEncodeId(lightId)) != 0"},
        },
        {
            "WarVKGetMetric", "nothing", "real",
            warning + "返回本地指标。",
            {"return S2R(GetLocalizedString(\"warvk:v1;system.metric\"))"},
        },
        {
            "WarVKConfigurePointLight",
            "boolean enabled, integer quality, integer lightId, real radius",
            "nothing",
            warning + "设置开关 ${enabled}、质量 ${quality}、点光 ${lightId}、半径 ${radius}。",
            {
                "call Preloader(\"warvk:v1;pointLight.configure\" + \";\" + "
                "WarVKEncodeBool(enabled) + \";\" + WarVKEncodeInteger(quality) + \";\" + "
                "WarVKEncodeId(lightId) + \";\" + WarVKEncodeReal(radius))",
            },
        },
        {
            "WarVKClearState", "nothing", "nothing",
            warning + "清除本地状态。",
            {"call Preloader(\"warvk:v1;system.clear\")"},
        },
    };
}

std::optional<std::string> Field(
    const UiDefinitionRevision& revision,
    std::string_view key) {
    std::optional<std::string> result;
    for (const auto& [fieldKey, value] : revision.fields) {
        if (fieldKey != key) {
            continue;
        }
        if (result.has_value()) {
            Fail("duplicate Catalog field '" + std::string(key) + "' in " + revision.name);
        }
        result = value;
    }
    return result;
}

std::string RequiredField(
    const UiDefinitionRevision& revision,
    std::string_view key) {
    const auto value = Field(revision, key);
    if (!value.has_value()) {
        Fail("missing Catalog field '" + std::string(key) + "' in " + revision.name);
    }
    return *value;
}

std::vector<std::string> ExtractPlaceholders(std::string_view description) {
    std::vector<std::string> result;
    std::size_t cursor = 0;
    while (cursor < description.size()) {
        const std::size_t opening = description.find("${", cursor);
        if (opening == std::string_view::npos) {
            break;
        }
        const std::size_t closing = description.find('}', opening + 2);
        if (closing == std::string_view::npos || closing == opening + 2) {
            Fail("malformed Catalog description placeholder");
        }
        const std::string name(description.substr(opening + 2, closing - opening - 2));
        Expect(std::isalpha(static_cast<unsigned char>(name.front())) != 0 &&
            std::all_of(name.begin() + 1, name.end(), [](unsigned char character) {
                return std::isalnum(character) != 0;
            }), "Catalog placeholder is not a safe JASS identifier");
        result.push_back(name);
        cursor = closing + 1;
    }
    return result;
}

void ValidateDefinition(
    const UiDefinition& definition,
    const ExpectedDefinition& expected,
    std::string_view catalogName) {
    const std::string context = std::string(catalogName) + ":" + expected.name;
    Expect(definition.kind == expected.kind, context + " kind mismatch");
    Expect(definition.name == expected.name, context + " name mismatch");
    Expect(definition.group.empty(), context + " unexpectedly has a group");
    Expect(definition.revisions.size() == 1, context + " must have exactly one revision");
    Expect(definition.Signature() == expected.signature, context + " signature mismatch");

    const UiDefinitionRevision& active = definition.Active();
    std::vector<std::string> argumentTypes;
    for (const auto& argument : active.arguments) {
        argumentTypes.push_back(argument.type);
    }
    Expect(argumentTypes == expected.argumentTypes, context + " argument type sequence mismatch");
    const std::size_t expectedArgumentCount = static_cast<std::size_t>(std::count_if(
        expected.argumentTypes.begin(), expected.argumentTypes.end(),
        [](const std::string& type) { return type != "nothing"; }));
    Expect(definition.ArgumentCount() == expectedArgumentCount,
        context + " active argument count mismatch");

    Expect(RequiredField(active, "category") == expected.category,
        context + " category mismatch");
    Expect(RequiredField(active, "script_name") == expected.name,
        context + " script_name mismatch");
    Expect(RequiredField(active, "title") == expected.title,
        context + " title mismatch");
    const std::string description = RequiredField(active, "description");
    Expect(description == expected.description,
        context + " description mismatch; expected='" + expected.description +
            "' actual='" + description + "'");
    Expect(description.starts_with(kLocalWarning), context + " local-only warning is missing");
    Expect(ExtractPlaceholders(description) == expected.placeholders,
        context + " description placeholder order mismatch");

    const auto returns = Field(active, "returns");
    Expect(returns == expected.returns, context + " returns field mismatch");
}

void CompareCatalogDefinitions(
    const UiDefinition& mapui,
    const UiDefinition& legacy,
    const ExpectedDefinition& expected) {
    const auto& mapuiActive = mapui.Active();
    const auto& legacyActive = legacy.Active();
    Expect(mapui.kind == legacy.kind, expected.name + " Catalog kind divergence");
    Expect(mapui.name == legacy.name, expected.name + " Catalog name divergence");
    Expect(mapui.Signature() == legacy.Signature(), expected.name + " Catalog signature divergence");
    for (const std::string_view key : {
             std::string_view("category"), std::string_view("script_name"),
             std::string_view("title"), std::string_view("description"),
             std::string_view("returns")}) {
        Expect(Field(mapuiActive, key) == Field(legacyActive, key),
            expected.name + " Catalog field divergence: " + std::string(key));
    }
    Expect(mapuiActive.arguments.size() == legacyActive.arguments.size(),
        expected.name + " Catalog argument count divergence");
    for (std::size_t index = 0; index < mapuiActive.arguments.size(); ++index) {
        Expect(mapuiActive.arguments[index].type == legacyActive.arguments[index].type,
            expected.name + " Catalog argument type divergence");
    }
}

GenericDefinition ReadGenericDefinition(
    const UiDefinition& definition,
    std::string_view catalogName) {
    const std::string context = std::string(catalogName) + ":" + definition.name;
    static const std::regex publicName(R"(^WarVK[A-Za-z0-9]+$)");
    Expect(definition.kind == UiDefinitionKind::Action ||
        definition.kind == UiDefinitionKind::Call,
        context + " has an unsupported definition kind");
    Expect(std::regex_match(definition.name, publicName),
        context + " has an unsafe public JASS name");
    Expect(definition.group.empty(), context + " unexpectedly has a group");
    Expect(definition.revisions.size() == 1,
        context + " must have exactly one generated revision");

    const UiDefinitionRevision& active = definition.Active();
    Expect(!active.arguments.empty(),
        context + " must encode a no-argument definition with the nothing sentinel");
    std::vector<std::string> argumentTypes;
    if (active.arguments.size() == 1 && active.arguments.front().type == "nothing") {
        Expect(definition.ArgumentCount() == 0,
            context + " nothing sentinel did not produce zero active arguments");
    } else {
        for (const auto& argument : active.arguments) {
            Expect(argument.type == "boolean" || argument.type == "integer" ||
                argument.type == "real",
                context + " has an unsupported public argument type " + argument.type);
            argumentTypes.push_back(argument.type);
        }
        Expect(definition.ArgumentCount() == argumentTypes.size(),
            context + " active argument count mismatch");
    }

    const std::string category = RequiredField(active, "category");
    Expect(category.starts_with("TC_WARVK_") && category.size() > 9,
        context + " has an invalid generated category");
    Expect(RequiredField(active, "script_name") == definition.name,
        context + " script_name does not match the public wrapper");
    const std::string title = RequiredField(active, "title");
    Expect(!title.empty(), context + " has an empty title");
    const std::string description = RequiredField(active, "description");
    Expect(description.starts_with(kLocalWarning),
        context + " is missing the complete local-visual multiplayer warning");
    const std::vector<std::string> argumentNames = ExtractPlaceholders(description);
    Expect(argumentNames.size() == argumentTypes.size(),
        context + " placeholder count does not match active arguments");
    std::set<std::string> uniqueArgumentNames(argumentNames.begin(), argumentNames.end());
    Expect(uniqueArgumentNames.size() == argumentNames.size(),
        context + " has duplicate description placeholders");

    const auto returns = Field(active, "returns");
    if (definition.kind == UiDefinitionKind::Action) {
        Expect(!returns.has_value(), context + " action unexpectedly declares a return type");
    } else {
        Expect(returns.has_value(), context + " call is missing its return type");
        Expect(*returns == "boolean" || *returns == "integer" ||
            *returns == "real" || *returns == "string",
            context + " has an unsupported public return type");
    }

    return GenericDefinition{
        definition.kind,
        definition.name,
        definition.Signature(),
        std::move(argumentTypes),
        argumentNames,
        category,
        title,
        description,
        returns,
    };
}

std::vector<GenericDefinition> ValidateGenericCatalogs(
    const std::filesystem::path& mapuiRoot,
    const std::filesystem::path& legacyRoot) {
    auto mapuiResult = UiCatalog::Load(mapuiRoot);
    auto legacyResult = UiCatalog::Load(legacyRoot);
    if (!mapuiResult.Succeeded() || !legacyResult.Succeeded()) {
        const auto printDiagnostics = [](const auto& result, std::string_view label) {
            for (const auto& diagnostic : result.diagnostics) {
                std::cerr << label << ": " << diagnostic.code << ": "
                          << diagnostic.message << '\n';
            }
        };
        printDiagnostics(mapuiResult, "mapui");
        printDiagnostics(legacyResult, "legacy");
        Fail("generated Catalog failed UiCatalog::Load");
    }

    const UiCatalog& mapui = *mapuiResult.value;
    const UiCatalog& legacy = *legacyResult.value;
    Expect(mapui.Definitions(UiDefinitionKind::Event).empty() &&
        mapui.Definitions(UiDefinitionKind::Condition).empty(),
        "MapUI Catalog contains an unexpected event or condition");
    Expect(legacy.Definitions(UiDefinitionKind::Event).empty() &&
        legacy.Definitions(UiDefinitionKind::Condition).empty(),
        "legacy Catalog contains an unexpected event or condition");

    std::vector<const UiDefinition*> mapuiDefinitions;
    std::vector<const UiDefinition*> legacyDefinitions;
    for (const UiDefinitionKind kind : {UiDefinitionKind::Action, UiDefinitionKind::Call}) {
        const auto mapuiKind = mapui.Definitions(kind);
        const auto legacyKind = legacy.Definitions(kind);
        Expect(mapuiKind.size() == legacyKind.size(),
            std::string(War3Map::Trigger::ToString(kind)) +
                " definition count differs between generated Catalogs");
        mapuiDefinitions.insert(mapuiDefinitions.end(), mapuiKind.begin(), mapuiKind.end());
        legacyDefinitions.insert(legacyDefinitions.end(), legacyKind.begin(), legacyKind.end());
    }
    Expect(!mapuiDefinitions.empty(), "generated Catalog has no action/call definitions");
    Expect(mapuiDefinitions.size() == legacyDefinitions.size(),
        "generated Catalog action/call set sizes differ");

    std::vector<GenericDefinition> models;
    models.reserve(mapuiDefinitions.size());
    std::set<std::string> publicNames;
    for (const UiDefinition* mapuiDefinition : mapuiDefinitions) {
        Expect(publicNames.insert(mapuiDefinition->name).second,
            "duplicate public wrapper name across action/call definitions: " +
                mapuiDefinition->name);
        const UiDefinition* legacyDefinition = legacy.Find(
            mapuiDefinition->kind, mapuiDefinition->name);
        Expect(legacyDefinition != nullptr,
            "legacy Catalog is missing " + mapuiDefinition->name);
        const GenericDefinition mapuiModel = ReadGenericDefinition(*mapuiDefinition, "mapui");
        const GenericDefinition legacyModel = ReadGenericDefinition(*legacyDefinition, "legacy");
        Expect(mapuiModel == legacyModel,
            mapuiDefinition->name +
                " kind/signature/fields/arguments/placeholders diverge between Catalogs");
        models.push_back(mapuiModel);
    }
    Expect(models.size() == legacyDefinitions.size(),
        "legacy Catalog contains an unmatched action/call definition");
    return models;
}

void ValidateCatalogs(
    const std::filesystem::path& mapuiRoot,
    const std::filesystem::path& legacyRoot) {
    auto mapuiResult = UiCatalog::Load(mapuiRoot);
    auto legacyResult = UiCatalog::Load(legacyRoot);
    if (!mapuiResult.Succeeded() || !legacyResult.Succeeded()) {
        const auto printDiagnostics = [](const auto& result, std::string_view label) {
            for (const auto& diagnostic : result.diagnostics) {
                std::cerr << label << ": " << diagnostic.code << ": "
                          << diagnostic.message << '\n';
            }
        };
        printDiagnostics(mapuiResult, "mapui");
        printDiagnostics(legacyResult, "legacy");
        Fail("generated Catalog failed UiCatalog::Load");
    }

    const UiCatalog& mapui = *mapuiResult.value;
    const UiCatalog& legacy = *legacyResult.value;
    Expect(mapui.Definitions(UiDefinitionKind::Action).size() == 2,
        "MapUI Catalog action definition count mismatch");
    Expect(mapui.Definitions(UiDefinitionKind::Call).size() == 6,
        "MapUI Catalog call definition count mismatch");
    Expect(legacy.Definitions(UiDefinitionKind::Action).size() == 2,
        "legacy Catalog action definition count mismatch");
    Expect(legacy.Definitions(UiDefinitionKind::Call).size() == 6,
        "legacy Catalog call definition count mismatch");
    Expect(mapui.Definitions(UiDefinitionKind::Event).empty() &&
        mapui.Definitions(UiDefinitionKind::Condition).empty(),
        "MapUI Catalog contains an unexpected event or condition");
    Expect(legacy.Definitions(UiDefinitionKind::Event).empty() &&
        legacy.Definitions(UiDefinitionKind::Condition).empty(),
        "legacy Catalog contains an unexpected event or condition");

    for (const ExpectedDefinition& expected : ExpectedDefinitions()) {
        const UiDefinition* mapuiDefinition = mapui.Find(expected.kind, expected.name);
        const UiDefinition* legacyDefinition = legacy.Find(expected.kind, expected.name);
        Expect(mapuiDefinition != nullptr, "MapUI Catalog is missing " + expected.name);
        Expect(legacyDefinition != nullptr, "legacy Catalog is missing " + expected.name);
        ValidateDefinition(*mapuiDefinition, expected, "mapui");
        ValidateDefinition(*legacyDefinition, expected, "legacy");
        CompareCatalogDefinitions(*mapuiDefinition, *legacyDefinition, expected);
    }
}

std::vector<ParsedJassFunction> ParseJassSubset(
    const std::string& source,
    std::vector<std::string>& globalComments) {
    static const std::regex header(
        R"(^function ([A-Za-z][A-Za-z0-9]*) takes ((?:(?:boolean|integer|real|string) [A-Za-z][A-Za-z0-9]*(?:, (?:boolean|integer|real|string) [A-Za-z][A-Za-z0-9]*)*)|nothing) returns (nothing|boolean|integer|real|string)$)");
    static const std::regex ifStatement(R"(^if [A-Za-z][A-Za-z0-9]* then$)");
    static const std::regex returnStatement(R"(^return .+$)");
    static const std::regex callStatement(R"(^call Preloader\(.+\)$)");

    std::vector<ParsedJassFunction> functions;
    std::optional<ParsedJassFunction> current;
    std::istringstream lines(source);
    std::string line;
    std::size_t lineNumber = 0;
    int ifDepth = 0;
    while (std::getline(lines, line)) {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::string trimmed = Trim(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed.starts_with("//")) {
            std::string comment = Trim(trimmed.substr(2));
            if (current.has_value()) {
                current->comments.push_back(std::move(comment));
            } else {
                globalComments.push_back(std::move(comment));
            }
            continue;
        }

        if (!current.has_value()) {
            std::smatch match;
            if (!std::regex_match(trimmed, match, header)) {
                Fail("unsupported top-level JASS syntax at line " + std::to_string(lineNumber));
            }
            current.emplace(ParsedJassFunction{
                match[1].str(), match[2].str(), match[3].str(), {}, {}});
            ifDepth = 0;
            continue;
        }

        if (trimmed == "endfunction") {
            Expect(ifDepth == 0,
                "JASS function ended with an open if at line " + std::to_string(lineNumber));
            functions.push_back(std::move(*current));
            current.reset();
            continue;
        }
        if (std::regex_match(trimmed, ifStatement)) {
            ++ifDepth;
            Expect(ifDepth == 1, "nested if is outside the generated JASS subset");
        } else if (trimmed == "endif") {
            Expect(ifDepth == 1,
                "unmatched endif at line " + std::to_string(lineNumber));
            --ifDepth;
        } else if (!std::regex_match(trimmed, returnStatement) &&
                   !std::regex_match(trimmed, callStatement)) {
            Fail("unsupported generated JASS statement at line " + std::to_string(lineNumber));
        }
        current->statements.push_back(trimmed);
    }
    Expect(!current.has_value(), "generated JASS ended before endfunction");
    return functions;
}

void ValidateJassPreamble(const std::vector<std::string>& globalComments) {
    Expect(globalComments.size() == 2, "generated JASS must have exactly two global warnings");
    static const std::regex sourceComment(
        R"(^Generated file\. Source manifest SHA-256: [0-9A-Fa-f]{64}$)");
    Expect(std::regex_match(globalComments[0], sourceComment),
        "generated JASS source manifest binding comment is invalid");
    Expect(globalComments[1] ==
        "All functions are local-visual only. Never use returned values in multiplayer sync branches.",
        "generated JASS global multiplayer warning is invalid");
}

std::vector<std::pair<std::string, std::string>> ParseTakes(std::string_view takes) {
    if (takes == "nothing") {
        return {};
    }
    std::vector<std::pair<std::string, std::string>> result;
    std::size_t cursor = 0;
    while (cursor < takes.size()) {
        const std::size_t delimiter = takes.find(", ", cursor);
        const std::size_t end = delimiter == std::string_view::npos ? takes.size() : delimiter;
        const std::string_view parameter = takes.substr(cursor, end - cursor);
        const std::size_t separator = parameter.find(' ');
        Expect(separator != std::string_view::npos &&
            parameter.find(' ', separator + 1) == std::string_view::npos,
            "generated JASS takes clause is not in the strict type/name subset");
        result.emplace_back(
            parameter.substr(0, separator), parameter.substr(separator + 1));
        if (delimiter == std::string_view::npos) {
            break;
        }
        cursor = delimiter + 2;
    }
    return result;
}

std::string_view UnwrapStatement(
    std::string_view statement,
    std::string_view prefix,
    std::string_view suffix,
    std::string_view context) {
    Expect(statement.starts_with(prefix) && statement.ends_with(suffix) &&
        statement.size() >= prefix.size() + suffix.size(),
        std::string(context) + " does not use the required stock carrier expression");
    return statement.substr(prefix.size(), statement.size() - prefix.size() - suffix.size());
}

std::string ValidatePayloadExpression(
    std::string_view expression,
    const GenericDefinition& definition) {
    Expect(expression.starts_with('"'),
        definition.name + " payload does not start with a canonical literal");
    const std::size_t closingQuote = expression.find('"', 1);
    Expect(closingQuote != std::string_view::npos,
        definition.name + " payload has an unterminated canonical literal");
    const std::string route(expression.substr(1, closingQuote - 1));
    static const std::regex canonicalRoute(
        R"(^warvk:v1;[A-Za-z][A-Za-z0-9]*(?:\.[A-Za-z][A-Za-z0-9]*)+$)");
    Expect(std::regex_match(route, canonicalRoute),
        definition.name + " payload route is not a canonical warvk:v1 command");

    std::size_t cursor = closingQuote + 1;
    constexpr std::string_view separator = " + \";\" + ";
    for (std::size_t index = 0; index < definition.argumentTypes.size(); ++index) {
        Expect(expression.substr(cursor).starts_with(separator),
            definition.name + " payload argument separator mismatch");
        cursor += separator.size();
        const std::size_t opening = expression.find('(', cursor);
        const std::size_t closing = opening == std::string_view::npos ?
            std::string_view::npos : expression.find(')', opening + 1);
        Expect(opening != std::string_view::npos && closing != std::string_view::npos,
            definition.name + " payload encoder call is malformed");
        const std::string encoder(expression.substr(cursor, opening - cursor));
        const std::string argument(expression.substr(opening + 1, closing - opening - 1));
        Expect(argument == definition.argumentNames[index],
            definition.name + " payload argument order/name mismatch");

        const std::string& type = definition.argumentTypes[index];
        bool encoderAllowed = false;
        if (type == "boolean") {
            encoderAllowed = encoder == "WarVKEncodeBool";
        } else if (type == "real") {
            encoderAllowed = encoder == "WarVKEncodeReal";
        } else if (type == "integer") {
            encoderAllowed = encoder == "WarVKEncodeInteger" || encoder == "WarVKEncodeId";
        }
        Expect(encoderAllowed,
            definition.name + " payload uses an encoder incompatible with Catalog argument " +
                std::to_string(index));
        cursor = closing + 1;
    }
    Expect(cursor == expression.size(),
        definition.name + " payload contains an unexpected token or argument");
    return route;
}

void ValidateGenericJass(
    const std::filesystem::path& path,
    const std::vector<GenericDefinition>& definitions) {
    std::vector<std::string> globalComments;
    const auto functions = ParseJassSubset(ReadText(path), globalComments);
    ValidateJassPreamble(globalComments);

    const auto fixedFunctions = ExpectedJassFunctions();
    constexpr std::size_t helperCount = 4;
    Expect(fixedFunctions.size() >= helperCount, "internal helper fixture is incomplete");
    Expect(functions.size() == helperCount + definitions.size(),
        "generated JASS function count does not equal four helpers plus Catalog wrappers");

    std::set<std::string> functionNames;
    for (std::size_t index = 0; index < helperCount; ++index) {
        const ParsedJassFunction& actual = functions[index];
        const ExpectedJassFunction& expected = fixedFunctions[index];
        Expect(functionNames.insert(actual.name).second,
            "duplicate generated JASS helper " + actual.name);
        Expect(actual.name == expected.name && actual.takes == expected.takes &&
            actual.returns == expected.returns,
            "generated JASS fixed helper header mismatch at index " + std::to_string(index));
        Expect(actual.comments == std::vector<std::string>{expected.comment} &&
            actual.statements == expected.statements,
            actual.name + " fixed helper body mismatch");
    }

    std::unordered_map<std::string, const GenericDefinition*> definitionsByName;
    for (const GenericDefinition& definition : definitions) {
        Expect(definitionsByName.emplace(definition.name, &definition).second,
            "duplicate Catalog wrapper name " + definition.name);
    }

    std::set<std::string> payloadRoutes;
    std::set<std::string> visitedWrappers;
    for (std::size_t index = helperCount; index < functions.size(); ++index) {
        const ParsedJassFunction& function = functions[index];
        Expect(functionNames.insert(function.name).second,
            "duplicate generated JASS function " + function.name);
        const auto found = definitionsByName.find(function.name);
        Expect(found != definitionsByName.end(),
            "generated JASS public wrapper is absent from both Catalogs: " + function.name);
        const GenericDefinition& definition = *found->second;
        Expect(visitedWrappers.insert(function.name).second,
            "generated JASS repeats Catalog wrapper " + function.name);
        Expect(function.comments == std::vector<std::string>{definition.description},
            function.name + " JASS warning/comment differs from Catalog description");

        const auto parameters = ParseTakes(function.takes);
        Expect(parameters.size() == definition.argumentTypes.size(),
            function.name + " JASS/Catalog argument count mismatch");
        for (std::size_t argument = 0; argument < parameters.size(); ++argument) {
            Expect(parameters[argument].first == definition.argumentTypes[argument] &&
                parameters[argument].second == definition.argumentNames[argument],
                function.name + " JASS/Catalog argument type/name/order mismatch");
        }
        const std::string expectedReturn = definition.returns.value_or("nothing");
        Expect(function.returns == expectedReturn,
            function.name + " JASS/Catalog return type mismatch");
        Expect(function.statements.size() == 1,
            function.name + " wrapper must contain exactly one carrier statement");

        const std::string_view statement = function.statements.front();
        std::string_view payload;
        if (definition.kind == UiDefinitionKind::Action) {
            payload = UnwrapStatement(
                statement, "call Preloader(", ")", function.name);
        } else if (*definition.returns == "boolean") {
            payload = UnwrapStatement(
                statement, "return GetLocalizedHotkey(", ") != 0", function.name);
        } else if (*definition.returns == "integer") {
            payload = UnwrapStatement(
                statement, "return GetLocalizedHotkey(", ")", function.name);
        } else if (*definition.returns == "real") {
            payload = UnwrapStatement(
                statement, "return S2R(GetLocalizedString(", "))", function.name);
        } else if (*definition.returns == "string") {
            payload = UnwrapStatement(
                statement, "return GetLocalizedString(", ")", function.name);
        } else {
            Fail(function.name + " has an unsupported Catalog return type");
        }
        const std::string route = ValidatePayloadExpression(payload, definition);
        Expect(payloadRoutes.insert(route).second,
            "generated JASS reuses canonical payload route " + route);
    }
    Expect(visitedWrappers.size() == definitions.size(),
        "one or more Catalog definitions have no generated JASS wrapper");
}

void ValidateJass(const std::filesystem::path& path) {
    std::vector<std::string> globalComments;
    const auto functions = ParseJassSubset(ReadText(path), globalComments);
    ValidateJassPreamble(globalComments);

    const auto expected = ExpectedJassFunctions();
    Expect(functions.size() == expected.size(),
        "generated JASS must contain exactly four helpers and eight public wrappers");
    std::set<std::string> names;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto& actual = functions[index];
        const auto& wanted = expected[index];
        Expect(names.insert(actual.name).second, "duplicate generated JASS function " + actual.name);
        Expect(actual.name == wanted.name, "generated JASS function order/name mismatch");
        Expect(actual.takes == wanted.takes, actual.name + " takes signature mismatch");
        Expect(actual.returns == wanted.returns, actual.name + " return type mismatch");
        Expect(actual.comments == std::vector<std::string>{wanted.comment},
            actual.name + " warning/comment mismatch");
        Expect(actual.statements == wanted.statements,
            actual.name + " statement or carrier expression mismatch");
    }
}

} // namespace

int main(int argc, char** argv) {
    bool generic = false;
    int pathOffset = 1;
    if (argc == 5 && std::string_view(argv[1]) == "--generic") {
        generic = true;
        pathOffset = 2;
    } else if (argc == 5 && std::string_view(argv[1]) == "--synthetic") {
        pathOffset = 2;
    } else if (argc != 4) {
        std::cerr << "usage: warvk-generated-static-verifier "
                     "[--synthetic|--generic] <mapui-catalog-root> "
                     "<legacy-catalog-root> <WarVK.j>\n";
        return 2;
    }
    try {
        const std::filesystem::path mapuiRoot = argv[pathOffset];
        const std::filesystem::path legacyRoot = argv[pathOffset + 1];
        const std::filesystem::path jassPath = argv[pathOffset + 2];
        const auto definitions = ValidateGenericCatalogs(mapuiRoot, legacyRoot);
        ValidateGenericJass(jassPath, definitions);
        if (!generic) {
            ValidateCatalogs(mapuiRoot, legacyRoot);
            ValidateJass(jassPath);
        }
        std::cout << "WarVK generated Catalog/JASS "
                  << (generic ? "generic" : "synthetic exact")
                  << " static gate passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "WarVK generated static gate failed: " << error.what() << '\n';
        return 1;
    }
}
