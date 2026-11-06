// 文件作用：呈现远端 Session 的可见记录；普通输入按原问题或任务投递，权限决定独立发往所属设备。
#include "agent/RemoteAgentPanel.h"

#include "MaiCloudDriveClient.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QMap>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "markdown/MarkdownView.h"
#include "ui/ComposerTextEdit.h"
#include "ui/UiZoom.h"

namespace {

QString sessionKey(const QJsonObject& value) {
    return value.value(QStringLiteral("device_slot")).toString() + QLatin1Char(':') +
           value.value(QStringLiteral("login_session_id")).toString() + QLatin1Char(':') +
           value.value(QStringLiteral("id")).toString();
}

QString attachmentSummary(const QStringList& paths) {
    QStringList names;
    for (const QString& path : paths.mid(0, 3)) names.append(QFileInfo(path).fileName());
    return QStringLiteral("待发送 %1 项：%2%3")
        .arg(paths.size())
        .arg(names.join(QStringLiteral("、")), paths.size() > 3 ? QStringLiteral(" 等") : QString());
}

QString questionText(const QJsonObject& input) {
    QString text = input.value(QStringLiteral("question")).toString();
    if (text.isEmpty()) return {};
    int index = 0;
    for (const QJsonValue& option : input.value(QStringLiteral("options")).toArray())
        text += QStringLiteral("\n%1. %2").arg(++index).arg(option.toString());
    return text;
}

QString questionPartText(const QJsonObject& part) {
    if (part.value(QStringLiteral("kind")) != QStringLiteral("tool") ||
        part.value(QStringLiteral("tool")) != QStringLiteral("question"))
        return {};
    const QJsonValue input = part.value(QStringLiteral("input"));
    return questionText(input.isObject() ? input.toObject()
                                         : QJsonDocument::fromJson(input.toString().toUtf8()).object());
}

QString questionPartId(const QJsonObject& question) {
    const QString part = question.value(QStringLiteral("partId")).toString();
    return part.isEmpty() ? QStringLiteral("question-part:") + question.value(QStringLiteral("id")).toString() : part;
}

QJsonObject questionMessage(const QJsonObject& question) {
    const QString input = QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("question"), question.value(QStringLiteral("question"))},
                                  {QStringLiteral("options"), question.value(QStringLiteral("options"))}})
            .toJson(QJsonDocument::Compact));
    return {
        {QStringLiteral("id"), QStringLiteral("pending-question:") + question.value(QStringLiteral("id")).toString()},
        {QStringLiteral("role"), QStringLiteral("assistant")},
        {QStringLiteral("created"), question.value(QStringLiteral("asked"))},
        {QStringLiteral("questionProjection"), true},
        {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("id"), questionPartId(question)},
                                                         {QStringLiteral("kind"), QStringLiteral("tool")},
                                                         {QStringLiteral("tool"), QStringLiteral("question")},
                                                         {QStringLiteral("state"), QStringLiteral("running")},
                                                         {QStringLiteral("input"), input}}}}};
}

double coreIdTime(const QString& id) {
    if ((!id.startsWith(QStringLiteral("prt_")) && !id.startsWith(QStringLiteral("msg_"))) || id.size() < 14) return 0;
    const QString alphabet = QStringLiteral("0123456789ABCDEFGHJKMNPQRSTVWXYZ");
    quint64 value = 0;
    for (const QChar ch : id.mid(4, 10)) {
        const int index = alphabet.indexOf(ch);
        if (index < 0) return 0;
        value = value * 32 + quint64(index);
    }
    return double(value);
}

QList<QJsonObject> questionDisplayTimeline(const QMap<QString, QJsonObject>& source) {
    struct Unit {
        QJsonObject message;
        QJsonArray parts;
        double time;
        QString order;
    };
    QVector<Unit> units;
    for (const QJsonObject& message : source) {
        const QString id = message.value(QStringLiteral("id")).toString();
        const QJsonArray parts = message.value(QStringLiteral("parts")).toArray();
        double previous = message.value(QStringLiteral("created")).toDouble();
        if (message.value(QStringLiteral("role")) == QStringLiteral("user") || parts.isEmpty()) {
            units.append({message, parts, previous, coreIdTime(id) > 0 ? id.mid(4) : id});
            continue;
        }
        for (int index = 0; index < parts.size(); ++index) {
            const QJsonObject part = parts[index].toObject();
            if (part.value(QStringLiteral("kind")) == QStringLiteral("reasoning")) continue;
            const QString partId = part.value(QStringLiteral("id")).toString();
            double time = part.value(QStringLiteral("created")).toDouble();
            if (time <= 0) time = coreIdTime(partId);
            previous = qMax(previous, time);
            const QString order =
                coreIdTime(partId) > 0 ? partId.mid(4) : id + QStringLiteral(":%1").arg(index, 6, 10, QLatin1Char('0'));
            units.append({message, QJsonArray{part}, previous, order});
        }
    }
    std::stable_sort(units.begin(), units.end(), [](const Unit& a, const Unit& b) {
        return a.time == b.time ? a.order < b.order : a.time < b.time;
    });
    QList<QJsonObject> result;
    QHash<QString, int> occurrences;
    for (const Unit& unit : units) {
        const QString sourceId = unit.message.value(QStringLiteral("id")).toString();
        if (!result.isEmpty() && result.last().value(QStringLiteral("source_message_id")).toString() == sourceId &&
            unit.message.value(QStringLiteral("role")) != QStringLiteral("user")) {
            QJsonArray parts = result.last().value(QStringLiteral("parts")).toArray();
            for (const auto& part : unit.parts) parts.append(part);
            result.last().insert(QStringLiteral("parts"), parts);
        } else {
            QJsonObject fragment = unit.message;
            fragment.insert(QStringLiteral("source_message_id"), sourceId);
            fragment.insert(QStringLiteral("parts"), unit.parts);
            if (occurrences.value(sourceId) > 0) {
                const QString part = unit.parts.isEmpty() ? QStringLiteral("empty")
                                                          : unit.parts.first()
                                                                .toObject()
                                                                .value(QStringLiteral("id"))
                                                                .toString(QString::number(occurrences.value(sourceId)));
                fragment.insert(QStringLiteral("id"), sourceId + QStringLiteral(":segment:") + part);
            }
            ++occurrences[sourceId];
            result.append(fragment);
        }
    }
    return result;
}

QString messageText(const QJsonObject& message) {
    QStringList text;
    int toolCount = 0;
    QString lastTool;
    QString lastState;
    for (const QJsonValue& value : message.value(QStringLiteral("parts")).toArray()) {
        const QJsonObject part = value.toObject();
        const QString kind = part.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("text"))
            text.append(part.value(QStringLiteral("text")).toString());
        else if (kind == QStringLiteral("tool")) {
            const QString question = questionPartText(part);
            if (!question.isEmpty()) {
                text.append(question);
                continue;
            }
            ++toolCount;
            lastTool = part.value(QStringLiteral("tool")).toString();
            lastState = part.value(QStringLiteral("state")).toString();
        } else if (!part.value(QStringLiteral("attachment_id")).toString().isEmpty()) {
            const QString id = part.value(QStringLiteral("attachment_id")).toString();
            QString name = part.value(QStringLiteral("name")).toString(QStringLiteral("附件"));
            name.replace(QLatin1Char('['), QLatin1Char(' '));
            name.replace(QLatin1Char(']'), QLatin1Char(' '));
            name.replace(QLatin1Char('\n'), QLatin1Char(' '));
            text.append(QStringLiteral("[下载%1 · %2](maichat-attachment:%3)")
                            .arg(kind == QStringLiteral("image")   ? QStringLiteral("图片")
                                 : kind == QStringLiteral("video") ? QStringLiteral("视频")
                                 : kind == QStringLiteral("audio") ? QStringLiteral("音频")
                                                                   : QStringLiteral("文件"),
                                 name, id));
        } else if (kind == QStringLiteral("image") || kind == QStringLiteral("video")) {
            text.append(kind == QStringLiteral("image") ? QStringLiteral("[图片在所属设备，暂不可跨设备预览]")
                                                        : QStringLiteral("[视频在所属设备，暂不可跨设备预览]"));
        }
    }
    if (toolCount > 0) {
        text.append(QStringLiteral("工具调用 %1 次 · %2 · %3").arg(toolCount).arg(lastTool, lastState));
    }
    return text.join(QStringLiteral("\n\n"));
}

QIcon actionIcon(bool stop, const QColor& color) {
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (stop) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(14, 14, 20, 20), 3, 3);
    } else {
        painter.drawLine(QPointF(24, 38), QPointF(24, 10));
        painter.drawLine(QPointF(13, 21), QPointF(24, 10));
        painter.drawLine(QPointF(35, 21), QPointF(24, 10));
    }
    return QIcon(pixmap);
}

class RemotePromptEdit final : public ComposerTextEdit {
public:
    explicit RemotePromptEdit(QWidget* parent) : ComposerTextEdit(parent) {}
    std::function<void()> submit;

protected:
    void keyPressEvent(QKeyEvent* event) override {
        const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
        if (enter && !(event->modifiers() & Qt::ShiftModifier)) {
            if (submit) submit();
            return;
        }
        ComposerTextEdit::keyPressEvent(event);
    }
};

}  // namespace

