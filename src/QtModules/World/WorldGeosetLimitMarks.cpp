#include "WorldGeosetLimitMarks.h"

#include "War3MapCore/Render/ModelGameLimits.h"
#include "War3MapCore/Render/ModelPicking.h"

#include <algorithm>

namespace War3Map::Modules {

bool WorldGeosetLimitMarks::Contains(std::uint32_t request) const noexcept {
    return std::binary_search(requests.begin(), requests.end(), request);
}

const std::string* WorldGeosetLimitMarks::Detail(std::string_view modelPath) const {
    const auto found = details.find(WorldModelPathKey(modelPath));
    return found == details.end() ? nullptr : &found->second;
}

std::string WorldModelPathKey(std::string_view path) {
    std::string key(path);
    for (auto& c : key) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return key;
}

WorldGeosetLimitMarks FindWorldGeosetLimitMarks(const std::vector<Diagnostic>& warnings,
    std::size_t requestCount, const std::function<std::string_view(std::size_t)>& modelPath) {
    WorldGeosetLimitMarks marks;
    std::map<std::string, std::string> flagged;
    for (const auto& warning : warnings) {
        if (warning.code != Render::kGeosetOverGameLimitCode || !warning.origin) continue;
        const auto source = warning.origin->source.u8string();
        const std::string path(reinterpret_cast<const char*>(source.data()), source.size());
        auto detail = warning.message;
        const auto prefix = std::string("模型 ") + path + "：";
        if (detail.starts_with(prefix)) detail.erase(0, prefix.size());
        flagged.emplace(WorldModelPathKey(path), std::move(detail));
    }
    if (flagged.empty()) return marks;
    for (std::size_t request = 0; request < requestCount; ++request) {
        const auto found = flagged.find(WorldModelPathKey(modelPath(request)));
        if (found == flagged.end()) continue;
        marks.requests.push_back(static_cast<std::uint32_t>(request));
        marks.details.emplace(found->first, found->second);
    }
    return marks;
}

namespace {
Render::WorldModelScene BoundsOnly(const Render::WorldModelScene& source) {
    Render::WorldModelScene marked;
    marked.modelBounds = source.modelBounds; // 共享只读租约，不复制数据
    return marked;
}
} // namespace

Render::WorldModelScene GeosetLimitInstances(const Render::WorldModelScene& scene,
    const WorldGeosetLimitMarks& marks, const WorldRequestVisible& visible) {
    auto marked = BoundsOnly(scene);
    if (marks.requests.empty()) return marked;
    const auto count = std::min(scene.instances.size(), scene.instanceRequests.size());
    for (std::size_t i = 0; i < count && marked.instances.size() < kGeosetLimitMaxBoxes; ++i) {
        const auto request = scene.instanceRequests[i];
        if (request == Render::kBakedInstance || !marks.Contains(request) || (visible && !visible(request))) continue;
        marked.instances.push_back(scene.instances[i]); marked.instanceRequests.push_back(request);
    }
    return marked;
}

Render::WorldModelScene GeosetLimitInstances(const Render::AnimatedModelScene& scene,
    const Render::AnimationFrame& frame, const WorldGeosetLimitMarks& marks, const WorldRequestVisible& visible) {
    auto marked = BoundsOnly(scene.geometry);
    if (marks.requests.empty()) return marked;
    const auto count = std::min(frame.instances.size(), frame.poses.size());
    for (std::size_t i = 0; i < count && marked.instances.size() < kGeosetLimitMaxBoxes; ++i) {
        const auto& pose = frame.poses[i];
        if (pose.hidden || !marks.Contains(pose.requestIndex) || (visible && !visible(pose.requestIndex))) continue;
        marked.instances.push_back(frame.instances[i].instance); marked.instanceRequests.push_back(pose.requestIndex);
    }
    return marked;
}

void AppendGeosetLimitBoxes(Render::OverlayLines& lines, const Render::WorldModelScene& marked) {
    for (std::uint32_t i = 0; i < marked.instances.size(); ++i)
        if (const auto box = Render::ModelInstanceBox(marked, i)) Render::AppendBoxLines(lines, *box, kGeosetLimitBoxColor);
}

} // namespace War3Map::Modules
