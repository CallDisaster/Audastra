void FormattedDataPlane() {
    Reset(); g_raiseTable = false; g_tableObject = reinterpret_cast<Address>(g_tableWords.data());
    GameUiLibrary ui(Entries());
    GameUiDataPlane data({&ResolveTable, [] { return true; }});
    data.Attach(ui);
    const auto text = ui.Create("Text", 0, 0, 0).handle;
    const auto object = g_frames.back()->Self();
    const auto background = ui.Create("Backdrop", 0, 0, 0).handle;
    Check(data.RegisterTextFormat(text, "{0}") == AJ_UNSUPPORTED, "format requires a bound private table");
    Check(data.Bind(1234, 1, 1) == AJ_OK, "format test binds private table");
    Check(data.RegisterTextFormat(background, "{0}") == AJ_UNSUPPORTED, "non-text frame rejected before drawing");
    Check(data.RegisterTextFormat(text, "血 {0:2}/{1}") == AJ_OK, "valid format registers once");
    const float real = 123.125f;
    g_log.clear();
    Check(data.SaveReal(1234, text, 16, &real).applied && g_log.empty(), "real argument only updates bounded cache");
    Check(!data.SaveBoolean(1234, text, 6, 1).applied && g_log.empty(), "missing integer never draws a partial result");
    Check(data.SaveInteger(1234, text, 25, INT32_MAX).applied && g_log.empty(), "integer argument keeps all int32 bits");
    Check(data.SaveBoolean(1234, text, 6, 1).applied
        && Logged("ttext " + Hex(object) + " 血 123.13/2147483647"), "commit draws one complete half-away result");
    Check(data.RegisterTextFormat(text, "{8}") == AJ_ARGUMENT, "invalid replacement rejected");
    g_log.clear();
    Check(data.SaveBoolean(1234, text, 6, 1).applied
        && Logged("ttext " + Hex(object) + " 血 123.13/2147483647"), "bad registration preserves previous format and values");
    VmText overrideText("override");
    Check(data.SaveStr(1234, text, 3, overrideText.Self()).applied, "ordinary text writes remain available");
    g_log.clear();
    Check(data.SaveBoolean(1234, text, 6, 1).applied
        && Logged("ttext " + Hex(object) + " 血 123.13/2147483647"), "explicit commit respects last-call ordering after a plain text write");
    Check(data.RegisterTextFormat(text, "{0}") == AJ_OK, "replacement format clears old argument validity");
    Check(!data.SaveBoolean(1234, text, 6, 1).applied, "replacement cannot inherit argument values");
    Check(data.SaveInteger(1234, text, 24, INT32_MIN).applied && data.SaveBoolean(1234, text, 6, 1).applied,
        "full negative integer commits without float conversion");
    Check(Logged("ttext " + Hex(object) + " -2147483648"), "minimum int32 rendered exactly");
    const float invalid = std::numeric_limits<float>::infinity();
    Check(!data.SaveReal(1234, text, 16, &invalid).applied, "nonfinite writes rejected");
    Check(!data.SaveInteger(1234, text, 23, 1).applied && !data.SaveBoolean(1234, text, 6, 0).applied,
        "wrong type slot and false commit rejected");
    const auto parent = ui.Create("Backdrop", 0, 0, 0).handle;
    const auto child = ui.Create("Text", parent, 0, 0).handle;
    Check(data.RegisterTextFormat(child, "constant") == AJ_OK && data.SaveBoolean(1234, child, 6, 1).applied,
        "constant format needs no arguments");
    Check(ui.Destroy(parent) == UiError::None && !data.SaveBoolean(1234, child, 6, 1).applied,
        "destroyed subtree cannot render cached child format");
    data.Stop();
    Check(data.RegisterTextFormat(text, "new") == AJ_STALE && !data.SaveInteger(1234, text, 24, 1).intercepted,
        "session stop releases format state and input ownership");
}

