#include "War3MapCore/CompatibilityProfile.h"
#include "War3MapCore/EditorSession.h"
#include "War3MapCore/MapArchiveSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Mpq/MpqTableAudit.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Image/BlpImage.h"
#include "War3MapCore/IO/FileBytes.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/Render/MdxModel.h"
#include "War3MapCore/Render/MdxPose.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/Trigger/TriggerTypes.h"
#include "ScriptBuildCommand.h"
#include "UiLibraryCommand.h"
#include "MapDataRoundtripCommand.h"
#include "MapDataQueryCommand.h"
#include "TerrainQueryCommand.h"
#include "TerrainBlightSaveCommand.h"
#include "MapEditSaveCommand.h"
#include "ObjectModelListCommand.h"
#include "ScriptMigrationCommand.h"
#include "ObjectLayerCommand.h"
#include "WctSourceCommand.h"
#include "WtgSourceCommand.h"
#include "LuaTriggerCandidateCommand.h"
#include "MapCreateCommand.h"
#include "MapTestCommand.h"
#include "ArchiveInspectCommand.h"
#include "ArchiveMemberCommand.h"
#include "ImportQueryCommand.h"
#include "UpdateCommand.h"
#include "NameQueryCommand.h"
#include "ResourceCheckCommand.h"
#include "GameExtractCommand.h"
#include "AudastraVersion.generated.h"

#include <algorithm>
#include <cstddef>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

// 命令行参数（UTF-16）转 UTF-8；MPQ 内部路径与字段名都按 UTF-8 处理。
std::string Utf8Argument(std::wstring_view text) {
    const auto converted = std::filesystem::path(text).u8string();
    return {converted.begin(), converted.end()};
}

void PrintDiagnostics(const std::vector<War3Map::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics) {
        std::cerr << diagnostic.subsystem << ": "
                  << diagnostic.code << ": "
                  << diagnostic.message << '\n';
    }
}

std::optional<War3Map::CompatibilityProfile> SelectProfile(const std::wstring& id) {
    // 配置仅描述魔兽版本和语言，第三方函数由 UI 数据根自动发现
    if (id == L"war3-1.27a-zhCN" || id == L"default") {
        return War3Map::CompatibilityProfile::Default();
    }
    return std::nullopt;
}

