#include "McpWorkspace.h"
#include "McpObjectTools.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/Mpq/MpqWriter.h"
#include "War3MapCore/ObjectData/EffectiveObjectCatalog.h"
#include "War3MapCore/Services/ObjectCommands.h"
#include "../services/ObjectCommandsFixture.h"

#include <Windows.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
using namespace War3Map::Mcp;
using namespace War3Map::Services;

void Require(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
void Run(const char* name, void (*test)()) {
    try { test(); }
    catch (const std::exception& error) { throw std::runtime_error(std::string(name) + ": " + error.what()); }
}
std::string Codes(const auto& result) {
    std::string text;
    for (const auto& diagnostic : result.diagnostics) text += diagnostic.code + ": " + diagnostic.message + '\n';
    return text;
}
bool HasCode(const auto& result, std::string_view code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
        [&](const auto& diagnostic) { return diagnostic.code == code; });
}
void Write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::filesystem::create_directories(path.parent_path());
    errno = 0;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    const auto error = errno;
    Require(output.good(), "write private synthetic fixture: " + path.string() + " (errno=" + std::to_string(error)
        + ": " + std::generic_category().message(error) + ')');
}
void Write(const std::filesystem::path& path, std::string_view text) { Write(path, Tests::ObjectBytes(text)); }
DWORD ProbeFileAccess(const std::filesystem::path& path, DWORD access) {
    const auto handle = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return GetLastError();
    CloseHandle(handle);
    return ERROR_SUCCESS;
}

class TempDirectory final {
public:
    TempDirectory() : parent_(std::filesystem::absolute(std::filesystem::temp_directory_path()).lexically_normal()) {
        static std::atomic<unsigned> serial{0};
        path_ = parent_ / ("Audastra-McpEffectiveObjects-" + std::to_string(GetCurrentProcessId()) + '-'
            + std::to_string(GetTickCount64()) + '-' + std::to_string(serial.fetch_add(1)));
        Require(std::filesystem::create_directory(path_), "create exclusive original fixture");
    }
    ~TempDirectory() noexcept {
        const auto target = path_.lexically_normal();
        if (target.parent_path() != parent_ || !target.filename().string().starts_with("Audastra-McpEffectiveObjects-")) return;
        std::error_code error;
        std::filesystem::remove_all(target, error);
    }
    const auto& Path() const noexcept { return path_; }
private:
    std::filesystem::path parent_, path_;
};

std::string UnitTable(std::string_view id, std::string_view model) {
    return Tests::ObjectTable({{{"ID", std::string(id)}, {"file", std::string(model)}, {"Name", "Original fixture unit"}}});
}
std::string Metadata(std::string_view id) {
    return Tests::ObjectTable({{{"ID", std::string(id)}, {"field", "file"}, {"slk", "UnitUI"},
        {"type", "model"}, {"useUnit", "1"}, {"useHero", "1"}, {"useBuilding", "1"},
        {"displayName", "Original model field"}}});
}
struct Fixture {
    TempDirectory temporary;
    std::filesystem::path source = temporary.Path() / "source";
    std::filesystem::path ui = temporary.Path() / "ui";
    std::filesystem::path game = temporary.Path() / "game";
    std::vector<std::byte> originalWtg, originalWct;
    explicit Fixture(bool withMapTable = true, std::string_view model = "MapA.mdx",
        std::string gameUnitTable = UnitTable("u001", "Game.mdx")) {
        Write(ui / "config", "base\n");
        Write(ui / "base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n");
        const auto catalog = Trigger::UiCatalog::Load(ui);
        if (!catalog.Succeeded()) throw std::runtime_error(Codes(catalog));
        auto document = Trigger::WtgWctCodec{}.CreateEmpty(*catalog.value);
        if (!document.Succeeded()) throw std::runtime_error(Codes(document));
        auto pair = Trigger::WtgWctCodec{}.Serialize(*document.value, *catalog.value);
        if (!pair.Succeeded()) throw std::runtime_error(Codes(pair));
        originalWtg = pair.value->wtg; originalWct = pair.value->wct;
        Write(source / "map/war3map.wtg", originalWtg);
        Write(source / "map/war3map.wct", originalWct);
        if (withMapTable) Write(source / "map/Units/UnitData.slk", UnitTable("M001", model));
        // 同名地图字段定义是陷阱：有效对象数据不能替换可信游戏schema。
        Write(source / "map/Units/UnitMetaData.slk", Metadata("evil"));
        std::filesystem::create_directories(game);
        const std::vector<Mpq::MpqBuildFile> files{
            {"Units/UnitData.slk", Tests::ObjectBytes(gameUnitTable), false},
            {"Units/UnitMetaData.slk", Tests::ObjectBytes(Metadata("umdl")), false}};
        const auto written = Mpq::WriteMpqArchive(game / "war3.mpq", files);
        Require(written.Succeeded(), "write original game metadata and table\n" + Codes(written));
    }
    std::unique_ptr<McpWorkspace> Open() const {
        auto result = McpWorkspace::Open(source, ui, game);
        if (!result.Succeeded()) throw std::runtime_error(Codes(result));
        return std::move(*result.value);
    }
    void OriginalPairUnchanged() const {
        const auto wtg = IO::ReadFileBytes(source / "map/war3map.wtg", 1024 * 1024);
        const auto wct = IO::ReadFileBytes(source / "map/war3map.wct", 1024 * 1024);
        Require(wtg.Succeeded() && wct.Succeeded() && *wtg.value == originalWtg && *wct.value == originalWct,
            "queries and rejected operations preserve original editable trigger pair");
    }
};

