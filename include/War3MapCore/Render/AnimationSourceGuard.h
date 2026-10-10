#pragma once
#include "War3MapCore/Types.h"
#include <filesystem>
#include <memory>

namespace War3Map::Render {
// Windows 只读来源租约。读取前登记，读取后及复用前 Verify；不允许把重命名后的旧句柄
// 当成当前路径。缺失的高优先级游戏包也登记，后来新增补丁包会使来源失效。
// 不遍历游戏资源，不持有 QWidget；同一读取器/worker 串行拥有。
class AnimationSourceGuard {
public:
    AnimationSourceGuard();
    ~AnimationSourceGuard();
    AnimationSourceGuard(const AnimationSourceGuard&) = delete;
    AnimationSourceGuard& operator=(const AnimationSourceGuard&) = delete;
    [[nodiscard]] Status Watch(const std::filesystem::path& path, bool allowMissing = false);
    [[nodiscard]] Status WatchGameRoot(const std::filesystem::path& root);
    [[nodiscard]] Status Verify() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace War3Map::Render
