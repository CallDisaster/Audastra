#include "War3MapCore/Services/LuaFolder.h"
#include "War3MapCore/Services/LuaFolderSave.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Hash.h"

#include <Windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> void Good(const T& value, const char* message) {
    if (value.Succeeded()) return;
    std::string text(message);
    for (const auto& d : value.diagnostics) text += " | " + d.code + ": " + d.message;
    throw std::runtime_error(text);
}
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}
void Put(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(text.data(), text.size());
    Require(stream.good(), "fixture write");
}
struct Temp {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("audastra-lua-folder-" + std::to_string(GetCurrentProcessId()) + '-' + std::to_string(GetTickCount64()));
    Temp() { Require(std::filesystem::create_directory(root), "fresh temp"); }
    ~Temp() { std::error_code ec; std::filesystem::remove_all(root, ec); }
};
void SnapshotAndWrites() {
    Temp temp;
    const auto folder = temp.root / "external";
    std::filesystem::create_directory(folder);
    auto empty = CaptureLuaFolder(folder);
    Good(empty, "capture empty"); Require(empty.value->files.empty(), "actual empty source");
    Put(folder / "lessons/reload.lua", "return 1\n");
    Put(folder / ".git/ignored.lua", "not lua syntax and ignored");
    Put(folder / ".vscode/ignored.lua", "also ignored");
    Put(folder / "image.bin", "not a script");
    auto first = CaptureLuaFolder(folder, true);
    Good(first, "capture source"); Require(first.value->files.size() == 1, "only Lua without dot directories");
    const auto old = first.value->files.front();
    const auto body = Bytes("return 2\n");
    Good(WriteLuaFolderFile(folder, old.path, body, old.sha256), "CAS disk write");
    Require(!WriteLuaFolderFile(folder, old.path, Bytes("return 3"), old.sha256).Succeeded(), "stale write refused");
    Require(!VerifyLuaFolder(*first.value).Succeeded(), "changed directory proof refused");
    auto current = CaptureLuaFolder(folder, true);
    Good(current, "changed source");
    Good(RenameLuaFolderFile(folder, current.value->files.front(), "lessons/renamed.lua"), "rename keeps same source");
    auto renamed = CaptureLuaFolder(folder);
    Good(renamed, "read rename");
    Good(RemoveLuaFolderFile(folder, renamed.value->files.front()), "delete exact file");
    Require(!std::filesystem::exists(folder / "lessons/renamed.lua"), "actual deleted path");
    Put(folder / "bad.lua", std::string("return \xff", 8));
    Require(!CaptureLuaFolder(folder).Succeeded(), "invalid UTF8 not silently skipped");
    std::filesystem::remove(folder / "bad.lua");
    Put(folder / "large.lua", std::string(2 * 1024 * 1024 + 1, ' '));
    Require(!CaptureLuaFolder(folder).Succeeded(), "oversized exact Lua rejected");
    std::filesystem::remove(folder / "large.lua");
    Put(folder / "syntax.lua", "local =");
    Good(CaptureLuaFolder(folder), "incomplete external editing can display");
    Require(!CaptureLuaFolder(folder, true).Succeeded(), "save validates syntax");
    std::filesystem::remove(folder / "syntax.lua");
    Put(temp.root / "outside/secret.lua", "return 7");
    std::error_code ec;
    std::filesystem::create_directory_symlink(temp.root / "outside", folder / "outside-link", ec);
    if (!ec) {
        auto linked = CaptureLuaFolder(folder);
        Good(linked, "external directory link ignored");
        Require(linked.value->files.empty() && linked.value->skippedLinks == 1, "no outside Lua reads");
    }
}
void ReconcileAndMetadata() {
    Temp temp;
    const auto folder = temp.root / "external";
    std::filesystem::create_directory(folder);
    const auto a = Bytes("return 'map'");
    std::vector<LuaFolderFile> map{{"lessons/reload.lua", *Sha256(a).value, std::make_shared<const std::vector<std::byte>>(a)}};
    auto empty = CaptureLuaFolder(folder);
    Good(empty, "empty review");
    Good(ReconcileLuaFolder(*empty.value, map, LuaFolderChoice::Map), "empty exports map modules");
    Put(folder / "lessons/reload.lua", "return 'disk'");
    Put(folder / "extra.lua", "return 2");
    auto review = CaptureLuaFolder(folder);
    Good(review, "different review");
    const auto diff = CompareLuaFolder(map, *review.value);
    Require(diff.different.size() == 1 && diff.folderOnly.size() == 1, "review identifies both differences");
    Require(!ReconcileLuaFolder(*review.value, map, LuaFolderChoice::Cancel).Succeeded(), "cancel has no writes");
    Good(VerifyLuaFolder(*review.value), "cancel kept bytes");
    Good(ReconcileLuaFolder(*review.value, map, LuaFolderChoice::Folder), "disk choice has no disk writes");
    Good(VerifyLuaFolder(*review.value), "disk source unchanged");
    Good(ReconcileLuaFolder(*review.value, map, LuaFolderChoice::Map), "explicit map choice writes and deletes extras");
    Require(!std::filesystem::exists(folder / "extra.lua"), "map authority removes folder-only Lua");
    const auto mapPath = temp.root / "maps/source.w3x";
    const auto metadata = EncodeLuaFolderLink(folder, mapPath);
    Good(metadata, "encode relative link");
    const std::string text(reinterpret_cast<const char*>(metadata.value->data()), metadata.value->size());
    Require(text.find("../external") != std::string::npos, "same drive relative to map file");
    const auto decoded = DecodeLuaFolderLink(*metadata.value, mapPath);
    Good(decoded, "decode relative link"); Require(**decoded.value == folder, "original folder retained");
    Require(!DecodeLuaFolderLink(Bytes("{\"schema\":1,\"schema\":1,\"path\":\"x\"}"), mapPath).Succeeded(), "duplicate metadata rejected");
    Require(!DecodeLuaFolderLink(Bytes("{}"), mapPath).Succeeded(), "missing schema rejected");
}
void SaveRoundTripAndMissingFallback() {
    Temp temp;
    const auto folder = temp.root / "external";
    Put(folder / "lessons/reload.lua", "return 'new'");
    const auto source = temp.root / "source.w3x";
    const auto metadata = EncodeLuaFolderLink(folder, source);
    Good(metadata, "save link");
    const std::vector<MapArchiveMember> members{{"lessons/reload.lua", Bytes("return 'old'")},
        {"deleted.lua", Bytes("return 1")}, {std::string(kLuaFolderMember), *metadata.value}, {"unknown.bin", Bytes("opaque")}};
    Good(WriteVerifiedArchive(source, members), "write map fixture");
    auto archive = MapArchiveSession::Open(source); Good(archive, "open map fixture");
    const auto originalHash = archive.value->SourceSha256();
    MapSaveTask unconfirmed;
    Good(unconfirmed.FreezeArchive(*archive.value), "freeze unconfirmed");
    unconfirmed.target = temp.root / "must-not-exist.w3x";
    MapSaveService::Run(unconfirmed);
    Require(!unconfirmed.succeeded && !std::filesystem::exists(unconfirmed.target), "deletion needs explicit confirmation");
    auto map = CaptureMapLuaFiles(*archive.value); Good(map, "current map scripts");
    auto disk = CaptureLuaFolder(folder); Good(disk, "save source");
    const auto approval = LuaFolderDeletionIdentity(*map.value, *disk.value); Good(approval, "delete approval");
    MapSaveTask task;
    Good(task.FreezeArchive(*archive.value), "freeze confirmed");
    task.target = temp.root / "saved.w3x"; task.luaFolderDeletionApproval = *approval.value;
    MapSaveService::Run(task);
    Good(Status{task.diagnostics}, "normal map save"); Require(task.succeeded, "save success");
    Require(task.nextSession->ReadMember("lessons/reload.lua").value == std::optional(Bytes("return 'new'")), "folder bytes saved");
    Require(!task.nextSession->HasMember("deleted.lua"), "confirmed removal read back");
    Require(*Sha256File(source).value == originalHash, "source map untouched");
    Good(WriteLuaFolderFile(folder, "lessons/reload.lua", Bytes("return 'after save'"), task.luaFolderSnapshot->files[0].sha256), "save releases folder lease");
    std::filesystem::rename(folder, temp.root / "renamed");
    MapSaveTask missing;
    Good(missing.FreezeArchive(*task.nextSession), "freeze missing folder fallback");
    missing.target = temp.root / "fallback.w3x";
    MapSaveService::Run(missing);
    Require(missing.succeeded && missing.nextSession->HasMember("lessons/reload.lua"), "missing does not clear map Lua");
    Require(ReadLuaFolderLink(*missing.nextSession).value->has_value(), "missing association retained");
    std::filesystem::rename(temp.root / "renamed", folder);
    Good(CaptureLuaFolder(folder), "restored folder reconnects");
}
}
int main() {
    try { SnapshotAndWrites(); ReconcileAndMetadata(); SaveRoundTripAndMissingFallback(); std::cout << "Lua folder: all scenarios passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
