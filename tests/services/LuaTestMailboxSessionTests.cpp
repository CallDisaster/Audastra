#include "War3MapCore/Services/LuaTestMailboxSession.h"
#include "../../src/Core/Services/LuaTestMailboxSessionInternal.h"
#include "../../src/WarcraftRuntime32/OwnLua/LuaMailboxRuntime.h"

#include <windows.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace War3Map::Services;
using namespace Audastra::Runtime32;
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

struct Fixture {
    std::shared_ptr<LuaTestMailboxSession> session;
    std::unique_ptr<detail::LuaTestMailboxLaunchHandle> inherited;
    LuaTestMailbox* mailbox = nullptr;
    std::unique_ptr<OwnLua::LuaMailboxRuntime> runtime;
    std::atomic<detail::LuaTestProcessOwnership> ownership{detail::LuaTestProcessOwnership::OwnedRunning};
    explicit Fixture(bool enableReload = true) {
        auto created = CreateLuaTestMailboxSession(std::string(64, 'a'));
        Check(created.Succeeded(), "create"); session = *created.value;
        auto prepared = detail::LuaTestMailboxLaunchAccess::Prepare(session, [this](std::uint32_t id) {
            return id == GetCurrentProcessId() ? ownership.load() : detail::LuaTestProcessOwnership::Unconfirmed;
        }, enableReload);
        Check(prepared.Succeeded(), "prepare"); inherited = std::move(*prepared.value);
        mailbox = static_cast<LuaTestMailbox*>(MapViewOfFile(reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(inherited->Value())),
            FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(LuaTestMailbox)));
        Check(mailbox != nullptr, "fixture view");
        runtime = std::make_unique<OwnLua::LuaMailboxRuntime>(*mailbox);
        Check(runtime->Attach(inherited->Nonce(), session->MapSha256()), "attach original fixture provider");
    }
    ~Fixture() { runtime.reset(); if (mailbox) UnmapViewOfFile(mailbox); }
};

void Lifecycle() {
    Check(!CreateLuaTestMailboxSession("invalid").Succeeded(), "invalid map identity");
    auto unbound = CreateLuaTestMailboxSession(std::string(64, 'b'));
    Check(unbound.Succeeded(), "unbound creation");
    Check(QueryLuaTestReload(**unbound.value).value->state == LuaTestReloadState::Starting, "not ready yet");
    Check(!SubmitLuaTestReload(**unbound.value, {"@change.lua", "return 1"}).Succeeded(), "unbound rejected");
    Fixture fixture;
    Check(!detail::LuaTestMailboxLaunchAccess::Prepare(fixture.session, [](std::uint32_t) {
        return detail::LuaTestProcessOwnership::OwnedRunning;
    }).Succeeded(), "cannot bind launch twice");
    Check(QueryLuaTestReload(*fixture.session).value->state == LuaTestReloadState::Idle, "ready idle");
    for (std::uint32_t expected = 1; expected <= 20; ++expected) {
        auto sent = SubmitLuaTestReload(*fixture.session, {"@trigger/test.lua", "counter = (counter or 0) + 1"});
        Check(sent.Succeeded() && *sent.value == expected, "monotonic request");
        Check(!SubmitLuaTestReload(*fixture.session, {"@other.lua", "return 2"}).Succeeded(), "cannot overwrite queued");
        Check(QueryLuaTestReload(*fixture.session, expected).value->state == LuaTestReloadState::Queued, "queued");
        auto chunk = fixture.runtime->Take();
        Check(chunk && chunk->sequence == expected && chunk->chunkName == "@trigger/test.lua", "receive frozen request");
        Check(!CancelLuaTestReload(*fixture.session, expected).Succeeded(), "running cannot cancel");
        Check(QueryLuaTestReload(*fixture.session, expected).value->state == LuaTestReloadState::Running, "running");
        Check(fixture.runtime->Complete(expected, LuaReloadResult::Success, "原创模拟接收完成"), "ack");
        auto snapshot = QueryLuaTestReload(*fixture.session, expected);
        Check(snapshot.Succeeded() && snapshot.value->state == LuaTestReloadState::Complete
            && snapshot.value->resultCode == LuaReloadResult::Success
            && snapshot.value->message == "原创模拟接收完成" && snapshot.value->processActive, "exact ack");
        if (expected > 1) Check(!QueryLuaTestReload(*fixture.session, expected - 1).Succeeded(), "old sequence refused");
    }
    auto cancelled = SubmitLuaTestReload(*fixture.session, {"@cancel.lua", "return 1"});
    Check(cancelled.Succeeded() && CancelLuaTestReload(*fixture.session, *cancelled.value).Succeeded(), "cancel before running");
    Check(!fixture.runtime->Take(), "cancelled request not executed");
    Check(QueryLuaTestReload(*fixture.session, *cancelled.value).value->resultCode == LuaReloadResult::Cancelled, "cancel ack");
}

