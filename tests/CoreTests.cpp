#include "War3MapCore/CompatibilityProfile.h"
#include "War3MapCore/EditorSession.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapInfo.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/RenderPlugin.h"

#include <Windows.h>

#include <cstddef>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class TempDirectory {
public:
    TempDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("War3MapEditor64-tests-" + std::to_string(GetCurrentProcessId()) +
             "-" + std::to_string(GetTickCount64()));
        std::filesystem::create_directory(path_);
    }

    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& Path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void WriteText(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    Require(static_cast<bool>(output), "unable to write test fixture");
}

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>(),
    };
}

std::vector<std::byte> Bytes(const std::string& text) {
    const auto* begin = reinterpret_cast<const std::byte*>(text.data());
    return {begin, begin + text.size()};
}

void TestProfiles() {
    const auto profile = War3Map::CompatibilityProfile::Default();
    Require(profile.Validate().Succeeded(), "default compatibility profile must validate");
    Require(profile.definitionSha256.size() == 64, "profile SHA must be stable");
}

void TestSha256Vectors() {
    const auto empty = War3Map::Sha256Text("");
    const auto abc = War3Map::Sha256Text("abc");
    Require(empty.Succeeded(), "empty SHA-256 must succeed");
    Require(abc.Succeeded(), "abc SHA-256 must succeed");
    Require(
        *empty.value == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "empty SHA-256 must match the standard vector");
    Require(
        *abc.value == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "abc SHA-256 must match the standard vector");
}

void TestSha256IndependentContexts() {
    // 标准长向量以及交错的文件/内存/多线程调用，确认缓存provider不共享hash状态。
    const std::string million(1000000, 'a');
    const std::string expected = "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";
    const auto longHash = War3Map::Sha256Text(million);
    Require(longHash.Succeeded() && *longHash.value == expected, "million-a standard SHA-256 vector");
    TempDirectory temp;
    const auto path = temp.Path() / "hash-vector.bin";
    WriteText(path, million);
    const auto file = War3Map::Sha256File(path);
    Require(file.Succeeded() && *file.value == expected, "file hash matches the standard vector");
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    Require(handle != INVALID_HANDLE_VALUE, "hash fixture handle opens");
    LARGE_INTEGER position{}; position.QuadPart = 173;
    const bool positioned = SetFilePointerEx(handle, position, nullptr, FILE_BEGIN) != FALSE;
    const auto fromHandle = War3Map::Sha256Handle(handle);
    LARGE_INTEGER after{}, zero{};
    const bool readPosition = SetFilePointerEx(handle, zero, &after, FILE_CURRENT) != FALSE;
    CloseHandle(handle);
    Require(positioned && fromHandle.Succeeded() && *fromHandle.value == expected
        && readPosition && after.QuadPart == 173, "handle hashes full bytes and restores the caller position");
    Require(!War3Map::Sha256Handle(INVALID_HANDLE_VALUE).Succeeded(), "invalid handle remains rejected");
    Require(!War3Map::Sha256File(temp.Path() / "missing").Succeeded(), "missing file remains rejected");
    std::vector<std::future<bool>> jobs;
    for (unsigned worker = 0; worker < 4; ++worker) jobs.push_back(std::async(std::launch::async, [] {
        for (unsigned i = 0; i < 64; ++i) {
            const auto abc = War3Map::Sha256Text("abc");
            const auto empty = War3Map::Sha256Text("");
            if (!abc.Succeeded() || !empty.Succeeded()
                || *abc.value != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
                || *empty.value != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") return false;
        }
        return true;
    }));
    for (auto& job : jobs) Require(job.get(), "independent SHA contexts across concurrent threads");
}

void MeasureHashThroughput() {
    // 显式诊断入口，不把墙钟性能阈值加入常规CTest。
    for (const std::size_t size : {512u, 1536u, 4096u}) {
        std::string payload(size, 'x'); payload.replace(0, 4, "SHA;");
        const auto expected = War3Map::Sha256Text(payload);
        Require(expected.Succeeded(), "benchmark initial hash");
        for (unsigned sample = 0; sample < 3; ++sample) {
            const auto started = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < 6077; ++i) {
                const auto actual = War3Map::Sha256Text(payload);
                Require(actual.Succeeded() && *actual.value == *expected.value, "benchmark full SHA identity");
            }
            const auto milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            std::cout << "{\"bytes\":" << size << ",\"count\":6077,\"sample\":" << sample
                << ",\"elapsed_ms\":" << milliseconds << ",\"sha256\":\"" << *expected.value << "\"}\n";
        }
    }
}

