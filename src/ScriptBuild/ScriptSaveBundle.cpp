#include "War3MapCore/Script/ScriptSaveBundle.h"
#include "War3MapCore/ObjectData/ObjectImport.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Hash.h"

#include <map>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace War3Map::Script {
namespace {
constexpr std::size_t kByteLimit = 64 * 1024 * 1024;
constexpr std::array<std::string_view, 7> kNative{
    "WAR3MAP.W3U", "WAR3MAP.W3A", "WAR3MAP.W3T", "WAR3MAP.W3H",
    "WAR3MAP.W3Q", "WAR3MAP.W3D", "WAR3MAP.W3B"};
struct Rejection { std::vector<Diagnostic> diagnostics; };
[[noreturn]] void Reject(std::string code, std::string message) {
    throw Rejection{{{DiagnosticSeverity::Error, "Script.SaveBundle", std::move(code), std::move(message), {}}}};
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded()) throw Rejection{std::move(result.diagnostics)};
    return std::move(*result.value);
}
std::string Key(std::string_view name) {
    if (name.empty() || name.size() > 1024) Reject("SCRIPT_BUNDLE_PATH", "保存成员路径为空或超过预算。");
    return Take(Resources::CanonicalImportPath(name));
}
bool ScriptName(std::string_view name) { return name == "WAR3MAP.J" || name == "SCRIPTS\\WAR3MAP.J"; }
} // namespace

Result<ScriptSaveBundle> PrepareScriptSaveBundle(const PipelineResult& pipeline, std::string_view scriptMember) {
    try {
        if (!pipeline.Succeeded() || pipeline.script.empty()) Reject("SCRIPT_BUNDLE_UNCHECKED", "完整脚本流水线未成功，不能准备保存结果。");
        const auto scriptKey = Key(scriptMember);
        if (!ScriptName(scriptKey)) Reject("SCRIPT_BUNDLE_SCRIPT_PATH", "脚本输出不是受支持的 JASS 成员。");
        if (pipeline.script.size() > kByteLimit || pipeline.objectMembers.size() > kNative.size())
            Reject("SCRIPT_BUNDLE_LIMIT", "模板结果超过脚本、成员数量或字节预算。");
        ScriptSaveBundle output;
        std::set<std::string> names;
        std::size_t bytes = 0;
        for (const auto& member : pipeline.objectMembers) {
            const auto key = Key(member.name);
            const auto found = std::find(kNative.begin(), kNative.end(), key);
            if (found == kNative.end() || !names.insert(key).second)
                Reject("SCRIPT_BUNDLE_MEMBER", "模板输出未知或重复成员：" + member.name);
            if (member.data.size() > kByteLimit - bytes) Reject("SCRIPT_BUNDLE_LIMIT", "生成物编超过总字节预算。");
            bytes += member.data.size();
            const auto index = static_cast<std::size_t>(found - kNative.begin());
            const auto type = static_cast<ObjectData::ObjectType>(index);
            const ObjectData::NativeObjectCodec codec(type);
            auto parsed = codec.Parse(member.data);
            if (!parsed.Succeeded()) throw Rejection{std::move(parsed.diagnostics)};
            auto* native = dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get());
            if (!native || native->Version() != 2 || !native->IsStructurallySafe() || !native->IsEditable())
                Reject("SCRIPT_BUNDLE_NATIVE", "生成物编不能作为经典可编辑基线。");
            if (Take(codec.Serialize(*native)) != member.data) Reject("SCRIPT_BUNDLE_ROUNDTRIP", "生成物编往返字节不一致。");
            auto lniBytes = Take(ObjectData::ConvertNativeObjectDataToLni(type, member.data));
            auto lni = ObjectData::LniObjectCodec(type).Parse(lniBytes);
            if (!lni.Succeeded()) throw Rejection{std::move(lni.diagnostics)};
            auto* editable = dynamic_cast<ObjectData::LniObjectDocument*>(lni.document.get());
            if (!editable || !editable->IsStructurallySafe()) Reject("SCRIPT_BUNDLE_EDITABLE", "不能建立生成物编的可编辑文档。");
            output.nativeBaselines[index].reset(static_cast<ObjectData::NativeObjectDocument*>(parsed.document.release()));
            output.editableBaselines[index].reset(static_cast<ObjectData::LniObjectDocument*>(lni.document.release()));
            output.members.push_back({std::string(ObjectData::NativeFileName(type)), member.data});
        }
        const auto* first = reinterpret_cast<const std::byte*>(pipeline.script.data());
        output.members.push_back({std::string(scriptMember), {first, first + pipeline.script.size()}});
        return Result<ScriptSaveBundle>::Success(std::move(output));
    } catch (const Rejection& failure) {
        Result<ScriptSaveBundle> result; result.diagnostics = failure.diagnostics; return result;
    } catch (const std::exception&) {
        return Result<ScriptSaveBundle>::Failure("Script.SaveBundle", "SCRIPT_BUNDLE_RESOURCE", "无法准备完整保存结果，未返回部分结果。");
    }
}

