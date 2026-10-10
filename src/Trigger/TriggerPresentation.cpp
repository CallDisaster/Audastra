#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Trigger/TriggerPresentation.h"

#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace War3Map::Trigger {
namespace {

constexpr std::size_t kMaximumPresentationDepth = 64;



std::optional<std::string_view> Field(
    std::span<const std::pair<std::string, std::string>> fields,
    std::string_view name) {
    const std::string lowerName = Text::LowerCType(name);
    const auto match = std::find_if(fields.begin(), fields.end(), [&](const auto& field) {
        return Text::LowerCType(field.first) == lowerName;
    });
    return match == fields.end()
        ? std::nullopt
        : std::optional<std::string_view>(match->second);
}

bool IsHex(char character) {
    const unsigned char value = static_cast<unsigned char>(character);
    return std::isxdigit(value) != 0;
}

std::string CleanUiText(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size();) {
        if (index + 10 <= value.size() && value[index] == '|' &&
            (value[index + 1] == 'c' || value[index + 1] == 'C') &&
            std::all_of(value.begin() + static_cast<std::ptrdiff_t>(index + 2),
                value.begin() + static_cast<std::ptrdiff_t>(index + 10), IsHex)) {
            index += 10;
            continue;
        }
        if (index + 2 <= value.size() && value[index] == '|') {
            const char control = static_cast<char>(
                std::tolower(static_cast<unsigned char>(value[index + 1])));
            if (control == 'r') {
                index += 2;
                continue;
            }
            if (control == 'n') {
                result.push_back(' ');
                index += 2;
                continue;
            }
        }
        result.push_back(value[index]);
        ++index;
    }
    return result;
}

std::string ResolvePreset(const UiCatalog& catalog, std::string_view rawValue) {
    const auto preset = FindPreset(catalog, rawValue);
    if (!preset.has_value()) {
        return std::string(rawValue);
    }
    // 显示名无法解析时（未提供字符串表），脚本值仍比内部预设键更适合作为只读展示。
    return CleanUiText(preset->display);
}

std::string PresentRawArgument(
    const TriggerDocument& document,
    const ArgumentNode& argument) {
    (void)document;
    return CleanUiText(argument.value.text);
}

struct PresentationContext {
    const TriggerDocument& document;
    const UiCatalog& catalog;
    const PresentationOptions* options = nullptr;
    std::unordered_set<std::uint32_t> ecaStack;
    std::unordered_set<std::uint32_t> argumentStack;
    std::optional<TriggerTypeSystem> ownedTypes;
    const TriggerTypeSystem* types = nullptr;
    std::vector<LocalVariable> locals;
    StableId localsOwner = 0;

    PresentationContext(const TriggerDocument& doc, const UiCatalog& cat, const PresentationOptions* opts)
        : document(doc), catalog(cat), options(opts) {
        if (opts && opts->typeSystem) types = opts->typeSystem;
        else { ownedTypes.emplace(cat); types = &*ownedTypes; }
    }

    std::string ExpectedType(const EcaNode& eca, std::size_t position) {
        const auto* definition = catalog.Find(eca.type, eca.name.text);
        if (!definition) return {};
        if (TriggerTypeSystem::DeclaredType(*definition, position) == "Null" && eca.sourceTriggerId != localsOwner) {
            localsOwner = eca.sourceTriggerId;
            locals.clear();
            const auto trigger = std::find_if(document.triggers.begin(), document.triggers.end(),
                [&](const auto& candidate) { return candidate.id == localsOwner; });
            if (trigger != document.triggers.end()) locals = types->CollectLocals(document, *trigger);
        }
        return types->ExpectedType(document, eca, position, locals);
    }
};

// 物体引用类 UI 参数类型（值为四字码 rawcode）。
bool IsObjectCodeType(std::string_view uiType) {
    return uiType == "unitcode" || uiType == "itemcode" || uiType == "abilcode" ||
        uiType == "buffcode" || uiType == "techcode" || uiType == "doodadcode" ||
        uiType == "destructablecode" || uiType == "heroskillcode";
}

EcaPresentation PresentEcaInternal(
    PresentationContext& context,
    std::uint32_t ecaIndex,
    std::size_t depth);

