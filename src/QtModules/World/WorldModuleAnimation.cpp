#include "WorldModule.h"
#include "WorldAnimationPlayer.h"
#include "WorldDiagnosticJson.h"
#include "WorldAnimationDiagnostics.h"
#include "WorldPlacementModels.h"
#include "WorldPlacementPreview.h"
#include "WorldSelectionPresenter.h"
#include "WorldBuildingSplatPresenter.h"
#include "WorldModelBugPlaceholderPresenter.h"
#include "WorldPremPresenter.h"
#include "WorldCliffLayer.h"
#include "WorldPathingHelperPresenter.h"
#include "EditorProject.h"
#include "ObjectNames.h"
#include "Widgets/ElidedLabel.h"
#include "War3MapCore/MapData/MapDataEditSession.h"
#include "War3MapCore/MapDocument.h"
#include "War3MapCore/Hash.h"
#include "War3MapCore/Render/AnimationParticleCoverage.h"
#include "War3MapCore/Render/FrameCapture.h"
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QStringList>
#include <QThread>
#include <QVariant>
#include <QVBoxLayout>
#include <QToolTip>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QJsonArray>
#include <QApplication>
#include <QEventLoop>
#include <QPixmap>
#include <QScreen>
#include <algorithm>
#include <chrono>
#include <exception>
#include <iterator>
#include <unordered_set>

namespace War3Map::Modules {
namespace {
std::uint64_t StatusStampMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct SelectionOverlayTarget {
    std::uint32_t request = 0;
    std::array<float,12> transform{};
    bool operator==(const SelectionOverlayTarget&) const = default;
};
std::vector<SelectionOverlayTarget> SelectionOverlayTargets(const Render::WorldModelScene& scene,
    const std::vector<MapData::PlacementId>& selected,
    const std::map<std::pair<MapData::PlacementFile,std::uint32_t>,std::uint32_t>& requests) {
    std::unordered_set<std::uint32_t> wanted;
    for (const auto& id:selected) {
        const auto found=requests.find({id.file,id.index});
        if (found!=requests.end()) wanted.insert(found->second);
    }
    std::vector<SelectionOverlayTarget> result;
    if (wanted.empty()) return result;
    result.reserve(wanted.size());
    for (std::size_t i=0; i<scene.instanceRequests.size(); ++i) {
        const auto request=scene.instanceRequests[i];
        if (!wanted.contains(request)) continue;
        SelectionOverlayTarget target; target.request=request;
        std::copy(std::begin(scene.instances[i].transform),std::end(scene.instances[i].transform),target.transform.begin());
        result.push_back(target);
    }
    return result;
}
QString BoundedDiagnosticText(std::string_view text, std::size_t maximum = 2048) {
    const auto bytes = std::min(text.size(), maximum);
    auto result = QString::fromUtf8(text.data(), static_cast<qsizetype>(bytes));
    if (bytes < text.size()) result += QObject::tr("（文本截断）");
    return result;
}
// 悬停提示只放摘要：去掉来源路径、SHA、字节偏移和逐实例示例，限制行数与行宽。
// 完整文本保留在标签属性里，单击“模型”一行打开可滚动、可复制的诊断窗口。
QString CompactDiagnostics(const QString& text, bool* truncated = nullptr) {
    static const QRegularExpression noise(QStringLiteral(
        " · 示例 .*$| · SHA [0-9A-Fa-f]+| · 字节偏移 ?\\d+| · 字节偏移未知|，?原诊断指纹 [0-9A-Fa-f]+"));
    constexpr int kMaxLines = 14;
    constexpr int kMaxWidth = 96;
    QStringList kept;
    int dropped = 0;
    bool cut = false;
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QStringLiteral("来源")) || line.startsWith(QStringLiteral("实际模型来源"))
            || line.startsWith(QStringLiteral("完整原始诊断")) || line.startsWith(QStringLiteral("统计对应已接收场景"))) {
            cut = true;
            continue;
        }
        const QString compact = QString(line).remove(noise).trimmed();
        if (compact != line) cut = true;
        line = compact;
        if (line.size() > kMaxWidth) { line = line.left(kMaxWidth - 1) + QStringLiteral("…"); cut = true; }
        if (kept.size() >= kMaxLines) { ++dropped; continue; }
        kept << line;
    }
    if (dropped) { kept << QObject::tr("…另有 %1 行").arg(dropped); cut = true; }
    if (truncated) *truncated = cut;
    return kept.join(QLatin1Char('\n'));
}

QString DiagnosticToolTip(const QString& text) {
    if (text.isEmpty()) return {};
    bool truncated = false;
    QString compact = CompactDiagnostics(text, &truncated);
    if (truncated) compact += QObject::tr("\n单击查看完整诊断");
    return QStringLiteral("<qt><div style='white-space:pre-wrap'>") + compact.toHtmlEscaped().replace(
        QLatin1Char('\n'), QStringLiteral("<br/>")) + QStringLiteral("</div></qt>");
}

// 单击诊断标签时显示完整文本。只读窗口，不改变地图或渲染状态。
class DiagnosticViewer final : public QObject {
public:
    explicit DiagnosticViewer(QObject* parent, std::function<void()> openModels = {})
        : QObject(parent), openModels_(std::move(openModels)) {}
protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::MouseButtonRelease) return false;
        auto* label = qobject_cast<QLabel*>(watched);
        if (!label || static_cast<QMouseEvent*>(event)->button() != Qt::LeftButton) return false;
        if (openModels_) { openModels_(); return true; }
        const QString text = label->property("fullDiagnostics").toString();
        if (text.isEmpty()) return false;
        if (!dialog_) {
            dialog_ = new QDialog(label->window());
            dialog_->setWindowTitle(QObject::tr("模型诊断"));
            dialog_->resize(760, 520);
            auto* layout = new QVBoxLayout(dialog_);
            layout->setContentsMargins(14, 14, 14, 12);
            view_ = new QPlainTextEdit(dialog_);
            view_->setReadOnly(true);
            view_->setLineWrapMode(QPlainTextEdit::WidgetWidth);
            layout->addWidget(view_);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog_);
            QObject::connect(buttons, &QDialogButtonBox::rejected, dialog_, &QDialog::hide);
            layout->addWidget(buttons);
        }
        view_->setPlainText(text);
        dialog_->show();
        dialog_->raise();
        return true;
    }
private:
    QPointer<QDialog> dialog_;
    QPlainTextEdit* view_ = nullptr;
    std::function<void()> openModels_;
};

void SetDiagnostics(QLabel& label, const QString& text) {
    label.setProperty("fullDiagnostics", text);
    label.setToolTip(DiagnosticToolTip(text));
    if (!label.property("diagnosticViewer").toBool()) {
        label.installEventFilter(new DiagnosticViewer(&label));
        label.setProperty("diagnosticViewer", true);
        label.setCursor(Qt::PointingHandCursor);
    }
}
bool CanLoadStaticAfterAnimationFailure(const std::vector<Diagnostic>& diagnostics) {
    bool animationFailure = false;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != DiagnosticSeverity::Error) continue;
        const auto& code = diagnostic.code;
        // 来源/身份失败不能用另一读取路径掩盖。静态加载仍走原解析器和同代来源校验。
        if (code.find("SOURCE") != std::string::npos || code.find("IDENTITY") != std::string::npos
            || code.find("STALE") != std::string::npos || code == "ANIMATION_SESSION_INVALID"
            || (!code.starts_with("ANIMATION_") && !code.starts_with("MDX_")
                && !code.starts_with("SKIN_") && !code.starts_with("PARTICLE_"))) return false;
        animationFailure = true;
    }
    return animationFailure;
}
QString FrameSummary(const Render::AnimatedModelScene& scene, const Render::AnimationFrame& frame,
    const Render::AnimationParticleCoverage* coverage) {
    auto result=QObject::tr("%1 个动画 · %2 个静态 · %3 个本帧隐藏 · %4 个不可用")
        .arg(frame.animatedInstances).arg(frame.retainedStaticInstances).arg(frame.hiddenInstances)
        .arg(scene.unavailableInstances.size());
    if (frame.budgetStaticInstances)
        result += QObject::tr(" · %1 个因内存预算以 Stand 静态显示").arg(frame.budgetStaticInstances);
    result += QObject::tr(" · 视野外 %1 · 预算省略 %2").arg(frame.offscreenInstances).arg(frame.budgetParticleInstances);
    if (scene.particlesEnabled && scene.particleInstanceCount) {
        const auto simulated=std::count_if(frame.particleLinks.begin(),frame.particleLinks.end(),[](const auto& p) {return p.simulated;});
        result+=QObject::tr(" · %1 个粒子实例").arg(simulated);
        const auto partial=std::count_if(frame.particleLinks.begin(),frame.particleLinks.end(),[](const auto& p) {return p.partial;});
        if (partial) result+=QObject::tr(" · %1 个粒子部分支持").arg(partial);
    } else if (!scene.particlesEnabled && std::any_of(scene.particleCapabilities.begin(),scene.particleCapabilities.end(),
        [](const auto& c) {return c.particle2Count || c.ribbonCount || c.premCount;}))
        result+=QObject::tr(" · 粒子预览不可用");
    if (coverage && coverage->notShown)
        result += QObject::tr(" · %1 个粒子发射器未显示").arg(coverage->notShown);
    return result;
}
QStringList FrameDetails(const Render::AnimatedModelScene& scene, const Render::AnimationFrame& frame,
    const Render::AnimationParticleCoverage* coverage, const WorldAnimationDiagnosticSummary& grouped) {
    QStringList details{QObject::tr("动画与粒子为预览近似，尚未逐帧验证与原版一致。")};
    if (coverage && coverage->declared) {
        details << QObject::tr("已知 %1 个发射器（按摆放实例计），%2 个因未支持或拒绝未显示；正常发射间歇不计入，未解析模型的发射器数量未知。")
            .arg(coverage->declared).arg(coverage->notShown);
        details << QObject::tr("统计来自当前接受的模拟帧；实际绘制结果以完成帧截图为准。");
        if (coverage->notShown) details << QObject::tr("未显示原因：PREM 未支持 %1 · 局部无效记录 %2 · 预览关闭 %3 · 资产拒绝 %4 · 本帧拒绝 %5 · 预算省略 %6 · 视野外 %7 · 冻结 %8")
            .arg(coverage->unsupportedPrem).arg(coverage->isolated).arg(coverage->disabled)
            .arg(coverage->assetRejected).arg(coverage->runtimeRejected).arg(coverage->budgetRejected)
            .arg(coverage->outsideView).arg(coverage->frozen);
    }
    if (scene.particlesEnabled)
        details<<QObject::tr("Stand 局部时钟按实例身份错开，全局时钟从共同的 1000 ms 同步推进；世界粒子只推进当前有界区间，冻结恢复不补发历史，镜头变化保留已发状态；粒子与模型使用同一帧版本；纯粒子没有模型点选框。");
    else if (std::any_of(scene.particleCapabilities.begin(),scene.particleCapabilities.end(),
        [](const auto& c) {return c.particle2Count || c.ribbonCount || c.premCount;}))
        details<<QObject::tr("当前插件不支持粒子尾部，模型预览保留，粒子未模拟或绘制。");
    std::size_t skippedEmitters=0, affectedInstances=0;
    for (const auto& capability:scene.particleCapabilities) {
        if (!capability.skippedEmitters || capability.skippedEmitters->empty()) continue;
        skippedEmitters+=capability.skippedEmitters->size();
        ++affectedInstances;
    }
    if (skippedEmitters)
        details<<QObject::tr("局部跳过 %1 个发射器（按实例计），涉及 %2 个实例；粒子效果为部分预览。")
            .arg(skippedEmitters).arg(affectedInstances);
    if (!frame.geosetIssues.empty()) {
        const auto isolated=std::count_if(frame.poses.begin(),frame.poses.end(),[](const auto& p) {return p.partial;});
        const auto invisible=std::count_if(frame.poses.begin(),frame.poses.end(),[](const auto& p) {return p.partial && p.noVisibleGeometry;});
        details<<QObject::tr("本帧局部隔离 %1 个 GEOA 部件（按实例计），涉及 %2 个实例，其中 %3 个没有可显示的模型几何；独立粒子按各自诊断处理。")
            .arg(frame.geosetIssues.size()).arg(isolated).arg(invisible);
    }
    details.append(grouped.rows);
    return details;
}
void ShowFrameStatus(QLabel& label, std::shared_ptr<const Render::AnimatedModelScene> ownedScene,
    const Render::AnimationFrame& frame, WorldAnimationDiagnostics& diagnostics, const QString& notice={}) {
    const auto& scene = *ownedScene;
    label.setTextFormat(Qt::PlainText);
    const auto coverage = Render::QueryAnimationParticleCoverage(scene, frame);
    const auto* counts = coverage.Succeeded() ? &*coverage.value : nullptr;
    label.setText(FrameSummary(scene,frame,counts)+(notice.isEmpty()?QString{}:QStringLiteral(" · ")+notice));
    auto details=FrameDetails(scene,frame,counts,diagnostics.Summarize(std::move(ownedScene),frame));
    details.prepend(QObject::tr("统计对应已接收场景 %1 / 帧 %2；界面统计最多每 250 ms 更新一次。").arg(frame.sceneVersion).arg(frame.contentVersion));
    if (!coverage.Succeeded()) details << WorldDiagnosticText(coverage.diagnostics);
    details.prepend(QObject::tr("%1 个未解析摆放").arg(label.property("animationUnresolvedPlacements").toULongLong()));
    if (!notice.isEmpty()) details.prepend(notice);
    const auto splats = label.property("buildingSplatPreview").toString();
    if (!splats.isEmpty()) details.prepend(splats);
    const auto prem = label.property("premPreview").toString();
    if (!prem.isEmpty()) details.prepend(prem);
    SetDiagnostics(label, details.join(QLatin1Char('\n')));
}
} // namespace

