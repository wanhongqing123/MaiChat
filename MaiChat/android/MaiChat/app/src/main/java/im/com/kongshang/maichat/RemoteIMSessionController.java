// 文件作用：Android IM 身份、消息同步与本地历史。
// RemoteIMSessionController 负责流程编排、异步回调和状态转换，本文件给出其实现。
// 主要入口：enqueueIO、switchAccount、persistContact、updateHumanTyping。
package com.kongshang.maichat;

import android.content.Context;
import android.os.Handler;
import android.os.Looper;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;

public final class RemoteIMSessionController implements MaiSessionIMBindingService.Host {
    static final String FALLBACK_OWNER_USER_ID = "android-user";
    static final String DEFAULT_CONTACT_USER_ID = "mac-office";
    private static final long SESSION_CHECK_INTERVAL_MS = 30_000L;
    static final long AVATAR_SDK_TIMEOUT_MS = 60_000L;

    public interface Listener {
        void onStateChanged();
        void onError(String message);
        default void onActivityChanged(String peerId) { }

        default void onNewIncomingMessage(
            RemoteIMMessage message,
            boolean conversationVisible
        ) {
        }
    }

    public interface BroadcastCompletion {
        void onFinished(int total, List<String> failedUserIds);
    }

    public interface AccountCompletion {
        void onFinished(String error);
    }

    /** 只有公开资料；accountId 是 MaiChat 账号 ID，imUserId 是独立的 IM 身份。 */
    public static final class CurrentAccount {
        public final boolean loggedIn;
        public final String accountId, imUserId, nickname, phoneMasked, avatarUrl;

        public CurrentAccount(boolean loggedIn, String accountId, String imUserId,
                       String nickname, String phoneMasked, String avatarUrl) {
            this.loggedIn = loggedIn;
            this.accountId = accountId;
            this.imUserId = imUserId;
            this.nickname = nickname;
            this.phoneMasked = phoneMasked;
            this.avatarUrl = avatarUrl;
        }

        public static CurrentAccount loggedOut() {
            return new CurrentAccount(false, null, null, null, null, null);
        }
    }

    public interface AccountProfileCompletion {
        void onFinished(CurrentAccount profile, String error);
    }

    public interface AvatarCompletion {
        void onFinished(String avatarUrl, String error);
    }

    public interface AccountTask { void cancel(); }

    public interface MessageHistoryCompletion {
        void onFinished(RemoteIMMessageHistory.Page page, String error);
    }

    public interface CodeCompletion {
        void onFinished(String challengeId, String error);
    }

    public interface SocialLoginCompletion {
        void onFinished(MaiChatAuthClient.SocialLoginResult result, String error);
    }

    public interface GroupCreationCompletion {
        void onFinished(RemoteIMGroup group, boolean activated,
                        boolean newlyCreated, String error);
    }

    public interface GroupSendCompletion {
        void onFinished(RemoteIMGroupMessage message, String error);
    }

    public interface GroupHistoryCompletion {
        void onFinished(String error);
    }

    private final LocalSettingsStore settingsStore;
    private final Context appContext;
    private final MaiChatAuthClient authClient;
    private final LocalChatHistoryStore legacyHistoryStore;
    private final AndroidChatHistoryStore historyStore;
    private final AndroidIMIngressWriter ingressWriter;
    private final TencentIMClient client;
    private final RemoteIMGroupCreationCoordinator groupCreation;
    private final RemoteDesktopController remoteDesktop;
    private final Listener listener;
    private final Handler mainHandler;
    private final Map<String, Integer> unreadByUserId = new HashMap<>();
    private final Map<String, TencentIMClient.PresenceStatus> presenceByUserId = new HashMap<>();
    private final Map<String, Boolean> hasEarlierByUserId = new HashMap<>();
    private final Map<String, Long> oldestLoadedCreatedAtByUserId = new HashMap<>();
    private final Map<String, String> oldestLoadedMessageIdByUserId = new HashMap<>();
    private final RemoteIMSelfProfileState selfProfileState = new RemoteIMSelfProfileState();
    private final boolean productionMode;

    private volatile RemoteIMSettings settings;
    private volatile MaiChatAuthClient.Credential imCredential;
    private volatile ChatState chatState;
    private volatile boolean destroyed;
    // 密码等账号绑定写入也会在 worker 开始前检查代际，切号必须对该线程立即可见。
    private volatile int accountGeneration;
    private volatile MaiChatAcceptedAccountCleanup.Pending acceptedAccountDeletion;
    private final java.util.Set<AccountRequest> accountRequests = new java.util.HashSet<>();
    private final java.util.Set<MessageHistoryRequest> messageHistoryRequests = new java.util.HashSet<>();
    // 历史查询不能排在网络、上传或媒体任务后；队列有界，每次请求同时限制排队和 SQL 时间。
    private static final long MESSAGE_HISTORY_TIMEOUT_MS = 5_000L;
    private static final java.util.concurrent.ScheduledThreadPoolExecutor MESSAGE_HISTORY_TIMEOUTS =
        createMessageHistoryTimeouts();

    private static java.util.concurrent.ScheduledThreadPoolExecutor createMessageHistoryTimeouts() {
        // 全进程只保留一个独立时钟；主线程繁忙时仍能取消 SQLite 和清除排队任务。
        java.util.concurrent.ScheduledThreadPoolExecutor timers = new java.util.concurrent.ScheduledThreadPoolExecutor(1,
            action -> { Thread thread = new Thread(action, "MaiChat-HistoryTimeout"); thread.setDaemon(true); return thread; });
        timers.setRemoveOnCancelPolicy(true);
        return timers;
    }

    private final java.util.concurrent.ThreadPoolExecutor messageHistoryExecutor =
        new java.util.concurrent.ThreadPoolExecutor(1, 1, 0L, java.util.concurrent.TimeUnit.MILLISECONDS,
            new java.util.concurrent.ArrayBlockingQueue<>(8));
    // 由主线程保留到 SDK 回执或超时；上传 worker 返回不代表头像资料写入已经结束。
    private AccountRequest avatarRequest;
    private volatile String selfAvatarUrl;
    interface AvatarUploadOperation {
        String upload(File source, String accountId, long authGeneration,
                      java.util.function.BooleanSupplier current,
                      java.util.function.Consumer<MaiAccountAvatar.Scope> uploadScope) throws Exception;
    }
    private AvatarUploadOperation avatarUploadOperation = this::prepareAndUploadAvatar;
    private final Map<String, RemoteIMVideoAttachment> pendingVideoMedia = new java.util.LinkedHashMap<>();
    private int mediaRevision;
    public int mediaRevision() { return mediaRevision; }
    private boolean restoringAccount;
    private final java.util.concurrent.ExecutorService ioExecutor =
            java.util.concurrent.Executors.newSingleThreadExecutor();
    private final java.util.Set<String> loadingPeers = new java.util.HashSet<>();
    private final Map<String, Integer> pageRequests = new HashMap<>();
    private final Map<String, Integer> peerEpochs = new HashMap<>();
    private final java.util.Set<String> acceptedFriendUserIds = new java.util.HashSet<>();
    private volatile java.util.Set<String> bindingFriendSnapshot = Collections.emptySet();
    private volatile boolean acceptedFriendsLoaded;
    private volatile MaiSessionIMBindingService sessionIMBindings;
    private final java.util.Set<String> pendingFriendSyncUserIds = new java.util.HashSet<>();
    private final Map<String, String> friendAccountByIm = new HashMap<>();
    private final Map<String, String> friendDisplayByIm = new HashMap<>();
    private final Map<String, String> friendRequestIdByIm = new HashMap<>();
    private final Map<String, RemoteIMGroup> groupsById = new java.util.LinkedHashMap<>();
    private final Map<String, List<RemoteIMGroupMessage>> groupMessagesById = new HashMap<>();
    private final Map<String, Integer> unreadByGroupId = new HashMap<>();
    private final Map<String, Boolean> hasEarlierByGroupId = new HashMap<>();
    private final Map<String, String> groupHistoryCursorById = new HashMap<>();
    private final java.util.Set<String> loadingGroupIds = new java.util.HashSet<>();
    private final Map<String, Integer> groupPageRequests = new HashMap<>();
    private final Map<String, String> groupSenderNames = new HashMap<>();
    private final java.util.Set<String> requestedGroupSenderNames = new java.util.HashSet<>();
    private final java.util.Set<String> synchronizedGroupIds = new java.util.HashSet<>();
    private final Map<String, List<GroupHistoryCompletion>> groupHistoryWaiters = new HashMap<>();
    private final Map<String, GroupSendCompletion> groupSendWaiters = new HashMap<>();
    private String visibleGroupId = "";
    private int groupMembershipRequest;
    private interface IOAction { void run() throws Exception; }

    private TencentIMClient.ConnectionState connectionState = TencentIMClient.ConnectionState.DISCONNECTED;
    private String connectionDetail = "未连接";
    private String visibleConversationUserId = "";
    private Runnable pendingStateNotification;
    private final RemoteIMActivityState activities = new RemoteIMActivityState();
    private Runnable activityExpiry;
    private Runnable typingIdle;
    private String typingPeer = "", typingId = "";
    private long typingSequence, typingLastSent;
    private boolean appForeground;
    private final Runnable sessionValidation = new Runnable() {
        @Override public void run() {
            if (destroyed || !appForeground || requiresLogin() || restoringAccount) return;
            int generation = accountGeneration;
            ioExecutor.execute(() -> {
                try {
                    String previousNickname = authClient.nickname();
                    authClient.validateSession();
                    if (!previousNickname.equals(authClient.nickname()))
                        runOnMain(() -> {
                            if (generation == accountGeneration) notifyStateChanged();
                        });
                } catch (Exception failure) {
                    if (authClient.hasSession()) return;
                    runOnMain(() -> {
                        if (generation != accountGeneration || requiresLogin()) return;
                        switchAccount(RemoteIMSettings.empty());
                        reportError("登录已过期，请重新登录");
                    });
                }
            });
            mainHandler.postDelayed(this, SESSION_CHECK_INTERVAL_MS);
        }
    };


    public RemoteIMSessionController(
        LocalSettingsStore settingsStore,
        LocalChatHistoryStore historyStore
    ) {
        this.settingsStore = settingsStore;
        this.appContext = null;
        this.authClient = null;
        this.legacyHistoryStore = historyStore;
        this.historyStore = null;
        this.ingressWriter = null;
        this.client = null;
        this.groupCreation = null;
        this.remoteDesktop = null;
        this.listener = null;
        this.mainHandler = null;
        this.productionMode = false;
        settings = loadSettings();
        chatState = loadLegacyChatState();
        ensureDefaultContactIfNeeded();
    }

    public RemoteIMSessionController(Context context, Listener listener) {
        Context applicationContext = context.getApplicationContext();
        this.appContext = applicationContext;
        this.settingsStore = new LocalSettingsStore(
            new java.io.File(applicationContext.getFilesDir(), "remote-im-settings/settings.properties")
        );
        this.authClient = MaiChatAuthClient.shared(applicationContext);
        this.legacyHistoryStore = null;
        this.historyStore = new AndroidChatHistoryStore(applicationContext);
        this.ingressWriter = new AndroidIMIngressWriter(this.historyStore, ioExecutor);
        this.listener = listener;
        this.mainHandler = new Handler(Looper.getMainLooper());
        this.productionMode = true;
        this.authClient.setSessionTerminationListener(message -> runOnMain(() -> {
            if (!requiresLogin()) switchAccount(RemoteIMSettings.empty());
            reportError(message);
        }));
        this.client = new TencentIMClient(applicationContext, new ClientListener());
        this.groupCreation = new RemoteIMGroupCreationCoordinator((name,members,completion) ->
            client.createGroup(name,members,new TencentIMClient.GroupCreationCompletion() {
                @Override public void onCreated(RemoteIMGroup group,boolean activated,
                                                 String activationError) {
                    runOnMain(() -> completion.onCreated(group,activated,activationError));
                }
                @Override public void onError(int code,String message) {
                    runOnMain(() -> completion.onError(code,message));
                }
            }));
        this.remoteDesktop = new RemoteDesktopController(
            applicationContext,
            client,
            this::notifyStateChanged
        );
        settings = RemoteIMSettings.empty();
        chatState = new ChatState(FALLBACK_OWNER_USER_ID);
        restoringAccount = true;
        int generation = ++accountGeneration;
        enqueueIO(() -> {
            List<MaiChatAcceptedAccountCleanup.Pending> pendingDeletions;
            try { pendingDeletions = MaiChatAcceptedAccountCleanup.pending(appContext.getNoBackupFilesDir()); }
            catch (Exception invalidCleanupRecord) { pendingDeletions = Collections.emptyList(); }
            // 已受理账号在恢复登录前离线清 token；另一个账号的设置和认证不能受影响。
            RemoteIMSettings cachedBeforeRestore = loadSettings();
            for (MaiChatAcceptedAccountCleanup.Pending pending : pendingDeletions)
                if (pending.owner.equals(cachedBeforeRestore.loginUserId()) && pending.account.equals(authClient.accountId())) {
                    authClient.discardLocalSession(); settingsStore.save(RemoteIMSettings.empty());
                }
            List<MaiChatAcceptedAccountCleanup.Pending> cleanupAfterPresentation = pendingDeletions;
            // 旧版只存 IM UserID 的设置不能代表已验证手机号；以服务端会话为准。
            RemoteIMSettings restoredSettings = RemoteIMSettings.empty();
            if (authClient.hasSession()) {
                RemoteIMSettings cached = loadSettings();
                try {
                    restoredSettings = new RemoteIMSettings(authClient.credential().userId);
                    if (!restoredSettings.equals(cached)) settingsStore.save(restoredSettings);
                } catch (Exception ignored) {
                    // 断网可显示已验证会话的本地记录，但 IM 连接仍须先领取服务端凭证。
                    if (authClient.hasSession()) restoredSettings = cached;
                }
            }
            RemoteIMSettings accountToRestore = restoredSettings;
            ChatState restored = readProductionChatState(accountToRestore);
            List<RemoteIMGroup> restoredGroups = accountToRestore.requiresLogin()
                ? Collections.emptyList() : historyStore.loadGroups(accountToRestore.loginUserId());
            List<RemoteIMGroupMessage> groupSummaries = accountToRestore.requiresLogin()
                ? Collections.emptyList() : historyStore.loadGroupSummaries(accountToRestore.loginUserId());
            runOnMain(() -> {
                if (generation != accountGeneration) return;
                settings = accountToRestore; chatState = restored; restoringAccount = false;
                restoreGroupCache(restoredGroups, groupSummaries);
                AIAssistantController.configureAccount(accountToRestore.loginUserId());
                notifyStateChanged();
                if (!requiresLogin()) connect();
                scheduleSessionValidation();
                // 大目录和 SQLite 清理排在页面恢复之后，不延迟登录页展示。
                for (MaiChatAcceptedAccountCleanup.Pending pending : cleanupAfterPresentation)
                    ioExecutor.execute(() -> cleanAcceptedAccount(pending, null));
            });
        });
    }

    int accountGeneration() { return accountGeneration; }
    boolean acceptsOutgoing(int generation, String owner, String peer) {
        return !destroyed && !requiresLogin() && generation == accountGeneration
            && chatState.ownerUserId().equals(owner) && contactExists(peer);
    }

    public boolean isRestoringAccount() { return restoringAccount; }
    public boolean isLoadingMessages(String peer) { return loadingPeers.contains(clean(peer)); }

    private void enqueueIO(IOAction action) {
        if (destroyed) return;
        int generation = accountGeneration;
        ioExecutor.execute(() -> {
            try { action.run(); }
            catch (Exception error) {
                runOnMain(() -> {
                    if (generation != accountGeneration) return;
                    restoringAccount = false;
                    reportError("本地数据操作失败：" + error.getMessage());
                    notifyStateChanged();
                });
            }
        });
    }