struct RemoteAgentPanel::Runtime {
    QJsonObject session;
    QLabel* status = nullptr;
    QLabel* thinking = nullptr;
    MarkdownView* view = nullptr;
    RemotePromptEdit* editor = nullptr;
    QPushButton* send = nullptr;
    QPushButton* stop = nullptr;
    QPushButton* recover = nullptr;
    QString outboxFile;
    QJsonArray outbox;
    QHash<QString, QString> controlRequests;
    QHash<QString, QJsonObject> owners;
    bool restoringDraft = false;
    bool storageUsable = true;
    QPushButton* older = nullptr;
    QLabel* attachmentsLabel = nullptr;
    QPushButton* clearAttachments = nullptr;
    QWidget* requests = nullptr;
    QVBoxLayout* requestsLayout = nullptr;
    QTimer historyTimeout;
    QMap<QString, QJsonObject> messages;
    QMap<QString, QJsonObject> windowUpdates;
    QStringList windowUpdateOrder;
    QSet<QString> historyMessageIds;
    void retainWindowUpdate(const QString& id, const QJsonObject& message) {
        windowUpdates.insert(id, message);
        windowUpdateOrder.removeAll(id);
        windowUpdateOrder.append(id);
        while (windowUpdateOrder.size() > 20) windowUpdates.remove(windowUpdateOrder.takeFirst());
    }
    QMap<QString, QJsonObject> questionMessages;
    QJsonArray questions;
    // ACK precedes the next canonical tool snapshot. These are display echoes,
    // never invented Core user rows or remote user_message_id receipts.
    QHash<QString, QJsonObject> acceptedAnswerEchoes;
    QStringList acceptedAnswerOrder;
    QSet<QString> reservedQuestions;
    QStringList reservedQuestionOrder;
    void reserveQuestion(const QString& key) {
        if (reservedQuestions.contains(key)) return;
        reservedQuestions.insert(key);
        reservedQuestionOrder.append(key);
        while (reservedQuestionOrder.size() > 128) reservedQuestions.remove(reservedQuestionOrder.takeFirst());
    }
    void acceptAnswerEcho(const QString& key, const QJsonObject& entry) {
        acceptedAnswerEchoes.insert(key, entry);
        acceptedAnswerOrder.removeAll(key);
        acceptedAnswerOrder.append(key);
        while (acceptedAnswerOrder.size() > 64) acceptedAnswerEchoes.remove(acceptedAnswerOrder.takeFirst());
    }
    QMap<QString, QString> rendered;
    QSet<QString> optimisticMessages;
    QHash<QString, QString> drafts;
    QHash<QString, QStringList> attachmentDrafts;
    QHash<QString, QString> copiedUnknownRequests;
    QHash<QString, QJsonObject> recoveredQuestions;
    QStringList attachments;
    QString oldest;
    bool pendingCommand = false;
    bool historyInFlight = false;
    quint64 historyGeneration = 0;
    bool awaitingResponse = false;
    bool observedBusy = false;
    qint64 commandSentAt = 0;
    CommandHandler command;
    HistoryHandler history;
    DownloadHandler download;
};

