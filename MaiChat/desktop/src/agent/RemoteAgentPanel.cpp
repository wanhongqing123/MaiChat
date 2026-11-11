// 文件作用：呈现远端 Session 的可见记录；普通输入按原问题或任务投递，权限决定独立发往所属设备。
#include "agent/RemoteAgentPanel.h"

#include "MaiCloudDriveClient.h"
#include "MaiCloudModelCatalog.h"

#include <QDateTime>
#include <QCoreApplication>
#include <QEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFile>
#include <QFrame>
#include <QFutureWatcher>
#include <QSaveFile>
#include <QScreen>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QList>
#include <QMap>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QStringConverter>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <atomic>

#include "agent/AgentToolGroupWidget.h"
#include "markdown/MarkdownView.h"
#include "ui/ComposerTextEdit.h"
#include "ui/UiZoom.h"
#include "ui/DesktopRasterImage.h"
#include "ui/ImagePreviewDialog.h"
#include "agent/DesktopSessionAttachments.h"

namespace {

struct ImageCancellation {
    std::atomic_bool canceled{false};
    MaiCloudDriveCancel* transfer = mai_cloud_drive_cancel_create();
    ~ImageCancellation() { mai_cloud_drive_cancel_destroy(transfer); }
    void cancel() {
        canceled = true;
        mai_cloud_drive_cancel_request(transfer);
    }
};

class RemoteImageLabel final : public QLabel {
public:
    std::function<void()> activated;
    RemoteImageLabel() {
        setScaledContents(true);
        setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    }
    bool hasHeightForWidth() const override { return !pixmap().isNull(); }
    int heightForWidth(int width) const override {
        const auto image = pixmap();
        if (image.isNull()) return QLabel::heightForWidth(width);
        // The shared decoder bounds the thumbnail; layout scales it down for
        // a narrow reading column while retaining its original aspect ratio.
        const int inner = qBound(1, width - margin() * 2, image.width());
        return qMax(1, qRound(double(image.height()) * inner / image.width())) + margin() * 2;
    }
protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && !pixmap().isNull() && activated) {
            activated();
            event->accept();
            return;
        }
        QLabel::mousePressEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space) &&
            !pixmap().isNull() && activated) {
            activated();
            event->accept();
            return;
        }
        QLabel::keyPressEvent(event);
    }
};

QString sessionKey(const QJsonObject& value) {
    return value.value(QStringLiteral("device_slot")).toString() + QLatin1Char(':') +
           value.value(QStringLiteral("login_session_id")).toString() + QLatin1Char(':') +
           value.value(QStringLiteral("id")).toString();
}

const QStringList& sessionModelChoices() {
    static const QStringList models{QStringLiteral("glm-5.3"), QStringLiteral("glm-5.3-flash"),
                                    QStringLiteral("deepseek-flash"), QStringLiteral("deepseek-v4-pro")};
    return models;
}

bool validModelOwner(const QJsonObject& value) {
    return !value.value(QStringLiteral("id")).toString().isEmpty() &&
           !value.value(QStringLiteral("device_slot")).toString().isEmpty() &&
           !value.value(QStringLiteral("login_session_id")).toString().isEmpty();
}

bool sameModelRoom(const QJsonObject& first, const QJsonObject& second) {
    return first.value(QStringLiteral("id")) == second.value(QStringLiteral("id")) &&
           first.value(QStringLiteral("device_slot")) == second.value(QStringLiteral("device_slot"));
}

QString sessionEffortTitle(const QString& effort) {
    if (effort == QStringLiteral("low")) return QStringLiteral("简单");
    if (effort == QStringLiteral("high")) return QStringLiteral("标准");
    if (effort == QStringLiteral("max")) return QStringLiteral("深度");
    return QStringLiteral("未确认");
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
        const QString sourceId = unit.message.value(QStringLiteral("source_message_id"))
                                     .toString(unit.message.value(QStringLiteral("id")).toString());
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
    return text.join(QStringLiteral("\n\n"));
}

bool hasVisibleActivity(const QJsonObject& message) {
    if (!messageText(message).trimmed().isEmpty()) return true;
    // Ordinary tools are now embedded widgets rather than answer text. They
    // still acknowledge visible activity and must suppress a second thinking row.
    for (const auto& value : message.value(QStringLiteral("parts")).toArray())
        if (value.toObject().value(QStringLiteral("kind")) == QStringLiteral("tool")) return true;
    return false;
}

QString toolDetailKey(const QJsonObject& tool) {
    return tool.value(QStringLiteral("messageId")).toString() + QChar(0x1f) +
           tool.value(QStringLiteral("id")).toString();
}

bool validDetailRevision(const QString& revision) {
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(revision).hasMatch();
}

const QStringList& toolDetailFields() {
    static const QStringList fields{QStringLiteral("input"), QStringLiteral("output"), QStringLiteral("error")};
    return fields;
}

// A tool keeps the same quiet metadata row as the local card. Its selectable
// plain-text fields have their own disclosure, separate from the answer Markdown.
class RemoteToolDetailRow final : public QWidget {
public:
    std::function<void(const QJsonObject&, const QString&)> activated;
    explicit RemoteToolDetailRow(QWidget* parent = nullptr) : QWidget(parent) {
        auto* column = new QVBoxLayout(this);
        column->setContentsMargins(0, UiZoom::s(2), 0, UiZoom::s(2));
        column->setSpacing(UiZoom::s(5));
        auto* head = new QHBoxLayout;
        metadata_ = new QLabel(this);
        metadata_->setObjectName(QStringLiteral("remoteAgentToolDetail"));
        metadata_->setTextFormat(Qt::PlainText);
        metadata_->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        metadata_->setWordWrap(true);
        UiZoom::setFont(metadata_, UiZoom::TextRole::Detail, metadata_->font());
        UiZoom::setStyle(metadata_, QStringLiteral("QLabel{background:transparent;color:#667085;font-size:@detail;}"));
        head->addWidget(metadata_, 1);
        disclosure_ = new QPushButton(QStringLiteral("›"), this);
        disclosure_->setObjectName(QStringLiteral("remoteAgentToolDisclosure"));
        disclosure_->setAccessibleName(QStringLiteral("展开工具参数、输出与错误"));
        disclosure_->setCursor(Qt::PointingHandCursor);
        disclosure_->setFixedSize(UiZoom::s(22), UiZoom::s(22));
        disclosure_->setStyleSheet(QStringLiteral("QPushButton{background:transparent;border:none;color:#98a2b3;padding:0;}"));
        head->addWidget(disclosure_);
        column->addLayout(head);
        body_ = new QWidget(this);
        auto* fields = new QVBoxLayout(body_);
        fields->setContentsMargins(UiZoom::s(9), UiZoom::s(7), UiZoom::s(9), UiZoom::s(7));
        fields->setSpacing(UiZoom::s(5));
        body_->setObjectName(QStringLiteral("remoteAgentToolFields"));
        body_->setStyleSheet(QStringLiteral("QWidget#remoteAgentToolFields{background:#f7f8fa;border:1px solid #e4e7ec;border-radius:7px;}"));
        note_ = new QLabel(body_);
        note_->setWordWrap(true);
        UiZoom::setFont(note_, UiZoom::TextRole::Detail, note_->font());
        fields->addWidget(note_);
        for (const QString& field : toolDetailFields()) {
            auto* section = new QWidget(body_);
            auto* layout = new QVBoxLayout(section);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(UiZoom::s(3));
            const QString title = field == QStringLiteral("input") ? QStringLiteral("参数")
                                  : field == QStringLiteral("output") ? QStringLiteral("输出") : QStringLiteral("错误");
            auto* label = new QLabel(title, section);
            UiZoom::setFont(label, UiZoom::TextRole::Detail, label->font());
            label->setStyleSheet(QStringLiteral("color:#667085;background:transparent;"));
            layout->addWidget(label);
            auto* text = new QLabel(section);
            text->setObjectName(QStringLiteral("remoteAgentToolFieldText"));
            text->setProperty("detailField", field);
            text->setTextFormat(Qt::PlainText);
            text->setWordWrap(true);
            text->setAlignment(Qt::AlignLeft | Qt::AlignTop);
            text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            UiZoom::setFont(text, UiZoom::TextRole::Detail, text->font());
            text->setStyleSheet(QStringLiteral("color:#475467;background:transparent;"));
            layout->addWidget(text);
            auto* more = new QPushButton(section);
            more->setObjectName(QStringLiteral("remoteAgentToolFieldMore"));
            more->setProperty("detailField", field);
            more->setCursor(Qt::PointingHandCursor);
            more->setStyleSheet(QStringLiteral("QPushButton{background:transparent;border:none;color:#0b67b7;text-align:left;padding:2px 0;}"));
            layout->addWidget(more, 0, Qt::AlignLeft);
            texts_.insert(field, text);
            more_.insert(field, more);
            fields->addWidget(section);
            QObject::connect(more, &QPushButton::clicked, this, [this, field] {
                if (activated) activated(tool_, field);
            });
        }
        body_->hide();
        column->addWidget(body_);
        QObject::connect(disclosure_, &QPushButton::clicked, this, [this] {
            if (activated) activated(tool_, {});
        });
    }

    void setTool(const QJsonObject& tool) {
        tool_ = tool;
        refreshMetadata(tool);
        setProperty("detailKey", toolDetailKey(tool));
    }

    const QJsonObject& tool() const { return tool_; }

