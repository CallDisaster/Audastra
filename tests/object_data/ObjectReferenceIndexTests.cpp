#include "War3MapCore/MapDocument.h"
#include "War3MapCore/ObjectData/ObjectFieldSchema.h"
#include "War3MapCore/ObjectData/ObjectReferenceIndex.h"

#include "../MapDataFixtures.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace War3Map;
using namespace War3Map::ObjectData;

void Require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

std::vector<std::byte> Bytes(std::string_view text) {
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return {first, first + text.size()};
}

void Transform(MapDataFixtures::Fixture& fixture, std::string_view rawcode) {
    fixture.Raw(rawcode);
    fixture.Int(0);
    for (int i = 0; i < 4; ++i) fixture.Float(0);
    for (int i = 0; i < 3; ++i) fixture.Float(1);
}

std::vector<std::byte> UnitBytes() {
    MapDataFixtures::Fixture fixture;
    fixture.Raw("W3do"); fixture.Int(8); fixture.Int(11); fixture.Int(2);
    Transform(fixture, "H001");
    fixture.Byte(0); fixture.Int(0); fixture.Byte(0); fixture.Byte(0);
    fixture.Int(-1); fixture.Int(-1); fixture.Int(-1);
    fixture.Int(1); fixture.Int(1); fixture.Raw("I001"); fixture.Int(100);
    fixture.Int(0); fixture.Float(0);
    for (int i = 0; i < 4; ++i) fixture.Int(0);
    fixture.Int(1); fixture.Int(0); fixture.Raw("I002");
    fixture.Int(1); fixture.Raw("A001"); fixture.Int(0); fixture.Int(1);
    fixture.Int(-1); fixture.Int(-1); fixture.Int(-1); fixture.Int(101);
    Transform(fixture, "uDNR");
    fixture.Byte(0); fixture.Int(0); fixture.Byte(0); fixture.Byte(0);
    fixture.Int(-1); fixture.Int(-1); fixture.Int(-1); fixture.Int(0);
    fixture.Int(0); fixture.Float(0);
    for (int i = 0; i < 4; ++i) fixture.Int(0);
    fixture.Int(0); fixture.Int(0);
    fixture.Int(2); fixture.Int(1); fixture.Raw("hfoo"); fixture.Int(100);
    fixture.Int(-1); fixture.Int(-1); fixture.Int(102);
    return fixture.bytes;
}

std::vector<std::byte> DoodadBytes() {
    MapDataFixtures::Fixture fixture;
    fixture.Raw("W3do"); fixture.Int(8); fixture.Int(11); fixture.Int(1);
    Transform(fixture, "LTlt");
    fixture.Byte(0); fixture.Byte(100); fixture.Int(-1);
    fixture.Int(1); fixture.Int(1); fixture.Raw("I003"); fixture.Int(100);
    fixture.Int(201);
    fixture.Int(0); fixture.Int(1);
    fixture.Raw("YTst"); fixture.Int(0); fixture.Int(1); fixture.Int(2);
    return fixture.bytes;
}

ObjectFieldSchema Schema() {
    const std::string metadata =
        "ID;PWXL\n"
        "C;X1;Y1;K\"ID\"\n"
        "C;X2;Y1;K\"field\"\n"
        "C;X3;Y1;K\"slk\"\n"
        "C;X4;Y1;K\"type\"\n"
        "C;X1;Y2;K\"uabi\"\n"
        "C;X2;Y2;K\"abilList\"\n"
        "C;X3;Y2;K\"UnitAbilities\"\n"
        "C;X4;Y2;K\"abilityList\"\n"
        "C;X1;Y3;K\"ureq\"\n"
        "C;X2;Y3;K\"Requires\"\n"
        "C;X3;Y3;K\"UnitData\"\n"
        "C;X4;Y3;K\"techList\"\n"
        "E\n";
    ObjectFieldSchemaOptions options;
    options.sources = {{ObjectType::Unit, "unit.slk"}};
    options.stringFiles.clear();
    options.worldEditDataFile.clear();
    options.unitEditorDataFile.clear();
    auto schema = ObjectFieldSchema::Load(
        [&metadata](const std::string& path) {
            if (path == "unit.slk") return Result<std::vector<std::byte>>::Success(Bytes(metadata));
            return Result<std::vector<std::byte>>::Failure("Test", "missing", path);
        }, options);
    Require(schema.Succeeded(), "synthetic metadata schema");
    return std::move(*schema.value);
}

ObjectModification Modification(std::string field, Value value) {
    ObjectModification modification;
    modification.key.fieldId = std::move(field);
    modification.value = std::move(value);
    return modification;
}

const ReferenceInputCoverage& Coverage(const ObjectReferenceIndex& index, std::string_view member) {
    const auto found = std::find_if(index.Coverage().begin(), index.Coverage().end(),
        [member](const ReferenceInputCoverage& value) { return value.memberName == member; });
    Require(found != index.Coverage().end(), "expected coverage member");
    return *found;
}

