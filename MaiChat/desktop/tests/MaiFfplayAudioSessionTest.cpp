// 文件作用：验证共享 FFplay 纯音频会话的实际播放、暂停、跳转和停止。
#include <QDataStream>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <thread>

#include "MaiFfplayAudioSession.h"

namespace {

struct FirstAudioCallback {
    FirstAudioCallback() {
        maiFfplaySetDiagnosticSink([](void* context, int, const char* line) {
            int volume = -1;
            int muted = -1;
            int nonSilent = -1;
            if (line && std::sscanf(line, "Initial audio callback volume=%d muted=%d non_silent=%d",
                                    &volume, &muted, &nonSilent) == 3) {
                auto* observation = static_cast<FirstAudioCallback*>(context);
                observation->volume = volume;
                observation->muted = muted;
                observation->nonSilent = nonSilent;
                observation->seen = true;
            }
        }, this);
    }
    ~FirstAudioCallback() { maiFfplaySetDiagnosticSink(nullptr, nullptr); }
    std::atomic<bool> seen{false};
    std::atomic<int> volume{-1};
    std::atomic<int> muted{-1};
    std::atomic<int> nonSilent{-1};
};

// The C API test uses a real audio-only FFplay session so queued commands also
// cover hosts without a video window. The worker always stops before its input
// and host storage leave scope, including an early Qt assertion failure.
class AudioSettingsPlayback {
public:
    AudioSettingsPlayback(const QByteArray& path, double speed,
                          const MaiFfplayAudioOptions* options, int cliVolume = -1,
                          bool legacyApi = false) : path_(path) {
        host_.graphics_view_id = 1;
        worker_ = std::thread([this, speed, options, cliVolume, legacyApi] {
            char name[] = "ffplay";
            char noDisplay[] = "-nodisp";
            char volumeOption[] = "-volume";
            QByteArray volume = QByteArray::number(cliVolume);
            char* arguments[] = {name, noDisplay, volumeOption, volume.data(), path_.data()};
            char* defaultArguments[] = {name, noDisplay, path_.data()};
            char** selected = cliVolume < 0 ? defaultArguments : arguments;
            const int count = cliVolume < 0 ? 3 : 5;
            result_ = legacyApi ? maiFfplayRunAtSpeed(&host_, count, selected, speed, 0)
                                : maiFfplayRunAtSpeedWithAudio(&host_, count, selected, speed, 0, options);
            finished_ = true;
        });
    }
    ~AudioSettingsPlayback() {
        for (int attempt = 0; attempt < 100 && !finished_; ++attempt) {
            if (maiFfplaySendCommand("close") == 1) break;
            QTest::qWait(10);
        }
        if (worker_.joinable()) worker_.join();
    }

private:
    QByteArray path_;
    MaiFfplayHost host_{};
    std::thread worker_;
    std::atomic<bool> finished_{false};
    int result_ = -1;
};

bool writeAudioSettingsFixture(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 sampleRate = 8'000;
    constexpr quint32 count = sampleRate * 5;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + count * 2);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << sampleRate
           << quint32(sampleRate * 2) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(count * 2);
    for (quint32 index = 0; index < count; ++index)
        stream << qint16(std::sin(index * (440.0 * 6.283185307179586 / sampleRate)) * 2000);
    return stream.status() == QDataStream::Ok;
}

}  // namespace

class MaiFfplayAudioSessionTest : public QObject {
    Q_OBJECT
private slots:
    void audioOnlyPauseHoldsClockAndSeekResumes();
    void audioSettingsValidateSizesAndInactiveCalls();
    void audioSettingsApplyExactValuesAndOrderedCommands();
    void audioSettingsRestoreOnSpeedRestart_data();
    void audioSettingsRestoreOnSpeedRestart();
    void nullAudioOptionsPreserveVolumeArgument_data();
    void nullAudioOptionsPreserveVolumeArgument();
};

