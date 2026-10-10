#include "War3MapCore/Script/ScriptToolchainLayout.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/VerifiedFile.h"

#include <Windows.h>

#include <algorithm>
#include <exception>
#include <system_error>

namespace War3Map::Script {
namespace {

using Kind = ScriptToolchainBindingKind;
using LayoutResult = Result<ScriptToolchainLayout>;
constexpr std::uint64_t kScriptBytes = 16ull * 1024 * 1024;
constexpr std::uint64_t kConfigBytes = 4ull * 1024 * 1024;
constexpr std::uint64_t kExecutableBytes = 32ull * 1024 * 1024;
constexpr std::uint64_t kTotalBytes = 128ull * 1024 * 1024;
constexpr std::size_t kMaxBindings = 32;

void Diagnose(std::vector<Diagnostic>& diagnostics, DiagnosticSeverity severity,
    std::string code, std::string message, const std::filesystem::path& path) {
    diagnostics.push_back({severity, "Script.Layout", std::move(code), std::move(message), OriginSpan{path}});
}

Kind PathKind(const std::filesystem::path& path, std::error_code& error) {
    const auto state = std::filesystem::status(path, error);
    if (error == std::errc::no_such_file_or_directory || error == std::errc::not_a_directory) {
        error.clear();
        return Kind::Missing;
    }
    if (state.type() == std::filesystem::file_type::not_found) return Kind::Missing;
    if (std::filesystem::is_regular_file(state)) return Kind::RegularFile;
    if (std::filesystem::is_directory(state)) return Kind::Directory;
    return Kind::Other;
}

bool Directory(const std::filesystem::path& path) {
    std::error_code error;
    return PathKind(path, error) == Kind::Directory && !error;
}

bool Regular(const std::filesystem::path& path) {
    std::error_code error;
    return PathKind(path, error) == Kind::RegularFile && !error;
}

bool Named(const std::filesystem::path& path, const wchar_t* name) {
    return CompareStringOrdinal(path.filename().c_str(), -1, name, -1, TRUE) == CSTR_EQUAL;
}

bool ModernMarker(const std::filesystem::path& root) { return Directory(root / "compiler"); }
bool LegacyMarker(const std::filesystem::path& root) {
    return Directory(root / "plugin" / "wave") && Directory(root / "jass" / "system");
}

class FileLease {
public:
    explicit FileLease(const std::filesystem::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {}
    ~FileLease() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    [[nodiscard]] HANDLE Get() const { return handle_; }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

// 初次摘要也在禁止写入/删除的句柄下计算；不会先对可增长文件做无界 Sha256File。
bool BindFile(ScriptToolchainLayout& layout, const std::filesystem::path& path,
    std::uint64_t maxBytes, std::uint64_t& total, std::vector<Diagnostic>& diagnostics) {
    if (layout.bindingFiles.size() >= kMaxBindings) {
        Diagnose(diagnostics, DiagnosticSeverity::Error, "SCRIPT_LAYOUT_BINDING_BUDGET",
            "工具链关键来源数量超过读取预算。", path);
        return false;
    }
    std::error_code error;
    ScriptToolchainFileBinding binding{path, PathKind(path, error), {}, 0, maxBytes};
    if (error) {
        Diagnose(diagnostics, DiagnosticSeverity::Error, "SCRIPT_LAYOUT_STAT_FAILED", "无法确认工具链来源类型。", path);
        return false;
    }
    if (binding.kind == Kind::RegularFile) {
        FileLease lease(path);
        BY_HANDLE_FILE_INFORMATION info{};
        if (lease.Get() == INVALID_HANDLE_VALUE || GetFileType(lease.Get()) != FILE_TYPE_DISK ||
            !GetFileInformationByHandle(lease.Get(), &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE))) {
            Diagnose(diagnostics, DiagnosticSeverity::Error, "SCRIPT_LAYOUT_BINDING_FAILED",
                "无法只读锁定工具链普通文件；未返回未绑定来源。", path);
            return false;
        }
        binding.bytes = (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
        if (binding.bytes > maxBytes || binding.bytes > kTotalBytes - total) {
            Diagnose(diagnostics, DiagnosticSeverity::Error, "SCRIPT_LAYOUT_BINDING_BUDGET",
                "工具链文件超过关键来源读取预算。", path);
            return false;
        }
        auto hash = Sha256Handle(lease.Get());
        if (!hash.Succeeded()) {
            diagnostics.insert(diagnostics.end(), hash.diagnostics.begin(), hash.diagnostics.end());
            return false;
        }
        auto verified = ReadVerifiedFile(path, *hash.value, maxBytes);
        if (!verified.Succeeded() || verified.value->size() != binding.bytes) {
            diagnostics.insert(diagnostics.end(), verified.diagnostics.begin(), verified.diagnostics.end());
            Diagnose(diagnostics, DiagnosticSeverity::Error, "SCRIPT_LAYOUT_BINDING_FAILED",
                "工具链文件无法取得稳定内容快照。", path);
            return false;
        }
        binding.sha256 = std::move(*hash.value);
        total += binding.bytes;
    }
    layout.bindingFiles.push_back(std::move(binding));
    return true;
}

bool RequiredFile(ScriptToolchainLayout& layout, const std::filesystem::path& path,
    std::uint64_t& total, std::vector<Diagnostic>& diagnostics) {
    if (!BindFile(layout, path, kScriptBytes, total, diagnostics)) return false;
    const auto& binding = layout.bindingFiles.back();
    if (binding.kind == Kind::RegularFile && binding.bytes != 0) return true;
    Diagnose(diagnostics, DiagnosticSeverity::Error, "SCRIPT_LAYOUT_BASE_SCRIPT_MISSING",
        "基础脚本必须是非空、可读取的普通文件。", path);
    return false;
}

bool RequiredDirectory(ScriptToolchainLayout& layout, const std::filesystem::path& path,
    std::uint64_t& total, std::vector<Diagnostic>& diagnostics) {
    if (!BindFile(layout, path, 0, total, diagnostics)) return false;
    if (layout.bindingFiles.back().kind == Kind::Directory) return true;
    Diagnose(diagnostics, DiagnosticSeverity::Error, "SCRIPT_LAYOUT_INCLUDE_MISSING", "工具链包含目录不存在。", path);
    return false;
}

bool ExternalTool(ScriptToolchainLayout& layout, const std::filesystem::path& path,
    std::filesystem::path& selected, std::uint64_t& total, std::vector<Diagnostic>& diagnostics) {
    if (!BindFile(layout, path, kExecutableBytes, total, diagnostics)) return false;
    const auto& binding = layout.bindingFiles.back();
    if (binding.kind == Kind::RegularFile && binding.bytes != 0) selected = path;
    else Diagnose(diagnostics, DiagnosticSeverity::Warning, "SCRIPT_LAYOUT_EXTERNAL_UNAVAILABLE",
        "外部工具缺失、为空或不是普通文件；对应外部阶段不可用。", path);
    return true;
}

bool SameBindings(const ScriptToolchainLayout& a, const ScriptToolchainLayout& b) {
    if (a.bindingFiles.size() != b.bindingFiles.size()) return false;
    for (std::size_t index = 0; index < a.bindingFiles.size(); ++index) {
        const auto& left = a.bindingFiles[index];
        const auto& right = b.bindingFiles[index];
        if (left.path != right.path || left.kind != right.kind || left.sha256 != right.sha256 ||
            left.bytes != right.bytes || left.maxBytes != right.maxBytes) return false;
    }
    return true;
}

} // namespace

Result<ScriptToolchainLayout> ResolveScriptToolchainLayout(const ToolchainLayoutRequest& request) {
    try {
        if (request.toolRoot.empty() || (request.warcraftVersion != 20 && request.warcraftVersion != 24) ||
            (request.actualVersionMacro && (*request.actualVersionMacro < 100 || *request.actualVersionMacro > 199))) {
            return LayoutResult::Failure("Script.Layout", "SCRIPT_LAYOUT_REQUEST_INVALID",
                "工具根不能为空；校验版本仅支持20/24，实际经典版本宏必须在100至199之间。");
        }
        std::error_code error;
        auto root = std::filesystem::absolute(request.toolRoot, error).lexically_normal();
        if (root.has_relative_path() && root.filename().empty()) root = root.parent_path();
        if (error || !Directory(root)) return LayoutResult::Failure("Script.Layout", "SCRIPT_LAYOUT_ROOT_MISSING", "工具根不是可读取目录。");
        // 固定回溯，无任意父级搜索；候选根仍接受后续冲突检查。
        if (Named(root, L"ui") && Regular(root / "config") && ModernMarker(root.parent_path())) root = root.parent_path();
        else if (Named(root, L"mpq") && Named(root.parent_path(), L"share") &&
            Regular(root / "config") && LegacyMarker(root.parent_path().parent_path())) root = root.parent_path().parent_path();
        const bool modern = ModernMarker(root);
        const bool legacy = LegacyMarker(root);
        if (modern && legacy) return LayoutResult::Failure("Script.Layout", "SCRIPT_LAYOUT_AMBIGUOUS", "同一工具根同时具有现代和旧雪月布局；不能任意择一。");
        if (!modern && !legacy) return LayoutResult::Failure("Script.Layout", "SCRIPT_LAYOUT_UNRECOGNIZED", "工具根不符合完整现代或旧雪月布局标记。");

        ScriptToolchainLayout layout;
        layout.root = root;
        layout.kind = modern ? ScriptToolchainLayoutKind::Modern : ScriptToolchainLayoutKind::LegacyXueyue;
        layout.semanticsSupported = modern;
        layout.executionPolicy = modern ? ScriptToolchainExecutionPolicy::Modern
            : ScriptToolchainExecutionPolicy::FrameworkInputsRequired;
        layout.versionMacro = request.actualVersionMacro.value_or(100 + request.warcraftVersion);
        layout.allowReturnBug = request.warcraftVersion == 20;
        const auto compiler = root / (modern ? "compiler" : "plugin");
        const auto include = modern ? compiler / "include" : compiler / "wave" / "include";
        const auto jass = modern ? compiler / "jass" / std::to_string(request.warcraftVersion)
            : root / "jass" / "system" / (layout.allowReturnBug ? "rb" : "ht");
        layout.systemIncludes.push_back(include);
        if (legacy) layout.systemIncludes.push_back(root / "plugin");
        layout.commonScript = jass / "common.j";
        layout.blizzardScript = jass / "blizzard.j";
        LayoutResult result;
        std::uint64_t total = 0;
        if (!RequiredDirectory(layout, root, total, result.diagnostics) ||
            !RequiredDirectory(layout, include, total, result.diagnostics) ||
            !RequiredFile(layout, layout.commonScript, total, result.diagnostics) ||
            !RequiredFile(layout, layout.blizzardScript, total, result.diagnostics)) return result;
        // 绑定相反布局标记的缺失，防止解析后新增另一布局造成静默择一。
        if (!BindFile(layout, modern ? root / "plugin" / "wave" : root / "compiler", kConfigBytes, total, result.diagnostics) ||
            !BindFile(layout, modern ? root / "jass" / "system" : root / "plugin", kConfigBytes, total, result.diagnostics)) return result;
        const auto force = include / "WaveForce.i";
        if (!BindFile(layout, force, kConfigBytes, total, result.diagnostics)) return result;
        if (layout.bindingFiles.back().kind == Kind::RegularFile) layout.forceIncludes.push_back(force);
        else if (layout.bindingFiles.back().kind != Kind::Missing) {
            Diagnose(result.diagnostics, DiagnosticSeverity::Error, "SCRIPT_LAYOUT_FORCE_INCLUDE_INVALID",
                "WaveForce.i 存在但不是普通文件，不能静默省略。", force);
            return result;
        }
        if (!ExternalTool(layout, compiler / "wave" / "Wave.exe", layout.waveExecutable, total, result.diagnostics) ||
            !ExternalTool(layout, compiler / "jasshelper" / "clijasshelper.exe", layout.jassHelperExecutable, total, result.diagnostics) ||
            !ExternalTool(layout, compiler / "pjass" / "pjass-latest.exe", layout.pjassExecutable, total, result.diagnostics) ||
            !BindFile(layout, modern ? root / "ui" / "config" : root / "share" / "mpq" / "config",
                kConfigBytes, total, result.diagnostics)) return result;
        const auto scripts = modern ? compiler / "script" : root / "share" / "script" / "compile";
        for (const auto* name : {"template.lua", "w3x2lni/slk_lib.lua", "computed/order_list.lua", "computed/order_id.lua"}) {
            if (!BindFile(layout, scripts / name, kConfigBytes, total, result.diagnostics)) return result;
        }
        if (legacy) {
            for (const auto* name : {"wave.lua", "jasshelper.lua", "inject_code.lua", "native.lua"}) {
                if (!BindFile(layout, scripts / name, kConfigBytes, total, result.diagnostics)) return result;
            }
            if (!BindFile(layout, root / "share" / "script" / "ydwe_on_save.lua", kConfigBytes, total, result.diagnostics)) return result;
            Diagnose(result.diagnostics, DiagnosticSeverity::Info, "SCRIPT_LAYOUT_FRAMEWORK_INPUTS_REQUIRED",
                "已识别share/mpq框架入口；必须捕获正常cfg、实际include及档案身份，未知方言、中文函数或模板API在实际阶段拒绝。", root);
        }
        result.value.emplace(std::move(layout));
        return result;
    } catch (const std::exception&) {
        return LayoutResult::Failure("Script.Layout", "SCRIPT_LAYOUT_RESOLUTION_FAILED", "无法取得有界工具链布局与来源快照。");
    }
}

Status CheckScriptUiToolchainSemantics(const ToolchainLayoutRequest& request) {
    auto resolved = ResolveScriptToolchainLayout(request);
    if (!resolved.Succeeded() || !resolved.value) {
        // 普通 UI 根只放元数据，不要求拥有编译器；其他错误不能被解释为普通 UI。
        const bool standaloneUi = !resolved.diagnostics.empty() && std::all_of(
            resolved.diagnostics.begin(), resolved.diagnostics.end(), [](const Diagnostic& diagnostic) {
                return diagnostic.code == "SCRIPT_LAYOUT_UNRECOGNIZED";
            });
        if (standaloneUi) return Status::Ok();
        if (resolved.diagnostics.empty()) return Status::Failure("Script.Layout", "SCRIPT_LAYOUT_RESOLUTION_FAILED",
            "独立UI的工具链布局没有返回完整结果；不能推定为普通元数据目录。");
        return Status{std::move(resolved.diagnostics)};
    }
    if (!resolved.value->semanticsSupported) {
        auto failed = Status::Failure("Script.Layout", "SCRIPT_TOOLCHAIN_UNSUPPORTED",
            "旧UI的重写和模板语义不能通过另选现代编译器绕过。");
        failed.diagnostics.back().origin = OriginSpan{resolved.value->root};
        return failed;
    }
    return Status{std::move(resolved.diagnostics)};
}

Status VerifyScriptToolchainLayout(const ScriptToolchainLayout& layout) {
    if (layout.bindingFiles.empty() || layout.bindingFiles.size() > kMaxBindings || !layout.root.is_absolute()) {
        return Status::Failure("Script.Layout", "SCRIPT_LAYOUT_BINDING_INVALID", "工具链布局没有有效的有界来源绑定。");
    }
    // 用同一解析器重读关键来源并比较全部派生字段；不另维护第二份布局规则。
    auto current = ResolveScriptToolchainLayout({layout.root, layout.allowReturnBug ? 20 : 24, layout.versionMacro});
    if (!current.Succeeded()) {
        Status failed{std::move(current.diagnostics)};
        Diagnose(failed.diagnostics, DiagnosticSeverity::Error, "SCRIPT_LAYOUT_STALE", "工具链布局或关键来源已改变；请重新准备保存任务。", layout.root);
        return failed;
    }
    const auto& now = *current.value;
    if (layout.kind != now.kind || layout.root != now.root || layout.systemIncludes != now.systemIncludes ||
        layout.forceIncludes != now.forceIncludes || layout.commonScript != now.commonScript ||
        layout.blizzardScript != now.blizzardScript || layout.waveExecutable != now.waveExecutable ||
        layout.jassHelperExecutable != now.jassHelperExecutable || layout.pjassExecutable != now.pjassExecutable ||
        layout.versionMacro != now.versionMacro || layout.allowReturnBug != now.allowReturnBug ||
        layout.semanticsSupported != now.semanticsSupported || layout.executionPolicy != now.executionPolicy || !SameBindings(layout, now)) {
        return Status::Failure("Script.Layout", "SCRIPT_LAYOUT_STALE", "工具链关键内容、可用工具或布局字段与冻结来源不一致。");
    }
    return Status::Ok();
}

} // namespace War3Map::Script
