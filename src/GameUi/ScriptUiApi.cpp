#include "ScriptUiApi.h"

#include "War3MapCore/GameUi/GameCodeLayout.h"

#include <unordered_map>

namespace War3Map::GameUi::detail {
namespace {

constexpr auto kBlz = UiApiFamily::Blizzard;
constexpr auto kDz = UiApiFamily::Dz;
constexpr auto kMh = UiApiFamily::MemHack;

// 认识的界面函数。设置类调用的第 0 个参数都是框架；参数位置与各接口的公开声明一致。
constexpr CallSpec kCalls[] = {
    {.name = "BlzCreateFrame", .family = kBlz, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "BlzCreateSimpleFrame", .family = kBlz, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "BlzCreateFrameByType", .family = kBlz, .kind = CallKind::Create, .typeArg = 0, .nameArg = 1,
        .templateArg = 3, .parentArg = 2},
    {.name = "BlzGetOriginFrame", .family = kBlz, .kind = CallKind::Origin},
    {.name = "BlzGetFrameByName", .family = kBlz, .kind = CallKind::ByName},
    {.name = "BlzFrameSetAbsPoint", .family = kBlz, .kind = CallKind::SetAbsPoint},
    {.name = "BlzFrameSetPoint", .family = kBlz, .kind = CallKind::SetPoint},
    {.name = "BlzFrameClearAllPoints", .family = kBlz, .kind = CallKind::ClearPoints},
    {.name = "BlzFrameSetAllPoints", .family = kBlz, .kind = CallKind::SetAllPoints},
    {.name = "BlzFrameSetSize", .family = kBlz, .kind = CallKind::SetSize},
    {.name = "BlzFrameSetTexture", .family = kBlz, .kind = CallKind::SetTexture},
    {.name = "BlzFrameSetText", .family = kBlz, .kind = CallKind::SetText},
    {.name = "BlzFrameSetVisible", .family = kBlz, .kind = CallKind::SetVisible},
    {.name = "BlzLoadTOCFile", .family = kBlz, .kind = CallKind::LoadToc},
    {.name = "BlzHideOriginFrames", .family = kBlz, .kind = CallKind::Other},
    {.name = "BlzEnableUIAutoPosition", .family = kBlz, .kind = CallKind::Other},
    {.name = "BlzDestroyFrame", .family = kBlz, .kind = CallKind::FrameOther},
    {.name = "BlzTriggerRegisterFrameEvent", .family = kBlz, .kind = CallKind::FrameOther},
    {.name = "BlzGetTriggerFrame", .family = kBlz, .kind = CallKind::Query},
    {.name = "BlzGetTriggerFrameEvent", .family = kBlz, .kind = CallKind::Query},
    {.name = "BlzGetTriggerFrameValue", .family = kBlz, .kind = CallKind::Query},
    {.name = "BlzGetTriggerFrameText", .family = kBlz, .kind = CallKind::Query},

    {.name = "DzCreateFrame", .family = kDz, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "DzCreateSimpleFrame", .family = kDz, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "DzCreateFrameByTagName", .family = kDz, .kind = CallKind::Create, .typeArg = 0, .nameArg = 1,
        .templateArg = 3, .parentArg = 2},
    {.name = "DzGetGameUI", .family = kDz, .kind = CallKind::GameUi},
    {.name = "DzFrameFindByName", .family = kDz, .kind = CallKind::ByName},
    {.name = "DzSimpleFrameFindByName", .family = kDz, .kind = CallKind::ByName},
    {.name = "DzSimpleFontStringFindByName", .family = kDz, .kind = CallKind::ByName},
    {.name = "DzSimpleTextureFindByName", .family = kDz, .kind = CallKind::ByName},
    {.name = "DzFrameGetMinimap", .family = kDz, .kind = CallKind::CodeArea, .area = "Minimap"},
    {.name = "DzFrameGetPortrait", .family = kDz, .kind = CallKind::CodeArea, .area = "Portrait"},
    {.name = "DzFrameGetChatMessage", .family = kDz, .kind = CallKind::CodeArea, .area = "ChatMessage"},
    {.name = "DzFrameGetTopMessage", .family = kDz, .kind = CallKind::CodeArea, .area = "TopMessage"},
    {.name = "DzFrameGetUnitMessage", .family = kDz, .kind = CallKind::CodeArea, .area = "UnitMessage"},
    {.name = "DzFrameGetItemBarButton", .family = kDz, .kind = CallKind::CodeArea, .area = "InventoryButton",
        .indexed = true},
    {.name = "DzFrameGetMinimapButton", .family = kDz, .kind = CallKind::CodeArea, .area = "MinimapButton",
        .indexed = true},
    {.name = "DzFrameSetAbsolutePoint", .family = kDz, .kind = CallKind::SetAbsPoint},
    {.name = "DzFrameSetPoint", .family = kDz, .kind = CallKind::SetPoint},
    {.name = "DzFrameClearAllPoints", .family = kDz, .kind = CallKind::ClearPoints},
    {.name = "DzFrameSetAllPoints", .family = kDz, .kind = CallKind::SetAllPoints},
    {.name = "DzFrameSetSize", .family = kDz, .kind = CallKind::SetSize},
    {.name = "DzFrameSetTexture", .family = kDz, .kind = CallKind::SetTexture},
    {.name = "DzFrameSetText", .family = kDz, .kind = CallKind::SetText},
    {.name = "DzFrameShow", .family = kDz, .kind = CallKind::SetVisible},
    {.name = "DzLoadToc", .family = kDz, .kind = CallKind::LoadToc},
    {.name = "DzFrameHideInterface", .family = kDz, .kind = CallKind::Other},
    {.name = "DzFrameEditBlackBorders", .family = kDz, .kind = CallKind::Other},
    {.name = "DzDestroyFrame", .family = kDz, .kind = CallKind::FrameOther},

    {.name = "MHFrame_Create", .family = kMh, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "MHFrame_CreateEx", .family = kMh, .kind = CallKind::Create, .typeArg = 0, .nameArg = 1,
        .templateArg = 2, .parentArg = 3},
    {.name = "MHFrame_CreateSimple", .family = kMh, .kind = CallKind::Create, .templateArg = 0, .parentArg = 1},
    {.name = "MHUI_GetGameUI", .family = kMh, .kind = CallKind::GameUi},
    {.name = "MHFrame_GetByName", .family = kMh, .kind = CallKind::ByName},
    {.name = "MHFrame_SetAbsolutePoint", .family = kMh, .kind = CallKind::SetAbsPoint},
    {.name = "MHFrame_SetRelativePoint", .family = kMh, .kind = CallKind::SetPoint},
    {.name = "MHFrame_ClearAllPoints", .family = kMh, .kind = CallKind::ClearPoints},
    {.name = "MHFrame_SetAllPoints", .family = kMh, .kind = CallKind::SetAllPoints},
    {.name = "MHFrame_SetSize", .family = kMh, .kind = CallKind::SetSize},
    {.name = "MHFrame_SetWidth", .family = kMh, .kind = CallKind::SetWidth},
    {.name = "MHFrame_SetHeight", .family = kMh, .kind = CallKind::SetHeight},
    {.name = "MHFrame_SetTexture", .family = kMh, .kind = CallKind::SetTexture},
    {.name = "MHFrame_SetText", .family = kMh, .kind = CallKind::SetText},
    {.name = "MHFrame_Hide", .family = kMh, .kind = CallKind::SetHidden},
    {.name = "MHFrame_LoadTOC", .family = kMh, .kind = CallKind::LoadToc},
    {.name = "MHFrame_Destroy", .family = kMh, .kind = CallKind::FrameOther},
};

// 表外但属于界面函数族的名字：Get/Is 开头视为只读，其余列为不认识。
struct FamilyPrefix {
    std::string_view prefix;
    UiApiFamily family;
};
constexpr FamilyPrefix kPrefixes[] = {
    {"BlzFrame", kBlz}, {"DzFrame", kDz}, {"MHFrame_", kMh}, {"MHUI_", kMh}};

struct OriginEntry {
    std::string_view constant;
    std::string_view area;
    bool indexed = false;
};
constexpr OriginEntry kOrigins[] = {
    {"ORIGIN_FRAME_GAME_UI", "GameUI"},
    {"ORIGIN_FRAME_WORLD_FRAME", "WorldFrame"},
    {"ORIGIN_FRAME_HERO_BAR", "HeroBar"},
    {"ORIGIN_FRAME_PORTRAIT", "Portrait"},
    {"ORIGIN_FRAME_MINIMAP", "Minimap"},
    {"ORIGIN_FRAME_MINIMAP_BUTTON", "MinimapButton", true},
    {"ORIGIN_FRAME_ITEM_BUTTON", "InventoryButton", true},
    {"ORIGIN_FRAME_CHAT_MSG", "ChatMessage"},
    {"ORIGIN_FRAME_UNIT_MSG", "UnitMessage"},
    {"ORIGIN_FRAME_TOP_MSG", "TopMessage"},
};

std::optional<std::string> IndexedArea(std::string_view area, bool indexed, std::optional<float> index) {
    std::string id(area);
    if (indexed) {
        if (!index || *index < 0.0f) return std::nullopt;
        id += std::to_string(static_cast<int>(*index) + 1);
    }
    return id;
}

} // namespace

std::optional<CallSpec> LookupCall(std::string_view name) {
    static const auto table = [] {
        std::unordered_map<std::string_view, const CallSpec*> map;
        for (const auto& spec : kCalls) map.emplace(spec.name, &spec);
        return map;
    }();
    if (const auto found = table.find(name); found != table.end()) return *found->second;
    for (const auto& prefix : kPrefixes) {
        if (!name.starts_with(prefix.prefix)) continue;
        const std::string_view rest = name.substr(prefix.prefix.size());
        CallSpec spec;
        spec.name = name;
        spec.family = prefix.family;
        spec.kind = rest.starts_with("Get") || rest.starts_with("Is") ? CallKind::Query : CallKind::Unknown;
        return spec;
    }
    return std::nullopt;
}

bool IsSetter(CallKind kind) noexcept {
    return kind >= CallKind::SetAbsPoint && kind <= CallKind::SetHidden;
}

std::optional<std::string> OriginArea(std::string_view constant, std::optional<float> index) {
    for (const auto& known : kOrigins) {
        if (known.constant == constant) return IndexedArea(known.area, known.indexed, index);
    }
    return std::nullopt;
}

std::optional<std::string> CodeAreaOf(const CallSpec& spec, std::optional<float> index) {
    if (spec.kind != CallKind::CodeArea) return std::nullopt;
    return IndexedArea(spec.area, spec.indexed, index);
}

std::optional<ScriptFrameRef> FrameRefOfCall(const CallSpec& spec, const std::vector<std::optional<ScriptValue>>& args) {
    const auto argument = [&](std::size_t i) { return i < args.size() ? args[i] : std::nullopt; };
    std::optional<std::string> area;
    switch (spec.kind) {
    case CallKind::GameUi:
        area = std::string(kGameCodeRoot);
        break;
    case CallKind::Origin: {
        const auto origin = argument(0);
        if (origin && origin->kind == ScriptValue::Kind::Origin) area = OriginArea(origin->text, NumberOf(argument(1)));
        break;
    }
    case CallKind::CodeArea:
        area = CodeAreaOf(spec, NumberOf(argument(0)));
        break;
    case CallKind::ByName:
        if (auto name = StringOf(argument(0))) return ScriptFrameRef{ScriptFrameTarget::Named, std::move(*name), 0};
        return std::nullopt;
    default:
        return std::nullopt;
    }
    if (!area) return std::nullopt;
    return ScriptFrameRef{ScriptFrameTarget::CodeArea, std::move(*area), 0};
}

bool ApplyFrameCall(ScriptFrame& frame, CallKind kind, const std::vector<std::optional<ScriptValue>>& args,
    const ScriptFrameRef& relative) {
    const auto argument = [&](std::size_t i) { return i < args.size() ? args[i] : std::nullopt; };
    const auto upsert = [&](ScriptAnchor anchor) {
        for (auto& existing : frame.anchors) {
            if (existing.point == anchor.point) {
                existing = std::move(anchor);
                return;
            }
        }
        frame.anchors.push_back(std::move(anchor));
    };
    switch (kind) {
    case CallKind::SetAbsPoint: {
        const auto point = PointOf(argument(1));
        const auto x = NumberOf(argument(2));
        const auto y = NumberOf(argument(3));
        if (!point || !x || !y) return false;
        upsert(ScriptAnchor{*point, ScriptFrameRef{ScriptFrameTarget::Screen, {}, 0}, FramePoint::BottomLeft, *x, *y});
        return true;
    }
    case CallKind::SetPoint: {
        const auto point = PointOf(argument(1));
        const auto relativePoint = PointOf(argument(3));
        const auto x = NumberOf(argument(4));
        const auto y = NumberOf(argument(5));
        if (!point || relative.target == ScriptFrameTarget::Unknown || !relativePoint || !x || !y) return false;
        upsert(ScriptAnchor{*point, relative, *relativePoint, *x, *y});
        return true;
    }
    case CallKind::ClearPoints:
        frame.anchors.clear();
        frame.clearPoints = true;
        return true;
    case CallKind::SetAllPoints:
        if (relative.target == ScriptFrameTarget::Unknown) return false;
        upsert(ScriptAnchor{FramePoint::TopLeft, relative, FramePoint::TopLeft, 0.0f, 0.0f});
        upsert(ScriptAnchor{FramePoint::BottomRight, relative, FramePoint::BottomRight, 0.0f, 0.0f});
        return true;
    case CallKind::SetSize: {
        const auto width = NumberOf(argument(1));
        const auto height = NumberOf(argument(2));
        if (!width || !height) return false;
        frame.width = width;
        frame.height = height;
        return true;
    }
    case CallKind::SetWidth:
        if (const auto width = NumberOf(argument(1))) {
            frame.width = width;
            return true;
        }
        return false;
    case CallKind::SetHeight:
        if (const auto height = NumberOf(argument(1))) {
            frame.height = height;
            return true;
        }
        return false;
    case CallKind::SetTexture:
        if (auto path = StringOf(argument(1))) {
            frame.texture = std::move(*path);
            return true;
        }
        return false;
    case CallKind::SetText:
        if (auto text = StringOf(argument(1))) {
            frame.text = std::move(*text);
            return true;
        }
        return false;
    case CallKind::SetVisible:
    case CallKind::SetHidden:
        if (const auto flag = BooleanOf(argument(1))) {
            frame.visible = kind == CallKind::SetVisible ? *flag : !*flag;
            return true;
        }
        return false;
    default:
        return false;
    }
}

} // namespace War3Map::GameUi::detail
