#include "AudastraVersion.generated.h"
#include "War3MapCore/Update/UpdateInstaller.h"
#include "War3MapCore/IO/FileBytes.h"

#include <nlohmann/json.hpp>
#include <Windows.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::Update;
using Json = nlohmann::json;

std::string Utf8(const std::wstring& value) { return IO::PathUtf8(std::filesystem::path(value)); }
std::string Message(const std::vector<Diagnostic>& diagnostics) {
    std::string result;
    for (const auto& diagnostic : diagnostics) result += diagnostic.code + ": " + diagnostic.message + "\n";
    return result;
}
bool Record(const std::filesystem::path& directory, const Json& record) {
    if (directory.empty()) return true;
    const auto raw = record.dump() + "\n";
    const auto path = directory / (L"fixture-" + std::to_wstring(GetCurrentProcessId()) + L".json");
    // 父测试显式创建独占TEMP下的receipts目录；夹具不写安装树、不读取作者数据。
    const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool success = WriteFile(handle, raw.data(), static_cast<DWORD>(raw.size()), &written, nullptr)
        && written == raw.size() && FlushFileBuffers(handle);
    CloseHandle(handle);
    return success;
}
Result<std::filesystem::path> InstallRoot() {
    std::array<wchar_t, 32768> path{};
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return Result<std::filesystem::path>::Failure(
        "UpdateFixture", "FIXTURE_SELF_PATH", "无法定位原创夹具路径。");
    return Result<std::filesystem::path>::Success(std::filesystem::path(std::wstring(path.data(), count)).parent_path().parent_path());
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::map<std::wstring, std::wstring> values;
        std::set<std::wstring> flags;
        const std::set<std::wstring> recognized{L"--update-health-transaction", L"--update-health-challenge", L"--update-health-pipe",
            L"--update-error", L"--fixture-receipts", L"--fixture-parent-exit-event"};
        for (int index = 1; index < argc; ++index) {
            const std::wstring name(argv[index]);
            if (name == L"--fixture-fail" || name == L"--fixture-cleanup") {
                if (!flags.insert(name).second) return 3;
            } else {
                if (!recognized.contains(name) || index + 1 >= argc || !values.emplace(name, argv[++index]).second) return 3;
            }
        }
        const bool health = values.contains(L"--update-health-transaction") || values.contains(L"--update-health-challenge")
            || values.contains(L"--update-health-pipe");
        const auto identity = ReadUpdateProcessIdentity();
        if (!identity.Succeeded()) return 3;
        const auto receipts = values.contains(L"--fixture-receipts") ? std::filesystem::path(values.at(L"--fixture-receipts")) : std::filesystem::path{};
        Json record{{"format", "audastra-original-update-fixture"}, {"version", BuildInfo::Version}, {"commit", BuildInfo::Commit},
            {"pid", identity.value->processId}, {"creation_time_100ns", identity.value->creationTime100ns}, {"health_requested", health}};

        if (health) {
            if (!values.contains(L"--update-health-transaction") || !values.contains(L"--update-health-challenge")
                || !values.contains(L"--update-health-pipe")) return 3;
            if (flags.contains(L"--fixture-fail")) {
                record["kind"] = "natural-health-failure"; record["exit_code"] = 4;
                if (!Record(receipts, record)) return 3;
                std::cout << record.dump() << '\n';
                return 4; // 原创新版主动自然退出，更新器必须回滚；从不结束其他进程。
            }
            const auto text = Utf8(values.at(L"--update-health-pipe"));
            std::uintptr_t pipe = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), pipe);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !pipe) return 3;
            const auto reply = ReplyUpdateHealth(pipe, Utf8(values.at(L"--update-health-transaction")),
                Utf8(values.at(L"--update-health-challenge")), BuildInfo::Version, BuildInfo::Commit);
            record["kind"] = "health-reply"; record["health_reply_succeeded"] = reply.Succeeded();
            record["exit_code"] = reply.Succeeded() ? 0 : 3;
            if (!reply.Succeeded()) record["error"] = Message(reply.diagnostics);
            if (!Record(receipts, record)) return 3;
            std::cout << record.dump() << '\n';
            // 真实reply后短驻留，让安装器能观察真实活动子进程，再正常返回。
            Sleep(250);
            return reply.Succeeded() ? 0 : 3;
        }

        if (values.contains(L"--fixture-parent-exit-event")) {
            const auto event = OpenEventW(SYNCHRONIZE, FALSE, values.at(L"--fixture-parent-exit-event").c_str());
            if (!event) return 3;
            record["kind"] = "parent-natural-exit";
            const auto wait = WaitForSingleObject(event, 30000); CloseHandle(event);
            record["exit_code"] = wait == WAIT_OBJECT_0 ? 0 : 5;
            if (!Record(receipts, record)) return 3;
            std::cout << record.dump() << '\n';
            return wait == WAIT_OBJECT_0 ? 0 : 5;
        }

        if (flags.contains(L"--fixture-cleanup")) {
            const auto root = InstallRoot(); const auto key = ParseTrustedUpdateKeyHex(BuildInfo::UpdatePublicKeyHex);
            if (!root.Succeeded() || !key.Succeeded()) return 3;
            const auto cleanup = CleanupUpdateBackups(*root.value, *key.value);
            record["kind"] = "next-start-cleanup"; record["cleanup_succeeded"] = cleanup.Succeeded();
            if (cleanup.Succeeded()) {
                record["removed_backups"] = cleanup.value->removedBackups;
                record["retained_backups"] = cleanup.value->retainedBackups;
                record["retained_unknown"] = cleanup.value->retainedUnknown;
            } else record["error"] = Message(cleanup.diagnostics);
            record["exit_code"] = cleanup.Succeeded() ? 0 : 3;
            if (!Record(receipts, record)) return 3;
            std::cout << record.dump() << '\n';
            return cleanup.Succeeded() ? 0 : 3;
        }

        // 旧版rollback restart带--fixture-fail但没有health，必须正常返回，不再次假失败。
        record["kind"] = "ordinary-start"; record["exit_code"] = 0;
        record["rollback_reason_present"] = values.contains(L"--update-error");
        if (!Record(receipts, record)) return 3;
        std::cout << record.dump() << '\n';
        return 0;
    } catch (const std::exception&) { return 3; }
}