void MapBaseAndGameSchemaHaveIndependentSources() {
    Fixture fixture;
    auto workspace = fixture.Open();
    auto context = workspace->ObjectContext(ObjectType::Unit);
    if (!context.Succeeded()) throw std::runtime_error(Codes(context));
    const auto* object = context.value->standard->Find(ObjectType::Unit, "M001");
    Require(object && object->Field("file") == "MapA.mdx" && !context.value->standard->Find(ObjectType::Unit, "u001"),
        "MCP reads whole map SLK instead of mixing game rows into it");
    Require(!object->nativeOverridesSupported, "map SLK reads do not acquire native object write authority");
    Require(context.value->schema->FindNative(ObjectType::Unit, "umdl")
        && !context.value->schema->FindNative(ObjectType::Unit, "evil"), "map metadata never controls field schema");
    const auto snapshot = workspace->EffectiveObjectSources();
    Require(snapshot && &snapshot->Catalog() == context.value->standard, "queries share the owned effective catalog without copying");
    const auto* source = snapshot->Input("Units/UnitData.slk");
    Require(source && source->present && source->location.layer == Resources::ResourceLayer::MapImport
        && source->sha256.size() == 64 && !source->containerIdentity.empty(), "MCP exposes actual map source identity for base-table rows");
    const auto listed = ListObjects(*context.value, ObjectListScope::All);
    Require(listed.Succeeded() && listed.value->size() == 1 && listed.value->front().key.rawcode == "M001",
        "shared command query uses the same effective membership");
    fixture.OriginalPairUnchanged();
}

void ReadOnlyMapBasesCannotEnterAnObjectTransaction() {
    Fixture fixture;
    auto workspace = fixture.Open();
    const auto context = workspace->ObjectContext(ObjectType::Unit);
    Require(context.Succeeded(), "capture effective object context for read-only policy\n" + Codes(context));
    const auto fields = ReadObjectFields(*context.value, "M001");
    Require(fields.Succeeded() && !fields.value->empty()
        && std::none_of(fields.value->begin(), fields.value->end(), [](const auto& field) { return field.editable; }),
        "SLK-dependent inherited fields explicitly report read only");
    const std::array<ObjectFieldChange, 1> changes{{{"umdl", {}, {}, std::string("Changed.mdx")}}};
    const auto changed = PlanObjectFieldChanges(*context.value, "M001", changes);
    Require(!changed.Succeeded() && HasCode(changed, "OBJECT_CATALOG_READ_ONLY"), "set-fields rejects unverified SLK writeback");
    const auto created = PlanObjectCreation(*context.value, "M001", std::string("M002"), {});
    Require(!created.Succeeded() && HasCode(created, "OBJECT_CATALOG_READ_ONLY"), "create and clone cannot treat a map SLK row as a writable game base");
    const auto parsed = LniObjectCodec(ObjectType::Unit).Parse(Tests::ObjectBytes("[M002]\n_parent=\"M001\"\n"));
    const auto* after = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
    Require(after, "prepare a structurally valid but unauthorized object plan");
    ObjectCommandPlan plan;
    plan.subject = {ObjectType::Unit, "M002"};
    plan.before = *context.value->document;
    plan.after = *after;
    const auto committed = workspace->ApplyObjectPlan(plan);
    Require(!committed.Succeeded() && HasCode(committed, "OBJECT_CATALOG_READ_ONLY") && !workspace->HasActiveTransaction(),
        "host publication rechecks current base authority before creating a writer transaction");
    fixture.OriginalPairUnchanged();

    Fixture gameOnly(false);
    auto regular = gameOnly.Open();
    const auto regularContext = regular->ObjectContext(ObjectType::Unit);
    Require(regularContext.Succeeded() && regularContext.value->standard->Find(ObjectType::Unit, "u001")->nativeOverridesSupported,
        "unaffected game sources retain existing native override support");
    Require(PlanObjectCreation(*regularContext.value, "u001", std::string("U010"), {}).Succeeded(),
        "read-only map policy does not disable ordinary game-based object plans");
}