void PrintUsage() {
    std::cout
        << "war3-map-cli --version | --version-json\n"
        << "war3-map-cli update-check --manifest <json> --signature <sig> [--package <zip>] [--channel stable|beta]\n"
        << "war3-map-cli update-apply --package <zip> --manifest <json> --signature <sig> --install-dir <directory> [--baseline-manifest <json> --baseline-signature <sig>] --dry-run\n"
        << "war3-map-cli profiles\n"
        << "war3-map-cli map-create <profile> <new-map.w3x> <new-work-directory> <ydwe-root> <war3-root> <name> [author] [--init-trigger <name> --init-code <jass-line> ...] [--add-file <member> <file> ...]\n"
        << "war3-map-cli map-test <map.w3x> <war3-root> <helper-dir> [--ydwe <root>] [--lua-engine owned|ydwe] [--no-hot-reload] [--seconds N] [--reload-after <seconds> <module> <file> ...]\n"
        << "war3-map-cli map-data-inspect <map-archive-or-directory>\n"
        << "war3-map-cli resource-check <map-archive> <war3-root>\n"
        << "war3-map-cli name-query <map-archive> <source-sha256> <ui-root-or-dash> <game-root-or-dash> <raw-value> <ui-type>\n"
        << "war3-map-cli map-data-query <map-archive> <source-sha256> <settings|units|doodads|regions|cameras|sounds|strings> <start-index> <limit-1..128>\n"
        << "war3-map-cli imports-query <archive-or-directory> --expected-source-sha256 <sha256> [--query <text>] [--limit <1..128>] [--expected-inventory-sha256 <sha256> --cursor <token>]\n"
        << "war3-map-cli imports-get <archive-or-directory> --expected-source-sha256 <sha256> --expected-inventory-sha256 <sha256> --import-id <id>\n"
        << "war3-map-cli map-data-roundtrip <map-archive>\n"
        << "war3-map-cli script-migration-check <map-archive> <source-sha256> <classic-jass|lua> [ydwe-root]\n"
        << "war3-map-cli wtg-source-inspect <wtg-file> <source-sha256> <ui-data-root> [--max-nodes N]\n"
        << "war3-map-cli wtg-source-query <wtg-file> <source-sha256> <ui-data-root> <catalog-sha256> <items|variables|deleted|ecas|arguments> <start> <limit-1..128> [--max-nodes N] [--trigger <items-row>]\n"
        << "  --trigger only returns that trigger's ecas or arguments, in tree order; row numbers stay table-wide.\n"
        << "war3-map-cli lua-trigger-candidate <wtg-file> <wct-file> <ui-root> <target-tsv> <inspect|input-revision> [--sources <script-sha> <line-or-0> <offset> <limit>]\n"
        << "war3-map-cli terrain-snapshot <map-archive>\n"
        << "war3-map-cli terrain-block <map-archive> <source-sha256> <terrain-sha256> <x> <y> <width> <height>\n"
        << "war3-map-cli terrain-blight-save <source-map> <source-sha256> <terrain-sha256> <ydwe-root> <war3-root> <vertex-x> <vertex-y> <radius-0..16> <set|clear> <new-candidate.w3x> [--layer <id> ...]\n"
        << "war3-map-cli map-edit-save <source-map> <source-sha256> <terrain-sha256> <ydwe-root> <war3-root> <new-candidate.w3x> --stroke <raise|lower|smooth> <circle|square> <vertex-x> <vertex-y> <radius-0..16> <strength> [--stroke ...] [--generate-missing-minimap | --generate-missing-playable-minimap] [--layer <id> ...]\n"
        << "war3-map-cli map-edit-save <source-map> <source-sha256> - <ydwe-root> <war3-root> <new-candidate.w3x> --field <kind> <row> <row-key> <member-sha256> <field-id> <expected-raw> <new-value> [--field ...] [--layer <id> ...]\n"
        << "war3-map-cli map-edit-save <source-map> <source-sha256> - <ydwe-root> <war3-root> <new-candidate.w3x> --copy-source <units|doodads> <row> <row-key> <member-sha256> [--copy-source ...] --copy-offset <dx> <dy> <dz> [--layer <id> ...]\n"
        << "war3-map-cli map-edit-save <source-map> <source-sha256> - <ydwe-root> <war3-root> <new-candidate.w3x> --rotate <units|doodads> <row> <row-key> <member-sha256> <degrees> [--layer <id> ...]\n"
        << "war3-map-cli map-field-catalog\n"
        << "war3-map-cli map-trigger-text <source-map> <source-sha256> <ui-root> <global|trigger> <trigger-id-or-0> <utf8-offset> <limit-4..65536> [--catalog-sha256 <sha>] [--layer <id> ...]\n"
        << "  map-edit-save --ui-root <ui-data-root> binds a separate UI catalog; compiler root and its safety checks stay unchanged.\n"
        << "  map-edit-save --lua-map-target <configuration> uses a SHA-bound Lua profile and checker for explicit v31 Lua saving.\n"
        << "  --source-encoding <absolute-file> utf8|gbk; --include-access <absolute-path> directory|file utf8|gbk|auto required|optional. Include access does not add search or injection roots.\n"
        << "  1..16 ordered strokes; vertex coordinates; doodads stay fixed. Other brush kinds require a future explicit CLI schema.\n"
        << "war3-map-cli archive-session-inspect <map-archive> [--expected-source-sha256 <64hex>] [--member <internal-path> ...]\n"
        << "war3-map-cli archive-candidate-copy <map-archive> <new-candidate-path>\n"
        << "war3-map-cli archive-member-add <source-map> <expected-source-sha256> <new-candidate.w3x> <member> <file> [<member> <file> ...]\n"
        << "war3-map-cli archive-member-remove <source-map> <expected-source-sha256> <new-candidate.w3x> <member> [<member> ...]\n"
        << "war3-map-cli inspect <expanded-workspace> [profile-id]\n"
        << "war3-map-cli validate <expanded-workspace> [profile-id]\n"
        << "war3-map-cli trigger-inspect <ydwe-root> <war3map.wtg> <war3map.wct>\n"
        << "war3-map-cli wct-source-inspect <wct-file> <member-sha256> <start-index> <limit-1..128>\n"
        << "war3-map-cli wct-source-chunk <wct-file> <member-sha256> <global|block-index> <byte-offset> <limit-1..65536>\n"
        << "war3-map-cli trigger-roundtrip <ydwe-root> <war3map.wtg> <war3map.wct>\n"
        << "war3-map-cli ui-library-inspect <ydwe-root> [<war3map.wtg> <war3map.wct>]\n"
        << "war3-map-cli script-build <ydwe-root> <war3-root> <map> <out.j> [--compile <work-dir> [--external|--cross-check]]\n"
        << "war3-map-cli script-template <ydwe-root> <war3-root> <map> <wave-output.j> <out.j>\n"
        << "war3-map-cli object-field <war3-root> <unit|item|ability> <rawcode>\n"
        << "war3-map-cli object-model-list <war3-root> <new-models.txt>\n"
        << "war3-map-cli skin-layer-inspect <map-archive>\n"
        << "war3-map-cli skin-layer-query <map-archive> <source-sha256> <unit|ability|item|buff|upgrade|doodad|destructable> <start-index> <limit-1..128>\n"
        << "war3-map-cli object-inspect <type> <table.ini>\n"
        << "war3-map-cli object-roundtrip <type> <table.ini>\n"
        << "war3-map-cli game-extract <war3-root> <internal\\path> <out-file> [--tileset <A-Z>]\n"
        << "  --tileset: strict game-only JSON source and member SHA256, max 64 MiB; excludes map/session overrides\n"
        << "war3-map-cli mpq-list <archive> [name-filter]\n"
        << "war3-map-cli mpq-extract <archive> <internal\\path> <out-file>\n"
        << "war3-map-cli blp-decode <in.blp> <out.ppm>\n"
        << "war3-map-cli mdx-inspect <in.mdx>\n"
        << "war3-map-cli mdx-pose-sample <in.mdx> <sequence-index> <elapsed-ms> <object-id> [global-elapsed-ms]\n";
}

