#include "WorldAnimationDiagnostics.h"
#include "War3MapCore/Render/ModelAnimation.h"
#include "War3MapCore/Services/WorldInstanceRegistry.h"
#include "WorldSelectionProperties.h"

#include <QObject>
#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QPointer>
#include <QSaveFile>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace War3Map::Modules {
bool WorldDiagnosticPlacementMatches(const MapData::PlacementId& owner, std::size_t request,
    const Render::AnimationInstanceSpec& spec, const Render::AnimationInstanceState& state,
    const Services::WorldInstanceRegistry* registry) {
    if (spec.identity != state.identity) return false;
    if (!registry) return spec.hostKey == WorldPlacementHostKey(owner);
    const auto identity = registry->Find(owner);
    const auto& captured = registry->CapturedSpecs();
    return identity && request < captured.size() && captured[request].identity == *identity
        && spec.identity == *identity && spec.hostKey == captured[request].hostKey;
}
namespace {
constexpr auto kNoRequest = std::numeric_limits<std::uint32_t>::max();
enum class Stage { Model, Pose, PoseWarning, Geoset, Material, Emitter, Particle, Scene };

QString Text(std::string_view text, std::size_t maximum = 512) {
    const auto bytes = std::min(text.size(), maximum);
    auto result = QString::fromUtf8(text.data(), static_cast<qsizetype>(bytes));
    if (bytes < text.size()) result += QObject::tr("（文本截断）");
    return result;
}
QString StageName(Stage stage) {
    switch (stage) {
    case Stage::Model: return QObject::tr("模型不可用");
    case Stage::Pose: return QObject::tr("本帧隐藏");
    case Stage::PoseWarning: return QObject::tr("兼容预览警告");
    case Stage::Geoset: return QObject::tr("网格隔离");
    case Stage::Material: return QObject::tr("材质隔离");
    case Stage::Emitter: return QObject::tr("发射器跳过");
    case Stage::Particle: return QObject::tr("粒子帧诊断");
    case Stage::Scene: return QObject::tr("场景警告");
    }
    return {};
}

struct Key {
    Stage stage = Stage::Scene;
    std::string path, layer, digest, kind, code, subsystem, unboundReason;
    std::optional<std::uint32_t> component, layerIndex, node;
    std::optional<std::uint64_t> byteOffset;
    std::optional<std::uint32_t> warningTrack, warningSequence;
    std::optional<std::int64_t> warningLeftFrame, warningRightFrame;
    std::optional<std::uint64_t> warningElapsedMs, warningGlobalMs;
    bool warningCachedStand = false;
    std::uint32_t unknownRequest = kNoRequest;
    bool sourceKnown = false;
    // 运行结果不同不可被同一 code 抹平（例如未模拟与已模拟但部分支持）。
    unsigned state = 0;
    auto Fields() const {
        return std::tie(stage, path, layer, digest, kind, code, subsystem, unboundReason, component,
            layerIndex, node, byteOffset, warningTrack, warningSequence, warningLeftFrame, warningRightFrame,
            warningElapsedMs, warningGlobalMs, warningCachedStand, unknownRequest, sourceKnown, state);
    }
    bool operator<(const Key& other) const { return Fields() < other.Fields(); }
};
struct Group {
    std::string message;
    bool multipleMessages = false;
    std::size_t records = 0;
    std::vector<std::uint32_t> requests;
};
using Groups = std::map<Key, Group>;
void Add(Groups& groups, Key key, std::uint32_t request, std::string_view message, std::size_t recordCount = 1) {
    auto [found, inserted] = groups.try_emplace(std::move(key));
    auto& group = found->second;
    if (inserted) group.message = message;
    else if (group.message != message) group.multipleMessages = true;
    group.records += recordCount;
    if (request != kNoRequest) group.requests.push_back(request);
}
void UniqueRequests(Groups& groups) {
    for (auto& [key, group] : groups) {
        std::sort(group.requests.begin(), group.requests.end());
        group.requests.erase(std::unique(group.requests.begin(), group.requests.end()), group.requests.end());
    }
}
void MarkNode(Key& key, std::uint32_t node) {
    if (node != War3RenderCore::NoIndex) key.node = node;
}

QString WarningTrackName(War3RenderCore::TrackKind track) {
    switch (track) {
    case War3RenderCore::TrackKind::Translation: return QStringLiteral("TRS/Translation（位移）");
    case War3RenderCore::TrackKind::Rotation: return QStringLiteral("TRS/Rotation（旋转）");
    case War3RenderCore::TrackKind::Scale: return QStringLiteral("TRS/Scale（缩放）");
    case War3RenderCore::TrackKind::MaterialAlpha: return QStringLiteral("MaterialAlpha");
    case War3RenderCore::TrackKind::MaterialTexture: return QStringLiteral("MaterialTexture");
    case War3RenderCore::TrackKind::None: return QObject::tr("轨道未知");
    }
    return QObject::tr("轨道未知（%1）").arg(static_cast<std::uint32_t>(track));
}
void MarkWarningClock(Key& key, const Render::AnimationPoseWarningView& view) {
    key.warningSequence = view.sequenceIndex;
    key.warningElapsedMs = view.elapsedMs;
    key.warningGlobalMs = view.globalMs;
    key.warningCachedStand = view.cachedStand;
}
QString WarningClock(const Render::AnimationPoseWarningView& view) {
    return QObject::tr("%1 · 动作索引 %2 · 实际局部/全局时钟 %3/%4 ms")
        .arg(view.cachedStand ? QObject::tr("缓存 Stand 采样") : QObject::tr("当前动态采样"))
        .arg(view.sequenceIndex).arg(view.elapsedMs).arg(view.globalMs);
}
std::string WarningReason(const War3RenderCore::PoseDiagnostic& diagnostic,
    const Render::AnimationPoseWarningView& view, const War3RenderCore::MaterialAlphaPreviewWarning& material) {
    if (material.materialIndex != War3RenderCore::NoIndex)
        return QObject::tr("相同透明键重复帧的兼容预览，未确认原版等价 · 材质 %1/层 %2 · KMTA 原帧 %3 · 原键字节 %4/%5 · 值位 %6 · %7")
            .arg(material.materialIndex).arg(material.layerIndex).arg(material.sourceFrame)
            .arg(material.leftSourceByteOffset).arg(material.rightSourceByteOffset)
            .arg(material.sourceValueBits, 8, 16, QLatin1Char('0')).arg(WarningClock(view)).toStdString();
    const auto node = diagnostic.objectId == War3RenderCore::NoIndex ? QObject::tr("节点未知")
        : QObject::tr("节点 %1").arg(diagnostic.objectId);
    return QObject::tr("兼容近似预览，未确认原版等价 · %1 · %2 · 原帧 %3 → %4 · %5 · %6")
        .arg(node, WarningTrackName(diagnostic.track))
        .arg(diagnostic.leftFrame).arg(diagnostic.rightFrame)
        .arg(WarningClock(view), Text(War3RenderCore::DescribeDiagnostic(diagnostic), 1024)).toStdString();
}

QString Row(const Key& key, const Group& group, const Render::AnimatedModelScene& scene) {
    auto component = Text(key.kind, 32);
    if (key.component) component += QObject::tr("[%1]").arg(*key.component);
    if (key.layerIndex) component += QObject::tr("/层[%1]").arg(*key.layerIndex);
    if (key.node) component += QObject::tr(" 节点 %1").arg(*key.node);
    if (key.stage == Stage::Particle) {
        component += (key.state & 1u) ? QObject::tr(" 已模拟") : QObject::tr(" 未模拟");
        if (key.state & 2u) component += QObject::tr(" 隐藏");
        if (key.state & 4u) component += QObject::tr(" 部分支持");
        if (key.state & 8u) component += QObject::tr(" 预算省略");
        if (key.state & 16u) component += QObject::tr(" 视野外");
        if (key.state & 32u) component += QObject::tr(" 冻结");
    }
    const auto location = key.byteOffset ? QObject::tr("字节偏移 %1").arg(*key.byteOffset)
        : QObject::tr("字节偏移未知");
    const auto affected = group.requests.empty() ? QObject::tr("实例归属未知")
        : QObject::tr("%1 个摆放").arg(group.requests.size());
    auto text = QObject::tr("%1 %2 · %3 · %4 · %5，%6 条记录 · %7")
        .arg(StageName(key.stage), component,
            key.path.empty() ? QObject::tr("模型路径未知") : Text(key.path), Text(key.code, 128), affected)
        .arg(group.records).arg(location);
    if (key.sourceKnown) {
        text += QObject::tr("\n来源 %1 · SHA %2").arg(Text(key.layer), Text(key.digest, 128));
    } else if (key.stage == Stage::Scene) {
        text += QObject::tr("\n实际模型来源未知");
        if (!key.digest.empty()) text += QObject::tr(" · 原诊断指纹 %1").arg(Text(key.digest, 128));
    } else {
        text += QObject::tr("\n来源/SHA未知，未跨实例合并");
    }
    QStringList samples;
    for (std::size_t i = 0; i < std::min(group.requests.size(), std::size_t{3}); ++i) {
        const auto request = group.requests[i];
        if (request >= scene.specs.size()) { samples << QObject::tr("请求 %1（身份未知）").arg(request); continue; }
        const auto& spec = scene.specs[request];
        samples << QObject::tr("%1（实例 %2/%3）").arg(Text(spec.hostKey, 160))
            .arg(spec.identity.value).arg(spec.identity.generation);
    }
    if (!samples.empty()) text += QObject::tr(" · 示例 %1").arg(samples.join(QStringLiteral("、")));
    if (!group.message.empty()) text += QStringLiteral("\n") + Text(group.message);
    if (group.multipleMessages) text += QObject::tr("（原因文本有差异，此处为首个样本）");
    return text;
}
} // namespace

