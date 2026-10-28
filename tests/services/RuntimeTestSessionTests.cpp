#include "War3MapCore/Services/RuntimeTestSession.h"
#include "../../src/WarcraftRuntime32/Japi/RuntimeTestWriter.h"
#include <windows.h>
#include <iostream>
#include <stdexcept>
using namespace War3Map;
using namespace Audastra::Runtime32;
using Audastra::Runtime32::Japi::RuntimeTestWriter;
namespace {
void Check(bool v, const char* text) { if (!v) throw std::runtime_error(text); }
struct Mapping {
    HANDLE handle = nullptr; RuntimeTestEvidence* value = nullptr;
    explicit Mapping(const std::string& nonce) {
        const auto name = std::wstring(kRuntimeTestMappingPrefix) + std::wstring(nonce.begin(), nonce.end());
        handle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.c_str());
        if (handle) value = static_cast<RuntimeTestEvidence*>(MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(RuntimeTestEvidence)));
        Check(value != nullptr, "test opens only its own named evidence mapping");
    }
    ~Mapping() { if (value) UnmapViewOfFile(value); if (handle) CloseHandle(handle); }
};
}
int main() {
    try {
        FILETIME born{}, exited{}, kernel{}, user{}; Check(GetProcessTimes(GetCurrentProcess(), &born, &exited, &kernel, &user), "original fixture process creation identity");
        const auto birth = (static_cast<std::uint64_t>(born.dwHighDateTime) << 32) | born.dwLowDateTime;
        const auto made = Services::RuntimeTestSession::Create(std::string(64, 'a'), [birth](auto id, auto created) { return id == GetCurrentProcessId() && created == birth; });
        Check(made.Succeeded(), "create test session"); const auto session = *made.value;
        Check(session->Query().value->state == "waiting_for_module", "absence is not load or success");
        Mapping mapping(session->Nonce());
        SetEnvironmentVariableW(kRuntimeTestEnvironment, nullptr);
        { RuntimeTestWriter absent; absent.Loaded(); absent.Installed(0, std::string(64, 'b'));
            Check(!absent.Report(1, 3, 0, std::string(64, 'b')) && !mapping.value->pid, "ordinary player has no evidence writes"); }
        const auto nonce = std::wstring(session->Nonce().begin(), session->Nonce().end());
        SetEnvironmentVariableW(kRuntimeTestEnvironment, nonce.c_str());
        RuntimeTestWriter runtime; runtime.Loaded(); runtime.Installed(0, std::string(64, 'b'));
        Check(session->Query().value->state == "module_loaded", "load alone is not hello");
        Check(!runtime.Report(1, 3, 0, std::string(64, 'b')), "no ack before successful hello");
        runtime.Hello(1);
        Check(!runtime.Report(1, 3, 0, std::string(64, 'c')), "different identity cannot claim success");
        Check(!runtime.Report(2, 3, 0, std::string(64, 'b')), "old epoch refused");
        Check(runtime.Report(1, 3, 0, std::string(64, 'b')), "SDK identity acknowledgement");
        auto ready = session->Query();
        Check(ready.Succeeded() && ready.value->handshakeSuccess && ready.value->reportMs && ready.value->processCreated
            && ready.value->mapSha256 == std::string(64, 'a') && ready.value->epoch == 1, "bounded external receipt ties process map and VM");
        mapping.value->pid = 0; Check(!session->Query().Succeeded(), "missing verified process cannot claim handshake success"); mapping.value->pid = GetCurrentProcessId();
        mapping.value->epoch = 0; Check(!session->Query().Succeeded(), "missing VM epoch cannot claim handshake success"); mapping.value->epoch = 1;
        runtime.Installed(14, {});
        Check(session->Query().value->state == "initialization_rejected" && !session->Query().value->handshakeSuccess,
            "later policy rejection cannot borrow an old successful acknowledgement");
        runtime.Reset(2); Check(!session->Query().value->handshakeSuccess, "VM reset clears prior success");
        runtime.Installed(0, std::string(64, 'b'));
        runtime.Hello(2); Check(runtime.Report(2, -3, 14, std::string(64, 'c')), "explicit identity refusal");
        Check(session->Query().value->state == "identity_rejected", "external refusal");
        runtime.Reset(3); runtime.Hello(3); Check(runtime.Report(3, -2, 8, std::string(64, 'b')), "explicit SDK timeout");
        Check(session->Query().value->state == "handshake_timeout" && !session->Query().value->hostDeadline, "SDK timeout distinguished");
        runtime.Reset(4); mapping.value->loadedMs = GetTickCount64() - 36000;
        Check(session->Query().value->hostDeadline && session->Query().value->state == "handshake_timeout", "missing bridge produces labelled host deadline");
        mapping.value->pid ^= 0x40000000u; Check(!session->Query().Succeeded(), "foreign process refused"); mapping.value->pid = GetCurrentProcessId();
        mapping.value->createdLow ^= 1; Check(!session->Query().Succeeded(), "reused process identity refused"); mapping.value->createdLow ^= 1;
        mapping.value->mapSha256[0] = 'c'; Check(!session->Query().Succeeded(), "wrong map refused"); mapping.value->mapSha256[0] = 'a';
        mapping.value->revision |= 1; Check(!session->Query().Succeeded(), "torn write not success"); mapping.value->revision += 1;
        SetEnvironmentVariableW(kRuntimeTestEnvironment, L"invalid");
        { RuntimeTestWriter invalid; invalid.Loaded(); Check(!invalid.Report(4, 3, 0, std::string(64, 'b')), "invalid environment cannot attach"); }
        SetEnvironmentVariableW(kRuntimeTestEnvironment, nullptr);
        std::cout << "runtime test evidence boundaries passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
