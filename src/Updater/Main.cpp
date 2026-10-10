#include "War3MapCore/Update/UpdateInstaller.h"
#include "War3MapCore/IO/FileBytes.h"
#include "AudastraVersion.generated.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <iostream>
#include <map>
#include <set>
#include <Windows.h>

namespace {

using namespace War3Map;
using namespace War3Map::Update;
using Json = nlohmann::json;
bool showResult = false;
void ShowFailure(std::string_view text) {
    if (!showResult) return;
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return;
    std::wstring message(static_cast<std::size_t>(size), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), message.data(), size)) return;
    MessageBoxW(nullptr, message.c_str(), L"Audastra 更新未完成", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
}
std::string Utf8(std::wstring_view value) { return IO::PathUtf8(std::filesystem::path(value)); }
template<typename T>
T Integer(std::wstring_view value) {
    const auto text = Utf8(value);
    T number{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
        || text.front() < '0' || text.front() > '9') throw std::runtime_error("invalid integer");
    return number;
}
template<typename T>
int Errors(const Result<T>& result) {
    Json diagnostics = Json::array();
    for (const auto& diagnostic : result.diagnostics) diagnostics.push_back({{"code", diagnostic.code}, {"message", diagnostic.message}});
    std::cout << Json{{"installed", false}, {"diagnostics", std::move(diagnostics)}}.dump() << '\n';
    std::string message;
    for (const auto& diagnostic : result.diagnostics) message += diagnostic.code + ": " + diagnostic.message + "\n";
    ShowFailure(message + "\n请保留更新回执。若当前目录不可写，可将程序移到可写目录，或由你选择以管理员身份运行更新程序。");
    return 1;
}
int Emit(const Result<UpdateInstallReceipt>& result) {
    if (!result.Succeeded()) return Errors(result);
    const auto& receipt = *result.value;
    const auto outcome = receipt.outcome == UpdateInstallOutcome::Committed ? "committed"
        : receipt.outcome == UpdateInstallOutcome::RolledBack ? "rolled-back"
        : receipt.outcome == UpdateInstallOutcome::RollbackPending ? "rollback-pending" : "abandoned";
    Json child = nullptr;
    if (receipt.childProcess) child = {{"pid", receipt.childProcess->processId}, {"creation_time_100ns", receipt.childProcess->creationTime100ns}};
    std::cout << Json{{"installed", receipt.outcome == UpdateInstallOutcome::Committed}, {"outcome", outcome},
        {"transaction", receipt.transactionId}, {"receipt_path", IO::PathUtf8(receipt.transactionDirectory / L"journal.json")},
        {"transaction_directory", IO::PathUtf8(receipt.transactionDirectory)}, {"install_directory", IO::PathUtf8(receipt.installDirectory)},
        {"backup_directory", IO::PathUtf8(receipt.backupDirectory)}, {"failed_directory", IO::PathUtf8(receipt.failedDirectory)},
        {"child", std::move(child)}, {"health_verified", receipt.healthVerified}, {"user_files_preserved", receipt.userFilesPreserved},
        {"old_version_restarted", receipt.oldVersionRestarted}, {"reason_code", receipt.reasonCode}, {"reason", receipt.reason}}.dump() << '\n';
    if (receipt.outcome != UpdateInstallOutcome::Committed) {
        const auto state = receipt.outcome == UpdateInstallOutcome::RolledBack ? "旧版本已恢复。"
            : receipt.outcome == UpdateInstallOutcome::RollbackPending ? "新版本进程尚未自然退出，或恢复条件仍不齐。请保留事务目录，之后重新恢复。"
            : "安装未完成。";
        ShowFailure(std::string(state) + "\n" + receipt.reasonCode + ": " + receipt.reason
            + "\n\n回执：" + IO::PathUtf8(receipt.transactionDirectory / L"journal.json"));
    }
    return receipt.outcome == UpdateInstallOutcome::Committed ? 0 : receipt.outcome == UpdateInstallOutcome::RollbackPending ? 2 : 1;
}

