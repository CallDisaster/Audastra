#include "War3MapCore/Trigger/GuiConversionSnapshots.h"

#include "War3MapCore/Hash.h"
#include "War3MapCore/Trigger/TriggerCodec.h"

#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace War3Map::Trigger {
namespace {
constexpr std::uint32_t kMagic = 0x53474341; // ACGS
constexpr std::uint32_t kVersion = 1;

std::string Hash(std::span<const std::byte> bytes) {
    auto result = Sha256(bytes);
    if (!result.Succeeded() || !result.value) throw std::runtime_error("快照摘要计算失败。");
    return std::move(*result.value);
}
std::string Hash(std::string_view text) {
    return Hash(std::as_bytes(std::span(text.data(), text.size())));
}

// 私有工程边车使用有界长度前缀；快照子树仍由共用 WTG/WCT codec 编码，不再维护第二种图格式。
struct Writer {
    std::vector<std::byte> bytes;
    void U32(std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::byte>((value >> (i * 8)) & 255));
    }
    void U64(std::uint64_t value) { U32(static_cast<std::uint32_t>(value)); U32(static_cast<std::uint32_t>(value >> 32)); }
    void Data(std::span<const std::byte> value) {
        if (bytes.size() > GuiConversionSnapshots::kMaxBytes - 4 || value.size() > GuiConversionSnapshots::kMaxBytes - bytes.size() - 4)
            throw std::runtime_error("转换快照超过 64 MiB 工程预算。");
        U32(static_cast<std::uint32_t>(value.size())); bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void Text(std::string_view value) { Data(std::as_bytes(std::span(value.data(), value.size()))); }
};
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t at = 0;
    std::uint32_t U32() {
        if (at > bytes.size() || bytes.size() - at < 4) throw std::runtime_error("转换快照被截断。");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[at++]) << (i * 8);
        return value;
    }
    std::uint64_t U64() { const auto low = U32(); return low | (static_cast<std::uint64_t>(U32()) << 32); }
    std::span<const std::byte> Data() {
        const auto count = U32();
        if (count > bytes.size() - at) throw std::runtime_error("转换快照成员长度无效。");
        const auto value = bytes.subspan(at, count); at += count; return value;
    }
    std::string Text() {
        const auto value = Data(); return {reinterpret_cast<const char*>(value.data()), value.size()};
    }
};
const TriggerRecord* Record(const TriggerDocument& document, StableId id) {
    const TriggerRecord* result = nullptr;
    for (const auto& record : document.triggers) if (record.id == id) {
        if (result) return nullptr;
        result = &record;
    }
    return result;
}
std::string RecordHash(const TriggerRecord& record) {
    Writer writer;
    writer.Text(record.name.text); writer.Text(record.description.text);
    writer.U32(record.type); writer.U32(record.enabled); writer.U32(record.initiallyClosed); writer.U32(record.categoryId);
    // customTextFlag/runOnInit 在转换期间由生成器调整；正文与快照本身另行绑定。
    return Hash(writer.bytes);
}
template<class T> T Take(Result<T> result) {
    if (!result.Succeeded() || !result.value) throw std::runtime_error(result.diagnostics.empty()
        ? "转换快照结构无效。" : result.diagnostics.front().message);
    return std::move(*result.value);
}
Status Failure(std::string message) {
    return Status::Failure("Trigger.Snapshot", "GUI_SNAPSHOT_INVALID", std::move(message));
}
} // namespace

Result<GuiConversionSnapshots::PreparedSnapshot> GuiConversionSnapshots::Prepare(const TriggerDocument& document, const UiCatalog& catalog,
    StableId id, std::string_view generatedCode) {
    try {
        const auto* record = Record(document, id);
        if (!record || id == 0 || !document.HasTrustedSourceSeal() || document.UiCatalogSha256() != catalog.Sha256())
            return {std::nullopt, Failure("转换快照缺少当前触发器或可信函数库来源。").diagnostics};
        Entry entry;
        entry.fragment = Take(TriggerCodeEditor::CaptureGuiFragment(document, id));
        entry.sourceWtg = document.SourceWtgSha256(); entry.sourceWct = document.SourceWctSha256();
        entry.catalogSha = catalog.Sha256(); entry.codeSha = Hash(generatedCode); entry.recordSha = RecordHash(*record);
        Entries prepared;
        prepared.emplace(id, std::move(entry));
        PreparedSnapshot result;
        result.entry_ = prepared.extract(id);
        result.id_ = id;
        return Result<PreparedSnapshot>::Success(std::move(result));
    } catch (const std::exception& error) { return {std::nullopt, Failure(error.what()).diagnostics}; }
}

