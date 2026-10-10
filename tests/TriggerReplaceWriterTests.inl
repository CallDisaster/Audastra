// 原 buffered framing 独立 oracle：不调用新 Writer/Text/Number，不借新流实现造预期。
class LegacyBufferedReplaceWriter {
public:
    explicit LegacyBufferedReplaceWriter(std::size_t limit) : limit_(limit) {}
    bool Text(std::string_view value) {
        const auto length = std::to_string(value.size());
        if (!Charge(length.size() + 1) || !Charge(value.size())) { valid_ = false; return false; }
        bytes_ += length; bytes_ += ':'; bytes_ += value; return true;
    }
    template<class T> bool Number(T value) {
        if constexpr (std::is_signed_v<T>) return Text(std::to_string(static_cast<std::int64_t>(value)));
        else return Text(std::to_string(static_cast<std::uint64_t>(value)));
    }
    std::size_t Used() const { return used_; }
    Result<std::string> Hash() const {
        return valid_ ? Sha256Text(bytes_) : Result<std::string>::Failure(
            "Trigger.Replace", "TRIGGER_REPLACE_TEXT_BUDGET", "来源摘要超过有界文本预算。");
    }
private:
    bool Charge(std::size_t count) {
        if (used_ > limit_ || count > limit_ - used_) return false;
        used_ += count; return true;
    }
    std::size_t limit_, used_ = 0; std::string bytes_; bool valid_ = true;
};
bool CheckedWriterTestAdd(std::size_t& used, std::size_t count, std::size_t cap) {
    if (used > cap || count > cap - used) return false;
    used += count; return true;
}
using StreamReplaceWriter = War3Map::Services::detail::TriggerReplaceWriter<CheckedWriterTestAdd>;
void TestStreamWriterOriginalFramingAndBoundaries() {
    const auto fixed = [](auto& writer) {
        return writer.Text({}) && writer.Text(std::string_view("a\0b", 3))
            && writer.Number((std::numeric_limits<std::uint64_t>::max)())
            && writer.Number((std::numeric_limits<std::int64_t>::min)())
            && writer.Number(0) && writer.Number(true) && writer.Text("中");
    };
    // 手写原完整字节帧，含 NUL，不从任一 Writer 的输出倒推。
    constexpr char literal[] = "0:3:a\0b20:1844674407370955161520:-92233720368547758081:01:13:中";
    const auto literalHash = Sha256Text(std::string_view(literal, sizeof(literal) - 1));
    Require(literalHash.Succeeded() && *literalHash.value == "ec0e89e7ae7a43111c3db148b3b2b144a39125f7243ec776186570c520adffe6", "independent literal SHA golden");
    for (const bool below : {false, true}) {
        const auto cap = sizeof(literal) - 1 - static_cast<std::size_t>(below);
        LegacyBufferedReplaceWriter old(cap); StreamReplaceWriter stream(cap);
        Require(fixed(old) == fixed(stream) && old.Used() == stream.Used(), "original partial-charge budget semantics");
        const auto expected = old.Hash(), actual = stream.Hash();
        Require(expected.Succeeded() == actual.Succeeded() && expected.value == actual.value, "exact cap and cap-minus-one match original buffered oracle");
        if (below) Require(!actual.Succeeded() && !actual.value && actual.diagnostics.front().code == "TRIGGER_REPLACE_TEXT_BUDGET", "typed budget failure no prefix SHA");
        else {
            Require(*actual.value == *literalHash.value && stream.Hash().value == actual.value, "same literal bytes and repeated terminal Hash cached");
            const auto beforeUsed = stream.Used();
            Require(!stream.Text("late") && !stream.Number(7) && stream.Used() == beforeUsed, "terminal writer cannot append");
            const auto late = stream.Hash();
            Require(!late.Succeeded() && !late.value && late.diagnostics.front().code == "TRIGGER_REPLACE_WRITER_CLOSED", "closed write must not return old cached SHA");
        }
    }
    for (const auto count : {std::size_t{0}, std::size_t{1}, std::size_t{8190}, std::size_t{8191}, std::size_t{8192},
            std::size_t{8193}, std::size_t{16383}, std::size_t{16384}, std::size_t{65535}, std::size_t{65536}, std::size_t{65537}}) {
        std::string input(count, 'x'); if (count > 4) { input[1] = '\0'; input[3] = static_cast<char>(0xff); }
        LegacyBufferedReplaceWriter old(128 * 1024); StreamReplaceWriter stream(128 * 1024);
        Require(old.Text("prefix") && stream.Text("prefix") && old.Text(input) && stream.Text(input)
            && old.Number(-42) && stream.Number(-42) && old.Text("尾") && stream.Text("尾"), "all original binary/raw fields admitted");
        const auto expected = old.Hash(), actual = stream.Hash();
        Require(expected.Succeeded() && actual.Succeeded() && expected.value == actual.value && old.Used() == stream.Used(), "8Ki/64Ki boundary order and tail flush match original buffered bytes");
    }
}
void TestStreamWriterDoesNotOwnInputPayload() {
    const std::string input(2 * 1024 * 1024, 'x');
    {
        TriggerReplaceAllocationWatch::Window watch(1024 * 1024);
        LegacyBufferedReplaceWriter old(input.size() + 64); Require(old.Text(input) && old.Hash().Succeeded(), "original buffered control succeeds");
        Require(TriggerReplaceAllocationWatch::largeRequests != 0, "original buffered input/control must actually allocate payload");
    }
    {
        TriggerReplaceAllocationWatch::Window watch(1024 * 1024);
        StreamReplaceWriter stream(input.size() + 64); Require(stream.Text(input) && stream.Hash().Succeeded(), "real stream path succeeds");
        Require(TriggerReplaceAllocationWatch::largeRequests == 0, "stream writer has no ordinary-new whole input copy; not total RSS proof");
    }
}
void OracleReplaceLimits(LegacyBufferedReplaceWriter& w, const TriggerReplaceLimits& l) {
    Require(w.Number(l.pageLimit) && w.Number(l.maxGraphRecords) && w.Number(l.maxGraphEdges)
        && w.Number(l.maxScanTextBytes) && w.Number(l.maxResultBytes) && w.Number(l.maxSelection)
        && w.Number(l.maxTargetRecords) && w.Number(l.maxTargetTextBytes) && w.Number(l.maxNameRecords)
        && w.Number(l.maxNameTextBytes) && w.Number(l.maxPreviewDocumentBytes) && w.Number(l.maxPreviewWorkUnits)
        && w.Number(static_cast<unsigned>(l.scalarWorkPolicy)), "original request limits framing");
}
void TestPublicQueryAndOptionsKeepOriginalSnapshotProofs() {
    Fixture f;
    for (std::size_t i = 0; i < 128; ++i) f.Add("StoreInt", 3, std::to_string(i));
    f.doc.variables.back().name.text = "中文变量";
    f.doc.argumentNodes.front().value.originalBytes = Bytes(f.doc.argumentNodes.front().value.text);
    const auto context = f.Context(); const auto before = f.Encoded();
    TriggerReplaceQuery q; q.mode = TriggerReplaceMode::Function; q.match = TriggerReplaceMatch::Exact; q.needle = "StoreInt";
    TriggerReplaceLimits limits;
    LegacyBufferedReplaceWriter snapshot(limits.maxScanTextBytes);
    snapshot.Text("trigger-replace-snapshot-v1"); snapshot.Text(context.revision); snapshot.Text(f.catalog.Sha256());
    const auto raw = [&](const RawText& value) {
        Require(snapshot.Text(value.text) && snapshot.Number(static_cast<unsigned>(value.encoding))
            && snapshot.Text(value.originalBytes.empty() ? std::string_view{} : std::string_view(reinterpret_cast<const char*>(value.originalBytes.data()), value.originalBytes.size())), "old snapshot raw byte layout");
    };
    for (const auto& category : f.doc.categories) { snapshot.Number(category.id); raw(category.name); }
    for (const auto& variable : f.doc.variables) {
        snapshot.Number(variable.id); raw(variable.name); raw(variable.typeName); raw(variable.value);
        snapshot.Number(variable.isArray); snapshot.Number(variable.size); snapshot.Number(variable.hasDefault);
    }
    for (const auto& trigger : f.doc.triggers) {
        snapshot.Number(trigger.id); raw(trigger.name); snapshot.Number(trigger.type); snapshot.Number(trigger.customTextFlag); snapshot.Number(trigger.enabled);
        for (auto index : trigger.ecas) snapshot.Number(index);
    }
    for (const auto& node : f.doc.ecaNodes) {
        snapshot.Number(node.id); raw(node.name); snapshot.Text(node.uiSignature); snapshot.Text(node.uiLayer);
        snapshot.Number(node.sourceTriggerId); snapshot.Number(node.type); snapshot.Number(node.enabled);
        snapshot.Number(node.childId.has_value()); if (node.childId) snapshot.Number(*node.childId);
        for (auto index : node.arguments) snapshot.Number(index);
        for (auto index : node.children) snapshot.Number(index);
    }
    for (const auto& node : f.doc.argumentNodes) {
        snapshot.Number(node.id); raw(node.value); snapshot.Number(node.type); snapshot.Number(node.insertCall); snapshot.Number(node.insertIndex);
        snapshot.Number(node.callEca.has_value()); snapshot.Number(node.indexArgument.has_value());
        if (node.callEca) snapshot.Number(*node.callEca); if (node.indexArgument) snapshot.Number(*node.indexArgument);
    }
    Require(f.strings.entries.empty(), "fixed public oracle does not hide WTS fields");
    snapshot.Number(false); snapshot.Number(false); snapshot.Number(0); snapshot.Number(128);
    const auto expectedSnapshot = snapshot.Hash(); Require(expectedSnapshot.Succeeded() && snapshot.Used() > 8192, "real public snapshot crosses local stream block");
    const auto page = QueryTriggerReplace(context, context.revision, q, limits);
    Require(page.Succeeded() && page.value->coverage.state == TriggerReplaceState::Ready
        && page.value->coverage.totalMatches == std::optional<std::size_t>{128}
        && page.value->proof.snapshotSha256 == *expectedSnapshot.value && page.value->coverage.textBytesCharged == snapshot.Used(), "public Query uses exact old full-domain snapshot and charged bytes");
    LegacyBufferedReplaceWriter request(16384); request.Text("trigger-replace-query-v1"); OracleReplaceLimits(request, limits);
    request.Number(1); request.Number(0); request.Text("StoreInt"); request.Number(true); request.Number(false); request.Number(false); request.Number(false);
    const auto expectedRequest = request.Hash();
    Require(expectedRequest.Succeeded() && *expectedRequest.value == "7b19355476fcea4d3828742210d6a14608d8aba29263f9768dd6a9b62f8e2fce"
        && page.value->proof.requestSha256 == *expectedRequest.value, "external fixed request golden, original decimal framing and policy");
    auto exact = limits; exact.maxScanTextBytes = snapshot.Used();
    const auto exactPage = QueryTriggerReplace(context, context.revision, q, exact);
    Require(exactPage.Succeeded() && exactPage.value->proof.snapshotSha256 == *expectedSnapshot.value, "public Query exact old source byte cap");
    --exact.maxScanTextBytes; const auto below = QueryTriggerReplace(context, context.revision, q, exact);
    Require(!below.Succeeded() && !below.value && below.diagnostics.front().code == "TRIGGER_REPLACE_TEXT_BUDGET", "public Query cap-minus-one refuses without partial proof");

    LegacyBufferedReplaceWriter target(limits.maxTargetTextBytes);
    for (const auto& variable : f.doc.variables) { target.Text(variable.name.text); target.Text(variable.typeName.text); target.Number(variable.id); target.Number(variable.isArray); }
    for (const auto& trigger : f.doc.triggers) { target.Text(trigger.name.text); target.Number(trigger.id); target.Number(trigger.enabled); target.Number(trigger.type); }
    target.Number(false); const auto expectedTarget = target.Hash();
    const auto options = QueryTriggerReplacementOptions(context, context.revision, TriggerReplacementKind::Variable, {}, limits);
    Require(expectedTarget.Succeeded() && options.Succeeded() && options.value->coverage.state == TriggerReplaceState::Ready
        && options.value->coverage.totalMatches == std::optional<std::size_t>{3}
        && options.value->proof.snapshotSha256 == *expectedTarget.value && options.value->coverage.textBytesCharged == target.Used(), "public options retain original buffered snapshot");
    LegacyBufferedReplaceWriter targetRequest(16384); targetRequest.Text("trigger-replace-options-v1"); OracleReplaceLimits(targetRequest, limits); targetRequest.Number(0); targetRequest.Text({});
    const auto expectedTargetRequest = targetRequest.Hash();
    Require(expectedTargetRequest.Succeeded() && *expectedTargetRequest.value == "a019cb24d0e63dc5b926f772d2d2d9eeb647a7d7fd9bd6af878baa5e60554875"
        && options.value->proof.requestSha256 == *expectedTargetRequest.value, "public options request fixed golden");
    const auto after = f.Encoded(); Require(after.wtg == before.wtg && after.wct == before.wct, "read-only hash/source oracle preserves actual source WTG/WCT");
}

