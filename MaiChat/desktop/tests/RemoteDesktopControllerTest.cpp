// 文件作用：验证桌面端远控首帧、输入许可、会话结束和按键释放。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QtTest/QtTest>

#include "ui/RemoteDesktopDeviceDialog.h"
#include <QListWidget>
#include <QComboBox>
#include <QPushButton>
#include <memory>

#include "remote/RemoteDesktopAuth.h"
#include "remote/RemoteDesktopController.h"
#include "remote/RemoteDesktopSignal.h"

using namespace RemoteDesktop;
using RemoteDesktopSignals::Signal;
using RemoteDesktopSignals::Type;

namespace {

// 记录调用的假引擎：让"是否真的进房推流了"成为可断言的事实，
// 而不是靠读代码推断。
// 记录型注入 sink：断言"到底动没动鼠标"，而不是只看有没有报错。
class RecordingInputSink final : public RemoteInput::IRemoteInputSink {
public:
    QStringList calls;

    void moveTo(double x, double y) override {
        calls << QStringLiteral("move %1,%2").arg(x).arg(y);
    }
    void mouseButton(RemoteInput::MouseButton button, bool pressed, double, double) override {
        calls << QStringLiteral("btn %1 %2")
                     .arg(static_cast<int>(button))
                     .arg(pressed ? QStringLiteral("down") : QStringLiteral("up"));
    }
    void wheel(int delta, double, double) override {
        calls << QStringLiteral("wheel %1").arg(delta);
    }
    void key(quint32 keyCode, bool pressed) override {
        calls << QStringLiteral("key %1 %2")
                     .arg(keyCode)
                     .arg(pressed ? QStringLiteral("down") : QStringLiteral("up"));
    }
    void text(const QString& value) override { calls << QStringLiteral("text %1").arg(value); }
};

class FakeTrtcEngine final : public ITrtcEngine {
public:
    struct SentMessage {
        int cmdId = 0;
        QByteArray payload;
        bool reliable = false;
        bool ordered = false;
    };
    QVector<SentMessage> sentMessages;
    CustomMessageCallback customMessageCallback;

    void setRemoteVideoCallback(RemoteVideoCallback callback) override {
        remoteVideoCallback = std::move(callback);
    }
    void setErrorCallback(ErrorCallback callback) override {
        errorCallback = std::move(callback);
    }
    void setCustomMessageCallback(CustomMessageCallback callback) override {
        customMessageCallback = std::move(callback);
    }
    void setRemoteVideoSizeCallback(RemoteVideoSizeCallback callback) override {
        remoteVideoSizeCallback = std::move(callback);
    }
    RemoteVideoSizeCallback remoteVideoSizeCallback;
    bool sendCustomMessage(int cmdId, const QByteArray& payload, bool reliable,
                           bool ordered) override {
        if (!active) return false;
        sentMessages.append({cmdId, payload, reliable, ordered});
        return true;
    }
    void bindRemoteView(const QString& userId, void*) override {
        bindCalls += 1;
        lastBoundUserId = userId;
    }

    QString sdkVersion() const override { return QStringLiteral("0.0.0-fake"); }

    bool startScreenShare(const TrtcRoomParams& params) override {
        if (failNextStart) return false;
        shareCalls += 1;
        lastRoomId = params.roomId;
        lastMediaUserId = params.userId;
        active = true;
        return true;
    }

    std::optional<CaptureGeometry> localCaptureGeometry() const override {
        return localGeometry;
    }

    bool startViewing(const TrtcRoomParams& params, void*) override {
        viewCalls += 1;
        lastRoomId = params.roomId;
        lastMediaUserId = params.userId;
        active = true;
        if (emitVideoDuringViewStart && remoteVideoCallback) {
            remoteVideoCallback(videoUserIdDuringViewStart, true);
        }
        return true;
    }

    void stop() override {
        stopCalls += 1;
        active = false;
    }

    bool isActive() const override { return active; }

    int shareCalls = 0;
    int viewCalls = 0;
    int stopCalls = 0;
    int bindCalls = 0;
    bool active = false;
    bool failNextStart = false;
    bool emitVideoDuringViewStart = false;
    QString videoUserIdDuringViewStart = QStringLiteral("remote-host");
    QString lastRoomId;
    QString lastMediaUserId;
    QString lastBoundUserId;
    std::optional<CaptureGeometry> localGeometry;
    RemoteVideoCallback remoteVideoCallback;
    ErrorCallback errorCallback;
};

struct SentSignal {
    QString peerId;
    Signal signal;
};

const QString kHostUser = QStringLiteral("desktop-im");
const QString kPeerUser = QStringLiteral("whq-iphone");
const QString kHostDevice = QStringLiteral("owned-host-device");
const QString kPeerDevice = QStringLiteral("owned-peer-device");
QString encodePeerSignal(Signal signal) {
    if (signal.sourceDeviceId.isEmpty())
        signal.sourceDeviceId = kPeerDevice;
    if (signal.targetDeviceId.isEmpty() && signal.type != Type::Discover)
        signal.targetDeviceId = kHostDevice;
    return RemoteDesktopSignals::encodeSignal(signal);
}

const QString kPassword = QStringLiteral("pw-remote-1");

class Harness {
public:
  explicit Harness(HostMode mode, bool withPassword = true, QString deviceId = kHostDevice) {
      settings.mode = mode;
      if (withPassword) {
          settings.secret =
              RemoteDesktopAuth::deriveSecret(kPassword, RemoteDesktopAuth::generateSalt());
      }

      RemoteDesktopController::Config config;
      config.sdkAppId = 1600148979;
      config.localUserId = kHostUser;
      config.localDeviceId = deviceId;
      config.localDeviceName = QStringLiteral("Owned computer");
      config.platform = QStringLiteral("macOS");

      auto engineOwner = std::make_unique<FakeTrtcEngine>();
      engine = engineOwner.get();

      controller = std::make_unique<RemoteDesktopController>(
          config, settings, std::move(engineOwner),
          [this](const QString &peerId, const QString &text,
                 RemoteDesktopController::SignalSendCompletion completion) {
              sent.append({peerId, RemoteDesktopSignals::decodeSignal(text)});
              if (!completion)
                  return;
              if (autoCompleteSignalSends) {
                  completion();
              } else {
                  pendingSignalCompletions.append(std::move(completion));
              }
          });
      controller->setIdGenerator([] { return QStringLiteral("fixed-id"); });
      controller->setMediaCredential(RemoteDesktopSignals::mediaUserId(kHostUser, deviceId),
          QStringLiteral("owned-test-signature"), QDateTime::currentSecsSinceEpoch() + 1800);


      // 换掉真实注入器，否则跑一遍测试就真去动鼠标了。
      auto sinkOwner = std::make_unique<RecordingInputSink>();
      sink = sinkOwner.get();
      controller->setInputSink(std::move(sinkOwner));
  }