std::string PresentArgumentInternal(
    PresentationContext& context,
    std::uint32_t argumentIndex,
    std::size_t depth, std::string_view uiType = {}, ReferencePresentation* reference = nullptr) {
    if (depth > kMaximumPresentationDepth ||
        argumentIndex >= context.document.argumentNodes.size() ||
        !context.argumentStack.insert(argumentIndex).second) {
        return "<参数结构无效>";
    }

    const ArgumentNode& argument = context.document.argumentNodes[argumentIndex];
    if (reference) reference->rawValue = argument.value.text;
    std::string resolverType(uiType);
    if (context.options && context.options->referenceResolver) {
        for (std::size_t i = 0; i < 64 && !IsObjectCodeType(resolverType); ++i) {
            const auto* type = context.types->Find(resolverType);
            if (!type || type->baseType.empty() || type->baseType == resolverType) break;
            resolverType = type->baseType;
        }
    }
    std::string result;
    if (argument.insertCall != 0 && argument.callEca.has_value()) {
        result = PresentEcaInternal(context, *argument.callEca, depth + 1).text;
    } else if (context.options && context.options->showRawValues) {
        result = argument.value.text;
    } else if (argument.type == 0) {
        result = ResolvePreset(context.catalog, argument.value.text);
        if (context.options && context.options->referenceResolver && IsObjectCodeType(resolverType)) {
            if (const auto preset = FindPreset(context.catalog, argument.value.text)) {
                auto resolved = context.options->referenceResolver(preset->script, resolverType);
                if (!resolved.displayName.empty() && resolved.state == "resolved") result = resolved.displayName;
                resolved.rawValue = argument.value.text;
                if (reference) *reference = std::move(resolved);
            }
        }
    } else {
        result = PresentRawArgument(context.document, argument);
        if (context.options && context.options->referenceResolver) {
            auto resolved = context.options->referenceResolver(argument.value.text, resolverType);
            if (!resolved.displayName.empty()) result = resolved.displayName;
            if (reference) *reference = std::move(resolved);
        } else if (context.options && context.options->objectNameResolver && IsObjectCodeType(uiType)) {
            auto rawcode = argument.value.text;
            if (rawcode.size() == 6 && (rawcode.front() == '"' || rawcode.front() == '\'') && rawcode.back() == rawcode.front())
                rawcode = rawcode.substr(1, 4);
            if (rawcode.size() == 4) {
                const auto name = context.options->objectNameResolver(rawcode);
                if (!name.empty()) result = name;
            }
        }
    }

    if (argument.insertIndex != 0 && argument.indexArgument.has_value()) {
        result.push_back('[');
        result.append(PresentArgumentInternal(context, *argument.indexArgument, depth + 1));
        result.push_back(']');
    }
    if (reference) reference->displayName = result;
    context.argumentStack.erase(argumentIndex);
    return result;
}

EcaPresentation PresentEcaInternal(
    PresentationContext& context,
    std::uint32_t ecaIndex,
    std::size_t depth) {
    EcaPresentation result;
    if (depth > kMaximumPresentationDepth || ecaIndex >= context.document.ecaNodes.size()) {
        result.text = "<ECA 结构无效>";
        return result;
    }
    const EcaNode& eca = context.document.ecaNodes[ecaIndex];
    result.rawFunctionName = eca.name.text;
    if (!context.ecaStack.insert(ecaIndex).second) {
        result.text = "<ECA 循环引用：" + eca.name.text + ">";
        return result;
    }

    const UiDefinition* definition = context.catalog.Find(eca.type, eca.name.text);
    result.definitionAvailable = definition != nullptr;
    if (definition == nullptr) {
        result.text = "[缺失 UI 定义] " + eca.name.text;
        context.ecaStack.erase(ecaIndex);
        return result;
    }

    const UiDefinitionRevision& revision = definition->Active();
    const auto description = Field(revision.fields, "description");
    const auto title = Field(revision.fields, "title");
    std::string format = description.has_value()
        ? CleanUiText(*description)
        : (title.has_value() ? CleanUiText(*title) : std::string{});

    struct DisplayedArgument {
        std::uint32_t index = 0;
        std::string uiType; // 空 = 定义外参数
        std::size_t source = 0;
    };
    std::vector<DisplayedArgument> displayedArguments;
    displayedArguments.reserve(eca.arguments.size());
    std::size_t sourceArgument = 0;
    for (const UiArgumentDefinition& argumentDefinition : revision.arguments) {
        if (argumentDefinition.type == "nothing") {
            continue;
        }
        if (sourceArgument < eca.arguments.size()) {
            displayedArguments.push_back(
                DisplayedArgument{eca.arguments[sourceArgument], context.ExpectedType(eca, sourceArgument), sourceArgument});
        }
        ++sourceArgument;
    }
    while (sourceArgument < eca.arguments.size()) {
        displayedArguments.push_back(
            DisplayedArgument{eca.arguments[sourceArgument], {}, sourceArgument});
        ++sourceArgument;
    }

    std::string rendered;
    rendered.reserve(format.size() + displayedArguments.size() * 12);
    const auto recordSpan = [&](const DisplayedArgument& shown, std::size_t begin, const ReferencePresentation& reference) {
        PresentationArgumentSpan span;
        span.begin = begin;
        span.length = rendered.size() - begin;
        span.position = shown.source;
        span.uiType = shown.uiType;
        span.rawValue = reference.rawValue;
        span.displayName = reference.displayName;
        span.iconPath = reference.iconPath;
        span.issue = reference.issue;
        span.reference = reference.reference;
        span.state = reference.state;
        span.candidates = reference.candidates;
        if (shown.index < context.document.argumentNodes.size()) {
            const ArgumentNode& argument = context.document.argumentNodes[shown.index];
            span.call = argument.insertCall != 0 && argument.callEca.has_value();
            span.empty = !span.call && CleanUiText(argument.value.text).empty();
        }
        result.arguments.push_back(std::move(span));
    };
    std::size_t cursor = 0;
    std::size_t argumentPosition = 0;
    bool placeholdersValid = true;
    while (cursor < format.size()) {
        const std::size_t opening = format.find("${", cursor);
        if (opening == std::string::npos) {
            rendered.append(format.substr(cursor));
            break;
        }
        rendered.append(format.substr(cursor, opening - cursor));
        const std::size_t closing = format.find('}', opening + 2);
        if (closing == std::string::npos) {
            rendered.append(format.substr(opening));
            placeholdersValid = false;
            break;
        }
        if (argumentPosition < displayedArguments.size()) {
            const DisplayedArgument& shown = displayedArguments[argumentPosition];
            ReferencePresentation reference;
            std::string text = PresentArgumentInternal(
                context,
                shown.index,
                depth + 1, shown.uiType, &reference);
            const std::size_t begin = rendered.size();
            rendered.append(std::move(text));
            recordSpan(shown, begin, reference);
            ++result.renderedArgumentCount;
        } else {
            rendered.append("<缺少参数>");
            placeholdersValid = false;
        }
        ++argumentPosition;
        cursor = closing + 1;
    }

    if (format.empty()) {
        rendered = eca.name.text;
        result.arguments.clear();
        placeholdersValid = false;
    }
    if (argumentPosition < displayedArguments.size()) {
        rendered.append(rendered.empty() ? "参数：" : "（参数：");
        for (std::size_t index = argumentPosition; index < displayedArguments.size(); ++index) {
            if (index != argumentPosition) {
                rendered.append("，");
            }
            const std::size_t begin = rendered.size();
            ReferencePresentation reference;
            rendered.append(PresentArgumentInternal(
                context,
                displayedArguments[index].index,
                depth + 1, displayedArguments[index].uiType, &reference));
            recordSpan(displayedArguments[index], begin, reference);
            ++result.renderedArgumentCount;
        }
        if (!rendered.starts_with("参数：")) {
            rendered.append("）");
        }
        placeholdersValid = false;
    }
    result.text = rendered;
    result.templateComplete = placeholdersValid &&
        argumentPosition == displayedArguments.size();
    context.ecaStack.erase(ecaIndex);
    return result;
}

