#include "agent/DesktopAgentStoragePreparation.h"
#include "agent/DesktopSessionDatabaseUseCheck.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>

QString accountAgentDirectory(const QString& ownerUserId) {
    if (ownerUserId.isEmpty()) return {};
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (root.isEmpty()) root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) return {};
    const QString account = QString::fromLatin1(
        QCryptographicHash::hash(ownerUserId.toUtf8(), QCryptographicHash::Sha256).toHex());
    return QDir(root).filePath(QStringLiteral("Agent/Accounts/%1").arg(account));
}

QString agentDatabasePath(const QString& ownerUserId) {
    const QString root = accountAgentDirectory(ownerUserId);
    // Directory creation belongs to the opening worker, including failures.
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("agent.db"));
}

DesktopAgentStoragePreparationResult prepareDesktopAgentStorage(
    const QString& databasePath, QWidget* parent, DesktopAgentStorageOpener opener, int progressDelayMs) {
    if (databasePath.isEmpty()) return {nullptr, QStringLiteral("无法确定本地会话保存位置。"), false};
    if (!opener)
        opener = [](const QString& path, const MaiSqliteStoreOpenOptions& options)
            -> MaiResult<std::unique_ptr<MaiSessionStore>> {
            const QByteArray utf8 = path.toUtf8();
            const auto inUse = checkDesktopSessionDatabaseUse(MaiFilePath::fromUtf8(
                std::string(utf8.constData(), static_cast<size_t>(utf8.size()))));
            if (inUse) return inUse;
            return makeMaiSqliteStore(
                std::string(utf8.constData(), static_cast<size_t>(utf8.size())), options);
        };

    QProgressDialog progress(QStringLiteral("正在准备本地会话…"), QStringLiteral("取消"), 0, 0, parent);
    progress.setObjectName(QStringLiteral("agentStoragePreparation"));
    progress.setAttribute(Qt::WA_QuitOnClose, false);
    progress.setWindowTitle(QStringLiteral("MaiChat"));
    progress.setWindowModality(Qt::ApplicationModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.hide();
    auto canceled = std::make_shared<std::atomic_bool>(false);
    QObject::connect(&progress, &QProgressDialog::canceled, &progress, [&] {
        canceled->store(true);
        progress.setLabelText(QStringLiteral("正在取消，请稍候…"));
        progress.setCancelButton(nullptr);
        // A native close runs QDialog::closeEvent after emitting canceled.
        // Re-show on the next GUI turn so it cannot hide the safe-stop status.
        QTimer::singleShot(0, &progress, [&progress] { progress.show(); });
    });
    QObject::connect(qApp, &QCoreApplication::aboutToQuit, &progress,
                     [canceled] { canceled->store(true); });

    MaiSqliteStoreOpenOptions options;
    options.isCanceled = [canceled] { return canceled->load(); };
    // The worker owns no widgets. Phase updates are queued to the GUI thread;
    // cancellation text wins over any progress already in the event queue.
    options.progress = [&progress, canceled](const std::string& phase) {
        const bool migrating = phase.rfind("migration", 0) == 0 || phase == "migrating";
        QMetaObject::invokeMethod(&progress, [&progress, canceled, migrating] {
            if (!canceled->load()) progress.setLabelText(migrating
                ? QStringLiteral("正在整理本地会话，请稍候…")
                : QStringLiteral("正在准备本地会话…"));
        }, Qt::QueuedConnection);
    };
    QFutureWatcher<std::shared_ptr<DesktopAgentStoragePreparationResult>> watcher;
    QEventLoop loop;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
    QTimer delay;
    delay.setSingleShot(true);
    QObject::connect(&delay, &QTimer::timeout, &progress, [&] {
        if (!watcher.isFinished()) progress.show();
    });
    watcher.setFuture(QtConcurrent::run([databasePath, opener = std::move(opener), options, canceled] {
        auto result = std::make_shared<DesktopAgentStoragePreparationResult>();
        auto opened = opener(databasePath, options);
        if (!opened) {
            result->error = opened.error().code() == MaiErrorCode::Busy
                                ? QStringLiteral(
                                      "该账号的本地会话库仍被其他App占用。请先退出旧版或另一份MaiCh"
                                      "at，再重试。")
                                : QString::fromUtf8(opened.error().message().c_str());
            result->canceled = opened.error().code() == MaiErrorCode::Canceled;
        } else if (!opened.value()) {
            result->error = QStringLiteral("本地会话未能打开。");
        } else if (canceled->load()) {
            // A cancel racing the final ready boundary still prevents startup.
            result->canceled = true;
        } else {
            result->store = std::move(opened.value());
        }
        return result;
    }));
    delay.start(qMax(0, progressDelayMs));
    if (!watcher.isFinished()) loop.exec();
    // aboutToQuit may end nested event loops. Keep borrowed callbacks alive
    // until the worker observes cancellation and reaches its safe boundary.
    watcher.waitForFinished();
    delay.stop();
    progress.hide();
    return std::move(*watcher.result());
}