std::optional<std::vector<std::byte>> ReadBytes(
    const std::filesystem::path& path) {
    auto result = War3Map::IO::ReadFileBytes(path, std::numeric_limits<std::uint64_t>::max());
    return result.Succeeded() ? std::move(result.value) : std::nullopt;
}

std::optional<std::uint64_t> ParseUnsignedArgument(std::wstring_view argument) {
    const auto text = Utf8Argument(argument);
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return value;
}

std::optional<War3Map::ObjectData::ObjectType> ParseObjectType(
    const std::wstring& name) {
    using War3Map::ObjectData::ObjectType;
    if (name == L"unit") return ObjectType::Unit;
    if (name == L"ability") return ObjectType::Ability;
    if (name == L"item") return ObjectType::Item;
    if (name == L"buff") return ObjectType::Buff;
    if (name == L"upgrade") return ObjectType::Upgrade;
    if (name == L"doodad") return ObjectType::Doodad;
    if (name == L"destructable") return ObjectType::Destructable;
    return std::nullopt;
}

int RunTriggerCommand(
    bool roundTrip,
    bool typeAudit,
    const std::filesystem::path& ydweRoot,
    const std::filesystem::path& wtgPath,
    const std::filesystem::path& wctPath) {
    const auto wtg = ReadBytes(wtgPath);
    const auto wct = ReadBytes(wctPath);
    if (!wtg || !wct) {
        std::cerr << "Trigger: FILE_READ_FAILED: unable to read WTG/WCT input\n";
        return 1;
    }
    auto catalog = War3Map::Trigger::UiCatalog::Load(ydweRoot);
    if (!catalog.Succeeded()) {
        PrintDiagnostics(catalog.diagnostics);
        return 1;
    }
    War3Map::Trigger::WtgWctCodec codec;
    auto parsed = codec.Parse(*wtg, *wct, *catalog.value);
    if (!parsed.Succeeded()) {
        PrintDiagnostics(parsed.diagnostics);
        return 1;
    }
    const auto validation = codec.Validate(*parsed.value, *catalog.value);
    if (!validation.Succeeded()) {
        PrintDiagnostics(validation.diagnostics);
        return 1;
    }
    const auto inspection = codec.Inspect(*parsed.value);
    std::cout << "uiCatalogSha256=" << catalog.value->Sha256() << '\n'
              << "uiSourcesPresent=" << catalog.value->PresentSourceCount() << '\n'
              << "uiSourcesParsed=" << catalog.value->ParsedSourceCount() << '\n'
              << "triggers=" << inspection.triggerCount << '\n'
              << "ecas=" << inspection.ecaCount << '\n'
              << "arguments=" << inspection.argumentCount << '\n'
              << "customTextSlots=" << inspection.customTextSlotCount << '\n';
    if (typeAudit) {
        // 参数类型审计：Null 解析、值类型与期望类型是否相容（衡量 YDWE 函数的适配程度）。
        const auto audit = War3Map::Trigger::AuditArgumentTypes(*parsed.value, *catalog.value);
        std::cout << "typedArguments=" << audit.arguments << '\n'
                  << "nullArguments=" << audit.nullArguments << '\n'
                  << "resolvedNull=" << audit.resolvedNull << '\n'
                  << "anyReturnValues=" << audit.anyReturnValues << '\n'
                  << "unresolved=" << audit.unresolved << '\n'
                  << "mismatched=" << audit.mismatched << '\n'
                  << "localTypeHints=" << audit.localTypeHints << '\n'
                  << "unknownTypes=" << audit.unknownTypes << '\n'
                  << "missingDefinitions=" << audit.missingDefinitions << '\n';
        for (const auto& [key, count] : audit.problemsByFunction) {
            std::cout << "problem\t" << count << '\t' << key << '\n';
        }
        for (const auto& [key, count] : audit.blockBranches) {
            std::cout << "block\t" << count << '\t' << key << '\n';
        }
        for (const auto& issue : audit.samples) {
            std::cout << "sample\t" << issue.problem << '\t' << issue.function << " #" << issue.position
                      << "\tdeclared=" << issue.declared << "\texpected=" << issue.expected
                      << "\tactual=" << issue.actual << "\ttrigger=" << issue.trigger << '\n';
        }
        return 0;
    }
    if (!roundTrip) {
        return 0;
    }
    auto encoded = codec.Serialize(*parsed.value, *catalog.value);
    if (!encoded.Succeeded()) {
        PrintDiagnostics(encoded.diagnostics);
        return 1;
    }
    const bool wtgEqual = encoded.value->wtg == *wtg;
    const bool wctEqual = encoded.value->wct == *wct;
    std::cout << "wtgByteIdentical=" << (wtgEqual ? "true" : "false") << '\n'
              << "wctByteIdentical=" << (wctEqual ? "true" : "false") << '\n';
    return wtgEqual && wctEqual ? 0 : 1;
}

