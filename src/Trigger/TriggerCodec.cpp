#include "War3MapCore/Trigger/TriggerCodec.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Text/Utf8Scalar.h"
#include "TriggerBinaryRead.h"
#include "WtgDecoder.h"
#include "TriggerStableIdReservation.h"
#include "TriggerCodecDefinitionLookupMemo.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace War3Map::Trigger {

// 仅 WtgWctCodec::Parse 能创建此对象。TriggerDocument 的普通复制会共享同一份
// const seal，编辑器只能修改文档 IR，不能替换来源身份。
struct TriggerDocument::SourceSeal {
    std::string sourceWtgSha256;
    std::string sourceWctSha256;
    std::string uiCatalogSha256;
    // WCT 的空文本允许 length=0 或 length=1 + NUL；语义相同但原字节不同。
    // 普通槽跟随来源触发器 ID，不能在重排后按槽下标恢复。
    bool globalEmptyTerminated = false;
    std::unordered_set<StableId> emptyTerminatedTriggers;
};

namespace {

using ByteVector = std::vector<std::byte>;
using Detail::FormatError;
using Detail::MissingUiDefinitionError;
using Detail::MakeOrigin;
using Detail::Reader;

class ValidationError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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

ByteVector WideToCodePage(std::wstring_view value, UINT codePage) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        codePage,
        codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : 0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        throw ValidationError("text cannot be represented in its original encoding");
    }
    ByteVector result(static_cast<std::size_t>(size));
    if (WideCharToMultiByte(
            codePage,
            codePage == CP_UTF8 ? WC_ERR_INVALID_CHARS : 0,
            value.data(),
            static_cast<int>(value.size()),
            reinterpret_cast<char*>(result.data()),
            size,
            nullptr,
            nullptr) != size) {
        throw ValidationError("text encoding returned an inconsistent length");
    }
    return result;
}

std::pair<std::string, TextEncoding> DecodeRawText(std::span<const std::byte> raw) {
    try {
        return {WideToUtf8(BytesToWide(raw, CP_UTF8, MB_ERR_INVALID_CHARS)), TextEncoding::Utf8};
    } catch (const std::runtime_error&) {
    }
    try {
        return {WideToUtf8(BytesToWide(raw, 54936, MB_ERR_INVALID_CHARS)), TextEncoding::Gb18030};
    } catch (const std::runtime_error&) {
    }
    std::wstring latin1;
    latin1.reserve(raw.size());
    for (const std::byte value : raw) {
        latin1.push_back(static_cast<wchar_t>(std::to_integer<unsigned char>(value)));
    }
    return {WideToUtf8(latin1), TextEncoding::Latin1};
}

RawText MakeRawText(std::span<const std::byte> raw) {
    const auto [text, encoding] = DecodeRawText(raw);
    return RawText{text, encoding, ByteVector(raw.begin(), raw.end())};
}

std::optional<std::string> DecodeGb18030(std::span<const std::byte> raw) {
    try {
        return WideToUtf8(BytesToWide(raw, 54936, MB_ERR_INVALID_CHARS));
    } catch (const std::runtime_error&) {
        return std::nullopt;
    }
}

bool HasNonAscii(std::span<const std::byte> raw) {
    return std::any_of(raw.begin(), raw.end(), [](std::byte value) { return std::to_integer<unsigned char>(value) >= 0x80; });
}

template <class Visit>
void ForEachText(TriggerDocument& document, Visit&& visit) {
    for (auto& row : document.categories) visit(row.name);
    for (auto& row : document.variables) { visit(row.name); visit(row.typeName); visit(row.value); }
    for (auto& row : document.triggers) { visit(row.name); visit(row.description); visit(row.customText); }
    for (auto& row : document.ecaNodes) visit(row.name);
    for (auto& row : document.argumentNodes) visit(row.value);
    visit(document.wct.customComment);
    visit(document.wct.customCode);
}

bool IsCjk(std::uint32_t codepoint) {
    return (codepoint >= 0x3000 && codepoint <= 0x9FFF) || (codepoint >= 0xF900 && codepoint <= 0xFAFF)
        || (codepoint >= 0xFF00 && codepoint <= 0xFFEF) || (codepoint >= 0x20000 && codepoint <= 0x2FFFF);
}

// 非 ASCII 字符里有几个是汉字（含中文标点、全角），以及非 ASCII 字符总数。
std::pair<std::size_t, std::size_t> CjkCount(std::string_view text) {
    std::size_t cjk = 0, other = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto scalar = Text::Detail::ReadUtf8Scalar(text, offset);
        if (!scalar.Valid() || scalar.bytes == 0) break;
        if (scalar.codepoint >= 0x80) (IsCjk(scalar.codepoint) ? cjk : other) += 1;
        offset += scalar.bytes;
    }
    return {cjk, cjk + other};
}

// 中文原版 WE 按 GBK 存文字，短的 GBK 文字可能恰好也是合法 UTF-8（“远”D4 B6 读成亚美尼亚字母 U+0536），逐段先试 UTF-8 会认错字。
// 本编辑器写的文件里 GBK 和 UTF-8 本就混用，整份没有“只能按 UTF-8 读”的段也不能证明是 GBK（“火球”的 UTF-8 也能按 GBK 读）。
// 所以只在文件里确有 GBK 段时，把按 UTF-8 读一个汉字都没有、按 GB18030 读全是汉字的段改按 GB18030 读。
void ResolveAmbiguousGb18030(TriggerDocument& document) {
    bool gb18030 = false;
    ForEachText(document, [&](const RawText& value) {
        gb18030 = gb18030 || (value.encoding == TextEncoding::Gb18030 && HasNonAscii(value.originalBytes));
    });
    if (!gb18030) return;
    ForEachText(document, [](RawText& value) {
        if (value.encoding != TextEncoding::Utf8 || !HasNonAscii(value.originalBytes)) return;
        if (CjkCount(value.text).first != 0) return;
        auto decoded = DecodeGb18030(value.originalBytes);
        if (!decoded) return;
        const auto [cjk, total] = CjkCount(*decoded);
        if (total == 0 || cjk != total) return;
        value.text = std::move(*decoded);
        value.encoding = TextEncoding::Gb18030;
    });
}