void MaiFfplayAudioSessionTest::audioSettingsValidateSizesAndInactiveCalls() {
    uint64_t receipt = 123;
    QCOMPARE(maiFfplaySetVolumePercentWithReceipt(50, &receipt), 0);
    QCOMPARE(receipt, uint64_t(0));
    receipt = 123;
    QCOMPARE(maiFfplaySetMutedWithReceipt(2, &receipt), -1);
    QCOMPARE(receipt, uint64_t(0));
    QCOMPARE(maiFfplaySetVolumePercent(-1), -1);
    QCOMPARE(maiFfplaySetVolumePercent(101), -1);
    QCOMPARE(maiFfplaySetMuted(-1), -1);
    QCOMPARE(maiFfplaySetMuted(2), -1);
    QCOMPARE(maiFfplaySetVolumePercent(50), 0);
    QCOMPARE(maiFfplaySetMuted(1), 0);
    QCOMPARE(maiFfplayGetAudioStatus(nullptr), -1);
    MaiFfplayAudioStatus status{};
    QCOMPARE(maiFfplayGetAudioStatus(&status), -1);
    // A caller with only the version header must not receive later fields.
    struct HeaderOnly {
        uint32_t size = sizeof(uint32_t);
        uint32_t canary = 0x6a3d194b;
    } header;
    QCOMPARE(maiFfplayGetAudioStatus(reinterpret_cast<MaiFfplayAudioStatus*>(&header)), 0);
    QCOMPARE(header.canary, uint32_t(0x6a3d194b));
    // The former public status layout is still a valid short caller after
    // appending the receipt field; no padding or next object may be overwritten.
    struct OldAudioStatus {
        uint32_t size = 12;
        int volume = -1;
        int muted = -1;
        uint32_t canary = 0x7c42a58f;
    } oldStatus;
    QCOMPARE(maiFfplayGetAudioStatus(reinterpret_cast<MaiFfplayAudioStatus*>(&oldStatus)), 0);
    QCOMPARE(oldStatus.canary, uint32_t(0x7c42a58f));
    status.struct_size = offsetof(MaiFfplayAudioStatus, applied_serial) + sizeof(uint32_t);
    status.applied_serial = UINT64_C(0x1928374655aa7711);
    QCOMPARE(maiFfplayGetAudioStatus(&status), 0);
    QCOMPARE(status.applied_serial, UINT64_C(0x1928374655aa7711));
    MaiFfplayHost host{};
    host.graphics_view_id = 1;
    char name[] = "ffplay";
    char source[] = "unused.wav";
    char* arguments[] = {name, source};
    for (const auto invalid : {MaiFfplayAudioOptions{0, 50, 0},
                               MaiFfplayAudioOptions{sizeof(MaiFfplayAudioOptions), -1, 0},
                               MaiFfplayAudioOptions{sizeof(MaiFfplayAudioOptions), 101, 0},
                               MaiFfplayAudioOptions{sizeof(MaiFfplayAudioOptions), 50, 2}})
        QCOMPARE(maiFfplayRunAtSpeedWithAudio(&host, 2, arguments, 1, 0, &invalid), -1);
}

