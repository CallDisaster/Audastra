#include "War3MapCore/Campaign/CampaignInfo.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/Resources/ImportTable.h"
#include "War3MapCore/Services/CampaignService.h"
#include "../MapDataFixtures.h"
#include "../../src/Core/Mpq/MpqCrypt.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::Services;

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}
template<class T> T Good(Result<T> result) {
    if (!result.Succeeded() || !result.value) {
        std::string message = "campaign result failed";
        for (const auto& diagnostic : result.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
    return std::move(*result.value);
}
void Good(const Status& status) {
    if (!status.Succeeded()) {
        std::string message = "campaign status failed";
        for (const auto& diagnostic : status.diagnostics) message += " | " + diagnostic.code + ": " + diagnostic.message;
        throw std::runtime_error(message);
    }
}
class TemporaryDirectory {
public:
    TemporaryDirectory() {
        static std::atomic_uint count{0};
        path = std::filesystem::absolute(std::filesystem::temp_directory_path()) /
            ("audastra-campaign-test-" + std::to_string(GetCurrentProcessId()) + "-" +
             std::to_string(GetTickCount64()) + "-" + std::to_string(count.fetch_add(1)));
        Require(std::filesystem::create_directory(path), "exclusive owned temporary directory");
    }
    ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(path, error); }
    std::filesystem::path path;
};
std::vector<std::byte> Bytes(std::string_view text) {
    const auto view = std::as_bytes(std::span(text.data(), text.size()));
    return {view.begin(), view.end()};
}
std::vector<std::byte> Read(const std::filesystem::path& path) { return Good(IO::ReadFileBytes(path, 16u * 1024u * 1024u)); }
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(file.good(), "write owned fixture");
}
std::uint32_t U32(std::span<const std::byte> bytes, std::size_t at) {
    Require(at <= bytes.size() && bytes.size() - at >= 4, "raw field available");
    std::uint32_t value = 0;
    for (unsigned index = 0; index < 4; ++index) value |= std::to_integer<std::uint32_t>(bytes[at + index]) << (index * 8);
    return value;
}
void Put32(std::vector<std::byte>& bytes, std::size_t at, std::uint32_t value) {
    for (unsigned index = 0; index < 4; ++index) bytes.at(at + index) = static_cast<std::byte>((value >> (index * 8)) & 255);
}
std::vector<std::byte> Header(std::string_view name, std::uint32_t flags) {
    MapDataFixtures::Fixture writer;
    writer.Raw("HM3W"); writer.Int(0); writer.String(name); writer.UInt(flags); writer.Int(1);
    writer.bytes.resize(512); writer.bytes[400] = std::byte{0xa7};
    return writer.bytes;
}
void CheckHeader(const std::filesystem::path& path, std::string_view name, std::uint32_t flags) {
    const auto bytes = Read(path);
    Require(bytes.size() > 512 && U32(bytes, 0) == 0x57334d48u && U32(bytes, 4) == 0, "classic HM3W magic and reserved word");
    const auto expected = Bytes(name);
    Require(std::equal(expected.begin(), expected.end(), bytes.begin() + 8), "campaign header uses current localized name");
    const auto end = 8 + name.size();
    Require(bytes.at(end) == std::byte{0}, "campaign header name terminator");
    Require(U32(bytes, end + 1) == flags && U32(bytes, end + 5) == 1, "campaign header flags and final one");
}