struct WorldAnimationDiagnostics::State {
    std::shared_ptr<const Render::AnimatedModelScene> scene;
    std::shared_ptr<Render::AnimationDiagnosticQuery> query;
    std::vector<std::shared_ptr<const War3RenderCore::AssetSource>> sources;
    Groups fixed;
    QStringList failures;

    Key Instance(Stage stage, std::uint32_t request, std::string_view kind, std::string_view code) const {
        Key key; key.stage = stage; key.kind = kind; key.code = code;
        if (request < sources.size() && sources[request]) {
            const auto& source = *sources[request];
            key.path = source.virtualPath; key.layer = source.sourceLayer; key.digest = source.contentDigest;
            key.sourceKnown = true;
        } else {
            if (request < scene->specs.size()) key.path = scene->specs[request].model.modelPath;
            key.unknownRequest = request;
        }
        return key;
    }
    void Bind(std::shared_ptr<const Render::AnimatedModelScene> value) {
        scene = std::move(value); query.reset(); sources.clear(); fixed.clear(); failures.clear();
        if (!scene) return;
        const auto created = Render::AnimationDiagnosticQuery::Create(scene);
        if (created.Succeeded()) query = *created.value;
        else for (const auto& failure : created.diagnostics)
            failures << QObject::tr("诊断来源索引不可用：%1 · %2").arg(Text(failure.code), Text(failure.message));
        sources.resize(scene->specs.size());
        std::map<std::tuple<std::string, std::string, std::string>,
            std::shared_ptr<const War3RenderCore::AssetSource>> interned;
        if (query) for (std::uint32_t request = 0; request < scene->specs.size(); ++request) {
            const auto source = query->Source(request, scene->specs[request].identity);
            if (!source.Succeeded() || !*source.value) continue;
            const auto& actual = **source.value;
            if (actual.virtualPath.empty() || actual.sourceLayer.empty() || actual.contentDigest.size() != 64
                || !std::all_of(actual.contentDigest.begin(), actual.contentDigest.end(), [](unsigned char c) {
                    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                })) continue;
            const auto identity = std::make_tuple(actual.virtualPath, actual.sourceLayer, actual.contentDigest);
            auto [entry, inserted] = interned.try_emplace(identity);
            if (inserted) entry->second = std::make_shared<const War3RenderCore::AssetSource>(actual);
            sources[request] = entry->second;
        }
        for (const auto& rejected : scene->unavailableInstances)
            Add(fixed, Instance(Stage::Model, rejected.requestIndex, "model", rejected.code), rejected.requestIndex, {});
        for (std::uint32_t request = 0; request < scene->particleCapabilities.size(); ++request) {
            const auto& issues = scene->particleCapabilities[request].skippedEmitters;
            if (!issues) continue;
            for (const auto& issue : *issues) {
                auto key = Instance(Stage::Emitter, request, issue.kind, issue.code);
                key.component = issue.sourceIndex; key.node = issue.nodeId;
                Add(fixed, std::move(key), request, issue.message);
            }
        }
        for (const auto& warning : scene->geometry.warnings) {
            Key key; key.stage = Stage::Scene; key.code = warning.code; key.subsystem = warning.subsystem;
            // 没有实例/组件绑定的场景警告不能只凭同一 code 合并不同原因。
            key.unboundReason = warning.message;
            if (warning.origin) {
                const auto path = warning.origin->source.generic_u8string();
                key.path.assign(reinterpret_cast<const char*>(path.data()), path.size());
                key.digest = warning.origin->fingerprint;
                key.byteOffset = warning.origin->offset;
            }
            Add(fixed, std::move(key), kNoRequest, warning.message);
        }
        UniqueRequests(fixed);
    }
};

