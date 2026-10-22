#include "War3MapCore/Script/SlkDataset.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "MapDataFixtures.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>

// 原创 INI/W3I 小夹具，默认测试只走内存专用文件回调，不读取 YDWE/地图或执行 Lua。
namespace {
using namespace War3Map;
using namespace War3Map::Script;
constexpr std::array<std::string_view, 7> kTypes{"unit", "ability", "item", "buff", "upgrade", "doodad", "destructable"};
constexpr std::array<std::string_view, 9> kFiles{"ability", "buff", "unit", "item", "upgrade", "doodad", "destructable", "txt", "misc"};
void Require(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> result;
    for (const unsigned char c : text) result.push_back(static_cast<std::byte>(c));
    return result;
}
std::string Digest(const std::vector<std::byte>& bytes) {
    const auto hash = Sha256(bytes); Require(hash.Succeeded(), "fixture SHA failed"); return *hash.value;
}
std::vector<std::byte> W3i(std::int32_t dataSet = 1, std::uint32_t flags = 0) {
    auto parsed = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
    Require(parsed.Succeeded(), "original W3I fixture failed");
    parsed.value->gameDataSet = dataSet; parsed.value->flags = flags;
    auto output = parsed.value->Serialize(); Require(output.Succeeded(), "original W3I serialize failed");
    return std::move(*output.value);
}
std::string MetadataField(std::string_view name, std::string_view rawField, std::string_view id, int type) {
    return "[." + std::string(name) + "]\nfield = " + std::string(rawField) + "\nid = " + std::string(id) +
        "\nkey = " + std::string(name) + "\ntype = " + std::to_string(type) + "\n";
}
std::string Metadata() {
    std::string text;
    for (const auto type : kTypes) text += "[" + std::string(type) + "]\n" + MetadataField("name", "Name", "Name", 3) + "profile = true\n";
    text += "[P001]\n" + MetadataField("dataa", "DataA", "D001", 0) + "data = 1\nrepeat = 4\n";
    text += "[P002]\n" + MetadataField("dataa", "DataA", "D002", 2) + "data = 1\nrepeat = 4\n";
    text += "[misc]\n" + MetadataField("w2lobject", "W2LObject", "W2lo", 3) + "profile = true\n";
    text += "[FontScope]\ntype = misc\n" + MetadataField("height", "Height", "H001", 1);
    return text;
}
struct Fixture {
    std::filesystem::path core = std::filesystem::current_path() / "fixture-core";
    std::filesystem::path installed = std::filesystem::current_path() / "fixture-installed";
    std::map<std::filesystem::path, std::vector<std::byte>> files;
    std::vector<std::filesystem::path> requested;
    std::vector<std::byte> w3i = W3i();
    std::uint64_t revision = 17;
    Fixture() {
        files[core / "config.ini"] = Bytes("[global]\ndata = fixture-data\ndata_ui = ${YDWE}\ndata_meta = ${DEFAULT}\n[obj]\nread_slk = false\nconfusion = ''\n");
        files[core / "defined/metadata.ini"] = Bytes(Metadata());
        for (const auto family : {"Custom", "Melee"}) {
            for (const auto type : kTypes) {
                std::string record = "[P001]\n_id = \"P001\"\n_type = \"" + std::string(type) + "\"\n";
                // unit/doodad 缺 _code 是合法来源形状，不能为了旧物化器填造。
                if (type == "ability") record += "_code = \"P001\"\n";
                if (type == "unit") record += "_name = \"original-label\"\n";
                record += "name = \"" + std::string(family) + "\"\ninteger = 7\nreal = 7.0\nnegativezero = -0.0\n";
                record += "values = {1,2.0,\"\",false}\nempty = {}\npath = \"folder\\\\leaf\"\n";
                files[Prebuilt(family, type)] = Bytes(record);
            }
            files[Prebuilt(family, "txt")] = Bytes("[P001]\nname = {\"原创文本\"}\n_max_level = 1\n");
            files[Prebuilt(family, "misc")] = Bytes("[FontScope]\n_type = \"misc\"\nheight = 0.013\n");
        }
    }
    std::filesystem::path Prebuilt(std::string_view family, std::string_view type) const {
        return installed / "plugin/w3x2lni/data/fixture-data/prebuilt" / family / (std::string(type) + ".ini");
    }
    SlkDatasetFileReader Reader() {
        return [this](const std::filesystem::path& path, std::size_t maximum) {
            requested.push_back(path);
            const auto found = files.find(path);
            if (found == files.end()) return Result<std::vector<std::byte>>::Failure("Fixture", "MISSING", "missing original fixture");
            // 必须完整返回，读取器本身再检查不遵守最大长度的注入来源。
            (void)maximum;
            return Result<std::vector<std::byte>>::Success(found->second);
        };
    }
    SlkDatasetRequest Request() const { return {core, installed, w3i, revision}; }
    Result<SlkTypedDataset> Read(const SlkDatasetLimits& limits = {}) { return ReadSlkTypedDataset(Request(), Reader(), limits); }
};
const SlkTypedDataset& Good(const Result<SlkTypedDataset>& result) {
    if (!result.Succeeded() || !result.diagnostics.empty()) {
        throw std::runtime_error(result.diagnostics.empty() ? "dataset failed without diagnostic" : result.diagnostics.front().code + ": " + result.diagnostics.front().message);
    }
    return *result.value;
}
void Rejected(const Result<SlkTypedDataset>& result, std::string_view code) {
    Require(!result.value && !result.Succeeded(), "failure returned a partial typed dataset");
    Require(!result.diagnostics.empty() && result.diagnostics.front().code == code, "unexpected diagnostic code");
    Require(result.diagnostics.front().origin.has_value(), "failure lost source origin");
}
template<typename T> const T& Value(const SlkDatasetFields& fields, std::string_view key) {
    return std::get<T>(std::get<SlkSessionScalar>(fields.at(std::string(key)).value));
}
void SelectionAndTypedValues() {
    for (const auto dataSet : {-1, 0, 1, 2}) for (const auto flags : {0u, 4u}) {
        Fixture fixture; fixture.w3i = W3i(dataSet, flags);
        const auto result = fixture.Read(); const auto& data = Good(result);
        const bool melee = dataSet == 2 || (dataSet <= 0 && flags == 4);
        Require(data.family == (melee ? SlkDatasetFamily::Melee : SlkDatasetFamily::Custom), "W3I dataset selection differs");
        Require(data.coverage == SlkDatasetCoverage::DecodedPrebuiltFilesOnly, "reader invented complete-session coverage");
        Require(data.sources.size() == 11 && fixture.requested.size() == 11, "did not read exactly config + metadata + nine files");
        Require(data.sources[0].path == fixture.core / "config.ini", "config root was conflated with installed root");
        Require(data.sources[1].path == fixture.core / "defined/metadata.ini", "canonical metadata source differs");
        for (std::size_t i = 0; i < kFiles.size(); ++i) {
            Require(data.sources[i + 2].path == fixture.Prebuilt(melee ? "Melee" : "Custom", kFiles[i]), "prebuilt dedicated path differs");
            Require(data.sources[i + 2].sha256 == Digest(fixture.files.at(data.sources[i + 2].path)), "file SHA not bound to decoded bytes");
        }
        const auto& fields = data.defaults[0].at("P001").fields;
        Require(Value<std::string>(fields, "name") == (melee ? "Melee" : "Custom"), "wrong dataset content selected");
        Require(Value<std::int64_t>(fields, "integer") == 7 && Value<double>(fields, "real") == 7.0, "integer/double subtype lost");
        Require(std::signbit(Value<double>(fields, "negativezero")), "negative double zero sign lost");
        const auto& values = std::get<std::vector<SlkSessionScalar>>(fields.at("values").value);
        Require(values.size() == 4 && std::get<std::int64_t>(values[0]) == 1 && std::get<double>(values[1]) == 2.0 &&
            std::get<std::string>(values[2]).empty() && !std::get<bool>(values[3]), "dense typed values changed");
        Require(std::get<std::vector<SlkSessionScalar>>(fields.at("empty").value).empty(), "empty array lost");
        Require(!fields.contains("_code") && Value<std::string>(fields, "_name") == "original-label", "source internal fields fabricated/discarded");
        Require(!data.defaults[5].at("P001").fields.contains("_code"), "doodad base code fabricated");
        Require(Value<std::string>(fields, "path") == "folder\\leaf", "backslash bytes changed");
        Require(!data.txt.at("P001").fields.contains("_id") && !data.misc.empty(), "txt/misc source shape changed");
        const auto& position = fields.at("real").source;
        const auto& bytes = fixture.files.at(data.sources.at(position.sourceIndex).path);
        const std::string literal(reinterpret_cast<const char*>(bytes.data() + position.offset), position.length);
        Require(literal == "7.0", "typed value source span does not identify literal");
        Require(data.w3iSha256 == Digest(fixture.w3i) && data.documentRevision == fixture.revision, "candidate selection identity lost");
    }
    Fixture fixture; fixture.w3i = W3i(3);
    Rejected(fixture.Read(), "SLK_DATASET_W3I_UNSUPPORTED");
    Require(fixture.requested.empty(), "unknown W3I selector read defaults");
    fixture.w3i.clear(); Rejected(fixture.Read(), "SLK_DATASET_W3I_UNSUPPORTED");
}
void MetadataLayersAndReceipts() {
    Fixture fixture;
    auto text = Metadata();
    const auto at = text.find("[P001]");
    // 插在最后一个 common scope：同名 common/byCode 属性也不能预先合并。
    text.insert(at, MetadataField("dataa", "DataA", "C001", 3) + "default = {1500,1500}\n");
    fixture.files[fixture.core / "defined/metadata.ini"] = Bytes(text);
    const auto first = fixture.Read(); const auto& data = Good(first);
    Require(Value<std::int64_t>(data.metadata.byCode.at("P001").fields.at("dataa").fields, "type") == 0, "first baseCode schema type lost");
    Require(Value<std::int64_t>(data.metadata.byCode.at("P002").fields.at("dataa").fields, "type") == 2, "second baseCode schema collapsed");
    Require(Value<std::int64_t>(data.metadata.common[6].fields.at("dataa").fields, "type") == 3, "common schema overwritten by baseCode");
    Require(std::get<std::vector<SlkSessionScalar>>(data.metadata.common[6].fields.at("dataa").fields.at("default").value).size() == 2,
        "metadata default array coerced to string");
    Require(Value<std::string>(data.metadata.byCode.at("FontScope").properties, "type") == "misc", "named misc scope lost");
    Require(Value<std::string>(data.metadata.misc.fields.at("w2lobject").fields, "id") == "W2lo", "W2lo metadata source lost");
    const auto digest = data.manifestSha256;
    ++fixture.revision;
    const auto revised = fixture.Read(); Require(Good(revised).manifestSha256 != digest, "revision not bound into manifest");
    --fixture.revision;
    fixture.files[fixture.Prebuilt("Custom", "unit")] = Bytes("[P001]\nname = \"Changed\"\n");
    const auto changed = fixture.Read();
    Require(Good(changed).manifestSha256 != digest, "changed source bytes did not change manifest");
    Require(Value<std::string>(data.defaults[0].at("P001").fields, "name") == "Custom", "returned graph aliases mutable source callback");
}
void MetadataShortFieldIds() {
    // 原创名字覆盖 target c4 截NUL/补零的长度域；读取层必须保留原文字节。
    for (const auto id : {"X", "Ab", "Abc", "Abcd"}) {
        Fixture fixture;
        fixture.files[fixture.core / "defined/metadata.ini"] = Bytes(Metadata() + "[P003]\n" + MetadataField("sample", "Sample", id, 2));
        const auto result = fixture.Read();
        const auto& fields = Good(result).metadata.byCode.at("P003").fields.at("sample").fields;
        Require(Value<std::string>(fields, "id") == id, "metadata field ID was padded, truncated or guessed");
    }
    for (const auto id : {"\"\"", "Abcde"}) {
        Fixture fixture;
        fixture.files[fixture.core / "defined/metadata.ini"] = Bytes(Metadata() + "[P003]\n" + MetadataField("sample", "Sample", id, 2));
        Rejected(fixture.Read(), "SLK_DATASET_METADATA_UNSUPPORTED");
    }
    Fixture object;
    object.files[object.Prebuilt("Custom", "unit")] = Bytes("[Abc]\n_id = \"Abc\"\nname = \"short object\"\n");
    Rejected(object.Read(), "SLK_DATASET_OBJECT_ID");
}
void MissingFilesAndInvalidSyntax() {
    for (const auto file : kFiles) {
        Fixture fixture; fixture.files.erase(fixture.Prebuilt("Custom", file));
        Rejected(fixture.Read(), "SLK_DATASET_FALLBACK_UNSUPPORTED");
    }
    Fixture empty; empty.files[empty.Prebuilt("Custom", "txt")].clear();
    const auto emptyResult = empty.Read(); Require(Good(emptyResult).txt.empty(), "present empty prebuilt wrongly triggered fallback");
    Require(Good(emptyResult).sources[9].byteCount == 0, "empty source identity lost");
    for (const auto text : {"[P001]\nx = 1\nx = 2\n", "[P001]\nx = 1\n[P001]\ny = 2\n"}) {
        Fixture fixture; fixture.files[fixture.Prebuilt("Custom", "ability")] = Bytes(text);
        Rejected(fixture.Read(), "SLK_DATASET_DUPLICATE");
    }
    for (const auto key : {"10", "1name", "-1"}) {
        Fixture fixture; fixture.files[fixture.Prebuilt("Custom", "ability")] = Bytes("[P001]\n" + std::string(key) + " = 7\n");
        Rejected(fixture.Read(), "SLK_DATASET_SYNTAX");
    }
    for (const auto section : {"AB:C", "A.B0", "\"P001\"", "'P001'", "A<01", "A>01"}) {
        // txt 无 rawcode 长度约束，也不能把带引号/继承的节当成普通名称返回。
        Fixture fixture; fixture.files[fixture.Prebuilt("Custom", "txt")] = Bytes("[" + std::string(section) + "]\nname = \"x\"\n");
        Rejected(fixture.Read(), "SLK_DATASET_SYNTAX");
    }
    Fixture numericSection;
    numericSection.files[numericSection.Prebuilt("Custom", "unit")] = Bytes("[1001]\n_id = \"1001\"\nname = \"numeric section\"\n");
    const auto numericResult = numericSection.Read();
    Require(Good(numericResult).defaults[0].contains("1001"), "plain numeric section incorrectly rejected/coerced to numeric key");
    for (const auto value : {"{1,{2}}", "{1,}", "nil", "nan", "1 + 2", "io.open('x')", "\"bad\\q\"", "1e999", "9223372036854775808"}) {
        Fixture fixture; fixture.files[fixture.Prebuilt("Custom", "ability")] = Bytes("[P001]\nx = " + std::string(value) + "\n");
        Rejected(fixture.Read(), "SLK_DATASET_VALUE_UNSUPPORTED");
    }
    for (const auto identity : {"_id = 1", "_id = \"P002\"", "_type = false", "_type = \"unit\""}) {
        Fixture fixture; fixture.files[fixture.Prebuilt("Custom", "ability")] = Bytes("[P001]\n" + std::string(identity) + "\n");
        Rejected(fixture.Read(), "SLK_DATASET_OBJECT_ID");
    }
    Fixture nul; nul.files[nul.Prebuilt("Custom", "ability")].push_back(std::byte{0});
    Rejected(nul.Read(), "SLK_DATASET_SYNTAX");
    for (const auto property : {"unknown = true", "repeat = 1.5", "index = -1", "concat = 1"}) {
        Fixture fixture; fixture.files[fixture.core / "defined/metadata.ini"] = Bytes(Metadata() + std::string(property) + "\n");
        Rejected(fixture.Read(), "SLK_DATASET_METADATA_UNSUPPORTED");
    }
    Fixture duplicate;
    duplicate.files[duplicate.core / "defined/metadata.ini"] = Bytes(Metadata() + MetadataField("height", "Height", "H002", 1));
    Rejected(duplicate.Read(), "SLK_DATASET_DUPLICATE");
    Fixture collision;
    collision.files[collision.core / "defined/metadata.ini"] = Bytes(Metadata() + MetadataField("type", "Type", "T001", 3));
    Rejected(collision.Read(), "SLK_DATASET_DUPLICATE");
    Fixture missing; missing.files.erase(missing.core / "defined/metadata.ini");
    Rejected(missing.Read(), "SLK_DATASET_SOURCE_READ");
    Fixture path; path.core = "relative-core";
    Rejected(path.Read(), "SLK_DATASET_ROOT");
    for (const auto data : {"../outside", ".", "nested/path"}) {
        Fixture fixture; fixture.files[fixture.core / "config.ini"] = Bytes("[global]\ndata = \"" + std::string(data) + "\"\n");
        Rejected(fixture.Read(), "SLK_DATASET_SELECTION_UNSUPPORTED");
    }
}
void BareCommentsAreRejected() {
    Fixture config;
    config.files[config.core / "config.ini"] = Bytes("[global]\ndata = fixture--data\n");
    Rejected(config.Read(), "SLK_DATASET_VALUE_UNSUPPORTED");
    Fixture metadata;
    metadata.files[metadata.core / "defined/metadata.ini"] = Bytes(Metadata() + "displayname = original--comment\n");
    Rejected(metadata.Read(), "SLK_DATASET_VALUE_UNSUPPORTED");
    metadata.files[metadata.core / "defined/metadata.ini"] = Bytes(Metadata() + "displayname = \"original--comment\"\n");
    const auto parsed = metadata.Read();
    Require(Value<std::string>(Good(parsed).metadata.byCode.at("FontScope").fields.at("height").fields,
        "displayname") == "original--comment", "quoted comment marker was changed");
}
void LimitsAreFailClosed() {
    Fixture fixture;
    SlkDatasetLimits limits;
    limits.fileBytes = 1; Rejected(fixture.Read(limits), "SLK_DATASET_LIMIT");
    limits = {}; limits.fileBytes += 1; Rejected(fixture.Read(limits), "SLK_DATASET_LIMIT");
    limits = {}; limits.totalBytes = fixture.w3i.size(); Rejected(fixture.Read(limits), "SLK_DATASET_LIMIT");
    limits = {}; limits.records = 1; Rejected(fixture.Read(limits), "SLK_DATASET_LIMIT");
    limits = {}; limits.fields = 1; Rejected(fixture.Read(limits), "SLK_DATASET_LIMIT");
    limits = {}; limits.values = 1; Rejected(fixture.Read(limits), "SLK_DATASET_LIMIT");
    limits = {}; limits.stringBytes = 1; Rejected(fixture.Read(limits), "SLK_DATASET_LIMIT");
    limits = {}; limits.metadataFields = 1; Rejected(fixture.Read(limits), "SLK_DATASET_LIMIT");
    limits = {}; limits.arrayElements = 3; Rejected(fixture.Read(limits), "SLK_DATASET_LIMIT");
    // 数组只有空容器也必须占预算。先测出小夹具的最小通过值，再加一个空字段。
    std::size_t minimumValues = 0;
    std::size_t lower = 0, upper = 1000;
    while (lower < upper) {
        const auto middle = lower + (upper - lower) / 2;
        limits = {}; limits.values = middle;
        if (fixture.Read(limits).Succeeded()) upper = middle; else lower = middle + 1;
    }
    minimumValues = lower;
    limits = {}; limits.values = minimumValues;
    const auto exact = fixture.Read(limits); (void)Good(exact);
    auto& file = fixture.files[fixture.Prebuilt("Custom", "misc")];
    const auto extra = Bytes("empty = {}\n"); file.insert(file.end(), extra.begin(), extra.end());
    Rejected(fixture.Read(limits), "SLK_DATASET_LIMIT");
    limits.values = minimumValues + 2;
    const auto added = fixture.Read(limits); (void)Good(added);
}
void LegacyScriptPrebuiltLayout() {
    // 旧入口：metadata 与默认数据都在 script/prebuilt 下，一份默认数据，"名称:序号" 键统一成 名称_序号。
    Fixture fixture;
    const auto prebuilt = fixture.installed / "plugin/w3x2lni/script/prebuilt";
    fixture.files.clear();
    auto metadata = Metadata();
    metadata.insert(metadata.find("[P001]"), "[.\"buttonpos:1\"]\nfield = Buttonpos:1\nid = abpx\nindex = 1\nkey = buttonpos\ntype = 0\n");
    fixture.files[prebuilt / "defined/metadata.ini"] = Bytes(metadata);
    for (const auto type : kTypes)
        fixture.files[prebuilt / "default" / (std::string(type) + ".ini")] =
            Bytes("[P001]\n_id=\"P001\"\n_type=\"" + std::string(type) + "\"\nname=\"legacy\"\n\"buttonpos:1\"=2\n");
    fixture.files[prebuilt / "default/txt.ini"] = Bytes("[P001]\nname = {\"原创文本\"}\n");
    fixture.files[prebuilt / "default/misc.ini"] = Bytes("[FontScope]\n_type=\"misc\"\nheight=0.013\n");
    auto request = fixture.Request();
    request.coreRoot = prebuilt; request.layout = SlkDatasetLayout::ScriptPrebuiltDefault;
    for (const auto dataSet : {1, 2}) {
        fixture.w3i = W3i(dataSet); request.finalW3i = fixture.w3i; fixture.requested.clear();
        const auto result = ReadSlkTypedDataset(request, fixture.Reader());
        const auto& data = Good(result);
        Require(data.layout == SlkDatasetLayout::ScriptPrebuiltDefault && data.dataSetId.empty(), "legacy layout identity lost");
        Require(data.sources.size() == 10 && data.sources[0].path == prebuilt / "defined/metadata.ini", "legacy read config or wrong metadata");
        const auto& fields = data.defaults[1].at("P001").fields;
        Require(Value<std::string>(fields, "name") == "legacy" && Value<std::int64_t>(fields, "buttonpos_1") == 2, "indexed key not canonical");
        const auto& field = data.metadata.common[6].fields.at("buttonpos_1").fields; // 插在 destructable 节末
        Require(Value<std::string>(field, "field") == "Buttonpos_1", "metadata indexed field not canonical");
    }
    // 新版布局仍拒绝引号键；旧布局拒绝不是 名称:序号 的引号键。
    auto modern = Fixture();
    modern.files[modern.Prebuilt("Custom", "ability")] = Bytes("[P001]\n\"buttonpos:1\"=2\n");
    Rejected(modern.Read(), "SLK_DATASET_SYNTAX");
    fixture.files[prebuilt / "default/ability.ini"] = Bytes("[P001]\n\"button pos\"=2\n");
    Rejected(ReadSlkTypedDataset(request, fixture.Reader()), "SLK_DATASET_SYNTAX");
    fixture.files[prebuilt / "default/ability.ini"] = Bytes("[P001]\n\"buttonpos:1\"=2\nbuttonpos_1=3\n");
    Rejected(ReadSlkTypedDataset(request, fixture.Reader()), "SLK_DATASET_DUPLICATE");
}
void LayoutDetection() {
    const auto root = std::filesystem::temp_directory_path() / "audastra-slk-layout-fixture";
    std::filesystem::remove_all(root);
    const auto plugin = root / "plugin/w3x2lni";
    const auto layout = [&]() { return DetectSlkDatasetLayout(root); };
    Require(!layout().Succeeded() && layout().diagnostics.front().code == "SLK_DATASET_LAYOUT_MISSING", "empty root guessed a layout");
    std::filesystem::create_directories(plugin / "script/prebuilt/default");
    { std::ofstream(plugin / "script/w3x2lni.lua") << "-- original fixture\n"; }
    Require(layout().Succeeded() && *layout().value == SlkDatasetLayout::ScriptPrebuiltDefault, "legacy entry not detected");
    Require(SlkDatasetCoreRoot(root, SlkDatasetLayout::ScriptPrebuiltDefault) == plugin / "script/prebuilt", "legacy core root differs");
    std::filesystem::create_directories(plugin / "data");
    Require(!layout().Succeeded() && layout().diagnostics.front().code == "SLK_DATASET_LAYOUT_AMBIGUOUS", "ambiguous layout guessed");
    std::filesystem::remove(plugin / "script/w3x2lni.lua");
    Require(layout().Succeeded() && *layout().value == SlkDatasetLayout::DataSetPrebuilt, "data-set layout not detected");
    Require(SlkDatasetCoreRoot(root, SlkDatasetLayout::DataSetPrebuilt) == plugin / "script/core", "core root differs");
    std::filesystem::remove_all(root);
}
int Probe(int argc, char** argv) {
    if (argc != 5 || std::string_view(argv[1]) != "--probe" ||
        (std::string_view(argv[4]) != "Custom" && std::string_view(argv[4]) != "Melee" && std::string_view(argv[4]) != "Legacy")) {
        std::cerr << "Usage: --probe <core-root> <installed-ydwe-root> <Custom|Melee|Legacy>\n"; return 2;
    }
    const bool legacy = std::string_view(argv[4]) == "Legacy";
    const auto w3i = W3i(std::string_view(argv[4]) == "Melee" ? 2 : 1);
    const auto utf8Path = [](std::string_view input) {
        std::u8string text;
        for (const unsigned char byte : input) text.push_back(static_cast<char8_t>(byte));
        return std::filesystem::path(text);
    };
    const auto result = ReadSlkTypedDataset({utf8Path(argv[2]), utf8Path(argv[3]), w3i, 0,
        legacy ? SlkDatasetLayout::ScriptPrebuiltDefault : SlkDatasetLayout::DataSetPrebuilt});
    if (!result.Succeeded()) {
        for (const auto& diagnostic : result.diagnostics) {
            std::cerr << diagnostic.code << ": " << diagnostic.message;
            if (diagnostic.origin) std::cerr << " offset=" << diagnostic.origin->offset;
            std::cerr << '\n';
        }
        return 1;
    }
    const auto& data = *result.value;
    std::cout << "coverage=DecodedPrebuiltFilesOnly selectionSource=original-w3i-fixture\n";
    std::cout << "dataset=" << data.dataSetId << " family=" << argv[4] << " manifest=" << data.manifestSha256 << '\n';
    for (const auto& source : data.sources) std::cout << source.role << " bytes=" << source.byteCount << " sha256=" << source.sha256 << '\n';
    for (std::size_t i = 0; i < kTypes.size(); ++i)
        std::cout << kTypes[i] << " records=" << data.defaults[i].size() << " commonMetadata=" << data.metadata.common[i].fields.size() << '\n';
    std::cout << "txt=" << data.txt.size() << " misc=" << data.misc.size() << " metadataByCode=" << data.metadata.byCode.size() << '\n';
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1) return Probe(argc, argv);
        SelectionAndTypedValues(); MetadataLayersAndReceipts(); MetadataShortFieldIds(); MissingFilesAndInvalidSyntax();
        BareCommentsAreRejected(); LimitsAreFailClosed(); LegacyScriptPrebuiltLayout(); LayoutDetection();
        std::cout << "SLK typed dataset original-fixture tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
