#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace War3Map::Services {

struct CommandAvailability {
    bool available = true;
    std::string reason;
};

enum class CommandExecutionStatus { Executed, Unknown, Disabled, Failed, InvalidRequest };

// Executed 表示命令处理器已执行；保存或运行的业务验收由处理器自身报告。
struct CommandExecution {
    CommandExecutionStatus status = CommandExecutionStatus::Executed;
    std::string commandId;
    std::string message;
    [[nodiscard]] bool Succeeded() const noexcept { return status == CommandExecutionStatus::Executed; }
};

struct CommandDefinition {
    std::string id;
    std::string title;
    std::string group;
    std::string shortcut;
    std::string keywords;
    std::function<CommandAvailability()> availability;
    std::function<CommandExecution()> execute;
};

struct CommandMatch {
    CommandDefinition definition;
    CommandAvailability availability;
    std::int64_t score = 0;
};

struct CommandQueryResult {
    bool valid = true;
    std::string error;
    std::size_t totalMatches = 0;
    bool hasMore = false;
    std::vector<CommandMatch> items;
};

// 原型 commandRank.ts 的多词 AND、UTF16 长度/位置和 Unicode 字符子序列。
// 小写使用 Windows invariant NLS（无 Qt），不是完整 ECMAScript Unicode 版本保证：
// 例如 U+0130 展开、希腊 Final_Sigma 以及新增 Unicode 字符可能与浏览器不同。
// 输入须为合法 UTF8；query <=1024 字节，text <=16384 字节，超限/未命中返回 nullopt。
[[nodiscard]] std::optional<std::int64_t> FuzzyCommandScore(std::string_view query, std::string_view text);

// 宿主须串行调用；允许处理器中增删命令。ID 稳定、重复注册拒绝，不覆盖已有行为。
class CommandRegistry {
public:
    static constexpr std::size_t MaxCommands = 4096;
    static constexpr std::size_t MaxPageSize = 256;
    static constexpr std::size_t MaxQueryBytes = 1024;
    static constexpr std::size_t RecentLimit = 12;

    bool Register(CommandDefinition definition, std::string* error = nullptr);
    bool Remove(std::string_view id);
    void Clear();
    [[nodiscard]] std::size_t Size() const noexcept { return commands_.size(); }
    [[nodiscard]] CommandQueryResult List(std::size_t offset = 0, std::size_t limit = 40,
        bool includeUnavailable = true) const;
    [[nodiscard]] CommandQueryResult Search(std::string_view query, std::size_t offset = 0,
        std::size_t limit = 40, bool includeUnavailable = false) const;
    [[nodiscard]] CommandExecution Execute(std::string_view id);
    bool SetRecent(const std::vector<std::string>& ids, std::string* error = nullptr);
    [[nodiscard]] const std::vector<std::string>& Recent() const noexcept { return recent_; }

private:
    [[nodiscard]] CommandQueryResult Query(std::string_view query, std::size_t offset,
        std::size_t limit, bool includeUnavailable, bool ranked) const;
    std::vector<std::shared_ptr<const CommandDefinition>> commands_;
    std::vector<std::string> recent_;
};

} // namespace War3Map::Services