void EventTableDataPlane() {
    Reset(); g_raiseTable = false; g_tableObject = reinterpret_cast<Address>(g_tableWords.data());
    GameUiLibrary ui(Entries());
    const auto text = ui.Create("Text", 0, 0, 0).handle;
    std::map<int, int> output;
    std::vector<int> writeOrder;
    GameUiDataContext context{&ResolveTable, [] { return true; }};
    context.storeInteger = [&](int table, int parent, int child, int value) {
        Check(table == 1234 && parent == 0, "event output stays in bound reserved parent");
        output[child] = value; writeOrder.push_back(child);
    };
    std::vector<UiFrameEvent> pending{{text, kUiEventEnter}, {text, kUiEventClick}};
    unsigned refreshes = 0;
    GameUiDataPlane data(context);
    data.Attach(ui, [&](bool refresh, UiFrameEvent& event, bool& found) {
        if (refresh) ++refreshes;
        found = !pending.empty();
        if (found) { event = pending.front(); pending.erase(pending.begin()); }
        return UiError::None;
    });
    Check(data.Bind(1234, 1, 1) == AJ_OK && data.EnableEventTable() == AJ_OK, "event mailbox capability explicitly enables");
    Check(data.SaveBoolean(1234, 0, 7, 1).applied && output[0] == 1 && output[1] == text && output[2] == kUiEventEnter,
        "refresh publishes first event using integers");
    Check(writeOrder == std::vector<int>({0,1,2,0}), "completion flag is cleared first and published last");
    output.clear(); writeOrder.clear(); // 与客户端FlushChildHashtable(table,0)对应。
    Check(data.SaveBoolean(1234, 0, 8, 1).applied && output[0] == 1 && output[2] == kUiEventClick && refreshes == 1,
        "next event preserves FIFO and does not repoll pointer");
    output.clear();
    Check(data.SaveBoolean(1234, 0, 8, 1).applied && output[0] == 0 && output.size() == 1,
        "empty queue has only cleared completion flag, no string response");
    Check(!data.SaveBoolean(1234, text, 7, 1).applied, "event control requires reserved parent zero");
    Check(ui.Destroy(text) == UiError::None, "retire event frame");
    pending.push_back({text, kUiEventClick}); output.clear();
    Check(!data.SaveBoolean(1234, 0, 7, 1).applied && output[0] == 0,
        "stale frame cannot be published even by a faulty event source");
    GameUiDataPlane unsupported({&ResolveTable, [] { return true; }});
    unsupported.Attach(ui);
    Check(unsupported.Bind(1234, 1, 1) == AJ_OK && unsupported.EnableEventTable() == AJ_UNSUPPORTED,
        "missing original integer writer explicitly refuses capability");
    data.Stop(); output.clear();
    Check(!data.SaveBoolean(1234, 0, 7, 1).intercepted && output.empty(), "stopped mailbox cannot replay events");
}

void EventTableModule() {
    Reset(); g_raiseTable = false; g_tableObject = reinterpret_cast<Address>(g_tableWords.data());
    std::map<int, int> output;
    GameUiDataContext context{&ResolveTable, [] { return true; }};
    context.storeInteger = [&](int, int parent, int child, int value) {
        Check(parent == 0, "module mailbox writes reserved parent only"); output[child] = value;
    };
    auto data = std::make_shared<GameUiDataPlane>(context);
    ModuleHost m;
    Check(InstallGameUiModule(m.host, Entries(), {}, nullptr, {}, data) == AJ_OK
        && m.Call("audastraui.bindtable;i:1234").status == AJ_OK && m.Call("audastraui.eventtable").status == AJ_OK,
        "module explicitly exposes supported event mailbox");
    const auto button = m.Call("audastraui.create;s:Button;d:0;i:0;i:0").value.integer;
    const auto object = g_frames.back()->Self();
    Check(m.Call("audastraui.watch;d:" + std::to_string(button) + ";i:3").status == AJ_OK, "watch real module pointer source");
    g_tables.hovered = object;
    Check(data->SaveBoolean(1234, 0, 7, 1).applied && output[0] == 1 && output[1] == button && output[2] == 1,
        "native event channel consumes the same enter event as string API");
    g_tables.hovered = 0;
    Check(m.Call("audastraui.poll").value.integer == 1, "legacy path can produce a pending leave");
    output.clear();
    Check(data->SaveBoolean(1234, 0, 8, 1).applied && output[0] == 1 && output[2] == 2
        && m.Call("audastraui.event").value.text.empty(), "integer and legacy transports share exactly one queue");
    g_tables.hovered = object;
    Check(m.Call("audastraui.poll").value.integer == 1 && m.Call("audastraui.destroy;d:" + std::to_string(button)).status == AJ_OK,
        "destroy while an event is queued");
    output.clear();
    Check(data->SaveBoolean(1234, 0, 8, 1).applied && output[0] == 0, "queued retired token discarded before publication");
    const auto request = m.request;
    for (int i = 0; i < 100; ++i) {
        output.clear();
        Check(data->SaveBoolean(1234, 0, 7, 1).applied && output.size() == 1 && output[0] == 0,
            "idle event polling keeps a fixed-size integer result");
    }
    Check(m.request == request && data->Errors() == "0|", "event polling makes no string-protocol transactions");
    Check(m.host.Reset(2).applied && !data->Owns(1234), "epoch reset detaches callbacks and event outputs");
}

