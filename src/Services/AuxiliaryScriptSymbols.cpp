#include "War3MapCore/Services/AuxiliaryEditing.h"
#include "War3MapCore/Jass/ByteLexical.h"
#include "War3MapCore/Text/TextUtil.h"
#include "../Jass/JassLexer.h"
#include "../Lua/LuaLexer.h"

#include <algorithm>
#include <stdexcept>

namespace War3Map::Services {
Status CheckAuxiliaryCreatedScriptSymbol(const AuxiliaryReferenceContext& context, std::string_view symbol) {
    Status result;
    const auto note = [&](std::string code, std::string text) {
        result.diagnostics.push_back({DiagnosticSeverity::Info, "MapData.Auxiliary", std::move(code), std::move(text)});
    };
    const auto unknown = [&](const std::string& name, std::string reason) {
        auto failure = Status::Failure("MapData.Auxiliary", "AUX_SCRIPT_REFERENCES_UNKNOWN",
            "不能核对新增符号 " + std::string(symbol) + "：" + name + "（" + reason + "）。删除和更名的限制保持不变。");
        failure.diagnostics.insert(failure.diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
        return failure;
    };
    std::size_t total = 0;
    if (context.opaqueScriptSources.size() > 4096) return unknown("脚本来源", "超过 4096 项预算");
    for (const auto& name : context.opaqueScriptSources) {
        auto lower = Text::LowerAscii(name);
        std::replace(lower.begin(), lower.end(), '\\', '/');
        if (lower.ends_with(".ai")) {
            // 独立脚本环境不共享地图全局；此分支必须先于任何正文读取。
            note("AUX_SCRIPT_AI_SEPARATE_GLOBALS", name + "：独立 AI 全局环境，不读取正文，不参与新增地图全局符号检查。");
            continue;
        }
        if (!lower.ends_with(".j") && !lower.ends_with(".lua"))
            return unknown(name, "未证明该来源不参与脚本运行");
        if (!context.readScript) return unknown(name, "宿主未提供同源脚本文本");
        auto read = context.readScript(name);
        if (!read.Succeeded() || !read.value) return unknown(name, "读取失败或来源已变化");
        const auto& bytes = *read.value;
        if (bytes.size() > 2U * 1024U * 1024U || bytes.size() > 16U * 1024U * 1024U - total)
            return unknown(name, "单来源 2 MiB 或总计 16 MiB 预算超限");
        total += bytes.size();
        const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        // 无编码声明的导入只接受严格 UTF-8（包含 ASCII），不猜旧代码页。
        for (std::uint32_t at = 0; at < text.size();) {
            const auto scalar = Text::ReadSourceScalar(text, at, Text::SourceEncoding::Utf8);
            if (!scalar.Succeeded() || Jass::IsSourceControlByte(text[at])) return unknown(name, "编码不明或包含控制字节");
            at += scalar.value->bytes;
        }
        bool conflict = false, literal = false, comment = false, incomplete = false;
        try {
            if (lower.ends_with(".lua")) {
                const auto tokens = Lua::Detail::Tokenize(text, true, text.size() + 1);
                std::size_t end = 0;
                for (const auto& token : tokens) {
                    for (auto at = end; at < token.begin; ++at) incomplete |= static_cast<unsigned char>(text[at]) >= 128;
                    end = token.end;
                    const auto value = token.Text(text);
                    incomplete |= token.unterminated;
                    conflict |= token.kind == Lua::Detail::TokenKind::Name && value == symbol;
                    literal |= token.kind == Lua::Detail::TokenKind::String && value.find(symbol) != value.npos;
                    comment |= token.kind == Lua::Detail::TokenKind::Comment && value.find(symbol) != value.npos;
                }
            } else {
                Jass::detail::LexOptions options; options.blockComments = true;
                options.directiveComments = true; options.vjassOperators = true;
                const auto tokens = Jass::detail::LexJassBounded(text, Text::SourceEncoding::Utf8, options, text.size() + 1);
                if (!tokens.Succeeded()) return unknown(name, "JASS 词法检查未完成");
                for (const auto& token : *tokens.value) {
                    incomplete |= token.kind == Jass::detail::TokenKind::Invalid || token.kind == Jass::detail::TokenKind::Pragma;
                    conflict |= token.kind == Jass::detail::TokenKind::Identifier && token.text == symbol;
                    literal |= token.kind == Jass::detail::TokenKind::String && token.text.find(symbol) != token.text.npos;
                }
                for (std::size_t at = 0; at < text.size();) {
                    if (auto opaque = Jass::ScanByteOpaque(text, at)) {
                        if (text.substr(at).starts_with("//") || text.substr(at).starts_with("/*"))
                            comment |= text.substr(at, opaque->end - at).find(symbol) != text.npos;
                        incomplete |= !opaque->closed;
                        at = opaque->end;
                    } else ++at;
                }
            }
        } catch (const std::length_error&) { return unknown(name, "词法预算超限"); }
        if (conflict) {
            auto failure = Status::Failure("MapData.Auxiliary", "AUX_SCRIPT_SYMBOL_CONFLICT",
                name + " 中已有同名标识符 " + std::string(symbol) + "。");
            failure.diagnostics.insert(failure.diagnostics.end(), result.diagnostics.begin(), result.diagnostics.end());
            return failure;
        }
        if (incomplete) return unknown(name, "存在未闭合文本、执行字节或未展开指令");
        note("AUX_SCRIPT_SYMBOL_CHECKED", name + "：标识符已检查，无同名标识符。"
            + (literal ? "字符串中出现该名称。" : "") + (comment ? "注释中出现该名称。" : ""));
    }
    return result;
}
} // namespace War3Map::Services