bool IsUnchangedUtf8Original(const RawText& value) noexcept {
    if (value.originalBytes.empty() || value.encoding != TextEncoding::Utf8 ||
        value.originalBytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        value.originalBytes.size() != value.text.size()) {
        return false;
    }
    const std::string_view original(
        reinterpret_cast<const char*>(value.originalBytes.data()), value.originalBytes.size());
    if (original != value.text) {
        return false;
    }
    for (std::size_t offset = 0; offset < original.size();) {
        const auto scalar = Text::Detail::ReadUtf8Scalar(original, offset);
        if (!scalar.Valid()) {
            return false;
        }
        offset += scalar.bytes;
    }
    return true;
}

// 借用只存续于同次 const 文档写出；拥有分支的 span 每次从当前 vector 派生，
// 不保存可能在 move 后失效的内部指针。公共 RawText::ToBytes 仍返回独立副本。
class EncodedTextBytes final {
public:
    explicit EncodedTextBytes(std::span<const std::byte> borrowed) noexcept
        : borrowed_(borrowed), borrowing_(true) {}
    explicit EncodedTextBytes(ByteVector owned) noexcept : owned_(std::move(owned)) {}
    EncodedTextBytes(const EncodedTextBytes&) = delete;
    EncodedTextBytes& operator=(const EncodedTextBytes&) = delete;
    EncodedTextBytes(EncodedTextBytes&&) noexcept = default;
    EncodedTextBytes& operator=(EncodedTextBytes&&) noexcept = default;

    [[nodiscard]] std::span<const std::byte> Span() const noexcept {
        return borrowing_ ? borrowed_ : std::span<const std::byte>(owned_);
    }

private:
    ByteVector owned_;
    std::span<const std::byte> borrowed_;
    bool borrowing_ = false;
};

EncodedTextBytes EncodeOrThrow(const RawText& text) {
    if (IsUnchangedUtf8Original(text)) {
        return EncodedTextBytes(std::span<const std::byte>(text.originalBytes));
    }
    auto result = text.ToBytes();
    if (!result.Succeeded()) {
        throw ValidationError(
            result.diagnostics.empty() ? "text encoding failed" : result.diagnostics.front().message);
    }
    return EncodedTextBytes(std::move(*result.value));
}

Diagnostic MakeDiagnostic(
    DiagnosticSeverity severity,
    std::string code,
    std::string message,
    std::optional<OriginSpan> origin = std::nullopt) {
    return Diagnostic{
        severity,
        "Trigger.WtgWct",
        std::move(code),
        std::move(message),
        std::move(origin),
    };
}

class Writer {
public:
    [[nodiscard]] std::size_t Size() const noexcept { return data_.size(); }

    void WriteBytes(std::span<const std::byte> bytes) {
        data_.insert(data_.end(), bytes.begin(), bytes.end());
    }

    void WriteAscii(std::string_view value) {
        WriteBytes(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(value.data()), value.size()));
    }

    void WriteI32(std::int32_t value) {
        const std::uint32_t raw = static_cast<std::uint32_t>(value);
        const std::array<std::byte, 4> bytes{
            static_cast<std::byte>(raw), static_cast<std::byte>(raw >> 8U),
            static_cast<std::byte>(raw >> 16U), static_cast<std::byte>(raw >> 24U)};
        WriteBytes(bytes);
    }

    void WriteByte(std::byte value) {
        data_.push_back(value);
    }

    void WriteCString(const RawText& value) {
        const auto encoded = EncodeOrThrow(value);
        const auto bytes = encoded.Span();
        if (std::find(bytes.begin(), bytes.end(), std::byte{0}) != bytes.end()) {
            throw ValidationError("WTG/WCT string contains an embedded NUL");
        }
        WriteBytes(bytes);
        data_.push_back(std::byte{0});
    }

    [[nodiscard]] ByteVector Take() { return std::move(data_); }

private:
    ByteVector data_;
};

RawText ReadSizedText(Reader& reader, std::string_view field, bool& emptyTerminated) {
    emptyTerminated = false;
    const std::int32_t signedSize = reader.ReadI32(std::string(field) + " size");
    if (signedSize < 0) {
        throw FormatError(std::string(field) + " has a negative size");
    }
    if (signedSize == 0) {
        return {};
    }
    const auto bytes = reader.ReadExact(static_cast<std::size_t>(signedSize), field);
    if (bytes.back() != std::byte{0}) {
        throw FormatError(std::string(field) + " is not NUL terminated");
    }
    if (std::find(bytes.begin(), bytes.end() - 1, std::byte{0}) != bytes.end() - 1) {
        throw FormatError(std::string(field) + " contains an embedded NUL");
    }
    emptyTerminated = signedSize == 1;
    return MakeRawText(bytes.first(bytes.size() - 1));
}

void WriteSizedText(Writer& writer, const RawText& value, bool preserveEmptyTerminator) {
    const auto encoded = EncodeOrThrow(value);
    const auto bytes = encoded.Span();
    if (bytes.empty()) {
        writer.WriteI32(preserveEmptyTerminator ? 1 : 0);
        if (preserveEmptyTerminator) {
            writer.WriteByte(std::byte{0});
        }
        return;
    }
    if (bytes.size() >= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw ValidationError("WCT text exceeds the int32 length limit");
    }
    writer.WriteI32(static_cast<std::int32_t>(bytes.size() + 1));
    writer.WriteBytes(bytes);
    writer.WriteByte(std::byte{0});
}

std::int32_t CheckedCount(std::size_t value, std::string_view field) {
    if (value > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        throw ValidationError(std::string(field) + " exceeds the int32 count limit");
    }
    return static_cast<std::int32_t>(value);
}