    private void switchAccount(RemoteIMSettings next) {
        if (productionMode) AIAssistantController.configureAccount(next.loginUserId());
        resetActivities();
        int generation = ++accountGeneration;
        cancelAccountRequests();
        selfAvatarUrl = null;
        settings = next;
        imCredential = null;
        acceptedFriendUserIds.clear();
        bindingFriendSnapshot = Collections.emptySet(); acceptedFriendsLoaded = false;
        pendingFriendSyncUserIds.clear();
        friendAccountByIm.clear();
        friendDisplayByIm.clear();
        friendRequestIdByIm.clear();
        chatState = new ChatState(next.requiresLogin() ? FALLBACK_OWNER_USER_ID : next.loginUserId());
        selfProfileState.activate(chatState.ownerUserId(), nickname());
        // 同号重新登录也属于新代际，与公开头像缓存一起清除旧资料。
        selfProfileState.accept(new RemoteIMContact(chatState.ownerUserId(), nickname(), ""));
        unreadByUserId.clear(); presenceByUserId.clear(); hasEarlierByUserId.clear();
        oldestLoadedCreatedAtByUserId.clear(); oldestLoadedMessageIdByUserId.clear();
        loadingPeers.clear(); pageRequests.clear(); peerEpochs.clear(); pendingVideoMedia.clear(); mediaRevision++;
        visibleConversationUserId = "";
        visibleGroupId = "";
        groupsById.clear(); groupMessagesById.clear(); unreadByGroupId.clear();
        hasEarlierByGroupId.clear(); groupHistoryCursorById.clear();
        loadingGroupIds.clear(); groupPageRequests.clear();
        synchronizedGroupIds.clear();
        groupSenderNames.clear(); requestedGroupSenderNames.clear();
        for (List<GroupHistoryCompletion> waiters : groupHistoryWaiters.values())
            for (GroupHistoryCompletion completion : waiters) completion.onFinished("账号已切换");
        groupHistoryWaiters.clear();
        for (GroupSendCompletion completion : groupSendWaiters.values())
            completion.onFinished(null,"账号已切换");
        groupSendWaiters.clear();
        groupMembershipRequest++;
        if (groupCreation != null) groupCreation.reset();
        restoringAccount = !next.requiresLogin();
        scheduleSessionValidation();
        if (next.requiresLogin()) {
            if (remoteDesktop != null) remoteDesktop.stop();
            client.disconnect(new EmptyOperationCompletion());
            connectionState = TencentIMClient.ConnectionState.DISCONNECTED;
            connectionDetail = "未连接";
        }
        enqueueIO(() -> {
            settingsStore.save(next);
            if (next.requiresLogin()) return;
            ChatState restored = readProductionChatState(next);
            List<RemoteIMGroup> restoredGroups = historyStore.loadGroups(next.loginUserId());
            List<RemoteIMGroupMessage> groupSummaries = historyStore.loadGroupSummaries(next.loginUserId());
            runOnMain(() -> {
                if (generation != accountGeneration) return;
                chatState = restored; restoringAccount = false;
                restoreGroupCache(restoredGroups, groupSummaries);
                connect(); notifyStateChanged();
                scheduleSessionValidation();
            });
        });
        notifyStateChanged();
    }

    private void persistContact(RemoteIMContact contact) {
        String owner = chatState.ownerUserId();
        enqueueIO(() -> historyStore.upsertContact(owner, contact));
    }

    public RemoteIMActivitySignal activity(String peerId) { return activities.get(clean(peerId)); }

    public void updateHumanTyping(String peerId, boolean active) {
        if (!productionMode) return;
        String peer = clean(peerId);
        if (!active || !peer.equals(typingPeer)) stopHumanTyping();
        if (!active || peer.isEmpty() || connectionState != TencentIMClient.ConnectionState.CONNECTED) return;
        long now = android.os.SystemClock.elapsedRealtime();
        if (typingId.isEmpty()) {
            typingId = "typing:" + java.util.UUID.randomUUID();
            typingPeer = peer; typingSequence = 0; typingLastSent = now - 3000;
        }
        if (now - typingLastSent >= 3000) {
            typingLastSent = now;
            client.sendActivity(peer, new RemoteIMActivitySignal(typingId, ++typingSequence,
                RemoteIMActivitySignal.Kind.HUMAN_TYPING, true, 12000));
        }
        if (typingIdle != null) mainHandler.removeCallbacks(typingIdle);
        typingIdle = this::stopHumanTyping;
        mainHandler.postDelayed(typingIdle, 4000);
    }

    public void stopHumanTyping() {
        if (!productionMode) return;
        if (typingIdle != null) mainHandler.removeCallbacks(typingIdle);
        typingIdle = null;
        if (!typingId.isEmpty() && connectionState == TencentIMClient.ConnectionState.CONNECTED) {
            client.sendActivity(typingPeer, new RemoteIMActivitySignal(typingId, ++typingSequence,
                RemoteIMActivitySignal.Kind.HUMAN_TYPING, false, 1000));
        }
        typingId = ""; typingPeer = "";
    }

    private void resetActivities() {
        stopHumanTyping();
        if (activityExpiry != null) mainHandler.removeCallbacks(activityExpiry);
        activityExpiry = null;
        activities.reset();
    }

    private void scheduleActivityExpiry() {
        if (activityExpiry != null) mainHandler.removeCallbacks(activityExpiry);
        long next = activities.nextExpiry();
        if (next == Long.MAX_VALUE) { activityExpiry = null; return; }
        activityExpiry = () -> {
            for (String peer : activities.expire(android.os.SystemClock.elapsedRealtime())) {
                if (listener != null) listener.onActivityChanged(peer);
            }
            scheduleActivityExpiry();
        };
        mainHandler.postDelayed(activityExpiry, Math.max(1, next - android.os.SystemClock.elapsedRealtime()));
    }

    public RemoteIMSettings settings() {
        return settings;
    }

    public String accountId() {
        return productionMode ? authClient.accountId() : settings.loginUserId();
    }

    public String nickname() {
        return productionMode ? authClient.nickname() : "";
    }

    /** 本人身份使用 IM owner；账号昵称优先，头像未取得时由现有头像控件回退。 */
    public RemoteIMContact selfProfile() {
        selfProfileState.activate(chatState.ownerUserId(), nickname());
        return selfProfileState.current();
    }

    /** Picker, crop UI and host tools bind the whole operation to this login generation. */
    public String avatarChangeIdentity() {
        return destroyed || !productionMode || requiresLogin() ? ""
            : accountId() + "|" + accountGeneration + "|" + authClient.accountSessionGeneration()
                + "|" + chatState.ownerUserId();
    }
    public boolean isAvatarChangeIdentityCurrent(String expectedIdentity) {
        return expectedIdentity != null && !expectedIdentity.isEmpty()
            && expectedIdentity.equals(avatarChangeIdentity());
    }
    /** 普通照片选择只适配正式账号事务，和工具共用上传、SDK 确认及并发门禁。 */
    public void uploadSelfAvatar(java.io.File image, String expectedIdentity, AccountCompletion completion) {
        if (mainHandler != null && Looper.myLooper() != Looper.getMainLooper()) {
            mainHandler.post(() -> uploadSelfAvatar(image, expectedIdentity, completion)); return;
        }
        if (!isAvatarChangeIdentityCurrent(expectedIdentity)) {
            runAvatarCompletion(() -> completion.onFinished("登录账号已变化，请重新选择头像")); return;
        }
        setSelfAvatar(image, () -> isAvatarChangeIdentityCurrent(expectedIdentity),
            (url, error) -> runAvatarCompletion(() -> completion.onFinished(error)));
    }
    private void runAvatarCompletion(Runnable action) {
        // 即使 Activity/Session 已销毁也交付清理回调；current 仍阻止资料安装和 SDK 写入。
        if (mainHandler == null || Looper.myLooper() == Looper.getMainLooper()) action.run();
        else mainHandler.post(action);
    }

    private interface AccountFailure { void fail(String message); }

    // 一个请求同时绑定控制器代际、鉴权代际、公开账号和 IM 身份；同账号重登也不复用旧请求。
    private final class AccountRequest implements AccountTask {
        final int generation = accountGeneration;
        final long authGeneration = authClient == null ? -1 : authClient.accountSessionGeneration();
        final String account = accountId();
        final String imUser = settings.loginUserId();
        final java.util.function.BooleanSupplier scope;
        final java.util.function.BooleanSupplier externalCancellation;
        final AccountFailure failure;
        final java.util.concurrent.atomic.AtomicBoolean finished =
            new java.util.concurrent.atomic.AtomicBoolean();
        private MaiAccountAvatar.Scope upload;
        private volatile boolean cancellationRequested;
        private volatile boolean sdkWriteStarted;
        private Runnable sdkTimeout;

        AccountRequest(java.util.function.BooleanSupplier scope, AccountFailure failure) {
            this(scope, () -> false, failure);
        }

        AccountRequest(java.util.function.BooleanSupplier scope,
                       java.util.function.BooleanSupplier externalCancellation, AccountFailure failure) {
            this.scope = scope;
            this.externalCancellation = externalCancellation;
            this.failure = failure;
        }

        boolean isCurrent() {
            return !finished.get() && !cancellationRequested &&
                !externalCancellation.getAsBoolean() && identityIsCurrent();
        }

        boolean identityIsCurrent() {
            return accountIdentityIsCurrent() && scope.getAsBoolean();
        }

        boolean accountIdentityIsCurrent() {
            return !destroyed && productionMode && !requiresLogin() &&
                generation == accountGeneration && authGeneration == authClient.accountSessionGeneration() &&
                !account.isEmpty() && account.equals(accountId()) &&
                imUser.equals(settings.loginUserId());
        }

        synchronized void attach(MaiAccountAvatar.Scope value) {
            upload = value;
            if (value != null && finished.get()) value.cancel();
        }

        void succeed(Runnable completion) {
            runOnMain(() -> {
                boolean current = isCurrent();
                if (!finished.compareAndSet(false, true)) return;
                accountRequests.remove(this);
                if (!sdkWriteStarted) releaseAvatarRequest(this);
                if (current) completion.run();
                else failure.fail("登录账号或工作区已变化，请重新操作");
            });
        }

        void reject(String message) {
            runOnMain(() -> cancelOnMain(message, true));
        }

        void cancelOnMain(String message) {
            cancelOnMain(message, true);
        }

        void cancelOnMain(String message, boolean identityChangedOrFailed) {
            cancellationRequested = true;
            // IM 写入一旦发送就无法撤回；普通停止等 SDK 回执，不能把已更新的头像报成未更新。
            if (!identityChangedOrFailed && sdkWriteStarted && !finished.get()) return;
            if (!finished.compareAndSet(false, true)) return;
            synchronized (this) { if (upload != null) upload.cancel(); }
            accountRequests.remove(this);
            if (!sdkWriteStarted) releaseAvatarRequest(this);
            failure.fail(message);
        }

        void sdkSending() {
            sdkWriteStarted = true;
            runOnMain(() -> {
                if (avatarRequest != this) return;
                sdkTimeout = () -> {
                    releaseAvatarRequest(this);
                    cancelOnMain("IM 头像更新回执超时，请先查询当前头像再操作", true);
                };
                mainHandler.postDelayed(sdkTimeout, AVATAR_SDK_TIMEOUT_MS);
            });
        }

        void rejectSdk(String message) {
            runOnMain(() -> {
                releaseAvatarRequest(this);
                cancelOnMain(message, true);
            });
        }

        void confirmAvatar(Runnable completion) {
            runOnMain(() -> {
                releaseAvatarRequest(this);
                boolean current = accountIdentityIsCurrent();
                if (!finished.compareAndSet(false, true)) return;
                accountRequests.remove(this);
                if (current) completion.run();
                else failure.fail("登录账号或工作区已变化，请重新操作");
            });
        }

        @Override public void cancel() {
            cancellationRequested = true;
            synchronized (this) { if (upload != null) upload.cancel(); }
            runOnMain(() -> cancelOnMain("账号资料操作已取消", false));
        }
    }

    private void cancelAccountRequests() {
        for (AccountRequest request : new ArrayList<>(accountRequests))
            request.cancelOnMain("登录账号已变化，请重新操作");
        for (MessageHistoryRequest request : new ArrayList<>(messageHistoryRequests))
            request.reject("登录账号已变化，请重新操作");
    }

    /** 普通业务与 Agent 共用：主线程捕获账号，独立 worker 查询，主线程校验后交付。 */
    public AccountTask queryPersistedMessages(RemoteIMMessageHistory.Query query,
            MessageHistoryCompletion completion) {
        return queryPersistedMessages(query, () -> true, completion);
    }

    public AccountTask queryPersistedMessages(RemoteIMMessageHistory.Query query,
            java.util.function.BooleanSupplier scope, MessageHistoryCompletion completion) {
        MessageHistoryRequest request = new MessageHistoryRequest(query, scope, completion);
        // 在主线程交接前计时，普通代码调用也不能依靠 Agent 的七秒等待来兜底。
        request.timeoutFuture = MESSAGE_HISTORY_TIMEOUTS.schedule(
            () -> request.reject("本机消息查询超时"), MESSAGE_HISTORY_TIMEOUT_MS, java.util.concurrent.TimeUnit.MILLISECONDS);
        runHistoryCompletion(() -> {
            if (request.finished.get()) return;
            if (request.cancellation.isCanceled()) { request.finish(null, "本机消息查询已取消"); return; }
            if (destroyed || requiresLogin() || historyStore == null || !scope.getAsBoolean()) {
                request.finish(null, "本机消息数据库或登录账号不可用"); return;
            }
            request.owner = settings.loginUserId();
            request.loginScope = messageHistoryLoginScope();
            messageHistoryRequests.add(request);
            long remaining = request.deadline - android.os.SystemClock.uptimeMillis();
            if (remaining <= 0) { request.reject("本机消息查询超时"); return; }
            try {
                request.future = messageHistoryExecutor.submit(() -> {
                    if (!request.current()) { request.reject("登录账号已变化或查询已取消"); return; }
                    try {
                        RemoteIMMessageHistory.Page page = historyStore.queryMessages(request.owner,
                            request.loginScope, query, request.cancellation);
                        runHistoryCompletion(() -> request.finish(page, null));
                    } catch (IllegalArgumentException invalid) {
                        request.reject("消息筛选或分页游标无效");
                    } catch (android.os.OperationCanceledException canceled) {
                        request.reject("本机消息查询已取消");
                    } catch (Exception failure) {
                        // 数据库异常不能成为成功的空结果，也不能把 SQL/消息内容写入日志。
                        request.reject("本机消息数据库查询失败");
                    }
                });
            } catch (java.util.concurrent.RejectedExecutionException busy) {
                request.reject("本机消息查询繁忙，请稍后重试");
            }
        });
        return request;
    }

    private String messageHistoryLoginScope() {
        return accountGeneration + ":" + (authClient == null ? -1 : authClient.accountSessionGeneration());
    }

    /** 保留页面供普通业务使用时也可检查账号；不读取可变的聊天窗口或联系人缓存。 */
    public boolean isMessageHistoryCurrent(RemoteIMMessageHistory.Page page) {
        return page != null && !destroyed && !requiresLogin() && page.owner.equals(settings.loginUserId())
            && page.loginScope.equals(messageHistoryLoginScope()) && historyStore != null
            && historyStore.isMessageHistoryCurrent(page);
    }

    private void runHistoryCompletion(Runnable completion) {
        // 销毁和取消仍须结束等待者；runOnMain 的 destroyed 丢弃规则不适用于只读查询清理。
        if (mainHandler == null || Looper.myLooper() == Looper.getMainLooper()) completion.run();
        else mainHandler.post(completion);
    }

    private final class MessageHistoryRequest implements AccountTask {
        final android.os.CancellationSignal cancellation = new android.os.CancellationSignal();
        final java.util.concurrent.atomic.AtomicBoolean finished = new java.util.concurrent.atomic.AtomicBoolean();
        final java.util.function.BooleanSupplier scope;
        final MessageHistoryCompletion completion;
        final long deadline = android.os.SystemClock.uptimeMillis() + MESSAGE_HISTORY_TIMEOUT_MS;
        volatile java.util.concurrent.ScheduledFuture<?> timeoutFuture;
        volatile java.util.concurrent.Future<?> future;
        volatile String owner, loginScope;