bool WorldModule::InstanceDiagnosticsCurrent() const {
    if (!animation_ || !viewport_ || !viewport_->HasAnimationScene()
        || !instanceDiagnosticScene_ || !instanceDiagnosticQuery_ || !instanceDiagnosticVersion_ || !selectionMapData_
        || !viewport_->ModelPresentationCurrent() || !animation_->Ready() || animation_->Scene() != instanceDiagnosticScene_
        || instanceDiagnosticRegistry_ != instanceRegistry_
        || scene_ != animation_->Picking() || modelWorker_ || placementsDirty_
        || generation_ != instanceDiagnosticGeneration_ || MapKey() != instanceDiagnosticMapKey_
        || selectionMapData_->Binding() != instanceDiagnosticVersion_->document
        || selectionCatalogRevision_ != instanceDiagnosticVersion_->catalogRevision
        || selectionBoundCatalogRevision_ != selectionCatalogRevision_
        || instanceDiagnosticVersion_->document.generation != std::max<std::uint64_t>(1, project_.editRevision_.Generation())
        || instanceDiagnosticVersion_->document.revision != (project_.mapEdits_ ? project_.mapEdits_->Revision() : 0)) return false;
    const auto frame = animation_->Frame();
    return frame && frame->sceneVersion == instanceDiagnosticScene_->sceneVersion
        && frame->documentRevision == instanceDiagnosticScene_->documentRevision
        && frame->documentRevision == instanceDiagnosticVersion_->document.revision;
}

void WorldModule::BindInstanceDiagnostics() {
    if (bugPlaceholderPresenter_) bugPlaceholderPresenter_->Clear();
    if (premPresenter_) premPresenter_->Clear();
    instanceDiagnosticScene_.reset(); instanceDiagnosticRegistry_.reset(); instanceDiagnosticQuery_.reset(); instanceDiagnosticRequests_.clear(); instanceDiagnosticVersion_.reset();
    if (!animation_ || !animation_->Ready() || !worldSelection_.Version()) return;
    const auto scene = animation_->Scene();
    const auto frame = animation_->Frame();
    if (!scene || !frame || frame->sceneVersion != scene->sceneVersion
        || frame->documentRevision != scene->documentRevision || frame->states.size() != scene->specs.size()
        || placementIds_.size() > scene->specs.size()) return;
    if (instanceRegistry_) {
        const auto& binding = instanceRegistry_->Binding();
        const auto& document = worldSelection_.Version()->document;
        if (binding.sourceGeneration != scene->generation || binding.mapRevision != document.revision
            || binding.sourceIdentity != document.sourceSha256
            || binding.editSessionIdentity != (project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0)
            || binding.editRevision != (project_.mapEdits_ ? project_.mapEdits_->Revision() : 0)) return;
    }
    std::map<std::pair<MapData::PlacementFile, std::uint32_t>, std::uint32_t> requests;
    for (std::size_t request = 0; request < placementIds_.size(); ++request) {
        const auto& id = placementIds_[request];
        if (!WorldDiagnosticPlacementMatches(id, request, scene->specs[request], frame->states[request], instanceRegistry_.get())
            || !requests.emplace(std::make_pair(id.file, id.index), static_cast<std::uint32_t>(request)).second) return;
    }
    auto query = Render::AnimationDiagnosticQuery::Create(scene);
    if (!query.Succeeded()) { project_.AddDiagnostics(query.diagnostics); return; }
    auto attached = (*query.value)->SetFrame(frame);
    if (!attached.Succeeded()) { project_.AddDiagnostics(attached.diagnostics); return; }
    instanceDiagnosticScene_ = scene; instanceDiagnosticRegistry_ = instanceRegistry_; instanceDiagnosticRequests_ = std::move(requests);
    instanceDiagnosticQuery_ = std::move(*query.value);
    instanceDiagnosticVersion_ = worldSelection_.Version();
    instanceDiagnosticMapKey_ = MapKey(); instanceDiagnosticGeneration_ = generation_;
    if (bugPlaceholderPresenter_ || premPresenter_) {
        const auto registry = instanceRegistry_;
        const auto expected = *instanceDiagnosticVersion_;
        const auto editSessionIdentity = project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0;
        const auto live = [this, scene, registry, expected, editSessionIdentity] {
            return InstanceDiagnosticsCurrent() && !project_.sourceChangeSuspected_ && !project_.sourceChangeConfirmed_
                && animation_->Scene() == scene && instanceRegistry_ == registry && worldSelection_.Version()
                && (project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0) == editSessionIdentity
                && worldSelection_.Version()->document == expected.document
                && worldSelection_.Version()->catalogRevision == expected.catalogRevision;
        };
        const Services::WorldModelBugFlagsReader flags = [this, registry, live](const MapData::PlacementId& owner)
            -> std::optional<Services::WorldEditorFlags> {
                if (!live() || (registry && !registry->Find(owner))) return std::nullopt;
                return worldSelection_.Flags(Services::WorldObjectId{owner});
            };
        if (bugPlaceholderPresenter_ && registry) bugPlaceholderPresenter_->Bind(scene, registry, placementIds_, live, flags);
        else if (bugPlaceholderPresenter_ && unresolvedPlacements_.empty()) {
            const auto document = CurrentDocument();
            if (document) {
                auto source = Services::WorldModelBugReadOnlySource::Capture(*document, scene,
                    {scene->generation, expected.document.revision,
                        project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0,
                        project_.mapEdits_ ? project_.mapEdits_->Revision() : 0, expected.document.sourceSha256}, placementIds_);
                if (source.Succeeded()) bugPlaceholderPresenter_->BindReadOnly(scene, std::move(*source.value), live, flags);
                else project_.AddDiagnostics(source.diagnostics);
            }
        }
        if (premPresenter_ && registry) premPresenter_->Bind(scene, registry, placementIds_, live);
    }
    RefreshInstanceDiagnostics();
    PublishResourceProblems();
}

void WorldModule::PublishResourceProblems() {
    constexpr std::size_t kMaxModelRows = 200;
    auto problems = terrainProblems_;
    // 同一模型、同一原因只列一条并写明涉及几处，详情仍在“模型诊断”窗口。
    std::map<std::pair<QString, QString>, std::pair<std::size_t, QString>> models;
    for (const auto& row : ModelDiagnosticRows()) {
        auto& entry = models[{row.modelPath, row.reasonCode}];
        entry.first += std::max<std::size_t>(1, row.owners.size()); entry.second = row.reason;
    }
    std::size_t shown = 0;
    for (const auto& [key, entry] : models) {
        if (shown++ == kMaxModelRows) {
            problems.push_back({DiagnosticSeverity::Warning, {}, "WORLD_MODEL_RESOURCE",
                tr("另有 %1 类模型问题，详见“模型诊断”。").arg(models.size() - kMaxModelRows).toStdString(), std::nullopt});
            break;
        }
        problems.push_back({DiagnosticSeverity::Warning, {}, key.second.toStdString(),
            tr("模型 %1（%2 处）：%3").arg(key.first.isEmpty() ? tr("（无路径）") : key.first).arg(entry.first).arg(entry.second).toStdString(),
            std::nullopt});
    }
    project_.ReplaceDiagnostics(QStringLiteral("World.Resources"), std::move(problems));
}

QString WorldModule::InstanceDiagnosticText(const Services::WorldSelectionVersion& expected,
    const MapData::PlacementId& id) const {
    if (!InstanceDiagnosticsCurrent() || expected.document != instanceDiagnosticVersion_->document
        || expected.catalogRevision != instanceDiagnosticVersion_->catalogRevision) return {};
    const auto found = instanceDiagnosticRequests_.find({id.file, id.index});
    if (found == instanceDiagnosticRequests_.end()) return tr("动画实例诊断 · 场景 %1 · 已接受帧 %2\n%3\n当前场景没有此摆放的模型请求；没有逐实例动画诊断。")
        .arg(instanceDiagnosticScene_->sceneVersion).arg(animation_->Frame()->contentVersion)
        .arg(BoundedDiagnosticText(WorldPlacementHostKey(id)));
    const auto request = found->second;
    if (request >= placementIds_.size() || placementIds_[request] != id
        || request >= instanceDiagnosticScene_->specs.size()
        || request >= animation_->Frame()->states.size()
        || !WorldDiagnosticPlacementMatches(id, request, instanceDiagnosticScene_->specs[request],
            animation_->Frame()->states[request], instanceDiagnosticRegistry_.get())) return {};
    // 这里只接已被播放器/视口接受的同一帧；公共查询不证明 GPU 完成或实际呈现。
    const auto attached = instanceDiagnosticQuery_->SetFrame(animation_->Frame());
    if (!attached.Succeeded()) return {};
    const auto binding = instanceDiagnosticQuery_->Binding(request, instanceDiagnosticScene_->specs[request].identity);
    if (!binding.Succeeded()) return {};
    constexpr std::uint32_t issueLimit = 8;
    const auto queried = instanceDiagnosticQuery_->Query(*binding.value, 0, issueLimit);
    if (!queried.Succeeded() || !queried.value->hasFrame) return {};
    const auto& page = *queried.value;
    QStringList helperDetails;
    if (pathingHelperPresentation_) {
        const auto helper = std::find_if(pathingHelperPresentation_->helpers.begin(), pathingHelperPresentation_->helpers.end(),
            [request](const auto& value) { return value.requestIndex == request; });
        if (helper != pathingHelperPresentation_->helpers.end() && helper->row && helper->currentOwner == id
            && pathingHelperPresentation_->version.document == expected.document
            && pathingHelperPresentation_->version.catalogRevision == expected.catalogRevision) {
            const auto& row = *helper->row;
            helperDetails << tr("路径阻断器 · 紫黑方块为编辑辅助预览；原版锚点、旋转与物编缩放未验。")
                << tr("模型原字段 bfil：%1").arg(BoundedDiagnosticText(row.fields.modelPath.value));
            if (row.footprint) helperDetails << tr("通行性来源 %1 × %2 像素 · 平面 %3 × %4；未乘物编缩放")
                .arg(row.footprint->pixelWidth).arg(row.footprint->pixelHeight)
                .arg(row.footprint->worldWidth).arg(row.footprint->worldHeight);
            else helperDetails << (row.footprintState == Services::PlacementFootprintState::Absent
                ? tr("实际占地字段为空或缺失，只显示小标记；未填默认占地。")
                : tr("占地未知，只显示小标记：%1").arg(BoundedDiagnosticText(row.footprintReason)));
            if (row.textureSource) helperDetails << tr("通行性来源 %1 · %2 · SHA %3")
                .arg(BoundedDiagnosticText(row.textureSource->sourceId), BoundedDiagnosticText(row.textureSource->sourcePath),
                    BoundedDiagnosticText(row.textureSha256.value_or("未知"), 128));
            for (const auto& origin : row.fields.origins)
                helperDetails << tr("字段 %1 · 对象 %2 · 来源 %3 · %4")
                    .arg(BoundedDiagnosticText(origin.field, 16), BoundedDiagnosticText(origin.objectRawcode, 16),
                        BoundedDiagnosticText(origin.sourceId), BoundedDiagnosticText(origin.sourcePath));
        }
    }
    const auto frame=animation_->Frame();
    const bool outside=frame && request<frame->requestVisibility.size() && !frame->requestVisibility[request];
    bool frozen=false;
    if (frame) {
        const auto particle=std::lower_bound(frame->particleLinks.begin(),frame->particleLinks.end(),request,
            [](const auto& link,auto index) { return link.requestIndex<index; });
        frozen=particle!=frame->particleLinks.end() && particle->requestIndex==request && particle->frozen;
    }
    if (page.issues.empty() && !page.hidden && !page.unavailable && !page.partial && !outside && !frozen)
        return helperDetails.join(QLatin1Char('\n'));
    QStringList details{
        tr("动画实例诊断 · 场景 %1 · 已接受帧 %2 · 姿势 %3")
            .arg(page.binding.sceneVersion).arg(page.binding.contentVersion).arg(page.binding.poseVersion),
        tr("请求 %1 · 实例 %2/%3 · 地图修订 %4")
            .arg(page.binding.requestIndex).arg(page.binding.identity.value).arg(page.binding.identity.generation)
            .arg(page.binding.documentRevision),
        BoundedDiagnosticText(page.hostKey) + QStringLiteral(" · ") + BoundedDiagnosticText(page.modelPath),
        tr("预览近似；诊断对应服务帧，显隐开关另行生效。")};
    details.append(helperDetails);
    if (page.unavailable) details << tr("此实例资产不可用。");
    if (outside) details << tr("本服务帧在带余量的视野外，未分配姿势/粒子预算；保留实例身份与播放时钟。");
    if (frozen) details << (outside ? tr("粒子检查点已冻结，返回视野后按当前时间恢复。")
        : tr("粒子因当前预算不足而冻结，检查点保留；这不属于资产或局部采样拒绝。"));
    if (page.hidden) details << tr("本服务帧因局部拒绝而隐藏此实例。");
    if (page.partial) details << tr("此实例仅部分预览。");
    if (page.noVisibleGeometry) details << (page.animated && !page.hidden && page.binding.poseVersion
        ? tr("本帧没有可显示的模型几何；骨架姿势仍有效，独立粒子按各自诊断处理。")
        : tr("本帧没有可显示的模型几何；独立粒子按各自诊断处理。"));
    if (page.source) details << tr("模型来源 %1 · SHA %2")
        .arg(BoundedDiagnosticText(page.source->sourceLayer), BoundedDiagnosticText(page.source->contentDigest, 128));
    for (const auto& issue : page.issues) {
        auto source = BoundedDiagnosticText(issue.stage, 64);
        if (!issue.kind.empty()) source += QStringLiteral(" · ") + BoundedDiagnosticText(issue.kind, 8);
        if (issue.sourceIndex) source += tr(" 源索引 %1（第 %2 个）").arg(*issue.sourceIndex)
            .arg(static_cast<qulonglong>(*issue.sourceIndex) + 1);
        if (issue.nodeId) source += tr(" · 节点 %1").arg(*issue.nodeId);
        if (issue.geosetIndex) source += tr(" · geoset %1（第 %2 个网格部件）").arg(*issue.geosetIndex)
            .arg(static_cast<qulonglong>(*issue.geosetIndex) + 1);
        details << tr("%1：%2 · %3").arg(source, BoundedDiagnosticText(issue.code, 256), BoundedDiagnosticText(issue.message));
    }
    const auto returned = static_cast<std::uint64_t>(page.offset) + page.issues.size();
    if (returned < page.total) details << tr("另有 %1 条诊断未展开").arg(page.total - returned);
    auto text = details.join(QLatin1Char('\n'));
    constexpr qsizetype textLimit = 12 * 1024;
    return text.size() <= textLimit ? text : text.left(textLimit) + tr("\n（文本截断）");
}

