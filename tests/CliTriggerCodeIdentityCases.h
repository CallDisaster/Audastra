#pragma once
void CustomCodeIdentityCliUsesNormalSave() {
    namespace I = AudastraTest::CodeIdentity;
    for (const bool copy : {false, true}) {
        I::Fixture f; auto direct = f.Request(copy ? "cli-direct-copy" : "cli-direct-rename");
        direct.triggerCodeIdentity = I::Take(War3Map::Services::ParseTriggerCodeIdentityEdit(I::Envelope(f.Edit("CodeNew", copy), false)));
        I::Take(War3Map::Services::SaveMapEditCandidate(direct));
        const auto file = f.base.root / "identity-command.json";
        { std::ofstream output(file, std::ios::binary); output << I::Envelope(f.Edit("CodeNew", copy), true); }
        const auto candidate = f.base.root / "cli-code-identity.w3x";
        const auto sha = std::wstring(f.source.sha256.begin(), f.source.sha256.end());
        std::vector<std::wstring> args{L"war3-map-cli", L"map-edit-save", f.source.path.wstring(), sha, L"-", f.base.inputs.tool.wstring(),
            f.base.inputs.game.wstring(), candidate.wstring(), L"--trigger-code-edit-file", file.wstring()};
        auto preview = Invoke(args);
        if (preview.exitCode != 0 || preview.output.find("preview_only=true") == std::string::npos) std::cerr << "CLI preview exit=" << preview.exitCode << " stdout=" << preview.output << " stderr=" << preview.error << '\n';
        Require(preview.exitCode == 0 && preview.output.find("preview_only=true") != std::string::npos && !std::filesystem::exists(candidate), "CLI code preview stays readonly");
        Require(preview.output.find("\"before\":{") != std::string::npos && preview.output.find("\"after\":{") != std::string::npos
            && preview.output.find(f.Edit().expected.currentWtgSha256) != std::string::npos, "CLI preview exposes before/after graph identities");
        { std::ofstream output(file, std::ios::binary | std::ios::trunc); output << I::Envelope(f.Edit("CodeNew", copy), false); }
        auto saved = Invoke(args); Require(saved.exitCode == 0 && saved.output.find("preview_only=false") != std::string::npos, "CLI code normal save succeeds");
        auto actual = I::Take(War3Map::MapArchiveSession::Open(candidate)), expected = I::Take(War3Map::MapArchiveSession::Open(direct.targetCandidate));
        for (const auto member : {"war3map.wtg", "war3map.wct", "war3map.j"})
            Require(I::Take(actual.ReadMember(member)) == I::Take(expected.ReadMember(member)), "CLI and service code artifacts identical");
        auto missing = I::Envelope(f.Edit(), false); const auto key = missing.find("\"dry_run\":false,"); missing.erase(key, std::string("\"dry_run\":false,").size());
        { std::ofstream output(file, std::ios::binary | std::ios::trunc); output << missing; }
        Require(Invoke(args).exitCode != 0, "CLI missing explicit dry_run rejected before save");
    }
}
