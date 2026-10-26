#include "War3MapCore/Trigger/TriggerTextNotation.h"

#include "War3MapCore/Trigger/TriggerBlocks.h"
#include "War3MapCore/Trigger/TriggerDataText.h"
#include "War3MapCore/Trigger/MapObjectReferences.h"
#include "War3MapCore/Trigger/TriggerDirectory.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <optional>

namespace War3Map::Trigger {
namespace {

constexpr const char* kDomain = "Trigger.Text";
constexpr std::size_t kMaximumErrors = 64;

struct Position {
    std::size_t line = 1, column = 1;
};

std::string Where(const Position& at) {
    return "第 " + std::to_string(at.line) + " 行第 " + std::to_string(at.column) + " 列";
}

// ---- 词法 ----

enum class TokenKind : std::uint8_t {
    Name, String, Code, Number, LParen, RParen, LBracket, RBracket, LBrace, RBrace,
    Comma, Colon, Semicolon, Tilde, Branch, Unset, End,
};

struct Token {
    TokenKind kind = TokenKind::End;
    std::string text;
    Position at;
};

bool Delimiter(char c) {
    switch (c) {
    case '(': case ')': case '[': case ']': case '{': case '}': case ',': case ':': case ';':
    case '"': case '\'': case '`': case '~': case '#': case '/': case '?':
        return true;
    default:
        return std::isspace(static_cast<unsigned char>(c)) != 0;
    }
}

bool NumberBody(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '$';
}

bool NumberStart(std::string_view text, std::size_t i) {
    const char c = text[i];
    if (std::isdigit(static_cast<unsigned char>(c))) return true;
    return (c == '-' || c == '.' || c == '$') && i + 1 < text.size() && NumberBody(text[i + 1]) && text[i + 1] != '$';
}

class Lexer {
public:
    explicit Lexer(std::string_view text) : text_(text) {}

    Result<std::vector<Token>> Run() {
        std::vector<Token> tokens;
        while (true) {
            Skip();
            Token token; token.at = at_;
            if (i_ >= text_.size()) { tokens.push_back(token); break; }
            const char c = text_[i_];
            const auto single = [&](TokenKind kind) { token.kind = kind; token.text = std::string(1, c); Advance(1); };
            switch (c) {
            case '(': single(TokenKind::LParen); break;
            case ')': single(TokenKind::RParen); break;
            case '[': single(TokenKind::LBracket); break;
            case ']': single(TokenKind::RBracket); break;
            case '{': single(TokenKind::LBrace); break;
            case '}': single(TokenKind::RBrace); break;
            case ',': single(TokenKind::Comma); break;
            case ':': single(TokenKind::Colon); break;
            case '?': single(TokenKind::Unset); break;
            case ';': single(TokenKind::Semicolon); break;
            case '~': single(TokenKind::Tilde); break;
            case '"': if (auto failed = QuotedString(token)) return *failed; break;
            case '\'': case '`': if (auto failed = Raw(token, c)) return *failed; break;
            case '#': {
                Advance(1); token.kind = TokenKind::Branch;
                while (i_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[i_]))) { token.text += text_[i_]; Advance(1); }
                if (token.text.empty() || token.text.size() > 9) return Error(token.at, "# 后须跟分支号。");
                break;
            }
            default:
                if (NumberStart(text_, i_)) {
                    token.kind = TokenKind::Number; token.text += c; Advance(1);
                    while (i_ < text_.size() && NumberBody(text_[i_])) { token.text += text_[i_]; Advance(1); }
                } else if (Delimiter(c)) {
                    return Error(token.at, std::string("无法识别的字符“") + c + "”。");
                } else {
                    token.kind = TokenKind::Name;
                    while (i_ < text_.size() && !Delimiter(text_[i_])) { token.text += text_[i_]; Advance(1); }
                }
            }
            tokens.push_back(std::move(token));
        }
        return Result<std::vector<Token>>::Success(std::move(tokens));
    }

private:
    using Failed = std::optional<Result<std::vector<Token>>>;

    static Result<std::vector<Token>> Error(const Position& at, std::string message) {
        return Result<std::vector<Token>>::Failure(kDomain, "TRIGGER_TEXT_SYNTAX", Where(at) + "：" + std::move(message));
    }
    void Advance(std::size_t count) {
        for (; count > 0 && i_ < text_.size(); --count, ++i_) {
            const auto byte = static_cast<unsigned char>(text_[i_]);
            if (byte == '\n') { ++at_.line; at_.column = 1; }
            else if ((byte & 0xC0U) != 0x80U) ++at_.column; // 列按字符计，不按 UTF-8 字节
        }
    }
    void Skip() {
        while (i_ < text_.size()) {
            if (std::isspace(static_cast<unsigned char>(text_[i_]))) Advance(1);
            else if (text_.substr(i_).starts_with("//")) { while (i_ < text_.size() && text_[i_] != '\n') Advance(1); }
            else break;
        }
    }
    Failed QuotedString(Token& token) {
        token.kind = TokenKind::String; Advance(1);
        while (true) {
            if (i_ >= text_.size()) return Error(token.at, "文本缺少结束引号。");
            const char c = text_[i_];
            if (c == '"') { Advance(1); return std::nullopt; }
            if (c != '\\') { token.text += c; Advance(1); continue; }
            if (i_ + 1 >= text_.size()) return Error(token.at, "文本以转义符结束。");
            switch (text_[i_ + 1]) {
            case '"': token.text += '"'; break;
            case '\\': token.text += '\\'; break;
            case 'n': token.text += '\n'; break;
            case 'r': token.text += '\r'; break;
            case 't': token.text += '\t'; break;
            default: return Error(at_, std::string("不支持的转义 \\") + text_[i_ + 1] + "。");
            }
            Advance(2);
        }
    }
    Failed Raw(Token& token, char quote) {
        token.kind = quote == '\'' ? TokenKind::Code : TokenKind::Name; Advance(1);
        while (i_ < text_.size() && text_[i_] != quote && text_[i_] != '\n') { token.text += text_[i_]; Advance(1); }
        if (i_ >= text_.size() || text_[i_] != quote)
            return Error(token.at, quote == '\'' ? "物编编号缺少结束单引号。" : "反引号名字缺少结束反引号。");
        Advance(1);
        if (token.text.empty()) return Error(token.at, "引号内不能为空。");
        return std::nullopt;
    }

