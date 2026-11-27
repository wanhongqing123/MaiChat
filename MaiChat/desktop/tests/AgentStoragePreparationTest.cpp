// Slow/failing/canceled opens must not block the GUI or start an empty agent.
#include "agent/DesktopAgentStoragePreparation.h"
#include "agent/AgentController.h"
#include "MaiMemoryStore.h"

#include <QApplication>
#include <QFile>
#include <QProgressDialog>
#include <QPushButton>
#include <QThread>
#include <QTemporaryDir>
#include "../../../MaiAgent/third_party/sqlite/sqlite3.h"
#include "../../../MaiAgent/tests/support/MaiLegacySessionFixture.h"
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
    // 旧连接保持打开且 WAL 有内容；新版启动不得碰旧 writer，也不得自动导入。
    QTemporaryDir legacyDirectory;
    check(legacyDirectory.isValid(), "isolated legacy account directory");
    if (legacyDirectory.isValid()) {
        const QString legacyPath = legacyDirectory.filePath(QStringLiteral("agent.db"));
        sqlite3* legacy = nullptr;
        check(sqlite3_open(legacyPath.toUtf8().constData(), &legacy) == SQLITE_OK,
              "create owned legacy database");
        check(sqlite3_exec(legacy, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr) == SQLITE_OK,
              "legacy fixture uses a live WAL connection");
        check(sqlite3_exec(legacy, kMaiLegacySessionFrozenV15Sql, nullptr, nullptr, nullptr) == SQLITE_OK,
              "populate frozen legacy history");
        const QStringList originals{legacyPath, legacyPath + QStringLiteral("-wal"),
                                    legacyPath + QStringLiteral("-shm")};
        QList<QByteArray> before;
        for (const QString& path : originals) {
            QFile file(path);
            check(file.open(QIODevice::ReadOnly), "legacy main and sidecars exist");
            before.append(file.readAll());
        }
        auto fresh = prepareDesktopAgentStorage(legacyPath);
        check(fresh.store != nullptr, "new session startup must ignore the live legacy database");
        if (fresh.store) {
            check(fresh.store->listSessions().empty(), "legacy session history must not be imported");
            MaiSession session;
            session.id = "new-format-only-session";
            fresh.store->putSession(session);
            check(!fresh.store->lastWriteError(), "new-format history remains writable");
            fresh.store.reset();
            auto reopened = prepareDesktopAgentStorage(legacyPath);
            MaiSession restored;
            check(reopened.store && reopened.store->getSession(session.id, restored),
                  "new-format records survive reopen without importing legacy history");
            check(reopened.store && reopened.store->listSessions().size() == 1,
                  "reopen contains only the new-format session");
        }
        for (qsizetype i = 0; i < originals.size(); ++i) {
            QFile file(originals[i]);
            check(file.open(QIODevice::ReadOnly) && file.readAll() == before[i],
                  "legacy main WAL and SHM remain byte-identical during new startup");
        }
        sqlite3_close(legacy);
    }
    if (app.arguments().contains(QStringLiteral("fresh-start-only")))
        return failures ? 1 : 0;
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
