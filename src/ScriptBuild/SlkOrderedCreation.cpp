#include "War3MapCore/Script/SlkOrderedCreation.h"
#include "War3MapCore/Text/TextUtil.h"
#include "SlkMaterializerInternal.h"

#include <algorithm>
#include <charconv>
#include <exception>
#include <set>

// 根据固定目标slk_lib.lua的语义独立实现，没有复制Lua实现。
// primary try_meta:31–81, fill:228–258, __newindex:303–392, new:447–489。
namespace War3Map::Script {
namespace {
namespace Detail = MaterializerDetail;
constexpr std::string_view kLibrary = "9BA2C9B8E5B53005CF5714E5188E533186D97F770C398E47E998B2B3D3CC74F1";
constexpr std::size_t kAbility = 1, kLevelLimit = 256;
using Scalar = SlkSessionScalar;
using Array = std::vector<Scalar>;
using Fields = SlkDatasetFields;
using Scope = SlkDatasetMetadataScope;
bool Letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool StaticId(std::string_view id) {
    return id.size() == 4 && std::all_of(id.begin(), id.end(), [](char c) { return Letter(c) || (c >= '0' && c <= '9'); });
}
[[noreturn]] void Reject(std::string code, std::string message, const OriginSpan& origin) {
    Detail::Reject("SLK_CREATION_" + std::move(code), std::move(message), origin);
}
const Scalar* Get(const Fields& fields, std::string_view key) {
    const auto found = fields.find(std::string(key));
    return found == fields.end() ? nullptr : std::get_if<Scalar>(&found->second.value);
}
const std::string* String(const Fields& fields, std::string_view key) {
    const auto* scalar = Get(fields, key); return scalar ? std::get_if<std::string>(scalar) : nullptr;
}
bool Truthy(const Scalar& value) {
    const auto* boolean = std::get_if<bool>(&value); return !boolean || *boolean;
}
std::size_t Level(const Scalar* value, const OriginSpan& origin) {
    const auto* number = value ? std::get_if<std::int64_t>(value) : nullptr;
    if (!number || *number < 0 || *number > static_cast<std::int64_t>(kLevelLimit))
        Reject("LEVEL_UNSUPPORTED", "new最大等级只支持0–256整数；不把浮点/布尔转换为级数。", origin);
    return static_cast<std::size_t>(*number);
}
struct Rule { std::string name; bool repeated = false, profile = false; };
using Rules = std::map<std::string, Rule>;
Rules RulesFor(const Scope& scope, Detail::Budget& budget, const OriginSpan& origin) {
    if (scope.fields.size() > 10000) Reject("LIMIT", "metadata超过10000字段。", origin);
    Rules result;
    constexpr std::string_view allowed[]{"appendindex", "cantempty", "concat", "data", "default", "displayname", "field", "id", "index", "key", "profile", "repeat", "splite", "type"};
    for (const auto& [name, record] : scope.fields) {
        budget.FieldKey(name, origin); budget.Text(name, origin);
        const auto* field = String(record.fields, "field");
        const auto* id = String(record.fields, "id");
        const auto* kindValue = Get(record.fields, "type");
        const auto* kind = kindValue ? std::get_if<std::int64_t>(kindValue) : nullptr;
        if (name.empty() || name.front() == '_' || !field || field->size() != name.size() || Text::LowerAscii(*field) != name || !id || id->empty() || id->size() > 4 ||
            !std::all_of(id->begin(), id->end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; }) ||
            !kind || *kind < 0 || *kind > 3 || !String(record.fields, "key"))
            Reject("METADATA_UNSUPPORTED", "canonical字段身份不完整，不能推断别名。", origin);
        for (const auto& [property, value] : record.fields) {
            if (std::find(std::begin(allowed), std::end(allowed), property) == std::end(allowed))
                Reject("METADATA_UNSUPPORTED", "canonical metadata含未支持属性。", origin);
            const auto* scalar = std::get_if<Scalar>(&value.value);
            if (property == "repeat" || property == "index" || property == "data") {
                const auto* number = scalar ? std::get_if<std::int64_t>(scalar) : nullptr;
                if (!number || *number < 0 || *number > 256) Reject("METADATA_UNSUPPORTED", "canonical索引须为有界整数。", origin);
            }
            if (property == "appendindex" || property == "profile" || property == "splite" || property == "cantempty" || property == "concat")
                if (!scalar || !std::holds_alternative<bool>(*scalar)) Reject("METADATA_UNSUPPORTED", "canonical标记须为boolean。", origin);
        }
        const auto* profile = Get(record.fields, "profile");
        result.emplace(name, Rule{name, record.fields.contains("repeat"), profile && std::get<bool>(*profile)});
    }
    return result;
}
const Rule* Find(const Rules& common, const Rules& specific, const std::string& key) {
    if (const auto found = common.find(key); found != common.end()) return &found->second;
    const auto found = specific.find(key); return found == specific.end() ? nullptr : &found->second;
}
struct Address { const Rule* rule = nullptr; std::optional<std::size_t> level; };
Address Resolve(std::string_view field, const Rules& common, const Rules& specific, Detail::Budget& budget, const OriginSpan& origin) {
    budget.FieldKey(field, origin);
    if (field.empty() || !Letter(field.front())) Reject("FIELD_UNSUPPORTED", "不支持内部字段或未知字段名。", origin);
    const std::string key = Text::LowerAscii(field);
    if (key == "w2lobject" || key == "code") Reject("FIELD_UNSUPPORTED", "不支持改写生成归属或base code。", origin);
    const auto letters = static_cast<std::size_t>(std::find_if_not(key.begin(), key.end(), Letter) - key.begin());
    const std::string prefix = key.substr(0, letters);
    const auto* indexed = Find(common, specific, prefix + "_1");
    if (indexed && key == prefix) return {indexed, {}};
    if (key == prefix) {
        const auto* rule = Find(common, specific, key);
        if (!rule) Reject("FIELD_UNSUPPORTED", "字段没有canonical定义，不能把未知写入当作可保存属性。", origin);
        return {rule, rule->repeated ? std::optional<std::size_t>{1} : std::nullopt};
    }
    const auto suffix = std::string_view(key).substr(letters);
    if (!suffix.empty() && std::all_of(suffix.begin(), suffix.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        const auto* rule = Find(common, specific, prefix);
        std::size_t level = 0;
        const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), level);
        if (!rule || !rule->repeated || parsed.ec != std::errc{} || level == 0 || level > kLevelLimit)
            Reject("FIELD_UNSUPPORTED", "显式等级字段必须匹配repeat且级号为1–256。", origin);
        return {rule, level};
    }
    // 闭集只支持真实indexed键。Lua未锚定模式对其他后缀的宽松匹配不在本单元猜测。
    if (indexed && suffix.size() > 1 && suffix.front() == '_' &&
        std::all_of(suffix.begin() + 1, suffix.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        bool reached = false;
        // 目标按连续indexed定义扫描；缺号后先尝试裸repeat，再查完整key。
        // 不能直接查完整foo_3绕过foo_2缺失时的裸foo优先级。
        for (std::size_t index = 1; ; ++index) {
            budget.Spend(budget.metadataVisits, 1, 50000000, origin);
            const auto candidate = prefix + "_" + std::to_string(index);
            if (key == candidate) { reached = true; break; }
            if (!Find(common, specific, candidate)) break;
        }
        const auto* bare = Find(common, specific, prefix);
        if (!reached && bare && bare->repeated)
            Reject("FIELD_UNSUPPORTED", "indexed缺号后会命中裸repeat并变成标量写入，当前子集不支持此歧义路径。", origin);
        const auto* rule = Find(common, specific, key);
        if (rule && !rule->repeated) return {rule, {}};
    }
    Reject("FIELD_UNSUPPORTED", "字段后缀或indexed地址未受支持。", origin);
}
void Fill(SlkSessionObject& object, const SlkDatasetRecord& base, const Rules& rules, Detail::Budget& budget,
    SlkOrderedCreation& result, const OriginSpan& origin) {
    const auto levels = Level(Detail::Scalar(object, "_max_level"), origin);
    for (const auto& [name, rule] : rules) {
        budget.Spend(budget.metadataVisits, 1, 50000000, origin);
        if (!rule.repeated) continue;
        auto found = object.fields.find(name);
        const auto original = base.fields.find(name);
        auto* data = found == object.fields.end() ? nullptr : std::get_if<Array>(&found->second);
        const auto* defaults = original == base.fields.end() ? nullptr : std::get_if<Array>(&original->second.value);
        if (!data || !defaults) Reject("FILL_UNSUPPORTED", "repeat缺少实际对象/默认数组；不能推断空表。", origin);
        while (data->size() < levels) {
            const auto index = data->size();
            const Scalar* selected = index < defaults->size() && Truthy((*defaults)[index]) ? &(*defaults)[index] : nullptr;
            if (!selected) {
                if (rule.profile) { if (!data->empty()) selected = &data->back(); }
                else if (!defaults->empty()) selected = &defaults->back();
            }
            if (!selected) Reject("FILL_UNSUPPORTED", "new fill会产生稀疏表，当前稠密图不支持。", origin);
            budget.ScalarValue(*selected, origin); // 在字符串复制/vector扩容前记账。
            data->push_back(*selected); ++result.filledValues;
        }
        if (data->size() > levels) data->resize(levels);
    }
}
SlkSessionObject CopyDefault(const std::string& id, const SlkDatasetRecord& record, Detail::Budget& budget, const OriginSpan& origin) {
    const auto* actualId = String(record.fields, "_id"); const auto* type = String(record.fields, "_type");
    if (!actualId || *actualId != id || !type || *type != "ability" || record.fields.contains("_parent") || record.fields.contains("_obj") ||
        record.fields.contains("_create") || record.fields.contains("w2lobject"))
        Reject("DEFAULT_UNSUPPORTED", "new父对象必须是未覆盖的标准ability默认。", origin);
    if (record.fields.contains("_code") && (!String(record.fields, "_code") || !Detail::Rawcode(*String(record.fields, "_code"))))
        Reject("DEFAULT_UNSUPPORTED", "存在的_code必须为四字节。", origin);
    (void)Level(Get(record.fields, "_max_level"), origin);
    budget.Text(id, origin);
    SlkSessionObject object{id, {}};
    for (const auto& [name, value] : record.fields) {
        budget.FieldKey(name, origin);
        if (const auto* scalar = std::get_if<Scalar>(&value.value)) budget.ScalarValue(*scalar, origin);
        else {
            const auto& array = std::get<Array>(value.value);
            if (array.size() > kLevelLimit) Reject("LIMIT", "默认数组超过256项。", origin);
            budget.Container(origin);
            for (const auto& item : array) budget.ScalarValue(item, origin);
        }
        object.fields.emplace(name, value.value);
    }
    return object;
}
void Put(SlkSessionObject& object, const std::string& name, Scalar value, Detail::Budget& budget, const OriginSpan& origin) {
    budget.FieldKey(name, origin); budget.ScalarValue(value, origin); object.fields.insert_or_assign(name, std::move(value));
}
void CheckInput(const SlkOrderedCreationInput& input, Detail::Budget& budget, const OriginSpan& origin) {
    if (!input.dataset || !input.oldFrontend) Reject("SOURCES_UNPROVEN", "需要canonical数据集及对应old物化结果。", origin);
    const auto& dataset = *input.dataset; const auto& old = *input.oldFrontend;
    if (old.coverage != SlkNativeFrontendCoverage::ExistingNativeFrontendOnly || !Detail::Hash(dataset.manifestSha256) ||
        !Detail::Hash(dataset.w3iSha256) || !Detail::Hash(old.session.bindings.candidateSha256) ||
        !Detail::Hash(old.session.bindings.mapInfo.sha256) || !Detail::Hash(old.session.bindings.slkLibrarySha256) ||
        !Detail::Hash(old.session.bindings.metadataSha256) ||
        old.canonicalDatasetManifest != dataset.manifestSha256 || old.session.bindings.dataSetIdentity != dataset.manifestSha256 ||
        old.session.bindings.documentRevision != dataset.documentRevision || old.session.bindings.mapInfo.presence != SlkSessionSourcePresence::Present ||
        Text::LowerAscii(old.session.bindings.mapInfo.sha256) != Text::LowerAscii(dataset.w3iSha256) ||
        Text::LowerAscii(old.session.bindings.slkLibrarySha256) != Text::LowerAscii(kLibrary) || !Detail::Hash(old.session.bindings.orderedTemplatesSha256))
        Reject("IDENTITY_MISMATCH", "canonical old、数据集、模板或固定slk_lib身份不一致。", origin);
    if (dataset.sources.size() != 11 || dataset.metadata.byCode.size() > 10000 || input.orderedPlans.size() > 4096)
        Reject("LIMIT", "来源/metadata scope/模板块数量超过子集界限。", origin);
    const auto sourceIdentity = [](const SlkSessionSourceIdentity& source) {
        return (source.presence == SlkSessionSourcePresence::Present && Detail::Hash(source.sha256)) ||
            (source.presence == SlkSessionSourcePresence::Missing && source.sha256.empty());
    };
    if (!sourceIdentity(old.session.bindings.strings) ||
        (!old.session.bindings.materializerSha256.empty() && !Detail::Hash(old.session.bindings.materializerSha256)) ||
        (!old.session.bindings.frontendModelSha256.empty() && !Detail::Hash(old.session.bindings.frontendModelSha256)))
        Reject("IDENTITY_MISMATCH", "old附加来源身份格式无效。", origin);
    for (const auto& source : old.session.bindings.nativeMembers)
        if (!sourceIdentity(source)) Reject("IDENTITY_MISMATCH", "old native身份格式无效。", origin);
    std::set<std::string> roles;
    for (const auto& hash : old.session.bindings.defaultsSha256)
        if (!Detail::Hash(hash)) Reject("IDENTITY_MISMATCH", "old默认来源摘要缺失。", origin);
    for (const auto& source : dataset.sources) {
        budget.FieldKey(source.role, origin);
        if (!Detail::Hash(source.sha256) || !roles.insert(source.role).second) Reject("IDENTITY_MISMATCH", "数据集来源身份缺失或重复。", origin);
        if (source.role == "canonical-metadata" && Text::LowerAscii(source.sha256) != Text::LowerAscii(old.session.bindings.metadataSha256))
            Reject("IDENTITY_MISMATCH", "metadata与old来源不匹配。", origin);
        for (std::size_t i = 0; i < Detail::kTypes.size(); ++i)
            if (source.role == "prebuilt-" + std::string(Detail::kNames[i]) && Text::LowerAscii(source.sha256) != Text::LowerAscii(old.session.bindings.defaultsSha256[i]))
                Reject("IDENTITY_MISMATCH", "默认数据与old来源不匹配。", origin);
    }
    for (const auto role : {"core-config", "canonical-metadata", "prebuilt-txt", "prebuilt-misc"})
        if (!roles.contains(role)) Reject("IDENTITY_MISMATCH", "缺少必要来源角色。", origin);
    for (const auto name : Detail::kNames)
        if (!roles.contains("prebuilt-" + std::string(name))) Reject("IDENTITY_MISMATCH", "缺少七类默认来源角色。", origin);
    for (std::size_t i = 0; i < Detail::kTypes.size(); ++i) {
        if (old.session.classes[i].type != Detail::kTypes[i]) Reject("IDENTITY_MISMATCH", "old七类顺序错误。", origin);
        budget.Spend(budget.objects, old.session.classes[i].existingObjects.size(), 20000, origin);
    }
}
} // namespace