    // 把一包输入按被控端收到的样子喂进去。
    void deliverInput(const QString& sessionId, quint32 sequence,
                      const QVector<RemoteInput::Event>& events,
                      const QString& fromUserId = kPeerUser) {
        RemoteInput::Packet packet;
        packet.sessionId = sessionId;
        packet.sequence = sequence;
        packet.events = events;
        if (engine->customMessageCallback) {
            engine->customMessageCallback(
                RemoteDesktopSignals::mediaUserId(fromUserId, kPeerDevice),
                RemoteInput::kCmdIdReliable, RemoteInput::encodePacket(packet));
        }
    }

    // 构造一条来自 kPeerUser 的 invite，proof 按需带上。
    QString inviteText(const QString& sessionId,
                       const QString& roomId,
                       const QString& password = QString()) const {
        Signal invite;
        invite.type = Type::Invite;
        invite.sessionId = sessionId;
        invite.roomId = roomId;
        if (!password.isEmpty()) {
            const RemoteDesktopAuth::StoredSecret peerSecret =
                RemoteDesktopAuth::deriveSecret(password, settings.secret.salt);
            invite.authProof =
                RemoteDesktopAuth::makeAuthProof(peerSecret, sessionId, roomId, kPeerUser);
        }
        return encodePeerSignal(invite);
    }

    Signal lastSent() const { return sent.isEmpty() ? Signal{} : sent.last().signal; }

    RemoteDesktopSettings settings;
    FakeTrtcEngine* engine = nullptr;
    RecordingInputSink* sink = nullptr;
    std::unique_ptr<RemoteDesktopController> controller;
    QVector<SentSignal> sent;
    bool autoCompleteSignalSends = true;
    QVector<RemoteDesktopController::SignalSendCompletion> pendingSignalCompletions;
};

}  // namespace

class RemoteDesktopControllerTest : public QObject {
    Q_OBJECT

private slots:
    void rejectsMissingOrMismatchedMediaCredentialBeforeInviting();
    void devicePickerSwitchesToOwnAccountAndDiscardsOldReplies();
  void sameAccountDevicesUseDistinctMediaParticipants();
  void discoversComputersViaIMWithoutOpeningRoom();
  void targetsOnlyTheSelectedComputer();
  void ignoresSiblingRejectionAndWrongAccount();
  void ignoresStopFromUnselectedDevice();
  void devicePickerRequiresExplicitSelection();
  void firstFrameEnablesInputSending();
  void handlesVideoCallbackDuringViewStart();
  void dropsRemoteInputWhenControlSwitchIsOff();
  void injectsRemoteInputOnceControlIsAllowed();
  void dropsRemoteInputFromNonPeerAndStaleSession();
  void releasesHeldKeysWhenSessionStops();
  void notifiesControlledPeerBeforeViewerShutdownCompletes();
  void notifiesViewerBeforeHostShutdownCompletes();
  void ignoresPlainChatText();
  void consumesMalformedSignalWithoutActing();
  void attendedModeEmitsConsentAndSharesAfterAccept();
  void attendedModeRejectsWhenUserDeclines();
  void unattendedModeSharesWithoutPrompting();
  void unattendedModeRejectsWrongPassword();
  void unattendedModeDowngradesAfterRepeatedFailures();
  void acceptsPeerWithoutDeviceList();
  void attendedModeRequestsConsentWithoutDeviceList();
  void ignoresInvitesWithoutSenderIdentity();
  void rejectsInviteWhileAlreadySharing();
  void busyInviteKeepsPendingConsentSession();
  void busyInviteKeepsActiveSessionInputAndStop();
  void reportsRejectionToViewer();
  void stopsSharingOnPeerStop();
  void reportsFailureWhenScreenShareCannotStart();
  void doesNotResendInviteWhileOneIsPending();
  void forwardsRemoteVideoHandlerToEngine();
  void forwardsRemoteVideoSizeToHandler();
  void hostAcceptCarriesLocalCaptureGeometry();
  void viewerAppliesAndClearsCaptureGeometryAcrossLifecycle();
};

namespace {

RemoteInput::Event keyDown(quint32 keyCode) {
    RemoteInput::Event event;
    event.type = RemoteInput::EventType::Key;
    event.keyCode = keyCode;
    event.pressed = true;
    return event;
}

// 让被控端进入共享态。Harness 默认设了密码，所以 invite 必须带上 proof。
void startSharing(Harness& h) {
    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s-1"), QStringLiteral("room-1"), kPassword));
    QCOMPARE(h.controller->hostState(), HostState::Sharing);
}

}  // namespace

void RemoteDesktopControllerTest::firstFrameEnablesInputSending() {
    Harness h(HostMode::Attended);
    // UI 会挂自己的画面回调；控制器必须先截一道再转交，不能被覆盖掉。
    int forwarded = 0;
    h.controller->setRemoteVideoHandler(
        [&forwarded](const QString&, bool) { ++forwarded; });

    h.controller->requestView(kPeerUser, {}, kPeerDevice);
    Signal accept;
    accept.type = Type::Accept;
    accept.sessionId = h.lastSent().sessionId;
    accept.roomId = QStringLiteral("room-1");
    h.controller->handleIncomingText(kPeerUser, encodePeerSignal(accept));
    QCOMPARE(h.controller->viewerState(), ViewerState::Connecting);

    // 首帧到达是 Connecting → Viewing 的唯一触发点，而输入会话正是跟着
    // Viewing 开的。少了这一步，画面照常显示、控制却一个包都发不出去。
    QVERIFY(h.engine->remoteVideoCallback);
    h.engine->remoteVideoCallback(RemoteDesktopSignals::mediaUserId(kPeerUser, kPeerDevice), true);
    QCOMPARE(h.controller->viewerState(), ViewerState::Viewing);
    QCOMPARE(forwarded, 1);

    h.engine->sentMessages.clear();
    h.controller->inputSender().queueKey(0x41, true);
    h.controller->flushPendingInput(1000);
    QVERIFY2(!h.engine->sentMessages.isEmpty(), "首帧之后仍然发不出输入包");
}

