// PMR01 保留的旧路径算法原文 oracle（只在原创夹具运行）。
// 保留按值路径 + std::to_string；不调用产品的 DiagnosticPath 或改写判定顺序。
namespace LegacyCodecDiagnosticPaths {
Diagnostic MakeDiagnostic(
    DiagnosticSeverity severity,
    std::string code,
    std::string message,
    std::optional<OriginSpan> origin = std::nullopt) {
    return Diagnostic{
        severity,
        "Trigger.WtgWct",
        std::move(code),
        std::move(message),
        std::move(origin),
    };
}

struct GraphState {
    std::vector<std::uint8_t> ecaState;
    std::vector<std::uint8_t> argumentState;
    std::vector<std::size_t> ecaReferences;
    std::vector<std::size_t> argumentReferences;
};

void AddValidationError(Status& status, std::string code, std::string message) {
    status.diagnostics.push_back(MakeDiagnostic(
        DiagnosticSeverity::Error, std::move(code), std::move(message)));
}

void ValidateArgument(
    std::uint32_t index,
    StableId owner,
    const TriggerDocument& document,
    const UiCatalog& catalog,
    GraphState& graph,
    Status& status,
    std::string path,
    std::size_t depth);

void ValidateEca(
    std::uint32_t index,
    bool child,
    StableId owner,
    const TriggerDocument& document,
    const UiCatalog& catalog,
    GraphState& graph,
    Status& status,
    std::string path,
    std::size_t depth) {
    if (depth > 256) {
        AddValidationError(status, "wtg.graph.depth", path + ": nesting exceeds 256 levels");
        return;
    }
    if (index >= document.ecaNodes.size()) {
        AddValidationError(status, "wtg.eca.bad-index", path + ": ECA index is out of range");
        return;
    }
    ++graph.ecaReferences[index];
    if (graph.ecaReferences[index] > 1) {
        AddValidationError(status, "wtg.eca.shared-node", path + ": ECA node has multiple owners");
        return;
    }
    if (graph.ecaState[index] == 1) {
        AddValidationError(status, "wtg.eca.cycle", path + ": ECA graph contains a cycle");
        return;
    }
    graph.ecaState[index] = 1;
    const EcaNode& eca = document.ecaNodes[index];
    const UiDefinition* definition = catalog.Find(eca.type, eca.name.text);
    if (definition == nullptr) {
        AddValidationError(status, "wtg.eca.unknown-ui", path + ": unknown UI definition [" + eca.name.text + "]");
    } else {
        if (definition->ArgumentCount() != eca.arguments.size()) {
            AddValidationError(status, "wtg.eca.argument-count", path +
                ": argument count does not match " + definition->Signature());
        }
    }
    if (eca.id == 0) {
        AddValidationError(status, "wtg.eca.missing-id", path + ": stable id is zero");
    }
    if (eca.enabled != 0 && eca.enabled != 1) {
        AddValidationError(status, "wtg.eca.enabled", path + ": enabled must be 0 or 1");
    }
    if (child != eca.childId.has_value()) {
        AddValidationError(status, "wtg.eca.child-id", path +
            (child ? ": child ECA is missing childId" : ": root/call ECA contains childId"));
    }
    for (std::size_t argument = 0; argument < eca.arguments.size(); ++argument) {
        ValidateArgument(
            eca.arguments[argument], owner, document, catalog, graph, status,
            path + "/arguments/" + std::to_string(argument), depth + 1);
    }
    for (std::size_t childIndex = 0; childIndex < eca.children.size(); ++childIndex) {
        ValidateEca(
            eca.children[childIndex], true, owner, document, catalog, graph, status,
            path + "/children/" + std::to_string(childIndex), depth + 1);
    }
    graph.ecaState[index] = 2;
}

void ValidateArgument(
    std::uint32_t index,
    StableId owner,
    const TriggerDocument& document,
    const UiCatalog& catalog,
    GraphState& graph,
    Status& status,
    std::string path,
    std::size_t depth) {
    if (depth > 256) {
        AddValidationError(status, "wtg.graph.depth", path + ": nesting exceeds 256 levels");
        return;
    }
    if (index >= document.argumentNodes.size()) {
        AddValidationError(status, "wtg.argument.bad-index", path + ": argument index is out of range");
        return;
    }
    ++graph.argumentReferences[index];
    if (graph.argumentReferences[index] > 1) {
        AddValidationError(status, "wtg.argument.shared-node", path + ": argument node has multiple owners");
        return;
    }
    if (graph.argumentState[index] == 1) {
        AddValidationError(status, "wtg.argument.cycle", path + ": argument graph contains a cycle");
        return;
    }
    graph.argumentState[index] = 1;
    const ArgumentNode& argument = document.argumentNodes[index];
    if (argument.id == 0) {
        AddValidationError(status, "wtg.argument.missing-id", path + ": stable id is zero");
    }
    if (argument.type < -1 || argument.type > 3) {
        AddValidationError(status, "wtg.argument.type", path + ": type must be in -1..3");
    }
    if (argument.insertCall != 0 && argument.insertCall != 1) {
        AddValidationError(status, "wtg.argument.insert-call", path + ": insertCall must be 0 or 1");
    }
    if ((argument.insertCall == 1) != argument.callEca.has_value()) {
        AddValidationError(status, "wtg.argument.call-presence", path +
            ": insertCall and call ECA presence disagree");
    }
    if (argument.callEca.has_value()) {
        ValidateEca(
            *argument.callEca, false, owner, document, catalog, graph, status,
            path + "/call", depth + 1);
    }
    if (argument.insertIndex != 0 && argument.insertIndex != 1) {
        AddValidationError(status, "wtg.argument.insert-index", path + ": insertIndex must be 0 or 1");
    }
    if ((argument.insertIndex == 1) != argument.indexArgument.has_value()) {
        AddValidationError(status, "wtg.argument.index-presence", path +
            ": insertIndex and index argument presence disagree");
    }
    if (argument.indexArgument.has_value()) {
        ValidateArgument(
            *argument.indexArgument, owner, document, catalog, graph, status,
            path + "/index", depth + 1);
    }
    graph.argumentState[index] = 2;
}

void ValidateGraph(
    const TriggerDocument& document,
    const UiCatalog& catalog,
    Status& status) {
    GraphState graph{
        std::vector<std::uint8_t>(document.ecaNodes.size()),
        std::vector<std::uint8_t>(document.argumentNodes.size()),
        std::vector<std::size_t>(document.ecaNodes.size()),
        std::vector<std::size_t>(document.argumentNodes.size()),
    };
    for (std::size_t triggerIndex = 0; triggerIndex < document.triggers.size(); ++triggerIndex) {
        const TriggerRecord& trigger = document.triggers[triggerIndex];
        for (std::size_t ecaIndex = 0; ecaIndex < trigger.ecas.size(); ++ecaIndex) {
            ValidateEca(
                trigger.ecas[ecaIndex], false, trigger.id, document, catalog, graph, status,
                "/triggers/" + std::to_string(triggerIndex) + "/ecas/" +
                    std::to_string(ecaIndex), 0);
        }
    }
    for (std::size_t index = 0; index < graph.ecaReferences.size(); ++index) {
        if (graph.ecaReferences[index] == 0) {
            AddValidationError(status, "wtg.eca.orphan", "/ecaNodes/" +
                std::to_string(index) + ": node is not owned by a trigger");
        }
    }
    for (std::size_t index = 0; index < graph.argumentReferences.size(); ++index) {
        if (graph.argumentReferences[index] == 0) {
            AddValidationError(status, "wtg.argument.orphan", "/argumentNodes/" +
                std::to_string(index) + ": node is not owned by an ECA");
        }
    }
}

} // namespace LegacyCodecDiagnosticPaths

