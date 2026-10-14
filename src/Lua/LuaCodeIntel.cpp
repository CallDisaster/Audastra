#include "LuaCodeModelInternal.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace War3Map::Lua {
using namespace Detail;
namespace {
constexpr std::size_t kModuleDepth = 8;

std::string ModuleKey(std::string_view name) {
    std::string key;
    for (const auto ch : name) key += ch == '/' || ch == '\\' ? '.' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return key;
}
std::string ApiSignature(const ApiMember& member, bool dropFirst = false) {
    const bool jass = member.module.starts_with("jass.");
    std::string text = member.module.empty() || jass ? member.name : member.module + "." + member.name;
    if (member.kind != ApiKind::Function) {
        const auto type = member.type.empty() ? std::string{} : ": " + member.type;
        return text + type + (member.value.empty() ? std::string{} : " = " + member.value);
    }
    text += '(';
    bool first = true;
    for (std::size_t index = dropFirst ? 1 : 0; index < member.parameters.size(); ++index) {
        if (!first) text += ", ";
        first = false;
        const auto& parameter = member.parameters[index];
        text += jass ? parameter.type + " " + parameter.name : parameter.name;
    }
    if (member.variadic) text += first ? "..." : ", ...";
    text += ')';
    if (!member.returns.empty()) text += ": " + member.returns;
    return text;
}
std::string FunctionSignature(const FunctionInfo& info, bool dropFirst = false) {
    std::string text = info.name.empty() ? std::string("function") : info.name;
    text += '(';
    bool first = true;
    for (std::size_t index = dropFirst ? 1 : 0; index < info.parameters.size(); ++index) {
        if (!first) text += ", ";
        first = false;
        text += info.parameters[index];
    }
    if (info.variadic) text += first ? "..." : ", ...";
    text += ')';
    if (const auto returns = TypeName(info.returns); !returns.empty()) text += ": " + returns;
    return text;
}
CompletionKind KindOf(const ApiMember& member) {
    switch (member.kind) {
    case ApiKind::Function: return CompletionKind::Function;
    case ApiKind::Constant: return CompletionKind::Constant;
    case ApiKind::Module: return CompletionKind::Module;
    case ApiKind::Field: return CompletionKind::Field;
    default: return CompletionKind::Variable;
    }
}
std::string KindText(const ApiMember& member) {
    if (member.module == "jass.common" && member.kind == ApiKind::Function) return "原生函数";
    switch (member.kind) {
    case ApiKind::Function: return "函数";
    case ApiKind::Constant: return "常量";
    case ApiKind::Module: return "模块";
    case ApiKind::Field: return "字段";
    default: return "变量";
    }
}
std::string TypeDetail(const Type& type) {
    if (type.function) return FunctionSignature(*type.function);
    if (type.api) return ApiSignature(*type.api);
    return TypeName(type);
}
// 光标是否在注释、数字或（非 require 的）字符串里。
const Token* Containing(const std::vector<Token>& tokens, std::size_t cursor) {
    for (const auto& token : tokens) {
        if (token.begin >= cursor) break;
        if (token.kind == TokenKind::End) break;
        const bool inside = cursor < token.end || (cursor == token.end && (token.kind == TokenKind::Comment
            || (token.kind == TokenKind::String && token.unterminated) || token.kind == TokenKind::Number));
        if (inside) return &token;
    }
    return nullptr;
}
} // namespace

struct CodeIntel::Impl {
    std::shared_ptr<const ApiCatalog> catalog;
    ModuleSources modules;
    std::string current;
    std::string cachedText;
    bool cachedWarnings = false;
    std::shared_ptr<Model> cached;
    struct Entry { std::size_t hash = 0; std::shared_ptr<Model> model; };
    std::map<std::string, Entry, std::less<>> moduleCache;
    std::set<std::string, std::less<>> resolving;
    std::optional<std::set<std::string, std::less<>>> known;
    std::vector<std::shared_ptr<Model>> sharedModels;
    std::map<std::string, Field, std::less<>> sharedGlobals; // 同名取第一个代码块

