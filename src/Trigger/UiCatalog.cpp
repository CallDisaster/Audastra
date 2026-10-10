#include "War3MapCore/Text/TextUtil.h"
#include "War3MapCore/Trigger/UiCatalog.h"

#include "War3MapCore/Game/WorldEditStrings.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/TextUtil.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace War3Map::Trigger {
namespace {

using ByteVector = std::vector<std::byte>;

struct DecodedText {
    std::string text;
    TextEncoding encoding = TextEncoding::Utf8;
};

std::string PathUtf8(const std::filesystem::path& path) {
    const std::wstring value = path.wstring();
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("cannot convert filesystem path to UTF-8");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("filesystem path conversion returned an inconsistent length");
    }
    return result;
}

std::wstring BytesToWide(std::span<const std::byte> bytes, UINT codePage, DWORD flags) {
    if (bytes.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(
        codePage, flags, reinterpret_cast<const char*>(bytes.data()),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0) {
        throw std::runtime_error("text decode failed");
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            codePage, flags, reinterpret_cast<const char*>(bytes.data()),
            static_cast<int>(bytes.size()), result.data(), size) != size) {
        throw std::runtime_error("text decode returned an inconsistent length");
    }
    return result;
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        throw std::runtime_error("UTF-8 encode failed");
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), size, nullptr, nullptr) != size) {
        throw std::runtime_error("UTF-8 encode returned an inconsistent length");
    }
    return result;
}

DecodedText DecodeText(std::span<const std::byte> input) {
    std::span<const std::byte> payload = input;
    bool bom = false;
    if (payload.size() >= 3 &&
        std::to_integer<unsigned char>(payload[0]) == 0xef &&
        std::to_integer<unsigned char>(payload[1]) == 0xbb &&
        std::to_integer<unsigned char>(payload[2]) == 0xbf) {
        payload = payload.subspan(3);
        bom = true;
    }
    try {
        return {
            WideToUtf8(BytesToWide(payload, CP_UTF8, MB_ERR_INVALID_CHARS)),
            bom ? TextEncoding::Utf8Bom : TextEncoding::Utf8,
        };
    } catch (const std::runtime_error&) {
    }
    try {
        return {
            WideToUtf8(BytesToWide(payload, 54936, MB_ERR_INVALID_CHARS)),
            TextEncoding::Gb18030,
        };
    } catch (const std::runtime_error&) {
    }

    std::wstring latin1;
    latin1.reserve(payload.size());
    for (const std::byte value : payload) {
        latin1.push_back(static_cast<wchar_t>(std::to_integer<unsigned char>(value)));
    }
    return {WideToUtf8(latin1), TextEncoding::Latin1};
}

std::string Unquote(std::string value) {
    if (value.size() < 2 || value.front() != value.back() ||
        (value.front() != '\'' && value.front() != '"')) {
        return value;
    }
    const char quote = value.front();
    value = value.substr(1, value.size() - 2);
    std::string result;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '\\' && index + 1 < value.size() &&
            (value[index + 1] == quote || value[index + 1] == '\\')) {
            ++index;
        }
        result.push_back(value[index]);
    }
    return result;
}



bool IsSafeRelativePath(const std::filesystem::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_directory() || path.has_root_name()) {
        return false;
    }
    for (const auto& component : path) {
        if (component == L"..") {
            return false;
        }
    }
    return true;
}

ByteVector ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open " + PathUtf8(path));
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff length = stream.tellg();
    if (length < 0) {
        throw std::runtime_error("cannot determine size of " + PathUtf8(path));
    }
    stream.seekg(0, std::ios::beg);
    ByteVector result(static_cast<std::size_t>(length));
    if (!result.empty()) {
        stream.read(reinterpret_cast<char*>(result.data()), length);
        if (!stream) {
            throw std::runtime_error("cannot read complete file " + PathUtf8(path));
        }
    }
    return result;
}

void AppendDigestInput(ByteVector& aggregate, std::string_view path, std::span<const std::byte> bytes) {
    aggregate.insert(
        aggregate.end(),
        reinterpret_cast<const std::byte*>(path.data()),
        reinterpret_cast<const std::byte*>(path.data() + path.size()));
    aggregate.push_back(std::byte{0});
    aggregate.insert(aggregate.end(), bytes.begin(), bytes.end());
}

UiDefinitionKind KindFromFile(const std::filesystem::path& path) {
    const std::string stem = Text::LowerCType(PathUtf8(path.stem()));
    if (stem == "define") return UiDefinitionKind::Define;
    if (stem == "event") return UiDefinitionKind::Event;
    if (stem == "condition") return UiDefinitionKind::Condition;
    if (stem == "action") return UiDefinitionKind::Action;
    if (stem == "call") return UiDefinitionKind::Call;
    return UiDefinitionKind::Unknown;
}

