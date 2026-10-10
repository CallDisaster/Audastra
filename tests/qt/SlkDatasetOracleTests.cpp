#include "War3MapCore/Script/SlkDataset.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/MapData/MapSettings.h"
#include "../MapDataFixtures.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

#ifndef SLK_LNI_ORACLE_EXPECTED_PATH
#error SLK_LNI_ORACLE_EXPECTED_PATH must name the fixed original oracle JSON fixture
#endif

// 只读取已固定的外部 lni.dll golden；不执行 Lua，不使用 C++ parser 生成任何期望值。
namespace {
using namespace War3Map;
using namespace War3Map::Script;
constexpr std::array<std::string_view, 7> kTypes{"unit", "ability", "item", "buff", "upgrade", "doodad", "destructable"};
constexpr std::array<std::string_view, 9> kFiles{"ability", "buff", "unit", "item", "upgrade", "doodad", "destructable", "txt", "misc"};
void Require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }
std::vector<std::byte> Bytes(std::string_view text) {
    std::vector<std::byte> result;
    for (const unsigned char byte : text) result.push_back(static_cast<std::byte>(byte));
    return result;
}
QByteArray Raw(std::string_view value) { return {value.data(), static_cast<qsizetype>(value.size())}; }
QString Hex(std::string_view value) { return QString::fromLatin1(Raw(value).toHex()); }
QString StringKey(std::string_view value) { return QStringLiteral("s:") + Hex(value); }
std::string Hash(const std::vector<std::byte>& bytes) {
    const auto result = Sha256(bytes); Require(result.Succeeded(), "fixture SHA failed"); return *result.value;
}
QString JsonString(const QJsonObject& object, const char* key) {
    Require(object.value(QLatin1String(key)).isString(), std::string("golden string missing: ") + key);
    return object.value(QLatin1String(key)).toString();
}
QByteArray DecodeHex(const QString& value) {
    const auto ascii = value.toLatin1();
    Require(ascii.size() % 2 == 0, "golden hex has odd byte count");
    for (const auto ch : ascii) Require((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'),
        "golden hex contains a non-hex byte");
    return QByteArray::fromHex(ascii);
}
std::string Decimal(const QJsonObject& object) {
    const auto value = JsonString(object, "decimal").toStdString();
    std::int64_t number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    Require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && std::to_string(number) == value,
        "golden integer is not canonical int64 decimal");
    return value;
}
QJsonObject Table(QJsonObject values) {
    return {{"kind", "table"}, {"count", values.size()}, {"values", std::move(values)}};
}
QJsonObject ActualScalar(const SlkSessionScalar& scalar) {
    if (const auto* integer = std::get_if<std::int64_t>(&scalar))
        return {{"kind", "integer"}, {"decimal", QString::fromStdString(std::to_string(*integer))}};
    if (const auto* real = std::get_if<double>(&scalar)) {
        static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);
        const auto bits = std::bit_cast<std::uint64_t>(*real);
        QByteArray bytes;
        for (unsigned shift = 0; shift < 64; shift += 8) bytes.append(static_cast<char>((bits >> shift) & 0xff));
        return {{"kind", "float"}, {"ieee754le", QString::fromLatin1(bytes.toHex())}};
    }
    if (const auto* string = std::get_if<std::string>(&scalar))
        return {{"kind", "string"}, {"byteCount", static_cast<qint64>(string->size())}, {"hex", Hex(*string)}};
    return {{"kind", "boolean"}, {"value", std::get<bool>(scalar)}};
}
QJsonObject ActualValue(const SlkSessionValue& value) {
    if (const auto* scalar = std::get_if<SlkSessionScalar>(&value)) return ActualScalar(*scalar);
    QJsonObject entries;
    const auto& array = std::get<std::vector<SlkSessionScalar>>(value);
    for (std::size_t index = 0; index < array.size(); ++index)
        entries.insert(QStringLiteral("i:") + QString::number(static_cast<qulonglong>(index + 1)), ActualScalar(array[index]));
    return Table(std::move(entries));
}
QJsonObject ActualFields(const SlkDatasetFields& fields) {
    QJsonObject entries;
    for (const auto& [key, field] : fields) entries.insert(StringKey(key), ActualValue(field.value));
    return Table(std::move(entries));
}
QJsonObject ActualRecords(const SlkDatasetTable& records) {
    QJsonObject entries;
    for (const auto& [key, record] : records) entries.insert(StringKey(key), ActualFields(record.fields));
    return Table(std::move(entries));
}
QJsonObject ActualScope(const SlkDatasetMetadataScope& scope) {
    QJsonObject entries;
    for (const auto& [key, property] : scope.properties) entries.insert(StringKey(key), ActualValue(property.value));
    for (const auto& [key, record] : scope.fields) {
        Require(!entries.contains(StringKey(key)), "C++ metadata has a property/child collision");
        entries.insert(StringKey(key), ActualFields(record.fields));
    }
    return Table(std::move(entries));
}
// 仅规范化 golden 的表项顺序/浮点显示字符串；值与类型完全来自独立 JSON。
QJsonObject Expected(const QJsonObject& node) {
    const auto kind = JsonString(node, "kind");
    if (kind == "integer") return {{"kind", kind}, {"decimal", QString::fromStdString(Decimal(node))}};
    if (kind == "float") {
        const auto bytes = DecodeHex(JsonString(node, "ieee754le"));
        Require(bytes.size() == 8, "golden double must have exactly eight bytes");
        return {{"kind", kind}, {"ieee754le", QString::fromLatin1(bytes.toHex())}};
    }
    if (kind == "string") {
        const auto bytes = DecodeHex(JsonString(node, "hex"));
        Require(node.value("byteCount").isDouble() && node.value("byteCount").toInteger(-1) == bytes.size(),
            "golden string byte count differs from hex");
        return {{"kind", kind}, {"byteCount", bytes.size()}, {"hex", QString::fromLatin1(bytes.toHex())}};
    }
    if (kind == "boolean") {
        Require(node.value("value").isBool(), "golden boolean is not JSON bool");
        return {{"kind", kind}, {"value", node.value("value")}};
    }
    Require(kind == "table" && node.value("entries").isArray(), "unsupported golden node kind");
    const auto items = node.value("entries").toArray();
    Require(node.value("count").isDouble() && node.value("count").toInteger(-1) == items.size(),
        "golden table count differs from entry count");
    QJsonObject entries;
    for (const auto& item : items) {
        Require(item.isObject(), "golden table entry is not an object");
        const auto entry = item.toObject();
        Require(entry.value("key").isObject() && entry.value("value").isObject(), "golden table entry missing key/value");
        const auto key = entry.value("key").toObject();
        const auto keyKind = JsonString(key, "kind");
        QString canonical;
        if (keyKind == "string") {
            (void)Expected(key);
            canonical = QStringLiteral("s:") + QString::fromLatin1(DecodeHex(JsonString(key, "hex")).toHex());
        } else {
            Require(keyKind == "integer", "supported golden key is not string/integer");
            canonical = QStringLiteral("i:") + QString::fromStdString(Decimal(key));
        }
        Require(!entries.contains(canonical), "duplicate golden typed key");
        entries.insert(canonical, Expected(entry.value("value").toObject()));
    }
    return Table(std::move(entries));
}
void Equal(const QJsonObject& actual, const QJsonObject& expected, const std::string& id) {
    if (actual != expected) throw std::runtime_error(id + ": typed mismatch\nexpected=" +
        QJsonDocument(expected).toJson(QJsonDocument::Compact).toStdString() + "\nactual=" +
        QJsonDocument(actual).toJson(QJsonDocument::Compact).toStdString());
}
std::string Metadata(bool includeAbility = true) {
    std::string text;
    for (const auto type : kTypes) if (includeAbility || type != "ability")
        text += "[" + std::string(type) + "]\n[.name]\nfield=Name\nid=Name\nkey=name\ntype=3\nprofile=true\n";
    return text;
}
enum class Entry { Unit, Config, Metadata };
struct Fixture {
    std::filesystem::path core = std::filesystem::current_path() / "oracle-memory-core";
    std::filesystem::path installed = std::filesystem::current_path() / "oracle-memory-installed";
    std::map<std::filesystem::path, std::vector<std::byte>> files;
    std::vector<std::filesystem::path> requested;
    std::vector<std::byte> w3i;
    Fixture() {
        auto settings = MapData::MapSettings::Parse(MapDataFixtures::FullMapSettingsV25().bytes);
        Require(settings.Succeeded(), "original W3I fixture parse failed");
        settings.value->gameDataSet = 1;
        auto encoded = settings.value->Serialize(); Require(encoded.Succeeded(), "original W3I fixture serialize failed");
        w3i = std::move(*encoded.value);
        files[core / "config.ini"] = Bytes("[global]\ndata=fixture-data\n");
        files[core / "defined/metadata.ini"] = Bytes(Metadata());
        for (const auto type : kFiles) files[Prebuilt(type)] = {};
    }
    std::filesystem::path Prebuilt(std::string_view type) const {
        return installed / "plugin/w3x2lni/data/fixture-data/prebuilt/Custom" / (std::string(type) + ".ini");
    }
    Result<SlkTypedDataset> Read() {
        return ReadSlkTypedDataset({core, installed, w3i, 31}, [this](const auto& path, std::size_t maximum) {
            requested.push_back(path);
            const auto found = files.find(path);
            if (found == files.end() || found->second.size() > maximum)
                return Result<std::vector<std::byte>>::Failure("OracleFixture", "MISSING_OR_LIMIT", "missing or oversized fixture input");
            return Result<std::vector<std::byte>>::Success(found->second);
        });
    }
};
std::string Diagnostics(const Result<SlkTypedDataset>& result) {
    std::string text;
    for (const auto& diagnostic : result.diagnostics) text += diagnostic.code + ":" + diagnostic.message + "; ";
    return text;
}
void SourceReceipt(const SlkTypedDataset& data, const Fixture& fixture, const std::filesystem::path& path) {
    const auto found = std::find_if(data.sources.begin(), data.sources.end(), [&](const auto& item) { return item.path == path; });
    Require(found != data.sources.end() && found->sha256 == Hash(fixture.files.at(path)) &&
        found->byteCount == fixture.files.at(path).size(), "oracle input receipt is not bound to the injected bytes");
}
const std::map<std::string, Entry> kSupported{
    {"decimal_types",Entry::Unit}, {"integer_bounds",Entry::Unit}, {"quoted_strings",Entry::Unit},
    {"dense_arrays",Entry::Unit}, {"metadata_layers",Entry::Metadata}, {"config_bare",Entry::Config},
    {"numeric_section",Entry::Unit}, {"crlf",Entry::Unit}, {"last_line_no_newline",Entry::Unit},
    {"present_empty_file",Entry::Unit}, {"keyword_field_keys",Entry::Unit}, {"negative_fraction",Entry::Unit}};
