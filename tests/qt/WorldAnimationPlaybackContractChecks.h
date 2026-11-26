#pragma once
#include "WorldAnimationPlayer.h"
#include "WorldPlaybackClock.h"
#include <QElapsedTimer>
#include <QTest>
#include <algorithm>

namespace WorldAnimationPlaybackContractChecks {
// 等播放器空闲，上限 20 秒（真卡住仍失败）。超过 1 秒仍忙时记一条警告写明卡在哪一步，
// 测试机上偶发的数秒延迟即使最终通过也留下定位数据。
inline bool WaitIdle(War3Map::Modules::WorldAnimationPlayer& player, const char* file, int line) {
    QElapsedTimer timer; timer.start();
    bool reported = false;
    while (player.Busy()) {
        if (!reported && timer.elapsed() > 1000) {
            qWarning("%s(%d) busy over 1 s: %s", file, line, qPrintable(player.BusyReason()));
            reported = true;
        }
        if (timer.elapsed() > 20000) return false;
        QTest::qWait(5);
    }
    if (reported) qWarning("%s(%d) idle after %lld ms", file, line, static_cast<long long>(timer.elapsed()));
    return true;
}
#define WORLD_WAIT_IDLE(player) \
    QVERIFY2(WorldAnimationPlaybackContractChecks::WaitIdle((player), __FILE__, __LINE__), qPrintable((player).BusyReason()))
// 调用方先用既有catalog gate建立“plan已就绪、旧pose尚未接受”的次序。
// 这里只延长真实接受帧的同步UI通知，不处理事件；验证使用实际rebase开始时间，
// 不把候选排队时间当成worker执行，也不以总耗时作为渲染性能出口。
class ReadyRosterNotificationProbe {
public:
    explicit ReadyRosterNotificationProbe(War3Map::Modules::WorldAnimationPlayer& player)
        : player_(player), scene_(player.Frame() ? player.Frame()->sceneVersion : 0),
          content_(player.Frame() ? player.Frame()->contentVersion : 0) {
        connection_=QObject::connect(&player_,&War3Map::Modules::WorldAnimationPlayer::Changed,&player_,[this] {
            const auto frame=player_.Frame();
            if (observed_ || !frame || frame->sceneVersion!=scene_ || frame->contentVersion==content_ || !player_.InstancesBusy()) return;
            observed_=true; acceptedContent_=frame->contentVersion;
            enteredNs_=War3Map::Modules::WorldInstanceNowNs();
            QTest::qSleep(150); // 模拟作者大图的同步状态/属性通知，期间不得消费Qt完成回调。
            returnedNs_=War3Map::Modules::WorldInstanceNowNs();
        });
    }
    ~ReadyRosterNotificationProbe() { QObject::disconnect(connection_); }
    ReadyRosterNotificationProbe(const ReadyRosterNotificationProbe&)=delete;
    ReadyRosterNotificationProbe& operator=(const ReadyRosterNotificationProbe&)=delete;
    [[nodiscard]] std::uint64_t AcceptedContent() const noexcept { return acceptedContent_; }
    void VerifyOverlap(const War3Map::Modules::WorldInstanceTiming& timing) const {
        QVERIFY(observed_);
        QVERIFY(enteredNs_ && returnedNs_>enteredNs_);
        QVERIFY(timing.rebaseStartedNs);
        QVERIFY2(timing.rebaseStartedNs<returnedNs_,
            "ready roster waited for all synchronous accepted-frame notifications before starting its independent rebase");
    }
private:
    War3Map::Modules::WorldAnimationPlayer& player_;
    QMetaObject::Connection connection_;
    std::uint64_t scene_=0,content_=0,acceptedContent_=0,enteredNs_=0,returnedNs_=0;
    bool observed_=false;
};
inline void ResumeDuringPausedWork(War3Map::Modules::WorldAnimationPlayer& player) {
    player.SetPlaying(false);
    WORLD_WAIT_IDLE(player);
    const auto before = player.Frame();
    QVERIFY(before && !before->states.empty());
    bool resumed = false;
    // StartNext先置Busy再发Changed、随后才启动worker：确定恢复请求到达旧暂停帧计算期间。
    const auto connection = QObject::connect(&player, &War3Map::Modules::WorldAnimationPlayer::Changed,
        &player, [&] {
            if (resumed || !player.Busy() || player.Playing()) return;
            resumed = true;
            player.SetPlaying(true);
        });
    player.Step();
    QObject::disconnect(connection);
    QVERIFY(resumed);
    const auto initialGlobal = before->states.front().globalMs;
    QTRY_VERIFY(player.Frame() && player.Frame()->contentVersion > before->contentVersion + 1
        && player.Frame()->states.front().globalMs > initialGlobal + 33
        && std::all_of(player.Frame()->states.begin(), player.Frame()->states.end(),
            [](const auto& state) { return state.playing; }));
    QVERIFY(player.Playing()); // 返回的旧paused帧不能把恢复意图吞掉。
    player.SetPlaying(false);
    WORLD_WAIT_IDLE(player);
}
// 播放时钟只扣整毫秒、余数留给下一次：60 秒不规则步长（含非整毫秒）下，每次消耗后实际经过与累计扣除之差恒在
// [0, 1 ms)，不单向漂移；落后超过 1 秒只给 1 秒，多出部分记为丢弃，恒等式仍成立。
inline void PlaybackClockKeepsTime() {
    War3Map::Modules::WorldPlaybackClock clock;
    QCOMPARE(clock.Consume(1'000'000'000), std::uint64_t{0}); // 未启动不消耗
    std::int64_t now = 7'000'000'123;
    clock.Start(now);
    std::uint32_t seed = 20261126u;
    std::uint64_t steps = 0;
    while (clock.ElapsedNs(now) < 60'000'000'000) {
        seed = seed * 1664525u + 1013904223u;
        now += 3'000'000 + static_cast<std::int64_t>(seed % 37'000'001u); // 3–40 ms，绝大多数不是整毫秒
        (void)clock.Consume(now);
        const auto lag = clock.ElapsedNs(now) - static_cast<std::int64_t>(clock.ConsumedNs());
        QVERIFY2(lag >= 0 && lag < 1'000'000, "playback clock drifted by a whole millisecond or more");
        ++steps;
    }
    QVERIFY(steps > 1000);
    QCOMPARE(clock.droppedNs, std::uint64_t{0});
    // 60 Hz 节奏（16.666… ms）同样不漂：3600 步后扣除恰为经过时间的整毫秒部分。
    clock.Start(now);
    const auto begin = now;
    for (int i = 1; i <= 3600; ++i) {
        now = begin + (std::int64_t{1'000'000'000} * i) / 60;
        (void)clock.Consume(now);
    }
    QCOMPARE(clock.consumedMs, std::uint64_t{60'000});
    QVERIFY(clock.ElapsedNs(now) - static_cast<std::int64_t>(clock.ConsumedNs()) < 1'000'000);
    // 落后 2.5 秒：只给 1 秒，丢弃 1.5 秒（加上原余数），消耗后余数归零。
    const auto before = clock.ConsumedNs();
    now += 2'500'000'000;
    QCOMPARE(clock.Consume(now), std::uint64_t{1000});
    QCOMPARE(clock.ElapsedNs(now), static_cast<std::int64_t>(clock.ConsumedNs()));
    QVERIFY(clock.ConsumedNs() - before >= 2'500'000'000u);
    // 接续帧固定网格：未开网格按回退时刻起算；晚不到一个间隔沿网格追回，晚一个间隔以上从实际开算时刻重排。
    War3Map::Modules::WorldChainGrid grid;
    constexpr std::int64_t interval = 16'000'000;
    QCOMPARE(grid.DueNs(5'000, interval), std::int64_t{5'000});
    QCOMPARE(grid.Start(100'000'000, 100'000'000, interval), std::int64_t{0});
    QCOMPARE(grid.DueNs(0, interval), std::int64_t{116'000'000});
    QCOMPARE(grid.Start(116'000'000, 119'000'000, interval), std::int64_t{3'000'000});
    QCOMPARE(grid.DueNs(0, interval), std::int64_t{132'000'000}); // 晚 3 ms 不顺延下一帧
    QCOMPARE(grid.Start(132'000'000, 150'000'000, interval), std::int64_t{18'000'000});
    QCOMPARE(grid.DueNs(0, interval), std::int64_t{166'000'000}); // 晚超过一个间隔：从实际开算重排
    grid.Reset(); QCOMPARE(grid.DueNs(7, interval), std::int64_t{7});
    // 求值 14 / 18 ms 交替（平均 16 ms）：网格节拍 100 帧共 1602 ms；旧节拍（上一帧开始 + 间隔）要 1700 ms。
    std::int64_t finished = 1'000'000'000;
    for (int i = 0; i < 100; ++i) {
        const auto due = grid.DueNs(finished, interval);
        const auto start = std::max(due, finished);
        (void)grid.Start(due, start, interval);
        finished = start + (i % 2 ? 18'000'000 : 14'000'000);
    }
    QCOMPARE(finished - 1'000'000'000, 100 * interval + 2'000'000);
}
// 常驻接续：稳定播放时求值线程直接接续、按刷新间隔起步。在它等间隔时编辑会收回许可：等待中的接续帧不求值、
// 按作废结束，随后编辑帧照常接收。这里实际走到“等待中作废”至少一次（chain_skipped > 0）。
inline void ChainSkipsOnRevoke(War3Map::Modules::WorldAnimationPlayer& player) {
    using War3Map::Modules::WorldAnimationTimingOutcome;
    player.SetPlaying(false);
    WORLD_WAIT_IDLE(player);
    QVERIFY(player.Frame() && !player.Frame()->states.empty());
    const auto cursor = player.LatestTimingSequence();
    const auto countSince = [&](std::uint64_t after, auto predicate) {
        const auto history = player.TimingHistory(after);
        return std::count_if(history.begin(), history.end(), predicate);
    };
    const auto directAccepted = [](const auto& t) { return t.directChained && t.outcome == WorldAnimationTimingOutcome::Accepted; };
    const auto skippedFrame = [](const auto& t) { return t.chainSkipped; };
    player.SetPlaying(true);
    QTRY_VERIFY(countSince(cursor, directAccepted) > 0);
    const double original = player.Frame()->states.front().speed;
    double speed = original;
    for (int round = 0; round < 40 && !countSince(cursor, skippedFrame); ++round) {
        const auto before = player.LatestTimingSequence();
        QTRY_VERIFY(countSince(before, directAccepted) > 0); // 许可已重新授予、求值线程又在接续
        War3Map::Render::AnimationEdit edit;
        edit.identity = player.Frame()->states.front().identity;
        speed = speed == original ? original * 1.25 : original; edit.speed = speed;
        player.Edit(std::move(edit));
        QTRY_VERIFY(player.Frame() && player.Frame()->states.front().speed == speed);
    }
    const auto history = player.TimingHistory(cursor);
    QVERIFY2(std::any_of(history.begin(), history.end(), skippedFrame),
        "no chained frame was dropped while waiting for the refresh interval");
    for (const auto& t : history) {
        if (!t.chainSkipped) continue;
        QVERIFY(t.directChained);
        QCOMPARE(t.outcome, WorldAnimationTimingOutcome::Cancelled);
        QCOMPARE(t.contentVersion, std::uint64_t{0}); // 未求值
    }
    // 含编辑的接收帧记有编辑到首帧时延。
    QVERIFY(std::any_of(history.begin(), history.end(), [](const auto& t) {
        return t.editQueuedNs && t.outcome == WorldAnimationTimingOutcome::Accepted && t.editLatencyNs > 0;
    }));
    // 播放时钟的累计扣除与实际经过之差恒在 1 ms 以内。
    for (const auto& t : history)
        if (t.clockElapsedNs) QVERIFY(t.clockElapsedNs >= t.clockConsumedNs && t.clockElapsedNs - t.clockConsumedNs < 1'000'000);
    if (speed != original) { // 还原速度，后续检查不受影响
        War3Map::Render::AnimationEdit restore;
        restore.identity = player.Frame()->states.front().identity; restore.speed = original;
        player.Edit(std::move(restore));
        QTRY_VERIFY(player.Frame() && player.Frame()->states.front().speed == original);
    }
    player.SetPlaying(false);
    WORLD_WAIT_IDLE(player);
}
} // namespace WorldAnimationPlaybackContractChecks
