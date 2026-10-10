// 原 WriteI32 的四次 push_back 是独立字节参考；不复用候选 Writer 或私有授权。
#include <array>

namespace {
struct LegacyIntegerWireWriter {
    std::vector<std::byte> bytes;
    void Integer(std::int32_t value) {
        const auto raw = static_cast<std::uint32_t>(value);
        bytes.push_back(static_cast<std::byte>(raw));
        bytes.push_back(static_cast<std::byte>(raw >> 8U));
        bytes.push_back(static_cast<std::byte>(raw >> 16U));
        bytes.push_back(static_cast<std::byte>(raw >> 24U));
    }
    void CString(const RawText& value) {
        const auto encoded = Take(value.ToBytes());
        bytes.insert(bytes.end(), encoded.begin(), encoded.end()); bytes.push_back(std::byte{0});
    }
    void Sized(const RawText& value) {
        const auto encoded = Take(value.ToBytes());
        if (encoded.empty()) { Integer(0); return; }
        Require(encoded.size() < static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()), "oracle small owned text");
        Integer(static_cast<std::int32_t>(encoded.size() + 1));
        bytes.insert(bytes.end(), encoded.begin(), encoded.end()); bytes.push_back(std::byte{0});
    }
};
EncodedTriggerDocument LegacyNoEcaIntegerWire(const TriggerDocument& document) {
    Require(document.ecaNodes.empty() && document.argumentNodes.empty(), "independent oracle only accepts its actual empty graph fixture");
    LegacyIntegerWireWriter wtg, wct;
    for (const auto c : std::string_view("WTG!")) wtg.bytes.push_back(static_cast<std::byte>(c));
    wtg.Integer(7); wtg.Integer(static_cast<std::int32_t>(document.categories.size()));
    for (const auto& row : document.categories) { wtg.Integer(row.categoryId); wtg.CString(row.name); wtg.Integer(row.comment); }
    wtg.Integer(document.variablesHeader); wtg.Integer(static_cast<std::int32_t>(document.variables.size()));
    for (const auto& row : document.variables) {
        wtg.CString(row.name); wtg.CString(row.typeName); wtg.Integer(row.unknown); wtg.Integer(row.isArray);
        wtg.Integer(row.size); wtg.Integer(row.hasDefault); wtg.CString(row.value);
    }
    wtg.Integer(static_cast<std::int32_t>(document.triggers.size()));
    for (const auto& row : document.triggers) {
        Require(row.ecas.empty(), "oracle trigger has no ECA roots");
        wtg.CString(row.name); wtg.CString(row.description); wtg.Integer(row.type); wtg.Integer(row.enabled);
        wtg.Integer(row.customTextFlag); wtg.Integer(row.initiallyClosed); wtg.Integer(row.runOnInit); wtg.Integer(row.categoryId); wtg.Integer(0);
    }
    wct.Integer(document.wct.version); wct.CString(document.wct.customComment); wct.Sized(document.wct.customCode);
    wct.Integer(static_cast<std::int32_t>(document.triggers.size()));
    for (const auto& row : document.triggers) wct.Sized(row.customText);
    return {std::move(wtg.bytes), std::move(wct.bytes)};
}
void CodecSingleAppendIntegersPreserveLegacyWireAndActualCapacity(const UiCatalog& catalog) {
    constexpr std::array<std::int32_t, 4> signedEdges{
        -1, std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max(), 0};
    constexpr std::array<std::size_t, 10> sizes{1, 14, 15, 16, 31, 32, 63, 8191, 8192, 8193};
    for (const auto edge : signedEdges) for (const auto size : sizes) {
        auto document = Empty(catalog);
        Check(TriggerDocumentEditor::AddVariable(document, "IntegerWire", "integer"));
        Take(TriggerDocumentEditor::AddTrigger(document, document.categories.front().categoryId, "IntegerWireTrigger"));
        document.categories.front().categoryId = edge; document.triggers.front().categoryId = edge;
        document.categories.front().name = RawText{std::string(size, 'C'), TextEncoding::Utf8, {}};
        document.variables.front().unknown = edge;
        document.variables.front().value = RawText{std::string(size, '1'), TextEncoding::Utf8, {}};
        document.wct.customCode = RawText{size % 2 ? std::string{} : std::string(size, 'x'), TextEncoding::Utf8, {}};
        Require(WtgWctCodec{}.Validate(document, catalog).Succeeded(), "signed integer wire fixture remains actually valid");
        const auto expected = LegacyNoEcaIntegerWire(document), actual = Wire(document, catalog);
        Same(expected, actual);
        auto parsed = Take(WtgWctCodec{}.Parse(actual.wtg, actual.wct, catalog));
        Require(WtgWctCodec{}.Validate(parsed, catalog).Succeeded() && parsed.variables.front().unknown == edge
            && parsed.categories.front().categoryId == edge && parsed.triggers.front().categoryId == edge,
            "signed MIN/MAX/-1 little-endian values survive actual Parse/Validate");
        Same(actual, Wire(parsed, catalog));
        const auto identity = Take(CaptureTriggerGraphReplayIdentity(document, catalog));
        Require(identity.EncodedBytes() == actual.wtg.size() + actual.wct.size()
            && identity.EncodedCapacityBytes() == actual.wtg.capacity() + actual.wct.capacity(),
            "capture ledger reports actual new writer capacity, not old growth or promised unchanged allocation");
        LegacyGraphEquivalence(document, catalog);
    }
}
} // namespace