const std::map<std::string, std::string> kRejected{
    {"bare_inline_comment","VALUE_UNSUPPORTED"}, {"metadata_inline_comment","VALUE_UNSUPPORTED"},
    {"numeric_key","SYNTAX"}, {"numeric_prefix_key","SYNTAX"}, {"inheritance","SYNTAX"},
    {"dotted_section","SYNTAX"}, {"quoted_section","SYNTAX"}, {"angle_section","SYNTAX"},
    {"duplicate_section","DUPLICATE"}, {"duplicate_field","DUPLICATE"}, {"scope_child_collision","DUPLICATE"},
    {"missing_value","VALUE_UNSUPPORTED"}, {"nil_field","VALUE_UNSUPPORTED"},
    {"integer_overflow_to_float","VALUE_UNSUPPORTED"}, {"float_underflow","VALUE_UNSUPPORTED"},
    {"float_overflow","VALUE_UNSUPPORTED"}, {"hexadecimal","VALUE_UNSUPPORTED"},
    {"decimal_escape","VALUE_UNSUPPORTED"}, {"unknown_escape","VALUE_UNSUPPORTED"},
    {"trailing_comma","VALUE_UNSUPPORTED"}, {"nested_array","VALUE_UNSUPPORTED"}, {"sparse_table","VALUE_UNSUPPORTED"},
    {"whole_line_comment","SYNTAX"}, {"long_string","VALUE_UNSUPPORTED"}, {"numeric_expression","VALUE_UNSUPPORTED"},
    {"call_is_bare_data","VALUE_UNSUPPORTED"}, {"unquoted_prebuilt_string","VALUE_UNSUPPORTED"},
    {"bare_nan_is_string","VALUE_UNSUPPORTED"}};

