#include "McpMapDataTools.h"
#include "McpMapDataJson.h"
#include "War3MapCore/Hash.h"
#include "../MapDataFixtures.h"
#include "../MapDataSpatialFixtures.h"

#include <QJsonDocument>

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Mcp;
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
QString Code(const QJsonObject& result) {
    const auto diagnostics = result.value("diagnostics").toArray();
    return diagnostics.isEmpty() ? QString{} : diagnostics.first().toObject().value("code").toString();
}
std::vector<MapArchiveMember> Members(std::size_t count = 3, bool largeNames = false) {
    RegionsDocument regions;
    for (std::size_t row = 0; row < count; ++row) {
        RegionRecord region{}; region.left = -10; region.bottom = -20; region.right = 30; region.top = 40;
        region.name = largeNames ? std::string(8180, static_cast<char>('A' + row % 26)) : "Original Region " + std::to_string(row);
        region.creationNumber = static_cast<std::int32_t>(row + 1); regions.entries.push_back(std::move(region));
    }
    CamerasDocument cameras; CameraRecord camera{}; camera.name = "Original Camera"; camera.targetDistance = 1600;
    camera.fieldOfView = 70; camera.farClippingPlane = 5000; camera.nearClippingPlane = 100; cameras.entries.push_back(camera);
    UnitPlacements units; UnitPlacement unit; unit.rawcode = "hfoo"; unit.scale = {1, 1, 1}; unit.creationId = 17; units.units.push_back(unit);
    DoodadPlacements doodads; DoodadPlacement doodad; doodad.rawcode = "LTlt"; doodad.scale = {1, 1, 1}; doodad.creationId = 9; doodads.doodads.push_back(doodad);
    SpecialDoodadPlacement special; special.rawcode = "LTlt"; special.position = {3, 4}; doodads.specialDoodads.push_back(special);
    const auto rb = regions.Serialize(), cb = cameras.Serialize(), ub = units.Serialize(), db = doodads.Serialize();
    Require(rb.Succeeded() && cb.Succeeded() && ub.Succeeded() && db.Succeeded(), "serialize original synthetic entity members");
    return {{"war3map.w3i", MapDataFixtures::FullMapSettingsV25().bytes}, {"war3mapUnits.doo", *ub.value},
        {"war3map.doo", *db.value}, {"war3map.w3r", *rb.value}, {"war3map.w3c", *cb.value}};
}
struct Fixture {
    std::shared_ptr<const MapDocument> original, current;
    std::unique_ptr<MapDataEditSession> session;
    MapDataToolHost host;
    bool independent = false, active = true, readOnly = false, rejectPublish = false, opaque = false;
    std::uint64_t generation = 0;
    int attempts = 0, validates = 0, publishes = 0;
    QJsonObject hostChanges;
    explicit Fixture(bool standalone = false, std::vector<MapArchiveMember> members = Members()) : independent(standalone) {
        original = std::make_shared<const MapDocument>(MapDocument::FromMembers(members)); current = original;
        session = std::make_unique<MapDataEditSession>(*original);
        host.identity = [&] {
            QJsonObject identity{{"project_identity", "synthetic-entity-map"}, {"revision", QStringLiteral("entity:%1:%2").arg(generation).arg(active ? 1 : 0)}};
            if (independent) {
                identity.insert("transaction_active", active); identity.insert("transaction_id", active ? "7" : "0");
                identity.insert("transaction_revision", QJsonObject{{"generation", "0"}, {"source_sha256", QString(64, 'a')}, {"ui_catalog_sha256", QString(64, 'b')}});
            }
            return identity;
        };
        host.context = [&] {
            if (!independent) {
                auto projected = ProjectMapData(*original, *session);
                if (!projected.Succeeded()) return Result<MapDataToolContext>{{}, projected.diagnostics};
                current = std::make_shared<const MapDocument>(std::move(*projected.value));
            }
            return Result<MapDataToolContext>::Success({original, current, {"synthetic-entities", 1, generation, std::string(64, 'a')}, opaque, independent && active, !independent});
        };
        host.apply = [&](const PreparedMapDataFields& plan, const MapDataPrepublishValidator& validate) -> Result<MapDataEditSession::EditReceipt> {
            ++attempts;
            if (readOnly) return Result<MapDataEditSession::EditReceipt>::Failure("Test", "HOST_READ_ONLY", "只读宿主。");
            if (independent && !active) return Result<MapDataEditSession::EditReceipt>::Failure("Test", "ENTITY_ACTIVE_TRANSACTION_REQUIRED", "没有活动事务。");
            auto nextDocument = std::make_shared<const MapDocument>(plan.After());
            std::unique_ptr<MapDataEditSession> candidate;
            auto* target = session.get();
            if (independent) {
                const auto frozen = FreezeOriginalMapDataFields(*original, plan.After());
                if (!frozen.Succeeded()) return {{}, frozen.diagnostics};
                candidate = std::make_unique<MapDataEditSession>(*original);
                const auto existing = FreezeOriginalMapDataFields(*original, *current);
                if (!existing.Succeeded()) return {{}, existing.diagnostics};
                for (const auto& member : existing.value->Members()) {
                    const auto found = std::find_if(original->Members().begin(), original->Members().end(),
                        [&](const auto& input) { return input.memberName == member.name; });
                    Require(found != original->Members().end(), "原冻结成员须在真实来源目录");
                    const auto seeded = candidate->SetSerialized(found->kind, member.data); if (!seeded.Succeeded()) return {{}, seeded.diagnostics};
                }
                target = candidate.get();
            }
            const auto status = target->ApplyPreparedFields(plan, [&](const auto& before, const auto& after, const auto& receipt) {
                ++validates;
                const auto accepted = validate(before, after, receipt, hostChanges); if (!accepted.Succeeded()) return accepted;
                return rejectPublish ? Status::Failure("Test", "HOST_PUBLICATION_DENIED", "候选发布前拒绝。") : Status::Ok();
            });
            if (!status.Succeeded()) return {{}, status.diagnostics};
            const auto receipt = target->LastEdit();
            if (independent) session.swap(candidate);
            current.swap(nextDocument); ++generation; ++publishes;
            return Result<MapDataEditSession::EditReceipt>::Success(receipt);
        };
    }
    QJsonObject IdentityArguments() const {
        const auto identity = host.identity(); return {{"project_identity", identity.value("project_identity")}, {"expected_revision", identity.value("revision")}};
    }
    QJsonObject Query(const char* kind = "regions") {
        auto arguments = IdentityArguments(); arguments.insert("kind", kind); return InvokeMapDataTool("entities_query", arguments, host);
    }
    QJsonObject Change(std::size_t row, const char* fieldId, const char* input, MapDataKind kind = MapDataKind::Regions) const {
        const auto* field = FindMapField(kind, fieldId); Require(field != nullptr, "real shared field for request");
        const auto key = MapFieldRowKey(*current, kind, row); const auto before = ReadMapField(*current, kind, row, *field);
        Require(key.Succeeded() && before.Succeeded(), "actual source index, row key and exact raw before");
        return {{"source_index", QString::number(row)}, {"row_key", QString::fromStdString(*key.value)}, {"field", fieldId},
            {"expected_raw", QString::fromStdString(FormatMapFieldRaw(*before.value))}, {"value", input}};
    }
    QJsonObject Request(QJsonArray changes, const char* kind = "regions") const {
        auto arguments = IdentityArguments(); arguments.insert("kind", kind);
        const auto parsed = ParseMapDataKindId(kind); Require(parsed.has_value(), "native kind");
        const auto sha = Sha256(current->Member(*parsed).sources.front().data); Require(sha.Succeeded(), "request actual current member sha");
        arguments.insert("expected_member_sha256", QString::fromStdString(*sha.value)); arguments.insert("changes", changes); return arguments;
    }
};
void Unchanged(Fixture& fixture, const QByteArray& before, int publishes, std::uint64_t revision, std::uint64_t command) {
    Require(QJsonDocument(fixture.Query()).toJson(QJsonDocument::Compact) == before && fixture.publishes == publishes
        && fixture.session->Revision() == revision && fixture.session->LastEdit().CommandIdentity() == command,
        "failure preserves full current query, native bytes, revision, command and publication count");
}

std::vector<MapArchiveMember> DMembers(std::string_view wts = "// keep\r\nSTRING 42\r\n{\r\nOriginal description\r\n}\r\nSTRING 7\r\n{\r\nUnaffected\r\n}\r\n") {
    auto members = Members();
    members.push_back({"war3map.wts", {reinterpret_cast<const std::byte*>(wts.data()), reinterpret_cast<const std::byte*>(wts.data() + wts.size())}});
    return members;
}
QJsonObject DRequest(Fixture& f, bool strings, QJsonArray changes) {
    auto result = f.IdentityArguments();
    const auto kind = strings ? MapDataKind::Strings : MapDataKind::Settings;
    const auto sha = Sha256(f.current->Member(kind).sources.front().data); Require(sha.Succeeded(), "真实D成员SHA");
    result.insert("expected_member_sha256", QString::fromStdString(*sha.value)); result.insert("changes", changes);
    result.insert("write_mode", strings ? "shared_string" : "literal");
    if (!strings) result.insert("setting_id", "w3i");
    return result;
}
QJsonObject DChange(Fixture& f, bool strings, std::size_t row, const char* field, const char* value) {
    auto result = f.Change(row, field, value, strings ? MapDataKind::Strings : MapDataKind::Settings);
    result.remove("source_index"); result.remove("row_key");
    if (strings) { result.remove("field"); result.insert("string_id", QString::number(f.current->Strings()->entries[row].id)); }
    return result;
}
QJsonObject DField(const QJsonObject& query, const char* id) {
    const auto rows = query.value("rows").toArray(); Require(rows.size() == 1, "实际设置唯一行");
    for (const auto& value : rows.first().toObject().value("fields").toArray())
        if (value.toObject().value("id") == QLatin1String(id)) return value.toObject();
    throw std::runtime_error("D字段未返回");
}