    std::string_view text_;
    std::size_t i_ = 0;
    Position at_;
};

// ---- 语法树 ----

struct Expression {
    TokenKind kind = TokenKind::Name; // Name / String / Code / Number；call=true 时为函数
    bool call = false;
    std::string text;
    Position at;
    std::vector<Expression> arguments;
    std::shared_ptr<Expression> index;
};

struct Statement;
struct Section {
    std::optional<Token> label;
    std::vector<Statement> statements;
};
struct Statement {
    bool disabled = false, block = false;
    std::string name;
    Position at;
    std::vector<Expression> arguments;
    std::vector<Section> sections;
};

class Parser {
public:
    Parser(std::vector<Token> tokens, std::size_t maxDepth) : tokens_(std::move(tokens)), maxDepth_(maxDepth) {}

    Result<std::vector<Section>> Run() {
        auto body = Body(0);
        if (!body.Succeeded()) return body;
        if (Peek().kind != TokenKind::End) return Error(Peek().at, "多余的“" + Peek().text + "”。");
        return body;
    }

private:
    const Token& Peek(std::size_t ahead = 0) const { return tokens_[std::min(i_ + ahead, tokens_.size() - 1)]; }
    Token Take() { Token token = Peek(); if (i_ < tokens_.size() - 1) ++i_; return token; }
    template <typename T = std::vector<Section>>
    static Result<T> Error(const Position& at, std::string message) {
        return Result<T>::Failure(kDomain, "TRIGGER_TEXT_SYNTAX", Where(at) + "：" + std::move(message));
    }

    Result<std::vector<Section>> Body(std::size_t depth) {
        if (depth > maxDepth_) return Error(Peek().at, "嵌套层数超过上限。");
        std::vector<Section> sections(1);
        while (Peek().kind != TokenKind::End && Peek().kind != TokenKind::RBrace) {
            if (Peek().kind == TokenKind::Semicolon) { Take(); continue; }
            if ((Peek().kind == TokenKind::Name || Peek().kind == TokenKind::Branch) && Peek(1).kind == TokenKind::Colon) {
                sections.push_back(Section{Take(), {}}); Take();
                continue;
            }
            auto statement = ParseStatement(depth);
            if (!statement.Succeeded()) return {std::nullopt, std::move(statement.diagnostics)};
            sections.back().statements.push_back(std::move(*statement.value));
        }
        if (sections.front().statements.empty() && sections.size() > 1) sections.erase(sections.begin());
        return Result<std::vector<Section>>::Success(std::move(sections));
    }

    Result<Statement> ParseStatement(std::size_t depth) {
        Statement statement;
        if (Peek().kind == TokenKind::Tilde) { Take(); statement.disabled = true; }
        const Token name = Take();
        if (name.kind != TokenKind::Name) return Error<Statement>(name.at, "此处应为事件/条件/动作函数名，实际是“" + name.text + "”。");
        statement.name = name.text; statement.at = name.at;
        bool written = false;
        if (Peek().kind == TokenKind::LParen) {
            auto arguments = Arguments(depth);
            if (!arguments.Succeeded()) return {std::nullopt, std::move(arguments.diagnostics)};
            statement.arguments = std::move(*arguments.value); written = true;
        }
        if (Peek().kind == TokenKind::LBrace) {
            Take(); statement.block = true; written = true;
            auto body = Body(depth + 1);
            if (!body.Succeeded()) return {std::nullopt, std::move(body.diagnostics)};
            if (Peek().kind != TokenKind::RBrace) return Error<Statement>(Peek().at, statement.name + " 的 { 缺少对应的 }。");
            Take(); statement.sections = std::move(*body.value);
        }
        if (!written) return Error<Statement>(name.at, "语句须写成 " + statement.name + "(参数...)。");
        return Result<Statement>::Success(std::move(statement));
    }

