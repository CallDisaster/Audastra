#include "FeedbackDialog.h"
#include "FeedbackUi.h"

#include <QDateTime>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace War3Map::QtFeedback {
namespace {

QString StateLabel(Feedback::QueueState state) {
    switch (state) {
    case Feedback::QueueState::Pending: return QStringLiteral("待发送");
    case Feedback::QueueState::Sent: return QStringLiteral("已发送");
    case Feedback::QueueState::Failed: return QStringLiteral("失败");
    case Feedback::QueueState::Deleted: return QStringLiteral("用户删除");
    }
    return QStringLiteral("未知");
}

} // namespace

class FeedbackQueuePage::Impl {
public:
    FeedbackQueuePage* page;
    FeedbackClient& client;
    QTableWidget* table = nullptr;
    QLabel* status = nullptr;
    QPushButton* preview = nullptr;
    QPushButton* remove = nullptr;
    quint64 previewRequest = 0;
    quint64 deleteRequest = 0;
    bool clearedSummaryShown = false;

    Impl(FeedbackQueuePage* q, FeedbackClient& service) : page(q), client(service) {
        page->setObjectName(QStringLiteral("feedbackQueuePage"));
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(24, 20, 24, 20);
        layout->setSpacing(12);
        layout->addWidget(Detail::Heading(QStringLiteral("反馈队列")));
        layout->addWidget(Detail::PlainLabel(QStringLiteral("用户和助手的报告共用本地队列；每份报告都需确认完整预览后发送。"), true));
        auto* actions = new QHBoxLayout;
        preview = new QPushButton(QStringLiteral("查看完整内容"));
        preview->setObjectName(QStringLiteral("feedbackQueuePreview"));
        remove = new QPushButton(QStringLiteral("删除"));
        remove->setObjectName(QStringLiteral("feedbackQueueDelete"));
        auto* refresh = new QPushButton(QStringLiteral("刷新"));
        refresh->setObjectName(QStringLiteral("feedbackQueueRefresh"));
        actions->addWidget(preview);
        actions->addWidget(remove);
        actions->addStretch();
        actions->addWidget(refresh);
        layout->addLayout(actions);
        table = new QTableWidget(0, 6);
        table->setObjectName(QStringLiteral("feedbackQueueTable"));
        table->setHorizontalHeaderLabels({QStringLiteral("类型"), QStringLiteral("来源"), QStringLiteral("错误 / 工具"),
            QStringLiteral("次数"), QStringLiteral("状态"), QStringLiteral("最近记录")});
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setAlternatingRowColors(true);
        table->verticalHeader()->setVisible(false);
        table->verticalHeader()->setDefaultSectionSize(32);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
        layout->addWidget(table, 1);
        status = Detail::PlainLabel({}, true);
        status->setObjectName(QStringLiteral("feedbackQueueStatus"));
        layout->addWidget(status);
        QObject::connect(&client, &FeedbackClient::SnapshotChanged, page, [this] { Refresh(); });
        QObject::connect(table, &QTableWidget::itemSelectionChanged, page, [this] { UpdateActions(); });
        QObject::connect(table, &QTableWidget::itemDoubleClicked, page, [this] { Preview(); });
        QObject::connect(preview, &QPushButton::clicked, page, [this] { Preview(); });
        QObject::connect(refresh, &QPushButton::clicked, page, [this] { client.RefreshQueue(); });
        QObject::connect(remove, &QPushButton::clicked, page, [this] {
            const auto id = SelectedId();
            if (id.isEmpty()) return;
            deleteRequest = client.DeleteReport(id);
            UpdateActions();
        });
        QObject::connect(&client, &FeedbackClient::PreviewReady, page,
            [this](quint64 request, const Feedback::Preview& report, const QString& endpoint) {
                if (request != previewRequest) return;
                previewRequest = 0;
                auto* dialog = new FeedbackPreviewDialog(client, report, endpoint, page);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->setWindowModality(Qt::WindowModal);
                dialog->show();
                UpdateActions();
            });
        QObject::connect(&client, &FeedbackClient::OperationFinished, page,
            [this](quint64 request, bool, const QString& message) {
                if (request == 0) { if (!message.isEmpty()) status->setText(message); return; }
                if (request == previewRequest) previewRequest = 0;
                else if (request == deleteRequest) deleteRequest = 0;
                else return;
                if (!message.isEmpty()) status->setText(message);
                UpdateActions();
            });
        Refresh();
        client.RefreshQueue();
    }