    void refresh(const QJsonObject& presentation) {
        auto metadata = tool_;
        if (presentation.contains(QStringLiteral("state")))
            metadata.insert(QStringLiteral("state"), presentation.value(QStringLiteral("state")));
        refreshMetadata(metadata);
        const bool expanded = presentation.value(QStringLiteral("expanded")).toBool();
        body_->setVisible(expanded);
        disclosure_->setText(expanded ? QStringLiteral("⌄") : QStringLiteral("›"));
        const QString note = presentation.value(QStringLiteral("note")).toString();
        note_->setText(note);
        note_->setVisible(!note.isEmpty());
        for (const QString& field : toolDetailFields()) {
            const auto value = presentation.value(field).toObject();
            QLabel* text = texts_.value(field);
            const QString content = value.value(QStringLiteral("text")).toString();
            if (text->text() != content) text->setText(content);
            text->setToolTip(value.value(QStringLiteral("invalidUtf8")).toBool()
                                 ? QStringLiteral("原文包含无效 UTF-8；显示替换字符，源字节按原样分页保留") : QString());
            QPushButton* more = more_.value(field);
            more->setText(value.value(QStringLiteral("button")).toString());
            more->setVisible(value.value(QStringLiteral("canLoad")).toBool());
            more->setEnabled(value.value(QStringLiteral("enabled")).toBool());
        }
        updateGeometry();
    }

private:
    void refreshMetadata(const QJsonObject& tool) {
        QString name = tool.value(QStringLiteral("tool")).toString().trimmed();
        if (name.isEmpty()) name = QStringLiteral("工具");
        QString detail = name + QStringLiteral(" · ") + agentToolStateText(tool.value(QStringLiteral("state")).toString());
        if (name == QStringLiteral("curl") && tool.value(QStringLiteral("state")) == QStringLiteral("error")) {
            const QString summary = tool.value(QStringLiteral("error")).toString();
            if (!summary.isEmpty()) detail += QLatin1Char('\n') + summary;
        }
        if (metadata_->text() != detail) metadata_->setText(detail);
    }
    QJsonObject tool_;
    QLabel* metadata_ = nullptr;
    QPushButton* disclosure_ = nullptr;
    QWidget* body_ = nullptr;
    QLabel* note_ = nullptr;
    QHash<QString, QLabel*> texts_;
    QHash<QString, QPushButton*> more_;
};

class RemoteToolGroupCard final : public AgentToolGroupWidget {
public:
    std::function<void(const QJsonObject&, const QString&)> activated;
    std::function<QJsonObject(const QJsonObject&)> presentation;
    explicit RemoteToolGroupCard(const QString& id) : AgentToolGroupWidget(id) {
        setObjectName(QStringLiteral("remoteAgentToolGroup"));
        disclosureButton()->setObjectName(QStringLiteral("remoteAgentToolGroupDisclosure"));
    }

    void setTools(const QJsonArray& tools) {
        QStringList states;
        QSet<QString> wanted;
        for (const auto& value : tools) {
            const QJsonObject tool = value.toObject();
            const QString id = tool.value(QStringLiteral("id")).toString();
            wanted.insert(id);
            const QString state = tool.value(QStringLiteral("state")).toString();
            states.append(state);
            RemoteToolDetailRow* row = rows_.value(id, nullptr);
            if (!row) {
                row = new RemoteToolDetailRow;
                row->activated = [this](const QJsonObject& tool, const QString& field) {
                    if (activated) activated(tool, field);
                };
                addDetail(row);
                rows_.insert(id, row);
            }
            row->setTool(tool);
            if (presentation) row->refresh(presentation(tool));
        }
        for (const QString& id : rows_.keys())
            if (!wanted.contains(id)) removeDetail(rows_.take(id));
        setTitle(agentToolGroupTitle(states));
        refreshDetails();
        updateGeometry();
    }

