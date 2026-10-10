#include "AgentSettingsDialog.h"
#include "AgentSettingsStore.h"
#include "CredentialStore.h"
#include "ProviderCatalog.h"

#include "Fonts/FontLoader.h"
#include "Theme/ThemeManager.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHash>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSaveFile>
#include <QSet>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace War3Map::Agent {
namespace {

QLabel* Text(const QString& text, QWidget* parent = nullptr, bool muted = false) {
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    auto sizePolicy = label->sizePolicy();
    sizePolicy.setHorizontalPolicy(QSizePolicy::Ignored);
    label->setSizePolicy(sizePolicy);
    if (muted) {
        auto palette = label->palette();
        palette.setColor(QPalette::WindowText, palette.color(QPalette::PlaceholderText));
        label->setPalette(palette);
    }
    return label;
}

QWidget* Page(const QString& heading, const QString& description, QVBoxLayout*& layout) {
    auto* page = new QWidget;
    layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto* title = Text(heading);
    auto font = title->font();
    font.setPointSize(font.pointSize() + 4);
    font.setBold(true);
    title->setFont(font);
    layout->addWidget(title);
    if (!description.isEmpty()) layout->addWidget(Text(description, nullptr, true));
    auto* divider = new QFrame;
    divider->setObjectName(QStringLiteral("agentSettingsDivider"));
    divider->setFixedHeight(1);
    layout->addWidget(divider);
    return page;
}

QLineEdit* Edit(const QString& name, const QString& placeholder = {}) {
    auto* edit = new QLineEdit;
    edit->setObjectName(name);
    edit->setPlaceholderText(placeholder);
    edit->setMaxLength(16384);
    return edit;
}

QLineEdit* Tokens(const QString& name) {
    auto* edit = Edit(name, QStringLiteral("自动"));
    edit->setMaximumWidth(180);
    edit->setMaxLength(15);
    edit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[0-9]{0,15}")), edit));
    edit->setToolTip(QStringLiteral("填写整数 tokens；留空使用站点或目录默认值，都未知时采用 262144。"));
    return edit;
}

QPushButton* Button(const QString& text, const QString& name = {}) {
    auto* button = new QPushButton(text);
    button->setObjectName(name);
    button->setAutoDefault(false);
    return button;
}

QString DefaultBase(const QString& protocol) {
    if (protocol == QStringLiteral("anthropic")) return QStringLiteral("https://api.anthropic.com/v1");
    if (protocol == QStringLiteral("gemini")) return QStringLiteral("https://generativelanguage.googleapis.com/v1beta");
    return QStringLiteral("https://api.openai.com/v1");
}

} // namespace

class AgentSettingsDialog::Impl {
public:
    explicit Impl(AgentSettingsDialog* owner, const AgentSettings& initial)
        : dialog(owner), settings(initial), original(initial), credentials(std::make_unique<CredentialStore>()), credentialStore(credentials.get()) {
        Build();
    }

    ProviderSettings* CurrentProvider() {
        return providerIndex >= 0 && providerIndex < settings.providers.size() ? &settings.providers[providerIndex] : nullptr;
    }

    ModelSettings* CurrentModel() {
        auto* provider = CurrentProvider();
        return provider != nullptr && modelIndex >= 0 && modelIndex < provider->models.size() ? &provider->models[modelIndex] : nullptr;
    }

    void Message(const QString& text) { message->setText(text); }

    void Build() {
        dialog->setObjectName(QStringLiteral("AgentSettingsDialog"));
        dialog->setWindowTitle(QStringLiteral("设置 · AI 与 Agent"));
        dialog->resize(980, 760);
        dialog->setMinimumSize(760, 620);
        auto* outer = new QVBoxLayout(dialog);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(0);
        auto* body = new QHBoxLayout;
        body->setSpacing(0);
        nav = new QListWidget;
        nav->setObjectName(QStringLiteral("agentSettingsNavigation"));
        nav->addItems({QStringLiteral("常规"), QStringLiteral("提供商与模型"), QStringLiteral("Agent 权限"), QStringLiteral("MCP 连接"), QStringLiteral("导入与导出")});
        nav->setFixedWidth(170);
        nav->setFrameShape(QFrame::NoFrame);
        nav->setSpacing(3);
        nav->setContentsMargins(8, 14, 8, 0);
        auto* divider = new QFrame;
        divider->setObjectName(QStringLiteral("agentSettingsDivider"));
        divider->setFixedWidth(1);
        // 外侧留白由窗口统一提供，内建页与宿主接入页使用同一内容边界。
        auto* content = new QWidget;
        content->setObjectName(QStringLiteral("applicationSettingsContent"));
        auto* contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(24, 20, 24, 20);
        contentLayout->setSpacing(0);
        pages = new QStackedWidget(content);
        pages->setObjectName(QStringLiteral("applicationSettingsPages"));
        contentLayout->addWidget(pages);
        body->addWidget(nav);
        body->addWidget(divider);
        body->addWidget(content, 1);
        outer->addLayout(body, 1);
        BuildGeneral();
        BuildProviders();
        BuildPermissions();
        BuildMcp();
        BuildTransfer();
        auto* footer = new QWidget;
        auto* footerLayout = new QHBoxLayout(footer);
        footerLayout->setContentsMargins(20, 12, 20, 12);
        message = Text(QStringLiteral("设置保存在当前用户目录；取消不会写入密钥。"), nullptr, true);
        message->setObjectName(QStringLiteral("agentSettingsMessage"));
        footerLayout->addWidget(message, 1);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存设置"));
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        footerLayout->addWidget(buttons);
        outer->addWidget(footer);
        QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &AgentSettingsDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &AgentSettingsDialog::reject);
        QObject::connect(nav, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
        nav->setCurrentRow(0);
        LoadProvider(settings.providers.isEmpty() ? -1 : 0);
        UpdateStatus();
    }