    Result<std::vector<Expression>> Arguments(std::size_t depth) {
        const Token open = Take();
        std::vector<Expression> arguments;
        if (Peek().kind == TokenKind::RParen) { Take(); return Result<std::vector<Expression>>::Success({}); }
        while (true) {
            auto argument = ParseExpression(depth + 1);
            if (!argument.Succeeded()) return {std::nullopt, std::move(argument.diagnostics)};
            arguments.push_back(std::move(*argument.value));
            const Token next = Take();
            if (next.kind == TokenKind::RParen) break;
            if (next.kind != TokenKind::Comma)
                return Error<std::vector<Expression>>(next.at, next.kind == TokenKind::End
                    ? Where(open.at) + " 的 ( 缺少对应的 )。" : "参数之间应为逗号，实际是“" + next.text + "”。");
        }
        return Result<std::vector<Expression>>::Success(std::move(arguments));
    }

    Result<Expression> ParseExpression(std::size_t depth) {
        if (depth > maxDepth_) return Error<Expression>(Peek().at, "嵌套层数超过上限。");
        const Token token = Take();
        Expression expression; expression.kind = token.kind; expression.text = token.text; expression.at = token.at;
        if (token.kind == TokenKind::String || token.kind == TokenKind::Code || token.kind == TokenKind::Number || token.kind == TokenKind::Unset)
            return Result<Expression>::Success(std::move(expression));
        if (token.kind != TokenKind::Name) return Error<Expression>(token.at, "此处应为参数，实际是“" + token.text + "”。");
        if (Peek().kind == TokenKind::LParen) {
            auto arguments = Arguments(depth);
            if (!arguments.Succeeded()) return {std::nullopt, std::move(arguments.diagnostics)};
            expression.call = true; expression.arguments = std::move(*arguments.value);
        } else if (Peek().kind == TokenKind::LBracket) {
            Take();
            auto index = ParseExpression(depth + 1);
            if (!index.Succeeded()) return index;
            if (Peek().kind != TokenKind::RBracket) return Error<Expression>(Peek().at, "数组下标缺少 ]。");
            Take(); expression.index = std::make_shared<Expression>(std::move(*index.value));
        }
        if (Peek().kind == TokenKind::LBrace) return Error<Expression>(Peek().at, "参数里的函数不能带 { } 分支。");
        return Result<Expression>::Success(std::move(expression));
    }

    std::vector<Token> tokens_;
    std::size_t i_ = 0;
    std::size_t maxDepth_;
};

// ---- 按函数库解析为图节点 ----

const char* KindName(UiDefinitionKind kind) {
    switch (kind) {
    case UiDefinitionKind::Event: return "事件";
    case UiDefinitionKind::Condition: return "条件";
    case UiDefinitionKind::Action: return "动作";
    case UiDefinitionKind::Call: return "函数";
    default: return "定义";
    }
}

std::size_t ArgumentCount(const UiDefinition& definition) {
    const auto& arguments = definition.Active().arguments;
    return static_cast<std::size_t>(std::count_if(arguments.begin(), arguments.end(),
        [](const UiArgumentDefinition& argument) { return argument.type != "nothing"; }));
}

std::string Signature(const UiDefinition& definition) {
    std::string text;
    for (const auto& argument : definition.Active().arguments) {
        if (argument.type == "nothing") continue;
        if (!text.empty()) text += ", ";
        text += argument.type;
    }
    return definition.name + "(" + text + ")";
}

class Resolver {
public:
    Resolver(const TriggerDocument& document, const UiCatalog& catalog, const TriggerGraphEditLimits& limits)
        : document_(document), catalog_(catalog), types_(catalog), limits_(limits) {}

    std::vector<Diagnostic> errors;

