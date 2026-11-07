#include "War3MapCore/HLua/ProjectConfiguration.h"
#include "War3MapCore/IO/FileBytes.h"
#include <nlohmann/json.hpp>
#include <set>

namespace War3Map::HLua {
namespace {
bool Specified(const FrameworkLocation& value) { return !value.repository.empty() || !value.runtime.empty() || !value.build.empty(); }
}
Result<bool> RecognizeProject(const std::filesystem::path& project) {
    try {
        std::error_code ec;
        const bool main = std::filesystem::is_regular_file(project / L"main.lua", ec);
        if (ec && ec != std::errc::no_such_file_or_directory) throw std::runtime_error("main metadata");
        ec.clear(); const bool hslk = std::filesystem::is_directory(project / L"hslk", ec);
        if (ec && ec != std::errc::no_such_file_or_directory) throw std::runtime_error("hslk metadata");
        ec.clear(); const bool map = std::filesystem::is_directory(project / L"map/w3x", ec);
        if (ec && ec != std::errc::no_such_file_or_directory) throw std::runtime_error("map metadata");
        return Result<bool>::Success(main && (hslk || map));
    } catch (...) {return Result<bool>::Failure("HLua","HLUA_PROJECT_IO","无法确认h-lua工程目录结构。");}
}
Result<ProjectConfiguration> ReadProjectConfiguration(const std::filesystem::path& project) {
    try {
        ProjectConfiguration result;
        const auto recognized=RecognizeProject(project);
        if(!recognized.Succeeded())return {std::nullopt,recognized.diagnostics};
        result.recognized=*recognized.value;
        if (!result.recognized) return Result<ProjectConfiguration>::Success(std::move(result));
        auto file = Services::ReadHLuaFolderConfiguration(project);
        if (!file.Succeeded()) return {std::nullopt, file.diagnostics};
        if (!*file.value) return Result<ProjectConfiguration>::Success(std::move(result));
        result.sha256 = (**file.value).sha256;
        const auto& bytes = *(**file.value).bytes;
        std::set<std::string> keys;
        const auto* text = reinterpret_cast<const char*>(bytes.data());
        auto json = nlohmann::json::parse(text, text + bytes.size(), [&](int depth, auto event, auto& value) {
            if (depth > 1 || (event == nlohmann::json::parse_event_t::key && !keys.insert(value.template get<std::string>()).second))
                throw std::runtime_error("configuration shape");
            return true;
        });
        if (!json.is_object() || json.size() != 2 || !json.contains("schema") || !json["schema"].is_number_integer()
            || json["schema"] != 1 || !json.contains("framework") || !json["framework"].is_string())
            throw std::runtime_error("configuration schema");
        const auto value = json["framework"].get<std::string>();
        if (value.size() > 4096 || value.find('\0') != value.npos) throw std::runtime_error("framework path");
        if (!value.empty()) {
            auto path = IO::PathFromUtf8(value);
            if (path.has_root_name() && !path.is_absolute()) throw std::runtime_error("drive relative framework");
            result.frameworkOverride = std::filesystem::absolute(path.is_relative() ? project / path : path).lexically_normal();
        }
        return Result<ProjectConfiguration>::Success(std::move(result));
    } catch (...) { return Result<ProjectConfiguration>::Failure("HLua", "HLUA_CONFIG_INVALID", "h-lua工程配置无效或来源不可确认，未忽略配置继续构建。"); }
}
Status ConfigureProjectFramework(const std::filesystem::path& project, const std::filesystem::path& framework, std::string_view expected) {
    try {
    const auto previous = ReadProjectConfiguration(project);
    if (!previous.Succeeded()) return {previous.diagnostics};
    if (!previous.value->recognized || previous.value->sha256 != expected)
        return Status::Failure("HLua", "HLUA_CONFIG_CONFLICT", "工程未识别或配置已改变，请重新读取。");
    if (!framework.empty()) {
        auto checked = HLuaFrameworkLocator::Capture({framework,{},{}});
        if (!checked.Succeeded()) return {std::move(checked.diagnostics)};
    }
    const auto path = framework.empty() ? std::filesystem::path{} : std::filesystem::absolute(framework).lexically_normal();
    const auto text = nlohmann::json{{"schema",1},{"framework",IO::GenericPathUtf8(path)}}.dump(2);
    return Services::WriteHLuaFolderConfiguration(project, {reinterpret_cast<const std::byte*>(text.data()), text.size()}, expected);
    } catch (...) { return Status::Failure("HLua", "HLUA_CONFIG_INVALID", "无法验证或写入h-lua工程配置；源工程未重新打包。"); }
}
Result<FrameworkLocation> ResolveProjectFramework(const std::filesystem::path& project, const FrameworkLocation& explicitLocation,
    const FrameworkLocation& applicationDefault) {
    const auto config = ReadProjectConfiguration(project);
    if (!config.Succeeded()) return {std::nullopt, config.diagnostics};
    if (Specified(explicitLocation)) return Result<FrameworkLocation>::Success(explicitLocation);
    if (!config.value->frameworkOverride.empty()) return Result<FrameworkLocation>::Success({config.value->frameworkOverride,{},{}});
    if (Specified(applicationDefault)) return Result<FrameworkLocation>::Success(applicationDefault);
    return Result<FrameworkLocation>::Failure("HLua", "HLUA_FRAMEWORK_UNSET", "请设置h-lua框架目录，或为当前工程指定覆盖目录。");
}
Status VerifyProjectConfiguration(const std::filesystem::path& project, const ProjectConfiguration& expected) {
    const auto current = ReadProjectConfiguration(project);
    if (!current.Succeeded()) return {current.diagnostics};
    if (current.value->recognized != expected.recognized || current.value->sha256 != expected.sha256)
        return Status::Failure("HLua", "HLUA_CONFIG_CONFLICT", "h-lua工程配置在操作期间改变，未发布结果。");
    return Status::Ok();
}
} // namespace War3Map::HLua