Result<SlkOrderedCreation> MaterializeSlkOrderedAbilityCreation(const SlkOrderedCreationInput& input) {
    try {
        const auto* old = input.oldFrontend;
        const OriginSpan origin{"slk-ordered-creation", 0, 0,
            old && Detail::Hash(old->session.bindings.orderedTemplatesSha256) ? old->session.bindings.orderedTemplatesSha256 : std::string{}};
        const SlkOrderedCreationLimits hard;
        if (input.limits.abilities > hard.abilities || input.limits.writes > hard.writes || input.limits.values > hard.values || input.limits.stringBytes > hard.stringBytes)
            Reject("LIMIT", "调用方只能收紧ordered creation硬预算。", origin);
        SlkNativeFrontendLimits storageLimits;
        storageLimits.values = input.limits.values; storageLimits.stringBytes = input.limits.stringBytes;
        Detail::Budget budget{storageLimits};
        CheckInput(input, budget, origin);
        const auto& dataset = *input.dataset;
        const auto common = RulesFor(dataset.metadata.common[kAbility], budget, origin);
        std::set<std::string> blocked, created;
        // static new只需同类表查重；额外拒绝大小写碰撞，避免目标全局lowercase索引别名。
        for (const auto& type : input.oldFrontend->session.classes) for (const auto& object : type.existingObjects) {
            if (!Detail::Rawcode(object.id)) Reject("OLD_UNSUPPORTED", "old对象ID无效。", origin);
            const auto marker = object.fields.find("w2lobject");
            bool generated = false;
            if (marker != object.fields.end()) {
                const auto* scalar = std::get_if<Scalar>(&marker->second);
                if (!scalar) Reject("OLD_UNSUPPORTED", "old W2lo必须是字符串或false。", origin);
                generated = std::holds_alternative<std::string>(*scalar);
                if (!generated && Truthy(*scalar)) Reject("OLD_UNSUPPORTED", "truthy old W2lo不能执行目标mark_obj。", origin);
            }
            if (!generated) { budget.FieldKey(object.id, origin); blocked.insert(Text::LowerAscii(object.id)); }
        }
        SlkOrderedCreation result; result.bindings = input.oldFrontend->session.bindings;
        std::size_t count = 0, writes = 0;
        for (const auto& plan : input.orderedPlans) {
            budget.Spend(count, plan.abilities.size(), input.limits.abilities, origin);
            for (const auto& ability : plan.abilities) budget.Spend(writes, ability.writes.size(), input.limits.writes, origin);
        }
        for (std::size_t block = 0; block < input.orderedPlans.size(); ++block) for (const auto& ability : input.orderedPlans[block].abilities) {
            OriginSpan where = origin; where.source = "slk-ordered-creation/block-" + std::to_string(block + 1);
            where.offset = ability.source.offset; where.length = ability.source.length;
            if (!StaticId(ability.id) || !Detail::Rawcode(ability.parentId)) Reject("ID_UNSUPPORTED", "只支持静态四字符新ID与四字节标准父。", where);
            const auto lower = Text::LowerAscii(ability.id);
            budget.FieldKey(ability.id, where);
            if (dataset.defaults[kAbility].contains(ability.id) || blocked.contains(lower) || !created.insert(lower).second)
                Reject("ID_CONFLICT", "新ID与标准/未移走old/本次创建对象冲突。", where);
            const auto parent = dataset.defaults[kAbility].find(ability.parentId);
            if (parent == dataset.defaults[kAbility].end()) Reject("PARENT_UNSUPPORTED", "new父对象不在原始标准default；不支持custom父链。", where);
            Rules specific;
            if (const auto* code = String(parent->second.fields, "_code")) {
                const auto scope = dataset.metadata.byCode.find(*code);
                if (scope != dataset.metadata.byCode.end()) specific = RulesFor(scope->second, budget, where);
            }
            auto object = CopyDefault(parent->first, parent->second, budget, where);
            Fill(object, parent->second, common, budget, result, where);
            Fill(object, parent->second, specific, budget, result, where);
            object.id = ability.id;
            Put(object, "_id", ability.id, budget, where); Put(object, "_parent", ability.parentId, budget, where);
            Put(object, "_type", std::string("ability"), budget, where); Put(object, "_obj", true, budget, where);
            Put(object, "_create", true, budget, where); Put(object, "w2lobject", std::string("static"), budget, where);
            for (const auto& write : ability.writes) {
                where.offset = write.source.offset; where.length = write.source.length;
                const auto address = Resolve(write.field, common, specific, budget, where);
                if (const auto* text = std::get_if<std::string>(&write.value); text && text->size() > 1023)
                    Reject("STRING_UNSUPPORTED", "目标写入会截断并报告字符串过长；本单元拒绝有损结果。", where);
                budget.ScalarValue(write.value, where);
                if (address.level) {
                    auto found = object.fields.find(address.rule->name);
                    auto* array = found == object.fields.end() ? nullptr : std::get_if<Array>(&found->second);
                    if (!array || *address.level > array->size() + 1)
                        Reject("SPARSE_UNSUPPORTED", "逐级写入会产生稀疏表或改变字段形状。", where);
                    if (*address.level > array->size()) array->push_back(write.value);
                    else (*array)[*address.level - 1] = write.value;
                } else {
                    budget.FieldKey(address.rule->name, where);
                    object.fields.insert_or_assign(address.rule->name, Scalar{write.value});
                    if (address.rule->name == "levels") {
                        (void)Level(&write.value, where);
                        Put(object, "_max_level", write.value, budget, where);
                        Fill(object, parent->second, common, budget, result, where);
                        Fill(object, parent->second, specific, budget, result, where);
                    }
                }
                ++result.appliedWrites;
            }
            result.createdAbilities.push_back(std::move(object));
        }
        result.unresolvedSemantics = {"OrderedAbilityCreationOnly：仅物化静态ability new及标量逐次写入。",
            "公开计划和typed图与其摘要之间仍需可信来源绑定；完整会话副作用/其他来源/目标frontend oracle未闭合。",
            "未设置任何Session Complete，未执行七类old/new/change比较或生产保存。"};
        auto output = Result<SlkOrderedCreation>::Success(std::move(result));
        output.diagnostics.push_back({DiagnosticSeverity::Warning, "Script.SlkCreation", "SLK_CREATION_SESSION_UNVERIFIED",
            "静态ability创建已物化，完整会话仍未验证；不能据此跳过模板或保存。", origin});
        return output;
    } catch (const Detail::Rejection& error) {
        std::string code = error.code;
        if (code.starts_with("SLK_MATERIALIZER_")) code = "SLK_CREATION_" + code.substr(std::string_view("SLK_MATERIALIZER_").size());
        auto output = Result<SlkOrderedCreation>::Failure("Script.SlkCreation", std::move(code), error.message);
        output.diagnostics.front().origin = error.origin;
        return output;
    } catch (const std::exception&) {
        return Result<SlkOrderedCreation>::Failure("Script.SlkCreation", "SLK_CREATION_RESOURCE_FAILURE", "创建物化未完成，没有返回部分对象。");
    }
}
} // namespace War3Map::Script