struct ParsedWct {
    WctHeader header;
    std::vector<RawText> slots;
    bool globalEmptyTerminated = false;
    std::vector<std::size_t> emptyTerminatedSlots;
};

ParsedWct ParseWct(std::span<const std::byte> data) {
    Reader reader(data, "war3map.wct");
    ParsedWct result;
    result.header.version = reader.ReadI32("version");
    if (result.header.version != kWctVersion && result.header.version != kWctLegacyVersion) {
        throw FormatError("war3map.wct: only versions 0 and 1 are supported; got " +
            std::to_string(result.header.version));
    }
    if (result.header.version == kWctVersion) {
        result.header.customComment = reader.ReadCString("custom comment");
        result.header.customCode = ReadSizedText(reader, "custom code", result.globalEmptyTerminated);
    }
    const std::size_t count = reader.ReadCount("trigger slot count", 4);
    result.slots.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        bool emptyTerminated = false;
        result.slots.push_back(ReadSizedText(reader, "trigger slot " + std::to_string(index), emptyTerminated));
        if (emptyTerminated) {
            result.emptyTerminatedSlots.push_back(index);
        }
    }
    reader.ExpectEof();
    return result;
}

struct GraphState {
    std::vector<std::uint8_t> ecaState;
    std::vector<std::uint8_t> argumentState;
    std::vector<std::size_t> ecaReferences;
    std::vector<std::size_t> argumentReferences;
    Detail::CodecDefinitionLookupMemo definitions;
};

void AddValidationError(Status& status, std::string code, std::string message) {
    status.diagnostics.push_back(MakeDiagnostic(
        DiagnosticSeverity::Error, std::move(code), std::move(message)));
}

// 诊断地址只借用同步递归栈链；成功验证不构造/复制每个节点的字符串。
// 原 256 深度门先执行，最长有效调用链加报错层和两个根段不超过 259 段。
class DiagnosticPath final {
public:
    enum class Kind { Triggers, Ecas, Arguments, Children, Call, Index };
    DiagnosticPath(const DiagnosticPath* parent, Kind kind, std::size_t index = 0) noexcept
        : parent_(parent), kind_(kind), index_(index) {}

    std::string Materialize() const {
        std::string result;
        AppendTo(result);
        return result;
    }

private:
    void AppendTo(std::string& result) const {
        if (parent_) parent_->AppendTo(result);
        switch (kind_) {
        case Kind::Triggers: result.append("/triggers/"); break;
        case Kind::Ecas: result.append("/ecas/"); break;
        case Kind::Arguments: result.append("/arguments/"); break;
        case Kind::Children: result.append("/children/"); break;
        case Kind::Call: result.append("/call"); return;
        case Kind::Index: result.append("/index"); return;
        }
        result.append(std::to_string(index_)); // 与旧诊断十进制格式一致，仅真正报错时执行。
    }

    const DiagnosticPath* parent_;
    Kind kind_;
    std::size_t index_;
};

void ValidateArgument(
    std::uint32_t index,
    StableId owner,
    const TriggerDocument& document,
    const UiCatalog& catalog,
    GraphState& graph,
    Status& status,
    const DiagnosticPath& path,
    std::size_t depth);

void ValidateEca(
    std::uint32_t index,
    bool child,
    StableId owner,
    const TriggerDocument& document,
    const UiCatalog& catalog,
    GraphState& graph,
    Status& status,
    const DiagnosticPath& path,
    std::size_t depth) {
    if (depth > 256) {
        AddValidationError(status, "wtg.graph.depth", path.Materialize() + ": nesting exceeds 256 levels");
        return;
    }
    if (index >= document.ecaNodes.size()) {
        AddValidationError(status, "wtg.eca.bad-index", path.Materialize() + ": ECA index is out of range");
        return;
    }
    ++graph.ecaReferences[index];
    if (graph.ecaReferences[index] > 1) {
        AddValidationError(status, "wtg.eca.shared-node", path.Materialize() + ": ECA node has multiple owners");
        return;
    }
    if (graph.ecaState[index] == 1) {
        AddValidationError(status, "wtg.eca.cycle", path.Materialize() + ": ECA graph contains a cycle");
        return;
    }
    graph.ecaState[index] = 1;
    const EcaNode& eca = document.ecaNodes[index];
    const auto lookup = graph.definitions.Lookup(eca.type, eca.name.text);
    const UiDefinition* definition = lookup.definition;
    if (definition == nullptr) {
        AddValidationError(status, "wtg.eca.unknown-ui", path.Materialize() + ": unknown UI definition [" + eca.name.text + "]");
    } else {
        if (lookup.argumentCount != eca.arguments.size()) {
            AddValidationError(status, "wtg.eca.argument-count", path.Materialize() +
                ": argument count does not match " + definition->Signature());
        }
    }
    if (eca.id == 0) {
        AddValidationError(status, "wtg.eca.missing-id", path.Materialize() + ": stable id is zero");
    }
    if (eca.enabled != 0 && eca.enabled != 1) {
        AddValidationError(status, "wtg.eca.enabled", path.Materialize() + ": enabled must be 0 or 1");
    }
    if (child != eca.childId.has_value()) {
        AddValidationError(status, "wtg.eca.child-id", path.Materialize() +
            (child ? ": child ECA is missing childId" : ": root/call ECA contains childId"));
    }
    for (std::size_t argument = 0; argument < eca.arguments.size(); ++argument) {
        ValidateArgument(
            eca.arguments[argument], owner, document, catalog, graph, status,
            DiagnosticPath{&path, DiagnosticPath::Kind::Arguments, argument}, depth + 1);
    }
    for (std::size_t childIndex = 0; childIndex < eca.children.size(); ++childIndex) {
        ValidateEca(
            eca.children[childIndex], true, owner, document, catalog, graph, status,
            DiagnosticPath{&path, DiagnosticPath::Kind::Children, childIndex}, depth + 1);
    }
    graph.ecaState[index] = 2;
}

