#include "LuaFolderCommand.h"
#include "War3MapCore/Services/LuaFolder.h"
#include "War3MapCore/Services/LuaFolderSave.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <algorithm>
#include <cwctype>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
using Json = nlohmann::json;
std::string Utf8(const std::filesystem::path& path) {
    const auto text = path.generic_u8string(); return {text.begin(), text.end()};
}
int Failed(const std::vector<Diagnostic>& diagnostics) {
    for (const auto& d : diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    return 1;
}
int Bad() {
    std::cerr << "LUA_FOLDER_ARGUMENT: inspect map [folder] | save map sha output [--lua-folder folder] [--confirm-lua-deletions sha] | link map sha output folder folder-sha folder|map|cancel | unlink map sha output | read map member | write map map-sha member text.lua file-sha|new output-map\n";
    return 2;
}
void Metadata(MapSaveTask& task, const std::optional<std::filesystem::path>& folder) {
    if (folder) {
        const auto encoded = EncodeLuaFolderLink(*folder, task.sourcePath);
        if (!encoded.Succeeded()) throw std::runtime_error("cannot encode link");
        task.replacements.push_back({std::string(kLuaFolderMember), *encoded.value});
    } else {
        task.removals.push_back(std::string(kLuaFolderMember));
        if (task.archive->HasMember("war3map.imp")) {
            const auto bytes = task.archive->ReadMember("war3map.imp");
            auto table = bytes.Succeeded() ? Resources::ImportTable::Parse(*bytes.value) : Result<Resources::ImportTable>{std::nullopt, bytes.diagnostics};
            if (!table.Succeeded()) throw std::runtime_error("invalid import table");
            const auto removed = table.value->Remove(kLuaFolderMember, [&](auto name) { return task.archive->HasMember(name); });
            if (!removed.Succeeded()) throw std::runtime_error("cannot remove link import");
            const auto wire = table.value->Serialize();
            if (!wire.Succeeded()) throw std::runtime_error("cannot serialize import table");
            task.replacements.push_back({"war3map.imp", *wire.value});
        }
    }
}
}

