#include "VJassTranslate.h"

#include <algorithm>
#include <iterator>

// vJass 翻译入口与整体输出：globals（各库、脚本、结构）→ 原生函数 → 结构生成函数 → 各库代码 → 其余脚本 → 结构初始化。
namespace War3Map::Jass {

bool VJassResult::Succeeded() const noexcept {
    if (!completed || !unsupported.empty()) return false;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == DiagnosticSeverity::Error) return false;
    }
    return true;
}

std::optional<VJassByteOrigin> VJassResult::FindByteOrigin(std::size_t offset) const noexcept {
    if (offset >= text.size()) return std::nullopt;
    const auto after = std::upper_bound(byteOrigins.begin(), byteOrigins.end(), offset,
        [](std::size_t value, const VJassByteOrigin& origin) { return value < origin.outputOffset; });
    if (after == byteOrigins.begin()) return std::nullopt;
    auto origin = *std::prev(after);
    const auto delta = offset - origin.outputOffset;
    if (delta >= origin.byteLength || offset > UINT32_MAX) return std::nullopt;
    if (origin.exact) {
        if (delta > UINT32_MAX - origin.sourceOffset || delta > UINT32_MAX - origin.sourceColumn) return std::nullopt;
        origin.sourceOffset += static_cast<std::uint32_t>(delta);
        origin.sourceColumn += static_cast<std::uint32_t>(delta);
    }
    origin.outputOffset = static_cast<std::uint32_t>(offset);
    origin.byteLength -= static_cast<std::uint32_t>(delta);
    return origin;
}