void DiagnosticsWithoutReload() {
    Fixture fixture(false);
    const auto refused = SubmitLuaTestReload(*fixture.session, {"@disabled.lua", "error('must not run')"});
    Check(!refused.Succeeded() && refused.diagnostics.front().code == "LUA_TEST_RELOAD_DISABLED",
        "diagnostics-only session cannot queue code");
    Check(!fixture.runtime->Take(), "disabled submit leaves mailbox untouched");
    Check(fixture.runtime->Report(LuaReloadResult::RuntimeError, "original asynchronous error"), "report without reload");
    const auto active = QueryLuaTestReload(*fixture.session);
    Check(active.Succeeded() && active.value->runtimeMessage == "original asynchronous error" && active.value->processActive,
        "diagnostics remain visible while reload disabled");
    fixture.ownership.store(detail::LuaTestProcessOwnership::OwnedExited);
    const auto ended = QueryLuaTestReload(*fixture.session);
    Check(ended.Succeeded() && !ended.value->processActive && ended.value->runtimeMessage == active.value->runtimeMessage,
        "last error remains readable after proven process exit");
}

void RejectionsAndUnknown() {
    Fixture fixture;
    Check(!SubmitLuaTestReload(*fixture.session, {"@bad.lua", std::string("a\0b", 3)}).Succeeded(), "NUL refused");
    Check(!SubmitLuaTestReload(*fixture.session, {"@bad.lua", std::string(kLuaReloadSourceCapacity + 1, 'a')}).Succeeded(), "size bound");
    Check(!SubmitLuaTestReload(*fixture.session, {"@bad.lua\n", "return 1"}).Succeeded(), "control name refused");
    fixture.ownership.store(detail::LuaTestProcessOwnership::Unconfirmed);
    Check(!SubmitLuaTestReload(*fixture.session, {"@test.lua", "return 1"}).Succeeded(), "unowned PID refused");
    Check(!QueryLuaTestReload(*fixture.session).Succeeded(), "unowned handshake refused");
    fixture.ownership.store(detail::LuaTestProcessOwnership::OwnedRunning);
    auto sent = SubmitLuaTestReload(*fixture.session, {"@test.lua", "return 1"});
    Check(sent.Succeeded() && fixture.runtime->Take().has_value(), "start");
    fixture.ownership.store(detail::LuaTestProcessOwnership::OwnedExited);
    auto stopped = QueryLuaTestReload(*fixture.session, *sent.value);
    Check(stopped.Succeeded() && !stopped.value->processActive && stopped.value->state == LuaTestReloadState::Running
        && stopped.value->resultCode != LuaReloadResult::Success, "exit during execution remains unknown");
    Check(!SubmitLuaTestReload(*fixture.session, {"@again.lua", "return 2"}).Succeeded(), "exited process rejected");
    fixture.ownership.store(detail::LuaTestProcessOwnership::OwnedRunning);
    Check(fixture.runtime->Complete(*sent.value, LuaReloadResult::RuntimeError, "@test.lua:1: failure"), "failure ack");
    auto failure = QueryLuaTestReload(*fixture.session, *sent.value);
    Check(failure.Succeeded() && failure.value->resultCode == LuaReloadResult::RuntimeError, "error preserved");
    fixture.mailbox->resultSequence = *sent.value + 1;
    Check(!QueryLuaTestReload(*fixture.session, *sent.value).Succeeded(), "wrong ACK refused");
    fixture.mailbox->resultSequence = *sent.value;
    fixture.mailbox->mapSha256[0] = 'b';
    Check(!QueryLuaTestReload(*fixture.session).Succeeded(), "map mutation refused");
}