int RunLuaFolder(int argc, wchar_t** argv) {
    if (argc < 4) return Bad();
    try {
        const std::wstring action = argv[2];
        if (action == L"link" && argc == 9 && std::wstring_view(argv[8]) == L"cancel") {
            std::cout << "{\"ok\":true,\"cancelled\":true}\n"; return 0;
        }
        if (action == L"inspect" || action == L"read") {
            const auto sourceSha = Sha256File(argv[3]);
            if (!sourceSha.Succeeded()) return Failed(sourceSha.diagnostics);
            auto link = ReadLuaFolderLinkFile(argv[3]);
            if (!link.Succeeded()) return Failed(link.diagnostics);
            if (action == L"inspect" && argc == 5) *link.value = std::filesystem::path(argv[4]);
            auto map = CaptureMapLuaFiles(std::filesystem::path(argv[3]));
            if (!map.Succeeded()) return Failed(map.diagnostics);
            std::optional<LuaFolderSnapshot> folder;
            if (*link.value) {
                auto captured = CaptureLuaFolder(**link.value);
                if (!captured.Succeeded()) return Failed(captured.diagnostics);
                folder = std::move(*captured.value);
            }
            if (Sha256File(argv[3]).value != sourceSha.value) return Failed({{DiagnosticSeverity::Error, "LuaFolder", "LUA_FOLDER_MAP_CHANGED", "读取期间地图改变，请重试。"}});
            Json output{{"ok", true}, {"source", folder && !folder->missing ? "folder" : "map"},
                {"map_sha256", *sourceSha.value}, {"folder", *link.value ? Json(Utf8(**link.value)) : Json(nullptr)}};
            if (action == L"inspect") {
                if (argc != 4 && argc != 5) return Bad();
                if (folder) {
                    const auto diff = CompareLuaFolder(*map.value, *folder);
                    const auto deletion = LuaFolderDeletionIdentity(*map.value, *folder);
                    if (!deletion.Succeeded()) return Failed(deletion.diagnostics);
                    output["map_only"] = diff.mapOnly; output["folder_only"] = diff.folderOnly; output["different"] = diff.different;
                    output["folder_sha256"] = folder->sha256; output["delete_confirmation_sha256"] = *deletion.value;
                    output["missing"] = folder->missing; output["skipped_links"] = folder->skippedLinks;
                }
            } else {
                if (argc != 5) return Bad();
                const auto wanted = Resources::CanonicalImportPath(Utf8(argv[4]));
                if (!wanted.Succeeded()) return Failed(wanted.diagnostics);
                const auto& files = folder && !folder->missing ? folder->files : *map.value;
                const auto found = std::find_if(files.begin(), files.end(), [&](const auto& file) { return Resources::CanonicalImportPath(file.path).value == wanted.value; });
                if (found == files.end()) return Failed({{DiagnosticSeverity::Error, "LuaFolder", "LUA_FOLDER_FILE_MISSING", "当前来源没有此Lua文件。"}});
                output["path"] = found->path; output["sha256"] = found->sha256;
                output["text"] = std::string(reinterpret_cast<const char*>(found->bytes->data()), found->bytes->size());
            }
            std::cout << output.dump(2) << '\n'; return 0;
        }
        auto archive = MapArchiveSession::Open(argv[3]);
        if (!archive.Succeeded()) return Failed(archive.diagnostics);
        const auto link = action == L"link" || action == L"unlink"
            ? Result<std::optional<std::filesystem::path>>::Success(std::nullopt) : ReadLuaFolderLink(*archive.value);
        if (!link.Succeeded()) return Failed(link.diagnostics);
        auto map = action == L"link" || action == L"write" ? CaptureMapLuaFiles(*archive.value)
            : Result<std::vector<LuaFolderFile>>::Success({});
        if (!map.Succeeded()) return Failed(map.diagnostics);
        std::optional<std::filesystem::path> folder = *link.value;
        std::optional<LuaFolderSnapshot> snapshot;
        if (folder && action == L"write") {
            auto captured = CaptureLuaFolder(*folder);
            if (!captured.Succeeded()) return Failed(captured.diagnostics);
            snapshot = std::move(*captured.value);
        }
        if (argc < 6 || Utf8(argv[4]) != archive.value->SourceSha256()) return Bad();
        MapSaveTask task;
        const auto frozen = task.FreezeArchive(*archive.value);
        if (!frozen.Succeeded()) return Failed(frozen.diagnostics);
        if (action == L"link") {
            if (argc != 9) return Bad();
            const std::wstring choice = argv[8];
            if (choice == L"cancel") { std::cout << "{\"ok\":true,\"cancelled\":true}\n"; return 0; }
            if (choice != L"map" && choice != L"folder") return Bad();
            auto reviewed = CaptureLuaFolder(argv[6]);
            if (!reviewed.Succeeded()) return Failed(reviewed.diagnostics);
            if (reviewed.value->sha256 != Utf8(argv[7])) return Failed({{DiagnosticSeverity::Error, "LuaFolder", "LUA_FOLDER_CHANGED", "文件夹与审阅SHA不同。"}});
            std::error_code ec;
            const bool empty = std::filesystem::is_empty(reviewed.value->root, ec);
            if (ec) return Bad();
            const auto reconciled = ReconcileLuaFolder(*reviewed.value, *map.value,
                empty || choice == L"map" ? LuaFolderChoice::Map : LuaFolderChoice::Folder);
            if (!reconciled.Succeeded()) return Failed(reconciled.diagnostics);
            Metadata(task, reviewed.value->root);
            // link是明确的三方选择；folder权威对应的地图独有文件已经在审阅中列出。
            const auto actual = CaptureLuaFolder(reviewed.value->root);
            if (!actual.Succeeded()) return Failed(actual.diagnostics);
            const auto approved = LuaFolderDeletionIdentity(*map.value, *actual.value);
            if (!approved.Succeeded()) return Failed(approved.diagnostics);
            task.luaFolderDeletionApproval = *approved.value;
            task.target = argv[5];
        } else if (action == L"unlink") {
            if (argc != 6) return Bad();
            Metadata(task, std::nullopt); task.target = argv[5];
        } else if (action == L"save") {
            task.target = argv[5];
            for (int i = 6; i < argc; i += 2) {
                if (i + 1 >= argc) return Bad();
                const std::wstring flag = argv[i];
                if (flag == L"--lua-folder" && task.luaFolderOverride.empty()) task.luaFolderOverride = argv[i + 1];
                else if (flag == L"--confirm-lua-deletions" && task.luaFolderDeletionApproval.empty()) task.luaFolderDeletionApproval = Utf8(argv[i + 1]);
                else return Bad();
            }
        } else if (action == L"write") {
            if (argc != 9) return Bad();
            const std::filesystem::path input = argv[6];
            auto extension = input.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (extension != L".lua") return Bad();
            const auto bytes = IO::ReadFileBytes(input, 2u * 1024u * 1024u);
            if (!bytes.Succeeded()) return Failed(bytes.diagnostics);
            const auto member = Utf8(argv[5]);
            const auto valid = ValidateLuaFolderFile(member, *bytes.value, true);
            if (!valid.Succeeded()) return Failed(valid.diagnostics);
            const auto expected = std::wstring_view(argv[7]) == L"new" ? std::string{} : Utf8(argv[7]);
            if (snapshot && !snapshot->missing) {
                const auto written = WriteLuaFolderFile(snapshot->root, member, *bytes.value, expected);
                if (!written.Succeeded()) return Failed(written.diagnostics);
                std::cout << "{\"ok\":true,\"disk_written\":true,\"map_undo\":false,\"map_saved\":false}\n";
                return 0;
            }
            const auto found = std::find_if(map.value->begin(), map.value->end(), [&](const auto& f) {
                return *Resources::CanonicalImportPath(f.path).value == *Resources::CanonicalImportPath(member).value;
            });
            if (found == map.value->end() ? !expected.empty() : expected != found->sha256)
                return Failed({{DiagnosticSeverity::Error, "LuaFolder", "LUA_FOLDER_WRITE_CONFLICT", "地图Lua与指定SHA不同。"}});
            task.replacements.push_back({member, *bytes.value}); task.target = argv[8];
            auto imports = Resources::ImportTable::CreateEmpty();
            if (archive.value->HasMember("war3map.imp")) {
                const auto raw = archive.value->ReadMember("war3map.imp");
                if (!raw.Succeeded()) return Failed(raw.diagnostics);
                const auto parsed = Resources::ImportTable::Parse(*raw.value);
                if (!parsed.Succeeded()) return Failed(parsed.diagnostics);
                imports = *parsed.value;
            }
            const auto added = imports.Upsert(member, [&](auto name) { return archive.value->HasMember(name) || name == member; });
            if (!added.Succeeded()) return Failed(added.diagnostics);
            const auto wire = imports.Serialize();
            if (!wire.Succeeded()) return Failed(wire.diagnostics);
            task.replacements.push_back({"war3map.imp", *wire.value});
        } else return Bad();
        MapSaveService::Run(task);
        if (!task.succeeded) return Failed(task.diagnostics);
        std::cout << Json{{"ok", true}, {"path", Utf8(task.written)}, {"sha256", task.nextSession->SourceSha256()}, {"source_map_unchanged", true}}.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) { std::cerr << "LUA_FOLDER_COMMAND: " << error.what() << '\n'; return 1; }
}