Result<ScriptSaveBundle> PrepareScriptSaveBundle(const LuaMapPipelineResult& pipeline, std::string_view scriptMember) {
    try {
        if (!pipeline.Succeeded() || pipeline.script.empty()) Reject("SCRIPT_BUNDLE_UNCHECKED", "完整Lua检查未成功，不能准备保存结果。");
        if (Key(scriptMember) != "WAR3MAP.LUA") Reject("SCRIPT_BUNDLE_SCRIPT_PATH", "Lua输出只支持根成员war3map.lua。");
        if (pipeline.script.size() > kByteLimit) Reject("SCRIPT_BUNDLE_LIMIT", "Lua脚本超过保存预算。");
        const auto hash = Take(Sha256Text(pipeline.script));
        if (hash != pipeline.scriptSha256) Reject("SCRIPT_BUNDLE_STALE", "Lua检查结果与待保存字节不一致。");
        ScriptSaveBundle output;
        const auto* first = reinterpret_cast<const std::byte*>(pipeline.script.data());
        output.members.push_back({std::string(scriptMember), {first, first + pipeline.script.size()}});
        return Result<ScriptSaveBundle>::Success(std::move(output));
    } catch (const Rejection& failure) {
        Result<ScriptSaveBundle> result; result.diagnostics = failure.diagnostics; return result;
    } catch (const std::exception&) {
        return Result<ScriptSaveBundle>::Failure("Script.SaveBundle", "SCRIPT_BUNDLE_RESOURCE", "无法准备完整Lua保存结果，未返回部分结果。");
    }
}

Status PreflightScriptSaveBaselineReplacement(const ScriptSaveBundle& bundle,
    const std::array<const ObjectData::LniObjectDocument*, 7>& currentEditable) {
    try {
        std::size_t totalBytes = 0;
        for (std::size_t index = 0; index < kNative.size(); ++index) {
            const auto& generatedNative = bundle.nativeBaselines[index];
            const auto& generatedEditable = bundle.editableBaselines[index];
            if (!generatedNative && !generatedEditable) continue;
            const auto type = static_cast<ObjectData::ObjectType>(index);
            if (!generatedNative || !generatedEditable || generatedNative->Type() != type ||
                generatedEditable->Type() != type || !generatedNative->IsEditable() ||
                !generatedEditable->IsStructurallySafe()) {
                Reject("SCRIPT_BUNDLE_BASELINE_INVALID", "生成类别缺少匹配的 native/LNI 基线。");
            }
            const auto* current = currentEditable[index];
            if (!current) continue;
            const auto category = std::string(ObjectData::LniFileName(type));
            if (current->Type() != type || !current->IsStructurallySafe()) {
                Reject("SCRIPT_BUNDLE_BASELINE_INVALID", "当前编辑快照的类别或结构不可确认：" + category);
            }

            std::map<std::string_view, const ObjectData::ObjectRecord*> objects;
            for (const auto& object : current->Objects()) {
                if (object.key.type != type || !objects.emplace(object.key.rawcode, &object).second) {
                    Reject("SCRIPT_BUNDLE_BASELINE_INVALID", "当前编辑快照含类型不符或重复对象：" + category);
                }
            }
            for (const auto& object : current->Objects()) {
                if (object.parentRawcode) {
                    const auto parent = objects.find(*object.parentRawcode);
                    if (*object.parentRawcode == object.key.rawcode ||
                        (parent != objects.end() && parent->second->parentRawcode)) {
                        Reject("SCRIPT_BUNDLE_PARENT_CHAIN", "生成结果不能整类替换自定义继承链：" +
                            category + "/" + object.key.rawcode + "；需要保留编辑父链的三方合并。");
                    }
                }
                for (const auto& modification : object.modifications) {
                    if (modification.valueWasOpaque ||
                        (!std::holds_alternative<std::int32_t>(modification.value) &&
                         !std::holds_alternative<float>(modification.value) &&
                         !std::holds_alternative<std::string>(modification.value))) {
                        Reject("SCRIPT_BUNDLE_OPAQUE_BASELINE", "生成结果不能丢弃无法证明 native 可表示的编辑值：" +
                            category + "/" + object.key.rawcode + "/" + modification.key.fieldId);
                    }
                }
            }

            // 不另写 LNI lexer 猜测注释/表/私有元信息；共享 codec 的无损文本必须恰好
            // 等于语义对象的规范渲染，才证明不存在整类重建会擦除的额外文本。
            const auto original = Take(ObjectData::LniObjectCodec(type).Serialize(*current));
            if (original.size() > kByteLimit - totalBytes) {
                Reject("SCRIPT_BUNDLE_BASELINE_LIMIT", "待替换编辑快照超过 64 MiB 文本总预算。");
            }
            totalBytes += original.size();
            auto canonical = ObjectData::RenderObjectRecordsToLni(type, current->Objects());
            if (!canonical.Succeeded() || !canonical.diagnostics.empty()) {
                Reject("SCRIPT_BUNDLE_TEXT_BASELINE", "当前编辑文档不能完整投影为规范 LNI：" + category);
            }
            if (original != *canonical.value) {
                Reject("SCRIPT_BUNDLE_TEXT_BASELINE", "无法证明整类重建不会改变已有注释、排版或额外文本：" +
                    category + "；当前仅支持与规范渲染逐字节一致的 LNI。");
            }

            // 复用原有字段/索引验证，不猜 _name 等文本字段是否可以成为四字节 native 字段。
            // 该独立投影只证明可表示性，不进入 bundle，也不推断原生 Real/Unreal 存储类型。
            auto projection = Take(ObjectData::NativeObjectDocument::CreateEmpty(type));
            const auto represented = ObjectData::ApplyLniChangesToNative(projection, *current);
            if (!represented.Succeeded()) {
                auto diagnostics = Status::Failure("Script.SaveBundle", "SCRIPT_BUNDLE_BASELINE_UNREPRESENTABLE",
                    "当前编辑文档含经典 native 无法完整表示的字段或索引：" + category).diagnostics;
                diagnostics.insert(diagnostics.end(), represented.diagnostics.begin(), represented.diagnostics.end());
                throw Rejection{std::move(diagnostics)};
            }
        }
        return {};
    } catch (const Rejection& failure) { Status result; result.diagnostics = failure.diagnostics; return result; }
    catch (const std::exception&) { return Status::Failure("Script.SaveBundle", "SCRIPT_BUNDLE_RESOURCE",
        "无法完成编辑基线替换预检，生成结果和当前文档均未修改。"); }
}