    void BuildGeneral() {
        QVBoxLayout* layout = nullptr;
        auto* page = Page(QStringLiteral("AI 工作区"), QStringLiteral("独立助手窗口与外部 MCP 共用地图工具、编辑事务和保存验证。"), layout);
        generalLayout = layout;
        aiEnabled = new QCheckBox(QStringLiteral("启用内置 AI 助手"));
        aiEnabled->setObjectName(QStringLiteral("agentAiEnabled"));
        aiEnabled->setChecked(settings.aiEnabled);
        layout->addWidget(aiEnabled);
        status = Text({}, nullptr, true);
        status->setObjectName(QStringLiteral("agentConfigurationStatus"));
        layout->addWidget(status);
        reducedMotion = new QCheckBox(QStringLiteral("减少界面动画"));
        reducedMotion->setObjectName(QStringLiteral("agentReducedMotion"));
        reducedMotion->setChecked(settings.reducedMotion);
        layout->addSpacing(16);
        layout->addWidget(reducedMotion);
        layout->addWidget(Text(QStringLiteral("开启后缩短计数和展开动效；实际工具结果和流式内容继续更新。"), nullptr, true));
        layout->addSpacing(16);
        layout->addWidget(Text(QStringLiteral("界面字体")));
        uiFont = new QComboBox;
        uiFont->setObjectName(QStringLiteral("settingsUiFont"));
        uiFont->setMaximumWidth(320);
        uiFont->addItem(QStringLiteral("默认（微软雅黑）"), QString());
        for (const QString& family : UiKit::SelectableUiFamilies()) uiFont->addItem(family, family);
        layout->addWidget(uiFont);
        fontPreview = Text(QStringLiteral("触发器 单位 物品 DeathEvent 0123456789"));
        fontPreview->setObjectName(QStringLiteral("settingsUiFontPreview"));
        layout->addWidget(fontPreview);
        QObject::connect(uiFont, &QComboBox::currentIndexChanged, dialog, [this] {
            uiFontFamily = uiFont->currentData().toString();
            QFont preview = QApplication::font();
            if (!uiFontFamily.isEmpty()) preview.setFamilies({uiFontFamily});
            preview.setPixelSize(15);
            fontPreview->setFont(preview);
            if (transferFont) transferFont->setText(uiFontFamily.isEmpty() ? QStringLiteral("默认（微软雅黑）") : uiFontFamily);
        });
        layout->addStretch(1);
        layout->addWidget(Text(QStringLiteral("地图逻辑默认写成可编辑的触发器（T）；代码与外部命令由任务和权限共同控制。"), nullptr, true));
        pages->addWidget(page);
        QObject::connect(aiEnabled, &QCheckBox::toggled, dialog, [this](bool value) { settings.aiEnabled = value; UpdateStatus(); });
        QObject::connect(reducedMotion, &QCheckBox::toggled, dialog, [this](bool value) { settings.reducedMotion = value; });
    }

    QVBoxLayout* generalLayout = nullptr;