void RemoteDesktopControllerTest::handlesVideoCallbackDuringViewStart() {
    const QString windowsUser = QStringLiteral("company-iphone");
    Harness h(HostMode::Attended);
    h.engine->emitVideoDuringViewStart = true;
    h.engine->videoUserIdDuringViewStart =
        RemoteDesktopSignals::mediaUserId(windowsUser, kPeerDevice);

    h.controller->requestView(windowsUser, {}, kPeerDevice);
    Signal accept;
    accept.type = Type::Accept;
    accept.sessionId = h.lastSent().sessionId;
    accept.roomId = QStringLiteral("room-windows");
    h.controller->handleIncomingText(windowsUser, encodePeerSignal(accept));

    QCOMPARE(h.controller->viewerPeerId(), windowsUser);
    QCOMPARE(h.controller->viewerState(), ViewerState::Viewing);
}

void RemoteDesktopControllerTest::dropsRemoteInputWhenControlSwitchIsOff() {
    Harness h(HostMode::Unattended);
    QVERIFY(!h.controller->isRemoteControlAllowed());  // 默认关
    startSharing(h);

    // 能看不等于能操作：开关没开时，输入必须一条都注不进去。
    h.deliverInput(QStringLiteral("s-1"), 1, {keyDown(0x41)});
    QVERIFY2(h.sink->calls.isEmpty(),
             qPrintable(QStringLiteral("控制开关关着却注入了：%1")
                            .arg(h.sink->calls.join(QStringLiteral(", ")))));
}

void RemoteDesktopControllerTest::injectsRemoteInputOnceControlIsAllowed() {
    Harness h(HostMode::Unattended);
    h.settings.allowRemoteControl = true;
    h.controller->updateSettings(h.settings);
    startSharing(h);

    h.deliverInput(QStringLiteral("s-1"), 1, {keyDown(0x41)});
    QCOMPARE(h.sink->calls, QStringList({QStringLiteral("key 65 down")}));
}

void RemoteDesktopControllerTest::dropsRemoteInputFromNonPeerAndStaleSession() {
    Harness h(HostMode::Unattended);
    h.settings.allowRemoteControl = true;
    h.controller->updateSettings(h.settings);
    startSharing(h);

    // 房间里混进第三方：它的输入一律不执行。
    h.deliverInput(QStringLiteral("s-1"), 1, {keyDown(0x41)}, QStringLiteral("someone-else"));
    QVERIFY(h.sink->calls.isEmpty());

    // 上一场会话的残留包不该操作这一场的电脑。
    h.deliverInput(QStringLiteral("s-0"), 2, {keyDown(0x42)});
    QVERIFY(h.sink->calls.isEmpty());

    // 对得上的照常放行，确认上面两条不是因为整条链路根本没通。
    h.deliverInput(QStringLiteral("s-1"), 3, {keyDown(0x43)});
    QCOMPARE(h.sink->calls, QStringList({QStringLiteral("key 67 down")}));
}

void RemoteDesktopControllerTest::releasesHeldKeysWhenSessionStops() {
    Harness h(HostMode::Unattended);
    h.settings.allowRemoteControl = true;
    h.controller->updateSettings(h.settings);
    startSharing(h);

    h.deliverInput(QStringLiteral("s-1"), 1, {keyDown(0x11)});  // Ctrl 按下
    QCOMPARE(h.sink->calls, QStringList({QStringLiteral("key 17 down")}));
    h.sink->calls.clear();

    // 会话结束必须把按住的键抬了：否则被控机一直是 Ctrl 按住状态，
    // 点什么都变成 Ctrl+点击，而人不在电脑旁没法自己解。
    h.controller->stopSession();
    QCOMPARE(h.controller->hostState(), HostState::Idle);
    QVERIFY2(h.sink->calls.contains(QStringLiteral("key 17 up")),
             qPrintable(QStringLiteral("会话结束没抬键，实际调用：%1")
                            .arg(h.sink->calls.join(QStringLiteral(", ")))));
}

void RemoteDesktopControllerTest::notifiesControlledPeerBeforeViewerShutdownCompletes() {
    Harness h(HostMode::Attended);
    h.controller->requestView(kPeerUser, {}, kPeerDevice);
    const QString sessionId = h.lastSent().sessionId;

    Signal accept;
    accept.type = Type::Accept;
    accept.sessionId = sessionId;
    accept.roomId = QStringLiteral("room-1");
    h.controller->handleIncomingText(kPeerUser, encodePeerSignal(accept));

    h.sent.clear();
    h.autoCompleteSignalSends = false;
    int shutdownCompletions = 0;
    h.controller->stopSession([&shutdownCompletions] { ++shutdownCompletions; });

    QCOMPARE(h.sent.size(), 1);
    QCOMPARE(h.sent.first().peerId, kPeerUser);
    QCOMPARE(static_cast<int>(h.sent.first().signal.type), static_cast<int>(Type::Stop));
    QCOMPARE(h.sent.first().signal.sessionId, sessionId);
    QCOMPARE(static_cast<int>(h.controller->viewerState()), static_cast<int>(ViewerState::Idle));
    QCOMPARE(shutdownCompletions, 0);
    QCOMPARE(h.pendingSignalCompletions.size(), 1);

    auto sendCompleted = std::move(h.pendingSignalCompletions.first());
    h.pendingSignalCompletions.clear();
    sendCompleted();
    QCOMPARE(shutdownCompletions, 1);
}