void ConcurrencyAndUtf8() {
    Fixture fixture;
    std::atomic<unsigned> successes{0};
    auto send = [&] { if (SubmitLuaTestReload(*fixture.session, {"@race.lua", "return 1"}).Succeeded()) ++successes; };
    std::thread first(send), second(send); first.join(); second.join();
    Check(successes == 1, "only one publisher owns slot");
    auto request = fixture.runtime->Take(); Check(request.has_value(), "receive race winner");
    std::string message;
    for (unsigned i = 0; i < 6000; ++i) message += "中";
    Check(fixture.runtime->Complete(request->sequence, LuaReloadResult::RuntimeError, message), "large UTF8 error");
    const auto response = QueryLuaTestReload(*fixture.session, request->sequence);
    Check(response.Succeeded() && response.value->resultTruncated && response.value->message.size() % 3 == 0,
        "truncated at full UTF8 boundary");
    fixture.runtime->Stop();
    auto stopped = QueryLuaTestReload(*fixture.session);
    Check(stopped.Succeeded() && stopped.value->state == LuaTestReloadState::Complete
        && stopped.value->runtimeState == LuaRuntimeState::Stopped && stopped.value->resultTruncated
        && stopped.value->message == response.value->message, "stopped runtime preserves bounded completed request");
}

void FinalAcknowledgment() {
    for (const bool stopRuntime : {false, true}) {
        Fixture fixture;
        const auto sent = SubmitLuaTestReload(*fixture.session, {"@final.lua", "return 1"});
        Check(sent.Succeeded() && fixture.runtime->Take().has_value(), "final request started");
        Check(fixture.runtime->Complete(*sent.value, LuaReloadResult::RuntimeError, "@final.lua:1: original failure"), "final error ACK");
        if (stopRuntime) fixture.runtime->Stop();
        fixture.ownership.store(detail::LuaTestProcessOwnership::Unconfirmed);
        Check(!QueryLuaTestReload(*fixture.session, *sent.value).Succeeded(), "terminal ACK bypassed process ownership");
        fixture.ownership.store(detail::LuaTestProcessOwnership::OwnedExited);
        const auto ended = QueryLuaTestReload(*fixture.session, *sent.value);
        Check(ended.Succeeded() && ended.value->state == LuaTestReloadState::Complete && !ended.value->processActive
            && ended.value->resultCode == LuaReloadResult::RuntimeError
            && ended.value->message == "@final.lua:1: original failure", "final ACK lost after exit");
        Check(!SubmitLuaTestReload(*fixture.session, {"@late.lua", "return 2"}).Succeeded(), "terminal request allowed submission");
        ++fixture.mailbox->resultSequence;
        Check(!QueryLuaTestReload(*fixture.session, *sent.value).Succeeded(), "terminal result sequence mismatch accepted");
        --fixture.mailbox->resultSequence;
        fixture.mailbox->resultTruncated = 2;
        Check(!QueryLuaTestReload(*fixture.session, *sent.value).Succeeded(), "invalid terminal truncation accepted");
        fixture.mailbox->resultTruncated = 0;
        fixture.mailbox->resultBytes = static_cast<std::uint32_t>(kLuaReloadResultCapacity);
        Check(!QueryLuaTestReload(*fixture.session, *sent.value).Succeeded(), "terminal oversized ACK accepted");
    }
    Fixture queued;
    const auto sent = SubmitLuaTestReload(*queued.session, {"@queued.lua", "return 1"});
    Check(sent.Succeeded(), "queued exit request");
    queued.runtime->Stop(); queued.ownership.store(detail::LuaTestProcessOwnership::OwnedExited);
    const auto ended = QueryLuaTestReload(*queued.session, *sent.value);
    Check(ended.Succeeded() && ended.value->state == LuaTestReloadState::Stopped
        && ended.value->resultCode != LuaReloadResult::Success && ended.value->message.empty(), "unexecuted request forged ACK");
}

