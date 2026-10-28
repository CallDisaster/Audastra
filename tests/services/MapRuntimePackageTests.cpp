#include "War3MapCore/Services/MapRuntimePackage.h"
#include "War3MapCore/Services/MapRuntimeCatalog.h"
#include "War3MapCore/Services/MapRuntimePreparation.h"
#include "War3MapCore/Services/MapEditSaveService.h"
#include "War3MapCore/ApplicationPaths.h"
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
void Preparation() {
    Fixture f;
    const std::vector<MapRuntimePackageSource> sources{f.source};
    const MapRuntimeSettings selected{MapRuntimeMode::Enabled, f.source.manifestSha256};
    Require(Code(ValidateMapRuntimeDecision({selected, false}, sources), "RUNTIME_CONSENT_REQUIRED"), "source declaration cannot grant consent");
    Require(Code(ValidateMapRuntimeDecision({selected, true}, {}), "RUNTIME_PACKAGE_MISSING"), "consent cannot invent installed sources");
    Require(ValidateMapRuntimeDecision({selected, true}, sources).Succeeded(), "explicit decision validates internal package without granting normal distribution");
    const auto source = f.root / "decision-source.w3x";
    Require(WriteVerifiedArchive(source, std::vector<MapArchiveMember>{{"war3map.j", Bytes("function main takes nothing returns nothing\nendfunction\n")}}).Succeeded(), "original decision archive");
    const auto archive = MapArchiveSession::Open(source); Require(archive.Succeeded(), "open decision source");
    const auto query = QueryMapRuntimeArchiveState(source, archive.value->SourceSha256(), sources);
    Require(query.Succeeded() && query.value->settings.packageSha256.empty() && query.value->packages.size() == 1, "source-bound path-free query");
    const auto json = nlohmann::json::parse(SerializeMapRuntimeArchiveState(*query.value));
    Require(json["approved"] == false && !json["packages"][0].contains("root"), "independent query never returns authorization or host paths");
    Require(Code(QueryMapRuntimeArchiveState(source, std::string(64, 'f'), sources), "RUNTIME_SOURCE_STALE"), "old source identity rejected");
    for (const auto purpose : {MapRuntimeSavePurpose::Normal, MapRuntimeSavePurpose::InternalValidation, MapRuntimeSavePurpose::TestRun}) {
        MapSaveTask task; Require(task.FreezeArchive(*archive.value).Succeeded(), "freeze decision source");
        MapRuntimeSaveOptions options{{MapRuntimeDecision{selected, false}}, purpose};
        Require(Code(PrepareMapRuntimeInputs(task, options, sources), "RUNTIME_CONSENT_REQUIRED")
            && task.replacements.empty() && task.runtimeApprovedPackageSha256.empty(), "failed approval leaves task intact");
        options.decision->approveNativeRuntime = true;
        Require(PrepareMapRuntimeInputs(task, options, sources).Succeeded() && task.runtimePurpose == purpose
            && task.testCopyOnly == (purpose != MapRuntimeSavePurpose::Normal)
            && task.runtimeApprovedPackageSha256 == f.source.manifestSha256, "freeze purpose, settings and consent before script preparation");
        Require(task.replacements.size() == 1 && ParseMapRuntimeSettings(task.replacements[0].data).value == selected,
            "settings use canonical shared member and codec");
    }
    const auto configuredSource = f.root / "configured-source.w3x";
    Require(WriteVerifiedArchive(configuredSource, std::vector<MapArchiveMember>{{std::string(kMapRuntimeSettingsMember),
        *SerializeMapRuntimeSettings(selected).value}}).Succeeded(), "original configured decision archive");
    const auto configured = MapArchiveSession::Open(configuredSource); Require(configured.Succeeded(), "open configured decision source");
    for (const auto* conflictSource : {&*archive.value, &*configured.value}) {
        for (const auto name : {"audastra/runtime/settings.json", "AUDAsTRA\\RUNTIME\\SETTINGS.JSON"}) {
            for (const bool remove : {false, true}) {
                MapSaveTask conflict; Require(conflict.FreezeArchive(*conflictSource).Succeeded(), "freeze settings conflict source");
                if (remove) conflict.removals.push_back(name);
                else conflict.replacements.push_back({name, Bytes("unrelated domain replacement")});
                const auto replacements = conflict.replacements.size();
                const auto removals = conflict.removals.size();
                Require(Code(PrepareMapRuntimeInputs(conflict, {{MapRuntimeDecision{selected, true}}, MapRuntimeSavePurpose::InternalValidation}, sources),
                    "RUNTIME_SETTINGS_CONFLICT") && conflict.replacements.size() == replacements && conflict.removals.size() == removals
                    && conflict.runtimeApprovedPackageSha256.empty(), "settings alias conflict rejected before task mutation");
            }
        }
    }
    MapEditSaveRequest independent; independent.sourceMap = source; independent.targetCandidate = f.root / "not-written.w3x";
    independent.toolRoot = independent.gameRoot = f.root; independent.expectedSourceSha256 = archive.value->SourceSha256();
    independent.minimapChoice = TerrainMinimapSaveChoice::Preserve;
    independent.runtime.decision = MapRuntimeDecision{selected, false}; independent.runtimePackageSources = sources;
    Require(Code(SaveMapEditCandidate(independent), "RUNTIME_CONSENT_REQUIRED") && !std::filesystem::exists(independent.targetCandidate),
        "runtime-only independent save reaches the common consent gate before compiling or writing");
    const auto bin = f.root / "bin"; Require(std::filesystem::create_directory(bin), "portable root fixture");
    // 临时目录可能带 8.3 短名（ADMINI~1），安装根按展开后的路径返回。
    Require(InstallationRootFromExecutableDirectory(bin) == std::filesystem::canonical(f.root), "portable bin uses adjacent release manifest");
    Require(!CurrentInstallationRoot().empty(), "current executable installation root does not use request paths");
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
void DiagnosticSaveRoundTrip() {
    Fixture f;
    // 仅原创合成包声明公开资格，以覆盖Normal与TestRun切换；不涉及真实内部载荷发布。
    f.manifest["scope"] = "public"; f.Save(); f.source.publicDistributionApproved = true;
    const auto settings = SerializeMapRuntimeSettings({MapRuntimeMode::Enabled, f.source.manifestSha256});
    const std::string clean = "globals\nstring AjExpectedRuntimeIdentity = \"\"\nendglobals\n"
        "function AjStartTestDiagnostics takes nothing returns nothing\nendfunction\n"
        "function main takes nothing returns nothing\ncall AjInitialize()\nendfunction\n";
    const auto source = f.root / "diagnostic-source.w3x";
    Require(WriteVerifiedArchive(source, std::vector<MapArchiveMember>{{"war3map.j",Bytes(clean)},
        {std::string(kMapRuntimeSettingsMember),*settings.value},{"unmanaged.bin",Bytes("retained")}}).Succeeded(), "write diagnostic save fixture");
    auto baseline = MapArchiveSession::Open(source); Require(baseline.Succeeded(), "open diagnostic fixture");
    unsigned sequence = 0;
    for (const auto purpose : {MapRuntimeSavePurpose::TestRun, MapRuntimeSavePurpose::TestRun,
        MapRuntimeSavePurpose::Normal, MapRuntimeSavePurpose::TestRun, MapRuntimeSavePurpose::InternalValidation}) {
        Script::MapRuntimeBootPlan boot;
        boot.payloadSha256 = *Sha256(Bytes("opaque synthetic payload bytes")).value;
        boot.testRunDiagnostics = purpose == MapRuntimeSavePurpose::TestRun;
        const auto built = Script::ApplyMapRuntimeBoot(clean, boot); Require(built.Succeeded(), "build diagnostic purpose fixture");
        MapSaveTask task; Require(task.FreezeArchive(*baseline.value).Succeeded(), "freeze diagnostic purpose");
        task.target = f.root / ("diagnostic-" + std::to_string(++sequence) + ".w3x");
        task.runtimePurpose = purpose; task.testCopyOnly = purpose != MapRuntimeSavePurpose::Normal;
        task.runtimePackageSources = {f.source}; task.runtimeApprovedPackageSha256 = f.source.manifestSha256;
        // 测保存终检/独立回读；这里不把原创合成脚本当真实编译证明。
        task.replacements.push_back({"war3map.j",Bytes(*built.value)}); task.scriptRebuilt = true;
        task.scriptInjectedSourceSha256 = {*Sha256(Bytes("original synthetic client source bytes")).value};
        MapSaveService::Run(task);
        if (!task.succeeded) for (const auto& d : task.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
        Require(task.succeeded, "test/normal/repeated shared save succeeds with matching final script");
        baseline = MapArchiveSession::Open(task.target); Require(baseline.Succeeded(), "independent diagnostic reopen");
        const auto bytes = baseline.value->ReadMember("war3map.j"); Require(bytes.Succeeded(), "read only diagnostic JASS member");
        const std::string script(reinterpret_cast<const char*>(bytes.value->data()), bytes.value->size());
        Require(Script::VerifyMapRuntimeTestDiagnostics(script, boot.testRunDiagnostics).Succeeded(), "saved diagnostic matches exact purpose");
        Require(baseline.value->ReadMember("unmanaged.bin").value == Bytes("retained"), "diagnostic policy preserves unrelated data");
    }
    MapSaveTask changed; Require(changed.FreezeArchive(*baseline.value).Succeeded(), "freeze purpose mutation");
    changed.target = f.root / "purpose-mutated.w3x"; changed.runtimePackageSources = {f.source};
    changed.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    changed.prepareFinalDerived = [](MapSaveTask& task) { task.runtimePurpose = MapRuntimeSavePurpose::TestRun; return Status::Ok(); };
    MapSaveService::Run(changed);
    Require(!changed.succeeded && Code(changed,"RUNTIME_SOURCE_CHANGED") && !std::filesystem::exists(changed.target), "late save purpose mutation cannot publish");
    const auto testArchive = MapArchiveSession::Open(f.root / "diagnostic-4.w3x"); Require(testArchive.Succeeded(), "reopen actual test copy to disable");
    const auto testBytes = testArchive.value->ReadMember("war3map.j"); Require(testBytes.Succeeded(), "read test-copy script only");
    std::string noBusinessCall(reinterpret_cast<const char*>(testBytes.value->data()), testBytes.value->size());
    const auto business = noBusinessCall.find("call AjInitialize()"); Require(business != std::string::npos, "locate original fixture business call");
    noBusinessCall.replace(business, std::string("call AjInitialize()").size(), "call Marker()");
    Script::MapRuntimeBootPlan removal; removal.enabled = false; removal.ownsExisting = true;
    const auto stripped = Script::ApplyMapRuntimeBoot(noBusinessCall, removal); Require(stripped.Succeeded(), "remove owned test scheduling with boot");
    MapSaveTask disabled; Require(disabled.FreezeArchive(*testArchive.value).Succeeded(), "freeze disable from TestRun");
    disabled.target = f.root / "diagnostic-disabled.w3x"; disabled.runtimePackageSources = {f.source};
    disabled.replacements = {{"war3map.j",Bytes(*stripped.value)},
        {std::string(kMapRuntimeSettingsMember),*SerializeMapRuntimeSettings({MapRuntimeMode::Disabled,f.source.manifestSha256}).value}};
    disabled.scriptRebuilt = true; MapSaveService::Run(disabled);
    Require(disabled.succeeded && !disabled.nextSession->HasMember(kMapRuntimeInstallationMember)
        && !disabled.nextSession->HasMember("warvk_runtime.blp"), "disable removes runtime members and test scheduling in the same save");
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
    const auto installedImportBytes = first.nextSession->ReadMember("war3map.imp");
    Require(installedImportBytes.Succeeded(), "installed import table exists");
    const auto installedImports = Resources::ImportTable::Parse(*installedImportBytes.value);
    const auto settingsKey = Resources::CanonicalImportPath(kMapRuntimeSettingsMember);
    Require(installedImports.Succeeded() && std::any_of(installedImports.value->Entries().begin(), installedImports.value->Entries().end(),
        [&](const auto& entry) { const auto key = Resources::CanonicalImportPath(entry.path);
            return key.Succeeded() && key.value == settingsKey.value; }), "standalone and UI save register the same shared settings member");
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
    for (const auto purpose : {MapRuntimeSavePurpose::Normal, MapRuntimeSavePurpose::TestRun, MapRuntimeSavePurpose::InternalValidation}) {
        MapSaveTask scoped;
        Require(scoped.FreezeArchive(*first.nextSession).Succeeded(), "freeze purpose fixture");
        scoped.target = f.root / ("purpose-" + std::to_string(static_cast<int>(purpose)) + ".w3x");
        scoped.runtimePurpose = purpose; scoped.testCopyOnly = true;
        scoped.runtimePackageSources = {f.source}; scoped.runtimeApprovedPackageSha256 = f.source.manifestSha256;
        MapSaveService::Run(scoped);
        if (purpose == MapRuntimeSavePurpose::Normal)
            Require(!scoped.succeeded && !std::filesystem::exists(scoped.target), "test marker does not grant ordinary save distribution approval");
        else if (purpose == MapRuntimeSavePurpose::TestRun)
            Require(!scoped.succeeded && Code(scoped, "RUNTIME_TEST_CLIENT_MISSING"), "old final script cannot silently bypass new test diagnostics");
        else
            Require(scoped.succeeded, "explicit test purposes package internal runtime into new copies");
    }
    MapSaveTask validationPreflight;
    Require(validationPreflight.FreezeArchive(*first.nextSession).Succeeded(), "freeze targetless test preflight");
    validationPreflight.runtimePurpose = MapRuntimeSavePurpose::InternalValidation; validationPreflight.testCopyOnly = true;
    validationPreflight.runtimePackageSources = {f.source}; validationPreflight.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    Require(MapSaveService::Preflight(validationPreflight).Succeeded() && validationPreflight.written.empty(),
        "targetless internal preflight validates without publishing");
    MapSaveTask overwrite;
    Require(overwrite.FreezeArchive(*first.nextSession).Succeeded(), "freeze test overwrite rejection");
    overwrite.target = source; overwrite.runtimePurpose = MapRuntimeSavePurpose::TestRun; overwrite.testCopyOnly = true;
    MapSaveService::Run(overwrite);
    Require(!overwrite.succeeded && Code(overwrite, "RUNTIME_INTERNAL_COPY_REQUIRED")
        && *Sha256File(source).value == opened.value->SourceSha256(), "test scope cannot overwrite any existing map");
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
void ModulePackageAndSave() {
    Fixture f;
    f.Add("module.blob", "warvk_runtime_module.dll", "module", "original synthetic module payload"); f.Save();
    Require(Code(MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::InternalValidation), "RUNTIME_PACKAGE_INVALID"), "schema1 never accepts module role");
    f.manifest["schema"] = 2; f.Save();
    const auto captured = MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::InternalValidation);
    Require(captured.Succeeded() && (*captured.value)->ModulePayloadSha256() == *Sha256(Bytes("original synthetic module payload")).value,
        "schema2 captures independent module content identity");
    const auto originalManifest = f.manifest;
    f.manifest["files"].erase(f.manifest["files"].size() - 1); f.Save();
    Require(Code(MapRuntimePackage::Capture(f.source, MapRuntimeSavePurpose::TestRun), "RUNTIME_PACKAGE_INVALID"), "schema2 requires module role");
    f.manifest = originalManifest; f.Save();
    const auto loader = Script::DefaultScriptModuleLoaders().at(0);
    const std::string original = "globals\nstring AjExpectedRuntimeIdentity=\"\"\nboolean AjRuntimeModuleExpected=false\nendglobals\nfunction " +
        loader.functionName + " takes string p, boolean a returns boolean\nreturn true\nendfunction\n"
        "function original_init takes nothing returns nothing\ncall StartCampaignAI(Player(12),\"original.blob\")\nendfunction\n"
        "function AjStartRuntimeModule takes nothing returns nothing\nendfunction\n"
        "function main takes nothing returns nothing\ncall original_init()\ncall AudastraUIAvailable()\nendfunction\n";
    const auto source = f.root / "module-source.w3x";
    const auto settings = *SerializeMapRuntimeSettings({MapRuntimeMode::Enabled, f.source.manifestSha256}).value;
    Require(WriteVerifiedArchive(source, std::vector<MapArchiveMember>{{"war3map.j", Bytes(original)},
        {std::string(kMapRuntimeSettingsMember), settings}, {"original.blob", Bytes("unmanaged opaque member")}}).Succeeded(), "original module fixture");
    const auto archive = MapArchiveSession::Open(source); Require(archive.Succeeded(), "open module fixture");
    MapSaveTask first; Require(first.FreezeArchive(*archive.value).Succeeded(), "freeze module fixture");
    first.target = f.root / "module-first.w3x"; first.runtimePurpose = MapRuntimeSavePurpose::InternalValidation;
    first.testCopyOnly = true; first.runtimePackageSources = {f.source}; first.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    first.script.emplace(); first.script->scriptMember = "war3map.j";
    Require(PrepareMapRuntimeSave(first).Succeeded(), "prepare module policy before compilation");
    auto& boot = *first.script->toolchain.mapRuntimeBoot;
    boot.injectedSourceSha256 = {*Sha256(Bytes("original synthetic client source bytes")).value};
    const auto compiled = Script::ApplyMapRuntimeBoot(original, boot);
    Require(compiled.Succeeded() && first.runtimeState->moduleLoader == loader, "common build selects module, captures consent and exact source identity");
    first.scriptInjectedSourceSha256 = boot.injectedSourceSha256;
    first.script.reset(); first.scriptRebuilt = true;
    first.replacements.push_back({"war3map.j", Bytes(*compiled.value)});
    // 本段只测脚本构建后的派生门；合成替身不构成真实工具链编译或游戏证明。
    Require(FinalizeMapRuntimeSave(first).Succeeded() && VerifyMapRuntimeSave(first).Succeeded(), "module final gates bind selected product and ownership");
    const auto metadata = std::find_if(first.replacements.begin(), first.replacements.end(), [](const auto& item) { return item.name == kMapRuntimeInstallationMember; });
    Require(metadata != first.replacements.end(), "module metadata exists");
    const auto json = nlohmann::json::parse(std::string(reinterpret_cast<const char*>(metadata->data.data()), metadata->data.size()));
    Require(json["schema"] == 2 && json["bootRule"] == 2 && json["moduleLoaderId"] == loader.id, "new ownership version is explicit");
    std::vector<MapArchiveMember> members;
    for (const auto& member : first.replacements) members.push_back({member.name, member.data});
    members.push_back({"original.blob", Bytes("unmanaged opaque member")});
    members.push_back({std::string(kMapRuntimeSettingsMember), settings});
    Require(WriteVerifiedArchive(first.target, members).Succeeded(), "serialize unit-test derived fixture");
    const auto installed = MapArchiveSession::Open(first.target); Require(installed.Succeeded(), "reopen module ownership fixture");
    MapSaveTask repeated; Require(repeated.FreezeArchive(*installed.value).Succeeded(), "freeze repeated module save");
    repeated.target = f.root / "module-repeat.w3x"; repeated.runtimePurpose = MapRuntimeSavePurpose::InternalValidation;
    repeated.testCopyOnly = true; repeated.runtimePackageSources = {f.source}; repeated.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    MapSaveService::Run(repeated);
    if (!repeated.succeeded) for (const auto& d : repeated.diagnostics) std::cerr << d.code << ": " << d.message << '\n';
    Require(repeated.succeeded && repeated.nextSession->ReadMember("war3map.j").value == Bytes(*compiled.value), "normal shared save restores module ownership without rebuilding or reordering");
    Require(repeated.nextSession->ReadMember("original.blob").value == Bytes("unmanaged opaque member"), "unmanaged member preserved");
    auto originalBody = original.substr(original.find("function original_init"), std::string("function original_init takes nothing returns nothing\ncall StartCampaignAI(Player(12),\"original.blob\")\nendfunction").size());
    Require(compiled.value->find(originalBody) != std::string::npos, "third-party initialization full body exact in same phase");
    auto tampered = *compiled.value; tampered.replace(tampered.find("warvk_runtime_module.dll"), 24, "wrong.dll");
    first.replacements[0].data = Bytes(tampered);
    Require(!VerifyMapRuntimeSave(first).Succeeded(), "post-finalization script drift refuses publication");
    MapSaveTask publicSave; Require(publicSave.FreezeArchive(*installed.value).Succeeded(), "freeze module normal purpose");
    publicSave.target = f.root / "forbidden-public.w3x"; publicSave.runtimePackageSources = {f.source}; publicSave.runtimeApprovedPackageSha256 = f.source.manifestSha256;
    MapSaveService::Run(publicSave);
    Require(!publicSave.succeeded && Code(publicSave, "RUNTIME_DISTRIBUTION_NOT_APPROVED") && !std::filesystem::exists(publicSave.target), "module strategy does not grant public distribution");
    auto unused = *compiled.value;
    unused.replace(unused.find("call AudastraUIAvailable()"), std::string("call AudastraUIAvailable()").size(), "call Marker()");
    Script::MapRuntimeBootPlan removal; removal.enabled = false; removal.ownsExisting = true; removal.ownedModuleLoader = loader;
    const auto stripped = Script::ApplyMapRuntimeBoot(unused, removal); Require(stripped.Succeeded(), "owned module removal plan");
    MapSaveTask disabled; Require(disabled.FreezeArchive(*installed.value).Succeeded(), "freeze module disable");
    disabled.target = f.root / "module-disabled.w3x"; disabled.runtimePackageSources = {f.source};
    disabled.replacements = {{"war3map.j", Bytes(*stripped.value)}, {std::string(kMapRuntimeSettingsMember),
        *SerializeMapRuntimeSettings({MapRuntimeMode::Disabled, {}}).value}};
    disabled.scriptRebuilt = true;
    MapSaveService::Run(disabled);
    Require(disabled.succeeded && !disabled.nextSession->HasMember("warvk_runtime_module.dll") &&
        !disabled.nextSession->HasMember(kMapRuntimeInstallationMember) && disabled.nextSession->HasMember("original.blob"),
        "normal-purpose disable removes only owned module members and leaves original initialization/member");
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
    try { Settings(); Preparation(); Catalog(); Package(); MissingAndPublic(); PackageNegativeBoundaries(); DiagnosticSaveRoundTrip(); SaveRoundTrip(); SaveGates(); UnknownOwnership(); ModulePackageAndSave(); LuaPolicy(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "runtime settings and package tests passed\n";
}
