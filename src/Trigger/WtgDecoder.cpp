#include "WtgDecoder.h"
#include "TriggerBinaryRead.h"
#include <array>

namespace War3Map::Trigger::Detail {
namespace {
class WtgParser {
public:
    WtgParser(
        std::span<const std::byte> data,
        const UiCatalog& catalog,
        const std::vector<RawText>& slots)
        : reader_(data, "war3map.wtg"), catalog_(catalog), slots_(&slots) {
    }

    WtgParser(std::span<const std::byte> data, const UiCatalog& catalog, const WtgSourceLimits& limits)
        : reader_(data, "war3map.wtg", &limits), catalog_(catalog), limits_(&limits) {
        reader_.Charge(data.size(), 1);
        reader_.Charge(1, sizeof(WtgSourceSnapshot));
    }

    TriggerDocument Parse(const WctHeader& header) {
        ReadSignature();
        const std::int32_t version = reader_.ReadI32("version");
        if (version != kWtgVersion) {
            throw FormatError("war3map.wtg: only version 7 is supported; got " +
                std::to_string(version));
        }

        document_.wct = header;
        const std::size_t categoryCount = reader_.ReadCount("category count", 9);
        document_.categories.reserve(categoryCount);
        for (std::size_t index = 0; index < categoryCount; ++index) {
            document_.categories.push_back(ReadCategory());
        }
        document_.variablesHeader = reader_.ReadI32("variables header");
        const std::size_t variableCount = reader_.ReadCount("variable count", 18);
        document_.variables.reserve(variableCount);
        for (std::size_t index = 0; index < variableCount; ++index) {
            document_.variables.push_back(ReadVariable());
        }
        const std::size_t triggerCount = reader_.ReadCount("trigger count", 33);
        if (triggerCount != slots_->size()) {
            throw FormatError("WTG trigger count " + std::to_string(triggerCount) +
                " does not match WCT slot count " + std::to_string(slots_->size()));
        }
        document_.triggers.reserve(triggerCount);
        for (std::size_t index = 0; index < triggerCount; ++index) {
            document_.triggers.push_back(ReadTrigger(index));
        }
        reader_.ExpectEof();
        return std::move(document_);
    }

