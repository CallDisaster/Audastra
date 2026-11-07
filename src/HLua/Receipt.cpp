#include "War3MapCore/HLua/Build.h"
#include "War3MapCore/IO/FileBytes.h"
#include <nlohmann/json.hpp>
#include <algorithm>

namespace War3Map::HLua {
namespace {
using Json = nlohmann::json;
Json InspectJson(const InspectReceipt& receipt) {
    return {{"framework_sha256", receipt.frameworkSha256}, {"project_sha256", receipt.projectSha256},
        {"runtime_files", receipt.runtimeFiles}, {"hslk_files", receipt.hslkFiles}, {"jass_modules", receipt.jassModules}};
}
}
std::string InspectReceiptJson(const InspectReceipt& receipt) {
    auto result = InspectJson(receipt); result["ok"] = true;
    result["framework_license"] = "用户提供的 h-lua 为 AGPL-3.0；编辑器不分发该框架。";
    return result.dump(2);
}
std::string BuildReceiptJson(const BuildReceipt& receipt, std::size_t offset, std::size_t limit) {
    Json objects = Json::array();
    const auto begin = std::min(offset, receipt.objects.size());
    const auto count = std::min({limit, std::size_t{1024}, receipt.objects.size() - begin});
    for (std::size_t i = begin; i < begin + count; ++i) {
        const auto& object = receipt.objects[i];
        objects.push_back({{"class",object.objectClass},{"id",object.id},{"parent",object.parent},{"name",object.name}});
    }
    Json result{{"ok",true},{"inputs",InspectJson(receipt.inputs)},{"base_sha256",receipt.baseSha256},
        {"output",receipt.output.empty() ? Json(nullptr) : Json(IO::GenericPathUtf8(receipt.output))},
        {"output_sha256",receipt.outputSha256},{"objects",std::move(objects)},{"object_count",receipt.objects.size()},
        {"offset",begin},{"next_offset",begin + count < receipt.objects.size() ? Json(begin + count) : Json(nullptr)},
        {"id_replay_passed",receipt.idReplayPassed},{"archive_readback_passed",receipt.archiveReadbackPassed},
        {"peak_lua_bytes",receipt.peakLuaBytes},{"lua_instructions",receipt.luaInstructions},
        {"sdk_differences",receipt.differences},{"console",receipt.console},
        {"original_editor_verified",false},{"game_verified",false}};
    return result.dump(2);
}
} // namespace War3Map::HLua