RemoteAgentPanel::RemoteAgentPanel(QWidget* parent, const QString& outboxFile)
    : QWidget(parent), runtime_(std::make_unique<Runtime>()) {
    runtime_->outboxFile = outboxFile.isEmpty()
                               ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                                     .filePath(QStringLiteral("remote-agent-outbox.json"))
                               : outboxFile;
    QFile saved(runtime_->outboxFile);
    if (saved.exists() && (!saved.open(QIODevice::ReadOnly) || saved.size() > 4 * 1024 * 1024))
        runtime_->storageUsable = false;
    if (saved.isOpen() && runtime_->storageUsable) {
        QJsonParseError parse;
        const QJsonDocument document = QJsonDocument::fromJson(saved.readAll(), &parse);
        runtime_->storageUsable = parse.error == QJsonParseError::NoError && document.isObject();
        const QJsonObject state = document.object();
        // A process restart cannot prove delivery. Retain original identities and
        // require explicit draft recovery instead of replaying an old turn.
        for (const QJsonValue& value : state.value(QStringLiteral("outbox")).toArray()) {
            QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("requestId")).toString().isEmpty() ||
                entry.value(QStringLiteral("owner")).toObject().isEmpty())
                continue;
            const QString state = entry.value(QStringLiteral("state")).toString();
            entry.insert(QStringLiteral("state"), state == QStringLiteral("sending") ||
                                                          state == QStringLiteral("forwarded") ||
                                                          state == QStringLiteral("unknown")
                                                      ? QStringLiteral("unknown")
                                                      : QStringLiteral("held"));
            runtime_->outbox.append(entry);
        }
        const QJsonObject drafts = state.value(QStringLiteral("drafts")).toObject();
        for (auto it = drafts.begin(); it != drafts.end(); ++it) {
            runtime_->drafts.insert(it.key(), it.value().toObject().value(QStringLiteral("text")).toString());
            const QString copied = it.value().toObject().value(QStringLiteral("copied_unknown_request")).toString();
            if (!copied.isEmpty()) runtime_->copiedUnknownRequests.insert(it.key(), copied);
            const QJsonObject question = it.value().toObject().value(QStringLiteral("answer_question")).toObject();
            if (!question.isEmpty()) runtime_->recoveredQuestions.insert(it.key(), question);
            QStringList paths;
            for (const QJsonValue& path : it.value().toObject().value(QStringLiteral("files")).toArray())
                paths.append(path.toString());
            runtime_->attachmentDrafts.insert(it.key(), paths);
        }
    }
    setObjectName(QStringLiteral("remoteAgentPanel"));
    setStyleSheet(QStringLiteral("background:#ffffff;"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto* header = new QWidget(this);
    header->setObjectName(QStringLiteral("remoteAgentHeader"));
    header->setFixedHeight(UiZoom::s(53));
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(UiZoom::s(16), 0, UiZoom::s(16), 0);
    headerLayout->addSpacing(UiZoom::s(34));
    auto* title = new QLabel(QStringLiteral("创作中心"), header);
    title->setObjectName(QStringLiteral("remoteAgentHeaderTitle"));
    QFont titleFont = title->font();
    titleFont.setPixelSize(UiZoom::s(18));
    titleFont.setBold(true);
    UiZoom::setFont(title, UiZoom::TextRole::Heading3, titleFont);
    title->setAlignment(Qt::AlignCenter);
    headerLayout->addWidget(title, 1);
    headerLayout->addSpacing(UiZoom::s(34));
    layout->addWidget(header);
    auto* divider = new QWidget(this);
    divider->setObjectName(QStringLiteral("remoteAgentHeaderDivider"));
    divider->setFixedHeight(qMax(1, UiZoom::s(1)));
    divider->setStyleSheet(QStringLiteral("QWidget#remoteAgentHeaderDivider{background:#e2e8f0;}"));
    layout->addWidget(divider);

    runtime_->older = new QPushButton(QStringLiteral("查看更早消息"), this);
    runtime_->older->setFlat(true);
    runtime_->older->hide();
    layout->addWidget(runtime_->older);
    runtime_->view = new MarkdownView(this);
    MarkdownTheme conversationTheme = MarkdownTheme::standard(UiZoom::markdownScale(), UiZoom::fontFamilies().first());
    conversationTheme.viewMargin = UiZoom::s(48);
    runtime_->view->setTheme(conversationTheme);
    layout->addWidget(runtime_->view, 1);
    runtime_->requests = new QWidget(this);
    runtime_->requestsLayout = new QVBoxLayout(runtime_->requests);
    runtime_->requestsLayout->setContentsMargins(UiZoom::s(16), 0, UiZoom::s(16), 0);
    runtime_->requests->hide();
    layout->addWidget(runtime_->requests);
    runtime_->status = new QLabel(this);
    runtime_->status->setObjectName(QStringLiteral("remoteAgentStatus"));
    runtime_->status->setWordWrap(true);
    runtime_->status->setContentsMargins(UiZoom::s(16), UiZoom::s(6), UiZoom::s(16), UiZoom::s(6));
    runtime_->status->hide();
    layout->addWidget(runtime_->status);
    auto* composer = new QWidget(this);
    composer->setObjectName(QStringLiteral("remoteAgentComposerPanel"));
    auto* composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(UiZoom::s(12), UiZoom::s(2), UiZoom::s(12), UiZoom::s(8));
    runtime_->editor = new RemotePromptEdit(composer);
    runtime_->editor->setObjectName(QStringLiteral("remoteAgentComposer"));
    runtime_->editor->setAcceptRichText(false);
    runtime_->editor->setPlaceholderText(QStringLiteral("向所属设备发送消息…"));
    runtime_->editor->setFixedHeight(UiZoom::s(136));
    runtime_->editor->setStyleSheet(
        UiZoom::scaleQss(QStringLiteral("QTextEdit#remoteAgentComposer{border:1px solid #d1d5db;border-radius:14px;"
                                        "background:#ffffff;color:#172033;padding:10px 52px 10px 13px;}"
                                        "QTextEdit#remoteAgentComposer:focus{border-color:#b5bbc4;}")));
    QFont editorFont = runtime_->editor->font();
    editorFont.setPixelSize(UiZoom::textPixels(UiZoom::TextRole::Body));
    UiZoom::setFont(runtime_->editor, UiZoom::TextRole::Body, editorFont);
    runtime_->editor->setMimeHandler(
        [](const QMimeData* mime) { return mime && (mime->hasImage() || mime->hasUrls()); });
    runtime_->editor->submit = [this] { sendOrStop(); };
    auto* attachment = new QPushButton(QStringLiteral("+"), runtime_->editor);
    attachment->setObjectName(QStringLiteral("remoteAgentAttachButton"));
    attachment->setAccessibleName(QStringLiteral("添加会话附件"));
    attachment->setToolTip(attachment->accessibleName());
    attachment->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    attachment->setStyleSheet(
        QStringLiteral("QPushButton{border:0;border-radius:18px;background:#f1f5f9;"
                       "color:#0b67b7;font-size:24px;}"));
    connect(attachment, &QPushButton::clicked, this, [this] {
        const QStringList picked =
            QFileDialog::getOpenFileNames(this, QStringLiteral("选择会话附件"), QDir::homePath());
        for (const QString& path : picked) {
            const QFileInfo file(path);
            const QByteArray name = file.fileName().toUtf8();
            MaiCloudDriveMediaInfo media{};
            if (!file.isFile() || !mai_session_attachment_media_info(name.constData(), &media) || file.size() < 1 ||
                quint64(file.size()) > media.max_size_bytes) {
                showStatus(QStringLiteral("附件格式或大小无效：%1").arg(file.fileName()), true);
                continue;
            }
            if (runtime_->attachments.size() >= 10) {
                showStatus(QStringLiteral("每条消息最多添加 10 项附件"), true);
                break;
            }
            if (!runtime_->attachments.contains(file.absoluteFilePath()))
                runtime_->attachments.append(file.absoluteFilePath());
        }
        runtime_->attachmentsLabel->setText(attachmentSummary(runtime_->attachments));
        runtime_->attachmentsLabel->setToolTip(runtime_->attachments.join(QLatin1Char('\n')));
        runtime_->attachmentsLabel->setVisible(!runtime_->attachments.isEmpty());
        runtime_->clearAttachments->setVisible(!runtime_->attachments.isEmpty());
        updateControls();
    });
    runtime_->editor->setLeadingAction(attachment);
    runtime_->stop = new QPushButton(runtime_->editor);
    runtime_->stop->setObjectName(QStringLiteral("remoteAgentStopButton"));
    runtime_->stop->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    runtime_->stop->setIconSize(QSize(UiZoom::s(18), UiZoom::s(18)));
    runtime_->stop->setIcon(actionIcon(true, QColor(QStringLiteral("#c24131"))));
    runtime_->stop->setToolTip(QStringLiteral("\u505c\u6b62\u5f53\u524d\u4efb\u52a1"));
    runtime_->stop->setAccessibleName(runtime_->stop->toolTip());
    runtime_->stop->setStyleSheet(
        QStringLiteral("QPushButton{background:#fff5f3;border:1px solid #f0c3bd;border-radius:18px;}"));
    runtime_->editor->setSecondaryLeadingAction(runtime_->stop);
    connect(runtime_->stop, &QPushButton::clicked, this, [this] {
        const QString turn = runtime_->session.value(QStringLiteral("turnId")).toString();
        if (turn.isEmpty()) {
            showStatus(
                QStringLiteral("\u5f53\u524d\u4efb\u52a1\u5df2\u53d8\u5316\uff0c\u8bf7\u5237\u65b0\u540e\u91cd\u8bd5"),
                true);
            return;
        }
        // Stop has its own receipt and bypasses attachment/message FIFO. Never
        // consume or clear the editor, queued supplement or its pending receipt.
        sendAction({{QStringLiteral("action"), QStringLiteral("stop")}, {QStringLiteral("turnId"), turn}}, false);
        for (int i = 0; i < runtime_->outbox.size(); ++i) {
            QJsonObject entry = runtime_->outbox[i].toObject();
            if (sessionKey(entry.value(QStringLiteral("owner")).toObject()) == sessionKey(runtime_->session) &&
                entry.value(QStringLiteral("state")) == QStringLiteral("queued")) {
                entry.insert(QStringLiteral("state"), QStringLiteral("held"));
                runtime_->outbox[i] = entry;
            }
        }
        persistOutbox();
        updateControls();
    });
    composerLayout->addWidget(runtime_->editor, 1);
    auto* attachmentsRow = new QWidget(composer);
    auto* attachmentsLayout = new QHBoxLayout(attachmentsRow);
    attachmentsLayout->setContentsMargins(0, 0, 0, 0);
    runtime_->attachmentsLabel = new QLabel(attachmentsRow);
    runtime_->attachmentsLabel->hide();
    attachmentsLayout->addWidget(runtime_->attachmentsLabel, 1);
    runtime_->clearAttachments = new QPushButton(QStringLiteral("清除附件"), attachmentsRow);
    runtime_->clearAttachments->hide();
    attachmentsLayout->addWidget(runtime_->clearAttachments);
    connect(runtime_->clearAttachments, &QPushButton::clicked, this, [this] {
        runtime_->attachments.clear();
        runtime_->attachmentsLabel->hide();
        runtime_->clearAttachments->hide();
        updateControls();
    });
    composerLayout->addWidget(attachmentsRow);
    runtime_->recover = new QPushButton(QStringLiteral("\u53d6\u56de\u5f85\u53d1\u8349\u7a3f"), composer);
    runtime_->recover->setObjectName(QStringLiteral("remoteAgentRecoverDraftButton"));
    runtime_->recover->hide();
    composerLayout->addWidget(runtime_->recover);
    connect(runtime_->recover, &QPushButton::clicked, this, &RemoteAgentPanel::recoverQueuedDraft);
    runtime_->send = new QPushButton(runtime_->editor);
    runtime_->send->setObjectName(QStringLiteral("remoteAgentSendButton"));
    runtime_->send->setCursor(Qt::PointingHandCursor);
    runtime_->send->setFixedSize(UiZoom::s(36), UiZoom::s(36));
    runtime_->send->setIconSize(QSize(UiZoom::s(18), UiZoom::s(18)));
    runtime_->editor->setCornerAction(runtime_->send);
    layout->addWidget(composer);
    connect(runtime_->send, &QPushButton::clicked, this, [this] { sendOrStop(); });
    connect(runtime_->editor, &QTextEdit::textChanged, this, [this] {
        if (!runtime_->restoringDraft && !runtime_->session.isEmpty())
            runtime_->drafts.insert(sessionKey(runtime_->session), runtime_->editor->toPlainText());
        if (!runtime_->restoringDraft && runtime_->editor->toPlainText().isEmpty()) {
            runtime_->copiedUnknownRequests.remove(sessionKey(runtime_->session));
            runtime_->recoveredQuestions.remove(sessionKey(runtime_->session));
        }
        updateControls();
    });
    connect(runtime_->older, &QPushButton::clicked, this, [this] {
        if (!runtime_->oldest.isEmpty()) requestHistory(runtime_->oldest, 20);
    });
    connect(runtime_->view, &MarkdownView::linkActivated, this, [this](const QString& href) {
        const QString retryPrefix = QStringLiteral("maichat-input-retry:");
        if (href.startsWith(retryPrefix)) {
            sendPrompt({}, href.mid(retryPrefix.size()));
            return;
        }
        const QString prefix = QStringLiteral("maichat-attachment:");
        if (!href.startsWith(prefix) || !runtime_->download) return;
        const QString id = href.mid(prefix.size());
        static const QRegularExpression valid(QStringLiteral("^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$"));
        if (!valid.match(id).hasMatch()) return;
        const QJsonObject session = runtime_->session;
        showStatus(QStringLiteral("正在下载会话附件…"));
        QPointer<RemoteAgentPanel> guard(this);
        runtime_->download(session, id, [guard, session](const QString& path, const QString& error) {
            if (!guard || sessionKey(session) != sessionKey(guard->runtime_->session)) return;
            if (!error.isEmpty()) {
                guard->showStatus(error, true);
                return;
            }
            if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
                guard->showStatus(QStringLiteral("已下载到 %1，无法自动打开").arg(path));
            else
                guard->showStatus({});
        });
    });
    runtime_->historyTimeout.setSingleShot(true);
    runtime_->historyTimeout.setInterval(30'000);
    connect(&runtime_->historyTimeout, &QTimer::timeout, this, [this] {
        if (!runtime_->historyInFlight) return;
        runtime_->historyInFlight = false;
        runtime_->older->setEnabled(true);
        if (runtime_->messages.isEmpty()) {
            showStatus(QStringLiteral("会话读取超时，可重新进入会话重试"), true);
        }
    });
    updateControls();
}

RemoteAgentPanel::~RemoteAgentPanel() {
    if (!runtime_->session.isEmpty()) {
        runtime_->drafts.insert(sessionKey(runtime_->session), runtime_->editor->toPlainText());
        runtime_->attachmentDrafts.insert(sessionKey(runtime_->session), runtime_->attachments);
    }
    persistOutbox();
}

void RemoteAgentPanel::setCommandHandler(CommandHandler handler) {
    runtime_->command = std::move(handler);
}
void RemoteAgentPanel::setHistoryHandler(HistoryHandler handler) {
    runtime_->history = std::move(handler);
}
void RemoteAgentPanel::setDownloadHandler(DownloadHandler handler) {
    runtime_->download = std::move(handler);
}

QJsonObject RemoteAgentPanel::session() const {
    return runtime_->session;
}

void RemoteAgentPanel::openSession(const QJsonObject& remote) {
    const QString previous = sessionKey(runtime_->session);
    const QString next = sessionKey(remote);
    if (previous != next) {
        ++runtime_->historyGeneration;
        runtime_->historyInFlight = false;
        runtime_->historyTimeout.stop();
        if (!previous.isEmpty()) {
            runtime_->drafts.insert(previous, runtime_->editor->toPlainText());
            runtime_->attachmentDrafts.insert(previous, runtime_->attachments);
        }
        runtime_->restoringDraft = true;
        runtime_->editor->setPlainText(runtime_->drafts.value(next));
        runtime_->restoringDraft = false;
        runtime_->attachments = runtime_->attachmentDrafts.value(next);
        runtime_->attachmentsLabel->setText(attachmentSummary(runtime_->attachments));
        runtime_->attachmentsLabel->setToolTip(runtime_->attachments.join(QLatin1Char('\n')));
        runtime_->attachmentsLabel->setVisible(!runtime_->attachments.isEmpty());
        runtime_->clearAttachments->setVisible(!runtime_->attachments.isEmpty());
        runtime_->messages.clear();
        runtime_->windowUpdates.clear();
        runtime_->windowUpdateOrder.clear();
        runtime_->historyMessageIds.clear();
        runtime_->questionMessages.clear();
        runtime_->questions = {};
        runtime_->rendered.clear();
        runtime_->optimisticMessages.clear();
        runtime_->view->clear();
        runtime_->thinking = nullptr;
        runtime_->oldest.clear();
        runtime_->older->hide();
        runtime_->pendingCommand = false;
        runtime_->awaitingResponse = false;
        runtime_->observedBusy = false;
    }
    runtime_->session = remote;
    runtime_->owners.insert(next, remote);
    dispatchQueued();
    updateControls();
    if (previous == next && !runtime_->messages.isEmpty()) return;
    showStatus(remote.value(QStringLiteral("online")).toBool() ? QStringLiteral("正在读取所属设备的会话…")
                                                               : QStringLiteral("所属设备离线；输入保留为草稿"));
    requestHistory({}, 10);
}

void RemoteAgentPanel::updateSession(const QJsonObject& remote) {
    runtime_->owners.insert(sessionKey(remote), remote);
    if (sessionKey(remote) != sessionKey(runtime_->session)) return;
    const bool wasOnline = runtime_->session.value(QStringLiteral("online")).toBool();
    runtime_->session = remote;
    runtime_->owners.insert(sessionKey(remote), remote);
    updateControls();
    if (remote.value(QStringLiteral("busy")).toBool() && runtime_->awaitingResponse) runtime_->observedBusy = true;
    if (!remote.value(QStringLiteral("busy")).toBool() && runtime_->observedBusy) {
        runtime_->awaitingResponse = false;
        runtime_->observedBusy = false;
    }
    updateThinking();
    if (!remote.value(QStringLiteral("online")).toBool()) {
        runtime_->historyInFlight = false;
        runtime_->historyTimeout.stop();
        runtime_->older->setEnabled(true);
        showStatus(QStringLiteral("所属设备离线；输入保留为草稿"));
    } else if (!wasOnline) {
        // WebSocket 断线期间的消息可能没收到；重新上线后补最近一页。
        refreshHistory();
    }
}

void RemoteAgentPanel::refreshHistory() {
    requestHistory({}, 10);
}

void RemoteAgentPanel::requestHistory(const QString& before, int limit) {
    if (!runtime_->history || runtime_->historyInFlight || !runtime_->session.value(QStringLiteral("online")).toBool())
        return;
    runtime_->historyInFlight = true;
    runtime_->older->setEnabled(false);
    runtime_->historyTimeout.start();
    const QJsonObject current = runtime_->session;
    const quint64 generation = ++runtime_->historyGeneration;
    QPointer<RemoteAgentPanel> guard(this);
    runtime_->history(current, before, limit, [guard, current, generation](const QString& error) {
        if (!guard || generation != guard->runtime_->historyGeneration ||
            sessionKey(current) != sessionKey(guard->runtime_->session) || error.isEmpty() ||
            !guard->runtime_->historyInFlight)
            return;
        guard->runtime_->historyInFlight = false;
        guard->runtime_->historyTimeout.stop();
        guard->runtime_->older->setEnabled(true);
        if (guard->runtime_->messages.isEmpty()) guard->showStatus(error, true);
    });
}

bool RemoteAgentPanel::persistOutbox() {
    if (!runtime_->storageUsable) return false;
    QJsonObject drafts;
    for (auto it = runtime_->drafts.begin(); it != runtime_->drafts.end(); ++it) {
        QJsonArray files;
        for (const QString& path : runtime_->attachmentDrafts.value(it.key())) files.append(path);
        drafts.insert(
            it.key(),
            QJsonObject{{QStringLiteral("text"), it.value()},
                        {QStringLiteral("files"), files},
                        {QStringLiteral("copied_unknown_request"), runtime_->copiedUnknownRequests.value(it.key())},
                        {QStringLiteral("answer_question"), runtime_->recoveredQuestions.value(it.key())}});
    }
    const QByteArray data =
        QJsonDocument(QJsonObject{{QStringLiteral("outbox"), runtime_->outbox}, {QStringLiteral("drafts"), drafts}})
            .toJson(QJsonDocument::Compact);
    if (data.size() > 4 * 1024 * 1024 || !QDir().mkpath(QFileInfo(runtime_->outboxFile).absolutePath())) return false;
    QSaveFile file(runtime_->outboxFile);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return file.write(data) == data.size() && file.commit();
}

void RemoteAgentPanel::refreshAttachments() {
    runtime_->attachmentsLabel->setText(attachmentSummary(runtime_->attachments));
    runtime_->attachmentsLabel->setToolTip(runtime_->attachments.join(QLatin1Char('\n')));
    runtime_->attachmentsLabel->setVisible(!runtime_->attachments.isEmpty());
    runtime_->clearAttachments->setVisible(!runtime_->attachments.isEmpty());
}

void RemoteAgentPanel::sendAction(const QJsonObject& requested, bool clearDraft, const QJsonObject& question) {
    if (!runtime_->command) return;
    const QJsonObject owner = runtime_->session;
    if (!owner.value(QStringLiteral("online")).toBool()) return;
    QJsonObject action = requested;
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    action.insert(QStringLiteral("requestId"), requestId);
    if (!clearDraft) {
        runtime_->controlRequests.insert(requestId, sessionKey(owner));
        QPointer<RemoteAgentPanel> guard(this);
        runtime_->command(owner, action, [guard, owner, requestId](const QString& error) {
            if (!guard || error.isEmpty()) return;
            guard->runtime_->controlRequests.remove(requestId);
            if (sessionKey(owner) == sessionKey(guard->runtime_->session)) guard->showStatus(error, true);
        });
        return;
    }
    if (runtime_->outbox.size() >= 128) {
        showStatus(QStringLiteral("\u5f85\u53d1\u6d88\u606f\u8fc7\u591a\uff0c\u8bf7\u5148\u53d6\u56de\u672a\u786e\u8ba4"
                                  "\u7684\u8349\u7a3f"),
                   true);
        return;
    }
    const QString key = sessionKey(owner);
    const bool isAnswer = action.value(QStringLiteral("action")) == QStringLiteral("answer");
    const QJsonArray old = runtime_->outbox;
    const QString oldDraft = runtime_->drafts.value(key);
    const QStringList oldFiles = runtime_->attachmentDrafts.value(key);
    const QJsonObject recoveredQuestion = runtime_->recoveredQuestions.value(key);
    QJsonObject entry{{QStringLiteral("owner"), owner},
                      {QStringLiteral("action"), action},
                      {QStringLiteral("requestId"), requestId},
                      {QStringLiteral("draft"), runtime_->editor->toPlainText()},
                      {QStringLiteral("created"), double(QDateTime::currentMSecsSinceEpoch())},
                      {QStringLiteral("state"), QStringLiteral("queued")}};
    if (isAnswer) entry.insert(QStringLiteral("question"), question);
    runtime_->outbox.append(entry);
    runtime_->drafts.insert(key, {});
    if (isAnswer) runtime_->recoveredQuestions.remove(key);
    runtime_->attachmentDrafts.insert(key, isAnswer ? runtime_->attachments : QStringList{});
    // Durable capture precedes clearing any live draft. A storage error leaves
    // the editor and attachments intact and sends nothing.
    if (!persistOutbox()) {
        runtime_->outbox = old;
        runtime_->drafts.insert(key, oldDraft);
        runtime_->attachmentDrafts.insert(key, oldFiles);
        if (!recoveredQuestion.isEmpty()) runtime_->recoveredQuestions.insert(key, recoveredQuestion);
        showStatus(
            QStringLiteral("\u65e0\u6cd5\u4fdd\u5b58\u5f85\u53d1\u6d88\u606f\uff0c\u8349\u7a3f\u5df2\u4fdd\u7559"),
            true);
        return;
    }
    if (isAnswer) runtime_->reserveQuestion(key + QLatin1Char('|') + action.value(QStringLiteral("id")).toString());
    const QString optimisticId = QStringLiteral("msg_z_pending_%1").arg(requestId);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString text = action.value(QStringLiteral("text")).toString();
    if (text.isEmpty()) text = QStringLiteral("\u5df2\u53d1\u9001\u9644\u4ef6");
    if (!isAnswer) {
        runtime_->optimisticMessages.insert(optimisticId);
        runtime_->messages.insert(
            optimisticId,
            {{QStringLiteral("id"), optimisticId},
             {QStringLiteral("role"), QStringLiteral("user")},
             {QStringLiteral("created"), double(now)},
             {QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                                              {QStringLiteral("text"), text}}}}});
    }
    runtime_->editor->clear();
    if (!isAnswer) runtime_->attachments.clear();
    refreshAttachments();
    renderMessages();
    dispatchQueued();
    updateControls();
}