    std::shared_ptr<GraphEcaValue> Ecas(const Statement& statement, std::int32_t ecaType, std::optional<std::int32_t> branch, std::size_t depth) {
        const auto kind = UiDefinitionKindFromEcaType(ecaType);
        const auto* definition = Definition(kind, statement.name, statement.at);
        auto eca = std::make_shared<GraphEcaValue>();
        eca->function = statement.name; eca->kind = kind; eca->enabled = !statement.disabled; eca->childBranch = branch;
        eca->origin = Where(statement.at) + " " + statement.name;
        if (!Count(statement.at, depth)) return eca;
        if (definition != nullptr) Arguments(*eca, *definition, statement.arguments, statement.at, depth);
        const auto* branches = FindTriggerBlockBranches(statement.name, &catalog_);
        if (statement.block && branches == nullptr) {
            UndeclaredBranches(*eca, statement, depth);
            return eca;
        }
        for (const auto& section : statement.sections) {
            const TriggerBlockBranch* target = nullptr;
            if (!section.label) {
                if (branches->size() == 1) target = &branches->front();
                else if (!section.statements.empty()) {
                    Error("TRIGGER_TEXT_BRANCH", section.statements.front().at, statement.name + " 有多个分支，请先写分支标签：" + Labels(*branches) + "。");
                    continue;
                }
            } else {
                target = Branch(*branches, *section.label);
                if (target == nullptr) {
                    Error("TRIGGER_TEXT_BRANCH", section.label->at, statement.name + " 没有分支“" + section.label->text + "”，可用：" + Labels(*branches) + "。");
                    continue;
                }
            }
            for (const auto& child : section.statements)
                eca->children.push_back(Ecas(child, target->type, target->branch, depth + 1));
        }
        return eca;
    }

private:
    // 原图里非块语句也可能带子节点（WTG 只存分支号）。只接受 #分支号: 标签，
    // 子语句种类按函数库里唯一同名的事件/条件/动作确定，不能确定就报错。
    void UndeclaredBranches(GraphEcaValue& eca, const Statement& statement, std::size_t depth) {
        for (const auto& section : statement.sections) {
            if (section.statements.empty()) continue;
            if (!section.label || section.label->kind != TokenKind::Branch) {
                Error("TRIGGER_TEXT_BRANCH", section.label ? section.label->at : section.statements.front().at,
                    statement.name + " 不是块语句；它的子节点只能写在 #分支号: 下。");
                continue;
            }
            const auto branch = static_cast<std::int32_t>(std::stol(section.label->text));
            for (const auto& child : section.statements) {
                std::optional<std::int32_t> type;
                bool ambiguous = false;
                for (const auto kind : {UiDefinitionKind::Event, UiDefinitionKind::Condition, UiDefinitionKind::Action}) {
                    if (catalog_.Find(kind, child.name) == nullptr) continue;
                    ambiguous = ambiguous || type.has_value();
                    type = EcaTypeFromUiDefinitionKind(kind);
                }
                if (!type || ambiguous) {
                    Error("TRIGGER_TEXT_BRANCH", child.at, "无法确定 " + child.name + " 在 " + statement.name + " 下是事件、条件还是动作。");
                    continue;
                }
                eca.children.push_back(Ecas(child, *type, branch, depth + 1));
            }
        }
    }
    void Error(const char* code, const Position& at, std::string message) {
        if (errors.size() < kMaximumErrors)
            errors.push_back(Diagnostic{DiagnosticSeverity::Error, kDomain, code, Where(at) + "：" + std::move(message), std::nullopt});
    }
    bool Count(const Position& at, std::size_t depth) {
        if (++records_ > limits_.maxAddedRecords || depth > limits_.maxDepth) {
            if (!budgetReported_) Error("TRIGGER_TEXT_BUDGET", at, "节点数量或嵌套层数超过单批上限，请分批写入。");
            budgetReported_ = true;
            return false;
        }
        return true;
    }
    static std::string Labels(const std::vector<TriggerBlockBranch>& branches) {
        std::string text;
        for (const auto& branch : branches) text += (text.empty() ? "" : " / ") + std::string(branch.key) + ":";
        return text;
    }
    static const TriggerBlockBranch* Branch(const std::vector<TriggerBlockBranch>& branches, const Token& label) {
        for (const auto& branch : branches) {
            // 中文名可写全称（“否则 - 动作”需反引号）或短名（“否则”）。
            const std::string_view full(branch.label);
            const auto shortName = full.substr(0, full.find(" - "));
            if (label.kind == TokenKind::Branch ? label.text == std::to_string(branch.branch)
                    : label.text == branch.key || label.text == full || label.text == shortName) return &branch;
        }
        return nullptr;
    }
    const UiDefinition* Definition(UiDefinitionKind kind, const std::string& name, const Position& at) {
        if (const auto* definition = catalog_.Find(kind, name)) return definition;
        for (const auto other : {UiDefinitionKind::Event, UiDefinitionKind::Condition, UiDefinitionKind::Action, UiDefinitionKind::Call}) {
            if (other != kind && catalog_.Find(other, name) != nullptr) {
                Error("TRIGGER_TEXT_KIND_MISMATCH", at, name + " 是" + KindName(other) + "，不能写在" + KindName(kind) + "位置。");
                return nullptr;
            }
        }
        Error("TRIGGER_TEXT_UNKNOWN_FUNCTION", at, "函数库里没有" + std::string(KindName(kind)) + " " + name + "。");
        return nullptr;
    }
    void Arguments(GraphEcaValue& eca, const UiDefinition& definition, const std::vector<Expression>& written,
        const Position& at, std::size_t depth) {
        const auto expected = ArgumentCount(definition);
        if (written.size() != expected) {
            Error("TRIGGER_TEXT_ARGUMENT_COUNT", at, definition.name + " 需要 " + std::to_string(expected) + " 个参数，写了 "
                + std::to_string(written.size()) + " 个。签名：" + Signature(definition));
            return;
        }
        for (std::size_t i = 0; i < written.size(); ++i)
            eca.arguments.push_back(Argument(written[i], ArgumentType(definition, written, i), depth + 1));
    }
    // 声明为 Null 的参数按前面的值定类型（同 TriggerTypeSystem::ExpectedType）：YDWE 取最近的类型选择器，
    // 原版（SetVariable 等）取前一个参数，这里只认已声明的变量。触发器等显示名据此换成 gg_ 生成名。
    std::string ArgumentType(const UiDefinition& definition, const std::vector<Expression>& written, std::size_t position) const {
        std::string declared = TriggerTypeSystem::DeclaredType(definition, position);
        if (declared != "Null") return declared;
        const auto plainName = [](const Expression& expression) { return !expression.call && expression.kind == TokenKind::Name; };
        for (std::size_t index = position; index-- > 0;) {
            if (!types_.IsTypeSelector(TriggerTypeSystem::DeclaredType(definition, index))) continue;
            if (!plainName(written[index])) return declared;
            const auto type = TriggerTypeSystem::TypeFromTypename(written[index].text);
            return type.empty() ? declared : type;
        }
        if (position == 0 || !plainName(written[position - 1])) return declared;
        const auto& name = written[position - 1].text;
        const auto variable = std::find_if(document_.variables.begin(), document_.variables.end(), [&](const VariableRecord& row) {
            return row.name.text == name || (name.starts_with("udg_") && row.name.text == name.substr(4));
        });
        return variable == document_.variables.end() ? declared : variable->typeName.text;
    }
    GraphArgumentValue Argument(const Expression& expression, const std::string& declaredType, std::size_t depth) {
        GraphArgumentValue argument;
        if (!Count(expression.at, depth)) return argument;
        if (expression.call) {
            const auto slot = TriggerTypeSystem::FunctionSlotKind(declaredType);
            const auto kind = slot && catalog_.Find(*slot, expression.text) != nullptr ? *slot : UiDefinitionKind::Call;
            const auto* definition = Definition(kind, expression.text, expression.at);
            auto call = std::make_shared<GraphEcaValue>();
            call->function = expression.text; call->kind = kind;
            call->origin = Where(expression.at) + " " + expression.text;
            if (definition != nullptr) Arguments(*call, *definition, expression.arguments, expression.at, depth);
            argument.call = std::move(call);
            return argument;
        }
        if (expression.kind == TokenKind::Unset) {
            argument.scalar = {-1, {}}; // WE 的“参数未设置”
            return argument;
        }
        if (expression.kind != TokenKind::Name) {
            argument.scalar = {3, expression.text};
            return argument;
        }
        const auto& name = expression.text;
        const auto variable = std::find_if(document_.variables.begin(), document_.variables.end(), [&](const VariableRecord& row) {
            return row.name.text == name || (name.starts_with("udg_") && row.name.text == name.substr(4));
        });
        const bool declared = variable != document_.variables.end();
        if (expression.index) {
            if (!declared && !name.starts_with("gg_")) {
                Error("TRIGGER_TEXT_UNKNOWN_NAME", expression.at, "没有数组变量 " + name + "。");
                return argument;
            }
            argument.scalar = {1, declared ? variable->name.text : name};
            argument.index = std::make_shared<GraphArgumentValue>(Argument(*expression.index, "integer", depth + 1));
            return argument;
        }
        if (name == "true" || name == "false") { argument.scalar = {3, name}; return argument; }
        const bool preset = !name.starts_with("udg_") && FindPreset(catalog_, name).has_value();
        if (preset && declared) {
            Error("TRIGGER_TEXT_AMBIGUOUS_NAME", expression.at, name + " 既是预设又是变量名；变量请写 udg_" + name + "。");
        } else if (preset) {
            argument.scalar = {0, name};
        } else if (declared) {
            // 与 WE 一致存变量原名（不带 udg_）。
            argument.scalar = {1, variable->name.text};
        } else if (name.starts_with("gg_")) {
            argument.scalar = {1, name};
        } else if (const auto symbol = NamedObjectSymbol(name, declaredType, expression.at)) {
            if (!symbol->empty()) argument.scalar = {1, *symbol};
        } else if (types_.LiteralKind(declaredType) == TriggerTypeSystem::Literal::Script) {
            argument.scalar = {3, name};
        } else if (name.starts_with("udg_")) {
            Error("TRIGGER_TEXT_UNKNOWN_NAME", expression.at, "没有变量 " + name + "。");
        } else {
            Error("TRIGGER_TEXT_UNKNOWN_NAME", expression.at, name + " 不是预设、变量或预放对象；文本请加引号，函数请写 " + name + "()。"
                + PresetHint(declaredType));
        }
        return argument;
    }

