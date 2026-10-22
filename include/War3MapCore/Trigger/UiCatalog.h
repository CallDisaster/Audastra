#pragma once

#include "War3MapCore/Trigger/UiCallbackActions.h"
#include "War3MapCore/Types.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace War3Map::Game {
class WorldEditStrings;
}

namespace War3Map::Trigger {

enum class TextEncoding : std::uint8_t {
    Utf8,
    Utf8Bom,
    Gb18030,
    Latin1,
};

enum class UiTextLineKind : std::uint8_t {
    Empty,
    Comment,
    Section,
    ArgumentSection,
    Assignment,
    Other,
};

struct UiTextLine {
    UiTextLineKind kind = UiTextLineKind::Other;
    std::size_t lineNumber = 0;
    std::string text;
    std::string lineEnding;
    std::string section;
    std::string key;
    std::string value;
};

// 文档刻意保留完整输入字节流；解析结果只是字节索引，未修改时必须逐字节序列化一致。
class UiTextDocument {
public:
    [[nodiscard]] static Result<UiTextDocument> Parse(
        std::filesystem::path source,
        std::span<const std::byte> bytes);

    [[nodiscard]] const std::filesystem::path& Source() const noexcept;
    [[nodiscard]] TextEncoding Encoding() const noexcept;
    [[nodiscard]] const std::vector<UiTextLine>& Lines() const noexcept;
    [[nodiscard]] std::span<const std::byte> OriginalBytes() const noexcept;
    [[nodiscard]] std::vector<std::byte> SerializeLossless() const;

private:
    std::filesystem::path source_;
    TextEncoding encoding_ = TextEncoding::Utf8;
    std::vector<std::byte> originalBytes_;
    std::vector<UiTextLine> lines_;
};

enum class UiDefinitionKind : std::uint8_t {
    Define,
    Event,
    Condition,
    Action,
    Call,
    Unknown,
};

struct UiArgumentDefinition {
    std::string type;
    std::string defaultValue;
    std::vector<std::pair<std::string, std::string>> fields;
};

struct UiDefinitionRevision {
    UiDefinitionKind kind = UiDefinitionKind::Unknown;
    std::string group;
    std::string name;
    std::string layer;
    std::filesystem::path source;
    std::string sourceSha256;
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<UiArgumentDefinition> arguments;
};

struct UiDefinition {
    UiDefinitionKind kind = UiDefinitionKind::Unknown;
    std::string group;
    std::string name;
    std::vector<UiDefinitionRevision> revisions;

    [[nodiscard]] const UiDefinitionRevision& Active() const;
    [[nodiscard]] std::size_t ArgumentCount() const;
    [[nodiscard]] std::string Signature() const;
};

struct UiSourceManifestEntry {
    std::string layer;
    UiDefinitionKind kind = UiDefinitionKind::Unknown;
    std::filesystem::path relativePath;
    bool expected = false;
    bool present = false;
    bool parsed = false;
    std::uintmax_t size = 0;
    std::string sha256;
    std::size_t definitionCount = 0;
};

class UiCatalog {
public:
    // UI 层来自用户数据根；缺失的可选层只记录诊断，不会在 Core 中硬编码或伪造函数定义。
    [[nodiscard]] static Result<UiCatalog> Load(
        const std::filesystem::path& toolRoot);
    // 按给定层序载入（两层启用的有效集合）；不读取也不修改数据根中的 config。
    [[nodiscard]] static Result<UiCatalog> LoadLayers(
        const std::filesystem::path& toolRoot,
        std::span<const std::string> layers);
    // 读取数据根 config 中按顺序启用的层（与 Load 使用同一解析规则）。
    [[nodiscard]] static Result<std::vector<std::string>> ReadConfigLayers(
        const std::filesystem::path& toolRoot);
    // toolRoot 可为含 ui/config 或旧式 share/mpq/config 的安装根，也可直接为含 config 的数据根。
    [[nodiscard]] static std::filesystem::path ResolveUiRoot(
        const std::filesystem::path& toolRoot);

    [[nodiscard]] const UiDefinition* Find(
        std::int32_t ecaType,
        std::string_view name) const;
    [[nodiscard]] const UiDefinition* Find(
        UiDefinitionKind kind,
        std::string_view name,
        std::string_view group = {}) const;
    // 返回按名称稳定排序的当前生效定义，供 GUI/CLI/MCP 构建动态函数选择器。
    [[nodiscard]] std::vector<const UiDefinition*> Definitions(
        UiDefinitionKind kind) const;

