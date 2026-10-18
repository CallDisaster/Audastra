#include "War3MapCore/Trigger/WtgSource.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace {
using namespace War3Map::Trigger;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Fixture {
    std::vector<std::byte> bytes;
    std::size_t firstType = 0;
    void I32(std::int32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>(static_cast<std::uint32_t>(value) >> shift));
    }
    void Text(std::string_view value) { for (char c : value) bytes.push_back(static_cast<std::byte>(c)); bytes.push_back(std::byte{0}); }
    Fixture(std::int32_t version, std::uint32_t marker, std::string_view action = "DoNothing", std::string_view triggerName = "Gui") {
        for (char c : std::string_view("WTG!")) bytes.push_back(static_cast<std::byte>(c));
        if (marker) I32(std::bit_cast<std::int32_t>(marker));
        I32(version);
        const auto category = [&](int id) {
            I32(id); Text("Folder"); if (version == 7) I32(1);
            if (marker) { I32(1); I32(-1); }
        };
        if (marker) {
            for (int i = 0; i < 8; ++i) { I32(99); I32(i == 1 ? 2 : 0); if (i == 1) { I32(-1); I32(123); } }
        } else { I32(1); category(7); }
        I32(2); I32(1); Text("v"); Text("integer"); I32(9); I32(1);
        if (version == 7) I32(3);
        I32(1); Text("5"); if (marker) { I32(64); I32(7); }
        I32(marker ? 6 : 1);
        if (marker) { firstType = bytes.size(); I32(1); category(7); I32(64); I32(64); Text("v"); I32(7); I32(8); }
        const auto trigger = [&](std::string_view name, int comment, int custom, int count) {
            Text(name); Text("note"); if (version == 7) I32(comment);
            if (marker) I32(7); // 故意与分类 ID 重复；来源偏移身份必须仍然唯一
            I32(1); I32(custom); I32(1); I32(0); I32(7); I32(count);
            for (int i = 0; i < count; ++i) { I32(2); Text(action); I32(1); if (version == 7) I32(0); }
        };
        trigger(triggerName, 0, 0, 4); // v4 四个零参数根 ECA，不能误读 childCount
        if (marker) { I32(4); category(9); I32(16); trigger("Comment",1,0,0); I32(32); trigger("Code",0,1,0); }
    }
};
bool Field(const WtgSourceRow& row, std::string_view key, std::string_view value) {
    return std::any_of(row.fields.begin(), row.fields.end(), [&](const auto& field) { return field.name == key && field.value == value; });
}
}