    std::string PresetHint(const std::string& type) const {
        const auto presets = PresetsOfType(catalog_, type);
        if (presets.empty()) return {};
        std::string hint = "此处类型 " + type + " 的预设有：";
        for (std::size_t i = 0; i < presets.size() && i < 8; ++i) hint += (i ? "、" : "") + presets[i].name;
        return hint + (presets.size() > 8 ? " 等 " + std::to_string(presets.size()) + " 个。" : "。");
    }
    // 触发器、区域、镜头、声音参数可直接写显示名。触发器与 WE 一致存按名字存储字节换算后的 gg_trg_ 全局名：
    // WE 生成脚本时把参数值原样写出，只有这样才和它生成的声明一致；区域/镜头/声音按脚本生成器的命名规则换成
    // gg_ 全局，是否真实存在由图编辑的预放对象校验决定。nullopt：类型不适用；空串：已报错。
    std::optional<std::string> NamedObjectSymbol(const std::string& name, const std::string& type, const Position& at) {
        if (type == "trigger") {
            std::vector<const TriggerRecord*> found;
            for (const auto& trigger : document_.triggers)
                if (trigger.type == 0 && trigger.name.text == name) found.push_back(&trigger);
            if (found.size() != 1) {
                Error("TRIGGER_TEXT_UNKNOWN_NAME", at, found.empty() ? "没有名为“" + name + "”的触发器。"
                    : "有多个名为“" + name + "”的触发器，请写 gg_trg_ 生成名。");
                return std::string{};
            }
            return TriggerDirectoryEditor::TriggerSymbol(found.front()->name);
        }
        if (type == "rect") return "gg_rct_" + TriggerDirectoryEditor::ScriptName(name);
        if (type == "camerasetup") return "gg_cam_" + TriggerDirectoryEditor::ScriptName(name);
        if (type == "sound") return "gg_snd_" + TriggerDirectoryEditor::ScriptName(name);
        return std::nullopt;
    }

