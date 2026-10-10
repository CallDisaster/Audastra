#pragma once
#include "WorldAnimationPlayer.h"
#include <QSignalSpy>
#include <QTest>

// 集成测试复用真实 Qt 视口/播放器和原创模型 fixture；不会接触作者窗口或地图。
// 调用者在完成 backend 接线后调用，播放器必须持有当前同修订场景。
namespace WorldAnimationViewContractChecks {
inline void PausedViewReplay(War3Map::Modules::WorldViewport& viewport,
    War3Map::Modules::WorldAnimationPlayer& player) {
    player.SetPlaying(false); QTRY_VERIFY(!player.Busy());
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
    QTRY_VERIFY(!player.Busy());
}
inline void OffscreenKeepsScheduling(War3Map::Modules::WorldViewport& viewport,
    War3Map::Modules::WorldAnimationPlayer& player) {
    player.SetPlaying(false); QTRY_VERIFY(!player.Busy());
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
    player.SetPlaying(false); QTRY_VERIFY(!player.Busy());
    QVERIFY(viewport.SetCamera(originalCamera,viewport.DocumentRevision()).Succeeded());
    QTRY_VERIFY(player.Frame()->worldView->camera.targetX==originalCamera.targetX);
    QVERIFY(!player.Picking()->instances.empty());
}
inline void MovingViewKeepsScheduling(War3Map::Modules::WorldViewport& viewport,
    War3Map::Modules::WorldAnimationPlayer& player) {
    player.SetPlaying(false); QTRY_VERIFY(!player.Busy());
    const auto original=viewport.Camera(); const auto before=player.Frame(); QVERIFY(before);
    QVERIFY(before->states.size()>1);
    War3Map::Render::AnimationEdit firstEdit, secondEdit;
    firstEdit.identity=before->states[0].identity; firstEdit.elapsedMs=345;
    secondEdit.identity=before->states[1].identity; secondEdit.elapsedMs=456;
    player.Edit(firstEdit);
    auto intermediate=original; intermediate.targetX+=1;
    QVERIFY(viewport.SetCamera(intermediate,viewport.DocumentRevision()).Succeeded());
    player.Edit(secondEdit); QTRY_VERIFY(!player.Busy());
    QCOMPARE(player.Frame()->states[0].elapsedMs,std::uint64_t{345});
    QCOMPARE(player.Frame()->states[1].elapsedMs,std::uint64_t{456}); // 相机排队不能丢在途实例编辑。
    std::uint64_t observedContent=before->contentVersion, latestGlobal=before->states.front().globalMs;
    unsigned cameraChanges=0, acceptedDuringMotion=0;
    // 在 worker 的 Changed 回调中换镜头，确保变更发生在工作尚未完成时；不依赖 CPU 求值耗时。
    const auto moving=QObject::connect(&player,&War3Map::Modules::WorldAnimationPlayer::Changed,&player,[&] {
        if (!player.Playing() || !player.Busy() || cameraChanges>=64) return;
        if (player.Frame()->contentVersion!=observedContent) {
            observedContent=player.Frame()->contentVersion; ++acceptedDuringMotion;
            latestGlobal=player.Frame()->states.front().globalMs;
        }
        auto camera=original; camera.targetX+=static_cast<float>(++cameraChanges);
        camera.yawRadians+=static_cast<float>(cameraChanges)*0.001f;
        QVERIFY(viewport.SetCamera(camera,viewport.DocumentRevision()).Succeeded());
    });
    player.SetPlaying(true);
    QTRY_VERIFY(cameraChanges>=64);
    QObject::disconnect(moving);
    QVERIFY(acceptedDuringMotion>=3); // 连续移动中接受姿势，不能等到鼠标停下再更新。
    QVERIFY(latestGlobal>before->states.front().globalMs); // 相机刷新也消费实际播放时间。
    player.SetPlaying(false); QTRY_VERIFY(!player.Busy());
    QVERIFY(viewport.SetCamera(original,viewport.DocumentRevision()).Succeeded());
    QTRY_VERIFY(player.Frame()->worldView && player.Frame()->worldView->camera.targetX==original.targetX);
}
} // namespace WorldAnimationViewContractChecks
