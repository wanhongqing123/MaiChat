// Shared tool metadata presentation; arguments, output and permission handling
// remain owned by each panel's detail widgets.
#include "agent/AgentToolGroupWidget.h"

#include <QPushButton>
#include <QVBoxLayout>

#include "ui/UiZoom.h"

QString agentToolStateText(const QString& state) {
    const QString value = state.trimmed().toLower();
    if (value == QStringLiteral("pending")) return QStringLiteral("等待授权");
    if (value == QStringLiteral("running")) return QStringLiteral("运行中");
    if (value == QStringLiteral("completed")) return QStringLiteral("已完成");
    if (value == QStringLiteral("error")) return QStringLiteral("失败");
    if (value == QStringLiteral("canceled")) return QStringLiteral("已取消");
    return QStringLiteral("状态未知");
}

QString agentToolGroupTitle(const QStringList& states) {
    bool running = false;
    bool pending = false;
    bool failed = false;
    bool canceled = false;
    bool unknown = false;
    for (const QString& state : states) {
        const QString value = state.trimmed().toLower();
        running |= value == QStringLiteral("running");
        pending |= value == QStringLiteral("pending");
        failed |= value == QStringLiteral("error");
        canceled |= value == QStringLiteral("canceled");
        unknown |= value != QStringLiteral("running") && value != QStringLiteral("pending") &&
                   value != QStringLiteral("error") && value != QStringLiteral("canceled") &&
                   value != QStringLiteral("completed");
    }
    // Keep an incomplete or unrecognized remote state from reporting success.
    // Running/approval take precedence while any member still needs attention.
    const QString state = running    ? QStringLiteral("运行中")
                          : pending  ? QStringLiteral("等待授权")
                          : failed   ? QStringLiteral("有失败")
                          : canceled ? QStringLiteral("部分取消")
                          : unknown  ? QStringLiteral("状态未知")
                                     : QStringLiteral("已完成");
    return QStringLiteral("工具调用 %1 次 · %2").arg(states.size()).arg(state);
}

AgentToolGroupWidget::AgentToolGroupWidget(const QString& id, QWidget* parent) : QFrame(parent), id_(id) {
    setObjectName(QStringLiteral("agentToolGroup"));
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(UiZoom::s(4));
    summary_ = new QPushButton(this);
    summary_->setObjectName(QStringLiteral("agentToolGroupDisclosure"));
    summary_->setCursor(Qt::PointingHandCursor);
    // This style belongs to the button itself. Remote panels may give it a
    // different object name without losing the shared metadata typography.
    UiZoom::setStyle(summary_,
                     QStringLiteral("QPushButton{background:transparent;border:none;"
                                    "color:#667085;font-size: @metadata;text-align:left;padding:3px 0;}"
                                    "QPushButton:hover{color:#0b67b7;}"));
    column->addWidget(summary_);
    details_ = new QWidget(this);
    detailsLayout_ = new QVBoxLayout(details_);
    detailsLayout_->setContentsMargins(UiZoom::s(12), 0, 0, 0);
    detailsLayout_->setSpacing(UiZoom::s(2));
    details_->hide();
    column->addWidget(details_);
    QObject::connect(summary_, &QPushButton::clicked, this, [this] {
        disclosureClicked();
        setExpanded(!isExpanded());
    });
}

const QString& AgentToolGroupWidget::id() const {
    return id_;
}

void AgentToolGroupWidget::addDetail(QWidget* detail) {
    detailsLayout_->addWidget(detail);
}

void AgentToolGroupWidget::removeDetail(QWidget* detail) {
    detailsLayout_->removeWidget(detail);
    detail->hide();
    detail->deleteLater();
}

int AgentToolGroupWidget::detailCount() const {
    return detailsLayout_->count();
}

void AgentToolGroupWidget::setExpanded(bool expanded) {
    // Store the user's disclosure state independently of ancestor visibility;
    // retained conversation pages can be hidden without collapsing their rows.
    expanded_ = expanded;
    details_->setVisible(expanded_);
    refreshTitle();
}

bool AgentToolGroupWidget::isExpanded() const {
    return expanded_;
}

void AgentToolGroupWidget::setTitle(const QString& title) {
    title_ = title;
    refreshTitle();
}

QPushButton* AgentToolGroupWidget::disclosureButton() const {
    return summary_;
}

void AgentToolGroupWidget::disclosureClicked() {}

void AgentToolGroupWidget::refreshTitle() {
    summary_->setText(QStringLiteral("⌘  %1  %2").arg(title_, expanded_ ? QStringLiteral("⌄") : QStringLiteral("›")));
}
