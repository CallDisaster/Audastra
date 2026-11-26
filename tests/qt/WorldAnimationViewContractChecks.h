#pragma once
#include "WorldAnimationPlayer.h"
#include "WorldAnimationPlaybackContractChecks.h"
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>

// 集成测试复用真实 Qt 视口/播放器和原创模型 fixture；不会接触作者窗口或地图。
// 调用者在完成 backend 接线后调用，播放器必须持有当前同修订场景。
namespace WorldAnimationViewContractChecks {
inline void PausedViewReplay(War3Map::Modules::WorldViewport& viewport,
    War3Map::Modules::WorldAnimationPlayer& player) {
    player.SetPlaying(false); WORLD_WAIT_IDLE(player);
    const auto originalCamera=viewport.Camera();
    const auto originalSize=viewport.size();
    const auto before=player.Frame(); QVERIFY(before);
    auto camera=originalCamera; camera.targetX+=32.0f;
    QVERIFY(viewport.SetCamera(camera,viewport.DocumentRevision()).Succeeded());
    QTRY_VERIFY(player.Frame() && player.Frame()->worldView
        && player.Frame()->worldView->camera.targetX==camera.targetX);
    const auto moved=player.Frame();
    QVERIFY(moved->contentVersion>before->contentVersion);
    QCOMPARE(moved->states.size(),before->states.size());
    for (std::size_t i=0; i<moved->states.size(); ++i) {
        QCOMPARE(moved->states[i].elapsedMs,before->states[i].elapsedMs);
        QCOMPARE(moved->states[i].globalMs,before->states[i].globalMs);
    }
    viewport.resize(originalSize.width()+47,originalSize.height()+19);
    QTRY_VERIFY(player.Frame() && player.Frame()->worldView
        && player.Frame()->worldView->physicalWidth==viewport.AnimationViewSnapshot().physicalWidth
        && player.Frame()->worldView->physicalHeight==viewport.AnimationViewSnapshot().physicalHeight);
    const auto resized=player.Frame();
    QVERIFY(resized->contentVersion>moved->contentVersion);
    QVERIFY(!player.Playing());
    for (std::size_t i=0; i<resized->states.size(); ++i) {
        QCOMPARE(resized->states[i].elapsedMs,before->states[i].elapsedMs);
        QCOMPARE(resized->states[i].globalMs,before->states[i].globalMs);
    }
    viewport.resize(originalSize);
    QVERIFY(viewport.SetCamera(originalCamera,viewport.DocumentRevision()).Succeeded());
    WORLD_WAIT_IDLE(player);
}
inline void OffscreenKeepsScheduling(War3Map::Modules::WorldViewport& viewport,
    War3Map::Modules::WorldAnimationPlayer& player) {
    player.SetPlaying(false); WORLD_WAIT_IDLE(player);
    const auto originalCamera=viewport.Camera();
    auto camera=originalCamera;
    camera.targetX=20000.0f; camera.targetY=20000.0f; camera.distance=2048.0f;
    QSignalSpy messages(&player, &War3Map::Modules::WorldAnimationPlayer::Message);
    QVERIFY(viewport.SetCamera(camera,viewport.DocumentRevision()).Succeeded());
    QTRY_VERIFY2(player.Frame() && player.Frame()->worldView
        && player.Frame()->worldView->camera.targetX==camera.targetX,
        messages.isEmpty() ? "no player diagnostic" : messages.last().front().toString().toUtf8().constData());
    QVERIFY(player.Frame()->offscreenPoseInstances>0);
    QVERIFY(player.Picking()->instances.empty()); // 原创模型全在远离镜头的原点附近。
    const auto offscreen=player.Frame(); const auto picking=player.Picking();
    player.SetPlaying(true);
    QTRY_VERIFY(player.Frame()->contentVersion>offscreen->contentVersion);
    QTRY_VERIFY(player.Frame()->states.front().elapsedMs>offscreen->states.front().elapsedMs);
    QVERIFY(player.Playing());
    QCOMPARE(player.Picking(),picking); // 同帧映射更新保留宿主持有的指针。
    player.SetPlaying(false); WORLD_WAIT_IDLE(player);
    QVERIFY(viewport.SetCamera(originalCamera,viewport.DocumentRevision()).Succeeded());
    QTRY_VERIFY(player.Frame()->worldView->camera.targetX==originalCamera.targetX);
    QVERIFY(!player.Picking()->instances.empty());
}
inline void MovingViewKeepsScheduling(War3Map::Modules::WorldViewport& viewport,
    War3Map::Modules::WorldAnimationPlayer& player) {
    player.SetPlaying(false); WORLD_WAIT_IDLE(player);
    const auto original=viewport.Camera(); const auto before=player.Frame(); QVERIFY(before);
    QVERIFY(before->states.size()>1);
    War3Map::Render::AnimationEdit firstEdit, secondEdit;
    firstEdit.identity=before->states[0].identity; firstEdit.elapsedMs=345;
    secondEdit.identity=before->states[1].identity; secondEdit.elapsedMs=456;
    player.Edit(firstEdit);
    auto intermediate=original; intermediate.targetX+=1;
    QVERIFY(viewport.SetCamera(intermediate,viewport.DocumentRevision()).Succeeded());
    player.Edit(secondEdit); WORLD_WAIT_IDLE(player);
    QCOMPARE(player.Frame()->states[0].elapsedMs,std::uint64_t{345});
    QCOMPARE(player.Frame()->states[1].elapsedMs,std::uint64_t{456}); // 相机排队不能丢在途实例编辑。
    std::uint64_t observedContent=before->contentVersion, latestGlobal=before->states.front().globalMs;
    unsigned cameraChanges=0, acceptedDuringMotion=0;
    // 每次接收后的 Changed 回调里换镜头：此时链上下一帧或在等刷新间隔、或已在计算，换镜头都必须带进下一帧。
    // 不再只在“忙”时换：等间隔时播放器报空闲，只在忙时换要靠计算恰好在跑，次数取决于机器快慢。
    const auto moving=QObject::connect(&player,&War3Map::Modules::WorldAnimationPlayer::Changed,&player,[&] {
        if (!player.Playing() || cameraChanges>=64) return;
        if (player.Frame()->contentVersion!=observedContent) {
            observedContent=player.Frame()->contentVersion; ++acceptedDuringMotion;
            latestGlobal=player.Frame()->states.front().globalMs;
        }
        auto camera=original; camera.targetX+=static_cast<float>(++cameraChanges);
        camera.yawRadians+=static_cast<float>(cameraChanges)*0.001f;
        QVERIFY(viewport.SetCamera(camera,viewport.DocumentRevision()).Succeeded());
    });
    const auto stopMoving=qScopeGuard([&] { QObject::disconnect(moving); }); // 断言提前返回也不再改镜头
    player.SetPlaying(true);
    // 64 次换镜头各等一帧接收，按刷新间隔至少约 2 秒；慢测试机上可达 5 秒，给 20 秒上限，真卡住仍会失败。
    QTRY_VERIFY_WITH_TIMEOUT(cameraChanges>=64, 20000);
    QObject::disconnect(moving);
    QVERIFY(acceptedDuringMotion>=3); // 连续移动中接受姿势，不能等到鼠标停下再更新。
    QVERIFY(latestGlobal>before->states.front().globalMs); // 相机刷新也消费实际播放时间。
    player.SetPlaying(false); WORLD_WAIT_IDLE(player);
    QVERIFY(viewport.SetCamera(original,viewport.DocumentRevision()).Succeeded());
    QTRY_VERIFY(player.Frame()->worldView && player.Frame()->worldView->camera.targetX==original.targetX);
}
} // namespace WorldAnimationViewContractChecks