    const TriggerDocument& document_;
    const UiCatalog& catalog_;
    TriggerTypeSystem types_;
    const TriggerGraphEditLimits& limits_;
    std::size_t records_ = 0;
    bool budgetReported_ = false;
};

std::optional<std::int32_t> SectionType(const Token& label) {
    if (label.kind != TokenKind::Name) return std::nullopt;
    if (label.text == "events" || label.text == "事件") return 0;
    if (label.text == "conditions" || label.text == "条件") return 1;
    if (label.text == "actions" || label.text == "动作") return 2;
    return std::nullopt;
}

// ---- 导出 ----

bool PlainName(std::string_view name) {
    if (name.empty() || name == "true" || name == "false" || NumberStart(name, 0)) return false;
    return std::none_of(name.begin(), name.end(), Delimiter);
}

std::string NameText(std::string_view name) {
    return PlainName(name) ? std::string(name) : "`" + std::string(name) + "`";
}

std::string Quoted(std::string_view value) {
    std::string text = "\"";
    for (const char c : value) {
        switch (c) {
        case '"': text += "\\\""; break;
        case '\\': text += "\\\\"; break;
        case '\n': text += "\\n"; break;
        case '\r': text += "\\r"; break;
        case '\t': text += "\\t"; break;
        default: text += c;
        }
    }
    return text + "\"";
}

class Renderer {
public:
    Renderer(const TriggerDocument& document, const UiCatalog& catalog, const TriggerRecord& trigger,
        const MapObjectReferenceSnapshot* mapObjects = nullptr)
        : document_(document), catalog_(catalog), mapObjects_(mapObjects), types_(catalog), locals_(types_.CollectLocals(document, trigger)) {}

    std::string error;
    std::vector<std::pair<StableId, std::size_t>> offsets; // (节点, 在输出中的字节位置)

