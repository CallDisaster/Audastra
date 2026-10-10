// 只使用原创已加载目录；旧完整图诊断 oracle 位于此前已 include 的 DiagnosticPathTests。
void CodecDefinitionMemoKeepsActualKeysCountsAndFullGraph(const std::filesystem::path& root) {
    Require(!std::filesystem::exists(root), "原创 memo 目录必须全新。");
    Write(root / "ui/config", "base\n");
    Write(root / "ui/base/define.txt", "[TriggerTypes]\ninteger=0,1,1,Integer,integer\n"
        "[Alpha]\nExactShared=alpha\n[Beta]\nExactShared=beta\n");
    std::string actions = "[ExactShared]\n[[.args]]\ntype=integer\n[NothingSentinel]\n[[.args]]\ntype=nothing\n";
    // 513 个实际不同 key 超过 512 槽：无论 hash 分布如何都必有碰撞/不缓存项。
    std::vector<std::string> names;
    names.reserve(513);
    for (std::size_t index = 0; index < 513; ++index) {
        names.push_back("MemoProbeWithOwnedKey" + std::to_string(index));
        actions += '[' + names.back() + "]\n";
        for (std::size_t argument = 0; argument < index % 3; ++argument) actions += "[[.args]]\ntype=integer\n";
    }
    Write(root / "ui/base/action.txt", actions);
    Write(root / "ui/base/call.txt", "[ExactShared]\nreturns=integer\n[[.args]]\ntype=integer\n[[.args]]\ntype=integer\n");
    auto catalog = Take(UiCatalog::Load(root));
    War3Map::Trigger::Detail::CodecDefinitionLookupMemo memo(catalog);
    const auto sameLookup = [&](UiDefinitionKind kind, std::string_view name, std::string_view group = {}) {
        const auto* expected = catalog.Find(kind, name, group);
        const auto actual = memo.Lookup(kind, name, group);
        Require(actual.definition == expected && actual.argumentCount == (expected ? expected->ArgumentCount() : 0),
            "memo 必须返回真实完整 kind/group/name key 的同一 definition 与实际参数数，碰撞不能授其它定义。");
        return actual;
    };
    const auto action = sameLookup(UiDefinitionKind::Action, "ExactShared");
    const auto call = sameLookup(UiDefinitionKind::Call, "ExactShared");
    Require(action.definition && call.definition && action.definition != call.definition
        && action.argumentCount == 1 && call.argumentCount == 2, "真实同名不同 kind 不能合并。");
    const auto alpha = sameLookup(UiDefinitionKind::Define, "ExactShared", "Alpha");
    const auto beta = sameLookup(UiDefinitionKind::Define, "ExactShared", "Beta");
    Require(alpha.definition && beta.definition && alpha.definition != beta.definition,
        "真实同 kind/name 不同 group 的定义不能合并。");
    Require(sameLookup(UiDefinitionKind::Action, "NothingSentinel").argumentCount == 0,
        "nothing 哨兵继续沿原 ArgumentCount，不改成 arguments.size。");
    for (const auto& name : names) (void)sameLookup(UiDefinitionKind::Action, name);
    for (auto name = names.rbegin(); name != names.rend(); ++name) (void)sameLookup(UiDefinitionKind::Action, *name);
    for (std::size_t repeat = 0; repeat < 8; ++repeat) {
        (void)sameLookup(UiDefinitionKind::Action, names.front());
        (void)sameLookup(UiDefinitionKind::Action, "ExactShared");
        (void)sameLookup(UiDefinitionKind::Call, "ExactShared");
        Require(sameLookup(UiDefinitionKind::Action, "MissingOwnedDefinition").definition == nullptr,
            "未知项每次仍真实查找，不缓存伪定义。");
        Require(sameLookup(UiDefinitionKind::Unknown, "ExactShared").definition == nullptr,
            "未知 kind 不能借同名已知定义授资格。");
    }

    auto document = Empty(catalog);
    const auto triggerId = Take(TriggerDocumentEditor::AddTrigger(document,
        document.categories.front().categoryId, "MemoFullGraph"));
    StableId greatest = 0;
    for (const auto& row : document.categories) greatest = (std::max)(greatest, row.id);
    for (const auto& row : document.variables) greatest = (std::max)(greatest, row.id);
    for (const auto& row : document.triggers) greatest = (std::max)(greatest, row.id);
    Require(greatest <= (std::numeric_limits<StableId>::max)() - 4096, "原创完整图 ID 预留须真实有界。");
    auto nextId = greatest + 1;
    const auto append = [&](const std::string& name) {
        const auto* definition = catalog.Find(2, name);
        Require(definition != nullptr, "图节点必须来自实际目录。");
        EcaNode node; node.id = nextId++; node.sourceTriggerId = triggerId; node.type = 2;
        node.name = {name, TextEncoding::Utf8, {}};
        node.uiSource = definition->Active().source; node.uiLayer = definition->Active().layer;
        node.uiSignature = definition->Signature();
        for (std::size_t index = 0; index < definition->ArgumentCount(); ++index) {
            ArgumentNode argument; argument.id = nextId++; argument.type = 3;
            argument.value = {"1", TextEncoding::Utf8, {}};
            node.arguments.push_back(static_cast<std::uint32_t>(document.argumentNodes.size()));
            document.argumentNodes.push_back(std::move(argument));
        }
        document.triggers.front().ecas.push_back(static_cast<std::uint32_t>(document.ecaNodes.size()));
        document.ecaNodes.push_back(std::move(node));
    };
    for (const auto& name : names) append(name);
    for (std::size_t repeat = 0; repeat < 8; ++repeat) append(names.front());
    Require(CheckCodecDiagnosticPaths(document, catalog).Succeeded(),
        "513 真定义及重复节点的完整公共 Validate/Serialize/Parse/旧布局身份必须通过。");
    const auto original = Wire(document, catalog);
    {
        auto broken = document;
        broken.ecaNodes.front().name.text = "ExactShared"; // 原零参数节点，对应实际一参数定义。
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        Require(std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [](const auto& diagnostic) {
            return diagnostic.code == "wtg.eca.argument-count";
        }), "缓存不能吞真实 ArgumentCount 错误，完整错误 oracle 必须一致。");
    }
    {
        auto broken = document;
        broken.ecaNodes.back().name.text = "MissingOwnedDefinition";
        broken.ecaNodes.front().enabled = 2;
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        Require(status.diagnostics.size() == 2 && status.diagnostics.front().code == "wtg.eca.enabled"
            && status.diagnostics.back().code == "wtg.eca.unknown-ui", "晚未知及早错误的旧完整诊断顺序必须保留。");
    }
    {
        auto broken = document;
        broken.ecaNodes.front().type = 3;
        Require(!CheckCodecDiagnosticPaths(broken, catalog).Succeeded(),
            "真实同名仅 action 定义不能作为 call 类型授资格。");
    }
    Same(original, Wire(document, catalog));
    // 新 capture 必须借其自己的真实目录，不能沿用前一 capture 的定义/参数数。
    Write(root / "other/ui/config", "base\n");
    Write(root / "other/ui/base/action.txt", "[ExactShared]\ntitle=Different\n");
    auto other = Take(UiCatalog::Load(root / "other"));
    War3Map::Trigger::Detail::CodecDefinitionLookupMemo otherMemo(other);
    const auto own = otherMemo.Lookup(2, "ExactShared");
    Require(own.definition == other.Find(2, "ExactShared") && own.definition != action.definition
        && own.argumentCount == 0 && memo.Lookup(2, "ExactShared").argumentCount == 1,
        "每调用真实 owner/实际数量隔离，前一 memo 不跨代或跨目录。");
}