void RemoteDesktopControllerTest::notifiesViewerBeforeHostShutdownCompletes() {
    Harness h(HostMode::Unattended);
    startSharing(h);

    h.sent.clear();
    h.autoCompleteSignalSends = false;
    int shutdownCompletions = 0;
    h.controller->stopSession([&shutdownCompletions] { ++shutdownCompletions; });

    QCOMPARE(h.sent.size(), 1);
    QCOMPARE(h.sent.first().peerId, kPeerUser);
    QCOMPARE(static_cast<int>(h.sent.first().signal.type), static_cast<int>(Type::Stop));
    QCOMPARE(h.sent.first().signal.sessionId, QStringLiteral("s-1"));
    QCOMPARE(static_cast<int>(h.controller->hostState()), static_cast<int>(HostState::Idle));
    QCOMPARE(shutdownCompletions, 0);
    QCOMPARE(h.pendingSignalCompletions.size(), 1);

    auto sendCompleted = std::move(h.pendingSignalCompletions.first());
    h.pendingSignalCompletions.clear();
    sendCompleted();
    QCOMPARE(shutdownCompletions, 1);
}

void RemoteDesktopControllerTest::ignoresPlainChatText() {
    Harness h(HostMode::Attended);
    QVERIFY(!h.controller->handleIncomingText(kPeerUser, QStringLiteral("你好")));
    QVERIFY(h.sent.isEmpty());
}

void RemoteDesktopControllerTest::consumesMalformedSignalWithoutActing() {
    Harness h(HostMode::Attended);
    // 前缀对但内容坏：必须消费掉（不泄漏到聊天记录），但不产生任何动作。
    const QString broken = RemoteDesktopSignals::signalPrefix() + QStringLiteral("{oops");
    QVERIFY(h.controller->handleIncomingText(kPeerUser, broken));
    QVERIFY(h.sent.isEmpty());
    QCOMPARE(h.engine->shareCalls, 0);
}

void RemoteDesktopControllerTest::attendedModeEmitsConsentAndSharesAfterAccept() {
    Harness h(HostMode::Attended);
    QSignalSpy consentSpy(h.controller.get(), &RemoteDesktopController::consentRequested);
    QSignalSpy startedSpy(h.controller.get(), &RemoteDesktopController::sharingStarted);

    QVERIFY(h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"))));

    QCOMPARE(consentSpy.count(), 1);
    // 用户还没点，绝不能先推流。
    QCOMPARE(h.engine->shareCalls, 0);
    QVERIFY(h.sent.isEmpty());

    h.controller->resolveConsent(true);
    QCOMPARE(h.engine->shareCalls, 1);
    QCOMPARE(h.engine->lastRoomId, QStringLiteral("room-1"));
    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Accept));
}

void RemoteDesktopControllerTest::hostAcceptCarriesLocalCaptureGeometry() {
    Harness h(HostMode::Attended);
    CaptureGeometry geometry;
    geometry.sourceSize = QSize(2560, 1600);
    geometry.captureRect = QRect(0, 0, 2560, 1600);
    geometry.contentMode = CaptureContentMode::Fit;
    geometry.revision = 3;
    h.engine->localGeometry = geometry;

    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s-geometry"), QStringLiteral("room-geometry")));
    h.controller->resolveConsent(true);

    QCOMPARE(h.lastSent().type, Type::Accept);
    QVERIFY(h.lastSent().captureGeometry.has_value());
    QCOMPARE(h.lastSent().captureGeometry->sourceSize, QSize(2560, 1600));
    QCOMPARE(h.lastSent().captureGeometry->captureRect, QRect(0, 0, 2560, 1600));
    QCOMPARE(h.lastSent().captureGeometry->revision, quint64(3));
}

void RemoteDesktopControllerTest::viewerAppliesAndClearsCaptureGeometryAcrossLifecycle() {
    CaptureGeometry geometry;
    geometry.sourceSize = QSize(2560, 1600);
    geometry.captureRect = QRect(0, 0, 2560, 1600);
    geometry.contentMode = CaptureContentMode::Fit;
    geometry.revision = 1;

    Harness h(HostMode::Attended);
    // 新邀请发出时必须先清旧机器的坐标系。
    h.controller->inputSender().setCaptureGeometry(geometry);
    h.controller->requestView(kPeerUser, {}, kPeerDevice);
    QVERIFY(!h.controller->inputSender().captureGeometry().has_value());

    Signal accept;
    accept.type = Type::Accept;
    accept.sessionId = h.lastSent().sessionId;
    accept.roomId = QStringLiteral("room-geometry");
    accept.captureGeometry = geometry;
    h.controller->handleIncomingText(kPeerUser, encodePeerSignal(accept));
    QVERIFY(h.controller->inputSender().captureGeometry().has_value());
    QCOMPARE(h.controller->inputSender().captureGeometry()->sourceSize, QSize(2560, 1600));

    Signal stop;
    stop.type = Type::Stop;
    stop.sessionId = accept.sessionId;
    h.controller->handleIncomingText(kPeerUser, encodePeerSignal(stop));
    QVERIFY(!h.controller->inputSender().captureGeometry().has_value());

    // Reject 同样不能留下任何增强几何（即便调用前有陈旧值）。
    Harness rejected(HostMode::Attended);
    rejected.controller->requestView(kPeerUser, {}, kPeerDevice);
    rejected.controller->inputSender().setCaptureGeometry(geometry);
    Signal reject;
    reject.type = Type::Reject;
    reject.sessionId = rejected.lastSent().sessionId;
    reject.reason = reasonBusy();
    rejected.controller->handleIncomingText(kPeerUser, encodePeerSignal(reject));
    QVERIFY(!rejected.controller->inputSender().captureGeometry().has_value());
}

void RemoteDesktopControllerTest::attendedModeRejectsWhenUserDeclines() {
    Harness h(HostMode::Attended);
    h.controller->handleIncomingText(kPeerUser,
                                     h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1")));
    h.controller->resolveConsent(false);

    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Reject));
    QCOMPARE(h.lastSent().reason, reasonDeclined());
    QCOMPARE(h.engine->shareCalls, 0);
    QCOMPARE(static_cast<int>(h.controller->hostState()), static_cast<int>(HostState::Idle));
}