    ParseOptions Options(bool warnings) {
        ParseOptions options;
        options.catalog = catalog.get();
        options.warnings = warnings;
        options.resolve = [this](std::string_view name) { return Resolve(name); };
        if (modules.list) options.moduleExists = [this](std::string_view name) { return Known().contains(ModuleKey(name)); };
        return options;
    }
    const std::set<std::string, std::less<>>& Known() {
        if (!known) {
            known.emplace();
            if (modules.list) for (const auto& name : modules.list()) known->insert(ModuleKey(name));
        }
        return *known;
    }
    Type Resolve(std::string_view name) {
        const auto key = ModuleKey(name);
        if (!modules.read || key == ModuleKey(current) || resolving.contains(key) || resolving.size() >= kModuleDepth) return {};
        const auto text = modules.read(name);
        if (!text) return {};
        const auto hash = std::hash<std::string_view>{}(*text);
        auto& entry = moduleCache[key];
        if (!entry.model || entry.hash != hash) {
            resolving.insert(key);
            entry.model = ParseModel(*text, Options(false));
            entry.hash = hash;
            resolving.erase(key);
        }
        return entry.model->exports;
    }
    const Model& Parse(std::string_view source, bool warnings) {
        if (!cached || cachedText != source || (warnings && !cachedWarnings)) {
            known.reset(); // 模块清单可能变化，每次重新解析时重取
            cached = ParseModel(source, Options(warnings));
            cachedText = std::string(source);
            cachedWarnings = warnings;
        }
        return *cached;
    }
    // 位置 at 可见的局部量（同名取最内层）。
    static const LocalSymbol* Visible(const Model& model, std::string_view name, std::size_t at) {
        const LocalSymbol* found = nullptr;
        for (const auto& symbol : model.locals)
            if (symbol.name == name && symbol.declared <= at && at <= symbol.scopeEnd) found = &symbol;
        return found;
    }
    Type RootType(const Model& model, std::string_view name, std::size_t at) const {
        if (const auto* local = Visible(model, name, at)) return local->type;
        if (const auto found = model.globals.find(name); found != model.globals.end()) return found->second.type;
        if (const auto found = sharedGlobals.find(name); found != sharedGlobals.end()) return found->second.type;
        if (!catalog) return {};
        const auto* member = catalog->Global(name);
        if (!member) return {};
        if (member->kind == ApiKind::Module) return {TypeKind::Library, member->table, {}, {}, member};
        if (member->kind == ApiKind::Function) return {TypeKind::Function, {}, {}, {}, member};
        auto type = TypeFromName(member->type, catalog.get());
        type.api = member;
        return type;
    }
    // tokens[last] 结尾的 a.b:c 链；method 表示最后一步是冒号。
    struct Chain { Type type; bool valid = false; bool method = false; std::vector<std::size_t> names; };
    Chain Evaluate(const Model& model, std::size_t last, std::size_t at) const {
        Chain chain;
        const auto& tokens = model.tokens;
        if (last >= tokens.size() || tokens[last].kind != TokenKind::Name) return chain;
        std::vector<std::size_t> names{last};
        std::vector<bool> colons;
        auto index = last;
        while (index >= 2 && tokens[index - 2].kind == TokenKind::Name
            && (tokens[index - 1].Is(cachedText, ".") || tokens[index - 1].Is(cachedText, ":"))) {
            colons.push_back(tokens[index - 1].Is(cachedText, ":"));
            index -= 2;
            names.push_back(index);
        }
        std::reverse(names.begin(), names.end());
        std::reverse(colons.begin(), colons.end());
        chain.type = RootType(model, tokens[names.front()].Text(cachedText), at);
        for (std::size_t step = 1; step < names.size(); ++step)
            chain.type = MemberOf(chain.type, tokens[names[step]].Text(cachedText), catalog.get(), colons[step - 1]);
        chain.method = !colons.empty() && colons.back();
        chain.names = std::move(names);
        chain.valid = true;
        return chain;
    }
    std::vector<CompletionItem> Members(const Type& type, bool colon) const {
        std::vector<CompletionItem> items;
        const auto fromModule = [&](const ApiModule* module, bool methods) {
            if (!module) return;
            for (const auto& member : module->members) {
                if (methods && member.kind != ApiKind::Function) continue;
                items.push_back({member.name, methods ? CompletionKind::Method : KindOf(member),
                    ApiSignature(member, methods), member.documentation});
            }
        };
        switch (type.kind) {
        case TypeKind::Module: case TypeKind::Library:
            if (catalog && !colon) fromModule(catalog->Module(type.name), false);
            break;
        case TypeKind::String:
            if (catalog && colon) fromModule(catalog->Module("string"), true);
            break;
        case TypeKind::Table:
            if (type.shape) for (const auto& [name, field] : type.shape->fields) {
                const bool function = field.type.kind == TypeKind::Function;
                if (colon && !function) continue;
                items.push_back({name, function ? (colon ? CompletionKind::Method : CompletionKind::Function) : CompletionKind::Field,
                    function && field.type.function ? FunctionSignature(*field.type.function, colon && !field.type.function->method)
                        : TypeDetail(field.type), field.documentation.empty() && field.type.function ? field.type.function->documentation : field.documentation});
            }
            break;
        default:
            break;
        }
        return items;
    }
    std::vector<CompletionItem> Scope(const Model& model, std::size_t at) const {
        std::vector<CompletionItem> items;
        std::set<std::string, std::less<>> seen;
        const auto add = [&](CompletionItem item) {
            if (item.label.empty() || seen.contains(item.label)) return;
            seen.insert(item.label);
            items.push_back(std::move(item));
        };
        for (auto symbol = model.locals.rbegin(); symbol != model.locals.rend(); ++symbol) {
            if (symbol->declared > at || at > symbol->scopeEnd) continue;
            const bool function = symbol->type.kind == TypeKind::Function;
            add({symbol->name, function ? CompletionKind::Function : symbol->parameter ? CompletionKind::Parameter : CompletionKind::Local,
                TypeDetail(symbol->type), symbol->documentation.empty() && symbol->type.function ? symbol->type.function->documentation : symbol->documentation});
        }
        for (const auto* globals : {&model.globals, &sharedGlobals})
            for (const auto& [name, field] : *globals) {
                const bool function = field.type.kind == TypeKind::Function;
                add({name, function ? CompletionKind::Function : CompletionKind::Variable, TypeDetail(field.type),
                    field.documentation.empty() && field.type.function ? field.type.function->documentation : field.documentation});
            }
        if (catalog) for (const auto& member : catalog->globals) add({member.name, KindOf(member), ApiSignature(member), member.documentation});
        for (const auto keyword : LuaKeywords()) add({std::string(keyword), CompletionKind::Keyword, {}, {}});
        return items;
    }
    // 光标前最后一个结束于 at 之前（含）的记号。
    static std::optional<std::size_t> Before(const std::vector<Token>& tokens, std::size_t at) {
        std::optional<std::size_t> found;
        for (std::size_t index = 0; index < tokens.size() && tokens[index].kind != TokenKind::End; ++index) {
            if (tokens[index].end > at) break;
            found = index;
        }
        return found;
    }
};

