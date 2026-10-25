#pragma once

void TestPathingSaveSchemaAndTransport() {
    Fixture fixture; McpServer server; RegisterAllTools(server); ScopedSaveRoots roots; roots.Set(fixture.ui);
    const auto listing = server.HandleMessage(R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}})");
    Expect(listing && !listing->error, "pathing save tool schema must be available");
    bool found = false;
    for (const auto& toolValue : listing->result.toObject().value("tools").toArray()) {
        const auto tool = toolValue.toObject();
        if (tool.value("name") != "save_map_edit_candidate") continue;
        const auto properties = tool.value("inputSchema").toObject().value("properties").toObject();
        for (const auto& variantValue : properties.value("terrain_strokes").toObject()
                .value("items").toObject().value("oneOf").toArray()) {
            const auto variant = variantValue.toObject();
            const auto fields = variant.value("properties").toObject();
            if (fields.value("kind").toObject().value("const") != "pathing_restriction") continue;
            found = fields.size() == 8 && variant.value("required").toArray().size() == 8
                && variant.value("additionalProperties") == false
                && fields.value("blocked_type").toObject().value("enum").toArray() == QJsonArray{"walk", "fly", "build"}
                && fields.value("value").toObject().value("type") == "boolean"
                && fields.value("size").toObject().value("minimum").toInt() == 1
                && fields.value("size").toObject().value("maximum").toInt() == 8
                && !fields.contains("radius") && !fields.contains("pathing_restrictions");
        }
    }
    Expect(found, "pathing schema must expose only named restrictions and its exact bounded footprint");
    const QJsonObject stroke{{"kind", "pathing_restriction"}, {"blocked_type", "walk"},
        {"shape", "circle"}, {"point_space", "world"}, {"size", 1}, {"value", true},
        {"points", QJsonArray{QJsonObject{{"x", 16}, {"y", 16}}}}, {"follow_doodads", false}};
    QJsonObject args{{"source_map_path", QString::fromStdWString((fixture.root / "missing.w3x").wstring())},
        {"output_path", QString::fromStdWString((fixture.root / "pathing-candidate.w3x").wstring())},
        {"expected_source_sha256", QString(64, 'a')}, {"expected_terrain_sha256", QString(64, 'b')}};
    const auto call = [&](QJsonObject value) {
        args["terrain_strokes"] = QJsonArray{value}; return Call(server, "save_map_edit_candidate", args);
    };
    for (const auto type : {"walk", "fly", "build"}) for (const auto shape : {"circle", "square"})
        for (const auto space : {"world", "terrain_vertex"}) for (const bool enabled : {true, false})
            for (const int size : {1, 8}) {
                auto valid = stroke; valid["blocked_type"] = type; valid["shape"] = shape;
                valid["point_space"] = space; valid["value"] = enabled; valid["size"] = size;
                Code(call(valid), "SOURCE_LEASE_FAILED");
            }
    for (const auto value : {QJsonValue("walks"), QJsonValue(2), QJsonValue(QJsonValue::Null)}) {
        auto invalid = stroke; invalid["blocked_type"] = value; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_PATHING_INVALID");
    }
    for (const auto value : {QJsonValue("true"), QJsonValue(1), QJsonValue(QJsonValue::Null)}) {
        auto invalid = stroke; invalid["value"] = value; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_PATHING_INVALID");
    }
    for (const auto field : {"value", "blocked_type"}) {
        auto invalid = stroke; invalid.remove(field); Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_PATHING_INVALID");
    }
    for (const int size : {0, 9}) {
        auto invalid = stroke; invalid["size"] = size; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_PATHING_INVALID");
    }
    auto invalid = stroke; invalid["size"] = "1"; Code(call(invalid), "MCP_INTEGER_REQUIRED");
    invalid = stroke; invalid["shape"] = "triangle"; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_SHAPE_INVALID");
    invalid = stroke; invalid["follow_doodads"] = true; Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_FOLLOW_UNSUPPORTED");
    for (const auto field : {"radius", "strength", "seed", "pathing_restrictions", "loaded_rules", "set_boundary"}) {
        invalid = stroke; invalid[field] = 2; Code(call(invalid), "MCP_MAP_EDIT_FIELD_UNSUPPORTED");
    }
    invalid = stroke; invalid["point_space"] = "terrain_vertex";
    invalid["points"] = QJsonArray{QJsonObject{{"x", 1.5}, {"y", 4}}};
    Code(call(invalid), "MCP_MAP_EDIT_TERRAIN_POINT_INVALID");
    Expect(!std::filesystem::exists(fixture.root / "pathing-candidate.w3x"),
        "invalid or unavailable-source pathing never publishes a partial map");
}
