#include "NameQueryCommand.h"
#include "War3MapCore/Services/NameResolution.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Mpq/MpqArchive.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"
#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerTypes.h"

#include <nlohmann/json.hpp>
#include <Windows.h>
#include <array>
#include <iostream>

namespace {
std::string Utf8(std::wstring_view text) {
    const auto bytes = std::filesystem::path(text).u8string();
    return {bytes.begin(), bytes.end()};
}
struct Lease {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~Lease() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};
int Failure(std::string_view code, std::string_view message) {
    std::cout << nlohmann::json{{"ok", false}, {"code", code}, {"message", message}}.dump() << '\n';
    return 1;
}
} // namespace

int RunNameQueryCommand(int argc, wchar_t** argv) {
    using namespace War3Map;
    using namespace War3Map::Services;
    if (argc != 8) return Failure("NAME_QUERY_ARGUMENTS", "name-query <map> <expected-sha256> <ui-root-or-dash> <game-root-or-dash> <raw-value> <ui-type>");
    const auto raw = Utf8(argv[6]), type = Utf8(argv[7]), expectedSha = Utf8(argv[3]);
    if (raw.size() > 65536 || type.size() > 128) return Failure("NAME_QUERY_BUDGET", "查询文字或类型超过预算");
    Lease lease;
    lease.handle = CreateFileW(argv[2], GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (lease.handle == INVALID_HANDLE_VALUE) return Failure("NAME_QUERY_SOURCE_UNAVAILABLE", "不能取得地图只读租约");
    const auto hash = Sha256Handle(lease.handle);
    if (!hash.Succeeded() || *hash.value != expectedSha) return Failure("NAME_QUERY_SOURCE_STALE", "地图与期望 SHA-256 不一致");
    const auto map = MapDocument::OpenReadOnlyArchive(argv[2]);
    if (!map.Succeeded()) return Failure("NAME_QUERY_MAP_INVALID", "基础地图成员不能完整读取");
    const auto archive = Mpq::MpqArchive::OpenReadOnly(argv[2]);
    if (!archive.Succeeded()) return Failure("NAME_QUERY_ARCHIVE_INVALID", "地图归档不能完整读取");
    std::array<std::unique_ptr<CodecDocument>, 7> documents;
    std::array<ProjectSearchObjectSource, 7> objects;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        auto& source = objects[i]; source.type = static_cast<ObjectData::ObjectType>(i);
        source.origin = ProjectSearchObjectOrigin::Native;
        const auto member = ObjectData::NativeFileName(source.type);
        if (!archive.value->HasFile(member)) { source.state = ProjectSearchState::Missing; continue; }
        const auto bytes = archive.value->ReadFileBounded(member, 64 * 1024 * 1024);
        if (!bytes.Succeeded()) { source.state = ProjectSearchState::Invalid; continue; }
        auto parsed = ObjectData::NativeObjectCodec(source.type).Parse(*bytes.value);
        auto* native = dynamic_cast<ObjectData::NativeObjectDocument*>(parsed.document.get());
        source.state = native && parsed.Succeeded() && native->IsStructurallySafe() ? ProjectSearchState::Ready : ProjectSearchState::Invalid;
        if (source.state == ProjectSearchState::Ready) source.records = native->Objects();
        documents[i] = std::move(parsed.document);
    }
    std::optional<Trigger::TriggerDocument> triggers;
    std::optional<Trigger::UiCatalog> nameCatalog;
    std::unique_ptr<Trigger::TriggerTypeSystem> types;
    if (std::wstring_view(argv[4]) != L"-" && archive.value->HasFile("war3map.wtg")) {
        auto ui = Trigger::UiCatalog::Load(argv[4]);
        auto wtg = archive.value->ReadFileBounded("war3map.wtg", 64 * 1024 * 1024);
        auto wct = archive.value->HasFile("war3map.wct") ? archive.value->ReadFileBounded("war3map.wct", 64 * 1024 * 1024)
            : Result<std::vector<std::byte>>::Success({});
        if (ui.Succeeded() && wtg.Succeeded() && wct.Succeeded()) {
            nameCatalog = std::move(*ui.value);
            types = std::make_unique<Trigger::TriggerTypeSystem>(*nameCatalog);
            auto parsed = Trigger::WtgWctCodec{}.Parse(*wtg.value, *wct.value, *nameCatalog);
            if (parsed.Succeeded()) triggers = std::move(*parsed.value);
        }
    }
    std::optional<ObjectData::StandardObjectCatalog> standard;
    if (std::wstring_view(argv[5]) != L"-") {
        auto game = Game::GameDataSource::MountStrict(argv[5]);
        if (game.Succeeded()) {
            auto loaded = ObjectData::StandardObjectCatalog::Load(*game.value);
            if (loaded.Succeeded()) standard = std::move(*loaded.value);
        }
    }
    NameResolutionContext context;
    context.objects = objects; context.mapDocument = &*map.value;
    context.standardObjects = standard ? &*standard : nullptr; context.triggers = triggers ? &*triggers : nullptr;
    context.types = types.get();
    const auto name = ResolveName(context, raw, type);
    nlohmann::json result{{"ok", true}, {"source_sha256", *hash.value}, {"raw_value", name.rawValue},
        {"display_name", name.displayName}, {"name_state", NameResolutionStateName(name.state)},
        {"name_reason", name.reason}, {"name_reference", name.reference}, {"icon_path", name.iconPath},
        {"name_candidates", name.candidates}, {"evidence", "readonly-source-name-query"}};
    const auto output = result.dump();
    if (output.size() > 256 * 1024) return Failure("NAME_QUERY_RESPONSE_BUDGET", "名称查询结果超过 256 KiB");
    std::cout << output << '\n';
    return 0;
}