// 独立写出一份带两章、三个菜单按钮的 v1 原始字节；不调用待测序列化器生成预期值。
std::vector<std::byte> InfoWire() {
    MapDataFixtures::Fixture wire;
    wire.Int(1); wire.Int(7); wire.Int(6061);
    wire.String("原创战役"); wire.String("普通"); wire.String("Fixture"); wire.String("Two original chapters");
    wire.UInt(0x80000003u); wire.Int(-1); wire.String("Background.mdx"); wire.String("Minimap.blp");
    wire.Int(-1); wire.String("Ambient.wav"); wire.Int(2);
    wire.UInt(0x80000000u); wire.UInt(0x7fc00017u); wire.Float(0.25f);
    wire.Byte(17); wire.Byte(34); wire.Byte(51); wire.Byte(68); wire.Int(2);
    wire.UInt(3);
    for (const auto& path : {"chapter-a.w3x", "chapter-b.w3x", "chapter-a.w3x"}) {
        wire.Int(7); wire.String("Chapter"); wire.String("TRIGSTR_0001"); wire.String(path);
    }
    wire.UInt(2);
    wire.String("unknown-prefix-a"); wire.String("chapter-a.w3x");
    wire.String("unknown-prefix-b"); wire.String("chapter-b.w3x");
    wire.Byte(0xef); wire.Byte(0); wire.Byte(0x91);
    return wire.bytes;
}
void CodecRoundTripAndRejection() {
    const auto original = InfoWire();
    auto info = Good(Campaign::ParseInfo(original));
    Require(info.name == "原创战役" && info.maps.size() == 2 && info.buttons.size() == 3, "independent wire fields");
    Require(info.maps[0].id != info.buttons[0].id && info.buttons[0].mapPath == info.buttons[2].mapPath,
        "chapters and buttons have separate identities, repeated menu target is valid");
    Require(info.maps[1].unknown == "unknown-prefix-b" && info.buttons[0].visible == 7, "unknown prefix and noncanonical visibility preserved");
    Require(info.fogBits[0] == 0x80000000u && info.fogBits[1] == 0x7fc00017u, "NaN payload and negative zero preserved");
    Require(Good(Campaign::SerializeInfo(info)) == original, "untouched W3F must be byte exact");
    info.name = "Changed";
    auto changed = Good(Campaign::ParseInfo(Good(Campaign::SerializeInfo(info))));
    Require(changed.name == "Changed" && changed.trailing == Bytes(std::string_view("\xef\0\x91", 3)), "edited codec retains unknown tail");
    auto unknown = original; Put32(unknown, 0, 2);
    Require(!Campaign::ParseInfo(unknown).Succeeded(), "unknown format cannot be guessed");
    for (std::size_t size = 0; size < original.size() - 3; ++size)
        Require(!Campaign::ParseInfo(std::span(original).first(size)).Succeeded(), "every truncated known field rejected");
    info.name = std::string("bad\0name", 8);
    Require(!Campaign::SerializeInfo(info).Succeeded(), "embedded NUL cannot silently truncate text");
    info.name = "ok"; info.maps.resize(Campaign::kMaximumCampaignEntries + 1);
    Require(!Campaign::SerializeInfo(info).Succeeded(), "entry budget enforced on writes");
}