void MaiFfplayAudioSessionTest::audioSettingsApplyExactValuesAndOrderedCommands() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("audio-settings.wav"));
    QVERIFY(writeAudioSettingsFixture(path));
    AudioSettingsPlayback playback(path.toUtf8(), 1.0, nullptr);
    MaiFfplayPlaybackStatus clock{};
    MaiFfplayAudioStatus status{sizeof(MaiFfplayAudioStatus), 0, 0};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&clock) == 1 && clock.position_us > 100'000, 3'000);
    QCOMPARE(maiFfplayGetAudioStatus(&status), 1);
    QCOMPARE(status.volume_percent, 100);
    QCOMPARE(status.muted, 0);
    for (const int volume : {37, 0, 1, 99, 100}) {
        QCOMPARE(maiFfplaySetVolumePercent(volume), 1);
        QTRY_VERIFY(maiFfplayGetAudioStatus(&status) == 1 && status.volume_percent == volume);
    }
    uint64_t volumeReceipt = 0;
    uint64_t muteReceipt = 0;
    QCOMPARE(maiFfplaySetVolumePercentWithReceipt(37, &volumeReceipt), 1);
    QCOMPARE(maiFfplaySetMutedWithReceipt(1, &muteReceipt), 1);
    QVERIFY(volumeReceipt > 0 && muteReceipt > volumeReceipt);
    QCOMPARE(maiFfplaySendCommand("mute"), 1);
    QCOMPARE(maiFfplaySendCommand("volume_down"), 1);
    uint64_t finalReceipt = 0;
    QCOMPARE(maiFfplaySetVolumePercentWithReceipt(63, &finalReceipt), 1);
    QVERIFY(finalReceipt >= muteReceipt + 3);
    // Value and completion serial must be one snapshot, including when later
    // legacy/tool commands have already superseded an earlier receipt.
    QTRY_VERIFY(maiFfplayGetAudioStatus(&status) == 1 && status.applied_serial >= finalReceipt &&
                status.volume_percent == 63 && !status.muted);
    QCOMPARE(maiFfplaySendCommand("volume_down"), 1);
    QTRY_VERIFY(maiFfplayGetAudioStatus(&status) == 1 && status.volume_percent < 63);
    const int lowerVolume = status.volume_percent;
    QCOMPARE(maiFfplaySendCommand("volume_up"), 1);
    QTRY_VERIFY(maiFfplayGetAudioStatus(&status) == 1 && status.volume_percent > lowerVolume);
    QCOMPARE(maiFfplaySetMuted(1), 1);
    QCOMPARE(maiFfplaySetMuted(1), 1);
    QTRY_VERIFY(maiFfplayGetAudioStatus(&status) == 1 && status.muted == 1);
}

void MaiFfplayAudioSessionTest::audioSettingsRestoreOnSpeedRestart_data() {
    QTest::addColumn<int>("volume");
    QTest::addColumn<int>("muted");
    QTest::addColumn<int>("optionsSize");
    QTest::newRow("silent-muted") << 37 << 1 << int(sizeof(MaiFfplayAudioOptions));
    QTest::newRow("zero") << 0 << 0 << int(sizeof(MaiFfplayAudioOptions));
    QTest::newRow("full") << 100 << 0 << int(sizeof(MaiFfplayAudioOptions));
    QTest::newRow("header-only-defaults") << -1 << -1 << int(sizeof(uint32_t));
    QTest::newRow("volume-only-default-mute") << 37 << -1 << int(offsetof(MaiFfplayAudioOptions, muted));
}

void MaiFfplayAudioSessionTest::audioSettingsRestoreOnSpeedRestart() {
    QFETCH(int, volume);
    QFETCH(int, muted);
    QFETCH(int, optionsSize);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("restored-audio.wav"));
    QVERIFY(writeAudioSettingsFixture(path));
    const int expectedVolume = optionsSize >= int(offsetof(MaiFfplayAudioOptions, volume_percent) + sizeof(int))
        ? volume : 100;
    const int expectedMuted = optionsSize >= int(offsetof(MaiFfplayAudioOptions, muted) + sizeof(int)) ? muted : 0;
    for (const double speed : {1.0, 1.5}) {
        const MaiFfplayAudioOptions options{uint32_t(optionsSize), volume, muted};
        FirstAudioCallback firstCallback;
        AudioSettingsPlayback playback(path.toUtf8(), speed, &options);
        MaiFfplayPlaybackStatus clock{};
        MaiFfplayAudioStatus status{sizeof(MaiFfplayAudioStatus), -1, -1};
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&clock) == 1 && clock.position_us > 100'000, 3'000);
        QCOMPARE(maiFfplayGetAudioStatus(&status), 1);
        QCOMPARE(status.volume_percent, expectedVolume);
        QCOMPARE(status.muted, expectedMuted);
        QCOMPARE(clock.speed_percent, int(speed * 100));
        QTRY_VERIFY_WITH_TIMEOUT(firstCallback.seen.load(), 2'000);
        QCOMPARE(firstCallback.volume.load(), expectedVolume);
        QCOMPARE(firstCallback.muted.load(), expectedMuted);
        if (expectedMuted || expectedVolume == 0) QCOMPARE(firstCallback.nonSilent.load(), 0);
        // A shorter status caller receives volume without overwriting its tail.
        status.struct_size = offsetof(MaiFfplayAudioStatus, muted);
        status.muted = 0x4d5e;
        QCOMPARE(maiFfplayGetAudioStatus(&status), 1);
        QCOMPARE(status.volume_percent, expectedVolume);
        QCOMPARE(status.muted, 0x4d5e);
    }
}