void GuiConversionSnapshots::Publish(PreparedSnapshot&& snapshot) noexcept {
    Swap(snapshot);
}

void GuiConversionSnapshots::Swap(PreparedSnapshot& snapshot) noexcept {
    if (!snapshot.id_) return;
    auto previous = snapshots_.extract(snapshot.id_);
    if (!snapshot.entry_.empty()) {
        if (!snapshot.entry_.mapped().version) snapshot.entry_.mapped().version = ++nextVersion_;
        snapshots_.insert(std::move(snapshot.entry_));
    }
    snapshot.entry_ = std::move(previous);
    ++revision_;
}

Status GuiConversionSnapshots::Remember(const TriggerDocument& document, const UiCatalog& catalog,
    StableId id, std::string_view generatedCode) {
    auto prepared = Prepare(document, catalog, id, generatedCode);
    if (!prepared.Succeeded() || !prepared.value) return {std::move(prepared.diagnostics)};
    Publish(std::move(*prepared.value));
    return Status::Ok();
}

const GuiTriggerFragment* GuiConversionSnapshots::Find(const TriggerDocument& document,
    const UiCatalog& catalog, StableId id) const {
    try {
        const auto found = snapshots_.find(id);
        const auto* record = Record(document, id);
        if (found == snapshots_.end() || !record || record->type != 0 || record->customTextFlag != 1 || !record->ecas.empty()) return nullptr;
        const auto& entry = found->second;
        if (!document.HasTrustedSourceSeal() || entry.sourceWtg != document.SourceWtgSha256() || entry.sourceWct != document.SourceWctSha256()
            || entry.catalogSha != catalog.Sha256() || entry.recordSha != RecordHash(*record)
            || entry.codeSha != Hash(TriggerCodeEditor::NormalizedText(record->customText))) return nullptr;
        return &entry.fragment;
    } catch (...) { return nullptr; }
}

std::string GuiConversionSnapshots::ActiveFingerprint(const TriggerDocument& document, const UiCatalog& catalog) const {
    std::string result;
    for (const auto& [id, entry] : snapshots_) if (Find(document, catalog, id)) {
        result += std::to_string(id) + ":" + std::to_string(entry.version) + ";";
    }
    return result;
}

Result<std::vector<std::byte>> GuiConversionSnapshots::Serialize(const TriggerDocument& document,
    const UiCatalog& catalog, std::string_view sourceIdentity) const {
    try {
        if (sourceIdentity.empty()) throw std::runtime_error("转换快照缺少工程来源身份。");
        const auto wire = Take(WtgWctCodec{}.Serialize(document, catalog));
        Writer writer; writer.U32(kMagic); writer.U32(kVersion);
        writer.Text(sourceIdentity); writer.Text(catalog.Sha256()); writer.Text(Hash(wire.wtg)); writer.Text(Hash(wire.wct));
        std::uint32_t count = 0;
        for (const auto& record : document.triggers) if (Find(document, catalog, record.id)) ++count;
        writer.U32(count);
        auto savedDocument = Take(WtgWctCodec{}.CreateEmpty(catalog));
        // 所有快照共用一次变量/分类表；大图不能为每个触发器重复保存整张变量表。
        if (count) { savedDocument.categories = document.categories; savedDocument.variables = document.variables; }
        StableId nextId = 1;
        for (auto& category : savedDocument.categories) category.id = nextId++;
        for (auto& variable : savedDocument.variables) variable.id = nextId++;
        for (std::size_t slot = 0; slot < document.triggers.size(); ++slot) {
            const auto& record = document.triggers[slot];
            const auto* fragment = Find(document, catalog, record.id);
            if (!fragment) continue;
            const auto ecaOffset = static_cast<std::uint32_t>(savedDocument.ecaNodes.size());
            const auto argumentOffset = static_cast<std::uint32_t>(savedDocument.argumentNodes.size());
            savedDocument.triggers.push_back(record);
            auto& target = savedDocument.triggers.back();
            // 转码已移除的旧节点身份可能被后续编辑重新使用；临时文档按内容重新编号。
            target.id = nextId++;
            target.customTextFlag = 0; target.customText = {}; target.ecas = fragment->roots; target.runOnInit = fragment->runOnInit ? 1 : 0;
            for (auto& root : target.ecas) root += ecaOffset;
            for (auto eca : fragment->ecas) {
                eca.id = nextId++; eca.sourceTriggerId = target.id;
                for (auto& child : eca.children) child += ecaOffset;
                for (auto& argument : eca.arguments) argument += argumentOffset;
                savedDocument.ecaNodes.push_back(std::move(eca));
            }
            for (auto argument : fragment->arguments) {
                argument.id = nextId++;
                if (argument.callEca) *argument.callEca += ecaOffset;
                if (argument.indexArgument) *argument.indexArgument += argumentOffset;
                savedDocument.argumentNodes.push_back(std::move(argument));
            }
            const auto& entry = snapshots_.at(record.id);
            writer.U32(static_cast<std::uint32_t>(slot)); writer.U64(record.id);
            writer.Text(entry.recordSha); writer.Text(entry.codeSha);
        }
        const auto saved = Take(WtgWctCodec{}.Serialize(savedDocument, catalog));
        writer.Data(saved.wtg); writer.Data(saved.wct);
        // 完整内容摘要发现损坏，不充当安全签名；整个工程仍走 ProjectStore 的来源/内容门禁。
        writer.Text(Hash(writer.bytes));
        return Result<std::vector<std::byte>>::Success(std::move(writer.bytes));
    } catch (const std::exception& error) { return {std::nullopt, Failure(error.what()).diagnostics}; }
}

