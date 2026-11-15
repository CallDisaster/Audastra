#include "War3MapCore/Feedback/ErrorReport.h"

#include "War3MapCore/IO/FileBytes.h"

#include <miniz.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

namespace War3Map::Feedback {
namespace {

namespace fs = std::filesystem;

Result<std::vector<std::byte>> ZipFailure(std::string message) {
    return Result<std::vector<std::byte>>::Failure("Feedback.ErrorReport", "ERROR_REPORT_ZIP", std::move(message));
}

fs::path CurrentProfile() {
#ifdef _WIN32
    wchar_t value[1024]{};
    const auto length = GetEnvironmentVariableW(L"USERPROFILE", value, 1024);
    if (length > 0 && length < 1024) return fs::path(value);
#else
    if (const auto* value = std::getenv("HOME"); value && *value) return fs::path(value);
#endif
    return {};
}

char Fold(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// 只按 ASCII 忽略大小写；中文用户名按原字节比较。
void ReplaceAll(std::string& text, std::string_view needle, std::string_view replacement) {
    if (needle.size() < 4) return;
    std::string result;
    std::size_t start = 0;
    for (std::size_t i = 0; i + needle.size() <= text.size();) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j)
            if (Fold(text[i + j]) != Fold(needle[j])) { match = false; break; }
        if (!match) { ++i; continue; }
        result.append(text, start, i - start);
        result += replacement;
        i += needle.size();
        start = i;
    }
    if (start == 0) return;
    result.append(text, start, std::string::npos);
    text = std::move(result);
}

} // namespace

std::string MaskUserProfile(std::string text, const fs::path& profile) {
    const auto root = profile.empty() ? CurrentProfile() : profile;
    if (root.empty()) return text;
    auto backslash = IO::PathUtf8(root);
    while (backslash.size() > 3 && (backslash.back() == '\\' || backslash.back() == '/')) backslash.pop_back();
    auto slash = backslash;
    for (auto& c : backslash) if (c == '/') c = '\\';
    for (auto& c : slash) if (c == '\\') c = '/';
    ReplaceAll(text, backslash, "%USERPROFILE%");
    if (slash != backslash) ReplaceAll(text, slash, "%USERPROFILE%");
    // JSON 等转义写法里的双反斜杠。
    std::string escaped;
    for (const char c : backslash) { escaped += c; if (c == '\\') escaped += '\\'; }
    if (escaped != backslash) ReplaceAll(text, escaped, "%USERPROFILE%");
    return text;
}

std::string RenderErrorReportText(const ErrorReportInput& input) {
    std::string text = "Audastra 错误报告\r\n\r\n【经过】\r\n";
    if (input.description.empty()) text += "（未填写）\r\n";
    for (const char c : input.description) {
        if (c == '\r') continue;
        if (c == '\n') text += "\r\n"; else text += c;
    }
    if (!text.ends_with("\r\n")) text += "\r\n";
    for (const auto& section : input.sections) {
        text += "\r\n【" + section.title + "】\r\n";
        for (const auto& [name, value] : section.rows) text += name + "：" + value + "\r\n";
    }
    text += "\r\n【诊断列表】（" + std::to_string(input.diagnostics.size()) + " 条）\r\n";
    for (const auto& line : input.diagnostics) text += line + "\r\n";
    text += "\r\n【附带文件】\r\n";
    if (input.files.empty()) text += "（无）\r\n";
    for (const auto& file : input.files) {
        std::error_code error;
        const auto size = fs::file_size(file.path, error);
        text += file.name + (error ? std::string("（无法读取）") : "（" + std::to_string(size) + " 字节）") + "\r\n";
    }
    return MaskUserProfile(std::move(text));
}

Result<std::vector<std::byte>> BuildErrorReportZip(const ErrorReportInput& input) {
    mz_zip_archive archive{};
    if (!mz_zip_writer_init_heap(&archive, 0, 0)) return ZipFailure("无法开始生成错误报告。");
    struct End {
        mz_zip_archive* archive;
        ~End() { mz_zip_writer_end(archive); }
    } end{&archive};
    const auto report = RenderErrorReportText(input);
    // 带 UTF-8 BOM，记事本直接按 UTF-8 打开。
    const std::string bom = "\xEF\xBB\xBF" + report;
    if (!mz_zip_writer_add_mem(&archive, "report.txt", bom.data(), bom.size(), MZ_BEST_COMPRESSION))
        return ZipFailure("无法写入 report.txt。");
    for (const auto& file : input.files) {
        if (file.name.empty() || file.name.find("..") != std::string::npos || file.name.front() == '/') continue;
        const auto bytes = IO::ReadFileBytes(file.path, kErrorReportFileLimit);
        std::string note;
        if (!bytes.Succeeded()) {
            note = "无法读取 " + MaskUserProfile(IO::PathUtf8(file.path)) + "（不存在、被占用或超过 16 MiB）。\r\n";
            const auto name = file.name + ".missing.txt";
            if (!mz_zip_writer_add_mem(&archive, name.c_str(), note.data(), note.size(), MZ_BEST_SPEED))
                return ZipFailure("无法写入 " + name + "。");
            continue;
        }
        if (file.text) {
            auto text = MaskUserProfile(std::string(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size()));
            if (!mz_zip_writer_add_mem(&archive, file.name.c_str(), text.data(), text.size(), MZ_BEST_COMPRESSION))
                return ZipFailure("无法写入 " + file.name + "。");
        } else if (!mz_zip_writer_add_mem(&archive, file.name.c_str(), bytes.value->data(), bytes.value->size(), MZ_BEST_COMPRESSION)) {
            return ZipFailure("无法写入 " + file.name + "。");
        }
    }
    void* heap = nullptr;
    std::size_t size = 0;
    if (!mz_zip_writer_finalize_heap_archive(&archive, &heap, &size)) return ZipFailure("无法完成错误报告 ZIP。");
    struct Free { void operator()(void* memory) const { mz_free(memory); } };
    std::unique_ptr<void, Free> owned(heap);
    if (!heap || !size || size > kErrorReportZipLimit) return ZipFailure("错误报告超过 64 MiB，没有导出。");
    std::vector<std::byte> result(size);
    std::memcpy(result.data(), heap, size);
    return Result<std::vector<std::byte>>::Success(std::move(result));
}

Status WriteErrorReport(const fs::path& target, const ErrorReportInput& input) {
    if (target.empty() || !target.is_absolute())
        return Status::Failure("Feedback.ErrorReport", "ERROR_REPORT_PATH", "错误报告的保存位置必须是绝对路径。");
    auto zip = BuildErrorReportZip(input);
    if (!zip.Succeeded()) return Status{std::move(zip.diagnostics)};
    // 先写同目录临时文件再替换，导出中断不留下半个 ZIP。
    auto temporary = target;
    temporary += ".part";
    std::error_code error;
    fs::remove(temporary, error);
    if (const auto written = IO::WriteFileBytes(temporary, *zip.value); !written.Succeeded()) {
        fs::remove(temporary, error);
        return Status::Failure("Feedback.ErrorReport", "ERROR_REPORT_WRITE", "无法写入 " + IO::PathUtf8(target) + "。");
    }
    fs::rename(temporary, target, error);
    if (error) {
        fs::remove(temporary, error);
        return Status::Failure("Feedback.ErrorReport", "ERROR_REPORT_WRITE", "无法写入 " + IO::PathUtf8(target) + "（文件可能正被打开）。");
    }
    return Status::Ok();
}

} // namespace War3Map::Feedback
