#include "War3MapCore/Services/MapRuntimePackage.h"
#include "War3MapCore/VerifiedFile.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Hash.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

// 内部开发工具，不安装、不提供MCP入口。所有载荷只交共用文件服务核对并原样复制，绝不转成文本。
int wmain(int argc, wchar_t** argv) {
    using namespace War3Map;
    using namespace War3Map::Services;
    using Json = nlohmann::json;
    const auto require = [](bool value) { if (!value) throw std::runtime_error("内部包冻结未通过，未登记安装目录；不完整的新目录保留供核对。"); };
    try {
        if (argc != 4) { std::cerr << "usage: runtime-package-freezer <plan.json> <plan-sha256> <new-package-directory>\n"; return 2; }
        const auto identity = std::filesystem::path(argv[2]).u8string();
        const auto plan = ReadVerifiedFile(argv[1], std::string(identity.begin(), identity.end()), 64 * 1024);
        require(plan.Succeeded());
        // 只有冻结计划是JSON；载荷缓冲从不经过解析器、输出或编译器。
        const std::string_view planText(reinterpret_cast<const char*>(plan.value->data()), plan.value->size());
        const auto json = Json::parse(planText);
        const std::map<std::string, std::pair<std::string, std::string>> targets{
            {"bootstrap", {"bootstrap.blob", "warvk_loader_raw.ai"}}, {"payload", {"payload.blob", "warvk_runtime.blp"}},
            {"client", {"client.j", ""}}, {"license", {"license.txt", "audastra/runtime/licenses/license-memhack.txt"}},
            {"origin", {"origin.txt", "audastra/runtime/licenses/origin.txt"}}};
        require(json.at("schema") == 1 && json.at("files").is_array() && json.at("files").size() == targets.size());
        struct File { std::string path; std::vector<std::byte> bytes; };
        std::vector<File> files;
        Json manifest{{"schema", 1}, {"id", json.at("id")}, {"version", json.at("version")}, {"abi", 0x20000}, {"scope", "internal"},
            {"gameSha256", "e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a"}, {"files", Json::array()}};
        std::set<std::string> seen;
        std::uint64_t total = 0;
        for (const auto& item : json.at("files")) {
            const auto role = item.at("role").get<std::string>();
            require(targets.contains(role) && seen.insert(role).second && item.at("size").is_number_unsigned());
            const auto size = item.at("size").get<std::uint64_t>();
            require(size && size <= 16 * 1024 * 1024 && total + size <= 32 * 1024 * 1024);
            const auto sourceText = item.at("source").get<std::string>();
            const auto sourcePath = std::filesystem::path(std::u8string(sourceText.begin(), sourceText.end()));
            const auto bytes = ReadVerifiedFile(sourcePath, item.at("sha256").get<std::string>(), size);
            require(bytes.Succeeded() && bytes.value->size() == size);
            const auto& [path, member] = targets.at(role);
            manifest["files"].push_back({{"path", path}, {"member", member}, {"size", size}, {"sha256", item.at("sha256")},
                {"role", role == "license" || role == "origin" ? "notice" : role}});
            files.push_back({path, std::move(*bytes.value)}); total += size;
        }
        const auto output = std::filesystem::absolute(argv[3]).lexically_normal();
        std::error_code error;
        const auto status = std::filesystem::symlink_status(output, error);
        require((!error && status.type() == std::filesystem::file_type::not_found) || error == std::errc::no_such_file_or_directory);
        require(std::filesystem::create_directory(output));
        for (const auto& file : files) require(IO::WriteFileBytes(output / file.path, file.bytes).Succeeded());
        const auto text = manifest.dump();
        const auto bytes = std::as_bytes(std::span(text.data(), text.size()));
        require(IO::WriteFileBytes(output / "manifest.json", bytes).Succeeded());
        const auto sha = Sha256(bytes); require(sha.Succeeded());
        const MapRuntimePackageSource source{output, *sha.value, false};
        const auto verified = MapRuntimePackage::Capture(source, MapRuntimeSavePurpose::InternalValidation);
        require(verified.Succeeded());
        std::cout << "package_sha256=" << *sha.value << "\npayload_sha256=" << (*verified.value)->PayloadSha256()
            << "\ninternal_only=true\nfiles=" << files.size() << '\n';
        return 0;
    } catch (const std::exception&) {
        std::cerr << "RUNTIME_FREEZE_REJECTED: incomplete internal package was not installed; no payload content returned\n";
        return 1;
    }
}