std::vector<std::string> ConfigEntries(const UiTextDocument& document) {
    std::vector<std::string> result;
    for (const UiTextLine& line : document.Lines()) {
        if (line.kind == UiTextLineKind::Empty || line.kind == UiTextLineKind::Comment) {
            continue;
        }
        std::string value = War3Map::Text::TrimCWhitespaceCopy(line.text);
        std::replace(value.begin(), value.end(), '\\', '/');
        if (!value.empty()) {
            result.push_back(std::move(value));
        }
    }
    return result;
}

std::vector<UiDefinitionRevision> ParseDefinitions(
    const UiTextDocument& document,
    UiDefinitionKind kind,
    const std::string& layer,
    const std::filesystem::path& source,
    const std::string& sha256) {
    std::vector<UiDefinitionRevision> result;
    if (kind == UiDefinitionKind::Unknown) {
        return result;
    }

    if (kind == UiDefinitionKind::Define) {
        std::string group;
        for (const UiTextLine& line : document.Lines()) {
            if (line.kind == UiTextLineKind::Section) {
                group = line.section;
                continue;
            }
            if (line.kind != UiTextLineKind::Assignment || group.empty()) {
                continue;
            }
            UiDefinitionRevision revision;
            revision.kind = kind;
            revision.group = group;
            revision.name = line.key;
            revision.layer = layer;
            revision.source = source;
            revision.sourceSha256 = sha256;
            revision.fields.emplace_back("value", line.value);
            result.push_back(std::move(revision));
        }
        return result;
    }

    UiDefinitionRevision* current = nullptr;
    UiArgumentDefinition* argument = nullptr;
    for (const UiTextLine& line : document.Lines()) {
        if (line.kind == UiTextLineKind::Section) {
            UiDefinitionRevision revision;
            revision.kind = kind;
            revision.name = line.section;
            revision.layer = layer;
            revision.source = source;
            revision.sourceSha256 = sha256;
            result.push_back(std::move(revision));
            current = &result.back();
            argument = nullptr;
            continue;
        }
        if (line.kind == UiTextLineKind::ArgumentSection) {
            if (current != nullptr) {
                current->arguments.emplace_back();
                argument = &current->arguments.back();
            }
            continue;
        }
        if (line.kind != UiTextLineKind::Assignment || current == nullptr) {
            continue;
        }
        if (argument != nullptr) {
            argument->fields.emplace_back(line.key, line.value);
            if (line.key == "type") {
                argument->type = line.value;
            } else if (line.key == "default") {
                argument->defaultValue = line.value;
            }
        } else {
            current->fields.emplace_back(line.key, line.value);
        }
    }
    return result;
}

// 旧版 Warcraft/YDWE 使用 TriggerData.txt + TriggerStrings.txt，而不是
// 当前 UI 层使用的 action.txt/call.txt。这里把旧格式转换成同一个只读
// UiDefinition IR；原始文件仍然保留在 Catalog manifest 中，不会被改写。
std::vector<std::string> SplitLegacyCsv(std::string_view value) {
    std::vector<std::string> result;
    std::string current;
    std::vector<bool> quotedCharacters;
    char quote = '\0';
    const auto append = [&](char character, bool quoted) {
        current.push_back(character);
        quotedCharacters.push_back(quoted);
    };
    const auto finish = [&] {
        std::size_t begin = 0;
        std::size_t end = current.size();
        while (begin < end && !quotedCharacters[begin] &&
               std::isspace(static_cast<unsigned char>(current[begin])) != 0) {
            ++begin;
        }
        while (end > begin && !quotedCharacters[end - 1] &&
               std::isspace(static_cast<unsigned char>(current[end - 1])) != 0) {
            --end;
        }
        result.push_back(current.substr(begin, end - begin));
        current.clear();
        quotedCharacters.clear();
    };
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (quote != '\0') {
            if (character == '\\' && index + 1 < value.size()) {
                append(value[++index], true);
                continue;
            }
            if (character == quote) {
                quote = '\0';
                continue;
            }
            append(character, true);
            continue;
        }
        if (character == '\'' || character == '"') {
            quote = character;
        } else if (character == ',') {
            finish();
        } else {
            append(character, false);
        }
    }
    finish();
    return result;
}

std::string LegacyStringFormat(std::string_view value) {
    std::string result;
    for (const std::string& part : SplitLegacyCsv(value)) {
        if (part.size() > 1 && part.front() == '~') {
            result.append("${");
            result.append(part.substr(1));
            result.push_back('}');
        } else {
            result.append(part);
        }
    }
    return result;
}

std::optional<UiDefinitionKind> LegacyDataKind(std::string_view section) {
    const std::string normalized = Text::LowerCType(std::string(section));
    if (normalized == "triggertypes") return UiDefinitionKind::Define;
    if (normalized == "triggerparams") return UiDefinitionKind::Define;
    if (normalized == "triggercategories") return UiDefinitionKind::Define;
    if (normalized == "triggerevents") return UiDefinitionKind::Event;
    if (normalized == "triggerconditions") return UiDefinitionKind::Condition;
    if (normalized == "triggeractions") return UiDefinitionKind::Action;
    if (normalized == "triggercalls") return UiDefinitionKind::Call;
    return std::nullopt;
}