void ValidateArgument(
    std::uint32_t index,
    StableId owner,
    const TriggerDocument& document,
    const UiCatalog& catalog,
    GraphState& graph,
    Status& status,
    const DiagnosticPath& path,
    std::size_t depth) {
    if (depth > 256) {
        AddValidationError(status, "wtg.graph.depth", path.Materialize() + ": nesting exceeds 256 levels");
        return;
    }
    if (index >= document.argumentNodes.size()) {
        AddValidationError(status, "wtg.argument.bad-index", path.Materialize() + ": argument index is out of range");
        return;
    }
    ++graph.argumentReferences[index];
    if (graph.argumentReferences[index] > 1) {
        AddValidationError(status, "wtg.argument.shared-node", path.Materialize() + ": argument node has multiple owners");
        return;
    }
    if (graph.argumentState[index] == 1) {
        AddValidationError(status, "wtg.argument.cycle", path.Materialize() + ": argument graph contains a cycle");
        return;
    }
    graph.argumentState[index] = 1;
    const ArgumentNode& argument = document.argumentNodes[index];
    if (argument.id == 0) {
        AddValidationError(status, "wtg.argument.missing-id", path.Materialize() + ": stable id is zero");
    }
    if (argument.type < -1 || argument.type > 3) {
        AddValidationError(status, "wtg.argument.type", path.Materialize() + ": type must be in -1..3");
    }
    if (argument.insertCall != 0 && argument.insertCall != 1) {
        AddValidationError(status, "wtg.argument.insert-call", path.Materialize() + ": insertCall must be 0 or 1");
    }
    if ((argument.insertCall == 1) != argument.callEca.has_value()) {
        AddValidationError(status, "wtg.argument.call-presence", path.Materialize() +
            ": insertCall and call ECA presence disagree");
    }
    if (argument.callEca.has_value()) {
        ValidateEca(
            *argument.callEca, false, owner, document, catalog, graph, status,
            DiagnosticPath{&path, DiagnosticPath::Kind::Call}, depth + 1);
    }
    if (argument.insertIndex != 0 && argument.insertIndex != 1) {
        AddValidationError(status, "wtg.argument.insert-index", path.Materialize() + ": insertIndex must be 0 or 1");
    }
    if ((argument.insertIndex == 1) != argument.indexArgument.has_value()) {
        AddValidationError(status, "wtg.argument.index-presence", path.Materialize() +
            ": insertIndex and index argument presence disagree");
    }
    if (argument.indexArgument.has_value()) {
        ValidateArgument(
            *argument.indexArgument, owner, document, catalog, graph, status,
            DiagnosticPath{&path, DiagnosticPath::Kind::Index}, depth + 1);
    }
    graph.argumentState[index] = 2;
}

void ValidateGraph(
    const TriggerDocument& document,
    const UiCatalog& catalog,
    Status& status) {
    GraphState graph{
        std::vector<std::uint8_t>(document.ecaNodes.size()),
        std::vector<std::uint8_t>(document.argumentNodes.size()),
        std::vector<std::size_t>(document.ecaNodes.size()),
        std::vector<std::size_t>(document.argumentNodes.size()),
        Detail::CodecDefinitionLookupMemo{catalog},
    };
    for (std::size_t triggerIndex = 0; triggerIndex < document.triggers.size(); ++triggerIndex) {
        const TriggerRecord& trigger = document.triggers[triggerIndex];
        const DiagnosticPath triggerPath{nullptr, DiagnosticPath::Kind::Triggers, triggerIndex};
        for (std::size_t ecaIndex = 0; ecaIndex < trigger.ecas.size(); ++ecaIndex) {
            ValidateEca(
                trigger.ecas[ecaIndex], false, trigger.id, document, catalog, graph, status,
                DiagnosticPath{&triggerPath, DiagnosticPath::Kind::Ecas, ecaIndex}, 0);
        }
    }
    for (std::size_t index = 0; index < graph.ecaReferences.size(); ++index) {
        if (graph.ecaReferences[index] == 0) {
            AddValidationError(status, "wtg.eca.orphan", "/ecaNodes/" +
                std::to_string(index) + ": node is not owned by a trigger");
        }
    }
    for (std::size_t index = 0; index < graph.argumentReferences.size(); ++index) {
        if (graph.argumentReferences[index] == 0) {
            AddValidationError(status, "wtg.argument.orphan", "/argumentNodes/" +
                std::to_string(index) + ": node is not owned by an ECA");
        }
    }
}

void WriteArgument(
    Writer& writer,
    const TriggerDocument& document,
    std::uint32_t index,
    std::int32_t version);

void WriteEca(
    Writer& writer,
    const TriggerDocument& document,
    std::uint32_t index,
    bool child,
    std::int32_t version) {
    const EcaNode& eca = document.ecaNodes.at(index);
    writer.WriteI32(eca.type);
    if (child) {
        if (!eca.childId.has_value()) {
            throw ValidationError("child ECA is missing childId");
        }
        writer.WriteI32(*eca.childId);
    }
    writer.WriteCString(eca.name);
    writer.WriteI32(eca.enabled);
    for (const std::uint32_t argument : eca.arguments) {
        WriteArgument(writer, document, argument, version);
    }
    if (version < kWtgVersion) return; // 版本 4 没有子节点计数（验证已拒绝带子节点的文档）
    writer.WriteI32(CheckedCount(eca.children.size(), "child ECA count"));
    for (const std::uint32_t childIndex : eca.children) {
        WriteEca(writer, document, childIndex, true, version);
    }
}