    void refreshDetails() {
        if (presentation) {
            QStringList states;
            for (auto* row : rows_) {
                const auto value = presentation(row->tool());
                row->refresh(value);
                states.append(value.value(QStringLiteral("state")).toString(
                    row->tool().value(QStringLiteral("state")).toString()));
            }
            setTitle(agentToolGroupTitle(states));
        }
        refreshWidth(isExpanded());
    }

protected:
    void disclosureClicked() override { refreshWidth(!isExpanded()); }

private:
    void refreshWidth(bool groupExpanded) {
        bool detailExpanded = false;
        if (groupExpanded && presentation)
            for (auto* row : rows_)
                detailExpanded |= presentation(row->tool()).value(QStringLiteral("expanded")).toBool();
        // MarkdownView intentionally keeps metadata-only Maximum widgets at
        // their natural width. A readable detail uses the available column;
        // collapsing restores the original compact group without a global rule.
        const auto policy = detailExpanded ? QSizePolicy::Preferred : QSizePolicy::Maximum;
        if (sizePolicy().horizontalPolicy() == policy) return;
        setSizePolicy(policy, QSizePolicy::Fixed);
        updateGeometry();
    }
    QHash<QString, RemoteToolDetailRow*> rows_;
};

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
    struct ToolField {
        QString text;
        QByteArray raw;
        QString pendingId;
        QString error;
        qint64 nextOffset = 0;
        qint64 totalBytes = 0;
        bool loaded = false;
        bool hasMore = false;
        bool invalidUtf8 = false;
    };
    struct ToolDetail {
        QJsonObject metadata;
        QHash<QString, ToolField> fields;
        QString revision;
        QString state;
        bool expanded = false;
        bool restartRequired = false;
        // Only an explicit Busy->user restart may learn a changed state before
        // its snapshot. The first fully validated page locks all three fields.
        bool busyIdentityPending = false;
    };
    struct ToolRead {
        QJsonObject owner;
        QJsonObject command;
        QString key;
        QString account;
        quint64 accountGeneration = 0;
        quint64 generation = 0;
    };
    struct HistoryLiveTool {
        QString messageId;
        QJsonObject part;
    };
    QString detailAccount;
    quint64 detailAccountGeneration = 0;
    quint64 detailGeneration = 0;
    QHash<QString, ToolDetail> toolDetails;
    QHash<QString, ToolRead> toolReads;
    QSet<QString> toolRefresh;
    // Only the latest part observed while this single history request was in
    // flight is retained. A fresh request clears the guard; hashes are unordered.
    quint64 historyToolGeneration = 0;
    QHash<QString, HistoryLiveTool> historyLiveTools;
    struct ModelConfiguration {
        QJsonObject owner;
        QJsonObject value;
    };
    struct ModelRequest {
        QJsonObject owner;
        QJsonObject command;
    };
    QJsonObject session;
    // Discovery/catalog rows are never a model database. Only exact owner ACKs
    // enter this bounded, login-scoped cache; drafts/outbox remain independent.
    QHash<QString, ModelConfiguration> modelConfigurations;
    QStringList modelConfigurationOrder;
    QHash<QString, ModelRequest> modelRequests;
    QHash<QString, QString> modelRequestByOwner;
    QString modelStatus;
    QString modelStatusOwner;
    QPushButton* more = nullptr;
    QFrame* actionPanel = nullptr;
    QPushButton* modelSection = nullptr;
    QPushButton* effortSection = nullptr;
    QLabel* modelValue = nullptr;
    QLabel* effortValue = nullptr;
    QWidget* modelOptions = nullptr;
    QWidget* effortOptions = nullptr;
    QList<QPushButton*> modelButtons;
    QList<QPushButton*> effortButtons;
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
    QHash<QString, RemoteToolGroupCard*> toolGroups;
    QHash<QString, QLabel*> imageCards;
    ImageAccessProvider imageAccess;
    quint64 imageGeneration = 0;
    QString imageAccount;
    QHash<QString, std::shared_ptr<ImageCancellation>> imageTransfers;
    QPointer<ImagePreviewDialog> imagePreview;
    std::shared_ptr<ImageCancellation> previewCancellation;
    quint64 previewGeneration = 0;
    QString previewIdentity;
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
    runtime_->more = new QPushButton(QStringLiteral("⋯"), header);
    runtime_->more->setObjectName(QStringLiteral("remoteAgentMoreActions"));
    runtime_->more->setAccessibleName(QStringLiteral("更多"));
    runtime_->more->setCursor(Qt::PointingHandCursor);
    runtime_->more->setFixedSize(UiZoom::s(34), UiZoom::s(34));
    UiZoom::setStyle(runtime_->more, QStringLiteral(
        "QPushButton{background:transparent;border:0;color:#24334a;font-size:@heading2;border-radius:8px;}"
        "QPushButton:hover{background:#f1f6fb;}"));
    headerLayout->addWidget(runtime_->more);
    layout->addWidget(header);
    // Match the local creation panel's expandable model/effort rows. Choices are
    // controls sent to the owner; clicking never changes the confirmed selection.
    runtime_->actionPanel = new QFrame(this, Qt::Popup | Qt::FramelessWindowHint);
    runtime_->actionPanel->setObjectName(QStringLiteral("remoteAgentActionPanel"));
    runtime_->actionPanel->setFixedWidth(UiZoom::s(288));
    UiZoom::setStyle(runtime_->actionPanel, QStringLiteral(
        "QFrame#remoteAgentActionPanel{background:#ffffff;border:1px solid #e2e8f0;border-radius:12px;}"
        "QFrame#remoteAgentActionPanel QPushButton{background:transparent;border:0;color:#24334a;"
        "text-align:left;padding:0 12px;font-size:@detail;border-radius:8px;}"
        "QFrame#remoteAgentActionPanel QPushButton:hover{background:#f1f6fb;}"
        "QFrame#remoteAgentActionPanel QPushButton:checked{background:#e6f2ff;color:#0a65ad;font-weight:600;}"
        "QFrame#remoteAgentActionPanel QPushButton:disabled{color:#98a2b3;}"));
    auto* actionLayout = new QVBoxLayout(runtime_->actionPanel);
    actionLayout->setContentsMargins(UiZoom::s(8), UiZoom::s(8), UiZoom::s(8), UiZoom::s(8));
    actionLayout->setSpacing(UiZoom::s(2));
    const auto summary = [this](const QString& name, const QString& title, const QString& valueName,
                                QLabel*& value) {
        auto* row = new QPushButton(runtime_->actionPanel);
        row->setObjectName(name);
        row->setCursor(Qt::PointingHandCursor);
        row->setFixedHeight(UiZoom::s(54));
        auto* rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(UiZoom::s(12), 0, UiZoom::s(12), 0);
        auto* column = new QVBoxLayout;
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(UiZoom::s(2));
        auto* label = new QLabel(title, row);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        UiZoom::setStyle(label, QStringLiteral("QLabel{border:0;background:transparent;color:#24334a;"
                                              "font-size:@detail;font-weight:600;}"));
        column->addWidget(label);
        value = new QLabel(QStringLiteral("未确认"), row);
        value->setObjectName(valueName);
        value->setTextFormat(Qt::PlainText);
        value->setAttribute(Qt::WA_TransparentForMouseEvents);
        UiZoom::setStyle(value, QStringLiteral("QLabel{border:0;background:transparent;color:#738198;"
                                              "font-size:@metadata;}"));
        column->addWidget(value);
        rowLayout->addLayout(column, 1);
        auto* arrow = new QLabel(QStringLiteral("›"), row);
        arrow->setAttribute(Qt::WA_TransparentForMouseEvents);
        UiZoom::setStyle(arrow, QStringLiteral("QLabel{border:0;background:transparent;color:#98a2b3;"
                                              "font-size:@heading3;}"));
        rowLayout->addWidget(arrow);
        return row;
    };
    runtime_->modelSection = summary(QStringLiteral("remoteAgentModelSection"), QStringLiteral("主模型"),
                                     QStringLiteral("remoteAgentModelValue"), runtime_->modelValue);
    actionLayout->addWidget(runtime_->modelSection);
    runtime_->modelOptions = new QWidget(runtime_->actionPanel);
    auto* modelOptionsLayout = new QVBoxLayout(runtime_->modelOptions);
    modelOptionsLayout->setContentsMargins(0, UiZoom::s(4), 0, UiZoom::s(4));
    modelOptionsLayout->setSpacing(0);
    for (const auto& model : sessionModelChoices()) {
        auto* option = new QPushButton(model, runtime_->modelOptions);
        option->setObjectName(QStringLiteral("remoteAgentModelOption-") + model);
        option->setProperty("model", model);
        option->setCheckable(true);
        option->setFixedHeight(UiZoom::s(38));
        modelOptionsLayout->addWidget(option);
        runtime_->modelButtons.append(option);
        connect(option, &QPushButton::clicked, this, [this, model] {
            updateSessionModelControls();  // Undo QPushButton's optimistic checked state.
            runtime_->actionPanel->hide();
            sendSessionModelCommand({{QStringLiteral("action"), QStringLiteral("set_model")},
                                     {QStringLiteral("model"), model}});
        });
    }
    actionLayout->addWidget(runtime_->modelOptions);
    runtime_->modelOptions->hide();
    runtime_->effortSection = summary(QStringLiteral("remoteAgentEffortSection"), QStringLiteral("思考强度"),
                                      QStringLiteral("remoteAgentEffortValue"), runtime_->effortValue);
    actionLayout->addWidget(runtime_->effortSection);
    runtime_->effortOptions = new QWidget(runtime_->actionPanel);
    auto* effortOptionsLayout = new QVBoxLayout(runtime_->effortOptions);
    effortOptionsLayout->setContentsMargins(0, UiZoom::s(4), 0, UiZoom::s(4));
    effortOptionsLayout->setSpacing(0);
    for (const auto& effort : {QStringLiteral("low"), QStringLiteral("high"), QStringLiteral("max")}) {
        auto* option = new QPushButton(sessionEffortTitle(effort), runtime_->effortOptions);
        option->setObjectName(QStringLiteral("remoteAgentEffortOption-") + effort);
        option->setProperty("reasoningEffort", effort);
        option->setCheckable(true);
        option->setFixedHeight(UiZoom::s(38));
        effortOptionsLayout->addWidget(option);
        runtime_->effortButtons.append(option);
        connect(option, &QPushButton::clicked, this, [this, effort] {
            updateSessionModelControls();
            runtime_->actionPanel->hide();
            const auto configuration = runtime_->modelConfigurations.value(sessionKey(runtime_->session)).value;
            sendSessionModelCommand({{QStringLiteral("action"), QStringLiteral("set_model")},
                                     {QStringLiteral("model"), configuration.value(QStringLiteral("model"))},
                                     {QStringLiteral("reasoningEffort"), effort}});
        });
    }
    actionLayout->addWidget(runtime_->effortOptions);
    runtime_->effortOptions->hide();
    connect(runtime_->modelSection, &QPushButton::clicked, this, [this] {
        runtime_->effortOptions->hide();
        runtime_->modelOptions->setVisible(!runtime_->modelOptions->isVisible());
        positionActionPanel();
    });
    connect(runtime_->effortSection, &QPushButton::clicked, this, [this] {
        runtime_->modelOptions->hide();
        runtime_->effortOptions->setVisible(!runtime_->effortOptions->isVisible());
        positionActionPanel();
    });
    connect(runtime_->more, &QPushButton::clicked, this, [this] {
        if (runtime_->actionPanel->isVisible()) {
            runtime_->actionPanel->hide();
        } else {
            if (!runtime_->modelConfigurations.contains(sessionKey(runtime_->session))) requestSessionModel();
            positionActionPanel();
            runtime_->actionPanel->show();
            runtime_->actionPanel->raise();
        }
    });
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
        runtime_->historyLiveTools.clear();
        runtime_->older->setEnabled(true);
        if (runtime_->messages.isEmpty()) {
            showStatus(QStringLiteral("会话读取超时，可重新进入会话重试"), true);
        }
    });
    updateControls();
}

RemoteAgentPanel::~RemoteAgentPanel() {
    resetImagePreview();
    for (const auto& transfer : runtime_->imageTransfers) transfer->cancel();
    if (!runtime_->session.isEmpty()) {
        runtime_->drafts.insert(sessionKey(runtime_->session), runtime_->editor->toPlainText());
        runtime_->attachmentDrafts.insert(sessionKey(runtime_->session), runtime_->attachments);
    }
    persistOutbox();
}

void RemoteAgentPanel::setCommandHandler(CommandHandler handler) {
    // A replacement transport may represent another authenticated connection.
    // Outstanding reads must not be adopted by its eventual receipts.
    invalidateToolDetailRequests(true);
    runtime_->command = std::move(handler);
    requestSessionModel();
}
void RemoteAgentPanel::setToolDetailScope(const QString& accountId, quint64 generation) {
    if (runtime_->detailAccount == accountId && runtime_->detailAccountGeneration == generation) return;
    runtime_->detailAccount = accountId;
    runtime_->detailAccountGeneration = generation;
    invalidateToolDetailRequests(true);
}

void RemoteAgentPanel::invalidateToolDetailRequests(bool clearCache) {
    ++runtime_->detailGeneration;
    runtime_->toolReads.clear();
    runtime_->toolRefresh.clear();
    runtime_->historyLiveTools.clear();
    if (clearCache) {
        runtime_->toolDetails.clear();
    } else {
        for (auto& detail : runtime_->toolDetails)
            for (auto& field : detail.fields)
                if (!field.pendingId.isEmpty()) {
                    field.pendingId.clear();
                    field.error = QStringLiteral("读取已中断，请重试");
                }
    }
    refreshToolDetailViews();
}

void RemoteAgentPanel::syncToolDetailMetadata(const QJsonObject& tool) {
    const QString key = toolDetailKey(tool);
    auto& detail = runtime_->toolDetails[key];
    const bool existed = !detail.metadata.isEmpty();
    const bool changed = existed &&
        (detail.metadata.value(QStringLiteral("detailRevision")) != tool.value(QStringLiteral("detailRevision")) ||
         detail.metadata.value(QStringLiteral("state")) != tool.value(QStringLiteral("state")) ||
         detail.metadata.value(QStringLiteral("callId")) != tool.value(QStringLiteral("callId")) ||
         detail.metadata.value(QStringLiteral("tool")) != tool.value(QStringLiteral("tool")) ||
         detail.metadata.value(QStringLiteral("detailsAvailable")) != tool.value(QStringLiteral("detailsAvailable")));
    if (changed) {
        for (auto it = runtime_->toolReads.begin(); it != runtime_->toolReads.end();)
            it = it->key == key ? runtime_->toolReads.erase(it) : ++it;
        detail.fields.clear();
        detail.restartRequired = false;
        detail.busyIdentityPending = false;
    }
    detail.metadata = tool;
    if (!existed || changed) {
        detail.revision = tool.value(QStringLiteral("detailRevision")).toString();
        detail.state = tool.value(QStringLiteral("state")).toString();
    }
    if (!tool.value(QStringLiteral("detailsAvailable")).toBool()) {
        // Old owners may have sent inline data. Display exactly what they did
        // provide without claiming absent fields are empty or complete.
        for (const QString& name : toolDetailFields()) {
            const QJsonValue value = tool.value(name);
            if (!value.isString() && !value.isObject() && !value.isArray()) continue;
            auto& field = detail.fields[name];
            field.text = value.isString() ? value.toString()
                         : QString::fromUtf8((value.isObject() ? QJsonDocument(value.toObject())
                                                               : QJsonDocument(value.toArray())).toJson(QJsonDocument::Indented));
            field.loaded = true;
        }
    }
    if (changed && detail.expanded) runtime_->toolRefresh.insert(key);
}

