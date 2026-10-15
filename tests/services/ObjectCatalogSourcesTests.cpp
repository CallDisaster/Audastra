#include "War3MapCore/Services/ObjectCatalogSources.h"
#include "War3MapCore/Services/CampaignService.h"
#include "War3MapCore/Services/ObjectCommands.h"
#include "War3MapCore/Workspace.h"
#include "ObjectCommandsFixture.h"
#include "../../src/Core/Mpq/MpqCrypt.h"

#include <Windows.h>
#include <array>
#include <atomic>
#include <fstream>
#include <iostream>

namespace {
using namespace War3Map;
using namespace War3Map::Services;
using namespace War3Map::ObjectData;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template <typename T> T Good(Result<T> result) {
    if (!result.Succeeded()) {
        std::string text;
        for (const auto& diagnostic : result.diagnostics) text += diagnostic.code + ':' + diagnostic.message + '\n';
        throw std::runtime_error(text);
    }
    return std::move(*result.value);
}
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic_uint index{0};
        path = std::filesystem::temp_directory_path() / ("audastra-object-sources-" + std::to_string(GetCurrentProcessId())
            + '-' + std::to_string(GetTickCount64()) + '-' + std::to_string(index.fetch_add(1)));
        Require(std::filesystem::create_directory(path), "exclusive fixture directory");
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
};
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(file.good(), "write original fixture bytes");
}
void Put(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::byte>((value >> (i * 8)) & 255);
}
// 原创无压缩 MPQ，无(listfile)。哈希与块布局由本测试明确构造，不调用目录捕获器。
void Archive(const std::filesystem::path& path, std::span<const MemberReplacement> members) {
    Require(members.size() < 32, "fixture member budget");
    std::vector<std::byte> bytes(32);
    std::vector<std::uint32_t> offsets;
    for (const auto& member : members) {
        offsets.push_back(static_cast<std::uint32_t>(bytes.size()));
        bytes.insert(bytes.end(), member.data.begin(), member.data.end());
    }
    constexpr std::size_t buckets = 64;
    const auto hashOffset = static_cast<std::uint32_t>(bytes.size()); bytes.resize(bytes.size() + buckets * 16, std::byte{0xff});
    const auto blockOffset = static_cast<std::uint32_t>(bytes.size()); bytes.resize(bytes.size() + members.size() * 16);
    std::array<bool, buckets> used{};
    const auto& crypt = Mpq::MpqCrypt::Instance();
    for (std::size_t i = 0; i < members.size(); ++i) {
        auto bucket = crypt.HashString(members[i].name, Mpq::MpqCrypt::HashType::TableOffset) & (buckets - 1);
        while (used[bucket]) bucket = (bucket + 1) & (buckets - 1);
        used[bucket] = true;
        const auto at = hashOffset + bucket * 16;
        Put(bytes, at, crypt.HashString(members[i].name, Mpq::MpqCrypt::HashType::NameA));
        Put(bytes, at + 4, crypt.HashString(members[i].name, Mpq::MpqCrypt::HashType::NameB));
        Put(bytes, at + 8, 0); Put(bytes, at + 12, static_cast<std::uint32_t>(i));
        const auto size = static_cast<std::uint32_t>(members[i].data.size());
        Put(bytes, blockOffset + i * 16, offsets[i]); Put(bytes, blockOffset + i * 16 + 4, size);
        Put(bytes, blockOffset + i * 16 + 8, size); Put(bytes, blockOffset + i * 16 + 12, Mpq::kFileSingleUnit | Mpq::kFileExists);
    }
    crypt.EncryptBlock(bytes.data() + hashOffset, buckets * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(bytes.data() + blockOffset, members.size() * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    Put(bytes, 0, 0x1a51504du); Put(bytes, 4, 32); Put(bytes, 8, static_cast<std::uint32_t>(bytes.size()));
    Put(bytes, 12, 0x00030000u); Put(bytes, 16, hashOffset); Put(bytes, 20, blockOffset);
    Put(bytes, 24, buckets); Put(bytes, 28, static_cast<std::uint32_t>(members.size()));
    Write(path, bytes);
}
std::vector<std::byte> Table(std::string id, std::string name = "Original") {
    return Tests::ObjectBytes(Tests::ObjectTable({{{"ID", std::move(id)}, {"Name", std::move(name)}, {"file", "Models\\Original.mdl"}}}));
}
CatalogLoadOptions Options() {
    CatalogLoadOptions options; options.stringFiles.clear();
    options.sources = {{ObjectType::Doodad, {"Doodads/Doodads.slk"}, {"Units/Profile.txt"}}};
    return options;
}
void CampaignCaptureRetainsItsPathHardCapWithSharedDefaults() {
    auto campaign = CampaignSession::Create();
    const std::vector<std::string> names{"Doodads/Doodads.slk", "Units/Absent.txt"};
    const auto payload = Table("D006");
    CampaignCommand add; add.operation = CampaignOperation::ImportResource;
    add.replacements = {{names.front(), payload}};
    (void)Good(campaign->Apply(campaign->Revision(), add));
    const auto captured = Good(campaign->CaptureExactMembers(campaign->Revision(), names));
    Require(captured.size() == 2 && captured.front().present && captured.front().bytes == payload
        && !captured.back().present, "default campaign capture distinguishes owned table from absent member");

    TemporaryDirectory temporary;
    const auto path = temporary.path / "original-campaign.w3n";
    // 战役重开走严格地图归档门禁；只为本夹具显式提供完整命名清单。
    const std::vector<MemberReplacement> original{
        {"war3campaign.w3f", Good(Campaign::SerializeInfo(campaign->Info()))}, {names.front(), payload},
        {"(listfile)", Tests::ObjectBytes("war3campaign.w3f\r\nDoodads/Doodads.slk\r\n")}};
    Archive(path, original);
    auto reopened = Good(CampaignSession::Open(path));
    const auto fromArchive = Good(reopened->CaptureExactMembers(reopened->Revision(), names));
    Require(fromArchive.size() == 2 && fromArchive.front().present && fromArchive.front().bytes == payload
        && !fromArchive.back().present, "shared default also works through the bounded archive reader");

    std::vector<std::string> boundary;
    for (int index = 0; index < 8; ++index)
        boundary.push_back(std::string(1023, 'a') + static_cast<char>('0' + index));
    const auto atHardCap = Good(campaign->CaptureExactMembers(campaign->Revision(), boundary));
    Require(atHardCap.size() == 8, "8192 actual path bytes remain within the campaign hard cap");
    boundary.push_back("x");
    const auto overHardCap = campaign->CaptureExactMembers(campaign->Revision(), boundary);
    Require(!overHardCap.Succeeded() && !overHardCap.diagnostics.empty()
        && overHardCap.diagnostics.front().code == "CAMPAIGN_SOURCE_PATHS",
        "shared default never permits 8193 actual path bytes");

    ArchiveExactMemberLimits tighter;
    tighter.totalPathBytes = names.front().size() - 1;
    Require(!campaign->CaptureExactMembers(campaign->Revision(), names, tighter).Succeeded(),
        "normalizing the default preserves a caller's tighter cumulative path budget");
    tighter = {}; tighter.pathBytes = names.front().size() - 1;
    Require(!campaign->CaptureExactMembers(campaign->Revision(), names, tighter).Succeeded(),
        "single-path caller budget remains enforced");
    tighter = {}; tighter.memberBytes = payload.size() - 1;
    Require(!campaign->CaptureExactMembers(campaign->Revision(), names, tighter).Succeeded(),
        "member-byte caller budget remains enforced");
    tighter = {}; tighter.totalBytes = payload.size() - 1;
    Require(!campaign->CaptureExactMembers(campaign->Revision(), names, tighter).Succeeded(),
        "total-byte caller budget remains enforced");
    tighter = {}; ++tighter.totalPathBytes;
    Require(!campaign->CaptureExactMembers(campaign->Revision(), names, tighter).Succeeded(),
        "configuration above the shared archive hard limit remains rejected");
}
void AnonymousAndLayering() {
    TemporaryDirectory temp;
    const auto gameRoot = temp.path / "game";
    const std::vector<MemberReplacement> gameFiles{{"Doodads/Doodads.slk", Table("D001")},
        {"Units/Profile.txt", Tests::ObjectBytes("[D003]\nName=Lower profile\n")}};
    Archive(gameRoot / "war3.mpq", gameFiles);
    const std::vector<MemberReplacement> noListFiles{{"Doodads/Doodads.slk", Table("D003")}, {"Units/Profile.txt", {}}};
    const auto readOnlyPath = temp.path / "no-list.w3x"; Archive(readOnlyPath, noListFiles);
    auto readOnly = Good(MpqWorkspace::OpenReadOnly(readOnlyPath));
    ObjectCatalogSources preview; preview.readOnlyArchive = &readOnly; preview.gameRoot = gameRoot;
    auto previewCatalog = Good(LoadEffectiveObjectCatalog(preview, Options()));
    Require(previewCatalog.Catalog().Find(ObjectType::Doodad, "D003") != nullptr, "read-only preview reads tables without listfile");
    auto mapFiles = noListFiles;
    mapFiles.push_back({"known.bin", Tests::ObjectBytes("fixture")});
    mapFiles.push_back({"(listfile)", Tests::ObjectBytes("known.bin\r\n")});
    const auto mapPath = temp.path / "anonymous.w3x"; Archive(mapPath, mapFiles);
    auto map = Good(MapArchiveSession::Open(mapPath));
    Require(map.HasOpaqueMembers(), "anonymous table is outside known member inventory");
    ObjectCatalogSources sources; sources.archive = &map; sources.gameRoot = gameRoot;
    auto catalog = Good(LoadEffectiveObjectCatalog(sources, Options()));
    Require(catalog.Catalog().Find(ObjectType::Doodad, "D003") && !catalog.Catalog().Find(ObjectType::Doodad, "D001"), "whole map table masks lower table");
    Require(catalog.Catalog().Find(ObjectType::Doodad, "D003")->displayName == "Original", "empty map profile masks lower profile");
    Require(catalog.Input("Units/Profile.txt")->present && catalog.Input("Units/Profile.txt")->byteSize == 0, "empty table differs from absent table");
    Require(VerifyEffectiveObjectCatalog(catalog, sources).Succeeded(), "anonymous capture can revalidate exact members");
    auto campaign = CampaignSession::Create();
    CampaignCommand add; add.operation = CampaignOperation::ImportResource;
    add.replacements = {{"Doodads/Doodads.slk", Table("D006")}};
    (void)Good(campaign->Apply(campaign->Revision(), add));
    sources.campaign = campaign.get();
    const std::vector<std::string> removed{"Doodads/Doodads.slk"}; sources.removals = removed;
    auto inherited = Good(LoadEffectiveObjectCatalog(sources, Options()));
    Require(inherited.Catalog().Find(ObjectType::Doodad, "D006") != nullptr, "map deletion exposes explicitly bound campaign");
    const std::vector<MemberReplacement> overlay{{"Doodads/Doodads.slk", Table("D009")}};
    sources.removals = {}; sources.replacements = overlay; sources.overlayIdentity = "owned-revision-1";
    auto session = Good(LoadEffectiveObjectCatalog(sources, Options()));
    Require(session.Catalog().Find(ObjectType::Doodad, "D009") && !session.Catalog().Find(ObjectType::Doodad, "D003"), "owned session table masks map");
    Require(!session.Catalog().Find(ObjectType::Doodad, "D009")->nativeOverridesSupported, "reading session SLK grants no write capability");
    auto parsed = LniObjectCodec(ObjectType::Doodad).Parse({});
    const auto* empty = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
    Require(empty != nullptr, "empty original document");
    auto descendant = *empty;
    Require(descendant.AddObject("D010", "D009").Succeeded(), "prepare native descendant");
    ObjectCommandContext context; context.type = ObjectType::Doodad; context.standard = &session.Catalog();
    Require(!VerifyObjectDocumentWriteSources(context, *empty, descendant).Succeeded(), "document gate rejects new descendants of read-only SLK base");
    sources.removals = removed;
    Require(!LoadEffectiveObjectCatalog(sources, Options()).Succeeded(), "replace and delete conflict cannot be resolved by order");
}
void TrustedAndStaleSources() {
    TemporaryDirectory temp;
    const auto gameRoot = temp.path / "game";
    const std::vector<MemberReplacement> gameFiles{{"Doodads/Doodads.slk", Table("D001", "WESTRING_SYNTHETIC")},
        {"UI/Strings.txt", Tests::ObjectBytes("WESTRING_SYNTHETIC=Trusted\n")}};
    Archive(gameRoot / "war3.mpq", gameFiles);
    const auto mapRoot = temp.path / "map";
    Write(mapRoot / "UI/Strings.txt", Tests::ObjectBytes("WESTRING_SYNTHETIC=Untrusted\n"));
    auto directory = Good(DirectoryWorkspace::Open(mapRoot));
    ObjectCatalogSources sources; sources.directory = &directory; sources.gameRoot = gameRoot;
    auto options = Options(); options.stringFiles = {"UI/Strings.txt"};
    auto catalog = Good(LoadEffectiveObjectCatalog(sources, options));
    Require(catalog.Catalog().Find(ObjectType::Doodad, "D001")->displayName == "Trusted", "map content cannot replace WESTRING metadata");
    Require(catalog.Input("UI/Strings.txt")->location.layer == Resources::ResourceLayer::Game, "trusted string location is actual game source");
    Require(catalog.Input("Doodads/Doodads.slk")->location.sourceId == "war3.mpq", "actual selected game MPQ retained");
    auto parsed = LniObjectCodec(ObjectType::Doodad).Parse({});
    const auto* empty = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
    Require(empty != nullptr, "empty original document");
    auto before = *empty; Require(before.AddOriginalObject("D001").Succeeded(), "original native row");
    auto after = before;
    Require(after.SetModification({ObjectType::Doodad, "D001"}, {"dnam"}, std::string("Edited")).Succeeded(), "native field change");
    ObjectCommandContext context; context.type = ObjectType::Doodad; context.standard = &catalog.Catalog();
    Require(VerifyObjectDocumentWriteSources(context, before, after).Succeeded(), "game source permits existing native write chain");
    const std::vector<MemberReplacement> overlay{{"Doodads/Doodads.slk", Table("D001")}};
    auto changedSources = sources; changedSources.replacements = overlay; changedSources.overlayIdentity = "new-read-only-base";
    auto changed = Good(LoadEffectiveObjectCatalog(changedSources, options)); context.standard = &changed.Catalog();
    Require(!VerifyObjectDocumentWriteSources(context, before, after).Succeeded(), "SLK imported after earlier native edits revokes save qualification");
    Archive(gameRoot / "war3patch.mpq", gameFiles);
    Require(!catalog.VerifyCapturedSources().Succeeded() && !VerifyEffectiveObjectCatalog(catalog, sources).Succeeded(), "new higher game archive revokes equal-byte snapshot");
    const std::vector<std::string> names{"Doodads/Doodads.slk"};
    ObjectCatalogSources mapOnly; mapOnly.directory = &directory;
    auto missing = Good(CaptureObjectCatalogSources(mapOnly, names));
    Require(!Good(missing.Read(names.front())), "fully checked missing directory member");
    Write(mapRoot / "Doodads/Doodads.slk", Table("D003"));
    Require(!missing.Verify().Succeeded(), "a formerly missing higher source appearing invalidates lease");
}
void BrokenAndDirectoryConflict() {
    TemporaryDirectory temp;
    const auto mapRoot = temp.path / "map";
    Write(mapRoot / "Doodads/Doodads.slk", Tests::ObjectBytes("ID;PWXL\nC;X0;Y1;K\"bad\"\nE\n"));
    const auto gameRoot = temp.path / "game";
    const std::vector<MemberReplacement> gameFiles{{"Doodads/Doodads.slk", Table("D001")}};
    Archive(gameRoot / "war3.mpq", gameFiles);
    auto directory = Good(DirectoryWorkspace::Open(mapRoot));
    ObjectCatalogSources sources; sources.directory = &directory; sources.gameRoot = gameRoot;
    Require(!LoadEffectiveObjectCatalog(sources, Options()).Succeeded(), "broken selected map table cannot fall back to game");
    Write(mapRoot / "map/Doodads/Doodads.slk", Table("D002"));
    auto conflicting = Good(DirectoryWorkspace::Open(mapRoot)); sources.directory = &conflicting;
    Require(!LoadEffectiveObjectCatalog(sources, Options()).Succeeded(), "root and map physical table conflict is explicit");
}
} // namespace
int main() {
    try { CampaignCaptureRetainsItsPathHardCapWithSharedDefaults(); AnonymousAndLayering(); TrustedAndStaleSources(); BrokenAndDirectoryConflict(); return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
