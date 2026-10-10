#include "Fixtures.h"

#include <nlohmann/json.hpp>

#include <iostream>
#include <chrono>
#include <future>
#include <thread>

using namespace War3Map;
using namespace War3Map::Feedback;
using namespace FeedbackFixtures;

namespace {

class PausedTransport final : public IReportTransport {
public:
    std::promise<void> entered;
    std::promise<TransportResult> release;
    std::shared_future<TransportResult> result = release.get_future().share();
    std::atomic<std::size_t> calls{0};
    bool Configured() const noexcept override { return true; }
    TransportResult Send(const Delivery&) override {
        ++calls;
        entered.set_value();
        return result.get();
    }
};

class PausedSend final {
public:
    PausedSend(const std::shared_ptr<FeedbackQueue>& queue, const DraftResult& draft, const Preview& preview, std::int64_t now) {
        auto entered = transport.entered.get_future();
        pending_ = std::async(std::launch::async, [&, queue, id = draft.entry.id, sha = preview.delivery.previewSha256, now] {
            return queue->TrySend(id, sha, now, transport);
        });
        const auto started = entered.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        if (!started) Release({false, false, "original paused fixture did not start"});
        Require(started, "paused original transport entered");
    }
    ~PausedSend() {
        if (!released_) Release({false, false, "original fixture scope cleanup"});
        if (pending_.valid()) pending_.wait();
    }
    Result<SendOutcome> Finish(TransportResult response) { Release(std::move(response)); return pending_.get(); }
    PausedTransport transport;
private:
    void Release(TransportResult response) { if (!released_) { released_ = true; transport.release.set_value(std::move(response)); } }
    bool released_ = false;
    std::future<Result<SendOutcome>> pending_;
};

void ConcurrentDelivery() {
    using namespace std::chrono_literals;
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "open concurrent queue");
    const auto draft = Take(queue->Draft(AgentDraft(), 6000), "concurrent initial draft");
    const auto preview = Take(queue->GetPreview(draft.entry.id), "concurrent complete preview");
    Require(queue->ConfirmPreview(draft.entry.id, preview.delivery.previewSha256).Succeeded() && queue->SetConsent(true).Succeeded(),
        "concurrent delivery approved");
    PausedSend paused(queue, draft, preview, 6001);
    FakeTransport second;
    auto queries = std::async(std::launch::async, [&] {
        const auto listed = Take(queue->List(), "list while network paused");
        Require(listed.size() == 1 && listed.front().attempts == 1, "inFlight durable snapshot visible while transport waits");
        Require(Take(queue->TrySend(draft.entry.id, preview.delivery.previewSha256, 6002, second), "second concurrent attempt").state
            == AttemptState::AlreadyComplete && second.calls == 0, "same inFlight cannot dispatch twice");
        return Take(queue->Draft(UserDraft(), 6002), "new draft while network paused");
    });
    const auto ready = queries.wait_for(1s) == std::future_status::ready;
    const auto outcome = paused.Finish({false, true, "original definitely rejected offline failure"});
    const auto other = queries.get();
    Require(ready, "network wait does not hold Core queue mutex");
    Require(Take(outcome, "paused retry outcome").state == AttemptState::RetryScheduled, "paused attempt receives bounded retry");
    const auto retained = Take(queue->List(), "latest snapshot preserves concurrently drafted report");
    Require(retained.size() == 2 && Take(queue->GetStatus(other.entry.id), "other draft preserved").state == QueueState::Pending,
        "late receipt merges current snapshot and does not erase parallel draft");
}