        MessageHistoryRequest(RemoteIMMessageHistory.Query query,
                java.util.function.BooleanSupplier scope, MessageHistoryCompletion completion) {
            if (query == null || scope == null || completion == null)
                throw new IllegalArgumentException("History query and completion are required");
            this.scope = scope; this.completion = completion;
        }

        boolean current() {
            return !finished.get() && !cancellation.isCanceled() && !destroyed && !requiresLogin()
                && android.os.SystemClock.uptimeMillis() < deadline && scope.getAsBoolean()
                && owner != null && owner.equals(settings.loginUserId())
                && loginScope.equals(messageHistoryLoginScope());
        }

        void finish(RemoteIMMessageHistory.Page page, String error) {
            if (error == null && (!current() || !isMessageHistoryCurrent(page)))
                error = "登录账号或数据库已变化，或查询已超时";
            if (!finished.compareAndSet(false, true)) return;
            cancelTimeout();
            messageHistoryRequests.remove(this);
            completion.onFinished(error == null ? page : null, error);
        }

        void reject(String error) {
            cancelTimeout();
            cancellation.cancel();
            java.util.concurrent.Future<?> task = future;
            if (task != null) {
                task.cancel(true);
                if (task instanceof Runnable) messageHistoryExecutor.remove((Runnable) task);
            }
            runHistoryCompletion(() -> finish(null, error));
        }

        void cancelTimeout() {
            java.util.concurrent.ScheduledFuture<?> timer = timeoutFuture;
            if (timer != null) { timer.cancel(false); timeoutFuture = null; }
        }

        @Override public void cancel() { reject("本机消息查询已取消"); }
    }

    private void releaseAvatarRequest(AccountRequest request) {
        if (request.sdkTimeout != null) {
            mainHandler.removeCallbacks(request.sdkTimeout);
            request.sdkTimeout = null;
        }
        if (avatarRequest == request) avatarRequest = null;
    }

    public AccountTask getCurrentAccount(AccountProfileCompletion completion) {
        return getCurrentAccount(() -> true, completion);
    }

    /** 普通资料入口与 Agent 共用；只读当前公开缓存，不发鉴权或 IM SDK 查询。
     * 账号验证和 IM 本人资料监听按原流程刷新缓存，离线时仍可读取上次同步结果。
     */
    public AccountTask getCurrentAccount(java.util.function.BooleanSupplier scope,
                                          AccountProfileCompletion completion) {
        if (destroyed) {
            completion.onFinished(null, "账号会话已关闭"); return () -> { };
        }
        if (requiresLogin()) {
            completion.onFinished(CurrentAccount.loggedOut(), null); return () -> { };
        }
        if (!productionMode) {
            completion.onFinished(new CurrentAccount(true, accountId(), settings.loginUserId(),
                null, null, null), null);
            return () -> { };
        }
        AccountRequest request = new AccountRequest(scope,
            error -> completion.onFinished(null, error));
        runOnMain(() -> {
            if (!request.isCurrent()) {
                request.cancelOnMain("登录账号或工作区已变化，请重新操作"); return;
            }
            accountRequests.add(request);
            try {
                MaiChatAuthClient.AccountProfile profile = authClient.currentAccountProfile(
                    request.account, request.authGeneration);
                // 升级前的公开缓存没有 IM 字段；本机会话已经绑定的 IM ID 可补足该字段，
                // 但缓存明确属于其他 IM 身份时必须拒绝，不能把两份账号资料拼在一起。
                if (!profile.imUserId.isEmpty() && !request.imUser.equals(profile.imUserId))
                    throw new IOException("IM 身份不一致");
                String avatar = selfAvatarUrl;
                request.succeed(() -> completion.onFinished(new CurrentAccount(true,
                    profile.accountId, request.imUser, profile.nickname, profile.phoneMasked,
                    avatar == null || avatar.isEmpty() ? null : avatar), null));
            } catch (Exception failure) {
                request.reject("当前账号资料缓存不可用，请检查登录状态");
            }
        });
        return request;
    }

    public AccountTask setSelfAvatar(File source, AvatarCompletion completion) {
        return setSelfAvatar(source, () -> true, completion);
    }

    /** 图片准备、共享上传和 SDK 写入共用这一条路径；成功意味着 SDK 已确认 faceURL。 */
    public AccountTask setSelfAvatar(File source, java.util.function.BooleanSupplier scope,
                                      AvatarCompletion completion) {
        return setSelfAvatar(source, scope, () -> false, completion);
    }

    public AccountTask setSelfAvatar(File source, java.util.function.BooleanSupplier scope,
                                      java.util.function.BooleanSupplier canceled,
                                      AvatarCompletion completion) {
        if (destroyed || !productionMode || requiresLogin()) {
            completion.onFinished(null, "请先登录 MaiChat 账号"); return () -> { };
        }
        AccountRequest request = new AccountRequest(scope, canceled,
            error -> completion.onFinished(null, error));
        runOnMain(() -> {
            if (!request.isCurrent() || !client.isLoggedInAs(request.imUser)) {
                request.cancelOnMain("请等待当前账号 IM 登录后再更新头像"); return;
            }
            if (avatarRequest != null) {
                request.cancelOnMain("已有头像修改正在处理中，请等待回执后再操作"); return;
            }
            avatarRequest = request;
            accountRequests.add(request);
            ioExecutor.execute(() -> {
                try {
                    if (!request.isCurrent()) throw new IOException("账号已变化");
                    String avatar = avatarUploadOperation.upload(source, request.account,
                        request.authGeneration,
                        () -> request.isCurrent() && client.isLoggedInAs(request.imUser), request::attach);
                    if (!request.isCurrent() || !client.isLoggedInAs(request.imUser))
                        throw new IOException("账号已变化");
                    String confirmedUrl = avatar;
                    client.setSelfAvatar(request.imUser, avatar, request::isCurrent,
                        request::accountIdentityIsCurrent, request::sdkSending,
                        new TencentIMClient.OperationCompletion() {
                            @Override public void onSuccess() {
                                request.confirmAvatar(() -> {
                                    installSelfProfile(request.imUser, selfProfile().displayName(), confirmedUrl);
                                    notifyStateChanged();
                                    completion.onFinished(confirmedUrl, null);
                                });
                            }
                            @Override public void onError(int code, String message) {
                                request.rejectSdk("IM 尚未确认头像更新，请检查登录和网络");
                            }
                        });
                } catch (Exception | LinkageError failure) {
                    // 不转发签名 URL、鉴权响应或底层 HTTP 异常文本到 Agent。
                    request.reject("头像尚未更新，请检查图片、登录状态和网络后重试");
                }
            });
        });
        return request;
    }

    // 正式传输仍只走图片准备和共享 C ABI；独立边界允许测试控制上传完成与 SDK 回执的次序。
    private String prepareAndUploadAvatar(File source, String accountId, long authGeneration,
            java.util.function.BooleanSupplier current,
            java.util.function.Consumer<MaiAccountAvatar.Scope> uploadScope) throws Exception {
        if (!current.getAsBoolean()) throw new IOException("账号已变化");
        byte[] jpeg = MaiAccountAvatar.prepare(source);
        if (!current.getAsBoolean()) throw new IOException("账号已变化");
        MaiChatAuthClient.MediaServiceAccess access = authClient.avatarServiceAccess(accountId, authGeneration);
        try (MaiAccountAvatar.Scope upload = new MaiAccountAvatar.Scope()) {
            uploadScope.accept(upload);
            try {
                if (!current.getAsBoolean()) throw new IOException("账号已变化");
                return MaiAccountAvatar.upload(access, accountId, jpeg, upload);
            } finally { uploadScope.accept(null); }
        }
    }

    private void installSelfProfile(String owner, String displayName, String avatarUrl) {
        selfAvatarUrl = avatarUrl;
        selfProfileState.activate(owner, nickname());
        selfProfileState.accept(new RemoteIMContact(owner, displayName, avatarUrl));
        if (connectionState == TencentIMClient.ConnectionState.CONNECTED)
            AvatarImageLoader.prefetch(avatarUrl);
    }

    /** 安全表单绑定账号和本次登录代际；同账号重新登录也不得沿用旧密码页。 */
    public String passwordChangeIdentity() {
        return !productionMode || requiresLogin() ? "" : accountId() + "|" + accountGeneration;
    }

    public void setAccountPassword(String password, String expectedIdentity, AccountCompletion completion) {
        if (!productionMode || requiresLogin() || !passwordChangeIdentity().equals(expectedIdentity)) {
            completion.onFinished("登录账号已变化，请重新打开密码页");
            return;
        }
        final int generation = accountGeneration;
        final String owner = accountId();
        ioExecutor.execute(() -> {
            String error = null;
            try {
                if (generation != accountGeneration)
                    throw new IOException("登录账号已变化，请重新打开密码页");
                authClient.setPassword(password, owner);
            } catch (Exception failure) {
                // 不转发任意异常/请求体，避免密码进入通用错误日志或 Agent 记录。
                error = "请重新登录后再修改密码".equals(failure.getMessage()) ||
                    "登录已过期，请重新登录".equals(failure.getMessage())
                    ? "请重新登录后再修改密码" : "密码修改尚未确认，请检查账号和网络后重试";
            }
            final String result = error;
            runOnMain(() -> {
                if (destroyed || generation != accountGeneration) {
                    completion.onFinished("登录账号已变化，请重新打开密码页");
                    return;
                }
                completion.onFinished(result);
            });
        });
    }

    public void setNickname(String nickname, AccountCompletion completion) {
        if (requiresLogin() || !productionMode) {
            completion.onFinished("请先登录 MaiChat 账号");
            return;
        }
        int generation = accountGeneration;
        ioExecutor.execute(() -> {
            String error = null;
            try { authClient.updateNickname(nickname); }
            catch (Exception failure) { error = failure.getMessage(); }
            String result = error;
            runOnMain(() -> {
                if (generation != accountGeneration || destroyed) return;
                if (result == null) notifyStateChanged();
                else reportError(result);
                completion.onFinished(result);
            });
        });
    }

    public String friendAccountId(String imUserId) {
        return friendAccountByIm.getOrDefault(imUserId, "");
    }

    public boolean isFriendSyncPending(String imUserId) {
        return pendingFriendSyncUserIds.contains(imUserId);
    }

    public List<RemoteIMContact> acceptedFriends() {
        if (!productionMode) return chatState.contacts();
        List<RemoteIMContact> result = new ArrayList<>();
        for (RemoteIMContact contact : chatState.contacts())
            if (acceptedFriendUserIds.contains(contact.userId())) result.add(contact);
        return result;
    }

    private void restoreGroupCache(List<RemoteIMGroup> groups,
                                   List<RemoteIMGroupMessage> summaries) {
        groupsById.clear(); groupMessagesById.clear();
        for (RemoteIMGroup group : groups) groupsById.put(group.groupId(), group);
        for (RemoteIMGroupMessage message : summaries) {
            if (!groupsById.containsKey(message.groupId())) continue;
            if (message.status() == RemoteIMGroupMessage.Status.PENDING) {
                message.updateDelivery("",RemoteIMGroupMessage.Status.FAILED);
                persistGroupMessage(message);
            }
            mergeGroupMessage(message);
        }
    }

    public List<RemoteIMGroup> groups() {
        return new ArrayList<>(groupsById.values());
    }

    public RemoteIMGroup group(String groupId) { return groupsById.get(clean(groupId)); }

    public boolean groupTransportReady() {
        return productionMode && !requiresLogin()
            && connectionState == TencentIMClient.ConnectionState.CONNECTED
            && client.isLoggedIn();
    }

    public String groupSenderName(String userId) {
        for (RemoteIMContact contact : chatState.contacts())
            if (contact.userId().equals(userId)) return contact.displayName();
        return groupSenderNames.getOrDefault(userId,userId);
    }

    private void refreshGroupSenderNames(List<RemoteIMGroupMessage> messages) {
        if (!productionMode || connectionState != TencentIMClient.ConnectionState.CONNECTED) return;
        List<String> missing = new ArrayList<>();
        for (RemoteIMGroupMessage message : messages) {
            String sender = message.senderId();
            if (sender.equals(chatState.ownerUserId()) || groupSenderNames.containsKey(sender)
                || !requestedGroupSenderNames.add(sender)) continue;
            boolean known = false;
            for (RemoteIMContact contact : chatState.contacts())
                if (contact.userId().equals(sender)) { known = true; break; }
            if (!known) missing.add(sender);
        }
        if (missing.isEmpty()) return;
        int generation = accountGeneration;
        client.groupSenderProfiles(missing,names -> runOnMain(() -> {
            if (generation != accountGeneration) return;
            groupSenderNames.putAll(names);
            for (String sender : missing) if (!names.containsKey(sender))
                requestedGroupSenderNames.remove(sender);
            notifyStateChanged();
        }));
    }

    public List<RemoteIMGroupMessage> groupMessages(String groupId) {
        return Collections.unmodifiableList(new ArrayList<>(
            groupMessagesById.getOrDefault(clean(groupId), Collections.emptyList())));
    }

    public RemoteIMGroupMessage latestGroupMessage(String groupId) {
        List<RemoteIMGroupMessage> messages = groupMessagesById.get(clean(groupId));
        return messages == null || messages.isEmpty() ? null : messages.get(messages.size() - 1);
    }

    public int groupUnreadCount(String groupId) {
        return unreadByGroupId.getOrDefault(clean(groupId), 0);
    }

    public boolean hasEarlierGroupMessages(String groupId) {
        return hasEarlierByGroupId.getOrDefault(clean(groupId), false);
    }

    public boolean isLoadingGroupMessages(String groupId) {
        return loadingGroupIds.contains(clean(groupId));
    }

    public void setGroupConversationVisible(String groupId, boolean visible) {
        String id = clean(groupId);
        if (visible) {
            visibleGroupId = id;
            unreadByGroupId.remove(id);
        } else if (visibleGroupId.equals(id)) visibleGroupId = "";
        notifyStateChanged();
    }

    public void refreshJoinedGroups() {
        if (!groupTransportReady()) return;
        int generation = accountGeneration;
        int request = ++groupMembershipRequest;
        String owner = chatState.ownerUserId();
        client.joinedGroups(new TencentIMClient.GroupsCompletion() {
            @Override public void onSuccess(List<RemoteIMGroup> groups) {
                runOnMain(() -> {
                    if (generation != accountGeneration || request != groupMembershipRequest
                        || !owner.equals(chatState.ownerUserId())) return;
                    java.util.Set<String> joined = new java.util.HashSet<>();
                    for (RemoteIMGroup group : groups) {
                        joined.add(group.groupId());
                        RemoteIMGroup existing = groupsById.get(group.groupId());
                        groupsById.put(group.groupId(), existing != null && existing.pendingActivation()
                            ? group.withPendingActivation(true) : group);
                    }
                    for (RemoteIMGroup pending : new ArrayList<>(groupsById.values()))
                        if (pending.pendingActivation()) joined.add(pending.groupId());
                    groupsById.keySet().removeIf(groupId -> !joined.contains(groupId));
                    groupMessagesById.keySet().removeIf(groupId -> !joined.contains(groupId));
                    unreadByGroupId.keySet().removeIf(groupId -> !joined.contains(groupId));
                    if (!joined.contains(visibleGroupId)) visibleGroupId = "";
                    List<RemoteIMGroup> snapshot = new ArrayList<>(groupsById.values());
                    enqueueIO(() -> historyStore.replaceGroups(owner, snapshot));
                    notifyStateChanged();
                    int requested = 0;
                    for (RemoteIMGroup group : groups) {
                        if (requested >= 20) break;
                        if (synchronizedGroupIds.add(group.groupId())) {
                            loadGroupMessages(group.groupId(), true);
                            requested++;
                        }
                    }
                });
            }
            @Override public void onError(int code, String message) {
                android.util.Log.w("MaiChat.im", "joined group refresh failed: " + code);
            }
        });
    }