    void BuildTransfer() {
        QVBoxLayout* layout = nullptr;
        auto* page = Page(QStringLiteral("导入与导出"), QStringLiteral("迁移目录、外观与提供商配置。密钥留在本机。"), layout);
        auto* form = new QFormLayout;
        form->setVerticalSpacing(14);
        transferGame = Edit(QStringLiteral("settingsTransferGameRoot"), QStringLiteral("Warcraft III 安装目录"));
        transferUi = Edit(QStringLiteral("settingsTransferUiRoot"), QStringLiteral("YDWE 或 UI 数据根目录"));
        transferFont = Text(QStringLiteral("默认（微软雅黑）"));
        transferFont->setObjectName(QStringLiteral("settingsTransferFont"));
        transferFont->setToolTip(QStringLiteral("可在“常规”页修改字体；未安装的字体名称会保留。"));
        transferTheme = new QComboBox;
        transferTheme->setObjectName(QStringLiteral("settingsTransferTheme"));
        transferTheme->addItem(QStringLiteral("默认主题"), QString());
        for (const auto& id : UiKit::ThemeManager::ThemeIds()) transferTheme->addItem(UiKit::ThemeManager::ThemeLabel(id), id);
        form->addRow(QStringLiteral("游戏目录"), transferGame);
        form->addRow(QStringLiteral("YDWE / UI 数据根"), transferUi);
        form->addRow(QStringLiteral("界面字体"), transferFont);
        form->addRow(QStringLiteral("主题"), transferTheme);
        layout->addLayout(form);
        layout->addWidget(Text(QStringLiteral("目录可按新电脑的位置修改，保存后下次启动生效。字体在“常规”页调整。"), nullptr, true));
        transferSummary = Text(QStringLiteral("应用设置尚未加载。"));
        transferSummary->setObjectName(QStringLiteral("settingsTransferSummary"));
        layout->addWidget(transferSummary);
        auto* actions = new QHBoxLayout;
        importSettings = Button(QStringLiteral("导入设置…"), QStringLiteral("settingsTransferImport"));
        exportSettings = Button(QStringLiteral("导出设置…"), QStringLiteral("settingsTransferExport"));
        importSettings->setEnabled(false); exportSettings->setEnabled(false);
        actions->addWidget(importSettings); actions->addWidget(exportSettings); actions->addStretch(1);
        layout->addLayout(actions);
        layout->addWidget(Text(QStringLiteral("导入先放入当前设置页；点击“保存设置”才应用，取消不改变本机设置。"), nullptr, true));
        layout->addStretch(1);
        pages->addWidget(page);
        QObject::connect(importSettings, &QPushButton::clicked, dialog, [this] {
            const auto path = QFileDialog::getOpenFileName(dialog, QStringLiteral("导入设置"), {}, QStringLiteral("Audastra 设置 (*.json)"));
            if (path.isEmpty()) return;
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > ApplicationSettingsTransfer::MaximumBytes) {
                Message(QStringLiteral("无法读取设置文件，或文件超过2MiB。")); return;
            }
            QString error;
            if (!dialog->ImportSettingsData(file.read(ApplicationSettingsTransfer::MaximumBytes + 1), &error)) Message(error);
        });
        QObject::connect(exportSettings, &QPushButton::clicked, dialog, [this] {
            QString error; const auto bytes = dialog->ExportSettingsData(&error);
            if (bytes.isEmpty()) { Message(error); return; }
            const auto path = QFileDialog::getSaveFileName(dialog, QStringLiteral("导出设置"), QStringLiteral("Audastra-settings.json"),
                QStringLiteral("Audastra 设置 (*.json)"));
            if (path.isEmpty()) return;
            QSaveFile file(path); file.setDirectWriteFallback(false);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
                Message(QStringLiteral("设置文件未写入，原文件保留。")); return;
            }
            Message(QStringLiteral("设置已导出；不含密钥或本机凭据引用。"));
        });
    }

    void RefreshTransferSummary() {
        qsizetype count = 0;
        for (const auto& provider : settings.providers) count += provider.models.size();
        if (transferSummary && applicationLoaded) transferSummary->setText(
            (importedSettings ? QStringLiteral("已暂存导入 · %1 个提供商，%2 个模型") : QStringLiteral("%1 个提供商，%2 个模型"))
                .arg(settings.providers.size()).arg(count));
    }

    void BuildProviders() {
        QVBoxLayout* layout = nullptr;
        auto* page = Page(QStringLiteral("提供商与模型"), QStringLiteral("配置 API 基础地址，获取模型后勾选在助手中使用的模型。"), layout);
        auto* split = new QSplitter(Qt::Horizontal);
        auto* left = new QWidget;
        auto* leftLayout = new QVBoxLayout(left);
        leftLayout->setContentsMargins(0, 0, 12, 0);
        providers = new QListWidget;
        providers->setObjectName(QStringLiteral("agentProviderList"));
        leftLayout->addWidget(providers, 1);
        auto* providerButtons = new QHBoxLayout;
        auto* addProvider = Button(QStringLiteral("添加"), QStringLiteral("agentAddProvider"));
        auto* deleteProvider = Button(QStringLiteral("移除"), QStringLiteral("agentRemoveProvider"));
        providerButtons->addWidget(addProvider);
        providerButtons->addWidget(deleteProvider);
        leftLayout->addLayout(providerButtons);
        split->addWidget(left);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        providerEditor = new QWidget;
        auto* rightLayout = new QVBoxLayout(providerEditor);
        rightLayout->setContentsMargins(6, 0, 2, 0);
        rightLayout->setSpacing(10);
        auto* form = new QFormLayout;
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        providerEnabled = new QCheckBox(QStringLiteral("在助手中可用"));
        providerEnabled->setObjectName(QStringLiteral("agentProviderEnabled"));
        providerName = Edit(QStringLiteral("agentProviderName"), QStringLiteral("提供商名称"));
        protocol = new QComboBox;
        protocol->setObjectName(QStringLiteral("agentProviderProtocol"));
        protocol->addItem(QStringLiteral("OpenAI 兼容 · Chat Completions"), QStringLiteral("openai-chat"));
        protocol->addItem(QStringLiteral("OpenAI 兼容 · Responses"), QStringLiteral("openai-responses"));
        protocol->addItem(QStringLiteral("Anthropic Messages"), QStringLiteral("anthropic"));
        protocol->addItem(QStringLiteral("Google Gemini"), QStringLiteral("gemini"));
        website = Edit(QStringLiteral("agentProviderWebsite"), QStringLiteral("https://example.com"));
        baseUrl = Edit(QStringLiteral("agentProviderBaseUrl"), QStringLiteral("https://api.example.com/v1"));
        form->addRow(QStringLiteral("名称"), providerName);
        form->addRow(QStringLiteral("状态"), providerEnabled);
        form->addRow(QStringLiteral("API 协议"), protocol);
        form->addRow(QStringLiteral("网站"), website);
        form->addRow(QStringLiteral("API 地址"), baseUrl);
        auto* secretRow = new QHBoxLayout;
        secret = Edit(QStringLiteral("agentProviderSecret"), QStringLiteral("未设置，可留空连接本机服务"));
        secret->setMaxLength(32768);
        secret->setEchoMode(QLineEdit::Password);
        auto* reveal = Button(QStringLiteral("显示"));
        reveal->setCheckable(true);
        auto* clear = Button(QStringLiteral("清除"), QStringLiteral("agentClearSecret"));
        secretRow->addWidget(secret, 1);
        secretRow->addWidget(reveal);
        secretRow->addWidget(clear);
        form->addRow(QStringLiteral("API 密钥"), secretRow);
        rightLayout->addLayout(form);
        auto* modelToolbar = new QHBoxLayout;
        fetchModels = Button(QStringLiteral("获取模型"), QStringLiteral("agentFetchModels"));
        cancelFetch = Button(QStringLiteral("取消读取"));
        cancelFetch->setVisible(false);
        auto* addModel = Button(QStringLiteral("手工添加"), QStringLiteral("agentAddModel"));
        auto* deleteModel = Button(QStringLiteral("移除模型"), QStringLiteral("agentRemoveModel"));
        modelToolbar->addWidget(fetchModels);
        modelToolbar->addWidget(cancelFetch);
        modelToolbar->addStretch(1);
        modelToolbar->addWidget(addModel);
        modelToolbar->addWidget(deleteModel);
        rightLayout->addLayout(modelToolbar);
        models = new QTableWidget(0, 4);
        models->setObjectName(QStringLiteral("agentModelTable"));
        models->setHorizontalHeaderLabels({QStringLiteral("启用"), QStringLiteral("模型 ID"), QStringLiteral("上下文"), QStringLiteral("来源")});
        models->setSelectionBehavior(QAbstractItemView::SelectRows);
        models->setSelectionMode(QAbstractItemView::SingleSelection);
        models->setEditTriggers(QAbstractItemView::NoEditTriggers);
        models->verticalHeader()->setVisible(false);
        models->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        models->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        models->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
        models->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
        models->setShowGrid(false);
        models->setMinimumHeight(180);
        rightLayout->addWidget(models, 1);
        modelEditor = new QWidget;
        auto* modelForm = new QFormLayout(modelEditor);
        modelForm->setContentsMargins(0, 0, 0, 0);
        modelId = Edit(QStringLiteral("agentModelId"));
        modelName = Edit(QStringLiteral("agentModelName"));
        context = Tokens(QStringLiteral("agentModelContextOverride"));
        output = Tokens(QStringLiteral("agentModelMaxOutput"));
        contextStatus = Text({}, nullptr, true);
        contextStatus->setObjectName(QStringLiteral("agentModelContextSource"));
        toolsSupported = new QCheckBox(QStringLiteral("允许工具调用"));
        imagesSupported = new QCheckBox(QStringLiteral("允许图片输入"));
        auto* capabilities = new QHBoxLayout;
        capabilities->addWidget(toolsSupported);
        capabilities->addWidget(imagesSupported);
        capabilities->addStretch(1);
        modelForm->addRow(QStringLiteral("模型 ID"), modelId);
        modelForm->addRow(QStringLiteral("显示名称"), modelName);
        modelForm->addRow(QStringLiteral("上下文 tokens"), context);
        modelForm->addRow(QStringLiteral("生效值"), contextStatus);
        modelForm->addRow(QStringLiteral("输出上限"), output);
        modelForm->addRow(QStringLiteral("模型能力"), capabilities);
        rightLayout->addWidget(modelEditor);
        rightLayout->addWidget(Text(QStringLiteral("能力开关是调用偏好，需站点实际支持。密钥只在保存设置时加密写入本机用户目录。"), nullptr, true));
        scroll->setWidget(providerEditor);
        split->addWidget(scroll);
        split->setSizes({170, 550});
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        layout->addWidget(split, 1);
        pages->addWidget(page);
        catalog = new ProviderCatalog(dialog);
        QObject::connect(providers, &QListWidget::currentRowChanged, dialog, [this](int row) { LoadProvider(row); });
        QObject::connect(addProvider, &QPushButton::clicked, dialog, [this] {
            ProviderSettings provider;
            provider.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            provider.name = QStringLiteral("新提供商");
            settings.providers.push_back(provider);
            RefreshProviders(settings.providers.size() - 1);
            providerName->setFocus(); providerName->selectAll();
        });
        QObject::connect(deleteProvider, &QPushButton::clicked, dialog, [this] {
            if (CurrentProvider() == nullptr) return;
            pendingSecrets.remove(CurrentProvider()->id);
            settings.providers.removeAt(providerIndex);
            RefreshProviders(std::min(providerIndex, static_cast<int>(settings.providers.size()) - 1));
        });
        const auto providerEdited = [this] {
            if (loading || CurrentProvider() == nullptr) return;
            catalog->Cancel();
            auto* provider = CurrentProvider();
            if (importedSettings && provider->baseUrl != baseUrl->text().trimmed()) {
                provider->credentialRef.clear();
                secret->setPlaceholderText(QStringLiteral("地址已变化，请重新填写密钥"));
            }
            provider->name = providerName->text(); provider->website = website->text().trimmed();
            provider->baseUrl = baseUrl->text().trimmed(); provider->enabled = providerEnabled->isChecked();
            if (auto* item = providers->item(providerIndex)) item->setText(provider->name.isEmpty() ? QStringLiteral("未命名提供商") : provider->name);
            UpdateStatus();
        };
        for (auto* edit : {providerName, website, baseUrl}) QObject::connect(edit, &QLineEdit::textChanged, dialog, providerEdited);
        QObject::connect(providerEnabled, &QCheckBox::toggled, dialog, providerEdited);
        QObject::connect(protocol, &QComboBox::currentIndexChanged, dialog, [this] {
            if (loading || CurrentProvider() == nullptr) return;
            catalog->Cancel();
            auto* provider = CurrentProvider();
            const auto previous = provider->protocol;
            provider->protocol = protocol->currentData().toString();
            if (importedSettings && previous != provider->protocol) {
                provider->credentialRef.clear();
                secret->setPlaceholderText(QStringLiteral("协议已变化，请重新填写密钥"));
            }
            if (baseUrl->text().isEmpty() || baseUrl->text() == DefaultBase(previous)) baseUrl->setText(DefaultBase(provider->protocol));
        });
        QObject::connect(secret, &QLineEdit::textChanged, dialog, [this] {
            if (!loading && CurrentProvider() != nullptr) {
                if (secret->text().isEmpty()) pendingSecrets.remove(CurrentProvider()->id);
                else pendingSecrets.insert(CurrentProvider()->id, secret->text());
            }
        });
        QObject::connect(reveal, &QPushButton::toggled, dialog, [this](bool checked) { secret->setEchoMode(checked ? QLineEdit::Normal : QLineEdit::Password); });
        QObject::connect(clear, &QPushButton::clicked, dialog, [this] {
            if (CurrentProvider() == nullptr) return;
            const QSignalBlocker blocker(secret);
            secret->clear(); secret->setPlaceholderText(QStringLiteral("将在保存设置时移除密钥"));
            pendingSecrets.insert(CurrentProvider()->id, QString{});
        });
        QObject::connect(fetchModels, &QPushButton::clicked, dialog, [this] { FetchModels(); });
        QObject::connect(cancelFetch, &QPushButton::clicked, catalog, &ProviderCatalog::Cancel);
        QObject::connect(catalog, &ProviderCatalog::BusyChanged, dialog, [this](bool busy) {
            fetchModels->setEnabled(!busy); cancelFetch->setVisible(busy);
        });
        QObject::connect(catalog, &ProviderCatalog::Failed, dialog, [this](const QString&, const QString& error) { Message(error); });
        QObject::connect(catalog, &ProviderCatalog::ModelsReady, dialog, [this](const QString& id, const QList<ModelSettings>& result) { MergeModels(id, result); });
        QObject::connect(addModel, &QPushButton::clicked, dialog, [this] {
            auto* provider = CurrentProvider(); if (provider == nullptr) return;
            ModelSettings model;
            int suffix = 1;
            do { model.id = QStringLiteral("model-%1").arg(suffix++); }
            while (std::any_of(provider->models.cbegin(), provider->models.cend(), [&model](const auto& existing) { return existing.id == model.id; }));
            model.name = model.id; provider->models.push_back(model);
            RefreshModels(static_cast<int>(provider->models.size()) - 1);
            modelId->setFocus(); modelId->selectAll(); UpdateStatus();
        });
        QObject::connect(deleteModel, &QPushButton::clicked, dialog, [this] {
            if (CurrentModel() == nullptr) return;
            CurrentProvider()->models.removeAt(modelIndex);
            RefreshModels(std::min(modelIndex, static_cast<int>(CurrentProvider()->models.size()) - 1)); UpdateStatus();
        });
        QObject::connect(models, &QTableWidget::currentCellChanged, dialog, [this](int row) { LoadModel(row); });
        QObject::connect(models, &QTableWidget::itemChanged, dialog, [this](QTableWidgetItem* item) {
            if (loading || item->column() != 0 || CurrentProvider() == nullptr) return;
            CurrentProvider()->models[item->row()].enabled = item->checkState() == Qt::Checked; UpdateStatus();
        });
        const auto modelEdited = [this] {
            auto* model = CurrentModel(); if (loading || model == nullptr) return;
            model->id = modelId->text().trimmed(); model->name = modelName->text();
            model->contextOverride = context->text().toLongLong(); model->maxOutputTokens = output->text().toLongLong();
            model->toolsSupported = toolsSupported->isChecked(); model->imagesSupported = imagesSupported->isChecked();
            UpdateModelRow(modelIndex); UpdateStatus();
        };
        for (auto* edit : {modelId, modelName, context, output}) QObject::connect(edit, &QLineEdit::textChanged, dialog, modelEdited);
        QObject::connect(toolsSupported, &QCheckBox::toggled, dialog, modelEdited);
        QObject::connect(imagesSupported, &QCheckBox::toggled, dialog, modelEdited);
        RefreshProviders(settings.providers.isEmpty() ? -1 : 0);
    }

    void BuildPermissions() {
        QVBoxLayout* layout = nullptr;
        auto* page = Page(QStringLiteral("Agent 权限"), QStringLiteral("权限在工具执行入口检查；已授权任务范围内连续执行。"), layout);
        for (const auto& entry : {std::pair{QStringLiteral("允许修改地图"), &settings.allowEdits},
            std::pair{QStringLiteral("允许保存地图"), &settings.allowSave}, std::pair{QStringLiteral("允许外部命令"), &settings.allowCommands}}) {
            auto* check = new QCheckBox(entry.first); check->setChecked(*entry.second);
            layout->addWidget(check);
            QObject::connect(check, &QCheckBox::toggled, dialog, [target = entry.second](bool value) { *target = value; });
        }
        layout->addSpacing(12);
        layout->addWidget(Text(QStringLiteral("关闭修改和保存后仍可查询。编辑、撤销、来源检查与保存验证使用编辑器同一套服务。"), nullptr, true));
        layout->addStretch(1);
        pages->addWidget(page);
    }

    void BuildMcp() {
        QVBoxLayout* layout = nullptr;
        auto* page = Page(QStringLiteral("MCP 连接"), QStringLiteral("外部 Agent 可独立使用 MCP；无需启用内置 AI 或填写模型 API 密钥。"), layout);
        mcpEnabled = new QCheckBox(QStringLiteral("启用本机 HTTP MCP"));
        mcpEnabled->setObjectName(QStringLiteral("agentMcpEnabled"));
        mcpEnabled->setChecked(settings.mcpEnabled);
        layout->addWidget(mcpEnabled);
        auto* form = new QFormLayout;
        port = new QSpinBox;
        port->setObjectName(QStringLiteral("agentMcpPort"));
        port->setRange(1, 65535); port->setValue(settings.mcpPort);
        port->setMaximumWidth(150);
        endpoint = Text({}); endpoint->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(QStringLiteral("端口"), port);
        form->addRow(QStringLiteral("监听地址"), endpoint);
        layout->addLayout(form);
        auto* connectionButtons = new QHBoxLayout;
        auto* copyAddress = Button(QStringLiteral("复制地址"));
        copyConfig = Button(QStringLiteral("复制客户端配置"), QStringLiteral("agentCopyMcpConfig"));
        copyConfig->setEnabled(false);
        connectionButtons->addWidget(copyAddress); connectionButtons->addWidget(copyConfig); connectionButtons->addStretch(1);
        layout->addLayout(connectionButtons);
        mcpStatus = Text(QStringLiteral("配置保存后启动；本机访问令牌由编辑器提供。"), nullptr, true);
        layout->addWidget(mcpStatus);
        layout->addSpacing(12);
        auto* search = Edit(QStringLiteral("agentToolSearch"), QStringLiteral("筛选工具名称"));
        layout->addWidget(search);
        toolList = new QListWidget; toolList->setObjectName(QStringLiteral("agentMcpTools"));
        layout->addWidget(toolList, 1);
        layout->addWidget(Text(QStringLiteral("关闭的工具从列表移除并拒绝执行。修改开关后，外部客户端需刷新工具列表。"), nullptr, true));
        pages->addWidget(page);
        QObject::connect(mcpEnabled, &QCheckBox::toggled, dialog, [this](bool value) { settings.mcpEnabled = value; });
        QObject::connect(port, &QSpinBox::valueChanged, dialog, [this](int value) { settings.mcpPort = static_cast<quint16>(value); UpdateEndpoint(); });
        QObject::connect(copyAddress, &QPushButton::clicked, dialog, [this] { QApplication::clipboard()->setText(endpoint->text()); });
        QObject::connect(copyConfig, &QPushButton::clicked, dialog, [this] {
            if (mcpToken.isEmpty()) return;
            const QJsonObject connection{{QStringLiteral("url"), endpoint->text()},
                {QStringLiteral("headers"), QJsonObject{{QStringLiteral("Authorization"), QStringLiteral("Bearer ") + mcpToken}}}};
            const QJsonObject config{{QStringLiteral("mcpServers"), QJsonObject{{QStringLiteral("forge"), connection}}}};
            QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Indented)));
            Message(QStringLiteral("已复制含本机访问令牌的客户端配置，请只交给本机可信客户端。"));
        });
        QObject::connect(toolList, &QListWidget::itemChanged, dialog, [this](QListWidgetItem* item) {
            const auto name = item->data(Qt::UserRole).toString();
            settings.disabledTools.removeAll(name);
            if (item->checkState() != Qt::Checked) settings.disabledTools.push_back(name);
        });
        QObject::connect(search, &QLineEdit::textChanged, dialog, [this](const QString& value) {
            for (int row = 0; row < toolList->count(); ++row) toolList->item(row)->setHidden(!toolList->item(row)->text().contains(value, Qt::CaseInsensitive));
        });
        UpdateEndpoint();
    }

    void UpdateStatus() {
        RefreshTransferSummary();
        qsizetype count = 0;
        for (const auto& provider : settings.providers) if (provider.enabled && !provider.baseUrl.isEmpty())
            for (const auto& model : provider.models) if (model.enabled && !model.id.isEmpty()) ++count;
        status->setText(!settings.aiEnabled ? QStringLiteral("内置 AI 已关闭；MCP 可单独开启。")
            : count == 0 ? QStringLiteral("已开启，尚无可选模型。请添加提供商，并获取或手工添加模型。")
            : QStringLiteral("已配置 %1 个可选模型。助手运行时会检查连接、密钥与模型能力。").arg(count));
    }

    void UpdateEndpoint() { endpoint->setText(QStringLiteral("http://127.0.0.1:%1/mcp").arg(settings.mcpPort)); }

    void RefreshProviders(qsizetype selected) {
        { const QSignalBlocker blocker(providers); providers->clear();
            for (const auto& provider : settings.providers) providers->addItem(provider.name.isEmpty() ? QStringLiteral("未命名提供商") : provider.name);
            providers->setCurrentRow(static_cast<int>(selected)); }
        LoadProvider(static_cast<int>(selected));
    }

    void LoadProvider(int row) {
        if (catalog != nullptr) catalog->Cancel();
        loading = true; providerIndex = row; modelIndex = -1;
        auto* provider = CurrentProvider(); providerEditor->setEnabled(provider != nullptr);
        providerName->setText(provider != nullptr ? provider->name : QString{});
        website->setText(provider != nullptr ? provider->website : QString{});
        baseUrl->setText(provider != nullptr ? provider->baseUrl : QString{});
        providerEnabled->setChecked(provider != nullptr && provider->enabled);
        protocol->setCurrentIndex(provider != nullptr ? protocol->findData(provider->protocol) : 0);
        secret->setText(provider != nullptr ? pendingSecrets.value(provider->id) : QString{});
        secret->setEchoMode(QLineEdit::Password);
        secret->setPlaceholderText(provider != nullptr && !provider->credentialRef.isEmpty()
            ? QStringLiteral("已保存，留空保持原密钥") : QStringLiteral("未设置，可留空连接本机服务"));
        loading = false;
        RefreshModels(provider == nullptr || provider->models.isEmpty() ? -1 : 0);
    }

    void RefreshModels(int selected) {
        loading = true;
        const auto* provider = CurrentProvider(); models->clearContents();
        models->setRowCount(provider != nullptr ? static_cast<int>(provider->models.size()) : 0);
        if (provider != nullptr) for (int row = 0; row < models->rowCount(); ++row) UpdateModelRow(row);
        loading = false;
        models->setCurrentCell(selected, selected >= 0 ? 1 : -1);
        LoadModel(selected);
    }

    void UpdateModelRow(int row) {
        const QSignalBlocker blocker(models);
        const auto& model = CurrentProvider()->models[row];
        if (models->item(row, 0) == nullptr) {
            for (int column = 0; column < 4; ++column) models->setItem(row, column, new QTableWidgetItem);
            models->item(row, 0)->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable);
        }
        const auto effective = ResolveContextWindow(model);
        models->item(row, 0)->setCheckState(model.enabled ? Qt::Checked : Qt::Unchecked);
        models->item(row, 1)->setText(model.id);
        models->item(row, 1)->setToolTip(model.name);
        models->item(row, 2)->setText(QString::number(effective.tokens));
        models->item(row, 3)->setText(effective.source);
        if (row == modelIndex) contextStatus->setText(QStringLiteral("%1 tokens · %2%3")
            .arg(effective.tokens).arg(effective.source, effective.estimated ? QStringLiteral("（估算）") : QString{}));
    }

    void LoadModel(int row) {
        loading = true; modelIndex = row;
        const auto* model = CurrentModel(); modelEditor->setEnabled(model != nullptr);
        modelId->setText(model != nullptr ? model->id : QString{});
        modelName->setText(model != nullptr ? model->name : QString{});
        context->setText(model != nullptr && model->contextOverride > 0 ? QString::number(model->contextOverride) : QString{});
        output->setText(model != nullptr && model->maxOutputTokens > 0 ? QString::number(model->maxOutputTokens) : QString{});
        toolsSupported->setChecked(model != nullptr && model->toolsSupported);
        imagesSupported->setChecked(model != nullptr && model->imagesSupported);
        if (model != nullptr) UpdateModelRow(row); else contextStatus->clear();
        loading = false;
    }

    void FetchModels() {
        const auto* provider = CurrentProvider(); if (provider == nullptr) return;
        QString error;
        const QString value = pendingSecrets.contains(provider->id) ? pendingSecrets.value(provider->id) : credentialStore->Read(provider->credentialRef, &error);
        if (!error.isEmpty()) { Message(error); return; }
        Message(QStringLiteral("正在读取站点实际模型目录…"));
        catalog->Fetch(*provider, value);
    }

    void MergeModels(const QString& id, const QList<ModelSettings>& result) {
        auto* provider = CurrentProvider(); if (provider == nullptr || provider->id != id) return;
        for (const auto& fetched : result) {
            auto existing = std::find_if(provider->models.begin(), provider->models.end(), [&fetched](const auto& model) { return model.id == fetched.id; });
            if (existing == provider->models.end()) provider->models.push_back(fetched);
            else { existing->providerContext = fetched.providerContext;
                if (existing->maxOutputTokens == 0) existing->maxOutputTokens = fetched.maxOutputTokens;
                if (existing->name.isEmpty() || existing->name == existing->id) existing->name = fetched.name; }
        }
        RefreshModels(provider->models.isEmpty() ? -1 : std::max(modelIndex, 0));
        Message(QStringLiteral("已读取 %1 个模型；新模型需勾选启用，原有配置保留。").arg(result.size())); UpdateStatus();
    }

    void ClearSecrets() {
        for (auto iterator = pendingSecrets.begin(); iterator != pendingSecrets.end(); ++iterator) iterator.value().fill(QChar(0));
        pendingSecrets.clear();
        const QSignalBlocker blocker(secret); secret->clear();
    }

    AgentSettingsDialog* dialog;
    AgentSettings settings;
    AgentSettings original;
    std::unique_ptr<CredentialStore> credentials;
    CredentialStore* credentialStore;
    std::function<bool(const AgentSettings&, QString*)> commit;
    QHash<QString, QString> pendingSecrets;
    QString mcpToken;
    bool loading = false;
    int providerIndex = -1;
    int modelIndex = -1;
    QListWidget *nav = nullptr, *providers = nullptr, *toolList = nullptr;
    QStackedWidget* pages = nullptr;
    QWidget *providerEditor = nullptr, *modelEditor = nullptr;
    QCheckBox *aiEnabled = nullptr, *reducedMotion = nullptr, *providerEnabled = nullptr,
        *toolsSupported = nullptr, *imagesSupported = nullptr, *mcpEnabled = nullptr;
    QLineEdit *providerName = nullptr, *website = nullptr, *baseUrl = nullptr, *secret = nullptr,
        *modelId = nullptr, *modelName = nullptr, *context = nullptr, *output = nullptr;
    QComboBox* protocol = nullptr;
    QComboBox* uiFont = nullptr;
    QLabel* fontPreview = nullptr;
    QString uiFontFamily;
    bool applicationLoaded = false, importedSettings = false;
    QLineEdit *transferGame = nullptr, *transferUi = nullptr;
    QComboBox* transferTheme = nullptr;
    QLabel *transferFont = nullptr, *transferSummary = nullptr;
    QPushButton *importSettings = nullptr, *exportSettings = nullptr;
    QTableWidget* models = nullptr;
    QSpinBox* port = nullptr;
    QLabel *status = nullptr, *contextStatus = nullptr, *message = nullptr, *endpoint = nullptr, *mcpStatus = nullptr;
    QPushButton *fetchModels = nullptr, *cancelFetch = nullptr, *copyConfig = nullptr;
    ProviderCatalog* catalog = nullptr;
};

