#pragma once
void TestCodeIdentityGraphTransport() {
    namespace I = AudastraTest::CodeIdentity;
    I::Fixture f; McpServer server; RegisterAllTools(server); ScopedSaveRoots roots;
    qputenv("FORGE_MCP_YDWE_ROOT", QString::fromStdWString(f.base.inputs.tool.wstring()).toUtf8());
    qputenv("FORGE_MCP_WAR3_ROOT", QString::fromStdWString(f.base.inputs.game.wstring()).toUtf8());
    const auto root = f.base.root / "code-workspace";
    for (const auto& member : f.members) Write(root / "map" / member.name, member.data);
    server.SetWorkspace(root, f.base.inputs.tool); Expect(server.Workspace() != nullptr, "code workspace opens");
    auto* workspace = server.Workspace(); const auto revision = workspace->Revision();
    const auto transaction = I::Take(workspace->BeginEdit(revision));
    auto context = I::Take(workspace->CodeIdentityContext()); auto edit = f.Edit();
    edit.expected = I::Take(Trigger::CaptureTriggerGraphBinding(workspace->VisibleDocument(), workspace->Catalog(), context.mapObjects));
    const auto& row = *std::find_if(workspace->VisibleDocument().triggers.begin(), workspace->VisibleDocument().triggers.end(), [](const auto& t) { return t.name.text == "CodeSource"; });
    edit.triggerId = row.id; edit.expectedName = row.name;
    const auto queried = Call(server, "query_trigger_graph", {{"table", "triggers"}, {"start", 0}, {"limit", 128}});
    Expect(queried.value("ok").toBool(), QJsonDocument(queried).toJson().constData());
    bool discovered = false;
    for (const auto& value : queried.value("rows").toArray()) {
        const auto item = value.toObject();
        if (item.value("name").toObject().value("text").toString() == "CodeSource") {
            edit.expectedCodeSha256 = item.value("custom_code_sha256").toString().toStdString(); discovered = edit.expectedCodeSha256.size() == 64;
        }
    }
    Expect(discovered, "MCP exposes actual code-slot SHA so clients need not read code files");
    auto envelope = QJsonDocument::fromJson(QByteArray::fromStdString(I::Envelope(edit, true))).object();
    envelope["transaction_id"] = QString::number(transaction); envelope["expected_revision"] = RevisionJson(revision);
    auto missing = envelope; missing.remove("dry_run"); Code(Call(server, "edit_trigger_graph", missing), "TRIGGER_CODE_REQUEST_INVALID");
    auto preview = Call(server, "edit_trigger_graph", envelope); Expect(preview.value("ok").toBool() && !preview.value("published").toBool(), QJsonDocument(preview).toJson().constData());
    Expect(workspace->VisibleDocument().triggers.size() == f.triggers.triggers.size(), "MCP dry run no new graph");
    Expect(workspace->VisibleDocument().triggers.back().name.text == "CodeSource", "MCP dry run retains source name");
    envelope["dry_run"] = false;
    auto changed = Call(server, "edit_trigger_graph", envelope); Expect(changed.value("ok").toBool() && changed.value("published").toBool(), QJsonDocument(changed).toJson().constData());
    Expect(workspace->VisibleDocument().triggers.back().name.text == "CodeNew", "MCP actual graph changed");
    Expect(!Call(server, "edit_trigger_graph", envelope).value("ok").toBool(), "MCP old binding refuses repeat");
    auto previewed = workspace->PreviewEdit(transaction, revision); Expect(previewed.Succeeded(), "MCP preview transaction");
    const auto validated = workspace->ValidateEdit(transaction, revision); Expect(validated.satisfied, "MCP validates transaction");
    auto request = f.Request("mcp-code-normal-save");
    const auto saved = I::Take(workspace->ExportArchiveCandidate(transaction, revision, request));
    auto direct = f.Request("mcp-code-direct"); direct.triggerCodeIdentity = I::Take(Services::ParseTriggerCodeIdentityEdit(I::Envelope(f.Edit(), false)));
    I::Take(Services::SaveMapEditCandidate(direct));
    auto actual = I::Take(MapArchiveSession::Open(saved.candidate)), expected = I::Take(MapArchiveSession::Open(direct.targetCandidate));
    for (const auto member : {"war3map.wtg", "war3map.wct", "war3map.j"}) Expect(I::Take(actual.ReadMember(member)) == I::Take(expected.ReadMember(member)), "MCP and direct normal artifacts identical");
    Expect(workspace->Rollback(transaction, revision).Succeeded(), "MCP rollback independent directory");
    Expect(workspace->VisibleDocument().triggers.back().name.text == "CodeSource", "MCP rollback restored code and name");
}
