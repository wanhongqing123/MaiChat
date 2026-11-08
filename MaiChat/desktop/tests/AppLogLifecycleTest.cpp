// 文件作用：用独立子进程验证 Qt 晚期日志、日志状态析构与原处理器恢复顺序。
// 运行边界：只使用 Qt test-mode 下的随机测试目录，不访问正式 AppData 或现装程序。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUuid>
#include <QtTest>

#include <cstdio>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

#include "logging/AppLog.h"

namespace {
constexpr char kLateWarning[] = "applog-fixture-late-qt-warning";
constexpr char kRestoredMarker[] = "applog-fixture-previous-handler-restored";
constexpr char kOrdinaryMessage[] = "applog-fixture-flush-before-exit";

void previousHandler(QtMsgType, const QMessageLogContext&, const QString& message) {
    // 处理器只写 C stderr；不能再借用已经离开生命周期的 QObject 或日志状态。
    if (message == QLatin1String(kLateWarning))
        std::fprintf(stderr, "%s\n", kRestoredMarker);
}

struct EarlyQtObject {
    ~EarlyQtObject() {
        // 模拟先于 AppLog 构造的 QFactoryLoader 在静态清理期发出的真实 Qt 日志。
        qWarning("%s", kLateWarning);
    }
};

QString testDataPath(const QString& applicationName) {
    const QString previousOrganization = QCoreApplication::organizationName();
    const QString previousApplication = QCoreApplication::applicationName();
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatAppLogTests"));
    QCoreApplication::setApplicationName(applicationName);
    const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QCoreApplication::setOrganizationName(previousOrganization);
    QCoreApplication::setApplicationName(previousApplication);
    return path;
}

struct TestDirectory {
    QString applicationName = QStringLiteral("lifecycle-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QString path = testDataPath(applicationName);
    bool cleanupAllowed = false;
    ~TestDirectory() {
        // 仅移除本次随机创建的 test-mode 命名空间，不能递归删除父目录。
        if (cleanupAllowed && QStandardPaths::isTestModeEnabled() && QFileInfo(path).fileName() == applicationName)
            QDir(path).removeRecursively();
    }
};

int runChild(const QString& applicationName, bool blockedDirectory) {
#ifdef Q_OS_WIN
    // 原实现的 native crash 必须返回父进程，不能弹出阻塞测试的系统错误对话框。
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("MaiChatAppLogTests"));
    QCoreApplication::setApplicationName(applicationName);
    if (!applicationName.startsWith(QStringLiteral("lifecycle-")) || QUuid(applicationName.mid(10)).isNull()) return 20;
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(directory)) return 21;
    if (blockedDirectory) {
        QFile sentinel(QDir(directory).filePath(QStringLiteral("logs")));
        if (!sentinel.open(QIODevice::WriteOnly) || sentinel.write("existing-file") != 13) return 22;
    }

    qInstallMessageHandler(previousHandler);
    static EarlyQtObject earlierThanLogger;
    AppLog::install();
    AppLog::install();  // 重复安装仍应只有一个启动记录及一个恢复哨兵。
    qInfo("%s", kOrdinaryMessage);
    const QString logPath = AppLog::filePath();
    if (!blockedDirectory) {
        QFile log(logPath);
        if (!log.open(QIODevice::ReadOnly) || !log.readAll().contains(kOrdinaryMessage)) return 23;
    }
    return 0;
}
}  // namespace

class AppLogLifecycleTest final : public QObject {
    Q_OBJECT
private slots:
    void lateQtWarningUsesPreviousHandler_data() {
        QTest::addColumn<bool>("blockedDirectory");
        QTest::newRow("writable-log") << false;
        QTest::newRow("failed-log-open") << true;
    }

    void lateQtWarningUsesPreviousHandler() {
        QFETCH(bool, blockedDirectory);
        TestDirectory directory;
        QVERIFY(QStandardPaths::isTestModeEnabled());
        QVERIFY(!directory.path.isEmpty());
        QVERIFY(!QFileInfo::exists(directory.path));
        directory.cleanupAllowed = true;
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--logger-child"), directory.applicationName,
                     blockedDirectory ? QStringLiteral("blocked") : QStringLiteral("writable")});
        QVERIFY2(child.waitForStarted(5000), qPrintable(child.errorString()));
        if (!child.waitForFinished(10000)) {
            child.kill();
            child.waitForFinished(5000);
            QFAIL("Logger child did not finish its normal and static shutdown");
        }
        const QByteArray output = child.readAll();
        QVERIFY2(child.exitStatus() == QProcess::NormalExit, output.constData());
        QCOMPARE(child.exitCode(), 0);
        QCOMPARE(output.count(kRestoredMarker), 1);
        QCOMPARE(output.count("========== MaiChat started =========="), 1);
        QVERIFY(output.contains(kOrdinaryMessage));
        const QString logs = QDir(directory.path).filePath(QStringLiteral("logs"));
        if (blockedDirectory) {
            QFile sentinel(logs);
            QVERIFY(sentinel.open(QIODevice::ReadOnly));
            QCOMPARE(sentinel.readAll(), QByteArray("existing-file"));
        } else {
            const QDir logDirectory(logs);
            const auto files = logDirectory.entryList({QStringLiteral("maichat-*.log")}, QDir::Files);
            QCOMPARE(files.size(), 1);
            QFile log(logDirectory.filePath(files.front()));
            QVERIFY(log.open(QIODevice::ReadOnly));
            const QByteArray bytes = log.readAll();
            QCOMPARE(bytes.count("========== MaiChat started =========="), 1);
            QCOMPARE(bytes.count(kOrdinaryMessage), 1);
            QVERIFY(!bytes.contains(kLateWarning));
            // QLockFile 仍按原语义释放，不能为规避析构次序泄漏日志文件或锁。
            QVERIFY(!QFileInfo::exists(logDirectory.filePath(QStringLiteral(".instance.lock"))));
        }
    }
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() == 4 && arguments.at(1) == QStringLiteral("--logger-child"))
        return runChild(arguments.at(2), arguments.at(3) == QStringLiteral("blocked"));
    QStandardPaths::setTestModeEnabled(true);
    AppLogLifecycleTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "AppLogLifecycleTest.moc"