    [[nodiscard]] const UiTextDocument& ConfigDocument() const noexcept;
    [[nodiscard]] const std::vector<std::string>& Layers() const noexcept;
    [[nodiscard]] const std::vector<UiSourceManifestEntry>& Manifest() const noexcept;
    [[nodiscard]] const std::vector<UiTextDocument>& Documents() const noexcept;
    [[nodiscard]] const std::string& Sha256() const noexcept;
    [[nodiscard]] std::size_t PresentSourceCount() const noexcept;
    [[nodiscard]] std::size_t ParsedSourceCount() const noexcept;

    // 公开行的 sizeof(UiDefinition) 已由消费者计费；补计深拷贝的私有根 native 字节、
    // 实际索引 key 字符串及 value_type 的剩余 inline 字节（含可能的 pair padding）。
    // 不复制、不拼接路径/键，不含容器 allocator/node/capacity，亦不等于 RSS。
    [[nodiscard]] std::optional<std::size_t> AdditionalOwnedBytes(std::size_t byteLimit) const noexcept {
        using DefinitionPair = decltype(definitions_)::value_type;
        static_assert(sizeof(DefinitionPair) >= sizeof(UiDefinition) + sizeof(std::string));
        constexpr std::size_t pairInlineBytes = sizeof(DefinitionPair) - sizeof(UiDefinition);
        const auto& native = root_.native();
        if (native.size() > byteLimit / sizeof(std::filesystem::path::value_type)) return std::nullopt;
        std::size_t bytes = native.size() * sizeof(std::filesystem::path::value_type);
        if (definitions_.size() > (byteLimit - bytes) / pairInlineBytes) return std::nullopt;
        bytes += definitions_.size() * pairInlineBytes;
        for (const auto& entry : definitions_) {
            if (entry.first.size() > byteLimit - bytes) return std::nullopt;
            bytes += entry.first.size();
        }
        return bytes;
    }

    // 回调块动作（UI 定义少末尾 code 参数）的识别结果：首次查询时按已启用层的函数库 J 声明计算一次，
    // 同一次载入的各份拷贝共用结果；只做身份比较的载入不付扫描代价。扫描诊断在结果的 diagnostics 中。
    // 树、蓝图、脚本生成与 Agent 工具共用同一份结果。
    [[nodiscard]] const UiCallbackActionIndex& CallbackActions() const;
    // 供测试或宿主用另一组声明重新计算（与载入时使用同一规则）。
    void SetCallbackDeclarations(std::span<const UiJassDeclaration> declarations, bool complete = true);

    // 显示文本用的字符串表（WESTRING_* 键）。只影响显示，不改变定义、保存或脚本生成。
    void SetStrings(std::shared_ptr<const Game::WorldEditStrings> strings) noexcept;
    [[nodiscard]] const Game::WorldEditStrings* Strings() const noexcept { return strings_.get(); }
    // text 是 WESTRING_* 键时按字符串表解析；没有字符串表或找不到时原样返回。
    [[nodiscard]] std::string Localize(std::string_view text) const;

private:
    [[nodiscard]] static Result<UiCatalog> LoadFromConfig(
        const std::filesystem::path& uiRoot,
        const std::vector<std::byte>& configBytes);
    static std::string DefinitionKey(
        UiDefinitionKind kind,
        std::string_view group,
        std::string_view name);

    std::filesystem::path root_;
    UiTextDocument configDocument_;
    std::vector<std::string> layers_;
    std::vector<UiSourceManifestEntry> manifest_;
    std::vector<UiTextDocument> documents_;
    std::unordered_map<std::string, UiDefinition> definitions_;
    std::string sha256_;
    std::shared_ptr<const Game::WorldEditStrings> strings_;
    struct CallbackState;
    std::shared_ptr<CallbackState> callbackActions_;
};

[[nodiscard]] UiDefinitionKind UiDefinitionKindFromEcaType(std::int32_t type) noexcept;
[[nodiscard]] std::optional<std::int32_t> EcaTypeFromUiDefinitionKind(UiDefinitionKind kind) noexcept;
[[nodiscard]] std::string_view ToString(UiDefinitionKind kind) noexcept;

} // namespace War3Map::Trigger