    public void createGroup(String name, List<String> memberUserIds,
                            GroupCreationCompletion completion) {
        if (!productionMode || groupCreation == null) {
            completion.onFinished(null,false,false,"IM 尚未连接"); return;
        }
        int generation = accountGeneration;
        String owner = chatState.ownerUserId();
        groupCreation.create(name,memberUserIds,acceptedFriendUserIds,owner,
            groupTransportReady(),
            groupsById.values(),(group,activated,newlyCreated,error) -> {
                if (generation != accountGeneration || !owner.equals(chatState.ownerUserId())) {
                    completion.onFinished(null,false,false,"账号已切换");
                    return;
                }
                if (group != null && newlyCreated) {
                    groupMembershipRequest++;
                    groupsById.put(group.groupId(),group);
                    enqueueIO(() -> historyStore.upsertGroup(owner,group));
                    notifyStateChanged();
                }
                completion.onFinished(group,activated,newlyCreated,error);
            });
    }

    public RemoteIMGroupMessage sendGroupText(String groupId, String text) {
        return sendGroupText(groupId,text,null);
    }

    public RemoteIMGroupMessage sendGroupText(String groupId, String text,
                                               GroupSendCompletion completion) {
        String id = clean(groupId), body = clean(text);
        if (!groupsById.containsKey(id) || body.isEmpty() || !groupTransportReady()) {
            reportError("群聊未连接或消息为空");
            if (completion != null) completion.onFinished(null,"群聊未连接或消息为空");
            return null;
        }
        int generation = accountGeneration;
        String owner = chatState.ownerUserId();
        RemoteIMGroupMessage queued = new RemoteIMGroupMessage(null, "", id, owner,
            body, System.currentTimeMillis(), RemoteIMGroupMessage.Status.PENDING);
        if (completion != null) groupSendWaiters.put(queued.id(),completion);
        mergeGroupMessage(queued);
        persistGroupMessage(queued);
        notifyStateChanged();
        client.sendGroupText(id, body, new TencentIMClient.SendCompletion() {
            @Override public void onSuccess(String remoteId, long createdAtMillis) {
                runOnMain(() -> {
                    GroupSendCompletion waiter = groupSendWaiters.remove(queued.id());
                    if (generation != accountGeneration || !owner.equals(chatState.ownerUserId())) return;
                    List<RemoteIMGroupMessage> messages = groupMessagesById.get(id);
                    if (messages == null) {
                        queued.updateDelivery(remoteId,RemoteIMGroupMessage.Status.SENT);
                        if (waiter != null) enqueueIO(() -> waiter.onFinished(queued,null));
                        return;
                    }
                    if (!remoteId.isEmpty() && messages.stream().anyMatch(message ->
                        !message.id().equals(queued.id()) && remoteId.equals(message.remoteId()))) {
                        messages.removeIf(message -> message.id().equals(queued.id()));
                        enqueueIO(() -> historyStore.deleteGroupMessage(owner,queued.id()));
                    } else {
                        queued.updateDelivery(remoteId, RemoteIMGroupMessage.Status.SENT);
                        persistGroupMessage(queued);
                    }
                    RemoteIMGroup group = groupsById.get(id);
                    if (group != null && group.pendingActivation()) {
                        RemoteIMGroup activatedGroup = group.withPendingActivation(false);
                        groupsById.put(id, activatedGroup);
                        enqueueIO(() -> historyStore.upsertGroup(owner,activatedGroup));
                    }
                    notifyStateChanged();
                    if (waiter != null) {
                        RemoteIMGroupMessage delivered = queued;
                        for (RemoteIMGroupMessage message : messages)
                            if (!remoteId.isEmpty() && remoteId.equals(message.remoteId())) {
                                delivered = message; break;
                            }
                        RemoteIMGroupMessage receipt = delivered;
                        // The Agent response may end this Activity immediately. Drain the
                        // per-account DB queue before claiming the SDK-acknowledged send.
                        enqueueIO(() -> waiter.onFinished(receipt,null));
                    }
                });
            }
            @Override public void onError(int code, String message) {
                runOnMain(() -> {
                    GroupSendCompletion waiter = groupSendWaiters.remove(queued.id());
                    if (generation != accountGeneration || !owner.equals(chatState.ownerUserId())) return;
                    queued.updateDelivery("", RemoteIMGroupMessage.Status.FAILED);
                    persistGroupMessage(queued);
                    notifyStateChanged();
                    reportError(message);
                    if (waiter != null) enqueueIO(() -> waiter.onFinished(queued,message));
                });
            }
        });
        return queued;
    }

    public void loadGroupMessages(String groupId, boolean initial) {
        loadGroupMessages(groupId, initial, null);
    }

    public void loadGroupMessages(String groupId, boolean initial,
                                  GroupHistoryCompletion completion) {
        String id = clean(groupId);
        if (!groupsById.containsKey(id)) {
            if (completion != null) completion.onFinished("找不到此群聊");
            return;
        }
        if (completion != null) groupHistoryWaiters.computeIfAbsent(id,
            ignored -> new ArrayList<>()).add(completion);
        if (loadingGroupIds.contains(id)) return;
        int generation = accountGeneration;
        String owner = chatState.ownerUserId();
        int request = groupPageRequests.getOrDefault(id, 0) + 1;
        groupPageRequests.put(id, request);
        loadingGroupIds.add(id);
        notifyStateChanged();
        if (initial) {
            enqueueIO(() -> {
                List<RemoteIMGroupMessage> cached = historyStore.loadGroupPage(owner,id,null,null,20);
                runOnMain(() -> {
                    if (generation != accountGeneration || groupPageRequests.getOrDefault(id,0) != request) return;
                    for (RemoteIMGroupMessage message : cached) mergeGroupMessage(message);
                    if (!groupTransportReady()) {
                        loadingGroupIds.remove(id);
                        hasEarlierByGroupId.put(id,cached.size() == 20);
                    }
                    notifyStateChanged();
                    if (!groupTransportReady())
                        finishGroupHistoryWaiters(id,null);
                });
            });
        }
        if (!groupTransportReady()) {
            if (!initial) loadEarlierCachedGroupMessages(id, owner, generation, request);
            return;
        }
        String cursor = initial ? null : groupHistoryCursorById.get(id);
        if (!initial && (cursor == null || cursor.isEmpty())) {
            loadingGroupIds.remove(id);
            hasEarlierByGroupId.put(id, false);
            notifyStateChanged();
            finishGroupHistoryWaiters(id,null);
            return;
        }
        client.groupHistory(id, cursor, new TencentIMClient.GroupHistoryCompletion() {
            @Override public void onSuccess(TencentIMClient.GroupHistoryPage page) {
                runOnMain(() -> {
                    if (generation != accountGeneration || groupPageRequests.getOrDefault(id,0) != request) return;
                    loadingGroupIds.remove(id);
                    for (RemoteIMGroupMessage message : page.messages()) {
                        mergeGroupMessage(message); persistGroupMessage(message);
                    }
                    refreshGroupSenderNames(page.messages());
                    groupHistoryCursorById.put(id, page.nextCursor() == null ? "" : page.nextCursor());
                    hasEarlierByGroupId.put(id, page.hasMore());
                    notifyStateChanged();
                    finishGroupHistoryWaiters(id,null);
                });
            }
            @Override public void onError(int code, String message) {
                runOnMain(() -> {
                    if (generation != accountGeneration || groupPageRequests.getOrDefault(id,0) != request) return;
                    loadingGroupIds.remove(id);
                    reportError(message);
                    notifyStateChanged();
                    finishGroupHistoryWaiters(id,message);
                });
            }
        });
    }

    private void loadEarlierCachedGroupMessages(String groupId, String owner,
                                                 int generation, int request) {
        List<RemoteIMGroupMessage> loaded = groupMessagesById.getOrDefault(groupId,Collections.emptyList());
        RemoteIMGroupMessage oldest = loaded.isEmpty() ? null : loaded.get(0);
        enqueueIO(() -> {
            List<RemoteIMGroupMessage> page = historyStore.loadGroupPage(owner,groupId,
                oldest == null ? null : oldest.createdAtMillis(),oldest == null ? null : oldest.id(),20);
            runOnMain(() -> {
                if (generation != accountGeneration || groupPageRequests.getOrDefault(groupId,0) != request) return;
                loadingGroupIds.remove(groupId);
                for (RemoteIMGroupMessage message : page) mergeGroupMessage(message);
                hasEarlierByGroupId.put(groupId,page.size() == 20);
                notifyStateChanged();
                finishGroupHistoryWaiters(groupId,null);
            });
        });
    }

    private void finishGroupHistoryWaiters(String groupId, String error) {
        List<GroupHistoryCompletion> waiters = groupHistoryWaiters.remove(groupId);
        if (waiters != null) for (GroupHistoryCompletion completion : waiters)
            completion.onFinished(error);
    }

    private boolean mergeGroupMessage(RemoteIMGroupMessage incoming) {
        List<RemoteIMGroupMessage> messages = groupMessagesById.computeIfAbsent(incoming.groupId(),
            ignored -> new ArrayList<>());
        for (RemoteIMGroupMessage current : messages) {
            if (current.id().equals(incoming.id()) || (!incoming.remoteId().isEmpty()
                && incoming.remoteId().equals(current.remoteId()))) return false;
        }
        messages.add(incoming);
        messages.sort((left,right) -> {
            int byTime = Long.compare(left.createdAtMillis(),right.createdAtMillis());
            return byTime == 0 ? left.id().compareTo(right.id()) : byTime;
        });
        return true;
    }

    private void persistGroupMessage(RemoteIMGroupMessage message) {
        if (!productionMode) return;
        String owner = chatState.ownerUserId();
        enqueueIO(() -> historyStore.upsertGroupMessage(owner,message));
    }

    public ChatState chatState() {
        return chatState;
    }

    public RemoteDesktopController remoteDesktop() {
        return remoteDesktop;
    }

    public TencentIMClient.ConnectionState connectionState() {
        return connectionState;
    }

    public String connectionDetail() {
        return connectionDetail;
    }

    public void retryConnection() {
        if (productionMode && !requiresLogin()
            && connectionState != TencentIMClient.ConnectionState.CONNECTING)
            connect();
    }

    public void setAppForeground(boolean foreground) {
        if (!productionMode || destroyed) return;
        appForeground = foreground;
        scheduleSessionValidation();
    }

    private void scheduleSessionValidation() {
        if (!productionMode) return;
        mainHandler.removeCallbacks(sessionValidation);
        if (appForeground && !destroyed && !restoringAccount && !requiresLogin())
            mainHandler.post(sessionValidation);
    }

    public boolean requiresLogin() {
        return settings.requiresLogin();
    }

    public String currentUserSig() {
        MaiChatAuthClient.Credential credential = imCredential;
        return credential != null && credential.expiresAt > System.currentTimeMillis() / 1000L + 60
            ? credential.userSig : "";
    }

    public int currentSdkAppId() {
        MaiChatAuthClient.Credential credential = imCredential;
        return credential == null ? 0 : credential.sdkAppId;
    }

    public void requestAccountCode(
        String phone, boolean ageConfirmed, CodeCompletion completion
    ) {
        ioExecutor.execute(() -> {
            String error = null;
            String challengeId = null;
            try { challengeId = authClient.requestCode(phone, ageConfirmed).id; }
            catch (Exception failure) { error = failure.getMessage(); }
            String result = error;
            String issuedId = challengeId;
            runOnMain(() -> completion.onFinished(issuedId, result));
        });
    }

    public void requestSocialBindCode(
        String phone, boolean ageConfirmed, CodeCompletion completion
    ) {
        ioExecutor.execute(() -> {
            String error = null;
            String challengeId = null;
            try { challengeId = authClient.requestSocialBindCode(phone, ageConfirmed).id; }
            catch (Exception failure) { error = failure.getMessage(); }
            String result = error;
            String issuedId = challengeId;
            runOnMain(() -> completion.onFinished(issuedId, result));
        });
    }

    public void loginWithAccountCode(
        String phone, String challengeId, String code, boolean ageConfirmed,
        AccountCompletion completion
    ) {
        loginWithAccount(() -> authClient.verifyCode(phone, challengeId, code, ageConfirmed),
            completion);
    }

    public void loginWithAccountPassword(
        String account, String password, boolean ageConfirmed, AccountCompletion completion
    ) {
        loginWithAccount(() -> authClient.loginWithPassword(account, password, ageConfirmed),
            completion);
    }

    public void loginWithSocialCredential(
        String provider, String credential, boolean ageConfirmed,
        SocialLoginCompletion completion
    ) {
        ioExecutor.execute(() -> {
            String error = null;
            MaiChatAuthClient.SocialLoginResult loginResult = null;
            try {
                loginResult = authClient.loginWithSocialCredential(provider, credential, ageConfirmed);
            } catch (Exception failure) {
                error = failure.getMessage();
            }
            String resultError = error;
            MaiChatAuthClient.SocialLoginResult result = loginResult;
            runOnMain(() -> {
                if (destroyed) return;
                if (resultError == null && result.isSignedIn())
                    switchAccount(new RemoteIMSettings(result.userId));
                completion.onFinished(result, resultError);
            });
        });
    }

    public void bindSocialPhone(
        String bindingToken, String phone, String challengeId, String code,
        boolean ageConfirmed, AccountCompletion completion
    ) {
        loginWithAccount(() -> authClient.bindSocialPhone(
            bindingToken, phone, challengeId, code, ageConfirmed), completion);
    }

    private interface AccountLogin { String perform() throws Exception; }

    private void loginWithAccount(AccountLogin login, AccountCompletion completion) {
        ioExecutor.execute(() -> {
            String error = null;
            String userId = null;
            try {
                userId = login.perform();
            } catch (Exception failure) {
                error = failure.getMessage();
            }
            String result = error;
            String signedUserId = userId;
            runOnMain(() -> {
                if (destroyed) return;
                if (result == null) switchAccount(new RemoteIMSettings(signedUserId));
                completion.onFinished(result);
            });
        });
    }

    public void login(String loginUserId) throws IOException {
        if (productionMode) throw new IOException("请使用 MaiChat 账号登录");
        saveChatState();
        settings = new RemoteIMSettings(loginUserId);
        settingsStore.save(settings);
        chatState = loadLegacyChatState();
        unreadByUserId.clear();
        selfProfileState.activate(chatState.ownerUserId(), nickname());
        presenceByUserId.clear();
        hasEarlierByUserId.clear();
        oldestLoadedCreatedAtByUserId.clear();
        oldestLoadedMessageIdByUserId.clear();
        if (productionMode) {
            connect();
        } else {
            ensureDefaultContactIfNeeded();
        }
        notifyStateChanged();
    }

    /** 仅由本地安全表单收到有效 202 回执后调用，固定原 IM owner 与认证代际。worker 调用。 */
    void rememberAcceptedAccountDeletion(String owner, String account, String authIdentity, String requestId) throws Exception {
        if (!productionMode || appContext == null) throw new IOException("Account deletion requires a production account");
        acceptedAccountDeletion = new MaiChatAcceptedAccountCleanup.Pending(owner, account, authIdentity, requestId);
        MaiChatAcceptedAccountCleanup.markPending(appContext.getNoBackupFilesDir(), owner, account, authIdentity, requestId);
    }

    /** 正常 UI 确认回执后的异步离线退出；不发送已失效账号 token 的远端 logout。 */
    public void completeAcceptedAccountDeletion() {
        MaiChatAcceptedAccountCleanup.Pending accepted = acceptedAccountDeletion;
        if (!productionMode || destroyed || accepted == null) return;
        acceptedAccountDeletion = null;
        java.util.concurrent.CompletableFuture<Void> agentStopped = AIAssistantController.stopForAcceptedAccountDeletion(accepted.owner);
        if (accepted.owner.equals(chatState.ownerUserId()) && !requiresLogin()) switchAccount(RemoteIMSettings.empty());
        // Auth 清理和所有文件/数据库 I/O 都在原串行 worker；新登录不能越过该队列。
        ioExecutor.execute(() -> {
            authClient.discardAcceptedAccountSession(accepted.account, accepted.authIdentity);
            cleanAcceptedAccount(accepted, agentStopped);
        });
        notifyStateChanged();
    }