QJsonObject RemoteAgentPanel::toolDetailPresentation(const QJsonObject& tool) const {
    const auto detail = runtime_->toolDetails.value(toolDetailKey(tool));
    const bool available = detail.metadata.value(QStringLiteral("detailsAvailable")).toBool() &&
                           validDetailRevision(detail.metadata.value(QStringLiteral("detailRevision")).toString());
    const bool online = runtime_->session.value(QStringLiteral("online")).toBool() && bool(runtime_->command);
    QJsonObject presentation{{QStringLiteral("expanded"), detail.expanded},
                              {QStringLiteral("state"), detail.state.isEmpty()
                                   ? tool.value(QStringLiteral("state")).toString() : detail.state}};
    if (!available)
        presentation.insert(QStringLiteral("note"), QStringLiteral("所属设备未提供分页详情；仅显示已有内嵌内容"));
    else if (detail.restartRequired)
        presentation.insert(QStringLiteral("note"), QStringLiteral("详情已更新，请重读；旧页不会拼入新版结果"));
    else if (!online)
        presentation.insert(QStringLiteral("note"), QStringLiteral("所属设备不可达；已缓存内容仍可查看，未读内容可在恢复连接后重试"));
    for (const QString& name : toolDetailFields()) {
        const auto field = detail.fields.value(name);
        QString text = field.text;
        if (!field.loaded) text = !field.pendingId.isEmpty() ? QStringLiteral("正在读取…")
                                       : !field.error.isEmpty() ? field.error
                                       : available ? QStringLiteral("尚未读取") : QStringLiteral("所属设备未提供此字段");
        else if (!field.error.isEmpty()) text += QStringLiteral("\n\n") + field.error;
        const bool canLoad = available && (detail.restartRequired || !field.loaded || field.hasMore || !field.error.isEmpty());
        presentation.insert(name, QJsonObject{{QStringLiteral("text"), text},
                                             {QStringLiteral("invalidUtf8"), field.invalidUtf8},
                                             {QStringLiteral("canLoad"), canLoad},
                                             {QStringLiteral("enabled"), online && field.pendingId.isEmpty()},
                                             {QStringLiteral("button"), detail.restartRequired ? QStringLiteral("重读详情")
                                                  : !field.pendingId.isEmpty() ? QStringLiteral("正在读取…")
                                                  : !field.error.isEmpty() ? QStringLiteral("重试")
                                                  : field.loaded ? QStringLiteral("继续加载") : QStringLiteral("读取")}});
    }
    return presentation;
}

void RemoteAgentPanel::refreshToolDetailViews() {
    for (auto* group : runtime_->toolGroups) group->refreshDetails();
}

void RemoteAgentPanel::activateToolDetail(const QJsonObject& tool, const QString& field) {
    const QString key = toolDetailKey(tool);
    if (!runtime_->toolDetails.contains(key)) syncToolDetailMetadata(tool);
    auto& detail = runtime_->toolDetails[key];
    if (field.isEmpty()) {
        detail.expanded = !detail.expanded;
        if (detail.expanded && !detail.restartRequired)
            for (const QString& name : toolDetailFields()) requestToolDetailPage(key, name);
    } else if (detail.restartRequired) {
        // A Busy reply proves the snapshot changed. First pages may omit the
        // retired revision; the first returned revision binds the whole refresh.
        detail.fields.clear();
        detail.revision.clear();
        detail.state.clear();
        detail.busyIdentityPending = true;
        detail.restartRequired = false;
        for (const QString& name : toolDetailFields()) requestToolDetailPage(key, name);
    } else if (toolDetailFields().contains(field)) {
        requestToolDetailPage(key, field);
    }
    refreshToolDetailViews();
}

void RemoteAgentPanel::requestToolDetailPage(const QString& key, const QString& name) {
    auto found = runtime_->toolDetails.find(key);
    if (found == runtime_->toolDetails.end()) return;
    auto& detail = found.value();
    if (!detail.metadata.value(QStringLiteral("detailsAvailable")).toBool() ||
        !validDetailRevision(detail.metadata.value(QStringLiteral("detailRevision")).toString())) return;
    auto& field = detail.fields[name];
    if (!field.pendingId.isEmpty() || (field.loaded && !field.hasMore && field.error.isEmpty())) return;
    if (!runtime_->command || !runtime_->session.value(QStringLiteral("online")).toBool() ||
        !validModelOwner(runtime_->session)) {
        field.error = QStringLiteral("所属设备不可达，请恢复连接后重试");
        return;
    }
    if (runtime_->toolReads.size() >= 128) {
        field.error = QStringLiteral("正在读取其他详情，请稍后重试");
        return;
    }
    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject command{{QStringLiteral("action"), QStringLiteral("get_tool_detail")},
                         {QStringLiteral("requestId"), requestId},
                         {QStringLiteral("messageId"), detail.metadata.value(QStringLiteral("messageId"))},
                         {QStringLiteral("partId"), detail.metadata.value(QStringLiteral("id"))},
                         {QStringLiteral("field"), name},
                         {QStringLiteral("offset"), field.loaded ? field.nextOffset : 0},
                         {QStringLiteral("limit"), 4096}};
    if (!detail.revision.isEmpty()) command.insert(QStringLiteral("detailRevision"), detail.revision);
    const QJsonObject current = runtime_->session;
    runtime_->toolReads.insert(requestId, {current, command, key, runtime_->detailAccount,
                                           runtime_->detailAccountGeneration, runtime_->detailGeneration});
    field.pendingId = requestId;
    field.error.clear();
    QPointer<RemoteAgentPanel> guard(this);
    const auto fail = [guard, requestId](const QString& error) {
        if (!guard) return;
        const auto request = guard->runtime_->toolReads.find(requestId);
        if (request == guard->runtime_->toolReads.end()) return;
        const auto read = request.value();
        guard->runtime_->toolReads.erase(request);
        auto& field = guard->runtime_->toolDetails[read.key].fields[read.command.value(QStringLiteral("field")).toString()];
        if (field.pendingId != requestId) return;
        field.pendingId.clear();
        field.error = error.isEmpty() ? QStringLiteral("所属设备未确认详情读取，请重试") : error.left(240);
        guard->refreshToolDetailViews();
    };
    QTimer::singleShot(30'000, this, [fail] { fail({}); });
    // Read-only and deliberately outside the durable business outbox: reconnect
    // never replays a tool execution or silently downloads the remaining pages.
    runtime_->command(current, command, [fail](const QString& error) { if (!error.isEmpty()) fail(error); });
}

bool RemoteAgentPanel::receiveToolDetailResult(const QJsonObject& owner, const QJsonObject& payload) {
    // command_id is the common wire identity. Android owners do not publish the
    // Desktop-specific optional request_id projection for these read commands.
    const QString id = payload.value(QStringLiteral("command_id")).toString();
    const auto found = runtime_->toolReads.constFind(id);
    if (found == runtime_->toolReads.cend()) return false;
    const auto read = found.value();
    const auto result = payload.value(QStringLiteral("result")).toObject();
    if ((payload.contains(QStringLiteral("request_id")) &&
         payload.value(QStringLiteral("request_id")).toString() != id) ||
        sessionKey(owner) != sessionKey(read.owner) || sessionKey(runtime_->session) != sessionKey(read.owner) ||
        read.account != runtime_->detailAccount || read.accountGeneration != runtime_->detailAccountGeneration ||
        read.generation != runtime_->detailGeneration || !result.value(QStringLiteral("ok")).isBool() ||
        result.value(QStringLiteral("requestId")).toString() != id ||
        result.value(QStringLiteral("sessionId")) != read.owner.value(QStringLiteral("id"))) return true;
    for (const QString& name : {QStringLiteral("messageId"), QStringLiteral("partId"), QStringLiteral("field"), QStringLiteral("offset")})
        if (result.value(name) != read.command.value(name)) return true;
    auto detail = runtime_->toolDetails.find(read.key);
    if (detail == runtime_->toolDetails.end()) return true;
    const QString fieldName = read.command.value(QStringLiteral("field")).toString();
    auto& field = detail->fields[fieldName];
    if (field.pendingId != id) return true;
    if (!result.value(QStringLiteral("ok")).toBool()) {
        if (result.value(QStringLiteral("errorCode")) == QStringLiteral("busy")) {
            for (auto it = runtime_->toolReads.begin(); it != runtime_->toolReads.end();)
                it = it->key == read.key ? runtime_->toolReads.erase(it) : ++it;
            detail->fields.clear();
            detail->restartRequired = true;
            detail->busyIdentityPending = false;
        } else {
            runtime_->toolReads.remove(id);
            field.pendingId.clear();
            field.error = result.value(QStringLiteral("error")).toString(QStringLiteral("所属设备无法读取详情")).left(240);
        }
        refreshToolDetailViews();
        return true;
    }
    const QString revision = result.value(QStringLiteral("detailRevision")).toString();
    const QString state = result.value(QStringLiteral("state")).toString();
    const bool freshBusyIdentity = detail->busyIdentityPending && detail->revision.isEmpty() && detail->state.isEmpty();
    const bool knownState = state == QStringLiteral("pending") || state == QStringLiteral("running") ||
                            state == QStringLiteral("completed") || state == QStringLiteral("error") ||
                            state == QStringLiteral("canceled");
    if (!validDetailRevision(revision) || (!detail->revision.isEmpty() && revision != detail->revision) ||
        (!read.command.value(QStringLiteral("detailRevision")).toString().isEmpty() &&
         revision != read.command.value(QStringLiteral("detailRevision")).toString()) ||
        result.value(QStringLiteral("tool")) != detail->metadata.value(QStringLiteral("tool")) ||
        !knownState || (!freshBusyIdentity && state != detail->state) ||
        (detail->metadata.contains(QStringLiteral("callId")) &&
         result.value(QStringLiteral("callId")) != detail->metadata.value(QStringLiteral("callId")))) return true;
    const auto integer = [&](const QString& name) {
        const QJsonValue value = result.value(name);
        const qint64 number = value.toInteger(-1);
        return value.isDouble() && number >= 0 && double(number) == value.toDouble() ? number : qint64(-1);
    };
    const qint64 offset = integer(QStringLiteral("offset"));
    const qint64 bytes = integer(QStringLiteral("sourceBytesRead"));
    const qint64 total = integer(QStringLiteral("totalBytes"));
    const qint64 next = integer(QStringLiteral("nextOffset"));
    if (offset < 0 || bytes < 0 || bytes > 4096 || total < offset || bytes > total - offset || next < 0 ||
        !result.value(QStringLiteral("hasMore")).isBool() || !result.value(QStringLiteral("textIsValidUtf8")).isBool() ||
        !result.value(QStringLiteral("text")).isString()) return true;
    const bool more = result.value(QStringLiteral("hasMore")).toBool();
    if (more != (offset + bytes < total) || next != (more ? offset + bytes : 0) || (more && bytes == 0) ||
        (offset != 0 && (!field.loaded || field.nextOffset != offset || field.totalBytes != total))) return true;
    const QString text = result.value(QStringLiteral("text")).toString();
    const bool validUtf8 = result.value(QStringLiteral("textIsValidUtf8")).toBool();
    QByteArray raw = validUtf8 ? text.toUtf8()
                             : QByteArray::fromBase64(result.value(QStringLiteral("rawBase64")).toString().toLatin1(),
                                                      QByteArray::AbortOnBase64DecodingErrors);
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = decoder(raw);
    if (raw.size() != bytes || decoded != text || decoder.hasError() == validUtf8) return true;
    runtime_->toolReads.remove(id);
    field.pendingId.clear();
    field.error.clear();
    if (offset == 0) { field.text.clear(); field.raw.clear(); field.invalidUtf8 = false; }
    field.raw += raw;
    field.text += text;
    field.invalidUtf8 |= !validUtf8;
    field.loaded = true;
    field.totalBytes = total;
    field.nextOffset = next;
    field.hasMore = more;
    detail->revision = revision;
    detail->state = state;
    detail->busyIdentityPending = false;
    refreshToolDetailViews();
    return true;
}
void RemoteAgentPanel::setHistoryHandler(HistoryHandler handler) {
    runtime_->history = std::move(handler);
}
void RemoteAgentPanel::setDownloadHandler(DownloadHandler handler) {
    runtime_->download = std::move(handler);
}

void RemoteAgentPanel::setImageAccessProvider(ImageAccessProvider provider) {
    resetImagePreview();
    for (const auto& transfer : runtime_->imageTransfers) transfer->cancel();
    runtime_->imageTransfers.clear();
    runtime_->imageAccess = std::move(provider);
    ++runtime_->imageGeneration;
}

QJsonObject RemoteAgentPanel::session() const {
    return runtime_->session;
}

void RemoteAgentPanel::positionActionPanel() {
    runtime_->actionPanel->adjustSize();
    QPoint position = runtime_->more->mapToGlobal(
        QPoint(runtime_->more->width() - runtime_->actionPanel->width(), runtime_->more->height() + UiZoom::s(6)));
    QScreen* screen = QGuiApplication::screenAt(position);
    if (!screen) screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect available = screen->availableGeometry();
        position.setX(qBound(available.left(), position.x(), available.right() - runtime_->actionPanel->width() + 1));
        if (position.y() + runtime_->actionPanel->height() > available.bottom())
            position.setY(runtime_->more->mapToGlobal(QPoint(0, 0)).y() - runtime_->actionPanel->height() - UiZoom::s(6));
    }
    runtime_->actionPanel->move(position);
}