void WorldModule::RefreshInstanceDiagnostics() {
    if (!instanceDiagnosticNote_ && selectionCard_) {
        if (auto* layout = qobject_cast<QVBoxLayout*>(selectionCard_->layout())) {
            instanceDiagnosticNote_ = new QLabel(selectionCard_);
            instanceDiagnosticNote_->setObjectName(QStringLiteral("worldSelectionDiagnostics"));
            instanceDiagnosticNote_->setTextFormat(Qt::PlainText);
            instanceDiagnosticNote_->setWordWrap(true);
            instanceDiagnosticNote_->hide();
            layout->addWidget(instanceDiagnosticNote_);
        }
    }
    if (!InstanceDiagnosticsCurrent()) {
        if (bugPlaceholderPresenter_) bugPlaceholderPresenter_->Clear();
        if (premPresenter_) premPresenter_->Clear();
        if (instanceDiagnosticScene_ && QToolTip::text().contains(
            tr("动画实例诊断 · 场景 %1 ·").arg(instanceDiagnosticScene_->sceneVersion))) QToolTip::hideText();
        instanceDiagnosticScene_.reset(); instanceDiagnosticRegistry_.reset(); instanceDiagnosticQuery_.reset(); instanceDiagnosticRequests_.clear(); instanceDiagnosticVersion_.reset();
        if (instanceDiagnosticNote_) { instanceDiagnosticNote_->clear(); instanceDiagnosticNote_->setToolTip({}); instanceDiagnosticNote_->hide(); }
        if (selectionMeta_) selectionMeta_->setToolTip({});
        return;
    }
    if (bugPlaceholderPresenter_) bugPlaceholderPresenter_->Refresh();
    if (premPresenter_) premPresenter_->Refresh();
    // 只查询主选及最多三个附加所选；目录行在 hover 时惰性查询，不逐帧拼整棵树。
    constexpr std::size_t maximum = 4;
    std::vector<MapData::PlacementId> selected;
    if (selection_) selected.push_back(*selection_);
    for (const auto& id : selectedPlacements_) {
        if (selected.size() == maximum) break;
        if (!selection_ || id != *selection_) selected.push_back(id);
    }
    QStringList details;
    for (const auto& id : selected) {
        const auto text = InstanceDiagnosticText(*instanceDiagnosticVersion_, id);
        if (!text.isEmpty()) details << text;
    }
    if (selectedPlacements_.size() > selected.size())
        details << tr("另有 %1 个所选摆放未展开").arg(selectedPlacements_.size() - selected.size());
    auto tooltip = details.join(QStringLiteral("\n\n"));
    constexpr qsizetype textLimit = 16 * 1024;
    if (tooltip.size() > textLimit) tooltip = tooltip.left(textLimit) + tr("\n（文本截断）");
    tooltip = DiagnosticToolTip(tooltip);
    if (selectionMeta_ && selectionMeta_->toolTip() != tooltip) selectionMeta_->setToolTip(tooltip);
    if (!instanceDiagnosticNote_) return;
    const auto summary = tooltip.isEmpty() ? QString{} : tr("动画/粒子诊断 · %1")
        .arg(selectedPlacements_.size());
    if (instanceDiagnosticNote_->text() != summary) instanceDiagnosticNote_->setText(summary);
    if (instanceDiagnosticNote_->toolTip() != tooltip) instanceDiagnosticNote_->setToolTip(tooltip);
    instanceDiagnosticNote_->setVisible(!tooltip.isEmpty() && !selectedPlacements_.empty());
}