std::optional<std::uint32_t> FindEcaIndex(
    const TriggerDocument& document,
    const EcaNode& eca) {
    for (std::size_t index = 0; index < document.ecaNodes.size(); ++index) {
        if (&document.ecaNodes[index] == &eca) {
            return static_cast<std::uint32_t>(index);
        }
    }
    const auto match = std::find_if(
        document.ecaNodes.begin(),
        document.ecaNodes.end(),
        [&](const EcaNode& candidate) { return candidate.id == eca.id; });
    return match == document.ecaNodes.end()
        ? std::nullopt
        : std::optional<std::uint32_t>(
            static_cast<std::uint32_t>(match - document.ecaNodes.begin()));
}

} // namespace

EcaPresentation PresentEca(
    const TriggerDocument& document,
    std::uint32_t ecaIndex,
    const UiCatalog& catalog) {
    PresentationContext context{document, catalog, nullptr};
    return PresentEcaInternal(context, ecaIndex, 0);
}

EcaPresentation PresentEca(
    const TriggerDocument& document,
    std::uint32_t ecaIndex,
    const UiCatalog& catalog,
    const PresentationOptions& options) {
    PresentationContext context{document, catalog, &options};
    return PresentEcaInternal(context, ecaIndex, 0);
}

EcaPresentation PresentEca(
    const TriggerDocument& document,
    const EcaNode& eca,
    const UiCatalog& catalog) {
    const auto index = FindEcaIndex(document, eca);
    if (!index.has_value()) {
        return EcaPresentation{
            "<ECA 不属于当前文档>",
            eca.name.text,
            false,
            false,
            0,
        };
    }
    return PresentEca(document, *index, catalog);
}

EcaPresentation PresentEca(
    const TriggerDocument& document,
    const EcaNode& eca,
    const UiCatalog& catalog,
    const PresentationOptions& options) {
    const auto index = FindEcaIndex(document, eca);
    if (!index.has_value()) {
        return EcaPresentation{
            "<ECA 不属于当前文档>",
            eca.name.text,
            false,
            false,
            0,
        };
    }
    return PresentEca(document, *index, catalog, options);
}

std::string PresentArgument(
    const TriggerDocument& document,
    std::uint32_t argumentIndex,
    const UiCatalog& catalog) {
    PresentationContext context{document, catalog, nullptr};
    return PresentArgumentInternal(context, argumentIndex, 0);
}

DefinitionNote PresentDefinitionNote(const UiDefinition& definition, const UiCatalog& catalog) {
    const auto& fields = definition.Active().fields;
    const auto text = [&](std::string_view key) {
        const auto value = Field(fields, key);
        return value ? CleanUiText(catalog.Localize(*value)) : std::string{};
    };
    DefinitionNote note{text("title"), text("hint")};
    if (note.hint.empty()) note.hint = text("comment");
    return note;
}

} // namespace War3Map::Trigger