void SameCodecDiagnostics(const Status& actual, const Status& expected) {
    Require(actual.Succeeded() == expected.Succeeded() && actual.diagnostics.size() == expected.diagnostics.size(),
        "Codec 全部诊断数/成功状态必须与独立旧路径算法一致。");
    for (std::size_t index = 0; index < expected.diagnostics.size(); ++index) {
        const auto& a = actual.diagnostics[index]; const auto& e = expected.diagnostics[index];
        Require(a.severity == e.severity && a.subsystem == e.subsystem && a.code == e.code && a.message == e.message
            && !a.origin && !e.origin, "Codec 诊断 severity/subsystem/code/完整路径/顺序必须与旧算法相同。");
    }
}

Status CheckCodecDiagnosticPaths(const TriggerDocument& document, const UiCatalog& catalog,
    std::span<const Diagnostic> prefix = {}) {
    Status expected; expected.diagnostics.assign(prefix.begin(), prefix.end());
    LegacyCodecDiagnosticPaths::ValidateGraph(document, catalog, expected);
    const auto actual = WtgWctCodec{}.Validate(document, catalog);
    SameCodecDiagnostics(actual, expected);
    const auto encoded = WtgWctCodec{}.Serialize(document, catalog);
    if (expected.Succeeded()) {
        Require(encoded.Succeeded() && encoded.value.has_value(), "旧路径算法通过的实际图不能误拒序列化。");
        const auto parsed = Take(WtgWctCodec{}.Parse(encoded.value->wtg, encoded.value->wct, catalog));
        Same(*encoded.value, Wire(parsed, catalog));
        LegacyGraphEquivalence(document, catalog);
    } else {
        Require(!encoded.Succeeded() && !encoded.value, "真实结构失败必须继续拒绝序列化，不能返回部分成员。");
        SameCodecDiagnostics(Status{encoded.diagnostics}, expected);
    }
    return actual;
}