void NativeLniInheritanceUsesEffectiveBase() {
    Fixture fixture;
    Write(fixture.source / "table/unit.ini", "[U002]\n_parent=\"M001\"\n");
    auto workspace = fixture.Open();
    auto context = workspace->ObjectContext(ObjectType::Unit);
    if (!context.Succeeded()) throw std::runtime_error(Codes(context));
    const auto fields = ReadObjectFields(*context.value, "U002");
    if (!fields.Succeeded()) throw std::runtime_error(Codes(fields));
    const auto model = std::find_if(fields.value->begin(), fields.value->end(), [](const auto& field) {
        return field.definition && field.definition->fieldId == "umdl";
    });
    Require(model != fields.value->end() && model->value && ValueToDisplayString(*model->value) == "MapA.mdx",
        "LNI child inherits the actual map SLK parent model");
    fixture.OriginalPairUnchanged();
}

void OldPlansCannotSwitchParentsOrHideOtherReadOnlyObjects() {
    const auto data = Tests::ObjectTable({{{"ID", "u001"}, {"file", "Game.mdx"}},
        {{"ID", "m001"}, {"file", "Other.mdx"}}});
    // 原创双行表在首次创建夹具时写入；保留 MPQ writer 拒绝覆盖既存目标的权限边界。
    Fixture fixture(false, "MapA.mdx", data);
    Write(fixture.source / "map/Units/HumanUnitStrings.txt", "[m001]\nName=Map profile\n");
    Write(fixture.source / "table/unit.ini", "[C001]\n_parent=\"u001\"\n");
    auto workspace = fixture.Open();
    const auto context = workspace->ObjectContext(ObjectType::Unit);
    Require(context.Succeeded(), "capture mixed game and read-only profile catalog\n" + Codes(context));
    const auto* writable = context.Succeeded() ? context.value->standard->Find(ObjectType::Unit, "u001") : nullptr;
    const auto* readOnly = context.Succeeded() ? context.value->standard->Find(ObjectType::Unit, "m001") : nullptr;
    Require(writable && readOnly && writable->nativeOverridesSupported && !readOnly->nativeOverridesSupported,
        "mixed catalog keeps game bases writable and profile-dependent bases read only");
    for (const std::string_view candidate : {"[C001]\n_parent=\"m001\"\n",
        "[C001]\n_parent=\"u001\"\n[M002]\n_parent=\"m001\"\n"}) {
        const auto parsed = LniObjectCodec(ObjectType::Unit).Parse(Tests::ObjectBytes(candidate));
        const auto* after = parsed.Succeeded() ? dynamic_cast<const LniObjectDocument*>(parsed.document.get()) : nullptr;
        Require(after, "prepare old plan candidate with a hidden read-only dependency");
        ObjectCommandPlan plan;
        plan.subject = {ObjectType::Unit, "C001"};
        plan.before = *context.value->document;
        plan.after = *after;
        const auto applied = workspace->ApplyObjectPlan(plan);
        Require(!applied.Succeeded() && HasCode(applied, "OBJECT_CATALOG_READ_ONLY") && !workspace->HasActiveTransaction(),
            "publication rejects both changed parent and non-subject read-only objects before acquiring a writer\n" + Codes(applied));
    }
    fixture.OriginalPairUnchanged();
}

