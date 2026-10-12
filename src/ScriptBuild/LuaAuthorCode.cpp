#include "War3MapCore/Script/LuaAuthorCode.h"
#include "LuaDeclarationScan.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

namespace War3Map::Script {
struct PreparedLuaAuthorCode::State {
    LuaAuthorCodeIdentity identity;
    std::string header;
    std::vector<LuaAuthorTriggerCode> triggers;
};
namespace {
constexpr std::size_t kDocumentBytes = 64u * 1024u * 1024u;
constexpr std::size_t kAuthorBytes = 8u * 1024u * 1024u;
constexpr std::size_t kTokens = 1024u * 1024u;
constexpr std::size_t kNameBytes = 4096;

[[noreturn]] void Reject(std::string code, std::string message, OriginSpan origin = {}) {
    throw Diagnostic{DiagnosticSeverity::Error, "Script.LuaAuthor", std::move(code), std::move(message), std::move(origin)};
}
std::string Hash(std::span<const std::byte> bytes) {
    auto result = Sha256(bytes);
    if (!result.Succeeded() || !result.value) Reject("LUA_AUTHOR_HASH_FAILED", "Lua 作者输入身份生成失败。");
    return std::move(*result.value);
}
std::string Hash(std::string_view text) {
    return Hash({reinterpret_cast<const std::byte*>(text.data()), text.size()});
}
void NativeSource(std::span<const std::byte> bytes) {
    if (bytes.size() > 4u * 1024u * 1024u) Reject("LUA_AUTHOR_SETTINGS_BUDGET", "源地图设置超过 4 MiB 预算。");
    const auto parsed = MapData::MapSettings::Parse(bytes);
    if (!parsed.Succeeded() || !parsed.value || parsed.value->SourceFormatVersion() != 31
        || parsed.value->formatVersion != 31 || !parsed.value->FullyParsed() || parsed.value->scriptLanguage != 1)
        Reject("LUA_AUTHOR_SOURCE_LANGUAGE", "作者 Lua 编辑需要实际解析的原生 Lua v31 源地图；选择目标语言不能提供此资格。");
}
void TrustedDocument(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog) {
    if (!document.HasTrustedSourceSeal() || document.UiCatalogSha256() != catalog.Sha256())
        Reject("LUA_AUTHOR_SOURCE_IDENTITY", "Lua 作者文档缺少可信 WTG/WCT 与函数目录身份。");
    if (document.categories.size() > 8192 || document.variables.size() > 8192 || document.triggers.size() > 8192
        || document.ecaNodes.size() > 200000 || document.argumentNodes.size() > 200000)
        Reject("LUA_AUTHOR_DOCUMENT_BUDGET", "Lua 作者文档超过图规模预算。");
    // 序列化之前先核所有可分配字符串，避免先复制超预算 IR 再拒绝。
    std::size_t bytes = 0;
    const auto add = [&](std::size_t count) {
        if (count > kDocumentBytes - bytes) Reject("LUA_AUTHOR_DOCUMENT_BUDGET", "Lua 作者文档超过 64 MiB 字节预算。");
        bytes += count;
    };
    const auto raw = [&](const Trigger::RawText& text) { add(text.text.size()); add(text.originalBytes.size()); };
    const auto path = [&](const std::filesystem::path& value) {
        if (value.native().size() > kDocumentBytes / sizeof(std::filesystem::path::value_type))
            Reject("LUA_AUTHOR_DOCUMENT_BUDGET", "Lua 作者来源路径超过文档字节预算。");
        add(value.native().size() * sizeof(std::filesystem::path::value_type));
    };
    const auto origin = [&](const OriginSpan& value) { path(value.source); add(value.fingerprint.size()); };
    const auto edges = [&](std::size_t count) {
        if (count > 200000) Reject("LUA_AUTHOR_DOCUMENT_BUDGET", "Lua 作者图边超过 200000 条预算。");
        add(count * sizeof(std::uint32_t));
    };
    raw(document.wct.customComment); raw(document.wct.customCode);
    for (const auto& row : document.categories) { raw(row.name); origin(row.origin); }
    for (const auto& row : document.variables) { raw(row.name); raw(row.typeName); raw(row.value); origin(row.origin); }
    for (const auto& row : document.triggers) { raw(row.name); raw(row.description); raw(row.customText); origin(row.origin); edges(row.ecas.size()); }
    for (const auto& row : document.ecaNodes) {
        raw(row.name); add(row.uiLayer.size()); add(row.uiSignature.size()); path(row.uiSource); origin(row.origin);
        edges(row.arguments.size()); edges(row.children.size());
    }
    for (const auto& row : document.argumentNodes) { raw(row.value); origin(row.origin); }
    const auto valid = Trigger::WtgWctCodec{}.Validate(document, catalog);
    if (!valid.Succeeded()) Reject("LUA_AUTHOR_DOCUMENT_INVALID", "Lua 作者文档未通过共用 WTG/WCT 门禁。");
}

class Binding {
public:
    void Number(std::uint64_t value) {
        Check(8);
        for (unsigned i = 0; i < 8; ++i) text_.push_back(static_cast<char>(value >> (8 * i)));
    }
    void Text(std::string_view value) { Number(value.size()); Check(value.size()); text_.append(value); }
    void Raw(const Trigger::RawText& value) {
        Number(static_cast<unsigned>(value.encoding)); Text(Hash(value.text)); Text(Hash(value.originalBytes));
    }
    void Path(const std::filesystem::path& value) {
        const auto text = value.generic_u8string(); Text({reinterpret_cast<const char*>(text.data()), text.size()});
    }
    void Origin(const OriginSpan& value) { Path(value.source); Number(value.offset); Number(value.length); Text(value.fingerprint); }
    std::string Finish() { return Hash(text_); }
private:
    void Check(std::size_t count) {
        if (count > kDocumentBytes - text_.size()) Reject("LUA_AUTHOR_BINDING_BUDGET", "Lua 作者图身份超过 64 MiB 预算。");
    }
    std::string text_;
};

LuaAuthorCodeIdentity Identify(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog) {
    TrustedDocument(document, catalog);
    const auto encoded = Trigger::WtgWctCodec{}.Serialize(document, catalog);
    if (!encoded.Succeeded() || !encoded.value) Reject("LUA_AUTHOR_DOCUMENT_INVALID", "Lua 作者文档不能完整序列化。");
    LuaAuthorCodeIdentity result;
    result.sourceWtgSha256 = document.SourceWtgSha256(); result.sourceWctSha256 = document.SourceWctSha256();
    result.catalogSha256 = catalog.Sha256(); result.currentWtgSha256 = Hash(encoded.value->wtg); result.currentWctSha256 = Hash(encoded.value->wct);
    Binding binding;
    binding.Text("forge-native-lua-author-v1"); binding.Text(result.sourceWtgSha256); binding.Text(result.sourceWctSha256);
    binding.Text(result.catalogSha256); binding.Text(result.currentWtgSha256); binding.Text(result.currentWctSha256);
    binding.Number(document.wct.version); binding.Raw(document.wct.customComment); binding.Raw(document.wct.customCode);
    binding.Number(document.categories.size());
    for (const auto& row : document.categories) { binding.Number(row.id); binding.Raw(row.name); binding.Origin(row.origin); }
    binding.Number(document.variables.size());
    for (const auto& row : document.variables) {
        binding.Number(row.id); binding.Raw(row.name); binding.Raw(row.typeName); binding.Raw(row.value); binding.Origin(row.origin);
    }
    binding.Number(document.triggers.size());
    for (const auto& row : document.triggers) {
        binding.Number(row.id); binding.Raw(row.name); binding.Raw(row.description); binding.Raw(row.customText); binding.Origin(row.origin);
        binding.Number(row.ecas.size()); for (const auto index : row.ecas) binding.Number(index);
    }
    binding.Number(document.ecaNodes.size());
    for (const auto& row : document.ecaNodes) {
        binding.Number(row.id); binding.Number(row.sourceTriggerId); binding.Raw(row.name); binding.Origin(row.origin);
        binding.Path(row.uiSource); binding.Text(row.uiLayer); binding.Text(row.uiSignature);
        binding.Number(row.arguments.size()); for (const auto index : row.arguments) binding.Number(index);
        binding.Number(row.children.size()); for (const auto index : row.children) binding.Number(index);
    }
    binding.Number(document.argumentNodes.size());
    for (const auto& row : document.argumentNodes) {
        binding.Number(row.id); binding.Raw(row.value); binding.Origin(row.origin);
        binding.Number(row.callEca.has_value()); if (row.callEca) binding.Number(*row.callEca);
        binding.Number(row.indexArgument.has_value()); if (row.indexArgument) binding.Number(*row.indexArgument);
    }
    result.graphSha256 = binding.Finish();
    return result;
}

void Poll(const Lua::SyntaxOptions& options) {
    if (options.cancel.stop_requested()) Reject("LUA_AUTHOR_CANCELLED", "Lua 作者代码捕获已取消。");
    if (std::chrono::steady_clock::now() >= options.deadline) Reject("LUA_AUTHOR_DEADLINE", "Lua 作者代码捕获超过截止时间。");
}
bool Identifier(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool IdentifierPart(char c) { return Identifier(c) || (c >= '0' && c <= '9'); }

using Token = lua::DeclarationToken;
class DeclarationTokens {
public:
    DeclarationTokens(std::string_view text, const Lua::SyntaxOptions& options) : text_(text), options_(options) {}
    std::vector<Token> Run() {
        std::vector<Token> tokens;
        tokens.reserve((std::min)(text_.size() / 8, std::size_t{4096}));
        while (at_ < text_.size()) {
            Step(); const auto start = at_; const auto c = text_[at_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') { ++at_; continue; }
            if (c == '-' && At(at_ + 1) == '-') {
                at_ += 2;
                if (const auto longSize = LongOpen(at_)) SkipLong(longSize);
                else while (at_ < text_.size() && text_[at_] != '\r' && text_[at_] != '\n') { Step(); ++at_; }
                continue;
            }
            if (c == '\'' || c == '"') {
                ++at_;
                while (at_ < text_.size()) {
                    Step(); const auto next = text_[at_++];
                    if (next == '\\' && at_ < text_.size()) { ++at_; continue; }
                    if (next == c) break;
                }
                Push(tokens, {"<literal>", start}); continue;
            }
            if (const auto longSize = LongOpen(at_)) { SkipLong(longSize); Push(tokens, {"<literal>", start}); continue; }
            if (Identifier(c)) {
                ++at_;
                while (at_ < text_.size() && IdentifierPart(text_[at_])) { Step(); ++at_; }
                if (at_ - start > kNameBytes) Reject("LUA_AUTHOR_NAME_BUDGET", "Lua 声明名称超过 4096 字节预算。");
            } else ++at_;
            Push(tokens, {text_.substr(start, at_ - start), start});
        }
        Poll(options_);
        return tokens;
    }
private:
    char At(std::size_t index) const { return index < text_.size() ? text_[index] : '\0'; }
    std::size_t LongOpen(std::size_t index) {
        if (At(index) != '[') return 0;
        auto end = index + 1;
        while (At(end) == '=') { Step(); ++end; }
        return At(end) == '[' ? end - index + 1 : 0;
    }
    void SkipLong(std::size_t open) {
        at_ += open;
        while (at_ < text_.size()) {
            Step();
            if (text_[at_] == ']') {
                auto end = at_ + 1;
                while (At(end) == '=') { Step(); ++end; }
                if (At(end) == ']' && end - at_ + 1 == open) { at_ = end + 1; return; }
            }
            ++at_;
        }
    }
    void Step() { if ((ticks_++ & 255u) == 0) Poll(options_); }
    static void Push(std::vector<Token>& tokens, Token token) {
        if (tokens.size() == kTokens) Reject("LUA_AUTHOR_TOKEN_BUDGET", "Lua 作者声明扫描超过 1048576 个记号预算。");
        tokens.push_back(token);
    }
    std::string_view text_;
    const Lua::SyntaxOptions& options_;
    std::size_t at_ = 0, ticks_ = 0;
};

// 此扫描只证明具名顶层函数声明；语法已由同一个 Lua 5.3 编译器检查。
// 不执行正文，也不把字符串、注释、局部或嵌套声明当成可调用初始化函数。
using Declarations = lua::AuthorDeclarations;
Declarations GlobalFunctions(std::string_view text, const Lua::SyntaxOptions& options) {
    const auto tokens = DeclarationTokens(text, options).Run();
    std::set<std::string> functions;
    std::set<std::string> locals;
    std::set<std::size_t> pendingControl;
    std::size_t depth = 0, grouping = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if ((i & 255u) == 0) Poll(options);
        const auto value = tokens[i].text;
        if (value == "(" || value == "[" || value == "{") {
            if (++grouping > 128) Reject("LUA_AUTHOR_NESTING_BUDGET", "Lua 作者声明扫描超过 128 层表达式预算。");
            continue;
        }
        if (value == ")" || value == "]" || value == "}") { if (grouping) --grouping; continue; }
        if (value == "for" || value == "while") pendingControl.insert(depth);
        if (value == "local" && depth == 0 && i + 1 < tokens.size()) {
            auto name = i + 1;
            if (tokens[name].text == "function") ++name;
            while (name < tokens.size() && !tokens[name].text.empty() && Identifier(tokens[name].text.front())) {
                locals.emplace(tokens[name].text);
                if (locals.size() > 8192) Reject("LUA_AUTHOR_DECLARATION_BUDGET", "Lua 作者块的局部名称超过 8192 项预算。");
                if (name + 1 >= tokens.size() || tokens[name + 1].text != ",") break;
                name += 2;
            }
        }
        if (value == "function") {
            std::string_view name;
            if (depth == 0 && grouping == 0 && !pendingControl.contains(0) && i + 2 < tokens.size() && tokens[i + 2].text == "("
                && !tokens[i + 1].text.empty() && Identifier(tokens[i + 1].text.front())
                && (i == 0 || tokens[i - 1].text != "local")) name = tokens[i + 1].text;
            else if (depth == 0 && grouping == 0 && !pendingControl.contains(0) && i >= 2 && tokens[i - 1].text == "=" && i + 1 < tokens.size()
                && tokens[i + 1].text == "(" && !tokens[i - 2].text.empty() && Identifier(tokens[i - 2].text.front())
                && (i == 2 || (tokens[i - 3].text != "local" && tokens[i - 3].text != "."
                    && tokens[i - 3].text != "," && tokens[i - 3].text != "]"))) name = tokens[i - 2].text;
            if (!name.empty() && !locals.contains(std::string(name)) && !functions.emplace(name).second)
                Reject("LUA_AUTHOR_DECLARATION_CONFLICT", "Lua 作者块包含重复的顶层函数声明。");
            if (functions.size() > 8192) Reject("LUA_AUTHOR_DECLARATION_BUDGET", "Lua 作者块的顶层函数超过 8192 项预算。");
            if (++depth > 128) Reject("LUA_AUTHOR_NESTING_BUDGET", "Lua 作者声明扫描超过 128 层块预算。");
        } else if (value == "if" || value == "do" || value == "repeat") {
            if (value == "do") pendingControl.erase(depth);
            if (++depth > 128) Reject("LUA_AUTHOR_NESTING_BUDGET", "Lua 作者声明扫描超过 128 层块预算。");
        } else if (value == "end" || value == "until") {
            if (!depth) Reject("LUA_AUTHOR_DECLARATION_SHAPE", "Lua 作者声明块身份不能完整确认。");
            --depth;
        }
    }
    return {std::move(functions), std::move(locals)};
}

Declarations Compile(std::string_view text, std::string source, const Lua::SyntaxOptions& options) {
    Poll(options);
    const auto checked = Lua::CheckSyntax(text, source, options);
    if (!checked.Succeeded()) {
        const auto code = checked.diagnostics.empty() ? "LUA_AUTHOR_SYNTAX_INCOMPLETE" : checked.diagnostics.front().code;
        const auto message = checked.diagnostics.empty() ? "Lua 作者代码编译未完成。" : checked.diagnostics.front().message;
        Reject(code, source + ": " + message, {std::filesystem::u8path(source), 0, 0, {}});
    }
    return GlobalFunctions(text, options);
}
bool ReservedFunction(std::string_view name) {
    constexpr std::string_view names[]{"config", "main", "InitGlobals", "InitCustomTriggers", "RunInitializationTriggers",
        "InitSounds", "CreateRegions", "CreateCameras", "InitUpgrades", "InitTechTree", "CreateAllDestructables",
        "CreateAllItems", "CreateAllUnits", "InitCustomPlayerSlots", "InitCustomTeams", "InitAllyPriorities"};
    return name.starts_with("__forge_") || std::find(std::begin(names), std::end(names), name) != std::end(names);
}
void AddDeclarations(const std::set<std::string>& declarations, std::set<std::string>& all,
    const std::set<std::string>& guiNames) {
    for (const auto& name : declarations)
        if (ReservedFunction(name) || guiNames.contains(name) || !all.insert(name).second)
            Reject("LUA_AUTHOR_DECLARATION_CONFLICT", "Lua 作者函数与生成函数或另一个活动作者块冲突。");
    if (all.size() > 8192) Reject("LUA_AUTHOR_DECLARATION_BUDGET", "活动 Lua 作者块的顶层函数合计超过 8192 项预算。");
}
template<class T> Result<T> Failure(const Diagnostic& diagnostic) { return {std::nullopt, {diagnostic}}; }
} // namespace

std::vector<lua::DeclarationToken> lua::ScanDeclarationTokens(std::string_view text, const Lua::SyntaxOptions& options) {
    return DeclarationTokens(text, options).Run();
}
lua::AuthorDeclarations lua::CollectAuthorDeclarations(std::string_view text, const Lua::SyntaxOptions& options) {
    return GlobalFunctions(text, options);
}

const LuaAuthorCodeIdentity* PreparedLuaAuthorCode::Identity() const noexcept { return state_ ? &state_->identity : nullptr; }
std::string_view PreparedLuaAuthorCode::Header() const noexcept { return state_ ? state_->header : std::string_view{}; }
std::span<const LuaAuthorTriggerCode> PreparedLuaAuthorCode::Triggers() const noexcept {
    return state_ ? std::span<const LuaAuthorTriggerCode>(state_->triggers) : std::span<const LuaAuthorTriggerCode>{};
}
Status PreparedLuaAuthorCode::Verify(const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog) const {
    try {
        if (!state_) Reject("LUA_AUTHOR_SEAL_MISSING", "缺少原生 Lua 作者代码凭据。");
        const auto current = Identify(document, catalog);
        if (current.graphSha256 != state_->identity.graphSha256)
            Reject("LUA_AUTHOR_SEAL_STALE", "原生 Lua 作者正文、图身份或目录已改变；需要重新捕获保存输入。");
        return Status::Ok();
    } catch (const Diagnostic& failure) { return {{failure}}; }
    catch (const std::bad_alloc&) { return Status::Failure("Script.LuaAuthor", "LUA_AUTHOR_ALLOCATION_FAILED", "Lua 作者身份验证内存不足。"); }
    catch (const std::exception&) { return Status::Failure("Script.LuaAuthor", "LUA_AUTHOR_IDENTITY_FAILED", "Lua 作者图身份不能完整验证。"); }
}

Result<PreparedLuaAuthorCode> CaptureNativeLuaAuthorCode(std::span<const std::byte> sourceSettings,
    const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog, const Lua::SyntaxOptions& requested) {
    try {
        Lua::SyntaxOptions options = requested;
        const auto now = std::chrono::steady_clock::now();
        const auto duration = (std::clamp)(options.maxDuration, std::chrono::milliseconds::zero(), std::chrono::milliseconds{30000});
        options.deadline = (std::min)(options.deadline, now + duration);
        options.maxSourceBytes = (std::min)(options.maxSourceBytes, kAuthorBytes);
        Poll(options); NativeSource(sourceSettings);
        auto state = std::make_shared<PreparedLuaAuthorCode::State>();
        state->identity = Identify(document, catalog); state->identity.sourceSettingsSha256 = Hash(sourceSettings);
        std::set<std::string> guiNames, allNames, topLevelLocals;
        for (const auto& trigger : document.triggers) {
            if (trigger.type != 0 || !trigger.enabled || trigger.customTextFlag) continue;
            const auto suffix = "F" + std::to_string(trigger.id);
            guiNames.insert("InitTrig_" + suffix); guiNames.insert("Trig_" + suffix + "_Actions"); guiNames.insert("Trig_" + suffix + "_Conditions");
            guiNames.insert("gg_trg_" + suffix);
        }
        std::size_t bytes = document.wct.customCode.text.size();
        if (bytes > kAuthorBytes) Reject("LUA_AUTHOR_SOURCE_BUDGET", "Lua 作者代码超过 8 MiB 总预算。");
        state->header = document.wct.customCode.text;
        const auto collect = [&](const Declarations& declarations) {
            AddDeclarations(declarations.functions, allNames, guiNames);
            topLevelLocals.insert(declarations.locals.begin(), declarations.locals.end());
            if (topLevelLocals.size() > 8192)
                Reject("LUA_AUTHOR_DECLARATION_BUDGET", "活动 Lua 作者块的顶层局部名称合计超过 8192 项预算。");
        };
        collect(Compile(state->header, "WCT/global.lua", options));
        for (const auto& trigger : document.triggers) {
            Poll(options);
            if (!trigger.customTextFlag) continue;
            if (trigger.type != 0 || trigger.customTextFlag != 1 || !trigger.ecas.empty())
                Reject("LUA_AUTHOR_TRIGGER_SHAPE", "Lua 自定义触发器不能含 GUI ECA 或未知标志。", trigger.origin);
            if (trigger.customText.text.size() > kAuthorBytes - bytes)
                Reject("LUA_AUTHOR_SOURCE_BUDGET", "Lua 作者代码超过 8 MiB 总预算。", trigger.origin);
            bytes += trigger.customText.text.size();
            if (!trigger.enabled) continue;
            if (trigger.name.text.size() > kNameBytes)
                Reject("LUA_AUTHOR_NAME_BUDGET", "Lua 触发器脚本名称超过 4096 字节预算。", trigger.origin);
            const auto suffix = Trigger::TriggerDirectoryEditor::ScriptName(trigger.name.text);
            LuaAuthorTriggerCode block{trigger.id, trigger.customText.text, "gg_trg_" + suffix, "InitTrig_" + suffix};
            if (guiNames.contains(block.variable))
                Reject("LUA_AUTHOR_DECLARATION_CONFLICT", "Lua 作者触发器变量与活动 GUI 的生成变量冲突。", trigger.origin);
            const auto declarations = Compile(block.text, "WCT/trigger-" + std::to_string(trigger.id) + ".lua", options);
            if (!declarations.functions.contains(block.initializer))
                Reject("LUA_AUTHOR_INITIALIZER_MISSING", "活动 Lua 自定义触发器缺少顶层 " + block.initializer + " 初始化函数。", trigger.origin);
            collect(declarations);
            state->triggers.push_back(std::move(block));
        }
        // Header 和活动 custom 实际合并为同一 chunk，生成调用位于全部作者块之后。
        // 任一顶层 local 都会遮蔽后续生成调用；不能只在各块声明当时检查局部名称。
        std::set<std::string_view> initializerNames;
        for (const auto& block : state->triggers) initializerNames.insert(block.initializer);
        for (const auto& name : topLevelLocals) {
            Poll(options);
            if (ReservedFunction(name) || guiNames.contains(name) || initializerNames.contains(name))
                Reject("LUA_AUTHOR_INITIALIZER_SHADOWED", "顶层局部名称遮蔽原生 Lua 初始化或生成入口，不能确认初始化声明合同。");
        }
        Poll(options);
        PreparedLuaAuthorCode result; result.state_ = std::move(state);
        return Result<PreparedLuaAuthorCode>::Success(std::move(result));
    } catch (const Diagnostic& failure) { return Failure<PreparedLuaAuthorCode>(failure); }
    catch (const std::bad_alloc&) { return Result<PreparedLuaAuthorCode>::Failure("Script.LuaAuthor", "LUA_AUTHOR_ALLOCATION_FAILED", "Lua 作者代码捕获内存不足。"); }
    catch (const std::exception&) { return Result<PreparedLuaAuthorCode>::Failure("Script.LuaAuthor", "LUA_AUTHOR_CAPTURE_FAILED", "Lua 作者代码捕获未完整完成。"); }
}

Result<Trigger::TriggerCodeEditPlan> LuaAuthorCodeEditor::PrepareText(std::span<const std::byte> sourceSettings,
    const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog,
    Trigger::CodeBlockTarget target, std::string_view normalizedUtf8, std::size_t maxEditedBytes) {
    try {
        NativeSource(sourceSettings); TrustedDocument(document, catalog);
        return Trigger::TriggerCodeEditor::PrepareText(document, catalog, target, normalizedUtf8, maxEditedBytes);
    } catch (const Diagnostic& failure) { return Failure<Trigger::TriggerCodeEditPlan>(failure); }
    catch (const std::bad_alloc&) { return Result<Trigger::TriggerCodeEditPlan>::Failure("Script.LuaAuthor", "LUA_AUTHOR_ALLOCATION_FAILED", "Lua 作者编辑计划内存不足。"); }
    catch (const std::exception&) { return Result<Trigger::TriggerCodeEditPlan>::Failure("Script.LuaAuthor", "LUA_AUTHOR_PREPARE_FAILED", "Lua 作者编辑计划未完整完成。"); }
}
Result<Trigger::TriggerCodeEditPlan> LuaAuthorCodeEditor::PrepareNewCustom(std::span<const std::byte> sourceSettings,
    const Trigger::TriggerDocument& document, const Trigger::UiCatalog& catalog, std::int32_t categoryId,
    std::string name, std::string_view normalizedUtf8, std::size_t maxEditedBytes, std::string newCategoryName) {
    try {
        NativeSource(sourceSettings); TrustedDocument(document, catalog);
        std::string skeleton;
        if (normalizedUtf8.empty()) {
            const auto suffix = Trigger::TriggerDirectoryEditor::ScriptName(name);
            skeleton = "function InitTrig_" + suffix + "()\n    gg_trg_" + suffix + " = CreateTrigger()\nend\n";
            normalizedUtf8 = skeleton;
        }
        return Trigger::TriggerCodeEditor::PrepareNewCustom(document, catalog, categoryId, std::move(name),
            normalizedUtf8, maxEditedBytes, std::move(newCategoryName));
    } catch (const Diagnostic& failure) { return Failure<Trigger::TriggerCodeEditPlan>(failure); }
    catch (const std::bad_alloc&) { return Result<Trigger::TriggerCodeEditPlan>::Failure("Script.LuaAuthor", "LUA_AUTHOR_ALLOCATION_FAILED", "Lua 作者新建计划内存不足。"); }
    catch (const std::exception&) { return Result<Trigger::TriggerCodeEditPlan>::Failure("Script.LuaAuthor", "LUA_AUTHOR_PREPARE_FAILED", "Lua 作者新建计划未完整完成。"); }
}
} // namespace War3Map::Script
