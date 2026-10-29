#include "War3MapCore/EditRevision.h"

#include <array>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

using namespace War3Map;
namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

void RequireOk(const Status& status) {
    Require(status.Succeeded(), status.diagnostics.empty() ? "operation failed" : status.diagnostics.front().message.c_str());
}

EditScopeSnapshot Original() {
    return {{"objects/unit", "unit-original"}, {"triggers", "trigger-original"}};
}

void TestContentAndUndo() {
    EditRevision history;
    const auto original = Original();
    RequireOk(history.Reset(original, original, original));
    Require(!history.IsDirty(EditSaveTarget::Project) && !history.IsDirty(EditSaveTarget::Map), "opened baselines clean");
    RequireOk(history.Observe(original, "取消编辑"));
    Require(history.Revision() == 0 && history.Journal().empty(), "no-op not an edit");
    auto edited = original;
    edited["objects/unit"] = "renamed-unit";
    RequireOk(history.Observe(edited, "重命名单位"));
    Require(history.IsDirty(EditSaveTarget::Project) && history.IsDirty(EditSaveTarget::Map), "edit dirty for both targets");
    Require(history.DirtyScopes(EditSaveTarget::Map) == std::vector<std::string>{"objects/unit"}, "only affected scope dirty");
    auto journal = history.Journal();
    Require(journal.size() == 1 && journal[0].revision == 1 && journal[0].description == "重命名单位", "edit provenance retained");
    Require(journal[0].changes.size() == 1 && journal[0].changes[0].before == "unit-original"
        && journal[0].changes[0].after == "renamed-unit", "before and after retained");
    journal.clear();
    Require(history.Journal().size() == 1, "journal is a detached snapshot");
    RequireOk(history.Observe(original, "撤销重命名", EditOperation::Undo));
    Require(!history.IsDirty(EditSaveTarget::Project) && !history.IsDirty(EditSaveTarget::Map), "actual original content clean after undo");
    Require(history.Revision() == 2 && history.Journal().back().operation == EditOperation::Undo, "revision monotonic through undo");
    RequireOk(history.Observe(edited, "重做重命名", EditOperation::Redo));
    Require(history.IsDirty(EditSaveTarget::Map) && history.Revision() == 3, "redo content observed");
}

void TestScopeCreationAndRemoval() {
    EditRevision history;
    auto original = Original();
    RequireOk(history.Reset(original, original, original));
    auto edited = original;
    edited.erase("objects/unit");
    edited["sounds"] = "sound-a";
    RequireOk(history.Observe(edited, "删除单位并增加声音"));
    Require(history.DirtyScopes(EditSaveTarget::Project) == std::vector<std::string>{"objects/unit", "sounds"}, "removed and added identities both included");
    const auto changes = history.Journal().back().changes;
    Require(changes.size() == 2 && changes[0].before == "unit-original" && !changes[0].after
        && !changes[1].before && changes[1].after == "sound-a", "absence is distinct from existing content");
    RequireOk(history.Observe(original, "撤销增删", EditOperation::Undo));
    Require(history.DirtyScopes(EditSaveTarget::Project).empty(), "scope deletion undo clean");
}

void TestSeparateBaselines() {
    EditRevision history;
    const auto original = Original();
    RequireOk(history.Reset(original, original, original));
    auto edited = original;
    edited["triggers"] = "trigger-edit-1";
    RequireOk(history.Observe(edited, "修改触发器"));
    auto project = history.BeginSave(EditSaveTarget::Project);
    Require(project.Succeeded() && project.value->Revision() == 1, "project ticket captures edit revision");
    Require(!history.BeginSave(EditSaveTarget::Project).Succeeded(), "overlapping same-target save rejected");
    RequireOk(history.CompleteSave(*project.value, true));
    Require(!history.IsDirty(EditSaveTarget::Project) && history.IsDirty(EditSaveTarget::Map), "project save does not mark map saved");
    Require(history.Baseline(EditSaveTarget::Project)->scopes == edited
        && history.Baseline(EditSaveTarget::Map)->scopes == original, "independent snapshots");
    auto failedMap = history.BeginSave(EditSaveTarget::Map);
    Require(failedMap.Succeeded(), "map ticket");
    RequireOk(history.CompleteSave(*failedMap.value, false));
    Require(history.IsDirty(EditSaveTarget::Map) && history.Baseline(EditSaveTarget::Map)->scopes == original, "failed write preserves map baseline");
    Require(!history.CompleteSave(*failedMap.value, true).Succeeded(), "failed ticket cannot be reused as success");
    auto map = history.BeginSave(EditSaveTarget::Map);
    Require(map.Succeeded(), "retry gets new ticket");
    RequireOk(history.CompleteSave(*map.value, true));
    Require(!history.IsDirty(EditSaveTarget::Map), "verified save commits map only");
    RequireOk(history.Observe(original, "撤销已保存修改", EditOperation::Undo));
    Require(history.IsDirty(EditSaveTarget::Project) && history.IsDirty(EditSaveTarget::Map), "undo before saved content is dirty");
}

