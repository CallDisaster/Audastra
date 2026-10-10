#include "War3MapCore/Script/ScriptInjectionSources.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"
#include "ScriptSourceBinding.h"

#include <Windows.h>
#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <sstream>
#include <string_view>

namespace War3Map::Script {
namespace {
using Kind = ScriptToolchainBindingKind;
using CaptureResult = Result<ScriptInjectionSources>;
constexpr std::size_t kNodes = 8192, kFiles = 4096, kRegistrations = 16384;
constexpr std::uint64_t kCfgBytes = 1024 * 1024, kJBytes = 8 * kCfgBytes;
constexpr std::uint64_t kTotalBytes = 32 * kCfgBytes, kIndexBytes = 64 * kCfgBytes;
struct PathLess {
    bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};
void Diagnose(std::vector<Diagnostic>& diagnostics, std::string code, std::string message,
    const std::filesystem::path& path = {}, DiagnosticSeverity severity = DiagnosticSeverity::Error) {
    diagnostics.push_back({severity, "Script.InjectionSources", std::move(code), std::move(message), OriginSpan{path}});
}
void Part(std::string& text, std::string_view value) {
    text += std::to_string(value.size()) + ":"; text.append(value);
}
void PathPart(std::string& text, const std::filesystem::path& value) {
    const auto raw = value.generic_u8string();
    Part(text, std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size()));
}
Result<std::string> PlanIdentity(const ScriptUserSourcePlan& plan) {
    std::string text;
    for (const auto value : {static_cast<int>(plan.bindingCoverage), static_cast<int>(plan.layoutKind),
        static_cast<int>(plan.nativeState), static_cast<int>(plan.executionSupported)}) Part(text, std::to_string(value));
    for (const auto* path : {&plan.toolRoot, &plan.uiRoot, &plan.legacyUserLibraryRoot, &plan.mapDirectory, &plan.nativeRoot}) PathPart(text, *path);
    Part(text, plan.layoutIdentitySha256); Part(text, plan.catalogSha256); Part(text, plan.profileIdentitySha256);
    Part(text, std::to_string(plan.profileTargetVersionMacro));
    Part(text, std::to_string(plan.effectiveLayers.size()));
    for (const auto& layer : plan.effectiveLayers) Part(text, layer);
    for (const auto* paths : {&plan.orderedInjectionRoots, &plan.orderedUserIncludes,
        &plan.orderedSystemIncludes, &plan.orderedForceIncludes}) {
        Part(text, std::to_string(paths->size())); for (const auto& path : *paths) PathPart(text, path);
    }
    Part(text, std::to_string(plan.bindings.size()));
    for (const auto& binding : plan.bindings) {
        PathPart(text, binding.path); Part(text, std::to_string(static_cast<int>(binding.kind))); Part(text, binding.sha256);
        Part(text, std::to_string(binding.bytes)); Part(text, std::to_string(binding.maxBytes));
    }
    Part(text, std::to_string(plan.totalFileBytes));
    return Sha256Text(text);
}
bool BindingEqual(const ScriptToolchainFileBinding& a, const ScriptToolchainFileBinding& b) {
    return a.path == b.path && a.kind == b.kind && a.sha256 == b.sha256 && a.bytes == b.bytes && a.maxBytes == b.maxBytes;
}
bool Equal(const ScriptInjectionSources& a, const ScriptInjectionSources& b) {
    if (a.coverage != b.coverage || a.executionSupported != b.executionSupported || a.version24 != b.version24 ||
        a.literalLookupSupported != b.literalLookupSupported || a.legacyLineSemanticsProven != b.legacyLineSemanticsProven ||
        a.entryPlanIdentitySha256 != b.entryPlanIdentitySha256 || a.totalFileBytes != b.totalFileBytes ||
        a.tree != b.tree || a.registrations != b.registrations || a.choices != b.choices ||
        a.linePolicy != b.linePolicy || a.textBoundaries != b.textBoundaries ||
        a.orderedRoots.size() != b.orderedRoots.size() || a.files.size() != b.files.size()) return false;
    for (std::size_t i = 0; i < a.orderedRoots.size(); ++i) if (!BindingEqual(a.orderedRoots[i], b.orderedRoots[i])) return false;
    for (std::size_t i = 0; i < a.files.size(); ++i) {
        if (!BindingEqual(a.files[i].binding, b.files[i].binding) || a.files[i].bytes != b.files[i].bytes ||
            a.files[i].lastWriteTicks != b.files[i].lastWriteTicks) return false;
    }
    return true;
}
std::string TrimWindowsTextC(std::string_view text) {
    const auto blank = [](unsigned char c) { return c == 0x20 || (c >= 0x09 && c <= 0x0d); };
    while (!text.empty() && blank(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && blank(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}
class Capture {
public:
    ScriptInjectionSources sources;
    std::vector<Diagnostic> diagnostics;
    bool Run(const ScriptUserSourcePlan& plan) {
        auto identity = PlanIdentity(plan);
        if (!identity.Succeeded()) { diagnostics = std::move(identity.diagnostics); return false; }
        sources.entryPlanIdentitySha256 = std::move(*identity.value);
        std::vector<std::filesystem::path> pending, configs;
        for (const auto& root : plan.orderedInjectionRoots) {
            auto captured = Detail::CaptureSourceBinding(root, Kind::Directory, false, 0);
            if (!captured.Succeeded()) return Failed(captured.diagnostics);
            if (!Charge(512 + root.native().size() * 4, root)) return false;
            sources.orderedRoots.push_back(std::move(captured.value->binding));
            if (sources.orderedRoots.back().kind == Kind::Directory) pending.push_back(root);
        }
        std::reverse(pending.begin(), pending.end());
        std::set<std::filesystem::path, PathLess> enumerated;
        while (!pending.empty()) {
            auto directory = std::move(pending.back()); pending.pop_back();
            if (!enumerated.insert(directory).second) continue;
            auto captured = Detail::CaptureSourceBinding(directory, Kind::Directory, true, 0);
            if (!captured.Succeeded()) return Failed(captured.diagnostics);
            if (!Node(directory, Kind::Directory)) return false;
            std::error_code error;
            std::filesystem::directory_iterator it(directory, error), end;
            while (!error && it != end) {
                std::filesystem::path path;
                if (!Detail::NormalizeSourcePath(it->path(), path)) return Error("INJECTION_SOURCE_PATH_INVALID", "树入口路径无效。", it->path());
                const auto attributes = GetFileAttributesW(path.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE))) {
                    return Error("INJECTION_SOURCE_ENTRY_REJECTED", "树入口不可读、非普通或包含 reparse；未返回部分来源。", path);
                }
                const Kind kind = attributes & FILE_ATTRIBUTE_DIRECTORY ? Kind::Directory : Kind::RegularFile;
                const bool unseen = !nodes_.contains(path);
                if (!Node(path, kind)) return false;
                if (unseen && kind == Kind::Directory) pending.push_back(path);
                if (unseen && kind == Kind::RegularFile && path.extension() == ".cfg") {
                    if (configs.size() >= kFiles) return Error("INJECTION_SOURCE_BUDGET", "cfg数量超过4096。", path);
                    configs.push_back(path);
                }
                it.increment(error);
            }
            if (error) return Error("INJECTION_SOURCE_ENUMERATION_FAILED", "来源目录枚举未完整结束。", directory);
        }
        std::sort(sources.tree.begin(), sources.tree.end(), [](const auto& a, const auto& b) { return PathLess{}(a.path, b.path); });
        std::sort(configs.begin(), configs.end(), PathLess{});
        for (const auto& config : configs) {
            std::size_t cfgIndex = 0, jIndex = 0;
            auto library = config; library.replace_extension(".j");
            if (!nodes_.contains(library) || nodes_.at(library) != Kind::RegularFile) {
                return Error("INJECTION_SOURCE_LIBRARY_MISSING", "cfg对应J缺失或不在完整普通文件清单中。", library);
            }
            if (!Own(config, kCfgBytes, cfgIndex, false) || !Own(library, kJBytes, jIndex, true) || !Parse(cfgIndex, jIndex)) return false;
        }
        for (auto& [key, choice] : choices_) { (void)key; sources.choices.push_back(std::move(choice)); }
        return true;
    }
private:
    std::map<std::filesystem::path, Kind, PathLess> nodes_;
    std::map<std::filesystem::path, std::size_t, PathLess> owned_;
    std::map<std::string, ScriptInjectionChoice> choices_;
    std::uint64_t indexBytes_ = 0;
    std::size_t libraryCount_ = 0;
    bool nonstandardReported_ = false;
    bool Error(std::string code, std::string message, const std::filesystem::path& path) {
        Diagnose(diagnostics, std::move(code), std::move(message), path); return false;
    }
    bool Failed(const std::vector<Diagnostic>& input) {
        diagnostics.insert(diagnostics.end(), input.begin(), input.end()); return false;
    }
    bool Charge(std::uint64_t bytes, const std::filesystem::path& path) {
        if (bytes > kIndexBytes - indexBytes_) return Error("INJECTION_SOURCE_BUDGET", "来源索引超过64MiB估算预算。", path);
        indexBytes_ += bytes; return true;
    }
    bool Node(const std::filesystem::path& path, Kind kind) {
        const auto found = nodes_.find(path);
        if (found != nodes_.end()) return found->second == kind || Error("INJECTION_SOURCE_CHANGED", "枚举期间入口类型改变。", path);
        if (nodes_.size() >= kNodes) return Error("INJECTION_SOURCE_BUDGET", "来源树节点超过8192。", path);
        if (!Charge(1024 + path.native().size() * 16, path)) return false;
        nodes_.emplace(path, kind); sources.tree.push_back({path, kind}); return true;
    }
    bool Own(const std::filesystem::path& path, std::uint64_t limit, std::size_t& index, bool library) {
        const auto found = owned_.find(path);
        if (found != owned_.end()) { index = found->second; return true; }
        if (library && libraryCount_ >= kFiles) return Error("INJECTION_SOURCE_BUDGET", "不同J数量超过4096。", path);
        if (!Charge(1024 + path.native().size() * 8, path)) return false;
        auto snapshot = Detail::CaptureSourceBinding(path, Kind::RegularFile, true,
            std::min(limit, kTotalBytes - sources.totalFileBytes));
        if (!snapshot.Succeeded()) return Failed(snapshot.diagnostics);
        index = sources.files.size(); owned_.emplace(path, index);
        sources.totalFileBytes += snapshot.value->binding.bytes;
        sources.files.push_back({std::move(snapshot.value->binding), std::move(snapshot.value->bytes), snapshot.value->lastWriteTicks});
        if (library) ++libraryCount_;
        return true;
    }
    bool Parse(std::size_t configIndex, std::size_t libraryIndex) {
        const auto& cfg = sources.files[configIndex];
        std::string text;
        if (!cfg.bytes.empty()) text.assign(reinterpret_cast<const char*>(cfg.bytes.data()), cfg.bytes.size());
        if (sources.linePolicy == ScriptInjectionLinePolicy::FrameworkText) return ParseFramework(text, configIndex, libraryIndex);
        if (sources.linePolicy == ScriptInjectionLinePolicy::Lua535WindowsTextC) {
            const auto boundary = text.find('\x1a');
            if (boundary != std::string::npos) {
                if (!Charge(64, cfg.binding.path)) return false;
                sources.textBoundaries.push_back({configIndex, static_cast<std::uint64_t>(boundary)});
                Diagnose(diagnostics, "INJECTION_SOURCE_TEXT_EOF", "所选Windows C文本策略在原字节偏移 " +
                    std::to_string(boundary) + " 遇到CTRL-Z；此处及后续字节仍完整拥有，但未参与注册解释。",
                    cfg.binding.path, DiagnosticSeverity::Warning);
                diagnostics.back().origin->offset = boundary;
                diagnostics.back().origin->length = 1;
                text.resize(boundary);
            }
            std::size_t written = 0;
            for (std::size_t read = 0; read < text.size(); ++read) {
                if (text[read] == '\r' && read + 1 < text.size() && text[read + 1] == '\n') continue;
                text[written++] = text[read];
            }
            text.resize(written); // 只转换CRLF；内部单独CR、BOM、NUL及编码保持不变。
        }
        std::istringstream stream(text);
        ScriptInjectionSection section = ScriptInjectionSection::General;
        std::uint32_t lineNumber = 0;
        for (std::string raw; std::getline(stream, raw);) {
            ++lineNumber;
            auto key = sources.linePolicy == ScriptInjectionLinePolicy::Lua535WindowsTextC ?
                TrimWindowsTextC(raw) : Text::TrimAsciiControlSpaceCopy(raw);
            if (!Register(std::move(key), section, lineNumber, configIndex, libraryIndex)) return false;
        }
        if (stream.bad()) return Error("INJECTION_SOURCE_PARSE_FAILED", "cfg完整字节的分行失败。", cfg.binding.path);
        return true;
    }
    bool FrameworkDiagnostic(std::string code, std::string message, const std::filesystem::path& path,
        std::size_t offset, std::size_t length, DiagnosticSeverity severity = DiagnosticSeverity::Error) {
        if (!Charge(512 + path.native().size() * 4, path)) return false;
        Diagnose(diagnostics, std::move(code), std::move(message), path, severity);
        diagnostics.back().origin->offset = offset;
        diagnostics.back().origin->length = length;
        return severity != DiagnosticSeverity::Error;
    }
    bool ParseFramework(std::string_view text, std::size_t configIndex, std::size_t libraryIndex) {
        const auto& path = sources.files[configIndex].binding.path;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto c = static_cast<unsigned char>(text[i]);
            if ((c < 0x20 && c != '\t' && c != '\r' && c != '\n') || c == 0x7f) {
                return FrameworkDiagnostic("INJECTION_FRAMEWORK_CONTROL_BYTE", "cfg含非法控制字节；未截断或返回部分快照。", path, i, 1);
            }
        }
        const auto normalized = [&](std::string message, std::size_t offset, std::size_t length) {
            return FrameworkDiagnostic("INJECTION_FRAMEWORK_TEXT_NORMALIZED", std::move(message), path, offset, length, DiagnosticSeverity::Warning);
        };
        std::size_t cursor = 0;
        if (text.starts_with("\xef\xbb\xbf")) {
            if (!normalized("首UTF8 BOM仅用于配置解释；owned原字节及SHA保持不变。", 0, 3)) return false;
            cursor = 3;
        }
        ScriptInjectionSection section = ScriptInjectionSection::General;
        std::uint32_t lineNumber = 0;
        bool crlfReported = false, crReported = false, commentReported = false;
        while (cursor < text.size()) {
            ++lineNumber;
            const auto begin = cursor;
            const auto found = text.find_first_of("\r\n", begin);
            const auto end = found == std::string_view::npos ? text.size() : found;
            cursor = end;
            if (cursor < text.size()) {
                const bool crlf = text[cursor] == '\r' && cursor + 1 < text.size() && text[cursor + 1] == '\n';
                if (text[cursor] == '\r' && !(crlf ? crlfReported : crReported)) {
                    if (!normalized(crlf ? "按正常CRLF换行解释配置；owned原字节及SHA保持不变。" :
                        "按正常CR换行解释配置，不沿用旧仅LF分行行为。", cursor, crlf ? 2 : 1)) return false;
                    (crlf ? crlfReported : crReported) = true;
                }
                cursor += crlf ? 2 : 1;
            }
            auto key = text.substr(begin, end - begin);
            while (!key.empty() && (key.front() == ' ' || key.front() == '\t')) key.remove_prefix(1);
            if (key.empty() || key.front() == '#') continue;
            const auto comment = key.find("//");
            if (comment != std::string_view::npos) {
                if (!commentReported && !normalized("按cfg行尾//注释解释；不会作为旧模式键参与匹配。",
                    static_cast<std::size_t>(key.data() - text.data()) + comment, 2)) return false;
                commentReported = true;
                key = key.substr(0, comment);
            }
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.remove_suffix(1);
            if (key.empty()) continue;
            const auto offset = static_cast<std::size_t>(key.data() - text.data());
            if (key.size() > 4096) return Error("INJECTION_SOURCE_BUDGET", "配置键超过4096字节。", path);
            if (key != "[general]" && key != "[new]" && key != "[old]") {
                for (std::size_t i = 0; i < key.size(); ++i) {
                    const auto c = static_cast<unsigned char>(key[i]);
                    if (c >= 0x80) return FrameworkDiagnostic("INJECTION_FRAMEWORK_KEY_ENCODING_UNKNOWN",
                        "注入键含非ASCII字节；中文及来源编码尚未支持，未猜测GBK或静默改写。", path, offset + i, 1);
                    const bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
                    if (!alpha && !(i > 0 && c >= '0' && c <= '9')) return FrameworkDiagnostic("INJECTION_FRAMEWORK_KEY_INVALID",
                        "注入键必须是ASCII标识符，未作为Lua模式解释。", path, offset + i, 1);
                }
            }
            if (!Register(std::string(key), section, lineNumber, configIndex, libraryIndex)) return false;
        }
        return true;
    }
    bool Register(std::string key, ScriptInjectionSection& section, std::uint32_t lineNumber,
        std::size_t configIndex, std::size_t libraryIndex) {
        const auto& path = sources.files[configIndex].binding.path;
        if (key.empty() || key.front() == '#') return true;
        if (key == "[general]") { section = ScriptInjectionSection::General; return true; }
        if (key == "[new]") { section = ScriptInjectionSection::New; return true; }
        if (key == "[old]") { section = ScriptInjectionSection::Old; return true; }
        if (key.size() > 4096 || sources.registrations.size() >= kRegistrations) {
            return Error("INJECTION_SOURCE_BUDGET", "注册超过16384或原键超过4096字节。", path);
        }
        if (!Charge(512 + key.size() * 4, path)) return false;
        const bool pattern = key.find_first_of("^$()%.[]*+-?") != std::string::npos;
        if (pattern && sources.literalLookupSupported) {
            Diagnose(diagnostics, "INJECTION_SOURCE_PATTERN_UNSUPPORTED", "保留Lua模式原键；未证明与literal查找等价。", path, DiagnosticSeverity::Warning);
        }
        sources.literalLookupSupported = sources.literalLookupSupported && !pattern;
        if (!nonstandardReported_ && key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos) {
            Diagnose(diagnostics, "INJECTION_SOURCE_NONSTANDARD_KEY",
                sources.linePolicy == ScriptInjectionLinePolicy::Lua535WindowsTextC ?
                    "保留非标准原键及所选文本行策略；嵌入式旧宿主行语义仍未验证。" :
                    "保留非标准原键及LF分行；旧宿主行语义未实测。", path, DiagnosticSeverity::Warning);
            nonstandardReported_ = true;
        }
        const auto registrationIndex = sources.registrations.size();
        sources.registrations.push_back({key, configIndex, libraryIndex, lineNumber, section, pattern});
        if (section != ScriptInjectionSection::General && section != (sources.version24 ? ScriptInjectionSection::New : ScriptInjectionSection::Old)) return true;
        auto [entry, inserted] = choices_.try_emplace(key, ScriptInjectionChoice{key, libraryIndex, {}, {}});
        auto& choice = entry->second;
        choice.registrationIndexes.push_back(registrationIndex);
        if (inserted || choice.selectedFile == libraryIndex || std::find(choice.shadowedFiles.begin(), choice.shadowedFiles.end(), libraryIndex) != choice.shadowedFiles.end()) return true;
        const auto time = sources.files[libraryIndex].lastWriteTicks;
        if (time == sources.files[choice.selectedFile].lastWriteTicks || std::any_of(choice.shadowedFiles.begin(), choice.shadowedFiles.end(), [&](auto prior) { return sources.files[prior].lastWriteTicks == time; })) {
            return Error("INJECTION_SOURCE_MTIME_AMBIGUOUS", "不同J为同一原键注册相同mtime，不能猜目录枚举胜者。", path);
        }
        if (time > sources.files[choice.selectedFile].lastWriteTicks) {
            choice.shadowedFiles.push_back(choice.selectedFile); choice.selectedFile = libraryIndex;
        } else choice.shadowedFiles.push_back(libraryIndex);
        return true;
    }
};
} // namespace