void DWireLiteralSharedStringsAndOriginalUndo() {
    {
        const std::string legacy = "STRING 42\r\n{\r\n|cffff0000\xba\xec\xc9\xab|r\r\n}\r\n";
        Fixture oldBytes(false, DMembers(legacy));
        const auto read = InvokeMapDataTool("map_settings_get", oldBytes.IdentityArguments(), oldBytes.host);
        const auto description = DField(read, "description");
        Require(read.value("ok").toBool() && description.value("raw_value") == "TRIGSTR_0042"
            && description.value("value") == QStringLiteral("|cffff0000红色|r") && description.value("name_state") == "resolved"
            && oldBytes.session->Revision() == 0 && oldBytes.publishes == 0, "legacy WTS is decoded for MCP without losing formatting or changing source");
    }
    for (const bool independent : {false, true}) {
        Fixture reference(independent, DMembers());
        const auto read = InvokeMapDataTool("map_settings_get", reference.IdentityArguments(), reference.host);
        const auto description = DField(read, "description");
        Require(read.value("ok").toBool() && read.value("setting_id") == "w3i" && read.value("total") == "1"
            && !read.value("truncated").toBool() && description.value("wire_value") == "TRIGSTR_0042"
            && description.value("value") == "Original description" && description.value("resolution") == "resolved",
            "D查询区分原wire引用、精确raw与真实解析文字");
        auto literalMembers = DMembers();
        auto literal = MapSettings::Parse(literalMembers.front().data);
        Require(literal.Succeeded(), "旧 literal 断言由完整 W3I 建立非引用来源");
        literal.value->description = "Original description";
        const auto literalBytes = literal.value->Serialize(); Require(literalBytes.Succeeded(), "literal 来源真实写出");
        literalMembers.front().data = *literalBytes.value;
        Fixture f(independent, literalMembers);
        const auto oldW3i = f.original->Member(MapDataKind::Settings).sources.front().data;
        const auto oldWts = f.original->Member(MapDataKind::Strings).sources.front().data;
        const auto request = DRequest(f, false, {DChange(f, false, 0, "author", "hex:ff"), DChange(f, false, 0, "description", "独立描述")});
        const auto changed = InvokeMapDataTool("map_settings_set", request, f.host);
        Require(changed.value("ok").toBool() && changed.value("write_mode") == "literal"
            && !changed.value("changes_shared_references").toBool() && !changed.value("saved").toBool()
            && f.current->Settings()->author == "hex:ff" && f.current->Settings()->description == "独立描述"
            && f.current->Member(MapDataKind::Strings).sources.front().data == oldWts,
            "literal把hex:前缀视为正文；W3I文字编辑不改任何共享WTS");
        Require(Code(InvokeMapDataTool("map_settings_set", request, f.host)) == "ENTITY_REVISION_CONFLICT", "旧D写请求严格修订冲突");
        if (!independent) {
            const auto receipt = f.session->LastEdit();
            Require(f.session->Replay(receipt, false).Succeeded()
                && DField(InvokeMapDataTool("map_settings_get", f.IdentityArguments(), f.host), "description").value("value") == "Original description"
                && f.current->Member(MapDataKind::Settings).sources.front().data == oldW3i,
                "原Undo恢复完整W3I原字节及TRIGSTR解析");
        } else {
            auto restore = DRequest(f, false, {DChange(f, false, 0, "description", "unused")});
            auto change = restore.value("changes").toArray().first().toObject(); change.insert("value", "TRIGSTR_0042");
            restore.insert("changes", QJsonArray{change});
            Require(Code(InvokeMapDataTool("map_settings_set", restore, f.host)) == "MAP_FIELD_VALUE_INVALID",
                "literal工具不能伪造原TRIGSTR引用；恢复引用由原Undo处理");
        }
        const auto settingsBeforeShared = f.current->Member(MapDataKind::Settings).sources.front().data;
        const auto shared = InvokeMapDataTool("strings_set", DRequest(f, true, {DChange(f, true, 0, "value", "TRIGSTR_7")}), f.host);
        Require(shared.value("ok").toBool() && shared.value("changes_shared_references").toBool()
            && shared.value("write_mode") == "shared_string" && f.current->Member(MapDataKind::Settings).sources.front().data == settingsBeforeShared
            && f.current->ResolveString("TRIGSTR_42").value == std::optional<std::string>("TRIGSTR_7")
            && f.current->Strings()->entries[1].value == "Unaffected", "WTS共享改写不改wire、不递归正文TRIGSTR且保留其它ID");
        const auto queried = InvokeMapDataTool("strings_query", f.IdentityArguments(), f.host);
        Require(queried.value("ok").toBool() && queried.value("total") == "2"
            && queried.value("rows").toArray().first().toObject().value("string_id") == "42"
            && queried.value("rows").toArray().first().toObject().value("name").toObject().value("display_utf8") == "TRIGSTR_7",
            "strings_query返回稳定ID与literal正文");
        if (!independent) {
            const auto receipt = f.session->LastEdit();
            Require(f.session->Replay(receipt, false).Succeeded(), "原WTS Undo可回放");
            (void)InvokeMapDataTool("strings_query", f.IdentityArguments(), f.host);
            Require(f.current->Member(MapDataKind::Strings).sources.front().data == oldWts, "WTS Undo恢复完整注释/CRLF原字节");
        }
    }
}

void DSettingsReferencesRequireExplicitSharedModeAndUseOriginalUndo() {
    for (const bool independent : {false, true}) {
        auto members = DMembers();
        auto settings = MapSettings::Parse(members.front().data); Require(settings.Succeeded(), "真实引用 W3I 来源");
        settings.value->author = "TRIGSTR_00042";
        settings.value->recommendedPlayers = "TRIGSTR_7";
        const auto wire = settings.value->Serialize(); Require(wire.Succeeded(), "三项引用 W3I 正常编码");
        members.front().data = *wire.value;
        Fixture f(independent, members);
        const auto oldW3i = f.current->Member(MapDataKind::Settings).sources.front().data;
        const auto oldWts = f.current->Member(MapDataKind::Strings).sources.front().data;
        const auto changes = QJsonArray{DChange(f, false, 0, "author", "共享作者/描述"),
            DChange(f, false, 0, "description", "共享作者/描述"), DChange(f, false, 0, "recommended_players", "3–5")};
        auto request = DRequest(f, false, changes);
        Require(Code(InvokeMapDataTool("map_settings_set", request, f.host)) == "MAP_TEXT_WRITE_MODE"
            && f.publishes == 0 && f.current->Member(MapDataKind::Strings).sources.front().data == oldWts,
            "原 literal 承诺不允许静默修改共享 WTS，错误模式在发布前拒绝");
        request.insert("write_mode", "shared_string");
        const auto changed = InvokeMapDataTool("map_settings_set", request, f.host);
        Require(changed.value("ok").toBool() && changed.value("write_mode") == "shared_string"
            && changed.value("changes_shared_references").toBool() && changed.value("shared_string_ids").toArray() == QJsonArray{"42", "7"}
            && !changed.value("shared_reference_notice").toString().isEmpty() && !changed.value("saved").toBool()
            && !changed.value("runtime_verified").toBool() && f.current->Member(MapDataKind::Settings).sources.front().data == oldW3i,
            "引用工具明确声明共享影响与稳定编号，仍只声称编辑且保留完整 W3I");
        const auto query = InvokeMapDataTool("map_settings_get", f.IdentityArguments(), f.host);
        Require(DField(query, "author").value("wire_value") == "TRIGSTR_00042"
            && DField(query, "description").value("wire_value") == "TRIGSTR_0042"
            && DField(query, "author").value("value") == "共享作者/描述"
            && DField(query, "description").value("value") == "共享作者/描述"
            && DField(query, "recommended_players").value("value") == "3–5",
            "独立 query 回读同一共享正文，精确 wire 前导零不改变");
        const auto frozen = FreezeOriginalMapDataFields(*f.original, *f.current);
        Require(frozen.Succeeded() && frozen.value->Operations().size() == 2 && frozen.value->Members().size() == 1,
            "三个引用值只冻结两个 WTS 原来源操作，沿用累计预算");
        Require(Code(InvokeMapDataTool("map_settings_set", request, f.host)) == "ENTITY_REVISION_CONFLICT",
            "W3I SHA 相同仍由工具修订身份拒绝旧请求");
        if (!independent) {
            const auto receipt = f.session->LastEdit();
            Require(f.session->Replay(receipt, false).Succeeded(), "三字段 WTS 修改仍只有一个原 Undo");
            (void)InvokeMapDataTool("map_settings_get", f.IdentityArguments(), f.host);
            Require(f.current->Member(MapDataKind::Strings).sources.front().data == oldWts
                && f.current->Member(MapDataKind::Settings).sources.front().data == oldW3i, "原 Undo 恢复完整正文来源而不改 W3I");
            Require(f.session->Replay(receipt, true).Succeeded(), "同一原 Redo 回执恢复引用候选");
        }
        auto mixed = DRequest(f, false, {DChange(f, false, 0, "description", "下一段共享描述"),
            DChange(f, false, 0, "game_data_set", "2")});
        mixed.insert("write_mode", "shared_string");
        const auto published = f.publishes;
        Require(Code(InvokeMapDataTool("map_settings_set", mixed, f.host)) == "MAP_TEXT_WRITE_MODE" && f.publishes == published,
            "共享正文加 W3I 数值需要明确 mixed，不得错报单成员写入");
        mixed.insert("write_mode", "mixed");
        const auto result = InvokeMapDataTool("map_settings_set", mixed, f.host);
        Require(result.value("ok").toBool() && result.value("write_mode") == "mixed" && result.value("changes_shared_references").toBool()
            && result.value("shared_string_ids").toArray() == QJsonArray{"42"} && f.current->Settings()->gameDataSet == 2
            && f.current->Settings()->description == "TRIGSTR_0042" && f.current->ResolveString("TRIGSTR_42").value == std::optional<std::string>("下一段共享描述"),
            "mixed 声明与真实 W3I/WTS 两成员写入一致，保留原引用");
    }
    Fixture missing(false, Members());
    auto request = DRequest(missing, false, {DChange(missing, false, 0, "description", "new")}); request.insert("write_mode", "shared_string");
    Require(Code(InvokeMapDataTool("map_settings_set", request, missing.host)) == "MAP_FIELD_SOURCE_UNSAFE"
        && missing.publishes == 0, "明确共享模式也不得重建缺失 WTS 或覆盖 W3I 引用");
    Fixture duplicate(false, DMembers("STRING 42\n{\nfirst\n}\nSTRING 42\n{\nsecond\n}\n"));
    request = DRequest(duplicate, false, {DChange(duplicate, false, 0, "description", "new")}); request.insert("write_mode", "shared_string");
    Require(Code(InvokeMapDataTool("map_settings_set", request, duplicate.host)) == "WTS_REFERENCE_AMBIGUOUS"
        && duplicate.publishes == 0, "明确共享模式也不得选择重复 WTS ID 中的一条");
}

