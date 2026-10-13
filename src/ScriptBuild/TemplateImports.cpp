#include "TemplateImports.h"

#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Script/ScriptSaveBundle.h"

#include <algorithm>
#include <cctype>

namespace War3Map::Script::detail {
namespace {

constexpr std::size_t kImportFileBytes = 16u * 1024u * 1024u;
constexpr std::size_t kImportTotalBytes = 64u * 1024u * 1024u;
constexpr std::size_t kImportCount = 4096;

std::string Lower(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

} // namespace

std::optional<std::string> TemplateImportSink::MemberName(std::string name, std::string& error) const {
    std::replace(name.begin(), name.end(), '/', '\\');
    if (name.empty() || name.size() > 240 || name.front() == '\\' || name.find(':') != std::string::npos
        || name.find("..") != std::string::npos
        || std::any_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; })) {
        error = "import 的文件名必须是地图内相对路径：" + name;
        return std::nullopt;
    }
    if (IsTemplateImportReserved(name)) {
        error = "import 不能覆盖地图脚本、物编或归档内部文件：" + name;
        return std::nullopt;
    }
    return name;
}

bool TemplateImportSink::Add(const std::string& member, std::string_view content, bool path, std::string& error) {
    // 只读检查场景：核对写法与名称，不读文件、不产生成员。
    if (!root_) return true;
    std::vector<std::byte> bytes;
    if (path) {
        std::string relative(content);
        std::replace(relative.begin(), relative.end(), '\\', '/');
        const std::filesystem::path file(std::u8string(relative.begin(), relative.end()));
        if (relative.empty() || file.is_absolute() || file.has_root_name() || file.has_root_directory()
            || std::any_of(file.begin(), file.end(), [](const auto& part) { return part == ".."; })) {
            error = "import(" + member + ") 的文件路径必须位于地图所在目录内：" + std::string(content);
            return false;
        }
        auto read = IO::ReadFileBytes(*root_ / file, kImportFileBytes);
        if (!read.Succeeded() || !read.value) {
            error = "import(" + member + ") 无法读取地图所在目录下的 " + std::string(content) + "（不存在、不是普通文件或超过 16 MiB）";
            return false;
        }
        bytes = std::move(*read.value);
    } else {
        const auto* first = reinterpret_cast<const std::byte*>(content.data());
        bytes.assign(first, first + content.size());
    }
    // 同名后者覆盖前者，与 YDWE 依次覆盖复制一致。
    const auto key = Lower(member);
    const auto existing = std::find_if(imports_.begin(), imports_.end(), [&](const auto& item) { return Lower(item.name) == key; });
    if (existing != imports_.end()) {
        bytes_ -= existing->data.size();
        diagnostics_.push_back({DiagnosticSeverity::Warning, "Script.Template", "TEMPLATE_IMPORT_DUPLICATE",
            "模板多次 import 同一文件 " + member + "，以最后一次为准", {}});
    } else if (imports_.size() >= kImportCount) {
        error = "模板 import 的文件超过 4096 个";
        return false;
    }
    if (bytes.size() > kImportTotalBytes - bytes_) {
        error = "模板 import 的文件合计超过 64 MiB";
        return false;
    }
    bytes_ += bytes.size();
    if (existing != imports_.end()) existing->data = std::move(bytes);
    else imports_.push_back({member, std::move(bytes)});
    return true;
}

} // namespace War3Map::Script::detail
