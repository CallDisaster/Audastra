#include "War3MapCore/Script/ScriptPipeline.h"
#include "War3MapCore/Services/MapRuntimePackage.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Hash.h"
#include <iostream>
#include <stdexcept>

using namespace War3Map;
int wmain(int argc, wchar_t** argv) {
    try {
        const auto require = [](bool value, const char* text) { if (!value) throw std::runtime_error(text); };
        require(argc == 2 || argc == 4, "bundle root, optionally frozen package root and manifest SHA");
        const auto root = std::filesystem::path(argv[1]) / "AudastraUI/jass";
        const auto bytes = IO::ReadFileBytes(root / "AudastraUI.j", 2 * 1024 * 1024);
        require(bytes.Succeeded(), "read actual generated bundle");
        const auto hash = Sha256(*bytes.value); require(hash.Succeeded(), "bundle identity");
        std::vector<Diagnostic> diagnostics;
        const auto table = Script::ScanInjectTable({root}, false, diagnostics);
        std::vector<std::filesystem::path> files; std::vector<std::string> hashes;
        const auto output = Script::InjectCode("function main takes nothing returns nothing\nif AudastraUIIsReady() then\nendif\nendfunction\n",
            table, diagnostics, &files, &hashes);
        require(files.size() == 1 && hashes == std::vector<std::string>{*hash.value}, "consumer records exactly the generated client bytes");
        require(output.find("audastraui.") != output.npos && output.find("bindtable") != output.npos, "current combined UI client consumed");
        const std::string unused = "function main takes nothing returns nothing\nendfunction\n";
        std::vector<std::filesystem::path> explicitFiles; std::vector<std::string> explicitHashes;
        constexpr std::string_view required[]{"AudastraUIIsReady"};
        const auto explicitOutput = Script::InjectCode(unused, table, diagnostics, &explicitFiles, &explicitHashes, required);
        require(explicitFiles.size() == 1 && explicitHashes == hashes && explicitOutput.starts_with(unused),
            "explicit dependency consumes exact current client once without adding a fake author call");
        std::vector<std::filesystem::path> unusedFiles;
        require(Script::InjectCode(unused, table, diagnostics, &unusedFiles) == unused && unusedFiles.empty(),
            "unused automatic input remains unchanged");
        if (argc == 4) {
            const std::wstring identity(argv[3]);
            const Services::MapRuntimePackageSource source{argv[2], std::string(identity.begin(), identity.end()), false};
            const auto package = Services::MapRuntimePackage::Capture(source, Services::MapRuntimeSavePurpose::InternalValidation);
            require(package.Succeeded(), "frozen internal package validates");
            require((*package.value)->VerifyScriptInputs(hashes).Succeeded(), "frozen package accepts actual consumed bundle");
            require(!(*package.value)->VerifyScriptInputs({}).Succeeded(), "missing client refused");
            hashes.push_back(hashes.front());
            require(!(*package.value)->VerifyScriptInputs(hashes).Succeeded(), "duplicate client refused");
        }
        std::cout << "consumed_client_sha256=" << *hash.value << "\ncurrent bundle and frozen input checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
