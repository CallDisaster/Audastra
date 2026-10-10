#include "VJassTranslate.h"

#include <algorithm>
#include <functional>
#include <numeric>

// vJass 翻译：名称改写、符号表（游戏脚本与地图）、库依赖排序、结构继承与编号、输出位置。
namespace War3Map::Jass::detail {

Translation::Translation(VProgram& program, const VJassOptions& options, VJassResult& result, std::string name, CheckBudget& budget)
    : program_(program), options_(options), result_(result), name_(std::move(name)), text_(program.text), budget_(budget),
      textBudget_{budget, options.check.maxTotalBytes}, trace_(program.text, name_, textBudget_) {
}

bool Translation::Live(std::int32_t library) const noexcept {
    return library < 0 || !program_.libraries[static_cast<std::size_t>(library)].duplicate;
}

bool Translation::Prepare() {
    for (const auto& error : program_.errors) Error(error.line, "VJASS_SYNTAX", error.message);
    for (const auto& warning : program_.warnings) Warning(warning.line, "VJASS_IGNORED", warning.message);
    if (Failed()) return false;
    AssignNames();
    AddBaseScripts();
    BuildSymbols();
    if (!OrderLibraries() || !LinkStructs()) return false;
    AssignOrder();
    ParseBodies();
    return !Failed();
}

void Translation::AssignNames() {
    libraryNames_.resize(program_.libraries.size());
    for (const auto& scoped : program_.scopedNames) {
        budget_.Poll();
        const auto& library = program_.libraries[static_cast<std::size_t>(scoped.library)];
        auto qualified = ScopedFinalName(library.name, scoped.name, scoped.visibility);
        textBudget_.Consume(qualified.size());
        const auto& stored = storage_.emplace_back(std::move(qualified));
        libraryNames_[static_cast<std::size_t>(scoped.library)].emplace(scoped.name, stored);
    }
    auto libraryName = [this](std::int32_t library) {
        return library < 0 ? std::string_view{} : program_.libraries[static_cast<std::size_t>(library)].name;
    };
    for (auto& global : program_.globals) {
        budget_.Poll();
        if (global.scope >= 0) {
            global.finalName = program_.scopes[static_cast<std::size_t>(global.scope)].path + "__" + std::string(global.decl.name);
        } else {
            global.finalName = ScopedFinalName(libraryName(global.library), global.decl.name, global.visibility);
        }
        textBudget_.Consume(global.finalName.size());
    }
    for (auto& data : program_.structs) {
        budget_.Poll();
        data.fullName = ScopedFinalName(libraryName(data.library), data.name, data.visibility);
        textBudget_.Consume(data.fullName.size());
    }
    for (auto& function : program_.functions) {
        budget_.Poll();
        if (function.scope >= 0) {
            function.finalName = program_.scopes[static_cast<std::size_t>(function.scope)].path + "__" + std::string(function.decl.name);
        } else {
            function.finalName = function.isMethod ? MethodName(function)
                                                   : ScopedFinalName(libraryName(function.library), function.decl.name, function.visibility);
        }
        textBudget_.Consume(function.finalName.size());
    }
    OrderScopes();
}

void Translation::AddBaseScripts() {
    for (const auto& source : options_.baseScripts) {
        budget_.Poll();
        const auto file = ParseProgram(source.text, &budget_);
        for (const auto& global : file.globals) {
            budget_.Poll();
            globals_.emplace(global.name, VGlobalSymbol{VType::Jass(global.type), global.array, false});
        }
        auto add = [this](std::string_view name, const std::vector<Param>& params, std::string_view returns) {
            VFunctionSymbol symbol;
            for (const auto& param : params) symbol.params.push_back(VType::Jass(param.type));
            symbol.returns = returns.empty() ? VType::Jass("nothing") : VType::Jass(returns);
            functions_.emplace(name, std::move(symbol));
        };
        for (const auto& native : file.natives) add(native.name, native.params, native.returns);
        for (const auto& function : file.functions) add(function.name, function.params, function.returns);
    }
}

VFunctionSymbol Translation::Signature(const VFunction& function, std::int32_t index) const {
    VFunctionSymbol symbol;
    symbol.function = index;
    for (const auto& param : function.decl.params) {
        budget_.Poll();
        symbol.params.push_back(ResolveType(param.type, function.library, function.structIndex));
    }
    symbol.returns = function.decl.returns.empty() ? VType::Jass("nothing")
                                                   : ResolveType(function.decl.returns, function.library, function.structIndex);
    return symbol;
}

void Translation::BuildSymbols() {
    for (std::size_t i = 0; i < program_.structs.size(); ++i) {
        budget_.Poll();
        const auto& data = program_.structs[i];
        if (!Live(data.library)) continue;
        if (!structs_.emplace(data.fullName, static_cast<std::int32_t>(i)).second) {
            Error(data.line, "VJASS_REDECLARED", "结构 " + data.fullName + " 重复定义");
        }
    }
    for (const auto& global : program_.globals) {
        budget_.Poll();
        if (!Live(global.library)) continue;
        const auto found = globals_.find(global.finalName);
        if (found != globals_.end() && found->second.fromMap) {
            Error(global.decl.line, "VJASS_REDECLARED", "全局变量 " + global.finalName + " 重复定义");
        }
        globals_[global.finalName] = VGlobalSymbol{ResolveType(global.decl.type, global.library, -1), global.decl.array, true};
    }
    for (const auto& native : program_.nativeDecls) {
        budget_.Poll();
        VFunctionSymbol symbol;
        for (const auto& param : native.params) symbol.params.push_back(VType::Jass(param.type));
        symbol.returns = native.returns.empty() ? VType::Jass("nothing") : VType::Jass(native.returns);
        functions_[native.name] = std::move(symbol);
    }
    signatures_.resize(program_.functions.size());
    for (std::size_t i = 0; i < program_.functions.size(); ++i) {
        budget_.Poll();
        const auto& function = program_.functions[i];
        signatures_[i] = Signature(function, static_cast<std::int32_t>(i));
        if (function.isMethod || !Live(function.library)) continue;
        const auto found = functions_.find(function.finalName);
        if (found != functions_.end() && found->second.function >= 0) {
            Error(function.decl.line, "VJASS_REDECLARED", "函数 " + function.finalName + " 重复定义");
        }
        functions_[function.finalName] = signatures_[i];
        if (function.library < 0 && function.decl.name == "main") mainFunction_ = static_cast<std::int32_t>(i);
    }
}

bool Translation::OrderLibraries() {
    const auto count = program_.libraries.size();
    std::unordered_map<std::string_view, std::int32_t> byName;
    for (std::size_t i = 0; i < count; ++i) {
        budget_.Poll();
        if (Live(static_cast<std::int32_t>(i))) byName.emplace(program_.libraries[i].name, static_cast<std::int32_t>(i));
    }
    // 层级 = 最长依赖链长度；0 未计算，-1 计算中（用于发现循环依赖）。
    std::vector<std::int32_t> state(count, 0);
    bool ok = true;
    std::function<std::int32_t(std::int32_t)> level = [&](std::int32_t index) -> std::int32_t {
        NestingGuard guard(&budget_, libraryNesting_);
        auto& library = program_.libraries[static_cast<std::size_t>(index)];
        auto& mark = state[static_cast<std::size_t>(index)];
        if (mark > 0) return library.level;
        if (mark < 0) {
            Error(library.line, "VJASS_REQUIRES", "库 " + std::string(library.name) + " 存在循环依赖");
            ok = false;
            return 0;
        }
        mark = -1;
        std::int32_t result = 0;
        for (const auto& requirement : library.requirements) {
            budget_.Poll();
            const auto found = byName.find(requirement.name);
            if (found == byName.end()) {
                if (!requirement.optional) {
                    Error(requirement.line, "VJASS_REQUIRES",
                        "库 " + std::string(library.name) + " 需要的库 " + std::string(requirement.name) + " 不存在");
                    ok = false;
                }
                continue;
            }
            result = std::max(result, level(found->second) + 1);
        }
        if (static_cast<std::size_t>(result) + 1 > (std::min)(options_.check.maxNesting, std::size_t{128}))
            budget_.Fail(CheckFailure::Nesting); // 已缓存子库也不能消掉实际依赖链深度预算。
        library.level = result;
        mark = 1;
        return result;
    };
    for (const auto& [name, index] : byName) {
        budget_.Poll();
        (void)name;
        level(index);
        libraryOrder_.push_back(index);
    }
    std::sort(libraryOrder_.begin(), libraryOrder_.end(), [this](std::int32_t a, std::int32_t b) {
        const auto& left = program_.libraries[static_cast<std::size_t>(a)];
        const auto& right = program_.libraries[static_cast<std::size_t>(b)];
        if (left.level != right.level) return left.level < right.level;
        return left.name < right.name; // 字节序，与 jasshelper 一致（大写在小写之前）
    });
    // 库的初始化函数：在 main 的 InitBlizzard 之后按库顺序调用。
    for (const auto index : libraryOrder_) {
        budget_.Poll();
        const auto& library = program_.libraries[static_cast<std::size_t>(index)];
        if (library.initializer.empty()) continue;
        const auto name = ScopedName(library.initializer, index);
        const auto found = functions_.find(name);
        if (found == functions_.end() || found->second.function < 0) {
            Error(library.line, "VJASS_INITIALIZER", "库 " + std::string(library.name) + " 的初始化函数 " + std::string(library.initializer) + " 不存在");
            ok = false;
            continue;
        }
        if (!found->second.params.empty()) {
            Error(library.line, "VJASS_INITIALIZER", "库 " + std::string(library.name) + " 的初始化函数不能带参数");
            ok = false;
        }
        textBudget_.Append(mainInsertion_, "call ExecuteFunc(\"" + std::string(name) + "\")\n");
    }
    return ok;
}

void Translation::OrderScopes() {
    std::vector<std::int32_t> order(program_.scopes.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [this](std::int32_t a, std::int32_t b) {
        return program_.scopes[static_cast<std::size_t>(a)].order < program_.scopes[static_cast<std::size_t>(b)].order;
    });
    for (const auto index : order) {
        budget_.Poll();
        const auto& scope = program_.scopes[static_cast<std::size_t>(index)];
        for (const auto& item : scope.items) {
            budget_.Poll();
            if (item.kind != ItemKind::Function) continue;
            const auto& function = program_.functions[static_cast<std::size_t>(item.index)];
            if (!function.decl.params.empty() || !function.decl.returns.empty()) continue;
            if (function.decl.name == "onInit") textBudget_.Append(mainInsertion_, "call ExecuteFunc(\"" + function.finalName + "\")\n");
        }
    }
}

bool Translation::LinkStructs() {
    bool ok = true;
    for (std::size_t i = 0; i < program_.structs.size(); ++i) {
        budget_.Poll();
        auto& data = program_.structs[i];
        if (!Live(data.library) || data.parentName.empty()) continue;
        const auto parent = FindStruct(data.parentName, data.library);
        if (parent < 0) {
            Error(data.line, "VJASS_STRUCT", "结构 " + std::string(data.name) + " 的父结构 " + std::string(data.parentName) + " 不存在");
            ok = false;
            continue;
        }
        data.parent = parent;
    }
    for (std::size_t i = 0; i < program_.structs.size(); ++i) {
        budget_.Poll();
        auto& data = program_.structs[i];
        if (data.parent < 0) continue;
        const auto& parent = program_.structs[static_cast<std::size_t>(data.parent)];
        if (parent.parent >= 0) {
            Fail(data.line, "多层 extends（" + std::string(data.name) + "）");
            ok = false;
        }
        if (data.onDestroy >= 0) {
            Fail(data.line, "子结构定义 onDestroy（" + std::string(data.name) + "）");
            ok = false;
        }
        program_.structs[static_cast<std::size_t>(data.parent)].children.push_back(static_cast<std::int32_t>(i));
    }
    // 类型编号按输出顺序：各库（按库顺序）中的结构，然后是库外的结构。
    auto number = [this](const std::vector<VItem>& items) {
        for (const auto& item : items) {
            budget_.Poll();
            if (item.kind != ItemKind::Struct) continue;
            auto& data = program_.structs[static_cast<std::size_t>(item.index)];
            structOrder_.push_back(item.index);
            data.typeId = static_cast<std::int32_t>(structOrder_.size());
        }
    };
    for (const auto library : libraryOrder_) number(program_.libraries[static_cast<std::size_t>(library)].items);
    for (const auto& top : program_.top) {
        budget_.Poll();
        if (top.kind == VTopItem::Kind::Item && top.item.kind == ItemKind::Struct) {
            auto& data = program_.structs[static_cast<std::size_t>(top.item.index)];
            structOrder_.push_back(top.item.index);
            data.typeId = static_cast<std::int32_t>(structOrder_.size());
        }
    }
    for (const auto index : structOrder_) {
        budget_.Poll();
        const auto& data = program_.structs[static_cast<std::size_t>(index)];
        if (data.onDestroy >= 0 || !data.children.empty()) usesTriggers_ = true;
    }
    return ok;
}

void Translation::AssignOrder() {
    destructorOrder_.assign(program_.structs.size(), 0);
    std::uint32_t order = 1;
    auto walk = [&](const VItem& item) {
        if (item.kind == ItemKind::Function) {
            program_.functions[static_cast<std::size_t>(item.index)].order = order++;
        } else if (item.kind == ItemKind::Struct) {
            const auto& data = program_.structs[static_cast<std::size_t>(item.index)];
            for (const auto method : data.methods) {
                budget_.Poll();
                program_.functions[static_cast<std::size_t>(method)].order = order++;
                if (method == data.onDestroy) destructorOrder_[static_cast<std::size_t>(item.index)] = order++;
            }
        }
    };
    for (const auto library : libraryOrder_) {
        budget_.Poll();
        for (const auto& item : program_.libraries[static_cast<std::size_t>(library)].items) walk(item);
    }
    for (const auto& top : program_.top) {
        budget_.Poll();
        if (top.kind == VTopItem::Kind::Item) walk(top.item);
    }
}

void Translation::ParseBodies() {
    bodies_.resize(program_.functions.size());
    for (std::size_t i = 0; i < program_.functions.size(); ++i) {
        budget_.Poll();
        const auto& function = program_.functions[i];
        if (!Live(function.library)) continue;
        bodies_[i] = ParseFunctionBody(program_.tokens, function.decl, true, &budget_);
        for (const auto& error : bodies_[i].errors) Error(error.line, "VJASS_SYNTAX", error.message);
    }
}

std::string_view Translation::ScopedName(std::string_view name, std::int32_t library) const {
    if (library >= 0) {
        const auto& names = libraryNames_[static_cast<std::size_t>(library)];
        if (const auto found = names.find(name); found != names.end()) return found->second;
    }
    return name;
}

std::int32_t Translation::FindStruct(std::string_view name, std::int32_t library) const {
    const auto found = structs_.find(ScopedName(name, library));
    return found == structs_.end() ? -1 : found->second;
}

VType Translation::ResolveType(std::string_view name, std::int32_t library, std::int32_t structIndex) const {
    if (name == "thistype" && structIndex >= 0) return VType::Instance(structIndex);
    if (const auto found = FindStruct(name, library); found >= 0) return VType::Instance(found);
    return VType::Jass(name);
}

} // namespace War3Map::Jass::detail