void WorldModule::SetupAnimationControls() {
    auto diagnostics = std::make_shared<WorldAnimationDiagnostics>();
    animation_=new WorldAnimationPlayer(*viewport_,this);
    ApplyEffectQualityFromSettings();
    if (modelStatus_ && !modelStatus_->property("diagnosticViewer").toBool()) {
        modelStatus_->installEventFilter(new DiagnosticViewer(modelStatus_, [this] { OpenModelDiagnostics(); }));
        modelStatus_->setProperty("diagnosticViewer", true); modelStatus_->setCursor(Qt::PointingHandCursor);
    }
    selectionPresenter_ = new WorldSelectionPresenter(*viewport_, this);
    buildingSplatPresenter_ = new WorldBuildingSplatPresenter(*viewport_, this);
    bugPlaceholderPresenter_ = new WorldModelBugPlaceholderPresenter(viewport_, this);
    premPresenter_ = new WorldPremPresenter(viewport_, this);
    cliffLayer_ = new WorldCliffLayerPresenter(viewport_, this);
    connect(animation_, &WorldAnimationPlayer::InstanceWorkWaitingChanged,
        selectionPresenter_, [this](bool waiting) {
            if (!waiting && !animation_->Ready()) selectionPresenter_->Clear();
            selectionPresenter_->SetSubmissionDeferred(waiting);
        }, Qt::DirectConnection);
    connect(animation_, &WorldAnimationPlayer::InstanceWorkWaitingChanged, this, [this](bool waiting) {
        if (buildingSplatPresenter_) {
            if (!waiting && !animation_->Ready()) buildingSplatPresenter_->Clear();
            buildingSplatPresenter_->SetSubmissionDeferred(waiting);
        }
        if (bugPlaceholderPresenter_ && (!animation_->Ready() || waiting)) bugPlaceholderPresenter_->Clear();
        if (premPresenter_ && (!animation_->Ready() || waiting)) premPresenter_->Clear();
        if (!waiting && animation_->Ready() && InstanceDiagnosticsCurrent()) BindInstanceDiagnostics();
        if (!placementPreview_) return;
        if (!waiting && !animation_->Ready()) placementPreview_->Clear();
        placementPreview_->SetSubmissionDeferred(waiting);
    }, Qt::DirectConnection);
    connect(selectionPresenter_, &WorldSelectionPresenter::Message, this, [this](const QString& text) {
        if (selectionStatus_) selectionStatus_->setText(text);
    });
    connect(buildingSplatPresenter_, &WorldBuildingSplatPresenter::Message, this, [this,diagnostics](const QString& text) {
        if (!modelStatus_) return;
        modelStatus_->setProperty("buildingSplatPreview", text);
        if (animation_ && animation_->Ready() && animation_->Frame())
            ShowFrameStatus(*modelStatus_, animation_->Scene(), *animation_->Frame(), *diagnostics);
        else SetDiagnostics(*modelStatus_, modelStatus_->property("fullDiagnostics").toString() + QStringLiteral("\n") + text);
    });
    connect(animationPlay_,&QPushButton::clicked,this,[this]{animation_->SetPlaying(true);});
    connect(animationPause_,&QPushButton::clicked,this,[this]{animation_->SetPlaying(false);});
    connect(animationStep_,&QPushButton::clicked,this,[this]{animation_->Step();});
    connect(animation_,&WorldAnimationPlayer::Changed,this,[this,diagnostics,lastFrame=std::uint64_t{0},lastScene=std::uint64_t{0},
        lastStatusMs=std::uint64_t{0},overlayTargets=std::vector<SelectionOverlayTarget>{}]() mutable {
        RefreshEffectBudgetStatus();
        if (bugPlaceholderPresenter_) bugPlaceholderPresenter_->Refresh();
        if (premPresenter_) {
            premPresenter_->Refresh();
            if (modelStatus_) modelStatus_->setProperty("premPreview", premPresenter_->Summary());
        }
        if (!page_) return;
        const bool animated=animation_->Ready() &&
            (animation_->Scene()->animatedInstances!=0 || animation_->Scene()->particleInstanceCount!=0);
        animationPlay_->setEnabled(animated && !animation_->Playing());
        animationPause_->setEnabled(animated && animation_->Playing());
        animationStep_->setEnabled(animated && !animation_->Playing() && !animation_->Busy());
        QStringList playbackDetails{
            tr("场景就绪 %1 · 播放 %2 · 求值/队列 %3 · 实例事务 %4")
                .arg(animation_->Ready() ? tr("是") : tr("否"), animation_->Playing() ? tr("是") : tr("否"),
                    animation_->Busy() ? tr("忙碌") : tr("空闲"), animation_->InstancesBusy() ? tr("等待") : tr("空闲"))};
        if (!animation_->Ready()) playbackDetails << tr("动画场景未安装或已失效；静态回退不代表正在播放。请查看模型状态的具体原因。");
        else if (!animated) playbackDetails << tr("当前接收的场景没有可播放的模型或粒子。");
        else if (animation_->Playing()) playbackDetails << tr("世界已请求播放，播放按钮因此禁用；暂停按钮用于停止播放。");
        if (const auto target = animation_->Frame())
            playbackDetails << tr("已接收场景 %1 · 内容 %2（实际画面仍以完成帧为准）")
                .arg(target->sceneVersion).arg(target->contentVersion);
        if (!animation_->LastDiagnostic().isEmpty()) playbackDetails << tr("最近动画诊断：%1").arg(animation_->LastDiagnostic());
        const auto playbackTooltip = playbackDetails.join(QLatin1Char('\n'));
        for (auto* button : {animationPlay_, animationPause_, animationStep_})
            if (button->toolTip() != playbackTooltip) button->setToolTip(playbackTooltip);
        const auto frame=animation_->Frame();
        if (!frame) { lastFrame=0; lastScene=0; lastStatusMs=0; overlayTargets.clear(); diagnostics->Clear(); RefreshInstanceDiagnostics(); return; }
        if (lastScene==frame->sceneVersion && lastFrame==frame->contentVersion) return;
        const bool newScene=lastScene!=frame->sceneVersion;
        lastScene=frame->sceneVersion;
        lastFrame=frame->contentVersion;
        const auto now=StatusStampMs();
        const bool refreshStatus=newScene || !animation_->Playing() || now-lastStatusMs>=250;
        if (refreshStatus && modelStatus_) ShowFrameStatus(*modelStatus_,animation_->Scene(),*frame,*diagnostics);
        if (scene_==animation_->Picking()) {
            // Player 的同修订门已复核地图身份；每帧只重定位主选，无需重新 Bind/Snapshot 整份选择上下文。
            if (newScene) ValidateSelection();
            else if (selection_) {
                const auto request=instanceDiagnosticRequests_.find({selection_->file,selection_->index});
                if (request!=instanceDiagnosticRequests_.end()) viewport_->SetSelectedInstance(InstanceOfRequest(request->second));
                else viewport_->SetSelectedInstance(SelectedRequest() ? InstanceOfRequest(*SelectedRequest()) : std::nullopt);
            } else viewport_->SetSelectedInstance(std::nullopt);
            const auto targets=SelectionOverlayTargets(*scene_,selectedPlacements_,instanceDiagnosticRequests_);
            if (newScene || targets!=overlayTargets) { UpdateOverlay(); overlayTargets=targets; }
        }
        if (refreshStatus) { RefreshInstanceDiagnostics(); lastStatusMs=now; }
    });
    connect(animation_,&WorldAnimationPlayer::Message,this,[this,diagnostics](const QString& text) {
        if (!page_ || !modelStatus_) return;
        if (const auto frame=animation_->Frame()) ShowFrameStatus(*modelStatus_,animation_->Scene(),*frame,*diagnostics,text);
        else {
            const auto note = viewport_->ModelPresentationCurrent() ? text : text + tr(" · 旧画面已冻结（只读）");
            modelStatus_->setText(note); SetDiagnostics(*modelStatus_, note);
            UpdateInspector();
            if (selectionStatus_ && !viewport_->ModelPresentationCurrent()) selectionStatus_->setText(note);
            // 只对上传拒绝尝试一次当前来源的静态加载；来源/身份失效保留原文，不无限重试。
            if (text.startsWith(tr("模型上传失败："))) StartStaticModels(generation_, text);
        }
    });
}
std::vector<WorldModelDiagnosticRow> WorldModule::ModelDiagnosticRows() const {
    std::vector<WorldModelDiagnosticRow> rows;
    const auto document = CurrentDocument(); if (!document) return rows;
    std::map<std::pair<MapData::PlacementFile, std::uint32_t>, QString> models;
    for (std::size_t i = 0; i < std::min(placementIds_.size(), requests_.size()); ++i)
        models.emplace(std::make_pair(placementIds_[i].file, placementIds_[i].index), QString::fromStdString(requests_[i].modelPath));
    std::map<std::pair<MapData::PlacementFile, std::string>, QString> names;
    const auto ownerName = [&](const MapData::PlacementId& owner) {
        const auto key = std::make_pair(owner.file, owner.rawcode);
        if (const auto found = names.find(key); found != names.end()) return found->second;
        const auto type = owner.file == MapData::PlacementFile::Units
            ? PlacementType(project_, owner.rawcode, ObjectData::ObjectType::Unit, ObjectData::ObjectType::Item)
            : PlacementType(project_, owner.rawcode, ObjectData::ObjectType::Destructable, ObjectData::ObjectType::Doodad);
        QString label;
        if (type) {
            label = QString::fromUtf8(ObjectData::ToString(*type)) + QStringLiteral(" · ");
            const auto display = ResolveProjectObjectPresentation(project_, {*type, owner.rawcode});
            label += display.name.isEmpty() ? QString::fromStdString(owner.rawcode) : display.name;
            label += QStringLiteral(" [%1]").arg(QString::fromStdString(owner.rawcode));
        } else label = tr("定义未知 · %1").arg(QString::fromStdString(owner.rawcode));
        names.emplace(key, label); return label;
    };
    const auto describeOwners = [&](WorldModelDiagnosticRow& row) {
        QStringList labels;
        for (const auto& owner : row.owners) { const auto label = ownerName(owner); if (!labels.contains(label)) labels << label; }
        row.ownerText = labels.isEmpty() ? tr("场景/辅助模型，没有摆放身份") : labels.join(QStringLiteral("；"));
    };
    if (InstanceDiagnosticsCurrent()) {
        WorldAnimationDiagnostics diagnostics;
        const auto summary = diagnostics.Summarize(instanceDiagnosticScene_, *animation_->Frame());
        for (const auto& group : summary.modelGroups) {
            WorldModelDiagnosticRow row;
            row.modelPath = group.modelPath; row.reasonCode = group.code; row.reason = group.stage + QStringLiteral(" · ") + group.reason;
            row.records = group.records;
            row.canMerge = group.sourceKnown;
            row.sourceText = group.sourceKnown ? group.sourceLayer + QStringLiteral(" · SHA ") + group.sourceSha256
                : tr("实际来源/SHA未知，未跨实例合并");
            for (const auto request : group.requests) if (request < placementIds_.size()) {
                const auto& owner = placementIds_[request];
                if (MapData::FindPlacement(*document, owner).Succeeded()) row.owners.push_back(owner);
            }
            describeOwners(row); rows.push_back(std::move(row));
        }
    }
    // 缺模型的行从未进入scene；单独复用同一个只读捕获，不伪造requestIndex。
    const auto unresolved = CaptureWorldPlacements(project_, *document);
    for (const auto& missing : unresolved.unresolvedRows) {
        WorldModelDiagnosticRow row;
        row.modelPath = QString::fromStdString(missing.modelPath); row.owners = {missing.owner}; row.records = 1;
        row.reasonCode = QString::fromStdString(missing.reasonCode); row.reason = QString::fromStdString(missing.reason);
        QStringList origins;
        for (const auto& origin : missing.origins) origins << QString::fromStdString(origin.sourceId + " · " + origin.sourcePath);
        row.sourceText = origins.isEmpty() ? tr("有效字段来源未知") : origins.join(QStringLiteral("；"));
        row.canMerge = !missing.origins.empty();
        describeOwners(row); rows.push_back(std::move(row));
    }
    if (buildingSplatPresenter_) for (const auto& missing : buildingSplatPresenter_->DiagnosticRows()) {
        WorldModelDiagnosticRow row; row.owners = {missing.owner}; row.records = 1;
        const auto model = models.find({missing.owner.file, missing.owner.index});
        if (model != models.end()) row.modelPath = model->second;
        row.reasonCode = QString::fromStdString(missing.reasonCode); row.reason = tr("建筑贴画 · %1").arg(QString::fromStdString(missing.reason));
        QStringList origins;
        for (const auto& origin : missing.origins) origins << QString::fromStdString(origin.field + " · " + origin.sourceId + " · " + origin.sourcePath);
        row.sourceText = origins.join(QStringLiteral("；")); row.canMerge = !missing.origins.empty();
        describeOwners(row); rows.push_back(std::move(row));
    }
    std::vector<WorldModelDiagnosticRow> grouped;
    std::map<std::tuple<QString, QString, QString, QString, QString>, std::size_t> indices;
    for (auto& row : rows) {
        const auto key = std::make_tuple(row.modelPath, row.ownerText, row.reasonCode, row.reason, row.sourceText);
        const auto found = indices.find(key);
        if (!row.canMerge || found == indices.end()) {
            if (row.canMerge) indices.emplace(key, grouped.size());
            grouped.push_back(std::move(row)); continue;
        }
        auto& previous = grouped[found->second]; previous.records += row.records;
        for (auto& owner : row.owners)
            if (std::find(previous.owners.begin(), previous.owners.end(), owner) == previous.owners.end()) previous.owners.push_back(std::move(owner));
    }
    const auto placeholderPlan = bugPlaceholderPresenter_ ? bugPlaceholderPresenter_->Snapshot() : nullptr;
    for (auto& row : grouped) {
        std::size_t prepared = 0;
        if (placeholderPlan) for (const auto& owner : row.owners)
            if (std::any_of(placeholderPlan->markers.begin(), placeholderPlan->markers.end(),
                [&](const auto& marker) { return marker.owner == owner; })) ++prepared;
        if (prepared) row.placeholderState = tr("已准备 %1/%2 紫黑占位（原完成帧 %3；显示待核验）")
            .arg(static_cast<qulonglong>(prepared)).arg(static_cast<qulonglong>(row.owners.size()))
            .arg(static_cast<qulonglong>(placeholderPlan->completedContentVersion));
        else if (row.reasonCode == QStringLiteral("WORLD_MODEL_FIELD_EMPTY"))
            row.placeholderState = tr("显式空模型；不生成故障占位");
        else row.placeholderState = tr("未准备故障占位；不据此判定故障");
    }
    return grouped;
}
void WorldModule::OpenModelDiagnostics() {
    if (!page_) return;
    if (!modelDiagnosticsDialog_) {
        modelDiagnosticsDialog_ = new WorldModelDiagnosticsDialog(page_->window());
        const QPointer<WorldModelDiagnosticsDialog> dialog = modelDiagnosticsDialog_;
        connect(this, &QObject::destroyed, modelDiagnosticsDialog_, [dialog] {
            if (dialog) { dialog->SetRows({}, {}); dialog->hide(); dialog->deleteLater(); }
        });
    }
    const auto expected = PlacementPresentationVersion(); const auto mapKey = MapKey(); const auto generation = generation_;
    const QPointer<WorldModule> ownerModule(this);
    modelDiagnosticsDialog_->SetRows(ModelDiagnosticRows(), [this, ownerModule, expected, mapKey, generation](const MapData::PlacementId& owner) {
        if (!ownerModule) return false;
        const auto current = PlacementPresentationVersion();
        if (!page_ || mapKey != MapKey() || generation != generation_ || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_
            || current.document != expected.document || current.catalogRevision != expected.catalogRevision
            || std::max<std::uint64_t>(1, project_.editRevision_.Generation()) != expected.document.generation
            || (project_.mapEdits_ ? project_.mapEdits_->Revision() : 0) != expected.document.revision) return false;
        return FocusPlacement(owner);
    });
    modelDiagnosticsDialog_->show(); modelDiagnosticsDialog_->raise();
}
namespace {
QJsonObject PlacementDiagnosticIdentity(const MapData::PlacementId& id) {
    return {{"file", id.file == MapData::PlacementFile::Units ? "war3mapUnits.doo" : "war3map.doo"},
        {"index", static_cast<double>(id.index)}, {"rawcode", QString::fromStdString(id.rawcode)}, {"creationId", id.creationId}};
}
QString FieldStateName(Services::PlacementFieldState state) {
    switch (state) {
    case Services::PlacementFieldState::Known: return QStringLiteral("known");
    case Services::PlacementFieldState::Absent: return QStringLiteral("absent");
    case Services::PlacementFieldState::Opaque: return QStringLiteral("opaque");
    case Services::PlacementFieldState::Unavailable: return QStringLiteral("unavailable");
    }
    return QStringLiteral("unavailable");
}
QJsonArray PlacementDiagnosticOrigins(const std::vector<Services::PlacementFieldOrigin>& origins, bool bounded = false) {
    QJsonArray result;
    for (std::size_t i = 0; i < origins.size() && (!bounded || i < 4); ++i) {
        const auto& origin = origins[i];
        QJsonObject row{{"byteOffset", origin.byteOffset ? QJsonValue(QString::number(*origin.byteOffset)) : QJsonValue::Null}};
        if (bounded) {
            WorldDiagnosticJson::Text(row, "field", std::string_view(origin.field), 128);
            WorldDiagnosticJson::Text(row, "objectRawcode", std::string_view(origin.objectRawcode), 32);
            WorldDiagnosticJson::Text(row, "sourceId", std::string_view(origin.sourceId), 256);
            WorldDiagnosticJson::Text(row, "sourcePath", std::string_view(origin.sourcePath), 1024);
        } else {
            row["field"] = QString::fromStdString(origin.field); row["objectRawcode"] = QString::fromStdString(origin.objectRawcode);
            row["sourceId"] = QString::fromStdString(origin.sourceId); row["sourcePath"] = QString::fromStdString(origin.sourcePath);
        }
        result.push_back(std::move(row));
    }
    return result;
}
}
QJsonObject WorldModule::ModelDiagnosticProgressSnapshot(std::uint32_t offset, std::uint32_t limit) const {
    limit = std::clamp(limit, 1u, 16u);
    const bool sceneCurrent = InstanceDiagnosticsCurrent();
    const bool captureCurrent = sceneCurrent || EmptyModelCaptureCurrent();
    const auto version = PlacementPresentationVersion();
    QJsonObject binding{{"generation", QString::number(version.document.generation)},
        {"revision", QString::number(version.document.revision)}, {"catalogRevision", QString::number(version.catalogRevision)}};
    WorldDiagnosticJson::Text(binding, "documentId", std::string_view(version.document.documentId));
    WorldDiagnosticJson::Text(binding, "sourceSha256", std::string_view(version.document.sourceSha256), 128);
    QJsonObject result{{"schemaVersion", 1}, {"detail", "bounded-progress"}, {"binding", binding},
        {"sceneCurrent", sceneCurrent}, {"captureCurrent", captureCurrent}, {"documentAvailable", selectionDocument_ != nullptr},
        {"sourceChangeSuspected", project_.sourceChangeSuspected_}, {"sourceChangeConfirmed", project_.sourceChangeConfirmed_},
        {"gpuCompletionProven", false}, {"gameplayVerified", false}, {"modelRowKind", "instance-diagnostic-page"},
        {"terrainOnly", TerrainOnlySceneDiagnosticSnapshot()}};
    if (buildingSplatPresenter_) result["buildingSplat"] = buildingSplatPresenter_->DiagnosticSnapshot(offset, limit);
    if (!captureCurrent) {
        // 旧缓存和加载前的零值都不能充当当前来源的空列表。
        result["unresolvedPlacementCount"] = QJsonValue::Null;
        result["modelRequestCount"] = QJsonValue::Null;
        result["rowsState"] = "pending-or-stale";
        return result;
    }
    QJsonArray unresolved;
    const auto unresolvedBegin = std::min<std::size_t>(offset, unresolvedPlacements_.size());
    for (std::size_t i = unresolvedBegin; i < unresolvedPlacements_.size() && i - unresolvedBegin < limit; ++i) {
        const auto& missing = unresolvedPlacements_[i];
        auto row = PlacementDiagnosticIdentity(missing.owner);
        row["position"] = QJsonArray{missing.position[0], missing.position[1], missing.position[2]};
        row["objectType"] = missing.objectType ? QJsonValue(QString::fromUtf8(ObjectData::ToString(*missing.objectType))) : QJsonValue::Null;
        WorldDiagnosticJson::Text(row, "modelPath", std::string_view(missing.modelPath));
        WorldDiagnosticJson::Text(row, "reasonCode", std::string_view(missing.reasonCode), 256);
        WorldDiagnosticJson::Text(row, "reason", std::string_view(missing.reason));
        const auto origins = PlacementDiagnosticOrigins(missing.origins, true);
        row["origins"] = origins;
        row["originsPage"] = WorldDiagnosticJson::Page(missing.origins.size(), 0, origins.size(), 4);
        unresolved.append(std::move(row));
    }
    result["unresolvedPlacementCount"] = static_cast<double>(unresolvedPlacements_.size());
    result["unresolvedPlacementRows"] = unresolved;
    result["unresolvedPlacementPage"] = WorldDiagnosticJson::Page(unresolvedPlacements_.size(), offset, unresolved.size(), limit);
    result["modelRequestCount"] = static_cast<double>(requests_.size());
    QJsonArray models;
    if (sceneCurrent) {
        const auto frame = animation_->Frame();
        const auto attached = instanceDiagnosticQuery_->SetFrame(frame);
        if (!attached.Succeeded()) {
            result["rowsState"] = "frame-binding-rejected";
            result["diagnostics"] = WorldDiagnosticJson::Diagnostics(attached.diagnostics);
            return result;
        }
        const auto& specs = instanceDiagnosticScene_->specs;
        const auto begin = std::min<std::size_t>(offset, specs.size());
        for (std::size_t i = begin; i < specs.size() && i - begin < limit; ++i) {
            QJsonObject row{{"requestIndex", static_cast<double>(i)}};
            const auto capturedBinding = instanceDiagnosticQuery_->Binding(static_cast<std::uint32_t>(i), specs[i].identity);
            const auto queried = capturedBinding.Succeeded() ? instanceDiagnosticQuery_->Query(*capturedBinding.value, 0, 4)
                : Result<Render::AnimationDiagnosticPage>{std::nullopt, capturedBinding.diagnostics};
            if (!queried.Succeeded()) {
                row["queryRejected"] = true; row["diagnostics"] = WorldDiagnosticJson::Diagnostics(queried.diagnostics, 4);
                models.append(std::move(row)); continue;
            }
            const auto& page = *queried.value;
            WorldDiagnosticJson::Text(row, "modelPath", std::string_view(page.modelPath));
            WorldDiagnosticJson::Text(row, "hostKey", std::string_view(page.hostKey));
            row["sceneVersion"] = QString::number(page.binding.sceneVersion);
            row["contentVersion"] = QString::number(page.binding.contentVersion);
            row["identity"] = QString::number(page.binding.identity.value);
            row["identityGeneration"] = QString::number(page.binding.identity.generation);
            row["hidden"] = page.hidden; row["unavailable"] = page.unavailable; row["partial"] = page.partial;
            row["noVisibleGeometry"] = page.noVisibleGeometry;
            QJsonArray issues;
            for (const auto& issue : page.issues) {
                QJsonObject item;
                WorldDiagnosticJson::Text(item, "stage", std::string_view(issue.stage), 128);
                WorldDiagnosticJson::Text(item, "kind", std::string_view(issue.kind), 128);
                WorldDiagnosticJson::Text(item, "code", std::string_view(issue.code), 256);
                WorldDiagnosticJson::Text(item, "message", std::string_view(issue.message));
                issues.append(std::move(item));
            }
            row["issues"] = issues; row["issuesPage"] = WorldDiagnosticJson::Page(page.total, page.offset, issues.size(), 4);
            models.append(std::move(row));
        }
        result["sceneVersion"] = QString::number(instanceDiagnosticScene_->sceneVersion);
        result["contentVersion"] = QString::number(frame->contentVersion);
    }
    result["rowsState"] = "current";
    result["modelRows"] = models;
    result["modelRowsPage"] = WorldDiagnosticJson::Page(
        sceneCurrent ? instanceDiagnosticScene_->specs.size() : 0, offset, models.size(), limit);
    return result;
}

