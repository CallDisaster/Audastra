#include "War3MapCore/GameUi/FrameLibrary.h"

#include "War3MapCore/Text/TextUtil.h"

#include <utility>

namespace War3Map::GameUi {
namespace {

constexpr const char* kSubsystem = "GameUi";
// 嵌套 IncludeFile 的上限，防止互相包含的文件无限展开。
constexpr int kMaxIncludeDepth = 16;

std::string NormalizePath(std::string_view path) {
    std::string normalized = Text::LowerAscii(path);
    for (char& c : normalized) {
        if (c == '/') c = '\\';
    }
    return normalized;
}

} // namespace

std::string BytesToText(const std::vector<std::byte>& bytes) {
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    return text;
}

void FrameLibrary::LoadToc(const FrameFileReader& reader, const std::string& tocPath, const std::string& origin) {
    auto bytes = reader(tocPath);
    if (!bytes.Succeeded() || !bytes.value) {
        diagnostics_.push_back(Diagnostic{DiagnosticSeverity::Error, kSubsystem, "GAMEUI_TOC_UNREADABLE",
            "无法读取界面目录 " + tocPath, std::nullopt});
        diagnostics_.insert(diagnostics_.end(), bytes.diagnostics.begin(), bytes.diagnostics.end());
        return;
    }
    for (const auto& path : ParseToc(BytesToText(*bytes.value))) {
        LoadFile(reader, path, origin);
    }
}

void FrameLibrary::LoadFile(const FrameFileReader& reader, const std::string& fdfPath, const std::string& origin) {
    LoadFileAt(reader, fdfPath, origin, 0);
}

void FrameLibrary::LoadFileAt(const FrameFileReader& reader, const std::string& fdfPath, const std::string& origin, int depth) {
    const std::string key = NormalizePath(fdfPath);
    if (loadedPaths_.contains(key)) return;
    loadedPaths_[key] = true;
    auto bytes = reader(fdfPath);
    if (!bytes.Succeeded() || !bytes.value) {
        diagnostics_.push_back(Diagnostic{DiagnosticSeverity::Warning, kSubsystem, "GAMEUI_FDF_UNREADABLE",
            "无法读取界面定义 " + fdfPath, std::nullopt});
        return;
    }
    auto parsed = ParseFdf(BytesToText(*bytes.value), fdfPath);
    diagnostics_.insert(diagnostics_.end(), parsed.diagnostics.begin(), parsed.diagnostics.end());
    if (!parsed.value) return;
    // IncludeFile 在游戏里于文件开头读入，所以被包含文件的模板先登记，本文件的同名定义随后覆盖。
    if (!parsed.value->includes.empty()) {
        if (depth >= kMaxIncludeDepth) {
            diagnostics_.push_back(Diagnostic{DiagnosticSeverity::Warning, kSubsystem, "GAMEUI_INCLUDE_TOO_DEEP",
                fdfPath + " 的 IncludeFile 嵌套过深，已停止展开", std::nullopt});
        } else {
            for (const auto& include : parsed.value->includes) LoadFileAt(reader, include, origin, depth + 1);
        }
    }
    AddParsed(std::move(*parsed.value), origin);
}

void FrameLibrary::AddParsed(FdfFile file, const std::string& origin) {
    auto source = std::make_unique<FrameSourceFile>();
    source->file = std::move(file);
    source->origin = origin;
    files_.push_back(std::move(source));
    Register(files_.size() - 1);
}

void FrameLibrary::Register(std::size_t fileIndex) {
    const FdfFile& file = files_[fileIndex]->file;
    for (const auto& definition : file.definitions) {
        if (definition.name.empty()) continue;
        const std::string key = Text::LowerAscii(definition.name);
        if (const auto existing = templates_.find(key); existing != templates_.end()) {
            diagnostics_.push_back(Diagnostic{DiagnosticSeverity::Info, kSubsystem, "GAMEUI_TEMPLATE_REDEFINED",
                "模板 " + definition.name + " 在 " + file.path + " 重新定义，覆盖 " +
                    files_[existing->second.fileIndex]->file.path + " 中的版本",
                std::nullopt});
        }
        templates_[key] = FrameTemplateRef{&definition, fileIndex};
    }
    for (const auto& [key, value] : file.strings) {
        strings_[Text::LowerAscii(key)] = value;
    }
}

std::optional<FrameTemplateRef> FrameLibrary::FindTemplate(std::string_view name) const {
    if (const auto found = templates_.find(Text::LowerAscii(name)); found != templates_.end()) return found->second;
    return std::nullopt;
}

std::optional<std::string> FrameLibrary::FindString(std::string_view key) const {
    if (const auto found = strings_.find(Text::LowerAscii(key)); found != strings_.end()) return found->second;
    return std::nullopt;
}

} // namespace War3Map::GameUi