std::filesystem::path Chapter(const std::filesystem::path& root, std::string_view file, std::string_view marker) {
    const auto path = root / file;
    const std::vector<MapArchiveMember> files{{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}, {"original-note.txt", Bytes(marker)}};
    (void)Good(WriteVerifiedArchive(path, files));
    return path;
}
StableId AddMap(CampaignSession& session, const std::filesystem::path& chapter, const std::string& member) {
    CampaignCommand command; command.operation = CampaignOperation::AddMap; command.path = member; command.mapSource = chapter;
    return Good(session.Apply(session.Revision(), command));
}
StableId AddButton(CampaignSession& session, const std::string& map, std::string title) {
    CampaignCommand command; command.operation = CampaignOperation::AddButton;
    command.button.mapPath = map; command.button.chapterTitle = std::move(title); command.button.mapTitle = "Original"; command.button.visible = 1;
    return Good(session.Apply(session.Revision(), command));
}
void NoPartial(const std::filesystem::path& root) {
    for (const auto& entry : std::filesystem::directory_iterator(root))
        Require(!entry.path().filename().wstring().starts_with(L".forge-candidate-"), "failed publication left a temporary archive");
}
void EditingIdentityHistoryAndFailure() {
    TemporaryDirectory temporary;
    const auto first = Chapter(temporary.path, "first.w3x", "chapter one");
    const auto second = Chapter(temporary.path, "second.w3x", "chapter two");
    const auto firstHash = Good(Sha256File(first)), secondHash = Good(Sha256File(second));
    auto session = CampaignSession::Create();
    const auto oldRevision = session->Revision();
    const auto one = AddMap(*session, first, "chapter-a.w3x");
    const auto two = AddMap(*session, second, "chapter-b.w3x");
    const auto button = AddButton(*session, "chapter-a.w3x", "First");
    const auto alternative = AddButton(*session, "chapter-a.w3x", "Alternate");
    Require(one != two && one != button && button != alternative, "independent stable objects");
    CampaignCommand move; move.operation = CampaignOperation::MoveMap; move.id = two; move.position = 0;
    const auto before = session->Revision();
    Require(!session->Apply(oldRevision, move).Succeeded() && session->Revision() == before, "stale revision cannot publish");
    auto other = CampaignSession::Create();
    Require(!session->Apply(other->Revision(), move).Succeeded(), "other campaign identity cannot edit this session");
    (void)Good(session->Apply(before, move));
    Require(session->Info().maps.front().id == two && session->Info().buttons.front().id == button, "reorder maps leaves menu order intact");
    Good(session->Undo(session->Revision())); Require(session->Info().maps.front().id == one, "undo map reorder");
    Good(session->Redo(session->Revision())); Require(session->Info().maps.front().id == two, "redo map reorder");
    CampaignCommand remove; remove.operation = CampaignOperation::RemoveMap; remove.id = one;
    Require(!session->Apply(session->Revision(), remove).Succeeded(), "referenced chapter cannot silently disappear");
    CampaignCommand info; info.operation = CampaignOperation::SetInfo; info.info = session->Info(); info.info.name = "原创示例"; info.info.flags = 3;
    (void)Good(session->Apply(session->Revision(), info));
    const auto output = temporary.path / "two-chapters.w3n";
    const auto saved = Good(session->Save(session->Revision(), output, false));
    Require(saved.written.sha256 == Good(Sha256File(output)) && !session->Dirty(), "save receipt binds published bytes");
    CheckHeader(output, "原创示例", 3);
    auto reopened = Good(CampaignSession::Open(output));
    Require(reopened->Info().maps.size() == 2 && reopened->Info().buttons.size() == 2, "reopen keeps independent lists");
    Require(Good(reopened->ReadMember(reopened->Revision(), "chapter-a.w3x")) == Read(first), "chapter payload byte exact");
    const auto exported = temporary.path / "export.w3x";
    Good(reopened->ExportMember(reopened->Revision(), "chapter-b.w3x", exported));
    Require(Read(exported) == Read(second), "chapter export is exact");
    Require(!reopened->ExportMember(reopened->Revision(), "chapter-a.w3x", exported).Succeeded(), "export refuses existing target");
    const auto revision = session->Revision();
    Require(!session->Save(revision, output, false).Succeeded(), "save-as refuses existing target");
    Require(!session->Save(revision, temporary.path / "absent" / "fail.w3n", false).Succeeded(), "missing parent rejected");
    Require(!session->Save(revision, temporary.path / "wrong.w3x", false).Succeeded(), "campaign cannot be saved as map");
    Require(session->Revision() == revision && Good(Sha256File(output)) == saved.written.sha256, "failed save retains revision and source bytes");
    Require(!session->Members(revision, 0, 0).Succeeded() && !session->Members(revision, 0, 257).Succeeded(), "bounded queries");
    Require(Good(session->Members(revision, 0, 1)).members.size() == 1, "query page honored");
    remove.id = two;
    (void)Good(session->Apply(session->Revision(), remove));
    (void)Good(session->Save(session->Revision(), temporary.path / "chapter-removed.w3n", false));
    Require(!session->ReadMember(session->Revision(), "chapter-b.w3x").Succeeded(), "removed chapter absent from saved session");
    Good(session->Undo(session->Revision()));
    Require(Good(session->ReadMember(session->Revision(), "chapter-b.w3x")) == Read(second), "save followed by undo restores removed chapter bytes");
    Good(session->Redo(session->Revision()));
    Require(!session->ReadMember(session->Revision(), "chapter-b.w3x").Succeeded(), "redo removes same chapter after baseline change");
    Require(Good(Sha256File(first)) == firstHash && Good(Sha256File(second)) == secondHash, "chapter inputs remained read only");
    NoPartial(temporary.path);
}

