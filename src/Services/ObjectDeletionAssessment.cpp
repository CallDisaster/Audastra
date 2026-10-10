#include "War3MapCore/Services/ObjectDeletionAssessment.h"

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Trigger/TriggerDocument.h"

#include <algorithm>
#include <array>
#include <set>
#include <string_view>
#include <utility>

namespace War3Map::Services {
namespace {

constexpr std::array<ObjectData::ObjectType, 7> kObjectTypes{
    ObjectData::ObjectType::Unit,
    ObjectData::ObjectType::Ability,
    ObjectData::ObjectType::Item,
    ObjectData::ObjectType::Buff,
    ObjectData::ObjectType::Upgrade,
    ObjectData::ObjectType::Doodad,
    ObjectData::ObjectType::Destructable,
};
constexpr std::size_t kMaxDisplayedEvidence = 32;

void AddReason(ObjectDeletionAssessment& result, std::string code,
    std::string source, std::string message) {
    result.reasons.push_back({std::move(code), std::move(source), std::move(message)});
}

void ScanScriptText(ObjectDeletionAssessment& result,
    std::string_view text, const std::string& source) {
    const std::string& rawcode = result.subject.rawcode;
    if (rawcode.size() != 4) return;
    std::size_t line = 1;
    for (std::size_t offset = 0; offset < text.size(); ++offset) {
        if (offset + rawcode.size() <= text.size()
            && text.compare(offset, rawcode.size(), rawcode) == 0) {
            ++result.scriptHintCount;
            if (result.scriptHints.size() < kMaxDisplayedEvidence) {
                result.scriptHints.push_back({source, offset, line});
            }
        }
        if (text[offset] == '\n') ++line;
    }
}

} // namespace

ObjectDeletionAssessment AssessObjectDeletion(
    const ProjectReferenceInputs& inputs,
    const ObjectData::ObjectKey& subject,
    std::span<const ScriptSourceInput> suppliedScripts) {
    ObjectDeletionAssessment result;
    result.subject = subject;

    if (subject.rawcode.size() != 4
        || std::find(kObjectTypes.begin(), kObjectTypes.end(), subject.type) == kObjectTypes.end()) {
        AddReason(result, "invalid-subject", "object", "对象类型或四字节 ID 无效，无法进行删除审计。");
    }

    std::size_t targetCount = 0;
    bool customTarget = false;
    for (const auto& source : inputs.objects) {
        if (source.type != subject.type) continue;
        for (const auto& record : source.records) {
            if (record.key != subject) continue;
            ++targetCount;
            customTarget = record.parentRawcode.has_value();
        }
    }
    if (targetCount != 1) {
        AddReason(result, "target-not-unique", "object",
            targetCount == 0 ? "当前物编输入没有目标对象。" : "目标对象在物编输入中重复，无法确定删除范围。");
    } else if (!customTarget) {
        AddReason(result, "original-object", "object",
            "原版对象不能删除；只能清除地图中的字段覆盖。");
    }

    if (inputs.map == nullptr) {
        AddReason(result, "map-unavailable", "map", "地图数据未载入，无法检查放置实例。");
    }
    const MapDocument emptyMap = MapDocument::FromMembers({});
    const auto staticIndex = ObjectData::ObjectReferenceIndex::Build(
        inputs.objects, inputs.map != nullptr ? *inputs.map : emptyMap, inputs.schema);
    result.objectReferences = staticIndex.Find(subject);
    result.coverage.assign(staticIndex.Coverage().begin(), staticIndex.Coverage().end());
    for (const auto& coverage : result.coverage) {
        if (coverage.state == ObjectData::ReferenceInputState::Ready) continue;
        AddReason(result, "source-incomplete", coverage.memberName,
            coverage.detail.empty() ? "来源缺失或未完整解析，不能证明没有引用。" : coverage.detail);
    }

    if (!result.objectReferences.possible.empty()) {
        const auto& first = result.objectReferences.possible.front();
        AddReason(result, "possible-object-reference", first.memberName,
            "对象 ID 出现在目标类型不确定的字段或放置实例中：" + first.location);
    }

    if (inputs.triggers == nullptr) {
        AddReason(result, "triggers-unavailable", "war3map.wtg/wct",
            "触发器文档未载入，无法检查 GUI 参数和自定义文本。");
    } else {
        const auto& triggers = *inputs.triggers;
        if (!triggers.HasTrustedSourceSeal()) {
            AddReason(result, "trigger-source-unverified", "war3map.wtg/wct",
                "触发器输入没有可信来源记录，不能认定已完整解析。");
        }
        if (!triggers.wct.customCode.text.empty()) {
            AddReason(result, "wct-custom-code", "war3map.wct",
                "地图包含 WCT 自定义代码，当前索引不解析其中的对象引用。");
            ScanScriptText(result, triggers.wct.customCode.text, "war3map.wct:header");
        }
        for (const auto& trigger : triggers.triggers) {
            if (trigger.customTextFlag == 0 && trigger.customText.text.empty()) continue;
            AddReason(result, "trigger-custom-text", "war3map.wct",
                "触发器包含自定义文本，当前索引不解析其中的对象引用。");
            ScanScriptText(result, trigger.customText.text,
                "war3map.wct:trigger:" + std::to_string(trigger.id));
        }
        for (std::size_t index = 0; index < triggers.argumentNodes.size(); ++index) {
            const auto& argument = triggers.argumentNodes[index];
            const bool variable = argument.type == 1;
            const bool functionCall = argument.callEca.has_value();
            const bool arrayIndex = argument.indexArgument.has_value();
            if (!variable && !functionCall && !arrayIndex) continue;
            ++result.dynamicValueCount;
            if (result.dynamicValues.size() < kMaxDisplayedEvidence) {
                result.dynamicValues.push_back({index, variable, functionCall, arrayIndex,
                    argument.value.text});
            }
        }
        if (result.dynamicValueCount != 0) {
            AddReason(result, "dynamic-gui-value", "war3map.wtg",
                "GUI 含变量、函数或数组取值；尚未按参数类型证明其运行时对象 ID。");
        }
        const auto tokens = std::array<std::string, 1>{subject.rawcode};
        const auto projectIndex = ProjectReferenceIndex::Build(inputs);
        for (const Usage* usage : projectIndex.UsagesOf(tokens)) {
            if (usage->module == UsageModule::Triggers || usage->module == UsageModule::Variables) {
                result.untypedGuiReferences.push_back(*usage);
            }
        }
        if (!result.untypedGuiReferences.empty()) {
            AddReason(result, "untyped-gui-reference", "war3map.wtg",
                "GUI 参数或变量初值包含此 ID，但当前索引不能证明目标对象类型。");
        }
    }

    if (inputs.catalog == nullptr) {
        AddReason(result, "ui-catalog-unavailable", "ui",
            "函数目录未载入，不能确定 GUI 参数和返回值类型。");
    }
    std::set<std::string> scriptNames;
    for (const auto& script : suppliedScripts) {
        if (script.name.empty()) {
            AddReason(result, "script-source-unnamed", "script",
                "提供的脚本缺少来源名称，无法定位候选引用。");
        } else if (!scriptNames.insert(script.name).second) {
            AddReason(result, "script-source-duplicate", script.name,
                "同名脚本来源重复，无法认定来源清单唯一。");
        }
        if (script.bytes.empty()) {
            AddReason(result, "script-source-empty", script.name,
                "脚本来源没有可检查的字节。");
            continue;
        }
        ScanScriptText(result,
            {reinterpret_cast<const char*>(script.bytes.data()), script.bytes.size()},
            script.name.empty() ? "<unnamed-script>" : script.name);
    }
    if (result.scriptHintCount != 0) {
        AddReason(result, "possible-script-reference", "script",
            "脚本文本出现目标 ID；可能位于代码、字符串或注释，需语义检查。");
    }
    if (result.scriptHintCount > result.scriptHints.size()) {
        AddReason(result, "script-hints-truncated", "script",
            "可疑脚本位置过多，结果只展开前 32 处。");
    }
    if (result.dynamicValueCount > result.dynamicValues.size()) {
        AddReason(result, "dynamic-values-truncated", "war3map.wtg",
            "动态 GUI 参数过多，结果只展开前 32 处。");
    }
    // ProjectReferenceInputs 没有 war3map.j、导入脚本及脚本构建来源的完整性证明。
    // 即使所有可见静态来源为 Ready 且零命中，也不能由此推导出可删除。
    AddReason(result, "script-source-unverified", "war3map.j",
        suppliedScripts.empty()
            ? "未提供完整且可信的 JASS/扩展脚本来源及动态 ID 分析。"
            : "已定位所提供脚本中的候选文本，但尚未证明脚本清单完整或动态 ID 的语义。");

    if (!result.objectReferences.definite.empty()) {
        const auto& first = result.objectReferences.definite.front();
        AddReason(result, "definite-object-reference", first.memberName,
            "已确认目标对象被引用：" + first.location);
        result.disposition = ObjectDeletionDisposition::BlockedReferences;
    } else {
        result.disposition = ObjectDeletionDisposition::BlockedUnknown;
    }
    return result;
}

} // namespace War3Map::Services