void TestWtgSource(const War3Map::Trigger::UiCatalog& catalog, std::span<const std::byte> classicWtg,
    std::span<const std::byte> classicWct) {
    // 1. 真实版本布局、原始树与删除编号、来源身份和写入隔离。
    for (const auto version : {4, 7}) for (const auto marker : {0u, 0x80000001u, 0x80000004u}) {
        const Fixture fixture(version, marker);
        auto parsed = ParseWtgSource(fixture.bytes, catalog);
        Require(parsed.Succeeded(), "WTG 来源版本读取失败");
        const auto data = parsed.value->Snapshot();
        Require(std::ranges::equal(parsed.value->OriginalBytes(), fixture.bytes) && data->baseVersion == version
            && data->subVersionMarker.value_or(0) == marker && data->document.ecaNodes.size() == 4, "WTG 原始布局或 ECA 消费错误");
        Require(!data->document.HasTrustedSourceSeal() && !WtgWctCodec{}.Serialize(data->document,catalog).Succeeded(), "只读来源获得了保存权限");
        auto page = QueryWtgSource(*parsed.value,data->sourceSha256,data->catalogSha256,WtgSourceTable::Items,0,128);
        Require(page.Succeeded() && page.value->totalRows == (marker ? 6u : 2u), "树项目计数错误");
        if (marker) {
            Require(data->counts[1].declaredCount == 99 && data->counts[1].deletedIds == std::vector<std::int32_t>{-1,123}, "删除计数被规范化");
            Require(data->items[0].sourceId != data->items[2].sourceId && data->items[0].storedId == data->items[2].storedId, "存盘 ID 不能替代来源身份");
            auto deleted=QueryWtgSource(*parsed.value,data->sourceSha256,data->catalogSha256,WtgSourceTable::DeletedIds,1,1);
            Require(deleted.Succeeded() && Field(deleted.value->rows[0],"stored_id","123") && deleted.value->rows[0].origin.length==4, "删除 ID 页或源范围错误");
        }
        for (std::size_t end=0; end<fixture.bytes.size(); ++end)
            Require(!ParseWtgSource(std::span<const std::byte>(fixture.bytes).first(end),catalog).Succeeded(), "截断 WTG 返回了来源快照");
    }
    // 2. ECA 共用解析、签名/预算/未知布局及查询拒绝，失败不返回半棵树。
    const auto classic=ParseWtgSource(classicWtg,catalog); const auto paired=WtgWctCodec{}.Parse(classicWtg,classicWct,catalog);
    Require(classic.Succeeded() && paired.Succeeded(), "经典双入口无法读取");
    const auto& left=classic.value->Snapshot()->document; const auto& right=*paired.value;
    Require(left.ecaNodes.size()==right.ecaNodes.size() && left.argumentNodes.size()==right.argumentNodes.size(), "共用 ECA 节点数量改变");
    for (std::size_t i=0;i<left.ecaNodes.size();++i)
        Require(left.ecaNodes[i].id==right.ecaNodes[i].id && left.ecaNodes[i].arguments==right.ecaNodes[i].arguments
            && left.ecaNodes[i].name.originalBytes==right.ecaNodes[i].name.originalBytes, "经典节点身份或原名改变");
    auto bytes=Fixture(7,0x80000004u).bytes; bytes.push_back(std::byte{0});
    Require(!ParseWtgSource(bytes,catalog).Succeeded(), "尾随未知字节被忽略");
    Fixture unknown(7,0x80000004u); unknown.bytes[unknown.firstType]=std::byte{2};
    Require(!ParseWtgSource(unknown.bytes,catalog).Succeeded() && !ParseWtgSource(Fixture(7,0x80000003u).bytes,catalog).Succeeded(), "未知类型/标记被猜测");
    auto missing=ParseWtgSource(Fixture(7,0x80000004u,"NoSuchFunction").bytes,catalog);
    Require(!missing.value && missing.diagnostics.front().code=="WTG_SOURCE_SIGNATURE_MISSING" && missing.diagnostics.front().origin.has_value(), "缺签名未定位阻断");
    for (int budget=0;budget<5;++budget) {
        WtgSourceLimits limits;
        if(budget==0) limits.maxDecodedBytes=128; else if(budget==1) limits.maxItems=0;
        else if(budget==2) limits.maxNodes=0; else if(budget==3) limits.maxStringBytes=0; else limits.maxBytes=0;
        Require(!ParseWtgSource(classicWtg,catalog,limits).Succeeded(), "收紧预算未生效");
    }
    // 显式提升的是节点预算；来源/解码/深度和完整分页并没有跟着放宽。
    const Fixture bounded(7,0); WtgSourceLimits raised; raised.maxNodes=285746;
    const auto configured=ParseWtgSource(bounded.bytes,catalog,raised);
    Require(configured.Succeeded() && configured.value->Snapshot()->document.ecaNodes.size()==4, "显式节点预算未接通");
    raised.maxNodes=WtgSourceLimits::hardMaxNodes+1;
    const auto overHard=ParseWtgSource(bounded.bytes,catalog,raised);
    Require(!overHard.value && overHard.diagnostics.front().code=="WTG_SOURCE_LIMITS", "节点硬预算被取消");
    raised.maxNodes=3; Require(!ParseWtgSource(bounded.bytes,catalog,raised).value, "节点超预算返回了部分索引");
    raised.maxNodes=4; const auto exact=ParseWtgSource(bounded.bytes,catalog,raised);
    Require(exact.Succeeded(), "精确节点边界拒绝");
    const auto last=QueryWtgSource(*exact.value,exact.value->Snapshot()->sourceSha256,catalog.Sha256(),WtgSourceTable::Ecas,3,1);
    Require(last.Succeeded() && last.value->totalRows==4 && last.value->rows.size()==1 && Field(last.value->rows[0],"eca_type","2"),
        "显式预算改变原始类型或分页总数");
    raised.maxNodes=285746; raised.maxDecodedBytes=128;
    Require(!ParseWtgSource(bounded.bytes,catalog,raised).value, "提升节点数放开了解码预算");
    const auto data=classic.value->Snapshot();
    Require(!QueryWtgSource(*classic.value,std::string(64,'0'),data->catalogSha256,WtgSourceTable::Items,0,1).Succeeded()
        && !QueryWtgSource(*classic.value,data->sourceSha256,std::string(64,'0'),WtgSourceTable::Items,0,1).Succeeded()
        && !QueryWtgSource(*classic.value,data->sourceSha256,data->catalogSha256,WtgSourceTable::Items,0,129).Succeeded(), "身份/页预算未拒绝");
    const auto large=ParseWtgSource(Fixture(7,0x80000004u,"DoNothing",std::string(600000,'x')).bytes,catalog);
    Require(large.Succeeded() && !QueryWtgSource(*large.value,large.value->Snapshot()->sourceSha256,catalog.Sha256(),WtgSourceTable::Items,2,1).Succeeded(), "大行查询返回了超预算结果");
    Require(WtgSourceLimits{}.maxNodes==WtgSourceLimits::hardMaxNodes, "默认节点预算与触发器事务不一致");

    // 3. 按触发器查询：只返回该触发器的 ECA/参数，行号仍是全表下标；分类/变量行和其它表拒绝。
    const auto marked=ParseWtgSource(Fixture(7,0x80000004u).bytes,catalog); Require(marked.Succeeded(), "扩展来源读取失败");
    const auto markedData=marked.value->Snapshot();
    const auto byTrigger=[&](const WtgSource& source, std::size_t item, WtgSourceTable table, std::size_t start, std::size_t limit) {
        const auto snapshot=source.Snapshot();
        return QueryWtgSourceTrigger(source,snapshot->sourceSha256,snapshot->catalogSha256,item,table,start,limit);
    };
    std::optional<std::size_t> guiItem;
    for (std::size_t i=0;i<markedData->items.size();++i) if (markedData->items[i].type==8) guiItem=i;
    Require(guiItem.has_value(), "扩展来源缺少触发器行");
    const auto guiEcas=byTrigger(*marked.value,*guiItem,WtgSourceTable::Ecas,1,2);
    Require(guiEcas.Succeeded() && guiEcas.value->totalRows==4 && guiEcas.value->rows.size()==2
        && guiEcas.value->rows[0].index==1 && guiEcas.value->rows[1].index==2, "触发器 ECA 页或全表行号错误");
    const auto guiArguments=byTrigger(*marked.value,*guiItem,WtgSourceTable::Arguments,0,128);
    Require(guiArguments.Succeeded() && guiArguments.value->totalRows==0, "零参数触发器返回了参数");
    Require(byTrigger(*marked.value,0,WtgSourceTable::Ecas,0,1).diagnostics.front().code=="WTG_SOURCE_TRIGGER_ITEM"
        && byTrigger(*marked.value,markedData->items.size(),WtgSourceTable::Ecas,0,1).diagnostics.front().code=="WTG_SOURCE_TRIGGER_ITEM"
        && byTrigger(*marked.value,*guiItem,WtgSourceTable::Items,0,1).diagnostics.front().code=="WTG_SOURCE_TABLE"
        && byTrigger(*marked.value,*guiItem,WtgSourceTable::Ecas,5,1).diagnostics.front().code=="WTG_SOURCE_QUERY_BOUNDS"
        && !QueryWtgSourceTrigger(*marked.value,std::string(64,'0'),markedData->catalogSha256,*guiItem,WtgSourceTable::Ecas,0,1).Succeeded(),
        "按触发器查询的拒绝条件错误");
    // 经典样例：各触发器的节点合起来恰好覆盖全表，每个节点只属于一个触发器。
    std::vector<int> ecaOwners(data->document.ecaNodes.size()), argumentOwners(data->document.argumentNodes.size());
    for (std::size_t item=0;item<data->items.size();++item) {
        const auto type=data->items[item].type;
        if (type==1 || type==4 || type==64) continue;
        for (const auto table : {WtgSourceTable::Ecas, WtgSourceTable::Arguments}) {
            auto& owners = table==WtgSourceTable::Ecas ? ecaOwners : argumentOwners;
            for (std::size_t start=0;;start+=128) {
                const auto page=byTrigger(*classic.value,item,table,start,128);
                Require(page.Succeeded(), "经典触发器分页失败");
                for (const auto& row : page.value->rows) ++owners[row.index];
                if (start+128>=page.value->totalRows) break;
            }
        }
    }
    Require(std::ranges::all_of(ecaOwners,[](int count){ return count==1; })
        && std::ranges::all_of(argumentOwners,[](int count){ return count==1; }), "按触发器的节点没有恰好覆盖全表");
}
