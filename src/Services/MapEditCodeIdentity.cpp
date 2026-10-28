#include "MapEditCodeIdentityInternal.h"
#include "War3MapCore/Services/MapObjectReferenceCapture.h"
#include "War3MapCore/Services/AuxiliaryEditing.h"
#include "War3MapCore/Services/AuxiliaryTransaction.h"
#include "War3MapCore/Trigger/TriggerCodec.h"
#include "War3MapCore/MapData/MapSettings.h"
#include <array>

namespace War3Map::Services {
Result<MapEditSaveReceipt> SaveTriggerCodeIdentityCandidate(const MapEditSaveRequest& request,
    const MapSaveService::Progress& progress) {
    using R = Result<MapEditSaveReceipt>;
    const auto fail = [](std::string code, std::string text) { return R::Failure("Trigger.CodeIdentity", "TRIGGER_CODE_" + code, std::move(text)); };
    if (!request.triggerCodeIdentity || request.triggerEdits || request.triggerTransaction || request.settingsCustomEdits
        || !request.fieldEdits.empty() || !request.terrainStrokes.empty() || !request.objectTransactions.empty()
        || request.auxiliaryTransaction || !std::holds_alternative<std::monostate>(request.placementEdit) || request.rebuildTerrainShadow)
        return fail("REQUEST_INVALID", "代码身份保存只接一个独立领域请求，不能混入其它地图修改。");
    if (request.luaTarget) return fail("LUA_UNSUPPORTED", "所选Lua保存目标不支持自定义代码触发器身份编辑。");
    auto opened = MapArchiveSession::Open(request.sourceMap); if (!opened.Succeeded()) return {{}, opened.diagnostics};
    auto& source = *opened.value;
    if (source.SourceSha256() != request.expectedSourceSha256) return fail("SOURCE_STALE", "实际地图来源SHA与请求不一致。");
    auto catalog = ResolveMapEditCatalog(request.uiRoot.empty() ? request.toolRoot : request.uiRoot, request.effectiveLayers);
    if (!catalog.Succeeded()) return {{}, catalog.diagnostics};
    const std::array<std::string, 3> names{"war3map.wtg", "war3map.wct", "war3map.w3i"};
    auto pair = source.CaptureExactMembers(names); if (!pair.Succeeded()) return {{}, pair.diagnostics};
    for (const auto& member : *pair.value) if (!member.present) return fail("SOURCE_STALE", "代码身份保存需要完整WTG/WCT/W3I。");
    auto settings = MapData::MapSettings::Parse((*pair.value)[2].bytes); if (!settings.Succeeded()) return {{}, settings.diagnostics};
    if (settings.value->scriptLanguage != 0) return fail("LUA_UNSUPPORTED", "实际地图语言不是已知JASS目标。");
    auto document = Trigger::WtgWctCodec{}.Parse((*pair.value)[0].bytes, (*pair.value)[1].bytes, *catalog.value);
    if (!document.Succeeded()) return {{}, document.diagnostics};
    auto objects = CaptureArchiveMapObjectReferences(source, request.gameRoot); if (!objects.Succeeded()) return {{}, objects.diagnostics};
    TriggerCodeIdentityContext context; context.document = &*document.value; context.catalog = &*catalog.value;
    context.mapObjects = (*objects.value)->References().empty() ? nullptr : objects.value->get();
    context.target = TriggerCodeScriptTarget::Jass; context.completeScriptSources = !source.HasOpaqueMembers();
    AuxiliaryReferenceContext references;
    for (const auto& member : source.Members()) {
        if (member.name == "war3map.lua") context.target = TriggerCodeScriptTarget::Lua;
        ObserveAuxiliaryScriptSource(references, member.name);
    }
    context.opaqueScriptSources = std::move(references.opaqueScriptSources);
    context.verify = [&source] { return source.VerifySourceUnchanged(); };
    auto plan = TriggerCodeIdentityEditing::Prepare(context, request.triggerCodeIdentity->request);
    if (!plan.Succeeded()) return {{}, plan.diagnostics};
    const auto applied = TriggerCodeIdentityEditing::Apply(context, *document.value, *plan.value);
    if (!applied.Succeeded()) return {{}, applied.diagnostics};
    if (request.triggerCodeIdentity->dryRun || !plan.value->Summary().changed) {
        const auto verified = source.VerifySourceUnchanged(); if (!verified.Succeeded()) return {{}, verified.diagnostics};
        MapEditSaveReceipt receipt; receipt.sourceSha256 = source.SourceSha256(); receipt.previewOnly = true;
        receipt.codeIdentity = plan.value->Summary(); receipt.codeIdentityGraph = *applied.value;
        return R::Success(std::move(receipt));
    }
    Trigger::TriggerTransactionBinding binding{source.SourceSha256(), 1, {1, source.SourceSha256(), catalog.value->Sha256()}, 1};
    auto transaction = Trigger::TriggerTransactionSnapshot::Freeze(*document.value, *catalog.value, (*pair.value)[0].bytes, (*pair.value)[1].bytes, binding);
    if (!transaction.Succeeded()) return {{}, transaction.diagnostics};
    auto inner = request; inner.triggerCodeIdentity.reset(); inner.triggerTransaction = *transaction.value;
    auto saved = SaveMapEditCandidate(inner, progress);
    if (saved.Succeeded()) {
        saved.value->codeIdentity = plan.value->Summary();
        saved.value->codeIdentityGraph = *applied.value;
    }
    return saved;
}
}
