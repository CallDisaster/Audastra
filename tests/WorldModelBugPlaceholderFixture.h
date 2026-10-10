#pragma once
#include "War3MapCore/Services/WorldModelBugPlaceholderService.h"
#include "MapDataFixtures.h"
#include <stdexcept>

namespace WorldModelBugPlaceholderTest {
using namespace War3Map;
using namespace War3Map::MapData;
using namespace War3Map::Services;
template<class T> T Good(Result<T> result) {
    if (!result.Succeeded()) throw std::runtime_error(result.diagnostics.empty() ? "fixture failed" : result.diagnostics.front().message);
    return std::move(*result.value);
}
struct Fixture {
    std::vector<PlacementId> owners;
    std::shared_ptr<const WorldInstanceRegistry> registry;
    std::shared_ptr<Render::AnimatedModelScene> scene;
    std::shared_ptr<Render::AnimationFrame> frame;
    std::shared_ptr<const Render::OwnedStaticModelAsset> cube;
    std::array<WorldEditorFlags, 3> policy{};
    explicit Fixture(std::array<float, 3> origin = {}, float angle = 0) {
        auto units = Good(ParseUnitPlacements(MapDataFixtures::EmptyUnitsV8Sub11()));
        auto doodads = Good(ParseDoodadPlacements(MapDataFixtures::EmptyDoodadsV8Sub11()));
        cube = Good(Render::CreateCheckerBlockerAsset(std::nullopt));
        std::vector<Render::ModelInstanceRequest> requests;
        for (std::uint32_t i = 0; i < 3; ++i) {
            UnitPlacement unit; unit.rawcode = "hfoo"; unit.creationId = static_cast<std::int32_t>(i + 20);
            unit.position = {origin[0] + static_cast<float>(i * 160), origin[1], origin[2]};
            unit.rotation = angle; unit.scale = {1, 1, 1}; units.units.push_back(unit);
            owners.push_back(MakePlacementId(PlacementFile::Units, i, unit));
            Render::ModelInstanceRequest request; request.modelPath = "synthetic-owned.mdx";
            request.ownedStaticAsset = cube; request.position = unit.position; request.angle = angle;
            requests.push_back(std::move(request));
        }
        const std::vector<MapArchiveMember> members{{"war3mapUnits.doo", Good(units.Serialize())}, {"war3map.doo", Good(doodads.Serialize())}};
        auto document = MapDocument::FromMembers(members); MapDataEditSession edits(document);
        registry = Good(WorldInstanceRegistry::Create(document, {7, 0, edits.SessionIdentity(), edits.Revision(), "synthetic-source-SHA"}, requests, owners));
        auto service = Good(Render::ModelAnimationService::Create({7, 0, "synthetic-source-SHA"}, registry->CapturedSpecs(),
            [](std::string_view, std::uint64_t) { return Result<Render::AnimationResource>::Failure("Fixture", "UNEXPECTED_READ", "owned fixture cannot read MDX"); }));
        scene = std::make_shared<Render::AnimatedModelScene>(*service->Scene());
        frame = std::make_shared<Render::AnimationFrame>(*Good(service->Prepare(nullptr, {})));
    }
    WorldModelBugFlagsReader Flags() {
        return [this](const PlacementId& owner) -> std::optional<WorldEditorFlags> {
            if (owner.index >= owners.size() || owner != owners[owner.index]) return std::nullopt;
            return policy[owner.index];
        };
    }
    // 明确的合成分类输入；没有把插入拒绝字段当作实际采样器/后端失败证明。
    void PoseFailure(std::size_t index = 0) {
        auto& pose = frame->poses[index]; pose.hidden = true; pose.poseVersion = 0;
        pose.rejection = {War3RenderCore::Code::PoseKeysInvalid};
        std::fill_n(frame->alphas.begin() + pose.firstAlpha, pose.alphaCount, 0.0f);
    }
    WorldModelBugPlaceholderPlan Plan() { return Good(PlanWorldModelBugPlaceholders(scene, *frame, registry, owners, Flags())); }
};
}