struct CodecPathFixture {
    TriggerDocument document;
    std::uint32_t root = 0, rootArgument = 0, call = 0, callArgument = 0, indexArgument = 0, child = 0;
};
StableId CodecPathNextIdentity(const TriggerDocument& document) {
    StableId greatest = 0;
    for (const auto& value : document.categories) greatest = (std::max)(greatest, value.id);
    for (const auto& value : document.variables) greatest = (std::max)(greatest, value.id);
    for (const auto& value : document.triggers) greatest = (std::max)(greatest, value.id);
    for (const auto& value : document.ecaNodes) greatest = (std::max)(greatest, value.id);
    for (const auto& value : document.argumentNodes) greatest = (std::max)(greatest, value.id);
    Require(greatest <= (std::numeric_limits<StableId>::max)() - 512, "原创诊断夹具稳定 ID 预留不足。");
    return greatest + 1;
}
CodecPathFixture MakeCodecPathFixture(const UiCatalog& catalog, bool nested) {
    CodecPathFixture out; out.document = Empty(catalog);
    const auto trigger = Take(TriggerDocumentEditor::AddTrigger(out.document,
        out.document.categories.front().categoryId, "DiagnosticPaths"));
    const auto appended = Take(TriggerDocumentEditor::AppendRootEca(out.document, catalog, trigger,
        UiDefinitionKind::Action, "SetVariable"));
    const auto root = std::find_if(out.document.ecaNodes.begin(), out.document.ecaNodes.end(),
        [&](const auto& node) { return node.id == appended.rootEcaId; });
    Require(root != out.document.ecaNodes.end() && root->arguments.size() == 2, "实际 SetVariable 夹具须有两个参数。");
    out.root = static_cast<std::uint32_t>(root - out.document.ecaNodes.begin());
    out.rootArgument = root->arguments[0];
    if (!nested) return out;
    auto next = CodecPathNextIdentity(out.document);
    const auto appendArgument = [&] {
        ArgumentNode value; value.id = next++; value.type = 3; value.value = {"1", TextEncoding::Utf8, {}};
        const auto index = static_cast<std::uint32_t>(out.document.argumentNodes.size());
        out.document.argumentNodes.push_back(std::move(value)); return index;
    };
    out.indexArgument = appendArgument(); out.callArgument = appendArgument();
    const auto appendEca = [&](std::int32_t type, std::string_view name, bool child) {
        const auto* definition = catalog.Find(type, name); Require(definition != nullptr, "原创节点需要真实已加载定义。");
        EcaNode value; value.id = next++; value.sourceTriggerId = trigger; value.type = type;
        value.name = {std::string(name), TextEncoding::Utf8, {}}; value.uiSignature = definition->Signature();
        if (child) value.childId = 0;
        const auto index = static_cast<std::uint32_t>(out.document.ecaNodes.size());
        out.document.ecaNodes.push_back(std::move(value)); return index;
    };
    out.call = appendEca(3, "Identity", false);
    out.document.ecaNodes[out.call].arguments.push_back(out.callArgument);
    out.child = appendEca(2, "DoNothing", true);
    out.document.ecaNodes[out.root].children.push_back(out.child);
    auto& first = out.document.argumentNodes[out.rootArgument]; first.insertIndex = 1; first.indexArgument = out.indexArgument;
    const auto secondIndex = out.document.ecaNodes[out.root].arguments[1];
    auto& second = out.document.argumentNodes[secondIndex]; second.insertCall = 1; second.callEca = out.call;
    return out;
}