void RemoteAgentPanel::dispatchQueued(const QString& requestedOwner) {
    if (!runtime_->command) return;
    const QString ownerKey = requestedOwner.isEmpty() ? sessionKey(runtime_->session) : requestedOwner;
    const bool visible = ownerKey == sessionKey(runtime_->session);
    for (int i = 0; i < runtime_->outbox.size(); ++i) {
        QJsonObject entry = runtime_->outbox[i].toObject();
        if (sessionKey(entry.value(QStringLiteral("owner")).toObject()) != ownerKey) continue;
        const QString state = entry.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("sending")) return;  // Finish this file upload/relay call, not its model task.
        if (state != QStringLiteral("queued")) continue;
        const QJsonObject current = runtime_->owners.value(ownerKey);
        if (!current.value(QStringLiteral("online")).toBool()) return;
        const QJsonObject action = entry.value(QStringLiteral("action")).toObject();
        const bool isAnswer = action.value(QStringLiteral("action")) == QStringLiteral("answer");
        const QString turn = action.value(QStringLiteral("turnId")).toString();
        if (action.value(QStringLiteral("action")) == QStringLiteral("steer") && turn.isEmpty()) return;
        if (action.value(QStringLiteral("action")) == QStringLiteral("steer") &&
            (turn.isEmpty() || turn != current.value(QStringLiteral("turnId")).toString())) {
            entry.insert(QStringLiteral("state"), QStringLiteral("held"));
            runtime_->outbox[i] = entry;
            persistOutbox();
            if (visible) {
                showStatus(QStringLiteral("原任务已变化，待发草稿已保留"), true);
                updateControls();
            }
            return;
        }
        const int attempt = entry.value(QStringLiteral("attempt")).toInt() + 1;
        entry.insert(QStringLiteral("attempt"), attempt);
        entry.insert(QStringLiteral("state"), QStringLiteral("sending"));
        runtime_->outbox[i] = entry;
        if (!persistOutbox()) {
            entry.insert(QStringLiteral("state"), QStringLiteral("queued"));
            runtime_->outbox[i] = entry;
            if (visible) showStatus(QStringLiteral("无法保存发送状态，草稿已保留"), true);
            return;
        }
        const QString requestId = entry.value(QStringLiteral("requestId")).toString();
        if (visible) {
            runtime_->pendingCommand = true;
            runtime_->commandSentAt = QDateTime::currentMSecsSinceEpoch();
        }
        QPointer<RemoteAgentPanel> guard(this);
        runtime_->command(current, action, [guard, requestId, current](const QString& error) {
            if (!guard) return;
            if (!error.isEmpty()) {
                guard->finishQueued(requestId, {{QStringLiteral("error"), error}}, true);
                return;
            }
            // A relay ACK releases this owner's sending lane immediately. The
            // business receipt remains independent and may arrive out of order.
            for (int i = 0; i < guard->runtime_->outbox.size(); ++i) {
                QJsonObject entry = guard->runtime_->outbox[i].toObject();
                if (entry.value(QStringLiteral("requestId")) != requestId ||
                    entry.value(QStringLiteral("state")) != QStringLiteral("sending"))
                    continue;
                entry.insert(QStringLiteral("state"), QStringLiteral("forwarded"));
                guard->runtime_->outbox[i] = entry;
                guard->persistOutbox();
                break;
            }
            if (sessionKey(current) == sessionKey(guard->runtime_->session)) {
                guard->showStatus(QStringLiteral("已发出，等待所属设备接收"));
                guard->updateControls();
            }
            guard->dispatchQueued(sessionKey(current));
        });
        const int timeout =
            action.value(QStringLiteral("local_files")).toArray().isEmpty() ? 30000 : 2 * 60 * 60 * 1000;
        QTimer::singleShot(timeout, this, [guard, requestId, attempt] {
            if (!guard) return;
            bool sameAttempt = false;
            for (const QJsonValue& value : guard->runtime_->outbox) {
                const QJsonObject entry = value.toObject();
                sameAttempt |= entry.value(QStringLiteral("requestId")) == requestId &&
                               entry.value(QStringLiteral("attempt")).toInt() == attempt;
            }
            if (sameAttempt)
                guard->finishQueued(
                    requestId,
                    {{QStringLiteral("error"),
                      QStringLiteral(
                          "\u6267\u884c\u72b6\u6001\u672a\u786e\u8ba4\uff0c\u8bf7\u5148\u67e5\u770b\u6240\u5c5e\u8bbe"
                          "\u5907\u4f1a\u8bdd\uff1b\u5f85\u53d1\u8349\u7a3f\u5df2\u4fdd\u7559")}},
                    true);
        });
        return;
    }
}

