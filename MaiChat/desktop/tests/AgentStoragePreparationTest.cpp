// Slow/failing/canceled opens must not block the GUI or start an empty agent.
#include "agent/DesktopAgentStoragePreparation.h"
#include "agent/AgentController.h"
#include "MaiMemoryStore.h"

#include <QApplication>
#include <QProgressDialog>
#include <QPushButton>
#include <QThread>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>

namespace {
bool progressVisible() {
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (qobject_cast<QProgressDialog*>(widget) && widget->isVisible()) {
            static bool captured = false;
            const QString capture = qEnvironmentVariable("MAICHAT_STORAGE_PREPARATION_CAPTURE");
            if (!captured && !capture.isEmpty()) captured = widget->grab().save(capture);
            return true;
        }
    }
    return false;
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    int failures = 0;
    auto check = [&failures](bool condition, const char* message) {
        if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
    };
    int ticks = 0;
    bool visible = false;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        ++ticks;
        visible = visible || progressVisible();
    });
    heartbeat.start();
    MaiSessionStore* openedPointer = nullptr;
    bool workerThread = false;
    auto result = prepareDesktopAgentStorage(QStringLiteral("unused"), nullptr,
        [&](const QString&, const MaiSqliteStoreOpenOptions&) {
            workerThread = QThread::currentThread() != app.thread();
            QThread::msleep(150);
            auto store = makeMaiMemoryStore();
            openedPointer = store.get();
            return MaiResult<std::unique_ptr<MaiSessionStore>>(std::move(store));
        }, 10);
    check(ticks >= 5 && visible && workerThread, "slow open must keep GUI ticks and visible progress");
    check(result.store.get() == openedPointer && result.store && result.error.isEmpty(),
          "transfer the exact opened backend");
    if (result.store) {
        AgentController controller(AgentController::ModelConfig{}, QString(),
            std::move(result.store), AgentController::ToolRegistrar{});
        MaiSession session;
        session.id = "prepared-store-identity";
        openedPointer->putSession(session);
        MaiSession restored;
        check(controller.agent().getSession(session.id, restored) && restored.id == session.id,
              "MaiAgent must read directly from the same prepared backend");
    }
    visible = false;
    auto quick = prepareDesktopAgentStorage(QStringLiteral("unused"), nullptr,
        [](const QString&, const MaiSqliteStoreOpenOptions&) {
            return MaiResult<std::unique_ptr<MaiSessionStore>>(makeMaiMemoryStore());
        }, 500);
    check(quick.store && !visible, "quick open must not flash progress");
    auto failed = prepareDesktopAgentStorage(QStringLiteral("unused"), nullptr,
        [](const QString&, const MaiSqliteStoreOpenOptions&) {
            return MaiResult<std::unique_ptr<MaiSessionStore>>(MaiErrorCode::Internal, "disk unavailable");
        });
    check(!failed.store && failed.error.contains(QStringLiteral("disk unavailable")),
          "storage failure must remain a failure without an empty fallback");
    bool emptyPathOpened = false;
    auto noPath = prepareDesktopAgentStorage(QString(), nullptr,
        [&](const QString&, const MaiSqliteStoreOpenOptions&) {
            emptyPathOpened = true;
            return MaiResult<std::unique_ptr<MaiSessionStore>>(makeMaiMemoryStore());
        });
    check(!noPath.store && !noPath.error.isEmpty() && !emptyPathOpened,
          "missing account storage path must not create a temporary backend");
    QTimer cancel;
    bool closeViaWindow = false;
    bool cancelPendingVisible = false;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] {
        if (closeViaWindow && !cancel.isActive()) cancelPendingVisible = cancelPendingVisible || progressVisible();
    });
    cancel.setInterval(5);
    QObject::connect(&cancel, &QTimer::timeout, [&] {
        for (QWidget* widget : QApplication::topLevelWidgets()) {
            if (auto* progress = qobject_cast<QProgressDialog*>(widget); progress && progress->isVisible()) {
                cancel.stop();
                if (closeViaWindow) progress->close();
                else if (auto* button = progress->findChild<QPushButton*>()) button->click();
            }
        }
    });
    cancel.start();
    auto canceled = prepareDesktopAgentStorage(QStringLiteral("unused"), nullptr,
        [](const QString&, const MaiSqliteStoreOpenOptions& options) {
            for (int i = 0; i < 200; ++i) {
                if (options.isCanceled && options.isCanceled())
                    return MaiResult<std::unique_ptr<MaiSessionStore>>(MaiErrorCode::Canceled, "canceled");
                QThread::msleep(5);
            }
            return MaiResult<std::unique_ptr<MaiSessionStore>>(makeMaiMemoryStore());
        }, 5);
    check(canceled.canceled && !canceled.store, "cancel must propagate into worker and prevent startup");
    closeViaWindow = true;
    cancel.start();
    auto cancelAtReady = prepareDesktopAgentStorage(QStringLiteral("unused"), nullptr,
        [](const QString&, const MaiSqliteStoreOpenOptions&) {
            QThread::msleep(100);
            return MaiResult<std::unique_ptr<MaiSessionStore>>(makeMaiMemoryStore());
        }, 5);
    cancel.stop();
    check(cancelAtReady.canceled && !cancelAtReady.store,
          "cancel racing ready must discard the prepared object and prevent startup");
    check(cancelPendingVisible, "closing progress must keep cancellation feedback visible until worker is safe");
    QTemporaryDir persistentDirectory;
    check(persistentDirectory.isValid(), "isolated persistent fixture directory");
    if (persistentDirectory.isValid()) {
        const QString path = persistentDirectory.filePath(QStringLiteral("agent.db"));
        auto first = prepareDesktopAgentStorage(path);
        check(first.store != nullptr, "ordinary startup opener must prepare the actual persistent store");
        if (first.store) {
            MaiSession session;
            session.id = "persisted-startup-session";
            first.store->putSession(session);
            first.store.reset();
            auto reopened = prepareDesktopAgentStorage(path);
            MaiSession restored;
            check(reopened.store && reopened.store->getSession(session.id, restored),
                  "ordinary startup opener must restore existing history");
        }
    }
    std::cout << "AgentStoragePreparationTest failures=" << failures << '\n';
    return failures ? 1 : 0;
}
