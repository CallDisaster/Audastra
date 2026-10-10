// 使用改造前的独立全字节 writer 验证；不通过优化 writer 自算期望值。
void GraphPathMemoLegacyEquivalence(const UiCatalog& catalog) {
    auto document = Empty(catalog);
    for (unsigned index = 0; index < 12; ++index)
        Take(TriggerDirectoryEditor::AddCategory(document, "Path" + std::to_string(index)));
    const auto trigger = Take(TriggerDocumentEditor::AddTrigger(
        document, document.categories.front().categoryId, "PathMemo"));
    for (unsigned index = 0; index < 20; ++index)
        Take(TriggerDocumentEditor::AppendRootEca(
            document, catalog, trigger, UiDefinitionKind::Action, "DoNothing"));
    const auto baselineWire = Wire(document, catalog);
    const auto exact = [&] {
        const auto reference = LegacyGraphBytes(document, catalog);
        const auto identity = Take(CaptureTriggerGraphReplayIdentity(document, catalog));
        Require(identity.Digest() == Take(Sha256Text(reference.bytes))
            && identity.BytesCharged() == reference.bytes.size(), "路径复用仍须精确保留旧 generic UTF-8 字节帧。");
        const auto encoded = Wire(document, catalog);
        Require(identity.CurrentWtgSha256() == Take(Sha256(encoded.wtg))
            && identity.CurrentWctSha256() == Take(Sha256(encoded.wct)), "公开捕获仍返回真实配对摘要。");
        Require(identity.EncodedBytes() == encoded.wtg.size() + encoded.wct.size(), "路径暂存不改变成员字节账。");
        Require(identity.ScratchBytes() < Sha256Stream::kBufferBytes + 160 * 1024,
            "夹具的路径缓存与最大实际临时容量须有界；这不是 RSS 证明。");
        Require(VerifyTriggerGraphReplayIdentity(document, catalog, identity).Succeeded(), "完整核验接受实际相同文档。");
        return identity;
    };
    exact(); // 空 origin 和实际 Catalog 路径。
    document.categories.front().origin.source = std::filesystem::path(std::wstring(2048, L'x'));
    exact(); // 缓存尚有空间时的长度边界。
    document.categories.front().origin.source = std::filesystem::path(std::wstring(2049, L'x'));
    exact(); // 长一字符须回退而仍保持旧字节帧。
    const std::filesystem::path repeated(L"原创/中文 😀/same.wtg");
    for (auto& category : document.categories) category.origin.source = repeated;
    for (auto& record : document.triggers) record.origin.source = repeated;
    for (auto& node : document.ecaNodes) {
        node.origin.source = repeated;
        node.uiSource = repeated;
    }
    const auto repeatedIdentity = exact();
    document.ecaNodes.back().uiSource = std::filesystem::path(L"原创/中文 😀/changed.wtg");
    Same(baselineWire, Wire(document, catalog));
    Require(!VerifyTriggerGraphReplayIdentity(document, catalog, repeatedIdentity).Succeeded(),
        "命中路径的元数据修改仍须拒绝，不能由 memo 放行。");
    document.ecaNodes.back().uiSource = repeated;
    Require(VerifyTriggerGraphReplayIdentity(document, catalog, repeatedIdentity).Succeeded(), "实际原路径恢复后才接受。");
    for (std::size_t index = 0; index < document.categories.size(); ++index)
        document.categories[index].origin.source = std::filesystem::path(L"原创/unique") / std::to_wstring(index);
    document.ecaNodes[0].uiSource = std::filesystem::path(L"relative\\nested\\file.wtg");
    document.ecaNodes[1].uiSource = std::filesystem::path(L"relative/nested/file.wtg");
    document.ecaNodes[2].uiSource = std::filesystem::path(std::wstring(2048, L'x'));
    document.ecaNodes[3].uiSource = std::filesystem::path(std::wstring(2049, L'y'));
    const auto uncachedIdentity = exact(); // 超过八个不同路径及短路径上限，原转换回退。
    document.categories.back().origin.source += L"-changed";
    Same(baselineWire, Wire(document, catalog));
    Require(!VerifyTriggerGraphReplayIdentity(document, catalog, uncachedIdentity).Succeeded(),
        "缓存满后的元数据也必须参与完整核验。");
    document.categories.back().origin.source = std::filesystem::path(L"原创/unique")
        / std::to_wstring(document.categories.size() - 1);
    exact();
    auto invalid = document;
    invalid.triggers.front().ecas.push_back(static_cast<std::uint32_t>(invalid.ecaNodes.size() + 1));
    Require(!VerifyTriggerGraphReplayIdentity(invalid, catalog, uncachedIdentity).Succeeded(),
        "省去核验未消费的配对摘要不能绕过真实图校验。");
    Require(!VerifyTriggerGraphReplayIdentity(document, catalog, TriggerGraphReplayIdentity{}).Succeeded(),
        "空摘要仍不是核验凭据。");
}
