#include "GameUiModule.h"

#include "EditorProject.h"
#include "GameUiCanvas.h"
#include "War3MapCore/GameUi/GameCodeLayout.h"
#include "War3MapCore/GameUi/UiSkin.h"

#include <QComboBox>
#include <QCryptographicHash>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonObject>
#include <QPushButton>
#include <QSplitter>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace War3Map::Modules {
namespace {

using namespace War3Map::GameUi;

// 代码区域紧跟控制台：画在控制台贴图之上、资源栏等界面之下。脚本界面总在最后展开，画在最上层。
const QString kCodeRoot = QString::fromLatin1(kGameCodeRoot.data(), static_cast<qsizetype>(kGameCodeRoot.size()));
const QString kScriptRootName = QString::fromLatin1(kScriptRoot.data(), static_cast<qsizetype>(kScriptRoot.size()));
const QStringList kDefaultRoots{QStringLiteral("ConsoleUI"), kCodeRoot, QStringLiteral("ResourceBarFrame"),
    QStringLiteral("UpperButtonBarFrame"), kScriptRootName};

// 同一份字体数据在进程内只登记一次；返回字体族名，失败为空。
QString RegisterFont(const std::vector<std::byte>& bytes) {
    static QHash<QByteArray, QString> registered;
    const QByteArray data(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
    const QByteArray digest = QCryptographicHash::hash(data, QCryptographicHash::Sha1);
    if (const auto found = registered.constFind(digest); found != registered.constEnd()) return *found;
    QString family;
    if (const int id = QFontDatabase::addApplicationFontFromData(data); id >= 0) {
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (!families.isEmpty()) family = families.front();
    }
    registered.insert(digest, family);
    return family;
}

QImage ToQImage(const Image::DecodedImage& decoded) {
    const QImage view(reinterpret_cast<const uchar*>(decoded.rgba.data()), static_cast<int>(decoded.width),
        static_cast<int>(decoded.height), static_cast<qsizetype>(decoded.width) * 4, QImage::Format_RGBA8888);
    return view.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

QString Text(const std::string& text) { return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())); }

} // namespace

QRect GameUiPixelRect(const FrameRect& rect, const FrameTreeOptions& options, double aspect, int height) {
    const double width = height * aspect;
    const double x = rect.left / options.screenWidth * width;
    const double y = (options.screenHeight - rect.top) / options.screenHeight * height;
    return QRect(qRound(x), qRound(y), qRound(rect.Width() / options.screenWidth * width),
        qRound(rect.Height() / options.screenHeight * height));
}

GameUiModule::GameUiModule(QtProject::EditorProject& project) : project_(project), roots_(kDefaultRoots) {
    connect(&project_, &QtProject::EditorProject::MapDocumentChanged, this, &GameUiModule::ScheduleReload);
    connect(&project_, &QtProject::EditorProject::ImportsChanged, this, [this] {
        if (!stagingOwnImport_) ScheduleReload();
    });
    connect(&project_, &QtProject::EditorProject::WorldEditStringsChanged, this, &GameUiModule::ScheduleReload);
}

GameUiModule::~GameUiModule() { delete page_.data(); }

Shell::ModuleInfo GameUiModule::Info() const {
    return {"game-ui", tr("界面"), "lucide/app-window", {}};
}

QWidget* GameUiModule::Widget() {
    if (page_) return page_;
    BuildPage();
    if (!definitions_) ScheduleReload();
    else RebuildScene();
    return page_;
}

void GameUiModule::AttachHost(Shell::ModuleHost* host) { host_ = host; }

bool GameUiModule::Reveal(const Shell::NavigationTarget& target) {
    if (target.kind != QLatin1String("frame")) return false;
    return SelectFrame(target.id);
}

void GameUiModule::ScheduleReload() {
    if (!page_ || reloadQueued_) return;
    reloadQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        if (!reloadQueued_) return;
        reloadQueued_ = false;
        Reload();
    });
}

