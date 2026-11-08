// 文件作用：用静音文件验证普通音频会话的 UI 绑定、所有权切换与视频交接；不创建媒体窗口。
#include "ui/DesktopAudioPlayback.h"

#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "MaiFfplayEntry.h"

static bool writeSilentAudio(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 sampleRate = 8000;
    constexpr quint32 dataBytes = sampleRate * 3 * 2;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + dataBytes);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << sampleRate << sampleRate * 2 << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << dataBytes;
    return stream.status() == QDataStream::Ok && file.write(QByteArray(dataBytes, '\0')) == dataBytes;
}

class DesktopAudioPlaybackTest final : public QObject {
    Q_OBJECT
private slots:
    void cleanup() {
        DesktopAudioPlayback::setVideoStopHandler({});
        DesktopAudioPlayback::stopAll();
    }
    void metadataDoesNotStartPlayback();
    void playPauseSeekAndReplay();
    void previousOwnerCannotControlOrStopNewOwner();
    void invalidSourceDoesNotStealPlayback();
    void videoHandoffStopsAudioAndAudioCallsVideoCloser();
};

void DesktopAudioPlaybackTest::metadataDoesNotStartPlayback() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("metadata.wav"));
    QVERIFY(writeSilentAudio(path));
    DesktopAudioPlayback playback;
    playback.setSource(path);
    QTRY_VERIFY_WITH_TIMEOUT(playback.durationUs() >= 2999000 && playback.durationUs() <= 3001000, 3000);
    QCOMPARE(playback.state(), DesktopAudioPlayback::State::Stopped);
    QCOMPARE(playback.positionUs(), qint64(0));
    MaiFfplayPlaybackStatus status{};
    QCOMPARE(maiFfplayGetPlaybackStatus(&status), 0);
}

void DesktopAudioPlaybackTest::playPauseSeekAndReplay() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("controls.wav"));
    QVERIFY(writeSilentAudio(path));
    DesktopAudioPlayback playback;
    playback.setSource(path);
    QVERIFY(playback.play());
    QTRY_VERIFY_WITH_TIMEOUT(playback.state() == DesktopAudioPlayback::State::Playing && playback.positionUs() > 100000,
                             3000);
    QVERIFY(playback.pause());
    QTRY_COMPARE_WITH_TIMEOUT(playback.state(), DesktopAudioPlayback::State::Paused, 1500);
    const qint64 pausedAt = playback.positionUs();
    QTest::qWait(150);
    QVERIFY(qAbs(playback.positionUs() - pausedAt) < 60000);
    QVERIFY(playback.play());
    QTRY_VERIFY_WITH_TIMEOUT(
        playback.state() == DesktopAudioPlayback::State::Playing && playback.positionUs() > pausedAt + 50000, 2000);
    QVERIFY(playback.seek(2800000));
    QTRY_COMPARE_WITH_TIMEOUT(playback.state(), DesktopAudioPlayback::State::Ended, 3000);
    QVERIFY(playback.play());
    QTRY_VERIFY_WITH_TIMEOUT(playback.state() == DesktopAudioPlayback::State::Playing && playback.positionUs() > 0 &&
                                 playback.positionUs() < 1000000,
                             2000);
    playback.stop();
    QCOMPARE(playback.state(), DesktopAudioPlayback::State::Stopped);
    MaiFfplayPlaybackStatus status{};
    QCOMPARE(maiFfplayGetPlaybackStatus(&status), 0);
}

void DesktopAudioPlaybackTest::previousOwnerCannotControlOrStopNewOwner() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString first = directory.filePath(QStringLiteral("first.wav"));
    const QString second = directory.filePath(QStringLiteral("second.wav"));
    QVERIFY(writeSilentAudio(first));
    QVERIFY(writeSilentAudio(second));
    auto previous = std::make_unique<DesktopAudioPlayback>();
    DesktopAudioPlayback current;
    previous->setSource(first);
    current.setSource(second);
    QVERIFY(previous->play());
    QTRY_COMPARE_WITH_TIMEOUT(previous->state(), DesktopAudioPlayback::State::Playing, 3000);
    QVERIFY(current.play());
    QTRY_VERIFY_WITH_TIMEOUT(current.state() == DesktopAudioPlayback::State::Playing && current.positionUs() > 100000,
                             3000);
    QVERIFY(previous->state() != DesktopAudioPlayback::State::Playing);
    QVERIFY(!previous->pause());
    QVERIFY(!previous->seek(2000000));
    previous->stop();
    previous.reset();
    const qint64 before = current.positionUs();
    QTRY_VERIFY_WITH_TIMEOUT(
        current.state() == DesktopAudioPlayback::State::Playing && current.positionUs() > before + 50000, 1000);
    current.stop();
}

void DesktopAudioPlaybackTest::videoHandoffStopsAudioAndAudioCallsVideoCloser() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("handoff.wav"));
    QVERIFY(writeSilentAudio(path));
    int videoCloses = 0;
    DesktopAudioPlayback::setVideoStopHandler([&] { ++videoCloses; });
    DesktopAudioPlayback playback;
    playback.setSource(path);
    QVERIFY(playback.play());
    QTRY_COMPARE_WITH_TIMEOUT(playback.state(), DesktopAudioPlayback::State::Playing, 3000);
    QCOMPARE(videoCloses, 1);
    DesktopAudioPlayback::prepareVideoPlayback();
    QCOMPARE(playback.state(), DesktopAudioPlayback::State::Stopped);
    MaiFfplayPlaybackStatus status{};
    QCOMPARE(maiFfplayGetPlaybackStatus(&status), 0);
    QVERIFY(!playback.pause());
    QVERIFY(!playback.seek(1000000));
    DesktopAudioPlayback::prepareVideoPlayback();
    QCOMPARE(videoCloses, 1);
    QVERIFY(playback.play());
    QTRY_COMPARE_WITH_TIMEOUT(playback.state(), DesktopAudioPlayback::State::Playing, 3000);
    QCOMPARE(videoCloses, 2);
    // 回调测试只验证交接发生；真实视频窗口和音频卡交接由原有 UI 回归补充覆盖。
    DesktopAudioPlayback::setVideoStopHandler({});
}

void DesktopAudioPlaybackTest::invalidSourceDoesNotStealPlayback() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("valid.wav"));
    QVERIFY(writeSilentAudio(path));
    DesktopAudioPlayback current;
    DesktopAudioPlayback invalid;
    current.setSource(path);
    QVERIFY(current.play());
    QTRY_VERIFY_WITH_TIMEOUT(current.state() == DesktopAudioPlayback::State::Playing && current.positionUs() > 100000,
                             3000);
    invalid.setSource(directory.filePath(QStringLiteral("missing.wav")));
    QVERIFY(!invalid.play());
    QTRY_COMPARE_WITH_TIMEOUT(invalid.state(), DesktopAudioPlayback::State::Error, 1500);
    QVERIFY(!invalid.errorString().isEmpty());
    const qint64 before = current.positionUs();
    QTRY_VERIFY_WITH_TIMEOUT(
        current.state() == DesktopAudioPlayback::State::Playing && current.positionUs() > before + 50000, 1000);
}

QTEST_GUILESS_MAIN(DesktopAudioPlaybackTest)
#include "DesktopAudioPlaybackTest.moc"