std::optional<UiDefinitionKind> LegacyStringsKind(std::string_view section) {
    const std::string normalized = Text::LowerCType(std::string(section));
    if (normalized == "triggereventstrings") return UiDefinitionKind::Event;
    if (normalized == "triggerconditionstrings") return UiDefinitionKind::Condition;
    if (normalized == "triggeractionstrings") return UiDefinitionKind::Action;
    if (normalized == "triggercallstrings") return UiDefinitionKind::Call;
    return std::nullopt;
}

bool IsLegacyTriggerData(const std::filesystem::path& path) {
    return Text::LowerCType(PathUtf8(path.filename())) == "triggerdata.txt";
}

bool IsLegacyTriggerStrings(const std::filesystem::path& path) {
    return Text::LowerCType(PathUtf8(path.filename())) == "triggerstrings.txt";
}

void SetUiField(UiDefinitionRevision& revision, std::string key, std::string value) {
    const auto found = std::find_if(
        revision.fields.begin(), revision.fields.end(),
        [&key](const auto& field) { return Text::LowerCType(field.first) == Text::LowerCType(key); });
    if (found == revision.fields.end()) {
        revision.fields.emplace_back(std::move(key), std::move(value));
    } else {
        found->second = std::move(value);
    }
}

struct LegacyDefinitionRecord {
    UiDefinitionKind kind = UiDefinitionKind::Unknown;
    std::string section;
    std::string name;
    std::string value;
    std::unordered_map<std::string, std::string> metadata;
};

std::vector<UiDefinitionRevision> ParseLegacyTriggerData(
    const UiTextDocument& document,
    const std::string& layer,
    const std::filesystem::path& source,
    const std::string& sha256) {
    std::vector<LegacyDefinitionRecord> records;
    std::unordered_map<std::string, std::size_t> recordIndexes;
    std::string section;
    for (const UiTextLine& line : document.Lines()) {
        if (line.kind == UiTextLineKind::Section) {
            section = line.section;
            continue;
        }
        if (line.kind != UiTextLineKind::Assignment || section.empty()) {
            continue;
        }
        const auto kind = LegacyDataKind(section);
        if (!kind.has_value()) {
            continue;
        }
        if (line.key.starts_with('_')) {
            // 函数名本身通常也含下划线（例如 DzAPI_Map_*），所以元数据
            // 分隔符必须取最后一个下划线，而不是第一个。
            const std::size_t delimiter = line.key.rfind('_');
            if (delimiter > 1 && delimiter + 1 < line.key.size()) {
                const std::string name = line.key.substr(1, delimiter - 1);
                const std::string metadata = line.key.substr(delimiter + 1);
                std::string recordKey = section;
                recordKey.push_back('\0');
                recordKey.append(name);
                const auto index = recordIndexes.find(recordKey);
                if (index != recordIndexes.end()) {
                    records[index->second].metadata[metadata] = line.value;
                } else {
                    // TriggerData 中元数据通常跟在主定义后面，但旧扩展并不
                    // 保证顺序；先建立占位记录，后续主定义会复用它。
                    const std::size_t placeholder = records.size();
                    records.push_back(LegacyDefinitionRecord{
                        *kind, section, name, {}, {{metadata, line.value}}});
                    recordIndexes.emplace(std::move(recordKey), placeholder);
                }
            }
            continue;
        }

        std::string key = section;
        key.push_back('\0');
        key.append(line.key);
        const auto existing = recordIndexes.find(key);
        if (existing == recordIndexes.end()) {
            const std::size_t index = records.size();
            records.push_back(LegacyDefinitionRecord{
                *kind, section, line.key, line.value, {}});
            recordIndexes.emplace(key, index);
        } else {
            records[existing->second].value = line.value;
            records[existing->second].kind = *kind;
        }
    }

    std::vector<UiDefinitionRevision> result;
    for (const LegacyDefinitionRecord& record : records) {
        if (record.name.empty()) {
            continue;
        }
        UiDefinitionRevision revision;
        revision.kind = record.kind;
        revision.name = record.name;
        revision.layer = layer;
        revision.source = source;
        revision.sourceSha256 = sha256;
        SetUiField(revision, "legacy_format", "TriggerData");

        if (record.kind == UiDefinitionKind::Define) {
            revision.group = record.section;
            SetUiField(revision, "value", record.value);
        } else {
            const std::vector<std::string> columns = SplitLegacyCsv(record.value);
            std::size_t argumentOffset = 1;
            if (record.kind == UiDefinitionKind::Call) {
                argumentOffset = 3;
                if (columns.size() > 2) {
                    SetUiField(revision, "returns", columns[2]);
                }
            } else if (record.kind == UiDefinitionKind::Condition) {
                // 经典 TriggerConditions 仅首列是版本/标记，后续全部为形参。
                // 条件隐含返回 boolean，不使用 TriggerCalls 的第三列返回类型布局。
                SetUiField(revision, "returns", "boolean");
            }
            if (!record.value.empty() && columns.size() >= argumentOffset) {
                for (std::size_t index = argumentOffset; index < columns.size(); ++index) {
                    UiArgumentDefinition argument;
                    argument.type = columns[index].empty() ? "nothing" : columns[index];
                    revision.arguments.push_back(std::move(argument));
                }
            }
        }

        for (const auto& [metadata, value] : record.metadata) {
            if (metadata == "Defaults") {
                const std::vector<std::string> defaults = SplitLegacyCsv(value);
                for (std::size_t index = 0;
                     index < defaults.size() && index < revision.arguments.size(); ++index) {
                    revision.arguments[index].defaultValue = defaults[index];
                }
                SetUiField(revision, "defaults", value);
            } else if (metadata == "ScriptName") {
                SetUiField(revision, "script_name", value);
            } else if (metadata == "Category") {
                SetUiField(revision, "category", value);
            } else {
                SetUiField(revision, Text::LowerCType(metadata), value);
            }
        }
        result.push_back(std::move(revision));
    }
    return result;
}

