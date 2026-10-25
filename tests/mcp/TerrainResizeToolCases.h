#pragma once

void ResizeToolRequiresBoundSourcesAndStrictIntent() {
    Fixture fixture; auto host = fixture.Host();
    const auto state = InvokeTerrainEditTool("get_terrain_edit_state", {}, host);
    Expect(state.value("ok").toBool(), "current terrain binding is available");
    QJsonObject request{{"project_identity", "terrain-fixture"}, {"expected_revision", "0"},
        {"terrain_binding", state.value("terrain_binding")}, {"west", -32}, {"south", -32}, {"east", 32}, {"north", 32}, {"dry_run", true}};
    const auto code = [&](const QJsonObject& arguments) {
        const auto diagnostics = InvokeTerrainEditTool("edit_terrain_resize", arguments, host).value("diagnostics").toArray();
        return diagnostics.isEmpty() ? QString{} : diagnostics.front().toObject().value("code").toString();
    };
    Expect(code(request) == "TERRAIN_RESIZE_HOST_UNAVAILABLE", "resize without actual rule owner is unavailable rather than successful");
    auto invalid = request; invalid["expected_revision"] = "999";
    Expect(code(invalid) == "TERRAIN_EDIT_STALE", "resize preserves revision CAS before source preparation");
    invalid = request; invalid["terrain_binding"] = QJsonObject{};
    Expect(code(invalid) == "TERRAIN_EDIT_STALE", "resize does not accept client-generated terrain identities");
    for (const char* field : {"verified", "rules", "algorithm", "raw", "clip", "move_objects", "follow_doodads"}) {
        invalid = request; invalid[field] = true;
        Expect(code(invalid) == "TERRAIN_ARGUMENT_UNKNOWN", "resize cannot grant source qualification, clip or move authority");
    }
    invalid = request; invalid.remove("dry_run");
    Expect(code(invalid) == "TERRAIN_ARGUMENT_REQUIRED", "resize preview versus commit is explicit");
    invalid = request; invalid["dry_run"] = "true";
    Expect(code(invalid) == "TERRAIN_ARGUMENT_TYPE", "resize dry_run must be boolean");
    for (const char* field : {"west", "south", "east", "north"}) {
        invalid = request; invalid[field] = 1.5;
        Expect(code(invalid) == "TERRAIN_ARGUMENT_NUMBER", "resize edges are exact signed integers");
        invalid = request; invalid[field] = 2147483648.0;
        Expect(code(invalid) == "TERRAIN_ARGUMENT_NUMBER", "resize rejects edges outside int32 before narrowing");
    }
    Expect(fixture.edits.Revision() == 0 && !fixture.edits.IsDirty(), "every unavailable or invalid resize leaves the session unchanged");
}