    WtgSourceSnapshot ParseSource() {
        ReadSignature();
        const auto first = static_cast<std::uint32_t>(reader_.ReadI32("version or subversion"));
        if (first == 0x80000001u || first == 0x80000004u) {
            source_.subVersionMarker = first;
            version_ = reader_.ReadI32("base version");
        } else if (first == 4 || first == 7) version_ = static_cast<std::int32_t>(first);
        else throw SourceReadError("WTG_SOURCE_VERSION_UNSUPPORTED", 4, "未知 WTG 版本或扩展标记");
        if (version_ != 4 && version_ != 7)
            throw SourceReadError("WTG_SOURCE_VERSION_UNSUPPORTED", 8, "扩展 WTG 的基础版本必须为 4/7");
        source_.baseVersion = version_;
        if (Extended()) {
            for (std::size_t index = 0; index < source_.counts.size(); ++index) {
                auto& group = source_.counts[index];
                const auto start = reader_.Offset();
                group.type = 1u << index;
                group.declaredCount = static_cast<std::int32_t>(reader_.ReadCount("declared type count", 0));
                const auto deleted = reader_.ReadCount("deleted count", 4);
                Items(deleted, sizeof(std::int32_t));
                group.deletedIds.reserve(deleted);
                for (std::size_t n = 0; n < deleted; ++n) group.deletedIds.push_back(reader_.ReadI32("deleted id"));
                group.origin = MakeOrigin("war3map.wtg", start, reader_.Offset());
            }
        } else {
            const auto categories = reader_.ReadCount("category count", version_ == 7 ? 9 : 5);
            Items(categories, sizeof(WtgSourceItem));
            document_.categories.reserve(categories);
            for (std::size_t n = 0; n < categories; ++n) ReadSourceItem(4);
        }
        document_.variablesHeader = reader_.ReadI32("game version");
        const auto variables = reader_.ReadCount("variable count", version_ == 7 ? 19 : 15);
        Items(variables, sizeof(VariableRecord) + sizeof(WtgSourceVariableIdentity));
        document_.variables.reserve(variables);
        source_.variableIdentities.reserve(variables);
        for (std::size_t n = 0; n < variables; ++n) document_.variables.push_back(ReadVariable());
        const auto items = reader_.ReadCount(Extended() ? "item count" : "trigger count", Extended() ? 4 : 26);
        Items(items, sizeof(WtgSourceItem));
        for (std::size_t n = 0; n < items; ++n) {
            const auto type = Extended() ? static_cast<std::uint32_t>(reader_.ReadI32("item type")) : 8u;
            ReadSourceItem(type);
        }
        reader_.ExpectEof();
        source_.document = std::move(document_);
        return std::move(source_);
    }

private:
    bool Extended() const { return source_.subVersionMarker.has_value(); }
    void ReadSignature() {
        const auto signature = reader_.ReadExact(4, "signature");
        constexpr std::array<std::byte, 4> expected{std::byte{'W'}, std::byte{'T'}, std::byte{'G'}, std::byte{'!'}};
        if (!std::equal(signature.begin(), signature.end(), expected.begin())) throw FormatError("war3map.wtg: signature is not WTG!");
    }
    void Items(std::size_t count, std::size_t size) {
        if (count > limits_->maxItems - itemCount_)
            throw SourceReadError("WTG_SOURCE_ITEM_LIMIT", reader_.Offset(), "项目/变量/删除 ID 合计超过预算");
        itemCount_ += count;
        reader_.Charge(count, size * 2); // 容器增长保留容量也计入预算
    }
    void ReadSourceItem(std::uint32_t type) {
        const auto start = reader_.Offset();
        WtgSourceItem item;
        item.type = type;
        currentItem_ = &item;
        if (type == 1 || type == 4) {
            reader_.Charge(2, sizeof(CategoryRecord));
            item.recordIndex = static_cast<std::uint32_t>(document_.categories.size());
            document_.categories.push_back(ReadCategory());
            item.sourceId = document_.categories.back().id;
        } else if (type == 8 || type == 16 || type == 32) {
            reader_.Charge(2, sizeof(TriggerRecord));
            item.recordIndex = static_cast<std::uint32_t>(document_.triggers.size());
            document_.triggers.push_back(ReadTrigger(item.recordIndex));
            item.sourceId = document_.triggers.back().id;
        } else if (type == 64) {
            reader_.Charge(2, sizeof(RawText));
            item.recordIndex = static_cast<std::uint32_t>(source_.variableItems.size());
            item.storedId = reader_.ReadI32("variable item id");
            source_.variableItems.push_back(reader_.ReadCString("variable item name"));
            item.parentId = reader_.ReadI32("variable item parent");
            item.sourceId = MakeSourceId(0x16, start);
        } else throw SourceReadError("WTG_SOURCE_ITEM_TYPE_UNSUPPORTED", start - 4,
            "未知项目类型没有可跳过长度：" + std::to_string(type));
        item.origin = MakeOrigin("war3map.wtg", start, reader_.Offset());
        source_.items.push_back(std::move(item));
        currentItem_ = nullptr;
    }

    CategoryRecord ReadCategory() {
        const std::size_t start = reader_.Offset();
        CategoryRecord result;
        result.id = MakeSourceId(0x11, start);
        result.categoryId = reader_.ReadI32("category id");
        result.name = reader_.ReadCString("category name");
        if (version_ >= 7) result.comment = reader_.ReadI32("category comment");
        if (currentItem_) currentItem_->storedId = result.categoryId;
        if (Extended()) {
            currentItem_->expandedRaw = reader_.ReadI32("category expanded");
            currentItem_->parentId = reader_.ReadI32("category parent");
        }
        result.origin = MakeOrigin("war3map.wtg", start, reader_.Offset());
        return result;
    }

