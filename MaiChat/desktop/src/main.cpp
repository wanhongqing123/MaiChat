// 文件作用：桌面端应用功能；main 负责桌面程序入口、服务初始化和事件循环，本文件给出其实现。
// 直接依赖：app/RemoteIMApplication.h、auth/DesktopAuthClient.h、logging/AppLog.h。
#include "ui/UiZoom.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QHash>
#include <QStandardPaths>
#include <QTimer>
#include <memory>
#include <utility>
#include <curl/curl.h>

#include "app/RemoteIMApplication.h"
#include "auth/DesktopAuthClient.h"
#include "agent/DesktopSessionSyncCoordinator.h"
#include "agent/DesktopSessionSyncChannel.h"
#include "logging/AppLog.h"
#include "platform/DesktopRemoteIMClientFactory.h"
#include "storage/LocalMessageDatabase.h"

#ifdef Q_OS_WIN
#include <shobjidl.h>
#endif
#include "ui/LoginDialog.h"
#include "ui/AppMessageDialog.h"
#include "ui/MainWindow.h"

namespace {

// 每账号一个本地消息库：登录后先从这里恢复全部历史（SDK 漫游只有几条，
// 降级为补充源）。目录约定与 TimSdk 缓存一致（AppDataLocation）。
QString messageDatabasePath(const QString& userId) {
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.maichat-desktop");
    return QDir(root).filePath(QStringLiteral("RemoteIMHistory/") + userId + QStringLiteral("/messages.db"));
}

}  // namespace

int main(int argc, char* argv[]) {
    // Qt 6 默认启用高 DPI；保留分数缩放，避免 150% 等比例被取整。
    QApplication::setHighDpiScaleFactorRoundingPolicy(
        Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

#ifdef Q_OS_WIN
    // 文字渲染走 DirectWrite，替换默认的 GDI 字体引擎。GDI 引擎只有 8 级
    // 灰度抗锯齿，且在 150% 这类分数缩放下按浮点位置摆放字形（横向无
    // hinting），黑字明显发灰发糊；DirectWrite 的栅格化质量对齐 Electron/
    // 原生控件。仍然不是 ClearType 次像素渲染（当前 raster 管线做不到），
    // 保持 Qt 6 的 DirectWrite 字体路径。注意：QSS 里的 font-weight 也别超过
    // 700——微软雅黑/Segoe UI 的真字面只到 Bold(700)，更高会触发合成假粗体，
    // 笔画交汇处断裂、粗细不均。必须与上面一样在 QApplication 之前设置。
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "windows:fontengine=directwrite");
    }
#endif

#ifdef Q_OS_WIN
    // 必须在 QApplication 之前：Windows 只在进程还没创建过窗口时接受这个设置。
    //
    // 没有它，系统托盘的通知在 Windows 10/11 上会被静默丢弃——代码照跑、日志照打、
    // 屏幕上什么都不出现。Windows 需要靠这个 id 把通知归属到某个"应用"，
    // 而这个 id 必须和开始菜单快捷方式上写的那一个完全一致（见 windows-installer.nsi）。
    SetCurrentProcessExplicitAppUserModelID(L"com.kongshang.maichat");
#endif

    QApplication app(argc, argv);
    // 独立应用身份：数据落 %APPDATA%\MaiChat\Desktop IM（macOS 同理在 Application Support 下），
    // 与其它应用的数据树分开。
    QApplication::setApplicationName(QStringLiteral("Desktop IM"));
    QApplication::setOrganizationName(QStringLiteral("MaiChat"));
    const QStringList arguments = QCoreApplication::arguments();
    const bool smokeMode = arguments.contains(QStringLiteral("--smoke"));
#ifdef MAICHAT_VERSION
    QApplication::setApplicationVersion(QStringLiteral(MAICHAT_VERSION));
