// Ordinary UI bridge for the process-wide native screenshot capture lifetime.
#pragma once

#include <QObject>
#include <QString>

#include <functional>

#include "MaiScreenshot.h"

class QTimer;

// Construct, call and destroy this object on its GUI thread. State polling only
// reads the native backend every 500ms; it never starts capture. Stops run off the GUI thread
// and affect the process-wide screenshot backend, independently of TRTC sharing.
// A later screenshot may restart capture; this is not a permission or input gate.
class DesktopScreenCaptureControl final : public QObject {
    Q_OBJECT

public:
    struct Backend {
        // state runs on the GUI thread and must return promptly. stop runs in a
        // worker and may block. Both functions must remain safe for their copied
        // lifetime; borrowed resources must outlive an in-flight stop even when
        // the control is destroyed. Prefer owning shared captures for injections.
        std::function<MaiScreenshotCaptureState()> state;
        std::function<MaiError()> stop;
    };

    explicit DesktopScreenCaptureControl(QObject* parent = nullptr);
    explicit DesktopScreenCaptureControl(Backend backend, QObject* parent = nullptr);
    ~DesktopScreenCaptureControl() override;

    MaiScreenshotCaptureState captureState() const;
    bool isStopping() const;
    QString errorMessage() const;

public slots:
    void refresh();
    // Coalesces requests while a stop is in flight. Completion reads actual state
    // before notifying observers, retaining failures for an explicit retry.
    void requestStop();

signals:
    void stateChanged();

private:
    void readState();
    void completeStop(const MaiError& error);

    Backend backend_;
    MaiScreenshotCaptureState state_;
    QString error_;
    QString stopError_;
    QTimer* pollTimer_ = nullptr;
    bool stopping_ = false;
};