WorldAnimationDiagnostics::WorldAnimationDiagnostics() : state_(std::make_unique<State>()) {}
WorldAnimationDiagnostics::~WorldAnimationDiagnostics() = default;
void WorldAnimationDiagnostics::Clear() { state_->Bind({}); }

WorldAnimationDiagnosticSummary WorldAnimationDiagnostics::Summarize(
    std::shared_ptr<const Render::AnimatedModelScene> scene, const Render::AnimationFrame& frame, std::size_t rowLimit) {
    WorldAnimationDiagnosticSummary result;
    if (scene != state_->scene) state_->Bind(std::move(scene));
    if (!state_->scene || frame.sceneVersion != state_->scene->sceneVersion
        || frame.documentRevision != state_->scene->documentRevision) {
        result.rows << QObject::tr("诊断帧与当前场景不匹配，未显示旧场景的聚合结果。");
        return result;
    }
    const auto& current = *state_->scene;
    Groups dynamic;
    bool hasPoseWarnings = false;
    for (const auto& pose : frame.poses) {
        const auto warnings = Render::ReadAnimationPoseWarnings(current, frame, pose);
        if (!warnings.Succeeded()) {
            hasPoseWarnings = true;
            // 失败的owner不能继续借用pose.requestIndex给任何摆放授予警告归属。
            Key key; key.stage = Stage::Scene; key.code = "ANIMATION_POSE_WARNING_BINDING_INVALID";
            key.unboundReason = "实例警告绑定无效，未使用记录。";
            for (const auto& failure : warnings.diagnostics)
                key.unboundReason += " " + failure.code + ": " + failure.message;
            const auto reason = key.unboundReason;
            Add(dynamic, std::move(key), kNoRequest, reason);
        } else {
            const auto& view = *warnings.value;
            hasPoseWarnings = hasPoseWarnings || !view.records.empty() || view.omitted != 0;
            for (std::size_t warningIndex = 0; warningIndex < view.records.size(); ++warningIndex) {
                const auto& warning = view.records[warningIndex];
                const auto& material = view.materialRecords[warningIndex];
                auto key = state_->Instance(Stage::PoseWarning, pose.requestIndex, "pose-preview",
                    War3RenderCore::CodeName(warning.code));
                MarkNode(key, warning.objectId); MarkWarningClock(key, view);
                key.warningTrack = static_cast<std::uint32_t>(warning.track);
                key.warningLeftFrame = warning.leftFrame; key.warningRightFrame = warning.rightFrame;
                if (material.materialIndex != War3RenderCore::NoIndex) {
                    key.kind = "material-alpha-preview";
                    key.component = material.materialIndex; key.layerIndex = material.layerIndex;
                    key.byteOffset = material.leftSourceByteOffset;
                }
                Add(dynamic, std::move(key), pose.requestIndex, WarningReason(warning, view, material));
            }
            if (view.omitted) {
                auto key = state_->Instance(Stage::PoseWarning, pose.requestIndex, "pose-preview",
                    "ANIMATION_POSE_WARNINGS_OMITTED");
                MarkWarningClock(key, view);
                // 每个实例独立保留精确省略数，不聚合算术溢出或猜未保留记录的节点/轨道。
                key.unknownRequest = pose.requestIndex;
                const auto reason = QObject::tr("%1 · 此次实际采样保留 %2 条警告，另有 %3 条因每实例 %4 条上限未保留；未保留警告的具体节点、轨道和原帧未知。")
                    .arg(WarningClock(view)).arg(view.records.size()).arg(view.omitted)
                    .arg(Render::AnimationPoseWarnings::Capacity).toStdString();
                Add(dynamic, std::move(key), pose.requestIndex, reason, 0);
            }
        }
        if (pose.hidden) {
            const auto code = pose.rejectionCode.empty() ? War3RenderCore::CodeName(pose.rejection.code) : pose.rejectionCode;
            auto key = state_->Instance(Stage::Pose, pose.requestIndex, "pose", code);
            if (pose.rejectionCode.empty()) MarkNode(key, pose.rejection.objectId);
            Add(dynamic, std::move(key), pose.requestIndex, pose.rejectionCode.empty()
                ? War3RenderCore::DescribeDiagnostic(pose.rejection) : pose.rejectionMessage);
        }
        const auto geosetEnd = static_cast<std::uint64_t>(pose.geosetIssues.offset) + pose.geosetIssues.count;
        const auto materialEnd = static_cast<std::uint64_t>(pose.materialIssues.offset) + pose.materialIssues.count;
        if (geosetEnd > frame.geosetIssues.size() || materialEnd > frame.materialIssues.size()) {
            result.rows << QObject::tr("实例 %1 的诊断范围无效，未使用越界记录。").arg(pose.requestIndex);
            continue;
        }
        for (std::size_t i = pose.geosetIssues.offset; i < geosetEnd; ++i) {
            const auto& issue = frame.geosetIssues[i];
            auto key = state_->Instance(Stage::Geoset, pose.requestIndex, "GEOA", War3RenderCore::CodeName(issue.diagnostic.code));
            key.component = issue.geosetIndex;
            Add(dynamic, std::move(key), pose.requestIndex, War3RenderCore::DescribeDiagnostic(issue.diagnostic));
        }
        for (std::size_t i = pose.materialIssues.offset; i < materialEnd; ++i) {
            const auto& issue = frame.materialIssues[i].source;
            auto key = state_->Instance(Stage::Material, pose.requestIndex, "KMTA", War3RenderCore::CodeName(issue.error));
            key.component = issue.materialIndex; key.layerIndex = issue.layerIndex; key.byteOffset = issue.sourceByteOffset;
            Add(dynamic, std::move(key), pose.requestIndex, War3RenderCore::CodeMessage(issue.error));
        }
    }
    for (const auto& particle : frame.particleLinks) {
        if (particle.rejectionCode.empty()) continue;
        auto key = state_->Instance(Stage::Particle, particle.requestIndex, "particle", particle.rejectionCode);
        key.state = static_cast<unsigned>(particle.simulated) | (static_cast<unsigned>(particle.hidden) << 1)
            | (static_cast<unsigned>(particle.partial) << 2) | (static_cast<unsigned>(particle.budgetLimited) << 3)
            | (static_cast<unsigned>(particle.outsideView) << 4) | (static_cast<unsigned>(particle.frozen) << 5);
        auto message = particle.rejectionMessage;
        if (particle.hidden) message += " (local/global ms=" + std::to_string(particle.rejectedElapsedMs)
            + "/" + std::to_string(particle.rejectedGlobalMs) + ")";
        Add(dynamic, std::move(key), particle.requestIndex, message);
    }
    UniqueRequests(dynamic);
    result.rows.append(state_->failures);
    rowLimit = std::min(rowLimit, std::size_t{24});
    auto fixed = state_->fixed.cbegin(); auto moving = dynamic.cbegin();
    std::size_t shown = 0;
    while (fixed != state_->fixed.cend() || moving != dynamic.cend()) {
        const auto fromFixed = moving == dynamic.cend() || (fixed != state_->fixed.cend() && fixed->first < moving->first);
        const auto& [key, group] = fromFixed ? *fixed++ : *moving++;
        ++result.groups; result.records += group.records;
        WorldAnimationDiagnosticGroup details;
        details.modelPath = Text(key.path, 4096);
        if (details.modelPath.isEmpty() && group.requests.size() == 1 && group.requests.front() < current.specs.size())
            details.modelPath = Text(current.specs[group.requests.front()].model.modelPath, 4096);
        details.stage = StageName(key.stage); details.code = Text(key.code);
        details.reason = group.message.empty() ? details.stage : Text(group.message, 2048);
        if (group.multipleMessages) details.reason += QObject::tr("（同一原因含不同原始说明，逐实例诊断保留）");
        details.sourceLayer = Text(key.layer, 4096); details.sourceSha256 = Text(key.digest);
        details.sourceKnown = key.sourceKnown; details.records = group.records; details.requests = group.requests;
        result.modelGroups.push_back(std::move(details));
        if (shown++ >= rowLimit) continue;
        result.rows << Row(key, group, current);
        result.representedRecords += group.records;
    }
    if (result.groups) {
        result.rows.prepend(QObject::tr("%1 条诊断按实际模型来源、SHA及组件原因合为 %2 组；每行摆放数独立计数，不能相加。")
            .arg(result.records).arg(result.groups));
        if (result.groups > rowLimit) result.rows << QObject::tr("另有 %1 组未展开，包含 %2 条原始记录。")
            .arg(result.groups - rowLimit).arg(result.records - result.representedRecords);
        result.rows << (hasPoseWarnings
            ? QObject::tr("已保留的诊断可逐实例分页查询；成功姿势警告每实例最多保留 8 条，未保留数量另列。兼容预览警告不表示采样失败、地图错误或已确认原版等价。索引从 0 开始，未记录的来源与字节偏移显示未知。")
            : QObject::tr("完整原始诊断仍保留；选中实例可查看原因，现有逐实例诊断查询支持分页。索引从 0 开始，未记录的来源与字节偏移显示未知。"));
    }
    return result;
}