void RuntimeDiagnostics() {
    Fixture fixture;
    auto sent = SubmitLuaTestReload(*fixture.session, {"@pending.lua", "return 1"});
    Check(sent.Succeeded(), "queue diagnostic overlap");
    Check(fixture.runtime->Report(LuaReloadResult::RuntimeError, "@timer.lua:3: first"), "separate diagnostic");
    auto queued = QueryLuaTestReload(*fixture.session, *sent.value);
    Check(queued.Succeeded() && queued.value->state == LuaTestReloadState::Queued
        && queued.value->runtimeDiagnosticSequence == 1
        && queued.value->runtimeDiagnosticCode == LuaReloadResult::RuntimeError
        && queued.value->runtimeMessage == "@timer.lua:3: first", "diagnostic preserves queued request");
    auto chunk = fixture.runtime->Take();
    Check(chunk.has_value(), "take after diagnostic");
    Check(fixture.runtime->Complete(chunk->sequence, LuaReloadResult::Success, "ack"), "ACK after diagnostic");
    std::string large;
    for (unsigned i = 0; i < 6000; ++i) large += "错";
    Check(fixture.runtime->Report(LuaReloadResult::BudgetExceeded, large), "bounded UTF8 diagnostic");
    auto complete = QueryLuaTestReload(*fixture.session, *sent.value);
    Check(complete.Succeeded() && complete.value->message == "ack"
        && complete.value->resultCode == LuaReloadResult::Success
        && complete.value->runtimeDiagnosticSequence == 2
        && complete.value->runtimeDiagnosticTruncated && complete.value->runtimeMessage.size() % 3 == 0,
        "runtime error leaves exact ACK intact");
    std::atomic<bool> finished{false}, failed{false};
    std::thread reader([&] {
        std::uint32_t prior = 2;
        while (!finished.load()) {
            const auto value = QueryLuaTestReload(*fixture.session);
            if (!value.Succeeded() || value.value->runtimeDiagnosticSequence < prior
                || value.value->message != "ack") { failed.store(true); break; }
            prior = value.value->runtimeDiagnosticSequence;
            if (prior > 2 && value.value->runtimeMessage != "error-" + std::to_string(prior)) {
                failed.store(true); break;
            }
        }
    });
    for (unsigned sequence = 3; sequence <= 300; ++sequence)
        if (!fixture.runtime->Report(LuaReloadResult::RuntimeError, "error-" + std::to_string(sequence))) failed.store(true);
    finished.store(true); reader.join();
    Check(!failed.load(), "concurrent diagnostic snapshot not torn");
    Check(QueryLuaTestReload(*fixture.session).value->runtimeDiagnosticSequence == 300, "latest diagnostic");
    // 模拟读到正在发布：保留上一份已核对记录，不读半份正文，也不阻塞 UI。
    fixture.mailbox->runtimeDiagnosticRevision = 601;
    Check(QueryLuaTestReload(*fixture.session).value->runtimeDiagnosticSequence == 300, "busy diagnostic uses prior snapshot");
    fixture.mailbox->runtimeDiagnosticRevision = 600;
    fixture.mailbox->runtimeDiagnosticBytes = static_cast<std::uint32_t>(kLuaReloadResultCapacity);
    Check(!QueryLuaTestReload(*fixture.session).Succeeded(), "invalid stable diagnostic rejected");
    fixture.mailbox->version = 1;
    Check(!QueryLuaTestReload(*fixture.session).Succeeded(), "old mailbox version rejected");
}
}

int main() {
    try {
        Lifecycle(); RejectionsAndUnknown(); ConcurrencyAndUtf8(); FinalAcknowledgment(); RuntimeDiagnostics(); DiagnosticsWithoutReload();
        std::cout << "Lua mailbox service fixture passed; simulated receiver only; game_evidence=0\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