void TestTransactionCommit() {
    TempDirectory temp;
    const auto source = temp.Path() / "source";
    const auto candidate = temp.Path() / "candidate";
    WriteText(source / "table/unit.ini", "[hfoo]\nName=\"Footman\"\n");

    auto opened = War3Map::EditorSession::Open({
        source,
        War3Map::CompatibilityProfile::Default(),
        "catalog-test",
    });
    Require(opened.Succeeded(), "workspace must open");
    auto& session = *opened.value;
    const auto baseRevision = session.Revision();

    auto transaction = session.BeginEdit(baseRevision);
    Require(transaction.Succeeded(), "transaction must begin");
    Require(
        !session.BeginEdit(baseRevision).Succeeded(),
        "a second writer must be rejected");

    War3Map::ObjectData::LniObjectCodec codec(War3Map::ObjectData::ObjectType::Unit);
    auto parsed = codec.Parse(Bytes("[hfoo]\nName=\"Footman\"\n"));
    Require(parsed.Succeeded(), "object source must parse");
    auto* document = dynamic_cast<War3Map::ObjectData::LniObjectDocument*>(parsed.document.get());
    Require(document != nullptr, "object document type must match");
    Require(document->SetModification(
        {War3Map::ObjectData::ObjectType::Unit, "hfoo"},
        {"Name", std::nullopt, std::nullopt, std::nullopt},
        std::string("Candidate Footman")).Succeeded(), "object edit must succeed");
    auto patch = codec.BuildPatch(
        *document,
        "table/unit.ini",
        "synthetic\n\"object\" edit");
    Require(patch.Succeeded(), "codec must seal the object patch");
    Require(
        session.ApplyObjectPatch(*transaction.value, std::move(*patch.value)).Succeeded(),
        "object patch must apply");

    const auto preview = session.PreviewEdit(*transaction.value);
    Require(preview.Succeeded(), "preview must succeed");
    Require(preview.value->changes.size() == 1, "preview must contain one change");

    const auto validation = session.Validate(
        *transaction.value,
        session.Profile(),
        War3Map::CompatibilityLevel::Parsed);
    Require(validation.satisfied, "foundation L0 validation must pass");

    Require(
        session.CommitCandidate(
            *transaction.value,
            baseRevision,
            candidate).Succeeded(),
        "candidate commit must succeed");
    Require(
        ReadText(source / "table/unit.ini") == "[hfoo]\nName=\"Footman\"\n",
        "source must remain unchanged");
    Require(
        ReadText(candidate / "table/unit.ini") == "[hfoo]\nName=\"Candidate Footman\"\n",
        "candidate must contain the validated object patch");
    Require(
        std::filesystem::is_regular_file(temp.Path() / "candidate.war3me-manifest.json"),
        "candidate manifest sidecar must exist");
    Require(
        !std::filesystem::exists(candidate / ".war3me-candidate-manifest.txt"),
        "candidate must not contain editor-only manifest members");
    const auto manifest = ReadText(temp.Path() / "candidate.war3me-manifest.json");
    Require(
        manifest.find("synthetic\\n\\\"object\\\" edit") != std::string::npos,
        "manifest strings must be JSON escaped");
    Require(session.Revision().generation == baseRevision.generation + 1, "generation must advance");
}

