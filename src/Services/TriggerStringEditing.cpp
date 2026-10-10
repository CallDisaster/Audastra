#include "War3MapCore/Services/TriggerStringEditing.h"

#include "War3MapCore/MapData/MapFields.h"
#include "War3MapCore/Trigger/TriggerEditing.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <type_traits>

namespace War3Map::Services {
namespace {
template<class T> Result<T> Fail(std::string code, std::string message) {
    return Result<T>::Failure("Trigger.StringEdit", std::move(code), std::move(message));
}
std::string_view Unquote(std::string_view value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') return value.substr(1, value.size() - 2);
    return value;
}
bool Same(const Trigger::ArgumentNode& a, const Trigger::ArgumentNode& b) {
    return a.id == b.id && a.type == b.type && a.value.text == b.value.text && a.value.encoding == b.value.encoding
        && a.value.originalBytes == b.value.originalBytes && a.insertCall == b.insertCall && a.callEca == b.callEca
        && a.insertIndex == b.insertIndex && a.indexArgument == b.indexArgument;
}
const MapData::StringTableDocument* Strings(const NameResolutionContext& context) {
    if (context.strings) return context.strings;
    return context.mapDocument && context.mapDocument->Strings() ? &*context.mapDocument->Strings() : nullptr;
}
const Trigger::ArgumentNode* UniqueArgument(const Trigger::TriggerDocument& document, StableId id) {
    const Trigger::ArgumentNode* found = nullptr;
    for (const auto& argument : document.argumentNodes) if (argument.id == id) {
        if (found) return nullptr;
        found = &argument;
    }
    return found;
}
} // namespace

TriggerStringUsage CountTriggerStringUsage(const NameResolutionContext& context, std::string_view reference) {
    reference = Unquote(reference);
    TriggerStringUsage usage;
    usage.complete = true;
    const auto referenceId = StringReferenceId(reference);
    const auto add = [&](std::string_view value) { if (referenceId && StringReferenceId(value) == referenceId) ++usage.count; };
    if (context.triggers) {
        for (const auto& argument : context.triggers->argumentNodes) if (!argument.callEca && argument.type == 3) add(argument.value.text);
        for (const auto& variable : context.triggers->variables) if (variable.hasDefault) add(variable.value.text);
        usage.coverage.emplace_back("WTG 标量参数和变量初值");
        if (!context.triggers->wct.customCode.text.empty() || std::any_of(context.triggers->triggers.begin(), context.triggers->triggers.end(),
            [](const auto& trigger) { return !trigger.customText.text.empty(); })) {
            usage.complete = false;
            usage.limitations.emplace_back("自定义代码中的动态字符串引用不能静态确定");
        }
    } else { usage.complete = false; usage.limitations.emplace_back("触发器文档未加载"); }
    std::array<bool, 7> provided{};
    for (const auto& source : context.objects) {
        const auto index = static_cast<std::size_t>(source.type);
        if (index >= provided.size() || provided[index]) { usage.complete = false; usage.limitations.emplace_back("物编来源重复或类型无效"); continue; }
        provided[index] = true;
        if (source.state == ProjectSearchState::Missing) continue;
        if (source.state != ProjectSearchState::Ready) { usage.complete = false; usage.limitations.emplace_back("部分物编来源未完整加载"); }
        if (source.state == ProjectSearchState::Ready || source.state == ProjectSearchState::Partial)
            for (const auto& object : source.records) for (const auto& modification : object.modifications)
                add(ObjectData::ValueToDisplayString(modification.value));
    }
    if (!std::all_of(provided.begin(), provided.end(), [](bool b) { return b; })) {
        usage.complete = false; usage.limitations.emplace_back("未提供全部七类物编来源");
    }
    usage.coverage.emplace_back("已加载物编字段");
    if (context.mapDocument) {
        const auto& map = *context.mapDocument;
        if (map.Settings()) {
            const auto& settings = *map.Settings();
            add(settings.mapName); add(settings.author); add(settings.description); add(settings.recommendedPlayers);
            for (const auto* screen : {&settings.loadingScreen, &settings.prologueScreen}) {
                add(screen->path); add(screen->text); add(screen->title); add(screen->subtitle);
            }
            add(settings.environment.soundEnvironment);
            for (const auto& player : settings.players) add(player.name);
            for (const auto& force : settings.forces) add(force.name);
            if (settings.custom) {
                for (const auto& table : settings.custom->randomUnits) add(table.name);
                for (const auto& table : settings.custom->randomItems) add(table.name);
            }
            if (map.Member(MapDataKind::Settings).state != MapDataState::Ready) {
                usage.complete = false; usage.limitations.emplace_back("W3I 有未完整解析部分");
            }
        } else if (map.Member(MapDataKind::Settings).state != MapDataState::Missing) {
            usage.complete = false; usage.limitations.emplace_back("W3I 未完整加载");
        }
        for (const auto kind : {MapDataKind::Regions, MapDataKind::Cameras, MapDataKind::Sounds}) {
            if (map.Member(kind).state == MapDataState::Missing) continue;
            if (map.Member(kind).state != MapDataState::Ready) { usage.complete = false; usage.limitations.emplace_back("部分基础地图成员未完整加载"); continue; }
            auto count = MapData::MapFieldRowCount(map, kind);
            if (!count.Succeeded()) { usage.complete = false; continue; }
            for (std::size_t row = 0; row < *count.value; ++row) for (const auto& field : MapData::MapFieldCatalog(kind)) {
                if (field.type != MapData::MapFieldType::Bytes) continue;
                const auto value = MapData::ReadMapField(map, kind, row, field);
                if (value.Succeeded()) add(value.value->bytes);
                else if (value.diagnostics.empty() || !std::all_of(value.diagnostics.begin(), value.diagnostics.end(),
                    [](const auto& diagnostic) { return diagnostic.code == "MAP_FIELD_NOT_APPLICABLE"; })) {
                    // 目录同时包含经典版和扩展版字段；当前记录不存在的可选字段不属于遗漏。
                    usage.complete = false;
                }
            }
        }
        usage.coverage.emplace_back("W3I、区域、镜头、声音的已知字符串字段");
    } else { usage.complete = false; usage.limitations.emplace_back("基础地图文档未加载"); }
    // 运行时脚本、导入文件等可能主动读取 WTS；产品始终说明这是已知编辑字段的计数。
    usage.limitations.emplace_back("计数限于以上编辑字段，不包含运行时代码和任意导入文件中的引用");
    return usage;
}

Result<TriggerStringEditPlan> PrepareTriggerStringEdit(const NameResolutionContext& context,
    StableId argumentId, std::string_view newTextUtf8, TriggerStringEditScope scope) {
    if (!context.triggers || !argumentId) return Fail<TriggerStringEditPlan>("STRING_ARGUMENT_MISSING", "触发器参数身份无效");
    const auto* argument = UniqueArgument(*context.triggers, argumentId);
    if (!argument || argument->type != 3 || argument->insertCall || argument->callEca || argument->insertIndex || argument->indexArgument)
        return Fail<TriggerStringEditPlan>("STRING_ARGUMENT_SHAPE", "仅地图字符串表标量参数可通过此事务修改");
    const auto reference = Unquote(argument->value.text);
    if (!reference.starts_with("TRIGSTR_")) return Fail<TriggerStringEditPlan>("STRING_REFERENCE_REQUIRED", "原参数不是地图字符串表引用");
    const auto resolution = ResolveName(context, argument->value.text, "string");
    if (resolution.state != NameResolutionState::Resolved) return Fail<TriggerStringEditPlan>("STRING_REFERENCE_UNRESOLVED", resolution.reason);
    if (newTextUtf8.size() > 4096) return Fail<TriggerStringEditPlan>("STRING_TEXT_BUDGET", "单条文字最多 4096 UTF-8 字节");
    MapData::MapFieldValue text;
    text.type = MapData::MapFieldType::Bytes;
    text.bytes = newTextUtf8;
    const auto valid = MapData::ValidateMapFieldValue(MapDataKind::Strings, "value", text);
    if (!valid.Succeeded()) { Result<TriggerStringEditPlan> result; result.diagnostics = valid.diagnostics; return result; }
    const auto* strings = Strings(context);
    const auto wire = strings->Serialize();
    if (!wire.Succeeded()) { Result<TriggerStringEditPlan> result; result.diagnostics = wire.diagnostics; return result; }
    TriggerStringEditPlan plan;
    plan.before_ = plan.after_ = *argument;
    plan.stringsBeforeBytes_ = *wire.value;
    // 使用当前序列化重解析的跨度，overlay 不沿用旧正文长度。
    auto reparsed = MapData::ParseStringTable(*wire.value);
    if (!reparsed.Succeeded()) { Result<TriggerStringEditPlan> result; result.diagnostics = reparsed.diagnostics; return result; }
    plan.stringsAfter_ = std::move(*reparsed.value);
    plan.usage_ = CountTriggerStringUsage(context, reference);
    std::uint32_t id = 0;
    const auto digits = reference.substr(8);
    std::from_chars(digits.data(), digits.data() + digits.size(), id);
    const auto original = std::find_if(plan.stringsAfter_.entries.begin(), plan.stringsAfter_.entries.end(),
        [id](const auto& entry) { return entry.id == id; });
    if (original == plan.stringsAfter_.entries.end())
        return Fail<TriggerStringEditPlan>("STRING_REFERENCE_UNRESOLVED", "原字符串条目不可读取");
    auto body = Trigger::DecodeSourceText(std::as_bytes(std::span(original->value.data(), original->value.size())));
    if (body.text == newTextUtf8) return Fail<TriggerStringEditPlan>("STRING_NO_CHANGE", "文字没有变化");
    // ASCII 正文没有编码辨识度；采用同一原表中首个有非 ASCII 字节正文的编码。
    // 逐条处理避免把混合来源整个转码，也不为一次编辑复制整张 WTS。
    const auto nonAscii = [](std::string_view bytes) {
        return std::any_of(bytes.begin(), bytes.end(), [](unsigned char c) { return c >= 0x80; });
    };
    if (!nonAscii(original->value)) {
        for (const auto& entry : plan.stringsAfter_.entries) if (nonAscii(entry.value)) {
            body.encoding = Trigger::DecodeSourceText(std::as_bytes(std::span(entry.value.data(), entry.value.size()))).encoding;
            break;
        }
    }
    body.text = newTextUtf8;
    const auto encodedBody = body.ToBytes();
    if (!encodedBody.Succeeded()) { Result<TriggerStringEditPlan> result; result.diagnostics = encodedBody.diagnostics; return result; }
    const std::string newSourceText = encodedBody.value->empty() ? std::string{} :
        std::string(reinterpret_cast<const char*>(encodedBody.value->data()), encodedBody.value->size());
    if (scope == TriggerStringEditScope::AllReferences) {
        for (auto& entry : plan.stringsAfter_.entries) if (entry.id == id) {
            entry.value = newSourceText;
        }
    } else {
        const auto added = plan.stringsAfter_.AddString(newSourceText);
        if (!added.Succeeded()) { Result<TriggerStringEditPlan> result; result.diagnostics = added.diagnostics; return result; }
        const bool quoted = argument->value.text.front() == '"';
        plan.after_.value.text = (quoted ? "\"" : "") + std::string("TRIGSTR_") + std::to_string(*added.value) + (quoted ? "\"" : "");
        const auto encoded = plan.after_.value.ToBytes();
        if (!encoded.Succeeded()) { Result<TriggerStringEditPlan> result; result.diagnostics = encoded.diagnostics; return result; }
    }
    const auto afterWire = plan.stringsAfter_.Serialize();
    if (!afterWire.Succeeded()) { Result<TriggerStringEditPlan> result; result.diagnostics = afterWire.diagnostics; return result; }
    return Result<TriggerStringEditPlan>::Success(std::move(plan));
}

Result<TriggerStringEditReceipt> ApplyTriggerStringEdit(Trigger::TriggerDocument& document,
    MapData::MapDataEditSession& edits, const TriggerStringEditPlan& plan) {
    auto* argument = const_cast<Trigger::ArgumentNode*>(UniqueArgument(document, plan.before_.id));
    if (!argument || !Same(*argument, plan.before_)) return Fail<TriggerStringEditReceipt>("STRING_ARGUMENT_STALE", "参数已变化，请重新编辑");
    auto current = edits.Snapshot(MapDataKind::Strings, 64 * 1024 * 1024);
    if (!current.Succeeded() || !current.value->Strings()) return Fail<TriggerStringEditReceipt>("STRING_TABLE_UNAVAILABLE", "当前字符串表不可完整读取");
    const auto currentBytes = current.value->Strings()->Serialize();
    if (!currentBytes.Succeeded() || *currentBytes.value != plan.stringsBeforeBytes_)
        return Fail<TriggerStringEditReceipt>("STRING_TABLE_STALE", "字符串表已变化，请重新编辑");
    TriggerStringEditReceipt receipt;
    receipt.before_ = plan.before_;
    receipt.after_ = plan.after_;
    auto replacement = receipt.after_;
    const auto committed = edits.EditStrings(plan.stringsAfter_);
    if (!committed.Succeeded()) { Result<TriggerStringEditReceipt> result; result.diagnostics = committed.diagnostics; return result; }
    receipt.strings_ = edits.LastEdit();
    static_assert(std::is_nothrow_swappable_v<Trigger::ArgumentNode>);
    std::swap(*argument, replacement);
    return Result<TriggerStringEditReceipt>::Success(std::move(receipt));
}

Status ReplayTriggerStringEdit(Trigger::TriggerDocument& document, MapData::MapDataEditSession& edits,
    const TriggerStringEditReceipt& receipt, bool forward) {
    auto* argument = const_cast<Trigger::ArgumentNode*>(UniqueArgument(document, receipt.ArgumentId()));
    if (!argument || !Same(*argument, forward ? receipt.before_ : receipt.after_))
        return Status::Failure("Trigger.StringEdit", "STRING_ARGUMENT_STALE", "参数与共同撤销回执不一致，保留两份当前数据");
    auto replacement = forward ? receipt.after_ : receipt.before_;
    const auto status = edits.Replay(receipt.strings_, forward);
    if (!status.Succeeded()) return status;
    std::swap(*argument, replacement);
    return Status::Ok();
}

} // namespace War3Map::Services