void WithdrawDuringDelivery() {
    using namespace std::chrono_literals;
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "open withdrawal queue");
    const auto draft = Take(queue->Draft(AgentDraft(), 7000), "withdrawal initial draft");
    const auto preview = Take(queue->GetPreview(draft.entry.id), "withdrawal complete preview");
    Require(queue->ConfirmPreview(draft.entry.id, preview.delivery.previewSha256).Succeeded() && queue->SetConsent(true).Succeeded(),
        "withdrawal delivery approved");
    PausedSend paused(queue, draft, preview, 7001);
    auto queries = std::async(std::launch::async, [&] {
        Require(queue->SetConsent(false).Succeeded() && !queue->Consent(), "close consent while transport paused");
        Require(Take(queue->List(), "closed queue while transport paused").empty(), "opt-out clears active pending record");
        const auto fresh = Take(queue->Draft(UserDraft(), 7002), "explicit local draft after opt out");
        Require(queue->SetConsent(true).Succeeded(), "new consent epoch explicitly enabled");
        return fresh;
    });
    const auto ready = queries.wait_for(1s) == std::future_status::ready;
    const auto late = paused.Finish({true, false, {}});
    const auto fresh = queries.get();
    Require(ready && !late.Succeeded(), "close is nonblocking and late sent receipt cannot restore removed report");
    const auto listed = Take(queue->List(), "new consent epoch list");
    Require(listed.size() == 1 && listed.front().id == fresh.entry.id && !queue->GetStatus(draft.entry.id).Succeeded(),
        "late receipt preserves only new epoch local draft");
    Require(queue->LastSentUnixSeconds() == 7001, "confirmed late acceptance updates only last-sent metadata");

    const auto secondPreview = Take(queue->GetPreview(fresh.entry.id), "delete-in-flight preview");
    Require(queue->ConfirmPreview(fresh.entry.id, secondPreview.delivery.previewSha256).Succeeded(), "delete-in-flight approval");
    PausedSend deleting(queue, fresh, secondPreview, 7003);
    auto remove = std::async(std::launch::async, [&] { return queue->Delete(fresh.entry.id); });
    const auto deleteReady = remove.wait_for(1s) == std::future_status::ready;
    const auto removedReceipt = deleting.Finish({true, false, {}});
    Require(remove.get().Succeeded() && deleteReady && !removedReceipt.Succeeded(), "delete during network does not block or revive content");
    const auto tombstone = Take(queue->GetStatus(fresh.entry.id), "deleted record after late receipt");
    Require(tombstone.state == QueueState::Deleted && !tombstone.hasPayload && tombstone.attempts == 0,
        "late receipt preserves explicit content-free tombstone");
}

void InterruptedReceiptAndRevokedGeneration() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "open receipt conflict queue");
    const auto draft = Take(queue->Draft(AgentDraft(), 8000), "receipt conflict draft");
    const auto preview = Take(queue->GetPreview(draft.entry.id), "receipt conflict preview");
    Require(queue->ConfirmPreview(draft.entry.id, preview.delivery.previewSha256).Succeeded() && queue->SetConsent(true).Succeeded(),
        "receipt conflict approved");
    const auto path = temp.Path() / L"queue" / L"queue.json";
    {
        PausedSend paused(queue, draft, preview, 8001);
        const auto inFlight = Read(path);
        Write(path, inFlight + " ");
        Require(!queue->SetConsent(false).Succeeded() && !queue->Consent(), "revocation stays effective if clear write conflicts");
        Write(path, inFlight);
        Require(queue->SetConsent(true).Succeeded(), "reenable after failed clear uses new consent generation");
        const auto result = Take(paused.Finish({false, true, "original retryable refusal"}), "revoked epoch final result");
        Require(result.state == AttemptState::Failed && result.entry.attempts == 1 && !result.entry.nextAttemptUnixSeconds,
            "old consent generation cannot schedule retry after disable and reenable");
    }
    const auto another = Take(queue->Draft(UserDraft(), 8002), "unknown receipt separate draft");
    const auto anotherPreview = Take(queue->GetPreview(another.entry.id), "unknown receipt complete preview");
    Require(queue->ConfirmPreview(another.entry.id, anotherPreview.delivery.previewSha256).Succeeded(), "unknown receipt approved");
    {
        PausedSend paused(queue, another, anotherPreview, 8003);
        Write(path, Read(path) + " ");
        Require(!paused.Finish({true, false, {}}).Succeeded(), "final receipt write conflict does not claim committed sent state");
        FakeTransport duplicate;
        Require(Take(queue->TrySend(another.entry.id, anotherPreview.delivery.previewSha256, 8004, duplicate), "inFlight after failed receipt").state
            == AttemptState::AlreadyComplete && duplicate.calls == 0, "failed receipt retains durable inFlight and blocks replay");
    }
    queue.reset();
    queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "recover receipt conflict without replay");
    const auto recovered = Take(queue->GetStatus(another.entry.id), "recovered unknown receipt status");
    Require(recovered.state == QueueState::Failed && recovered.attempts == 1 && !recovered.nextAttemptUnixSeconds,
        "restart unknown acceptance never auto-resends");
}

