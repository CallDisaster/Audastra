#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace Audastra::Runtime32::OwnLua {

class NativeTypeHierarchy;
struct NativeTypesResult {
    std::shared_ptr<const NativeTypeHierarchy> types;
    std::string sourceArchive;
    std::string error;
};
// 复用编辑器 JASS parser；不手写或猜测游戏 handle 继承关系。
class NativeTypeHierarchy final {
public:
    [[nodiscard]] static NativeTypesResult ReadGame(const std::filesystem::path& gameRoot);
    // 仅解析显式来源；生产调用者应使用上面的游戏 MPQ 读取入口。
    [[nodiscard]] static NativeTypesResult Parse(std::string_view commonSource);
    [[nodiscard]] bool IsA(std::string_view actual, std::string_view expected) const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept { return parents_.size(); }
private:
    std::map<std::string, std::string, std::less<>> parents_;
};

} // namespace Audastra::Runtime32::OwnLua