void RemoteAgentPanel::updateSessionModelControls() {
    const auto key = sessionKey(runtime_->session);
    const auto configuration = runtime_->modelConfigurations.value(key).value;
    const bool pending = runtime_->modelRequestByOwner.contains(key);
    const bool available = validModelOwner(runtime_->session) && runtime_->command &&
                           runtime_->session.value(QStringLiteral("online")).toBool() &&
                           !runtime_->session.value(QStringLiteral("busy")).toBool() &&
                           !configuration.isEmpty() && !pending;
    const auto model = configuration.value(QStringLiteral("model")).toString();
    const auto effort = configuration.value(QStringLiteral("reasoningEffort")).toString();
    // A confirmed local model remains selectable/readable, but its client does
    // not implement provider effort. Use the shared capability, not a UI guess.
    const auto capability = maiCloudModelInfo(model.toUtf8().toStdString());
    const bool effortAvailable = available && capability && capability->supportsReasoningEffort;
    runtime_->modelValue->setText(model.isEmpty()
                                     ? pending ? QStringLiteral("正在读取") : QStringLiteral("未确认")
                                     : model);
    runtime_->effortValue->setText(sessionEffortTitle(effort));
    runtime_->modelSection->setAccessibleName(QStringLiteral("主模型：") + runtime_->modelValue->text());
    runtime_->effortSection->setAccessibleName(QStringLiteral("思考强度：") + runtime_->effortValue->text());
    const QString detail = pending ? QStringLiteral("正在等待所属设备确认") : QString();
    runtime_->modelSection->setToolTip(detail);
    runtime_->effortSection->setToolTip(detail);
    runtime_->modelSection->setEnabled(available);
    runtime_->effortSection->setEnabled(effortAvailable);
    runtime_->more->setEnabled(validModelOwner(runtime_->session));
    for (auto* option : runtime_->modelButtons) {
        option->setChecked(option->property("model").toString() == model);
        option->setEnabled(available);
    }
    for (auto* option : runtime_->effortButtons) {
        option->setChecked(option->property("reasoningEffort").toString() == effort);
        option->setEnabled(effortAvailable);
    }
}

void RemoteAgentPanel::invalidateReplacedModelOwner(const QJsonObject& owner) {
    if (!validModelOwner(owner)) return;
    const auto replaced = [&](const QJsonObject& previous) {
        return sameModelRoom(previous, owner) && sessionKey(previous) != sessionKey(owner);
    };
    for (auto it = runtime_->modelRequests.begin(); it != runtime_->modelRequests.end();) {
        if (!replaced(it->owner)) { ++it; continue; }
        const auto key = sessionKey(it->owner);
        if (runtime_->modelRequestByOwner.value(key) == it.key()) runtime_->modelRequestByOwner.remove(key);
        it = runtime_->modelRequests.erase(it);
    }
    for (auto it = runtime_->modelConfigurations.begin(); it != runtime_->modelConfigurations.end();) {
        if (!replaced(it->owner)) { ++it; continue; }
        runtime_->modelConfigurationOrder.removeAll(it.key());
        it = runtime_->modelConfigurations.erase(it);
    }
}

void RemoteAgentPanel::requestSessionModel() {
    sendSessionModelCommand({{QStringLiteral("action"), QStringLiteral("get_session")}});
}