void ProtectedMapTablesAndNewShadowsEnforceMcpAuthority() {
    for (const bool withMap : {false, true}) {
        Fixture fixture(withMap);
        const auto table = fixture.source / "map/Units/UnitData.slk";
        // 替换控制文件放在工作区外，拒绝替换时不能靠遗留成员改变来源修订。
        const auto replacement = fixture.temporary.Path() / "UnitData-replacement.slk";
        if (withMap) {
            Require(ProbeFileAccess(table, GENERIC_WRITE) == ERROR_SUCCESS && ProbeFileAccess(table, DELETE) == ERROR_SUCCESS,
                "original fixture grants write and delete access before capturing a source lease");
            Write(replacement, UnitTable("M001", "MapA.mdx"));
            const auto replaced = MoveFileExW(replacement.c_str(), table.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            const auto error = replaced ? ERROR_SUCCESS : GetLastError();
            Require(replaced, "replace unleased synthetic table control (Win32=" + std::to_string(error) + ')');
        }
        auto workspace = fixture.Open();
        auto initial = workspace->ObjectContext(ObjectType::Unit);
        const std::string scenario = withMap ? "protected map table and new root shadow" : "new map shadow";
        Require(initial.Succeeded(), "capture initial classification authority (" + scenario + ")\n" + Codes(initial));
        const auto revision = workspace->Revision();
        if (withMap) {
            const auto readError = ProbeFileAccess(table, GENERIC_READ);
            const auto writeError = ProbeFileAccess(table, GENERIC_WRITE);
            const auto deleteError = ProbeFileAccess(table, DELETE);
            Require(readError == ERROR_SUCCESS && writeError == ERROR_SHARING_VIOLATION,
                "source lease permits reads and refuses write access (read=" + std::to_string(readError)
                + ", write=" + std::to_string(writeError) + ", delete=" + std::to_string(deleteError) + ')');
            Write(replacement, UnitTable("M001", "Changed.mdx"));
            const auto replaced = MoveFileExW(replacement.c_str(), table.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            const auto error = replaced ? ERROR_SUCCESS : GetLastError();
            // 普通 Win32 替换不能覆盖仍打开的目标；不尝试删除原文件或强制 POSIX 替换。
            Require(!replaced && (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION),
                "open source lease rejects ordinary replacement (Win32=" + std::to_string(error) + ')');
            const auto unchanged = IO::ReadFileBytes(table, 1024 * 1024);
            Require(unchanged.Succeeded() && *unchanged.value == Tests::ObjectBytes(UnitTable("M001", "MapA.mdx")),
                "rejected write and replacement preserve original table bytes\n" + Codes(unchanged));
            const auto current = workspace->ObjectContext(ObjectType::Unit);
            Require(current.Succeeded() && !workspace->HasActiveTransaction(),
                "blocked physical changes preserve current catalog authority without acquiring a writer\n" + Codes(current));
            std::cout << "source lease probes read=" << readError << " write=" << writeError << " delete=" << deleteError
                << " replacement=" << error << '\n';
            // 原表受保护，但尚不存在的 root 同名输入仍可新增，必须使旧目录失效。
            Write(fixture.source / "Units/UnitData.slk", UnitTable("M001", "Changed.mdx"));
        } else {
            Write(table, UnitTable("M001", "Changed.mdx"));
        }
        const auto stale = workspace->ObjectContext(ObjectType::Unit);
        Require(!stale.Succeeded(), "cached MCP catalog cannot survive changed or newly shadowing table (" + scenario + ")\n" + Codes(stale));
        const auto edit = workspace->BeginEdit(revision);
        Require(!edit.Succeeded(), "source drift rejects transactions before any overlay mutation (" + scenario + ")\n" + Codes(edit));
        fixture.OriginalPairUnchanged();
    }
}

void BadMapTableNeverBecomesGameData() {
    Fixture fixture;
    Write(fixture.source / "map/Units/UnitData.slk", "present but invalid SLK\n");
    auto workspace = fixture.Open();
    const auto context = workspace->ObjectContext(ObjectType::Unit);
    Require(!context.Succeeded(), "bad selected map table cannot silently fall back to game rows\n" + Codes(context));
    Require(!workspace->EffectiveObjectSources(), "failure does not publish a partial effective snapshot");
    fixture.OriginalPairUnchanged();
}

void IndependentWorkspacesDoNotShareMapRows() {
    Fixture first(true, "MapA.mdx"), second(true, "MapB.mdx");
    auto a = first.Open(), b = second.Open();
    const auto firstContext = a->ObjectContext(ObjectType::Unit), secondContext = b->ObjectContext(ObjectType::Unit);
    Require(firstContext.Succeeded() && secondContext.Succeeded(),
        "capture separate workspaces\nfirst: " + Codes(firstContext) + "second: " + Codes(secondContext));
    Require(firstContext.value->standard->Find(ObjectType::Unit, "M001")->Field("file") == "MapA.mdx"
        && secondContext.value->standard->Find(ObjectType::Unit, "M001")->Field("file") == "MapB.mdx",
        "same rawcode remains bound to its own map");
}

void ProtocolReportsActualCatalogSources() {
    Fixture fixture;
    struct ScopedGameRoot {
        std::optional<std::wstring> old;
        explicit ScopedGameRoot(const std::filesystem::path& value) {
            SetLastError(ERROR_SUCCESS);
            const DWORD size = GetEnvironmentVariableW(L"FORGE_MCP_WAR3_ROOT", nullptr, 0);
            if (size) {
                std::wstring buffer(size, L'\0');
                const auto length = GetEnvironmentVariableW(L"FORGE_MCP_WAR3_ROOT", buffer.data(), size);
                Require(length < size && length > 0, "capture process-local environment");
                buffer.resize(length); old = std::move(buffer);
            } else if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) old = std::wstring{};
            Require(SetEnvironmentVariableW(L"FORGE_MCP_WAR3_ROOT", value.c_str()), "configure only this test process");
        }
        ~ScopedGameRoot() { SetEnvironmentVariableW(L"FORGE_MCP_WAR3_ROOT", old ? old->c_str() : nullptr); }
    } configured(fixture.game);
    McpServer server;
    RegisterObjectTools(server);
    server.SetWorkspace(fixture.source, fixture.ui);
    Require(server.Workspace(), "open protocol workspace with original configured game fixture");
    for (const auto* name : {"objects_list", "objects_get"}) {
        QJsonObject arguments{{"kind", "unit"}};
        if (std::string_view(name) == "objects_get") arguments.insert("id", "M001");
        const QJsonObject request{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
            {"params", QJsonObject{{"name", QString::fromLatin1(name)}, {"arguments", arguments}}}};
        const auto response = server.HandleMessage(QJsonDocument(request).toJson(QJsonDocument::Compact));
        Require(response && !response->error, "protocol query returns a valid response");
        const auto content = response->result.toObject().value("content").toArray();
        Require(content.size() == 1, "query uses bounded normal text payload");
        const auto body = QJsonDocument::fromJson(content.first().toObject().value("text").toString().toUtf8()).object();
        Require(body.value("ok").toBool(), "effective object query succeeds at protocol boundary");
        const auto sources = body.value("catalog_sources").toArray();
        const auto selected = std::find_if(sources.begin(), sources.end(), [](const QJsonValue& value) {
            const auto row = value.toObject();
            return row.value("logical_path").toString().compare("UNITS\\UNITDATA.SLK", Qt::CaseInsensitive) == 0;
        });
        Require(selected != sources.end(), "query includes relevant logical classification table");
        const auto row = (*selected).toObject();
        Require(row.value("present").toBool() && row.value("layer") == "map"
            && row.value("sha256").toString().size() == 64 && !row.value("container_identity").toString().isEmpty(),
            "legacy standard/base enum is accompanied by exact map provenance");
    }
    fixture.OriginalPairUnchanged();
}
} // namespace

int main() {
    try {
        Run("MapBaseAndGameSchemaHaveIndependentSources", MapBaseAndGameSchemaHaveIndependentSources);
        Run("ReadOnlyMapBasesCannotEnterAnObjectTransaction", ReadOnlyMapBasesCannotEnterAnObjectTransaction);
        Run("NativeLniInheritanceUsesEffectiveBase", NativeLniInheritanceUsesEffectiveBase);
        Run("OldPlansCannotSwitchParentsOrHideOtherReadOnlyObjects", OldPlansCannotSwitchParentsOrHideOtherReadOnlyObjects);
        Run("ProtectedMapTablesAndNewShadowsEnforceMcpAuthority", ProtectedMapTablesAndNewShadowsEnforceMcpAuthority);
        Run("BadMapTableNeverBecomesGameData", BadMapTableNeverBecomesGameData);
        Run("IndependentWorkspacesDoNotShareMapRows", IndependentWorkspacesDoNotShareMapRows);
        Run("ProtocolReportsActualCatalogSources", ProtocolReportsActualCatalogSources);
        std::cout << "MCP effective object catalog passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