QJsonObject WorldModule::ModelDiagnosticSnapshot() const {
    QJsonObject result{{"schemaVersion", 1}, {"sceneCurrent", InstanceDiagnosticsCurrent()},
        {"sourceChangeSuspected", project_.sourceChangeSuspected_}, {"sourceChangeConfirmed", project_.sourceChangeConfirmed_},
        {"gpuCompletionProven", false}, {"gameplayVerified", false}};
    const auto document = CurrentDocument(); if (!document) { result["documentAvailable"] = false; return result; }
    result["documentAvailable"] = true;
    const auto version = PlacementPresentationVersion();
    result["binding"] = QJsonObject{{"documentId", QString::fromStdString(version.document.documentId)},
        {"sourceSha256", QString::fromStdString(version.document.sourceSha256)}, {"generation", QString::number(version.document.generation)},
        {"revision", QString::number(version.document.revision)}, {"catalogRevision", QString::number(version.catalogRevision)}};
    const auto captured = CaptureWorldPlacements(project_, *document);
    QJsonArray unresolved;
    for (const auto& missing : captured.unresolvedRows) {
        auto row = PlacementDiagnosticIdentity(missing.owner);
        row["position"] = QJsonArray{missing.position[0], missing.position[1], missing.position[2]};
        row["objectType"] = missing.objectType ? QJsonValue(QString::fromUtf8(ObjectData::ToString(*missing.objectType))) : QJsonValue::Null;
        row["modelPath"] = QString::fromStdString(missing.modelPath); row["reasonCode"] = QString::fromStdString(missing.reasonCode);
        row["reason"] = QString::fromStdString(missing.reason); row["origins"] = PlacementDiagnosticOrigins(missing.origins);
        unresolved.push_back(row);
    }
    result["unresolvedPlacementCount"] = static_cast<double>(captured.unresolved); result["unresolvedPlacementRows"] = unresolved;
    const auto splats = CaptureWorldBuildingSplats(project_, *document, version);
    if (splats.Succeeded()) {
        QJsonArray fields;
        for (const auto& observed : splats.value->fieldDiagnostics) fields.push_back(QJsonObject{{"rawcode", QString::fromStdString(observed.rawcode)},
            {"placements", static_cast<double>(observed.placements)}, {"uubsState", FieldStateName(observed.uubs.state)},
            {"uubsValue", QString::fromStdString(observed.uubs.value)}, {"reason", QString::fromStdString(observed.uubs.reason)},
            {"origins", PlacementDiagnosticOrigins(observed.origins)},
            {"configuration", observed.configuration == Services::BuildingSplatConfiguration::NotApplicableToNonBuilding
                ? "not-applicable-to-non-building" : "effective-field"},
            {"applicabilityReason", QString::fromStdString(observed.applicabilityReason)},
            {"applicabilityOrigins", PlacementDiagnosticOrigins(observed.applicabilityOrigins)}});
        result["buildingSplatInputCount"] = static_cast<double>(splats.value->units.size()); result["buildingSplatFieldRows"] = fields;
    }
    if (buildingSplatPresenter_) result["buildingSplat"] = buildingSplatPresenter_->DiagnosticSnapshot();
    QJsonArray groups;
    for (const auto& row : ModelDiagnosticRows()) {
        QJsonArray owners; for (const auto& owner : row.owners) owners.push_back(PlacementDiagnosticIdentity(owner));
        groups.push_back(QJsonObject{{"modelPath", row.modelPath}, {"ownerText", row.ownerText}, {"reasonCode", row.reasonCode},
            {"reason", row.reason}, {"placementCount", static_cast<double>(row.owners.size())}, {"records", static_cast<double>(row.records)},
            {"source", row.sourceText}, {"owners", owners}, {"placeholderState", row.placeholderState}});
    }
    result["modelRows"] = groups; return result;
}