void DFailuresPagingAndUnresolvedText() {
    Fixture f(false, DMembers());
    const auto originalW3i = f.current->Member(MapDataKind::Settings).sources.front().data;
    const auto originalWts = f.current->Member(MapDataKind::Strings).sources.front().data;
    const auto verifyUnchanged = [&] { Require(f.publishes == 0 && f.session->Revision() == 0 && !f.session->CanUndo()
        && f.current->Member(MapDataKind::Settings).sources.front().data == originalW3i
        && f.current->Member(MapDataKind::Strings).sources.front().data == originalWts, "D失败完整保留身份/Undo/修订/成员字节"); };
    auto good = DChange(f, false, 0, "author", "new author");
    for (const auto& value : {QStringLiteral("TRIGSTR_999"), QString(4097, 'x'), QString(1, QChar::Null), QString(1, QChar(0xd800))}) {
        auto bad = DChange(f, false, 0, "description", "valid"); bad.insert("value", value);
        Require(!InvokeMapDataTool("map_settings_set", DRequest(f, false, {good, bad}), f.host).value("ok").toBool(),
            "合法D前项加非法引用/字节预算/NUL/无损编码尾项必须全批失败"); verifyUnchanged();
    }
    auto mode = DRequest(f, true, {DChange(f, true, 0, "value", "new")}); mode.insert("write_mode", "literal");
    Require(Code(InvokeMapDataTool("strings_set", mode, f.host)) == "MAP_TEXT_WRITE_MODE", "共享影响必须明确确认"); verifyUnchanged();
    auto stale = good; stale.insert("expected_raw", "hex:00");
    Require(Code(InvokeMapDataTool("map_settings_set", DRequest(f, false, {good, stale}), f.host)) == "MAP_FIELD_DUPLICATE", "重复字段不得变成两次写入"); verifyUnchanged();
    stale = DChange(f, false, 0, "description", "new"); stale.insert("expected_raw", "hex:00");
    Require(Code(InvokeMapDataTool("map_settings_set", DRequest(f, false, {good, stale}), f.host)) == "MAP_FIELD_VALUE_STALE", "晚项精确前值冲突不得发布前项"); verifyUnchanged();
    f.readOnly = true;
    Require(Code(InvokeMapDataTool("map_settings_set", DRequest(f, false, {good}), f.host)) == "HOST_READ_ONLY"
        && Code(InvokeMapDataTool("strings_set", DRequest(f, true, {DChange(f, true, 0, "value", "new")}), f.host)) == "HOST_READ_ONLY", "只读宿主拒绝两类D修改"); verifyUnchanged();
    f.readOnly = false; f.rejectPublish = true;
    Require(Code(InvokeMapDataTool("strings_set", DRequest(f, true, {DChange(f, true, 0, "value", "new")}), f.host)) == "HOST_PUBLICATION_DENIED", "回执预算/宿主拒绝在原发布之前"); verifyUnchanged();
    Fixture duplicate(false, DMembers("STRING 42\n{\nfirst\n}\nSTRING 42\n{\nsecond\n}\n"));
    const auto unresolved = DField(InvokeMapDataTool("map_settings_get", duplicate.IdentityArguments(), duplicate.host), "description");
    Require(unresolved.value("value").isNull() && unresolved.value("resolution") == "unresolved"
        && unresolved.value("wire_value") == "TRIGSTR_0042", "重复ID的TRIGSTR不能猜测解析文字");
    Require(Code(InvokeMapDataTool("strings_set", DRequest(duplicate, true, {DChange(duplicate, true, 0, "value", "new")}), duplicate.host)) == "WTS_REFERENCE_AMBIGUOUS"
        && duplicate.publishes == 0, "重复ID不可选择首行悄悄修改");
    Fixture missing(false, Members());
    const auto missingText = DField(InvokeMapDataTool("map_settings_get", missing.IdentityArguments(), missing.host), "description");
    Require(missingText.value("value").isNull() && missingText.value("resolution_diagnostics").toArray().size() == 1, "WTS缺失明确返回未解析诊断");
    std::string wts;
    for (unsigned id = 0; id < 129; ++id) wts += "STRING " + std::to_string(id) + "\n{\nrow\n}\n";
    Fixture paged(false, DMembers(wts)); auto query = paged.IdentityArguments(); query.insert("page", QJsonObject{{"limit", 128}});
    const auto first = InvokeMapDataTool("strings_query", query, paged.host);
    Require(first.value("ok").toBool() && first.value("rows").toArray().size() == 128 && first.value("total") == "129" && first.value("truncated").toBool(), "WTS页长硬上限与真实总数");
    query.insert("page", QJsonObject{{"cursor", first.value("next_cursor")}, {"limit", 128}});
    const auto second = InvokeMapDataTool("strings_query", query, paged.host);
    Require(second.value("ok").toBool() && second.value("rows").toArray().size() == 1 && !second.value("truncated").toBool(), "原WTS cursor续页不丢ID");
    auto cursor = first.value("next_cursor").toObject(); cursor.insert("member_sha256", QString(64, 'f'));
    query.insert("page", QJsonObject{{"cursor", cursor}});
    Require(Code(InvokeMapDataTool("strings_query", query, paged.host)) == "ENTITY_QUERY_CURSOR_STALE", "WTS续页严格绑定成员SHA");
    std::string huge = "STRING 42\n{\n" + std::string(140000, 'x') + "\n}\n";
    Fixture budget(false, DMembers(huge));
    Require(Code(InvokeMapDataTool("map_settings_get", budget.IdentityArguments(), budget.host)) == "ENTITY_RESPONSE_BUDGET",
        "解析后文字与hex的完整JSON受256KiB预算，不只计算原TRIGSTR wire大小");
    const auto denied = InvokeMapDataTool("map_settings_set", DRequest(budget, false, {DChange(budget, false, 0, "author", "new")}), budget.host);
    Require(Code(denied) == "ENTITY_RESPONSE_BUDGET" && budget.publishes == 0 && budget.session->Revision() == 0 && !budget.session->CanUndo(),
        "巨大的解析前值回执预算须在原W3I命令发布之前拒绝");
    auto unsupportedMembers = DMembers();
    auto settings = MapSettings::Parse(unsupportedMembers.front().data); Require(settings.Succeeded(), "真实版本夹具");
    settings.value->formatVersion = 31; settings.value->gameVersion = {1, 32, 10, 20456};
    const auto versioned = settings.value->Serialize(); Require(versioned.Succeeded(), "实际v31 writer夹具，不伪造状态");
    unsupportedMembers.front().data = *versioned.value;
    Fixture unsupported(false, unsupportedMembers);
    const auto known = InvokeMapDataTool("map_settings_get", unsupported.IdentityArguments(), unsupported.host);
    Require(known.value("ok").toBool() && !DField(known, "author").value("writable").toBool()
        && Code(InvokeMapDataTool("map_settings_set", DRequest(unsupported, false, {DChange(unsupported, false, 0, "author", "new")}), unsupported.host)) == "MAP_FIELD_LAYOUT_READ_ONLY",
        "可读的v31不由writer存在推定D写入或完整保存资格");
}

void TwoDefinitionsAndSuccessfulOriginalHosts() {
    const auto definitions = MapDataToolDefinitions(); Require(definitions.size() == 9, "exact C2 plus D4 plus E3 shared tool count");
    std::set<QString> names;
    for (const auto& item : definitions) {
        names.insert(item.name); Require(item.maxRequestIdBytes == 4096 && item.maxResponseBytes == 1024U * 1024U
            && !item.inputSchema.value("additionalProperties").toBool(true), "strict shared schema and bounded outer transport");
    }
    Require(names == std::set<QString>{"entities_query", "entities_set_fields", "map_settings_get", "map_settings_set", "strings_query", "strings_set",
        "sounds_list", "sounds_get", "sounds_update"}, "严格保留C两工具、D四工具并追加E三工具，不允许未知工具");
    Require(std::count_if(definitions.begin(), definitions.end(), [](const auto& definition) { return definition.name.startsWith("entities_"); }) == 2
        && std::count_if(definitions.begin(), definitions.end(), [](const auto& definition) { return definition.name.startsWith("map_settings_") || definition.name.startsWith("strings_"); }) == 4,
        "C2与D4各自精确计数");
    Require(std::count_if(definitions.begin(), definitions.end(), [](const auto& definition) { return definition.name.startsWith("sounds_"); }) == 3,
        "E0仅三工具，声音增删仍不支持");
    for (const bool independent : {false, true}) {
        Fixture f(independent); const auto queried = f.Query(); Require(queried.value("ok").toBool(), "query current real typed context");
        const auto edited = InvokeMapDataTool("entities_set_fields", f.Request({f.Change(0, "left", "32"), f.Change(0, "right", "45")}), f.host);
        Require(edited.value("ok").toBool() && edited.value("applied_fields") == "2" && edited.value("changed_rows") == "1"
            && edited.value("before").toObject().value("rows").toArray().size() == 1 && edited.value("after").toObject().value("rows").toArray().size() == 1,
            "joint final bounds and actual selected before/after rows");
        const auto changes = edited.value("changes").toObject().value("items").toArray();
        Require(changes.size() == 2 && changes.first().toObject().value("object_id").toString().isEmpty()
            && changes.first().toObject().value("source_index") == "0" && !changes.first().toObject().value("target").toString().isEmpty(), "actual field items do not invent G navigation IDs");
        Require(!edited.value("saved").toBool() && !edited.value("runtime_verified").toBool()
            && edited.value("session_identity").toString() == QString::number(f.session->SessionIdentity())
            && edited.value("command_identity").toString() == QString::number(f.session->LastEdit().CommandIdentity()), "actual original receipt and bounded evidence claim");
        if (independent) Require(edited.value("transaction_active").toBool() && edited.value("transaction_id") == "7"
            && edited.value("transaction_revision").isObject(), "original active transaction envelope retained");
        Require(f.Query().value("member").toObject().value("member_sha256") == edited.value("after").toObject().value("member").toObject().value("member_sha256"), "current query consumes published typed overlay");
        if (!independent) {
            const auto receipt = f.session->LastEdit(); Require(f.session->Replay(receipt, false).Succeeded(), "GUI original receipt undo");
            Require(f.Query().value("member") == queried.value("member") && f.session->Replay(receipt, true).Succeeded(), "original GUI undo/redo native exactness");
        }
        auto stale = f.Request({f.Change(0, "left", "31")}); stale.insert("expected_revision", queried.value("revision"));
        const auto before = QJsonDocument(f.Query()).toJson(QJsonDocument::Compact); const auto revision = f.session->Revision(); const auto command = f.session->LastEdit().CommandIdentity();
        Require(Code(InvokeMapDataTool("entities_set_fields", stale, f.host)) == "ENTITY_REVISION_CONFLICT", "same transaction stale revision refused");
        Unchanged(f, before, f.publishes, revision, command);
    }
}

void IllegalAndLateBatchFailuresNeverPublish() {
    Fixture f; const auto before = QJsonDocument(f.Query()).toJson(QJsonDocument::Compact);
    const auto check = [&](QJsonObject request, const char* code) {
        Require(Code(InvokeMapDataTool("entities_set_fields", request, f.host)) == code, "precise failure diagnostic"); Unchanged(f, before, 0, 0, 0);
    };
    auto request = f.Request({f.Change(0, "left", "-9")}); request.insert("expected_member_sha256", QString(64, 'b')); check(request, "ENTITY_MEMBER_STALE");
    auto change = f.Change(0, "left", "-9"); change.insert("row_key", "wrong row"); check(f.Request({change}), "MAP_FIELD_ROW_STALE");
    change = f.Change(0, "left", "-9"); change.insert("expected_raw", "f32:00000000"); check(f.Request({change}), "MAP_FIELD_VALUE_STALE");
    change = f.Change(0, "left", "not-a-number"); check(f.Request({change}), "MAP_FIELD_INPUT_INVALID");
    check(f.Request({f.Change(0, "left", "-9"), f.Change(0, "right", "-10")}), "MAP_FIELD_ROW_INVALID");
    const auto name = f.Change(0, "name", "hex:4e6577"); check(f.Request({f.Change(0, "left", "-9"), name}), "MAP_FIELD_READ_ONLY");
    change = f.Change(0, "left", "-9"); change.insert("source_index", 0); check(f.Request({change}), "ENTITY_INDEX_INVALID");
    change = f.Change(0, "left", "-9"); change.insert("source_index", "01"); check(f.Request({change}), "ENTITY_INDEX_INVALID");
    check(f.Request({f.Change(0, "left", "-9"), f.Change(0, "left", "-8")}), "MAP_FIELD_DUPLICATE");
    f.rejectPublish = true; check(f.Request({f.Change(0, "left", "-9")}), "HOST_PUBLICATION_DENIED");
    f.rejectPublish = false; f.readOnly = true; check(f.Request({f.Change(0, "left", "-9")}), "HOST_READ_ONLY");
}

