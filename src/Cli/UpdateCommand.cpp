#include "UpdateCommand.h"

#include "AudastraVersion.generated.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Update/UpdateArchive.h"
#include "War3MapCore/Update/UpdatePlan.h"
#include "War3MapCore/Update/UpdateInstaller.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <map>
#include <optional>
#include <set>

namespace {
using namespace War3Map;
using namespace War3Map::Update;

int Invalid(const char* reason) {
    std::cerr << "Update.Cli: UPDATE_ARGUMENT_INVALID: " << reason << '\n';
    return 2;
}
void Diagnostics(const std::vector<Diagnostic>& values) {
    for (const auto& value : values) std::cerr << value.subsystem << ": " << value.code << ": " << value.message << '\n';
}
std::string Utf8(std::wstring_view value) {
    const auto bytes = std::filesystem::path(value).u8string();
    return {bytes.begin(), bytes.end()};
}
Result<VerifiedUpdateManifest> ReadVerified(
    const std::filesystem::path& manifestPath, const std::filesystem::path& signaturePath, const TrustedUpdateKey& key) {
    auto manifest = IO::ReadFileBytes(manifestPath, kMaxUpdateManifestBytes);
    if (!manifest.Succeeded()) return {{}, std::move(manifest.diagnostics)};
    auto signature = IO::ReadFileBytes(signaturePath, 64);
    if (!signature.Succeeded()) return {{}, std::move(signature.diagnostics)};
    return VerifyAndParseUpdateManifest(*manifest.value, *signature.value, key);
}
const char* Action(UpdateFileAction value) {
    switch (value) {
    case UpdateFileAction::CreateProgramFile: return "create_program_file";
    case UpdateFileAction::ReplaceProgramFile: return "replace_program_file";
    case UpdateFileAction::PreserveUserFile: return "preserve_user_file";
    case UpdateFileAction::PreserveObsoleteProgramFile: return "preserve_obsolete_program_file";
    }
    return "unknown";
}
} // namespace

