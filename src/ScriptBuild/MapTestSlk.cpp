#include "War3MapCore/Script/MapTestToolchain.h"

#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/SlkOptimizer.h"
#include "War3MapCore/VerifiedFile.h"

#include <algorithm>
#include <map>

namespace War3Map::Script {
namespace {

Status Failure(std::string code, std::string message) { return Status::Failure("Map.Test.Slk", std::move(code), std::move(message)); }
template<class T> Status Diagnostics(const Result<T>& value) { Status result; result.diagnostics = value.diagnostics; return result; }

std::string Key(std::string name) {
    for (auto& c : name) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == '/') c = '\\';
    }
    return name;
}

// 按名精确捕获优化器可能读取的全部成员；地图同名成员覆盖游戏数据，与游戏加载顺序一致。
Result<std::map<std::string, std::vector<std::byte>>> CaptureMembers(const MapArchiveSession& archive) {
    const auto names = ObjectData::SlkOptimizerMemberNames();
    auto captured = archive.CaptureExactMembers(names);
    if (!captured.Succeeded()) {
        Result<std::map<std::string, std::vector<std::byte>>> failed; failed.diagnostics = captured.diagnostics; return failed;
    }
    std::map<std::string, std::vector<std::byte>> members;
    for (auto& member : *captured.value)
        if (member.present) members.emplace(Key(member.name), std::move(member.bytes));
    return Result<std::map<std::string, std::vector<std::byte>>>::Success(std::move(members));
}

Status Info(Status status, std::string code, std::string message) {
    status.diagnostics.push_back({DiagnosticSeverity::Info, "Map.Test.Slk", std::move(code), std::move(message)});
    return status;
}

} // namespace

// 原生实现：不调用外部 Lua/w3x2lni。自定义物体写入 SLK 表行与 profile 节，
// 无法表示的修改留在精简物编文件；优化器内部回读逐字段证明有效值不变，
// 写出后再从候选归档重新读取每个替换成员核对字节。
Status OptimizeMapTestSlk(const Services::MapTestSlkRequest& request) {
    std::error_code ec;
    if (!request.inputMap.is_absolute() || !request.outputMap.is_absolute() || !request.workspace.is_absolute()
        || request.inputMap.parent_path() != request.workspace || request.outputMap.parent_path() != request.workspace
        || std::filesystem::exists(request.outputMap, ec) || ec)
        return Failure("MAP_TEST_SLK_PATH", "SLK 优化只允许向本次独占目录的新地图路径写出。");
    if (request.gameRoot.empty() || !request.gameRoot.is_absolute())
        return Failure("MAP_TEST_SLK_GAME", "SLK 优化需要游戏目录读取原版物编表。");
    // 严格挂载：高层归档损坏时整体失败，不悄悄退回低层数据生成错误的表。
    auto game = Game::GameDataSource::MountStrict(request.gameRoot);
    if (!game.Succeeded()) return Diagnostics(game);
    auto schema = ObjectData::ObjectFieldSchema::Load(*game.value);
    if (!schema.Succeeded()) return Diagnostics(schema);

    auto source = MapArchiveSession::Open(request.inputMap);
    if (!source.Succeeded()) return Diagnostics(source);
    auto verified = ReadVerifiedFile(request.inputMap, request.expectedInputSha256, 1024ull * 1024 * 1024);
    if (!verified.Succeeded()) return Diagnostics(verified);
    verified.value.reset();
    auto members = CaptureMembers(*source.value);
    if (!members.Succeeded()) return Diagnostics(members);

    ObjectData::SlkOptimizerInput input;
    input.mapMember = [&members](const std::string& name) -> std::optional<std::vector<std::byte>> {
        const auto found = members.value->find(Key(name));
        if (found == members.value->end()) return std::nullopt;
        return found->second;
    };
    input.gameFile = [&game](const std::string& name) { return game.value->ReadFile(name); };
    input.schema = &*schema.value;
    auto optimized = ObjectData::OptimizeObjectDataToSlk(input);
    if (!optimized.Succeeded()) return Diagnostics(optimized);

    std::vector<MemberReplacement> replacements;
    replacements.reserve(optimized.value->replacements.size());
    for (const auto& replacement : optimized.value->replacements) replacements.push_back({replacement.name, replacement.bytes});
    // 没有可转换物体时也写出候选，测试流程始终从本次独占目录的产物启动。
    auto saved = source.value->SaveCandidate(request.outputMap, replacements);
    if (!saved.Succeeded()) return Diagnostics(saved);

    auto output = MapArchiveSession::Open(request.outputMap);
    if (!output.Succeeded()) return Diagnostics(output);
    for (const auto& replacement : replacements) {
        auto written = output.value->ReadMember(replacement.name);
        if (!written.Succeeded()) return Diagnostics(written);
        if (*written.value != replacement.data)
            return Failure("MAP_TEST_SLK_READBACK", "SLK 测试副本回读与写入内容不一致：" + replacement.name);
    }
    auto unchanged = source.value->VerifySourceUnchanged();
    if (!unchanged.Succeeded()) return unchanged;

    const auto& summary = *optimized.value;
    Status status = Info(Status::Ok(), "MAP_TEST_SLK_SUMMARY",
        "SLK 优化：" + std::to_string(summary.convertedObjects) + " 个自定义物体写入表格，"
        + std::to_string(summary.keptCustomObjects) + " 个保留为物编自定义物体，"
        + std::to_string(summary.residualFields) + " 项修改留在物编文件；回读核对 "
        + std::to_string(summary.verifiedValues) + " 个值。");
    for (const auto& note : summary.notes) status = Info(std::move(status), "MAP_TEST_SLK_NOTE", note);
    return status;
}

} // namespace War3Map::Script