namespace detail {

void Translation::Fail(std::uint32_t line, std::string reason) {
    if (!result_.unsupported.empty()) return;
    result_.unsupported = line == 0 ? std::move(reason) : "第 " + std::to_string(line) + " 行：" + std::move(reason);
}

void Translation::Error(std::uint32_t line, std::string code, std::string message) {
    Report(DiagnosticSeverity::Error, line, std::move(code), std::move(message));
}

void Translation::Warning(std::uint32_t line, std::string code, std::string message) {
    Report(DiagnosticSeverity::Warning, line, std::move(code), std::move(message));
}

void Translation::Report(DiagnosticSeverity severity, std::uint32_t line, std::string code, std::string message) {
    budget_.Poll();
    Diagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.subsystem = "Jass.VJass";
    diagnostic.code = std::move(code);
    diagnostic.message = budget_.Message(DiagnosticText(name_) + ":" + std::to_string(line) + ": " + budget_.Message(message));
    const auto offset = trace_.Offset(line);
    const auto end = trace_.Offset(line + 1);
    diagnostic.origin = OriginSpan{std::filesystem::u8path(name_), offset, end - offset, {}};
    budget_.ConsumeDiagnostic(diagnostic.message.size() + diagnostic.code.size());
    result_.diagnostics.push_back(std::move(diagnostic));
}

bool Translation::Failed() const noexcept {
    return !result_.Succeeded();
}

void Translation::AppendSource(MappedOutput& out, std::uint32_t begin, std::uint32_t end) const {
    if (end > begin) out.Original(begin, end);
}

void Translation::EmitGlobal(MappedOutput& out, const VGlobal& global) {
    out.AnchorLine(global.decl.line);
    CodeScope scope;
    scope.pool = &program_.pool;
    scope.library = global.library;
    if (global.decl.constant) out += "constant ";
    out += OutputType(ResolveType(global.decl.type, global.library, -1));
    out += global.decl.array ? " array " : " ";
    out += global.finalName;
    if (global.decl.init >= 0) {
        const Rendered init = Render(global.decl.init, scope);
        out += "= ";
        const auto& expr = program_.pool.At(global.decl.init);
        if (init.changed) out.Tracked(init.text, init.byteOrigins, expr.begin);
        else out.Original(expr.begin, expr.end);
    }
    out += '\n';
}

void Translation::EmitScope(MappedOutput& out, std::int32_t index) {
    const auto& scope = program_.scopes[static_cast<std::size_t>(index)];
    out.Anchor(scope.header.begin);
    out += "//scope " + scope.path + ":\n";
    std::uint32_t at = scope.header.end;
    for (const auto& item : scope.items) {
        AppendSource(out, at, item.range.begin);
        if (item.kind == ItemKind::Keyword && item.index >= 0) EmitScope(out, item.index);
        else EmitItem(out, item);
        at = item.range.end;
    }
    AppendSource(out, at, scope.footer.begin);
    out.Anchor(scope.footer.begin);
    out += "//scope " + scope.path + " ends\n";
}

void Translation::EmitItem(MappedOutput& out, const VItem& item) {
    budget_.Poll();
    switch (item.kind) {
    case ItemKind::Function: EmitFunction(out, item.index); break;
    case ItemKind::Struct: EmitStruct(out, item.index); break;
    default: break; // globals、native 已移到开头；keyword 不输出
    }
}

void Translation::EmitItems(MappedOutput& out, const std::vector<VItem>& items, std::uint32_t begin, std::uint32_t end) {
    std::uint32_t at = begin;
    for (const auto& item : items) {
        AppendSource(out, at, item.range.begin);
        EmitItem(out, item);
        at = item.range.end;
    }
    AppendSource(out, at, end);
    if (!out.empty() && out.back() != '\n') out += '\n';
}

MappedText Translation::Emit() {
    MappedOutput out(trace_);
    out.reserve((std::min)(text_.size(), textBudget_.limit));
    std::uint32_t suffix = options_.initSuffix;
    if (suffix == 0) {
        std::uint32_t hash = 2166136261u; // FNV-1a：同一输入得到同一名称
        for (std::size_t i = 0; i < text_.size(); ++i) {
            budget_.PollOccasionally(i);
            hash = (hash ^ static_cast<unsigned char>(text_[i])) * 16777619u;
        }
        suffix = 1000000u + hash % 9000000u;
    }
    initName_ = "jasshelper__initstructs" + std::to_string(suffix);
    if (!structOrder_.empty()) mainInsertion_.insert(0, "call ExecuteFunc(\"" + initName_ + "\")\n");
    PrepareDestructors();

    std::vector<std::vector<const VGlobal*>> byLibrary(program_.libraries.size());
    std::vector<const VGlobal*> scriptGlobals;
    for (const auto& global : program_.globals) {
        if (global.library < 0 || global.scope >= 0) {
            scriptGlobals.push_back(&global);
        } else {
            byLibrary[static_cast<std::size_t>(global.library)].push_back(&global);
        }
    }
    out += "globals\n";
    bool initializers = false;
    for (const auto index : libraryOrder_) {
        const auto& library = program_.libraries[static_cast<std::size_t>(index)];
        const std::string name(library.name);
        out.Anchor(library.header.begin);
        out += "//globals from " + name + ":\nconstant boolean LIBRARY_" + name + "=true\n";
        for (const auto* global : byLibrary[static_cast<std::size_t>(index)]) EmitGlobal(out, *global);
        out.Anchor(library.footer.begin);
        out += "//endglobals from " + name + "\n";
        initializers = initializers || !library.initializer.empty();
    }
    for (const auto* global : scriptGlobals) EmitGlobal(out, *global);
    if (initializers) out += "\ntrigger l__library_init\n";
    if (!structOrder_.empty()) {
        out += "\n//JASSHelper struct globals:\n";
        EmitStructGlobals(out);
    }
    out.Anchor(0);
    out += "\nendglobals\n";
    // 原生函数：先脚本中的（按出现顺序），再各库中的（按库顺序）。
    auto emitNative = [&](const VItem& item) {
        if (item.kind != ItemKind::Native) return;
        const auto& range = program_.natives[static_cast<std::size_t>(item.index)];
        AppendSource(out, range.begin, range.end);
        if (out.back() != '\n') out += '\n';
    };
    for (const auto& top : program_.top) {
        if (top.kind == VTopItem::Kind::Item) emitNative(top.item);
    }
    for (const auto index : libraryOrder_) {
        for (const auto& item : program_.libraries[static_cast<std::size_t>(index)].items) emitNative(item);
    }
    if (!structOrder_.empty()) EmitStructFunctions(out);

    for (const auto index : libraryOrder_) {
        const auto& library = program_.libraries[static_cast<std::size_t>(index)];
        const std::string name(library.name);
        out.Anchor(library.header.begin);
        out += "//library " + name + ":\n";
        EmitItems(out, library.items, library.header.end, library.footer.begin);
        out.Anchor(library.footer.begin);
        out += "//library " + name + " ends\n";
    }
    std::uint32_t at = 0;
    for (const auto& top : program_.top) {
        AppendSource(out, at, top.item.range.begin);
        if (top.kind == VTopItem::Kind::Item) EmitItem(out, top.item);
        else if (top.library <= -2) EmitScope(out, -top.library - 2);
        at = top.item.range.end;
    }
    AppendSource(out, at, static_cast<std::uint32_t>(text_.size()));
    if (!out.empty() && out.back() != '\n') out += '\n';
    if (!structOrder_.empty()) EmitStructInitializers(out);
    if (mainFunction_ < 0 && !mainInsertion_.empty()) Fail(0, "脚本中没有 main 函数，无法插入初始化");

    result_.libraries = libraryOrder_.size();
    result_.structs = structOrder_.size();
    result_.functions = 0;
    for (const auto& function : program_.functions) {
        if (Live(function.library)) ++result_.functions;
    }
    return out.Take();
}

} // namespace detail