void CompleteEscapedReceiptsAreBudgetedBeforePublish() {
    Fixture f; const auto before = QJsonDocument(f.Query()).toJson(QJsonDocument::Compact);
    f.hostChanges = {{"items", QJsonArray{QJsonObject{{"old_text", QString(140000, '\n')}}}}};
    const auto denied = InvokeMapDataTool("entities_set_fields", f.Request({f.Change(0, "left", "-9")}), f.host);
    Require(Code(denied) == "ENTITY_RESPONSE_BUDGET" && f.validates == 1, "full escaped host changes are checked inside original guard");
    Unchanged(f, before, 0, 0, 0);
    Fixture historical(false, Members(2, true));
    const auto native = historical.current->Member(MapDataKind::Regions).sources.front().data;
    const auto request = historical.Request({historical.Change(0, "left", "-9"), historical.Change(1, "left", "-9")});
    const auto over = InvokeMapDataTool("entities_set_fields", request, historical.host);
    Require(Code(over) == "ENTITY_RESPONSE_BUDGET" && historical.publishes == 0 && historical.session->Revision() == 0
        && historical.current->Member(MapDataKind::Regions).sources.front().data == native, "complete historical before/after rows cannot be truncated after publication");
}

void GuiFieldsPreservePriorAuthorEditsAndOriginalUndo() {
    Fixture f;
    auto renamed = *f.original->Regions(); renamed.entries[0].name = "Author previously renamed this region";
    Require(f.session->EditRegions(renamed).Succeeded(), "seed wider original GUI author edit");
    const auto authorReceipt = f.session->LastEdit(); Require(f.Query().value("ok").toBool(), "query current author overlay");
    const auto edited = InvokeMapDataTool("entities_set_fields", f.Request({f.Change(0, "left", "-9")}), f.host);
    Require(edited.value("ok").toBool() && f.current->Regions()->entries[0].name == renamed.entries[0].name,
        "GUI field operation does not force standalone whole-document field projection");
    const auto receipt = f.session->LastEdit(); Require(receipt.SessionIdentity() == authorReceipt.SessionIdentity()
        && receipt.CommandIdentity() != authorReceipt.CommandIdentity() && f.session->Replay(receipt, false).Succeeded(), "new field receipt belongs to original GUI session");
    const auto undone = f.Query(); Require(undone.value("ok").toBool() && f.current->Regions()->entries[0].name == renamed.entries[0].name
        && f.current->Regions()->entries[0].left == -10, "field undo retains the author's earlier rename");
    Require(f.session->Replay(authorReceipt, false).Succeeded() && !f.session->IsDirty(), "earlier original GUI receipt remains usable");
}

void PagingReadonlyAndUnsafePresence() {
    Fixture f;
    auto query = f.IdentityArguments(); query.insert("kind", "regions"); query.insert("page", QJsonObject{{"start_index", "1"}, {"limit", 1}});
    const auto page = InvokeMapDataTool("entities_query", query, f.host);
    Require(page.value("ok").toBool() && page.value("rows").toArray().first().toObject().value("source_index") == "1"
        && page.value("next_cursor").toObject().value("next_index") == "2", "exact string indices and real next cursor");
    query.insert("page", QJsonObject{{"start_index", "9007199254740993"}, {"limit", 1}});
    Require(!InvokeMapDataTool("entities_query", query, f.host).value("ok").toBool(), "uint64 above JSON safe integer is parsed exactly and rejected out of range");
    query.insert("page", QJsonObject{{"start_index", "0"}, {"limit", 129}});
    Require(Code(InvokeMapDataTool("entities_query", query, f.host)) == "ENTITY_PAGE_LIMIT", "facade direct call obeys page cap");
    const auto doodads = f.Query("doodads"); Require(doodads.value("ok").toBool() && doodads.value("rows").toArray().size() == 2, "normal and special doodads remain queryable");
    for (const auto& row : doodads.value("rows").toArray()) for (const auto& field : row.toObject().value("fields").toArray())
        Require(!field.toObject().value("writable").toBool(), "all actual doodad records remain readonly");
    auto members = Members(); members.erase(std::remove_if(members.begin(), members.end(), [](const auto& member) { return member.name == "war3map.w3r"; }), members.end());
    Fixture missing(false, members); const auto absent = missing.Query();
    Require(absent.value("ok").toBool() && absent.value("member").toObject().value("state") == "missing" && absent.value("rows").toArray().isEmpty(), "known absence is explicit");
    missing.opaque = true; const auto opaque = missing.Query();
    Require(!opaque.value("ok").toBool() && opaque.value("member").toObject().value("state") == "opaque-unknown" && !opaque.contains("rows"), "opaque absence never becomes an empty table");
    members = Members(); members.push_back({"WAR3MAP.W3R", members[3].data}); Fixture ambiguous(false, members);
    const auto conflict = ambiguous.Query(); Require(!conflict.value("ok").toBool() && conflict.value("member").toObject().value("state") == "ambiguous", "project preserves actual multiple sources");
    members = Members(); members[3].data = {std::byte{1}}; Fixture invalid(false, members);
    const auto bad = invalid.Query(); Require(!bad.value("ok").toBool() && bad.value("member").toObject().value("state") == "invalid", "damaged native source stays invalid");
    members = Members();
    members[1].data.insert(members[1].data.begin() + 52, {std::byte{'S'}, std::byte{'K'}, std::byte{'I'}, std::byte{'N'}});
    members[0].data[0] = std::byte{31}; members[0].data[1] = members[0].data[2] = members[0].data[3] = std::byte{0};
    Fixture versioned(false, members); const auto skin = versioned.Query("units");
    Require(skin.value("ok").toBool() && skin.value("rows").toArray().size() == 1, "real readable versioned unit wire");
    for (const auto& field : skin.value("rows").toArray().first().toObject().value("fields").toArray())
        Require(!field.toObject().value("writable").toBool(), "readable SkinId layout does not advertise C1 writes");
}