void TestLazyWriterTinyAllocationAndOriginalFlushBoundary() {
    constexpr std::size_t largeAllocation = 64 * 1024;
    const auto emptyHash = Sha256Text({}); Require(emptyHash.Succeeded(), "original empty SHA control succeeds");
    {
        TriggerReplaceAllocationWatch::Window watch(largeAllocation);
        StreamReplaceWriter writer(0); const auto actual = writer.Hash();
        Require(actual.Succeeded() && actual.value == emptyHash.value && writer.Used() == 0
            && writer.Hash().value == actual.value && TriggerReplaceAllocationWatch::largeRequests == 0,
            "empty zero-budget terminal writer retains original SHA without a 64Ki stream-state allocation");
        Require(!writer.Text({}) && writer.Used() == 0, "empty zero-budget writer still refuses writes after terminal Hash");
        const auto closed = writer.Hash(); Require(!closed.Succeeded() && !closed.value
            && closed.diagnostics.front().code == "TRIGGER_REPLACE_WRITER_CLOSED", "empty terminal writer never returns an old SHA after writing");
    }
    {
        LegacyBufferedReplaceWriter old(0); StreamReplaceWriter writer(0);
        Require(!old.Text({}) && !writer.Text({}) && old.Used() == writer.Used(), "empty field still requires its original two framing bytes");
        const auto expected = old.Hash(), actual = writer.Hash();
        Require(!expected.Succeeded() && !actual.Succeeded() && expected.value == actual.value
            && actual.diagnostics.front().code == "TRIGGER_REPLACE_TEXT_BUDGET", "empty zero-budget field has no prefix SHA");
    }
    constexpr char literal[] = "0:3:a\0b20:1844674407370955161520:-92233720368547758081:01:13:中";
    const auto fixed = [](auto& writer) {
        return writer.Text({}) && writer.Text(std::string_view("a\0b", 3))
            && writer.Number((std::numeric_limits<std::uint64_t>::max)())
            && writer.Number((std::numeric_limits<std::int64_t>::min)())
            && writer.Number(0) && writer.Number(true) && writer.Text("中");
    };
    const auto literalHash = Sha256Text(std::string_view(literal, sizeof(literal) - 1));
    Require(literalHash.Succeeded() && *literalHash.value == "ec0e89e7ae7a43111c3db148b3b2b144a39125f7243ec776186570c520adffe6", "independent tiny literal golden remains exact");
    {
        TriggerReplaceAllocationWatch::Window watch(largeAllocation);
        StreamReplaceWriter writer(sizeof(literal) - 1);
        Require(fixed(writer), "tiny literal admits its exact original framing budget"); const auto actual = writer.Hash();
        Require(actual.Succeeded() && actual.value == literalHash.value && writer.Used() == sizeof(literal) - 1
            && TriggerReplaceAllocationWatch::largeRequests == 0, "tiny binary and decimal framing does not allocate the 64Ki stream state");
    }
    for (const auto payloadBytes : {std::size_t{8186}, std::size_t{8187}, std::size_t{8188}}) {
        std::string input(payloadBytes, 'x'); input[1] = '\0'; input[3] = static_cast<char>(0xff);
        const auto framedBytes = payloadBytes + 5; // 四位长度与冒号，分别得到 8KiB−1/8KiB/8KiB+1。
        LegacyBufferedReplaceWriter old(framedBytes); Require(old.Text(input), "original exact 8Ki framing control admits");
        const auto expected = old.Hash(); Require(expected.Succeeded() && old.Used() == framedBytes, "original exact 8Ki framing control hashes");
        {
            TriggerReplaceAllocationWatch::Window watch(largeAllocation);
            StreamReplaceWriter writer(framedBytes); Require(writer.Text(input), "lazy writer retains original exact 8Ki framing budget");
            const auto actual = writer.Hash();
            Require(actual.Succeeded() && actual.value == expected.value && writer.Used() == old.Used(), "8Ki minus/exact/plus-one SHA and charge match independent buffered oracle");
            Require(framedBytes < StreamReplaceWriter::kBufferBytes ? TriggerReplaceAllocationWatch::largeRequests == 0
                : TriggerReplaceAllocationWatch::largeRequests != 0, "original exact 8Ki flush starts the shared stream; sub-block frames stay small");
            Require(writer.Hash().value == actual.value, "both lazy and streamed terminal hashes remain cached");
            Require(!writer.Number(1), "both lazy and streamed terminal writers reject new framing");
            const auto closed = writer.Hash(); Require(!closed.Succeeded() && !closed.value
                && closed.diagnostics.front().code == "TRIGGER_REPLACE_WRITER_CLOSED", "both paths refuse an old SHA after terminal append");
        }
        LegacyBufferedReplaceWriter shortOld(framedBytes - 1); StreamReplaceWriter shortWriter(framedBytes - 1);
        Require(!shortOld.Text(input) && !shortWriter.Text(input) && shortOld.Used() == shortWriter.Used(), "8Ki boundary budget minus one keeps original partial charge");
        const auto expectedShort = shortOld.Hash(), actualShort = shortWriter.Hash();
        Require(!expectedShort.Succeeded() && !actualShort.Succeeded() && expectedShort.value == actualShort.value
            && actualShort.diagnostics.front().code == "TRIGGER_REPLACE_TEXT_BUDGET", "8Ki boundary refusal never hashes a prefix");
    }
}