void CodecLazyDiagnosticPathsPreserveAllGraphErrorsAndWire(const UiCatalog& catalog) {
    auto fixture = MakeCodecPathFixture(catalog, true);
    Require(CheckCodecDiagnosticPaths(fixture.document, catalog).Succeeded(), "嵌套 call/index/child 实际公共 Validate 必须通过。");
    const auto original = Wire(fixture.document, catalog);
    const std::string rootPath = "/triggers/0/ecas/0";
    const auto exactError = [&](const Status& status, std::string_view code, std::string_view message) {
        Require(std::any_of(status.diagnostics.begin(), status.diagnostics.end(), [&](const auto& diagnostic) {
            return diagnostic.code == code && diagnostic.message == message;
        }), "实际拒绝必须保留指定完整旧路径与错误码。");
    };
    {
        auto broken = fixture.document; broken.ecaNodes[fixture.root].arguments[0] = (std::numeric_limits<std::uint32_t>::max)();
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        exactError(status, "wtg.argument.bad-index", rootPath + "/arguments/0: argument index is out of range");
    }
    {
        auto broken = fixture.document; broken.ecaNodes[fixture.call].arguments[0] = (std::numeric_limits<std::uint32_t>::max)();
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        exactError(status, "wtg.argument.bad-index", rootPath + "/arguments/1/call/arguments/0: argument index is out of range");
    }
    {
        auto broken = fixture.document; broken.ecaNodes[fixture.root].children.push_back((std::numeric_limits<std::uint32_t>::max)());
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        exactError(status, "wtg.eca.bad-index", rootPath + "/children/1: ECA index is out of range");
    }
    {
        auto broken = fixture.document; broken.argumentNodes[fixture.rootArgument].indexArgument = (std::numeric_limits<std::uint32_t>::max)();
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        exactError(status, "wtg.argument.bad-index", rootPath + "/arguments/0/index: argument index is out of range");
    }
    {
        auto broken = fixture.document; broken.ecaNodes[fixture.root].arguments[1] = fixture.rootArgument;
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        exactError(status, "wtg.argument.shared-node", rootPath + "/arguments/1: argument node has multiple owners");
    }
    {
        auto broken = fixture.document; broken.ecaNodes[fixture.root].children.push_back(fixture.root);
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        // 原条件先计重复引用，故真实回边依原算法先报 shared-node；不伪造不可达的 cycle 顺序。
        exactError(status, "wtg.eca.shared-node", rootPath + "/children/1: ECA node has multiple owners");
    }
    {
        auto broken = fixture.document; broken.ecaNodes[fixture.call].arguments.clear();
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        const auto* definition = catalog.Find(3, "Identity");
        Require(definition != nullptr, "Identity 必须来自实际已加载的目录。");
        exactError(status, "wtg.eca.argument-count", rootPath + "/arguments/1/call: argument count does not match " + definition->Signature());
    }
    {
        auto broken = fixture.document; broken.ecaNodes[fixture.child].childId.reset();
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        exactError(status, "wtg.eca.child-id", rootPath + "/children/0: child ECA is missing childId");
    }
    {
        auto broken = fixture.document; broken.ecaNodes[fixture.child].id = broken.ecaNodes[fixture.root].id;
        const std::array prefix{LegacyCodecDiagnosticPaths::MakeDiagnostic(DiagnosticSeverity::Error, "wtg.stable-id",
            "/ecaNodes/" + std::to_string(fixture.child) + ": stable id is zero or duplicated")};
        const auto status = CheckCodecDiagnosticPaths(broken, catalog, prefix);
        Require(status.diagnostics.size() == 1, "实际非零重复 ID 仍由原唯一性门拒绝，不改图诊断顺序。");
    }
    {
        auto combined = MakeCodecPathFixture(catalog, true);
        const auto target = combined.document.triggers.front().id;
        for (unsigned index = 0; index < 12; ++index) {
            Take(TriggerDocumentEditor::AddTrigger(combined.document,
                combined.document.categories.front().categoryId, "PathPadding" + std::to_string(index)));
        }
        std::swap(combined.document.triggers.front(), combined.document.triggers.back());
        auto next = CodecPathNextIdentity(combined.document);
        const auto* definition = catalog.Find(2, "DoNothing");
        Require(definition != nullptr && definition->ArgumentCount() == 0, "非零索引夹具须使用真实零参数动作定义。");
        const auto append = [&](bool child) {
            EcaNode value; value.id = next++; value.sourceTriggerId = target; value.type = 2;
            value.name = {"DoNothing", TextEncoding::Utf8, {}}; value.uiSignature = definition->Signature();
            if (child) value.childId = 0;
            const auto index = static_cast<std::uint32_t>(combined.document.ecaNodes.size());
            combined.document.ecaNodes.push_back(std::move(value)); return index;
        };
        auto& roots = combined.document.triggers.back().ecas; roots.clear();
        for (unsigned index = 0; index < 12; ++index) roots.push_back(append(false));
        const auto outer = append(false); roots.push_back(outer);
        for (unsigned index = 0; index < 12; ++index) {
            const auto child = append(true); combined.document.ecaNodes[outer].children.push_back(child);
        }
        combined.document.ecaNodes[combined.root].childId = 12;
        combined.document.ecaNodes[outer].children.push_back(combined.root);
        Require(CheckCodecDiagnosticPaths(combined.document, catalog).Succeeded(), "非零 trigger/root/child 索引的完整真实图须可序列化并回读。");
        auto& argument = combined.document.argumentNodes[combined.callArgument]; argument.insertIndex = 1;
        argument.indexArgument = (std::numeric_limits<std::uint32_t>::max)();
        const auto status = CheckCodecDiagnosticPaths(combined.document, catalog);
        Require(status.diagnostics.size() == 1, "组合坏索引不得多报或丢失诊断。");
        exactError(status, "wtg.argument.bad-index",
            "/triggers/12/ecas/12/children/12/arguments/1/call/arguments/0/index: argument index is out of range");
    }
    {
        auto broken = fixture.document; broken.argumentNodes[fixture.indexArgument].id = 0;
        const std::array prefix{LegacyCodecDiagnosticPaths::MakeDiagnostic(DiagnosticSeverity::Error, "wtg.stable-id",
            "/argumentNodes/" + std::to_string(fixture.indexArgument) + ": stable id is zero or duplicated")};
        const auto status = CheckCodecDiagnosticPaths(broken, catalog, prefix);
        exactError(status, "wtg.argument.missing-id", rootPath + "/arguments/0/index: stable id is zero");
    }
    {
        auto broken = fixture.document; broken.ecaNodes[fixture.root].enabled = 7;
        broken.argumentNodes[fixture.indexArgument].insertIndex = 2;
        broken.ecaNodes[fixture.call].name.text = "MissingDefinition";
        broken.argumentNodes[fixture.callArgument].insertCall = 1;
        broken.ecaNodes[fixture.child].enabled = 2;
        const auto status = CheckCodecDiagnosticPaths(broken, catalog);
        const std::array<std::string, 5> codes{"wtg.eca.enabled", "wtg.argument.insert-index", "wtg.eca.unknown-ui", "wtg.argument.call-presence", "wtg.eca.enabled"};
        const std::array<std::string, 5> messages{rootPath + ": enabled must be 0 or 1",
            rootPath + "/arguments/0/index: insertIndex must be 0 or 1",
            rootPath + "/arguments/1/call: unknown UI definition [MissingDefinition]",
            rootPath + "/arguments/1/call/arguments/0: insertCall and call ECA presence disagree",
            rootPath + "/children/0: enabled must be 0 or 1"};
        Require(status.diagnostics.size() == codes.size(), "多错误夹具须完整保留五条诊断。");
        for (std::size_t index = 0; index < codes.size(); ++index)
            Require(status.diagnostics[index].code == codes[index] && status.diagnostics[index].message == messages[index],
                "父→index→call→child 的实际错误顺序与全串不变。");
    }
    for (const auto maximumDepth : {256U, 257U}) {
        auto deep = MakeCodecPathFixture(catalog, false); auto next = CodecPathNextIdentity(deep.document);
        auto previous = deep.rootArgument;
        for (unsigned depth = 2; depth <= maximumDepth; ++depth) {
            ArgumentNode value; value.id = next++; value.type = 3; value.value = {"1", TextEncoding::Utf8, {}};
            const auto index = static_cast<std::uint32_t>(deep.document.argumentNodes.size());
            deep.document.argumentNodes.push_back(std::move(value));
            auto& parent = deep.document.argumentNodes[previous]; parent.insertIndex = 1; parent.indexArgument = index; previous = index;
        }
        const auto status = CheckCodecDiagnosticPaths(deep.document, catalog);
        if (maximumDepth == 256) Require(status.Succeeded(), "深度恰256的同wire合法图不能误拒。");
        else {
            std::string path = rootPath + "/arguments/0";
            for (unsigned edge = 1; edge < maximumDepth; ++edge) path += "/index";
            Require(status.diagnostics.size() == 2 && status.diagnostics[0].code == "wtg.graph.depth"
                && status.diagnostics[0].message == path + ": nesting exceeds 256 levels"
                && status.diagnostics[1].code == "wtg.argument.orphan", "257层仍先完整depth地址、后旧orphan拒绝，不改变上限/顺序。");
        }
    }
    Same(original, Wire(fixture.document, catalog));
}