void StandaloneCumulativeLimitAndRestoreToOriginal() {
    Fixture f(true, Members(33)); Require(f.Query().value("ok").toBool(), "original standalone typed view");
    QJsonArray changes; for (std::size_t row = 0; row < 32; ++row) changes.append(f.Change(row, "left", "-11"));
    Require(InvokeMapDataTool("entities_set_fields", f.Request(changes), f.host).value("ok").toBool(), "standalone freezes 32 original field differences");
    const auto before = QJsonDocument(f.Query()).toJson(QJsonDocument::Compact); const auto revision = f.session->Revision(); const auto command = f.session->LastEdit().CommandIdentity();
    const auto denied = InvokeMapDataTool("entities_set_fields", f.Request({f.Change(32, "left", "-11")}), f.host);
    Require(Code(denied) == "MAP_FIELD_CUMULATIVE_COUNT", "second standalone request cannot bypass cumulative field save cap");
    Unchanged(f, before, 1, revision, command);
    changes = {}; for (std::size_t row = 0; row < 32; ++row) changes.append(f.Change(row, "left", "-10"));
    const auto restored = InvokeMapDataTool("entities_set_fields", f.Request(changes), f.host);
    Require(restored.value("ok").toBool() && FreezeOriginalMapDataFields(*f.original, *f.current).value->Operations().empty(), "restore cumulative fields to zero remains a legal editor operation");
    f.active = false; const auto inactive = QJsonDocument(f.Query()).toJson(QJsonDocument::Compact);
    const auto failed = InvokeMapDataTool("entities_set_fields", f.Request({f.Change(0, "left", "-9")}), f.host);
    Require(Code(failed) == "ENTITY_ACTIVE_TRANSACTION_REQUIRED", "standalone cannot write outside active edit");
    Unchanged(f, inactive, 2, f.session->Revision(), f.session->LastEdit().CommandIdentity());
}
std::vector<MapArchiveMember> FilterMembers(bool large = false) {
    auto members = Members();
    UnitPlacements units;
    const auto add = [&](std::string id, std::int32_t owner) {
        UnitPlacement unit; unit.rawcode = std::move(id); unit.owner = owner;
        unit.scale = {1, 1, 1}; unit.creationId = static_cast<std::int32_t>(units.units.size());
        units.units.push_back(std::move(unit));
    };
    if (large) {
        for (int i = 0; i < 4096; ++i) add("hfoo", 0);
        add("hbar", 1);
    } else {
        add("hfoo", 0); add("Hfoo", 1); add("hfoo", 1); add("hfoo", 0);
        add("hex:", std::numeric_limits<std::int32_t>::min());
        add(std::string("\0\xff\0\x0a", 4), std::numeric_limits<std::int32_t>::max());
        add(std::string("\xc3\xa8\xc3\xa8", 4), -99);
    }
    const auto serialized = units.Serialize(); Require(serialized.Succeeded(), "independent native filter fixture serializes");
    members[1].data = *serialized.value; return members;
}
QJsonObject FilterRequest(Fixture& fixture, QJsonObject filter, int limit = 1, QJsonObject cursor = {}) {
    auto request = fixture.IdentityArguments(); request.insert("kind", "units"); request.insert("filter", filter);
    QJsonObject page{{"limit", limit}}; if (!cursor.isEmpty()) page.insert("cursor", cursor);
    request.insert("page", page); return request;
}
void FilterPaginationIsExactAndPreservesCurrentOriginalSession() {
    Fixture f(false, FilterMembers());
    const auto original = f.original->Member(MapDataKind::Units).sources.front().data;
    const QJsonObject filter{{"type_id", "hfoo"}, {"player", 0}};
    const auto first = InvokeMapDataTool("entities_query", FilterRequest(f, filter), f.host);
    Require(first.value("ok").toBool() && first.value("rows").toArray().size() == 1
        && first.value("rows").toArray().first().toObject().value("source_index") == "0"
        && first.value("matched_total") == "2" && first.value("matched_total_complete").toBool()
        && first.value("examined") == "7" && first.value("truncated").toBool(), "small full scan counts exact filtered total while returning one native row");
    const auto cursor = first.value("next_cursor").toObject();
    Require(cursor.value("filter").toObject() == QJsonObject{{"type_id", "hex:68666f6f"}, {"player", 0}}
        && cursor.value("member_sha256") == first.value("member").toObject().value("member_sha256"), "cursor binds canonical filter and actual member SHA");
    const auto second = InvokeMapDataTool("entities_query", FilterRequest(f, {{"type_id", "hex:68666F6F"}, {"player", 0}}, 1, cursor), f.host);
    Require(second.value("ok").toBool() && second.value("rows").toArray().size() == 1
        && second.value("rows").toArray().first().toObject().value("source_index") == "3"
        && second.value("matched_total") == "2" && second.value("next_cursor").isNull(), "equivalent hex selector returns next sparse original index exactly once");
    const auto query = [&](QJsonObject selected) { return InvokeMapDataTool("entities_query", FilterRequest(f, selected, 128), f.host); };
    Require(query({{"type_id", "Hfoo"}}).value("matched_total") == "1", "rawcode case is significant");
    Require(query({{"player", 0}}).value("matched_total") == "2", "player zero is a real optional predicate");
    Require(query({{"type_id", "hex:"}}).value("rows").toArray().first().toObject().value("source_index") == "4", "literal four-byte hex colon rawcode is not mistaken for encoded selector");
    Require(query({{"type_id", "hex:00ff000a"}, {"player", std::numeric_limits<std::int32_t>::max()}}).value("matched_total") == "1", "non-UTF8 and NUL rawcode remains losslessly queryable");
    Require(query({{"player", std::numeric_limits<std::int32_t>::min()}}).value("matched_total") == "1", "query owner preserves full native int32 rather than writer domain");
    Require(query({{"type_id", QString::fromUtf8("\xc3\xa8\xc3\xa8")}, {"player", -99}}).value("matched_total") == "1", "four encoded UTF8 bytes can be fewer than four characters");
    const auto empty = query({{"type_id", "none"}});
    Require(empty.value("ok").toBool() && empty.value("rows").toArray().isEmpty() && empty.value("matched_total") == "0"
        && empty.value("matched_total_complete").toBool() && empty.value("next_cursor").isNull(), "reliable exact zero is distinguished from unfinished scan");
    Require(f.attempts == 0 && f.publishes == 0 && f.session->Revision() == 0
        && f.current->Member(MapDataKind::Units).sources.front().data == original, "all filtered reads preserve original native and undo session");
    const auto edited = InvokeMapDataTool("entities_set_fields", f.Request({f.Change(0, "owner", "1", MapDataKind::Units)}, "units"), f.host);
    Require(edited.value("ok").toBool(), "seed actual original-session owner edit");
    const auto receipt = f.session->LastEdit();
    const auto currentBytes = f.current->Member(MapDataKind::Units).sources.front().data;
    Require(query(filter).value("matched_total") == "1" && query({{"type_id", "hfoo"}, {"player", 1}}).value("matched_total") == "2", "filter includes current unsaved typed overlay");
    Require(Code(InvokeMapDataTool("entities_query", FilterRequest(f, filter, 1, cursor), f.host)) == "ENTITY_QUERY_CURSOR_STALE", "actual edit invalidates old cursor even with refreshed outer CAS");
    Require(f.publishes == 1 && f.session->Revision() == receipt.AfterRevision()
        && f.current->Member(MapDataKind::Units).sources.front().data == currentBytes, "stale query preserves live edit and original undo receipt");
    Require(f.session->Replay(receipt, false).Succeeded(), "same original receipt restores owner edit");
    ++f.generation;
    Require(query(filter).value("matched_total") == "2" && f.current->Member(MapDataKind::Units).sources.front().data == original, "undo restores original filtered native");
}
void InvalidFilterAndCursorNeverBecomeSilentEmptyTables() {
    Fixture f(false, FilterMembers()); const auto original = f.original->Member(MapDataKind::Units).sources.front().data;
    const QJsonObject filter{{"type_id", "hfoo"}};
    const auto first = InvokeMapDataTool("entities_query", FilterRequest(f, filter), f.host);
    const auto cursor = first.value("next_cursor").toObject();
    const auto reject = [&](QJsonObject args, const char* code) {
        Require(Code(InvokeMapDataTool("entities_query", args, f.host)) == code, "explicit filtered query refusal");
        Require(f.session->Revision() == 0 && f.publishes == 0 && f.attempts == 0
            && f.current->Member(MapDataKind::Units).sources.front().data == original, "filtered refusal never publishes or changes original native");
    };
    reject(FilterRequest(f, {{"type_id", "foo"}}), "ENTITY_FILTER_TYPE_ID");
    reject(FilterRequest(f, {{"type_id", "hex:zzzzzzzz"}}), "ENTITY_FILTER_TYPE_ID");
    reject(FilterRequest(f, {{"player", 2147483648.0}}), "ENTITY_FILTER_PLAYER");
    reject(FilterRequest(f, {{"player", 0.5}}), "ENTITY_FILTER_PLAYER");
    reject(FilterRequest(f, {{"player", "0"}}), "ENTITY_FILTER_PLAYER");
    auto request = FilterRequest(f, filter); request.insert("kind", "regions"); reject(request, "ENTITY_FILTER_KIND");
    request = FilterRequest(f, filter); request.insert("filter", QJsonValue::Null); reject(request, "ENTITY_FILTER_INVALID");
    reject(FilterRequest(f, {{"type_id", "Hfoo"}}, 1, cursor), "ENTITY_QUERY_CURSOR_STALE");
    auto drift = cursor; drift.insert("member_sha256", QString(64, 'f'));
    reject(FilterRequest(f, filter, 1, drift), "ENTITY_QUERY_CURSOR_STALE");
    drift = cursor; auto binding = drift.value("binding").toObject(); binding.insert("document_generation", "2"); drift.insert("binding", binding);
    reject(FilterRequest(f, filter, 1, drift), "ENTITY_QUERY_CURSOR_STALE");
    request = FilterRequest(f, filter, 1, cursor); auto page = request.value("page").toObject(); page.insert("start_index", "0"); request.insert("page", page);
    reject(request, "ENTITY_QUERY_CURSOR_INVALID");
    drift = cursor; drift.insert("next_index", "01"); reject(FilterRequest(f, filter, 1, drift), "ENTITY_INDEX_INVALID");
    auto members = FilterMembers(); members[1].data = {std::byte{1}}; Fixture damaged(false, members);
    const auto bad = InvokeMapDataTool("entities_query", FilterRequest(damaged, filter), damaged.host);
    Require(!bad.value("ok").toBool() && bad.value("member").toObject().value("state") == "invalid" && !bad.contains("rows"), "filtered malformed native is not fabricated zero matches");
    members = FilterMembers(); members.push_back({"WAR3MAPUNITS.DOO", members[1].data}); Fixture ambiguous(false, members);
    const auto conflict = InvokeMapDataTool("entities_query", FilterRequest(ambiguous, filter), ambiguous.host);
    Require(!conflict.value("ok").toBool() && conflict.value("member").toObject().value("state") == "ambiguous", "filtered conflicting source stays ambiguous");
    members = FilterMembers(); members.erase(members.begin() + 1); Fixture missing(false, members);
    const auto absent = InvokeMapDataTool("entities_query", FilterRequest(missing, filter), missing.host);
    Require(absent.value("ok").toBool() && absent.value("member").toObject().value("state") == "missing"
        && absent.value("matched_total") == "0" && absent.value("next_cursor").isNull(), "known missing native has an explicit exact empty result");
    missing.opaque = true;
    const auto unknown = InvokeMapDataTool("entities_query", FilterRequest(missing, filter), missing.host);
    Require(!unknown.value("ok").toBool() && unknown.value("member").toObject().value("state") == "opaque-unknown", "opaque missing native cannot advertise zero matches");
}
void FilterScanCapContinuesWithoutInventingGlobalTotals() {
    Fixture f(false, FilterMembers(true)); const auto bytes = f.original->Member(MapDataKind::Units).sources.front().data;
    const QJsonObject filter{{"type_id", "hbar"}, {"player", 1}};
    const auto first = InvokeMapDataTool("entities_query", FilterRequest(f, filter), f.host);
    Require(first.value("ok").toBool() && first.value("rows").toArray().isEmpty() && first.value("examined") == "4096"
        && first.value("matched_total").isNull() && !first.value("matched_total_complete").toBool()
        && first.value("scan_budget_exhausted").toBool() && first.value("truncated").toBool()
        && first.value("next_cursor").toObject().value("next_index") == "4096", "zero rows at scan limit remains unfinished and global count unknown");
    const auto second = InvokeMapDataTool("entities_query", FilterRequest(f, filter, 1, first.value("next_cursor").toObject()), f.host);
    Require(second.value("ok").toBool() && second.value("rows").toArray().size() == 1
        && second.value("rows").toArray().first().toObject().value("source_index") == "4096"
        && second.value("examined") == "1" && second.value("next_cursor").isNull()
        && second.value("matched_total").isNull() && !second.value("matched_total_complete").toBool(), "last sparse page ends continuation without claiming global count");
    Require(f.attempts == 0 && f.session->Revision() == 0 && f.current->Member(MapDataKind::Units).sources.front().data == bytes, "bounded scan is readonly");
}

QJsonObject WorldRect(double left = -32, double bottom = -16, double right = 32, double top = 16) {
    return {{"left", left}, {"bottom", bottom}, {"right", right}, {"top", top}};
}

void SpatialFiltersUseSameBoundsAndOriginalRowsAcrossHosts() {
    for (const bool independent : {false, true}) {
        Fixture f(independent, MapDataSpatialFixtures::Members(true));
        const QJsonObject filter{{"type_id", "hfoo"}, {"player", 0}, {"world_rect", WorldRect()}};
        const auto original = f.current;
        const auto query = [&](const char* kind, QJsonObject selected, int limit = 1, QJsonObject cursor = {}) {
            auto args = FilterRequest(f, selected, limit, cursor); args["kind"] = kind;
            return InvokeMapDataTool("entities_query", args, f.host);
        };
        auto first = query("units", filter);
        Require(first.value("ok").toBool() && first.value("matched_total") == "3"
            && first.value("rows").toArray().first().toObject().value("source_index") == "0",
            "spatial/type/player intersection is applied before page length in both host modes");
        auto cursor = first.value("next_cursor").toObject();
        Require(cursor.value("filter").toObject().value("world_rect").toObject() == WorldRect()
            && cursor.value("kind") == "units", "cursor carries exact world rectangle and kind");
        for (const auto expectedIndex : {"3", "6"}) {
            auto page = query("units", filter, 1, cursor);
            Require(page.value("ok").toBool() && page.value("matched_total") == "3"
                && page.value("rows").toArray().first().toObject().value("source_index") == expectedIndex,
                "filtered sparse native indices are not replaced by page ordinals");
            cursor = page.value("next_cursor").toObject();
        }
        Require(cursor.isEmpty(), "spatial last page has no cursor");
        const auto counts = [&](const char* kind, QJsonObject selected, const char* expectedCount) {
            const auto page = query(kind, selected, 128);
            const auto context = std::string("shared facade spatial meaning: ") + kind + " " + Code(page).toStdString();
            Require(page.value("ok").toBool() && page.value("matched_total") == expectedCount,
                context.c_str());
        };
        counts("units", {{"type_id", "hfoo"}, {"player", 0}, {"world_rect", WorldRect(-32, -16, -32, -16)}}, "1");
        counts("doodads", {{"type_id", "LTlt"}, {"world_rect", WorldRect()}}, "2");
        counts("regions", {{"world_rect", WorldRect()}}, "4");
        counts("cameras", {{"world_rect", WorldRect()}}, "2");
        counts("doodads", {{"type_id", "YTfb"}}, "1");
        Require(Code(query("doodads", {{"world_rect", WorldRect()}})) == "MAP_DATA_QUERY_SPATIAL_UNAVAILABLE",
            "unknown special-doodad world position is explicit rather than dropped");
        Require(Code(query("doodads", {{"player", 0}, {"world_rect", WorldRect()}})) == "ENTITY_FILTER_KIND",
            "owner selector cannot silently exclude all doodads");
        Require(Code(query("cameras", {{"type_id", "hfoo"}, {"world_rect", WorldRect()}})) == "ENTITY_FILTER_KIND",
            "rawcode selector cannot silently exclude all cameras");
        const auto regionFirst = query("regions", {{"world_rect", WorldRect()}}, 1);
        const auto regionNext = query("regions", {{"world_rect", WorldRect()}}, 1, regionFirst.value("next_cursor").toObject());
        Require(regionNext.value("ok").toBool()
            && regionNext.value("rows").toArray().first().toObject().value("source_index") == "1",
            "non-unit cursor binds the real member kind and resumes filtered rows");
        for (const auto kind : {"units", "doodads", "regions", "cameras"}) {
            auto plain = f.IdentityArguments(); plain["kind"] = kind; plain["page"] = QJsonObject{{"limit", 1}};
            const auto head = InvokeMapDataTool("entities_query", plain, f.host);
            const auto next = head.value("next_cursor").toObject();
            Require(head.value("ok").toBool() && next.value("filter").isObject()
                && next.value("filter").toObject().isEmpty() && next.value("member_sha256").isString(),
                "unfiltered first page emits the same complete entity cursor rather than an unusable shorthand");
            plain["page"] = QJsonObject{{"limit", 1}, {"cursor", next}};
            const auto page = InvokeMapDataTool("entities_query", plain, f.host);
            Require(page.value("ok").toBool() && page.value("rows").toArray().first().toObject().value("source_index") == "1"
                && page.value("total") == head.value("total"), "unfiltered cursors roundtrip unchanged for all four entity kinds");
        }
        Require(f.attempts == 0 && f.publishes == 0 && f.session->Revision() == 0,
            "spatial queries never publish into either host transaction");
        for (const auto& member : original->Members()) {
            const auto& current = f.current->Member(member.kind);
            Require(current.state == member.state && current.sources.size() == member.sources.size(), "query retains all source presences");
            for (std::size_t i = 0; i < member.sources.size(); ++i)
                Require(current.sources[i].data == member.sources[i].data, "query retains every original member byte");
        }
    }
}