namespace {
QString PlainCell(QString value) {
    value.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    value.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    value.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return value;
}
}
QString WorldModelDiagnosticText(const std::vector<WorldModelDiagnosticRow>& rows) {
    QStringList lines{QObject::tr("模型路径\t所属单位/装饰物\t原因代码\t原因\t摆放数量\t原始记录\t实际来源\t占位状态")};
    for (const auto& row : rows) {
        lines << QStringList{PlainCell(row.modelPath), PlainCell(row.ownerText), PlainCell(row.reasonCode),
            PlainCell(row.reason), QString::number(row.owners.size()), QString::number(row.records),
            PlainCell(row.sourceText), PlainCell(row.placeholderState)}.join(QLatin1Char('\t'));
        for (const auto& id : row.owners) lines << QObject::tr("  定位身份：%1 · 行 %2 · %3 · 创建编号 %4")
            .arg(id.file == MapData::PlacementFile::Units ? QStringLiteral("war3mapUnits.doo") : QStringLiteral("war3map.doo"))
            .arg(id.index).arg(PlainCell(QString::fromLatin1(id.rawcode.data(), static_cast<qsizetype>(id.rawcode.size())))).arg(id.creationId);
    }
    lines << QObject::tr("仅为冻结时的模型/预览诊断；各行摆放可重叠，不应相加。无来源证据标为未知，不代表地图缺资源已确认。");
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}
Status ExportWorldModelDiagnostics(const std::vector<WorldModelDiagnosticRow>& rows, const QString& path) {
    if (path.isEmpty() || QFileInfo(path).suffix().compare(QStringLiteral("txt"), Qt::CaseInsensitive) != 0)
        return Status::Failure("World.Diagnostics", "WORLD_DIAGNOSTIC_EXPORT_PATH", "诊断导出目标须为文本文件（.txt）。");
    QSaveFile file(path); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
        return Status::Failure("World.Diagnostics", "WORLD_DIAGNOSTIC_EXPORT_OPEN", file.errorString().toStdString());
    const auto text = WorldModelDiagnosticText(rows).toUtf8();
    if (file.write(text) != text.size() || !file.commit())
        return Status::Failure("World.Diagnostics", "WORLD_DIAGNOSTIC_EXPORT_WRITE", file.errorString().toStdString());
    return Status{};
}
WorldModelDiagnosticsDialog::WorldModelDiagnosticsDialog(QWidget* parent) : QDialog(parent) {
    setObjectName(QStringLiteral("worldModelDiagnosticsDialog"));
    setWindowTitle(tr("按模型列出诊断")); resize(1080, 640);
    auto* layout = new QVBoxLayout(this);
    table_ = new QTableWidget(this); table_->setObjectName(QStringLiteral("worldModelDiagnosticsTable"));
    table_->setColumnCount(7); table_->setHorizontalHeaderLabels({tr("模型路径"), tr("所属单位/装饰物"), tr("原因"), tr("数量"), tr("记录"), tr("占位状态"), tr("实际来源")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows); table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->setWordWrap(false);
    table_->verticalHeader()->setVisible(false); table_->horizontalHeader()->setStretchLastSection(true);
    table_->setColumnWidth(0, 280); table_->setColumnWidth(1, 200); table_->setColumnWidth(2, 270);
    table_->setColumnWidth(3, 55); table_->setColumnWidth(4, 55); table_->setColumnWidth(5, 200); layout->addWidget(table_);
    status_ = new QLabel(this); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true); layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(this);
    auto* exportButton = buttons->addButton(tr("导出为文本…"), QDialogButtonBox::ActionRole);
    exportButton->setObjectName(QStringLiteral("worldModelDiagnosticsExport"));
    buttons->addButton(tr("关闭"), QDialogButtonBox::RejectRole); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(table_, &QTableWidget::cellDoubleClicked, this, [this](int row, int) { if (row >= 0) (void)LocateRow(static_cast<std::size_t>(row)); });
    connect(exportButton, &QPushButton::clicked, this, [this] {
        (void)ExportWithChooser([](QWidget* parent) {
            return QFileDialog::getSaveFileName(parent, QObject::tr("导出模型诊断"),
                QStringLiteral("world-model-diagnostics.txt"), QObject::tr("文本文件 (*.txt)"));
        });
    });
}
void WorldModelDiagnosticsDialog::SetRows(std::vector<WorldModelDiagnosticRow> rows,
    std::function<bool(const MapData::PlacementId&)> locate) {
    ++rowsEpoch_;
    rows_ = std::move(rows); locate_ = std::move(locate);
    table_->clearContents(); table_->setRowCount(static_cast<int>(std::min(rows_.size(), static_cast<std::size_t>(std::numeric_limits<int>::max()))));
    for (int at = 0; at < table_->rowCount(); ++at) {
        const auto& row = rows_[static_cast<std::size_t>(at)];
        const QStringList cells{row.modelPath.isEmpty() ? tr("模型路径未知") : row.modelPath, row.ownerText,
            row.reasonCode + QStringLiteral(" · ") + row.reason, QString::number(row.owners.size()),
            QString::number(row.records), row.placeholderState.isEmpty() ? tr("未请求占位") : row.placeholderState, row.sourceText};
        for (int column = 0; column < cells.size(); ++column) {
            auto* item = new QTableWidgetItem(cells[column]);
            // tooltip explicitly escapes map-supplied text; cell display is always plain text.
            item->setToolTip(QStringLiteral("<pre>%1</pre>").arg(cells[column].toHtmlEscaped())); table_->setItem(at, column, item);
        }
    }
    status_->setText(tr("%1 条").arg(rows_.size()));
}
bool WorldModelDiagnosticsDialog::LocateRow(std::size_t row) {
    if (row >= rows_.size() || rows_[row].owners.empty() || !locate_) {
        status_->setText(tr("此条目无可定位摆放，或地图/来源/修订已变化；未跳转到替代对象。")); return false;
    }
    const auto owner = rows_[row].owners.front();
    const auto ownerCount = rows_[row].owners.size();
    const auto epoch = rowsEpoch_;
    const auto locate = locate_; // 回调重绑列表时，不销毁正在执行的函数对象。
    const QPointer<WorldModelDiagnosticsDialog> self(this);
    const bool located = locate(owner);
    if (!self || self->rowsEpoch_ != epoch) return false;
    if (!located) {
        self->status_->setText(tr("此条目无可定位摆放，或地图/来源/修订已变化；未跳转到替代对象。")); return false;
    }
    self->status_->setText(tr("已定位（共 %1 个）").arg(ownerCount)); return true;
}
bool WorldModelDiagnosticsDialog::ExportWithChooser(const std::function<QString(QWidget*)>& chooser) {
    if (!chooser) return false;
    const auto rows = rows_;
    const auto epoch = rowsEpoch_;
    const QPointer<WorldModelDiagnosticsDialog> self(this);
    auto path = chooser(this);
    if (!self || self->rowsEpoch_ != epoch || path.isEmpty()) return false;
    if (QFileInfo(path).suffix().isEmpty()) path += QStringLiteral(".txt");
    const auto exported = ExportWorldModelDiagnostics(rows, path);
    self->status_->setText(exported.Succeeded() ? tr("诊断已导出：%1").arg(path)
        : tr("诊断导出失败，原目标保持：%1").arg(QString::fromStdString(exported.diagnostics.front().message)));
    return exported.Succeeded();
}

} // namespace War3Map::Modules