Result<QJsonObject> WorldModule::ModelBugPlaceholderCaptureSnapshot(const Render::CapturedFrameRgba& capture) const try {
    using Output = Result<QJsonObject>;
    const auto fail = [](std::string message) { return Output::Failure("World.BugPlaceholder",
        "WORLD_BUG_PLACEHOLDER_CAPTURE_SOURCE_STALE", std::move(message)); };
    if (QThread::currentThread() != thread() || !InstanceDiagnosticsCurrent() || !bugPlaceholderPresenter_
        || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_)
        return fail("占位GPU读回不属于当前所属线程、地图来源或已接受场景；没有生成证明。");
    const auto scene = animation_->Scene();
    const auto frame = animation_->Frame();
    const auto registry = instanceRegistry_;
    const auto diagnostics = instanceDiagnosticQuery_;
    auto prepared = bugPlaceholderPresenter_->Snapshot();
    if (!registry && (!prepared || !prepared->readOnlySource)) {
        if (!scene || !frame || !unresolvedPlacements_.empty())
            return fail("只读场景仍有未解析摆放或缺少当前帧，不能证明无需占位。");
        auto absence = Services::PlanWorldModelBugPlaceholderAbsence(scene, *frame);
        if (!absence.Succeeded()) return {std::nullopt, std::move(absence.diagnostics)};
        prepared = std::make_shared<const Services::WorldModelBugPlaceholderPlan>(std::move(*absence.value));
    }
    const auto expected = PlacementPresentationVersion();
    const auto mapKey = MapKey(); const auto generation = generation_;
    if (!scene || !frame) return fail("占位捕获缺少当前主场景或动画帧。");
    if (!prepared) {
        std::string message = "占位捕获尚无当前来源的准备计划。";
        for (const auto& diagnostic : bugPlaceholderPresenter_->LastStatus().diagnostics)
            message += diagnostic.code + ": " + diagnostic.message + "；";
        return fail(std::move(message));
    }
    const auto readOnly = prepared->readOnlySource;
    if (readOnly) {
        const auto document = CurrentDocument();
        if (!document) return fail("只读占位缺少当前原始摆放文档。");
        auto current = Services::WorldModelBugReadOnlySource::Capture(*document, scene, readOnly->Binding(), placementIds_);
        if (!current.Succeeded()) return {std::nullopt, std::move(current.diagnostics)};
        if ((*current.value)->Members() != readOnly->Members() || (*current.value)->Placements() != readOnly->Placements())
            return fail("只读占位原始成员SHA、记录索引或资产绑定已变化。");
    }
    // 每个身份仍独立拒绝；精确说明失配项，避免大图失败只剩一条合并原因。
    const std::pair<const char*, bool> checks[]{
        {"plan.scene", prepared->sourceScene == scene},
        {"plan.registry", prepared->registry == registry},
        {"plan.binding", !registry || prepared->binding == registry->Binding()},
        {"capture.frame", capture.animation == frame},
        {"viewport.frame", viewport_->AnimationTarget() == frame},
        {"diagnostic.scene", instanceDiagnosticScene_ == scene},
        {"diagnostic.registry", instanceDiagnosticRegistry_ == registry},
        {"selection.version", worldSelection_.Version() && *worldSelection_.Version() == expected},
        {"source.identity", prepared->binding.sourceIdentity == expected.document.sourceSha256},
        {"source.generation", prepared->binding.sourceGeneration == scene->generation},
        {"map.revision", prepared->binding.mapRevision == expected.document.revision},
        {"edit.session", (!registry && !readOnly) || prepared->binding.editSessionIdentity == (project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0)},
        {"edit.revision", (!registry && !readOnly) || prepared->binding.editRevision == (project_.mapEdits_ ? project_.mapEdits_->Revision() : 0)},
    };
    for (const auto& [name, current] : checks) if (!current)
        return fail(std::string("占位捕获身份不符：") + name + "；没有生成完成证明。");
    const auto stillCurrent = [&] {
        if (!InstanceDiagnosticsCurrent() || MapKey() != mapKey || generation_ != generation
            || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_
            || animation_->Scene() != scene || animation_->Frame() != frame || viewport_->AnimationTarget() != frame
            || instanceRegistry_ != registry || instanceDiagnosticQuery_ != diagnostics
            || ((registry || readOnly) && bugPlaceholderPresenter_->Snapshot() != prepared)
            || (!registry && !unresolvedPlacements_.empty())
            || !worldSelection_.Version() || *worldSelection_.Version() != expected) return false;
        const auto actual = PlacementPresentationVersion();
        return actual.document == expected.document && actual.catalogRevision == expected.catalogRevision;
    };
    const Services::WorldModelBugFlagsReader flags = [&](const MapData::PlacementId& owner)
        -> std::optional<Services::WorldEditorFlags> {
        if (!stillCurrent() || (registry && !registry->Find(owner))) return std::nullopt;
        return worldSelection_.Flags(Services::WorldObjectId{owner});
    };
    auto actualPlan = Services::PlanWorldModelBugPlaceholderAbsence(scene, *capture.animation);
    if (registry) actualPlan = Services::PlanWorldModelBugPlaceholders(scene, *capture.animation, registry, placementIds_, flags);
    else if (readOnly) {
        const auto observed = Services::CaptureWorldModelBugPlaceholderFrame(scene, *capture.animation);
        if (!observed.Succeeded()) return {std::nullopt, observed.diagnostics};
        actualPlan = Services::PlanWorldModelBugReadOnlyPlaceholders(scene, *observed.value, readOnly, flags);
    }
    if (!actualPlan.Succeeded()) return {std::nullopt, std::move(actualPlan.diagnostics)};
    if (actualPlan.value->binding != prepared->binding || actualPlan.value->markers != prepared->markers)
        return fail("实际读回帧的故障、位置或可选状态与准备计划不同；没有把旧准备内容冒充当前画面。");
    const auto qualified = Services::QualifyWorldModelBugPlaceholderCapture(*prepared, capture);
    if (!qualified.Succeeded()) return {std::nullopt, qualified.diagnostics};
    const auto& facts = *qualified.value;
    const auto number = [](std::uint64_t value) { return QString::number(value); };
    QJsonArray markers;
    for (const auto& marker : actualPlan.value->markers) {
        if (marker.request >= scene->specs.size() || marker.request >= frame->states.size())
            return fail("占位对应的原始模型请求或状态范围已失效。");
        const auto& spec = scene->specs[marker.request]; const auto& state = frame->states[marker.request];
        auto row = PlacementDiagnosticIdentity(marker.owner);
        row["identity"] = number(marker.identity.value); row["sourceRequest"] = static_cast<double>(marker.request);
        row["hostKey"] = QString::fromStdString(spec.hostKey); row["modelPath"] = QString::fromStdString(marker.modelPath);
        row["reasonCode"] = QString::fromStdString(marker.reasonCode); row["reason"] = QString::fromStdString(marker.reason);
        row["position"] = QJsonArray{marker.position[0], marker.position[1], marker.position[2]};
        row["selectable"] = marker.selectable; row["mapSourceSha256"] = QString::fromStdString(scene->sourceIdentity);
        row["sequenceIndex"] = static_cast<double>(state.sequenceIndex);
        row["elapsedMs"] = number(state.elapsedMs); row["globalMs"] = number(state.globalMs);
        const auto source = diagnostics->Source(marker.request, marker.identity);
        if (!source.Succeeded()) return {std::nullopt, source.diagnostics};
        row["modelSourceKnown"] = source.value->has_value();
        row["modelSource"] = *source.value ? QJsonValue(QJsonObject{
            {"path", QString::fromStdString((**source.value).virtualPath)},
            {"layer", QString::fromStdString((**source.value).sourceLayer)},
            {"sha256", QString::fromStdString((**source.value).contentDigest)}}) : QJsonValue(QJsonValue::Null);
        markers.push_back(std::move(row));
    }
    QJsonObject output{{"schemaVersion", 1}, {"realSourceBound", true}, {"syntheticFixture", false},
        {"captureSource", "renderer_completed_frame"}, {"sceneCurrent", true},
        {"sourceChangeSuspected", false}, {"sourceChangeConfirmed", false},
        {"instanceLineageAvailable", registry != nullptr}, {"cpuFrameProvesNoPlaceholderDemand", !registry && !readOnly},
        {"planKind", readOnly ? "read-only-known-placements-v1" : registry ? "editable-lineage-v1" : "absence-v1"},
        {"gpuLayerCaptured", facts.layerCaptured}, {"gpuLayerCompletionProven", facts.layerCaptured},
        {"absenceReason", facts.layerCaptured ? QJsonValue(QJsonValue::Null) : QJsonValue("no-marker-for-captured-frame")},
        {"visibleAcceptance", false}, {"selectionAcceptance", false}, {"gameplayVerified", false},
        {"markerCount", static_cast<double>(facts.markerCount)}, {"markers", markers},
        {"frameNumber", number(facts.frameCount)}, {"parentModelEpoch", number(facts.parentModelEpoch)},
        {"layersRevision", number(facts.layersRevision)},
        {"layerId", number(Services::WorldModelBugPlaceholderLayerId)}, {"layerRevision", number(facts.layerRevision)},
        {"layerSceneVersion", number(facts.layerSceneVersion)}, {"layerContentVersion", number(facts.layerContentVersion)},
        {"preparedOriginalContentVersion", number(facts.preparedContentVersion)},
        {"capturedOriginalContentVersion", number(facts.sourceContentVersion)},
        {"originalSceneVersion", number(scene->sceneVersion)},
        {"binding", QJsonObject{{"documentId", QString::fromStdString(expected.document.documentId)},
            {"sourceSha256", QString::fromStdString(expected.document.sourceSha256)},
            {"generation", number(expected.document.generation)}, {"revision", number(expected.document.revision)},
            {"catalogRevision", number(expected.catalogRevision)}, {"editorRevision", number(expected.editorRevision)},
            {"sourceGeneration", number(prepared->binding.sourceGeneration)},
            {"editSessionIdentity", number(prepared->binding.editSessionIdentity)},
            {"editRevision", number(prepared->binding.editRevision)}}}};
    if (readOnly) {
        QJsonArray members;
        for (const auto& member : readOnly->Members()) members.push_back(QJsonObject{
            {"member", member.kind == MapDataKind::Units ? "war3mapUnits.doo" : "war3map.doo"},
            {"sourceState", member.state == MapDataState::Missing ? "missing" : member.state == MapDataState::Partial ? "partial" : "ready"},
            {"sourceSha256", member.sourceSha256.empty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QString::fromStdString(member.sourceSha256))},
            {"parsedRecordCount", static_cast<double>(member.parsedRecords)},
            {"initializationIdentityAmbiguous", member.initializationIdentityAmbiguous},
            {"boundRequestCount", static_cast<double>(member.boundRequests)}});
        output["sourceCoverage"] = QJsonObject{{"scope", "known-placement-records-only"}, {"version", 1},
            {"members", members}, {"fullMapModelCompleteness", false}, {"editable", false},
            {"unknownPlacements", static_cast<double>(unresolvedPlacements_.size())},
            {"snapshotBytes", static_cast<double>(readOnly->OwnedBytes())}};
    }
    for (const auto& layer : capture.modelLayers) if (layer.layerId == Services::WorldModelBugPlaceholderLayerId) {
        output["staticUploadCount"] = number(layer.skin.staticUploadCount);
        output["matrixUploadCount"] = number(layer.skin.matrixUploadCount);
        output["layerDrawFrameNumber"] = number(layer.skin.frameCount);
    }
    if (!stillCurrent()) return fail("占位读回资格核验期间来源、谱系、显隐或准备计划变化；结果已丢弃。");
    return Output::Success(std::move(output));
} catch (...) {
    return Result<QJsonObject>::Failure("World.BugPlaceholder", "WORLD_BUG_PLACEHOLDER_CAPTURE_FAILED",
        "无法拥有占位GPU读回证据；没有使用CPU准备数或事后查询伪造完成证明。");
}

Result<QJsonObject> WorldModule::ModelPremCaptureSnapshot(const Render::CapturedFrameRgba& capture) const {
    if (QThread::currentThread() != thread() || !InstanceDiagnosticsCurrent() || !premPresenter_ || !animation_
        || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_ || animation_->Frame() != capture.animation
        || !viewport_ || viewport_->AnimationTarget() != capture.animation)
        return Result<QJsonObject>::Failure("World.PREM", "WORLD_PREM_CAPTURE_SOURCE_STALE",
            "PREM 捕获未绑定当前所属线程、来源、实例谱系或已接受父帧。");
    const auto scene = animation_->Scene();
    const auto frame = animation_->Frame();
    if (!scene || !frame || !instanceDiagnosticVersion_
        || scene != instanceDiagnosticScene_ || scene->sourceIdentity != instanceDiagnosticVersion_->document.sourceSha256
        || scene->documentRevision != instanceDiagnosticVersion_->document.revision)
        return Result<QJsonObject>::Failure("World.PREM", "WORLD_PREM_CAPTURE_SOURCE_STALE",
            "PREM 捕获的CPU场景不属于当前已绑定地图来源与修订。");
    if (!frame->prem) return Result<QJsonObject>::Failure("World.PREM", "WORLD_PREM_CAPTURE_PLAN_MISSING",
        "当前父帧没有PREM准备计划，不能将未知当作空子层。");
    // 只读地图可能没有可编辑的实例谱系。空槽由当前CPU计划和同帧层清单共同证明；
    // 有槽仍走Presenter原有的所属对象、显隐、子矩阵与GPU完成门，不能借空层路径放行。
    if (frame->prem->childSlots.empty())
        return WorldPremPresenter::CaptureEmptySnapshot(capture, scene, frame, viewport_->DocumentRevision());
    return premPresenter_->CaptureSnapshot(capture);
}

bool WorldModule::ModelDiagnosticsReady() const {
    if (!page_ || !viewport_ || !buildingSplatPresenter_ || !selectionMapData_
        || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_
        || !buildingSplatPresenter_->DiagnosticPreparationReady()) return false;
    if (InstanceDiagnosticsCurrent()) return true;
    return EmptyModelCaptureCurrent();
}

bool WorldModule::EmptyModelCaptureCurrent() const {
    if (!page_ || !viewport_ || !selectionMapData_
        || project_.sourceChangeSuspected_ || project_.sourceChangeConfirmed_) return false;
    // 仅真实捕获并完成的空模型场景可走此门；启动前/失败时的空容器不表示就绪。
    const auto version = PlacementPresentationVersion();
    return emptyModelDiagnosticVersion_ && emptyModelDiagnosticMapKey_ == MapKey()
        && emptyModelDiagnosticGeneration_ == generation_ && !modelWorker_ && !placementsDirty_
        && !scene_ && requests_.empty() && (!animation_ || !animation_->Ready())
        && selectionMapData_->Binding() == emptyModelDiagnosticVersion_->document
        && version.document == emptyModelDiagnosticVersion_->document
        && version.catalogRevision == emptyModelDiagnosticVersion_->catalogRevision
        && version.document.generation == std::max<std::uint64_t>(1, project_.editRevision_.Generation())
        && version.document.revision == (project_.mapEdits_ ? project_.mapEdits_->Revision() : 0);
}

