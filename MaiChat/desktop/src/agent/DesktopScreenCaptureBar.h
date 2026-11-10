// Creation Center projection of the process-wide local screenshot lifetime.
#pragma once

#include <QWidget>

class DesktopScreenCaptureControl;
class QLabel;
class QPushButton;

// The borrowed control must outlive this widget. This bar owns no Session or
// remote-owner state; keep it in the common Creation Center layout so rebuilding
// a local/remote conversation page cannot hide the local capture stop entry.
class DesktopScreenCaptureBar final : public QWidget {
    Q_OBJECT

public:
    explicit DesktopScreenCaptureBar(DesktopScreenCaptureControl& control, QWidget* parent = nullptr);

private:
    void refreshStatus();

    DesktopScreenCaptureControl& control_;
    QLabel* label_ = nullptr;
    QPushButton* stopButton_ = nullptr;
};