void RemoteDesktopControllerTest::unattendedModeSharesWithoutPrompting() {
    Harness h(HostMode::Unattended);
    QSignalSpy consentSpy(h.controller.get(), &RemoteDesktopController::consentRequested);

    QVERIFY(h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), kPassword)));

    // 无人值守的核心承诺：不打扰用户，直接开始共享。
    QCOMPARE(consentSpy.count(), 0);
    QCOMPARE(h.engine->shareCalls, 1);
    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Accept));
    QCOMPARE(static_cast<int>(h.controller->hostState()), static_cast<int>(HostState::Sharing));
}

void RemoteDesktopControllerTest::unattendedModeRejectsWrongPassword() {
    Harness h(HostMode::Unattended);
    QVERIFY(h.controller->handleIncomingText(
        kPeerUser,
        h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), QStringLiteral("wrong"))));

    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Reject));
    QCOMPARE(h.lastSent().reason, reasonBadPassword());
    QCOMPARE(h.engine->shareCalls, 0);
    QCOMPARE(h.controller->settings().consecutiveAuthFailures, 1);
}

void RemoteDesktopControllerTest::unattendedModeDowngradesAfterRepeatedFailures() {
    Harness h(HostMode::Unattended);
    QSignalSpy downgradeSpy(h.controller.get(), &RemoteDesktopController::modeDowngraded);

    for (int i = 0; i < RemoteDesktopAuth::kMaxConsecutiveFailures; ++i) {
        h.controller->handleIncomingText(
            kPeerUser,
            h.inviteText(QStringLiteral("s%1").arg(i), QStringLiteral("room-1"),
                         QStringLiteral("wrong")));
    }

    QCOMPARE(downgradeSpy.count(), 1);
    // 降级后同一个正确密码也不再自动放行，而是走弹窗。
    QCOMPARE(static_cast<int>(h.controller->settings().mode), static_cast<int>(HostMode::Attended));

    QSignalSpy consentSpy(h.controller.get(), &RemoteDesktopController::consentRequested);
    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s-final"), QStringLiteral("room-2"), kPassword));
    QCOMPARE(consentSpy.count(), 1);
    QCOMPARE(h.engine->shareCalls, 0);
}

void RemoteDesktopControllerTest::acceptsPeerWithoutDeviceList() {
    Harness h(HostMode::Unattended);
    QVERIFY(h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), kPassword)));

    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Accept));
    QCOMPARE(h.engine->shareCalls, 1);
}

void RemoteDesktopControllerTest::attendedModeRequestsConsentWithoutDeviceList() {
    Harness h(HostMode::Attended, /*withPassword=*/false);
    QSignalSpy consentSpy(h.controller.get(), &RemoteDesktopController::consentRequested);
    QVERIFY(h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"))));

    QCOMPARE(consentSpy.count(), 1);
    QCOMPARE(h.controller->hostState(), HostState::AwaitingConsent);
    QCOMPARE(h.engine->shareCalls, 0);
}

void RemoteDesktopControllerTest::ignoresInvitesWithoutSenderIdentity() {
    // 名单移除后仍须有 IM 提供的发起身份，空值不能变成匿名连入。
    for (const QString& sender : {QString(), QStringLiteral("  ")}) {
        for (HostMode mode : {HostMode::Attended, HostMode::Unattended}) {
            Harness h(mode, /*withPassword=*/false);
            QSignalSpy consentSpy(h.controller.get(), &RemoteDesktopController::consentRequested);
            QVERIFY(h.controller->handleIncomingText(
                sender, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"))));
            QCOMPARE(h.controller->hostState(), HostState::Idle);
            QCOMPARE(consentSpy.count(), 0);
            QCOMPARE(h.engine->shareCalls, 0);
        }
    }
}

void RemoteDesktopControllerTest::rejectsInviteWhileAlreadySharing() {
    Harness h(HostMode::Unattended);
    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), kPassword));
    QCOMPARE(h.engine->shareCalls, 1);

    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s2"), QStringLiteral("room-2"), kPassword));

    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Reject));
    QCOMPARE(h.lastSent().reason, reasonBusy());
    // 已有会话不被抢占。
    QCOMPARE(h.engine->shareCalls, 1);
    QCOMPARE(h.engine->lastRoomId, QStringLiteral("room-1"));
}

void RemoteDesktopControllerTest::busyInviteKeepsPendingConsentSession() {
    Harness h(HostMode::Attended, /*withPassword=*/false);
    QSignalSpy consentSpy(h.controller.get(), &RemoteDesktopController::consentRequested);
    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("session-a"), QStringLiteral("room-a")));
    QCOMPARE(consentSpy.count(), 1);
    QCOMPARE(h.controller->hostState(), HostState::AwaitingConsent);

    const QString otherPeer = QStringLiteral("peer-b");
    h.controller->handleIncomingText(
        otherPeer, h.inviteText(QStringLiteral("session-b"), QStringLiteral("room-b")));
    QCOMPARE(h.lastSent().type, Type::Reject);
    QCOMPARE(h.lastSent().reason, reasonBusy());
    QCOMPARE(h.lastSent().sessionId, QStringLiteral("session-b"));
    QCOMPARE(h.sent.last().peerId, otherPeer);
    QCOMPARE(consentSpy.count(), 1);

    // B 被拒后，确认弹窗仍属于 A，不能把 A 的身份与 B 的房间拼在一起。
    h.controller->resolveConsent(true);
    QCOMPARE(h.engine->lastRoomId, QStringLiteral("room-a"));
    QCOMPARE(h.lastSent().type, Type::Accept);
    QCOMPARE(h.lastSent().sessionId, QStringLiteral("session-a"));
    QCOMPARE(h.lastSent().roomId, QStringLiteral("room-a"));
    QCOMPARE(h.sent.last().peerId, kPeerUser);
}