AgentSettingsDialog::AgentSettingsDialog(const AgentSettings& settings, QWidget* parent)
    : QDialog(parent), impl_(std::make_unique<Impl>(this, settings)) {}

AgentSettingsDialog::~AgentSettingsDialog() { impl_->ClearSecrets(); impl_->mcpToken.fill(QChar(0)); }

AgentSettings AgentSettingsDialog::Settings() const { return impl_->settings; }

void AgentSettingsDialog::SetCommitHandler(std::function<bool(const AgentSettings&, QString*)> handler) { impl_->commit = std::move(handler); }

void AgentSettingsDialog::SetUiFontFamily(const QString& family) {
    // 本机已不存在的字体按原名保留一项，避免打开设置就悄悄改回默认。
    int index = impl_->uiFont->findData(family);
    if (index < 0 && !family.isEmpty()) {
        impl_->uiFont->addItem(QStringLiteral("%1（本机未安装）").arg(family), family);
        index = impl_->uiFont->count() - 1;
    }
    impl_->uiFont->setCurrentIndex(std::max(0, index));
    impl_->uiFontFamily = family;
}

QString AgentSettingsDialog::UiFontFamily() const { return impl_->uiFontFamily; }

void AgentSettingsDialog::AddGeneralSection(QWidget* section) {
    if (!section) return;
    // 插入页尾的弹性空白与固定说明之前，保持现有设置的排列。
    impl_->generalLayout->insertWidget(std::max(0, impl_->generalLayout->count() - 2), section);
    if (!qobject_cast<QScrollArea*>(impl_->pages->widget(0))) {
        auto* general = impl_->pages->widget(0);
        impl_->pages->removeWidget(general);
        auto* scroll = new QScrollArea;
        scroll->setObjectName(QStringLiteral("applicationGeneralSettingsScroll"));
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setWidget(general);
        impl_->pages->insertWidget(0, scroll);
        impl_->pages->setCurrentIndex(impl_->nav->currentRow());
    }
}