struct LegacyStringUpdate {
    UiDefinitionKind kind = UiDefinitionKind::Unknown;
    std::string name;
    std::string field;
    std::string value;
};

std::vector<LegacyStringUpdate> ParseLegacyTriggerStrings(
    const UiTextDocument& document) {
    std::vector<LegacyStringUpdate> result;
    std::string section;
    std::unordered_map<std::string, std::size_t> titleCounts;
    for (const UiTextLine& line : document.Lines()) {
        if (line.kind == UiTextLineKind::Section) {
            section = line.section;
            continue;
        }
        if (line.kind != UiTextLineKind::Assignment || section.empty() ||
            line.key.starts_with('_')) {
            continue;
        }
        const auto kind = LegacyStringsKind(section);
        if (!kind.has_value()) {
            continue;
        }
        std::string name = line.key;
        std::string field = "title";
        if (name.size() > 5 && name.ends_with("Hint")) {
            name.resize(name.size() - 5);
            field = "hint";
        } else {
            std::string countKey = std::to_string(static_cast<int>(*kind));
            countKey.push_back('\0');
            countKey.append(name);
            const std::size_t count = titleCounts[countKey]++;
            field = count == 0 ? "title" : (count == 1 ? "description" :
                "legacy_text_" + std::to_string(count));
        }
        result.push_back(LegacyStringUpdate{
            *kind, std::move(name), std::move(field), LegacyStringFormat(line.value)});
    }
    return result;
}

Diagnostic MakeDiagnostic(
    DiagnosticSeverity severity,
    std::string code,
    std::string message,
    const std::filesystem::path& source = {}) {
    Diagnostic diagnostic{
        severity,
        "Trigger.UiCatalog",
        std::move(code),
        std::move(message),
        std::nullopt,
    };
    if (!source.empty()) {
        diagnostic.origin = OriginSpan{source, 0, 0, {}};
    }
    return diagnostic;
}

} // namespace

Result<UiTextDocument> UiTextDocument::Parse(
    std::filesystem::path source,
    std::span<const std::byte> bytes) {
    Result<UiTextDocument> result;
    UiTextDocument document;
    document.source_ = std::move(source);
    document.originalBytes_.assign(bytes.begin(), bytes.end());

    try {
        const DecodedText decoded = DecodeText(bytes);
        document.encoding_ = decoded.encoding;
        std::size_t start = 0;
        std::size_t lineNumber = 0;
        while (start <= decoded.text.size()) {
            ++lineNumber;
            const std::size_t newline = decoded.text.find_first_of("\r\n", start);
            const std::size_t end = newline == std::string::npos ? decoded.text.size() : newline;
            UiTextLine line;
            line.lineNumber = lineNumber;
            line.text = decoded.text.substr(start, end - start);
            if (newline != std::string::npos) {
                if (decoded.text[newline] == '\r' && newline + 1 < decoded.text.size() &&
                    decoded.text[newline + 1] == '\n') {
                    line.lineEnding = "\r\n";
                    start = newline + 2;
                } else {
                    line.lineEnding.assign(1, decoded.text[newline]);
                    start = newline + 1;
                }
            } else {
                start = decoded.text.size() + 1;
            }

            const std::string trimmed = War3Map::Text::TrimCWhitespaceCopy(line.text);
            if (trimmed.empty()) {
                line.kind = UiTextLineKind::Empty;
            } else if (trimmed.starts_with("//") || trimmed.starts_with('#') ||
                trimmed.starts_with(';')) {
                line.kind = UiTextLineKind::Comment;
            } else if (trimmed == "[[.args]]") {
                line.kind = UiTextLineKind::ArgumentSection;
            } else if (trimmed.size() >= 2 && trimmed.front() == '[' && trimmed.back() == ']') {
                line.kind = UiTextLineKind::Section;
                line.section = Unquote(War3Map::Text::TrimCWhitespaceCopy(trimmed.substr(1, trimmed.size() - 2)));
            } else {
                const std::size_t equal = trimmed.find('=');
                if (equal == std::string::npos) {
                    line.kind = UiTextLineKind::Other;
                } else {
                    line.kind = UiTextLineKind::Assignment;
                    line.key = War3Map::Text::TrimCWhitespaceCopy(trimmed.substr(0, equal));
                    line.value = Unquote(War3Map::Text::TrimCWhitespaceCopy(trimmed.substr(equal + 1)));
                }
            }
            document.lines_.push_back(std::move(line));
        }
        if (document.encoding_ == TextEncoding::Latin1 && !bytes.empty()) {
            result.diagnostics.push_back(MakeDiagnostic(
                DiagnosticSeverity::Warning,
                "ui.encoding.latin1-fallback",
                "text was neither strict UTF-8 nor GB18030; Latin-1 fallback was used",
                document.source_));
        }
        result.value.emplace(std::move(document));
    } catch (const std::exception& error) {
        result.diagnostics.push_back(MakeDiagnostic(
            DiagnosticSeverity::Error,
            "ui.text.parse-failed",
            error.what(),
            document.source_));
    }
    return result;
}