    VariableRecord ReadVariable() {
        const std::size_t start = reader_.Offset();
        VariableRecord result;
        result.id = MakeSourceId(0x12, start);
        result.name = reader_.ReadCString("variable name");
        result.typeName = reader_.ReadCString("variable type");
        result.unknown = reader_.ReadI32("variable unknown");
        result.isArray = reader_.ReadI32("variable array");
        if (version_ >= 7) result.size = reader_.ReadI32("variable size");
        result.hasDefault = reader_.ReadI32("variable has default");
        result.value = reader_.ReadCString("variable default");
        if (limits_) {
            WtgSourceVariableIdentity identity;
            if (Extended()) {
                identity.storedId = reader_.ReadI32("variable id");
                identity.parentId = reader_.ReadI32("variable parent");
            }
            source_.variableIdentities.push_back(identity);
        }
        result.origin = MakeOrigin("war3map.wtg", start, reader_.Offset());
        return result;
    }

    TriggerRecord ReadTrigger(std::size_t index) {
        const std::size_t start = reader_.Offset();
        TriggerRecord result;
        result.id = MakeSourceId(0x13, start);
        currentTriggerId_ = result.id;
        result.name = reader_.ReadCString("trigger name");
        result.description = reader_.ReadCString("trigger description");
        if (version_ >= 7) result.type = reader_.ReadI32("trigger type");
        if (Extended()) currentItem_->storedId = reader_.ReadI32("trigger id");
        result.enabled = reader_.ReadI32("trigger enabled");
        result.customTextFlag = reader_.ReadI32("trigger custom text");
        result.initiallyClosed = reader_.ReadI32("trigger initially closed");
        result.runOnInit = reader_.ReadI32("trigger run on init");
        result.categoryId = reader_.ReadI32("trigger category");
        if (currentItem_) currentItem_->parentId = result.categoryId;
        const std::size_t count = reader_.ReadCount("root ECA count", version_ >= 7 ? 13 : 9);
        reader_.Charge(count, sizeof(std::uint32_t));
        result.ecas.reserve(count);
        for (std::size_t ecaIndex = 0; ecaIndex < count; ++ecaIndex) {
            result.ecas.push_back(ReadEca(false, 0));
        }
        if (slots_) result.customText = (*slots_)[index];
        result.origin = MakeOrigin("war3map.wtg", start, reader_.Offset());
        return result;
    }

    std::uint32_t ReadEca(bool child, std::size_t depth) {
        if (depth > (limits_ ? limits_->maxDepth : 256)) {
            if (limits_) throw SourceReadError("WTG_SOURCE_DEPTH_LIMIT", reader_.Offset(), "ECA 嵌套超过预算");
            throw FormatError("war3map.wtg@" + std::to_string(reader_.Offset()) +
                ": ECA nesting exceeds " + std::to_string(limits_ ? limits_->maxDepth : 256) + " levels");
        }
        const std::size_t start = reader_.Offset();
        CheckNodeBudget(sizeof(EcaNode));
        if (document_.ecaNodes.size() >= kInvalidNodeIndex) {
            throw FormatError("ECA node count exceeds uint32 index capacity");
        }
        const std::uint32_t nodeIndex = static_cast<std::uint32_t>(document_.ecaNodes.size());
        document_.ecaNodes.emplace_back();
        EcaNode node;
        node.id = MakeSourceId(0x14, start);
        node.sourceTriggerId = currentTriggerId_;
        node.type = reader_.ReadI32("ECA type");
        if (child) {
            node.childId = reader_.ReadI32("ECA child id");
        }
        node.name = reader_.ReadCString("ECA name");
        node.enabled = reader_.ReadI32("ECA enabled");
        const UiDefinition* definition = catalog_.Find(node.type, node.name.text);
        if (definition == nullptr) {
            throw MissingUiDefinitionError(
                start, reader_.Offset(), node.type, node.name.text);
        }
        node.uiSource = definition->Active().source;
        node.uiLayer = definition->Active().layer;
        node.uiSignature = definition->Signature();
        reader_.Charge(node.uiSignature.size() + node.uiLayer.size() + node.uiSource.native().size() * sizeof(wchar_t), 2);
        reader_.Charge(definition->ArgumentCount(), sizeof(std::uint32_t));
        node.arguments.reserve(definition->ArgumentCount());
        for (std::size_t index = 0; index < definition->ArgumentCount(); ++index) {
            node.arguments.push_back(ReadArgument(depth + 1));
        }
        const std::size_t childCount = version_ >= 7 ? reader_.ReadCount("child ECA count", 17) : 0;
        reader_.Charge(childCount, sizeof(std::uint32_t));
        node.children.reserve(childCount);
        for (std::size_t index = 0; index < childCount; ++index) {
            node.children.push_back(ReadEca(true, depth + 1));
        }
        node.origin = MakeOrigin("war3map.wtg", start, reader_.Offset());
        document_.ecaNodes[nodeIndex] = std::move(node);
        return nodeIndex;
    }