void TestSafetyGates() {
    TempDirectory temp;
    const auto source = temp.Path() / "source";
    WriteText(source / "map/war3map.wtg", "WTG!original");
    WriteText(source / "table/unit.ini", "[hfoo]\nName=\"Footman\"\n");

    auto opened = War3Map::EditorSession::Open({
        source,
        War3Map::CompatibilityProfile::Default(),
        "catalog-test",
    });
    Require(opened.Succeeded(), "workspace must open");
    auto& session = *opened.value;
    auto transaction = session.BeginEdit(session.Revision());
    Require(transaction.Succeeded(), "transaction must begin");

    auto secondOpened = War3Map::EditorSession::Open({
        source,
        War3Map::CompatibilityProfile::Default(),
        "catalog-test",
    });
    Require(secondOpened.Succeeded(), "second read session must open");
    Require(
        !secondOpened.value->BeginEdit(secondOpened.value->Revision()).Succeeded(),
        "workspace-wide writer lease must reject a second session");

    const auto emptyValidation = session.Validate(
        *transaction.value,
        session.Profile(),
        War3Map::CompatibilityLevel::Parsed);
    Require(
        !emptyValidation.satisfied,
        "L0 must not pass before a codec-sealed patch exists");

    War3Map::ObjectData::LniObjectCodec codec(War3Map::ObjectData::ObjectType::Unit);
    auto parsed = codec.Parse(Bytes("[hfoo]\nName=\"Footman\"\n"));
    Require(parsed.Succeeded(), "object source must parse");
    auto* document = dynamic_cast<War3Map::ObjectData::LniObjectDocument*>(parsed.document.get());
    Require(document != nullptr, "object document type must match");
    Require(document->SetModification(
        {War3Map::ObjectData::ObjectType::Unit, "hfoo"},
        {"Name", std::nullopt, std::nullopt, std::nullopt},
        std::string("Changed")).Succeeded(), "safe edit must succeed");

    auto wrongDomain = codec.BuildPatch(*document, "map/war3map.wtg", "wrong domain");
    Require(wrongDomain.Succeeded(), "codec patch construction must succeed before domain check");
    Require(
        !session.ApplyObjectPatch(*transaction.value, std::move(*wrongDomain.value)).Succeeded(),
        "object patch must not modify a trigger member");

    auto differentSource = codec.Parse(Bytes("[hfoo]\nName=\"Different\"\n"));
    Require(differentSource.Succeeded(), "different source must parse");
    auto* differentDocument = dynamic_cast<War3Map::ObjectData::LniObjectDocument*>(
        differentSource.document.get());
    Require(differentDocument != nullptr, "different object document type must match");
    auto stalePatch = codec.BuildPatch(*differentDocument, "table/unit.ini", "stale identity");
    Require(stalePatch.Succeeded(), "different-source patch must be sealable");
    Require(
        !session.ApplyObjectPatch(*transaction.value, std::move(*stalePatch.value)).Succeeded(),
        "patch parsed from different source bytes must be rejected");

    auto validPatch = codec.BuildPatch(*document, "table/unit.ini", "safe object edit");
    Require(validPatch.Succeeded(), "valid object patch must build");
    Require(session.ApplyObjectPatch(
        *transaction.value, std::move(*validPatch.value)).Succeeded(), "valid patch must apply");

    Require(
        !session.CommitCandidate(
            *transaction.value,
            session.Revision(),
            temp.Path() / "SOURCE" / "candidate").Succeeded(),
        "case-insensitive path aliases inside the source must be rejected");

    WriteText(source / "map/war3map.wtg", "WTG!externally-changed");
    Require(
        !session.CommitCandidate(
            *transaction.value,
            session.Revision(),
            temp.Path() / "candidate").Succeeded(),
        "external source changes must block commit");
    Require(
        !std::filesystem::exists(temp.Path() / "candidate"),
        "failed commit must not create final candidate");
    Require(session.Rollback(*transaction.value).Succeeded(), "rollback must succeed");
}

