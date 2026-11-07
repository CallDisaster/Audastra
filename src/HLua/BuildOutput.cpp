#include "War3MapCore/HLua/Build.h"
#include <set>
#include <algorithm>

namespace War3Map::HLua {
namespace {
std::string Key(std::string name) {
    for (auto& c:name) {if(c=='/')c='\\';else if(c>='A' && c<='Z')c+='a'-'A';}
    return name;
}
}
Status PublishBuildOutput(const std::filesystem::path& candidate, const std::filesystem::path& target, std::string_view expected) {
    const auto original=MapArchiveSession::Open(target);
    if(!original.Succeeded())return {original.diagnostics};
    if(expected.empty() || original.value->SourceSha256()!=expected)
        return Status::Failure("HLua","HLUA_TARGET_CHANGED","输出地图与明确批准的身份不符，未覆盖。");
    const auto built=MapArchiveSession::Open(candidate);
    if(!built.Succeeded())return {built.diagnostics};
    if(original.value->HasOpaqueMembers() || built.value->HasOpaqueMembers())
        return Status::Failure("HLua","HLUA_TARGET_OPAQUE","替换目标含匿名成员，无法确认完整替换；请另存到新路径。");
    // 此入口需要重建命名成员集合。引导成员只能走归档原字节透传，不能在这里展开正文。
    for(const auto& member:built.value->Members()) if(Key(member.name).ends_with(".ai"))
        return Status::Failure("HLua","HLUA_TARGET_OPAQUE","候选含引导成员；请另存到新路径以保持原字节透传。");
    std::vector<MemberReplacement> replacements;
    std::vector<std::string> removals;
    std::set<std::string> names;
    std::size_t total=0;
    for(const auto& member:built.value->Members()) {
        auto bytes=built.value->ReadMember(member.name);
        if(!bytes.Succeeded())return {bytes.diagnostics};
        if(bytes.value->size()>256u*1024u*1024u-total)
            return Status::Failure("HLua","HLUA_TARGET_BUDGET","替换候选超过256 MiB成员预算；未覆盖原图。");
        total+=bytes.value->size();
        names.insert(Key(member.name));
        replacements.push_back({member.name,std::move(*bytes.value)});
    }
    for(const auto& member:original.value->Members()) if(!names.contains(Key(member.name)))removals.push_back(member.name);
    auto snapshot=original.value->WithLeadingHeader(built.value->Prefix());
    if(!snapshot.Succeeded())return {snapshot.diagnostics};
    const auto saved=snapshot.value->SaveInPlace(replacements,removals);
    if(!saved.Succeeded() && saved.diagnostics.empty())return Status::Failure("HLua","HLUA_TARGET_PUBLISH","输出地图未能替换。");
    return {saved.diagnostics};
}
} // namespace War3Map::HLua