void TestSaveInFlight() {
    EditRevision history;
    const auto original = Original();
    RequireOk(history.Reset(original, original, original));
    auto first = original;
    first["triggers"] = "first";
    RequireOk(history.Observe(first, "第一项修改"));
    auto project = history.BeginSave(EditSaveTarget::Project);
    Require(project.Succeeded(), "snapshot before background write");
    auto second = first;
    second["objects/unit"] = "second";
    RequireOk(history.Observe(second, "保存期间编辑"));
    auto map = history.BeginSave(EditSaveTarget::Map);
    Require(map.Succeeded() && map.value->Revision() == 2, "independent target can capture later revision");
    RequireOk(history.CompleteSave(*map.value, true));
    RequireOk(history.CompleteSave(*project.value, true));
    Require(history.IsDirty(EditSaveTarget::Project) && !history.IsDirty(EditSaveTarget::Map), "late project success cannot clear later edit");
    Require(history.Baseline(EditSaveTarget::Project)->revision == 1
        && history.Baseline(EditSaveTarget::Map)->revision == 2, "completion order does not change captured revision");
    Require(history.DirtyScopes(EditSaveTarget::Project) == std::vector<std::string>{"objects/unit"}, "late save retains correct scope dirtiness");
    RequireOk(history.Observe(first, "撤销保存期间的编辑", EditOperation::Undo));
    Require(!history.IsDirty(EditSaveTarget::Project) && history.IsDirty(EditSaveTarget::Map), "undo compares content rather than revision number");
}

void TestResetAndRecovery() {
    EditRevision history;
    const auto original = Original();
    RequireOk(history.Reset(original, original, original));
    auto oldSave = history.BeginSave(EditSaveTarget::Project);
    Require(oldSave.Succeeded(), "old session ticket");
    auto recovered = original;
    recovered["triggers"] = "recovered-overlay";
    RequireOk(history.Observe(recovered, "修改"));
    const auto generation = history.Generation();
    RequireOk(history.Reset(recovered, recovered, original));
    Require(history.Generation() == generation + 1 && history.Revision() == 0 && history.Journal().empty(), "restore starts new revision history");
    Require(!history.IsDirty(EditSaveTarget::Project) && history.IsDirty(EditSaveTarget::Map), "restored project baseline separate from embedded original map");
    Require(!history.CompleteSave(*oldSave.value, true).Succeeded(), "old save cannot mutate recovered session");
    auto active = history.BeginSave(EditSaveTarget::Project);
    Require(active.Succeeded(), "reset clears pending save slot");
    RequireOk(history.CompleteSave(*active.value, false));
    RequireOk(history.Reset());
    Require(history.Current().empty() && !history.IsDirty(EditSaveTarget::Project)
        && !history.IsDirty(EditSaveTarget::Map) && !history.Baseline(EditSaveTarget::Project), "closed empty session");
    RequireOk(history.Reset(recovered));
    Require(history.IsDirty(EditSaveTarget::Project) && history.IsDirty(EditSaveTarget::Map), "new nonempty document has no saved baseline");
}

void TestValidationAndTicketIsolation() {
    EditRevision history;
    const auto original = Original();
    RequireOk(history.Reset(original, original, original));
    auto save = history.BeginSave(EditSaveTarget::Project);
    Require(save.Succeeded(), "save before invalid operation");
    const auto generation = history.Generation();
    auto invalid = original;
    invalid["triggers"] = "";
    Require(!history.Observe(invalid, "未知指纹").Succeeded(), "unknown fingerprint rejected");
    Require(!history.Reset(original, invalid, original).Succeeded(), "invalid saved baseline rejected");
    Require(history.Generation() == generation && history.Current() == original && history.Revision() == 0, "validation failure leaves whole session intact");
    RequireOk(history.CompleteSave(*save.value, true));
    Require(!history.CompleteSave(*save.value, true).Succeeded(), "ticket completion is single use");
    invalid = original;
    invalid[""] = "hash";
    Require(!history.Observe(invalid, "无身份").Succeeded(), "empty scope rejected");
    invalid.erase("");
    invalid[std::string("scope\0alias", 11)] = "hash";
    Require(!history.Observe(invalid, "嵌入零字节身份").Succeeded(), "embedded nul scope rejected");
    const auto invalidTarget = static_cast<EditSaveTarget>(99);
    Require(!history.BeginSave(invalidTarget).Succeeded() && history.IsDirty(invalidTarget), "invalid target fails closed");
    Require(!history.Observe(original, "invalid", static_cast<EditOperation>(99)).Succeeded(), "invalid operation rejected");
    EditRevision other;
    RequireOk(other.Reset(original, original, original));
    auto first = history.BeginSave(EditSaveTarget::Map);
    auto second = other.BeginSave(EditSaveTarget::Map);
    Require(first.Succeeded() && second.Succeeded(), "two sessions capture equal content");
    Require(!other.CompleteSave(*first.value, true).Succeeded(), "foreign service ticket cannot complete matching pending save");
    RequireOk(other.CompleteSave(*second.value, true));
    RequireOk(history.CompleteSave(*first.value, true));
}