void SpatialInvalidCoordinatesCursorsAndScanLimitsRefusePrecisely() {
    Fixture f(false, MapDataSpatialFixtures::Members());
    const QJsonObject filter{{"type_id", "hfoo"}, {"player", 0}, {"world_rect", WorldRect()}};
    const auto first = InvokeMapDataTool("entities_query", FilterRequest(f, filter), f.host);
    const auto cursor = first.value("next_cursor").toObject();
    Require(first.value("ok").toBool() && !cursor.isEmpty(), "spatial cursor baseline");
    const auto rejectRect = [&](QJsonValue rectangle, const char* code = "ENTITY_FILTER_RECT") {
        auto selected = filter; selected["world_rect"] = rectangle;
        Require(Code(InvokeMapDataTool("entities_query", FilterRequest(f, selected), f.host)) == code,
            "malformed world rectangle has an explicit refusal");
    };
    rejectRect(QJsonValue::Null); rejectRect(WorldRect(33, -16, 32, 16)); rejectRect(WorldRect(-32, 17, 32, 16));
    auto rectangle = WorldRect(); rectangle["left"] = "-32"; rejectRect(rectangle);
    rectangle = WorldRect(); rectangle["left"] = std::numeric_limits<double>::quiet_NaN(); rejectRect(rectangle);
    rectangle = WorldRect(); rectangle["top"] = std::numeric_limits<double>::infinity(); rejectRect(rectangle);
    rectangle = WorldRect(); rectangle.remove("right"); rejectRect(rectangle, "ENTITY_ARGUMENT_REQUIRED");
    rectangle = WorldRect(); rectangle["ignore_unknown"] = true; rejectRect(rectangle, "ENTITY_ARGUMENT_UNKNOWN");
    auto changed = filter; changed["world_rect"] = WorldRect(-32, -16, 33, 16);
    Require(Code(InvokeMapDataTool("entities_query", FilterRequest(f, changed, 1, cursor), f.host)) == "ENTITY_QUERY_CURSOR_STALE",
        "changed rectangle invalidates an old cursor");
    auto wrongKind = cursor; wrongKind["kind"] = "doodads";
    Require(Code(InvokeMapDataTool("entities_query", FilterRequest(f, filter, 1, wrongKind), f.host)) == "ENTITY_QUERY_CURSOR_STALE",
        "cursor member kind cannot be rebound");
    ++f.generation;
    Require(Code(InvokeMapDataTool("entities_query", FilterRequest(f, filter, 1, cursor), f.host)) == "ENTITY_QUERY_CURSOR_STALE",
        "refreshed outer identity does not make the old spatial cursor current");
    Require(f.attempts == 0 && f.publishes == 0 && f.session->Revision() == 0, "all refusals remain read-only");
    Fixture large(false, FilterMembers(true));
    const QJsonObject lastFilter{{"type_id", "hbar"}, {"player", 1}, {"world_rect", WorldRect(0, 0, 0, 0)}};
    const auto head = InvokeMapDataTool("entities_query", FilterRequest(large, lastFilter), large.host);
    Require(head.value("ok").toBool() && head.value("rows").toArray().isEmpty() && head.value("examined") == "4096"
        && head.value("matched_total").isNull() && head.value("scan_budget_exhausted").toBool(),
        "spatial query preserves bounded scan and honest unknown total");
    const auto tail = InvokeMapDataTool("entities_query", FilterRequest(large, lastFilter, 1, head.value("next_cursor").toObject()), large.host);
    Require(tail.value("ok").toBool() && tail.value("examined") == "1" && tail.value("matched_total").isNull()
        && tail.value("rows").toArray().first().toObject().value("source_index") == "4096" && tail.value("next_cursor").isNull(),
        "spatial continuation reaches the last original row without widening scan budget");
}
void UnitPositionsUseOriginalAtomicFieldsAndRowCapabilities() {
    auto members = Members();
    auto table = ParseUnitPlacements(members[1].data);
    Require(table.Succeeded(), "parse real synthetic native units");
    auto start = table.value->units.front(); start.rawcode = "sloc"; start.creationId = 29;
    table.value->units.push_back(start); members[1].data = *table.value->Serialize().value;
    for (const bool independent : {false, true}) {
        Fixture f(independent, members); const auto baseline = f.Query("units");
        const auto rows = baseline.value("rows").toArray();
        const auto writable = [](const QJsonObject& row) {
            std::set<QString> result;
            for (const auto& value : row.value("fields").toArray()) {
                const auto field = value.toObject(); if (field.value("writable").toBool()) result.insert(field.value("id").toString());
            }
            return result;
        };
        const std::set<QString> oldFields{"owner", "facing", "hit_points", "mana"};
        auto ordinary = oldFields; ordinary.insert({"x", "y", "z", "hero_level"});
        Require(rows.size() == 2 && writable(rows[0].toObject()) == ordinary && writable(rows[1].toObject()) == oldFields,
            "ordinary XYZ capability is exact and start-location XYZ remains readonly");
        const auto before = QJsonDocument(baseline).toJson(QJsonDocument::Compact);
        const auto reject = [&](QJsonArray edits, const char* code) {
            const auto revision = f.session->Revision(), command = f.session->LastEdit().CommandIdentity();
            const auto attempts = f.attempts, publications = f.publishes;
            Require(Code(InvokeMapDataTool("entities_set_fields", f.Request(edits, "units"), f.host)) == code, "precise XYZ rejection diagnostic");
            Require(QJsonDocument(f.Query("units")).toJson(QJsonDocument::Compact) == before && f.session->Revision() == revision
                && f.session->LastEdit().CommandIdentity() == command && f.publishes == publications && f.attempts == attempts,
                "late XYZ refusal leaves query, native bytes, original history and publication untouched");
        };
        reject({f.Change(0, "x", "32", MapDataKind::Units), f.Change(1, "z", "16", MapDataKind::Units)}, "MAP_FIELD_POSITION_READ_ONLY");
        reject({f.Change(0, "x", "32", MapDataKind::Units), f.Change(0, "z", "1000000.001", MapDataKind::Units)}, "MAP_FIELD_INPUT_INVALID");
        reject({f.Change(0, "x", "f32:7f800000", MapDataKind::Units)}, "MAP_FIELD_VALUE_INVALID");
        const auto request = f.Request({f.Change(0, "x", "32", MapDataKind::Units), f.Change(0, "y", "-48", MapDataKind::Units),
            f.Change(0, "z", "8", MapDataKind::Units)}, "units");
        f.rejectPublish = true;
        const auto denied = InvokeMapDataTool("entities_set_fields", request, f.host);
        Require(Code(denied) == "HOST_PUBLICATION_DENIED" && f.publishes == 0 && f.session->Revision() == 0
            && QJsonDocument(f.Query("units")).toJson(QJsonDocument::Compact) == before, "prepublish denial preserves full XYZ baseline");
        f.rejectPublish = false;
        const auto applied = InvokeMapDataTool("entities_set_fields", request, f.host);
        Require(applied.value("ok").toBool() && applied.value("applied_fields") == "3" && applied.value("changed_rows") == "1"
            && f.current->Units()->units[0].position == std::array<float, 3>{32, -48, 8}
            && f.current->Units()->units[1].position == f.original->Units()->units[1].position,
            "three absolute native coordinates publish together through original field tool");
        const auto frozen = FreezeOriginalMapDataFields(*f.original, *f.current);
        Require(frozen.Succeeded() && frozen.value->Operations().size() == 3 && frozen.value->Members().size() == 1,
            "position uses the existing three-field native projection without a placement recipe");
        if (!independent) {
            const auto receipt = f.session->LastEdit(); auto* originalSession = f.session.get();
            Require(f.session->Replay(receipt, false).Succeeded() && f.Query("units").value("member") == baseline.value("member"), "original XYZ receipt undo is native-exact");
            Require(f.session->Replay(receipt, true).Succeeded() && f.Query("units").value("member") == applied.value("after").toObject().value("member"), "original XYZ receipt redo is native-exact");
            Require(f.session->Replay(receipt, false).Succeeded() && f.session.get() == originalSession, "final undo retains original session identity");
        } else {
            const auto restored = InvokeMapDataTool("entities_set_fields", f.Request({f.Change(0, "x", "0", MapDataKind::Units),
                f.Change(0, "y", "0", MapDataKind::Units), f.Change(0, "z", "0", MapDataKind::Units)}, "units"), f.host);
            Require(restored.value("ok").toBool(), "standalone XYZ original-value restoration succeeds");
            const auto empty = FreezeOriginalMapDataFields(*f.original, *f.current);
            Require(empty.Succeeded() && empty.value->Operations().empty() && empty.value->Members().empty(), "complete XYZ restoration clears existing net field projection");
        }
        Require(f.original->Member(MapDataKind::Units).sources.front().data == members[1].data, "original native source never changes");
    }
}
std::vector<MapArchiveMember> SoundMembers(std::vector<std::string> names = {"Owned sound"}, std::string path = "Sound\\Owned.wav") {
    auto members = Members(); SoundsDocument sounds;
    for (const auto& name : names) {
        SoundRecord sound{}; sound.name = name; sound.filePath = path; sound.eaxSetting = "DefaultEAXON";
        sound.volume = 64; sound.pitch = 1; sound.fadeInRate = 20; sound.fadeOutRate = 30;
        sounds.entries.push_back(std::move(sound));
    }
    const auto bytes = sounds.Serialize(); Require(bytes.Succeeded(), "complete owned classic W3S fixture");
    members.push_back({"war3map.w3s", *bytes.value}); return members;
}
QJsonObject SoundRequest(Fixture& fixture, std::size_t row, QJsonArray changes) {
    auto arguments = fixture.IdentityArguments();
    const auto key = MapFieldRowKey(*fixture.current, MapDataKind::Sounds, row);
    const auto sha = Sha256(fixture.current->Member(MapDataKind::Sounds).sources.front().data);
    Require(key.Succeeded() && sha.Succeeded(), "sound request uses actual raw name and member SHA");
    arguments.insert("sound_id", QString::fromStdString(*key.value)); arguments.insert("expected_member_sha256", QString::fromStdString(*sha.value));
    arguments.insert("changes", changes); return arguments;
}
QJsonObject SoundChange(Fixture& fixture, std::size_t row, const char* id, QJsonValue value) {
    const auto* field = FindMapField(MapDataKind::Sounds, id); Require(field != nullptr, "actual sound field descriptor");
    const auto old = ReadMapField(*fixture.current, MapDataKind::Sounds, row, *field); Require(old.Succeeded(), "actual sound raw before");
    return {{"field", id}, {"expected_raw", QString::fromStdString(FormatMapFieldRaw(*old.value))}, {"value", value}};
}
std::vector<MapArchiveMember> ExtendedSoundMembers(std::uint32_t version) {
    auto members = SoundMembers();
    auto& bytes = members.back().data;
    bytes.front() = static_cast<std::byte>(version);
    const auto text = [&](std::string_view value) {
        for (const auto byte : value) bytes.push_back(static_cast<std::byte>(byte));
        bytes.push_back(std::byte{0});
    };
    const auto integer = [&](std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::byte>((value >> shift) & 0xff));
    };
    text("Owned sound"); text("Preserved label"); text("Sound\\Owned.wav");
    integer(42); text("opaque2"); integer(7); integer(0);
    text("opaque6"); text("opaque7"); text("Face"); text("Group"); text("Face.fdf");
    if (version == 3) integer(11);
    return members;
}
void SoundsUseStableRawNamesTypedValuesAndOriginalUndo() {
    const auto rawName = std::string("raw-") + static_cast<char>(0xe9);
    for (const bool independent : {false, true}) {
        Fixture fixture(independent, SoundMembers({rawName}));
        const auto original = fixture.current->Member(MapDataKind::Sounds).sources.front().data;
        auto listed = InvokeMapDataTool("sounds_list", {}, fixture.host);
        const auto item = listed.value("rows").toArray().first().toObject();
        Require(listed.value("ok").toBool() && item.value("name").isNull() && item.value("name_hex") == "7261772de9"
            && item.value("sound_id") == "sound:7261772de9" && item.value("writable").toBool(), "non-UTF8 raw identity never becomes a lossy display name");
        auto get = fixture.IdentityArguments(); get.insert("sound_id", item.value("sound_id"));
        get.insert("fields", QJsonArray{"volume", "fade_in", "fade_out"});
        const auto read = InvokeMapDataTool("sounds_get", get, fixture.host);
        Require(read.value("ok").toBool() && read.value("rows").toArray().first().toObject().value("fields").toArray().size() == 3,
            "get projects only requested real catalog fields");
        const auto applied = InvokeMapDataTool("sounds_update", SoundRequest(fixture, 0,
            {SoundChange(fixture, 0, "volume", -1), SoundChange(fixture, 0, "fade_in", 0), SoundChange(fixture, 0, "fade_out", 2147483647)}), fixture.host);
        Require(applied.value("ok").toBool() && applied.value("applied_fields") == "3" && !applied.value("saved").toBool()
            && !applied.value("runtime_verified").toBool() && applied.value("revision") == fixture.host.identity().value("revision")
            && applied.value("changes").toObject().value("items").toArray().first().toObject().value("object_id") == item.value("sound_id"),
            "numeric update returns actual receipt and stable sound navigation identity, without saving");
        Require(fixture.current->Sounds()->entries.front().volume == -1 && fixture.current->Sounds()->entries.front().fadeInRate == 0
            && fixture.current->Sounds()->entries.front().fadeOutRate == 2147483647 && fixture.current->Sounds()->entries.front().name == rawName,
            "exact sentinel and integer endpoints preserve original bytes name");
        auto reread = InvokeMapDataTool("sounds_get", QJsonObject{{"sound_id", item.value("sound_id")}}, fixture.host);
        const auto fields = reread.value("rows").toArray().first().toObject().value("fields").toArray();
        const auto volume = std::find_if(fields.begin(), fields.end(), [](const auto& value) { return value.toObject().value("id") == "volume"; });
        Require(volume != fields.end() && volume->toObject().value("raw") == "-1" && volume->toObject().value("is_default").toBool()
            && volume->toObject().value("value").isNull(), "default display retains exact numeric raw and explicit sentinel");
        if (!independent) {
            const auto receipt = fixture.session->LastEdit();
            const auto edited = fixture.session->Snapshot().Member(MapDataKind::Sounds).sources.front().data;
            Require(receipt.CommandIdentity() == applied.value("command_identity").toString().toULongLong()
                && receipt.SessionIdentity() == fixture.session->SessionIdentity() && fixture.session->CanUndo() && !fixture.session->CanRedo(),
                "sound tool receipt identifies the original current history command");
            Require(fixture.session->Undo(), "sound tool uses original session Undo");
            Require(fixture.session->Snapshot().Member(MapDataKind::Sounds).sources.front().data == original, "original Undo restores complete W3S");
            const auto undone = fixture.session->LastAppliedEdit();
            Require(undone.CommandIdentity() == receipt.CommandIdentity() && undone.SessionIdentity() == receipt.SessionIdentity()
                && !undone.Forward() && undone.Restoring() && !fixture.session->CanUndo() && fixture.session->CanRedo(),
                "Undo restores the original receipt command and retains redo history");
            Require(fixture.session->Redo(), "sound tool uses original session Redo");
            const auto redone = fixture.session->LastAppliedEdit();
            Require(redone.CommandIdentity() == receipt.CommandIdentity() && redone.SessionIdentity() == receipt.SessionIdentity()
                && redone.Forward() && redone.Restoring() && fixture.session->CanUndo() && !fixture.session->CanRedo()
                && fixture.session->Snapshot().Member(MapDataKind::Sounds).sources.front().data == edited,
                "Redo restores the same original command and complete edited W3S");
        }
    }
}
void SoundFailuresNeverPublishOrCoerceTypes() {
    Fixture fixture(false, SoundMembers());
    const auto before = fixture.current->Member(MapDataKind::Sounds).sources.front().data;
    const auto revision = fixture.session->Revision(), command = fixture.session->LastEdit().CommandIdentity();
    const auto reject = [&](const QJsonObject& arguments, const char* expected = nullptr) {
        const auto result = InvokeMapDataTool("sounds_update", arguments, fixture.host);
        Require(!result.value("ok").toBool() && !Code(result).isEmpty(), "sound refusal has concrete diagnostic");
        if (expected) Require(Code(result) == QString::fromLatin1(expected), "sound refusal preserves exact gate diagnostic");
        Require(fixture.session->Revision() == revision && fixture.session->LastEdit().CommandIdentity() == command && fixture.publishes == 0
            && fixture.current->Member(MapDataKind::Sounds).sources.front().data == before, "sound refusal changes no byte, history or publication");
    };
    for (const QJsonValue value : {QJsonValue("80"), QJsonValue(true), QJsonValue(80.5), QJsonValue(2147483648.0), QJsonValue(QJsonValue::Null)})
        reject(SoundRequest(fixture, 0, {SoundChange(fixture, 0, "volume", value)}));
    reject(SoundRequest(fixture, 0, {SoundChange(fixture, 0, "volume", 80), SoundChange(fixture, 0, "fade_in", -2)}), "MAP_FIELD_VALUE_INVALID");
    reject(SoundRequest(fixture, 0, {SoundChange(fixture, 0, "volume", 128)}), "MAP_FIELD_VALUE_INVALID");
    const auto duplicate = SoundChange(fixture, 0, "volume", 80);
    reject(SoundRequest(fixture, 0, {duplicate, duplicate}), "MAP_FIELD_DUPLICATE");
    reject(SoundRequest(fixture, 0, {SoundChange(fixture, 0, "pitch", 2)}), "MAP_FIELD_READ_ONLY");
    auto stale = SoundRequest(fixture, 0, {duplicate}); stale.insert("expected_revision", "stale"); reject(stale, "ENTITY_REVISION_CONFLICT");
    stale = SoundRequest(fixture, 0, {duplicate}); stale.insert("expected_member_sha256", QString(64, 'f')); reject(stale, "ENTITY_MEMBER_STALE");
    stale = SoundRequest(fixture, 0, {duplicate}); stale.insert("sound_id", "sound:missing"); reject(stale, "MAP_SOUND_ID_INVALID");
    stale = SoundRequest(fixture, 0, {duplicate}); stale.insert("sound_id", "sound:4f74686572"); reject(stale, "MAP_SOUND_ID_MISSING");
    auto old = duplicate; old.insert("expected_raw", "064"); reject(SoundRequest(fixture, 0, {old}), "ENTITY_RAW_INVALID");
    old = duplicate; old.insert("expected_raw", "63"); reject(SoundRequest(fixture, 0, {old}), "MAP_FIELD_VALUE_STALE");
    fixture.readOnly = true; reject(SoundRequest(fixture, 0, {duplicate}), "HOST_READ_ONLY"); fixture.readOnly = false;
    auto readonlyHost = fixture.host; readonlyHost.apply = {};
    Require(Code(InvokeMapDataTool("sounds_update", SoundRequest(fixture, 0, {duplicate}), readonlyHost)) == "ENTITY_HOST_READ_ONLY", "query-only host rejects edit before apply");
    fixture.rejectPublish = true; reject(SoundRequest(fixture, 0, {duplicate}), "HOST_PUBLICATION_DENIED");
    Require(Code(InvokeMapDataTool("sounds_create", {}, fixture.host)) == "ENTITY_OPERATION_UNSUPPORTED"
        && Code(InvokeMapDataTool("sounds_delete", {}, fixture.host)) == "ENTITY_OPERATION_UNSUPPORTED", "unimplemented sound CRUD is explicit unsupported");
}
void SoundQueriesBindPaginationCoverageAndBudgets() {
    std::vector<std::string> names; for (int index = 0; index < 129; ++index) names.push_back("Owned sound " + std::to_string(index));
    Fixture fixture(false, SoundMembers(names));
    const auto first = InvokeMapDataTool("sounds_list", QJsonObject{{"page", QJsonObject{{"limit", 128}}}}, fixture.host);
    Require(first.value("ok").toBool() && first.value("rows").toArray().size() == 128 && first.value("total") == "129"
        && first.value("matched_total") == "129" && first.value("truncated").toBool(), "sound list has exact totals and hard page limit");
    const auto second = InvokeMapDataTool("sounds_list", QJsonObject{{"page", QJsonObject{{"cursor", first.value("next_cursor")}}}}, fixture.host);
    Require(second.value("ok").toBool() && second.value("rows").toArray().size() == 1 && !second.value("truncated").toBool(), "same sound cursor finishes without duplicate or dropped rows");
    const auto selected = InvokeMapDataTool("sounds_list", QJsonObject{{"query", "sound 128"}}, fixture.host);
    Require(selected.value("ok").toBool() && selected.value("rows").toArray().size() == 1 && selected.value("matched_total") == "1", "case-sensitive sound name/path query uses actual bytes");
    auto cursor = first.value("next_cursor").toObject(); cursor.insert("member_sha256", QString(64, 'f'));
    Require(Code(InvokeMapDataTool("sounds_list", QJsonObject{{"page", QJsonObject{{"cursor", cursor}}}}, fixture.host)) == "ENTITY_QUERY_CURSOR_STALE", "stale sound SHA cursor refused");
    cursor = first.value("next_cursor").toObject(); auto binding = cursor.value("binding").toObject(); binding.insert("document_id", "other-document");
    cursor.insert("binding", binding);
    Require(Code(InvokeMapDataTool("sounds_list", QJsonObject{{"page", QJsonObject{{"cursor", cursor}}}}, fixture.host)) == "ENTITY_QUERY_CURSOR_STALE", "same member SHA cannot authorize another document cursor");
    Require(Code(InvokeMapDataTool("sounds_list", QJsonObject{{"query", "other"}, {"page", QJsonObject{{"cursor", first.value("next_cursor")}}}}, fixture.host)) == "ENTITY_QUERY_CURSOR_STALE", "changed sound query cannot reuse cursor");
    Require(Code(InvokeMapDataTool("sounds_list", QJsonObject{{"page", QJsonObject{{"limit", 129}}}}, fixture.host)) == "ENTITY_QUERY_LIMIT", "sound page overflow refused");
    const auto id = first.value("rows").toArray().first().toObject().value("sound_id");
    Require(Code(InvokeMapDataTool("sounds_get", QJsonObject{{"sound_id", id}, {"fields", QJsonArray{"volume", "volume"}}}, fixture.host)) == "MAP_FIELD_DUPLICATE", "get does not silently collapse duplicate fields");
    Require(Code(InvokeMapDataTool("sounds_get", QJsonObject{{"sound_id", id}, {"fields", QJsonArray{"unknown-field"}}}, fixture.host)) == "MAP_FIELD_UNKNOWN", "get refuses undefined catalog fields");
    Require(Code(InvokeMapDataTool("sounds_get", QJsonObject{{"sound_id", id}, {"fields", QJsonArray{"extended_sound_name"}}}, fixture.host)) == "MAP_FIELD_NOT_APPLICABLE", "classic get refuses defined extension fields rather than inventing defaults");
    const auto wrongCase = InvokeMapDataTool("sounds_list", QJsonObject{{"query", "OWNED"}}, fixture.host);
    Require(wrongCase.value("ok").toBool() && wrongCase.value("rows").toArray().isEmpty() && wrongCase.value("matched_total") == "0", "sound query preserves byte case");
    for (const auto version : {2u, 3u}) {
        Fixture extended(false, ExtendedSoundMembers(version));
        const auto source = extended.current->Member(MapDataKind::Sounds).sources.front().data;
        Require(extended.current->Sounds() && extended.current->Sounds()->version == version && extended.current->Sounds()->entries.front().extended,
            "independent extension wire is really parsed as v2/v3");
        const auto list = InvokeMapDataTool("sounds_list", {}, extended.host);
        Require(list.value("ok").toBool() && list.value("rows").toArray().size() == 1 && !list.value("rows").toArray().first().toObject().value("writable").toBool(),
            "extended source remains visible readonly");
        const auto selectedId = list.value("rows").toArray().first().toObject().value("sound_id");
        const auto read = InvokeMapDataTool("sounds_get", QJsonObject{{"sound_id", selectedId}, {"fields", QJsonArray{"volume", "extended_sound_name"}}}, extended.host);
        Require(read.value("ok").toBool() && read.value("rows").toArray().first().toObject().value("fields").toArray().size() == 2,
            "extended get returns actual applicable fields");
        for (const auto& field : read.value("rows").toArray().first().toObject().value("fields").toArray())
            Require(!field.toObject().value("writable").toBool(), "every extended get field is readonly");
        const auto edit = InvokeMapDataTool("sounds_update", SoundRequest(extended, 0, {SoundChange(extended, 0, "volume", 80)}), extended.host);
        Require(Code(edit) == "MAP_FIELD_SOURCE_UNSAFE" && extended.publishes == 0 && extended.session->Revision() == 0
            && extended.current->Member(MapDataKind::Sounds).sources.front().data == source, "v2/v3 update refuses without native conversion or history");
    }
    auto invalidMembers = SoundMembers(); invalidMembers.back().data.push_back(std::byte{0});
    Fixture invalid(false, std::move(invalidMembers));
    const auto invalidList = InvokeMapDataTool("sounds_list", {}, invalid.host);
    Require(Code(invalidList) == "MAP_SOUND_TABLE_UNAVAILABLE" && !invalidList.contains("rows") && invalidList.value("member").toObject().value("state") == "invalid",
        "damaged sound source keeps diagnostic member state and never fabricates an empty table");
    Fixture missing;
    const auto absent = InvokeMapDataTool("sounds_list", {}, missing.host);
    Require(absent.value("ok").toBool() && absent.value("rows").toArray().isEmpty() && absent.value("member").toObject().value("state") == "missing", "missing sound source is explicit");
    Fixture duplicates(false, SoundMembers({"same", "same"}));
    const auto duplicateList = InvokeMapDataTool("sounds_list", {}, duplicates.host);
    Require(duplicateList.value("ok").toBool() && duplicateList.value("rows").toArray().size() == 2
        && !duplicateList.value("rows").toArray().first().toObject().value("writable").toBool(), "duplicate raw names are visible readonly");
    Require(Code(InvokeMapDataTool("sounds_get", QJsonObject{{"sound_id", "sound:73616d65"}}, duplicates.host)) == "MAP_FIELD_SOUND_NAME_AMBIGUOUS", "duplicate raw names never choose a row");
    Fixture symbols(false, SoundMembers({"same", "gg_snd_same"}));
    Require(!InvokeMapDataTool("sounds_list", {}, symbols.host).value("rows").toArray().first().toObject().value("writable").toBool()
        && Code(InvokeMapDataTool("sounds_update", SoundRequest(symbols, 0, {SoundChange(symbols, 0, "volume", 80)}), symbols.host)) == "MAP_FIELD_SOUND_NAME_AMBIGUOUS",
        "unique raw name cannot authorize a duplicate generated symbol");
    Fixture large(false, SoundMembers({"Owned sound"}, std::string(90000, '"')));
    const auto untouched = large.current->Member(MapDataKind::Sounds).sources.front().data;
    Require(!InvokeMapDataTool("sounds_list", {}, large.host).value("ok").toBool(), "escaped list exceeds full response budget");
    Require(!InvokeMapDataTool("sounds_update", SoundRequest(large, 0, {SoundChange(large, 0, "volume", 80)}), large.host).value("ok").toBool()
        && large.publishes == 0 && large.current->Member(MapDataKind::Sounds).sources.front().data == untouched, "full escaped update receipt is budgeted before any publication");
    names.clear(); for (int index = 0; index < 4097; ++index) names.push_back("No-match " + std::to_string(index));
    Fixture bounded(false, SoundMembers(names));
    const auto scan = InvokeMapDataTool("sounds_list", QJsonObject{{"query", "absent"}}, bounded.host);
    Require(scan.value("ok").toBool() && scan.value("rows").toArray().isEmpty() && scan.value("matched_total").isNull()
        && scan.value("examined") == "4096" && scan.value("scan_budget_exhausted").toBool() && scan.value("truncated").toBool(), "bounded empty page never invents global zero matches");
    const auto tail = InvokeMapDataTool("sounds_list", QJsonObject{{"query", "absent"}, {"page", QJsonObject{{"cursor", scan.value("next_cursor")}}}}, bounded.host);
    Require(tail.value("ok").toBool() && tail.value("examined") == "1" && !tail.value("truncated").toBool() && tail.value("matched_total").isNull(), "sound scan cursor resumes at actual next row");
}
void AuxiliaryFieldsShareBothHostTransactions() {
    for (const bool standalone : {false, true}) {
        Fixture fixture(standalone);
        const auto region = InvokeMapDataTool("entities_set_fields", fixture.Request({
            fixture.Change(0, "weather_id", "Rain"), fixture.Change(0, "color_bgra", "3351057")}), fixture.host);
        Require(region.value("ok").toBool(), "both hosts accept shared region weather and color fields");
        const auto& actual = fixture.current->Regions()->entries[0];
        Require(actual.weatherId == std::array{std::byte{'R'}, std::byte{'a'}, std::byte{'i'}, std::byte{'n'}}
            && actual.colorBgra[3] == 0, "full raw weather and reserved BGRA byte retained");
        Require(Code(InvokeMapDataTool("entities_set_fields", fixture.Request({
            fixture.Change(0, "color_bgra", "2134062101")}), fixture.host)) == "REGION_COLOR_RESERVED",
            "both hosts preserve the unknown fourth color byte");
        const auto camera = InvokeMapDataTool("entities_set_fields", fixture.Request({
            fixture.Change(0, "rotation", "75", MapDataKind::Cameras),
            fixture.Change(0, "angle_of_attack", "295", MapDataKind::Cameras),
            fixture.Change(0, "roll", "12", MapDataKind::Cameras),
            fixture.Change(0, "field_of_view", "80", MapDataKind::Cameras),
            fixture.Change(0, "far_z", "6000", MapDataKind::Cameras)}, "cameras"), fixture.host);
        Require(camera.value("ok").toBool() && fixture.current->Cameras()->entries[0].roll == 12,
            "both hosts use the same complete known camera fields");
        Require(Code(InvokeMapDataTool("entities_set_fields", fixture.Request({
            fixture.Change(0, "near_z", "150", MapDataKind::Cameras)}, "cameras"), fixture.host)) == "MAP_FIELD_READ_ONLY",
            "classic unknown slot cannot be opened by MCP");
        const auto frozen = FreezeOriginalMapDataFields(*fixture.original, *fixture.current);
        Require(frozen.Succeeded() && frozen.value->Operations().size() == 7,
            "new fields survive original-source freeze and exact native replay");
    }
}
} // namespace

