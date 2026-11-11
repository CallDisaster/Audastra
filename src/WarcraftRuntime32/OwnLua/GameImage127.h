#pragma once

#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace Audastra::Runtime32::OwnLua {

// 映像身份与文件租约共用一处；安装观察器不要求脚本 VM 已经创建。
class GameImage127 final {
public:
    struct OpenResult { std::shared_ptr<GameImage127> image; std::string error; };
    [[nodiscard]] static OpenResult Open(HMODULE module);
    ~GameImage127();
    [[nodiscard]] std::uintptr_t Base() const noexcept;
    [[nodiscard]] const std::filesystem::path& Path() const noexcept;
    [[nodiscard]] bool Executable(std::uintptr_t address, std::size_t bytes = 1) const noexcept;
    [[nodiscard]] bool Matches(std::uintptr_t rva, std::span<const unsigned char> bytes) const noexcept;
    [[nodiscard]] bool OriginalCode(std::uint32_t rva, std::span<unsigned char> bytes) const noexcept;
    // 只在已知原版归档 ABI 下返回活动地图的独立文件句柄；失败不回退到启动路径。
    [[nodiscard]] HANDLE DuplicateActiveMapFile() const;
private:
    struct Impl;
    explicit GameImage127(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace Audastra::Runtime32::OwnLua
