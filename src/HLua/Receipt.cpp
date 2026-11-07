#include "War3MapCore/HLua/Build.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace War3Map::HLua {
namespace {
using Json = nlohmann::json;
std::string Display(std::string_view text, std::size_t maximum) {
    if (!Audastra::Runtime32::LuaMailboxUtf8(text)) return "[非UTF-8文本，保留在原生产物中]";
    return std::string(text.substr(0,Audastra::Runtime32::LuaMailboxUtf8Prefix(text,maximum)));
}
Json InspectJson(const InspectReceipt& receipt,std::size_t offset=0,std::size_t limit=128) {
    Json modules=Json::array();
    Json japi=Json::array();
    const auto start=std::min(offset,receipt.japiNames.size()),count=std::min({std::size_t{128},limit,receipt.japiNames.size()-start});
    for(std::size_t i=start;i<start+count;++i) japi.push_back(Display(receipt.japiNames[i],240));
    Json missing=Json::array();
    for(std::size_t i=0;i<std::min<std::size_t>(128,receipt.missingCommonNames.size());++i) missing.push_back(Display(receipt.missingCommonNames[i],240));
    for (std::size_t i=0;i<std::min<std::size_t>(32,receipt.jassModules.size());++i) modules.push_back(Display(receipt.jassModules[i],240));
    return {{"framework_sha256", receipt.frameworkSha256}, {"project_sha256", receipt.projectSha256},
        {"runtime_files", receipt.runtimeFiles}, {"hslk_files", receipt.hslkFiles}, {"jass_modules", std::move(modules)},
        {"jass_module_count",receipt.jassModules.size()},{"japi_literal_names",std::move(japi)},
        {"japi_literal_name_count",receipt.japiNames.size()},{"japi_offset",start},
        {"japi_next_offset",start+count<receipt.japiNames.size() ? Json(start+count) : Json(nullptr)},
        {"japi_names_complete",false},{"common_sha256",receipt.commonSha256},
        {"common_names_checked",receipt.commonNamesChecked},{"common_missing_names",std::move(missing)},
        {"common_missing_count",receipt.missingCommonNames.size()},
        {"common_missing_policy","未知名称不会伪造函数；启动查询或调用时明确诊断。"}};
}
}
std::string InspectReceiptJson(const InspectReceipt& receipt,std::size_t offset,std::size_t limit) {
    auto result = InspectJson(receipt,offset,limit); result["ok"] = true;
    result["framework_license"] = "用户提供的 h-lua 为 AGPL-3.0；编辑器不分发该框架。";
    return result.dump(2);
}
std::string BuildReceiptJson(const BuildReceipt& receipt, std::size_t offset, std::size_t limit) {
    Json objects = Json::array();
    Json diagnostics = Json::array();
    Json audits = Json::array();
    Json unwritten = Json::array();
    const auto unwrittenBegin = std::min(offset, receipt.unwrittenFields.size());
    const auto unwrittenCount = std::min({limit, std::size_t{1024}, receipt.unwrittenFields.size() - unwrittenBegin});
    for (std::size_t i = unwrittenBegin; i < unwrittenBegin + unwrittenCount; ++i) {
        const auto& field = receipt.unwrittenFields[i];
        unwritten.push_back({{"class", field.objectClass}, {"id", field.id}, {"source", Display(field.source, 1024)},
            {"source_truncated", field.source.size() > 1024}, {"field", Display(field.field, 240)},
            {"field_truncated", field.field.size() > 240}, {"reason", "该类型没有这个字段"},
            {"other_object_classes", field.otherObjectClasses}});
    }
    const auto auditBegin = std::min(offset, receipt.redundantIndexedFields.size());
    const auto auditCount = std::min({limit, std::size_t{1024}, receipt.redundantIndexedFields.size() - auditBegin});
    for (std::size_t i = auditBegin; i < auditBegin + auditCount; ++i) {
        const auto& audit = receipt.redundantIndexedFields[i];
        Json indices = Json::array();
        for (const auto& index : audit.indices) indices.push_back({{"field", Display(index.field, 240)},
            {"native_id", index.nativeId}, {"effective_value", Display(index.effectiveValue, 4096)},
            {"effective_value_truncated", index.effectiveValue.size() > 4096}, {"value_source", Display(index.valueSource, 1024)},
            {"defaulted_text_empty", index.defaultedTextEmpty}, {"default_evidence", index.defaultEvidence}});
        audits.push_back({{"class", audit.objectClass}, {"id", audit.id}, {"source", Display(audit.source, 1024)},
            {"field", Display(audit.field, 240)}, {"declared_value", Display(audit.declaredValue, 4096)},
            {"declared_value_truncated", audit.declaredValue.size() > 4096}, {"indices", std::move(indices)}});
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(64, receipt.diagnostics.size()); ++i) {
        const auto& value = receipt.diagnostics[i];
        diagnostics.push_back({{"code",Display(value.code,128)},{"message",Display(value.message,512)}});
    }
    const auto begin = std::min(offset, receipt.objects.size());
    const auto count = std::min({limit, std::size_t{1024}, receipt.objects.size() - begin});
    for (std::size_t i = begin; i < begin + count; ++i) {
        const auto& object = receipt.objects[i];
        objects.push_back({{"class",object.objectClass},{"id",object.id},{"parent",object.parent},{"name",Display(object.name,128)},
            {"name_truncated",object.name.size()>128},{"name_utf8",Audastra::Runtime32::LuaMailboxUtf8(object.name)}});
    }
    Json result{{"ok",true},{"inputs",InspectJson(receipt.inputs)},{"base_sha256",receipt.baseSha256},
        {"output",receipt.output.empty() ? Json(nullptr) : Json(IO::GenericPathUtf8(receipt.output))},
        {"output_sha256",receipt.outputSha256},{"objects",std::move(objects)},{"object_count",receipt.objects.size()},
        {"offset",begin},{"next_offset",begin + count < receipt.objects.size() ? Json(begin + count) : Json(nullptr)},
        {"id_replay_passed",receipt.idReplayPassed},{"archive_readback_passed",receipt.archiveReadbackPassed},
        {"peak_lua_bytes",receipt.peakLuaBytes},{"lua_instructions",receipt.luaInstructions},
        {"sdk_differences",receipt.differences},{"console",Display(receipt.console,4096)},
        {"diagnostics",std::move(diagnostics)},{"diagnostic_count",receipt.diagnostics.size()},
        {"field_mapping_checked", receipt.fieldMappingChecked}, {"redundant_indexed_fields", std::move(audits)},
        {"unwritten_fields_title", "未写入的字段"}, {"unwritten_fields", std::move(unwritten)},
        {"unwritten_field_count", receipt.unwrittenFields.size()}, {"unwritten_offset", unwrittenBegin},
        {"unwritten_next_offset", unwrittenBegin + unwrittenCount < receipt.unwrittenFields.size() ? Json(unwrittenBegin + unwrittenCount) : Json(nullptr)},
        {"redundant_indexed_field_count", receipt.redundantIndexedFields.size()}, {"audit_offset", auditBegin},
        {"audit_next_offset", auditBegin + auditCount < receipt.redundantIndexedFields.size() ? Json(auditBegin + auditCount) : Json(nullptr)},
        {"original_editor_verified",false},{"game_verified",false}};
    return result.dump(2);
}
} // namespace War3Map::HLua