void RemoteAgentPanel::finishQueued(const QString& requestId, const QJsonObject& result, bool unknown) {
    for (int i = 0; i < runtime_->outbox.size(); ++i) {
        QJsonObject entry = runtime_->outbox[i].toObject();
        if (entry.value(QStringLiteral("requestId")) != requestId) continue;
        if (unknown && entry.value(QStringLiteral("state")) != QStringLiteral("sending") &&
            entry.value(QStringLiteral("state")) != QStringLiteral("forwarded"))
            return;
        const QString key = sessionKey(entry.value(QStringLiteral("owner")).toObject());
        const bool visible = key == sessionKey(runtime_->session);
        unknown |= result.value(QStringLiteral("unknown")).toBool() ||
                   result.value(QStringLiteral("delivery_unknown")).toBool();
        if (!result.value(QStringLiteral("ok")).isBool()) unknown = true;
        const QJsonObject sent = entry.value(QStringLiteral("action")).toObject();
        const bool isAnswer = sent.value(QStringLiteral("action")) == QStringLiteral("answer");
        if (!unknown && result.value(QStringLiteral("ok")).toBool() &&
            (isAnswer ? result.value(QStringLiteral("question_id")) != sent.value(QStringLiteral("id"))
                      : result.value(QStringLiteral("user_message_id")).toString().isEmpty()))
            unknown = true;
        const bool accepted = !unknown && result.value(QStringLiteral("ok")).toBool();
        const QString optimisticId = QStringLiteral("msg_z_pending_%1").arg(requestId);
        if (accepted) {
            const QString acceptedTurn =
                isAnswer ? entry.value(QStringLiteral("owner")).toObject().value(QStringLiteral("turnId")).toString()
                         : result.value(QStringLiteral("turnId")).toString();
            for (int j = i + 1; j < runtime_->outbox.size(); ++j) {
                QJsonObject tail = runtime_->outbox[j].toObject();
                QJsonObject action = tail.value(QStringLiteral("action")).toObject();
                if (sessionKey(tail.value(QStringLiteral("owner")).toObject()) == key &&
                    action.value(QStringLiteral("afterRequest")) == requestId) {
                    action.remove(QStringLiteral("afterRequest"));
                    action.insert(QStringLiteral("turnId"), acceptedTurn);
                    tail.insert(QStringLiteral("action"), action);
                    if (acceptedTurn.isEmpty()) tail.insert(QStringLiteral("state"), QStringLiteral("held"));
                    runtime_->outbox[j] = tail;
                }
            }
            if (!acceptedTurn.isEmpty() &&
                entry.value(QStringLiteral("action")).toObject().value(QStringLiteral("action")) ==
                    QStringLiteral("prompt") &&
                runtime_->owners.value(key).value(QStringLiteral("turnId")).toString().isEmpty()) {
                QJsonObject owner = runtime_->owners.value(key);
                owner.insert(QStringLiteral("turnId"), acceptedTurn);
                owner.insert(QStringLiteral("busy"), true);
                runtime_->owners.insert(key, owner);
                if (visible) runtime_->session = owner;
            }
            if (isAnswer) {
                const QString answerKey = key + QLatin1Char('|') + sent.value(QStringLiteral("id")).toString();
                runtime_->acceptAnswerEcho(answerKey, entry);
                runtime_->reserveQuestion(answerKey);
                if (visible) {
                    QJsonArray pending;
                    for (const QJsonValue& question : runtime_->questions)
                        if (question.toObject().value(QStringLiteral("id")) != sent.value(QStringLiteral("id")))
                            pending.append(question);
                    runtime_->questions = pending;
                }
            }
            runtime_->outbox.removeAt(i);
            if (visible) {
                const QString canonical = result.value(QStringLiteral("user_message_id")).toString();
                QJsonObject message = runtime_->messages.value(optimisticId);
                if (!isAnswer && !canonical.isEmpty()) {
                    removeOptimisticMessage(optimisticId);
                    if (!runtime_->messages.contains(canonical)) {
                        message.insert(QStringLiteral("id"), canonical);
                        runtime_->messages.insert(canonical, message);
                    }
                }
                runtime_->awaitingResponse = true;
                runtime_->observedBusy = false;
                renderMessages();
            }
        } else {
            entry.insert(QStringLiteral("state"), unknown ? QStringLiteral("unknown") : QStringLiteral("rejected"));
            runtime_->outbox[i] = entry;
            // A failed head freezes its owner's tail. Stop/finishing or uncertain
            // delivery must never turn a supplement into a later new task.
            for (int j = i + 1; j < runtime_->outbox.size(); ++j) {
                QJsonObject tail = runtime_->outbox[j].toObject();
                if (sessionKey(tail.value(QStringLiteral("owner")).toObject()) == key &&
                    tail.value(QStringLiteral("action")).toObject().value(QStringLiteral("action")) ==
                        QStringLiteral("steer") &&
                    tail.value(QStringLiteral("state")) == QStringLiteral("queued")) {
                    tail.insert(QStringLiteral("state"), QStringLiteral("held"));
                    runtime_->outbox[j] = tail;
                }
            }
            if (visible) {
                removeOptimisticMessage(optimisticId);
                showStatus(result.value(QStringLiteral("error"))
                               .toString(unknown ? QStringLiteral("接收结果未确认，原请求和草稿已保留")
                                                 : QStringLiteral("所属设备未受理，草稿已保留")),
                           true);
            }
        }
        persistOutbox();
        if (visible) {
            runtime_->pendingCommand = false;
            if (!isAnswer && !accepted && !unknown && runtime_->editor->toPlainText().isEmpty() &&
                runtime_->attachments.isEmpty())
                recoverQueuedDraft();
            updateThinking();
            updateControls();
            if (accepted) showStatus({});
        }
        dispatchQueued(key);
        return;
    }
}