void ConsentAndDedup() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "open new owned queue");
    Require(!queue->Consent() && !queue->LastSentUnixSeconds(), "consent default disabled, no historical send");
    auto draft = AgentDraft();
    draft.diagnostic = R"(original error at C:\Users\Synthetic User\private.txt apiKey="tiny-private-value")";
    draft.recentLogs = "private excluded-log-content";
    draft.conversationSnippet = "private excluded-conversation-content";
    const auto first = Take(queue->Draft(draft, 1000), "draft without consent");
    Require(!first.consent && !first.deduplicated && first.message.find("未同意") != std::string::npos, "no-consent local drafting result");
    const auto preview = Take(queue->GetPreview(first.entry.id), "complete cleaned preview");
    NoPrivate(preview.delivery.reportJson, {"Synthetic User", "private.txt", "tiny-private-value", "excluded-log-content", "excluded-conversation-content"});
    Require(preview.delivery.reportJson.find("0123456789abcdef0123456789abcdef01234567") != std::string::npos, "allow trusted commit field");
    const auto stored = Read(temp.Path() / L"queue" / L"queue.json");
    NoPrivate(stored, {"Synthetic User", "private.txt", "tiny-private-value", "excluded-log-content", "excluded-conversation-content"});
    FakeTransport transport;
    Require(Take(queue->TrySend(first.entry.id, preview.delivery.previewSha256, 1001, transport), "no-consent send gate").state == AttemptState::NoConsent, "no consent gate result");
    Require(transport.calls == 0, "unconsented feedback makes no transport call");
    Require(queue->ConfirmPreview(first.entry.id, preview.delivery.previewSha256).Succeeded(), "confirm complete report while offline");
    Require(queue->SetConsent(true).Succeeded(), "explicit consent");
    const auto duplicate = Take(queue->Draft(draft, 1010), "dedup repeat within 24h");
    Require(duplicate.deduplicated && duplicate.entry.id == first.entry.id && duplicate.entry.occurrences == 2 && !duplicate.entry.previewConfirmed, "dedup increments and invalidates confirmation");
    Require(!queue->ConfirmPreview(first.entry.id, preview.delivery.previewSha256).Succeeded(), "stale preview rejected after count changes");
    Require(Take(queue->TrySend(first.entry.id, preview.delivery.previewSha256, 1011, transport), "stale send gate").state == AttemptState::Unreviewed && transport.calls == 0, "stale preview never sent");
    const auto current = Take(queue->GetPreview(first.entry.id), "updated complete preview");
    Require(current.delivery.previewSha256 != preview.delivery.previewSha256, "actual body hash changes with count and last seen");
    Require(queue->ConfirmPreview(first.entry.id, current.delivery.previewSha256).Succeeded(), "confirm current complete preview");
    transport.configured = false;
    Require(Take(queue->TrySend(first.entry.id, current.delivery.previewSha256, 1011, transport), "endpoint absent gate").state == AttemptState::NoEndpoint && transport.calls == 0, "empty endpoint disables transport");
    transport.configured = true;
    const auto sent = Take(queue->TrySend(first.entry.id, current.delivery.previewSha256, 1012, transport), "send reviewed report through original in-memory fake");
    Require(sent.state == AttemptState::Sent && transport.calls == 1 && sent.entry.state == QueueState::Sent && !sent.entry.hasPayload, "sent state retains no payload");
    Require(queue->LastSentUnixSeconds() == 1012 && !queue->GetPreview(first.entry.id).Succeeded(), "last sent local state and removed preview");
    const auto afterSend = Read(temp.Path() / L"queue" / L"queue.json");
    NoPrivate(afterSend, {"original error", "tiny-private-value", "private.txt"});
    Require(Take(queue->TrySend(first.entry.id, current.delivery.previewSha256, 1013, transport), "sent no replay").state == AttemptState::AlreadyComplete && transport.calls == 1, "sent report is not sent twice");
    const auto sentDuplicate = Take(queue->Draft(draft, 1020), "dedup sent problem");
    Require(sentDuplicate.deduplicated && sentDuplicate.entry.state == QueueState::Sent && sentDuplicate.entry.occurrences == 3 && !sentDuplicate.entry.hasPayload, "sent repeat only adds count");
    const auto outsideWindow = Take(queue->Draft(draft, 1000 + 86400), "new issue at exact 24h boundary");
    Require(!outsideWindow.deduplicated && outsideWindow.entry.id != first.entry.id, "24h dedup boundary opens new record");
    auto newVersion = draft;
    newVersion.application.version = "1.2.4";
    Require(!Take(queue->Draft(newVersion, 1000 + 86401), "version fingerprint changes").deduplicated, "version differentiates problem");
    const auto busy = FeedbackQueue::Open(temp.Path() / L"queue", Context());
    Require(!busy.Succeeded(), "second writer cannot open same queue");
    queue.reset();
    queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "reopen persisted sanitized queue");
    Require(!queue->Consent() && queue->LastSentUnixSeconds() == 1012, "queue does not persist or migrate consent");
    Require(Take(queue->List(), "reopened queue list").size() == 3, "queue states persisted across restart");
    Require(queue->SetConsent(false).Succeeded(), "closing clears all unsent queue items");
    const auto remaining = Take(queue->List(), "list after closing consent");
    Require(remaining.size() == 1 && remaining.front().state == QueueState::Sent, "only sent summary remains after opt out");
    Require(queue->Delete(first.entry.id).Succeeded(), "delete retained sent summary");
    const auto deleted = Take(queue->GetStatus(first.entry.id), "deleted state");
    Require(deleted.state == QueueState::Deleted && !deleted.hasPayload && deleted.errorCode.empty() && deleted.version.empty(), "explicit delete leaves content-free tombstone");
}