void TestDeterministicEditSaveSequence() {
    EditRevision history;
    auto current = Original();
    std::array<EditScopeSnapshot, 2> saved{current, current};
    std::array<std::optional<EditSaveTicket>, 2> tickets;
    std::array<EditScopeSnapshot, 2> captures;
    RequireOk(history.Reset(current, current, current));
    std::uint32_t seed = 72023;
    std::uint64_t expectedRevision = 0;
    for (int step = 0; step < 300; ++step) {
        seed = seed * 1664525u + 1013904223u;
        const auto targetIndex = (seed >> 8) & 1u;
        const auto target = targetIndex ? EditSaveTarget::Map : EditSaveTarget::Project;
        switch (seed % 5u) {
        case 0: case 1: {
            auto next = current;
            next[targetIndex ? "triggers" : "objects/unit"] = "value-" + std::to_string((seed >> 16) % 7);
            if (next != current) ++expectedRevision;
            RequireOk(history.Observe(next, "确定性编辑"));
            current = std::move(next);
            break;
        }
        case 2:
            if (!tickets[targetIndex]) {
                auto result = history.BeginSave(target);
                Require(result.Succeeded(), "sequence save begins");
                tickets[targetIndex] = *result.value;
                captures[targetIndex] = current;
            }
            break;
        default:
            if (tickets[targetIndex]) {
                const bool succeeded = (seed % 5u) == 3;
                RequireOk(history.CompleteSave(*tickets[targetIndex], succeeded));
                if (succeeded) saved[targetIndex] = captures[targetIndex];
                tickets[targetIndex].reset();
            }
            break;
        }
        Require(history.Current() == current && history.Revision() == expectedRevision, "sequence actual content and revision");
        Require(history.IsDirty(EditSaveTarget::Project) == (current != saved[0])
            && history.IsDirty(EditSaveTarget::Map) == (current != saved[1]), "sequence baselines match independent save model");
    }
}

void TestConcurrentSnapshotReaders() {
    EditRevision history;
    const auto original = Original();
    RequireOk(history.Reset(original, original, original));
    auto ticket = history.BeginSave(EditSaveTarget::Project);
    Require(ticket.Succeeded(), "background completion ticket");
    bool completed = false;
    std::thread writer([&] {
        completed = history.CompleteSave(*ticket.value, true).Succeeded();
    });
    auto changed = original;
    changed["triggers"] = "later-edit";
    const auto observed = history.Observe(changed, "保存期间编辑");
    writer.join();
    RequireOk(observed);
    Require(completed && history.IsDirty(EditSaveTarget::Project), "completion and edit interleaving always retains later content");
}

} // namespace

void TestPreparedObservation() {
    EditRevision history, other;
    const auto original = Original(); RequireOk(history.Reset(original, original, original));
    auto next = original; next["imports/0"] = "owned:sha";
    auto prepared = history.PrepareObservation(history.Generation(), history.Revision(), next, "准备资源");
    Require(prepared.Succeeded() && history.Revision() == 0 && history.Journal().empty() && history.Current() == original,
        "preparing does not publish any logical revision");
    int asset = 0;
    auto moved = std::move(*prepared.value);
    Require(!history.CommitObservation(std::move(*prepared.value), [](void* state) noexcept { *static_cast<int*>(state) = 99; }, &asset).Succeeded()
        && asset == 0, "moved-from token cannot publish");
    Require(!other.CommitObservation(std::move(moved)).Succeeded(), "another owner cannot consume the proof");
    RequireOk(history.CommitObservation(std::move(moved), [](void* state) noexcept { *static_cast<int*>(state) = 1; }, &asset));
    Require(asset == 1 && history.Revision() == 1 && history.Current() == next && history.Journal().size() == 1, "asset and prepared revision publish together");
    Require(!history.CommitObservation(std::move(moved)).Succeeded(), "consumed token cannot publish twice");
    auto stale = history.PrepareObservation(history.Generation(), history.Revision(), original, "旧候选"); Require(stale.Succeeded(), "prepare stale probe");
    auto changed = next; changed["triggers"] = "later"; RequireOk(history.Observe(changed, "其他编辑"));
    Require(!history.CommitObservation(std::move(*stale.value), [](void* state) noexcept { *static_cast<int*>(state) = 99; }, &asset).Succeeded()
        && asset == 1 && history.Current() == changed, "stale candidate leaves assets and revision untouched");
}

int main() {
    try {
        TestContentAndUndo();
        TestScopeCreationAndRemoval();
        TestSeparateBaselines();
        TestSaveInFlight();
        TestResetAndRecovery();
        TestValidationAndTicketIsolation();
        TestDeterministicEditSaveSequence();
        TestConcurrentSnapshotReaders();
        TestPreparedObservation();
        std::cout << "EditRevision tests passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "EditRevision tests failed: " << exception.what() << '\n';
        return 1;
    }
}
