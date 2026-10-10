#include "War3MapCore/ObjectData/StandardObjectCatalog.h"
#include "War3MapCore/Game/GameDataSource.h"
#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Image/BlpImage.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <unordered_map>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;
int failures = 0;

void Check(bool condition, std::string_view message) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    for (const unsigned char value : text) bytes.push_back(static_cast<std::byte>(value));
    return bytes;
}

void Synthetic() {
    // 此处全部为原创数据，测试不携带 Blizzard 资源。
    const std::unordered_map<std::string, std::string> files{
        {"base.slk", "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"Name\"\nC;X3;K\"Race\"\n"
            "C;X1;Y2;K\"A001\"\nC;X2;K\"WESTRING_TEST_NAME\"\nC;X3;K\"human\"\n"
            "C;X1;Y3;K\"a001\"\nC;X2;K\"Original; quoted \"\"name\"\"\"\nC;X3;K\"orc\"\n"
            "C;X1;Y4;K\"A002\"\nC;X3;K\"human\"\n"
            "C;X1;Y5;K\"A003\"\nC;X1;Y6;K\"A004\"\nE\n"},
        {"extra.slk", "ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X2;K\"hero\"\nC;X3;K\"code\"\n"
            "C;X1;Y2;K\"A001\"\nC;X2;K1\nC;X1;Y3;K\"ZZZZ\"\nC;X2;K1\n"
            "C;X1;Y4;K\"A002\"\nC;X3;K\"A001\"\n"
            "C;X1;Y5;K\"A003\"\nC;X3;K\"ZZZZ\"\n"
            "C;X1;Y6;K\"A004\"\nC;X3;K\"zzzz\"\nE\n"},
        {"profile.txt", "\xEF\xBB\xBF// synthetic\n[A001]\nArt=Icons\\Example.blp\n"
            "Tip=\"First, level\",\"Second level\"\nName=\"本地化技能\"\n"
            "[ZZZZ]\nName=Profile-only base\nArt=Icons\\ProfileDonor.blp\n"},
        {"override.txt", "[A001]\nArt=Icons\\Patched.blp\n"},
        {"strings.txt", "[WorldEditStrings]\nWESTRING_TEST_NAME=可解析名称\n"},
    };
    const StandardObjectCatalog::FileReader reader = [&files](const std::string& path) {
        const auto found = files.find(path);
        return found == files.end() ? Result<std::vector<std::byte>>::Failure("Test", "missing", path)
            : Result<std::vector<std::byte>>::Success(Bytes(found->second));
    };
    CatalogLoadOptions options;
    options.sources = {{ObjectType::Ability, {"base.slk", "extra.slk"}, {"profile.txt", "override.txt"}}};
    options.sources[0].presentationParentField = "code";
    options.stringFiles = {"strings.txt"};
    auto loaded = StandardObjectCatalog::Load(reader, options);
    Check(loaded.Succeeded(), "catalog synthetic load");
    if (!loaded.Succeeded()) return;
    const auto& catalog = *loaded.value;
    Check(catalog.Count(ObjectType::Ability) == 5, "supplement/profile cannot create ghost objects");
    const auto* upper = catalog.Find(ObjectType::Ability, "A001");
    const auto* lower = catalog.Find(ObjectType::Ability, "a001");
    Check(upper && lower && upper != lower, "FourCC identity is case sensitive");
    Check(!catalog.Find(ObjectType::Unit, "A001"), "type participates in identity");
    if (upper) {
        Check(upper->displayName == "本地化技能" && upper->nameResolved, "UTF-8 BOM and profile override");
        Check(upper->iconPath == "Icons\\Patched.blp", "ordered profile override and path preservation");
        Check(upper->category == "hero" && upper->race == "human", "category from joined SLK fields");
        Check(upper->Field("TiP") == "\"First, level\",\"Second level\"", "CSV level values retained and field lookup insensitive");
    }
    if (lower) Check(lower->displayName == "Original; quoted \"name\"", "SLK delimiter and escaped quote parsing");
    const auto* alias = catalog.Find(ObjectType::Ability, "A002");
    Check(alias && alias->displayName == "本地化技能" && alias->iconPath == "Icons\\Patched.blp"
        && alias->nameSourceRawcode == "A001" && alias->Field("name").empty(),
        "code alias presentation fallback retains raw own fields and donor identity");
    const auto* profileAlias = catalog.Find(ObjectType::Ability, "A003");
    Check(profileAlias && profileAlias->displayName == "Profile-only base"
        && profileAlias->iconPath == "Icons\\ProfileDonor.blp" && profileAlias->nameSourceRawcode == "ZZZZ"
        && !catalog.Find(ObjectType::Ability, "ZZZZ"), "profile-only donor supplies presentation but is not a catalog object");
    const auto* wrongCaseAlias = catalog.Find(ObjectType::Ability, "A004");
    Check(wrongCaseAlias && !wrongCaseAlias->nameResolved && wrongCaseAlias->displayName == "A004"
        && wrongCaseAlias->iconPath.empty(), "profile-only donor lookup preserves rawcode case");
    options.sources[0].profileFiles.clear();
    auto localized = StandardObjectCatalog::Load(reader, options);
    Check(localized.Succeeded() && localized.value->Find(ObjectType::Ability, "A001")->displayName == "可解析名称", "WESTRING lookup");
    // 统一字符串表优先于 stringFiles（YDWE 覆盖层的中文先于游戏自带文本）。
    Game::WorldEditStrings table;
    table.AddText("overlay", "[WorldEditStrings]\nWESTRING_TEST_NAME=覆盖层名称\n");
    options.strings = &table;
    auto overlaid = StandardObjectCatalog::Load(reader, options);
    Check(overlaid.Succeeded() && overlaid.value->Find(ObjectType::Ability, "A001")->displayName == "覆盖层名称",
        "string table overrides string files");
    options.strings = nullptr;
    options.sources[0].slkFiles = {"missing.slk", "extra.slk"};
    Check(!StandardObjectCatalog::Load(reader, options).Succeeded(), "missing identity table cannot promote supplement");
    options.sources[0].slkFiles = {"bad.slk"};
    const auto malformedReader = [](const std::string&) { return Result<std::vector<std::byte>>::Success(Bytes("ID;PWXL\nC;X0;Y1;K\"alias\"\nE\n")); };
    Check(!StandardObjectCatalog::Load(malformedReader, options).Succeeded(), "invalid coordinate refused");
    const auto truncatedReader = [](const std::string&) { return Result<std::vector<std::byte>>::Success(Bytes("ID;PWXL\nC;X1;Y1;K\"alias\"\nC;X1;Y2;K\"A001\"\n")); };
    Check(!StandardObjectCatalog::Load(truncatedReader, options).Succeeded(), "truncated SLK refused");
    options.sources[0].slkFiles = {"base.slk"};
    options.maximumFileBytes = 8;
    Check(!StandardObjectCatalog::Load(reader, options).Succeeded(), "bounded source size");
}