Result<ScriptInjectionSources> CaptureScriptInjectionSources(const ScriptUserSourcePlan& entryPlan,
    const ScriptUserSourceRequest& currentRequest, bool version24, ScriptInjectionLinePolicy linePolicy) {
    if (linePolicy != ScriptInjectionLinePolicy::ExistingLfControlTrim && linePolicy != ScriptInjectionLinePolicy::Lua535WindowsTextC &&
        linePolicy != ScriptInjectionLinePolicy::FrameworkText) {
        return CaptureResult::Failure("Script.InjectionSources", "INJECTION_LINE_POLICY_INVALID", "未知的cfg文本读取策略。");
    }
    const auto verified = VerifyScriptUserSourcePlan(entryPlan, currentRequest);
    if (!verified.Succeeded()) { CaptureResult failure; failure.diagnostics = verified.diagnostics; return failure; }
    if (!entryPlan.profileIdentitySha256.empty()) {
        if (!currentRequest.profile || version24 != (currentRequest.profile->baseLibraryCategory == ScriptProfileBaseLibraryCategory::Post124)) {
            return CaptureResult::Failure("Script.InjectionSources", "INJECTION_PROFILE_VERSION_REJECTED", "注入版本段必须使用档案声明的基础库类别，不能借目标版本宏或调用参数覆盖。");
        }
        if (linePolicy == ScriptInjectionLinePolicy::Lua535WindowsTextC) return CaptureResult::Failure(
            "Script.InjectionSources", "INJECTION_PROFILE_POLICY_REJECTED", "工具链档案要求FrameworkText，不能使用历史离线文本策略。");
        linePolicy = ScriptInjectionLinePolicy::FrameworkText;
    }
    CaptureResult result;
    try {
        Capture capture;
        capture.sources.version24 = version24;
        capture.sources.linePolicy = linePolicy;
        if (capture.Run(entryPlan)) result.value = std::move(capture.sources);
        result.diagnostics = std::move(capture.diagnostics);
    } catch (const std::exception&) {
        Diagnose(result.diagnostics, "INJECTION_SOURCE_CAPTURE_FAILED", "来源捕获失败；未返回部分可执行快照。");
    }
    return result;
}
Status VerifyScriptInjectionSources(const ScriptInjectionSources& sources, const ScriptUserSourcePlan& entryPlan,
    const ScriptUserSourceRequest& currentRequest, bool version24, ScriptInjectionLinePolicy linePolicy) {
    auto current = CaptureScriptInjectionSources(entryPlan, currentRequest, version24, linePolicy);
    if (!current.Succeeded() || !Equal(sources, *current.value)) {
        Diagnose(current.diagnostics, "INJECTION_SOURCE_STALE", "注入来源或完整入口计划身份已变化，须重新捕获。");
        return Status{std::move(current.diagnostics)};
    }
    return Status{std::move(current.diagnostics)};
}
} // namespace War3Map::Script