int RunObjectCommand(
    bool roundTrip,
    War3Map::ObjectData::ObjectType type,
    const std::filesystem::path& inputPath) {
    const auto input = ReadBytes(inputPath);
    if (!input) {
        std::cerr << "ObjectData: FILE_READ_FAILED: unable to read LNI input\n";
        return 1;
    }
    War3Map::ObjectData::LniObjectCodec codec(type);
    auto parsed = codec.Parse(*input);
    if (!parsed.Succeeded()) {
        PrintDiagnostics(parsed.diagnostics);
        return 1;
    }
    const auto validation = codec.Validate(*parsed.document);
    if (!validation.Succeeded()) {
        PrintDiagnostics(validation.diagnostics);
        return 1;
    }
    const auto inspection = codec.Inspect(*parsed.document);
    std::cout << "format=" << inspection.formatName << '\n';
    for (const auto& [key, value] : inspection.properties) {
        std::cout << key << '=' << value << '\n';
    }
    PrintDiagnostics(inspection.diagnostics);
    if (!roundTrip) {
        return 0;
    }
    auto encoded = codec.Serialize(*parsed.document);
    if (!encoded.Succeeded()) {
        PrintDiagnostics(encoded.diagnostics);
        return 1;
    }
    const bool equal = *encoded.value == *input;
    std::cout << "byteIdentical=" << (equal ? "true" : "false") << '\n';
    return equal ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    const std::wstring command = argv[1];
    if (argc == 2 && command == L"--version") {
        std::cout << "Audastra " << War3Map::BuildInfo::Version << " (" << War3Map::BuildInfo::Commit << ")\n";
        return 0;
    }
    if (argc == 2 && command == L"--version-json") {
        std::cout << "{\"product\":\"Audastra\",\"version\":\"" << War3Map::BuildInfo::Version
            << "\",\"commit\":\"" << War3Map::BuildInfo::Commit
            << "\",\"release_owner\":\"" << War3Map::BuildInfo::ReleaseOwner
            << "\",\"release_repository\":\"" << War3Map::BuildInfo::ReleaseRepository
            << "\",\"update_public_key_hex\":\"" << War3Map::BuildInfo::UpdatePublicKeyHex << "\"}\n";
        return 0;
    }
    if (command == L"archive-session-inspect") return RunArchiveInspectCommand(argc, argv);
    if (command == L"archive-member-add") return War3Map::Cli::RunArchiveMemberAdd(argc, argv);
    if (command == L"archive-member-remove") return War3Map::Cli::RunArchiveMemberRemove(argc, argv);
    if (command == L"update-check" || command == L"update-apply") return RunUpdateCommand(argc, argv);
    if (command == L"imports-query" || command == L"imports-get") return RunImportQueryCommand(argc, argv);
    if (command == L"archive-candidate-copy" && argc == 4) {
        auto session = War3Map::MapArchiveSession::Open(argv[2]);
        if (!session.Succeeded() || !session.value) {
            PrintDiagnostics(session.diagnostics);
            return 1;
        }
        std::cout << "sourceSha256=" << session.value->SourceSha256()
            << "\nnamedMemberCount=" << session.value->Members().size()
            << "\nopaqueMemberCount=" << session.value->OpaqueMemberCount() << '\n';
        for (const auto& metadata : session.value->MetadataMembers()) {
            std::cout << "metadataMember=" << metadata
                << (session.value->HasOpaqueMembers() ? " (kept as source bytes)\n" : " (dropped on rebuild)\n");
        }
        if (command == L"archive-candidate-copy") {
            const auto candidate = session.value->SaveCandidate(argv[3], {});
            if (!candidate.Succeeded() || !candidate.value) {
                PrintDiagnostics(candidate.diagnostics);
                return 1;
            }
            std::cout << "candidateSha256=" << candidate.value->sha256 << '\n';
        }
        std::cout << "mapInitializationGenerated=false\noriginalEditorValidated=false\nruntimeValidated=false\n";
        return 0;
    }
    if (command == L"map-data-inspect" && argc == 3) {
        const std::filesystem::path path = argv[2];
        std::optional<War3Map::MapDocument> document;
        std::error_code ec;
        if (std::filesystem::is_directory(path, ec)) {
            auto workspace = War3Map::DirectoryWorkspace::Open(path);
            if (!workspace.Succeeded()) { PrintDiagnostics(workspace.diagnostics); return 1; }
            auto read = War3Map::MapDocument::FromDirectory(*workspace.value);
            if (!read.Succeeded()) { PrintDiagnostics(read.diagnostics); return 1; }
            document = std::move(*read.value);
        } else {
            auto read = War3Map::MapDocument::OpenReadOnlyArchive(path);
            if (!read.Succeeded()) { PrintDiagnostics(read.diagnostics); return 1; }
            document = std::move(*read.value);
        }
        constexpr std::array stateNames{"missing", "read", "partial", "invalid", "ambiguous", "unreadable"};
        for (const auto& member : document->Members()) {
            std::cout << member.memberName << " state=" << stateNames[static_cast<int>(member.state)] << " count=";
            if (member.recordCount) std::cout << *member.recordCount; else std::cout << "unknown";
            std::cout << '\n';
        }
        PrintDiagnostics(document->Diagnostics());
        const auto status = document->InitializationSourceStatus();
        PrintDiagnostics(status.diagnostics);
        std::cout << "initializationSourceData=" << (status.Succeeded() ? "available" : "incomplete")
            << "\nmapInitializationGenerated=false\noriginalEditorValidated=false\nruntimeValidated=false\n";
        return status.Succeeded() ? 0 : 2;
    }
    if (command == L"map-data-roundtrip" && argc == 3) {
        return RunMapDataRoundtrip(argv[2]);
    }
    if (command == L"map-create") return RunMapCreate(argc, argv);
    if (command == L"map-test") return RunMapTest(argc, argv);
    if (command == L"wtg-source-inspect" && (argc == 5 ||
            (argc == 7 && std::wstring_view(argv[5]) == L"--max-nodes" && argv[6][0])))
        return RunWtgSourceInspect(argv[2], Utf8Argument(argv[3]), argv[4], argc == 7 ? Utf8Argument(argv[6]) : "");
    if (command == L"wtg-source-query" && argc >= 9 && argc <= 13 && (argc - 9) % 2 == 0) {
        std::string maxNodes, triggerItem;
        bool valid = true;
        for (int index = 9; index + 1 < argc && valid; index += 2) {
            const std::wstring_view option(argv[index]);
            auto& target = option == L"--max-nodes" ? maxNodes : triggerItem;
            valid = (option == L"--max-nodes" || option == L"--trigger") && argv[index + 1][0] && target.empty();
            if (valid) target = Utf8Argument(argv[index + 1]);
        }
        if (valid)
            return RunWtgSourceQuery(argv[2], Utf8Argument(argv[3]), argv[4], Utf8Argument(argv[5]),
                Utf8Argument(argv[6]), Utf8Argument(argv[7]), Utf8Argument(argv[8]), maxNodes, triggerItem);
    }
    if (command == L"lua-trigger-candidate" && argc >= 7) {
        std::vector<std::string> options;
        for (int index = 7; index < argc; ++index) options.push_back(Utf8Argument(argv[index]));
        return RunLuaTriggerCandidate(argv[2], argv[3], argv[4], argv[5], Utf8Argument(argv[6]), options);
    }
    if (command == L"script-migration-check" && (argc == 5 || argc == 6)) {
        return RunScriptMigrationCheck(argv[2], Utf8Argument(argv[3]), Utf8Argument(argv[4]),
            argc == 6 ? std::optional<std::filesystem::path>(argv[5]) : std::nullopt);
    }
    if (command == L"wct-source-inspect" && argc == 6) {
        return RunWctSourceInspect(argv[2], Utf8Argument(argv[3]), Utf8Argument(argv[4]), Utf8Argument(argv[5]));
    }
    if (command == L"wct-source-chunk" && argc == 7) {
        return RunWctSourceChunk(argv[2], Utf8Argument(argv[3]), Utf8Argument(argv[4]), Utf8Argument(argv[5]), Utf8Argument(argv[6]));
    }
    if (command == L"map-data-query" && argc == 7) {
        return RunMapDataQuery(argv[2], Utf8Argument(argv[3]), Utf8Argument(argv[4]),
            Utf8Argument(argv[5]), Utf8Argument(argv[6]));
    }
    if (command == L"name-query") return RunNameQueryCommand(argc, argv);
    if (command == L"terrain-snapshot" && argc == 3) {
        return RunTerrainSnapshot(argv[2]);
    }
    if (command == L"terrain-block" && argc == 9) {
        return RunTerrainBlock(argv[2], Utf8Argument(argv[3]), Utf8Argument(argv[4]),
            Utf8Argument(argv[5]), Utf8Argument(argv[6]), Utf8Argument(argv[7]), Utf8Argument(argv[8]));
    }
    if (command == L"terrain-blight-save" && argc >= 12 && (argc - 12) % 2 == 0) {
        std::optional<std::vector<std::string>> layers;
        if (argc > 12) {
            layers.emplace();
            for (int index = 12; index < argc; index += 2) {
                if (std::wstring_view(argv[index]) != L"--layer" ||
                    layers->size() >= 128 || argv[index + 1][0] == L'\0') {
                    std::cerr << "TerrainBlight.Save: SAVE_LAYER_ARGUMENT_INVALID: expected --layer <id> pairs\n";
                    return 2;
                }
                layers->push_back(Utf8Argument(argv[index + 1]));
            }
        }
        return RunTerrainBlightSave(argv[2], Utf8Argument(argv[3]), Utf8Argument(argv[4]),
            argv[5], argv[6], Utf8Argument(argv[7]), Utf8Argument(argv[8]),
            Utf8Argument(argv[9]), Utf8Argument(argv[10]), argv[11], std::move(layers));
    }
    if (command == L"map-field-catalog" && argc == 2) return RunMapFieldCatalog();
    if (command == L"map-trigger-text") {
        return RunMapTriggerTextQuery(argc, argv);
    }
    if (command == L"map-edit-save") {
        if (argc == 3 && std::wstring_view(argv[2]) == L"--help") { PrintUsage(); return 0; }
        return RunMapEditSave(argc, argv);
    }
    if (command == L"profiles") {
        const auto profile = War3Map::CompatibilityProfile::Default();
        std::cout << profile.id << '\t'
                  << profile.displayName << '\t'
                  << profile.definitionSha256 << '\n';
        return 0;
    }

    if (command == L"object-field" && argc == 5) {
        return RunObjectField(argv[2], Utf8Argument(argv[3]), Utf8Argument(argv[4]));
    }
    if (command == L"resource-check" && argc == 4) return RunResourceCheck(argv[2], argv[3]);
    if (command == L"object-model-list" && argc == 4) {
        return RunObjectModelList(argv[2], argv[3]);
    }
    // object-layer-* 是旧名字：实际只读 war3mapSkin.* 皮肤层，保留给已有脚本。
    if ((command == L"skin-layer-inspect" || command == L"object-layer-inspect") && argc == 3) return RunObjectLayerInspect(argv[2]);
    if ((command == L"skin-layer-query" || command == L"object-layer-query") && argc == 7) {
        return RunObjectLayerQuery(argv[2], Utf8Argument(argv[3]), Utf8Argument(argv[4]),
            Utf8Argument(argv[5]), Utf8Argument(argv[6]));
    }
    if (command == L"script-build" && argc == 6) {
        return RunScriptBuild(argv[2], argv[3], argv[4], argv[5]);
    }
    if (command == L"script-build" && (argc == 8 || argc == 9) && std::wstring_view(argv[6]) == L"--compile") {
        const std::wstring_view engines = argc == 9 ? std::wstring_view(argv[8]) : std::wstring_view{};
        if (!engines.empty() && engines != L"--external" && engines != L"--cross-check") {
            std::cerr << "script-build: 未知选项（可用 --external、--cross-check）\n";
            return 1;
        }
        return RunScriptBuild(argv[2], argv[3], argv[4], argv[5], std::filesystem::path(argv[7]), engines);
    }
    if (command == L"script-template" && argc == 7) {
        return RunScriptTemplate(argv[2], argv[3], argv[4], argv[5], argv[6]);
    }
    if (command == L"ui-library-inspect" && (argc == 3 || argc == 5)) {
        const std::filesystem::path wtg = argc == 5 ? argv[3] : L"";
        const std::filesystem::path wct = argc == 5 ? argv[4] : L"";
        return RunUiLibraryInspect(argv[2], argc == 5 ? &wtg : nullptr, argc == 5 ? &wct : nullptr);
    }
    if ((command == L"trigger-inspect" || command == L"trigger-roundtrip" || command == L"trigger-type-audit") &&
        argc == 5) {
        return RunTriggerCommand(
            command == L"trigger-roundtrip",
            command == L"trigger-type-audit",
            argv[2],
            argv[3],
            argv[4]);
    }

    if ((command == L"object-inspect" || command == L"object-roundtrip") &&
        argc == 4) {
        const auto type = ParseObjectType(argv[2]);
        if (!type) {
            std::cerr << "ObjectData: TYPE_UNKNOWN: expected unit, ability, item, buff, upgrade, doodad or destructable\n";
            return 2;
        }
        return RunObjectCommand(
            command == L"object-roundtrip",
            *type,
            argv[3]);
    }

    if (command == L"game-extract") return RunGameExtract(argc, argv);

    if (command == L"mpq-list" && (argc == 3 || argc == 4)) {
        auto archive = War3Map::Mpq::MpqArchive::OpenReadOnly(argv[2]);
        if (!archive.Succeeded() || !archive.value.has_value()) {
            PrintDiagnostics(archive.diagnostics);
            return 1;
        }
        const std::string filter = argc == 4 ? Utf8Argument(argv[3]) : std::string{};
        const auto names = archive.value->ListFiles();
        std::size_t shown = 0;
        for (const auto& name : names) {
            if (!filter.empty() && name.find(filter) == std::string::npos) {
                continue;
            }
            std::cout << name << '\n';
            ++shown;
        }
        std::cout << "total=" << names.size() << " shown=" << shown << '\n';
        return 0;
    }

    if (command == L"mpq-hash-inspect" && argc == 3) {
        // 只读审计哈希表 / 块表：列出不满足严格清单规则的条目（诊断会话层拒绝的原因）。
        auto archive = War3Map::Mpq::MpqArchive::OpenReadOnly(argv[2]);
        if (!archive.Succeeded() || !archive.value.has_value()) {
            PrintDiagnostics(archive.diagnostics);
            return 1;
        }
        const auto audit = War3Map::Mpq::AuditTables(*archive.value);
        std::cout << "hash entries=" << audit.hashEntries << " active=" << audit.hashActive
                  << " deleted=" << audit.hashDeleted << " empty=" << audit.hashEmpty << '\n';
        for (const auto& [locale, count] : audit.activeByLocale) std::cout << "  locale " << locale << ": " << count << '\n';
        for (const auto& [platform, count] : audit.activeByPlatform) std::cout << "  platform " << int{platform} << ": " << count << '\n';
        for (const auto& [reserved, count] : audit.activeByReserved) std::cout << "  reserved " << int{reserved} << ": " << count << '\n';
        std::cout << "block entries=" << audit.blockEntries << " existing=" << audit.blocksExisting
                  << " unreferenced=" << audit.blocksUnreferenced << " aliased=" << audit.blocksAliased
                  << " anonymous=" << audit.anonymousBlocks << '\n';
        std::cout << "listfile names=" << audit.listedNames << " resolved=" << audit.listedResolved << '\n';
        for (const auto& special : audit.specialMembers) std::cout << "  special: " << special << '\n';
        for (std::size_t i = 0; i < audit.listedUnresolved.size() && i < 20; ++i) {
            std::cout << "  unresolved: " << audit.listedUnresolved[i] << '\n';
        }
        for (const auto& finding : audit.Findings()) std::cout << "finding: " << finding << '\n';
        return 0;
    }

    if (command == L"mpq-extract" && argc == 5) {
        auto archive = War3Map::Mpq::MpqArchive::OpenReadOnly(argv[2]);
        if (!archive.Succeeded() || !archive.value.has_value()) {
            PrintDiagnostics(archive.diagnostics);
            return 1;
        }
        const std::string name = Utf8Argument(argv[3]);
        auto data = archive.value->ReadFile(name);
        if (!data.Succeeded() || !data.value.has_value()) {
            PrintDiagnostics(data.diagnostics);
            return 1;
        }
        std::ofstream out(argv[4], std::ios::binary);
        if (!out) {
            std::cerr << "Mpq: OUTPUT_OPEN_FAILED\n";
            return 1;
        }
        out.write(reinterpret_cast<const char*>(data.value->data()),
                  static_cast<std::streamsize>(data.value->size()));
        std::cout << "extracted=" << name << " bytes=" << data.value->size() << '\n';
        return 0;
    }

    if (command == L"mdx-pose-sample" && (argc == 6 || argc == 7)) {
        const auto sequence = ParseUnsignedArgument(argv[3]);
        const auto elapsed = ParseUnsignedArgument(argv[4]);
        const auto objectId = ParseUnsignedArgument(argv[5]);
        const auto globalElapsed = argc == 7 ? ParseUnsignedArgument(argv[6]) : std::optional<std::uint64_t>{0};
        if (!sequence || *sequence > UINT32_MAX || !elapsed || !objectId || *objectId > UINT32_MAX || !globalElapsed) {
            std::cerr << "Mdx: MDX_POSE_ARGUMENT_INVALID: expected unsigned sequence/elapsed/object/global elapsed\n";
            return 2;
        }
        const auto bytes = ReadBytes(argv[2]);
        if (!bytes) {
            std::cerr << "Mdx: FILE_READ_FAILED\n";
            return 1;
        }
        auto parsed = War3Map::Render::ParseMdx(*bytes);
        if (!parsed.Succeeded() || !parsed.value) {
            PrintDiagnostics(parsed.diagnostics);
            return 1;
        }
        War3Map::Render::MdxPoseRequest request;
        request.sequenceIndex = static_cast<std::size_t>(*sequence);
        request.sequenceElapsedMs = *elapsed;
        request.globalElapsedMs = *globalElapsed;
        request.objectId = static_cast<std::uint32_t>(*objectId);
        const auto pose = War3Map::Render::SampleMdxNodePose(*parsed.value, request);
        if (!pose.Succeeded() || !pose.value) {
            PrintDiagnostics(pose.diagnostics);
            return 1;
        }
        if (pose.value->chain.size() > 128) {
            std::cerr << "Mdx: MDX_POSE_OUTPUT_BUDGET: node chain exceeds 128 entries\n";
            return 1;
        }
        if (!pose.diagnostics.empty()) PrintDiagnostics(pose.diagnostics);
        std::cout << "preview_label=通用算法预览\n";
        std::cout << "sequence_index=" << request.sequenceIndex
                  << "\nsequence_frame=" << pose.value->sequenceFrame
                  << "\nobject_id=" << request.objectId
                  << "\nchain_nodes=" << pose.value->chain.size() << '\n';
        std::cout << std::setprecision(9);
        for (const auto& node : pose.value->chain) {
            std::cout << "node=" << node.objectId << " parent=" << node.parentId << " world_matrix=";
            for (std::size_t index = 0; index < node.worldMatrix.size(); ++index) {
                if (index != 0) std::cout << ',';
                std::cout << node.worldMatrix[index];
            }
            std::cout << '\n';
        }
        return 0;
    }
    if (command == L"mdx-inspect" && argc == 3) {
        const auto bytes = ReadBytes(argv[2]);
        if (!bytes) {
            std::cerr << "Mdx: FILE_READ_FAILED\n";
            return 1;
        }
        const auto parsed = War3Map::Render::ParseMdx(*bytes);
        if (!parsed.Succeeded() || !parsed.value.has_value()) {
            PrintDiagnostics(parsed.diagnostics);
            return 1;
        }
        const auto& model = *parsed.value;
        std::cout << "version=" << model.version << " name=" << model.name << " sequences=" << model.sequences.size()
                  << " skippedChunks=" << model.skippedChunks << '\n';
        std::cout << "bones=" << model.bones.size() << " helpers=" << model.helpers.size()
                  << " pivots=" << model.pivots.size() << " globalSequences=" << model.globalSequences.size()
                  << " skinningTopologyComplete=" << (model.skinningTopologyComplete ? "true" : "false") << '\n';
        if (!model.skinningTopologyComplete) std::cout << "skinningTopologyIssue=" << model.skinningTopologyIssue << '\n';
        for (std::size_t i = 0; i < std::min<std::size_t>(model.sequences.size(), 64); ++i) {
            const auto& sequence = model.sequences[i];
            std::string name = sequence.name;
            for (auto& character : name) if (static_cast<unsigned char>(character) < 0x20) character = ' ';
            std::cout << "sequence[" << i << "] start=" << sequence.start << " end=" << sequence.end
                      << " flags=" << sequence.flags << " name=" << name << '\n';
        }
        if (model.sequences.size() > 64) std::cout << "sequenceListTruncated=true\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(model.bones.size(), 64); ++i)
            std::cout << "bone[" << i << "] objectId=" << model.bones[i].objectId
                      << " parentId=" << model.bones[i].parentId << '\n';
        if (model.bones.size() > 64) std::cout << "boneListTruncated=true\n";
        for (std::size_t i = 0; i < model.textures.size(); ++i)
            std::cout << "texture[" << i << "] replaceable=" << model.textures[i].replaceableId
                      << " path=" << model.textures[i].path << '\n';
        for (std::size_t i = 0; i < model.materials.size(); ++i)
            for (const auto& layer : model.materials[i].layers)
                std::cout << "material[" << i << "] filter=" << static_cast<std::uint32_t>(layer.filterMode)
                          << " shading=0x" << std::hex << layer.shadingFlags << std::dec << " texture=" << layer.textureId
                          << " alpha=" << layer.alpha << '\n';
        for (std::size_t i = 0; i < model.geosets.size(); ++i) {
            const auto& geoset = model.geosets[i];
            std::cout << "geoset[" << i << "] vertices=" << geoset.positions.size() << " triangles=" << geoset.indices.size() / 3
                      << " material=" << geoset.materialId << " visibleInStand=" << (geoset.visibleInStand ? 1 : 0)
                      << " skippedGroups=" << geoset.skippedPrimitiveGroups
                      << " matrixGroups=" << geoset.matrixGroupSizes.size();
            // 实际顶点包围盒（模型头的范围可能与几何不一致）。
            if (!geoset.positions.empty()) {
                auto low = geoset.positions.front(), high = low;
                for (const auto& p : geoset.positions) for (int axis = 0; axis < 3; ++axis) {
                    low[axis] = std::min(low[axis], p[axis]); high[axis] = std::max(high[axis], p[axis]);
                }
                std::cout << " min=(" << low[0] << ',' << low[1] << ',' << low[2] << ") max=(" << high[0] << ',' << high[1] << ',' << high[2] << ')';
            }
            std::cout << '\n';
        }
        return 0;
    }
    if (command == L"blp-decode" && argc == 4) {
        const auto blpBytes = ReadBytes(argv[2]);
        if (!blpBytes) {
            std::cerr << "Blp: FILE_READ_FAILED\n";
            return 1;
        }
        auto decoded = War3Map::Image::DecodeBlp(*blpBytes);
        if (!decoded.Succeeded() || !decoded.value.has_value()) {
            PrintDiagnostics(decoded.diagnostics);
            return 1;
        }
        const auto& img = *decoded.value;
        std::cout << "width=" << img.width << " height=" << img.height << '\n';
        // 写 PPM (P6, RGB)。
        std::ofstream out(argv[3], std::ios::binary);
        out << "P6\n" << img.width << ' ' << img.height << "\n255\n";
        for (std::size_t i = 0; i < static_cast<std::size_t>(img.width) * img.height; ++i) {
            out.put(static_cast<char>(img.rgba[i * 4 + 0]));
            out.put(static_cast<char>(img.rgba[i * 4 + 1]));
            out.put(static_cast<char>(img.rgba[i * 4 + 2]));
        }
        // 打印中心像素供校对。
        const std::size_t cx = img.width / 2;
        const std::size_t cy = img.height / 2;
        const std::size_t ci = (cy * img.width + cx) * 4;
        std::cout << "centerRGBA="
                  << static_cast<int>(img.rgba[ci + 0]) << ','
                  << static_cast<int>(img.rgba[ci + 1]) << ','
                  << static_cast<int>(img.rgba[ci + 2]) << ','
                  << static_cast<int>(img.rgba[ci + 3]) << '\n';
        return 0;
    }

    if ((command != L"inspect" && command != L"validate") || argc < 3) {
        PrintUsage();
        return 2;
    }

    const auto profile = SelectProfile(argc >= 4 ? argv[3] : L"default");
    if (!profile) {
        std::cerr << "Profile: PROFILE_UNKNOWN: expected war3-1.27a-zhCN or default\n";
        return 2;
    }
    auto session = War3Map::EditorSession::Open(War3Map::OpenRequest{
        argv[2],
        *profile,
        {},
    });
    if (!session.Succeeded()) {
        PrintDiagnostics(session.diagnostics);
        return 1;
    }

    std::cout << "profile=" << session.value->Profile().id << '\n'
              << "profileSha256=" << session.value->Profile().definitionSha256 << '\n'
              << "generation=" << session.value->Revision().generation << '\n'
              << "sourceSha256=" << session.value->Revision().sourceSha256 << '\n'
              << "uiCatalogSha256=" << session.value->Revision().uiCatalogSha256 << '\n';

    if (command == L"inspect") {
        return 0;
    }

    auto transaction = session.value->BeginEdit(session.value->Revision());
    if (!transaction.Succeeded()) {
        PrintDiagnostics(transaction.diagnostics);
        return 1;
    }
    const auto report = session.value->Validate(
        *transaction.value,
        *profile,
        War3Map::CompatibilityLevel::Parsed);
    PrintDiagnostics(report.diagnostics);
    const auto rollbackStatus = session.value->Rollback(*transaction.value);
    if (!rollbackStatus.Succeeded()) {
        PrintDiagnostics(rollbackStatus.diagnostics);
        return 1;
    }
    return report.satisfied ? 0 : 1;
}