#endif

    // 必须在上面三行之后：日志目录是按组织名/应用名推导的。放在这里也保证了
    // 后续所有 qInfo/qWarning（含 Qt 自身的）都能落盘——GUI 子系统下 stderr
    // 没有去处，不接管的话双击启动等于完全没有日志。
    AppLog::install();

    // IM 附件和头像的 HTTPS 传输走共享 libcurl；报告实际 TLS 后端，避免把 Qt
    // 的 supportsSsl() 误当成附件下载可用性的证据。
    const curl_version_info_data* curl = curl_version_info(CURLVERSION_NOW);
    const QString sslBackend = curl && curl->ssl_version ?
        QString::fromLatin1(curl->ssl_version) : QStringLiteral("unavailable");
    qInfo().noquote() << QStringLiteral("[im] libcurl TLS backend=%1").arg(sslBackend);
    if (!sslBackend.startsWith(QStringLiteral("OpenSSL/")))
        qWarning().noquote() << QStringLiteral("[im] bundled libcurl/OpenSSL unavailable");

    // 桌面端字体栈使用 "Inter", "Noto Sans SC", "PingFang SC", "Microsoft YaHei"。
    //
    // Inter/Noto/JetBrains Mono/Sarasa Fixed SC 随包打进资源（:/maichat/fonts/，SIL OFL 1.1），
    // 不依赖系统装没装：Inter 拉丁字形的现代感是 Electron 观感的主要来源，
    // Noto Sans SC 补上中文的 Medium(500)——雅黑只有 400/700 两档，500 会触发
    // 假粗体；Noto 有真 Medium/Bold，QSS 与代码里设的字重都落在真实字面上。
    // Inter/JetBrains Mono 是 latin 子集，缺的字形（箭头、全角标点等）由列表
    // 后面的中文字体接住；雅黑/苹方垫底，字体注册万一失败也不至于开天窗。
    // 代码单独使用完整 Sarasa Fixed SC：同一字面覆盖 ASCII、中文、线框和箭头，
    // 避免 Latin 子集与 CJK 回退字面的 advance 不兼容；正文的字体列表不改变。
    const QString bundledFonts[] = {
        QStringLiteral(":/maichat/fonts/Inter-latin-400.ttf"),
        QStringLiteral(":/maichat/fonts/Inter-latin-500.ttf"),
        QStringLiteral(":/maichat/fonts/Inter-latin-600.ttf"),
        QStringLiteral(":/maichat/fonts/Inter-latin-700.ttf"),
        QStringLiteral(":/maichat/fonts/NotoSansSC-Regular.otf"),
        QStringLiteral(":/maichat/fonts/NotoSansSC-Medium.otf"),
        QStringLiteral(":/maichat/fonts/NotoSansSC-Bold.otf"),
        QStringLiteral(":/maichat/fonts/NotoSerifSC-Regular.otf"),
        QStringLiteral(":/maichat/fonts/NotoSerifSC-Medium.otf"),
        QStringLiteral(":/maichat/fonts/NotoSerifSC-SemiBold.otf"),
        QStringLiteral(":/maichat/fonts/NotoSerifSC-Bold.otf"),
        QStringLiteral(":/maichat/fonts/JetBrainsMono-latin-400.ttf"),
        QStringLiteral(":/maichat/fonts/JetBrainsMono-latin-700.ttf"),
        QStringLiteral(":/maichat/fonts/SarasaFixedSC-Regular.ttf"),
    };
    for (const QString& path : bundledFonts) {
        if (QFontDatabase::addApplicationFont(path) < 0) {
            qWarning().noquote() << QStringLiteral("[font] bundled font failed to load: %1").arg(path);
        }
    }

    QFont appFont;
    // 全局关闭字形提示（PreferNoHinting），这是和字体栈绑定的一条决策：
    //
    // Noto Sans SC 的 OTF 是 CFF 轮廓、**没有任何手工 hinting 指令**，DirectWrite
    // 对它只能自动 hinting——小字号粗体上会产生「断墨」伪影：横画中段墨色变浅、
    // 笔画宽度周期性波动，一眼看去笔画像断了。关掉 hinting 走纯抗锯齿后笔画
    // 连续均匀（边缘略软，macOS 一贯就是这种风格，可接受）。
    //
    // 注意这条的前史：GDI 引擎 + 雅黑时代这里曾试过 PreferVerticalHinting 修拉丁
    // 字距，结果全界面发虚，撤了；后来 markdown 里那条也在换 DirectWrite 后删了
    // （见 MarkdownLayout.cpp）。**hinting 的取舍跟着「引擎 × 字体」组合走**：
    //   GDI  + 雅黑（TrueType 手工 hinting） → 默认全提示最好
    //   DW   + 雅黑                        → 默认即可
    //   DW   + Noto CFF（无 hinting 指令）  → NoHinting 才不坏
    // 换字体栈或引擎时，重验三件事：字距、粗体笔画连续性、小字锐度。
    appFont.setHintingPreference(QFont::PreferNoHinting);
    // 应用普通文字统一使用用户所选字体；系统默认保留平台回退。
    appFont.setFamilies(UiZoom::fontFamilies());
    appFont.setPixelSize(UiZoom::textPixels(UiZoom::TextRole::Body));
    app.setFont(appFont);

    DesktopAuthClient auth;
    LoginDialog loginDialog(&auth);
    if (smokeMode) QTimer::singleShot(300, &loginDialog, &QDialog::reject);
    else QTimer::singleShot(0, &loginDialog, &LoginDialog::start);
    if (loginDialog.exec() != QDialog::Accepted) {
        return 0;
    }

    const DesktopImCredential credential = auth.imCredential();
    if (!credential.isValid() || credential.userId != auth.authenticatedUserId()) {
        qCritical("[auth] invalid IM credential after desktop login");
        return 1;
    }
    RemoteIMApplication remoteIM(credential.userId,
                                 createDesktopRemoteIMClient(),
                                 std::make_unique<LocalMessageDatabase>(messageDatabasePath(credential.userId)));
    remoteIM.setAccountProfileProvider([&auth] {
        RemoteIMApplication::AccountProfile profile;
        profile.accountId = auth.maiChatAccountId();
        profile.imUserId = auth.authenticatedUserId();
        profile.loggedIn = !profile.accountId.isEmpty() && !profile.imUserId.isEmpty();
        profile.nickname = auth.nickname();
        profile.phoneMasked = auth.phoneMasked();
        return profile;
    });
    remoteIM.setAvatarAccessProvider([&auth, access = auth.mediaServiceAccessProvider()] {
        const auto media = access();
        return RemoteIMApplication::AvatarAccess{
            auth.maiChatAccountId(), auth.authenticatedUserId(),
            auth.authenticatedSessionIdentity(), media.first, media.second};
    });
    MainWindow window(remoteIM, credential.sdkAppId,
                      [&auth](const QString& userId) {
        const DesktopImCredential current = auth.imCredential();
        return current.isValid() && current.userId == userId ? current.userSig : QString();
    }, auth.arkApiKeyProvider(), auth.bailianCredentialProvider());
    window.setMediaServiceAccessProvider(auth.mediaServiceAccessProvider());
    window.setFriendQrProvider([&auth](MainWindow::FriendQrCompletion completion) {
        auth.fetchFriendQr(std::move(completion));
    });
    window.setMaiChatAccountIdProvider([&auth] { return auth.maiChatAccountId(); });
    window.setMaiChatPasswordSetter(
        [&auth](const QString& password, DesktopAuthClient::PasswordCompletion completion) {
            auth.setPassword(password, std::move(completion));
        }, [&auth] { return auth.authenticatedSessionIdentity(); });
    window.setVideoCircleAccessProvider(auth.videoCircleAccessProvider());
    window.setMaiChatNicknameProvider([&auth] { return auth.nickname(); });
    remoteIM.setNicknameUpdater([&auth](const QString& nickname,
                                        RemoteIMCompletion completion) {
        auth.updateNickname(nickname, std::move(completion));
    });
    window.setMaiChatNicknameUpdater([&remoteIM](const QString& nickname,
                                                  RemoteIMCompletion completion) {
        remoteIM.updateNickname(nickname, std::move(completion));
    });
    RemoteIMApplication::AccountFriendOperations friendOperations;
    friendOperations.list = [&auth](auto completion) {
        auth.fetchFriends([completion](const QList<DesktopFriend>& friends,
                                       const QString& error) {
            QList<RemoteIMApplication::AccountFriend> mapped;
            for (const DesktopFriend& friendAccount : friends)
                mapped.append({friendAccount.accountId, friendAccount.nickname,
                               friendAccount.imUserId});
            completion(mapped, error);
        });
    };
    friendOperations.requests = [&auth](auto completion) {
        auth.fetchFriendRequests([completion](const QList<DesktopFriendRequest>& requests,
                                              const QString& error) {
            QList<RemoteIMApplication::AccountFriendRequest> mapped;
            for (const DesktopFriendRequest& request : requests)
                mapped.append({request.id, request.accountId, request.nickname,
                               request.imUserId});
            completion(mapped, error);
        });
    };
    friendOperations.send = [&auth](const QString& account, RemoteIMCompletion completion) {
        auth.requestFriend(account, std::move(completion));
    };
    friendOperations.decide = [&auth](const QString& id, bool accept,
                                      RemoteIMCompletion completion) {
        auth.decideFriendRequest(id, accept, std::move(completion));
    };
    friendOperations.remove = [&auth](const QString& account, RemoteIMCompletion completion) {
        auth.removeFriend(account, std::move(completion));
    };
    remoteIM.setAccountFriendOperations(std::move(friendOperations));
    bool invalidatingSession = false;
    auth.setCredentialUpdatedHandler([&](const DesktopImCredential& renewed) {
        if (invalidatingSession || renewed.userId != credential.userId) return;
        const QString renewedSig = renewed.userSig;
        const int renewedSdkAppId = renewed.sdkAppId;
        remoteIM.disconnectFromService(
            [&, renewedSig, renewedSdkAppId](bool, const QString&) {
            const DesktopImCredential current = auth.imCredential();
            if (!invalidatingSession && current.isValid() &&
                current.userSig == renewedSig && current.userId == credential.userId) {
                remoteIM.connectToService(renewedSdkAppId, renewedSig);
            }
        });
    });
    auth.setSessionInvalidatedHandler([&](const QString& reason) {
        if (invalidatingSession) return;
        invalidatingSession = true;
        window.prepareForAccountDisconnect([&, reason] {
            auto finished = std::make_shared<bool>(false);
            auto showAndClose = [&window, reason, finished] {
                if (*finished) return;
                *finished = true;
                AppMessageDialog::show(&window, AppMessageDialog::Kind::Warning,
                                       QStringLiteral("登录已失效"), reason);
                window.finishAccountDisconnect();
                QCoreApplication::quit();
            };
            remoteIM.disconnectFromService([showAndClose](bool, const QString&) {
                showAndClose();
            });
            QTimer::singleShot(3000, &window, showAndClose);
        });
    });
    window.setLogoutHandler([&] {
        if (invalidatingSession) return;
        invalidatingSession = true;
        window.prepareForAccountDisconnect([&] {
            auto started = std::make_shared<bool>(false);
            auto revokeAndClose = [&, started] {
                if (*started) return;
                *started = true;
                auth.logout([&window](bool ok, const QString& error) {
                    if (!ok) {
                        AppMessageDialog::show(
                            &window, AppMessageDialog::Kind::Warning,
                            QStringLiteral("本机已退出"),
                            QStringLiteral("本机登录已清除，服务端撤销未确认：%1").arg(error));
                    }
                    window.finishAccountDisconnect();
                    QCoreApplication::quit();
                });
            };
            remoteIM.disconnectFromService([revokeAndClose](bool, const QString&) {
                revokeAndClose();
            });
            QTimer::singleShot(3000, &window, revokeAndClose);
        });
    });
    bool modelKeysInFlight = false;
    const auto refreshAccountModelKeys = [&] {
        if (invalidatingSession || modelKeysInFlight) return;
        modelKeysInFlight = true;
        auth.fetchAccountModelKeys([&](const QHash<QString, QString>& keys,
                                   const QString& error) {
            modelKeysInFlight = false;
            if (!invalidatingSession) window.setAccountModelKeys(keys, error);
        });
    };
    window.setModelKeyRefreshHandler(refreshAccountModelKeys);
    window.show();
    DesktopSessionSyncChannel sessionChannel(auth.sessionSyncSocketUrl(),
                                             auth.sessionSyncAccessTokenProvider());
    DesktopSessionSyncCoordinator sessionSync(
        [&sessionChannel](const QString& path, const QByteArray& method, const QJsonObject& body,
                DesktopSessionSyncCoordinator::Completion completion) {
            sessionChannel.request(path, method, body, std::move(completion));
        },
        [&window] { return window.activeAgentController(); });
    sessionChannel.setCatalogHandler([&sessionSync](const QJsonArray& sessions) {
        sessionSync.acceptCatalog(sessions);
    });
    sessionChannel.setDeviceHandler([&sessionSync](const QJsonArray& devices) {
        sessionSync.acceptDevices(devices);
    });
    sessionChannel.setInboxHandler([&sessionSync](const QJsonArray& messages) {
        sessionSync.acceptInbox(messages);
    });
    sessionChannel.setConnectedHandler([&sessionSync] { sessionSync.connectionReady(); });
    sessionChannel.setDisconnectedHandler([&sessionSync] { sessionSync.connectionLost(); });
    window.setSessionSyncCoordinator(&sessionSync);
    if (!smokeMode) {
        sessionSync.start();
        sessionChannel.start();
    }
    if (!smokeMode) refreshAccountModelKeys();
    remoteIM.connectToService(credential.sdkAppId, credential.userSig);
    auth.startSessionMonitoring();
    auth.startModelCredentialMonitoring();

    if (smokeMode) {
        QTimer::singleShot(300, &app, &QCoreApplication::quit);
    }

    return app.exec();
}