void Run(const QJsonObject& golden) {
    Require(JsonString(golden,"schema") == "slk-lni-typed-golden-v1" && golden.value("cases").isArray(), "wrong oracle fixture schema");
    Require(DecodeHex(JsonString(golden,"targetLniSha256")).size() == 32 &&
        DecodeHex(JsonString(golden,"fixtureSha256")).size() == 32, "oracle provenance digest missing");
    Fixture baseline; const auto background = baseline.Read();
    Require(background.Succeeded() && background.diagnostics.empty(), "background dataset fixture failed: " + Diagnostics(background));
    std::set<std::string> seen;
    std::size_t typedCount = 0, configCount = 0, rejectedCount = 0;
    for (const auto& value : golden.value("cases").toArray()) {
        Require(value.isObject(), "oracle case is not an object");
        const auto item = value.toObject();
        const auto id = JsonString(item,"caseId").toStdString();
        Require(seen.insert(id).second, "duplicate oracle case: " + id);
        const auto inputBytes = DecodeHex(JsonString(item,"inputHex"));
        const std::string input(inputBytes.constData(), static_cast<std::size_t>(inputBytes.size()));
        Fixture fixture;
        auto path = fixture.Prebuilt("unit");
        std::size_t inputOffset = 0;
        const auto policy = JsonString(item,"cppPolicy");
        if (policy == "supported") {
            Require(kSupported.contains(id), "unrouted supported case: " + id);
            Require(item.value("parseOk").isBool() && item.value("parseOk").toBool() && item.value("typed").isObject(),
                id + ": supported oracle did not return a typed tree");
            const auto entry = kSupported.at(id);
            if (entry == Entry::Config) path = fixture.core / "config.ini";
            if (entry == Entry::Metadata) path = fixture.core / "defined/metadata.ini";
            const std::string prefix = entry == Entry::Metadata ? Metadata(false) : "";
            fixture.files[path] = Bytes(prefix + input);
            const auto result = fixture.Read();
            Require(result.Succeeded() && result.diagnostics.empty(), id + ": supported input rejected: " + Diagnostics(result));
            SourceReceipt(*result.value, fixture, path);
            Require(fixture.requested.size() == 11 && result.value->sources.size() == 11,
                id + ": did not read the complete dedicated dataset");
            const auto expected = Expected(item.value("typed").toObject());
            if (entry == Entry::Config) {
                // API 不公开 data_ui/data_meta；只比较实际公开的 dataSetId，不填造这两个字段。
                const auto global = expected.value("values").toObject().value(StringKey("global")).toObject();
                const auto expectedData = global.value("values").toObject().value(StringKey("data")).toObject();
                Equal(ActualScalar(SlkSessionScalar{result.value->dataSetId}), expectedData, id + "/global/data");
                Require(result.value->dataSetId == "fixture-data" && result.value->family == SlkDatasetFamily::Custom,
                    id + ": config did not select the fixture data paths");
                ++configCount;
            } else if (entry == Entry::Metadata) {
                QJsonObject scopes;
                scopes.insert(StringKey("ability"), ActualScope(result.value->metadata.common[1]));
                for (const auto& [key, scope] : result.value->metadata.byCode)
                    scopes.insert(StringKey(key), ActualScope(scope));
                Equal(Table(std::move(scopes)), expected, id); ++typedCount;
            } else {
                Equal(ActualRecords(result.value->defaults[0]), expected, id); ++typedCount;
            }
        } else {
            Require(policy == "must-reject" && kRejected.contains(id), "unrouted rejection case: " + id);
            path = fixture.Prebuilt("txt"); // 无四字节对象 ID 门槛，避免形状错误掩盖语法/值错误。
            std::string prefix;
            if (id == "bare_inline_comment") path = fixture.core / "config.ini";
            if (id == "metadata_inline_comment" || id == "scope_child_collision") {
                path = fixture.core / "defined/metadata.ini";
                prefix = Metadata(false);
            }
            inputOffset = prefix.size();
            fixture.files[path] = Bytes(prefix + input);
            const auto result = fixture.Read();
            Require(!result.Succeeded() && !result.value && !result.diagnostics.empty(), id + ": unsupported case was accepted or returned partial data");
            const auto& diagnostic = result.diagnostics.front();
            Require(diagnostic.code == "SLK_DATASET_" + kRejected.at(id), id + ": wrong rejection gate: " + Diagnostics(result));
            Require(diagnostic.origin && diagnostic.origin->source == path && diagnostic.origin->fingerprint == Hash(fixture.files.at(path)) &&
                diagnostic.origin->offset >= inputOffset && diagnostic.origin->offset <= inputOffset + input.size(),
                id + ": rejection came from background data or lost the oracle byte identity");
            ++rejectedCount;
        }
    }
    Require(seen.size() == kSupported.size() + kRejected.size() && typedCount == 11 && configCount == 1 && rejectedCount == 28,
        "oracle coverage incomplete: every fixed case must be routed explicitly");
    std::cout << "SLK LNI oracle: 11 full typed trees, 1 config selection/source receipt, 28 exact-gate rejections passed.\n"
        << "Coverage: config_bare data_ui/data_meta are not exposed by SlkTypedDataset; their typed values are not compared.\n";
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QFile file(QString::fromUtf8(SLK_LNI_ORACLE_EXPECTED_PATH));
        Require(file.open(QIODevice::ReadOnly) && file.size() <= 1024 * 1024, "fixed oracle JSON could not be opened within budget");
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        Require(error.error == QJsonParseError::NoError && document.isObject(), "fixed oracle JSON parse failed");
        Run(document.object());
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SLK LNI oracle mismatch: " << error.what() << '\n';
        return 1;
    }
}
