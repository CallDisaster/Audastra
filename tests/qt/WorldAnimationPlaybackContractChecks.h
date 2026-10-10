#pragma once
#include "WorldAnimationPlayer.h"
#include <QTest>
#include <algorithm>

namespace WorldAnimationPlaybackContractChecks {
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
    QTRY_VERIFY(!player.Busy());
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
    QTRY_VERIFY(!player.Busy());
}
} // namespace WorldAnimationPlaybackContractChecks
