#pragma once

#include "War3MapCore/Types.h"

#include <cstdint>
#include <filesystem>

namespace War3Map::Script::detail {

// 让一个已核验身份的 1.27a 游戏进程用它自己的截图功能存一张画面：
// 置位截图键事件写的那个全局标志，游戏画完下一帧后同步写出 TGA 并清掉标志。
// 画面来自游戏自己的后台缓冲，屏幕关闭、锁屏、不可见桌面都能截。
// process 需要 PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION。
// 只认 gameRoot 下的 Game.dll 且入口字节与 1.27a 一致；限时内没有新帧时撤回标志并失败。
// 返回游戏在 gameRoot\Screenshots 下新写的 TGA，调用方读完后删除。
[[nodiscard]] Result<std::filesystem::path> RequestGame127Screenshot(void* process,
    const std::filesystem::path& gameRoot, std::uint32_t timeoutMs);

} // namespace War3Map::Script::detail
