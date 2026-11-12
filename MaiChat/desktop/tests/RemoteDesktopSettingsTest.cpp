// 文件作用：验证桌面端远控配置落盘、安全默认值和损坏配置回退。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QtTest/QtTest>

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "remote/RemoteDesktopSettings.h"

using RemoteDesktop::HostMode;

class RemoteDesktopSettingsTest : public QObject {
    Q_OBJECT

private slots:
    void defaultsToAttendedWithoutPassword();
    void roundTripsThroughDisk();
    void neverWritesPlaintextPassword();
    void keepsUnattendedModeWithoutPassword();
    void recoversFromCorruptFileWithSafeDefaults();
    void treatsUnknownModeAsAttended();
    void dropsLegacyDeviceListWhenSaving();
};

void RemoteDesktopSettingsTest::defaultsToAttendedWithoutPassword() {
    const RemoteDesktopSettings settings;
    QCOMPARE(static_cast<int>(settings.mode), static_cast<int>(HostMode::Attended));
    QVERIFY(!settings.hasPassword());
    QCOMPARE(settings.consecutiveAuthFailures, 0);
    QVERIFY(!settings.trtcProxyEnabled);
    QCOMPARE(settings.trtcProxyHost, QStringLiteral("127.0.0.1"));
    QCOMPARE(settings.trtcProxyPort, static_cast<quint16>(1082));
    QVERIFY(!settings.trtcProxyUdp);
}

void RemoteDesktopSettingsTest::roundTripsThroughDisk() {
    QTemporaryDir dir;
    const RemoteDesktopSettingsStore store(dir.filePath(QStringLiteral("remote-desktop.json")));

    RemoteDesktopSettings settings;
    settings.mode = HostMode::Unattended;
    settings.secret = RemoteDesktopAuth::deriveSecret(QStringLiteral("pw-123"),
                                                      RemoteDesktopAuth::generateSalt());
    settings.consecutiveAuthFailures = 2;
    settings.trtcProxyEnabled = true;
    settings.trtcProxyHost = QStringLiteral("proxy.internal");
    settings.trtcProxyPort = 1088;
    settings.trtcProxyUdp = false;
    QVERIFY(store.save(settings));

    const RemoteDesktopSettings loaded = store.load();
    QCOMPARE(static_cast<int>(loaded.mode), static_cast<int>(HostMode::Unattended));
    QCOMPARE(loaded.secret.salt, settings.secret.salt);
    QCOMPARE(loaded.secret.hash, settings.secret.hash);
    QCOMPARE(loaded.consecutiveAuthFailures, 2);
    QVERIFY(loaded.trtcProxyEnabled);
    QCOMPARE(loaded.trtcProxyHost, QStringLiteral("proxy.internal"));
    QCOMPARE(loaded.trtcProxyPort, static_cast<quint16>(1088));
    QVERIFY(!loaded.trtcProxyUdp);
    QVERIFY(loaded.hasPassword());
}

void RemoteDesktopSettingsTest::neverWritesPlaintextPassword() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("remote-desktop.json"));
    const RemoteDesktopSettingsStore store(path);

    const QString password = QStringLiteral("super-secret-passphrase");
    RemoteDesktopSettings settings;
    settings.mode = HostMode::Unattended;
    settings.secret =
        RemoteDesktopAuth::deriveSecret(password, RemoteDesktopAuth::generateSalt());
    QVERIFY(store.save(settings));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray onDisk = file.readAll();
    // 配置文件是明文 JSON，密码绝不能出现在里面。
    QVERIFY(!onDisk.contains(password.toUtf8()));
}

void RemoteDesktopSettingsTest::keepsUnattendedModeWithoutPassword() {
    RemoteDesktopSettings settings;
    settings.mode = HostMode::Unattended;
    // 密码是可选加固；没设密码时仍遵循用户选择的无人值守模式。
    QVERIFY(!settings.hasPassword());
    QCOMPARE(static_cast<int>(settings.effectiveMode()), static_cast<int>(HostMode::Unattended));
}

void RemoteDesktopSettingsTest::recoversFromCorruptFileWithSafeDefaults() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("remote-desktop.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ this is not valid json");
    file.close();

    const RemoteDesktopSettings loaded = RemoteDesktopSettingsStore(path).load();
    // 解析失败不得放宽权限。
    QCOMPARE(static_cast<int>(loaded.mode), static_cast<int>(HostMode::Attended));
    QVERIFY(!loaded.hasPassword());
}

void RemoteDesktopSettingsTest::treatsUnknownModeAsAttended() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("remote-desktop.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"mode\":\"always-allow-everyone\"}");
    file.close();

    QCOMPARE(static_cast<int>(RemoteDesktopSettingsStore(path).load().mode),
             static_cast<int>(HostMode::Attended));
}

void RemoteDesktopSettingsTest::dropsLegacyDeviceListWhenSaving() {
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("remote-desktop.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{\"mode\":\"unattended\",\"allowedUserIds\":[\"company-iphone\"]}");
    file.close();

    const RemoteDesktopSettingsStore store(path);
    const RemoteDesktopSettings settings = store.load();
    QCOMPARE(settings.mode, HostMode::Unattended);
    QVERIFY(store.save(settings));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject saved = QJsonDocument::fromJson(file.readAll()).object();
    QVERIFY(!saved.contains(QStringLiteral("allowedUserIds")));
}

QTEST_MAIN(RemoteDesktopSettingsTest)
#include "RemoteDesktopSettingsTest.moc"