void SharedDataHeaderAndSaveUndo() {
    TemporaryDirectory temporary;
    Campaign::CampaignInfo original; original.name = "TRIGSTR_0001"; original.flags = 1;
    original.trailing = {std::byte{0xf3}, std::byte{0}, std::byte{0x94}};
    const std::vector<MapArchiveMember> files{{"war3campaign.w3f", Good(Campaign::SerializeInfo(original))},
        {"war3campaign.wts", Bytes("STRING 1\r\n{\r\nLocalized name\r\n}\r\n")},
        {"unknown/data.bin", {std::byte{0}, std::byte{0xff}, std::byte{0x91}}}};
    const auto source = temporary.path / "source.w3n";
    (void)Good(WriteVerifiedArchive(source, files, Header("Localized name", 1)));
    const auto sourceHash = Good(Sha256File(source));
    auto session = Good(CampaignSession::Open(source));
    Require(Good(session->ResolveText("TRIGSTR_0001")) == "Localized name", "campaign WTS resolution");
    CampaignCommand shared; shared.operation = CampaignOperation::ReplaceSharedData;
    for (int value = 0; value < 7; ++value) {
        const auto type = static_cast<ObjectData::ObjectType>(value);
        const auto document = Good(ObjectData::NativeObjectDocument::CreateEmpty(type));
        shared.replacements.push_back({CampaignSession::ObjectMember(type), Good(ObjectData::NativeObjectCodec(type).Serialize(document))});
    }
    (void)Good(session->Apply(session->Revision(), shared));
    CampaignCommand imported; imported.operation = CampaignOperation::ImportResource;
    imported.replacements = {{"war3campaignImported/sample.txt", Bytes("first resource")}};
    (void)Good(session->Apply(session->Revision(), imported));
    const auto table = Good(Resources::ImportTable::Parse(Good(session->ReadMember(session->Revision(), "war3campaign.imp")), Resources::ImportScope::Campaign));
    Require(!table.Entries().empty(), "campaign resource creates scoped import table");
    const auto baseline = temporary.path / "baseline.w3n";
    (void)Good(session->Save(session->Revision(), baseline, false));
    CheckHeader(baseline, "Localized name", 1);
    imported.replacements.front().data = Bytes("second resource");
    (void)Good(session->Apply(session->Revision(), imported));
    InPlaceSaveOptions options; options.backupDirectory = temporary.path / "backups"; options.keepBackups = 2;
    const auto baselineBytes = Read(baseline);
    const auto saved = Good(session->Save(session->Revision(), {}, true, options));
    Require(Read(saved.backup) == baselineBytes, "in-place save backs up exact previous archive");
    Good(session->Undo(session->Revision()));
    Require(Good(session->ReadMember(session->Revision(), "war3campaignImported/sample.txt")) == Bytes("first resource"), "undo after save restores old payload");
    Good(session->Redo(session->Revision()));
    Require(Good(session->ReadMember(session->Revision(), "war3campaignImported/sample.txt")) == Bytes("second resource"), "redo after save restores new payload");
    CampaignCommand settings; settings.operation = CampaignOperation::SetInfo; settings.info = session->Info();
    settings.info.name = "New campaign header name"; settings.info.flags = 0x80000003u; settings.info.trailing.clear();
    (void)Good(session->Apply(session->Revision(), settings));
    const auto renamed = temporary.path / "renamed.w3n";
    (void)Good(session->Save(session->Revision(), renamed, false));
    CheckHeader(renamed, settings.info.name, settings.info.flags);
    const auto archive = Good(MapArchiveSession::Open(renamed));
    Require(archive.Prefix().size() == 512 && archive.Prefix()[400] == std::byte{0xa7}, "uninterpreted header padding preserved");
    const auto info = Good(Campaign::ParseInfo(Good(archive.ReadMember("war3campaign.w3f"))));
    Require(info.trailing == original.trailing, "settings command cannot delete unknown W3F tail");
    Require(Good(archive.ReadMember("unknown/data.bin")) == files[2].data, "unknown archive member survives edits");
    for (const auto& replacement : shared.replacements) Require(Good(archive.ReadMember(replacement.name)) == replacement.data, "seven campaign object kinds saved");
    Require(!archive.HasMember("war3map.w3u") && !archive.HasMember("war3map.imp"), "shared data never leaks into map namespace");
    Require(Good(Sha256File(source)) == sourceHash, "original source unchanged by save-as");
    NoPartial(temporary.path);
}