    bool Eca(std::uint32_t index, std::size_t indent, std::string& out) {
        if (index >= document_.ecaNodes.size()) return Fail("ECA 索引无效。");
        const auto& eca = document_.ecaNodes[index];
        if (eca.type < 0 || eca.type > 2) return Fail("语句位置出现了非事件/条件/动作节点：" + eca.name.text);
        if (!Name(eca.name.text)) return false;
        out.append(indent * 4, ' ');
        if (eca.enabled == 0) out += '~';
        if (!Call(eca, out)) return false;
        if (eca.children.empty()) { out += '\n'; return true; }
        out += " {\n";
        const auto* branches = FindTriggerBlockBranches(eca.name.text, &catalog_);
        std::map<std::int32_t, std::vector<std::uint32_t>> groups;
        for (const auto child : eca.children) {
            if (child >= document_.ecaNodes.size()) return Fail("子节点索引无效。");
            groups[document_.ecaNodes[child].childId.value_or(0)].push_back(child);
        }
        const auto label = [&](std::int32_t branch) -> std::string {
            if (branches != nullptr)
                for (const auto& spec : *branches) if (spec.branch == branch) return spec.key;
            return "#" + std::to_string(branch);
        };
        std::vector<std::int32_t> order;
        if (branches != nullptr) for (const auto& spec : *branches) if (groups.contains(spec.branch)) order.push_back(spec.branch);
        for (const auto& [branch, children] : groups)
            if (std::find(order.begin(), order.end(), branch) == order.end()) order.push_back(branch);
        for (const auto branch : order) {
            out.append((indent + 1) * 4, ' ');
            out += label(branch) + ":\n";
            for (const auto child : groups[branch]) if (!Eca(child, indent + 2, out)) return false;
        }
        out.append(indent * 4, ' ');
        out += "}\n";
        return true;
    }

private:
    bool Fail(std::string message) { if (error.empty()) error = std::move(message); return false; }
    bool Name(std::string_view name) {
        if (name.find('`') != std::string_view::npos || name.find('\n') != std::string_view::npos || name.empty())
            return Fail("名字无法写成紧凑写法：" + std::string(name));
        return true;
    }
    bool Call(const EcaNode& eca, std::string& out) {
        offsets.emplace_back(eca.id, out.size());
        out += NameText(eca.name.text) + "(";
        for (std::size_t i = 0; i < eca.arguments.size(); ++i) {
            if (i != 0) out += ", ";
            if (eca.arguments[i] >= document_.argumentNodes.size()) return Fail("参数索引无效。");
            if (!Argument(document_.argumentNodes[eca.arguments[i]], types_.ExpectedType(document_, eca, i, locals_), out)) {
                if (!located_) { error += "（" + eca.name.text + " 第 " + std::to_string(i + 1) + " 个参数）"; located_ = true; }
                return false;
            }
        }
        out += ")";
        return true;
    }
    bool Argument(const ArgumentNode& argument, const std::string& expected, std::string& out) {
        const auto& value = argument.value.text;
        if (argument.indexArgument && argument.type != 1) return Fail("非变量参数带了数组下标。");
        switch (argument.type) {
        case 0:
            if (!Name(value)) return false;
            out += NameText(value);
            return true;
        case 1: {
            if (!Name(value)) return false;
            const bool plain = !value.starts_with("udg_") && !value.starts_with("gg_") && std::any_of(document_.variables.begin(),
                document_.variables.end(), [&](const VariableRecord& row) { return row.name.text == value; });
            if (const auto name = !argument.indexArgument ? DisplayName(value, expected) : std::nullopt) {
                out += NameText(*name);
                return true;
            }
            out += NameText(plain ? "udg_" + value : value);
            if (argument.indexArgument) {
                if (*argument.indexArgument >= document_.argumentNodes.size()) return Fail("数组下标索引无效。");
                out += "[";
                if (!Argument(document_.argumentNodes[*argument.indexArgument], "integer", out)) return false;
                out += "]";
            }
            return true;
        }
        case 2:
            if (!argument.callEca || *argument.callEca >= document_.ecaNodes.size()) return Fail("函数参数缺少调用节点：" + value);
            if (!document_.ecaNodes[*argument.callEca].children.empty()) return Fail("参数函数带了子分支：" + value);
            if (!Name(document_.ecaNodes[*argument.callEca].name.text)) return false;
            return Call(document_.ecaNodes[*argument.callEca], out);
        case 3:
            out += Literal(value, expected);
            return true;
        case -1:
            if (!value.empty()) return Fail("未设置的参数带了值：" + value);
            out += '?';
            return true;
        default:
            return Fail("未知参数来源 " + std::to_string(argument.type) + "，值“" + value + "”");
        }
    }
    // 触发器、区域、镜头、声音参数导出为显示名（比 gg_ 生成名可读）；只在解析能唯一换回同一生成名时这样写，
    // 同一生成名对应多个对象（撞名）时保留生成名。
    std::optional<std::string> DisplayName(const std::string& symbol, const std::string& expected) const {
        std::string name;
        if (expected == "trigger" && symbol.starts_with("gg_trg_")) {
            const auto* found = TriggerDirectoryEditor::FindTriggerBySymbol(document_, symbol);
            if (!found || std::count_if(document_.triggers.begin(), document_.triggers.end(),
                    [&](const TriggerRecord& trigger) { return trigger.type == 0 && trigger.name.text == found->name.text; }) != 1)
                return std::nullopt;
            name = found->name.text;
        } else if (mapObjects_ && ((expected == "rect" && symbol.starts_with("gg_rct_"))
                || (expected == "camerasetup" && symbol.starts_with("gg_cam_")) || (expected == "sound" && symbol.starts_with("gg_snd_")))) {
            const auto references = mapObjects_->References();
            auto at = std::lower_bound(references.begin(), references.end(), symbol,
                [](const MapObjectReference& entry, const std::string& key) { return entry.symbol < key; });
            if (at == references.end() || at->symbol != symbol || at->name.empty() || at->name == symbol
                || (at + 1 != references.end() && (at + 1)->symbol == symbol)) return std::nullopt;
            name = at->name;
        } else return std::nullopt;
        if (name.empty() || name.find_first_of("`\r\n") != std::string::npos || name == "true" || name == "false"
            || name.starts_with("udg_") || name.starts_with("gg_") || FindPreset(catalog_, name).has_value()
            || std::any_of(document_.variables.begin(), document_.variables.end(),
                   [&](const VariableRecord& row) { return row.name.text == name; }))
            return std::nullopt;
        return name;
    }
    std::string Literal(const std::string& value, const std::string& expected) const {
        using Kind = TriggerTypeSystem::Literal;
        switch (types_.LiteralKind(expected)) {
        case Kind::Integer: if (TriggerTypeSystem::IsIntegerLiteral(value)) return value; break;
        case Kind::Real: if (TriggerTypeSystem::IsRealLiteral(value)) return value; break;
        case Kind::Boolean: if (value == "true" || value == "false") return value; break;
        case Kind::ObjectCode: if (TriggerTypeSystem::IsObjectCodeLiteral(value)) return "'" + value + "'"; break;
        default: break;
        }
        return Quoted(value);
    }

    const TriggerDocument& document_;
    const UiCatalog& catalog_;
    const MapObjectReferenceSnapshot* mapObjects_ = nullptr;
    TriggerTypeSystem types_;
    std::vector<LocalVariable> locals_;
    bool located_ = false;
};

} // namespace