Result<GuiConversionSnapshots> GuiConversionSnapshots::Parse(std::span<const std::byte> bytes,
    const TriggerDocument& document, const UiCatalog& catalog, std::string_view sourceIdentity) {
    try {
        if (bytes.size() > kMaxBytes || sourceIdentity.empty()) throw std::runtime_error("转换快照预算或工程来源身份无效。");
        Reader reader{bytes};
        if (reader.U32() != kMagic || reader.U32() != kVersion) throw std::runtime_error("转换快照版本不受支持。");
        const auto wire = Take(WtgWctCodec{}.Serialize(document, catalog));
        if (reader.Text() != sourceIdentity || reader.Text() != catalog.Sha256() || reader.Text() != Hash(wire.wtg) || reader.Text() != Hash(wire.wct))
            throw std::runtime_error("转换快照与当前工程来源、触发器顺序、正文或函数库不一致。");
        const auto count = reader.U32();
        if (count > document.triggers.size()) throw std::runtime_error("转换快照记录数无效。");
        GuiConversionSnapshots result;
        std::unordered_set<std::uint64_t> oldIds;
        std::vector<StableId> restoredIds;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto slot = reader.U32(); const auto oldId = reader.U64();
            if (slot >= document.triggers.size() || oldId == 0 || !oldIds.insert(oldId).second)
                throw std::runtime_error("转换快照身份或槽位重复/越界。");
            const auto& record = document.triggers[slot];
            Entry entry; entry.recordSha = reader.Text(); entry.codeSha = reader.Text();
            if (record.customTextFlag != 1 || record.type != 0 || !record.ecas.empty() || entry.recordSha != RecordHash(record)
                || entry.codeSha != Hash(TriggerCodeEditor::NormalizedText(record.customText)))
                throw std::runtime_error("转换快照目标记录或正文已变化。");
            entry.sourceWtg = document.SourceWtgSha256(); entry.sourceWct = document.SourceWctSha256(); entry.catalogSha = catalog.Sha256();
            entry.version = ++result.nextVersion_;
            if (!result.snapshots_.emplace(record.id, std::move(entry)).second) throw std::runtime_error("转换快照目标身份重复。");
            restoredIds.push_back(record.id);
        }
        const auto wtg = reader.Data(); const auto wct = reader.Data();
        const auto saved = Take(WtgWctCodec{}.Parse(wtg, wct, catalog));
        if (saved.triggers.size() != count) throw std::runtime_error("转换快照子树记录数不符。");
        for (std::size_t i = 0; i < saved.triggers.size(); ++i) {
            const auto& record = saved.triggers[i];
            auto& entry = result.snapshots_.at(restoredIds[i]);
            if (record.customTextFlag != 0 || RecordHash(record) != entry.recordSha)
                throw std::runtime_error("转换快照子树不属于绑定的触发器。");
            entry.fragment = Take(TriggerCodeEditor::CaptureGuiFragment(saved, record.id));
        }
        const auto hashedBytes = reader.at;
        if (reader.Text() != Hash(bytes.first(hashedBytes)) || reader.at != bytes.size()) throw std::runtime_error("转换快照损坏或含未知尾部。");
        result.revision_ = result.snapshots_.empty() ? 0 : 1;
        return Result<GuiConversionSnapshots>::Success(std::move(result));
    } catch (const std::exception& error) { return {std::nullopt, Failure(error.what()).diagnostics}; }
}

} // namespace War3Map::Trigger
