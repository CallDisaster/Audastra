#include "aj/WindowsLoader.h"
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <fstream>
#include <iterator>

using namespace audastra::japi;
namespace {
int failures = 0;
void Check(bool ok, const char* text) { if (!ok) { std::printf("FAIL: %s\n", text); ++failures; } }
ModuleContract Demo() {
    return {"demo", "2.0.0", {{"demo.add", "ii", AJ_INT, AJ_SYNC_COMMAND},
        {"demo.echo", "s", AJ_TEXT, AJ_LOCAL_COMMAND}, {"demo.notify", "s", AJ_VOID, AJ_LOCAL_COMMAND}}};
}
}
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const auto path = std::filesystem::absolute(argv[1]);
    std::ifstream stream(path, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
    Check(!bytes.empty(), "fresh sample DLL exists");
    Check(Sha256(std::span<const uint8_t>()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 known vector");
    auto loader = std::make_shared<PinnedDllLoader>(path.parent_path(), std::vector<ModulePin>{{Demo(), Sha256(bytes)}}, path.parent_path());
    Check(loader->Open("../demo").status == AJ_ARGUMENT, "path traversal rejected");
    Check(loader->Open("untrusted").status == AJ_POLICY, "no pin default deny");
    if (sizeof(void*) != 4) {
        Check(loader->Open("demo").status == AJ_UNSUPPORTED, "x64 host cannot load game DLL");
        std::printf("x64 loader policy: %d failures (x86 load intentionally unsupported)\n", failures);
        return failures ? 1 : 0;
    }
    Check(InspectPe32(bytes), "fresh C module PE32/export preflight");
    auto invalid = bytes; invalid[0] = 0;
    Check(!InspectPe32(invalid) && !InspectPe32(std::span(bytes).first(30)), "malformed/truncated PE rejected");
    invalid = bytes;
    uint32_t nt = 0; std::memcpy(&nt, invalid.data() + 0x3c, 4);
    invalid[nt + 4] = 0x64; invalid[nt + 5] = 0x86;
    Check(!InspectPe32(invalid), "x64 PE rejected");
    PinnedDllLoader denied(path.parent_path(), {{Demo(), std::string(64, '0')}}, path.parent_path());
    Check(denied.Open("demo").status == AJ_POLICY, "wrong digest rejected before LoadLibrary");
    Host host({}, loader);
    const auto loaded = host.Load("demo");
    if (loaded != AJ_OK) std::printf("load status=%d diagnostic=%s\n", loaded, host.Diagnostic().c_str());
    Check(loaded == AJ_OK && host.Load("demo") == AJ_OK && host.ModuleCount() == 1, "real DLL load and idempotence");
    auto reply = host.Dispatch("ajapi:2;1;1;demo.add;i:20;i:22");
    Check(reply.status == AJ_OK && reply.value.integer == 42, "wire to real DLL to owned response");
    Check(host.Dispatch("ajapi:2;1;2;demo.add;i:2147483647;i:1").status == AJ_LIMIT, "sample checked arithmetic");
    Check(host.Dispatch("ajapi:2;1;3;demo.echo;s:hello%3Bworld").value.text == "hello;world", "real DLL string result");
    Check(host.Unload("demo") == AJ_OK && host.ModuleCount() == 0, "real DLL stopped/destroyed/unloaded");
    Check(host.Load("demo") == AJ_OK && host.Close() == AJ_OK, "independent staged reload");
    const auto workerPath = std::filesystem::absolute(argv[2]);
    std::ifstream workerStream(workerPath, std::ios::binary);
    std::vector<uint8_t> workerBytes((std::istreambuf_iterator<char>(workerStream)), {});
    ModuleContract workerContract{"worker", "2.0.0", {{"worker.begin", "", AJ_VOID, AJ_LOCAL_COMMAND},
        {"worker.count", "", AJ_INT, AJ_LOCAL_COMMAND}}};
    auto workers = std::make_shared<PinnedDllLoader>(workerPath.parent_path(),
        std::vector<ModulePin>{{workerContract, Sha256(workerBytes)}}, workerPath.parent_path());
    Host threaded({}, workers);
    uint32_t request = 0;
    for (int cycle = 0; cycle < 5; ++cycle) {
        Check(threaded.Load("worker") == AJ_OK, "real worker DLL loaded");
        auto call = [&](const char* command) {
            return threaded.Dispatch("ajapi:2;1;" + std::to_string(++request) + ';' + command);
        };
        Check(call("worker.begin").status == AJ_OK, "real worker begins only after publication");
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        int32_t produced = 0;
        do {
            produced = call("worker.count").value.integer;
            if (produced == 1000) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        Check(produced == 1000, "1000 actual worker event attempts completed");
        const auto stats = call("module.stats;s:worker");
        Check(stats.status == AJ_OK && stats.value.text.find("|256|744|0|0|0") != std::string::npos,
            "actual worker admission: 256 accepted, 744 explicitly rejected, none silently evicted");
        const auto stop = threaded.Unload("worker");
        Check(stop == AJ_OK || stop == AJ_BUSY, "nonblocking real worker stop");
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (threaded.ModuleCount() && std::chrono::steady_clock::now() < deadline) {
            threaded.Pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Check(threaded.ModuleCount() == 0, "worker exit observed before final unload");
    }
    Check(threaded.Close() == AJ_OK, "threaded host quiescent");
    std::printf("x86 pinned DLL integration: %d failures\n", failures);
    return failures ? 1 : 0;
}
