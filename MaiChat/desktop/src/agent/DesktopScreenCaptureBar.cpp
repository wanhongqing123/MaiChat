#include "agent/DesktopScreenCaptureBar.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

#include "agent/DesktopScreenCaptureControl.h"
#include "ui/UiZoom.h"

DesktopScreenCaptureBar::DesktopScreenCaptureBar(DesktopScreenCaptureControl& control, QWidget* parent)
    : QWidget(parent), control_(control) {
    setObjectName(QStringLiteral("agentScreenCaptureStatus"));
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setMinimumWidth(0);
    UiZoom::setStyle(this,
        QStringLiteral("QWidget#agentScreenCaptureStatus{background:#fff7ed;"
                       "border-bottom:1px solid #fed7aa;}"));
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(UiZoom::s(16), UiZoom::s(8), UiZoom::s(16), UiZoom::s(8));
    row->setSpacing(UiZoom::s(12));

    label_ = new QLabel(this);
    label_->setObjectName(QStringLiteral("agentScreenCaptureLabel"));
    label_->setWordWrap(true);
    label_->setTextFormat(Qt::PlainText);
    // Errors may contain a long native reason. Wrap the label instead of
    // extending the common bar beyond the window or pushing its stop offscreen.
    label_->setMinimumWidth(0);
    label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    label_->setStyleSheet(QStringLiteral("background:transparent;color:#9a3412;border:0;"));
    UiZoom::setFont(label_, UiZoom::TextRole::Metadata, label_->font());
    row->addWidget(label_, 1);

    stopButton_ = new QPushButton(this);
    stopButton_->setObjectName(QStringLiteral("agentStopScreenCapture"));
    stopButton_->setAccessibleName(QStringLiteral("停止本机屏幕采集"));
    stopButton_->setToolTip(QStringLiteral("结束本机所有会话共用的采集会话。新的截图请求可再次开启。"));
    stopButton_->setCursor(Qt::PointingHandCursor);
    UiZoom::setFont(stopButton_, UiZoom::TextRole::Metadata, stopButton_->font());
    UiZoom::setStyle(stopButton_,
        QStringLiteral("QPushButton{color:#9a3412;background:#ffffff;border:1px solid #fdba74;"
                       "border-radius:7px;padding:6px 12px;}"
                       "QPushButton:hover{background:#ffedd5;}"
                       "QPushButton:disabled{color:#a8a29e;border-color:#e7d9c9;}"));
    row->addWidget(stopButton_);

    // Local native capture is process-wide, including when the center displays
    // a remote conversation. Never infer capture ownership from the current page.
    connect(stopButton_, &QPushButton::clicked, &control_, &DesktopScreenCaptureControl::requestStop);
    connect(&control_, &DesktopScreenCaptureControl::stateChanged, this,
            &DesktopScreenCaptureBar::refreshStatus);
    refreshStatus();
}

void DesktopScreenCaptureBar::refreshStatus() {
    const MaiScreenshotCaptureState state = control_.captureState();
    const bool stopping = control_.isStopping();
    const QString error = control_.errorMessage();
    setVisible(state.workerRunning || state.capturing || stopping || !error.isEmpty());
    stopButton_->setEnabled(!stopping);
    stopButton_->setText(error.isEmpty() ? QStringLiteral("停止采集") : QStringLiteral("重试停止"));
    if (stopping) {
        label_->setText(QStringLiteral("正在停止本机屏幕采集…"));
    } else if (!error.isEmpty()) {
        label_->setText(QStringLiteral("本机屏幕采集状态异常：%1").arg(error));
    } else if (state.capturing) {
        label_->setText(QStringLiteral("本机屏幕采集中 · 所有会话共用"));
    } else {
        label_->setText(QStringLiteral("本机屏幕采集待命 · 所有会话共用"));
    }
}
