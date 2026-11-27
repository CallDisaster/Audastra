#include "WorldGeosetLimitMarks.h"
#include "War3MapCore/Render/ModelGameLimits.h"
#include "../support/GeosetLimitMdxFixture.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// 世界页：场景按模型报一次的“几何体超出游戏上限”对回每个摆放请求，并给可见的摆放画警示框。
namespace {

using namespace War3Map;
using namespace War3Map::Modules;

void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

Diagnostic LimitWarning(std::string_view path) {
    auto model = Render::ParseMdx(GeosetLimitFixture::BuildMdx({{16385, 3}}));
    Require(model.Succeeded() && model.value, "合成 MDX 解析失败");
    auto warning = Render::GeosetGameLimitWarning(*model.value, path);
    Require(warning.has_value(), "合成超限模型应有提醒");
    return std::move(*warning);
}

War3RenderModelInstanceV2 Instance(float x) {
    War3RenderModelInstanceV2 instance{};
    instance.transform[0] = instance.transform[5] = instance.transform[10] = 1.0f;
    instance.transform[3] = x;
    instance.color[0] = instance.color[1] = instance.color[2] = 1.0f;
    return instance;
}

void TestMarksMatchRequestsByPathKey() {
    std::vector<Diagnostic> warnings{LimitWarning("Doodads\\Big.mdx"), LimitWarning("Units\\Helper.mdx"),
        {DiagnosticSeverity::Warning, "World.Model", "WORLD_MODEL_UNAVAILABLE", "Doodads\\Small.mdx：缺失",
            OriginSpan{std::filesystem::path(u8"Doodads\\Small.mdx"), 0, 0, {}}}};
    const std::vector<std::string> paths{"doodads/BIG.mdx", "Doodads\\Small.mdx", "Doodads\\Big.mdx"};
    const auto marks = FindWorldGeosetLimitMarks(warnings, paths.size(),
        [&](std::size_t request) -> std::string_view { return paths[request]; });
    Require(marks.requests == std::vector<std::uint32_t>{0, 2}, "大小写/斜杠不同的同一模型都命中；其他代码不算");
    Require(marks.Contains(0) && !marks.Contains(1) && marks.Contains(2), "按请求序号查");
    const auto* detail = marks.Detail("DOODADS/big.MDX");
    Require(detail && detail->starts_with("第 1 个几何体 16385 顶点、3 索引"), "说明去掉“模型 路径：”前缀");
    Require(!marks.Detail("Units\\Helper.mdx"), "没对上摆放的模型不进逐摆放说明（保留场景汇总行）");
    Require(FindWorldGeosetLimitMarks({}, paths.size(), [&](std::size_t r) -> std::string_view { return paths[r]; }).requests.empty(),
        "没有提醒时为空");
}

void TestStaticBoxesRespectVisibilityAndBaked() {
    Render::WorldModelScene scene;
    scene.modelBounds = {std::array<float, 6>{0, 0, 0, 100, 100, 100}};
    scene.instances = {Instance(0), Instance(300), Instance(600), Instance(900)};
    scene.instanceRequests = {0, 1, 2, Render::kBakedInstance};
    WorldGeosetLimitMarks marks; marks.requests = {0, 2};
    const auto marked = GeosetLimitInstances(scene, marks, [](std::uint32_t request) { return request != 2; });
    Require(marked.instanceRequests == std::vector<std::uint32_t>{0} && marked.instances.size() == 1, "隐藏与未命中的不画");
    Render::OverlayLines lines;
    AppendGeosetLimitBoxes(lines, marked);
    Require(lines.size() == 12, "一个包围盒 12 条棱");
    Require(std::all_of(lines.begin(), lines.end(), [](const auto& line) {
        return line.color[0] == kGeosetLimitBoxColor[0] && line.color[1] == kGeosetLimitBoxColor[1]
            && line.color[2] == kGeosetLimitBoxColor[2];
    }), "警示色");
}

void TestAnimatedBoxesUseWholeFrame() {
    Render::AnimatedModelScene scene;
    scene.geometry.modelBounds = {std::array<float, 6>{0, 0, 0, 100, 100, 100}};
    Render::AnimationFrame frame;
    for (std::uint32_t request = 0; request < 3; ++request) {
        frame.instances.push_back({Instance(static_cast<float>(request) * 300.0f), 0, 0});
        Render::AnimationPoseLink pose; pose.requestIndex = request;
        pose.outsideView = request == 0; // 视野外照样画：框在世界坐标里
        pose.hidden = request == 1;      // 姿势被拒的不画
        frame.poses.push_back(pose);
    }
    WorldGeosetLimitMarks marks; marks.requests = {0, 1, 2};
    const auto marked = GeosetLimitInstances(scene, frame, marks, {});
    Require(marked.instanceRequests == std::vector<std::uint32_t>{0, 2}, "动画帧：视野外照画，被拒的不画");
}

void TestBoxCap() {
    Render::WorldModelScene scene;
    scene.modelBounds = {std::array<float, 6>{0, 0, 0, 10, 10, 10}};
    WorldGeosetLimitMarks marks;
    for (std::uint32_t request = 0; request < kGeosetLimitMaxBoxes + 5; ++request) {
        scene.instances.push_back(Instance(static_cast<float>(request)));
        scene.instanceRequests.push_back(request); marks.requests.push_back(request);
    }
    Require(GeosetLimitInstances(scene, marks, {}).instances.size() == kGeosetLimitMaxBoxes, "警示框数量有上限");
}

} // namespace

int main() {
    try {
        TestMarksMatchRequestsByPathKey();
        TestStaticBoxesRespectVisibilityAndBaked();
        TestAnimatedBoxesUseWholeFrame();
        TestBoxCap();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "world geoset limit marks ok\n";
    return 0;
}