void TestKnownAndPossibleReferences() {
    auto schema = Schema();
    ObjectRecord object;
    object.key = {ObjectType::Unit, "H001"};
    object.parentRawcode = "hfoo";
    object.modifications = {
        Modification("uabi", RawcodeListValue{{"A001", "A002"}}),
        Modification("ureq", RawcodeValue{"R001"}),
        Modification("unam", std::string{"A003"}),
        Modification("xxxx", RawcodeValue{"I001"}),
    };
    const std::array records{object};
    const std::array sources{ObjectRecordSource{
        ObjectType::Unit, "table/unit.ini", ReferenceInputState::Ready, records}};
    const std::vector<MapArchiveMember> members{
        {"war3mapUnits.doo", UnitBytes()},
        {"war3map.doo", DoodadBytes()},
    };
    const auto map = MapDocument::FromMembers(members);
    Require(map.Units().has_value() && map.Doodads().has_value(), "typed placement fixtures");
    const auto index = ObjectReferenceIndex::Build(sources, map, &schema);
    Require(Coverage(index, "table/unit.ini").state == ReferenceInputState::Partial,
        "unknown rawcode field leaves target coverage partial");
    Require(Coverage(index, "war3mapUnits.doo").state == ReferenceInputState::Ready,
        "parsed placements covered");
    Require(Coverage(index, "war3map.doo").state == ReferenceInputState::Ready,
        "parsed doodads covered");
    Require(Coverage(index, "ability").state == ReferenceInputState::Missing,
        "omitted object type is not an empty complete source");

    const auto ability = index.Find({ObjectType::Ability, "A001"});
    Require(ability.definite.size() == 2 && ability.possible.empty(),
        "metadata ability and placed ability are definite");
    Require(index.Find({ObjectType::Ability, "A003"}).definite.empty()
        && index.Find({ObjectType::Ability, "A003"}).possible.empty(),
        "plain string is never guessed as rawcode");
    const auto unit = index.Find({ObjectType::Unit, "H001"});
    Require(unit.definite.empty() && unit.possible.size() == 1,
        "unit/item placement requires type disambiguation");
    const auto parent = index.Find({ObjectType::Unit, "hfoo"});
    Require(parent.definite.size() == 2 && parent.possible.empty(),
        "parent and random custom unit choice have known type");
    Require(index.Find({ObjectType::Item, "hfoo"}).definite.empty()
        && index.Find({ObjectType::Item, "hfoo"}).possible.empty(),
        "exact parent and random choice do not leak to item type");
    Require(index.Find({ObjectType::Item, "I002"}).definite.size() == 1,
        "placed inventory item has known type");
    Require(index.Find({ObjectType::Item, "I003"}).definite.size() == 1,
        "doodad drop item has known type");
    Require(index.Find({ObjectType::Doodad, "LTlt"}).possible.size() == 1
        && index.Find({ObjectType::Destructable, "LTlt"}).possible.size() == 1,
        "doodad/destructable instance stays ambiguous");
    Require(index.Find({ObjectType::Unit, "R001"}).possible.size() == 1
        && index.Find({ObjectType::Upgrade, "R001"}).possible.size() == 1,
        "tech list metadata preserves two valid target categories");
    Require(index.Find({ObjectType::Ability, "I001"}).possible.size() == 1,
        "typed rawcode with unknown metadata is conservatively possible");
    Require(index.Find({ObjectType::Unit, "uDNR"}).possible.empty(),
        "random unit placeholder is not a placed type reference");
}

void TestIncompleteInputsAndOrdering() {
    ObjectRecord object;
    object.key = {ObjectType::Unit, "H001"};
    object.modifications = {Modification("xraw", RawcodeValue{"BAD"})};
    const std::array records{object};
    const std::array source{ObjectRecordSource{
        ObjectType::Unit, "table/unit.ini", ReferenceInputState::Invalid, records}};
    std::vector<MapArchiveMember> members{{"war3mapUnits.doo", UnitBytes()},
        {"war3map.doo", DoodadBytes()}};
    members[0].data = Bytes("bad");
    auto map = MapDocument::FromMembers(members);
    auto index = ObjectReferenceIndex::Build(source, map);
    Require(Coverage(index, "table/unit.ini").state == ReferenceInputState::Invalid,
        "invalid object source remains invalid after malformed typed field");
    Require(Coverage(index, "war3mapUnits.doo").state == ReferenceInputState::Invalid,
        "invalid placement source cannot be treated as no references");
    Require(index.Find({ObjectType::Unit, "H001"}).possible.empty(),
        "invalid placement did not expose fake partial placements");
    Require(index.Find({ObjectType::Ability, "BAD"}).possible.empty(),
        "malformed rawcode is not indexed as valid object identity");

    members[0].data = UnitBytes();
    map = MapDocument::FromMembers(members);
    const auto first = ObjectReferenceIndex::Build(source, map);
    const auto second = ObjectReferenceIndex::Build(source, map);
    Require(first.References().size() == second.References().size(), "deterministic size");
    for (std::size_t i = 0; i < first.References().size(); ++i) {
        Require(first.References()[i].rawcode == second.References()[i].rawcode
            && first.References()[i].location == second.References()[i].location
            && first.References()[i].memberName == second.References()[i].memberName,
            "deterministic reference ordering");
    }
}

} // namespace

int main() {
    try {
        TestKnownAndPossibleReferences();
        TestIncompleteInputsAndOrdering();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "Read-only object reference index checks passed\n";
    return 0;
}