WorldModule::TerrainOnlyFacts WorldModule::QualifyTerrainOnlyScene() const {
    TerrainOnlyFacts facts;
    if (!ViewportForTools() || !selectionDocument_ || !EmptyModelCaptureCurrent()
        || (cliffs_ && !cliffs_->meshes.empty())) return facts;
    const auto& document = *selectionDocument_;
    const auto& units = document.Member(MapDataKind::Units);
    const auto& doodads = document.Member(MapDataKind::Doodads);
    const auto unitCount = document.Units() ? document.Units()->units.size() : 0;
    const auto doodadCount = document.Doodads()
        ? document.Doodads()->doodads.size() + document.Doodads()->specialDoodads.size() : 0;
    facts.unitsVerified = (units.state == MapDataState::Missing && !document.Units())
        || (units.state == MapDataState::Ready && document.Units() && units.recordCount == unitCount);
    facts.doodadsVerified = (doodads.state == MapDataState::Missing && !document.Doodads())
        || (doodads.state == MapDataState::Ready && document.Doodads() && doodads.recordCount == doodadCount);
    if (!facts.unitsVerified || !facts.doodadsVerified) {
        facts.reason = "WORLD_EMPTY_MODEL_SOURCE_INCOMPLETE"; return facts;
    }
    if (!unresolvedPlacements_.empty()) {
        facts.reason = "WORLD_EMPTY_MODEL_UNRESOLVED_PLACEMENTS"; return facts;
    }
    if (doodadCount != 0) {
        facts.reason = "WORLD_EMPTY_MODEL_DOODADS_NOT_REPRESENTED"; return facts;
    }
    if (document.Units()) for (const auto& unit : document.Units()->units) {
        // CaptureWorldPlacements / CaptureWorldBuildingSplats 都明确排除原版起点。
        // 只承认逐条验证过的 sloc，不用 requests.empty() 推断单位来源为空。
        if (unit.rawcode != "sloc") {
            facts.reason = "WORLD_EMPTY_MODEL_UNITS_NOT_REPRESENTED"; return facts;
        }
        ++facts.startLocations;
    }
    facts.ready = true;
    facts.reason = facts.startLocations ? "WORLD_START_LOCATIONS_HAVE_NO_MODEL" : "WORLD_PLACEMENT_SOURCES_EMPTY";
    return facts;
}

bool WorldModule::TerrainOnlySceneReady() const { return QualifyTerrainOnlyScene().ready; }

QJsonObject WorldModule::TerrainOnlySceneDiagnosticSnapshot() const {
    const auto facts = QualifyTerrainOnlyScene();
    QJsonObject result{{"ready", facts.ready}, {"reason_code", facts.reason},
        {"units_source_verified", facts.unitsVerified}, {"doodads_source_verified", facts.doodadsVerified},
        {"source_start_location_count", static_cast<double>(facts.startLocations)},
        {"model_requests", static_cast<double>(requests_.size())},
        {"unresolved_placements", static_cast<double>(unresolvedPlacements_.size())},
        {"start_location_model_exclusion", "original-start-location-not-a-placed-model"},
        {"start_location_splat_exclusion", "original-start-location-not-a-building"},
        {"start_location_marker_implemented", true}, {"start_location_marker_visual_verified", false},
        {"start_location_marker_source_current", StartLocationsCurrent()},
        {"start_location_marker_overlay_submitted", startLocationOverlaySubmitted_},
        {"start_location_marker_count", static_cast<double>(startLocationMarkers_.markers.size())},
        {"start_location_marker_omitted", static_cast<double>(startLocationMarkers_.omittedCount)},
        {"completion_proven", false}};
    if (selectionDocument_) {
        const auto stateName = [](MapDataState state) {
            switch (state) {
            case MapDataState::Missing: return "missing";
            case MapDataState::Ready: return "ready";
            case MapDataState::Partial: return "partial";
            case MapDataState::Invalid: return "invalid";
            case MapDataState::Ambiguous: return "ambiguous";
            case MapDataState::Unreadable: return "unreadable";
            }
            return "unknown";
        };
        result["units_source_state"] = stateName(selectionDocument_->Member(MapDataKind::Units).state);
        result["doodads_source_state"] = stateName(selectionDocument_->Member(MapDataKind::Doodads).state);
    }
    return result;
}

void WorldModule::FinishEmptyModelCapture() {
    emptyModelDiagnosticVersion_ = PlacementPresentationVersion(); emptyModelDiagnosticMapKey_ = MapKey();
    emptyModelDiagnosticGeneration_ = generation_;
    const auto facts = QualifyTerrainOnlyScene();
    if (facts.ready) RecordModelLoad("captured-no-renderable-models");
    else RecordModelLoad("empty-capture-rejected", Status::Failure("World", facts.reason,
        "模型捕获已结束，但摆放来源不完整或存在未表示的摆放；不能把空请求当作纯地形成功。").diagnostics, true);
}
Result<WorldModelDiagnosticsCapture> WorldModule::CaptureModelDiagnosticsView() {
    if (!ModelDiagnosticsReady()) return Result<WorldModelDiagnosticsCapture>::Failure(
        "World.Diagnostics", "WORLD_DIAGNOSTIC_CAPTURE_NOT_READY", "模型诊断来源/修订或建筑准备尚未完成；没有发布截图。");
    const auto document = CurrentDocument();
    if (!document) return Result<WorldModelDiagnosticsCapture>::Failure(
        "World.Diagnostics", "WORLD_DIAGNOSTIC_CAPTURE_DOCUMENT", "真实地图文档不可用；没有用原创列表替代。");
    const auto expected = PlacementPresentationVersion();
    const auto mapKey = MapKey(); const auto generation = generation_;
    const auto scene = animation_ ? animation_->Scene() : nullptr;
    const auto frame = animation_ ? animation_->Frame() : nullptr;
    const auto rows = ModelDiagnosticRows();
    const QPointer<WorldModule> self(this);
    const auto stillCurrent = [&] {
        if (!self || !self->ModelDiagnosticsReady() || self->MapKey() != mapKey || self->generation_ != generation) return false;
        const auto current = self->PlacementPresentationVersion();
        return current.document == expected.document && current.catalogRevision == expected.catalogRevision
            && (!self->animation_ || (self->animation_->Scene() == scene && self->animation_->Frame() == frame));
    };
    QRect screens;
    for (const auto* screen : QApplication::screens()) screens = screens.united(screen->geometry());
    if (screens.isEmpty()) return Result<WorldModelDiagnosticsCapture>::Failure(
        "World.Diagnostics", "WORLD_DIAGNOSTIC_CAPTURE_SCREEN", "没有可核对的屏幕几何；没有显示或捕获列表。");
    // 无parent的栈窗口只由本调用拥有，模块销毁不会在嵌套事件中删除它。
    WorldModelDiagnosticsDialog dialog;
    dialog.setAttribute(Qt::WA_ShowWithoutActivating);
    dialog.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    dialog.SetRows(rows, {}); // 明确没有定位回调，也不发送任何系统输入。
    dialog.move(screens.left() - dialog.width() - 256, screens.top() - dialog.height() - 256);
    dialog.show();
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    if (!stillCurrent()) return Result<WorldModelDiagnosticsCapture>::Failure(
        "World.Diagnostics", "WORLD_DIAGNOSTIC_CAPTURE_STALE", "布局期间地图/场景/来源已变化；没有发布陈旧截图。");
    WorldModelDiagnosticsCapture capture;
    capture.exportText = WorldModelDiagnosticText(rows);
    const auto textBytes = capture.exportText.toUtf8();
    auto textSha = Sha256Text(std::string_view(textBytes.constData(), static_cast<std::size_t>(textBytes.size())));
    if (!textSha.Succeeded()) return Result<WorldModelDiagnosticsCapture>::Failure(
        "World.Diagnostics", "WORLD_DIAGNOSTIC_CAPTURE_TEXT_SHA", "完整诊断文本身份计算失败；没有发布截图。");
    capture.image = dialog.grab().toImage();
    if (capture.image.isNull() || !stillCurrent()) return Result<WorldModelDiagnosticsCapture>::Failure(
        "World.Diagnostics", "WORLD_DIAGNOSTIC_CAPTURE_STALE", "截图为空或地图/场景/来源已变化；没有发布结果。");
    capture.binding = self->ModelDiagnosticSnapshot();
    capture.binding["sceneCurrent"] = self->InstanceDiagnosticsCurrent();
    capture.binding["emptySceneCompleted"] = !scene && self->emptyModelDiagnosticVersion_.has_value();
    capture.binding["realSourceBound"] = true;
    capture.binding["syntheticFixture"] = false;
    capture.binding["widgetVisualEvidence"] = true;
    capture.binding["authorVisibleAcceptance"] = false;
    capture.binding["locatePerformed"] = false;
    capture.binding["exportTextSha256"] = QString::fromStdString(*textSha.value);
    capture.binding["sceneVersion"] = scene ? QJsonValue(QString::number(scene->sceneVersion)) : QJsonValue::Null;
    capture.binding["contentVersion"] = frame ? QJsonValue(QString::number(frame->contentVersion)) : QJsonValue::Null;
    capture.binding["rowCount"] = static_cast<double>(rows.size());
    capture.binding["geometry"] = QJsonObject{{"logicalWidth", dialog.width()}, {"logicalHeight", dialog.height()},
        {"pixelWidth", capture.image.width()}, {"pixelHeight", capture.image.height()}, {"dpr", capture.image.devicePixelRatio()}};
    if (!stillCurrent()) return Result<WorldModelDiagnosticsCapture>::Failure(
        "World.Diagnostics", "WORLD_DIAGNOSTIC_CAPTURE_STALE", "诊断取证期间来源/修订变化；没有发布结果。");
    return Result<WorldModelDiagnosticsCapture>::Success(std::move(capture));
}