void TestTypedMissingObjectCreation() {
    using namespace War3Map;
    using namespace War3Map::ObjectData;
    TempDirectory temp;
    const auto source = temp.Path() / "source";
    WriteText(source / "map/readme.txt", "original fixture remains read-only");
    auto opened = EditorSession::Open({source, CompatibilityProfile::Default(), "catalog-test"});
    Require(opened.Succeeded(), "missing-object fixture opens");
    auto& session = *opened.value;
    const auto revision = session.Revision();
    const auto transaction = session.BeginEdit(revision);
    Require(transaction.Succeeded(), "typed creation owns the normal writer lease");
    LniObjectCodec codec(ObjectType::Unit);
    auto parsed = codec.Parse(Bytes(""));
    auto* document = parsed.Succeeded() ? dynamic_cast<LniObjectDocument*>(parsed.document.get()) : nullptr;
    Require(document && document->AddObject("H009", "hfoo").Succeeded(), "missing category starts from an actual empty LNI document");
    Require(document->SetModification({ObjectType::Unit, "H009"}, {"unam"}, std::string{"Created by typed command"}).Succeeded(),
        "first object receives a real typed value");
    const auto apply = [&](const std::filesystem::path& path, bool create) {
        auto patch = codec.BuildPatch(*document, path, "新增单位类别");
        Require(patch.Succeeded(), "test uses a codec-sealed patch");
        return create ? session.CreateObjectMember(*transaction.value, std::move(*patch.value))
            : session.ApplyObjectPatch(*transaction.value, std::move(*patch.value));
    };
    Require(!apply("table/unit.ini", false).Succeeded(), "generic existing-member replacement still rejects new files");
    Require(!apply("table/ability.ini", true).Succeeded(), "unit codec cannot create an ability member");
    Require(!apply("map/war3map.w3u", true).Succeeded(), "LNI codec cannot create a native member");
    Require(!apply("../unit.ini", true).Succeeded(), "creation rejects escaping paths");
    auto preview = session.PreviewEdit(*transaction.value);
    Require(preview.Succeeded() && preview.value->changes.empty(), "failed creations leave the complete overlay unchanged");
    auto nonempty = codec.Parse(Bytes("[hfoo]\nunam=\"copied old source\"\n"));
    const auto* old = nonempty.Succeeded() ? dynamic_cast<const LniObjectDocument*>(nonempty.document.get()) : nullptr;
    Require(old != nullptr, "nonempty source parses for the negative proof test");
    auto oldPatch = codec.BuildPatch(*old, "table/unit.ini", "nonempty baseline");
    Require(oldPatch.Succeeded() && !session.CreateObjectMember(*transaction.value, std::move(*oldPatch.value)).Succeeded(),
        "missing member cannot be created from another nonempty source identity");
    Require(apply("table/unit.ini", true).Succeeded(), "codec empty-baseline creation succeeds");
    Require(!apply("UNIT.INI", true).Succeeded(), "an existing overlay cannot be duplicated through a root/case alias");
    Require(document->SetModification({ObjectType::Unit, "H009"}, {"unam"}, std::string{"Second typed edit"}).Succeeded(),
        "caller can continue editing the same missing category");
    Require(apply("table/unit.ini", true).Succeeded(), "subsequent edit replaces the same typed creation overlay");
    preview = session.PreviewEdit(*transaction.value);
    Require(preview.Succeeded() && preview.value->changes.size() == 1
        && preview.value->changes.front().kind == "object" && preview.value->changes.front().beforeSha256.empty(),
        "preview records absent source separately from a present empty source");

    NativeObjectCodec nativeCodec(ObjectType::Ability);
    auto native = NativeObjectDocument::CreateEmpty(ObjectType::Ability, 2);
    Require(native.Succeeded() && native.value->AddCustomObject("A009", "A000").Succeeded(), "classic-v2 native empty base creates a real object");
    Require(native.value->AddModification({ObjectType::Ability, "A009"}, {"anam", 1u, 0u}, NativeValueType::String,
        std::string{"Typed ability"}).Succeeded(), "ability fixture has its actual native field layout");
    auto nativeWrong = nativeCodec.BuildPatch(*native.value, "map/war3map.w3u", "wrong native category");
    Require(nativeWrong.Succeeded() && !session.CreateObjectMember(*transaction.value, std::move(*nativeWrong.value)).Succeeded(),
        "native sealed category must match the native path");
    auto nativeAsLni = nativeCodec.BuildPatch(*native.value, "table/ability.ini", "wrong native format");
    Require(nativeAsLni.Succeeded() && !session.CreateObjectMember(*transaction.value, std::move(*nativeAsLni.value)).Succeeded(),
        "native codec cannot create an LNI member");
    std::vector<std::byte> versionOneBytes(12);
    versionOneBytes.front() = std::byte{1};
    NativeObjectCodec itemCodec(ObjectType::Item);
    const auto versionOne = itemCodec.Parse(versionOneBytes);
    const auto* versionOneDocument = versionOne.Succeeded() ? dynamic_cast<const NativeObjectDocument*>(versionOne.document.get()) : nullptr;
    Require(versionOneDocument != nullptr, "v1 empty table is a readable negative fixture");
    auto versionOnePatch = itemCodec.BuildPatch(*versionOneDocument, "map/war3map.w3t", "v1 absence");
    Require(versionOnePatch.Succeeded() && !session.CreateObjectMember(*transaction.value, std::move(*versionOnePatch.value)).Succeeded(),
        "missing native creation cannot silently choose a v1 baseline");
    auto nativePatch = nativeCodec.BuildPatch(*native.value, "map/war3map.w3a", "新增技能类别");
    Require(nativePatch.Succeeded() && session.CreateObjectMember(*transaction.value, std::move(*nativePatch.value)).Succeeded(),
        "classic-v2 typed native creation uses the same session transaction");
    const auto expectedNative = nativeCodec.Serialize(*native.value);
    const auto candidate = temp.Path() / "candidate";
    Require(session.CommitCandidate(*transaction.value, revision, candidate).Succeeded(), "normal guarded candidate commit publishes both typed members");
    Require(!std::filesystem::exists(source / "table/unit.ini") && !std::filesystem::exists(source / "map/war3map.w3a"),
        "source workspace remains missing those members after candidate publication");
    const auto reopened = codec.Parse(Bytes(ReadText(candidate / "table/unit.ini")));
    const auto* unit = reopened.Succeeded() ? dynamic_cast<const LniObjectDocument*>(reopened.document.get()) : nullptr;
    Require(unit && unit->FindObject({ObjectType::Unit, "H009"}), "created LNI member reopens with the real object");
    Require(expectedNative.Succeeded() && Bytes(ReadText(candidate / "map/war3map.w3a")) == *expectedNative.value,
        "created native member publishes exact codec bytes");
}