void RemoteAgentPanel::recoverQueuedDraft() {
    if (!runtime_->editor->toPlainText().isEmpty() || !runtime_->attachments.isEmpty()) {
        showStatus(QStringLiteral("\u8bf7\u5148\u4fdd\u5b58\u6216\u53d1\u9001\u5f53\u524d\u8349\u7a3f\uff0c\u518d\u53d6"
                                  "\u56de\u5f85\u53d1\u7a3f"),
                   true);
        return;
    }
    const QString key = sessionKey(runtime_->session);
    for (int i = 0; i < runtime_->outbox.size(); ++i) {
        const QJsonObject entry = runtime_->outbox[i].toObject();
        if (sessionKey(entry.value(QStringLiteral("owner")).toObject()) != key) continue;
        if (entry.value(QStringLiteral("state")) == QStringLiteral("sending") ||
            entry.value(QStringLiteral("state")) == QStringLiteral("forwarded") ||
            entry.value(QStringLiteral("state")) == QStringLiteral("queued"))
            continue;
        if (entry.value(QStringLiteral("state")) == QStringLiteral("unknown") &&
            entry.value(QStringLiteral("action")).toObject().value(QStringLiteral("action")) ==
                QStringLiteral("prompt")) {
            sendPrompt({}, entry.value(QStringLiteral("requestId")).toString());
            return;
        }
        const QJsonArray before = runtime_->outbox;
        const QJsonObject previousQuestion = runtime_->recoveredQuestions.value(key);
        const QJsonObject question = entry.value(QStringLiteral("question")).toObject();
        if (!question.isEmpty()) runtime_->recoveredQuestions.insert(key, question);
        runtime_->drafts.insert(key, entry.value(QStringLiteral("draft")).toString());
        QStringList paths;
        for (const QJsonValue& path :
             entry.value(QStringLiteral("action")).toObject().value(QStringLiteral("local_files")).toArray())
            paths.append(path.toString());
        runtime_->attachmentDrafts.insert(key, paths);
        // Unknown delivery retains its original request and blocks this owner's
        // FIFO even after copying text. A copied draft is not proof of rejection
        // and must not permit a new-ID retry of a possibly accepted operation.
        const bool unknown = entry.value(QStringLiteral("state")) == QStringLiteral("unknown");
        if (unknown) runtime_->copiedUnknownRequests.insert(key, entry.value(QStringLiteral("requestId")).toString());
        if (!unknown) runtime_->outbox.removeAt(i);
        if (!persistOutbox()) {
            runtime_->outbox = before;
            if (previousQuestion.isEmpty())
                runtime_->recoveredQuestions.remove(key);
            else
                runtime_->recoveredQuestions.insert(key, previousQuestion);
            return;
        }
        runtime_->editor->setPlainText(runtime_->drafts.value(key));
        runtime_->attachments = paths;
        refreshAttachments();
        if (unknown)
            showStatus(QStringLiteral("发送结果未确认，请先核对所属设备；原请求仍已保留。"), true);
        else if (!question.isEmpty())
            showStatus(QStringLiteral("已取回回答；重发仍回答原问题。若问题已结束，请先清空文字再写新消息。"));
        removeOptimisticMessage(
            QStringLiteral("msg_z_pending_%1").arg(entry.value(QStringLiteral("requestId")).toString()));
        updateControls();
        return;
    }
}

void RemoteAgentPanel::removeOptimisticMessage(const QString& id) {
    if (id.isEmpty()) return;
    runtime_->optimisticMessages.remove(id);
    runtime_->messages.remove(id);
    runtime_->rendered.remove(id);
    runtime_->view->removeItem(id);
}

void RemoteAgentPanel::sendOrStop() {
    const QString text = runtime_->editor->toPlainText().trimmed();
    if ((text.isEmpty() && runtime_->attachments.isEmpty()) || text.toUtf8().size() > 16 * 1024) return;
    const QJsonObject recovered = runtime_->recoveredQuestions.value(sessionKey(runtime_->session));
    if (!recovered.isEmpty()) {
        if (text.isEmpty()) {
            showStatus(QStringLiteral("原回答仅发送文字，附件已保留"));
            return;
        }
        sendAction({{QStringLiteral("action"), QStringLiteral("answer")},
                    {QStringLiteral("id"), recovered.value(QStringLiteral("id"))},
                    {QStringLiteral("text"), text}},
                   true, recovered);
        return;
    }
    QVector<QJsonObject> questions;
    const QString ownerKey = sessionKey(runtime_->session);
    for (const QJsonValue& value : runtime_->questions) {
        const QJsonObject question = value.toObject();
        const QString id = question.value(QStringLiteral("id")).toString();
        bool reserved = id.isEmpty() || runtime_->reservedQuestions.contains(ownerKey + QLatin1Char('|') + id);
        for (const QJsonValue& queued : runtime_->outbox) {
            const QJsonObject entry = queued.toObject();
            const QJsonObject action = entry.value(QStringLiteral("action")).toObject();
            reserved |= sessionKey(entry.value(QStringLiteral("owner")).toObject()) == ownerKey &&
                        action.value(QStringLiteral("action")) == QStringLiteral("answer") &&
                        action.value(QStringLiteral("id")) == id;
        }
        if (!reserved) questions.append(question);
    }
    std::sort(questions.begin(), questions.end(), [](const QJsonObject& left, const QJsonObject& right) {
        const double a = left.value(QStringLiteral("asked")).toDouble();
        const double b = right.value(QStringLiteral("asked")).toDouble();
        return a == b ? left.value(QStringLiteral("id")).toString() < right.value(QStringLiteral("id")).toString()
                      : a < b;
    });
    if (!questions.isEmpty()) {
        if (text.isEmpty()) {
            showStatus(QStringLiteral("请输入回答文字，附件仍保留在输入栏"));
            return;
        }
        const QJsonObject question = questions.first();
        // Only the captured question is answered. Media stays in the ordinary
        // composer, and cancellation cannot turn this answer into a new prompt.
        sendAction({{QStringLiteral("action"), QStringLiteral("answer")},
                    {QStringLiteral("id"), question.value(QStringLiteral("id"))},
                    {QStringLiteral("text"), text}},
                   true, question);
        return;
    }
    QJsonArray files;
    for (const QString& path : runtime_->attachments) files.append(path);
    sendAction({{QStringLiteral("action"), QStringLiteral("prompt")},
                {QStringLiteral("text"), text},
                {QStringLiteral("local_files"), files}},
               true);
}

void RemoteAgentPanel::rememberPreparedPrompt(const QString& requestId, const QJsonObject& payload) {
    if (payload.value(QStringLiteral("requestId")).toString() != requestId) return;
    for (int i = 0; i < runtime_->outbox.size(); ++i) {
        QJsonObject entry = runtime_->outbox[i].toObject();
        if (entry.value(QStringLiteral("requestId")) != requestId) continue;
        entry.insert(QStringLiteral("action"), payload);
        runtime_->outbox[i] = entry;
        persistOutbox();  // Retain uploaded references for a same-identity retry.
        return;
    }
}

void RemoteAgentPanel::sendPrompt(const QJsonObject&, const QString& retryId) {
    for (int i = 0; i < runtime_->outbox.size(); ++i) {
        QJsonObject entry = runtime_->outbox[i].toObject();
        const QString state = entry.value(QStringLiteral("state")).toString();
        if (entry.value(QStringLiteral("requestId")) != retryId ||
            sessionKey(entry.value(QStringLiteral("owner")).toObject()) != sessionKey(runtime_->session) ||
            entry.value(QStringLiteral("action")).toObject().value(QStringLiteral("action")) !=
                QStringLiteral("prompt") ||
            (state != QStringLiteral("unknown") && state != QStringLiteral("rejected") &&
             state != QStringLiteral("held")))
            continue;
        entry.insert(QStringLiteral("state"), QStringLiteral("queued"));
        runtime_->outbox[i] = entry;
        if (!persistOutbox()) return;
        dispatchQueued();
        renderMessages();
        updateControls();
        return;
    }
}