CodeIntel::CodeIntel(std::shared_ptr<const ApiCatalog> catalog, ModuleSources modules, std::string currentModule)
    : impl_(std::make_unique<Impl>()) {
    impl_->catalog = std::move(catalog);
    impl_->modules = std::move(modules);
    impl_->current = std::move(currentModule);
}
CodeIntel::~CodeIntel() = default;

void CodeIntel::SetSharedSources(const std::vector<std::string>& sources) {
    impl_->sharedModels.clear();
    impl_->sharedGlobals.clear();
    for (const auto& source : sources) {
        auto model = ParseModel(source, impl_->Options(false));
        for (const auto& [name, field] : model->globals) impl_->sharedGlobals.emplace(name, field);
        impl_->sharedModels.push_back(std::move(model)); // 字段类型引用解析结果，一并保留
    }
}

CompletionList CodeIntel::Complete(std::string_view source, std::size_t cursor) {
    cursor = (std::min)(cursor, source.size());
    CompletionList list;
    list.replaceBegin = list.replaceEnd = cursor;
    const auto all = Tokenize(source, true);
    if (const auto* inside = Containing(all, cursor)) {
        if (inside->kind != TokenKind::String) return list;
        // require "…" 里补全模块名。
        const auto index = static_cast<std::size_t>(inside - all.data());
        std::size_t previous = index;
        const auto skipComments = [&] { while (previous > 0 && all[previous - 1].kind == TokenKind::Comment) --previous; };
        skipComments();
        if (previous > 0 && all[previous - 1].Is(source, "(")) { --previous; skipComments(); }
        if (previous == 0 || all[previous - 1].kind != TokenKind::Name || all[previous - 1].Text(source) != "require") return list;
        const auto text = inside->Text(source);
        if (text.empty() || (text.front() != '"' && text.front() != '\'')) return list;
        list.moduleName = true;
        list.replaceBegin = inside->begin + 1;
        list.replaceEnd = inside->unterminated ? inside->end : inside->end - 1;
        if (impl_->catalog) for (const auto& [name, module] : impl_->catalog->modules)
            if (module.requirable) list.items.push_back({name, CompletionKind::Module, name, module.documentation});
        if (impl_->modules.list) for (const auto& name : impl_->modules.list())
            if (ModuleKey(name) != ModuleKey(impl_->current)) list.items.push_back({name, CompletionKind::Module, name + ".lua", {}});
        return list;
    }
    const auto& model = impl_->Parse(source, false);
    const auto& tokens = model.tokens;
    std::size_t anchor = cursor;
    for (const auto& token : tokens) {
        if (token.kind == TokenKind::End || token.begin >= cursor) break;
        if ((token.kind == TokenKind::Name || token.kind == TokenKind::Keyword) && cursor <= token.end) {
            list.replaceBegin = token.begin; list.replaceEnd = token.end; anchor = token.begin;
        }
    }
    const auto previous = Impl::Before(tokens, anchor);
    if (previous && (tokens[*previous].Is(source, ".") || tokens[*previous].Is(source, ":"))) {
        list.member = true;
        if (*previous == 0) return list;
        const auto chain = impl_->Evaluate(model, *previous - 1, tokens[*previous - 1].begin);
        if (!chain.valid) return list;
        list.items = impl_->Members(chain.type, tokens[*previous].Is(source, ":"));
        return list;
    }
    // 正在声明新名字（local x、function f、for i）时不提示已有名字，免得接受键把新名字换掉。
    if (previous && (tokens[*previous].Is(source, "local") || tokens[*previous].Is(source, "function")
        || tokens[*previous].Is(source, "for"))) return list;
    if (previous && tokens[*previous].Is(source, "..")) {
        // 文本连接之后仍是普通表达式
    } else if (previous && tokens[*previous].kind == TokenKind::Number && tokens[*previous].end == anchor) {
        return list;
    }
    list.items = impl_->Scope(model, cursor);
    return list;
}