void RemoteDesktopControllerTest::busyInviteKeepsActiveSessionInputAndStop() {
    Harness h(HostMode::Unattended);
    h.settings.allowRemoteControl = true;
    h.controller->updateSettings(h.settings);
    startSharing(h);

    h.controller->handleIncomingText(
        QStringLiteral("peer-b"),
        h.inviteText(QStringLiteral("session-b"), QStringLiteral("room-b")));
    QCOMPARE(h.lastSent().type, Type::Reject);
    QCOMPARE(h.lastSent().sessionId, QStringLiteral("session-b"));
    QCOMPARE(h.lastSent().reason, reasonBusy());

    // 忙碌拒绝不得改写已共享的会话：A 后续输入和 stop 必须继续匹配。
    h.deliverInput(QStringLiteral("s-1"), 1, {keyDown(0x41)});
    const QStringList inputCalls = h.sink->calls;
    Signal stop;
    stop.type = Type::Stop;
    stop.sessionId = QStringLiteral("s-1");
    h.controller->handleIncomingText(kPeerUser, encodePeerSignal(stop));
    QVERIFY2(inputCalls == QStringList{QStringLiteral("key 65 down")} &&
                 h.engine->stopCalls == 1 && h.controller->hostState() == HostState::Idle,
             qPrintable(QStringLiteral("A input calls=%1, stop calls=%2, host state=%3")
                            .arg(inputCalls.join(QStringLiteral(", ")))
                            .arg(h.engine->stopCalls)
                            .arg(static_cast<int>(h.controller->hostState()))));
}

void RemoteDesktopControllerTest::reportsRejectionToViewer() {
    Harness h(HostMode::Attended);
    QSignalSpy stateSpy(h.controller.get(), &RemoteDesktopController::viewerStateChanged);

    h.controller->requestView(kPeerUser, {}, kPeerDevice);
    QCOMPARE(static_cast<int>(h.controller->viewerState()), static_cast<int>(ViewerState::Inviting));
    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Invite));
    const QString sessionId = h.lastSent().sessionId;

    Signal reject;
    reject.type = Type::Reject;
    reject.sessionId = sessionId;
    reject.reason = reasonBusy();
    h.controller->handleIncomingText(kPeerUser, encodePeerSignal(reject));

    QCOMPARE(static_cast<int>(h.controller->viewerState()), static_cast<int>(ViewerState::Failed));
    QCOMPARE(stateSpy.last().at(1).toString(), reasonBusy());
}

void RemoteDesktopControllerTest::stopsSharingOnPeerStop() {
    Harness h(HostMode::Unattended);
    QSignalSpy stoppedSpy(h.controller.get(), &RemoteDesktopController::sharingStopped);

    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), kPassword));

    Signal stop;
    stop.type = Type::Stop;
    stop.sessionId = QStringLiteral("s1");
    h.controller->handleIncomingText(kPeerUser, encodePeerSignal(stop));

    QCOMPARE(h.engine->stopCalls, 1);
    QCOMPARE(stoppedSpy.count(), 1);
    QCOMPARE(static_cast<int>(h.controller->hostState()), static_cast<int>(HostState::Idle));
}

void RemoteDesktopControllerTest::reportsFailureWhenScreenShareCannotStart() {
    Harness h(HostMode::Unattended);
    h.engine->failNextStart = true;

    h.controller->handleIncomingText(
        kPeerUser, h.inviteText(QStringLiteral("s1"), QStringLiteral("room-1"), kPassword));

    // 进房失败必须如实回拒，否则对端会一直卡在"连接中"。
    QCOMPARE(static_cast<int>(h.lastSent().type), static_cast<int>(Type::Reject));
    QCOMPARE(static_cast<int>(h.controller->hostState()), static_cast<int>(HostState::Idle));
}

void RemoteDesktopControllerTest::doesNotResendInviteWhileOneIsPending() {
    Harness h(HostMode::Attended);

    h.controller->requestView(kPeerUser, {}, kPeerDevice);
    QCOMPARE(h.sent.size(), 1);

    // 用户连点两下 🖥 不应该发两次邀请，也不能把已有会话的 sessionId 冲掉。
    const QString firstSessionId = h.lastSent().sessionId;
    h.controller->requestView(kPeerUser, {}, kPeerDevice);
    QCOMPARE(h.sent.size(), 1);
    QCOMPARE(h.lastSent().sessionId, firstSessionId);
}

void RemoteDesktopControllerTest::forwardsRemoteVideoHandlerToEngine() {
    Harness h(HostMode::Attended);
    h.controller->requestView(kPeerUser, {}, kPeerDevice);

    QString seenUser;
    bool seenAvailable = false;
    h.controller->setRemoteVideoHandler([&](const QString& userId, bool available) {
        seenUser = userId;
        seenAvailable = available;
    });
    QVERIFY(h.engine->remoteVideoCallback != nullptr);

    // 模拟 SDK 通知远端画面到达 → UI 层据此才去绑渲染窗口。
    h.engine->remoteVideoCallback(RemoteDesktopSignals::mediaUserId(kPeerUser, kPeerDevice), true);
    QCOMPARE(seenUser, kPeerUser);
    QVERIFY(seenAvailable);

    h.controller->bindRemoteView(kPeerUser, nullptr);
    QCOMPARE(h.engine->bindCalls, 1);
    QCOMPARE(h.engine->lastBoundUserId, RemoteDesktopSignals::mediaUserId(kPeerUser, kPeerDevice));
}

void RemoteDesktopControllerTest::forwardsRemoteVideoSizeToHandler() {
    Harness h(HostMode::Attended);
    h.controller->requestView(kPeerUser, {}, kPeerDevice);

    int seenWidth = 0;
    int seenHeight = 0;
    h.controller->setRemoteVideoSizeHandler([&](const QString&, int width, int height) {
        seenWidth = width;
        seenHeight = height;
    });
    QVERIFY2(h.engine->remoteVideoSizeCallback != nullptr,
             "控制器没有把尺寸回调注册到引擎上");

    // 接收端编码帧回调可能是 2560x1600（16:10）；控制器必须原样转给 UI，
    // 不能继续拿写死的 1920x1080 算 surface 外层黑边。
    h.engine->remoteVideoSizeCallback(RemoteDesktopSignals::mediaUserId(kPeerUser, kPeerDevice),
                                      2560, 1600);
    QCOMPARE(seenWidth, 2560);
    QCOMPARE(seenHeight, 1600);
}

