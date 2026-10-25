#pragma once

void DoodadDisplayParentUsesTheFormalSaveAndUndoChain() {
    Fixture fixture; UniformDoodad(fixture);
    auto document = Take(NativeObjectDocument::CreateEmpty(ObjectType::Doodad));
    Check(document.AddOriginalObject("Doo0"));
    Check(document.AddCustomObject("NewD", "Doo0"));
    Check(document.AddModification({ObjectType::Doodad, "Doo0"}, {"Cp00"}, NativeValueType::Int32, std::int32_t{0}));
    Check(document.AddModification({ObjectType::Doodad, "Doo0"}, {"Sc00"}, NativeValueType::Unreal, 2.f));
    Check(document.AddModification({ObjectType::Doodad, "NewD"}, {"Nm00"}, NativeValueType::String, std::string("child")));
    const auto objects = Take(NativeObjectCodec(ObjectType::Doodad).Serialize(document));
    fixture.members.push_back({"war3map.w3d", objects});
    fixture.members.push_back({"Doodads/DoodadMetaData.slk", Slk({"ID", "field", "slk", "type", "index", "repeat"},
        {{"Dt00", "pathTex", "DoodadData", "pathingTexture", "0", "0"},
         {"Cp00", "canPlaceRandScale", "DoodadData", "bool", "0", "0"},
         {"Sc00", "defScale", "DoodadData", "unreal", "0", "0"},
         {"Nm00", "Name", "DoodadData", "string", "0", "0"}})});
    auto placements = Take(ParseDoodadPlacements(fixture.Member("war3map.doo")));
    placements.doodads.front().rawcode = "NewD";
    fixture.Member("war3map.doo") = Take(placements.Serialize());
    fixture.Open(); fixture.Paint();
    auto deep = fixture.ExpectedPathing(true), shallow = fixture.ExpectedPathing(false);
    UniformDoodadExpectedFootprint(deep); UniformDoodadExpectedFootprint(shallow);
    fixture.SaveExpected("display-parent-water.w3x", fixture.ExpectedTerrain(true), deep, nullptr, true);
    Require(fixture.edits->Undo(), "undo saved water with a validated display-only parent");
    fixture.SaveExpected("display-parent-undo.w3x", fixture.ExpectedTerrain(false), shallow, nullptr, true);
    Require(fixture.edits->Redo(), "redo saved water with a validated display-only parent");
    fixture.SaveExpected("display-parent-redo.w3x", fixture.ExpectedTerrain(true), deep, nullptr, true);
    Require(Take(fixture.archive->ReadMember("war3map.w3d")) == objects,
        "formal saved history preserves the full Original and Custom object bytes");
}