    private void cleanAcceptedAccount(MaiChatAcceptedAccountCleanup.Pending accepted,
                                     java.util.concurrent.CompletableFuture<Void> stopped) {
        try {
            MaiChatAcceptedAccountCleanup.markPending(appContext.getNoBackupFilesDir(), accepted.owner,
                accepted.account, accepted.authIdentity, accepted.requestId);
            java.util.concurrent.CompletableFuture<Void> barrier = stopped == null
                ? AIAssistantController.stopForAcceptedAccountDeletion(accepted.owner) : stopped;
            barrier.get(35, java.util.concurrent.TimeUnit.SECONDS);
            historyStore.purgeAcceptedAccount(accepted.owner);
            MaiChatAcceptedAccountCleanup.removeWorkspace(appContext.getNoBackupFilesDir(), accepted.owner);
            MaiChatAcceptedAccountCleanup.finishPending(appContext.getNoBackupFilesDir(), accepted);
        } catch (Exception failure) {
            runOnMain(() -> { if (requiresLogin()) reportError("本机账号数据清理未完成；已保存的清理记录将在下次启动重试。"); });
        }
    }

    public void logout() throws IOException {
        if (productionMode) {
            String token = authClient.discardLocalSession();
            ioExecutor.execute(() -> MaiChatAuthClient.revokeServerSession(token));
            switchAccount(RemoteIMSettings.empty());
            return;
        }
        saveChatState();
        settings = RemoteIMSettings.empty();
        settingsStore.save(settings);
        unreadByUserId.clear();
        presenceByUserId.clear();
        hasEarlierByUserId.clear();
        oldestLoadedCreatedAtByUserId.clear();
        oldestLoadedMessageIdByUserId.clear();
        visibleConversationUserId = "";
        if (productionMode) {
            if (remoteDesktop != null) remoteDesktop.stop();
            client.disconnect(new EmptyOperationCompletion());
            connectionState = TencentIMClient.ConnectionState.DISCONNECTED;
            connectionDetail = "未连接";
            chatState = new ChatState(FALLBACK_OWNER_USER_ID);
        } else {
            chatState = loadLegacyChatState();
        }
        selfProfileState.activate(chatState.ownerUserId(), nickname());
        notifyStateChanged();
    }

    public void saveChatState() throws IOException {
        if (!productionMode && !requiresLogin()) legacyHistoryStore.save(chatState);
    }

    public void addContact(String userId) {
        addContact(userId, null);
    }

    public void addContact(String userId, java.util.function.Consumer<Boolean> completion) {
        String cleanAccount = clean(userId);
        if (productionMode) {
            int generation = accountGeneration;
            if (cleanAccount.isEmpty() || requiresLogin()) {
                if (completion != null) completion.accept(false);
                return;
            }
            ioExecutor.execute(() -> {
                boolean sent = false;
                String error = null;
                try { authClient.requestFriend(cleanAccount); sent = true; }
                catch (Exception failure) { error = failure.getMessage(); }
                boolean result = sent;
                String message = error;
                runOnMain(() -> {
                    if (generation != accountGeneration) return;
                    if (message != null) reportError(message);
                    if (completion != null) completion.accept(result);
                });
            });
            return;
        }
        if (FriendQrPolicy.link(cleanAccount).isEmpty() || requiresLogin() ||
            cleanAccount.equals(chatState.ownerUserId())) {
            reportError("好友账号无效或不能添加自己");
            if (completion != null) completion.accept(false);
            return;
        }
        addLocalContact(cleanAccount);
        if (completion != null) completion.accept(true);
    }

    private void addLocalContact(String cleanUserId) {
        RemoteIMContact contact = new RemoteIMContact(cleanUserId, cleanUserId);
        chatState.upsertContact(contact);
        chatState.selectPeer(cleanUserId);
        if (productionMode) {
            persistContact(contact);
            refreshContactMetadata(Collections.singletonList(cleanUserId));
        } else {
            try {
                saveChatState();
            } catch (IOException ignored) {
            }
        }
        notifyStateChanged();
    }

    public void refreshAcceptedFriends() {
        if (requiresLogin() || !productionMode) return;
        int generation = accountGeneration;
        ioExecutor.execute(() -> {
            try {
                List<MaiChatAuthClient.FriendInfo> friends = authClient.friends();
                runOnMain(() -> {
                    if (generation != accountGeneration || requiresLogin()) return;
                    acceptedFriendsLoaded = true;
                    List<String> added = new ArrayList<>();
                    acceptedFriendUserIds.clear();
                    pendingFriendSyncUserIds.clear();
                    friendAccountByIm.clear();
                    friendDisplayByIm.clear();
                    for (MaiChatAuthClient.FriendInfo friend : friends) {
                        String userId = friend.imUserId;
                        if (userId == null || userId.isEmpty()) continue;
                        acceptedFriendUserIds.add(userId);
                        if (friend.pendingImSync) pendingFriendSyncUserIds.add(userId);
                        friendAccountByIm.put(userId, friend.accountId);
                        String displayName = friend.nickname.isEmpty()
                            ? friend.accountId : friend.nickname;
                        friendDisplayByIm.put(userId, displayName);
                        RemoteIMContact existing = null;
                        for (RemoteIMContact contact : chatState.contacts())
                            if (userId.equals(contact.userId())) { existing = contact; break; }
                        if (existing != null) {
                            if (!displayName.equals(existing.displayName())) {
                                RemoteIMContact updated = chatState.setContactDisplayName(
                                    userId, displayName);
                                if (updated != null) persistContact(updated);
                            }
                            continue;
                        }
                        RemoteIMContact contact = new RemoteIMContact(userId, displayName);
                        chatState.upsertContact(contact);
                        persistContact(contact);
                        added.add(userId);
                    }
                    if (!added.isEmpty()) refreshContactMetadata(added);
                    notifyStateChanged();
                });
            } catch (Exception failure) {
                android.util.Log.w("MaiChat.account", "friend list refresh failed");
                runOnMain(() -> {
                    if (generation == accountGeneration && !requiresLogin())
                        reportError("获取好友列表失败，请稍后重试");
                });
            }
        });
    }

    public void friendRequests(
        java.util.function.BiConsumer<List<RemoteIMFriendRequest>, String> completion) {
        if (requiresLogin() || !productionMode) {
            completion.accept(Collections.emptyList(), "请先登录 MaiChat 账号");
            return;
        }
        int generation = accountGeneration;
        ioExecutor.execute(() -> {
            try {
                List<MaiChatAuthClient.FriendRequestInfo> requests = authClient.friendRequests();
                runOnMain(() -> {
                    if (generation != accountGeneration || requiresLogin()) return;
                    friendRequestIdByIm.clear();
                    List<RemoteIMFriendRequest> display = new ArrayList<>();
                    for (MaiChatAuthClient.FriendRequestInfo request : requests) {
                        friendRequestIdByIm.put(request.imUserId, request.id);
                        display.add(new RemoteIMFriendRequest(request.imUserId,
                            request.nickname.isEmpty() ? request.accountId : request.nickname,
                            "", ""));
                    }
                    completion.accept(display, null);
                });
            } catch (Exception failure) {
                runOnMain(() -> completion.accept(Collections.emptyList(),
                    "获取好友申请失败，请稍后重试"));
            }
        });
    }

    public void decideFriendRequest(String userId, boolean accept,
                                    java.util.function.Consumer<Boolean> completion) {
        if (requiresLogin() || !productionMode) { completion.accept(false); return; }
        int generation = accountGeneration;
        String requestId = friendRequestIdByIm.get(userId);
        if (requestId == null) { completion.accept(false); return; }
        ioExecutor.execute(() -> {
            boolean success = false;
            String error = null;
            try { authClient.decideFriendRequest(requestId, accept); success = true; }
            catch (Exception failure) { error = failure.getMessage(); }
            boolean decided = success;
            String message = error;
            runOnMain(() -> {
                if (generation != accountGeneration || requiresLogin()) return;
                if (decided) {
                    friendRequestIdByIm.remove(userId);
                    if (accept) refreshAcceptedFriends();
                } else reportError(message);
                completion.accept(decided);
            });
        });
    }

    public void deleteContact(String userId) {
        String cleanUserId = clean(userId);
        if (!productionMode) {
            chatState.removeContact(cleanUserId);
            try {
                saveChatState();
            } catch (IOException ignored) {
            }
            notifyStateChanged();
            return;
        }
        String accountId = friendAccountByIm.get(cleanUserId);
        if (accountId == null) { reportError("好友账号尚未同步"); return; }
        int generation = accountGeneration;
        ioExecutor.execute(() -> {
            String error = null;
            try { authClient.removeFriend(accountId); }
            catch (Exception failure) { error = failure.getMessage(); }
            String result = error;
            runOnMain(() -> {
                if (generation != accountGeneration) return;
                if (result != null) { reportError(result); return; }
                acceptedFriendUserIds.remove(cleanUserId);
                pendingFriendSyncUserIds.remove(cleanUserId);
                friendAccountByIm.remove(cleanUserId);
                friendDisplayByIm.remove(cleanUserId);
                // 好友列表立即移除，但历史会话与消息仍保留。
                notifyStateChanged();
            });
        });
    }

    public boolean createContactGroup(String name) {
        String cleanName = ContactGroups.normalize(name);
        if (!ContactGroups.isAcceptableName(cleanName) || requiresLogin()) return false;
        if (!chatState.addContactGroup(cleanName)) return false;
        if (productionMode) {
            String owner = chatState.ownerUserId();
            enqueueIO(() -> historyStore.createContactGroup(owner, cleanName));
        }
        persistLegacyAndNotify();
        return true;
    }

    public boolean renameContactGroup(String from, String to) {
        String oldName = ContactGroups.normalize(from);
        String newName = ContactGroups.normalize(to);
        if (!ContactGroups.isAcceptableName(newName) || requiresLogin()) return false;
        if (!chatState.renameContactGroup(oldName, newName)) return false;
        if (productionMode) {
            String owner = chatState.ownerUserId();
            enqueueIO(() -> historyStore.renameContactGroup(owner, oldName, newName));
        }
        persistLegacyAndNotify();
        return true;
    }

    public boolean deleteContactGroup(String name) {
        String cleanName = ContactGroups.normalize(name);
        if (cleanName.isEmpty() || requiresLogin()) return false;
        if (!chatState.removeContactGroup(cleanName)) return false;
        if (productionMode) {
            String owner = chatState.ownerUserId();
            enqueueIO(() -> historyStore.deleteContactGroup(owner, cleanName));
        }
        persistLegacyAndNotify();
        return true;
    }

    public boolean setContactGroup(String userId, String groupName) {
        String cleanUserId = clean(userId);
        if (cleanUserId.isEmpty() || requiresLogin()) return false;
        if (!chatState.setContactGroup(cleanUserId, groupName)) return false;
        if (productionMode) {
            String owner = chatState.ownerUserId();
            enqueueIO(() -> historyStore.setContactGroup(owner, cleanUserId, ContactGroups.normalize(groupName)));
        }
        persistLegacyAndNotify();
        return true;
    }

    private void persistLegacyAndNotify() {
        if (!productionMode) {
            try {
                saveChatState();
            } catch (IOException ignored) {
            }
        }
        notifyStateChanged();
    }

    public void clearHistory(String userId) {
        String cleanUserId = clean(userId);
        if (!productionMode) {
            chatState.removeMessagesWith(cleanUserId);
            try {
                saveChatState();
            } catch (IOException ignored) {
            }
            notifyStateChanged();
            return;
        }
        String ownerUserId = chatState.ownerUserId();
        client.clearHistory(cleanUserId, new TencentIMClient.OperationCompletion() {
            @Override
            public void onSuccess() {
                runOnMain(() -> {
                    if (!ownerUserId.equals(chatState.ownerUserId())) return;
                    chatState.removeMessagesWith(cleanUserId);
                    peerEpochs.put(cleanUserId, peerEpochs.getOrDefault(cleanUserId, 0) + 1);
                    pageRequests.put(cleanUserId, pageRequests.getOrDefault(cleanUserId, 0) + 1);
                    loadingPeers.remove(cleanUserId);
                    enqueueIO(() -> historyStore.deleteConversation(ownerUserId, cleanUserId));
                    unreadByUserId.remove(cleanUserId);
                    hasEarlierByUserId.put(cleanUserId, false);
                    oldestLoadedCreatedAtByUserId.remove(cleanUserId);
                    oldestLoadedMessageIdByUserId.remove(cleanUserId);
                    notifyStateChanged();
                });
            }

            @Override
            public void onError(int code, String message) {
                if (!ownerUserId.equals(chatState.ownerUserId())) return;
                reportError(message == null || message.trim().isEmpty()
                    ? "清空聊天记录失败（" + code + "）"
                    : message);
            }
        });
    }

    public RemoteIMMessage sendTextMessage(String text) throws IOException {
        return sendTextMessage(text, null);
    }

    public RemoteIMMessage sendTextMessage(String text, RemoteIMQuote quote) throws IOException {
        return sendTextMessageTo(chatState.selectedPeerId(), text, quote);
    }

    public RemoteIMMessage sendTextMessageTo(String peer, String text, RemoteIMQuote quote) throws IOException {
        stopHumanTyping();
        RemoteIMMessage message = chatState.queueOutgoingTextTo(peer, text);
        message.setQuote(quote);
        if (!productionMode) {
            markMessageSentAndSave(message);
            return message;
        }
        persistMessage(message);
        notifyStateChanged();
        client.sendText(
            message.toUserId(),
            message.text(),
            RemoteIMOrigin.HUMAN,
            quote,
            sendCompletion(message)
        );
        return message;
    }

    // 群发逐个收件人建立发送记录并汇总完成结果；单人失败不应中断剩余收件人。
    // 返回已接受的收件人数，最终失败明细经 completion 交付，避免把入队当成送达。
    public int broadcastText(
        List<String> rawUserIds,
        String text,
        BroadcastCompletion completion
    ) throws IOException {
        List<String> recipients = BroadcastSelectionPolicy.uniqueRecipientIds(rawUserIds);
        String cleanText = clean(text);
        if (recipients.isEmpty() || cleanText.isEmpty() || requiresLogin()) return 0;

        List<RemoteIMMessage> queued = new ArrayList<>();
        for (String userId : recipients) {
            RemoteIMMessage message = chatState.queueOutgoingTextTo(userId, cleanText);
            queued.add(message);
            if (productionMode) persistMessage(message);
        }
        notifyStateChanged();

        if (!productionMode) {
            for (RemoteIMMessage message : queued) {
                chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.SENT);
            }
            saveChatState();
            notifyStateChanged();
            if (completion != null) completion.onFinished(queued.size(), Collections.emptyList());
            return queued.size();
        }