Services::WorldSelectionVersion WorldModule::PlacementPresentationVersion() const {
    const auto context = SelectionContext();
    return {context.binding, context.catalogRevision, worldSelection_.Version() ? worldSelection_.Version()->editorRevision : 0};
}
void WorldModule::StartAnimatedModels(std::uint64_t generation) {
    RecordModelLoad("capturing-animated");
    if (bugPlaceholderPresenter_) bugPlaceholderPresenter_->Clear();
    if (premPresenter_) premPresenter_->Clear();
    emptyModelDiagnosticVersion_.reset();
    instanceDiagnosticVersion_.reset(); RefreshInstanceDiagnostics(); // 加载/等待期间不沿用旧实例原因。
    if (modelWorker_) { RecordModelLoad("waiting-for-model-worker"); modelGeneration_=generation; placementsDirty_=true; return; }
    if (!viewport_->VertexCount()) { RecordModelLoad("waiting-for-terrain"); modelGeneration_=generation; placementsDirty_=true; return; }
    const auto document=CurrentDocument();
    if (!document) {
        ReportModelPreparationFailure(tr("地图数据不可用，未显示摆放模型"), Status::Failure("World",
            "WORLD_MODEL_DOCUMENT_UNAVAILABLE", "当前地图文档不可用，不能捕获摆放模型。").diagnostics); return;
    }
    CaptureAuxiliary(*document); RefreshOutliner();
    RefreshBuildingSplats(false, &*document); // 先捕获typed uubs，不由模型可用性决定贴画来源。
    auto placed=std::make_shared<WorldPlacementCapture>(CaptureWorldPlacements(project_,*document));
    (void)ApplyModelVariantFallbacks(*placed, modelVariants_);
    placementsDirty_=false; // 此次捕获已包含先前修改；仅启动后新增的修改要求再加载。
    const auto cliffs=cliffs_;
    // 有摆放模型时悬崖放独立层：之后的地形笔刷只重建悬崖层。没有模型时主场景只能装悬崖（附加层需要父场景）。
    const bool cliffLayer = cliffLayer_ && viewport_->HasModelLayerSupport() && !placed->requests.empty();
    if (placed->requests.empty() && (!cliffs || cliffs->meshes.empty())) {
        animation_->Clear(); (void)viewport_->SetModelScene(nullptr); scene_.reset();
        cliffsInLayer_=false; if (cliffLayer_) cliffLayer_->Clear();
        requests_=placed->requests; placementIds_=placed->ids; unresolvedPlacements_=placed->unresolvedRows;
        FinishEmptyModelCapture();
        modelStatus_->setText(tr("无模型")); UpdateOverlay(); return;
    }
    auto sources=CaptureWorldTerrainSources(project_);
    if (!sources.Succeeded()) { ReportModelPreparationFailure(tr("模型资源来源准备失败"), sources.diagnostics); return; }
    const bool retainCurrent = animation_->CanRetainCurrentScene();
    if (!retainCurrent) {
        viewport_->InvalidateAnimationPicking();
        animation_->Clear(); // 地图/修订或真实来源变化时，旧模型必须失效。
    }
    UpdateInspector();
    modelStatus_->setText(retainCurrent ? tr("正在准备新模型，当前世界继续播放…") : tr("正在准备动画模型…"));
    const auto revision=project_.mapEdits_?project_.mapEdits_->Revision():0;
    const auto mapKey=MapKey(); const auto epoch=Render::NextAnimationSerial();
    auto revisionGate = std::make_shared<std::uint64_t>(revision);
    Render::AnimationInstanceSpec defaults; defaults.globalMs = 1000;
    auto registered = Services::WorldInstanceRegistry::Create(*document,
        {epoch, revision, project_.mapEdits_ ? project_.mapEdits_->SessionIdentity() : 0, revision, sources.value->archiveSha},
        placed->requests, placed->ids, defaults);
    // 不可编辑或歧义的旧来源仍允许只读世界预览；正式增量资格独立报告。
    std::shared_ptr<const Services::WorldInstanceRegistry> registry = registered.Succeeded() ? *registered.value : nullptr;
    const auto selectionSources = *sources.value;
    const auto worldView=viewport_->AnimationViewSnapshot();
    const bool particles=viewport_->HasParticleSupport();
    const auto presentationVersion = PlacementPresentationVersion();
    auto helperIdentity = WorldPathingHelperResourceIdentity(*sources.value, presentationVersion.catalogRevision);
    if (!helperIdentity.Succeeded()) {
        ReportModelPreparationFailure(tr("动画模型的辅助物资源身份准备失败"), helperIdentity.diagnostics); return;
    }
    const auto helperCache = pathingHelperCache_;
    struct Work {
        std::shared_ptr<Render::ModelAnimationService> service;
        std::shared_ptr<WorldResourceReader> source;
        std::shared_ptr<const Render::AnimationFrame> frame;
        std::vector<Diagnostic> diagnostics;
        std::optional<WorldPathingHelperPrepared> helpers;
        std::optional<WorldCliffLayerFrame> cliffLayer;
        ModelVariantFallbacks variants;
    };
    auto work=std::make_shared<Work>();
    auto* worker=QThread::create([work,sources=std::move(*sources.value),placed,cliffs,revision,epoch,worldView,particles,registry,
        presentationVersion, helperIdentity = std::move(*helperIdentity.value), helperCache, cliffLayer] {
        try {
            auto reader=WorldResourceReader::Open(sources);
            if (!reader.Succeeded()) { work->diagnostics=std::move(reader.diagnostics); return; }
            work->source=std::shared_ptr<WorldResourceReader>(std::move(*reader.value));
            ResolveModelVariantFallbacks(*placed, work->source->Resolver(), work->variants);
            (void)ApplyModelVariantFallbacks(*placed, work->variants);
            Services::PlacementPresentationContext helperContext{presentationVersion, helperIdentity,
                WorldPathingHelperResourceReader(work->source)};
            auto helpers = PrepareWorldPathingHelpers(placed->requests, placed->presentations, helperContext, work->source, helperCache);
            if (!helpers.Succeeded()) { work->diagnostics = std::move(helpers.diagnostics); return; }
            work->helpers = std::move(*helpers.value);
            std::vector<Render::AnimationInstanceSpec> specs;
            if (registry) {
                specs = registry->CapturedSpecs();
                for (std::size_t i = 0; i < specs.size(); ++i) specs[i].model = placed->requests[i];
            }
            else for (std::size_t i=0;i<placed->requests.size();++i) {
                const auto& id=placed->ids[i];
                Render::AnimationInstanceSpec spec; spec.identity={i+1,epoch};
                spec.hostKey=WorldPlacementHostKey(id);
                spec.model=placed->requests[i];
                // 局部相位的粒子回放需要同样长的全局历史；使用共同且有界的预览起点。
                spec.elapsedMs=0; spec.globalMs=1000;
                specs.push_back(std::move(spec));
            }
            auto limits=Render::InteractiveAnimationPreviewLimits();
            limits.retainUnsupportedStatic=true; limits.enableParticles=particles;
            limits.enablePremPreview=true;
            // 整张世界共用一份帧预算：上万实例的姿势会占满默认 64 MiB，粒子几乎全被预算拒绝。
            // 世界页放宽到 256 MiB（仍是有界活跃预算，独立预览/缩略图保持原默认）。
            limits.maxFrameBytes=256ull*1024*1024;
            const auto read=[&](auto path,auto maximum){return work->source->ReadAnimationResource(path,maximum);};
            auto service=Render::ModelAnimationService::Create({epoch,revision,sources.archiveSha},specs,read,limits,
                cliffs&&!cliffLayer?std::span<const Render::BakedModelMesh>(cliffs->meshes):std::span<const Render::BakedModelMesh>{});
            if (!service.Succeeded()) { work->diagnostics=std::move(service.diagnostics); return; }
            work->service=std::move(*service.value);
            if (cliffLayer) {
                auto layer=PrepareWorldCliffLayer(cliffs,read,{epoch,revision,sources.archiveSha});
                if (!layer.Succeeded()) { work->diagnostics=std::move(layer.diagnostics); return; }
                work->cliffLayer=std::move(*layer.value);
            }
            Render::AnimationAdvance first; first.worldView=worldView;
            first.worldCamera=Render::AnimationWorldCamera(worldView.camera);
            const auto& scene=*work->service->Scene();
            for (std::size_t i=0;i<placed->requests.size();++i) {
                const auto stand=Render::FindAnimationSequence(scene.sequences[i],"Stand",specs[i].model.animationProperties);
                if (!stand) continue;
                const auto& sequence=scene.sequences[i][*stand];
                if (sequence.end<=sequence.start) continue;
                Render::AnimationEdit phase; phase.identity=specs[i].identity;
                phase.elapsedMs=WorldStandStartMs(specs[i].hostKey,
                    std::min<std::uint64_t>(1000, static_cast<std::uint64_t>(sequence.end)-sequence.start));
                first.edits.push_back(std::move(phase));
            }
            auto frame=work->service->Prepare(nullptr,first);
            if (!frame.Succeeded()) { work->diagnostics=std::move(frame.diagnostics); return; }
            auto verify=work->source->VerifyCurrentSource();
            if (!verify.Succeeded()) { work->diagnostics=std::move(verify.diagnostics); return; }
            work->frame=std::move(*frame.value);
        } catch (const std::exception& e) {
            work->diagnostics.push_back({DiagnosticSeverity::Error,"World","WORLD_ANIMATION_BUILD",e.what(),std::nullopt});
        }
    });
    modelWorker_=worker;
    RecordModelLoad("preparing-animated");
    connect(worker,&QThread::finished,this,[this,work,placed,generation,cliffs,revision,mapKey,registry,revisionGate,selectionSources,
        catalogRevision = presentationVersion.catalogRevision, cliffLayer] {
        modelWorker_=nullptr;
        for (const auto& [variant, target] : work->variants) modelVariants_.insert_or_assign(variant, target);
        if (!page_) return;
        const auto current=[this,generation,revisionGate,mapKey,catalogRevision] {
            return page_ && generation==generation_ && MapKey()==mapKey &&
                selectionCatalogRevision_==catalogRevision &&
                (project_.mapEdits_?project_.mapEdits_->Revision():0)==*revisionGate;
        };
        if (!current() || cliffs_ != cliffs || placementsDirty_) {
            placementsDirty_=true; modelGeneration_=generation_;
            if (viewport_->VertexCount()) StartModels(generation_);
            else modelStatus_->setText(tr("等待地形就绪后重新准备动画…"));
            return;
        }
        if (!work->frame) {
            RecordModelLoad("animated-preparation-failed", work->diagnostics, true);
            project_.AddDiagnostics(work->diagnostics);
            emit animation_->Message(tr("动画场景准备失败：%1").arg(WorldDiagnosticText(work->diagnostics)));
            const bool retained = animation_->CanRetainCurrentScene();
            if (!retained) { animation_->Clear(); viewport_->InvalidateAnimationPicking(); }
            QStringList details{retained ? tr("新模型准备失败，当前世界的有效场景保持播放；未提交新模型。")
                                         : tr("动画场景失败，保留此前画面（只读）；未提交新模型。")};
            for (const auto& diagnostic:work->diagnostics)
                details<<QString::fromUtf8(diagnostic.code+"："+diagnostic.message);
            modelStatus_->setText(tr("动画场景加载失败"));
            SetDiagnostics(*modelStatus_, details.join(QLatin1Char('\n')));
            UpdateOverlay(); UpdateSelectionText(); UpdateInspector();
            if (retained) BindInstanceDiagnostics();
            if (selectionStatus_) selectionStatus_->setText(retained ? tr("新模型未接收，原世界仍可操作；详见模型状态。")
                                                                   : tr("动画场景失败，旧画面只读；详见模型状态。"));
            if (!retained && CanLoadStaticAfterAnimationFailure(work->diagnostics))
                StartStaticModels(generation, details.join(QLatin1Char('\n')));
            return;
        }
        modelStatus_->setProperty("animationUnresolvedPlacements",QVariant::fromValue(static_cast<qulonglong>(placed->unresolved)));
        if (!animation_->Install(work->service,work->frame,work->source,current)) {
            RecordModelLoad("animated-submit-failed", viewport_->LastAnimationFailure().diagnostics, true);
            const bool retained = animation_->CanRetainCurrentScene();
            if (!retained) { animation_->Clear(); viewport_->InvalidateAnimationPicking(); }
            UpdateInspector();
            const auto diagnostic = WorldDiagnosticText(viewport_->LastAnimationFailure().diagnostics);
            const auto reason = (retained ? tr("新模型未接收，原世界保持可用：") : tr("模型上传失败，旧画面只读：")) + diagnostic;
            modelStatus_->setText(reason); SetDiagnostics(*modelStatus_, reason);
            if (retained) BindInstanceDiagnostics();
            else StartStaticModels(generation, reason);
            return;
        }
        scene_=animation_->Picking(); requests_=placed->requests; placementIds_=placed->ids;
        RecordModelLoad("prepared-animated");
        unresolvedPlacements_ = placed->unresolvedRows;
        cliffsInLayer_ = cliffLayer && work->cliffLayer;
        if (cliffsInLayer_) { cliffLayer_->Show(*work->cliffLayer); cliffLayer_->RebindParent(); }
        else if (cliffLayer_) cliffLayer_->Clear(); // 主场景已含悬崖，不能再叠一层
        pathingHelperRequests_.assign(requests_.size(), 0);
        pathingHelperPresentation_.reset();
        if (work->helpers) {
            pathingHelperCache_ = work->helpers->cache;
            pathingHelperRequests_ = work->helpers->helperRequests;
            project_.AddDiagnostics(work->helpers->diagnostics);
            modelStatus_->setProperty("pathingHelperCount", QVariant::fromValue(static_cast<qulonglong>(work->helpers->helpers.size())));
            modelStatus_->setProperty("pathingHelperUnknownFootprints", QVariant::fromValue(static_cast<qulonglong>(work->helpers->unknownFootprints)));
            pathingHelperPresentation_ = std::make_shared<const WorldPathingHelperPrepared>(std::move(*work->helpers));
        }
        stagedPresentations_ = placed->presentations;
        instanceRegistry_ = registry; stagedInstanceRegistry_ = registry;
        stagedRequests_ = requests_; stagedPlacementIds_ = placementIds_;
        instanceRevisionGate_ = revisionGate; allowedInstanceEditRevision_ = revision;
        if (selectionPresenter_) {
            selectionPresenter_->SetSource(selectionSources, {work->service->Scene()->generation, revision, selectionSources.archiveSha},
                [this, generation, mapKey] { return page_ && generation_ == generation && MapKey() == mapKey; });
            selectionPresenter_->RebindParent();
        }
        RefreshBuildingSplats(true);
        // 正式地图事务仍走原场景替换；附加层按新父 epoch 重新建立，预览本身不再触发该路径。
        if (placing_ || pasting_) StartPlacementPreview();
        ValidateSelection(); BindInstanceDiagnostics(); UpdateOverlay(); UpdateSelectionText();
        (void)viewport_->RefreshEditorVisibility(); // 分类只在真实accepted世界发布后加入独立显隐。
    });
    connect(worker,&QThread::finished,worker,&QObject::deleteLater); worker->start(QThread::LowPriority);
}
} // namespace War3Map::Modules