    std::uint32_t ReadArgument(std::size_t depth) {
        if (depth > (limits_ ? limits_->maxDepth : 256)) {
            if (limits_) throw SourceReadError("WTG_SOURCE_DEPTH_LIMIT", reader_.Offset(), "参数嵌套超过预算");
            throw FormatError("war3map.wtg@" + std::to_string(reader_.Offset()) +
                ": argument nesting exceeds " + std::to_string(limits_ ? limits_->maxDepth : 256) + " levels");
        }
        const std::size_t start = reader_.Offset();
        CheckNodeBudget(sizeof(ArgumentNode));
        if (document_.argumentNodes.size() >= kInvalidNodeIndex) {
            throw FormatError("argument node count exceeds uint32 index capacity");
        }
        const std::uint32_t nodeIndex = static_cast<std::uint32_t>(document_.argumentNodes.size());
        document_.argumentNodes.emplace_back();
        ArgumentNode node;
        node.id = MakeSourceId(0x15, start);
        node.type = reader_.ReadI32("argument type");
        node.value = reader_.ReadCString("argument value");
        node.insertCall = reader_.ReadI32("argument insert call");
        if (limits_ && node.insertCall != 0 && node.insertCall != 1)
            throw SourceReadError("WTG_SOURCE_CALL_FLAG_INVALID", reader_.Offset() - 4, "嵌套函数标志必须为 0/1");
        if (node.insertCall == 1) {
            node.callEca = ReadEca(false, depth + 1);
        }
        node.insertIndex = reader_.ReadI32("argument insert index");
        if (limits_ && node.insertIndex != 0 && node.insertIndex != 1)
            throw SourceReadError("WTG_SOURCE_INDEX_FLAG_INVALID", reader_.Offset() - 4, "数组下标标志必须为 0/1");
        if (node.insertIndex == 1) {
            node.indexArgument = ReadArgument(depth + 1);
        }
        node.origin = MakeOrigin("war3map.wtg", start, reader_.Offset());
        document_.argumentNodes[nodeIndex] = std::move(node);
        return nodeIndex;
    }

    Reader reader_;
    const UiCatalog& catalog_;
    const std::vector<RawText>* slots_ = nullptr;
    TriggerDocument document_;
    StableId currentTriggerId_ = 0;
    const WtgSourceLimits* limits_ = nullptr;
    WtgSourceSnapshot source_;
    WtgSourceItem* currentItem_ = nullptr;
    std::int32_t version_ = 7;
    std::size_t itemCount_ = 0;

    void CheckNodeBudget(std::size_t size) {
        if (!limits_) return;
        if (document_.ecaNodes.size() + document_.argumentNodes.size() >= limits_->maxNodes)
            throw SourceReadError("WTG_SOURCE_NODE_LIMIT", reader_.Offset(), "ECA/参数合计超过预算");
        reader_.Charge(2, size);
    }
};

} // namespace
TriggerDocument DecodeClassicWtg(std::span<const std::byte> bytes, const UiCatalog& catalog,
    const std::vector<RawText>& slots, const WctHeader& header) {
    return WtgParser(bytes, catalog, slots).Parse(header);
}
WtgSourceSnapshot DecodeWtgSource(std::span<const std::byte> bytes, const UiCatalog& catalog,
    const WtgSourceLimits& limits) {
    return WtgParser(bytes, catalog, limits).ParseSource();
}
} // namespace War3Map::Trigger::Detail