        String ownerUserId = chatState.ownerUserId();
        BroadcastDeliveryTracker tracker = new BroadcastDeliveryTracker(
            queued.size(),
            (total, failed) -> {
                if (completion != null) completion.onFinished(total, failed);
            }
        );
        for (RemoteIMMessage message : queued) {
            client.sendText(
                message.toUserId(),
                message.text(),
                RemoteIMOrigin.HUMAN,
                new TencentIMClient.SendCompletion() {
                    @Override
                    public void onSuccess(String remoteId, long createdAtMillis) {
                        runOnMain(() -> {
                            if (!ownerUserId.equals(chatState.ownerUserId())) return;
                            chatState.updateMessageDelivery(message.id(), remoteId);
                            persistMessage(message);
                            notifyStateChanged();
                            tracker.record(message.toUserId(), true);
                        });
                    }

                    @Override
                    public void onError(int code, String description) {
                        runOnMain(() -> {
                            if (!ownerUserId.equals(chatState.ownerUserId())) return;
                            chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.FAILED);
                            persistMessage(message);
                            notifyStateChanged();
                            // 不逐条 reportError；等全部回执到齐后一次列出失败的人。
                            tracker.record(message.toUserId(), false);
                        });
                    }
                }
            );
        }
        return queued.size();
    }

    public RemoteIMMessage sendApprovalDecision(
        String peerId,
        RemoteIMApprovalRequest request,
        RemoteIMApprovalAction action
    ) throws IOException {
        if (request == null || !request.allows(action)) {
            throw new IllegalArgumentException("该审批请求不允许此操作");
        }
        RemoteIMMessage message = chatState.queueOutgoingApprovalDecision(
            peerId,
            request.token(),
            action
        );
        if (!productionMode) {
            markMessageSentAndSave(message);
            return message;
        }
        persistMessage(message);
        notifyStateChanged();
        try {
            client.sendApprovalDecision(
                peerId,
                message.approvalDecision(),
                sendCompletion(message)
            );
        } catch (RuntimeException error) {
            chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.FAILED);
            persistMessage(message);
            notifyStateChanged();
            throw error;
        }
        return message;
    }

    /** 独立临时活动通道；只把原 signal 发给 SDK，完整授权 envelope 仅交回 Native。 */
    @Override public void activity(JSONObject value, MaiSessionIMBindingService.Backend backend) {
        JSONObject owned = MaiSessionIMBindingService.copy(value), scope = owned.optJSONObject("scope");
        String peer = owned.optString("peer_id"); int generation = accountGeneration;
        Runnable ack = () -> backend.control(MaiSessionIMBindingService.activityControl("ack_activity", owned));
        java.util.function.BooleanSupplier current = () -> generation == accountGeneration && current(scope, peer);
        if (!current.getAsBoolean() || client == null) { ack.run(); return; }
        JSONObject signalValue = owned.optJSONObject("signal");
        RemoteIMActivitySignal signal = signalValue == null ? null
            : RemoteIMActivitySignal.decode(signalValue.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8));
        client.sendSessionIMActivity(scope.optString("owner_id"), client.sessionIMLoginEpoch(), current,
            () -> { JSONObject claim = backend.control(MaiSessionIMBindingService.activityControl("claim_activity", owned));
                return claim.optBoolean("ok") && claim.optBoolean("allowed"); }, peer, signal, ack);
    }

    /** SDK 交付适配：Native claim/dispatch ack 与网络回执分开，绝不重发不确定结果。 */
    @Override public void delivery(JSONObject value, MaiSessionIMBindingService.Backend backend) {
        JSONObject owned = MaiSessionIMBindingService.copy(value);
        JSONObject scope = owned.optJSONObject("scope");
        String peer = owned.optString("peer_id");
        int generation = accountGeneration;
        java.util.concurrent.atomic.AtomicBoolean acknowledged = new java.util.concurrent.atomic.AtomicBoolean();
        Runnable ack = () -> {
            if (acknowledged.compareAndSet(false, true)) backend.control(AIAssistantController.controlDelivery("ack", owned));
        };
        java.util.function.BooleanSupplier current = () -> generation == accountGeneration && current(scope, peer);
        if (!current.getAsBoolean() || client == null) { ack.run(); return; }
        long sdkEpoch = client.sessionIMLoginEpoch();
        try {
            ioExecutor.execute(() -> MaiSessionIMOutgoing.prepareAndDispatch(appContext, owned, current, ack,
                error -> runHistoryCompletion(() -> { if (current.getAsBoolean()) reportError(error); }), prepared -> {
                java.util.concurrent.atomic.AtomicBoolean issued = new java.util.concurrent.atomic.AtomicBoolean();
                client.sendSessionIM(scope.optString("owner_id"), sdkEpoch, current,
                    () -> {
                        JSONObject claim = backend.control(AIAssistantController.controlDelivery("claim", owned));
                        return claim.optBoolean("ok") && claim.optBoolean("allowed");
                    }, prepared.message, prepared.exactBody,
                    () -> {
                        issued.set(true);
                        runHistoryCompletion(() -> {
                            if (!current.getAsBoolean()) return;
                            chatState.mergeMessages(Collections.singletonList(prepared.message));
                            persistMessage(prepared.message); notifyStateChanged();
                        });
                    }, ack,
                    new TencentIMClient.SendCompletion() {
                        @Override public void onSuccess(String remoteId, long createdAtMillis) {
                            runHistoryCompletion(() -> {
                                if (!current.getAsBoolean()) return;
                                RemoteIMMessage source = prepared.message;
                                RemoteIMMessage sent = new RemoteIMMessage(source.id(), remoteId, source.fromUserId(),
                                    source.toUserId(), source.text(), RemoteIMMessage.Direction.OUTGOING, RemoteIMMessage.Status.SENT,
                                    createdAtMillis, source.imageAttachment(), null, source.fileAttachment(), source.videoAttachment(),
                                    RemoteIMOrigin.MACHINE, source.approvalRequest(), source.approvalDecision());
                                chatState.updateMessageDelivery(source.id(), remoteId);
                                persistMessage(sent); notifyStateChanged();
                            });
                        }
                        @Override public void onError(int code, String error) {
                            if (!issued.get()) { prepared.discard(); return; }
                            runHistoryCompletion(() -> {
                                if (!current.getAsBoolean()) return;
                                if (code != -2) chatState.updateMessageStatus(prepared.message.id(), RemoteIMMessage.Status.FAILED);
                                persistMessage(prepared.message); notifyStateChanged();
                                reportError(error);
                            });
                        }
                    });
            }));
        } catch (java.util.concurrent.RejectedExecutionException closed) { ack.run(); }
    }

    public RemoteIMMessage sendImageMessage(
        String localPath,
        int width,
        int height,
        long sizeBytes
    ) throws IOException {
        return sendImageMessageTo(chatState.selectedPeerId(), localPath, width, height, sizeBytes);
    }

    public RemoteIMMessage sendImageMessageTo(String peer, String localPath, int width, int height, long sizeBytes)
            throws IOException {
        return sendImageMessageTo(peer, localPath, width, height, sizeBytes, null);
    }

    public RemoteIMMessage sendImageMessageTo(String peer, String localPath, int width, int height, long sizeBytes,
            RemoteIMQuote quote) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingImageTo(peer, localPath, width, height, sizeBytes);
        message.setQuote(quote);
        if (!productionMode) {
            markMessageSentAndSave(message);
            return message;
        }
        persistMessage(message);
        notifyStateChanged();
        client.sendImage(
            message.toUserId(),
            localPath,
            RemoteIMOrigin.HUMAN,
            quote,
            sendCompletion(message)
        );
        return message;
    }

    public RemoteIMMessage sendVoiceMessage(String localPath, int durationSeconds) throws IOException {
        return sendVoiceMessageTo(chatState.selectedPeerId(), localPath, durationSeconds);
    }

    public RemoteIMMessage sendVoiceMessageTo(String peer, String localPath, int durationSeconds) throws IOException {
        return sendVoiceMessageTo(peer, localPath, durationSeconds, null);
    }

    public RemoteIMMessage sendVoiceMessageTo(String peer, String localPath, int durationSeconds, RemoteIMQuote quote)
            throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingVoiceTo(peer, localPath, durationSeconds);
        message.setQuote(quote);
        if (!productionMode) {
            markMessageSentAndSave(message);
            return message;
        }
        persistMessage(message);
        notifyStateChanged();
        client.sendVoice(
            message.toUserId(),
            localPath,
            durationSeconds,
            RemoteIMOrigin.HUMAN,
            quote,
            sendCompletion(message)
        );
        return message;
    }

    public RemoteIMMessage sendFileMessage(
        String localPath,
        String fileName,
        String mimeType,
        long sizeBytes
    ) throws IOException {
        return sendFileMessageTo(chatState.selectedPeerId(), localPath, fileName, mimeType, sizeBytes);
    }

    public RemoteIMMessage sendFileMessageTo(
            String peer, String localPath, String fileName, String mimeType, long sizeBytes) throws IOException {
        return sendFileMessageTo(peer, localPath, fileName, mimeType, sizeBytes, null);
    }

    public RemoteIMMessage sendFileMessageTo(String peer, String localPath, String fileName, String mimeType,
            long sizeBytes, RemoteIMQuote quote) throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingFileTo(
            peer, localPath,
            fileName,
            mimeType,
            sizeBytes
        );
        message.setQuote(quote);
        if (!productionMode) {
            markMessageSentAndSave(message);
            return message;
        }
        persistMessage(message);
        notifyStateChanged();
        client.sendFile(
            message.toUserId(),
            localPath,
            fileName,
            RemoteIMOrigin.HUMAN,
            quote,
            sendCompletion(message)
        );
        return message;
    }

    public RemoteIMMessage sendVideoMessageTo(String peer, RemoteIMVideoAttachment attachment) throws IOException {
        return sendVideoMessageTo(peer, attachment, null);
    }

    public RemoteIMMessage sendVideoMessageTo(String peer, RemoteIMVideoAttachment attachment, RemoteIMQuote quote)
            throws IOException {
        RemoteIMMessage message = chatState.queueOutgoingVideoTo(peer, attachment);
        message.setQuote(quote);
        if (!productionMode) { markMessageSentAndSave(message); return message; }
        persistMessage(message); notifyStateChanged();
        client.sendVideo(peer, attachment, RemoteIMOrigin.HUMAN, quote, sendCompletion(message));
        return message;
    }

    public void forwardMessageAsync(RemoteIMMessage source, String targetUserId,
        java.util.function.Consumer<RemoteIMMessage> completed, java.util.function.Consumer<String> failed) {
        if (source == null) { failed.accept("消息不存在"); return; }
        RemoteIMMessage snapshot = source.snapshot();
        int generation = accountGeneration;
        enqueueIO(() -> {
            try {
                if (snapshot.imageAttachment() != null)
                    requireForwardingFile(snapshot.imageAttachment().localPath(), "图片");
                if (snapshot.voiceAttachment() != null)
                    requireForwardingFile(snapshot.voiceAttachment().localPath(), "语音");
                if (snapshot.videoAttachment() != null)
                    requireForwardingFile(snapshot.videoAttachment().localPath(), "视频");
                if (snapshot.fileAttachment() != null)
                    requireForwardingFile(snapshot.fileAttachment().localPath(), "文件");
                runOnMain(() -> {
                    if (generation != accountGeneration) return;
                    try { completed.accept(forwardMessage(snapshot, targetUserId, true)); }
                    catch (IOException | RuntimeException error) { failed.accept(error.getMessage()); }
                });
            } catch (Exception error) {
                runOnMain(() -> {
                    if (generation == accountGeneration)
                        failed.accept(error.getMessage());
                });
            }
        });
    }

    public RemoteIMMessage forwardMessage(RemoteIMMessage source, String targetUserId)
        throws IOException { return forwardMessage(source, targetUserId, false); }

    // 校验转发目标与原消息，再按消息类型复制正文或媒体引用并发往目标会话。
    // 只有 SDK 或本地队列确认接受后才返回新消息，失败时保留原消息不动。
    private RemoteIMMessage forwardMessage(RemoteIMMessage source, String targetUserId, boolean validated)
        throws IOException {
        if (source == null) throw new IllegalArgumentException("message is required");
        String target = clean(targetUserId);
        if (target.isEmpty() || target.equals(chatState.ownerUserId())) {
            throw new IllegalArgumentException("target user is required");
        }

        RemoteIMMessage forwarded;
        if (source.imageAttachment() != null) {
            RemoteIMImageAttachment attachment = source.imageAttachment();
            if (!validated) requireForwardingFile(attachment.localPath(), "图片");
            forwarded = chatState.queueOutgoingImageTo(
                target,
                attachment.localPath(),
                attachment.width(),
                attachment.height(),
                attachment.sizeBytes()
            );
        } else if (source.voiceAttachment() != null) {
            RemoteIMVoiceAttachment attachment = source.voiceAttachment();
            if (!validated) requireForwardingFile(attachment.localPath(), "语音");
            forwarded = chatState.queueOutgoingVoiceTo(
                target,
                attachment.localPath(),
                attachment.durationSeconds()
            );
        } else if (source.fileAttachment() != null) {
            RemoteIMFileAttachment attachment = source.fileAttachment();
            if (!validated) requireForwardingFile(attachment.localPath(), "文件");
            forwarded = chatState.queueOutgoingFileTo(
                target,
                attachment.localPath(),
                attachment.fileName(),
                attachment.mimeType(),
                attachment.sizeBytes()
            );
        } else if (source.videoAttachment() != null) {
            RemoteIMVideoAttachment attachment = source.videoAttachment();
            if (!validated) requireForwardingFile(attachment.localPath(), "视频");
            forwarded = chatState.queueOutgoingVideoTo(target, attachment);
        } else {
            forwarded = chatState.queueOutgoingTextTo(target, source.text());
        }

        if (!productionMode) {
            markMessageSentAndSave(forwarded);
            return forwarded;
        }
        persistMessage(forwarded);
        notifyStateChanged();
        TencentIMClient.SendCompletion completion = sendCompletion(forwarded);
        if (forwarded.imageAttachment() != null) {
            client.sendImage(target, forwarded.imageAttachment().localPath(),
                RemoteIMOrigin.HUMAN, completion);
        } else if (forwarded.voiceAttachment() != null) {
            client.sendVoice(target, forwarded.voiceAttachment().localPath(),
                forwarded.voiceAttachment().durationSeconds(), RemoteIMOrigin.HUMAN, completion);
        } else if (forwarded.videoAttachment() != null) {
            client.sendVideo(target, forwarded.videoAttachment(), RemoteIMOrigin.HUMAN, completion);
        } else if (forwarded.fileAttachment() != null) {
            client.sendFile(target, forwarded.fileAttachment().localPath(),
                forwarded.fileAttachment().fileName(), RemoteIMOrigin.HUMAN, completion);
        } else {
            client.sendText(target, forwarded.text(), RemoteIMOrigin.HUMAN, completion);
        }
        return forwarded;
    }

    private static void requireForwardingFile(String path, String type) throws IOException {
        if (path == null || path.trim().isEmpty() || !new File(path).isFile()) {
            throw new IOException(type + "尚未下载完成或本地缓存已被清理");
        }
    }

    public void selectContact(String userId) {
        chatState.selectPeer(userId);
        unreadByUserId.remove(clean(userId));
        notifyStateChanged();
    }

    public void setConversationVisible(String userId, boolean visible) {
        if (!visible || !clean(userId).equals(visibleConversationUserId)) stopHumanTyping();
        String cleanUserId = clean(userId);
        if (visible) {
            visibleConversationUserId = cleanUserId;
            unreadByUserId.remove(cleanUserId);
        } else if (visibleConversationUserId.equals(cleanUserId)) {
            visibleConversationUserId = "";
            if (productionMode) chatState.retainRecentMessages(cleanUserId, 20);
        }
    }

    public int unreadCount(String userId) {
        return unreadByUserId.getOrDefault(clean(userId), 0);
    }

    public int totalUnreadCount() {
        int total = 0;
        for (int count : unreadByUserId.values()) total += count;
        for (int count : unreadByGroupId.values()) total += count;
        return total;
    }

    public TencentIMClient.PresenceStatus presenceStatus(String userId) {
        return presenceByUserId.getOrDefault(clean(userId), TencentIMClient.PresenceStatus.UNKNOWN);
    }

    public void loadInitialMessages(String userId) { loadMessagePage(userId, true); }

    public boolean loadEarlierMessages(String userId) {
        if (!productionMode || isLoadingMessages(userId)) return false;
        loadMessagePage(userId, false);
        return true;
    }

    private void loadMessagePage(String userId, boolean initial) {
        if (!productionMode || requiresLogin()) return;
        String peer = clean(userId), owner = chatState.ownerUserId();
        if (peer.isEmpty()) return;
        int generation = accountGeneration;
        int request = pageRequests.getOrDefault(peer, 0) + 1;
        pageRequests.put(peer, request); loadingPeers.add(peer);
        Long cursorTime = initial ? null : oldestLoadedCreatedAtByUserId.get(peer);
        String cursorId = initial ? null : oldestLoadedMessageIdByUserId.get(peer);
        enqueueIO(() -> {
            AndroidChatHistoryStore.Page page;
            try { page = historyStore.loadConversationPage(owner, peer, cursorTime, cursorId, 20); }
            catch (RuntimeException error) {
                runOnMain(() -> {
                    if (generation == accountGeneration && pageRequests.getOrDefault(peer, 0) == request)
                        loadingPeers.remove(peer);
                });
                throw error;
            }
            runOnMain(() -> {
                if (generation != accountGeneration || pageRequests.getOrDefault(peer, 0) != request) return;
                loadingPeers.remove(peer);
                java.util.Set<String> loadedIds = new java.util.HashSet<>();
                for (RemoteIMMessage message : chatState.messages()) loadedIds.add(message.id());
                List<RemoteIMMessage> additional = new ArrayList<>();
                for (RemoteIMMessage message : page.messages()) {
                    if (!loadedIds.contains(message.id()) && (message.remoteId().isEmpty()
                        || chatState.messageWithRemoteId(message.remoteId()) == null)) additional.add(message);
                }
                chatState.mergeMessages(additional);
                hasEarlierByUserId.put(peer, page.hasEarlier());
                updateOldestLoadedCursor(peer, page.messages());
                notifyStateChanged();
            });
        });
    }

    public List<RemoteIMMessageSearchHit> searchMessages(String query, int limit) {
        String cleanQuery = clean(query);
        if (cleanQuery.isEmpty() || requiresLogin()) return Collections.emptyList();
        if (productionMode) {
            return historyStore.searchMessages(chatState.ownerUserId(), cleanQuery, limit);
        }
        List<RemoteIMMessageSearchHit> hits = new ArrayList<>();
        String lowerQuery = cleanQuery.toLowerCase(java.util.Locale.ROOT);
        for (RemoteIMContact contact : chatState.contacts()) {
            for (RemoteIMMessage message : chatState.messagesWith(contact.userId())) {
                if (message.text().toLowerCase(java.util.Locale.ROOT).contains(lowerQuery)) {
                    hits.add(new RemoteIMMessageSearchHit(contact.userId(), message));
                }
            }
        }
        hits.sort((left, right) -> Long.compare(
            right.message().createdAtMillis(),
            left.message().createdAtMillis()
        ));
        return Collections.unmodifiableList(
            new ArrayList<>(hits.subList(0, Math.min(Math.max(limit, 1), hits.size())))
        );
    }

    public RemoteIMContact openMessageSearchHit(RemoteIMMessageSearchHit hit) {
        if (hit == null) return null;
        RemoteIMContact contact = null;
        for (RemoteIMContact candidate : chatState.contacts()) {
            if (candidate.userId().equals(hit.peerUserId())) {
                contact = candidate;
                break;
            }
        }
        if (contact == null) return null;
        selectContact(contact.userId());
        loadInitialMessages(contact.userId());
        chatState.mergeMessages(Collections.singletonList(hit.message()));
        return contact;
    }

    public RemoteIMMessage findQuotedMessage(String peerUserId, String remoteId) {
        String cleanPeerId = clean(peerUserId);
        String cleanRemoteId = clean(remoteId);
        if (cleanPeerId.isEmpty() || cleanRemoteId.isEmpty()) return null;
        RemoteIMMessage inMemory = chatState.messageWithRemoteId(cleanRemoteId);
        if (inMemory != null) return inMemory;
        if (!productionMode) return null;
        return null;
    }

    public void findQuotedMessage(
            String peer, String remoteId, java.util.function.Consumer<RemoteIMMessage> completion) {
        RemoteIMMessage cached = findQuotedMessage(peer, remoteId);
        if (cached != null || !productionMode) { completion.accept(cached); return; }
        String owner = chatState.ownerUserId();
        int generation = accountGeneration;
        enqueueIO(() -> {
            RemoteIMMessage stored = historyStore.messageWithRemoteId(owner, clean(peer), clean(remoteId));
            runOnMain(() -> {
                if (generation != accountGeneration) return;
                if (stored != null) chatState.mergeMessages(Collections.singletonList(stored));
                completion.accept(stored);
            });
        });
    }

    private void updateOldestLoadedCursor(String peerId, List<RemoteIMMessage> pageMessages) {
        if (pageMessages == null || pageMessages.isEmpty()) return;
        RemoteIMMessage oldest = pageMessages.get(0);
        recordOldestLoadedCursor(peerId, oldest.createdAtMillis(), oldest.id());
    }

    void recordOldestLoadedCursor(String peerId, long createdAtMillis, String messageId) {
        String cleanPeerId = clean(peerId);
        String cleanMessageId = clean(messageId);
        if (cleanPeerId.isEmpty() || cleanMessageId.isEmpty()) return;
        oldestLoadedCreatedAtByUserId.put(cleanPeerId, createdAtMillis);
        oldestLoadedMessageIdByUserId.put(cleanPeerId, cleanMessageId);
    }

    Long oldestLoadedCreatedAt(String peerId) {
        return oldestLoadedCreatedAtByUserId.get(clean(peerId));
    }

    String oldestLoadedMessageId(String peerId) {
        return oldestLoadedMessageIdByUserId.get(clean(peerId));
    }

    public boolean hasEarlierMessages(String userId) {
        return hasEarlierByUserId.getOrDefault(clean(userId), false);
    }

    public void destroy() {
        if (!productionMode) return;
        cancelAccountRequests();
        selfAvatarUrl = null;
        authClient.setSessionTerminationListener(null);
        appForeground = false;
        mainHandler.removeCallbacks(sessionValidation);
        resetActivities();
        if (pendingStateNotification != null) {
            mainHandler.removeCallbacks(pendingStateNotification);
            pendingStateNotification = null;
        }
        if (remoteDesktop != null) remoteDesktop.destroy();
        destroyed = true;
        messageHistoryExecutor.shutdownNow();
        client.destroy();
        ioExecutor.execute(historyStore::close);
        ioExecutor.shutdown();
    }

    private void connect() {
        if (requiresLogin() || !productionMode) return;
        String expectedUser = settings.loginUserId();
        int generation = accountGeneration;
        connectionState = TencentIMClient.ConnectionState.CONNECTING;
        connectionDetail = "连接中";
        notifyStateChanged();
        ioExecutor.execute(() -> {
            MaiChatAuthClient.Credential credential = null;
            String error = null;
            try {
                credential = authClient.credential();
                if (!expectedUser.equals(credential.userId))
                    throw new IOException("当前 MaiChat 账号与本机 IM 账号不一致");
            } catch (Exception failure) {
                error = failure.getMessage();
            }
            MaiChatAuthClient.Credential signed = credential;
            String failureText = error;
            runOnMain(() -> {
                if (destroyed || generation != accountGeneration) return;
                if (failureText != null) {
                    if (!authClient.hasSession() || failureText.contains("不一致")) {
                        switchAccount(RemoteIMSettings.empty());
                    }
                    connectionState = TencentIMClient.ConnectionState.FAILED;
                    connectionDetail = failureText;
                    reportError(failureText);
                    notifyStateChanged();
                    return;
                }
                imCredential = signed;
                try {
                    client.connect(signed.sdkAppId, expectedUser, signed.userSig);
                } catch (RuntimeException failure) {
                    connectionState = TencentIMClient.ConnectionState.FAILED;
                    connectionDetail = failure.getMessage() == null
                        ? "登录失败" : failure.getMessage();
                    reportError(connectionDetail);
                    notifyStateChanged();
                }
            });
        });
    }

    private void refreshContactMetadata() {
        if (!productionMode || requiresLogin()) return;
        List<String> userIds = new ArrayList<>();
        for (RemoteIMContact contact : chatState.contacts()) userIds.add(contact.userId());
        // 本人由 CONNECTED 的独立 getSelfProfile 加载；好友刷新不重复查询本人。
        refreshContactMetadata(userIds);
    }

    private void refreshContactMetadata(List<String> userIds) {
        if (!productionMode || userIds == null || userIds.isEmpty()) return;
        client.refreshProfiles(userIds);
        client.refreshAndSubscribePresence(userIds);
    }

    private TencentIMClient.SendCompletion sendCompletion(RemoteIMMessage message) {
        String ownerUserId = chatState.ownerUserId();
        return new TencentIMClient.SendCompletion() {
            @Override
            public void onSuccess(String remoteId, long createdAtMillis) {
                runOnMain(() -> {
                    if (!ownerUserId.equals(chatState.ownerUserId())) return;
                    chatState.updateMessageDelivery(message.id(), remoteId);
                    persistMessage(message);
                    notifyStateChanged();
                });
            }

            @Override
            public void onError(int code, String description) {
                runOnMain(() -> {
                    if (!ownerUserId.equals(chatState.ownerUserId())) return;
                    chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.FAILED);
                    persistMessage(message);
                    reportError(description == null || description.trim().isEmpty()
                        ? "消息发送失败（" + code + "）"
                        : description);
                    notifyStateChanged();
                });
            }
        };
    }

    private void receive(RemoteIMMessage incoming) { receive(incoming, null); }

    private boolean ingressCurrent(RemoteIMIngress.Metadata metadata, int generation, long authGeneration) {
        return productionMode && !destroyed && !requiresLogin() && generation == accountGeneration && authClient != null
            && authGeneration == authClient.accountSessionGeneration() && settings.loginUserId().equals(metadata.scope.ownerId)
            && currentSdkAppId() == metadata.scope.sdkAppId && client != null && client.isIngressScopeCurrent(metadata.scope);
    }

    private java.util.concurrent.CompletableFuture<RemoteIMIngress.Stored> persistRawIncoming(RemoteIMIngress.Metadata metadata) {
        java.util.concurrent.CompletableFuture<RemoteIMIngress.Stored> unavailable = new java.util.concurrent.CompletableFuture<>();
        if (ingressWriter == null || authClient == null) {
            unavailable.completeExceptionally(new IllegalStateException("本机历史 writer 不可用")); return unavailable;
        }
        int generation = accountGeneration; long authGeneration = authClient.accountSessionGeneration();
        java.util.function.BooleanSupplier current = () -> ingressCurrent(metadata, generation, authGeneration);
        java.util.concurrent.CompletableFuture<RemoteIMIngress.Stored> saved = ingressWriter.initial(metadata, current);
        saved.thenAccept(receipt -> {
            if (!receipt.inserted) return;
            runHistoryCompletion(() -> {
                if (!current.getAsBoolean() || !receipt.isCurrent()) return;
                RemoteIMMessage initial = metadata.message();
                applyIncomingMessage(initial, activities.get(initial.fromUserId()), true);
            });
        });
        return saved;
    }

    private void receiveTracked(RemoteIMMessage incoming, TencentIMClient.IncomingTicket ticket) {
        ticket.stored.whenComplete((receipt, failure) -> {
            if (failure != null || receipt == null || ingressWriter == null) return;
            java.util.function.BooleanSupplier current = () -> !destroyed && !requiresLogin() && receipt.isCurrent()
                && client != null && client.isIngressScopeCurrent(ticket.metadata.scope)
                && settings.loginUserId().equals(ticket.metadata.scope.ownerId) && currentSdkAppId() == ticket.metadata.scope.sdkAppId;
            if (!current.getAsBoolean()) { ticket.abandoned("canceled"); return; }
            ingressWriter.enrich(ticket.metadata, receipt, incoming.snapshot(), current).whenComplete((ready, error) -> {
                if (error != null) { ticket.abandoned("preparation_failed"); return; }
                runHistoryCompletion(() -> {
                    if (!current.getAsBoolean()) return;
                    if (chatState.enrichPersistedMessage(ready)) notifyStateChanged();
                });
            });
        });
    }

    private void receive(RemoteIMMessage incoming, TencentIMClient.LiveAdmission admission) {
        if (admission instanceof TencentIMClient.IncomingTicket) {
            receiveTracked(incoming, (TencentIMClient.IncomingTicket) admission); return;
        }
        if (incoming == null || requiresLogin() || !chatState.ownerUserId().equals(incoming.toUserId())) {
            if (admission != null) admission.abandoned(); return;
        }
        if (incoming.text() != null
            && remoteDesktop != null
            && remoteDesktop.handleIncomingText(incoming.fromUserId(), incoming.text())) {
            if (admission != null) admission.abandoned();
            return;
        }
        String owner = chatState.ownerUserId();
        int generation = accountGeneration;
        int peerEpoch = peerEpochs.getOrDefault(incoming.fromUserId(), 0);
        RemoteIMActivitySignal replacedActivity = activities.get(incoming.fromUserId());
        enqueueIO(() -> {
            boolean duplicate;
            try { duplicate = !incoming.remoteId().isEmpty() && historyStore.containsRemoteId(owner, incoming.remoteId()); }
            catch (Exception failed) {
                if (admission != null) admission.abandoned();
                throw failed;
            }
            runOnMain(() -> {
                if (duplicate || generation != accountGeneration
                    || peerEpoch != peerEpochs.getOrDefault(incoming.fromUserId(), 0)
                    || !incoming.remoteId().isEmpty() && chatState.messageWithRemoteId(incoming.remoteId()) != null) {
                    if (admission != null) admission.abandoned(); return;
                }
                applyIncomingMessage(incoming, replacedActivity);
                if (admission != null) admission.persisted(acceptedFriendUserIds.contains(incoming.fromUserId()));
            });
        });
    }

    // 作用：将入站消息与会话历史、未读数和活动状态合并。
    private void applyIncomingMessage(RemoteIMMessage incoming, RemoteIMActivitySignal replacedActivity) {
        applyIncomingMessage(incoming, replacedActivity, false);
    }
    private void applyIncomingMessage(RemoteIMMessage incoming, RemoteIMActivitySignal replacedActivity, boolean persisted) {
        RemoteIMVideoAttachment preparedVideo = pendingVideoMedia.remove(incoming.remoteId());
        if (preparedVideo != null && incoming.videoAttachment() != null)
            incoming = incoming.withVideoAttachment(preparedVideo);
        RemoteIMActivitySignal currentActivity = activities.get(incoming.fromUserId());
        if (replacedActivity != null && currentActivity != null
                && replacedActivity.activityId.equals(currentActivity.activityId)) {
            activities.clear(incoming.fromUserId());
        }
        boolean knownContact = contactExists(incoming.fromUserId());
        int previousMessageCount = chatState.messages().size();

        RemoteIMMessage message;
        if (persisted) {
            if (!knownContact) chatState.upsertContact(new RemoteIMContact(incoming.fromUserId(), incoming.fromUserId()));
            chatState.addRestoredMessage(incoming); message = incoming;
        } else if (incoming.imageAttachment() != null) {
            RemoteIMImageAttachment image = incoming.imageAttachment();
            message = chatState.receiveImage(
                image.localPath(),
                incoming.fromUserId(),
                image.width(),
                image.height(),
                image.sizeBytes(),
                incoming.remoteId(),
                incoming.createdAtMillis(),
                incoming.origin(),
                incoming.text(),
                incoming.captionAbove()
            );
        } else if (incoming.voiceAttachment() != null) {
            RemoteIMVoiceAttachment voice = incoming.voiceAttachment();
            message = chatState.receiveVoice(
                voice.localPath(),
                voice.durationSeconds(),
                incoming.fromUserId(),
                incoming.remoteId(),
                incoming.createdAtMillis(),
                incoming.origin()
            );
        } else if (incoming.videoAttachment() != null) {
            RemoteIMVideoAttachment video = incoming.videoAttachment();
            message = chatState.receiveVideo(
                video.localPath(),
                video.coverPath(),
                video.durationSeconds(),
                video.width(),
                video.height(),
                video.sizeBytes(),
                incoming.fromUserId(),
                incoming.remoteId(),
                incoming.createdAtMillis(),
                incoming.origin(),
                incoming.text(),
                incoming.captionAbove()
            );
        } else if (incoming.fileAttachment() != null) {
            RemoteIMFileAttachment file = incoming.fileAttachment();
            message = chatState.receiveFile(
                file.localPath(),
                incoming.fromUserId(),
                file.fileName(),
                file.mimeType(),
                file.sizeBytes(),
                incoming.remoteId(),
                incoming.createdAtMillis(),
                incoming.origin(),
                incoming.text(),
                incoming.captionAbove()
            );
        } else {
            message = chatState.receiveText(
                incoming.text(),
                incoming.fromUserId(),
                incoming.remoteId(),
                incoming.createdAtMillis(),
                incoming.origin(),
                incoming.approvalRequest(),
                incoming.approvalDecision()
            );
        }
        message.setQuote(incoming.quote());
        boolean wasInserted = chatState.messages().size() > previousMessageCount;
        RemoteIMContact contact = findContact(incoming.fromUserId());
        persistContact(contact);
        if (!persisted) persistMessage(message);
        if (wasInserted && !visibleConversationUserId.equals(incoming.fromUserId())) {
            unreadByUserId.put(
                incoming.fromUserId(),
                unreadByUserId.getOrDefault(incoming.fromUserId(), 0) + 1
            );
        }
        if (!visibleConversationUserId.equals(incoming.fromUserId()))
            chatState.retainRecentMessages(incoming.fromUserId(), 20);
        if (!knownContact) {
            refreshContactMetadata(Collections.singletonList(incoming.fromUserId()));
        }
        if (wasInserted) {
            listener.onNewIncomingMessage(
                message,
                visibleConversationUserId.equals(incoming.fromUserId())
            );
        }
        notifyStateChanged();
    }

    private boolean contactExists(String userId) {
        for (RemoteIMContact contact : chatState.contacts()) {
            if (contact.userId().equals(userId)) return true;
        }
        return false;
    }

    private RemoteIMContact findContact(String userId) {
        for (RemoteIMContact contact : chatState.contacts()) {
            if (contact.userId().equals(userId)) return contact;
        }
        return new RemoteIMContact(userId, userId);
    }

    private void persistMessage(RemoteIMMessage message) {
        if (productionMode) {
            String owner = chatState.ownerUserId();
            RemoteIMMessage snapshot = message.snapshot();
            enqueueIO(() -> historyStore.upsertMessage(owner, snapshot));
        }
    }

    private void markMessageSentAndSave(RemoteIMMessage message) throws IOException {
        chatState.updateMessageStatus(message.id(), RemoteIMMessage.Status.SENT);
        saveChatState();
    }

    private RemoteIMSettings loadSettings() {
        try {
            return settingsStore.load();
        } catch (IOException error) {
            return RemoteIMSettings.empty();
        }
    }

    private ChatState loadLegacyChatState() {
        String ownerUserId = requiresLogin() ? FALLBACK_OWNER_USER_ID : settings.loginUserId();
        try {
            return legacyHistoryStore.load(ownerUserId);
        } catch (IOException error) {
            return new ChatState(ownerUserId);
        }
    }

    private ChatState readProductionChatState(RemoteIMSettings account) {
        String ownerUserId = account.requiresLogin() ? FALLBACK_OWNER_USER_ID : account.loginUserId();
        if (account.requiresLogin()) return new ChatState(ownerUserId);
        ChatState state = new ChatState(ownerUserId);
        state.setContactGroups(historyStore.loadContactGroups(ownerUserId));
        for (RemoteIMContact contact : historyStore.loadContacts(ownerUserId)) {
            state.upsertContact(contact);
        }
        state.mergeMessages(historyStore.loadConversationSummaries(ownerUserId));
        if (!state.contacts().isEmpty()) state.selectPeer(state.contacts().get(0).userId());
        return state;
    }

    private void ensureDefaultContactIfNeeded() {
        if (requiresLogin()) return;
        if (chatState.contacts().isEmpty()) {
            chatState.upsertContact(new RemoteIMContact(DEFAULT_CONTACT_USER_ID, DEFAULT_CONTACT_USER_ID));
            chatState.selectPeer(DEFAULT_CONTACT_USER_ID);
        } else if (chatState.selectedPeerId() == null) {
            chatState.selectPeer(chatState.contacts().get(0).userId());
        }
    }

    private void notifyStateChanged() {
        if (mainHandler == null || Looper.myLooper() == Looper.getMainLooper()) publishSessionIMIdentity();
        else mainHandler.post(this::publishSessionIMIdentity);
        if (listener == null) return;
        if (mainHandler == null) {
            listener.onStateChanged();
            return;
        }
        Runnable schedule = () -> {
            if (pendingStateNotification != null) return;
            pendingStateNotification = () -> {
                pendingStateNotification = null;
                listener.onStateChanged();
            };
            mainHandler.post(pendingStateNotification);
        };
        if (Looper.myLooper() == Looper.getMainLooper()) schedule.run();
        else mainHandler.post(schedule);
    }

    private void publishSessionIMIdentity() {
        bindingFriendSnapshot = Collections.unmodifiableSet(new java.util.HashSet<>(acceptedFriendUserIds));
        MaiSessionIMBindingService target = sessionIMBindings;
        if (target != null) target.updateAccount();
    }

    public void setSessionIMBindings(MaiSessionIMBindingService bindings) {
        sessionIMBindings = bindings;
        if (bindings != null) publishSessionIMIdentity();
    }

    public JSONObject activation() {
        if (!productionMode || destroyed || requiresLogin() || !acceptedFriendsLoaded || currentSdkAppId() <= 0
                || authClient.accountSessionGeneration() <= 0 || !client.isLoggedInAs(settings.loginUserId())) return null;
        JSONObject value = new JSONObject();
        MaiSessionIMBindingService.put(value, "sdk_app_id", Integer.toString(currentSdkAppId()));
        MaiSessionIMBindingService.put(value, "owner_id", settings.loginUserId());
        MaiSessionIMBindingService.put(value, "auth_generation", Long.toString(authClient.accountSessionGeneration()));
        List<String> friends = new ArrayList<>(bindingFriendSnapshot);
        Collections.sort(friends);
        MaiSessionIMBindingService.put(value, "friends", new JSONArray(friends));
        JSONObject source = new JSONObject();
        MaiSessionIMBindingService.put(source, "profile", "android");
        MaiSessionIMBindingService.put(source, "path", appContext.getDatabasePath(historyStore.getDatabaseName()).getAbsolutePath());
        MaiSessionIMBindingService.put(source, "provenance", "owner_only_legacy");
        MaiSessionIMBindingService.put(value, "history_source", source);
        return value;
    }

    @Override public void publishIngressState(Runnable publication) {
        if (client == null) publication.run(); else client.publishIngressState(publication);
    }

    @Override public RemoteIMIngress.Fence historyFence(JSONObject task) {
        JSONObject scope = task.optJSONObject("scope"); String peer = task.optString("peer_id");
        if (ingressWriter == null || client == null || !current(scope, peer))
            throw new java.util.concurrent.CancellationException("历史绑定身份已变化");
        int generation = accountGeneration; long epoch = client.sessionIMLoginEpoch();
        RemoteIMIngress.Scope sdk = new RemoteIMIngress.Scope(currentSdkAppId(), settings.loginUserId(), epoch);
        java.util.function.BooleanSupplier current = () -> generation == accountGeneration && current(scope, peer)
            && client.isIngressScopeCurrent(sdk);
        return client.captureIngressFence(sdk).afterWriter(() -> ingressWriter.fence(current)).requireCurrent(current);
    }

    public boolean current(JSONObject scope, String peer) {
        return scope != null && productionMode && !destroyed && !requiresLogin() && acceptedFriendsLoaded
            && settings.loginUserId().equals(scope.optString("owner_id"))
            && Integer.toString(currentSdkAppId()).equals(scope.optString("sdk_app_id"))
            && Long.toString(authClient.accountSessionGeneration()).equals(scope.optString("auth_generation"))
            && (peer.isEmpty() || bindingFriendSnapshot.contains(peer));
    }

    @Override public void warning(String value) {
        reportError(value == null ? "会话绑定处理未完成" : value.substring(0, Math.min(200, value.length())));
    }

    private void reportError(String message) {
        if (listener == null) return;
        runOnMain(() -> listener.onError(message == null ? "操作失败" : message));
    }

    private void runOnMain(Runnable runnable) {
        if (destroyed) return;
        if (mainHandler == null || Looper.myLooper() == Looper.getMainLooper()) runnable.run();
        else mainHandler.post(() -> { if (!destroyed) runnable.run(); });
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    private final class ClientListener implements TencentIMClient.Listener {
        @Override public void onSelfProfileUpdated(TencentIMClient.SelfProfile profile) {
            final int generation = accountGeneration;
            runOnMain(() -> {
                if (generation != accountGeneration || requiresLogin() ||
                        !settings.loginUserId().equals(profile.imUserId) ||
                        !client.isSelfProfileReadCurrent(profile)) return;
                installSelfProfile(profile.imUserId, profile.nickname, profile.avatarUrl);
                notifyStateChanged();
            });
        }
        @Override
        public void onConnectionStateChanged(
            TencentIMClient.ConnectionState state,
            String detail
        ) {
            runOnMain(() -> {
                if (state != TencentIMClient.ConnectionState.CONNECTED) {
                    resetActivities();
                    synchronizedGroupIds.clear();
                    if (listener != null) listener.onActivityChanged(visibleConversationUserId);
                }
                connectionState = state;
                connectionDetail = detail;
                if (state == TencentIMClient.ConnectionState.FAILED
                    && "登录凭证已过期，请重新登录".equals(detail)
                    && authClient.hasSession()) {
                    retryConnection();
                    return;
                }
                if (state == TencentIMClient.ConnectionState.CONNECTED) {
                    final int generation = accountGeneration;
                    final String owner = settings.loginUserId();
                    client.getSelfProfile(owner,
                        () -> !destroyed && generation == accountGeneration &&
                            owner.equals(settings.loginUserId()),
                        new TencentIMClient.SelfProfileCompletion() {
                            @Override public void onSuccess(TencentIMClient.SelfProfile profile) {
                                if (generation == accountGeneration) onSelfProfileUpdated(profile);
                            }
                            @Override public void onError(int code, String message) { }
                        });
                    AvatarImageLoader.beginConnection();
                    for (RemoteIMContact contact : chatState.contacts())
                        AvatarImageLoader.prefetch(contact.avatarUrl());
                    refreshContactMetadata();
                    refreshAcceptedFriends();
                    refreshJoinedGroups();
                }
                notifyStateChanged();
            });
        }

        @Override
        public void onVideoMediaUpdated(
                String sender, String recipient, String remoteId, RemoteIMVideoAttachment attachment) {
            runOnMain(() -> {
                if (requiresLogin() || !chatState.ownerUserId().equals(recipient)) return;
                pendingVideoMedia.put(remoteId, attachment);
                if (pendingVideoMedia.size() > 128)
                    pendingVideoMedia.remove(pendingVideoMedia.keySet().iterator().next());
                RemoteIMMessage current = chatState.updateVideoMedia(remoteId, attachment);
                if (current != null) {
                    pendingVideoMedia.remove(remoteId); persistMessage(current);
                } else {
                    enqueueIO(() -> {
                        RemoteIMMessage stored = historyStore.messageWithRemoteId(recipient, sender, remoteId);
                        if (stored != null)
                            historyStore.upsertMessage(recipient, stored.withVideoAttachment(attachment));
                    });
                }
                mediaRevision++; notifyStateChanged();
            });
        }

        @Override
        public void onIncomingActivity(String sender, String recipient, RemoteIMActivitySignal signal) {
            runOnMain(() -> {
                if (requiresLogin() || !chatState.ownerUserId().equals(recipient)
                    || sender.equals(recipient) || !contactExists(sender)) return;
                if (activities.receive(sender, signal, android.os.SystemClock.elapsedRealtime()) && listener != null) {
                    listener.onActivityChanged(sender);
                }
                scheduleActivityExpiry();
            });
        }

        @Override
        public void onIncomingMessage(RemoteIMMessage message) {
            runOnMain(() -> receive(message));
        }

        @Override public TencentIMClient.LiveAdmission captureLiveAdmission(String sender, String recipient,
                String messageId, RemoteIMOrigin origin, boolean controlFrame, boolean hasContent) {
            MaiSessionIMBindingService target = sessionIMBindings;
            return target == null ? null : target.capture(sender, recipient, messageId, origin, controlFrame, hasContent);
        }

        @Override public TencentIMClient.LiveAuthority captureLiveAuthority() {
            MaiSessionIMBindingService target = sessionIMBindings;
            return target == null ? (sender, recipient, id, origin, control, content) -> null : target.captureAuthority();
        }

        @Override public java.util.concurrent.CompletableFuture<RemoteIMIngress.Stored> persistRawMetadata(RemoteIMIngress.Metadata metadata) {
            return persistRawIncoming(metadata);
        }

        @Override public void onRawPersistenceFailure(RemoteIMIngress.Scope scope) {
            if (!destroyed && !requiresLogin() && client != null && client.isIngressScopeCurrent(scope)
                    && settings.loginUserId().equals(scope.ownerId)) reportError("本机消息元数据保存失败，请检查本机存储");
        }

        @Override public void onIncomingMessage(RemoteIMMessage message, TencentIMClient.LiveAdmission admission) {
            runOnMain(() -> receive(message, admission));
        }

        @Override public void onGroupMembershipChanged() {
            runOnMain(RemoteIMSessionController.this::refreshJoinedGroups);
        }

        @Override public void onIncomingGroupText(String ownerUserId, RemoteIMGroupMessage message) {
            runOnMain(() -> {
                if (requiresLogin() || !ownerUserId.equals(chatState.ownerUserId())) return;
                boolean inserted = mergeGroupMessage(message);
                if (!inserted) return;
                persistGroupMessage(message);
                refreshGroupSenderNames(Collections.singletonList(message));
                if (!message.senderId().equals(chatState.ownerUserId())
                    && !message.groupId().equals(visibleGroupId))
                    unreadByGroupId.put(message.groupId(),groupUnreadCount(message.groupId()) + 1);
                if (!groupsById.containsKey(message.groupId())) refreshJoinedGroups();
                notifyStateChanged();
            });
        }

        @Override
        public void onProfilesUpdated(List<RemoteIMContact> contacts) {
            onProfilesUpdated(contacts, client == null ? -1 : client.selfProfileReadRevision());
        }

        @Override
        public void onProfilesUpdated(List<RemoteIMContact> contacts, long selfRevision) {
            int generation = accountGeneration;
            String owner = chatState.ownerUserId();
            runOnMain(() -> {
                // SDK 已核对请求所属账号；这里还要拒绝排队期间发生切号的回调。
                if (destroyed || requiresLogin() || generation != accountGeneration
                    || !owner.equals(chatState.ownerUserId())) return;
                selfProfileState.activate(owner, nickname());
                for (RemoteIMContact contact : contacts) {
                    // SDK 回调排队到 main 期间也可能完成头像写入，故应用前再次核对读代际。
                    if (contact != null && owner.equals(contact.userId()) && client != null
                        && !client.isSelfProfileReadRevisionCurrent(selfRevision)) continue;
                    if (contact != null && owner.equals(contact.userId())) {
                        installSelfProfile(owner, contact.displayName(), contact.avatarUrl());
                        continue;
                    }
                    // 资料只更新现有好友，不把旧账号本人或失效查询结果变成新联系人。
                    if (contact == null || !contactExists(contact.userId())) continue;
                    chatState.upsertContact(contact);
                    String accountName = friendDisplayByIm.get(contact.userId());
                    RemoteIMContact current = accountName == null ? contact :
                        chatState.setContactDisplayName(contact.userId(), accountName);
                    persistContact(current == null ? contact : current);
                    if (connectionState == TencentIMClient.ConnectionState.CONNECTED)
                        AvatarImageLoader.prefetch(contact.avatarUrl());
                }
                notifyStateChanged();
            });
        }

        @Override
        public void onPresenceUpdated(Map<String, TencentIMClient.PresenceStatus> statuses) {
            runOnMain(() -> {
                presenceByUserId.putAll(statuses);
                notifyStateChanged();
            });
        }
    }

    private static final class EmptyOperationCompletion implements TencentIMClient.OperationCompletion {
        @Override
        public void onSuccess() {
        }

        @Override
        public void onError(int code, String message) {
        }
    }
}