void MaiFfplayAudioSessionTest::nullAudioOptionsPreserveVolumeArgument_data() {
    QTest::addColumn<int>("volume");
    QTest::addColumn<bool>("legacyApi");
    QTest::newRow("legacy-zero") << 0 << true;
    QTest::newRow("legacy-reduced") << 37 << true;
    QTest::newRow("optional-zero") << 0 << false;
    QTest::newRow("optional-reduced") << 37 << false;
}

// Adding optional audio settings must retain the original argv volume contract;
// in particular, an existing caller's -volume 0 cannot emit a full-volume burst.
void MaiFfplayAudioSessionTest::nullAudioOptionsPreserveVolumeArgument() {
    QFETCH(int, volume);
    QFETCH(bool, legacyApi);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("argv-volume.wav"));
    QVERIFY(writeAudioSettingsFixture(path));
    for (const double speed : {1.0, 1.5}) {
        FirstAudioCallback firstCallback;
        AudioSettingsPlayback playback(path.toUtf8(), speed, nullptr, volume, legacyApi);
        MaiFfplayPlaybackStatus clock{};
        MaiFfplayAudioStatus audio{sizeof(MaiFfplayAudioStatus), -1, -1};
        QTRY_VERIFY_WITH_TIMEOUT(maiFfplayGetPlaybackStatus(&clock) == 1 && clock.position_us > 100'000, 3000);
        QCOMPARE(maiFfplayGetAudioStatus(&audio), 1);
        QCOMPARE(audio.volume_percent, volume);
        QCOMPARE(audio.muted, 0);
        QTRY_VERIFY_WITH_TIMEOUT(firstCallback.seen.load(), 2000);
        QCOMPARE(firstCallback.volume.load(), volume);
        QCOMPARE(firstCallback.muted.load(), 0);
        if (volume == 0) QCOMPARE(firstCallback.nonSilent.load(), 0);
    }
}

void MaiFfplayAudioSessionTest::audioOnlyPauseHoldsClockAndSeekResumes() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile file(directory.filePath(QStringLiteral("music.wav")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 sampleRate = 8'000;
    constexpr quint32 count = sampleRate * 5;
    stream.writeRawData("RIFF", 4);
    stream << quint32(36 + count * 2);
    stream.writeRawData("WAVEfmt ", 8);
    stream << quint32(16) << quint16(1) << quint16(1) << sampleRate
           << quint32(sampleRate * 2) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(count * 2);
    for (quint32 index = 0; index < count; ++index) {
        const double phase = index * (440.0 * 6.283185307179586 / sampleRate);
        stream << qint16(std::sin(phase) * 2000);
    }
    file.close();

    const QByteArray path = file.fileName().toUtf8();
    auto* session = maiFfplayAudioStart(path.constData());
    QVERIFY(session);
    MaiFfplayPlaybackStatus state{};
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAudioStatus(session, &state) == 1 &&
                             state.duration_us >= 4'900'000 && state.position_us > 100'000,
                             3'000);
    QCOMPARE(maiFfplayAudioCommand(session, "pause"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAudioStatus(session, &state) == 1 && state.paused == 1,
                             2'000);
    const int64_t pausePosition = state.position_us;
    QTest::qWait(450);
    QVERIFY(maiFfplayAudioStatus(session, &state) == 1);
    QVERIFY(std::llabs(state.position_us - pausePosition) < 200'000);
    QCOMPARE(maiFfplayAudioCommand(session, "pause"), 1);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAudioStatus(session, &state) == 1 && state.paused == 0,
                             2'000);
    QCOMPARE(maiFfplayAudioSeekAbsoluteUs(session, 2'000'000), 1);
    QTRY_VERIFY_WITH_TIMEOUT(maiFfplayAudioStatus(session, &state) == 1 &&
                             state.position_us >= 1'800'000, 2'000);
    maiFfplayAudioStop(session);
}

QTEST_GUILESS_MAIN(MaiFfplayAudioSessionTest)
#include "MaiFfplayAudioSessionTest.moc"