void TestMissingObjectCreationSourceGuards() {
    using namespace War3Map;
    using namespace War3Map::ObjectData;
    const auto check = [](bool external) {
        TempDirectory temp;
        const auto source = temp.Path() / "source";
        WriteText(source / "map/readme.txt", "source");
        if (!external) WriteText(source / "TABLE/UNIT.INI", "");
        auto opened = EditorSession::Open({source, CompatibilityProfile::Default(), "catalog-test"});
        Require(opened.Succeeded(), "source-guard fixture opens");
        const auto transaction = opened.value->BeginEdit(opened.value->Revision());
        Require(transaction.Succeeded(), "source-guard transaction starts");
        LniObjectCodec codec(ObjectType::Unit);
        auto parsed = codec.Parse(Bytes(""));
        const auto* document = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
        Require(document != nullptr, "empty creation document parses");
        auto patch = codec.BuildPatch(*document, "unit.ini", "typed absence check");
        Require(patch.Succeeded(), "guard patch is sealed");
        if (external) WriteText(source / "table/unit.ini", "externally added");
        Require(!opened.value->CreateObjectMember(*transaction.value, std::move(*patch.value)).Succeeded(),
            external ? "external member creation invalidates captured absence" : "present empty member at case/path alias is not absence");
        const auto preview = opened.value->PreviewEdit(*transaction.value);
        Require(preview.Succeeded() && preview.value->changes.empty(), "source guard failure does not publish partial overlay");
        Require(opened.value->Rollback(*transaction.value).Succeeded(), "failed creation releases the normal transaction cleanly");
    };
    check(false);
    check(true);
}

