#pragma once

#include "War3MapCore/Types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <functional>
#include <span>

namespace War3Map::Script::detail {

// 让一个已核验身份的 1.27a 游戏进程用它自己的截图功能存一张画面：
// 置位截图键事件写的那个全局标志，游戏画完下一帧后同步写出 TGA 并清掉标志。
// 画面来自游戏自己的后台缓冲，屏幕关闭、锁屏、不可见桌面都能截。
// process 需要 PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION。
// 只认 gameRoot 下完整SHA锁定的Game.dll，并核验映射头与可写映像数据页；限时内没有新帧时撤回标志并失败。
// 返回游戏在 gameRoot\Screenshots 下新写的 TGA，调用方读完后删除。
[[nodiscard]] Result<std::filesystem::path> RequestGame127Screenshot(void* process,
    const std::filesystem::path& gameRoot, std::uint32_t timeoutMs);
// 原生截图键通过调用方的原进程/窗口输入租约发送，不需要重新取得进程内存写权限。
[[nodiscard]] Result<std::filesystem::path> RequestGame127KeyScreenshot(const std::filesystem::path& gameRoot,
    std::uint32_t timeoutMs, const std::function<Result<std::uint32_t>()>& request);
// 内部文件收集器：只接受请求后新建且已结束写入的普通TGA，旧截图不能顶替本次画面。
[[nodiscard]] Result<std::filesystem::path> CollectNativeGameScreenshot(const std::filesystem::path& directory,
    std::uint32_t timeoutMs, const std::function<Status()>& request);
struct ScreenshotWriter { std::uint32_t processId = 0; bool querySucceeded = false, sameImage = false; };
[[nodiscard]] Status VerifyNativeScreenshotWriters(std::uint32_t originalProcessId, bool enumerationComplete,
    std::span<const ScreenshotWriter> writers);

struct VerifiedScreenshot127 {
    std::uintptr_t flag = 0;
    bool eventChanged = false;
    std::shared_ptr<void> fileLease;
    bool dosStubChanged = false;
};
// 内部纯核验步骤，不写进程、不调用游戏；base只由上面的模块归属检查或原创映像夹具提供。
[[nodiscard]] Result<VerifiedScreenshot127> QualifyGame127Screenshot(void* process,
    std::uintptr_t base, const std::filesystem::path& modulePath);

} // namespace War3Map::Script::detail