const std::filesystem::path& UiTextDocument::Source() const noexcept {
    return source_;
}

TextEncoding UiTextDocument::Encoding() const noexcept {
    return encoding_;
}

const std::vector<UiTextLine>& UiTextDocument::Lines() const noexcept {
    return lines_;
}

std::span<const std::byte> UiTextDocument::OriginalBytes() const noexcept {
    return originalBytes_;
}

std::vector<std::byte> UiTextDocument::SerializeLossless() const {
    return originalBytes_;
}

const UiDefinitionRevision& UiDefinition::Active() const {
    if (revisions.empty()) {
        throw std::logic_error("UI definition has no revisions");
    }
    return revisions.back();
}

std::size_t UiDefinition::ArgumentCount() const {
    return static_cast<std::size_t>(std::count_if(
        Active().arguments.begin(), Active().arguments.end(), [](const UiArgumentDefinition& argument) {
            return argument.type != "nothing";
        }));
}

std::string UiDefinition::Signature() const {
    std::string result(ToString(kind));
    result.push_back(':');
    if (!group.empty()) {
        result.append(group);
        result.push_back('/');
    }
    result.append(name);
    result.push_back('(');
    bool first = true;
    for (const UiArgumentDefinition& argument : Active().arguments) {
        if (!first) {
            result.push_back(',');
        }
        result.append(argument.type);
        first = false;
    }
    result.push_back(')');
    return result;
}

std::string UiCatalog::DefinitionKey(
    UiDefinitionKind kind,
    std::string_view group,
    std::string_view name) {
    std::string result(ToString(kind));
    result.push_back('\0');
    result.append(group);
    result.push_back('\0');
    result.append(name);
    return result;
}

std::filesystem::path UiCatalog::ResolveUiRoot(const std::filesystem::path& toolRoot) {
    std::filesystem::path uiRoot = std::filesystem::absolute(toolRoot).lexically_normal();
    if (std::filesystem::is_regular_file(uiRoot / "ui" / "config")) {
        uiRoot /= "ui";
    } else if (!std::filesystem::is_regular_file(uiRoot / "config") &&
        std::filesystem::is_regular_file(uiRoot / "share" / "mpq" / "config")) {
        // 旧版雪月/YDWE 将同一层配置放在 share/mpq；仍保留 ui 与直接数据根的优先级。
        uiRoot /= "share";
        uiRoot /= "mpq";
    }
    return uiRoot;
}

Result<UiCatalog> UiCatalog::Load(const std::filesystem::path& toolRoot) {
    try {
        const std::filesystem::path uiRoot = ResolveUiRoot(toolRoot);
        const std::filesystem::path configPath = uiRoot / "config";
        if (!std::filesystem::is_regular_file(configPath)) {
            return Result<UiCatalog>::Failure(
                "Trigger.UiCatalog",
                "ui.config.missing",
                "UI config was not found below " + PathUtf8(toolRoot));
        }
        return LoadFromConfig(uiRoot, ReadFile(configPath));
    } catch (const std::exception& error) {
        return Result<UiCatalog>::Failure("Trigger.UiCatalog", "ui.catalog.load-failed", error.what());
    }
}

Result<std::vector<std::string>> UiCatalog::ReadConfigLayers(const std::filesystem::path& toolRoot) {
    try {
        const std::filesystem::path configPath = ResolveUiRoot(toolRoot) / "config";
        if (!std::filesystem::is_regular_file(configPath)) {
            return Result<std::vector<std::string>>::Failure(
                "Trigger.UiCatalog", "ui.config.missing", "UI config was not found below " + PathUtf8(toolRoot));
        }
        auto parsed = UiTextDocument::Parse("ui/config", ReadFile(configPath));
        if (!parsed.Succeeded()) {
            Result<std::vector<std::string>> failure;
            failure.diagnostics = std::move(parsed.diagnostics);
            return failure;
        }
        return Result<std::vector<std::string>>::Success(ConfigEntries(*parsed.value));
    } catch (const std::exception& error) {
        return Result<std::vector<std::string>>::Failure("Trigger.UiCatalog", "ui.config.unreadable", error.what());
    }
}