void RetryAndCleanup() {
    Temp temp;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "open retry fixture queue");
    const auto draft = Take(queue->Draft(AgentDraft(), 2000), "retry fixture draft");
    const auto preview = Take(queue->GetPreview(draft.entry.id), "retry fixture preview");
    Require(queue->ConfirmPreview(draft.entry.id, preview.delivery.previewSha256).Succeeded() && queue->SetConsent(true).Succeeded(), "retry fixture approved");
    FakeTransport transport;
    transport.response = {false, true, R"(offline endpoint https://user:secret@example.invalid/?token=private-server-token credentialRef="private-ref")"};
    auto first = Take(queue->TrySend(draft.entry.id, preview.delivery.previewSha256, 2001, transport), "first retry failure");
    Require(first.state == AttemptState::RetryScheduled && first.entry.attempts == 1 && first.entry.nextAttemptUnixSeconds == 2031, "first retry exponential budget");
    NoPrivate(first.entry.failureReason, {"private-server-token", "private-ref", "user:secret"});
    Require(Take(queue->TrySend(draft.entry.id, preview.delivery.previewSha256, 2030, transport), "retry before due").state == AttemptState::NotDue && transport.calls == 1, "backoff prevents request");
    const auto second = Take(queue->TrySend(draft.entry.id, preview.delivery.previewSha256, 2031, transport), "second retry failure");
    Require(second.state == AttemptState::RetryScheduled && second.entry.attempts == 2 && second.entry.nextAttemptUnixSeconds == 2091, "second retry doubles delay");
    const auto third = Take(queue->TrySend(draft.entry.id, preview.delivery.previewSha256, 2091, transport), "third retry failure");
    Require(third.state == AttemptState::Failed && third.entry.attempts == 3 && !third.entry.nextAttemptUnixSeconds && transport.calls == 3, "three total attempts then fail");
    Require(Take(queue->TrySend(draft.entry.id, preview.delivery.previewSha256, 9999, transport), "no infinite retry").state == AttemptState::AlreadyComplete && transport.calls == 3, "failed record never auto loops");
    Require(queue->SetConsent(false).Succeeded(), "failed queue clear");
    Require(Take(queue->List(), "cleared failed list").empty(), "opt out removes failed reports completely");
    queue.reset();
    queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context()), "reopen cleared queue");
    Require(Take(queue->List(), "persisted cleared list").empty(), "empty queue persists");

    const auto unknown = Take(queue->Draft(AgentDraft(), 3000), "throwing channel fixture draft");
    const auto unknownPreview = Take(queue->GetPreview(unknown.entry.id), "throwing channel fixture preview");
    Require(queue->ConfirmPreview(unknown.entry.id, unknownPreview.delivery.previewSha256).Succeeded() && queue->SetConsent(true).Succeeded(), "throwing fixture approved");
    transport.throwOnSend = true;
    const auto stopped = Take(queue->TrySend(unknown.entry.id, unknownPreview.delivery.previewSha256, 3001, transport), "channel exception bounded");
    Require(stopped.state == AttemptState::Failed && stopped.entry.attempts == 1 && !stopped.entry.nextAttemptUnixSeconds, "unknown channel result is not retried");
}