int RunUpdateCommand(int argc, wchar_t** argv) {
    try {
        if (argc < 2 || argc > 24) return Invalid("参数数量无效。");
        const std::wstring command = argv[1];
        const bool apply = command == L"update-apply";
        const std::set<std::wstring> allowed = {L"--manifest", L"--signature", L"--package", L"--install-dir",
            L"--baseline-manifest", L"--baseline-signature", L"--current-version", L"--channel", L"--exe"};
        std::map<std::wstring, std::wstring> options;
        bool dryRun = false;
        for (int i = 2; i < argc; ++i) {
            const std::wstring name = argv[i];
            if (apply && name == L"--dry-run") {
                if (dryRun) return Invalid("重复的 --dry-run。");
                dryRun = true;
                continue;
            }
            if (!allowed.contains(name) || options.contains(name) || i + 1 >= argc || !*argv[i + 1])
                return Invalid("未知、重复或缺值的参数。");
            options.emplace(name, argv[++i]);
        }
        if (!options.contains(L"--manifest") || !options.contains(L"--signature"))
            return Invalid("需要 --manifest 和 --signature；更新包不能提供可信公钥。");
        if (options.contains(L"--baseline-manifest") != options.contains(L"--baseline-signature"))
            return Invalid("旧清单与签名必须一起提供。");
        if (apply && (!options.contains(L"--package") || !options.contains(L"--install-dir")))
            return Invalid("update-apply 需要 --package 和 --install-dir。");
        if (!apply && (options.contains(L"--install-dir") || options.contains(L"--baseline-manifest") || options.contains(L"--exe")))
            return Invalid("update-check 不接受安装计划参数。");
        const auto channelText = options.contains(L"--channel") ? options.at(L"--channel") : L"stable";
        if (channelText != L"stable" && channelText != L"beta") return Invalid("channel 必须是 stable 或 beta。");
        const auto channel = channelText == L"beta" ? UpdateChannel::Beta : UpdateChannel::Stable;
        const auto currentVersion = options.contains(L"--current-version")
            ? Utf8(options.at(L"--current-version")) : std::string(BuildInfo::Version);
        auto key = ParseTrustedUpdateKeyHex(BuildInfo::UpdatePublicKeyHex);
        if (!key.Succeeded()) { Diagnostics(key.diagnostics); return 1; }
        auto verified = ReadVerified(options.at(L"--manifest"), options.at(L"--signature"), *key.value);
        if (!verified.Succeeded()) { Diagnostics(verified.diagnostics); return 1; }
        auto check = CheckUpdate(*verified.value, currentVersion, channel);
        if (!check.Succeeded()) { Diagnostics(check.diagnostics); return 1; }
        nlohmann::json response = {{"product", "Audastra"}, {"current_version", check.value->currentVersion},
            {"target_version", check.value->targetVersion}, {"update_available", check.value->updateAvailable},
            {"directly_upgradeable", check.value->directlyUpgradeable}, {"reason", check.value->reason},
            {"manifest_sha256", verified.value->ManifestSha256()}, {"signature_verified", true}, {"installed", false}};
        if (options.contains(L"--package")) {
            auto package = VerifyUpdatePackage(*verified.value, options.at(L"--package"));
            if (!package.Succeeded()) { Diagnostics(package.diagnostics); return 1; }
            response["package_verified"] = true;
            response["package_sha256"] = package.value->sha256;
        }
        if (apply) {
            std::optional<VerifiedUpdateManifest> baseline;
            if (options.contains(L"--baseline-manifest")) {
                auto old = ReadVerified(options.at(L"--baseline-manifest"), options.at(L"--baseline-signature"), *key.value);
                if (!old.Succeeded()) { Diagnostics(old.diagnostics); return 1; }
                baseline = std::move(*old.value);
            }
            auto plan = BuildUpdateInstallPlan(*verified.value, baseline ? &*baseline : nullptr,
                options.at(L"--install-dir"), currentVersion);
            if (!plan.Succeeded()) { Diagnostics(plan.diagnostics); return 1; }
            response["inventory_sha256"] = plan.value->inventorySha256;
            response["baseline_manifest_sha256"] = plan.value->baselineManifestSha256;
            response["files"] = nlohmann::json::array();
            for (const auto& file : plan.value->files) response["files"].push_back({{"path", file.path},
                {"action", Action(file.action)}, {"current_bytes", file.currentBytes},
                {"current_sha256", file.currentSha256}, {"target_bytes", file.targetBytes}, {"target_sha256", file.targetSha256}});
            response["dry_run"] = dryRun;
            if (!dryRun) {
                UpdateInstallRequest request;
                request.installDirectory = options.at(L"--install-dir");
                request.packagePath = options.at(L"--package");
                request.manifestPath = options.at(L"--manifest");
                request.signaturePath = options.at(L"--signature");
                request.currentVersion = currentVersion;
                if (options.contains(L"--exe")) request.executableRelativePath = Utf8(options.at(L"--exe"));
                if (options.contains(L"--baseline-manifest")) {
                    request.baselineManifestPath = options.at(L"--baseline-manifest");
                    request.baselineSignaturePath = options.at(L"--baseline-signature");
                }
                const auto installed = InstallUpdate(request, *key.value);
                if (!installed.Succeeded()) { Diagnostics(installed.diagnostics); return 1; }
                const auto& receipt = *installed.value;
                response["installed"] = receipt.outcome == UpdateInstallOutcome::Committed;
                response["outcome"] = receipt.outcome == UpdateInstallOutcome::Committed ? "committed"
                    : receipt.outcome == UpdateInstallOutcome::RolledBack ? "rolled-back"
                    : receipt.outcome == UpdateInstallOutcome::RollbackPending ? "rollback-pending" : "abandoned";
                response["transaction_directory"] = IO::PathUtf8(receipt.transactionDirectory);
                response["receipt_path"] = IO::PathUtf8(receipt.transactionDirectory / L"journal.json");
                response["health_verified"] = receipt.healthVerified;
                response["user_files_preserved"] = receipt.userFilesPreserved;
                response["reason_code"] = receipt.reasonCode;
                response["reason"] = receipt.reason;
                std::cout << response.dump() << '\n';
                return receipt.outcome == UpdateInstallOutcome::Committed ? 0
                    : receipt.outcome == UpdateInstallOutcome::RollbackPending ? 2 : 1;
            }
        }
        std::cout << response.dump() << '\n';
        return 0;
    } catch (const std::exception&) {
        std::cerr << "Update.Cli: UPDATE_COMMAND_FAILED: 更新请求未完成。\n";
        return 1;
    }
}