Result<UiCatalog> UiCatalog::LoadLayers(
    const std::filesystem::path& toolRoot, std::span<const std::string> layers) {
    // 按调用方给出的层序合成一份 config 字节，只在内存中使用，不写回任何 config。
    std::string config;
    for (const std::string& layer : layers) {
        if (layer.find_first_of("\r\n") != std::string::npos) {
            return Result<UiCatalog>::Failure(
                "Trigger.UiCatalog", "ui.layer.invalid-name", "UI layer name contains a line break");
        }
        config += layer;
        config += "\r\n";
    }
    try {
        std::filesystem::path uiRoot = ResolveUiRoot(toolRoot);
        if (!std::filesystem::is_regular_file(uiRoot / "config") && std::filesystem::is_directory(uiRoot / "ui")) {
            uiRoot /= "ui";
        }
        return LoadFromConfig(uiRoot, ByteVector(
            reinterpret_cast<const std::byte*>(config.data()),
            reinterpret_cast<const std::byte*>(config.data() + config.size())));
    } catch (const std::exception& error) {
        return Result<UiCatalog>::Failure("Trigger.UiCatalog", "ui.catalog.load-failed", error.what());
    }
}

Result<UiCatalog> UiCatalog::LoadFromConfig(
    const std::filesystem::path& uiRoot, const std::vector<std::byte>& configBytes) {
    Result<UiCatalog> result;
    UiCatalog catalog;
    try {
        catalog.root_ = uiRoot;

        ByteVector digestInput;
        auto configParsed = UiTextDocument::Parse("ui/config", configBytes);
        result.diagnostics.insert(
            result.diagnostics.end(), configParsed.diagnostics.begin(), configParsed.diagnostics.end());
        if (!configParsed.Succeeded()) {
            return result;
        }
        catalog.configDocument_ = std::move(*configParsed.value);
        catalog.layers_ = ConfigEntries(catalog.configDocument_);
        if (catalog.layers_.empty()) {
            return Result<UiCatalog>::Failure(
                "Trigger.UiCatalog",
                "ui.config.empty",
                "UI config does not enable any layers");
        }
        AppendDigestInput(digestInput, "ui/config", configBytes);
        const auto configHash = War3Map::Sha256(configBytes);
        if (!configHash.Succeeded()) {
            result.diagnostics.insert(
                result.diagnostics.end(), configHash.diagnostics.begin(), configHash.diagnostics.end());
            return result;
        }
        catalog.manifest_.push_back(UiSourceManifestEntry{
            {}, UiDefinitionKind::Unknown, "ui/config", true, true, true,
            configBytes.size(), *configHash.value, 0,
        });

        static constexpr std::array<std::string_view, 5> kExpectedNames{
            "define.txt", "event.txt", "condition.txt", "action.txt", "call.txt"};
        std::unordered_set<std::string> configuredLayers;
        for (const std::string& configuredLayer : catalog.layers_) {
            const std::filesystem::path layerPath = std::filesystem::path(configuredLayer).lexically_normal();
            if (!IsSafeRelativePath(layerPath)) {
                result.diagnostics.push_back(MakeDiagnostic(
                    DiagnosticSeverity::Error,
                    "ui.layer.unsafe-path",
                    "configured UI layer is not a safe relative path: " + configuredLayer,
                    "ui/config"));
                continue;
            }
            const std::string layerKey = Text::LowerCType(PathUtf8(layerPath.generic_wstring()));
            if (!configuredLayers.insert(layerKey).second) {
                result.diagnostics.push_back(MakeDiagnostic(
                    DiagnosticSeverity::Warning,
                    "ui.layer.duplicate",
                    "configured UI layer appears more than once: " + configuredLayer,
                    "ui/config"));
            }

            std::unordered_map<std::string, std::size_t> manifestByRelativePath;
            for (const std::string_view expectedName : kExpectedNames) {
                const std::filesystem::path relative = layerPath / expectedName;
                const std::size_t index = catalog.manifest_.size();
                manifestByRelativePath.emplace(Text::LowerCType(PathUtf8(relative.generic_wstring())), index);
                catalog.manifest_.push_back(UiSourceManifestEntry{
                    configuredLayer,
                    KindFromFile(relative),
                    std::filesystem::path("ui") / relative,
                    true,
                    false,
                    false,
                    0,
                    {},
                    0,
                });
            }

            const std::filesystem::path absoluteLayer = uiRoot / layerPath;
            if (!std::filesystem::is_directory(absoluteLayer)) {
                result.diagnostics.push_back(MakeDiagnostic(
                    DiagnosticSeverity::Warning,
                    "ui.layer.missing",
                    "可选 UI 层目录不存在，已跳过：" + configuredLayer,
                    std::filesystem::path("ui") / layerPath));
                continue;
            }

            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::recursive_directory_iterator(absoluteLayer)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                if (Text::LowerCType(PathUtf8(entry.path().extension())) == ".txt") {
                    files.push_back(entry.path());
                }
            }
            std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
                return left.generic_wstring() < right.generic_wstring();
            });

            // 旧式 TriggerData/TriggerStrings 需要先全部读入，再按数据→
            // 文本的顺序合并，避免文件系统排序或扩展层覆盖顺序影响结果。
            std::vector<std::pair<std::size_t, std::size_t>> legacyDataDocuments;
            std::vector<std::pair<std::size_t, std::size_t>> legacyStringDocuments;

            for (const std::filesystem::path& file : files) {
                const std::filesystem::path relativeToUi =
                    std::filesystem::relative(file, uiRoot).lexically_normal();
                if (!IsSafeRelativePath(relativeToUi)) {
                    result.diagnostics.push_back(MakeDiagnostic(
                        DiagnosticSeverity::Error,
                        "ui.file.outside-root",
                        "UI TXT resolved outside the UI root: " + PathUtf8(file),
                        file));
                    continue;
                }
                const std::string relativeKey = Text::LowerCType(PathUtf8(relativeToUi.generic_wstring()));
                std::size_t manifestIndex = 0;
                const auto expected = manifestByRelativePath.find(relativeKey);
                if (expected == manifestByRelativePath.end()) {
                    manifestIndex = catalog.manifest_.size();
                    catalog.manifest_.push_back(UiSourceManifestEntry{
                        configuredLayer,
                        KindFromFile(file),
                        std::filesystem::path("ui") / relativeToUi,
                        false,
                        false,
                        false,
                        0,
                        {},
                        0,
                    });
                } else {
                    manifestIndex = expected->second;
                }

                UiSourceManifestEntry& manifest = catalog.manifest_[manifestIndex];
                manifest.present = true;
                const ByteVector bytes = ReadFile(file);
                manifest.size = bytes.size();
                const auto fileHash = War3Map::Sha256(bytes);
                if (!fileHash.Succeeded()) {
                    result.diagnostics.insert(
                        result.diagnostics.end(), fileHash.diagnostics.begin(), fileHash.diagnostics.end());
                    continue;
                }
                manifest.sha256 = *fileHash.value;
                const std::string digestPath = PathUtf8((std::filesystem::path("ui") / relativeToUi).generic_wstring());
                AppendDigestInput(digestInput, digestPath, bytes);

                auto parsed = UiTextDocument::Parse(manifest.relativePath, bytes);
                result.diagnostics.insert(
                    result.diagnostics.end(), parsed.diagnostics.begin(), parsed.diagnostics.end());
                if (!parsed.Succeeded()) {
                    result.diagnostics.push_back(MakeDiagnostic(
                        DiagnosticSeverity::Error,
                        "ui.file.not-parsed",
                        "enabled UI TXT could not be parsed: " + PathUtf8(manifest.relativePath),
                        manifest.relativePath));
                    continue;
                }
                manifest.parsed = true;
                catalog.documents_.push_back(std::move(*parsed.value));
                const std::size_t documentIndex = catalog.documents_.size() - 1;
                if (IsLegacyTriggerData(file)) {
                    legacyDataDocuments.emplace_back(documentIndex, manifestIndex);
                } else if (IsLegacyTriggerStrings(file)) {
                    legacyStringDocuments.emplace_back(documentIndex, manifestIndex);
                } else {
                    const auto revisions = ParseDefinitions(
                        catalog.documents_.back(),
                        manifest.kind,
                        configuredLayer,
                        manifest.relativePath,
                        manifest.sha256);
                    manifest.definitionCount = revisions.size();
                    for (UiDefinitionRevision revision : revisions) {
                        const std::string key = DefinitionKey(
                            revision.kind, revision.group, revision.name);
                        UiDefinition& definition = catalog.definitions_[key];
                        definition.kind = revision.kind;
                        definition.group = revision.group;
                        definition.name = revision.name;
                        definition.revisions.push_back(std::move(revision));
                    }
                }
            }

            for (const auto& [documentIndex, manifestIndex] : legacyDataDocuments) {
                UiSourceManifestEntry& manifest = catalog.manifest_[manifestIndex];
                const auto revisions = ParseLegacyTriggerData(
                    catalog.documents_[documentIndex],
                    configuredLayer,
                    manifest.relativePath,
                    manifest.sha256);
                manifest.definitionCount = revisions.size();
                for (UiDefinitionRevision revision : revisions) {
                    const std::string key = DefinitionKey(
                        revision.kind, revision.group, revision.name);
                    UiDefinition& definition = catalog.definitions_[key];
                    definition.kind = revision.kind;
                    definition.group = revision.group;
                    definition.name = revision.name;
                    definition.revisions.push_back(std::move(revision));
                }
            }

            for (const auto& [documentIndex, manifestIndex] : legacyStringDocuments) {
                (void)manifestIndex;
                const auto updates = ParseLegacyTriggerStrings(catalog.documents_[documentIndex]);
                for (const LegacyStringUpdate& update : updates) {
                    const auto definition = catalog.definitions_.find(
                        DefinitionKey(update.kind, {}, update.name));
                    if (definition == catalog.definitions_.end() ||
                        definition->second.revisions.empty() ||
                        definition->second.revisions.back().layer != configuredLayer) {
                        // TriggerStrings 可能保留已经从 TriggerData 移除的旧
                        // API；没有可验证参数签名时不凭字符串虚构定义。
                        continue;
                    }
                    SetUiField(
                        definition->second.revisions.back(), update.field, update.value);
                }
            }

            for (const auto& [relativeKey, index] : manifestByRelativePath) {
                (void)relativeKey;
                const UiSourceManifestEntry& entry = catalog.manifest_[index];
                if (!entry.present) {
                    result.diagnostics.push_back(MakeDiagnostic(
                        DiagnosticSeverity::Info,
                        "ui.file.not-provided",
                        "已启用 UI 层未提供可选的 " +
                            std::string(ToString(entry.kind)) + ".txt，已跳过。",
                        entry.relativePath));
                }
            }
        }

        const auto aggregateHash = War3Map::Sha256(digestInput);
        if (!aggregateHash.Succeeded()) {
            result.diagnostics.insert(
                result.diagnostics.end(), aggregateHash.diagnostics.begin(), aggregateHash.diagnostics.end());
            return result;
        }
        catalog.sha256_ = *aggregateHash.value;
        result.value.emplace(std::move(catalog));
    } catch (const std::exception& error) {
        result.diagnostics.push_back(MakeDiagnostic(
            DiagnosticSeverity::Error,
            "ui.catalog.load-failed",
            error.what(),
            uiRoot));
    }
    return result;
}