void WriteArgument(
    Writer& writer,
    const TriggerDocument& document,
    std::uint32_t index,
    std::int32_t version) {
    const ArgumentNode& argument = document.argumentNodes.at(index);
    writer.WriteI32(argument.type);
    writer.WriteCString(argument.value);
    writer.WriteI32(argument.insertCall);
    if (argument.callEca.has_value()) {
        WriteEca(writer, document, *argument.callEca, false, version);
    }
    writer.WriteI32(argument.insertIndex);
    if (argument.indexArgument.has_value()) {
        WriteArgument(writer, document, *argument.indexArgument, version);
    }
}

} // namespace

RawText DecodeSourceText(std::span<const std::byte> raw) { return MakeRawText(raw); }
RawText Detail::DecodeSourceText(std::span<const std::byte> raw) { return Trigger::DecodeSourceText(raw); }

Result<std::vector<std::byte>> RawText::ToBytes() const {
    try {
        // 仅合法 UTF-8 且当前文字逐字节未改时省略宽字符往返；其余保留原解码/编码门。
        // NUL/BOM 是合法标量，是否可写由原 CString/上层规则决定。
        if (IsUnchangedUtf8Original(*this)) {
            return Result<ByteVector>::Success(originalBytes);
        }
        if (!originalBytes.empty()) {
            const auto [decoded, ignoredEncoding] = DecodeRawText(originalBytes);
            (void)ignoredEncoding;
            if (decoded == text) {
                return Result<ByteVector>::Success(originalBytes);
            }
            // 按整份证据改判成 GB18030 的段（见 ResolveAmbiguousGb18030）。
            if (encoding == TextEncoding::Gb18030) {
                if (const auto gb18030 = DecodeGb18030(originalBytes); gb18030 && *gb18030 == text)
                    return Result<ByteVector>::Success(originalBytes);
            }
        }
        const std::wstring wide = BytesToWide(
            std::span<const std::byte>(
                reinterpret_cast<const std::byte*>(text.data()), text.size()),
            CP_UTF8,
            MB_ERR_INVALID_CHARS);
        switch (encoding) {
        case TextEncoding::Utf8:
        case TextEncoding::Utf8Bom:
            return Result<ByteVector>::Success(ByteVector(
                reinterpret_cast<const std::byte*>(text.data()),
                reinterpret_cast<const std::byte*>(text.data() + text.size())));
        case TextEncoding::Gb18030:
            return Result<ByteVector>::Success(WideToCodePage(wide, 54936));
        case TextEncoding::Latin1: {
            ByteVector result;
            result.reserve(wide.size());
            for (const wchar_t character : wide) {
                if (character > 0xff) {
                    return Result<ByteVector>::Failure(
                        "Trigger.WtgWct", "text.latin1.unrepresentable",
                        "edited text cannot be represented as Latin-1");
                }
                result.push_back(static_cast<std::byte>(character));
            }
            return Result<ByteVector>::Success(std::move(result));
        }
        }
    } catch (const std::exception& error) {
        return Result<ByteVector>::Failure(
            "Trigger.WtgWct", "text.encode-failed", error.what());
    }
    return Result<ByteVector>::Failure(
        "Trigger.WtgWct", "text.unknown-encoding", "unknown text encoding");
}

std::string TriggerDocument::FormatName() const {
    return "WTG v7 / WCT v1";
}

bool TriggerDocument::HasTrustedSourceSeal() const noexcept {
    return sourceSeal_ != nullptr;
}

std::string_view TriggerDocument::SourceWtgSha256() const noexcept {
    return sourceSeal_ ? std::string_view(sourceSeal_->sourceWtgSha256) : std::string_view{};
}

std::string_view TriggerDocument::SourceWctSha256() const noexcept {
    return sourceSeal_ ? std::string_view(sourceSeal_->sourceWctSha256) : std::string_view{};
}

std::string_view TriggerDocument::UiCatalogSha256() const noexcept {
    return sourceSeal_ ? std::string_view(sourceSeal_->uiCatalogSha256) : std::string_view{};
}

Result<TriggerDocument> WtgWctCodec::Parse(
    std::span<const std::byte> wtg,
    std::span<const std::byte> wct,
    const UiCatalog& catalog) const {
    Result<TriggerDocument> result;
    try {
        ParsedWct parsedWct = ParseWct(wct);
        TriggerDocument document = Detail::DecodeClassicWtg(wtg, catalog, parsedWct.slots, parsedWct.header);
        ResolveAmbiguousGb18030(document);
        auto sourceSeal = std::make_shared<TriggerDocument::SourceSeal>();
        const auto wtgHash = Sha256(wtg);
        const auto wctHash = Sha256(wct);
        if (!wtgHash.Succeeded() || !wctHash.Succeeded()) {
            result.diagnostics.insert(
                result.diagnostics.end(), wtgHash.diagnostics.begin(), wtgHash.diagnostics.end());
            result.diagnostics.insert(
                result.diagnostics.end(), wctHash.diagnostics.begin(), wctHash.diagnostics.end());
            return result;
        }
        sourceSeal->sourceWtgSha256 = *wtgHash.value;
        sourceSeal->sourceWctSha256 = *wctHash.value;
        sourceSeal->uiCatalogSha256 = catalog.Sha256();
        sourceSeal->globalEmptyTerminated = parsedWct.globalEmptyTerminated;
        for (const auto slot : parsedWct.emptyTerminatedSlots) {
            // WtgParser 已核对 WTG 记录数和 WCT 槽数完全一致。
            sourceSeal->emptyTerminatedTriggers.insert(document.triggers[slot].id);
        }
        document.sourceSeal_ = std::move(sourceSeal);
        result.value.emplace(std::move(document));
    } catch (const MissingUiDefinitionError& error) {
        result.diagnostics.push_back(MakeDiagnostic(
            DiagnosticSeverity::Error,
            "wtg.missing-ui-definition",
            error.what(),
            MakeOrigin("war3map.wtg", error.Offset(), error.EndOffset())));
    } catch (const std::exception& error) {
        result.diagnostics.push_back(MakeDiagnostic(
            DiagnosticSeverity::Error,
            "wtg.parse-failed",
            error.what()));
    }
    return result;
}