Result<std::vector<std::shared_ptr<const GraphEcaValue>>> ParseTriggerText(std::string_view text,
    const TriggerDocument& document, const UiCatalog& catalog, const TriggerTextTarget& target,
    const TriggerGraphEditLimits& limits) {
    using Output = Result<std::vector<std::shared_ptr<const GraphEcaValue>>>;
    if (text.size() > limits.maxRequestTextBytes) return Output::Failure(kDomain, "TRIGGER_TEXT_BUDGET", "文本超过单批上限，请分批写入。");
    auto tokens = Lexer(text).Run();
    if (!tokens.Succeeded()) return {std::nullopt, std::move(tokens.diagnostics)};
    auto sections = Parser(std::move(*tokens.value), limits.maxDepth).Run();
    if (!sections.Succeeded()) return {std::nullopt, std::move(sections.diagnostics)};

    Resolver resolver(document, catalog, limits);
    std::vector<std::shared_ptr<const GraphEcaValue>> roots;
    const std::vector<TriggerBlockBranch>* branches = nullptr;
    const TriggerBlockBranch* branch = nullptr;
    if (!target.blockFunction.empty()) {
        branches = FindTriggerBlockBranches(target.blockFunction, &catalog);
        if (branches != nullptr)
            for (const auto& spec : *branches) if (spec.branch == target.childBranch) branch = &spec;
        if (branch == nullptr)
            return Output::Failure(kDomain, "TRIGGER_TEXT_BRANCH", target.blockFunction + " 没有分支号 " + std::to_string(target.childBranch) + "。");
    }
    for (const auto& section : *sections.value) {
        std::int32_t type = 2;
        if (branch != nullptr) {
            if (section.label) {
                return Output::Failure(kDomain, "TRIGGER_TEXT_BRANCH",
                    Where(section.label->at) + "：插入到块语句分支时不能再写分支标签。");
            }
            type = branch->type;
        } else if (section.label) {
            const auto sectionType = SectionType(*section.label);
            if (!sectionType) {
                return Output::Failure(kDomain, "TRIGGER_TEXT_BRANCH", Where(section.label->at) + "：触发器根只有 events: / conditions: / actions: 三段，没有“"
                    + section.label->text + "”。");
            }
            type = *sectionType;
        }
        for (const auto& statement : section.statements)
            roots.push_back(resolver.Ecas(statement, type, branch ? std::optional(branch->branch) : std::nullopt, 0));
    }
    if (!resolver.errors.empty()) return {std::nullopt, std::move(resolver.errors)};
    return Output::Success(std::move(roots));
}

Result<std::string> ParseTriggerTextValue(std::string_view text) {
    auto tokens = Lexer(text).Run();
    if (!tokens.Succeeded()) return {std::nullopt, std::move(tokens.diagnostics)};
    const auto& list = *tokens.value;
    if (list.size() != 2 || list[1].kind != TokenKind::End
        || (list[0].kind != TokenKind::String && list[0].kind != TokenKind::Code && list[0].kind != TokenKind::Number
            && list[0].kind != TokenKind::Name))
        return Result<std::string>::Failure(kDomain, "TRIGGER_TEXT_VALUE", "这里只能写一个值（\"文本\"、'A000'、数字或名字）：" + std::string(text));
    return Result<std::string>::Success(list[0].text);
}

std::string TriggerTextQuoted(std::string_view value) { return Quoted(value); }
std::string TriggerTextName(std::string_view name) { return NameText(name); }

Result<std::string> RenderTriggerText(const TriggerDocument& document, const UiCatalog& catalog,
    StableId triggerId, std::size_t maxBytes, std::vector<TriggerTextAnchor>* anchors, const MapObjectReferenceSnapshot* mapObjects) {
    const auto trigger = std::find_if(document.triggers.begin(), document.triggers.end(),
        [triggerId](const TriggerRecord& row) { return row.id == triggerId; });
    if (trigger == document.triggers.end())
        return Result<std::string>::Failure(kDomain, "TRIGGER_TEXT_TRIGGER_MISSING", "找不到触发器 " + std::to_string(triggerId) + "。");
    if (trigger->customTextFlag != 0)
        return Result<std::string>::Failure(kDomain, "TRIGGER_TEXT_CUSTOM_CODE", "自定义代码触发器没有事件/条件/动作树。");
    Renderer renderer(document, catalog, *trigger, mapObjects);
    static constexpr std::array<const char*, 3> kSections{"events", "conditions", "actions"};
    std::string out;
    for (std::int32_t type = 0; type < 3; ++type) {
        bool any = false;
        for (const auto index : trigger->ecas) {
            if (index >= document.ecaNodes.size() || document.ecaNodes[index].type != type) continue;
            if (!any) { out += kSections[static_cast<std::size_t>(type)]; out += ":\n"; any = true; }
            if (!renderer.Eca(index, 1, out))
                return Result<std::string>::Failure(kDomain, "TRIGGER_TEXT_RENDER", "触发器“" + trigger->name.text + "”无法如实导出：" + renderer.error);
            if (out.size() > maxBytes)
                return Result<std::string>::Failure(kDomain, "TRIGGER_TEXT_TOO_LARGE", "导出文本超过 " + std::to_string(maxBytes) + " 字节上限。");
        }
    }
    for (const auto index : trigger->ecas) {
        if (index >= document.ecaNodes.size() || document.ecaNodes[index].type < 0 || document.ecaNodes[index].type > 2)
            return Result<std::string>::Failure(kDomain, "TRIGGER_TEXT_RENDER", "触发器“" + trigger->name.text + "”的根节点类型无效。");
    }
    if (anchors != nullptr) {
        anchors->clear();
        anchors->reserve(renderer.offsets.size());
        Position at;
        std::size_t scanned = 0;
        for (const auto& [eca, offset] : renderer.offsets) {
            for (; scanned < offset; ++scanned) {
                const auto byte = static_cast<unsigned char>(out[scanned]);
                if (byte == '\n') { ++at.line; at.column = 1; }
                else if ((byte & 0xC0U) != 0x80U) ++at.column;
            }
            anchors->push_back({eca, at.line, at.column});
        }
    }
    return Result<std::string>::Success(std::move(out));
}

} // namespace War3Map::Trigger
