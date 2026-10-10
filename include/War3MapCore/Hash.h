#pragma once

#include "War3MapCore/Types.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace War3Map {
class ProjectSaveControl;

// 只接收原字节；调用方显式指定总量上限。64 KiB 尾块合批短字段，
// 不拥有完整输入。对象独占 provider/hash，禁止并发使用。
// 预算/更新失败会封闭流；Finish 成功或失败后均不能再次更新或终结。
class Sha256Stream final {
public:
    static constexpr std::size_t kBufferBytes = 64 * 1024;
    explicit Sha256Stream(std::size_t maximumBytes);
    ~Sha256Stream();
    Sha256Stream(const Sha256Stream&) = delete;
    Sha256Stream& operator=(const Sha256Stream&) = delete;
    Sha256Stream(Sha256Stream&&) = delete;
    Sha256Stream& operator=(Sha256Stream&&) = delete;

    [[nodiscard]] Status Update(std::span<const std::byte> bytes);
    [[nodiscard]] Result<std::string> Finish();
    [[nodiscard]] std::size_t BytesAccepted() const noexcept;
    // C++ 状态与 CNG 对象缓冲的实际拥有账；不包含 provider 的系统内部内存，不是 RSS。
    [[nodiscard]] std::size_t ScratchBytes() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};

[[nodiscard]] Result<std::string> Sha256(
    std::span<const std::byte> bytes, const ProjectSaveControl* control = nullptr);
[[nodiscard]] Result<std::string> Sha256Text(
    std::string_view text);
[[nodiscard]] Result<std::string> Sha256File(
    const std::filesystem::path& path, const ProjectSaveControl* control = nullptr);
// Windows 文件句柄必须具有 GENERIC_READ；函数从文件起点读取并恢复原指针位置。
[[nodiscard]] Result<std::string> Sha256Handle(void* fileHandle, const ProjectSaveControl* control = nullptr);

} // namespace War3Map