namespace {
void ValidateSealAndHeaders(const TriggerDocument& document, const UiCatalog& catalog, Status& status) {
    if (!document.HasTrustedSourceSeal()) {
        AddValidationError(
            status,
            "wtg.source-seal.missing",
            "WTG 文档缺少解析器建立的可信来源 seal，禁止验证或序列化。");
    } else if (document.SourceWtgSha256().size() != 64 ||
        document.SourceWctSha256().size() != 64 ||
        document.UiCatalogSha256().size() != 64 ||
        document.UiCatalogSha256() != catalog.Sha256()) {
        AddValidationError(
            status,
            "wtg.source-seal.identity-mismatch",
            "WTG 来源 seal 或 UI Catalog 身份不匹配，禁止验证或序列化。");
    }
    // 该字段是游戏版本：冰封王座格式（WTG 7）为 2，混乱之治格式（WTG 4）为 1。原值写回，不改版本。
    const std::int32_t expectedGameVersion = document.wtgVersion == kWtgLegacyVersion ? 1 : 2;
    if (document.variablesHeader != expectedGameVersion) {
        AddValidationError(status, "wtg.variables-header", "WTG v" + std::to_string(document.wtgVersion)
            + " variablesHeader (game version) must be " + std::to_string(expectedGameVersion));
    }
    if (document.wtgVersion != kWtgVersion && document.wtgVersion != kWtgLegacyVersion) {
        AddValidationError(status, "wtg.version", "WTG version must be 4 or 7");
    } else if (document.wtgVersion == kWtgLegacyVersion) {
        // 混乱之治格式写不下这些内容；明确拒绝，不悄悄丢弃。
        const auto reject = [&](const std::string& what) {
            AddValidationError(status, "wtg.legacy-format", "这张图的触发器是混乱之治格式（WTG 版本 4），" + what);
        };
        for (const CategoryRecord& category : document.categories)
            if (category.comment != 0) { reject("不能保存注释类分类：" + category.name.text); break; }
        for (const TriggerRecord& trigger : document.triggers)
            if (trigger.type != 0) { reject("不能保存注释触发器：" + trigger.name.text); break; }
        for (const VariableRecord& variable : document.variables)
            if (variable.size != 1) { reject("变量没有数组大小字段：" + variable.name.text); break; }
        for (const EcaNode& eca : document.ecaNodes)
            if (!eca.children.empty()) { reject("不支持带子动作的块（如“多个动作”“如果-则-否则（多项）”）：" + eca.name.text); break; }
    }
    if (document.wct.version != kWctVersion && document.wct.version != kWctLegacyVersion) {
        AddValidationError(status, "wct.version", "WCT version must be 0 or 1");
    }
}

void ValidateVariables(const TriggerDocument& document, Status& status) {
    std::set<std::string> variableNames;
    for (std::size_t index = 0; index < document.variables.size(); ++index) {
        const VariableRecord& variable = document.variables[index];
        if (!variableNames.insert(variable.name.text).second) {
            AddValidationError(status, "wtg.variable.duplicate-name", "/variables/" +
                std::to_string(index) + ": duplicate variable name");
        }
        if ((variable.isArray != 0 && variable.isArray != 1) ||
            (variable.hasDefault != 0 && variable.hasDefault != 1)) {
            AddValidationError(status, "wtg.variable.flags", "/variables/" +
                std::to_string(index) + ": array/default flags must be 0 or 1");
        }
    }
}

void ValidateTriggerFlags(const TriggerRecord& trigger, const std::string& path, Status& status) {
    for (const auto [flag, name] : std::array<std::pair<std::int32_t, const char*>, 4>{
             std::pair{trigger.enabled, "enabled"},
             std::pair{trigger.customTextFlag, "customTextFlag"},
             std::pair{trigger.initiallyClosed, "initiallyClosed"},
             std::pair{trigger.runOnInit, "runOnInit"}}) {
        if (flag != 0 && flag != 1) {
            AddValidationError(status, "wtg.trigger.flag", path + ": " + name + " must be 0 or 1");
        }
    }
}
} // namespace