void EventTableFaultIsolation() {
    Reset(); g_raiseTable = false; g_tableObject = reinterpret_cast<Address>(g_tableWords.data());
    GameUiLibrary ui(Entries());
    const auto frame = ui.Create("Text", 0, 0, 0).handle;
    unsigned writes = 0;
    GameUiDataContext context{&ResolveTable, [] { return true; }};
    context.storeInteger = [&](int, int, int, int) { ++writes; RaiseException(0xE050DA71, 0, 0, nullptr); };
    GameUiDataPlane data(context, &ObserveFault);
    data.Attach(ui, [](bool, UiFrameEvent&, bool& found) { found = false; return UiError::None; });
    Check(data.Bind(1234, 1, 1) == AJ_OK && data.EnableEventTable() == AJ_OK, "fault test enables event mailbox");
    g_faultCount = 0;
    Check(!data.SaveBoolean(1234, 0, 7, 1).applied && g_faultCount == 1 && g_fault.code == 0xE050DA71
        && std::string_view(g_fault.entry) == "audastraui.eventwrite", "integer output SEH retains actual code and entry");
    Check(!data.SaveBoolean(1234, 0, 7, 1).applied && writes == 1 && !ui.Faulted()
        && ui.SetWidth(frame, 0.2f) == UiError::None, "failed writer is not retried and drawing stays available");
    GameUiDataContext stoppedContext{&ResolveTable, [] { return true; }};
    stoppedContext.storeInteger = [](int, int, int, int) {};
    GameUiDataPlane stopped(stoppedContext);
    stopped.Attach(ui, [&](bool, UiFrameEvent&, bool& found) { stopped.Stop(); found = false; return UiError::None; });
    Check(stopped.Bind(1234, 1, 1) == AJ_OK && stopped.EnableEventTable() == AJ_OK
        && !stopped.SaveBoolean(1234, 0, 7, 1).applied && !stopped.Owns(1234),
        "synchronous stop keeps current callback alive until it returns without publishing");
}

void FormattedDataCapacity() {
    Reset(); g_raiseTable = false; g_tableObject = reinterpret_cast<Address>(g_tableWords.data());
    GameUiLibrary ui(Entries()); GameUiDataPlane data({&ResolveTable, [] { return true; }}); data.Attach(ui);
    Check(data.Bind(1234, 1, 1) == AJ_OK, "capacity fixture binds");
    FrameHandle first = 0;
    for (int i = 0; i < 1024; ++i) {
        const auto frame = ui.Create("Text", 0, 0, 0).handle;
        if (!first) first = frame;
        Check(frame && data.RegisterTextFormat(frame, "constant") == AJ_OK, "documented live format capacity is usable");
    }
    const auto extra = ui.Create("Text", 0, 0, 0).handle;
    Check(data.RegisterTextFormat(extra, "constant") == AJ_LIMIT, "live format capacity is bounded");
    Check(ui.Destroy(first) == UiError::None && data.RegisterTextFormat(extra, "constant") == AJ_OK,
        "retired formats do not accumulate across UI rebuilds");
}

void TextObservationIsolation() {
    Reset(); g_raiseTable = false; g_tableObject = reinterpret_cast<Address>(g_tableWords.data());
    GameUiLibrary ui(Entries());
    unsigned arguments = 0, commits = 0;
    bool observationsValid = true;
    GameUiDataContext context{&ResolveTable, [] { return true; }};
    context.observeTextArgument = [&](std::uint32_t ordinal, FrameHandle, int, bool integer, std::uint32_t) {
        if (!integer || ordinal != ++arguments) observationsValid = false;
        throw std::runtime_error("observer failure");
    };
    context.observeTextCommit = [&](std::uint32_t ordinal, FrameHandle, std::string_view) {
        if (ordinal != ++commits) observationsValid = false;
        throw std::runtime_error("observer failure");
    };
    GameUiDataPlane data(context); data.Attach(ui);
    const auto frame = ui.Create("Text", 0, 0, 0).handle;
    Check(data.Bind(1234, 1, 1) == AJ_OK && data.RegisterTextFormat(frame, "{0}") == AJ_OK, "observation fixture registers");
    for (int i = 0; i < 600; ++i) {
        Check(data.SaveInteger(1234, frame, 24, i).applied && data.SaveBoolean(1234, frame, 6, 1).applied,
            "diagnostic observer exceptions cannot change writes or rendering");
    }
    Check(observationsValid && arguments == 512 && commits == 512 && data.Errors() == "0|", "diagnostics stop at a fixed per-session budget");
}
