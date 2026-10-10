#include "War3MapCore/Update/UpdateArchive.h"
#include "War3MapCore/IO/FileBytes.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <map>

// 仅开发测试工具：调用正式Core验证真实打包脚本的输出。
// 不注册到产品CLI，不随便携包分发；外部公钥参数不能构成产品信任配置。
int wmain(int argc, wchar_t** argv) {
    using namespace War3Map;
    using namespace War3Map::Update;
    nlohmann::json output{{"verified", false}, {"version", ""}, {"files", 0},
        {"diagnostics", nlohmann::json::array()}};
    const auto finish = [&](const auto& result) {
        for (const auto& diagnostic : result.diagnostics) {
            output["diagnostics"].push_back({{"code", diagnostic.code}, {"message", diagnostic.message}});
        }
        std::cout << output.dump() << '\n';
        return 1;
    };
    try {
        std::map<std::wstring, std::filesystem::path> options;
        for (int index = 1; index < argc; index += 2) {
            const std::wstring name(argv[index]);
            if (index + 1 >= argc || (name != L"--manifest" && name != L"--signature"
                && name != L"--package" && name != L"--public-key-file")
                || !options.emplace(name, argv[index + 1]).second) {
                return finish(Status::Failure("UpdateTest", "UPDATE_TEST_ARGUMENTS", "测试探针参数未知、缺值或重复。"));
            }
        }
        if (options.size() != 4) return finish(Status::Failure("UpdateTest", "UPDATE_TEST_ARGUMENTS",
            "测试探针需要manifest、signature、package、public-key-file四个参数。"));
        const auto keyBytes = IO::ReadFileBytes(options.at(L"--public-key-file"), 128);
        if (!keyBytes.Succeeded()) return finish(keyBytes);
        std::string keyText(reinterpret_cast<const char*>(keyBytes.value->data()), keyBytes.value->size());
        while (!keyText.empty() && (keyText.back() == '\r' || keyText.back() == '\n')) keyText.pop_back();
        const auto key = ParseTrustedUpdateKeyHex(keyText);
        if (!key.Succeeded()) return finish(key);
        const auto raw = IO::ReadFileBytes(options.at(L"--manifest"), kMaxUpdateManifestBytes);
        if (!raw.Succeeded()) return finish(raw);
        const auto signature = IO::ReadFileBytes(options.at(L"--signature"), 64);
        if (!signature.Succeeded()) return finish(signature);
        const auto manifest = VerifyAndParseUpdateManifest(*raw.value, *signature.value, *key.value);
        if (!manifest.Succeeded()) return finish(manifest);
        output["version"] = manifest.value->Manifest().version;
        output["files"] = manifest.value->Manifest().files.size();
        const auto package = VerifyUpdatePackage(*manifest.value, options.at(L"--package"));
        if (!package.Succeeded()) return finish(package);
        output["verified"] = true;
        std::cout << output.dump() << '\n';
        return 0;
    } catch (const std::exception&) {
        return finish(Status::Failure("UpdateTest", "UPDATE_TEST_PROBE_FAILED", "测试探针无法完整验证输入。"));
    }
}
