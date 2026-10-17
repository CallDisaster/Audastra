#include "ArchiveMemberCommand.h"

#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/Resources/ImportTable.h"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace War3Map;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* data = reinterpret_cast<const std::byte*>(text.data());
    return {data, data + text.size()};
}

int Run(std::vector<std::wstring> arguments) {
    arguments.insert(arguments.begin(), {L"war3-map-cli", L"archive-member-add"});
    std::vector<wchar_t*> argv;
    for (auto& argument : arguments) argv.push_back(argument.data());
    return Cli::RunArchiveMemberAdd(static_cast<int>(argv.size()), argv.data());
}

std::wstring Wide(const std::string& text) { return {text.begin(), text.end()}; }

} // namespace

int main() {
    try {
        const auto root = std::filesystem::temp_directory_path()
            / ("audastra-archive-member-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        const auto imp = Resources::ImportTable::CreateEmpty().Serialize();
        Require(imp.Succeeded() && imp.value, "empty import table");
        const std::vector<MapArchiveMember> members{{"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")},
            {"war3map.imp", *imp.value}, {"war3mapImported\\old.mdx", Bytes("old")}};
        const auto source = root / "source.w3x";
        const auto written = WriteVerifiedArchive(source, members);
        Require(written.Succeeded() && written.value, "write synthetic source map");
        const auto sha = Wide(written.value->sha256);
        const auto payload = root / "Hero.mdx";
        std::ofstream(payload, std::ios::binary) << "MDLX";

        // 编辑器管理的数据文件和已有成员都不能用这个命令写入。
        Require(Run({source.wstring(), sha, (root / "a.w3x").wstring(), L"war3map.w3u", payload.wstring()}) == 2,
            "managed member is rejected");
        Require(Run({source.wstring(), sha, (root / "b.w3x").wstring(), L"WAR3MAPIMPORTED/OLD.MDX", payload.wstring()}) == 2,
            "existing member is not overwritten");
        Require(Run({source.wstring(), sha, (root / "c.w3x").wstring(), L"a.mdx", payload.wstring(),
            L"A.MDX", payload.wstring()}) == 2, "duplicate member is rejected");
        Require(!std::filesystem::exists(root / "a.w3x") && !std::filesystem::exists(root / "b.w3x")
            && !std::filesystem::exists(root / "c.w3x"), "rejected requests write no candidate");

        const auto candidate = root / "added.w3x";
        Require(Run({source.wstring(), sha, candidate.wstring(), L"war3mapImported/Hero.mdx", payload.wstring()}) == 0,
            "new member is added");
        auto opened = MapArchiveSession::Open(candidate);
        Require(opened.Succeeded() && opened.value, "reopen candidate");
        const auto added = opened.value->ReadMember("war3mapImported\\Hero.mdx");
        Require(added.Succeeded() && added.value && *added.value == Bytes("MDLX"), "member bytes are written");
        Require(opened.value->ReadMember("war3mapImported\\old.mdx").value == std::optional{Bytes("old")},
            "existing member is kept");
        bool listed = false;
        for (const auto& member : opened.value->Members())
            if (member.name == "war3mapImported\\Hero.mdx") listed = true;
        Require(listed, "member keeps the author's spelling");
        const auto table = Resources::ImportTable::Parse(*opened.value->ReadMember("war3map.imp").value);
        Require(table.Succeeded() && table.value && table.value->Entries().size() == 1, "import table records the new file");
        std::filesystem::remove_all(root);
        std::cout << "archive-member-add tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