void TestRendererAbi() {
    const auto* api = War3MapEditor_GetRenderPluginApi(WAR3_RENDER_PLUGIN_API_VERSION);
    Require(api != nullptr, "renderer API table must be available");
    Require(
        api->structSize == sizeof(War3RenderPluginApiV1) &&
        api->apiVersion == WAR3_RENDER_PLUGIN_API_VERSION,
        "renderer ABI version must match");
    auto* renderer = api->createRenderer();
    Require(renderer != nullptr, "foundation renderer must be created without a device");
    War3SceneSnapshotV1 snapshot{};
    snapshot.structSize = sizeof(snapshot);
    snapshot.schemaVersion = 1;
    snapshot.documentRevision = 7;
    const char payload[] = "grid";
    snapshot.payload = payload;
    snapshot.payloadSize = sizeof(payload) - 1;
    Require(api->submitSceneSnapshot(renderer, &snapshot) == 0, "valid snapshot must be accepted");
    snapshot.payload = nullptr;
    Require(api->submitSceneSnapshot(renderer, &snapshot) != 0, "payload size without bytes must be rejected");
    int window = 0;
    Require(api->attachNativeWindow(renderer, &window) == 0, "native window handle must be retained by the stub");
    Require(api->attachNativeWindow(renderer, nullptr) != 0, "missing native window must be rejected");
    api->destroyRenderer(renderer);
    Require(
        War3MapEditor_GetRenderPluginApi(WAR3_RENDER_PLUGIN_API_VERSION + 1) == nullptr,
        "renderer must reject unknown API versions");
}

void TestMapInfo() {
    // 构造合成 w3i 字节。
    std::vector<std::byte> w3i;
    auto putI32 = [&w3i](std::int32_t v) {
        const std::size_t at = w3i.size();
        w3i.resize(at + 4);
        std::memcpy(w3i.data() + at, &v, 4);
    };
    auto putStr = [&w3i](const char* s) {
        while (*s) w3i.push_back(static_cast<std::byte>(*s++));
        w3i.push_back(std::byte{0});
    };
    putI32(25);  // version
    putI32(3);   // saves
    putI32(6072); // editorVersion
    putStr("Test Map");
    putStr("Author X");
    putStr("A test map");
    putStr("1-4");
    // 模拟尾部数据（相机/玩家等）。
    for (int i = 0; i < 64; ++i) {
        w3i.push_back(static_cast<std::byte>(i));
    }

    // 解析。
    auto parsed = War3Map::MapInfo::Parse(w3i);
    Require(parsed.Succeeded() && parsed.value.has_value(), "w3i parse must succeed");
    auto& info = *parsed.value;
    Require(info.formatVersion == 25, "version must be 25");
    Require(info.saves == 3, "saves must be 3");
    Require(info.mapName == "Test Map", "mapName mismatch");
    Require(info.author == "Author X", "author mismatch");
    Require(info.description == "A test map", "description mismatch");
    Require(info.recommendedPlayers == "1-4", "recommendedPlayers mismatch");
    Require(info.trailing.size() == 64, "trailing size must be 64");

    // 零修改往返。
    const auto serialized = info.Serialize();
    Require(serialized == w3i, "zero-edit roundtrip must be byte-identical");

    // 编辑字段后往返。
    info.mapName = "Edited Map";
    info.author = "New Author";
    const auto edited = info.Serialize();
    auto reparsed = War3Map::MapInfo::Parse(edited);
    Require(reparsed.Succeeded() && reparsed.value.has_value(), "edited w3i must re-parse");
    Require(reparsed.value->mapName == "Edited Map", "edited mapName must persist");
    Require(reparsed.value->author == "New Author", "edited author must persist");
    Require(reparsed.value->trailing.size() == 64, "trailing must be preserved after edit");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--measure-hash-throughput") {
            MeasureHashThroughput();
            return 0;
        }
        static_assert(!std::is_default_constructible_v<War3Map::TriggerPatch>);
        static_assert(!std::is_default_constructible_v<War3Map::ObjectPatch>);
        TestProfiles();
        TestSha256Vectors();
        TestSha256IndependentContexts();
        TestTransactionCommit();
        TestSafetyGates();
        TestTypedMissingObjectCreation();
        TestMissingObjectCreationSourceGuards();
        TestRendererAbi();
        TestMapInfo();
        std::cout << "War3MapEditor64 foundation tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "TEST FAILURE: " << exception.what() << '\n';
        return 1;
    }
}