const UiDefinition* UiCatalog::Find(std::int32_t ecaType, std::string_view name) const {
    return Find(UiDefinitionKindFromEcaType(ecaType), name);
}

const UiDefinition* UiCatalog::Find(
    UiDefinitionKind kind,
    std::string_view name,
    std::string_view group) const {
    const auto iterator = definitions_.find(DefinitionKey(kind, group, name));
    return iterator == definitions_.end() ? nullptr : &iterator->second;
}

std::vector<const UiDefinition*> UiCatalog::Definitions(
    UiDefinitionKind kind) const {
    std::vector<const UiDefinition*> result;
    result.reserve(definitions_.size());
    for (const auto& [key, definition] : definitions_) {
        (void)key;
        if (definition.kind == kind) {
            result.push_back(&definition);
        }
    }
    std::sort(result.begin(), result.end(), [](const UiDefinition* left, const UiDefinition* right) {
        if (left->group != right->group) {
            return left->group < right->group;
        }
        return left->name < right->name;
    });
    return result;
}

const UiTextDocument& UiCatalog::ConfigDocument() const noexcept {
    return configDocument_;
}

const std::vector<std::string>& UiCatalog::Layers() const noexcept {
    return layers_;
}

const std::vector<UiSourceManifestEntry>& UiCatalog::Manifest() const noexcept {
    return manifest_;
}