std::optional<SignatureHelp> CodeIntel::Signature(std::string_view source, std::size_t cursor) {
    cursor = (std::min)(cursor, source.size());
    const auto all = Tokenize(source, true);
    if (Containing(all, cursor)) return std::nullopt;
    const auto& model = impl_->Parse(source, false);
    const auto& tokens = model.tokens;
    const auto last = Impl::Before(tokens, cursor);
    if (!last) return std::nullopt;
    int depth = 0, commas = 0;
    std::optional<std::size_t> open;
    for (std::size_t steps = 0, index = *last + 1; index-- > 0 && steps < 4096; ++steps) {
        const auto& token = tokens[index];
        const auto text = token.Text(source);
        if (token.kind == TokenKind::Symbol && (text == ")" || text == "}" || text == "]")) { ++depth; continue; }
        if (token.kind == TokenKind::Symbol && (text == "(" || text == "{" || text == "[")) {
            if (depth == 0) { if (text == "(") open = index; break; }
            --depth; continue;
        }
        if (depth == 0 && token.kind == TokenKind::Symbol && text == ",") { ++commas; continue; }
        if (depth == 0 && token.kind == TokenKind::Keyword && text != "nil" && text != "true" && text != "false"
            && text != "and" && text != "or" && text != "not" && text != "function") break;
    }
    if (!open || *open == 0) return std::nullopt;
    // function f( / function a.b( / function(… 是定义，不是调用。
    for (std::size_t index = *open; index-- > 0;) {
        const auto& token = tokens[index];
        if (token.kind == TokenKind::Name || token.Is(source, ".") || token.Is(source, ":")) continue;
        if (token.Is(source, "function")) return std::nullopt;
        break;
    }
    const auto chain = impl_->Evaluate(model, *open - 1, tokens[*open - 1].begin);
    if (!chain.valid) return std::nullopt;
    SignatureHelp help;
    help.active = commas;
    std::vector<std::string> parameters;
    std::string name, returns;
    bool variadic = false;
    if (chain.type.api && chain.type.api->kind == ApiKind::Function) {
        const auto& api = *chain.type.api;
        const bool jass = api.module.starts_with("jass.");
        const bool dropFirst = chain.method && api.module == "string";
        name = api.module.empty() || jass || chain.method ? api.name : api.module + "." + api.name;
        for (std::size_t index = dropFirst ? 1 : 0; index < api.parameters.size(); ++index)
            parameters.push_back(jass ? api.parameters[index].type + " " + api.parameters[index].name : api.parameters[index].name);
        variadic = api.variadic; returns = api.returns; help.documentation = api.documentation;
    } else if (chain.type.function) {
        const auto& info = *chain.type.function;
        const bool dropFirst = chain.method && !info.method;
        name = info.name.empty() ? std::string(tokens[*open - 1].Text(source)) : info.name;
        for (std::size_t index = dropFirst ? 1 : 0; index < info.parameters.size(); ++index) parameters.push_back(info.parameters[index]);
        variadic = info.variadic; returns = TypeName(info.returns); help.documentation = info.documentation;
    } else {
        return std::nullopt;
    }
    if (variadic) parameters.push_back("...");
    help.label = name + "(";
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        if (index) help.label += ", ";
        const auto begin = help.label.size();
        help.label += parameters[index];
        help.parameters.emplace_back(begin, help.label.size());
    }
    help.label += ")";
    if (!returns.empty()) help.label += ": " + returns;
    if (variadic && help.active >= static_cast<int>(parameters.size())) help.active = static_cast<int>(parameters.size()) - 1;
    return help;
}

