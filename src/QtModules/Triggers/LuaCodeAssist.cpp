#include "LuaCodeAssist.h"

#include "War3MapCore/Lua/LuaCodeModel.h"

#include <QPalette>
#include <QWidget>

namespace War3Map::Modules {
namespace {
UiKit::CodeCompletionKind Kind(Lua::CompletionKind kind) {
    switch (kind) {
    case Lua::CompletionKind::Keyword: return UiKit::CodeCompletionKind::Keyword;
    case Lua::CompletionKind::Module: return UiKit::CodeCompletionKind::Module;
    case Lua::CompletionKind::Function: return UiKit::CodeCompletionKind::Function;
    case Lua::CompletionKind::Method: return UiKit::CodeCompletionKind::Method;
    case Lua::CompletionKind::Field: return UiKit::CodeCompletionKind::Field;
    case Lua::CompletionKind::Variable: return UiKit::CodeCompletionKind::Variable;
    case Lua::CompletionKind::Constant: return UiKit::CodeCompletionKind::Constant;
    case Lua::CompletionKind::Local: return UiKit::CodeCompletionKind::Local;
    case Lua::CompletionKind::Parameter: return UiKit::CodeCompletionKind::Parameter;
    }
    return UiKit::CodeCompletionKind::Text;
}
QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

// 同一正文的 UTF-8 副本与位置换算；编辑器每次传入的都是当前全文。
struct Utf8Text {
    QByteArray bytes;
    explicit Utf8Text(const QString& text) : bytes(text.toUtf8()) {}
    [[nodiscard]] std::string_view View() const { return {bytes.constData(), static_cast<std::size_t>(bytes.size())}; }
    static std::size_t Offset(const QString& text, int position) {
        return static_cast<std::size_t>(QStringView(text).left(qBound(0, position, static_cast<int>(text.size()))).toUtf8().size());
    }
    [[nodiscard]] int Position(std::size_t offset) const {
        return static_cast<int>(QString::fromUtf8(bytes.constData(), static_cast<qsizetype>((std::min)(offset, static_cast<std::size_t>(bytes.size())))).size());
    }
};

struct LuaAssistState {
    std::shared_ptr<Lua::CodeIntel> intel;
    QHash<QString, QString> notes;
    QPointer<QWidget> style;
    QString Documentation(const QString& name, const std::string& documentation) const {
        auto text = Text(documentation);
        if (const auto note = notes.value(name); !note.isEmpty() && !text.contains(note))
            text = text.isEmpty() ? note : text + QStringLiteral("\n\n") + note;
        return text;
    }
};
} // namespace

std::string LuaModuleName(const QString& path) {
    if (!path.endsWith(QStringLiteral(".lua"), Qt::CaseInsensitive)) return {};
    auto name = path.left(path.size() - 4);
    name.replace('\\', '.').replace('/', '.');
    return name.toStdString();
}

UiKit::CodeAssist MakeLuaCodeAssist(std::shared_ptr<const Lua::ApiCatalog> catalog,
    std::shared_ptr<const LuaModuleTexts> modules, std::string currentModule,
    QHash<QString, QString> notes, QPointer<QWidget> style) {
    if (!modules) modules = std::make_shared<const LuaModuleTexts>();
    Lua::ModuleSources sources{
        [modules] { std::vector<std::string> names; for (const auto& [name, text] : *modules) names.push_back(name); return names; },
        [modules](std::string_view name) -> std::optional<std::string> {
            // 地图内路径不区分大小写。
            for (const auto& [module, text] : *modules)
                if (module.size() == name.size() && std::equal(module.begin(), module.end(), name.begin(),
                    [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
                    return text;
            return std::nullopt;
        }};
    auto state = std::make_shared<LuaAssistState>();
    state->intel = std::make_shared<Lua::CodeIntel>(std::move(catalog), std::move(sources), std::move(currentModule));
    state->notes = std::move(notes);
    state->style = std::move(style);

    UiKit::CodeAssist assist;
    assist.complete = [state](const QString& text, int position) {
        const Utf8Text utf8(text);
        const auto list = state->intel->Complete(utf8.View(), Utf8Text::Offset(text, position));
        UiKit::CodeCompletionList result;
        result.replaceStart = utf8.Position(list.replaceBegin);
        result.member = list.member;
        result.path = list.moduleName;
        result.items.reserve(static_cast<qsizetype>(list.items.size()));
        for (const auto& item : list.items) {
            const auto label = Text(item.label);
            result.items.push_back({label, Text(item.detail), state->Documentation(label, item.documentation), Kind(item.kind)});
        }
        return result;
    };
    assist.signature = [state](const QString& text, int position) -> std::optional<UiKit::CodeSignatureHelp> {
        const Utf8Text utf8(text);
        const auto help = state->intel->Signature(utf8.View(), Utf8Text::Offset(text, position));
        if (!help) return std::nullopt;
        UiKit::CodeSignatureHelp result;
        result.label = Text(help->label);
        const auto prefix = [&](std::size_t bytes) {
            return static_cast<int>(QString::fromUtf8(help->label.data(), static_cast<qsizetype>((std::min)(bytes, help->label.size()))).size());
        };
        for (const auto& [begin, end] : help->parameters) result.parameters.push_back({prefix(begin), prefix(end)});
        result.active = help->active;
        const auto open = help->label.find('(');
        const auto name = Text(open == std::string::npos ? help->label : help->label.substr(0, open)).section('.', -1).section(':', -1);
        result.documentation = state->Documentation(name, help->documentation);
        return result;
    };
    assist.hover = [state](const QString& text, int position) -> QString {
        const Utf8Text utf8(text);
        const auto hover = state->intel->Hover(utf8.View(), Utf8Text::Offset(text, position));
        if (!hover || hover->signature.empty()) return {};
        const auto muted = state->style ? state->style->palette().color(QPalette::PlaceholderText).name() : QStringLiteral("#8b949e");
        const auto signature = Text(hover->signature);
        const auto open = signature.indexOf('(');
        const auto name = (open < 0 ? signature : signature.left(open)).section(':', 0, 0).section('.', -1).trimmed();
        auto html = QStringLiteral("<p style='white-space:pre'><span style='color:%1'>%2</span> <code>%3</code></p>")
            .arg(muted, Text(hover->kind).toHtmlEscaped(), signature.toHtmlEscaped());
        if (const auto documentation = state->Documentation(name, hover->documentation); !documentation.isEmpty())
            html += QStringLiteral("<p>%1</p>").arg(documentation.toHtmlEscaped().replace('\n', QStringLiteral("<br>")));
        return html;
    };
    return assist;
}

} // namespace War3Map::Modules