void RemoteAgentPanel::sendSessionModelCommand(const QJsonObject& requested) {
    const QJsonObject owner = runtime_->session;
    const auto key = sessionKey(owner);
    if (!runtime_->command || !validModelOwner(owner) || !owner.value(QStringLiteral("online")).toBool() ||
        runtime_->modelRequestByOwner.contains(key)) return;
    const bool changing = requested.value(QStringLiteral("action")) == QStringLiteral("set_model");
    if (changing && (owner.value(QStringLiteral("busy")).toBool() ||
                     !runtime_->modelConfigurations.contains(key))) return;
    if (runtime_->modelRequests.size() >= 128) return;
    QJsonObject command = requested;
    const auto requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    command.insert(QStringLiteral("requestId"), requestId);
    runtime_->modelRequests.insert(requestId, {owner, command});
    runtime_->modelRequestByOwner.insert(key, requestId);
    updateSessionModelControls();
    QPointer<RemoteAgentPanel> guard(this);
    const auto fail = [guard, key, requestId, changing](const QString& error) {
        if (!guard || !guard->runtime_->modelRequests.contains(requestId) ||
            guard->runtime_->modelRequestByOwner.value(key) != requestId) return;
        guard->runtime_->modelRequests.remove(requestId);
        guard->runtime_->modelRequestByOwner.remove(key);
        // A lost enqueue/ACK can leave a completed owner mutation. Require a new
        // explicit read before showing/selecting a model after this unknown result.
        if (changing) {
            guard->runtime_->modelConfigurations.remove(key);
            guard->runtime_->modelConfigurationOrder.removeAll(key);
        }
        guard->updateSessionModelControls();
        guard->showSessionModelFailure(key, error);
    };
    QTimer::singleShot(30'000, this, [fail] { fail(QStringLiteral("所属设备未确认会话模型，请重新读取后重试")); });
    runtime_->command(owner, command, [fail](const QString& error) { if (!error.isEmpty()) fail(error); });
}

bool RemoteAgentPanel::receiveSessionModelResult(const QJsonObject& owner, const QJsonObject& payload) {
    QString id = payload.value(QStringLiteral("request_id")).toString();
    if (id.isEmpty()) id = payload.value(QStringLiteral("command_id")).toString();
    const auto it = runtime_->modelRequests.constFind(id);
    if (it == runtime_->modelRequests.cend()) return false;
    const auto request = it.value();
    const auto key = sessionKey(request.owner);
    const auto commandId = payload.value(QStringLiteral("command_id")).toString();
    if (sessionKey(owner) != key || runtime_->modelRequestByOwner.value(key) != id ||
        (!commandId.isEmpty() && commandId != id)) return true;
    const auto result = payload.value(QStringLiteral("result")).toObject();
    const auto expectedSession = request.owner.value(QStringLiteral("id"));
    if (!result.value(QStringLiteral("ok")).isBool() ||
        (result.contains(QStringLiteral("session")) && result.value(QStringLiteral("session")) != expectedSession))
        return true;
    if (result.value(QStringLiteral("ok")).toBool()) {
        const auto model = result.value(QStringLiteral("model")).toString();
        const auto effort = result.value(QStringLiteral("reasoningEffort")).toString();
        const auto wire = result.value(QStringLiteral("wire")).toString();
        const bool local = model.startsWith(QStringLiteral("local:")) && model.size() > 6 && model.size() <= 256 &&
                           !QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")).match(model).hasMatch();
        const bool validEffort = effort == QStringLiteral("low") || effort == QStringLiteral("high") ||
                                 effort == QStringLiteral("max");
        const bool validWire = wire == QStringLiteral("chat_completions") || wire == QStringLiteral("responses") ||
                               wire == QStringLiteral("local");
        if (result.value(QStringLiteral("session")) != expectedSession ||
            (!sessionModelChoices().contains(model) && !local) ||
            !validEffort || !validWire)
            return true;
        if (request.command.value(QStringLiteral("action")) == QStringLiteral("set_model") &&
            (request.command.value(QStringLiteral("model")) != result.value(QStringLiteral("model")) ||
             (request.command.contains(QStringLiteral("reasoningEffort")) &&
              request.command.value(QStringLiteral("reasoningEffort")) != result.value(QStringLiteral("reasoningEffort")))))
            return true;
        const QJsonObject configuration{{QStringLiteral("session"), result.value(QStringLiteral("session"))},
                                         {QStringLiteral("model"), model},
                                         {QStringLiteral("reasoningEffort"), effort},
                                         {QStringLiteral("wire"), wire}};
        runtime_->modelConfigurations.insert(key, {request.owner, configuration});
        runtime_->modelConfigurationOrder.removeAll(key);
        runtime_->modelConfigurationOrder.append(key);
        while (runtime_->modelConfigurationOrder.size() > 128)
            runtime_->modelConfigurations.remove(runtime_->modelConfigurationOrder.takeFirst());
        if (sessionKey(runtime_->session) == key) {
            if (runtime_->modelStatusOwner == key && !runtime_->modelStatus.isEmpty())
                showStatus({});
            runtime_->modelStatus.clear();
            runtime_->modelStatusOwner.clear();
        }
    } else if (sessionKey(runtime_->session) == key) {
        showSessionModelFailure(key, result.value(QStringLiteral("error")).toString());
    }
    runtime_->modelRequests.remove(id);
    runtime_->modelRequestByOwner.remove(key);
    updateSessionModelControls();
    return true;
}

void RemoteAgentPanel::showSessionModelFailure(const QString& ownerKey, const QString& message) {
    if (ownerKey != sessionKey(runtime_->session)) return;
    const QString reason = message.isEmpty() ? QStringLiteral("所属设备拒绝读取或修改会话模型") : message.left(120);
    showStatus(reason, true);
    runtime_->modelStatus = reason;
    runtime_->modelStatusOwner = ownerKey;
}

void RemoteAgentPanel::openSession(const QJsonObject& remote) {
    const QString previous = sessionKey(runtime_->session);
    const QString next = sessionKey(remote);
    if (previous != next) {
        invalidateToolDetailRequests(true);
        runtime_->actionPanel->hide();
        runtime_->modelOptions->hide();
        runtime_->effortOptions->hide();
        runtime_->modelStatus.clear();
        runtime_->modelStatusOwner.clear();
        resetImagePreview();
        for (const auto& transfer : runtime_->imageTransfers) transfer->cancel();
        runtime_->imageTransfers.clear();
        ++runtime_->imageGeneration;
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
        runtime_->toolGroups.clear();
        runtime_->imageCards.clear();
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
    invalidateReplacedModelOwner(remote);
    runtime_->owners.insert(next, remote);
    dispatchQueued();
    updateControls();
    if (previous == next && !runtime_->messages.isEmpty()) {
        requestSessionModel();
        return;
    }
    showStatus(remote.value(QStringLiteral("online")).toBool() ? QStringLiteral("正在读取所属设备的会话…")
                                                               : QStringLiteral("所属设备离线；输入保留为草稿"));
    requestHistory({}, 10);
    requestSessionModel();
}

void RemoteAgentPanel::updateSession(const QJsonObject& remote) {
    invalidateReplacedModelOwner(remote);
    runtime_->owners.insert(sessionKey(remote), remote);
    if (sessionKey(remote) != sessionKey(runtime_->session)) {
        if (sameModelRoom(remote, runtime_->session)) {
            invalidateToolDetailRequests(false);
            runtime_->session.insert(QStringLiteral("online"), false);
            refreshToolDetailViews();
            runtime_->actionPanel->hide();
            updateControls();
            showStatus(QStringLiteral("所属设备登录已变化，请重新选择会话"), true);
        }
        return;
    }
    const bool wasOnline = runtime_->session.value(QStringLiteral("online")).toBool();
    runtime_->session = remote;
    runtime_->owners.insert(sessionKey(remote), remote);
    refreshToolDetailViews();
    updateControls();
    if (remote.value(QStringLiteral("busy")).toBool() && runtime_->awaitingResponse) runtime_->observedBusy = true;
    if (!remote.value(QStringLiteral("busy")).toBool() && runtime_->observedBusy) {
        runtime_->awaitingResponse = false;
        runtime_->observedBusy = false;
    }
    updateThinking();
    if (!remote.value(QStringLiteral("online")).toBool()) {
        invalidateToolDetailRequests(false);
        runtime_->historyInFlight = false;
        runtime_->historyTimeout.stop();
        runtime_->older->setEnabled(true);
        showStatus(QStringLiteral("所属设备离线；输入保留为草稿"));
    } else if (!wasOnline) {
        // WebSocket 断线期间的消息可能没收到；重新上线后补最近一页。
        refreshHistory();
        requestSessionModel();
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
    runtime_->historyToolGeneration = generation;
    runtime_->historyLiveTools.clear();
    QPointer<RemoteAgentPanel> guard(this);
    runtime_->history(current, before, limit, [guard, current, generation](const QString& error) {
        if (!guard || generation != guard->runtime_->historyGeneration ||
            sessionKey(current) != sessionKey(guard->runtime_->session) || error.isEmpty() ||
            !guard->runtime_->historyInFlight)
            return;
        guard->runtime_->historyInFlight = false;
        guard->runtime_->historyLiveTools.clear();
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
        if (receiveToolDetailResult(remote, payload)) return;
        if (receiveSessionModelResult(remote, payload)) return;
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
    const auto protectedTools = kind == QStringLiteral("history_page") && runtime_->historyInFlight &&
                                        runtime_->historyToolGeneration == runtime_->historyGeneration
                                    ? runtime_->historyLiveTools
                                    : QHash<QString, Runtime::HistoryLiveTool>{};
    if (kind == QStringLiteral("history_page")) {
        runtime_->historyInFlight = false;
        runtime_->historyLiveTools.clear();
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
        QJsonObject message = value.toObject();
        const QString id = message.value(QStringLiteral("id")).toString();
        const QString sourceId = message.value(QStringLiteral("source_message_id")).toString(id);
        QJsonArray parts = message.value(QStringLiteral("parts")).toArray();
        QSet<QString> presentTools;
        for (int index = 0; index < parts.size(); ++index) {
            const QJsonObject part = parts[index].toObject();
            const QString partId = part.value(QStringLiteral("id")).toString();
            if (part.value(QStringLiteral("kind")) != QStringLiteral("tool") || partId.isEmpty()) continue;
            const QString key = sourceId + QChar(0x1f) + partId;
            presentTools.insert(key);
            if (kind == QStringLiteral("history_page")) {
                if (protectedTools.contains(key)) parts[index] = protectedTools.value(key).part;
            } else if (runtime_->historyInFlight &&
                       runtime_->historyToolGeneration == runtime_->historyGeneration) {
                runtime_->historyLiveTools.insert(key, {sourceId, part});
            }
        }
        // Keep a newly appended live tool when the older snapshot predates its
        // creation. Other text parts and the original history cursor stay intact.
        if (kind == QStringLiteral("history_page"))
            for (auto it = protectedTools.cbegin(); it != protectedTools.cend(); ++it)
                if (it->messageId == sourceId && !presentTools.contains(it.key())) parts.append(it->part);
        message.insert(QStringLiteral("parts"), parts);
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
            (!message.value(QStringLiteral("active")).toBool() || hasVisibleActivity(message)))
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
    // A history/event receipt cannot acknowledge a model mutation. Preserve a
    // currently displayed model failure only for this exact owner; a newer
    // prompt status goes through showStatus and supersedes that marker.
    const bool modelFailure = runtime_->modelStatusOwner == sessionKey(runtime_->session) &&
                              !runtime_->modelStatus.isEmpty();
    if (!runtime_->pendingCommand && !modelFailure) showStatus({});
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
        QJsonArray tools;
        QJsonObject image;
        QString sourceMessageId;
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
        const QString sourceId = message.value(QStringLiteral("source_message_id")).toString(messageId);
        const QJsonArray parts = message.value(QStringLiteral("parts")).toArray();
        const bool projection = message.value(QStringLiteral("questionProjection")).toBool();
        const auto style = message.value(QStringLiteral("role")) == QStringLiteral("user")
                               ? MarkdownView::Style::Bubble
                               : MarkdownView::Style::Document;
        bool hasQuestion = false;
        for (const QJsonValue& value : parts) hasQuestion |= !questionPartText(value.toObject()).isEmpty();
        bool hasTools = false;
        bool hasImage = false;
        for (const QJsonValue& value : parts)
            hasTools |= value.toObject().value(QStringLiteral("kind")) == QStringLiteral("tool");
        for (const QJsonValue& value : parts)
            hasImage |= value.toObject().value(QStringLiteral("kind")) == QStringLiteral("image") &&
                        value.toObject().value(QStringLiteral("remoteImage")).isObject();
        if (!hasQuestion && !projection && !hasTools && !hasImage) {
            const QString text = messageText(message);
            if (!text.isEmpty()) segments.append({messageId, text, style});
            continue;
        }
        QJsonArray ordinary;
        QJsonArray tools;
        int run = 0;
        bool firstOrdinary = true;
        const auto partIdentity = [&](const QJsonObject& part, int index) {
            const QString id = part.value(QStringLiteral("id")).toString();
            return id.isEmpty() ? messageId + QStringLiteral(":part:") + QString::number(index) : id;
        };
        const auto flush = [&] {
            if (ordinary.isEmpty()) return;
            QJsonObject chunk = message;
            chunk.insert(QStringLiteral("parts"), ordinary);
            const QString text = messageText(chunk);
            const QString firstPartId = ordinary.first().toObject().value(QStringLiteral("id")).toString();
            const QString first = firstPartId.isEmpty() ? QString::number(run) : firstPartId;
            if (!text.isEmpty()) {
                // A question can arrive after the prose, or split its owner
                // into chronological fragments. Keep real part identities tied
                // to that source, so selections survive either update. Legacy
                // parts without IDs retain the fragment prefix: local run
                // numbers alone would collide across separate fragments.
                const QString prefix = firstPartId.isEmpty() ? messageId : sourceId;
                const QString id = firstOrdinary && messageId == sourceId && !projection
                                       ? sourceId : prefix + QStringLiteral(":segment:") + first;
                segments.append({id, text, style});
                firstOrdinary = false;
            }
            ordinary = {};
            ++run;
        };
        const auto flushTools = [&] {
            if (tools.isEmpty()) return;
            const QString id = QStringLiteral("remote-tool-group:") +
                               tools.first().toObject().value(QStringLiteral("id")).toString();
            const QString fingerprint = QString::fromUtf8(QJsonDocument(tools).toJson(QJsonDocument::Compact));
            segments.append({id, fingerprint, MarkdownView::Style::Document, tools});
            tools = {};
        };
        for (int index = 0; index < parts.size(); ++index) {
            const QJsonObject part = parts[index].toObject();
            const QString kind = part.value(QStringLiteral("kind")).toString();
            if (kind == QStringLiteral("reasoning")) continue;
            if (kind == QStringLiteral("image") && part.value(QStringLiteral("remoteImage")).isObject()) {
                flushTools();
                flush();
                const auto id = sourceId + QStringLiteral(":image:") + partIdentity(part, index);
                segments.append({id, QString::fromUtf8(QJsonDocument(part).toJson(QJsonDocument::Compact)),
                                 style, {}, part, sourceId});
                continue;
            }
            const QString question = questionPartText(part);
            if (question.isEmpty()) {
                if (projection) continue;
                if (kind == QStringLiteral("tool")) {
                    flush();
                    // Detail version and real source identity participate in the
                    // fingerprint even when tool/state have not changed.
                    QJsonObject tool{{QStringLiteral("id"), partIdentity(part, index)},
                                      {QStringLiteral("messageId"), sourceId},
                                      {QStringLiteral("tool"), part.value(QStringLiteral("tool"))},
                                      {QStringLiteral("state"), part.value(QStringLiteral("state"))}};
                    for (const QString& name : {QStringLiteral("callId"), QStringLiteral("detailRevision"),
                                                 QStringLiteral("detailsAvailable")})
                        if (part.contains(name)) tool.insert(name, part.value(name));
                    // New snapshots contain no full fields. Legacy inline data
                    // stays readable, with an explicit completeness boundary.
                    if (!part.value(QStringLiteral("detailsAvailable")).toBool())
                        for (const QString& name : toolDetailFields())
                            if (part.contains(name)) tool.insert(name, part.value(name));
                    if (part.value(QStringLiteral("tool")) == QStringLiteral("curl") &&
                        part.value(QStringLiteral("state")) == QStringLiteral("error")) {
                        const QString summary = part.value(QStringLiteral("error")).toString();
                        if (!summary.isEmpty() && summary.size() <= 256)
                            tool.insert(QStringLiteral("error"), summary);
                    }
                    syncToolDetailMetadata(tool);
                    tools.append(tool);
                } else {
                    flushTools();
                    // Media is a separate visible artifact, so a later tool or
                    // text run cannot be merged across it.
                    if (kind != QStringLiteral("text")) flush();
                    ordinary.append(part);
                    if (kind != QStringLiteral("text")) flush();
                }
                continue;
            }
            flushTools();
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
        flushTools();
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
        // MarkdownView defers widget destruction. A merged/split group must
        // stop painting and receiving input during this update immediately.
        if (RemoteToolGroupCard* group = runtime_->toolGroups.take(old)) group->hide();
        if (QLabel* image = runtime_->imageCards.take(old)) {
            image->hide();
            const auto identity = image->property("imageIdentity").toString();
            if (runtime_->previewIdentity == identity) resetImagePreview();
            if (const auto transfer = runtime_->imageTransfers.take(identity)) transfer->cancel();
        }
        runtime_->view->removeItem(old);
        runtime_->rendered.remove(old);
    }
    for (const Segment& segment : segments) {
        if (!segment.image.isEmpty()) {
            auto* card = runtime_->imageCards.value(segment.id, nullptr);
            if (!card) {
                card = new RemoteImageLabel;
                card->setObjectName(QStringLiteral("remoteSessionImage"));
                card->setAccessibleName(QStringLiteral("远端图片"));
                card->setToolTip(QStringLiteral("查看大图"));
                card->setFocusPolicy(Qt::StrongFocus);
                QPointer<RemoteAgentPanel> guard(this);
                QPointer<QLabel> label(card);
                static_cast<RemoteImageLabel*>(card)->activated = [guard, label] {
                    if (guard && label) guard->openImagePreview(label.data());
                };
                card->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                card->setMargin(UiZoom::s(10));
                card->setMinimumHeight(UiZoom::s(42));
                if (segment.style == MarkdownView::Style::Bubble)
                    card->setStyleSheet(QStringLiteral("background:#edf2f7;border-radius:12px;color:#667085;"));
                else card->setStyleSheet(QStringLiteral("color:#667085;"));
                runtime_->view->addWidget(segment.id, card);
                runtime_->imageCards.insert(segment.id, card);
            }
            renderImage(card, segment.sourceMessageId, segment.image);
        } else if (!segment.tools.isEmpty()) {
            RemoteToolGroupCard* group = runtime_->toolGroups.value(segment.id, nullptr);
            if (!group) {
                group = new RemoteToolGroupCard(segment.id);
                group->activated = [this](const QJsonObject& tool, const QString& field) {
                    activateToolDetail(tool, field);
                };
                group->presentation = [this](const QJsonObject& tool) { return toolDetailPresentation(tool); };
                runtime_->view->addWidget(segment.id, group);
                runtime_->toolGroups.insert(segment.id, group);
            }
            if (runtime_->rendered.value(segment.id) != segment.text) group->setTools(segment.tools);
        } else if (!runtime_->rendered.contains(segment.id))
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
    const auto refresh = runtime_->toolRefresh;
    runtime_->toolRefresh.clear();
    for (const QString& key : refresh)
        if (runtime_->toolDetails.value(key).expanded)
            for (const QString& field : toolDetailFields()) requestToolDetailPage(key, field);
    refreshToolDetailViews();
    updateThinking();
    if (followedLatest) runtime_->view->scrollToBottom();
}

void RemoteAgentPanel::renderImage(QLabel* card, const QString& messageId, const QJsonObject& part) {
    const auto image = part.value(QStringLiteral("remoteImage")).toObject();
    const auto state = image.value(QStringLiteral("state")).toString();
    const auto access = runtime_->imageAccess ? runtime_->imageAccess() : DesktopVideoCircleAccess{};
    if (runtime_->imageAccount != access.accountId) {
        resetImagePreview();
        for (const auto& transfer : runtime_->imageTransfers) transfer->cancel();
        runtime_->imageTransfers.clear();
        runtime_->imageAccount = access.accountId;
        ++runtime_->imageGeneration;
    }
    const auto owner = runtime_->session;
    const auto partId = part.value(QStringLiteral("id")).toString();
    const QString identity = access.accountId + QLatin1Char(':') + sessionKey(owner) + QLatin1Char(':') +
        messageId + QLatin1Char(':') + partId + QLatin1Char(':') +
        QString::fromUtf8(QJsonDocument(image).toJson(QJsonDocument::Compact));
    if (card->property("imageIdentity").toString() == identity) return;
    if (runtime_->previewIdentity == card->property("imageIdentity").toString()) resetImagePreview();
    if (const auto transfer = runtime_->imageTransfers.take(card->property("imageIdentity").toString()))
        transfer->cancel();
    card->setProperty("imageIdentity", identity);
    card->setProperty("imageState", state);
    card->setProperty("imageAccount", access.accountId);
    card->setProperty("imageCachePath", QString());
    card->setCursor(Qt::ArrowCursor);
    card->setPixmap({});
    card->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    if (state != QStringLiteral("ready") &&
        !(state == QStringLiteral("expired") && !image.value(QStringLiteral("attachment_id")).toString().isEmpty())) {
        card->setText(state == QStringLiteral("pending") ? QStringLiteral("图片正在发布…") :
                      state == QStringLiteral("expired") ? QStringLiteral("图片已过期") :
                      QStringLiteral("图片发布失败"));
        return;
    }
    if (!access.isValid()) {
        card->setText(QStringLiteral("登录后查看图片"));
        return;
    }
    card->setText(QStringLiteral("图片加载中…"));
    const DesktopSessionRoom room{owner.value(QStringLiteral("device_slot")).toString(),
        image.value(QStringLiteral("owner_login_session_id")).toString(), owner.value(QStringLiteral("id")).toString()};
    struct ImageResult { QImage pixels; QString error; QString path; };
    const auto generation = runtime_->imageGeneration;
    const auto cancellation = std::make_shared<ImageCancellation>();
    runtime_->imageTransfers.insert(identity, cancellation);
    QPointer<RemoteAgentPanel> guard(this);
    QPointer<QLabel> label(card);
    auto* watcher = new QFutureWatcher<ImageResult>(this);
    connect(watcher, &QFutureWatcher<ImageResult>::finished, this,
        [guard, label, watcher, generation, identity, access, owner, cancellation] {
            const auto result = watcher->result();
            watcher->deleteLater();
            if (guard && guard->runtime_->imageTransfers.value(identity) == cancellation)
                guard->runtime_->imageTransfers.remove(identity);
            if (!guard || !label || guard->runtime_->imageGeneration != generation ||
                label->property("imageIdentity").toString() != identity ||
                sessionKey(guard->runtime_->session) != sessionKey(owner) || !guard->runtime_->imageAccess ||
                guard->runtime_->imageAccess().accountId != access.accountId) return;
            if (!result.error.isEmpty()) {
                label->setText(result.error);
                label->setProperty("imageState", result.error == QStringLiteral("图片已过期")
                    ? QStringLiteral("expired") : QStringLiteral("failed"));
                return;
            }
            label->setText({});
            const auto pixmap = QPixmap::fromImage(result.pixels);
            label->setMinimumHeight(0);
            label->setPixmap(pixmap);
            label->setMaximumSize(pixmap.width() + label->margin() * 2, pixmap.height() + label->margin() * 2);
            label->setProperty("imageState", QStringLiteral("ready"));
            label->setProperty("imageCachePath", result.path);
            label->setCursor(Qt::PointingHandCursor);
            label->updateGeometry();
            // MarkdownView owns this row's geometry without a QLayout. Notify
            // its widget filter explicitly; updateGeometry alone informs the
            // viewport and leaves the former pending-label height in place.
            QEvent layout(QEvent::LayoutRequest);
            QCoreApplication::sendEvent(label.data(), &layout);
        });
    watcher->setFuture(QtConcurrent::run([access, room, messageId, partId, image, cancellation] {
        const auto downloaded = DesktopSessionAttachments::downloadTemporaryImage(
            access, room, messageId, partId, image, cancellation->transfer);
        if (!downloaded.ok()) return ImageResult{{}, downloaded.error};
        MaiImageReadOptions options{};
        options.max_encoded_bytes = 20'000'000;
        options.max_source_pixels = 100'000'000;
        options.max_output_width = 480;
        options.max_output_height = 360;
        options.is_canceled = [](void* context) {
            return static_cast<ImageCancellation*>(context)->canceled.load() ? 1 : 0;
        };
        options.context = cancellation.get();
        const auto decoded = desktopReadRasterFile(downloaded.path, options);
        return decoded.image.isNull() ? ImageResult{{}, QStringLiteral("图片无法解码")}
                                     : ImageResult{decoded.image, {}, downloaded.path};
    }));
}

void RemoteAgentPanel::resetImagePreview() {
    ++runtime_->previewGeneration;
    if (runtime_->previewCancellation) runtime_->previewCancellation->cancel();
    runtime_->previewCancellation.reset();
    runtime_->previewIdentity.clear();
    if (runtime_->imagePreview) runtime_->imagePreview->close();
    runtime_->imagePreview.clear();
}

void RemoteAgentPanel::openImagePreview(QLabel* card) {
    const auto access = runtime_->imageAccess ? runtime_->imageAccess() : DesktopVideoCircleAccess{};
    const auto path = card->property("imageCachePath").toString();
    if (!access.isValid() || access.accountId != card->property("imageAccount").toString() ||
        path.isEmpty() || card->property("imageState").toString() != QStringLiteral("ready")) return;
    resetImagePreview();
    const auto identity = card->property("imageIdentity").toString();
    const auto owner = sessionKey(runtime_->session);
    const auto generation = runtime_->previewGeneration;
    const auto cancellation = std::make_shared<ImageCancellation>();
    runtime_->previewCancellation = cancellation;
    runtime_->previewIdentity = identity;
    QPointer<RemoteAgentPanel> guard(this);
    QPointer<QLabel> label(card);
    auto* watcher = new QFutureWatcher<DesktopRasterImageResult>(this);
    connect(watcher, &QFutureWatcher<DesktopRasterImageResult>::finished, this,
        [guard, label, watcher, cancellation, generation, identity, owner, access] {
            const auto decoded = watcher->result();
            watcher->deleteLater();
            if (!guard || !label || cancellation->canceled || guard->runtime_->previewGeneration != generation ||
                guard->runtime_->previewIdentity != identity || sessionKey(guard->runtime_->session) != owner ||
                label->property("imageIdentity").toString() != identity || !guard->runtime_->imageAccess ||
                guard->runtime_->imageAccess().accountId != access.accountId) return;
            guard->runtime_->previewCancellation.reset();
            if (decoded.image.isNull()) {
                guard->showStatus(QStringLiteral("图片无法打开"), true);
                return;
            }
            // Preview uses only the previously committed local original and
            // the ordinary codec. It cannot renew a URL or trigger an upload.
            auto* dialog = new ImagePreviewDialog(QPixmap::fromImage(decoded.image), guard.data());
            dialog->setObjectName(QStringLiteral("remoteSessionImagePreview"));
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setModal(false);
            dialog->setWindowModality(Qt::NonModal);
            guard->runtime_->imagePreview = dialog;
            dialog->show();
        });
    watcher->setFuture(QtConcurrent::run([path, cancellation] {
        MaiImageReadOptions options{};
        options.max_encoded_bytes = 20'000'000;
        options.max_source_pixels = 100'000'000;
        options.max_output_width = options.max_output_height = 4096;
        options.is_canceled = [](void* context) {
            return static_cast<ImageCancellation*>(context)->canceled.load() ? 1 : 0;
        };
        options.context = cancellation.get();
        return desktopReadRasterFile(path, options);
    }));
}

void RemoteAgentPanel::updateThinking() {
    const QJsonObject last = runtime_->messages.isEmpty() ? QJsonObject{} : runtime_->messages.last();
    const bool assistantActive = last.value(QStringLiteral("role")).toString() == QStringLiteral("assistant") &&
                                 last.value(QStringLiteral("active")).toBool();
    const bool visibleAnswer = assistantActive && hasVisibleActivity(last);
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
    updateSessionModelControls();
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
    // Other actions own their own progress/error text. Never resurrect an older
    // model error over a prompt, attachment, reconnect or replacement status.
    runtime_->modelStatus.clear();
    runtime_->modelStatusOwner.clear();
    runtime_->status->setText(message);
    runtime_->status->setVisible(!message.isEmpty());
    runtime_->status->setStyleSheet(failure ? QStringLiteral("color:#b42318;background:#fff5f3;")
                                            : QStringLiteral("color:#667085;background:transparent;font-size:12px;"));
}
