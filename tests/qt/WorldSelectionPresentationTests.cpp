#include "WorldSelectionPresentation.h"
#include "War3MapCore/MapData/MapTerrain.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include "../../src/Render/Scene/ModelSkinInput.h"
#include <cstring>
#include <QTest>
#include <cmath>

using namespace War3Map;
using namespace War3Map::Modules;

class WorldSelectionPresentationTests final : public QObject {
    Q_OBJECT
private slots:
    void RendererOutlineUsesCompletedMatrixAndDpi() {
        Render::ViewProjectionFrame frame{{2, 3, 4, 5}, {2,0,0,0, 0,3,0,0, 0,0,1,0, 1,-2,0,1}, 180, 120};
        const Render::ViewLogicalMetrics metrics{120, 80, 1.5};
        const auto outline = BuildWorldSelectionOutline(frame, metrics, {90, 60}, {30, 20});
        QVERIFY(outline.Succeeded()); QCOMPARE(outline.value->size(), std::size_t(4));
        const std::array<std::array<double,2>,4> expected{{{30,20}, {90,20}, {90,60}, {30,60}}};
        for (std::size_t i = 0; i < expected.size(); ++i) {
            const auto& line = outline.value->at(i);
            const double screenX = (static_cast<double>(line.from[0]) * 2 + 1 + 1) * 60;
            const double screenY = (1 - (static_cast<double>(line.from[1]) * 3 - 2)) * 40;
            QVERIFY(std::abs(screenX - expected[i][0]) < 0.0001);
            QVERIFY(std::abs(screenY - expected[i][1]) < 0.0001);
            QVERIFY(line.from[2] > 0 && line.from[2] < 1); // 在同一射线的near内侧，不被近面舍入裁掉。
            for (std::size_t axis = 0; axis < 3; ++axis)
                QCOMPARE(line.to[axis], outline.value->at((i + 1) % 4).from[axis]);
        }
        const auto next = BuildWorldSelectionOutline(frame, metrics, {90, 60}, {50, 40});
        QVERIFY(next.Succeeded()); QCOMPARE(next.value->size(), std::size_t(4)); // 新请求只有当前四边，未累积旧框。
    }
    void OutlineRejectsStaleOrInvalidViewWithoutPartialEdges() {
        Render::ViewProjectionFrame frame{{2, 3, 4, 5}, {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}, 120, 80};
        QVERIFY(BuildWorldSelectionOutline(frame, {120,80,1}, {-20,-30}, {160,120}).Succeeded());
        auto invalid = BuildWorldSelectionOutline(frame, {120,80,2}, {10,10}, {20,20});
        QVERIFY(!invalid.Succeeded()); QVERIFY(!invalid.value);
        frame.identity.frameCount = 0;
        invalid = BuildWorldSelectionOutline(frame, {120,80,1}, {10,10}, {20,20});
        QVERIFY(!invalid.Succeeded()); QVERIFY(!invalid.value);
    }
    // 原版 MiscData 有多档选取圈：各档网格必须各占一段索引，否则渲染核心拒收整层，单位脚下没有圈。
    void MultiBandCircleLayerPassesRendererInputValidation() {
        const std::string ui = "[SelectionCircle]\r\nNumSizes=3\r\nScaleFactor=72\r\nSize00=80\r\nSize01=200\r\n"
            "Texture00=ReplaceableTextures\\Selection\\SelectionCircleSmall\r\n"
            "Texture01=ReplaceableTextures\\Selection\\SelectionCircleMed\r\n"
            "Texture02=ReplaceableTextures\\Selection\\SelectionCircleLarge\r\nColorFriend=255,0,255,0\r\n";
        const std::string units = "[Misc]\r\nSelectionCircleBaseZ=8\r\n";
        std::vector<std::byte> blp(1181); std::memcpy(blp.data(), "BLP1", 4); blp[158] = std::byte{255};
        const auto put = [&](std::size_t offset, std::uint32_t value) { std::memcpy(blp.data() + offset, &value, 4); };
        for (const auto offset : {4u, 12u, 16u, 92u}) put(offset, 1);
        put(28, 1180);
        const auto bytes = [](const std::string& text) {
            std::vector<std::byte> out(text.size()); std::memcpy(out.data(), text.data(), text.size()); return out;
        };
        const Render::AnimationResourceReader read = [&](std::string_view path, std::uint64_t) {
            const auto data = path == "UI\\MiscData.txt" ? bytes(ui) : path == "Units\\MiscData.txt" ? bytes(units) : blp;
            return Result<Render::AnimationResource>::Success({data, "game", std::string(64, 'a')});
        };
        auto assets = LoadWorldSelectionCircleAssets({3, 4, "selection-test"}, read);
        QVERIFY2(assets.Succeeded(), assets.diagnostics.empty() ? "" : assets.diagnostics.front().message.c_str());
        QCOMPARE(assets.value->bands.size(), std::size_t(3));
        std::array<Render::ModelInstanceRequest, 3> requests;
        const std::array<float, 3> scales{1, 2, 4}; // 72 / 144 / 288：三档各一个
        for (std::size_t i = 0; i < requests.size(); ++i) {
            requests[i].position = {64.0f * static_cast<float>(i), 0, 0};
            requests[i].selection = Render::ModelSelectionDescription{scales[i], 0, true};
        }
        auto frame = BuildWorldSelectionCircleFrame(*assets.value, requests, nullptr, 5);
        QVERIFY(frame.Succeeded()); QCOMPARE(frame.value->frame->instances.size(), std::size_t(3));
        Render::AnimatedSceneDescriptor descriptor; descriptor.Refresh(*assets.value->scene, *frame.value->frame);
        Render::ModelScene model; std::uint64_t payload = 0;
        QVERIFY(Render::CopySkinnedModelInput(descriptor.scene, model, payload));
        QCOMPARE(model.models.size(), std::size_t(3));
    }
    void CircleUsesDisplayedGroundWithoutChangingPlacement() {
        WorldSelectionCircleAssets assets;
        auto scene = std::make_shared<Render::AnimatedModelScene>();
        scene->sceneVersion = 8; scene->documentRevision = 9; scene->models.resize(1);
        assets.scene = scene; assets.bands.push_back({100, 0}); assets.scaleFactor = 72; assets.baseHeight = 16;
        MapData::TerrainGridSnapshot terrain; terrain.header.width = terrain.header.height = 2;
        terrain.minimum = {0,0,140}; terrain.maximum = {128,128,140};
        for (const auto xy : std::array<std::array<float,2>,4>{{{0,0},{128,0},{0,128},{128,128}}}) {
            MapData::TerrainVertex vertex; vertex.position = {xy[0],xy[1],140};
            vertex.groundHeightRaw = 8192; vertex.cliffAndLayerRaw = 2; terrain.vertices.push_back(vertex);
        }
        terrain.tiles.push_back({{0,1,2,3}});
        std::array<Render::ModelInstanceRequest,1> requests;
        requests[0].position = {64,64,0}; requests[0].selection = Render::ModelSelectionDescription{1,0,true};
        auto frame = BuildWorldSelectionCircleFrame(assets, requests, &terrain, 10);
        QVERIFY(frame.Succeeded()); QCOMPARE(frame.value->frame->instances[0].instance.transform[11], 156.0f);
        QCOMPARE(requests[0].position[2], 0.0f); QVERIFY(frame.value->previewApproximate);
        requests[0].position[2] = 300; requests[0].selection->circleHeight = 20;
        frame = BuildWorldSelectionCircleFrame(assets, requests, &terrain, 11);
        QVERIFY(frame.Succeeded()); QCOMPARE(frame.value->frame->instances[0].instance.transform[11], 336.0f);
        frame = BuildWorldSelectionCircleFrame(assets, requests, nullptr, 12);
        QVERIFY(frame.Succeeded()); QCOMPARE(frame.value->frame->instances[0].instance.transform[11], 336.0f);
        QCOMPARE(frame.value->terrainUnavailable, std::size_t{4});
    }
};

QTEST_APPLESS_MAIN(WorldSelectionPresentationTests)
#include "WorldSelectionPresentationTests.moc"