void RemoteAgentPanel::receivePayload(const QJsonObject& remote, const QString& kind, const QJsonObject& payload) {
    if (kind == QStringLiteral("event") &&
        payload.value(QStringLiteral("type")).toString() == QStringLiteral("command_result")) {
        QString id = payload.value(QStringLiteral("request_id")).toString();
        if (id.isEmpty()) id = payload.value(QStringLiteral("command_id")).toString();
        if (runtime_->controlRequests.value(id) == sessionKey(remote)) {
            runtime_->controlRequests.remove(id);
            return;
        }
        if (id.isEmpty()) return;
        for (const QJsonValue& value : runtime_->outbox) {
            const QJsonObject entry = value.toObject();
            if (entry.value(QStringLiteral("requestId")) == id &&
                sessionKey(entry.value(QStringLiteral("owner")).toObject()) == sessionKey(remote)) {
                finishQueued(id, payload.value(QStringLiteral("result")).toObject());
                return;
            }
        }
        return;
    }
    if (sessionKey(remote) != sessionKey(runtime_->session)) return;
    if (kind == QStringLiteral("history_page")) {
        runtime_->historyInFlight = false;
        runtime_->historyTimeout.stop();
        runtime_->older->setEnabled(true);
    }
    if (!payload.value(QStringLiteral("ok")).toBool()) {
        if (kind == QStringLiteral("history_page"))
            showStatus(payload.value(QStringLiteral("error")).toString(QStringLiteral("所属设备无法读取这段会话")),
                       true);
        return;
    }
    const QJsonArray received = payload.value(QStringLiteral("messages")).toArray();
    if (payload.contains(QStringLiteral("questions")))
        runtime_->questions = payload.value(QStringLiteral("questions")).toArray();
    if (payload.contains(QStringLiteral("questionMessages"))) {
        runtime_->questionMessages.clear();
        for (const QJsonValue& value : payload.value(QStringLiteral("questionMessages")).toArray()) {
            const QJsonObject message = value.toObject();
            const QString id = message.value(QStringLiteral("id")).toString();
            if (!id.isEmpty()) runtime_->questionMessages.insert(id, message);
        }
    }
    for (const QJsonValue& value : received) {
        const QJsonObject message = value.toObject();
        const QString id = message.value(QStringLiteral("id")).toString();
        if (!id.isEmpty()) {
            const bool outsideWindow = kind != QStringLiteral("history_page") && !runtime_->messages.contains(id) &&
                                       !runtime_->oldest.isEmpty() && id < runtime_->oldest;
            if (outsideWindow)
                runtime_->retainWindowUpdate(id, message);
            else {
                runtime_->messages.insert(id, message);
                runtime_->windowUpdates.remove(id);
                runtime_->windowUpdateOrder.removeAll(id);
            }
            if (kind == QStringLiteral("history_page")) runtime_->historyMessageIds.insert(id);
        }
        if (runtime_->awaitingResponse &&
            message.value(QStringLiteral("role")).toString() == QStringLiteral("assistant") &&
            message.value(QStringLiteral("created")).toVariant().toLongLong() >= runtime_->commandSentAt - 1000 &&
            (!message.value(QStringLiteral("active")).toBool() || !messageText(message).isEmpty()))
            runtime_->awaitingResponse = false;
    }
    if (!received.isEmpty() && kind == QStringLiteral("history_page")) {
        const QString oldest = received.last().toObject().value(QStringLiteral("id")).toString();
        const bool initial = runtime_->oldest.isEmpty();
        if (initial || oldest < runtime_->oldest) {
            runtime_->oldest = oldest;
            runtime_->older->setVisible(received.size() >= (initial ? 10 : 20));
        }
        // A live point read can precede the first SQL page. Once its actual
        // cursor is known, keep unknown older owners in the same bounded bypass.
        for (auto it = runtime_->messages.begin(); it != runtime_->messages.end();) {
            if (it.key() < runtime_->oldest && !runtime_->historyMessageIds.contains(it.key())) {
                runtime_->retainWindowUpdate(it.key(), it.value());
                it = runtime_->messages.erase(it);
            } else
                ++it;
        }
    }
    renderMessages();
    renderRequests(payload);
    if (!runtime_->pendingCommand) showStatus({});
}

void RemoteAgentPanel::renderMessages() {
    for (const QJsonValue& value : runtime_->outbox) {
        const QJsonObject entry = value.toObject();
        if (sessionKey(entry.value(QStringLiteral("owner")).toObject()) != sessionKey(runtime_->session)) continue;
        // Answers retain the question transcript echo instead of an ordinary pending row.
        if (entry.value(QStringLiteral("action")).toObject().value(QStringLiteral("action")) ==
            QStringLiteral("answer"))
            continue;
        const QString state = entry.value(QStringLiteral("state")).toString();
        const QString request = entry.value(QStringLiteral("requestId")).toString();
        const QString optimistic = QStringLiteral("msg_z_pending_%1").arg(request);
        QString content = entry.value(QStringLiteral("action")).toObject().value(QStringLiteral("text")).toString();
        if (content.isEmpty()) content = QStringLiteral("已发送会话附件");
        if (state == QStringLiteral("unknown") || state == QStringLiteral("held") ||
            state == QStringLiteral("rejected"))
            content += QStringLiteral("\n\n[重试确认](maichat-input-retry:%1)").arg(request);
        QJsonObject message = runtime_->messages.value(optimistic);
        message.insert(QStringLiteral("id"), optimistic);
        message.insert(QStringLiteral("role"), QStringLiteral("user"));
        if (!message.contains(QStringLiteral("created")))
            message.insert(QStringLiteral("created"), entry.value(QStringLiteral("created")));
        message.insert(QStringLiteral("parts"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("text")},
                                                                       {QStringLiteral("text"), content}}});
        runtime_->messages.insert(optimistic, message);
        runtime_->optimisticMessages.insert(optimistic);
    }
    struct Segment {
        QString id;
        QString text;
        MarkdownView::Style style;
    };
    const QString ownerKey = sessionKey(runtime_->session);
    QMap<QString, QJsonObject> displayed = runtime_->messages;
    const double cutoff = runtime_->messages.value(runtime_->oldest).value(QStringLiteral("created")).toDouble();
    for (auto it = runtime_->windowUpdates.cbegin(); it != runtime_->windowUpdates.cend(); ++it) {
        if (displayed.contains(it.key())) continue;
        QJsonObject message = it.value();
        QJsonArray recent;
        for (const QJsonValue& value : message.value(QStringLiteral("parts")).toArray()) {
            const QJsonObject part = value.toObject();
            // New hosts supply part.created. For old ULID snapshots the timeline
            // still orders each retained part; unknown age stays conservative.
            double created = part.value(QStringLiteral("created")).toDouble();
            if (created <= 0) created = coreIdTime(part.value(QStringLiteral("id")).toString());
            if (created <= 0) created = message.value(QStringLiteral("created")).toDouble();
            if (created >= cutoff || !questionPartText(part).isEmpty()) recent.append(value);
        }
        if (recent.isEmpty()) continue;
        message.insert(QStringLiteral("parts"), recent);
        displayed.insert(it.key(), message);
    }
    for (auto it = runtime_->questionMessages.begin(); it != runtime_->questionMessages.end(); ++it) {
        if (displayed.contains(it.key())) continue;
        QJsonObject projection = it.value();
        projection.insert(QStringLiteral("questionProjection"), true);
        displayed.insert(it.key(), projection);
    }
    QSet<QString> knownParts;
    QSet<QString> completedParts;
    for (const QJsonObject& message : displayed) {
        for (const QJsonValue& value : message.value(QStringLiteral("parts")).toArray()) {
            const QJsonObject part = value.toObject();
            if (questionPartText(part).isEmpty()) continue;
            const QString id = part.value(QStringLiteral("id")).toString();
            knownParts.insert(id);
            if (part.value(QStringLiteral("state")) == QStringLiteral("completed") &&
                !part.value(QStringLiteral("output")).toString().isEmpty())
                completedParts.insert(id);
        }
    }
    QHash<QString, QString> answerEchoes;
    const auto includeEcho = [&](const QJsonObject& entry) {
        if (sessionKey(entry.value(QStringLiteral("owner")).toObject()) != ownerKey) return;
        const QJsonObject question = entry.value(QStringLiteral("question")).toObject();
        if (question.value(QStringLiteral("id")).toString().isEmpty()) return;
        const QString part = questionPartId(question);
        if (!completedParts.contains(part))
            answerEchoes.insert(
                part, entry.value(QStringLiteral("action")).toObject().value(QStringLiteral("text")).toString());
        if (!knownParts.contains(part)) {
            const QJsonObject message = questionMessage(question);
            displayed.insert(message.value(QStringLiteral("id")).toString(), message);
            knownParts.insert(part);
        }
    };
    for (auto it = runtime_->acceptedAnswerEchoes.begin(); it != runtime_->acceptedAnswerEchoes.end();) {
        const QJsonObject entry = it.value();
        if (sessionKey(entry.value(QStringLiteral("owner")).toObject()) == ownerKey &&
            completedParts.contains(questionPartId(entry.value(QStringLiteral("question")).toObject()))) {
            it = runtime_->acceptedAnswerEchoes.erase(it);
        } else {
            includeEcho(entry);
            ++it;
        }
    }
    for (const QJsonValue& value : runtime_->outbox) {
        const QJsonObject entry = value.toObject();
        const QString state = entry.value(QStringLiteral("state")).toString();
        if ((state == QStringLiteral("queued") || state == QStringLiteral("sending")) &&
            entry.value(QStringLiteral("action")).toObject().value(QStringLiteral("action")) ==
                QStringLiteral("answer"))
            includeEcho(entry);
    }
    for (const QJsonValue& value : runtime_->questions) {
        const QJsonObject question = value.toObject();
        if (question.value(QStringLiteral("id")).toString().isEmpty() || knownParts.contains(questionPartId(question)))
            continue;
        const QJsonObject message = questionMessage(question);
        displayed.insert(message.value(QStringLiteral("id")).toString(), message);
        knownParts.insert(questionPartId(question));
    }
    const QList<QJsonObject> ordered = questionDisplayTimeline(displayed);
    QVector<Segment> segments;
    QSet<QString> renderedQuestions;
    for (const QJsonObject& message : ordered) {
        const QString messageId = message.value(QStringLiteral("id")).toString();
        const QJsonArray parts = message.value(QStringLiteral("parts")).toArray();
        const bool projection = message.value(QStringLiteral("questionProjection")).toBool();
        const auto style = message.value(QStringLiteral("role")) == QStringLiteral("user")
                               ? MarkdownView::Style::Bubble
                               : MarkdownView::Style::Document;
        bool hasQuestion = false;
        for (const QJsonValue& value : parts) hasQuestion |= !questionPartText(value.toObject()).isEmpty();
        if (!hasQuestion && !projection) {
            const QString text = messageText(message);
            if (!text.isEmpty()) segments.append({messageId, text, style});
            continue;
        }
        QJsonArray ordinary;
        int run = 0;
        const auto flush = [&] {
            if (ordinary.isEmpty()) return;
            QJsonObject chunk = message;
            chunk.insert(QStringLiteral("parts"), ordinary);
            const QString text = messageText(chunk);
            const QString first =
                ordinary.first().toObject().value(QStringLiteral("id")).toString(QString::number(run));
            if (!text.isEmpty()) segments.append({messageId + QStringLiteral(":segment:") + first, text, style});
            ordinary = {};
            ++run;
        };
        for (const QJsonValue& value : parts) {
            const QJsonObject part = value.toObject();
            const QString question = questionPartText(part);
            if (question.isEmpty()) {
                if (!projection) ordinary.append(value);
                continue;
            }
            flush();
            const QString partId = part.value(QStringLiteral("id")).toString(messageId + QStringLiteral(":question"));
            if (renderedQuestions.contains(partId)) continue;
            renderedQuestions.insert(partId);
            segments.append({QStringLiteral("remote-question:") + partId, question, MarkdownView::Style::Document});
            QString answer = part.value(QStringLiteral("state")) == QStringLiteral("completed")
                                 ? part.value(QStringLiteral("output")).toString()
                                 : answerEchoes.value(partId);
            if (!answer.isEmpty())
                segments.append({QStringLiteral("remote-answer:") + partId, answer, MarkdownView::Style::Bubble});
        }
        flush();
    }
    const bool followedLatest = runtime_->view->isAtBottom();
    QSet<QString> wanted;
    bool changed = segments.size() != runtime_->rendered.size();
    for (const Segment& segment : segments) {
        wanted.insert(segment.id);
        changed |= runtime_->rendered.value(segment.id) != segment.text;
    }
    runtime_->view->beginBatchUpdates();
    if (runtime_->thinking && changed) {
        // removeItem defers QWidget destruction. Hide it now so rapid question
        // updates cannot paint multiple retired thinking labels in one frame.
        runtime_->thinking->hide();
        runtime_->view->removeItem(QStringLiteral("remote-thinking"));
        runtime_->thinking = nullptr;
    }
    for (const QString& old : runtime_->rendered.keys()) {
        if (wanted.contains(old)) continue;
        runtime_->view->removeItem(old);
        runtime_->rendered.remove(old);
    }
    for (const Segment& segment : segments) {
        if (!runtime_->rendered.contains(segment.id))
            runtime_->view->addItem(segment.id, segment.style, segment.text);
        else if (runtime_->rendered.value(segment.id) != segment.text)
            runtime_->view->updateItem(segment.id, segment.text);
        runtime_->rendered.insert(segment.id, segment.text);
    }
    // Reorder existing rows only. Prepending or a late question projection must
    // retain Markdown selection, layout caches and the original row lifetime.
    for (int index = segments.size() - 2; index >= 0; --index)
        runtime_->view->moveItemBefore(segments[index].id, segments[index + 1].id);
    runtime_->view->endBatchUpdates();
    updateThinking();
    if (followedLatest) runtime_->view->scrollToBottom();
}