void RemoteDesktopControllerTest::discoversComputersViaIMWithoutOpeningRoom() {
    Harness viewer(HostMode::Attended, false);
    viewer.controller->discoverDevices(kPeerUser);
    QCOMPARE(viewer.lastSent().type, Type::Discover);
    QVERIFY(viewer.lastSent().targetDeviceId.isEmpty());
    QCOMPARE(viewer.lastSent().sourceDeviceId, kHostDevice);
    QCOMPARE(viewer.controller->viewerState(), ViewerState::Idle);
    Harness host(HostMode::Attended, false, QStringLiteral("computer-b"));
    host.controller->handleIncomingText(kPeerUser,
                                        RemoteDesktopSignals::encodeSignal(viewer.lastSent()));
    QCOMPARE(host.lastSent().type, Type::Device);
    QCOMPARE(host.lastSent().targetDeviceId, kHostDevice);
    QCOMPARE(host.controller->hostState(), HostState::Idle);
    QCOMPARE(host.lastSent().sourceDeviceId, QStringLiteral("computer-b"));
    viewer.controller->handleIncomingText(kPeerUser,
                                          RemoteDesktopSignals::encodeSignal(host.lastSent()));
    QCOMPARE(viewer.controller->discoveredDevices().size(), 1);
    viewer.controller->cancelDiscovery();
    auto late = host.lastSent();
    late.sourceDeviceId = QStringLiteral("late-computer");
    viewer.controller->handleIncomingText(kPeerUser, RemoteDesktopSignals::encodeSignal(late));
    QCOMPARE(viewer.controller->discoveredDevices().size(), 1);
}
void RemoteDesktopControllerTest::targetsOnlyTheSelectedComputer() {
    Harness selected(HostMode::Attended, false);
    Harness sibling(HostMode::Attended, false, QStringLiteral("sibling-computer"));
    QSignalSpy chosen(selected.controller.get(), &RemoteDesktopController::consentRequested);
    QSignalSpy other(sibling.controller.get(), &RemoteDesktopController::consentRequested);
    const auto invite =
        selected.inviteText(QStringLiteral("owned-session"), QStringLiteral("owned-room"));
    selected.controller->handleIncomingText(kPeerUser, invite);
    sibling.controller->handleIncomingText(kPeerUser, invite);
    QCOMPARE(chosen.count(), 1);
    QCOMPARE(other.count(), 0);
    QVERIFY(sibling.sent.isEmpty());
    QCOMPARE(sibling.controller->hostState(), HostState::Idle);
}
void RemoteDesktopControllerTest::ignoresSiblingRejectionAndWrongAccount() {
    Harness viewer(HostMode::Attended, false);
    viewer.controller->requestView(kPeerUser, {}, kPeerDevice);
    Signal rejection;
    rejection.type = Type::Reject;
    rejection.sessionId = viewer.lastSent().sessionId;
    rejection.sourceDeviceId = QStringLiteral("sibling-phone");
    rejection.reason = QStringLiteral("Not a shareable computer");
    viewer.controller->handleIncomingText(kPeerUser, encodePeerSignal(rejection));
    QCOMPARE(viewer.controller->viewerState(), ViewerState::Inviting);
    rejection.sourceDeviceId = kPeerDevice;
    viewer.controller->handleIncomingText(QStringLiteral("another-account"),
                                          encodePeerSignal(rejection));
    QCOMPARE(viewer.controller->viewerState(), ViewerState::Inviting);
    Signal accept;
    accept.type = Type::Accept;
    accept.sessionId = rejection.sessionId;
    accept.roomId = QStringLiteral("owned-room");
    viewer.controller->handleIncomingText(kPeerUser, encodePeerSignal(accept));
    QCOMPARE(viewer.controller->viewerState(), ViewerState::Connecting);
}
void RemoteDesktopControllerTest::ignoresStopFromUnselectedDevice() {
    Harness host(HostMode::Attended, false);
    host.controller->handleIncomingText(
        kPeerUser, host.inviteText(QStringLiteral("owned-session"), QStringLiteral("owned-room")));
    Signal stop;
    stop.type = Type::Stop;
    stop.sessionId = QStringLiteral("owned-session");
    stop.sourceDeviceId = QStringLiteral("sibling-phone");
    host.controller->handleIncomingText(kPeerUser, encodePeerSignal(stop));
    QCOMPARE(host.controller->hostState(), HostState::AwaitingConsent);
    stop.sourceDeviceId = kPeerDevice;
    host.controller->handleIncomingText(kPeerUser, encodePeerSignal(stop));
    QCOMPARE(host.controller->hostState(), HostState::Idle);
    // 无端点的 v1 邀请仅被消费，不能重新弹授权窗。
    auto legacy = host.lastSent();
    legacy.type = Type::Invite;
    legacy.protocolVersion = 1;
    host.controller->handleIncomingText(kPeerUser, encodePeerSignal(legacy));
    QCOMPARE(host.controller->hostState(), HostState::Idle);
}
void RemoteDesktopControllerTest::devicePickerRequiresExplicitSelection() {
    Harness viewer(HostMode::Attended, false);
    RemoteDesktopDeviceDialog picker(*viewer.controller, kPeerUser);
    picker.show();
    auto *list = picker.findChild<QListWidget *>(QStringLiteral("remoteDeviceList"));
    auto *button = picker.findChild<QPushButton *>(QStringLiteral("remoteDeviceConnect"));
    QVERIFY(list && button);
    QTRY_COMPARE(viewer.lastSent().type, Type::Discover);
    Signal device;
    device.type = Type::Device;
    device.sourceDeviceId = kPeerDevice;
    device.deviceName = QStringLiteral("Owned Windows computer");
    device.platform = QStringLiteral("Windows");
    device.sessionId = viewer.lastSent().sessionId;
    device.canShareScreen = device.available = true;
    viewer.controller->handleIncomingText(kPeerUser, encodePeerSignal(device));
    QTRY_COMPARE(list->count(), 1);
    QVERIFY(!button->isEnabled());
    list->setCurrentRow(0);
    QVERIFY(button->isEnabled());
    QCOMPARE(picker.selectedDeviceId(), kPeerDevice);
    const auto capture = qEnvironmentVariable("MAICHAT_REMOTE_DEVICE_CAPTURE");
    if (!capture.isEmpty())
        QVERIFY(picker.grab().save(capture));
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(picker.result(), int(QDialog::Accepted));
    QCOMPARE(viewer.controller->viewerState(), ViewerState::Idle);
}