void GameUiModule::Reload() {
    ++generation_;
    reader_ = std::make_shared<GameUiReader>(CaptureGameUiSources(project_));
    sourceNote_ = reader_->Sources().mapNote;
    textures_.clear();
    fontFamilies_.clear();
    assetOrigins_.clear();
    assetErrors_.clear();
    requestedAssets_.clear();
    assetsPending_ = false;
    assetsRerun_ = false;
    StartDefinitionsJob();
}

void GameUiModule::StartDefinitionsJob() {
    definitionsPending_ = true;
    UpdateProblems();
    const auto generation = generation_;
    auto reader = reader_;
    auto result = std::make_shared<GameUiDefinitions>();
    auto* worker = QThread::create([reader, result] { *result = LoadGameUiDefinitions(*reader); });
    connect(worker, &QThread::finished, this, [this, generation, result] {
        if (generation != generation_) return;
        definitionsPending_ = false;
        ApplyDefinitions(std::move(*result));
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}

void GameUiModule::LoadDefinitionsForTesting(GameUiDefinitions definitions) {
    ++generation_;
    reader_.reset();
    reloadQueued_ = false;
    definitionsPending_ = false;
    ApplyDefinitions(std::move(definitions));
}

void GameUiModule::ApplyDefinitions(GameUiDefinitions definitions) {
    loadDiagnostics_ = definitions.diagnostics;
    // 脚本可能已变化：之前的模拟不再对应，停止。
    ResetSimulation();
    definitions_ = std::move(definitions);
    // 预览数值按描述编号保留（重新读取后接着看），已不存在的描述丢弃。
    for (auto it = descriptionStates_.begin(); it != descriptionStates_.end();) {
        const bool exists = std::any_of(definitions_->descriptions.begin(), definitions_->descriptions.end(),
            [&](const GameUiDescriptionFile& file) { return file.description.id == it->first; });
        it = exists ? std::next(it) : descriptionStates_.erase(it);
    }
    descriptionPanelFor_.clear();
    if (addRootBox_) {
        const QString typed = addRootBox_->currentText();
        addRootBox_->clear();
        addRootBox_->addItems(AvailableTemplates());
        addRootBox_->setCurrentIndex(-1);
        addRootBox_->setEditText(typed);
    }
    RebuildScene();
    UpdateSimulationPanel();
}

QStringList GameUiModule::MapTemplates() const {
    QStringList names;
    if (!definitions_ || !definitions_->library) return names;
    const auto& files = definitions_->library->Files();
    for (const auto& entry : definitions_->library->Templates()) {
        if (entry.definition->kind == DefinitionKind::Frame && files[entry.fileIndex]->origin == "map")
            names.push_back(Text(entry.definition->name));
    }
    return names;
}

const GameUi::ScriptFramePlan* GameUiModule::ScriptFrames() const {
    if (simulatedPlan_) return &*simulatedPlan_;
    return definitions_ ? &definitions_->scriptFrames : nullptr;
}

QStringList GameUiModule::AvailableTemplates() const {
    if (!definitions_ || !definitions_->library) return {};
    // 地图脚本界面与地图自带的界面排在前面，其后是游戏代码区域与游戏界面。
    QStringList names;
    if (!definitions_->scriptFrames.frames.empty()) names.push_back(kScriptRootName);
    names += MapTemplates();
    names.push_back(kCodeRoot);
    const auto& files = definitions_->library->Files();
    for (const auto& entry : definitions_->library->Templates()) {
        if (entry.definition->kind == DefinitionKind::Frame && files[entry.fileIndex]->origin != "map")
            names.push_back(Text(entry.definition->name));
    }
    return names;
}

double GameUiModule::Aspect() const {
    return canvas_ ? canvas_->Aspect() : pendingView_.value(QStringLiteral("aspect")).toDouble(16.0 / 9.0);
}

void GameUiModule::SetAspect(double aspect) {
    if (!aspectBox_) {
        pendingView_.insert(QStringLiteral("aspect"), aspect);
        return;
    }
    for (int i = 0; i < aspectBox_->count(); ++i) {
        if (std::abs(aspectBox_->itemData(i).toDouble() - aspect) < 0.01) aspectBox_->setCurrentIndex(i);
    }
}

UiSkin GameUiModule::MakeSkin() const {
    UiSkin skin;
    if (!definitions_) return skin;
    skin.SetGameSkins(definitions_->gameSkins);
    skin.SetMiscUi(definitions_->miscUi);
    if (!definitions_->mapSkin.empty()) skin.SetMapSkin(definitions_->mapSkin);
    skin.SetTeamColors(definitions_->teamColors);
    return skin;
}

void GameUiModule::RebuildScene() {
    tree_.reset();
    drawList_ = {};
    sceneDiagnostics_.clear();
    if (definitions_ && definitions_->library) {
        const UiSkin skin = MakeSkin();
        FrameTreeOptions options;
        options.race = race_.toStdString();
        options.expansion = expansion_;
        auto tree = std::make_unique<FrameTree>(options);
        for (const QString& root : roots_) {
            if (root.compare(kCodeRoot, Qt::CaseInsensitive) == 0) {
                tree->AddGameCodeAreas();
                continue;
            }
            if (root.compare(kScriptRootName, Qt::CaseInsensitive) == 0) continue;
            auto created = tree->Instantiate(*definitions_->library, &skin, root.toStdString());
            sceneDiagnostics_.insert(sceneDiagnostics_.end(), created.diagnostics.begin(), created.diagnostics.end());
        }
        // 脚本框架可以锚在游戏代码区域上，所以在其他根之后展开。
        if (roots_.contains(kScriptRootName, Qt::CaseInsensitive)) {
            (void)AddScriptFrames(*tree, *definitions_->library, &skin, *ScriptFrames(), sceneDiagnostics_);
        }
        // 地图自己的界面描述总是显示；同样可以锚在游戏代码区域上，所以最后展开。
        for (const auto& file : definitions_->descriptions) {
            (void)AddUiDescription(*tree, *definitions_->library, &skin, file.description,
                descriptionStates_[file.description.id], sceneDiagnostics_);
        }
        const auto layout = tree->Layout();
        sceneDiagnostics_.insert(sceneDiagnostics_.end(), layout.begin(), layout.end());
        tree_ = std::move(tree);
    }
    selected_ = kNoNode;
    if (tree_ && !selectedKey_.isEmpty()) {
        for (const auto& node : tree_->Nodes()) {
            if (node.id != kScreenNode && NodeKey(node.id) == selectedKey_) {
                selected_ = node.id;
                break;
            }
        }
    }
    RebuildDrawList();
    PopulateTree();
    UpdateProperties();
    UpdateProblems();
    emit SceneChanged();
    StartAssetsJob();
}

void GameUiModule::RebuildDrawList() {
    FrameDrawOptions options;
    options.drawScriptHidden = showHidden_;
    if (tree_ && !hiddenKeys_.isEmpty()) {
        for (const auto& node : tree_->Nodes()) {
            if (node.id != kScreenNode && hiddenKeys_.contains(NodeKey(node.id))) options.hidden.insert(node.id);
        }
    }
    drawList_ = tree_ ? BuildDrawList(*tree_, options) : FrameDrawList{};
    if (canvas_) {
        canvas_->SetScene(tree_.get(), drawList_);
        canvas_->SetSelected(selected_);
    }
}

QString GameUiModule::NodeKey(std::uint32_t node) const {
    if (!tree_ || node >= tree_->Nodes().size()) return {};
    QStringList parts;
    for (std::uint32_t current = node; current != kScreenNode && current != kNoNode;) {
        const FrameNode& item = tree_->Nodes()[current];
        if (!item.name.empty()) {
            parts.push_front(Text(item.name));
        } else {
            const auto& siblings = tree_->Nodes()[item.parent].children;
            const auto index = std::find(siblings.begin(), siblings.end(), current) - siblings.begin();
            parts.push_front(QStringLiteral("#%1").arg(index));
        }
        current = item.parent;
    }
    return parts.join(QLatin1Char('/'));
}

void GameUiModule::StartAssetsJob() {
    if (!reader_ || !tree_ || definitionsPending_) return;
    if (assetsPending_) {
        assetsRerun_ = true;
        return;
    }
    std::vector<std::string> textures;
    std::vector<std::string> fonts;
    const auto want = [this](const std::string& path, std::vector<std::string>& output) {
        if (path.empty()) return;
        const QString key = GameUiAssetKey(Text(path));
        if (requestedAssets_.contains(key)) return;
        requestedAssets_.insert(key);
        output.push_back(path);
    };
    for (const auto* list : {&drawList_, &previewDrawList_}) {
        for (const auto& command : list->commands) {
            if (command.kind == DrawKind::Image) want(command.image.path, textures);
            else want(command.text.fontPath, fonts);
        }
    }
    if (textures.empty() && fonts.empty()) return;
    assetsPending_ = true;
    UpdateProblems();
    const auto generation = generation_;
    auto reader = reader_;
    auto result = std::make_shared<std::vector<GameUiAsset>>();
    auto* worker = QThread::create([reader, result, textures = std::move(textures), fonts = std::move(fonts)] {
        *result = LoadGameUiAssets(*reader, textures, fonts);
    });
    connect(worker, &QThread::finished, this, [this, generation, result] {
        if (generation != generation_) return;
        assetsPending_ = false;
        ApplyAssets(std::move(*result));
        if (assetsRerun_) {
            assetsRerun_ = false;
            StartAssetsJob();
        }
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start(QThread::LowPriority);
}

void GameUiModule::ApplyAssets(std::vector<GameUiAsset> assets) {
    for (auto& asset : assets) {
        const QString key = GameUiAssetKey(Text(asset.path));
        if (!asset.origin.empty()) assetOrigins_.insert(key, Text(asset.origin));
        if (!asset.error.empty()) {
            assetErrors_.insert(key, Text(asset.error));
        } else if (asset.image) {
            textures_.insert(key, ToQImage(*asset.image));
        } else {
            const QString family = RegisterFont(asset.fontBytes);
            if (family.isEmpty()) assetErrors_.insert(key, tr("字体无法载入"));
            else fontFamilies_.insert(key, family);
        }
    }
    if (canvas_) canvas_->update();
    UpdateProperties();
    UpdateProblems();
    emit AssetsChanged();
}

bool GameUiModule::AddRoot(const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || !definitions_ || !definitions_->library) return false;
    QString canonical = kCodeRoot;
    if (trimmed.compare(kScriptRootName, Qt::CaseInsensitive) == 0) {
        canonical = kScriptRootName;
    } else if (trimmed.compare(kCodeRoot, Qt::CaseInsensitive) != 0) {
        const auto found = definitions_->library->FindTemplate(trimmed.toStdString());
        if (!found || found->definition->kind != DefinitionKind::Frame) return false;
        canonical = Text(found->definition->name);
    }
    if (!roots_.contains(canonical, Qt::CaseInsensitive)) {
        roots_.push_back(canonical);
        RebuildScene();
    }
    return SelectFrame(canonical);
}

bool GameUiModule::RemoveRoot(const QString& name) {
    for (qsizetype i = 0; i < roots_.size(); ++i) {
        if (roots_[i].compare(name, Qt::CaseInsensitive) == 0) {
            roots_.removeAt(i);
            RebuildScene();
            return true;
        }
    }
    return false;
}

void GameUiModule::SetRace(const QString& race) {
    if (race.isEmpty() || race == race_) return;
    race_ = race;
    if (raceBox_) {
        const int index = raceBox_->findData(race);
        if (index >= 0 && index != raceBox_->currentIndex()) raceBox_->setCurrentIndex(index);
    }
    RebuildScene();
}

void GameUiModule::SetExpansion(bool expansion) {
    if (expansion == expansion_) return;
    expansion_ = expansion;
    if (versionBox_) versionBox_->setCurrentIndex(expansion ? 0 : 1);
    RebuildScene();
}

bool GameUiModule::SelectFrame(const QString& name) {
    if (!tree_) return false;
    const auto found = tree_->FindByName(name.toStdString());
    if (!found) return false;
    SelectNode(*found);
    return true;
}

QImage GameUiModule::RenderImage(const QSize& size) const {
    return canvas_ ? canvas_->RenderImage(size) : QImage();
}

QJsonObject GameUiModule::SaveViewState() const {
    QJsonObject state = page_ ? QJsonObject() : pendingView_;
    state.insert(QStringLiteral("roots"), QJsonArray::fromStringList(roots_));
    state.insert(QStringLiteral("race"), race_);
    state.insert(QStringLiteral("expansion"), expansion_);
    if (canvas_) state.insert(QStringLiteral("aspect"), canvas_->Aspect());
    if (showContent_) state.insert(QStringLiteral("content"), showContent_->isChecked());
    if (showBounds_) state.insert(QStringLiteral("bounds"), showBounds_->isChecked());
    if (showNames_) state.insert(QStringLiteral("names"), showNames_->isChecked());
    state.insert(QStringLiteral("showHidden"), showHidden_);
    if (mainSplit_) state.insert(QStringLiteral("mainSplit"), QString::fromLatin1(mainSplit_->saveState().toBase64()));
    if (centerSplit_) state.insert(QStringLiteral("centerSplit"), QString::fromLatin1(centerSplit_->saveState().toBase64()));
    return state;
}

void GameUiModule::RestoreViewState(const QJsonObject& state) {
    if (const auto roots = state.value(QStringLiteral("roots")); roots.isArray()) {
        QStringList restored;
        for (const auto& value : roots.toArray()) {
            if (value.isString() && !value.toString().isEmpty()) restored.push_back(value.toString());
        }
        roots_ = restored;
    }
    if (const QString race = state.value(QStringLiteral("race")).toString(); !race.isEmpty()) race_ = race;
    expansion_ = state.value(QStringLiteral("expansion")).toBool(expansion_);
    showHidden_ = state.value(QStringLiteral("showHidden")).toBool(showHidden_);
    if (showHiddenButton_) showHiddenButton_->setChecked(showHidden_);
    if (!page_) {
        pendingView_ = state;
    } else {
        if (const int index = raceBox_->findData(race_); index >= 0) raceBox_->setCurrentIndex(index);
        versionBox_->setCurrentIndex(expansion_ ? 0 : 1);
        if (state.contains(QStringLiteral("aspect"))) {
            const double aspect = state.value(QStringLiteral("aspect")).toDouble();
            for (int i = 0; i < aspectBox_->count(); ++i) {
                if (std::abs(aspectBox_->itemData(i).toDouble() - aspect) < 0.01) aspectBox_->setCurrentIndex(i);
            }
        }
        showContent_->setChecked(state.value(QStringLiteral("content")).toBool(true));
        showBounds_->setChecked(state.value(QStringLiteral("bounds")).toBool(false));
        showNames_->setChecked(state.value(QStringLiteral("names")).toBool(false));
        if (const QString split = state.value(QStringLiteral("mainSplit")).toString(); !split.isEmpty())
            mainSplit_->restoreState(QByteArray::fromBase64(split.toLatin1()));
        if (const QString split = state.value(QStringLiteral("centerSplit")).toString(); !split.isEmpty())
            centerSplit_->restoreState(QByteArray::fromBase64(split.toLatin1()));
    }
    if (definitions_) RebuildScene();
}

} // namespace War3Map::Modules