Status WtgWctCodec::Validate(
    const TriggerDocument& document,
    const UiCatalog& catalog) const {
    Status status;
    ValidateSealAndHeaders(document, catalog, status);

    std::set<std::int32_t> categoryIds;
    // 局部 arena 只改变临时集合分配，保留原增长与完整身份校验。
    alignas(std::max_align_t) std::array<std::byte, 16 * 1024> stableIdStorage;
    std::pmr::monotonic_buffer_resource stableIdResource(
        stableIdStorage.data(), stableIdStorage.size(), std::pmr::new_delete_resource());
    std::pmr::unordered_set<StableId> stableIds{std::pmr::polymorphic_allocator<StableId>{&stableIdResource}};
    // 仅实际五域记录数在可选预留窗口内时减少 rehash；不是文档准入上限。
    Detail::ReserveStableIdBuckets(stableIds, std::array<std::size_t, 5>{
        document.categories.size(), document.variables.size(), document.triggers.size(),
        document.ecaNodes.size(), document.argumentNodes.size()});
    for (std::size_t index = 0; index < document.categories.size(); ++index) {
        const CategoryRecord& category = document.categories[index];
        if (!categoryIds.insert(category.categoryId).second) {
            AddValidationError(status, "wtg.category.duplicate-id", "/categories/" +
                std::to_string(index) + ": duplicate category id");
        }
        if (category.comment != 0 && category.comment != 1) {
            AddValidationError(status, "wtg.category.comment", "/categories/" +
                std::to_string(index) + ": comment must be 0 or 1");
        }
        if (category.id == 0 || !stableIds.insert(category.id).second) {
            AddValidationError(status, "wtg.stable-id", "/categories/" +
                std::to_string(index) + ": stable id is zero or duplicated");
        }
    }

    ValidateVariables(document, status);
    for (std::size_t index = 0; index < document.variables.size(); ++index) {
        const VariableRecord& variable = document.variables[index];
        if (variable.id == 0 || !stableIds.insert(variable.id).second) {
            AddValidationError(status, "wtg.stable-id", "/variables/" +
                std::to_string(index) + ": stable id is zero or duplicated");
        }
    }

    for (std::size_t index = 0; index < document.triggers.size(); ++index) {
        const TriggerRecord& trigger = document.triggers[index];
        const std::string path = "/triggers/" + std::to_string(index);
        if (!categoryIds.contains(trigger.categoryId)) {
            AddValidationError(status, "wtg.trigger.category", path + ": category does not exist");
        }
        ValidateTriggerFlags(trigger, path, status);
        if (trigger.id == 0 || !stableIds.insert(trigger.id).second) {
            AddValidationError(status, "wtg.stable-id", path + ": stable id is zero or duplicated");
        }
    }
    for (std::size_t index = 0; index < document.ecaNodes.size(); ++index) {
        const StableId id = document.ecaNodes[index].id;
        if (id == 0 || !stableIds.insert(id).second) {
            AddValidationError(status, "wtg.stable-id", "/ecaNodes/" +
                std::to_string(index) + ": stable id is zero or duplicated");
        }
    }
    for (std::size_t index = 0; index < document.argumentNodes.size(); ++index) {
        const StableId id = document.argumentNodes[index].id;
        if (id == 0 || !stableIds.insert(id).second) {
            AddValidationError(status, "wtg.stable-id", "/argumentNodes/" +
                std::to_string(index) + ": stable id is zero or duplicated");
        }
    }
    ValidateGraph(document, catalog, status);
    return status;
}

Status WtgWctCodec::ValidateTrigger(
    const TriggerDocument& document,
    const UiCatalog& catalog,
    StableId triggerId) const {
    Status status;
    ValidateSealAndHeaders(document, catalog, status);
    ValidateVariables(document, status);
    std::size_t found = document.triggers.size();
    for (std::size_t index = 0; index < document.triggers.size(); ++index) {
        if (document.triggers[index].id != triggerId) continue;
        if (found != document.triggers.size()) {
            AddValidationError(status, "wtg.stable-id", "/triggers/" +
                std::to_string(index) + ": stable id is zero or duplicated");
            return status;
        }
        found = index;
    }
    if (triggerId == 0 || found == document.triggers.size()) {
        AddValidationError(status, "wtg.trigger.missing", "target trigger does not exist");
        return status;
    }
    const TriggerRecord& trigger = document.triggers[found];
    const std::string path = "/triggers/" + std::to_string(found);
    if (std::none_of(document.categories.begin(), document.categories.end(),
            [&](const CategoryRecord& category) { return category.categoryId == trigger.categoryId; })) {
        AddValidationError(status, "wtg.trigger.category", path + ": category does not exist");
    }
    ValidateTriggerFlags(trigger, path, status);
    // 目标子树按完整校验的同一规则检查下标、环、共享、childId 与函数库定义；
    // 与其他触发器之间的共享节点和孤儿节点只由保存前的完整 Validate 判定。
    GraphState graph{
        std::vector<std::uint8_t>(document.ecaNodes.size()),
        std::vector<std::uint8_t>(document.argumentNodes.size()),
        std::vector<std::size_t>(document.ecaNodes.size()),
        std::vector<std::size_t>(document.argumentNodes.size()),
        Detail::CodecDefinitionLookupMemo{catalog},
    };
    const DiagnosticPath triggerPath{nullptr, DiagnosticPath::Kind::Triggers, found};
    for (std::size_t ecaIndex = 0; ecaIndex < trigger.ecas.size(); ++ecaIndex) {
        ValidateEca(
            trigger.ecas[ecaIndex], false, trigger.id, document, catalog, graph, status,
            DiagnosticPath{&triggerPath, DiagnosticPath::Kind::Ecas, ecaIndex}, 0);
    }
    return status;
}

namespace {
// Serialize 与 EncodeForIdentity 共用的写出；调用前须确认来源 seal 存在。
// seal 类型是 TriggerDocument 的私有嵌套类型，由友元成员推导传入。
template <class Seal>
EncodedTriggerDocument EncodeDocument(const TriggerDocument& document, const Seal& seal) {
    Writer wtg;
    const std::int32_t version = document.wtgVersion;
    wtg.WriteAscii("WTG!");
    wtg.WriteI32(version);
    wtg.WriteI32(CheckedCount(document.categories.size(), "category count"));
    for (const CategoryRecord& category : document.categories) {
        wtg.WriteI32(category.categoryId);
        wtg.WriteCString(category.name);
        if (version >= kWtgVersion) wtg.WriteI32(category.comment);
    }
    wtg.WriteI32(document.variablesHeader);
    wtg.WriteI32(CheckedCount(document.variables.size(), "variable count"));
    for (const VariableRecord& variable : document.variables) {
        wtg.WriteCString(variable.name);
        wtg.WriteCString(variable.typeName);
        wtg.WriteI32(variable.unknown);
        wtg.WriteI32(variable.isArray);
        if (version >= kWtgVersion) wtg.WriteI32(variable.size);
        wtg.WriteI32(variable.hasDefault);
        wtg.WriteCString(variable.value);
    }
    wtg.WriteI32(CheckedCount(document.triggers.size(), "trigger count"));
    for (const TriggerRecord& trigger : document.triggers) {
        wtg.WriteCString(trigger.name);
        wtg.WriteCString(trigger.description);
        if (version >= kWtgVersion) wtg.WriteI32(trigger.type);
        wtg.WriteI32(trigger.enabled);
        wtg.WriteI32(trigger.customTextFlag);
        wtg.WriteI32(trigger.initiallyClosed);
        wtg.WriteI32(trigger.runOnInit);
        wtg.WriteI32(trigger.categoryId);
        wtg.WriteI32(CheckedCount(trigger.ecas.size(), "root ECA count"));
        for (const std::uint32_t eca : trigger.ecas) {
            WriteEca(wtg, document, eca, false, version);
        }
    }
    ByteVector encodedWtg = wtg.Take();

    Writer wct;
    // 版本 0 没有地图头代码段；作者加了头注释或自定义代码时只能写成版本 1。
    const bool legacy = document.wct.version == kWctLegacyVersion
        && document.wct.customComment.text.empty() && document.wct.customComment.originalBytes.empty()
        && document.wct.customCode.text.empty() && document.wct.customCode.originalBytes.empty();
    wct.WriteI32(legacy ? kWctLegacyVersion : kWctVersion);
    if (!legacy) {
        wct.WriteCString(document.wct.customComment);
        WriteSizedText(wct, document.wct.customCode, seal.globalEmptyTerminated);
    }
    wct.WriteI32(CheckedCount(document.triggers.size(), "WCT trigger slot count"));
    for (const TriggerRecord& trigger : document.triggers) {
        WriteSizedText(wct, trigger.customText, seal.emptyTerminatedTriggers.contains(trigger.id));
    }
    return EncodedTriggerDocument{std::move(encodedWtg), wct.Take()};
}
} // namespace