const std::vector<UiTextDocument>& UiCatalog::Documents() const noexcept {
    return documents_;
}

const std::string& UiCatalog::Sha256() const noexcept {
    return sha256_;
}

std::size_t UiCatalog::PresentSourceCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        manifest_.begin(), manifest_.end(), [](const UiSourceManifestEntry& entry) {
            return entry.present;
        }));
}

std::size_t UiCatalog::ParsedSourceCount() const noexcept {
    return static_cast<std::size_t>(std::count_if(
        manifest_.begin(), manifest_.end(), [](const UiSourceManifestEntry& entry) {
            return entry.parsed;
        }));
}

void UiCatalog::SetStrings(std::shared_ptr<const Game::WorldEditStrings> strings) noexcept {
    strings_ = std::move(strings);
}

std::string UiCatalog::Localize(std::string_view text) const {
    return strings_ != nullptr ? strings_->Resolve(text) : std::string(text);
}

UiDefinitionKind UiDefinitionKindFromEcaType(std::int32_t type) noexcept {
    switch (type) {
    case 0: return UiDefinitionKind::Event;
    case 1: return UiDefinitionKind::Condition;
    case 2: return UiDefinitionKind::Action;
    case 3: return UiDefinitionKind::Call;
    default: return UiDefinitionKind::Unknown;
    }
}

std::string_view ToString(UiDefinitionKind kind) noexcept {
    switch (kind) {
    case UiDefinitionKind::Define: return "define";
    case UiDefinitionKind::Event: return "event";
    case UiDefinitionKind::Condition: return "condition";
    case UiDefinitionKind::Action: return "action";
    case UiDefinitionKind::Call: return "call";
    case UiDefinitionKind::Unknown: return "unknown";
    }
    return "unknown";
}

} // namespace War3Map::Trigger
