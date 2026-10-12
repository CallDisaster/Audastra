#include "../OwnLua/NativeSignature.h"
#include "../OwnLua/LuaMailboxRuntime.h"
#include "War3MapCore/Runtime/LuaTestMailboxValidation.h"
#include <windows.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

using namespace Audastra::Runtime32;
using namespace Audastra::Runtime32::OwnLua;
namespace {
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Signature() {
    const auto timer = ParseNativeSignature("(Htimer;RBC)V");
    Check(timer && timer->arguments.size() == 4 && timer->arguments[0].handleType == "timer"
        && timer->arguments[1].kind == NativeKind::Real && timer->arguments[2].kind == NativeKind::Boolean
        && timer->arguments[3].kind == NativeKind::Code && timer->returns.kind == NativeKind::Nothing,
        "timer registration signature");
    const auto conversion = ParseNativeSignature("(I)Hplayer;");
    Check(conversion && conversion->returns.kind == NativeKind::Handle && conversion->returns.handleType == "player",
        "typed handle return");
    Check(ParseNativeSignature("()S").has_value() && ParseNativeSignature("(SS)B").has_value(), "empty/string args");
    for (const char scalar : std::string_view("IRBS")) {
        const auto canonical = std::string("(Hhandle;II)") + scalar;
        const auto terminated = ParseNativeSignature(canonical + ';');
        const auto ordinary = ParseNativeSignature(canonical);
        Check(ordinary && terminated && *ordinary == *terminated,
            "single scalar return terminator preserves argument and return semantics");
        Check(ParseNativeSignature(std::string("()") + scalar + ';').has_value(), "empty-argument scalar return terminator");
    }
    for (const auto text : {"", "()", "(I)Vjunk", "(V)V", "(H;)V", "(Hunit)V", "(Z)V", "(R)r",
        "(Hunit; )V", "(H../unit;)V", "()V\n", "()I()I", "()I;;", "()R;I", "()B;\n", "()S; ",
        "(I;)I", "()Hhandle;;", "()V;", "()C;", "()Z;"})
        Check(!ParseNativeSignature(text), "malformed signature accepted");
    Check(!ParseNativeSignature("(" + std::string(33, 'I') + ")V"), "unbounded args accepted");
    Check(!ParseNativeSignature("(" + std::string(33, 'I') + ")I;"), "scalar terminator cannot bypass argument budget");
    Check(!NativeIdentifier("9unit") && !NativeIdentifier(std::string(129, 'A')), "identifier budget");
}

void Publish(LuaTestMailbox& channel, std::uint32_t seq, std::string_view text = "value = 1") {
    channel.state = static_cast<LONG>(LuaMailboxState::Writing);
    channel.requestSequence = seq;
    std::memcpy(channel.requestNonce, channel.sessionNonce, sizeof(channel.sessionNonce));
    std::memcpy(channel.requestMapSha256, channel.mapSha256, sizeof(channel.mapSha256));
    constexpr char name[] = "@hot-reload/原创.lua";
    channel.chunkNameBytes = sizeof(name) - 1;
    std::memcpy(channel.chunkName, name, sizeof(name));
    channel.sourceBytes = static_cast<std::uint32_t>(text.size());
    std::memcpy(channel.source, text.data(), text.size());
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&channel.state), static_cast<LONG>(LuaMailboxState::Ready));
}