void RealGame(const std::filesystem::path& root) {
    auto source = Game::GameDataSource::Mount(root);
    Check(source.Succeeded(), "mount local game read-only");
    if (!source.Succeeded()) return;
    auto loaded = StandardObjectCatalog::Load(*source.value);
    for (const auto& diagnostic : loaded.diagnostics) {
        std::cout << "diagnostic=" << diagnostic.code << ' ' << diagnostic.message << '\n';
    }
    Check(loaded.Succeeded(), "real MPQ catalog load");
    if (!loaded.Succeeded()) return;
    constexpr std::array types{ObjectType::Unit, ObjectType::Ability, ObjectType::Item, ObjectType::Buff,
        ObjectType::Upgrade, ObjectType::Doodad, ObjectType::Destructable};
    std::unordered_map<std::string, bool> decodedPaths;
    for (const auto type : types) {
        std::size_t named = 0, icons = 0, readableIcons = 0, visible = 0, decodedIcons = 0, unnamedVisible = 0;
        for (const auto& entry : loaded.value->Entries()) {
            if (entry.key.type != type) continue;
            named += entry.nameResolved;
            visible += entry.editorVisible;
            unnamedVisible += entry.editorVisible && !entry.nameResolved;
            if (!entry.nameResolved) std::cout << "unnamed=" << entry.key.rawcode << " visible=" << entry.editorVisible
                << " comment=" << entry.Field("comments") << '\n';
            if (!entry.iconPath.empty()) {
                ++icons;
                readableIcons += source.value->HasFile(entry.iconPath);
                if (!decodedPaths.contains(entry.iconPath)) {
                    auto bytes = source.value->ReadFile(entry.iconPath);
                    auto image = bytes.Succeeded() ? Image::DecodeBlp(*bytes.value)
                        : Result<Image::DecodedImage>::Failure("Test", "icon-unreadable", entry.iconPath);
                    const bool valid = image.Succeeded() && image.value->Valid();
                    decodedPaths[entry.iconPath] = valid;
                    if (!valid) std::cout << "iconDecodeFailure=" << entry.iconPath << '\n';
                }
                decodedIcons += decodedPaths.at(entry.iconPath);
            }
        }
        const auto count = loaded.value->Count(type);
        Check(count > 0, "all seven kinds have standard objects");
        Check(decodedIcons == icons, "every declared standard icon reads and decodes through Core BLP");
        std::cout << ToString(type) << " count=" << count << " named=" << named << " icons=" << icons
            << " existingIcons=" << readableIcons << " decodedIcons=" << decodedIcons << " editorVisible=" << visible
            << " unnamedVisible=" << unnamedVisible << '\n';
    }
    for (const auto& key : std::array{ObjectKey{ObjectType::Unit, "hfoo"}, ObjectKey{ObjectType::Ability, "AHbz"},
        ObjectKey{ObjectType::Item, "ratf"}, ObjectKey{ObjectType::Upgrade, "Rhme"}}) {
        const auto* entry = loaded.value->Find(key.type, key.rawcode);
        Check(entry && entry->nameResolved && !entry->iconPath.empty(), "known classic object has name and icon");
        if (entry) std::cout << "sample=" << entry->key.rawcode << " name=" << entry->displayName
            << " race=" << entry->race << " category=" << entry->category << " icon=" << entry->iconPath << '\n';
    }
    std::cout << "loadedFiles=" << loaded.value->LoadedFiles().size() << '\n';
    std::cout << "uniqueDecodedPaths=" << decodedPaths.size() << '\n';
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    Synthetic();
    if (argc == 3 && std::wstring_view(argv[1]) == L"--game-root") RealGame(argv[2]);
    std::cout << "standard-object-catalog failures=" << failures << '\n';
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