    QString SelectedId() const {
        const auto row = table->currentRow();
        return row < 0 || !table->item(row, 0) ? QString() : table->item(row, 0)->data(Qt::UserRole).toString();
    }

    const Feedback::EntrySummary* Selected() const {
        const auto id = SelectedId().toStdString();
        for (const auto& entry : client.Snapshot().entries) if (entry.id == id) return &entry;
        return nullptr;
    }

    void Refresh() {
        const auto oldId = SelectedId();
        table->setRowCount(static_cast<int>(client.Snapshot().entries.size()));
        int row = 0;
        int oldRow = -1;
        for (auto iterator = client.Snapshot().entries.rbegin(); iterator != client.Snapshot().entries.rend(); ++iterator, ++row) {
            const auto& entry = *iterator;
            const QStringList cells{
                entry.kind == Feedback::ReportKind::Problem ? QStringLiteral("问题") : QStringLiteral("建议"),
                entry.source == Feedback::ReportSource::Agent ? QStringLiteral("助手") : QStringLiteral("用户"),
                QString::fromStdString(entry.errorCode.empty() ? entry.toolName : entry.errorCode),
                QString::number(entry.occurrences), StateLabel(entry.state),
                QDateTime::fromSecsSinceEpoch(entry.lastSeenUnixSeconds).toLocalTime().toString(QStringLiteral("MM-dd HH:mm"))};
            for (int column = 0; column < cells.size(); ++column) {
                auto* item = new QTableWidgetItem(cells[column]);
                item->setData(Qt::UserRole, QString::fromStdString(entry.id));
                table->setItem(row, column, item);
            }
            if (QString::fromStdString(entry.id) == oldId) oldRow = row;
        }
        if (oldRow >= 0) table->selectRow(oldRow);
        else if (table->rowCount() > 0) table->selectRow(0);
        status->setText(table->rowCount() == 0 ? QStringLiteral("暂无本地反馈。") : client.Snapshot().consent
            ? QStringLiteral("已同意上报。选择报告，查看完整内容后确认发送。")
            : QStringLiteral("未同意上报。报告仍可在本地查看、删除或导出。"));
        UpdateActions();
    }

    void UpdateActions() {
        const auto* entry = Selected();
        if (clearedSummaryShown && (!entry || entry->hasPayload)) {
            status->setText(!entry ? QStringLiteral("暂无本地反馈。") : client.Snapshot().consent
                ? QStringLiteral("已同意上报。选择报告，查看完整内容后确认发送。")
                : QStringLiteral("未同意上报。报告仍可在本地查看、删除或导出。"));
            clearedSummaryShown = false;
        }
        preview->setEnabled(entry && entry->hasPayload && previewRequest == 0 && deleteRequest == 0);
        remove->setEnabled(entry && entry->state != Feedback::QueueState::Deleted && deleteRequest == 0);
        if (entry && !entry->failureReason.empty()) status->setText(QString::fromStdString(entry->failureReason));
        if (entry && !entry->hasPayload) {
            const auto cleared = entry->state == Feedback::QueueState::Sent
                ? QStringLiteral("内容已在发送后清除，仅保留状态摘要。")
                : QStringLiteral("内容已按用户删除操作清除，仅保留删除状态。");
            status->setText(cleared);
            preview->setToolTip(cleared);
            clearedSummaryShown = true;
        } else preview->setToolTip({});
    }

    void Preview() {
        const auto* entry = Selected();
        if (!entry || !entry->hasPayload || previewRequest != 0) return;
        previewRequest = client.RequestPreview(QString::fromStdString(entry->id));
        UpdateActions();
    }
};

FeedbackQueuePage::FeedbackQueuePage(FeedbackClient& client, QWidget* parent)
    : QWidget(parent), impl_(std::make_unique<Impl>(this, client)) {}
FeedbackQueuePage::~FeedbackQueuePage() = default;

} // namespace War3Map::QtFeedback