void StrictPersistenceAndBounds() {
    Temp temp;
    QueueLimits limits;
    limits.maxEntries = 2;
    auto queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context(), limits), "open bounded queue");
    const auto first = Take(queue->Draft(UserDraft(), 4000), "bounded first user draft");
    const auto second = Take(queue->Draft(UserDraft(), 4001), "bounded second user draft");
    Require(first.entry.id != second.entry.id && !second.deduplicated, "user suggestions are not globally deduplicated");
    const auto baseline = Read(temp.Path() / L"queue" / L"queue.json");
    Require(!queue->Draft(UserDraft(), 4002).Succeeded(), "full unsent queue fails closed");
    Require(Read(temp.Path() / L"queue" / L"queue.json") == baseline, "full queue failure leaves complete old snapshot");
    Require(!queue->List(0, 129).Succeeded() && Take(queue->List(999, 1), "bounded past-end list").empty(), "list is bounded");
    Require(queue->Delete(first.entry.id).Succeeded(), "create replaceable tombstone");
    Require(queue->Draft(UserDraft(), 4002).Succeeded(), "old tombstone is boundedly pruned for new draft");
    Require(queue->SetConsent(true).Succeeded(), "external modification fixture consent");
    const auto preview = Take(queue->GetPreview(second.entry.id), "external modification fixture preview");
    Require(queue->ConfirmPreview(second.entry.id, preview.delivery.previewSha256).Succeeded(), "external fixture confirmation");
    const auto valid = Read(temp.Path() / L"queue" / L"queue.json");
    Write(temp.Path() / L"queue" / L"queue.json", valid + " ");
    FakeTransport transport;
    Require(!queue->TrySend(second.entry.id, preview.delivery.previewSha256, 4003, transport).Succeeded() && transport.calls == 0, "external bytes changed prevents send before dispatch");
    Require(!queue->SetConsent(false).Succeeded() && !queue->Consent(), "opt-out write conflict still disables sending without false clear claim");
    Require(!Take(queue->List(), "uncommitted clear still has records").empty(), "failed atomic clear does not claim removal");
    queue.reset();

    const auto path = temp.Path() / L"queue" / L"queue.json";
    auto unknown = nlohmann::json::parse(valid);
    unknown["mapContent"] = "private raw map content";
    Write(path, unknown.dump());
    Require(!FeedbackQueue::Open(temp.Path() / L"queue", Context(), limits).Succeeded(), "unknown root field rejected");
    Require(Read(path) == unknown.dump(), "invalid queue not rewritten or repaired silently");
    auto unclean = nlohmann::json::parse(valid);
    for (auto& entry : unclean["entries"]) if (!entry["report"].is_null()) {
        entry["report"]["diagnostic"] = R"(private content C:\Users\Synthetic User\private.txt)";
        break;
    }
    Write(path, unclean.dump());
    Require(!FeedbackQueue::Open(temp.Path() / L"queue", Context(), limits).Succeeded(), "unclean persistent report rejected");
    const auto duplicateKey = std::string(R"({"schema":"audastra-feedback-queue-v1","schema":"audastra-feedback-queue-v1","nextSequence":1,"lastSentUnixSeconds":null,"entries":[]})");
    Write(path, duplicateKey);
    Require(!FeedbackQueue::Open(temp.Path() / L"queue", Context(), limits).Succeeded(), "duplicate JSON key rejected");
    Write(path, valid.substr(0, valid.size() / 2));
    Require(!FeedbackQueue::Open(temp.Path() / L"queue", Context(), limits).Succeeded(), "partial JSON snapshot rejected");
    Write(path, valid);
    auto oversizedLimits = limits;
    oversizedLimits.maxStorageBytes = 1024;
    Require(!FeedbackQueue::Open(temp.Path() / L"queue", Context(), oversizedLimits).Succeeded(), "file budget rejects existing oversized snapshot");

    // 构造原创的已确认 inFlight 快照，模拟发送已开始但进程没有完整留下回执。
    auto interrupted = nlohmann::json::parse(valid);
    for (auto& entry : interrupted["entries"]) if (entry["id"] == second.entry.id) {
        entry["attempts"] = 1;
        entry["deliveryInFlight"] = true;
        entry["nextAttemptUnixSeconds"] = nullptr;
    }
    Write(path, interrupted.dump());
    queue = Take(FeedbackQueue::Open(temp.Path() / L"queue", Context(), limits), "recover unknown inFlight result without resend");
    const auto state = Take(queue->GetStatus(second.entry.id), "interrupted state");
    Require(state.state == QueueState::Failed && state.attempts == 1 && state.failureReason.find("结果未知") != std::string::npos, "interrupted delivery stops automatically");
    Require(queue->SetConsent(true).Succeeded(), "recover fixture consent");
    Require(Take(queue->TrySend(second.entry.id, preview.delivery.previewSha256, 5000, transport), "unknown delivery gate").state == AttemptState::AlreadyComplete && transport.calls == 0, "unknown past result not replayed");
}

} // namespace

int main() {
    try {
        ConsentAndDedup();
        RetryAndCleanup();
        StrictPersistenceAndBounds();
        ConcurrentDelivery();
        WithdrawDuringDelivery();
        InterruptedReceiptAndRevokedGeneration();
        std::cout << "feedback queue checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "feedback queue failure: " << error.what() << '\n';
        return 1;
    }
}