void OpaqueHeaderAndChangedSource() {
    TemporaryDirectory temporary;
    Campaign::CampaignInfo info; info.name = "Opaque";
    const std::array<MapArchiveMember, 3> members{{{"war3campaign.w3f", Good(Campaign::SerializeInfo(info))},
        {"hidden.bin", Bytes("hidden original bytes")}, {"(listfile)", Bytes("war3campaign.w3f\r\n")}}};
    // 独立小 MPQ：一个合法成员未列入 listfile，检验受保护的匿名块路径。
    std::vector<std::byte> mpq(32);
    std::array<std::uint32_t, 3> offsets{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        offsets[i] = static_cast<std::uint32_t>(mpq.size()); mpq.insert(mpq.end(), members[i].data.begin(), members[i].data.end());
    }
    const auto hashOffset = static_cast<std::uint32_t>(mpq.size()); mpq.resize(mpq.size() + 8 * 16, std::byte{0xff});
    const auto blockOffset = static_cast<std::uint32_t>(mpq.size()); mpq.resize(mpq.size() + 3 * 16);
    const auto& crypt = Mpq::MpqCrypt::Instance();
    for (std::uint32_t i = 0; i < members.size(); ++i) {
        auto bucket = crypt.HashString(members[i].name, Mpq::MpqCrypt::HashType::TableOffset) & 7;
        while (U32(mpq, hashOffset + bucket * 16 + 12) != 0xffffffffu) bucket = (bucket + 1) & 7;
        const auto at = hashOffset + bucket * 16;
        Put32(mpq, at, crypt.HashString(members[i].name, Mpq::MpqCrypt::HashType::NameA));
        Put32(mpq, at + 4, crypt.HashString(members[i].name, Mpq::MpqCrypt::HashType::NameB)); Put32(mpq, at + 8, 0); Put32(mpq, at + 12, i);
        Put32(mpq, blockOffset + i * 16, offsets[i]);
        Put32(mpq, blockOffset + i * 16 + 4, static_cast<std::uint32_t>(members[i].data.size()));
        Put32(mpq, blockOffset + i * 16 + 8, static_cast<std::uint32_t>(members[i].data.size()));
        Put32(mpq, blockOffset + i * 16 + 12, Mpq::kFileExists | Mpq::kFileSingleUnit);
    }
    crypt.EncryptBlock(mpq.data() + hashOffset, 8 * 16, crypt.HashString("(hash table)", Mpq::MpqCrypt::HashType::FileKey));
    crypt.EncryptBlock(mpq.data() + blockOffset, 3 * 16, crypt.HashString("(block table)", Mpq::MpqCrypt::HashType::FileKey));
    Put32(mpq, 0, 0x1a51504du); Put32(mpq, 4, 32); Put32(mpq, 8, static_cast<std::uint32_t>(mpq.size()));
    Put32(mpq, 12, 0x00030000u); Put32(mpq, 16, hashOffset); Put32(mpq, 20, blockOffset); Put32(mpq, 24, 8); Put32(mpq, 28, 3);
    auto bytes = Header(info.name, info.flags); bytes.insert(bytes.end(), mpq.begin(), mpq.end());
    const auto source = temporary.path / "opaque.w3n"; Write(source, bytes);
    Require(Good(MapArchiveSession::Open(source)).HasOpaqueMembers(), "fixture really contains an unnamed block");
    auto session = Good(CampaignSession::Open(source));
    CampaignCommand settings; settings.operation = CampaignOperation::SetInfo; settings.info = session->Info(); settings.info.description = "changed";
    (void)Good(session->Apply(session->Revision(), settings));
    const auto copied = temporary.path / "opaque-copy.w3n";
    (void)Good(session->Save(session->Revision(), copied, false));
    Require(Good(Good(Mpq::MpqArchive::OpenReadOnly(copied)).ReadFile("hidden.bin")) == members[1].data, "anonymous member preserved by supported save");
    settings.info = session->Info(); settings.info.name = "Header change requires refusal";
    (void)Good(session->Apply(session->Revision(), settings));
    const auto refused = temporary.path / "opaque-refused.w3n";
    Require(!session->Save(session->Revision(), refused, false).Succeeded() && !std::filesystem::exists(refused), "unsupported anonymous-header update leaves no output");
    auto stale = Good(CampaignSession::Open(source));
    bytes.at(400) = std::byte{0xb8}; Write(source, bytes);
    const auto target = temporary.path / "source-changed.w3n";
    const auto revision = stale->Revision();
    Require(!stale->Save(revision, target, false).Succeeded() && stale->Revision() == revision && !std::filesystem::exists(target), "external change refuses save without publishing");
    NoPartial(temporary.path);
}
} // namespace

int main() {
    try {
        CodecRoundTripAndRejection(); EditingIdentityHistoryAndFailure(); SharedDataHeaderAndSaveUndo(); OpaqueHeaderAndChangedSource();
        std::cout << "Campaign codec, identity, shared data, header, atomic save and history checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