int EmitCleanup(const Result<UpdateBackupCleanupReceipt>& result) {
    Json output{{"cleanup_succeeded", result.Succeeded()}};
    Json diagnostics = Json::array();
    std::string message;
    for (const auto& diagnostic : result.diagnostics) {
        diagnostics.push_back({{"code", diagnostic.code}, {"message", diagnostic.message}});
        message += diagnostic.code + ": " + diagnostic.message + "\n";
    }
    output["diagnostics"] = std::move(diagnostics);
    if (result.value) {
        const auto& receipt = *result.value;
        output["removed_backups"] = receipt.removedBackups;
        output["retained_backups"] = receipt.retainedBackups;
        output["retained_unknown"] = receipt.retainedUnknown;
        output["partially_cleaned_backups"] = receipt.partiallyCleanedBackups;
        output["files_marked_for_deletion"] = receipt.filesMarkedForDeletion;
        output["directories_marked_for_deletion"] = receipt.directoriesMarkedForDeletion;
        output["file_attribute_changes"] = receipt.fileAttributeChanges;
        auto partial = Json::array();
        for (const auto& directory : receipt.partialBackupDirectories) {
            const auto path = IO::PathUtf8(directory);
            partial.push_back(path);
            message += "部分清理备份：" + path + "\n";
        }
        output["partial_backup_directories"] = std::move(partial);
    }
    std::cout << output.dump() << '\n';
    if (!result.Succeeded()) ShowFailure(message + "\n请保留剩余备份和事务回执，清理未被报告为全部完成。");
    return result.Succeeded() ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::map<std::wstring, std::wstring> options;
        std::vector<std::string> arguments;
        bool recover = false, cleanup = false;
        const std::set<std::wstring> recognized{L"--install-dir", L"--package", L"--manifest", L"--signature", L"--current-version",
            L"--baseline-manifest", L"--baseline-signature", L"--parent-pid", L"--parent-created", L"--exe", L"--transaction-dir",
            L"--parent-timeout-ms", L"--health-timeout-ms"};
        for (int index = 1; index < argc; ++index) {
            const std::wstring name(argv[index]);
            if (name == L"--show-result") { if (showResult) throw std::runtime_error("duplicate display option"); showResult = true; continue; }
            if (name == L"--recover") { if (recover || cleanup) throw std::runtime_error("duplicate mode"); recover = true; continue; }
            if (name == L"--cleanup") { if (recover || cleanup) throw std::runtime_error("duplicate mode"); cleanup = true; continue; }
            if (index + 1 >= argc) throw std::runtime_error("missing value");
            if (name == L"--startup-argument") { arguments.push_back(Utf8(argv[++index])); continue; }
            if (!recognized.contains(name) || !options.emplace(name, argv[++index]).second) throw std::runtime_error("unknown argument");
        }
        const auto key = ParseTrustedUpdateKeyHex(BuildInfo::UpdatePublicKeyHex);
        if (!key.Succeeded()) return Errors(key);
        if (!key.value->configured) return Errors(Result<int>::Failure("Update", "UPDATE_TRUST_NOT_CONFIGURED", "尚未配置编译期可信发布公钥，拒绝更新。"));
        if (recover) {
            if (options.size() != 1 || !options.contains(L"--transaction-dir") || !arguments.empty()) throw std::runtime_error("recover arguments");
            return Emit(RecoverUpdate(std::filesystem::path(options.at(L"--transaction-dir")), *key.value));
        }
        if (cleanup) {
            if (options.size() != 1 || !options.contains(L"--install-dir") || !arguments.empty()) throw std::runtime_error("cleanup arguments");
            return EmitCleanup(CleanupUpdateBackups(std::filesystem::path(options.at(L"--install-dir")), *key.value));
        }
        for (const auto* name : {L"--install-dir", L"--package", L"--manifest", L"--signature", L"--current-version"})
            if (!options.contains(name)) throw std::runtime_error("required install argument");
        if (options.contains(L"--transaction-dir") || options.contains(L"--parent-pid") != options.contains(L"--parent-created"))
            throw std::runtime_error("install argument pairing");
        UpdateInstallRequest request;
        request.installDirectory = options.at(L"--install-dir"); request.packagePath = options.at(L"--package");
        request.manifestPath = options.at(L"--manifest"); request.signaturePath = options.at(L"--signature");
        request.currentVersion = Utf8(options.at(L"--current-version")); request.startupArguments = std::move(arguments);
        if (options.contains(L"--baseline-manifest")) request.baselineManifestPath = options.at(L"--baseline-manifest");
        if (options.contains(L"--baseline-signature")) request.baselineSignaturePath = options.at(L"--baseline-signature");
        if (options.contains(L"--exe")) request.executableRelativePath = Utf8(options.at(L"--exe"));
        if (options.contains(L"--parent-pid")) request.parentProcess = UpdateProcessIdentity{
            Integer<std::uint32_t>(options.at(L"--parent-pid")), Integer<std::uint64_t>(options.at(L"--parent-created"))};
        if (options.contains(L"--parent-timeout-ms")) request.parentExitTimeoutMs = Integer<std::uint32_t>(options.at(L"--parent-timeout-ms"));
        if (options.contains(L"--health-timeout-ms")) request.healthTimeoutMs = Integer<std::uint32_t>(options.at(L"--health-timeout-ms"));
        return Emit(InstallUpdate(request, *key.value));
    } catch (const std::exception&) {
        return Errors(Result<int>::Failure("Update", "UPDATE_UPDATER_ARGUMENT_INVALID", "更新程序参数无效、未知、重复或未成对；不接受外部公钥/跳过校验。"));
    }
}
