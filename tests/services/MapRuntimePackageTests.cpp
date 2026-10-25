#include "War3MapCore/Services/MapRuntimePackage.h"
#include "War3MapCore/Services/MapRuntimeCatalog.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Services/MapSaveService.h"
#include "War3MapCore/Script/MapRuntimeBoot.h"
#include "../../src/Services/MapRuntimeSave.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "../../src/Core/Mpq/MpqCrypt.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace War3Map;
using namespace War3Map::Services;
namespace {
void Require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data()); return {first, first + text.size()};
}
bool Code(const auto& result, std::string_view code) {
    for (const auto& item : result.diagnostics) if (item.code == code) return true;
    return false;
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("audastra-runtime-package-"
        + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    nlohmann::json manifest;
    MapRuntimePackageSource source;
    Fixture() {
        Require(std::filesystem::create_directory(root), "private fixture directory");
        manifest = {{"schema",1},{"id","test-package"},{"version","1.0.0"},{"abi",0x20000},
            {"gameSha256","e04d1716603c075eb0c8e1e21cf1093a664adc5249efab396bfa08d7b09d0c3a"},
            {"scope","internal"},{"files",nlohmann::json::array()}};
        // 原创任意字节不是AI源码；引导/载荷夹具均为blob，客户端为原创J字节。
        Add("boot.blob", "warvk_loader_raw.ai", "bootstrap", "opaque synthetic bootstrap bytes");
        Add("payload.blob", "warvk_runtime.blp", "payload", "opaque synthetic payload bytes");
        Add("notice.blob", "audastra/runtime/licenses/source.txt", "notice", "original fixture notice");
        Add("client.j", "", "client", "original synthetic client source bytes");
        source.root = root; Save();
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root, error); }
    void Add(std::string path, std::string member, std::string role, std::string text) {
        const auto bytes = Bytes(text); const auto hash = Sha256(bytes);
        Require(hash.Succeeded(), "fixture digest");
        Require(IO::WriteFileBytes(root / path, bytes).Succeeded(), "fixture write");
        manifest["files"].push_back({{"path",path},{"member",member},{"role",role},{"size",bytes.size()},{"sha256",*hash.value}});
    }
    void Save() {
        const auto bytes = Bytes(manifest.dump());
        Require(IO::WriteFileBytes(root / "manifest.json", bytes).Succeeded(), "manifest write");
        source.manifestSha256 = *Sha256(bytes).value;
    }
};
// 原创最小MPQ夹具：只省略指定成员的清单行，不读取外部地图或引导器。
void WriteHiddenArchive(const std::filesystem::path& path, std::vector<MapArchiveMember> members, std::string_view hidden) {
    std::string list;
    for (const auto& member : members) if (member.name != hidden) list += member.name + "\r\n";
    members.push_back({"(listfile)",Bytes(list)});
    constexpr std::uint32_t hashCount = 32;
    Require(members.size() < hashCount / 2, "small synthetic archive budget");
    std::uint32_t hashPos = 32;
    for (const auto& member : members) hashPos += static_cast<std::uint32_t>(member.data.size());
    const auto blockPos = hashPos + hashCount * 16;
    const auto size = blockPos + static_cast<std::uint32_t>(members.size()) * 16;
    std::vector<std::byte> bytes(size);
    const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
    put(0,0x1a51504d); put(4,32); put(8,size); put(12,0x00030000);
    put(16,hashPos); put(20,blockPos); put(24,hashCount); put(28,static_cast<std::uint32_t>(members.size()));
    std::fill(bytes.begin() + hashPos, bytes.begin() + blockPos, std::byte{0xff});
    const auto& crypt = Mpq::MpqCrypt::Instance();
    std::uint32_t pos = 32;
    for (std::uint32_t index = 0; index < members.size(); ++index) {
        const auto& member = members[index];
        std::copy(member.data.begin(),member.data.end(),bytes.begin() + pos);
        auto bucket = crypt.HashString(member.name,Mpq::MpqCrypt::HashType::TableOffset) & (hashCount - 1);
        while (bytes[hashPos + bucket * 16 + 12] != std::byte{0xff}) bucket = (bucket + 1) & (hashCount - 1);
        const auto at = hashPos + bucket * 16;
        put(at,crypt.HashString(member.name,Mpq::MpqCrypt::HashType::NameA));
        put(at + 4,crypt.HashString(member.name,Mpq::MpqCrypt::HashType::NameB)); put(at + 8,0); put(at + 12,index);
        const auto block = blockPos + index * 16;
        const auto length = static_cast<std::uint32_t>(member.data.size());
        put(block,pos); put(block + 4,length); put(block + 8,length); put(block + 12,Mpq::kFileExists | Mpq::kFileSingleUnit);
        pos += length;
    }
    crypt.EncryptBlock(bytes.data() + hashPos,hashCount * 16,crypt.HashString("(hash table)",Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blockPos,members.size() * 16,crypt.HashString("(block table)",Mpq::MpqCrypt::HashType::FileKey));
    Require(IO::WriteFileBytes(path,bytes).Succeeded(), "write original hidden-member fixture");
}
void Settings() {
    MapRuntimeSettings initial;
    auto encoded = SerializeMapRuntimeSettings(initial);
    Require(encoded.Succeeded() && ParseMapRuntimeSettings(*encoded.value).value == initial, "default settings roundtrip");
    initial.mode = MapRuntimeMode::Enabled; initial.packageSha256 = std::string(64, 'a');
    encoded = SerializeMapRuntimeSettings(initial);
    Require(encoded.Succeeded() && ParseMapRuntimeSettings(*encoded.value).value == initial, "approved identity roundtrip");
    for (const auto text : {"{}", "[]", "{\"schema\":1,\"mode\":\"enabled\",\"mode\":\"disabled\",\"packageSha256\":\"\"}",
        "{\"schema\":1,\"mode\":\"enabled\",\"packageSha256\":\"../payload.dll\"}",
        "{\"schema\":1,\"mode\":\"automatic\",\"packageSha256\":\"\",\"path\":\"payload\"}"})
        Require(Code(ParseMapRuntimeSettings(Bytes(text)), "RUNTIME_SETTINGS_INVALID"), "invalid settings rejected");
    Require(Code(ParseMapRuntimeSettings(Bytes(std::string(4097, ' '))), "RUNTIME_SETTINGS_INVALID"), "settings bounded");
}
void Catalog() {
    Fixture f;
    Require(LoadInstalledMapRuntimeSources(f.root).value->empty(), "absent installed catalog is empty");
    const auto directory = f.root / "runtime";
    Require(std::filesystem::create_directory(directory), "create synthetic runtime directory");
    const auto catalog = directory / "catalog.json";
    const auto save = [&](const nlohmann::json& value) {
        Require(IO::WriteFileBytes(catalog,Bytes(value.dump())).Succeeded(), "write synthetic installed catalog");
    };
    nlohmann::json json{{"schema",1},{"packages",nlohmann::json::array({{
        {"directory","test-1"},{"manifestSha256",f.source.manifestSha256},{"publicDistributionApproved",false}}})}};
    save(json);
    const auto loaded = LoadInstalledMapRuntimeSources(f.root);
    Require(loaded.Succeeded() && loaded.value->size() == 1, "installed catalog accepted");
    const auto& source = loaded.value->front();
    Require(source.root == directory / "packages" / "test-1" && !source.publicDistributionApproved,
        "fixed relative installed root and independent public permission");
    const auto missing = QueryMapRuntimePackages(*loaded.value);
    Require(missing.Succeeded() && !missing.value->front().available && !missing.value->front().diagnostics.empty(),
        "missing installed package is visible, not silently replaced");
    Require(std::filesystem::create_directories(source.root), "create installed fixture package");
    for (const auto* name : {"boot.blob","payload.blob","notice.blob","client.j","manifest.json"})
        Require(std::filesystem::copy_file(f.root / name,source.root / name), "copy original non-AI fixture bytes");
    const auto packages = QueryMapRuntimePackages(*loaded.value);
    Require(packages.Succeeded() && packages.value->front().available && packages.value->front().internalOnly
        && packages.value->front().id == "test-package" && packages.value->front().version == "1.0.0"
        && packages.value->front().packageSha256 == f.source.manifestSha256,
        "path-free metadata names exactly the verified installed package");
    Require(Code(MapRuntimePackage::Capture(source,MapRuntimeSavePurpose::Normal),"RUNTIME_DISTRIBUTION_NOT_APPROVED"),
        "catalog query does not grant normal distribution");
    Require(IO::WriteFileBytes(source.root / "payload.blob",Bytes("changed")).Succeeded(), "mutate synthetic package");
    const auto changed = QueryMapRuntimePackages(*loaded.value);
    Require(changed.Succeeded() && !changed.value->front().available
        && Code(changed.value->front(),"RUNTIME_HASH_MISMATCH"), "catalog exposes source drift without content");
    Require(Code(QueryMapRuntimePackages(std::vector<MapRuntimePackageSource>{source,source}),"RUNTIME_PACKAGE_CONFLICT"),
        "duplicate package identity rejected before captures");
    Require(Code(QueryMapRuntimePackages(std::vector<MapRuntimePackageSource>(17,source)),"RUNTIME_CATALOG_INVALID"),
        "catalog query is bounded");
    for (const auto& name : {"../escape","absolute/path","c:/package","CON","test-1."}) {
        auto bad = json; bad["packages"][0]["directory"] = name; save(bad);
        Require(Code(LoadInstalledMapRuntimeSources(f.root),"RUNTIME_CATALOG_INVALID"), "invalid installed directory rejected");
    }
    auto bad = json; bad["packages"].push_back(bad["packages"][0]); save(bad);
    Require(Code(LoadInstalledMapRuntimeSources(f.root),"RUNTIME_CATALOG_INVALID"), "duplicate installed package rejected");
    bad = json; bad["path"] = "map-provided"; save(bad);
    Require(Code(LoadInstalledMapRuntimeSources(f.root),"RUNTIME_CATALOG_INVALID"), "unknown catalog keys rejected");
    Require(IO::WriteFileBytes(catalog,Bytes("{\"schema\":1,\"schema\":1,\"packages\":[]}")).Succeeded(), "write duplicate catalog key");
    Require(Code(LoadInstalledMapRuntimeSources(f.root),"RUNTIME_CATALOG_INVALID"), "duplicate catalog key rejected");
    Require(IO::WriteFileBytes(catalog,Bytes(std::string(32769,' '))).Succeeded(), "write oversized catalog");
    Require(Code(LoadInstalledMapRuntimeSources(f.root),"RUNTIME_CATALOG_INVALID"), "installed catalog read is bounded");
}
void Package() {
    Fixture f;
    Require(Code(MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::Normal), "RUNTIME_DISTRIBUTION_NOT_APPROVED"), "internal package blocked for normal save");
    auto package = MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::InternalValidation);
    Require(package.Succeeded() && package.value && (*package.value)->Members().size() == 3, "capture complete opaque package");
    Require((*package.value)->VerifySource().Succeeded(), "frozen source verification");
    Require((*package.value)->Identity() == f.source.manifestSha256 && (*package.value)->InternalOnly(), "content identity and internal scope");
    const auto clientHash = *Sha256(Bytes("original synthetic client source bytes")).value;
    Require((*package.value)->VerifyScriptInputs(std::vector<std::string>{clientHash}).Succeeded(), "exact client injection identity accepted");
    for (const auto& inputs : {std::vector<std::string>{}, std::vector<std::string>{std::string(64, 'a')}, std::vector<std::string>{clientHash,clientHash}})
        Require(Code((*package.value)->VerifyScriptInputs(inputs), "RUNTIME_SCRIPT_INPUT_MISMATCH"), "missing, wrong, duplicate client injection rejected");
    Require(IO::WriteFileBytes(f.root / "payload.blob", Bytes("changed")).Succeeded(), "mutate fixture source");
    Require(Code((*package.value)->VerifySource(), "RUNTIME_SOURCE_CHANGED"), "source drift detected");
    Require(Code(MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::TestRun), "RUNTIME_HASH_MISMATCH"), "bad payload hash rejected");
    f.manifest["files"][1]["path"] = "../payload.blob"; f.Save();
    Require(Code(MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::TestRun), "RUNTIME_PACKAGE_INVALID"), "path traversal rejected before reads");
    f.manifest["files"][1]["path"] = "payload.blob";
    f.manifest["files"].push_back(f.manifest["files"][0]); f.Save();
    Require(!MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::TestRun).Succeeded(), "duplicate files rejected");
}
void MissingAndPublic() {
    Fixture f;
    auto missing = f.source; missing.manifestSha256 = std::string(64, '0');
    Require(Code(MapRuntimePackage::Capture(missing, MapRuntimeSavePurpose::TestRun), "RUNTIME_HASH_MISMATCH"), "wrong manifest identity rejected");
    f.manifest["scope"] = "public"; f.Save();
    Require(Code(MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::Normal), "RUNTIME_DISTRIBUTION_NOT_APPROVED"), "manifest cannot grant itself public approval");
    f.source.publicDistributionApproved = true;
    Require(MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::Normal).Succeeded(), "trusted public approval distinct from map data");
    f.manifest["files"][0]["path"] = "missing.blob"; f.Save();
    Require(Code(MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::Normal), "RUNTIME_PACKAGE_MISSING"), "missing file rejected");
}
void PackageNegativeBoundaries() {
    Fixture f;
    Require(Code(MapRuntimePackage::Capture(f.source, static_cast<MapRuntimeSavePurpose>(77)), "RUNTIME_SAVE_PURPOSE_INVALID"),
        "invalid enum cannot bypass distribution gate");
    for (const auto path : {"con.blob", "aux/item.blob", "nul", "com1.blob", "lpt9.txt", "folder./x", "x//y", "x:y", "\\\\server\\x"}) {
        f.manifest["files"][0]["path"] = path; f.Save();
        Require(Code(MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::TestRun), "RUNTIME_PACKAGE_INVALID"),
            "device and ambiguous paths rejected before opening files");
    }
    auto invalidRoot = f.source; invalidRoot.root = L"\\\\?\\C:\\untrusted";
    Require(Code(MapRuntimePackage::Capture(invalidRoot, MapRuntimeSavePurpose::TestRun), "RUNTIME_PACKAGE_SOURCE_INVALID"), "device root rejected");
    f.manifest["files"][0]["path"] = "boot.blob";
    f.manifest["schema"] = 1.0; f.Save();
    Require(Code(MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::TestRun), "RUNTIME_PACKAGE_INVALID"), "floating schema rejected");
}
void SaveRoundTrip() {
    Fixture f;
    const auto settings = SerializeMapRuntimeSettings({MapRuntimeMode::Automatic, f.source.manifestSha256});
    const std::string sourceScript = "globals\nstring AjExpectedRuntimeIdentity = \"\"\nendglobals\n"
        "function main takes nothing returns nothing\ncall AudastraUIAvailable()\nendfunction\n";
    Script::MapRuntimeBootPlan boot; boot.payloadSha256 = *Sha256(Bytes("opaque synthetic payload bytes")).value;
    const auto built = Script::ApplyMapRuntimeBoot(sourceScript, boot);
    Require(built.Succeeded(), "prepare synthetic final script");
    const auto source = f.root / "source.w3x";
    const std::vector<MapArchiveMember> sourceMembers{{"war3map.j",Bytes(*built.value)},
        {std::string(kMapRuntimeSettingsMember),*settings.value},{"unmanaged.bin",Bytes("preserved")}};
    Require(WriteVerifiedArchive(source, sourceMembers).Succeeded(), "write original fixture archive");
    const auto opened = MapArchiveSession::Open(source);
    Require(opened.Succeeded(), "open synthetic source");
    MapSaveTask first;
    Require(first.FreezeArchive(*opened.value).Succeeded(), "freeze serialization fixture");
    first.target = f.root / "saved.w3x"; first.runtimePackageSources = {f.source};
    first.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    first.runtimePurpose = MapRuntimeSavePurpose::InternalValidation;
    first.testCopyOnly = true;
    // 该夹具只测已编译产物之后的保存事务，不把合成脚本当作真实编译或进游戏证据。
    first.scriptRebuilt = true;
    first.scriptInjectedSourceSha256 = {*Sha256(Bytes("original synthetic client source bytes")).value};
    MapSaveService::Run(first);
    if (!first.succeeded) for (const auto& d : first.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    Require(first.succeeded && first.nextSession, "shared service packages and reopens candidate");
    const auto payload = first.nextSession->ReadMember("warvk_runtime.blp");
    Require(payload.Succeeded() && *payload.value == Bytes("opaque synthetic payload bytes"), "payload readback byte equality");
    Require(first.nextSession->HasMember(kMapRuntimeInstallationMember), "managed installation receipt written");
    Require(*Sha256File(source).value == opened.value->SourceSha256(), "source archive unchanged");
    MapSaveTask second;
    Require(second.FreezeArchive(*first.nextSession).Succeeded(), "freeze repeated save");
    second.target = f.root / "again.w3x"; second.runtimePurpose = MapRuntimeSavePurpose::InternalValidation;
    second.testCopyOnly = true;
    second.runtimePackageSources = {f.source}; second.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    MapSaveService::Run(second);
    Require(second.succeeded, "repeat save reuses exact managed members");
    Require(second.nextSession->ReadMember("war3map.imp").value == first.nextSession->ReadMember("war3map.imp").value,
        "repeat save does not duplicate imports");
    MapSaveTask staleIdentity;
    Require(staleIdentity.FreezeArchive(*first.nextSession).Succeeded(), "freeze wrong identity fixture");
    staleIdentity.target = f.root / "stale-identity.w3x"; staleIdentity.runtimePurpose = MapRuntimeSavePurpose::InternalValidation;
    staleIdentity.runtimePackageSources = {f.source};
    staleIdentity.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    staleIdentity.testCopyOnly = true;
    auto wrongScript = *built.value;
    wrongScript.replace(wrongScript.find(boot.payloadSha256), boot.payloadSha256.size(), std::string(64, 'f'));
    staleIdentity.replacements.push_back({"war3map.j",Bytes(wrongScript)});
    MapSaveService::Run(staleIdentity);
    Require(!staleIdentity.succeeded && Code(staleIdentity,"RUNTIME_IDENTITY_MISMATCH") && !std::filesystem::exists(staleIdentity.target),
        "installed reuse rejects author-changed expected identity before publication");
    for (unsigned kind = 0; kind < 3; ++kind) {
        auto members = std::vector<MapArchiveMember>(first.nextSession->Members().begin(), first.nextSession->Members().end());
        const auto receipt = std::find_if(members.begin(), members.end(), [](const auto& m) { return m.name == kMapRuntimeInstallationMember; });
        Require(receipt != members.end(), "installation fixture receipt present");
        std::string text(reinterpret_cast<const char*>(receipt->data.data()), receipt->data.size());
        if (kind == 0) text.insert(1, "\"schema\":1,");
        else if (kind == 1) text = "{\"deep\":" + std::string(32, '[') + "0" + std::string(32, ']') + "}";
        else text = std::string(32769, ' ');
        receipt->data = Bytes(text);
        const auto malformed = f.root / ("malformed" + std::to_string(kind) + ".w3x");
        Require(WriteVerifiedArchive(malformed, members).Succeeded(), "write malformed synthetic installation");
        auto altered = MapArchiveSession::Open(malformed);
        MapSaveTask rejected;
        Require(altered.Succeeded() && rejected.FreezeArchive(*altered.value).Succeeded(), "freeze malformed installation");
        rejected.target = f.root / ("malformed-saved" + std::to_string(kind) + ".w3x");
        rejected.runtimePurpose = MapRuntimeSavePurpose::InternalValidation; rejected.runtimePackageSources = {f.source};
        rejected.testCopyOnly = true;
        MapSaveService::Run(rejected);
        Require(Code(rejected,"RUNTIME_INSTALLATION_INVALID") && !rejected.succeeded && !std::filesystem::exists(rejected.target),
            "duplicate, deeply nested, and oversized installation metadata fail atomically");
    }
    MapSaveTask tampered;
    Require(tampered.FreezeArchive(*first.nextSession).Succeeded(), "freeze derived conflict");
    tampered.target = f.root / "tampered.w3x"; tampered.runtimePurpose = MapRuntimeSavePurpose::InternalValidation;
    tampered.testCopyOnly = true;
    tampered.runtimePackageSources = {f.source};
    tampered.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    tampered.prepareFinalDerived = [](MapSaveTask& task) {
        for (auto& member : task.replacements) if (member.name == "warvk_runtime.blp") member.data = Bytes("tampered");
        return Status::Ok();
    };
    MapSaveService::Run(tampered);
    Require(!tampered.succeeded && Code(tampered,"RUNTIME_MEMBER_CONFLICT") && !std::filesystem::exists(tampered.target),
        "late derived overwrite rejected before publication");
    MapSaveTask removed;
    Require(removed.FreezeArchive(*first.nextSession).Succeeded(), "freeze disable fixture");
    removed.target = f.root / "removed.w3x";
    removed.runtimePackageSources = {f.source};
    removed.replacements = {{std::string(kMapRuntimeSettingsMember),*SerializeMapRuntimeSettings({MapRuntimeMode::Disabled,{}}).value},
        {"war3map.j",Bytes("function main takes nothing returns nothing\nendfunction\n")}};
    MapSaveService::Run(removed);
    Require(removed.succeeded && !removed.nextSession->HasMember("warvk_runtime.blp")
        && !removed.nextSession->HasMember(kMapRuntimeInstallationMember), "disable removes only unchanged managed installation");
    Require(removed.nextSession->ReadMember("unmanaged.bin").value == std::optional(Bytes("preserved")), "unmanaged source remains byte-identical");
    MapSaveTask notACopy;
    Require(notACopy.FreezeArchive(*first.nextSession).Succeeded(), "freeze unapproved target scope");
    notACopy.target = f.root / "not-a-test-copy.w3x";
    notACopy.runtimePurpose = MapRuntimeSavePurpose::InternalValidation; notACopy.runtimePackageSources = {f.source};
    notACopy.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    MapSaveService::Run(notACopy);
    Require(Code(notACopy,"RUNTIME_INTERNAL_COPY_REQUIRED") && !std::filesystem::exists(notACopy.target),
        "internal purpose alone does not authorize writing an unmarked map");
    MapSaveTask wrongClient;
    Require(wrongClient.FreezeArchive(*opened.value).Succeeded(), "freeze wrong client provenance");
    wrongClient.target = f.root / "wrong-client.w3x"; wrongClient.testCopyOnly = true;
    wrongClient.runtimePurpose = MapRuntimeSavePurpose::InternalValidation; wrongClient.runtimePackageSources = {f.source};
    wrongClient.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    wrongClient.scriptRebuilt = true; wrongClient.scriptInjectedSourceSha256 = {std::string(64, 'f')};
    MapSaveService::Run(wrongClient);
    Require(Code(wrongClient,"RUNTIME_SCRIPT_INPUT_MISMATCH") && !std::filesystem::exists(wrongClient.target),
        "correct payload and boot cannot hide stale client injection");
    MapSaveTask changedBody;
    Require(changedBody.FreezeArchive(*first.nextSession).Succeeded(), "freeze changed script without compiler");
    changedBody.target = f.root / "changed-body.w3x"; changedBody.testCopyOnly = true;
    changedBody.runtimePurpose = MapRuntimeSavePurpose::InternalValidation; changedBody.runtimePackageSources = {f.source};
    changedBody.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    changedBody.replacements.push_back({"war3map.j",Bytes(*built.value + "// changed after previous compilation\n")});
    MapSaveService::Run(changedBody);
    Require(Code(changedBody,"RUNTIME_SCRIPT_REBUILD_REQUIRED") && !std::filesystem::exists(changedBody.target),
        "reusing an installed package requires the exact previous checked script unless rebuilt");
    for (bool changeDuringSave : {false,true}) {
        MapSaveTask untrusted;
        Require(untrusted.FreezeArchive(*first.nextSession).Succeeded(), "freeze map-carried declaration");
        untrusted.target = f.root / (changeDuringSave ? "changed-approval.w3x" : "no-host-approval.w3x");
        untrusted.runtimePurpose = MapRuntimeSavePurpose::InternalValidation;
        untrusted.runtimePackageSources = {f.source}; untrusted.testCopyOnly = true;
        if (changeDuringSave) {
            untrusted.runtimeApprovedPackageSha256 = f.source.manifestSha256;
            untrusted.prepareFinalDerived = [](MapSaveTask& task) {
                task.runtimeApprovedPackageSha256.clear(); return Status::Ok();
            };
        }
        MapSaveService::Run(untrusted);
        Require(!untrusted.succeeded && !std::filesystem::exists(untrusted.target)
            && Code(untrusted, changeDuringSave ? "RUNTIME_SOURCE_CHANGED" : "RUNTIME_CONSENT_REQUIRED"),
            "map declaration cannot grant host approval and frozen approval cannot drift");
    }
}
void SaveGates() {
    Fixture f;
    const auto source = f.root / "plain.w3x";
    const std::vector<MapArchiveMember> sourceMembers{{"war3map.j",Bytes("function main takes nothing returns nothing\nendfunction\n")}};
    Require(WriteVerifiedArchive(source, sourceMembers).Succeeded(), "plain fixture archive");
    auto opened = MapArchiveSession::Open(source);
    MapSaveTask plain;
    Require(plain.FreezeArchive(*opened.value).Succeeded(), "freeze plain map");
    plain.target = f.root / "plain-saved.w3x";
    MapSaveService::Run(plain);
    Require(plain.succeeded && !plain.nextSession->HasMember("warvk_runtime.blp") && !plain.nextSession->HasMember("war3map.imp"), "unused map adds no runtime or import table");
    MapSaveTask consent;
    Require(consent.FreezeArchive(*opened.value).Succeeded(), "freeze missing consent");
    consent.target = f.root / "no-consent.w3x";
    consent.replacements.push_back({"war3map.j",Bytes("function main takes nothing returns nothing\ncall AudastraUIAvailable()\nendfunction\n")});
    MapSaveService::Run(consent);
    Require(Code(consent,"RUNTIME_CONSENT_REQUIRED") && !std::filesystem::exists(consent.target), "missing consent refuses before publication");
}
void UnknownOwnership() {
    Fixture f;
    const auto settings = *SerializeMapRuntimeSettings({MapRuntimeMode::Automatic,f.source.manifestSha256}).value;
    Script::MapRuntimeBootPlan boot; boot.payloadSha256 = *Sha256(Bytes("opaque synthetic payload bytes")).value;
    const auto built = Script::ApplyMapRuntimeBoot("globals\nstring AjExpectedRuntimeIdentity = \"\"\nendglobals\nfunction main takes nothing returns nothing\ncall AudastraUIAvailable()\nendfunction\n",boot);
    Require(built.Succeeded(), "original ownership script");
    for (unsigned kind = 0; kind < 6; ++kind) {
        const auto source = f.root / ("ownership-" + std::to_string(kind) + ".w3x");
        std::vector<MapArchiveMember> members{{"war3map.j",Bytes(*built.value)},{std::string(kMapRuntimeSettingsMember),settings}};
        if (kind == 0 || kind == 1) members.push_back({"warvk_runtime.blp",Bytes("opaque synthetic payload bytes")});
        if (kind == 3) {
            auto table = Resources::ImportTable::CreateEmpty();
            Require(table.Upsert("WARVK_RUNTIME.BLP").Succeeded(), "orphan import record");
            members.push_back({"war3map.imp",*table.Serialize().value});
        }
        if (kind == 4) members.push_back({std::string(kMapRuntimeInstallationMember),Bytes("{}")});
        if (kind == 5) members.back().data = Bytes("{}");
        if (kind == 1 || kind >= 4) WriteHiddenArchive(source,members,kind == 1 ? "warvk_runtime.blp"
            : kind == 4 ? kMapRuntimeInstallationMember : kMapRuntimeSettingsMember);
        else Require(WriteVerifiedArchive(source,members).Succeeded(), "original ownership archive");
        auto archive = MapArchiveSession::Open(source); Require(archive.Succeeded(), "open ownership source");
        if (kind == 1 || kind >= 4) Require(archive.value->HasOpaqueMembers(), "fixture genuinely has hidden member");
        MapSaveTask task; Require(task.FreezeArchive(*archive.value).Succeeded(), "freeze ownership source");
        task.target = f.root / ("rejected-ownership-" + std::to_string(kind) + ".w3x");
        task.runtimePurpose = MapRuntimeSavePurpose::InternalValidation; task.testCopyOnly = true;
        task.runtimePackageSources = {f.source}; task.runtimeApprovedPackageSha256 = f.source.manifestSha256;
        task.scriptRebuilt = true; task.scriptInjectedSourceSha256 = {*Sha256(Bytes("original synthetic client source bytes")).value};
        if (kind == 2) task.replacements.push_back({"warvk_runtime.blp",Bytes("opaque synthetic payload bytes")});
        MapSaveService::Run(task);
        Require(!task.succeeded && !std::filesystem::exists(task.target) && Code(task,kind == 4
            ? "RUNTIME_INSTALLATION_INVALID" : kind == 5 ? "RUNTIME_SETTINGS_INVALID" : "RUNTIME_MEMBER_CONFLICT"),
            "named, hidden, queued, and orphan imports cannot grant ownership; hidden metadata cannot be skipped");
        Require(*Sha256File(source).value == archive.value->SourceSha256(), "rejected ownership leaves source unchanged");
    }
}
void LuaPolicy() {
    Fixture f;
    MapData::MapSettings settings; settings.formatVersion = 31; settings.scriptLanguage = 1;
    settings.gameVersion = {1,32,10,20456};
    settings.tileset = 'L'; settings.custom.emplace(); settings.graphicsModes = 3; settings.gameDataVersion = 1;
    const auto encoded = settings.Serialize();
    if (!encoded.Succeeded()) for (const auto& d : encoded.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    Require(encoded.Succeeded(), "synthetic Lua settings");
    const std::string ordinary = "-- AjInitialize() is a comment\nlocal t = {label = [[AudastraUIAvailable()]]}\nfunction main() print(t.label) end\n";
    const auto source = f.root / "lua-policy.w3x";
    Require(WriteVerifiedArchive(source, std::vector<MapArchiveMember>{{"war3map.w3i",*encoded.value},{"war3map.lua",Bytes(ordinary)}}).Succeeded(), "synthetic Lua policy archive");
    const auto archive = MapArchiveSession::Open(source); Require(archive.Succeeded(), "open Lua policy archive");
    for (const auto mode : {MapRuntimeMode::Automatic, MapRuntimeMode::Disabled, MapRuntimeMode::Enabled}) {
        MapSaveTask task; Require(task.FreezeArchive(*archive.value).Succeeded(), "freeze Lua policy");
        task.replacements.push_back({std::string(kMapRuntimeSettingsMember),*SerializeMapRuntimeSettings({mode,f.source.manifestSha256}).value});
        Require(PrepareMapRuntimeSave(task).Succeeded(), "prepare Lua runtime policy without requesting a package");
        const auto final = FinalizeMapRuntimeSave(task);
        Require(mode == MapRuntimeMode::Enabled ? Code(final,"RUNTIME_LUA_UNSUPPORTED")
            : final.Succeeded() && task.replacements.size() == 1, "unused Lua survives historical automatic consent; explicit runtime remains unsupported");
    }
    MapSaveTask dependency; Require(dependency.FreezeArchive(*archive.value).Succeeded(), "freeze Lua call policy");
    dependency.replacements.push_back({"war3map.lua",Bytes("function main() AjInitialize() end")});
    Require(PrepareMapRuntimeSave(dependency).Succeeded() && Code(FinalizeMapRuntimeSave(dependency),"RUNTIME_LUA_UNSUPPORTED"),
        "actual Lua runtime call reports target incompatibility without installing or deleting calls");
}
}
int main() {
    try { Settings(); Catalog(); Package(); MissingAndPublic(); PackageNegativeBoundaries(); SaveRoundTrip(); SaveGates(); UnknownOwnership(); LuaPolicy(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "runtime settings and package tests passed\n";
}
