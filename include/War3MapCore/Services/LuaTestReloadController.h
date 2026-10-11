#pragma once

#include "War3MapCore/Services/LuaTestMailboxSession.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Script/LuaTestReload.h"

#include <chrono>

namespace War3Map::Services {

enum class LuaReloadOutcome { Ready, NoChanges, Pending, Succeeded, Failed, RestartRequired, Stopped };
struct LuaReloadControllerSnapshot {
    LuaReloadOutcome outcome = LuaReloadOutcome::Ready;
    std::uint32_t sequence = 0;
    std::string message, baselineScriptSha256;
    double seconds = 0;
    std::optional<Script::LuaTestReloadLocation> location;
};

// 一局测试拥有一份基线。UI/MCP共用控制器；只接受生成单元，不接受外部任意Lua文本。
// 提交后不前推，只有对应成功ACK前推；部分执行失败必须重新测试。
class LuaTestReloadController final {
public:
    LuaTestReloadController(std::shared_ptr<LuaTestMailboxSession> mailbox,
        Script::LuaTriggerUnit baseline, std::string mapBinding);
    ~LuaTestReloadController();
    LuaTestReloadController(const LuaTestReloadController&) = delete;
    LuaTestReloadController& operator=(const LuaTestReloadController&) = delete;
    [[nodiscard]] Result<LuaReloadControllerSnapshot> Submit(Script::LuaTriggerUnit current,
        std::string_view mapBinding, const Lua::SyntaxOptions& syntax = {});
    [[nodiscard]] Result<LuaReloadControllerSnapshot> Poll();
private:
    struct State;
    std::unique_ptr<State> state_;
};

// 最终非脚本成员的内容身份。未知匿名成员绑定源SHA，来源变化时要求重新测试。
// 在后台调用；不解压、不写地图。排除的脚本名只能由可信保存链提供。
[[nodiscard]] Result<std::string> CaptureLuaTestMapBinding(const MapArchiveSession& archive,
    std::span<const MemberReplacement> replacements, std::span<const std::string> removals,
    std::span<const std::string> generatedScriptMembers);

} // namespace War3Map::Services