VJassResult TranslateVJass(std::string_view text, const std::string& name, const VJassOptions& options) {
    VJassResult result;
    detail::CheckBudget budget(options.check);
    try {
    budget.Poll();
    if (name.size() > 4096) budget.Fail(detail::CheckFailure::SourceBytes);
    if (options.debug) {
        result.unsupported = "调试模式（debug）";
        return result;
    }
    auto program = detail::ScanVJass(text, &budget);
    if (!program.unsupported.empty()) {
        result.unsupported = program.unsupported;
        return result;
    }
    detail::Translation translation(program, options, result, name, budget);
    if (!translation.Prepare()) return result;
    auto output = translation.Emit();
    budget.Poll();
    if (result.Succeeded()) {
        result.text = std::move(output.text);
        result.sourceMap = std::move(output.sourceMap);
        result.byteOrigins = std::move(output.byteOrigins);
    }
    } catch (const detail::CheckAborted& aborted) {
        budget.MarkFailure(aborted.Cause());
        result.completed = false;
    } catch (const std::bad_alloc&) {
        budget.MarkFailure(detail::CheckFailure::Allocation);
        result.completed = false;
    } catch (...) {
        budget.MarkFailure(detail::CheckFailure::Worker);
        result.completed = false;
    }
    result.tokens = budget.Tokens();
    result.astNodes = budget.AstNodes();
    result.maxAstDepthObserved = budget.MaxDepth();
    if (!result.completed) {
        result.text.clear();
        result.sourceMap = {};
        result.byteOrigins.clear();
        result.unsupported.clear(); // 硬失败不能触发外部编译器回退绕过预算。
        auto lexical = budget.TakeLexicalDiagnostics();
        for (auto& diagnostic : lexical) result.diagnostics.push_back(std::move(diagnostic));
        const auto cause = budget.Failure();
        const std::string code = cause == detail::CheckFailure::Cancelled ? "JASS_CHECK_CANCELLED"
            : cause == detail::CheckFailure::Deadline ? "JASS_CHECK_DEADLINE" : "VJASS_RESOURCE_BUDGET";
        result.diagnostics.push_back({DiagnosticSeverity::Error, "Jass.VJass", code,
            "vJass 展开未完成，硬预算/取消/内部失败类型=" + std::to_string(static_cast<unsigned>(cause)), std::nullopt});
    }
    return result;
}

} // namespace War3Map::Jass