Status MergeScriptSaveMembers(std::vector<MemberReplacement>& pending,
    const std::vector<std::string>& removals, std::vector<MemberReplacement>& generated) {
    try {
        if (&pending == &generated || pending.size() > 65536 || removals.size() > 65536 || generated.size() > 8)
            Reject("SCRIPT_BUNDLE_LIMIT", "成员合并输入无效或超过数量预算。");
        std::map<std::string, std::size_t> existing;
        std::set<std::string> deleted, additions;
        for (std::size_t i = 0; i < pending.size(); ++i) {
            if (!existing.emplace(Key(pending[i].name), i).second) Reject("SCRIPT_BUNDLE_CONFLICT", "原待保存列表有重复成员。");
        }
        for (const auto& name : removals) {
            const auto key = Key(name);
            if (!deleted.insert(key).second || existing.contains(key)) Reject("SCRIPT_BUNDLE_CONFLICT", "待保存成员被重复删除或同时替换和删除。");
        }
        std::vector<std::size_t> slots;
        for (const auto& member : generated) {
            const auto key = Key(member.name);
            if ((!ScriptName(key) && key != "WAR3MAP.LUA" && std::find(kNative.begin(), kNative.end(), key) == kNative.end()) ||
                !additions.insert(key).second || deleted.contains(key)) Reject("SCRIPT_BUNDLE_CONFLICT", "生成成员无效、重复或同时被删除。");
            const auto found = existing.find(key);
            slots.push_back(found == existing.end() ? pending.size() : found->second);
        }
        const auto oldSize = pending.size();
        pending.reserve(oldSize + generated.size());
        static_assert(std::is_nothrow_move_constructible_v<MemberReplacement> && std::is_nothrow_move_assignable_v<MemberReplacement>);
        for (std::size_t i = 0; i < generated.size(); ++i) {
            if (slots[i] == oldSize) pending.push_back(std::move(generated[i]));
            else pending[slots[i]] = std::move(generated[i]);
        }
        generated.clear();
        return {};
    } catch (const Rejection& failure) { Status result; result.diagnostics = failure.diagnostics; return result; }
    catch (const std::exception&) { Status result; result.diagnostics.push_back({DiagnosticSeverity::Error,
        "Script.SaveBundle", "SCRIPT_BUNDLE_RESOURCE", "无法准备成员合并，原待保存列表保持不变。", {}}); return result; }
}
} // namespace War3Map::Script
