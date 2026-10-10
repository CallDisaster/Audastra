#include "EditorProject.h"
#include "ProjectQuerySources.h"

#include "War3MapCore/MapDocument.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/ObjectData/LniObjectCodec.h"
#include "War3MapCore/ObjectData/NativeObjectCodec.h"

#include <QElapsedTimer>
#include <QUndoStack>
#include <algorithm>

// 工程级引用索引缓存：物体管理、物编静态引用与删除前检查共用。
// 文档替换或任一撤销栈变化后，下次查询时重建；索引本身在 Services 层，不依赖 Qt。
namespace War3Map::QtProject {

std::uint64_t EditorProject::ContentCacheEpoch() const {
    auto* owner = const_cast<EditorProject*>(this);
    const auto generation = editRevision_.Generation(), revision = editRevision_.Revision();
    if (contentCacheEditGeneration_ != generation || contentCacheEditRevision_ != revision) {
        contentCacheEditGeneration_ = generation; contentCacheEditRevision_ = revision;
        ++contentCacheEpoch_;
    }
    std::erase_if(contentCacheStacks_, [](const auto& stack) { return stack.isNull(); });
    const auto subscribe = [&](QUndoStack* stack) {
        if (!stack || std::any_of(contentCacheStacks_.begin(), contentCacheStacks_.end(),
            [stack](const auto& existing) { return existing.data() == stack; })) return;
        contentCacheStacks_.emplace_back(stack);
        ++contentCacheEpoch_;
        QObject::connect(stack, &QUndoStack::indexChanged, owner, [owner] {
            ++owner->contentCacheEpoch_;
        });
    };
    subscribe(triggerUndoStack_); subscribe(mapDataUndoStack_);
    for (auto* stack : objectUndoStacks_) subscribe(stack);
    return contentCacheEpoch_;
}

std::string EditorProject::ReferenceStamp() const {
    std::string stamp = std::to_string(reinterpret_cast<std::uintptr_t>(triggerDocument_.get())) + '|' +
        std::to_string(reinterpret_cast<std::uintptr_t>(uiCatalog_.get())) + '|' +
        std::to_string(reinterpret_cast<std::uintptr_t>(mapDocument_.get())) + '|' +
        std::to_string(reinterpret_cast<std::uintptr_t>(objectFieldSchema_.get()));
    for (const auto& document : objectDocuments_) {
        stamp += '|' + std::to_string(reinterpret_cast<std::uintptr_t>(document.get()));
    }
    for (const auto& document : nativeObjectDocuments_) {
        stamp += '|' + std::to_string(reinterpret_cast<std::uintptr_t>(document.get()));
    }
    stamp += '|' + std::to_string(reinterpret_cast<std::uintptr_t>(mapEdits_.get())) + ':' +
        std::to_string(mapEdits_ ? mapEdits_->Revision() : 0);
    const auto add = [&stamp](const QUndoStack* stack) {
        stamp += stack == nullptr ? std::string("|-") : '|' + std::to_string(stack->index()) + ':' + std::to_string(stack->count());
    };
    add(triggerUndoStack_);
    for (const auto* stack : objectUndoStacks_) add(stack);
    return stamp + '|' + std::to_string(ContentCacheEpoch());
}

const Services::ProjectReferenceIndex& EditorProject::References() const {
    const std::string stamp = ReferenceStamp();
    if (referenceIndex_ && stamp == referenceStamp_) return *referenceIndex_;
    QElapsedTimer timer;
    timer.start();
    std::vector<ObjectData::ObjectRecordSource> sources;
    for (const auto& source : CaptureProjectObjectSources(*this)) {
        using InputState = ObjectData::ReferenceInputState;
        InputState state = InputState::Unreadable;
        switch (source.state) {
        case Services::ProjectSearchState::Ready: state = InputState::Ready; break;
        case Services::ProjectSearchState::Partial: state = InputState::Partial; break;
        case Services::ProjectSearchState::Missing: state = InputState::Missing; break;
        case Services::ProjectSearchState::Invalid: state = InputState::Invalid; break;
        case Services::ProjectSearchState::Ambiguous: state = InputState::Ambiguous; break;
        default: break;
        }
        const std::string member = source.origin == Services::ProjectSearchObjectOrigin::Native
            ? std::string(ObjectData::NativeFileName(source.type))
            : "table/" + std::string(ObjectData::LniFileName(source.type));
        sources.push_back({source.type, member, state, source.records});
    }
    // 索引构建即复制所需引用；当前 WTS、放置实例等 overlay 不借用到缓存中。
    const auto currentMap = mapEdits_ ? std::optional<MapDocument>(mapEdits_->Snapshot()) : std::nullopt;
    Services::ProjectReferenceInputs inputs;
    inputs.triggers = triggerDocument_.get();
    inputs.catalog = uiCatalog_.get();
    inputs.objects = sources;
    inputs.map = currentMap ? &*currentMap : mapDocument_.get();
    inputs.schema = objectFieldSchema_.get();
    referenceIndex_ = std::make_unique<Services::ProjectReferenceIndex>(Services::ProjectReferenceIndex::Build(inputs));
    referenceStamp_ = stamp;
    referenceBuildMs_ = timer.elapsed();
    ++referenceGeneration_;
    return *referenceIndex_;
}

} // namespace War3Map::QtProject