void RemoteDesktopControllerTest::sameAccountDevicesUseDistinctMediaParticipants() {
    Harness phone(HostMode::Attended, false, QStringLiteral("owned-phone-device"));
    Harness mac(HostMode::Attended, false, QStringLiteral("owned-mac-device"));
    phone.controller->discoverDevices(kHostUser);
    const auto discover = phone.lastSent();
    phone.controller->handleIncomingText(kHostUser, RemoteDesktopSignals::encodeSignal(discover));
    QCOMPARE(phone.sent.size(), 1); // 自己发出的发现帧不能当成另一台电脑来回应。
    mac.controller->handleIncomingText(kHostUser, RemoteDesktopSignals::encodeSignal(discover));
    QCOMPARE(mac.lastSent().type, Type::Device);
    phone.controller->handleIncomingText(kHostUser,
                                         RemoteDesktopSignals::encodeSignal(mac.lastSent()));
    QCOMPARE(phone.controller->discoveredDevices().size(), 1);
    phone.controller->requestView(kHostUser, {}, mac.controller->localDeviceId());
    mac.controller->handleIncomingText(kHostUser,
                                       RemoteDesktopSignals::encodeSignal(phone.lastSent()));
    QCOMPARE(mac.controller->hostState(), HostState::AwaitingConsent);
    mac.controller->resolveConsent(true);
    phone.controller->handleIncomingText(kHostUser,
                                         RemoteDesktopSignals::encodeSignal(mac.lastSent()));
    QCOMPARE(phone.controller->viewerState(), ViewerState::Connecting);
    QVERIFY(!mac.engine->lastMediaUserId.isEmpty());
    QVERIFY(mac.engine->lastMediaUserId != phone.engine->lastMediaUserId);
    QCOMPARE(mac.engine->lastMediaUserId,
             RemoteDesktopSignals::mediaUserId(kHostUser, mac.controller->localDeviceId()));
    QCOMPARE(phone.engine->lastMediaUserId,
             RemoteDesktopSignals::mediaUserId(kHostUser, phone.controller->localDeviceId()));
    phone.controller->bindRemoteView(kHostUser, nullptr);
    QCOMPARE(phone.engine->lastBoundUserId, mac.engine->lastMediaUserId);
    phone.controller->stopSession();
    mac.controller->handleIncomingText(kHostUser,
                                       RemoteDesktopSignals::encodeSignal(phone.lastSent()));
    QCOMPARE(mac.controller->hostState(), HostState::Idle);
}


void RemoteDesktopControllerTest::devicePickerSwitchesToOwnAccountAndDiscardsOldReplies() {
    Harness viewer(HostMode::Attended, false);
    int sequence = 0;
    viewer.controller->setIdGenerator([&sequence] { return QString::number(++sequence); });
    RemoteDesktopDeviceDialog picker(*viewer.controller, kPeerUser);
    picker.show();
    auto* scope = picker.findChild<QComboBox*>(QStringLiteral("remoteDeviceAccountScope"));
    auto* list = picker.findChild<QListWidget*>(QStringLiteral("remoteDeviceList"));
    auto* button = picker.findChild<QPushButton*>(QStringLiteral("remoteDeviceConnect"));
    QVERIFY(scope && list && button);
    QTRY_COMPARE(viewer.lastSent().type, Type::Discover);
    const QString oldQuery = viewer.lastSent().sessionId;
    QCOMPARE(scope->count(), 2);
    scope->setCurrentIndex(1);
    QCOMPARE(picker.selectedPeerId(), kHostUser);
    QCOMPARE(viewer.sent.last().peerId, kHostUser);
    QVERIFY(viewer.lastSent().sessionId != oldQuery);
    Signal device;
    device.type = Type::Device;
    device.sessionId = oldQuery;
    device.canShareScreen = device.available = true;
    viewer.controller->handleIncomingText(kPeerUser, encodePeerSignal(device));
    QCOMPARE(list->count(), 0);
    device.sessionId = viewer.lastSent().sessionId;
    device.sourceDeviceId = QStringLiteral("owned-other-computer");
    device.deviceName = QStringLiteral("Owned Mac");
    device.platform = QStringLiteral("macOS");
    viewer.controller->handleIncomingText(kHostUser, encodePeerSignal(device));
    QTRY_COMPARE(list->count(), 1);
    QVERIFY(!button->isEnabled());
    list->setCurrentRow(0);
    QVERIFY(button->isEnabled());
    const QString capture = qEnvironmentVariable("MAICHAT_REMOTE_DEVICE_CAPTURE");
    if (!capture.isEmpty()) QVERIFY(picker.grab().save(capture + QStringLiteral("-my-devices.png")));
    device.available = false;
    viewer.controller->handleIncomingText(kHostUser, encodePeerSignal(device));
    QVERIFY(!button->isEnabled());
    QVERIFY(picker.selectedDeviceId().isEmpty());
}


void RemoteDesktopControllerTest::rejectsMissingOrMismatchedMediaCredentialBeforeInviting() {
    Harness viewer(HostMode::Attended, false);
    viewer.controller->setMediaCredential(QStringLiteral("another-device"), QStringLiteral("owned-sig"),
        QDateTime::currentSecsSinceEpoch() + 1800);
    viewer.controller->requestView(kPeerUser, {}, kPeerDevice);
    QCOMPARE(viewer.controller->viewerState(), ViewerState::Failed);
    QVERIFY(viewer.sent.isEmpty());
    QCOMPARE(viewer.engine->viewCalls, 0);
    viewer.controller->setMediaCredential(RemoteDesktopSignals::mediaUserId(kHostUser, kHostDevice),
        QStringLiteral("owned-sig"), QDateTime::currentSecsSinceEpoch() - 1);
    viewer.controller->requestView(kPeerUser, {}, kPeerDevice);
    QVERIFY(viewer.sent.isEmpty());
}

QTEST_MAIN(RemoteDesktopControllerTest)
#include "RemoteDesktopControllerTest.moc"