Result<EncodedTriggerDocument> WtgWctCodec::Serialize(
    const TriggerDocument& document,
    const UiCatalog& catalog) const {
    Result<EncodedTriggerDocument> result;
    Status validation = Validate(document, catalog);
    result.diagnostics = validation.diagnostics;
    if (!validation.Succeeded()) {
        return result;
    }

    try {
        result.value.emplace(EncodeDocument(document, *document.sourceSeal_));
    } catch (const std::exception& error) {
        result.diagnostics.push_back(MakeDiagnostic(
            DiagnosticSeverity::Error,
            "wtg.serialize-failed",
            error.what()));
    }
    return result;
}

Result<EncodedTriggerDocument> WtgWctCodec::EncodeForIdentity(const TriggerDocument& document) const {
    Result<EncodedTriggerDocument> result;
    if (!document.sourceSeal_) {
        result.diagnostics.push_back(MakeDiagnostic(DiagnosticSeverity::Error, "wtg.identity-unsealed",
            "触发器文档没有来源 seal，不能计算内容身份。"));
        return result;
    }
    try {
        result.value.emplace(EncodeDocument(document, *document.sourceSeal_));
    } catch (const std::exception& error) {
        result.diagnostics.push_back(MakeDiagnostic(
            DiagnosticSeverity::Error,
            "wtg.identity-encode-failed",
            error.what()));
    }
    return result;
}
TriggerInspection WtgWctCodec::Inspect(const TriggerDocument& document) const noexcept {
    TriggerInspection inspection;
    inspection.categoryCount = document.categories.size();
    inspection.variableCount = document.variables.size();
    inspection.triggerCount = document.triggers.size();
    inspection.ecaCount = document.ecaNodes.size();
    inspection.argumentCount = document.argumentNodes.size();
    inspection.customTextSlotCount = static_cast<std::size_t>(std::count_if(
        document.triggers.begin(), document.triggers.end(), [](const TriggerRecord& trigger) {
            return !trigger.customText.text.empty() || !trigger.customText.originalBytes.empty();
        }));
    for (const EcaNode& eca : document.ecaNodes) {
        switch (eca.type) {
        case 0: ++inspection.eventCount; break;
        case 1: ++inspection.conditionCount; break;
        case 2: ++inspection.actionCount; break;
        case 3: ++inspection.callCount; break;
        default: break;
        }
    }
    return inspection;
}

Result<War3Map::TriggerPatch> WtgWctCodec::BuildPatch(
    const TriggerDocument& document,
    const UiCatalog& catalog,
    std::filesystem::path wtgRelativePath,
    std::filesystem::path wctRelativePath,
    std::string summary) const {
    auto encoded = Serialize(document, catalog);
    if (!encoded.Succeeded()) {
        Result<War3Map::TriggerPatch> result;
        result.diagnostics = std::move(encoded.diagnostics);
        return result;
    }
    if (document.SourceWtgSha256().size() != 64 ||
        document.SourceWctSha256().size() != 64 ||
        document.UiCatalogSha256() != catalog.Sha256()) {
        return Result<War3Map::TriggerPatch>::Failure(
            "Trigger",
            "TRIGGER_SOURCE_IDENTITY_MISSING",
            "Trigger patches require source WTG/WCT hashes and the exact parsed UI Catalog identity.");
    }

    std::vector<War3Map::BinaryFilePatch> files;
    files.push_back(War3Map::BinaryFilePatch{
        std::move(wtgRelativePath),
        std::move(encoded.value->wtg),
        summary + " (WTG)",
        std::string(document.SourceWtgSha256()),
    });
    files.push_back(War3Map::BinaryFilePatch{
        std::move(wctRelativePath),
        std::move(encoded.value->wct),
        std::move(summary) + " (WCT)",
        std::string(document.SourceWctSha256()),
    });
    return Result<War3Map::TriggerPatch>::Success(War3Map::TriggerPatch(
        std::move(files),
        catalog.Sha256()));
}

Result<TriggerDocument> WtgWctCodec::CreateEmpty(const UiCatalog& catalog) const {
    Writer wtg;
    wtg.WriteAscii("WTG!"); wtg.WriteI32(kWtgVersion); wtg.WriteI32(0);
    wtg.WriteI32(2); wtg.WriteI32(0); wtg.WriteI32(0);
    Writer wct;
    wct.WriteI32(kWctVersion); wct.WriteCString(RawText{}); wct.WriteI32(0); wct.WriteI32(0);
    return Parse(wtg.Take(), wct.Take(), catalog);
}

} // namespace War3Map::Trigger