int main() {
    try {
        TwoDefinitionsAndSuccessfulOriginalHosts(); IllegalAndLateBatchFailuresNeverPublish(); CompleteEscapedReceiptsAreBudgetedBeforePublish();
        GuiFieldsPreservePriorAuthorEditsAndOriginalUndo(); PagingReadonlyAndUnsafePresence(); StandaloneCumulativeLimitAndRestoreToOriginal();
        FilterPaginationIsExactAndPreservesCurrentOriginalSession(); InvalidFilterAndCursorNeverBecomeSilentEmptyTables();
        FilterScanCapContinuesWithoutInventingGlobalTotals();
        SpatialFiltersUseSameBoundsAndOriginalRowsAcrossHosts(); SpatialInvalidCoordinatesCursorsAndScanLimitsRefusePrecisely();
        UnitPositionsUseOriginalAtomicFieldsAndRowCapabilities();
        DWireLiteralSharedStringsAndOriginalUndo(); DSettingsReferencesRequireExplicitSharedModeAndUseOriginalUndo(); DFailuresPagingAndUnresolvedText();
        SoundsUseStableRawNamesTypedValuesAndOriginalUndo(); SoundFailuresNeverPublishOrCoerceTypes(); SoundQueriesBindPaginationCoverageAndBudgets();
        AuxiliaryFieldsShareBothHostTransactions();
        std::cout << "实体、地图文字与声音工具：19 组场景通过\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