void RemoteAgentPanel::updateThinking() {
    const QJsonObject last = runtime_->messages.isEmpty() ? QJsonObject{} : runtime_->messages.last();
    const bool assistantActive = last.value(QStringLiteral("role")).toString() == QStringLiteral("assistant") &&
                                 last.value(QStringLiteral("active")).toBool();
    const bool visibleAnswer = assistantActive && !messageText(last).trimmed().isEmpty();
    const bool show =
        (runtime_->awaitingResponse || runtime_->session.value(QStringLiteral("busy")).toBool() || assistantActive) &&
        !visibleAnswer && runtime_->questions.isEmpty();
    if (show && !runtime_->thinking) {
        const bool followedLatest = runtime_->view->isAtBottom();
        runtime_->thinking = new QLabel(QStringLiteral("正在思考…"));
        runtime_->thinking->setObjectName(QStringLiteral("remoteThinkingIndicator"));
        runtime_->thinking->setStyleSheet(QStringLiteral("color:#98a2b3;background:transparent;font-size:12px;"));
        runtime_->thinking->setContentsMargins(0, UiZoom::s(2), 0, UiZoom::s(2));
        runtime_->view->addWidget(QStringLiteral("remote-thinking"), runtime_->thinking);
        if (followedLatest) runtime_->view->scrollToBottom();
    } else if (!show && runtime_->thinking) {
        runtime_->thinking->hide();
        runtime_->view->removeItem(QStringLiteral("remote-thinking"));
        runtime_->thinking = nullptr;
    }
}

void RemoteAgentPanel::renderRequests(const QJsonObject& payload) {
    while (QLayoutItem* item = runtime_->requestsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const QJsonValue& value : payload.value(QStringLiteral("permissions")).toArray()) {
        const QJsonObject permission = value.toObject();
        const QString id = permission.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) continue;
        auto* row = new QWidget(runtime_->requests);
        auto* line = new QHBoxLayout(row);
        line->addWidget(
            new QLabel(QStringLiteral("请求批准 · %1").arg(permission.value(QStringLiteral("tool")).toString()), row),
            1);
        const auto addDecision = [&](const QString& title, const QString& decision) {
            auto* button = new QPushButton(title, row);
            line->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, id, decision] {
                sendAction({{QStringLiteral("action"), QStringLiteral("permission")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("decision"), decision}},
                           false);
            });
        };
        addDecision(QStringLiteral("允许一次"), QStringLiteral("approved"));
        if (permission.value(QStringLiteral("allowForSession")).toBool())
            addDecision(QStringLiteral("本次会话允许"), QStringLiteral("approved_for_session"));
        addDecision(QStringLiteral("拒绝"), QStringLiteral("denied"));
        runtime_->requestsLayout->addWidget(row);
    }
    runtime_->requests->setVisible(runtime_->requestsLayout->count() > 0);
}

void RemoteAgentPanel::updateControls() {
    const bool online = runtime_->session.value(QStringLiteral("online")).toBool();
    const bool busy = runtime_->session.value(QStringLiteral("busy")).toBool();
    runtime_->send->setText({});
    runtime_->send->setIcon(actionIcon(false, QColor(Qt::white)));
    runtime_->send->setToolTip(QStringLiteral("\u53d1\u9001\u5230\u6240\u5c5e\u8bbe\u5907"));
    runtime_->send->setAccessibleName(runtime_->send->toolTip());
    runtime_->send->setStyleSheet(UiZoom::scaleQss(
        QStringLiteral("QPushButton#remoteAgentSendButton{background:#0b67b7;border:none;border-radius:18px;padding:0;}"
                       "QPushButton#remoteAgentSendButton:hover{background:#095a9f;}"
                       "QPushButton#remoteAgentSendButton:disabled{background:#b8d3e8;}")));
    runtime_->send->setEnabled(
        online && (!runtime_->editor->toPlainText().trimmed().isEmpty() || !runtime_->attachments.isEmpty()));
    runtime_->stop->setVisible(busy);
    runtime_->stop->setEnabled(online && busy);
    runtime_->editor->positionCornerAction();
    bool recovery = false;
    runtime_->pendingCommand = false;
    for (const QJsonValue& value : runtime_->outbox) {
        const QJsonObject entry = value.toObject();
        if (sessionKey(entry.value(QStringLiteral("owner")).toObject()) != sessionKey(runtime_->session)) continue;
        const QString state = entry.value(QStringLiteral("state")).toString();
        runtime_->pendingCommand |= state == QStringLiteral("sending");
        recovery |= state != QStringLiteral("sending") && state != QStringLiteral("forwarded") &&
                    state != QStringLiteral("queued");
    }
    runtime_->recover->setVisible(recovery);
}

void RemoteAgentPanel::showStatus(const QString& message, bool failure) {
    runtime_->status->setText(message);
    runtime_->status->setVisible(!message.isEmpty());
    runtime_->status->setStyleSheet(failure ? QStringLiteral("color:#b42318;background:#fff5f3;")
                                            : QStringLiteral("color:#667085;background:transparent;font-size:12px;"));
}
