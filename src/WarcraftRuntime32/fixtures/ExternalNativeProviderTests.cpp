#include "../RuntimeNativeProviders.h"
#include "../RuntimeFiles.h"

#include <iostream>
#include <memory>
#include <stdexcept>

using namespace Audastra::Runtime32;
namespace {
void Check(bool value, const char* detail) {
    if (!value) throw std::runtime_error(detail);
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        Check(argc == 2, "fixture DLL path required");
        const auto path = std::filesystem::absolute(argv[1]);
        Handle file(OpenLockedFile(path));
        Check(file.value != INVALID_HANDLE_VALUE && IsX86Image(file.value), "original x86 fixture");
        const auto sha = Sha256(file.value);
        Check(sha.size() == 64, "fixture SHA");
        auto evidence = std::make_unique<SharedEvidence>();
        evidence->providerModuleCount = 1;
        evidence->providerNativeCount = 3;
        auto& module = evidence->providerModules.front();
        wcscpy_s(module.file.path, path.c_str());
        strcpy_s(module.file.expectedSha256, sha.c_str());
        strcpy_s(module.entry, "RegisterFixture");
        const std::array names{"AudastraFixtureAdd", "AudastraFixturePlayer", "AudastraFixtureCallback"};
        const std::array signatures{"(I)I", "(Hplayer;)Hplayer;", "(C)I"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            strcpy_s(evidence->providerNatives[i].name, names[i]);
            strcpy_s(evidence->providerNatives[i].signature, signatures[i]);
        }
        RuntimeNativeProviders provider(*evidence);
        const auto load = provider.Load();
        if (!load.empty()) throw std::runtime_error(load);
        Check(module.file.loaded && module.file.actualSha256 == sha, "loaded original by declared identity");
        const auto hooks = provider.Hooks();
        Check(!hooks.validate(reinterpret_cast<std::uintptr_t>(&Check), names[0], signatures[0]).empty(),
            "reject addresses outside the declared provider");
        const auto run = [&](std::string& rejected, OwnLua::NativeRegistry& registry) {
            return hooks.registerAll([&](auto address, auto name, auto signature) {
                rejected = hooks.validate(address, name, signature);
                if (rejected.empty()) Check(registry.Observe(name, signature, address, true), "store actual observed registration");
            });
        };
        OwnLua::NativeRegistry positive;
        std::string rejected;
        Check(run(rejected, positive).empty() && rejected.empty() && hooks.verify(positive).empty(),
            "actual provider entry registers all expected natives with observed signatures");
        Check(evidence->providerRegistrationsVerified == 1 && positive.Size() == 3,
            "positive registration witness");

        evidence->providerRegistrationsVerified = 0;
        strcpy_s(module.entry, "RegisterFixtureWrongSignature");
        OwnLua::NativeRegistry mismatch;
        Check(run(rejected, mismatch).empty() && rejected.find(names[0]) != std::string::npos &&
            rejected.find("(I)B") != std::string::npos, "actual entry signature mismatch is diagnosed");
        Check(!hooks.verify(mismatch).empty() && evidence->providerRegistrationsVerified == 0,
            "mismatched registration cannot yield a verified receipt");

        strcpy_s(module.entry, "RegisterFixtureMissing");
        OwnLua::NativeRegistry absent;
        rejected.clear();
        Check(run(rejected, absent).empty() && rejected.empty() &&
            hooks.verify(absent).find(names[0]) != std::string::npos && evidence->providerRegistrationsVerified == 0,
            "success returned by the entry cannot conceal absent registrations");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