std::optional<HoverInfo> CodeIntel::Hover(std::string_view source, std::size_t cursor) {
    cursor = (std::min)(cursor, source.size());
    const auto all = Tokenize(source, true);
    for (const auto& token : all) {
        if (token.begin > cursor || token.kind == TokenKind::End) break;
        if ((token.kind == TokenKind::Comment || token.kind == TokenKind::String) && cursor < token.end) return std::nullopt;
    }
    const auto& model = impl_->Parse(source, false);
    const auto& tokens = model.tokens;
    std::optional<std::size_t> target;
    for (std::size_t index = 0; index < tokens.size() && tokens[index].kind != TokenKind::End; ++index) {
        if (tokens[index].begin > cursor) break;
        if (tokens[index].kind == TokenKind::Name && cursor <= tokens[index].end) target = index;
    }
    if (!target) return std::nullopt;
    const auto chain = impl_->Evaluate(model, *target, tokens[*target].begin);
    if (!chain.valid) return std::nullopt;
    const auto name = tokens[*target].Text(source);
    HoverInfo info;
    const auto& type = chain.type;
    if (chain.names.size() == 1) {
        // 声明处的名字本身尚未进入作用域，按声明位置找。
        const auto declaredHere = std::find_if(model.locals.begin(), model.locals.end(),
            [&](const LocalSymbol& symbol) { return symbol.nameEnd == tokens[*target].end && symbol.name == name; });
        const auto* local = declaredHere != model.locals.end() ? &*declaredHere : Impl::Visible(model, name, tokens[*target].begin);
        if (local) {
            const auto& localType = local->type;
            info.kind = local->parameter ? "参数" : localType.kind == TypeKind::Function ? "局部函数" : "局部变量";
            const auto typeName = TypeName(localType);
            info.signature = localType.function ? FunctionSignature(*localType.function)
                : std::string(name) + (typeName.empty() ? std::string{} : ": " + typeName);
            info.documentation = !local->documentation.empty() ? local->documentation
                : localType.function ? localType.function->documentation : std::string{};
            return info;
        }
    }
    if (type.api) {
        info.kind = KindText(*type.api);
        info.signature = ApiSignature(*type.api);
        if (type.api->module.starts_with("jass.")) info.signature = type.api->module + "." + info.signature;
        info.documentation = type.api->documentation;
        return info;
    }
    if (type.kind == TypeKind::Module && impl_->catalog) {
        info.kind = "模块";
        info.signature = type.name;
        if (const auto* module = impl_->catalog->Module(type.name)) info.documentation = module->documentation;
        return info;
    }
    if (type.function) {
        info.kind = "函数";
        info.signature = FunctionSignature(*type.function);
        info.documentation = type.function->documentation;
        return info;
    }
    // 表字段（本文件或 require 的模块）上的文档。
    if (chain.names.size() > 1) {
        auto owner = impl_->RootType(model, tokens[chain.names.front()].Text(source), tokens[chain.names.front()].begin);
        for (std::size_t step = 1; step + 1 < chain.names.size(); ++step)
            owner = MemberOf(owner, tokens[chain.names[step]].Text(source), impl_->catalog.get());
        if (owner.kind == TypeKind::Table && owner.shape) {
            if (const auto found = owner.shape->fields.find(name); found != owner.shape->fields.end()) {
                info.kind = "字段";
                const auto typeName = TypeName(type);
                info.signature = std::string(name) + (typeName.empty() ? std::string{} : ": " + typeName);
                info.documentation = found->second.documentation;
                return info;
            }
        }
    }
    const Field* global = nullptr;
    if (const auto found = model.globals.find(name); found != model.globals.end()) global = &found->second;
    else if (const auto shared = impl_->sharedGlobals.find(name); shared != impl_->sharedGlobals.end()) global = &shared->second;
    if (global) {
        info.kind = "全局变量";
        const auto typeName = TypeName(type);
        info.signature = std::string(name) + (typeName.empty() ? std::string{} : ": " + typeName);
        info.documentation = global->documentation;
        return info;
    }
    if (type.kind == TypeKind::Table || type.kind == TypeKind::Jass) {
        info.kind = "值";
        info.signature = std::string(name) + ": " + TypeName(type);
        return info;
    }
    return std::nullopt;
}

std::vector<CodeWarning> CodeIntel::Check(std::string_view source) {
    return impl_->Parse(source, true).warnings;
}

} // namespace War3Map::Lua