void Mailbox() {
    auto channel = std::make_unique<LuaTestMailbox>();
    const std::string nonce(32, 'a'), sha(64, 'b');
    std::memcpy(channel->sessionNonce, nonce.c_str(), nonce.size() + 1);
    std::memcpy(channel->mapSha256, sha.c_str(), sha.size() + 1);
    LuaMailboxRuntime runtime(*channel);
    channel->version = 1;
    Check(!runtime.Attach(nonce, sha), "old mailbox protocol accepted");
    channel->version = kLuaMailboxVersion;
    Check(!runtime.Attach(std::string(32, 'c'), sha), "wrong launch nonce accepted");
    Check(runtime.Attach(nonce, sha), "owned mailbox attach");
    Check(!runtime.Attach(nonce, sha), "duplicate attach accepted");
    Check(channel->gameProcessId == GetCurrentProcessId() && channel->mainThreadId == GetCurrentThreadId(), "thread identity");
    Check(runtime.Report(LuaReloadResult::RuntimeError, "@main.lua:1: runtime error"), "runtime diagnostic");
    Check(channel->runtimeDiagnosticSequence == 1 && channel->runtimeDiagnosticRevision == 2
        && channel->resultSequence == 0 && channel->state == static_cast<LONG>(LuaMailboxState::Empty),
        "runtime diagnostic stole request ACK");
    bool otherReported = true;
    std::thread reporter([&] { otherReported = runtime.Report(LuaReloadResult::HostFailure, "worker"); });
    reporter.join(); Check(!otherReported, "worker published runtime diagnostic");
    for (std::uint32_t seq = 1; seq <= 20; ++seq) {
        Publish(*channel, seq);
        if (seq == 1) {
            bool otherTook = true;
            std::thread other([&] { otherTook = runtime.Take().has_value(); runtime.Stop(); });
            other.join(); Check(!otherTook && channel->state == static_cast<LONG>(LuaMailboxState::Ready), "worker executed mailbox");
        }
        const auto request = runtime.Take();
        Check(request && request->sequence == seq && request->source == "value = 1", "request not snapshotted");
        Check(!runtime.Take() && !runtime.Complete(seq + 1, LuaReloadResult::Success, ""), "running overwrite/foreign ACK");
        Check(runtime.Complete(seq, LuaReloadResult::Success, "已执行"), "completion failed");
        Check(channel->resultSequence == seq && channel->resultCode == LuaReloadResult::Success, "ack identity lost");
    }
    Publish(*channel, 20);
    Check(!runtime.Take() && channel->resultCode == LuaReloadResult::InvalidRequest, "replayed sequence executed");
    Publish(*channel, 21); channel->requestMapSha256[0] = 'c';
    Check(!runtime.Take() && channel->resultCode == LuaReloadResult::IdentityMismatch, "wrong map executed");
    Publish(*channel, 22); channel->sourceBytes = static_cast<DWORD>(kLuaReloadSourceCapacity + 1);
    Check(!runtime.Take() && channel->resultCode == LuaReloadResult::InvalidRequest, "oversized source read");
    Publish(*channel, 23, std::string_view("a\0b", 3));
    Check(!runtime.Take() && channel->resultCode == LuaReloadResult::InvalidRequest, "embedded NUL accepted");
    Publish(*channel, 24);
    Check(runtime.Take().has_value(), "valid request after rejects");
    std::string message(kLuaReloadResultCapacity - 2, 'x'); message += "错误";
    Check(runtime.Complete(24, LuaReloadResult::RuntimeError, message), "large result failed");
    Check(channel->resultTruncated && channel->resultBytes == kLuaReloadResultCapacity - 2
        && LuaMailboxUtf8({channel->result, channel->resultBytes}), "result splits UTF-8");
    runtime.Stop();
    Check(channel->runtimeState == static_cast<LONG>(LuaRuntimeState::Stopped) && !runtime.Take(), "stopped runtime resumed");
    Check(!runtime.Report(LuaReloadResult::RuntimeError, "after stop"), "stopped runtime wrote diagnostic");
    Check(!LuaMailboxUtf8("\xc0\x80") && !LuaMailboxUtf8("\xed\xa0\x80")
        && !LuaMailboxUtf8("\xf4\x90\x80\x80") && !LuaMailboxUtf8("\xe4\xb8"), "invalid UTF-8 accepted");
}
}

int main() {
    try {
        Signature(); Mailbox();
        std::cout << "PASS own-lua protocol/signatures; simulated mailbox only; game_evidence=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; return 1;
    }
}