void AgentSettingsDialog::AddApplicationPage(const QString& label, QWidget* page) {
    if (!page || label.isEmpty()) return;
    // 页面只负责内部排版；窗口统一留白，内容过高时在本页滚动。
    if (page->layout()) page->layout()->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setObjectName(page->objectName() + QStringLiteral("Scroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    impl_->nav->addItem(label);
    impl_->pages->addWidget(scroll);
}

void AgentSettingsDialog::SetApplicationPreferences(const ApplicationPreferences& application) {
    impl_->transferGame->setText(application.gameRoot); impl_->transferUi->setText(application.uiDataRoot);
    SetUiFontFamily(application.uiFontFamily);
    impl_->transferFont->setText(application.uiFontFamily.isEmpty() ? QStringLiteral("默认（微软雅黑）") : application.uiFontFamily);
    int theme = impl_->transferTheme->findData(application.themeId);
    if (theme < 0) {
        impl_->transferTheme->addItem(QStringLiteral("%1（当前版本未提供）").arg(application.themeId), application.themeId);
        theme = impl_->transferTheme->count() - 1;
    }
    impl_->transferTheme->setCurrentIndex(theme);
    impl_->applicationLoaded = true;
    impl_->importSettings->setEnabled(true); impl_->exportSettings->setEnabled(true);
    impl_->RefreshTransferSummary();
}

ApplicationPreferences AgentSettingsDialog::ApplicationSettings() const {
    return {impl_->transferGame->text(), impl_->transferUi->text(), impl_->uiFontFamily, impl_->transferTheme->currentData().toString()};
}

bool AgentSettingsDialog::ImportSettingsData(const QByteArray& bytes, QString* error) {
    if (error) error->clear();
    const auto fail = [this, error](const QString& message) {
        if (error) *error = message;
        impl_->Message(message); return false;
    };
    if (!impl_->applicationLoaded) return fail(QStringLiteral("应用设置尚未加载，未暂存导入。"));
    if (!impl_->pendingSecrets.isEmpty())
        return fail(QStringLiteral("存在尚未保存的密钥修改或清除操作，请先保存或取消这些修改，再导入设置。"));
    QString diagnostic;
    auto imported = ApplicationSettingsTransfer::Decode(bytes, &diagnostic);
    if (!imported) return fail(diagnostic);
    if (!imported->application.themeId.isEmpty() && !UiKit::ThemeManager::ThemeIds().contains(imported->application.themeId))
        return fail(QStringLiteral("迁移文件的主题在当前版本中不可用，未暂存导入。"));
    impl_->catalog->Cancel();
    // 凭据只与原已提交地址匹配；未保存的endpoint编辑不能把旧ref转交新地址。
    auto matching = ApplicationSettingsTransfer::MergeProviders(impl_->original, imported->providers);
    impl_->settings.providers = std::move(matching.providers);
    impl_->importedSettings = true;
    SetApplicationPreferences(imported->application);
    impl_->RefreshProviders(impl_->settings.providers.isEmpty() ? -1 : 0);
    impl_->UpdateStatus();
    impl_->Message(QStringLiteral("设置已暂存。可核对目录、字体、主题与提供商；保存设置后应用，目录下次启动生效。"));
    return true;
}

QByteArray AgentSettingsDialog::ExportSettingsData(QString* error) const {
    if (!impl_->applicationLoaded) {
        if (error) *error = QStringLiteral("应用设置尚未加载，未导出。");
        return {};
    }
    return ApplicationSettingsTransfer::Encode(ApplicationSettings(), impl_->settings, error);
}

void AgentSettingsDialog::SetCredentialStore(CredentialStore* store) { if (store != nullptr) impl_->credentialStore = store; }

void AgentSettingsDialog::SetMcpAccessToken(const QString& token) {
    impl_->mcpToken = token;
    impl_->copyConfig->setEnabled(!token.isEmpty());
    impl_->mcpStatus->setText(token.isEmpty() ? QStringLiteral("配置保存后启动；本机访问令牌由编辑器提供。")
        : QStringLiteral("已取得本机访问令牌；复制配置可交给支持 HTTP MCP 的客户端。"));
}

void AgentSettingsDialog::SetAvailableTools(const QStringList& tools) {
    const QSignalBlocker blocker(impl_->toolList);
    impl_->toolList->clear();
    auto names = tools;
    names.append(impl_->settings.disabledTools); names.removeDuplicates(); names.sort();
    for (const auto& name : names) {
        auto* item = new QListWidgetItem(name + (tools.contains(name) ? QString{} : QStringLiteral(" · 当前未提供")), impl_->toolList);
        item->setData(Qt::UserRole, name); item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(impl_->settings.disabledTools.contains(name) ? Qt::Unchecked : Qt::Checked);
    }
}

void AgentSettingsDialog::accept() {
    auto candidate = impl_->settings;
    QString error;
    if (!impl_->commit) { impl_->Message(QStringLiteral("设置保存服务尚未连接，未写入设置或密钥。")); return; }
    if (!AgentSettingsStore::Validate(candidate, &error)) { impl_->Message(error); return; }
    // 普通设置保存保持原提供商验证规则；便携URL白名单只限制导入/导出。
    if (impl_->applicationLoaded && ApplicationSettingsTransfer::Encode(ApplicationSettings(), AgentSettings{}, &error).isEmpty()) {
        impl_->Message(error); return;
    }
    impl_->catalog->Cancel();
    QStringList created;
    for (auto& provider : candidate.providers) {
        if (!impl_->pendingSecrets.contains(provider.id)) continue;
        const auto value = impl_->pendingSecrets.value(provider.id);
        if (value.isEmpty()) provider.credentialRef.clear();
        else {
            const auto reference = QStringLiteral("forge/provider/") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (!impl_->credentialStore->Write(reference, value, &error)) {
                for (const auto& ref : created) static_cast<void>(impl_->credentialStore->Remove(ref));
                impl_->Message(error); return;
            }
            created.push_back(reference); provider.credentialRef = reference;
        }
    }
    if (impl_->commit && !impl_->commit(candidate, &error)) {
        for (const auto& ref : created) static_cast<void>(impl_->credentialStore->Remove(ref));
        impl_->Message(error.isEmpty() ? QStringLiteral("设置未写入；原设置和密钥保留。") : error); return;
    }
    QSet<QString> retained;
    for (const auto& provider : candidate.providers) if (!provider.credentialRef.isEmpty()) retained.insert(provider.credentialRef);
    for (const auto& provider : impl_->original.providers) if (!provider.credentialRef.isEmpty() && !retained.contains(provider.credentialRef)) {
        // 迁移更换地址/提供商不默默删除旧凭据；保留在本机但不转交新endpoint。
        // 用户明确编辑或清除密钥时仍按原事务清理被替换引用。
        if (!impl_->importedSettings || impl_->pendingSecrets.contains(provider.id))
            static_cast<void>(impl_->credentialStore->Remove(provider.credentialRef));
    }
    impl_->settings = candidate;
    impl_->original = candidate;
    impl_->ClearSecrets();
    emit SettingsChanged(candidate);
    QDialog::accept();
}

void AgentSettingsDialog::reject() {
    impl_->catalog->Cancel(); impl_->ClearSecrets();
    QDialog::reject();
}

} // namespace War3Map::Agent
