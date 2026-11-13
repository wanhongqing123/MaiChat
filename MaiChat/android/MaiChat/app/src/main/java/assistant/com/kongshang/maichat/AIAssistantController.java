// 文件作用：Android 创作中心的会话、消息和 Agent 交互。
// AIAssistantController 负责流程编排、异步回调和状态转换。
// 主要入口：shared、configureAccount、invalidateSharedTemporaryModelCredentials、nativeCreate。
package com.kongshang.maichat;

import android.content.Context;
import android.database.Cursor;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Matrix;
import android.media.ExifInterface;
import android.net.Uri;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.util.Base64;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.ByteBuffer;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.nio.file.AtomicMoveNotSupportedException;
import java.security.MessageDigest;
import java.util.HashMap;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;
import org.json.JSONArray;
import org.json.JSONObject;

/** Native core, JSON, SQLite, credentials and imports are confined to the named worker. */
public final class AIAssistantController {
    interface HostToolHandler {
        Object execute(String toolName, JSONObject arguments) throws Exception;
    }
    interface Listener {
        void onState(State state);
    }
    // Ordinary read-only transport seam. Callers still pass through the controller's owner,
    // epoch and ACK checks; no prompt, tool execution or mutable session command uses it.
    interface SessionReads {
        void history(JSONObject owner, String before, int limit,
                     java.util.function.BiConsumer<JSONObject, String> completion) throws Exception;
        String command(JSONObject owner, JSONObject request,
                       java.util.function.BiConsumer<String, String> sent,
                       java.util.function.BiConsumer<JSONObject, JSONObject> received) throws Exception;
    }
    static final class State {
        final JSONObject data;
        final JSONObject remoteSession;
        final String selected, baseUrl, model, reasoningEffort, policy, error, syncStatus, restoreDraft;
        final int restoreGeneration;
        final boolean remoteCommandPending;
        final boolean ready;
        final int messageHash, messageLength;
        State(JSONObject data, JSONObject remoteSession, String selected, String baseUrl,
              String model, String policy, String error, String syncStatus,
              String restoreDraft, int restoreGeneration, boolean remoteCommandPending,
              boolean ready) {
            this.data = data;
            // State 交给主线程后不再共享可变目录对象，避免点击捕获过程中轮次被后台改写。
            try { this.remoteSession = remoteSession == null ? null : new JSONObject(remoteSession.toString()); }
            catch (Exception invalid) { throw new IllegalArgumentException("Invalid remote owner snapshot", invalid); }
            this.selected = selected;
            this.model = AIAssistantSessionModels.model(data, remoteSession, selected,
                remoteSession == null ? model : "");
            this.baseUrl = this.model.startsWith("glm-") ? "https://open.bigmodel.cn/api/paas/v4"
                : this.model.startsWith("deepseek-") ? DEEPSEEK_URL : baseUrl;
            this.reasoningEffort = AIAssistantSessionModels.effort(data, remoteSession, selected,
                remoteSession == null ? data.optString("defaultReasoningEffort", "high") : "");
            this.policy = policy;
            this.error = error;
            this.syncStatus = syncStatus;
            this.restoreDraft = restoreDraft;
            this.restoreGeneration = restoreGeneration;
            this.remoteCommandPending = remoteCommandPending;
            this.ready = ready;
            // 在工作线程计算可见消息指纹，UI 只在正文或工具状态变化时重绑列表。
            JSONArray messages = data.optJSONArray("messages");
            String encoded = (messages == null ? "" : messages.toString())
                + String.valueOf(data.optJSONArray("questionMessages")) + String.valueOf(data.optJSONArray("questions"))
                + String.valueOf(data.optJSONArray("questionAnswerEchoes")) + String.valueOf(data.optJSONArray("messageDisplayUpdates"))
                + String.valueOf(data.optJSONObject("toolDetails"));
            this.messageHash = encoded.hashCode();
            this.messageLength = encoded.length();
        }
        JSONObject runtime(String session) {
            if (remoteSession != null && session.equals(remoteSession.optString("id"))) return remoteSession;
            JSONArray sessions = data.optJSONArray("sessions");
            if (sessions != null) for (int i = 0; i < sessions.length(); i++) {
                JSONObject item = sessions.optJSONObject(i);
                if (item != null && session.equals(item.optString("id"))) return item;
            }
            return new JSONObject();
        }
        boolean busy() {
            if (remoteSession != null) return remoteSession.optBoolean("busy");
            JSONArray sessions = data.optJSONArray("sessions");
            if (sessions != null)
                for (int i = 0; i < sessions.length(); i++) {
                    JSONObject s = sessions.optJSONObject(i);
                    if (s != null && selected.equals(s.optString("id")))
                        return s.optBoolean("busy");
                }
            return false;
        }
    }
    public static final class ImportedFile {
        public final String relativePath;
        public final String mimeType;
        final String displayName;
        final boolean image;
        ImportedFile(String relativePath, String mimeType, boolean image) {
            this(relativePath, mimeType, image, relativePath);
        }
        ImportedFile(String relativePath, String mimeType, boolean image, String displayName) {
            this.relativePath = relativePath;
            this.mimeType = mimeType;
            this.image = image;
            this.displayName = displayName;
        }
    }
    private static AIAssistantController shared;
    private static String currentAccountUserId = "";
    private static Context applicationContext;
    private static final Map<String, List<java.lang.ref.WeakReference<Thread>>> closingAccountWorkers = new HashMap<>();
    private static final Map<String, List<CompletableFuture<Void>>> closingAttachmentJobs = new HashMap<>();
    public static synchronized AIAssistantController shared(Context context) {
        applicationContext = context.getApplicationContext();
        if (shared == null)
            shared = new AIAssistantController(context, null);
        return shared;
    }
    /** 账号工具的只读归属检查不能顺带初始化另一个账号的 Agent 或触发凭据请求。 */
    public static synchronized AIAssistantController initializedAccountToolOwner() { return shared; }
    public static synchronized void configureAccount(String userId) {
        String next = userId == null ? "" : userId.trim();
        if (next.equals(currentAccountUserId) && (shared == null ||
                shared.accountToolLoginGeneration == shared.accountAuth.accountSessionGeneration())) return;
        MaiCloudDriveAgentTools.cancelAll(applicationContext);
        String legacyOwner = currentAccountUserId.isEmpty() ? next : currentAccountUserId;
        if (applicationContext != null && !legacyOwner.isEmpty()) {
            try { claimLegacyWorkspace(applicationContext, legacyOwner); }
            catch (IOException failure) {
                // 标记失败时新账号使用独立目录，不得误认领旧账号的 AI 数据库。
                Log.w("MaiChatAgent", "Legacy AI workspace ownership was not saved", failure);
            }
        }
        if (shared != null) {
            shared.close();
            shared = null;
        }
        currentAccountUserId = next;
    }
    public static synchronized void invalidateSharedTemporaryModelCredentials() {
        if (shared == null) return;
        shared.worker.post(() -> {
            shared.arkApiKey = "";
            shared.arkExpiresAt = 0;
            shared.nextArkRefreshAt = 0;
            shared.wanApiKey = "";
            shared.wanExpiresAt = 0;
            shared.nextWanRefreshAt = 0;
            shared.wanWorkspaceId = "";
        });
    }

    /** 注销清理必须等旧原生会话与 SQLite 句柄真正关闭，不能在异步 close 后立刻删除目录。 */
    static synchronized CompletableFuture<Void> stopForAcceptedAccountDeletion(String owner) {
        if (shared != null && owner.equals(shared.accountUserId)) {
            shared.close(); shared = null;
            if (owner.equals(currentAccountUserId)) currentAccountUserId = "";
        }
        List<Thread> closing = new ArrayList<>();
        List<java.lang.ref.WeakReference<Thread>> registered = closingAccountWorkers.get(owner);
        if (registered != null) for (java.lang.ref.WeakReference<Thread> reference : registered) {
            Thread worker = reference.get(); if (worker != null && worker.isAlive()) closing.add(worker);
        }
        List<CompletableFuture<Void>> attachments = new ArrayList<>(
            closingAttachmentJobs.getOrDefault(owner, java.util.Collections.emptyList()));
        CompletableFuture<Void> barrier = closing.isEmpty() && attachments.isEmpty() ? CompletableFuture.completedFuture(null)
            : CompletableFuture.runAsync(() -> {
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(30);
            for (Thread worker : closing) {
                long remaining = deadline - System.nanoTime();
                try { if (remaining > 0) TimeUnit.NANOSECONDS.timedJoin(worker, remaining); }
                catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); throw new java.util.concurrent.CompletionException(interrupted); }
                if (worker.isAlive()) throw new java.util.concurrent.CompletionException(new IOException("AI account is still closing"));
            }
            for (CompletableFuture<Void> preparation : attachments) {
                long remaining = deadline - System.nanoTime();
                try {
                    if (remaining <= 0) throw new java.util.concurrent.TimeoutException("Attachment cleanup is still running");
                    preparation.get(remaining, TimeUnit.NANOSECONDS);
                } catch (Exception failure) { throw new java.util.concurrent.CompletionException(failure); }
            }
        });
        return barrier.thenRun(() -> {
            // 仅服务端已接受的注销清私有待发正文；普通换号保留原账号队列。
            if (applicationContext != null && !applicationContext.getSharedPreferences(
                "maichat-agent-outgoing-" + owner, Context.MODE_PRIVATE).edit().clear().commit())
                throw new java.util.concurrent.CompletionException(new IOException("AI 待发内容清理失败"));
        });
    }
    static synchronized void registerClosingAccountWorker(String owner, Thread worker) {
        closingAccountWorkers.entrySet().removeIf(entry -> {
            entry.getValue().removeIf(reference -> { Thread previous = reference.get(); return previous == null || !previous.isAlive(); });
            return entry.getValue().isEmpty();
        });
        // 快速 A→B→A 时保留 A 的每一代活跃线程，后一次 close 不能覆盖前一代的屏障。
        closingAccountWorkers.computeIfAbsent(owner, ignored -> new ArrayList<>()).add(new java.lang.ref.WeakReference<>(worker));
    }
    static synchronized void registerClosingAttachmentJobs(String owner, CompletableFuture<Void> finished) {
        closingAttachmentJobs.values().forEach(jobs -> jobs.removeIf(CompletableFuture::isDone));
        closingAttachmentJobs.computeIfAbsent(owner, ignored -> new ArrayList<>()).add(finished);
    }
    private static native long nativeCreate();
    private static synchronized void onNativeSessionChanged(String sessionId) {
        if (shared != null && shared.sessionSync != null) shared.queueNativeSessionChange(sessionId);
    }
    private static native void nativeDestroy(long handle);
    private static native byte[] nativeRequest(long handle, byte[] request);
    static native byte[] nativeMatteVideo(byte[] arguments, byte[] workspace,
        byte[] modelPath, byte[] runtimePath);
    private static native boolean nativeSetHostToolHandler(long handle, AIAssistantController owner);
    private static native byte[] nativeFilterBitmap(Bitmap bitmap, String operation, int[] dimensions,
        byte[][] metadata, byte[] profile, byte[] source);
    private static native void nativeConfigurePortrait(byte[] models, byte[] runtime);
    private static native byte[] nativePortraitRequest(byte[] profile, byte[] request, byte[] output, int action);

    private final Context context;
    private final HandlerThread thread = new HandlerThread("MaiChat-Agent");
    private final Handler worker;
    private final AIAssistantPreparationJobs preparations;
    private long nativeEpoch;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final String testEndpoint;
    private final String accountUserId;
    private AIAssistantOutgoingQueue outgoingQueue;
    private final AIAssistantQuestionEchoes questionEchoes = new AIAssistantQuestionEchoes();
    private final Set<String> nativeChangedSessions = java.util.concurrent.ConcurrentHashMap.newKeySet();
    private final java.util.concurrent.atomic.AtomicBoolean nativeChangesPosted = new java.util.concurrent.atomic.AtomicBoolean();
    private final java.util.LinkedHashMap<String, String> childPublishRoots = new java.util.LinkedHashMap<>();
    private android.content.SharedPreferences outgoingPreferences;
    private final Set<String> outgoingActiveOwners = new HashSet<>();
    private final Map<String, String> outgoingActiveInputs = new HashMap<>();
    private final Map<String, OutgoingReceipt> outgoingReceipts = new HashMap<>();
    private final AIAssistantSessionModels.Remote remoteModels = new AIAssistantSessionModels.Remote();
    private final AIAssistantToolDetails toolDetails = new AIAssistantToolDetails();
    private final Map<String, String> remoteSteeringInputs = new java.util.LinkedHashMap<>();
    private static final class IncomingInput {
        final JSONObject action;
        final Consumer<JSONObject> completion;
        IncomingInput(JSONObject action, Consumer<JSONObject> completion) {
            this.action = action; this.completion = completion;
        }
    }
    // The durable WebSocket mailbox retains commands until their individual
    // owner receipts. Serialize attachment preparation only within one session.
    private final Map<String, java.util.ArrayDeque<IncomingInput>> incomingInputs = new HashMap<>();
    private String outgoingEmptySession = "";
    private static final class OutgoingReceipt {
        final JSONObject owner;
        final Consumer<JSONObject> completion;
        final boolean modelConfiguration;
        OutgoingReceipt(JSONObject owner, Consumer<JSONObject> completion) {
            this(owner, completion, false);
        }
        OutgoingReceipt(JSONObject owner, Consumer<JSONObject> completion, boolean modelConfiguration) {
            this.owner = owner; this.completion = completion; this.modelConfiguration = modelConfiguration;
        }
    }
    private final MaiChatAuthClient accountAuth;
    private final long accountToolLoginGeneration;
    private final AndroidSessionSyncCoordinator sessionSync;
    private final SessionReads sessionReads;
    private volatile Listener listener;
    private volatile HostToolHandler hostToolHandler;
    static final String GLM_CHAT_URL = "https://open.bigmodel.cn/api/paas/v4";
    static final String DEEPSEEK_URL = "https://api.deepseek.com";
    static final String DEFAULT_MODEL = "deepseek-flash";
    volatile State state = new State(new JSONObject(), null, "", DEEPSEEK_URL,
        DEFAULT_MODEL, "on-request", "", "", "", 0, false, false);
    private long handle;
    private long preparedLocalEpoch = -1;
    private File root;
    private File rvmModelFile;
    private volatile String selected = "", baseUrl = state.baseUrl, model = state.model, policy = state.policy,
                   error = "";
    private volatile String reasoningEffort = "high";
    private volatile AIAssistantModelSettings primarySettings = AIAssistantModelSettings.defaults();
    private volatile boolean changingModelSource, modelConfigurationAvailable;
    private final AtomicBoolean modelSettingsChange = new AtomicBoolean(true);
    private JSONObject committedModelConfiguration;
    private volatile String deepseekApiKey = "";
    private volatile String glmApiKey = "";
    private volatile String arkApiKey = "";
    private volatile long arkExpiresAt;
    private long nextArkRefreshAt;
    private volatile String wanApiKey = "";
    private volatile long wanExpiresAt;
    private long nextWanRefreshAt;
    private volatile String wanWorkspaceId = "";
    private volatile String miniMaxApiKey = "";
    private volatile JSONObject data = new JSONObject();
    private final AIAssistantMessagePages localPages = new AIAssistantMessagePages();
    private final AIAssistantMessagePages remotePages = new AIAssistantMessagePages(true);
    private JSONArray remoteSessions = new JSONArray();
    private JSONArray remoteDevices = new JSONArray();
    private final Map<String, Long> remoteCreatePendingUntil = new HashMap<>();
    private JSONObject selectedRemote;
    private JSONObject pendingRemoteSelection;
    private int remoteSelectionGeneration;
    private JSONObject remoteData = new JSONObject();
    private String remoteCommandStatus = "";
    private String remoteRestoreDraft = "";
    private int remoteRestoreGeneration;
    private static final class PendingRemoteFiles {
        final String session;
        final java.util.List<ImportedFile> files;
        PendingRemoteFiles(String session, java.util.List<ImportedFile> files) {
            this.session = session;
            this.files = files;
        }
    }
    private final Map<String, PendingRemoteFiles> pendingRemoteFilesById = new HashMap<>();
    private final Map<String, java.util.List<ImportedFile>> remoteRestoreFiles = new HashMap<>();
    private final Set<String> pendingRemoteCommandIds = new HashSet<>();
    private final Set<String> remoteDeliveryPendingIds = new HashSet<>();
    private final Map<String, String> pendingRemoteDraftById = new HashMap<>();
    private final Map<String, String> remoteOptimisticByCommandId = new HashMap<>();
    private final Map<String, Boolean> remoteCommandResultsById = new HashMap<>();

    private final Map<String, JSONArray> mediaMentionCache = new HashMap<>();
    private boolean ready;
    private volatile boolean closed;
    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (closed || listener == null)
                return;
            if (ready) {
                refreshSeedanceCredential(false);
                refreshBailianCredential(false);
                refresh(false);
            }
            worker.postDelayed(this, data.optBoolean("busy") ? 100 : 500);
        }
    };

    AIAssistantController(Context context, String testEndpoint) {
        this(context, testEndpoint, null);
    }
    AIAssistantController(Context context, String testEndpoint, SessionReads sessionReads) {
        this.context = context.getApplicationContext();
        this.sessionReads = sessionReads;
        accountAuth = MaiChatAuthClient.shared(this.context);
        accountToolLoginGeneration = accountAuth.accountSessionGeneration();
        if (testEndpoint != null && !BuildConfig.DEBUG)
            throw new IllegalArgumentException("Test endpoint in release build");
        this.testEndpoint = testEndpoint;
        accountUserId = testEndpoint == null ? currentAccountUserId : "";
        try {
            android.content.SharedPreferences saved = this.context.getSharedPreferences(
                "maichat-agent-outgoing-" + accountUserId, Context.MODE_PRIVATE);
            outgoingPreferences = saved;
            outgoingQueue = new AIAssistantOutgoingQueue(new AIAssistantOutgoingQueue.Storage() {
                public String read() { return saved.getString("entries", ""); }
                public boolean write(String value) {
                    android.content.SharedPreferences.Editor edit = saved.edit().putString("entries", value);
                    try {
                        JSONObject recovery = new JSONObject(saved.getString("recovery", "{}"));
                        JSONArray next = new JSONArray(value);
                        for (int i = 0; i < next.length(); i++) {
                            JSONObject payload = next.getJSONObject(i).getJSONObject("payload");
                            if (!recovery.optString("id").isEmpty()
                                && recovery.optString("id").equals(payload.optString("recoveryId")))
                                edit.remove("recovery");
                        }
                    } catch (Exception invalid) { return false; }
                    // 新 capture 和恢复稿的移交在同一次私有 prefs 事务完成，避免重启后重复恢复。
                    return edit.commit();
                }
            });
        } catch (IOException failure) { error = safeMessage(failure); }
        thread.start();
        worker = new Handler(thread.getLooper());
        preparations = new AIAssistantPreparationJobs(command -> {
            if (!worker.post(command)) throw new java.util.concurrent.RejectedExecutionException("Agent control queue closed");
        }, java.util.concurrent.Executors.newFixedThreadPool(2, command -> new Thread(command, "MaiChat-AttachmentIO")));
        worker.post(this::initialize);
        sessionSync = testEndpoint == null && !accountUserId.isEmpty()
            ? new AndroidSessionSyncCoordinator(this.context, this, accountAuth) : null;
        if (sessionSync != null) sessionSync.start();
    }
    void setListener(Listener next) {
        listener = next;
        worker.post(() -> {
            worker.removeCallbacks(poll);
            if (next != null && !closed) {
                publishState();
                poll.run();
            }
        });
    }
    void clearListener(Listener expected) {
        if (listener == expected)
            setListener(null);
    }
    void setHostToolHandler(HostToolHandler handler) {
        hostToolHandler = handler;
    }
    @SuppressWarnings("unused") // Called from MaiAgentJni.cpp on an agent worker thread.
    // 原生 Agent 工具回调在这里按名称分派到手机宿主。
    // 解析失败和权限拒绝都编码为可回传错误。
    private byte[] onNativeHostTool(byte[] toolBytes, byte[] argumentsBytes) {
        String tool = new String(toolBytes, StandardCharsets.UTF_8);
        String arguments = new String(argumentsBytes, StandardCharsets.UTF_8);
        if (tool.equals("mobile_decode_text")) {
            try {
                JSONObject request = new JSONObject(arguments);
                byte[] bytes = Base64.decode(request.getString("base64"), Base64.DEFAULT);
                if (bytes.length > 8 * 1024 * 1024)
                    return hostToolFailure("file exceeds the 8 MB text limit", "invalid_input");
                String decoded = AIAssistantTextDecoder.decode(bytes, request.getString("encoding"));
                return new JSONObject().put("ok", true).put("output", decoded)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Throwable failure) {
                return hostToolFailure(safeMessage(failure), "invalid_input");
            }
        }
        if (tool.equals("ark_api_key")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key",
                        System.currentTimeMillis() / 1000L < arkExpiresAt ? arkApiKey : ""))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Ark key is unavailable", "not_configured");
            }
        }
        if (tool.equals("mobile_ark_service_config")) {
            try {
                MaiChatAuthClient.MediaServiceAccess access = accountAuth.mediaServiceAccess();
                if (access == null)
                    return hostToolFailure("Account login is required", "not_configured");
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("base_url", access.baseUrl)
                        .put("token", access.accountToken))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Ark Assets service is not configured", "not_configured");
            }
        }
        if (tool.equals("glm_api_key")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key", testEndpoint == null ? glmApiKey : "test-key"))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("GLM key is unavailable", "not_configured");
            }
        }
        if (tool.equals("deepseek_api_key")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key", testEndpoint == null ? deepseekApiKey : "test-key"))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("DeepSeek key is unavailable", "not_configured");
            }
        }
        if (tool.equals("wan_credentials")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key",
                        System.currentTimeMillis() / 1000L < wanExpiresAt ? wanApiKey : "")
                        .put("workspace_id",
                            System.currentTimeMillis() / 1000L < wanExpiresAt
                                ? wanWorkspaceId : ""))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Wan credentials are unavailable", "not_configured");
            }
        }
        if (tool.equals("minimax_api_key")) {
            try {
                return new JSONObject().put("ok", true)
                    .put("output", new JSONObject().put("key", miniMaxApiKey))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Exception failure) {
                return hostToolFailure("Creative model key is unavailable", "not_configured");
            }
        }
        if (tool.equals("maichat_remote_session_list") ||
            tool.equals("maichat_remote_session_create") ||
            tool.equals("maichat_remote_session_delete") ||
            tool.equals("maichat_remote_session_set_workspace")) {
            if (Looper.myLooper() == Looper.getMainLooper())
                return hostToolFailure("远端会话工具不能在 UI 线程执行");
            try {
                JSONObject output = remoteSessionTool(tool, new JSONObject(arguments));
                return new JSONObject().put("ok", true).put("output", output)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Throwable failure) {
                return hostToolFailure(safeMessage(failure));
            }
        }
        HostToolHandler target = hostToolHandler;
        if (tool.equals("maichat_session_attachment_open")) {
            if (Looper.myLooper() == Looper.getMainLooper())
                return hostToolFailure("会话附件工具不能在 UI 线程执行", "internal");
            try {
                JSONObject input = new JSONObject(arguments);
                String session = input.optString("session_id");
                String id = input.optString("attachment_id");
                if (sessionSync == null || !hasLocalSession(session) ||
                    sessionSync.localLoginSessionId().isEmpty() || id.isEmpty())
                    return hostToolFailure("本机会话或同步登录状态不可用", "not_found");
                MaiSessionAttachments.Room room = new MaiSessionAttachments.Room(
                    sessionSync.localSlot(),sessionSync.localLoginSessionId(),session);
                MaiSessionAttachments.Downloaded downloaded =
                    new MaiSessionAttachments(context).download(id,room);
                ImportedFile file;
                try { file = importDownloadedSessionFile(downloaded); }
                finally { downloaded.file.delete(); }
                JSONObject output = new JSONObject().put("path",file.relativePath)
                    .put("mime_type",file.mimeType);
                return new JSONObject().put("ok",true).put("output",output)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Throwable failure) { return hostToolFailure(safeMessage(failure)); }
        }
        if (tool.equals("maichat_calendar_create_event")) {
            try {
                JSONObject input = new JSONObject(arguments);
                if (input.optString("session_id").isEmpty())
                    return hostToolFailure("所属 Session 无效", "invalid_input");
                JSONObject output = MaiSystemCalendarEvents.create(context, input);
                return new JSONObject().put("ok", true).put("output", output)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (SecurityException failure) {
                return hostToolFailure(safeMessage(failure), "not_configured");
            } catch (IllegalArgumentException failure) {
                return hostToolFailure(safeMessage(failure), "invalid_input");
            } catch (Exception failure) {
                return hostToolFailure(safeMessage(failure));
            }
        }
        if (target == null)
            return hostToolFailure("MaiChat 宿主工具尚未连接");

        if (tool.equals("mobile_transform_image") || tool.equals("mobile_beautify_image")
                || tool.equals("mobile_reshape_body")) {
            if (Looper.myLooper() == Looper.getMainLooper())
                return hostToolFailure("图片处理不能在 UI 线程执行", "internal");
            try {
                // 使用承载本次 native Agent 的实例，不能在账号切换后重新查 shared 并改新号文件。
                JSONObject input = new JSONObject(arguments);
                String workspace = input.optString("workspace_path"), session = input.optString("session_id");
                if (root == null || (!workspace.isEmpty() && !new File(root, "Workspace").getCanonicalFile()
                        .equals(new File(workspace).getCanonicalFile())) || (!session.isEmpty() && !hasLocalSession(session)))
                    return hostToolFailure("图片所属账号或会话已变化，请重新操作", "canceled");
                // 这些字段由共享 factory 注入，不属于模型修饰参数。
                input.remove("workspace_path");
                input.remove("session_id");
                if (tool.equals("mobile_beautify_image")) input.put("operation", "beautify");
                if (tool.equals("mobile_reshape_body")) input.put("operation", "reshape_body");
                return new JSONObject().put("ok", true).put("output", transformImageResultForHost(input))
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Throwable failure) {
                return hostToolFailure(safeMessage(failure));
            }
        }

        if (tool.equals("maichat_cancel_avatar")) {
            try {
                // 原生取消监视线程可并发进入；只投递给本地请求表，不等待主线程或网络。
                Object output = target.execute(tool, new JSONObject(arguments));
                return new JSONObject().put("ok", true).put("output", output)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Throwable failure) {
                return hostToolFailure("头像取消请求无效", "invalid_input");
            }
        }

        if (tool.equals("maichat_get_current_account") || tool.equals("maichat_set_avatar")) {
            if (Looper.myLooper() == Looper.getMainLooper())
                return hostToolFailure("账号资料工具不能在 UI 线程执行", "internal");
            try {
                JSONObject input = new JSONObject(arguments);
                if (!accountToolScopeMatches(accountUserId, accountToolLoginGeneration,
                        input.optString("workspace_path"), input.optString("session_id")))
                    return hostToolFailure("登录账号或工作区已变化，请重新操作", "canceled");
                // 身份来自承载原生 Agent 的实例；模型参数和新 shared 实例都不能重绑旧调用。
                input.put("_account_im_user_id", accountUserId)
                    .put("_account_auth_generation", accountToolLoginGeneration);
                Object output = target.execute(tool, input);
                return new JSONObject().put("ok", true).put("output", output)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Throwable failure) {
                return hostToolFailure("账号资料操作尚未确认，请检查登录状态和网络");
            }
        }

        if (tool.equals("maichat_get_messages") || tool.equals("maichat_search_messages")) {
            if (Looper.myLooper() == Looper.getMainLooper())
                return hostToolFailure("本机消息查询须在 Agent worker 执行", "internal");
            try {
                JSONObject input = new JSONObject(arguments);
                if (!accountToolScopeMatches(accountUserId, accountToolLoginGeneration,
                        input.optString("workspace_path"), input.optString("session_id")))
                    return hostToolFailure("登录账号或工作区已变化", "canceled");
                // 只读数据库调用在独立查询 worker 执行，绝不进入下方主线程同步桥。
                input.put("_account_im_user_id", accountUserId)
                    .put("_account_auth_generation", accountToolLoginGeneration);
                Object output = target.execute(tool, input);
                if (!accountToolIdentityMatches(accountUserId, accountToolLoginGeneration))
                    return hostToolFailure("登录账号已变化", "canceled");
                return new JSONObject().put("ok", true).put("output", output)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (IllegalArgumentException invalid) {
                return hostToolFailure(safeMessage(invalid), "invalid_input");
            } catch (Throwable failure) { return hostToolFailure(safeMessage(failure)); }
        }

        // Cloud callbacks post their result to MAIN. Calling the host on MAIN
        // and then waiting for that callback would deadlock every cloud tool.
        if (tool.startsWith("maichat_cloud_drive_")) {
            if (Looper.myLooper() == Looper.getMainLooper())
                return hostToolFailure("Cloud drive tools require an Agent worker", "internal");
            try {
                Object output = target.execute(tool, new JSONObject(arguments));
                return new JSONObject().put("ok", true).put("output", output)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (MaiCloudDriveAgentTools.Failure failure) {
                return hostToolFailure(safeMessage(failure), failure.code);
            } catch (Throwable failure) {
                return hostToolFailure(safeMessage(failure));
            }
        }

        final byte[][] response = new byte[1][];
        Runnable invoke = () -> {
            try {
                Object output = target.execute(tool, new JSONObject(arguments));
                response[0] = new JSONObject().put("ok", true).put("output", output)
                    .toString().getBytes(StandardCharsets.UTF_8);
            } catch (Throwable failure) {
                response[0] = hostToolFailure(safeMessage(failure));
            }
        };
        // 私有 PDF 密码桥自行做短时 UI 调度；安全报告在 worker 复用账号服务，避免网络阻塞主线程。
        if (tool.startsWith("mobile_") || tool.equals("generate_pdf") || tool.equals("pdf_password_request")
            || tool.equals("maichat_create_group") || tool.equals("maichat_get_group_messages")
            || tool.equals("maichat_send_group_text") || tool.equals("maichat_set_avatar")
            || tool.equals("maichat_list_safety_reports")) {
            if (Looper.myLooper() == Looper.getMainLooper())
                return hostToolFailure("此宿主工具不能在 UI 线程执行");
            invoke.run();
            return response[0];
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            invoke.run();
            return response[0];
        }
        CountDownLatch completed = new CountDownLatch(1);
        main.post(() -> {
            try { invoke.run(); }
            finally { completed.countDown(); }
        });
        try {
            completed.await();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return hostToolFailure("MaiChat 宿主工具执行被中断");
        }
        return response[0] == null ? hostToolFailure("MaiChat 宿主工具未返回结果") : response[0];
    }
    private static byte[] hostToolFailure(String message) {
        return hostToolFailure(message, "internal");
    }
    private static byte[] hostToolFailure(String message, String code) {
        try {
            return new JSONObject().put("ok", false).put("error", message).put("errorCode", code)
                .toString().getBytes(StandardCharsets.UTF_8);
        } catch (Exception ignored) {
            return "{\"ok\":false,\"error\":\"MaiChat host tool failed\"}"
                .getBytes(StandardCharsets.UTF_8);
        }
    }
    // 作用：初始化控制器依赖、回调和原生 Agent 会话。
    private void initialize() {
        try {
            requireModelOwner();
            System.loadLibrary("maichat_agent");
            handle = nativeCreate();
            if (!nativeSetHostToolHandler(handle, this))
                throw new IllegalStateException("无法注册 MaiChat 宿主工具");
            root = testEndpoint == null ? accountRoot()
                : new File(context.getNoBackupFilesDir(), "AIAssistantTest-" + UUID.randomUUID());
            if (!root.mkdirs() && !root.isDirectory())
                throw new IllegalStateException("无法创建 AI 工作区");
            requireModelOwner();
            try {
                rvmModelFile = prepareBundledModel("rvm_mobilenetv3_fp32.onnx",
                    "88d4531297118f595bf2fd60f6f566aec2e559393802d1f436c380f0cbbd2828");
            }
            catch (Exception failure) { Log.w("MaiChatAgent", "Agent model staging failed", failure); }
            try { preparePortraitModels(); }
            catch (Exception failure) { Log.w("MaiChatAgent", "Portrait models are unavailable"); }
            exportSystemCertificates();
            modelConfigurationAvailable = true;
            // Old account settings supply defaults only. Session models are authoritative in
            // SQLite; registering the local slot must not copy a 2B weight on cloud startup.
            File settings = new File(root, "settings.json");
            if (settings.isFile()) {
                JSONObject saved =
                    new JSONObject(new String(Files.readAllBytes(settings.toPath()), StandardCharsets.UTF_8));
                primarySettings = AIAssistantModelSettings.fromJson(saved);
            }
            primarySettings = primarySettings.selectSource(false).withOptions(DEFAULT_MODEL,
                primarySettings.policy, primarySettings.effort);
            bindPrimarySettings(primarySettings, null);
            publishState();
            if (testEndpoint == null) {
                try {
                    refreshAccountModelKeys();
                } catch (Exception failure) {
                    Log.w("MaiChatAgent", "Account model keys are unavailable");
                }
            }
            requireModelOwner();
            JSONObject configuration = primaryModelConfiguration(primarySettings, null,
                cloudKey(primarySettings));
            invokeNativeAgent(configuration);
            requireModelOwner();
            committedModelConfiguration = configuration;
            bindPrimarySettings(primarySettings, configuration);
            ready = true;
            refresh(true);
            JSONArray sessions = data.optJSONArray("sessions");
            if (sessions != null && sessions.length() > 0) {
                selected = sessions.getJSONObject(0).getString("id");
                refresh(true);
                prepareLocalModel(state.model);
            }
            // These are existing media-tool credentials, independent of the primary model.
            // Local model selection and configuration themselves never fetch model API keys.
            refreshSeedanceCredential(true);
            refreshBailianCredential(true);
        } catch (Throwable failure) {
            error = safeMessage(failure);
        } finally {
            changingModelSource = false;
            modelSettingsChange.set(false);
            if (!closed) publishState();
        }
    }
    static String supportedModelOrDefault(String selected) {
        return AIAssistantModelSettings.supportedCloud(selected) ? selected : DEFAULT_MODEL;
    }
    private static String safeMessage(Throwable error) {
        String detail = error.getMessage();
        return detail == null || detail.trim().isEmpty() ? "创作中心操作失败" : detail;
    }
    private static void claimLegacyWorkspace(Context context, String userId) throws IOException {
        File base = new File(context.getNoBackupFilesDir(), "AIAssistant");
        File marker = new File(base, "legacy-owner-user-id.txt");
        if (!marker.isFile() && new File(base, "sessions.sqlite").isFile())
            Files.write(marker.toPath(), userId.getBytes(StandardCharsets.UTF_8));
    }
    private File accountRoot() throws Exception {
        File base = new File(context.getNoBackupFilesDir(), "AIAssistant");
        if (!base.mkdirs() && !base.isDirectory())
            throw new IOException("无法创建 AI 工作区");
        if (accountUserId.isEmpty()) return new File(base, "Guest");
        File marker = new File(base, "legacy-owner-user-id.txt");
        String legacyOwner = marker.isFile()
            ? new String(Files.readAllBytes(marker.toPath()), StandardCharsets.UTF_8).trim() : "";
        if (accountUserId.equals(legacyOwner)) return base;
        byte[] digest = MessageDigest.getInstance("SHA-256")
            .digest(accountUserId.getBytes(StandardCharsets.UTF_8));
        StringBuilder folder = new StringBuilder(digest.length * 2);
        for (byte value : digest) {
            folder.append(Character.forDigit((value >>> 4) & 15, 16));
            folder.append(Character.forDigit(value & 15, 16));
        }
        return new File(new File(base, "Accounts"), folder.toString());
    }
    private static final class NativeAgentFailure extends IOException {
        NativeAgentFailure(String message) { super(message); }
    }
    private JSONObject invokeNativeAgentResponse(JSONObject request) throws Exception {
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new IllegalStateException("Native agent on UI thread");
        if (handle == 0)
            throw new IllegalStateException("AI 核心尚未初始化");
        byte[] response = nativeRequest(handle, request.toString().getBytes(StandardCharsets.UTF_8));
        if (response == null)
            throw new IllegalStateException("AI 核心未返回结果");
        return new JSONObject(new String(response, StandardCharsets.UTF_8));
    }
    private JSONObject invokeNativeAgent(JSONObject request) throws Exception {
        JSONObject result = invokeNativeAgentResponse(request);
        if (!result.optBoolean("ok"))
            throw new NativeAgentFailure(result.optString("error", "操作失败"));
        if (request.optString("op").equals("configure")) nativeEpoch++;
        return result;
    }
    private JSONObject makeOperationRequest(String operationName) throws Exception {
        return new JSONObject().put("op", operationName).put("session", selected);
    }
    /** 普通设置页与 Agent 配置使用同一组 App 自有目录；不需要初始化模型工具。 */
    public static final class StorageRoots {
        public final File temporary, cache, workspace;
        public final java.util.function.BooleanSupplier current;
        private StorageRoots(File temporary, File cache, File workspace,
                             java.util.function.BooleanSupplier current) {
            this.temporary = temporary; this.cache = cache; this.workspace = workspace;
            this.current = current;
        }
    }
    public StorageRoots storageRootsForHost() throws IOException {
        final File ownerRoot = root;
        if (ownerRoot == null || closed) throw new IOException("账号工作区暂不可用");
        File temporary = new File(context.getFilesDir(), "storage-temporary");
        File cache = context.getCacheDir();
        File workspace = new File(ownerRoot, "Workspace");
        for (File directory : new File[]{temporary, cache, workspace})
            if (!directory.mkdirs() && !directory.isDirectory()) throw new IOException("无法打开存储目录");
        return new StorageRoots(temporary, cache, workspace, () -> !closed && root == ownerRoot
            && accountToolLoginGeneration == accountAuth.accountSessionGeneration());
    }
    private JSONObject primaryModelConfiguration(AIAssistantModelSettings settings, File localFile,
                                                 String key) throws Exception {
        requireModelOwner();
        StorageRoots storage = storageRootsForHost();
        File workspace = storage.workspace;
        File runtime = new File(context.getApplicationInfo().nativeLibraryDir,
            "libonnxruntime.so");
        JSONObject request = settings.configuration(localFile, key);
        request.put("localModelName", AIAssistantSessionModels.LOCAL)
            .put("glmBaseUrl", testEndpoint == null ? GLM_CHAT_URL : testEndpoint)
            .put("deepseekBaseUrl", testEndpoint == null ? DEEPSEEK_URL : testEndpoint);
        if (testEndpoint != null && !settings.isLocal()) {
            request.put("baseUrl", testEndpoint).put("model", "test-model")
                .put("wire", "chat_completions").put("apiKey", "test-key");
        }
        return request.put("op", "configure").put("session", selected)
                .put("database", new File(root, "sessions.sqlite").getPath())
                .put("scheduleDeviceName",
                    context.getResources().getConfiguration().smallestScreenWidthDp >= 600
                        ? "Android tablet" : "Android phone")
                .put("workspace", workspace.getPath())
                .put("temporaryDirectory", storage.temporary.getPath())
                .put("cacheDirectory", storage.cache.getPath())
                .put("appRoot", context.getFilesDir().getParentFile().getCanonicalPath())
                .put("rvmModelPath", rvmModelFile != null ? rvmModelFile.getPath() : "")
                .put("ortRuntimePath", runtime.isFile() ? runtime.getPath() : "")
                .put("caBundle", new File(root, "trusted-roots.pem").getPath());
    }

    private File resolvePrimaryModel(AIAssistantModelSettings settings) throws Exception {
        requireModelOwner();
        if (!settings.isLocal()) return null;
        if (!AIAssistantModelSettings.LOCAL_MODEL_ID.equals(MaiBundledPrimaryModel.MODEL_ID))
            throw new IOException("内置主模型身份不一致，请更新完整 App");
        File localFile = MaiBundledPrimaryModel.resolve(context);
        // Hashing/copying the immutable public weight can outlive a login or account deletion.
        // It must never proceed to the old account's native handle or settings after that point.
        requireModelOwner();
        return localFile;
    }

    private String cloudKey(AIAssistantModelSettings settings) {
        return cloudKey(settings.cloudModel);
    }

    private String cloudKey(String model) {
        if (testEndpoint != null) return "test-key";
        return model.startsWith("deepseek-") ? deepseekApiKey : glmApiKey;
    }

    private void bindPrimarySettings(AIAssistantModelSettings settings, JSONObject configuration) {
        primarySettings = settings;
        model = configuration == null ? settings.model() : configuration.optString("model");
        baseUrl = configuration == null ? settings.baseUrl() : configuration.optString("baseUrl");
        policy = settings.policy;
        reasoningEffort = settings.effort;
    }

    private boolean ownsModelConfiguration() {
        synchronized (AIAssistantController.class) {
            return !closed && (testEndpoint != null || (accountUserId.equals(currentAccountUserId)
                && accountToolLoginGeneration == accountAuth.accountSessionGeneration()));
        }
    }

    private void requireModelOwner() throws IOException {
        if (!ownsModelConfiguration()) throw new IOException("账号已切换，未修改主模型");
    }

    private File prepareBundledModel(String name, String expected) throws Exception {
        File directory = new File(root, "Models");
        if (!directory.mkdirs() && !directory.isDirectory())
            throw new IOException("Cannot create Agent model directory");
        File target = new File(directory, name);
        if (target.isFile() && expected.equals(sha256(target))) return target;
        File staged = new File(directory, name + ".part");
        try {
            try (InputStream input = context.getAssets().open(
                     "MaiAgentModels/" + name);
                 FileOutputStream output = new FileOutputStream(staged)) {
                byte[] bytes = new byte[1024 * 1024];
                int count;
                while ((count = input.read(bytes)) != -1) output.write(bytes, 0, count);
                output.getFD().sync();
            }
            if (!expected.equals(sha256(staged)))
                throw new IOException("Bundled Agent model checksum is invalid: " + name);
            try {
                Files.move(staged.toPath(), target.toPath(),
                    StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
            } catch (AtomicMoveNotSupportedException unsupported) {
                Files.move(staged.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
            }
            return target;
        } finally {
            Files.deleteIfExists(staged.toPath());
        }
    }

    private void preparePortraitModels() throws Exception {
        String[][] models = {
            {"face_landmarks.onnx", "f38c3321ceffbc9e95103480ad38cc3f52e7e1bde2bcee7cd9355d0b9138ac0c"},
            {"selfie_multiclass.onnx", "35ec1ecd9ee7f85073c99c00020b7f6751b69506eeacf683bc8665f6117f85b0"},
            {"person_detection.onnx", "47fd5599d6fa17608f03e0eb0ae230baa6e597d7e8a2c8199fe00abea55a701f"},
            {"pose_estimation.onnx", "9d89c599319a18fb7d2e28451a883476164543182bafca5f09eb2cf767ed2f3f"}
        };
        File directory = new File(root, "PortraitModels");
        if (!directory.mkdirs() && !directory.isDirectory()) throw new IOException("Cannot create portrait model directory");
        for (String[] model : models) {
            File target = new File(directory, model[0]);
            if (target.isFile() && sha256(target).equals(model[1])) continue;
            File staged = new File(directory, model[0] + ".part");
            try {
                try (InputStream input = context.getAssets().open("MaiPortraitModels/" + model[0]);
                     FileOutputStream output = new FileOutputStream(staged)) {
                    byte[] buffer = new byte[1024 * 1024]; int count;
                    while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
                    output.getFD().sync();
                }
                if (!sha256(staged).equals(model[1])) throw new IOException("Portrait model checksum mismatch");
                Files.move(staged.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
            } finally { Files.deleteIfExists(staged.toPath()); }
        }
        nativeConfigurePortrait(directory.getPath().getBytes(StandardCharsets.UTF_8),
            new File(context.getApplicationInfo().nativeLibraryDir, "libonnxruntime.so").getPath().getBytes(StandardCharsets.UTF_8));
    }

    public JSONObject portraitPersonalizationForHost(JSONObject arguments, boolean feedback) throws Exception {
        JSONObject request = new JSONObject(arguments.toString());
        if (feedback) request.put("action", "record_feedback");
        return portraitEnvelope(nativePortraitRequest(new File(root, "PortraitPreferences").getPath().getBytes(StandardCharsets.UTF_8),
            request.toString().getBytes(StandardCharsets.UTF_8), null, 0));
    }
    private static JSONObject portraitEnvelope(byte[] response) throws Exception {
        if (response == null) throw new IOException("Portrait service returned no response");
        JSONObject envelope = new JSONObject(new String(response, StandardCharsets.UTF_8));
        if (!envelope.optBoolean("ok")) throw new IllegalArgumentException(envelope.optString("error", "Portrait operation failed"));
        return envelope.getJSONObject("output");
    }

    private static String sha256(File file) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        try (InputStream input = Files.newInputStream(file.toPath())) {
            byte[] bytes = new byte[1024 * 1024];
            int count;
            while ((count = input.read(bytes)) != -1) digest.update(bytes, 0, count);
        }
        StringBuilder hex = new StringBuilder(64);
        for (byte value : digest.digest()) {
            hex.append(Character.forDigit((value >> 4) & 15, 16));
            hex.append(Character.forDigit(value & 15, 16));
        }
        return hex.toString();
    }
    private void refresh(boolean force) {
        try {
            AIAssistantMessagePages.Ticket page = localPages.select("local:" + selected);
            JSONObject result = invokeNativeAgent(localPages.snapshotRequest(selected, force));
            if (result.optBoolean("changed")) {
                hideReasoningText(result);
                attachMediaMentions(result);
                JSONObject updates = new JSONObject().put("messages", result.optJSONArray("messageUpdates"));
                hideReasoningText(updates);
                attachMediaMentions(updates);
                localPages.acceptSnapshot(page, result);
                applyMessagePages(result, localPages);
                result.remove("messageUpdates");
                data = result;
                if (!result.optString("error").isEmpty())
                    error = result.optString("error");
                publishState();
            }
        } catch (Exception e) {
            error = safeMessage(e);
            publishState();
        }
    }
    private static void applyMessagePages(JSONObject target, AIAssistantMessagePages pages)
            throws org.json.JSONException {
        target.put("messages", pages.messages()).put("messageDisplayUpdates", pages.displayUpdates())
            .put("hasOlderMessages", pages.hasOlder())
            .put("loadingOlder", pages.loading());
    }
    private static String pageOwner(JSONObject remote) {
        return new JSONArray().put(remote.optString("device_slot"))
            .put(remote.optString("login_session_id")).put(remote.optString("id")).toString();
    }
    /** Existing history read APIs provide actual earlier rows; a failed read keeps its cursor and rows. */
    void loadOlder(Consumer<Boolean> completion) {
        final State clicked = state;
        worker.post(() -> {
            boolean remote = clicked.remoteSession != null;
            String owner = remote ? pageOwner(clicked.remoteSession)
                : "local:" + clicked.selected;
            if (closed || !clicked.selected.equals(state.selected)
                || (remote && !sameRemote(clicked.remoteSession, selectedRemote))
                || (!remote && selectedRemote != null)) {
                main.post(() -> completion.accept(false)); return;
            }
            AIAssistantMessagePages pages = remote ? remotePages : localPages;
            pages.select(owner);
            AIAssistantMessagePages.Ticket ticket = pages.beginOlder();
            if (ticket == null) { main.post(() -> completion.accept(false)); return; }
            try {
                applyMessagePages(remote ? remoteData : data, pages);
                publishState();
                if (remote) {
                    int selection = remoteSelectionGeneration;
                    sessionSync.readRemoteHistory(clicked.remoteSession, ticket.before,
                        AIAssistantMessagePages.OLDER_LIMIT, (page, failure) -> worker.post(() -> {
                        boolean accepted = false;
                        if (!closed && selection == remoteSelectionGeneration
                            && sameRemote(clicked.remoteSession, selectedRemote) && pages.matches(ticket)) {
                            try {
                                if (!failure.isEmpty() || page == null || !page.optBoolean("ok"))
                                    throw new IOException(failure.isEmpty() ? "读取历史消息失败" : failure);
                                sanitizeRemoteMedia(page.optJSONArray("messages"));
                                accepted = pages.acceptOlder(ticket, page.optJSONArray("messages"));
                                error = "";
                            } catch (Exception problem) { pages.failOlder(ticket); error = safeMessage(problem); }
                            try { applyMessagePages(remoteData, pages); } catch (Exception ignored) { }
                            publishState();
                        }
                        boolean result = accepted; main.post(() -> completion.accept(result));
                    }));
                    return;
                }
                JSONObject result = invokeNativeAgent(pages.pageRequest(clicked.selected, ticket));
                hideReasoningText(result); attachMediaMentions(result);
                boolean accepted = pages.acceptOlder(ticket, result.optJSONArray("messages"));
                applyMessagePages(data, pages); error = ""; publishState();
                main.post(() -> completion.accept(accepted));
            } catch (Exception problem) {
                pages.failOlder(ticket); error = safeMessage(problem);
                try { applyMessagePages(remote ? remoteData : data, pages); } catch (Exception ignored) { }
                publishState(); main.post(() -> completion.accept(false));
            }
        });
    }
    private static void hideReasoningText(JSONObject snapshot) {
        JSONArray messages = snapshot.optJSONArray("messages");
        if (messages == null) return;
        for (int i = 0; i < messages.length(); i++) {
            JSONObject message = messages.optJSONObject(i);
            JSONArray parts = message == null ? null : message.optJSONArray("parts");
            if (parts == null) continue;
            for (int j = 0; j < parts.length(); j++) {
                JSONObject part = parts.optJSONObject(j);
                if (part != null && part.optString("kind").equals("reasoning"))
                    part.remove("text");
            }
        }
    }
    // 作用：把回复里实际存在的媒体引用关联到消息展示项。
    private void attachMediaMentions(JSONObject snapshot) throws org.json.JSONException {
        JSONArray messages = snapshot.optJSONArray("messages");
        if (messages == null) return;
        if (mediaMentionCache.size() > 500) mediaMentionCache.clear();
        for (int i = 0; i < messages.length(); i++) {
            JSONObject message = messages.optJSONObject(i);
            if (message == null || message.optBoolean("active")
                || (message.optString("role").equals("assistant")
                    && message.optLong("completed") == 0)) continue;
            String key = selected + ":" + message.optString("id") + ":"
                + message.optLong("completed");
            JSONArray cards = mediaMentionCache.get(key);
            if (cards == null) {
                StringBuilder text = new StringBuilder();
                JSONArray parts = message.optJSONArray("parts");
                Set<String> alreadyShown = new HashSet<>();
                if (parts != null)
                    for (int j = 0; j < parts.length(); j++) {
                        JSONObject part = parts.optJSONObject(j);
                        if (part == null) continue;
                        if (part.optString("kind").equals("text"))
                            text.append(part.optString("text")).append('\n');
                        if (part.optString("kind").equals("video")
                            || part.optString("kind").equals("audio")) {
                            File existing = workspaceFile(part.optString("path"));
                            if (existing != null) alreadyShown.add(existing.getPath());
                        }
                        if (part.optString("tool").equals("agent_send_media")
                            && part.optString("state").equals("completed")) {
                            try {
                                JSONObject artifact = new JSONObject(part.optString("output"));
                                File existing = workspaceFile(artifact.optString("path"));
                                if (existing != null) alreadyShown.add(existing.getPath());
                            } catch (org.json.JSONException ignored) { }
                        }
                    }
                List<AIAssistantMediaMentions.Mention> found =
                    AIAssistantMediaMentions.cards(text.toString(), this::workspaceFile);
                cards = new JSONArray();
                for (AIAssistantMediaMentions.Mention mention : found) {
                    if (alreadyShown.contains(mention.file.getPath())) continue;
                    cards.put(new JSONObject()
                        .put("path", mention.file.getAbsolutePath())
                        .put("kind", mention.kind == AIAssistantMediaMentions.Kind.VIDEO
                            ? "video" : "audio"));
                }
                mediaMentionCache.put(key, cards);
            }
            message.put("inline_media", cards);
        }
    }
    private void publishState() {
        JSONObject visible = selectedRemote == null ? data : remoteData;
        String shown = selectedRemote == null ? selected : selectedRemote.optString("id");
        JSONObject visibleOwner = selectedRemote;
        try {
            visible = new JSONObject(visible.toString());
            JSONArray combined = new JSONArray();
            JSONArray local = data.optJSONArray("sessions");
            if (local != null) for (int i = 0; i < local.length(); i++) combined.put(local.get(i));
            for (int i = 0; i < remoteSessions.length(); i++) {
                JSONObject remote = remoteSessions.optJSONObject(i);
                if (remote != null) combined.put(remoteModels.project(remote));
            }
            if (selectedRemote != null) {
                visibleOwner = remoteModels.project(selectedRemote);
                visible.put("remoteModelPending", remoteModels.changing(selectedRemote));
            }
            visible.put("sessions", combined);
            visible.put("devices", new JSONArray(remoteDevices.toString()));
            toolDetails.select(accountUserId, accountToolLoginGeneration, shown, selectedRemote);
            toolDetails.observeMessages(visible.optJSONArray("messages"));
            toolDetails.observeMessages(visible.optJSONArray("messageDisplayUpdates"));
            visible.put("toolDetails", toolDetails.snapshot());
            visible.put("outgoing", outgoingQueue == null ? new JSONArray() : outgoingQueue.entries());
            visible.put("questionAnswerEchoes", questionEchoes.visible(outgoingOwner(shown, selectedRemote),
                visible.optJSONArray("messages"), visible.optJSONArray("questionMessages"),
                visible.optJSONArray("messageDisplayUpdates")));
            visible.put("emptyDraftSession", outgoingEmptySession);
            visible.put("defaultReasoningEffort", reasoningEffort);
            visible.put("outgoingRecovery", outgoingPreferences == null ? new JSONObject()
                : new JSONObject(outgoingPreferences.getString("recovery", "{}")));
        } catch (Exception ignored) { visible = data; }
        String status = pendingRemoteSelection != null
            ? "正在读取 " + pendingRemoteSelection.optString("device_name") + " 的对话…"
            : selectedRemote == null ? "" : remoteCommandStatus;
        if (ready && outgoingQueue != null) try {
            JSONArray queued = outgoingQueue.entries();
            Set<String> owners = new HashSet<>();
            for (int i = 0; i < queued.length(); i++) {
                // 发布状态不改写私有队列；异常条目保留，且不能因 Android 的 checked JSON API 卡住刷新。
                JSONObject entry = queued.optJSONObject(i);
                if (entry != null && !entry.optString("owner").isEmpty()) owners.add(entry.optString("owner"));
            }
            for (String owner : owners) worker.post(() -> drainOutgoing(owner));
        } catch (IOException ignored) { /* 保留私有记录，不能因发布界面而清除内容。 */ }
        State next = new State(visible, visibleOwner, shown, baseUrl, model, policy,
            error, status, remoteRestoreDraft, remoteRestoreGeneration,
            selectedRemote != null && !pendingRemoteCommandIds.isEmpty(), ready);
        state = next;
        Listener target = listener;
        if (target != null)
            main.post(() -> {
                if (listener == target && !closed)
                    target.onState(next);
            });
    }

    JSONArray localSessionsForSync() {
        if (listener == null && state.ready && !closed) {
            CompletableFuture<JSONArray> updated = new CompletableFuture<>();
            worker.post(() -> {
                try {
                    JSONObject snapshot = invokeNativeAgent(new JSONObject().put("op", "snapshot")
                        .put("session", "").put("messageLimit", 0).put("force", true));
                    JSONArray sessions = snapshot.optJSONArray("sessions");
                    JSONObject next = new JSONObject(data.toString());
                    next.put("sessions", sessions == null ? new JSONArray() : sessions);
                    data = next;
                    publishState();
                    updated.complete(sessions == null ? new JSONArray() : sessions);
                } catch (Exception failure) {
                    updated.completeExceptionally(failure);
                }
            });
            try { return new JSONArray(updated.get(3, TimeUnit.SECONDS).toString()); }
            catch (Exception ignored) { /* Fall back to the last published Session list. */ }
        }
        JSONArray sessions = data.optJSONArray("sessions");
        if (sessions == null) return new JSONArray();
        try { return new JSONArray(sessions.toString()); }
        catch (Exception ignored) { return new JSONArray(); }
    }

    private static boolean sameRemote(JSONObject first, JSONObject second) {
        return first != null && second != null &&
            first.optString("device_slot").equals(second.optString("device_slot")) &&
            first.optString("login_session_id").equals(second.optString("login_session_id")) &&
            first.optString("id").equals(second.optString("id"));
    }

    void acceptRemoteCatalog(JSONArray entries, JSONArray devices, String ownSlot) {
        worker.post(() -> {
            JSONArray next = new JSONArray();
            if (entries != null) for (int i = 0; i < entries.length(); i++) {
                JSONObject entry = entries.optJSONObject(i);
                if (entry != null && !ownSlot.equals(entry.optString("device_slot")))
                    next.put(entry);
            }
            long now = System.currentTimeMillis();
            java.util.Iterator<Map.Entry<String, Long>> pending =
                remoteCreatePendingUntil.entrySet().iterator();
            while (pending.hasNext()) {
                Map.Entry<String, Long> item = pending.next();
                boolean appeared = false;
                for (int i = 0; i < next.length(); i++) {
                    JSONObject entry = next.optJSONObject(i);
                    if (entry != null && item.getKey().equals(entry.optString("id"))) {
                        appeared = true;
                        break;
                    }
                }
                if (appeared || item.getValue() <= now) {
                    pending.remove();
                    continue;
                }
                for (int i = 0; i < remoteSessions.length(); i++) {
                    JSONObject entry = remoteSessions.optJSONObject(i);
                    if (entry != null && item.getKey().equals(entry.optString("id"))) {
                        boolean ownerOnline = false;
                        if (devices != null) for (int index = 0; index < devices.length(); index++) {
                            JSONObject device = devices.optJSONObject(index);
                            if (device != null && device.optBoolean("online") &&
                                entry.optString("device_slot").equals(device.optString("device_slot")) &&
                                entry.optString("login_session_id").equals(
                                    device.optString("login_session_id"))) {
                                ownerOnline = true;
                                break;
                            }
                        }
                        if (ownerOnline) next.put(entry);
                        else pending.remove();
                        break;
                    }
                }
            }
            remoteSessions = next;
            JSONArray nextDevices = new JSONArray();
            if (devices != null) for (int i = 0; i < devices.length(); i++) {
                JSONObject device = devices.optJSONObject(i);
                if (device != null && !ownSlot.equals(device.optString("device_slot")))
                    nextDevices.put(device);
            }
            remoteDevices = nextDevices;
            if (selectedRemote != null) {
                JSONObject updated = null;
                for (int i = 0; i < next.length(); i++) {
                    JSONObject entry = next.optJSONObject(i);
                    if (sameRemote(selectedRemote, entry)) { updated = entry; break; }
                }
                if (updated != null) selectedRemote = updated;
                else {
                    boolean ownerOnline = false;
                    for (int i = 0; i < nextDevices.length(); i++) {
                        JSONObject device = nextDevices.optJSONObject(i);
                        if (device != null && device.optBoolean("online") &&
                            selectedRemote.optString("device_slot").equals(
                                device.optString("device_slot")) &&
                            selectedRemote.optString("login_session_id").equals(
                                device.optString("login_session_id"))) {
                            ownerOnline = true;
                            break;
                        }
                    }
                    if (ownerOnline) {
                        selectedRemote = null;
                        pendingRemoteSelection = null;
                        remoteData = new JSONObject();
                        selected = "";
                        remoteCommandStatus = "";
                        error = "所属设备已删除这个对话";
                        refresh(true);
                    } else try {
                        selectedRemote = new JSONObject(selectedRemote.toString())
                            .put("online", false);
                    } catch (Exception ignored) { /* Keep the prior offline view. */ }
                }
            }
            publishState();
        });
    }

    void markRemoteCatalogOffline() {
        worker.post(() -> {
            boolean changed = false;
            for (int i = 0; i < remoteDevices.length(); i++) {
                JSONObject device = remoteDevices.optJSONObject(i);
                if (device == null || !device.optBoolean("online")) continue;
                try { device.put("online", false); changed = true; }
                catch (Exception ignored) { /* Already decoded JSON. */ }
            }
            for (int i = 0; i < remoteSessions.length(); i++) {
                JSONObject session = remoteSessions.optJSONObject(i);
                if (session == null || !session.optBoolean("online")) continue;
                try { session.put("online", false); changed = true; }
                catch (Exception ignored) { /* Already decoded JSON. */ }
            }
            if (selectedRemote != null && selectedRemote.optBoolean("online")) {
                try { selectedRemote.put("online", false); changed = true; }
                catch (Exception ignored) { /* Already decoded JSON. */ }
            }
            if (changed) publishState();
        });
    }

    static String publishedSessionId(String changed, JSONObject snapshot) {
        String root = snapshot.optString("rootSessionId");
        return root.isEmpty() ? changed : root;
    }
    private void queueNativeSessionChange(String sessionId) {
        if (closed || sessionId == null || sessionId.isEmpty()) return;
        nativeChangedSessions.add(sessionId);
        // Core 回调只排队，150ms 合并同一批流式事件；数据库/root 解析全部留在 controller worker。
        if (nativeChangesPosted.compareAndSet(false, true)) worker.postDelayed(() -> {
            nativeChangesPosted.set(false);
            if (closed || !ready || sessionSync == null) return;
            List<String> changed = new ArrayList<>(nativeChangedSessions);
            nativeChangedSessions.removeAll(changed);
            for (String id : changed) try {
                String root = id;
                if (!hasLocalSession(id)) {
                    root = childPublishRoots.get(id);
                    if (root == null) {
                        JSONObject snapshot = invokeNativeAgent(new JSONObject().put("op", "remote_snapshot")
                            .put("session", id).put("limit", 1));
                        root = publishedSessionId(id, snapshot);
                        childPublishRoots.put(id, root);
                        while (childPublishRoots.size() > 256)
                            childPublishRoots.remove(childPublishRoots.keySet().iterator().next());
                    }
                }
                sessionSync.schedulePublish(root);
            } catch (Exception removedSession) { /* 已删除的旧会话事件不发布到别的所有者。 */ }
        }, 150);
    }
    static void sanitizeHistoryForSync(JSONObject snapshot) {
        for (String field : new String[] {"messages", "messageUpdates"}) {
            JSONArray messages = snapshot.optJSONArray(field);
            if (messages != null) for (int i = 0; i < messages.length(); i++) {
                JSONObject message = messages.optJSONObject(i);
                JSONArray parts = message == null ? null : message.optJSONArray("parts");
                if (parts == null) continue;
                for (int j = 0; j < parts.length(); j++) {
                    JSONObject part = parts.optJSONObject(j);
                    if (part == null) continue;
                    part.remove("path");
                    String kind = part.optString("kind");
                    if (kind.equals("tool") && !part.optString("tool").equals("question")) {
                        part.remove("input"); part.remove("output"); part.remove("error");
                    } else if (kind.equals("reasoning")) part.remove("text");
                }
            }
        }
    }

    void historyForSync(String sessionId, String before, int limit, Consumer<JSONObject> completion) {
        historyForSync(sessionId, before, limit, false, completion);
    }
    void historyForSync(String sessionId, String before, int limit, boolean includeMessageUpdates,
                        Consumer<JSONObject> completion) {
        worker.post(() -> {
            JSONObject result;
            try {
                if (closed) throw new IOException("账号已切换");
                if (!hasLocalSession(sessionId)) throw new IOException("本机会话不存在");
                result = invokeNativeAgent(new JSONObject().put("op", "remote_snapshot")
                    .put("session", sessionId).put("before", before).put("includeMessageUpdates", includeMessageUpdates)
                    .put("limit", Math.max(1, Math.min(20, limit))));
                sanitizeHistoryForSync(result);
            } catch (Exception failure) {
                result = new JSONObject();
                try { result.put("ok", false).put("error", safeMessage(failure)); }
                catch (Exception ignored) { /* JSONObject has no external input here. */ }
            }
            completion.accept(result);
        });
    }

    private boolean hasLocalSession(String sessionId) {
        JSONArray sessions = data.optJSONArray("sessions");
        if (sessions == null) return false;
        for (int i = 0; i < sessions.length(); i++) {
            JSONObject entry = sessions.optJSONObject(i);
            if (entry != null && sessionId.equals(entry.optString("id"))) return true;
        }
        return false;
    }

    /** Android 本机会话统一使用本账号 Workspace；可以验证任一本机会话，无需切换 UI 选择。
     * 空上下文的普通资料查询仍由实例身份和登录代际绑定；有工作区时必须精确匹配。
     */
    public boolean accountToolScopeMatches(String expectedImUserId, long expectedGeneration,
                                           String workspacePath, String sessionId) {
        if (!accountToolIdentityMatches(expectedImUserId, expectedGeneration)) return false;
        if (workspacePath != null && !workspacePath.isEmpty()) {
            if (root == null) return false;
            try {
                if (!new File(root, "Workspace").getCanonicalFile()
                        .equals(new File(workspacePath).getCanonicalFile())) return false;
            } catch (IOException failure) { return false; }
        }
        return sessionId == null || sessionId.isEmpty() || hasLocalSession(sessionId);
    }

    /** SDK 已确认后的真实结果仍可读取；停止或删除发起 Session 不能撤回已发生的账号写入。 */
    public boolean accountToolIdentityMatches(String expectedImUserId, long expectedGeneration) {
        synchronized (AIAssistantController.class) {
            if (shared != this || closed || !accountUserId.equals(expectedImUserId) ||
                    expectedGeneration != accountToolLoginGeneration ||
                    accountToolLoginGeneration != accountAuth.accountSessionGeneration()) return false;
        }
        return true;
    }

    private JSONObject prepareIncomingInput(String sessionId, JSONObject action,
                                             AIAssistantPreparationJobs.Cancellation cancel) throws Exception {
        cancel.check();
        String operation = action.optString("action");
        JSONObject request = new JSONObject(action.toString()).put("session", sessionId);
        JSONArray synced = new JSONArray();
        String text = action.optString("text");
        JSONArray attachments = action.optJSONArray("attachments");
        if ((text.isEmpty() && (attachments == null || attachments.length() == 0)) ||
            text.getBytes(StandardCharsets.UTF_8).length > 16 * 1024 ||
            (attachments != null && attachments.length() > 10))
            throw new IOException("消息为空或过长");
        JSONArray images = new JSONArray();
        JSONArray videos = new JSONArray();
        StringBuilder documents = new StringBuilder();
        if (attachments != null && attachments.length() > 0) {
            String login = sessionSync == null ? "" :
                sessionSync.localLoginSessionId();
            if (login.isEmpty()) throw new IOException("会话同步登录状态不可用");
            MaiSessionAttachments transfer = new MaiSessionAttachments(context, accountUserId);
            MaiSessionAttachments.Room room = new MaiSessionAttachments.Room(
                sessionSync.localSlot(),login,sessionId);
            for (int i = 0; i < attachments.length(); i++) {
                cancel.check();
                JSONObject reference = attachments.optJSONObject(i);
                if (reference == null || reference.optString("attachment_id").isEmpty())
                    throw new IOException("会话附件引用无效");
                MaiSessionAttachments.Downloaded downloaded = transfer.download(
                    reference.optString("attachment_id"),room,cancel);
                ImportedFile file;
                try {
                    file = importDownloadedSessionFile(downloaded);
                    if (operation.equals("prompt")) {
                        JSONObject pinned = invokeNativeAgent(new JSONObject().put("op", "pin_input_file")
                            .put("path", file.relativePath).put("requestId", action.optString("requestId"))
                            .put("attachmentId", reference.optString("attachment_id")));
                        file = new ImportedFile(pinned.getString("path"), file.mimeType, file.image, file.displayName);
                    }
                } finally { downloaded.file.delete(); }
                // 随机导入副本尚未交给 Core；只清理被取消任务的副本，原附件引用保持可用。
                File preparedFile = workspaceFile(file.relativePath);
                if (preparedFile != null) cancel.onCancel(() -> preparedFile.delete());
                cancel.check();
                JSONObject record = new JSONObject()
                    .put("attachment_id",reference.optString("attachment_id"))
                    .put("name",downloaded.name)
                    .put("kind",downloaded.mediaType)
                    .put("mimeType",downloaded.contentType)
                    .put("path",file.relativePath);
                synced.put(record);
                if (file.image) images.put(new JSONObject()
                    .put("path",file.relativePath).put("mimeType",file.mimeType));
                else if (file.mimeType.startsWith("video/"))
                    videos.put(new JSONObject().put("path",file.relativePath)
                        .put("mimeType",file.mimeType));
                else documents.append("\n- ").append(file.relativePath);
            }
        }
        if (documents.length() > 0)
            text += "\n\n会话附件已保存到本机工作目录：" + documents;
        request.put("op", operation.equals("steer") ? "steer" : "enqueue").put("text",text)
            .put("images", images).put("videos", videos)
            .put("quoteMessageId", action.optString("quoteMessageId"));
        cancel.check();
        return new JSONObject().put("request", request).put("synced", synced);
    }
    private static JSONObject rejectedPreparation(Exception failure) {
        JSONObject result = new JSONObject();
        try { result.put("ok", false).put("error", safeMessage(failure)); }
        catch (Exception ignored) { }
        return result;
    }
    void commandForSync(String sessionId, JSONObject action, Consumer<JSONObject> completion) {
        worker.post(() -> {
            // A detail read returns the persisted original call. It never enters the prompt
            // queue, prepares a model, refreshes the conversation or reruns the tool.
            if (action != null && action.optString("action").equals("get_tool_detail")) {
                completion.accept(readToolDetailForSync(sessionId, action)); return;
            }
            if (action == null || !(action.optString("action").equals("prompt") || action.optString("action").equals("steer"))) {
                performCommandForSync(sessionId, action, completion); return;
            }
            java.util.ArrayDeque<IncomingInput> queue = incomingInputs.computeIfAbsent(sessionId,
                ignored -> new java.util.ArrayDeque<>());
            if (queue.size() >= 256) {
                completion.accept(rejectedPreparation(new IOException("所属设备输入队列已满，内容未受理"))); return;
            }
            queue.addLast(new IncomingInput(action, completion));
            if (queue.size() == 1) drainIncomingInput(sessionId);
        });
    }
    private JSONObject readToolDetailForSync(String sessionId, JSONObject action) {
        try {
            JSONObject request = AIAssistantToolDetails.nativeRequest(sessionId, action);
            if (!ownsModelConfiguration()) return AIAssistantToolDetails.failure(sessionId, action,
                "not_configured", "原账号已改变，未读取详情");
            if (!hasLocalSession(sessionId)) return AIAssistantToolDetails.failure(sessionId, action,
                "not_found", "本机会话不存在");
            // Keep the Core's bound failure and revision instead of throwing away its tuple.
            return invokeNativeAgentResponse(request);
        } catch (IllegalArgumentException invalid) {
            return AIAssistantToolDetails.failure(sessionId, action, "invalid_input", "工具详情请求无效");
        } catch (Exception failure) {
            return AIAssistantToolDetails.failure(sessionId, action, "internal", safeMessage(failure));
        }
    }

    void loadToolDetails(State clicked, String messageId, JSONObject part) {
        for (String field : AIAssistantToolDetails.FIELDS)
            loadToolDetail(clicked, messageId, part, field, false, false);
    }
    void loadToolDetail(State clicked, String messageId, JSONObject source, String field, boolean more, boolean retry) {
        final JSONObject captured = AIAssistantToolDetails.copy(source);
        worker.post(() -> {
            AIAssistantToolDetails.Ticket ticket = null;
            try {
                if (!ownsModelConfiguration() || clicked == null || !clicked.ready || pendingRemoteSelection != null
                    || !clicked.selected.equals(state.selected)
                    || (clicked.remoteSession == null ? selectedRemote != null : !sameRemote(clicked.remoteSession, selectedRemote))) return;
                JSONObject current = findVisibleToolPart(messageId, captured.optString("id"));
                if (current == null || !current.optString("detailRevision").equals(captured.optString("detailRevision"))
                    || !current.optString("callId").equals(captured.optString("callId"))) return;
                toolDetails.select(accountUserId, accountToolLoginGeneration, clicked.selected, selectedRemote);
                ticket = toolDetails.begin(messageId, current, field, more, retry);
                if (ticket == null) return;
                final AIAssistantToolDetails.Ticket pending = ticket;
                if (selectedRemote == null) {
                    toolDetails.bind(pending, pending.request.optString("requestId"));
                    JSONObject result = readToolDetailForSync(clicked.selected, pending.request);
                    if (!toolDetails.accept(pending.request.optString("requestId"), null, result))
                        toolDetails.fail(pending, "详情回执与原调用不符，可重试");
                } else {
                    if ((sessionSync == null && sessionReads == null) || !hasRemoteModelOwner(selectedRemote) || !selectedRemote.optBoolean("online"))
                        throw new IOException("所属设备离线，已加载详情保留，可重试");
                    String command = sendReadOnlyRemote(pending.owner, pending.request,
                        (message, failure) -> worker.post(() -> {
                            if (!failure.isEmpty()) {
                                toolDetails.fail(pending, "所属设备暂不可达，已加载详情保留，可重试"); publishState();
                            }
                        }));
                    toolDetails.bind(pending, command);
                    worker.postDelayed(() -> {
                        toolDetails.fail(pending, "读取所属设备详情超时，已加载内容保留，可重试"); publishState();
                    }, 20000);
                }
            } catch (Exception failure) {
                if (ticket != null) toolDetails.fail(ticket, safeMessage(failure));
            }
            if (ownsModelConfiguration()) publishState();
        });
    }
    private JSONObject findVisibleToolPart(String messageId, String partId) {
        JSONObject visible = selectedRemote == null ? data : remoteData;
        for (String field : new String[] {"messages", "messageDisplayUpdates"}) {
            JSONArray messages = visible.optJSONArray(field);
            if (messages == null) continue;
            for (int i = 0; i < messages.length(); i++) {
                JSONObject message = messages.optJSONObject(i);
                if (message == null || !messageId.equals(message.optString("id"))) continue;
                JSONArray parts = message.optJSONArray("parts");
                if (parts != null) for (int j = 0; j < parts.length(); j++) {
                    JSONObject part = parts.optJSONObject(j);
                    if (part != null && partId.equals(part.optString("id")) && part.optString("kind").equals("tool")) return part;
                }
            }
        }
        return null;
    }
    private String sendReadOnlyRemote(JSONObject owner, JSONObject request,
                                     java.util.function.BiConsumer<String, String> sent) throws Exception {
        String action = request.optString("action");
        if (!action.equals("get_tool_detail") && !action.equals("get_session"))
            throw new IllegalArgumentException("Only read-only session commands use this transport");
        if (sessionReads != null) return sessionReads.command(AIAssistantToolDetails.copy(owner),
            AIAssistantToolDetails.copy(request), sent, this::acceptRemotePayload);
        return sessionSync.sendToRemote(owner, "command", request, sent);
    }
    private void drainIncomingInput(String sessionId) {
        java.util.ArrayDeque<IncomingInput> queue = incomingInputs.get(sessionId);
        if (queue == null || queue.isEmpty()) return;
        IncomingInput input = queue.peekFirst();
        performCommandForSync(sessionId, input.action, result -> worker.post(() -> {
            if (incomingInputs.get(sessionId) != queue || queue.peekFirst() != input) return;
            queue.removeFirst();
            input.completion.accept(result);
            if (queue.isEmpty()) incomingInputs.remove(sessionId);
            else drainIncomingInput(sessionId);
        }));
    }
    private void performCommandForSync(String sessionId, JSONObject action, Consumer<JSONObject> completion) {
        worker.post(() -> {
            try {
                JSONObject captured = new JSONObject(action.toString());
                String operation = captured.optString("action");
                if (operation.equals("prompt") && captured.optString("requestId").isEmpty())
                    captured.put("requestId", UUID.randomUUID().toString());
                if (operation.equals("stop")) {
                    String turn = captured.optString("turnId");
                    if (!turn.isEmpty()) {
                        JSONObject runtime = outgoingRuntime(sessionId, null);
                        if (!runtime.optBoolean("busy") || !turn.equals(runtime.optString("turnId")))
                            throw new IOException("原任务已结束或改变，没有停止新的任务");
                    }
                    cancelPreparationsForSession(sessionId, null);
                }
                if (operation.equals("steer") && (!(captured.opt("turnId") instanceof String)
                    || !(captured.opt("requestId") instanceof String) || captured.optString("turnId").isEmpty()
                    || captured.optString("requestId").isEmpty()))
                    throw new IOException("补充需要原任务和请求编号，内容未派发");
                JSONArray files = captured.optJSONArray("attachments");
                String key = sessionId + "\u001f" + captured.optString("turnId") + "\u001f" + captured.optString("requestId");
                if ((operation.equals("prompt") || operation.equals("steer")) && files != null && files.length() > 0
                    && !(operation.equals("steer") && remoteSteeringInputs.containsKey(key))) {
                    if (closed || !hasLocalSession(sessionId)) throw new IOException("原会话不可用，附件未派发");
                    if (!preparations.start("incoming:" + sessionId, nativeEpoch, () -> nativeEpoch,
                        cancel -> prepareIncomingInput(sessionId, captured, cancel), (prepared, failure) -> {
                            if (failure != null) { completion.accept(rejectedPreparation(failure)); return; }
                            if (closed || accountAuth.accountSessionGeneration() != accountToolLoginGeneration
                                || !hasLocalSession(sessionId)) {
                                completion.accept(rejectedPreparation(new IOException("原会话已改变，附件未派发")));
                                return;
                            }
                            executeCommandForSync(sessionId, captured, prepared, completion);
                        })) throw new IOException("原会话已有附件准备任务，内容未派发");
                } else executeCommandForSync(sessionId, captured, null, completion);
            } catch (Exception failure) { completion.accept(rejectedPreparation(failure)); }
        });
    }
    private void executeCommandForSync(String sessionId, JSONObject action, JSONObject preparedOverride,
                                       Consumer<JSONObject> completion) {
        JSONObject result = new JSONObject();
        try {
            if (closed) throw new IOException("账号已切换");
            if (!hasLocalSession(sessionId)) throw new IOException("本机会话不存在");
            if (action == null) throw new IOException("远端操作无效");
            String operation = action.optString("action");
            JSONObject request = new JSONObject(action.toString()).put("op", operation)
                .put("session", sessionId);
            JSONArray synced = new JSONArray();
            String importKey = sessionId + "\u001f" + action.optString("turnId")
                + "\u001f" + action.optString("requestId");
            String fingerprint = operation.equals("steer") ? AndroidSessionSyncCoordinator.fingerprint(
                new JSONObject().put("id", sessionId), action) : "";
            JSONObject prepared = remoteSteeringInputs.containsKey(importKey)
                ? new JSONObject(remoteSteeringInputs.get(importKey)) : null;
            if (operation.equals("steer") && prepared != null) {
                if (!fingerprint.equals(prepared.getString("fingerprint")))
                    throw new IOException("同一补充编号的内容已改变，未重复执行");
                request = prepared.getJSONObject("request");
                synced = prepared.getJSONArray("synced");
            } else if (operation.equals("prompt") || operation.equals("steer")) {
                JSONObject input = preparedOverride == null
                    ? prepareIncomingInput(sessionId, action, new AIAssistantPreparationJobs.Cancellation())
                    : preparedOverride;
                request = input.getJSONObject("request");
                synced = input.getJSONArray("synced");
                if (operation.equals("steer")) {
                    // Core 按完整输入去重；同编号重试必须复用第一次导入路径，不能再生成随机副本。
                    if (action.optString("turnId").isEmpty() || action.optString("requestId").isEmpty())
                        throw new IOException("补充缺少原任务和请求编号");
                    remoteSteeringInputs.put(importKey, new JSONObject().put("fingerprint", fingerprint)
                        .put("request", request).put("synced", synced).toString());
                    while (remoteSteeringInputs.size() > 128)
                        remoteSteeringInputs.remove(remoteSteeringInputs.keySet().iterator().next());
                }
            } else if (operation.equals("get_session") || operation.equals("set_model")) {
                // This allowlist is deliberately separate from trusted local JSON operations.
                // A peer can choose a registered model, never submit a path, endpoint or key.
                java.util.Iterator<String> fields = action.keys();
                while (fields.hasNext()) {
                    String field = fields.next();
                    if (!field.equals("action") && !field.equals("requestId")
                        && !(operation.equals("set_model") && (field.equals("model")
                            || field.equals("reasoningEffort"))))
                        throw new IOException("远端主模型参数无效");
                }
                request = new JSONObject().put("op", operation).put("session", sessionId);
                if (operation.equals("set_model")) {
                    if (!AIAssistantModelSettings.supportedCloud(action.optString("model")))
                        throw new IOException("请选择受支持的云端主模型");
                    requireModelOwner();
                    if (testEndpoint == null && cloudKey(action.optString("model")).isEmpty())
                        refreshAccountModelKeys();
                    requireModelOwner();
                    request.put("model", action.getString("model"));
                    if (action.has("reasoningEffort")) request.put("reasoningEffort",
                        action.getString("reasoningEffort"));
                }
            } else if (!operation.equals("stop") && !operation.equals("permission") &&
                !operation.equals("answer") && !operation.equals("rename")) {
                throw new IOException("不支持的远端操作");
            }
            if (operation.equals("stop") && !action.optString("turnId").isEmpty()) {
                JSONObject runtime = outgoingRuntime(sessionId, null);
                if (!runtime.optBoolean("busy") || !action.optString("turnId").equals(runtime.optString("turnId")))
                    throw new IOException("原任务已结束或改变，没有停止新的任务");
            }
            JSONObject accepted = AIAssistantSessionModels.executeSessionRequest(request,
                this::invokeNativeAgent, this::prepareLocalModel);
            result = operation.equals("get_session") || operation.equals("set_model") ? accepted : operation.equals("prompt")
                ? new JSONObject().put("ok", true).put("user_message_id", accepted.getString("id"))
                : AndroidSessionSyncCoordinator.commandReceipt(action, accepted);
            if (result.optBoolean("ok") && (operation.equals("prompt") || operation.equals("steer"))) {
                try {
                    JSONObject latest = invokeNativeAgent(new JSONObject()
                        .put("op", "remote_snapshot").put("session", sessionId)
                        .put("limit", 1));
                    // 收据的 user 可能已离开最新窗口；使用共享核心的普通精读入口，不拼造分页游标。
                    JSONObject exact = invokeNativeAgent(new JSONObject().put("op", "message")
                        .put("session", sessionId).put("id", result.getString("user_message_id")));
                    JSONArray recent = exact.optJSONArray("messages");
                    if (recent != null) for (int i = 0; i < recent.length(); i++) {
                        JSONObject message = recent.optJSONObject(i);
                        if (message != null && message.optString("id").equals(result.optString("user_message_id"))) {
                            if (synced.length() > 0 && sessionSync != null) {
                                JSONObject decorated = new JSONObject(message.toString());
                                JSONArray parts = decorated.optJSONArray("parts");
                                if (parts == null) parts = new JSONArray();
                                for (int j = 0; j < synced.length(); j++) {
                                    JSONObject record = synced.getJSONObject(j);
                                    JSONObject matching = null;
                                    for (int k = 0; k < parts.length(); k++) {
                                        JSONObject part = parts.optJSONObject(k);
                                        if (part != null &&
                                            record.optString("path").equals(part.optString("path")) &&
                                            record.optString("kind").equals(part.optString("kind"))) {
                                            matching = part;
                                            break;
                                        }
                                    }
                                    if (matching != null) matching
                                        .put("attachment_id",record.optString("attachment_id"))
                                        .put("name",record.optString("name"));
                                    else parts.put(new JSONObject()
                                        .put("id","session_attachment_" +
                                            record.optString("attachment_id"))
                                        .put("kind",record.optString("kind"))
                                        .put("mimeType",record.optString("mimeType"))
                                        .put("attachment_id",record.optString("attachment_id"))
                                        .put("name",record.optString("name")));
                                }
                                for (int k = 0; k < parts.length(); k++) {
                                    JSONObject part = parts.optJSONObject(k);
                                    if (part != null) part.remove("path");
                                }
                                decorated.put("parts",parts);
                                sessionSync.publishAttachedMessage(sessionId,decorated,
                                    latest.optJSONArray("permissions"),
                                    latest.optJSONArray("questions"), latest.optJSONArray("questionMessages"), latest.optBoolean("busy"),
                                    latest.optString("turnId"));
                            }
                            break;
                        }
                    }
                } catch (Exception ignored) { /* The send already succeeded. */ }
            }
            refresh(true);
        } catch (Exception failure) {
            try { result.put("ok", false).put("error", safeMessage(failure)); }
            catch (Exception ignored) { /* JSONObject has no external input here. */ }
        }
        completion.accept(result);
    }

    void lifecycleForSync(JSONObject action, Consumer<JSONObject> completion) {
        worker.post(() -> {
            JSONObject result = new JSONObject();
            try {
                if (closed || !ready) throw new IOException("所属设备尚未就绪");
                if (action == null) throw new IOException("远端操作无效");
                String operation = action.optString("action");
                if (operation.equals("session_create")) {
                    if (!action.optString("workspace_path").isEmpty())
                        throw new IOException("Android 会话使用应用工作区，不支持指定路径");
                    JSONObject created = invokeNativeAgent(new JSONObject().put("op", "create"));
                    String id = created.optString("id");
                    if (id.isEmpty()) throw new IOException("所属设备没有返回会话 ID");
                    result.put("ok", true).put("session_id", id);
                } else if (operation.equals("session_delete")) {
                    String id = action.optString("target_session_id");
                    if (id.isEmpty() || !hasLocalSession(id))
                        throw new IOException("所属设备的会话不存在");
                    cancelPreparationsForSession(id, null);
                    invokeNativeAgent(new JSONObject().put("op", "delete").put("session", id));
                    if (id.equals(selected)) selected = "";
                    result.put("ok", true).put("session_id", id);
                } else if (operation.equals("session_set_workspace")) {
                    throw new IOException("Android 会话使用应用沙盒工作区，不支持远端切换路径");
                } else throw new IOException("不支持的远端会话操作");
                error = "";
                refresh(true);
                if (sessionSync != null) sessionSync.requestPresenceRefresh();
            } catch (Exception failure) {
                String detail = safeMessage(failure);
                if (detail.length() > 120) detail = detail.substring(0, 120);
                try { result.put("ok", false).put("error", detail); }
                catch (Exception ignored) { /* Fixed result fields. */ }
            }
            completion.accept(result);
        });
    }

    void createOnDevice(JSONObject device, String workspacePath, Consumer<Boolean> completion) {
        if (sessionSync == null || device == null || !device.optBoolean("online")) {
            worker.post(() -> {
                error = "所属设备离线或同步服务不可用";
                publishState();
                main.post(() -> completion.accept(false));
            });
            return;
        }
        try {
            JSONObject action = remoteCreateAction(workspacePath);
            sessionSync.sendLifecycle(device, action, (result, failure) -> worker.post(() -> {
                if (!failure.isEmpty() || result == null || !result.optBoolean("ok") ||
                    result.optString("session_id").isEmpty()) {
                    error = !failure.isEmpty() ? failure : result == null
                        ? "所属设备没有返回创建结果" : result.optString("error", "创建失败");
                    publishState();
                    main.post(() -> completion.accept(false));
                    return;
                }
                try {
                    JSONObject created = new JSONObject(device.toString())
                        .put("id", result.optString("session_id"))
                        .put("title", "新对话").put("busy", false);
                    JSONArray next = new JSONArray();
                    next.put(created);
                    for (int i = 0; i < remoteSessions.length(); i++)
                        next.put(remoteSessions.get(i));
                    remoteSessions = next;
                    remoteCreatePendingUntil.put(created.optString("id"),
                        System.currentTimeMillis() + 30000);
                    error = "";
                    publishState();
                    selectRemote(created, completion);
                } catch (Exception decode) {
                    error = safeMessage(decode);
                    publishState();
                    main.post(() -> completion.accept(false));
                }
            }));
        } catch (Exception failure) {
            worker.post(() -> {
                error = safeMessage(failure);
                publishState();
                main.post(() -> completion.accept(false));
            });
        }
    }

    static JSONObject remoteCreateAction(String workspacePath) throws Exception {
        JSONObject action = new JSONObject().put("action", "session_create");
        if (workspacePath != null && !workspacePath.trim().isEmpty())
            action.put("workspace_path", workspacePath.trim());
        return action;
    }

    static JSONObject remoteWorkspaceAction(String sessionId, String workspacePath)
            throws Exception {
        return new JSONObject().put("action", "session_set_workspace")
            .put("target_session_id", sessionId).put("workspace_path", workspacePath);
    }

    void deleteSession(JSONObject target, Consumer<Boolean> completion) {
        if (target == null) {
            main.post(() -> completion.accept(false));
            return;
        }
        String id = target.optString("id");
        if (!target.has("device_slot")) {
            worker.post(() -> {
                boolean success = false;
                try {
                    if (!hasLocalSession(id)) throw new IOException("本机会话不存在");
                    invokeNativeAgent(new JSONObject().put("op", "delete").put("session", id));
                    if (id.equals(selected)) selected = "";
                    error = "";
                    refresh(true);
                    if (sessionSync != null) sessionSync.requestPresenceRefresh();
                    success = true;
                } catch (Exception failure) {
                    error = safeMessage(failure);
                    publishState();
                }
                boolean finished = success;
                main.post(() -> completion.accept(finished));
            });
            return;
        }
        if (sessionSync == null || !target.optBoolean("online")) {
            worker.post(() -> {
                error = "所属设备离线，无法删除会话";
                publishState();
                main.post(() -> completion.accept(false));
            });
            return;
        }
        try {
            JSONObject action = new JSONObject().put("action", "session_delete")
                .put("target_session_id", id);
            sessionSync.sendLifecycle(target, action, (result, failure) -> worker.post(() -> {
                if (!failure.isEmpty() || result == null || !result.optBoolean("ok")) {
                    error = !failure.isEmpty() ? failure : result == null
                        ? "所属设备没有返回删除结果" : result.optString("error", "删除失败");
                    publishState();
                    main.post(() -> completion.accept(false));
                    return;
                }
                JSONArray kept = new JSONArray();
                for (int i = 0; i < remoteSessions.length(); i++) {
                    JSONObject entry = remoteSessions.optJSONObject(i);
                    if (entry != null && !sameRemote(entry, target)) kept.put(entry);
                }
                remoteSessions = kept;
                remoteCreatePendingUntil.remove(id);
                if (sameRemote(selectedRemote, target)) {
                    selectedRemote = null;
                    pendingRemoteSelection = null;
                    remoteData = new JSONObject();
                    selected = "";
                    remoteCommandStatus = "";
                    refresh(true);
                }
                error = "";
                publishState();
                main.post(() -> completion.accept(true));
            }));
        } catch (Exception failure) {
            worker.post(() -> {
                error = safeMessage(failure);
                publishState();
                main.post(() -> completion.accept(false));
            });
        }
    }

    JSONObject remoteSessionTool(String tool, JSONObject arguments) throws Exception {
        State snapshot = state;
        JSONArray devices = snapshot.data.optJSONArray("devices");
        JSONArray sessions = snapshot.data.optJSONArray("sessions");
        if (tool.equals("maichat_remote_session_list")) {
            if (arguments.length() != 0) throw new IOException("此工具不接受参数");
            JSONArray deviceItems = new JSONArray();
            if (devices != null) for (int i = 0; i < devices.length(); i++) {
                JSONObject entry = devices.optJSONObject(i);
                if (entry != null) deviceItems.put(new JSONObject()
                    .put("device_slot", entry.optString("device_slot"))
                    .put("device_name", entry.optString("device_name"))
                    .put("online", entry.optBoolean("online")));
            }
            JSONArray remoteOnly = new JSONArray();
            int total = 0;
            if (sessions != null) for (int i = 0; i < sessions.length(); i++) {
                JSONObject entry = sessions.optJSONObject(i);
                if (entry == null || !entry.has("device_slot")) continue;
                total++;
                if (remoteOnly.length() >= 100) continue;
                remoteOnly.put(new JSONObject().put("device_slot", entry.optString("device_slot"))
                    .put("session_id", entry.optString("id"))
                    .put("title", entry.optString("title"))
                    .put("busy", entry.optBoolean("busy"))
                    .put("online", entry.optBoolean("online")));
            }
            return new JSONObject().put("devices", deviceItems)
                .put("sessions", remoteOnly).put("total_sessions", total);
        }
        if (sessionSync == null) throw new IOException("设备同步服务不可用");
        String slot = arguments.optString("device_slot");
        if (slot.isEmpty()) throw new IOException("请指定所属设备的 device_slot");
        JSONObject owner = null;
        if (devices != null) for (int i = 0; i < devices.length(); i++) {
            JSONObject device = devices.optJSONObject(i);
            if (device != null && slot.equals(device.optString("device_slot"))) {
                owner = device;
                break;
            }
        }
        if (owner == null || !owner.optBoolean("online"))
            throw new IOException("所属设备离线或未登录");
        JSONObject action = new JSONObject();
        if (tool.equals("maichat_remote_session_create")) {
            if (arguments.length() > (arguments.has("workspace_path") ? 2 : 1))
                throw new IOException("创建会话参数无效");
            String path = arguments.optString("workspace_path").trim();
            action = remoteCreateAction(path);
        } else if (tool.equals("maichat_remote_session_delete") ||
                   tool.equals("maichat_remote_session_set_workspace")) {
            boolean changingWorkspace = tool.equals("maichat_remote_session_set_workspace");
            if (arguments.length() != (changingWorkspace ? 3 : 2))
                throw new IOException("会话操作参数无效");
            String id = arguments.optString("session_id");
            if (id.isEmpty()) throw new IOException("请指定 session_id");
            JSONObject found = null;
            if (sessions != null) for (int i = 0; i < sessions.length(); i++) {
                JSONObject entry = sessions.optJSONObject(i);
                if (entry != null && id.equals(entry.optString("id")) &&
                    slot.equals(entry.optString("device_slot"))) {
                    found = entry;
                    break;
                }
            }
            if (found == null) throw new IOException("所属设备的会话不存在");
            if (found.optBoolean("busy")) throw new IOException("对话仍在执行，请先停止");
            if (changingWorkspace) {
                if (!slot.equals("mac") && !slot.equals("windows"))
                    throw new IOException("只有 Desktop 会话支持远端切换工作区");
                String path = arguments.optString("workspace_path").trim();
                if (path.isEmpty() || path.length() > 4096)
                    throw new IOException("请填写所属电脑上已存在的绝对路径");
                action = remoteWorkspaceAction(id, path);
            } else action.put("action", "session_delete").put("target_session_id", id);
        } else throw new IOException("不支持的远端会话工具");
        CompletableFuture<JSONObject> completed = new CompletableFuture<>();
        sessionSync.sendLifecycle(owner, action, (result, failure) -> {
            try {
                completed.complete(new JSONObject().put("result", result == null
                    ? JSONObject.NULL : result).put("error", failure));
            } catch (Exception encoding) { completed.completeExceptionally(encoding); }
        });
        JSONObject outcome = completed.get(32, TimeUnit.SECONDS);
        String failure = outcome.optString("error");
        if (!failure.isEmpty()) throw new IOException(failure);
        JSONObject result = outcome.optJSONObject("result");
        if (result == null || !result.optBoolean("ok"))
            throw new IOException(result == null ? "所属设备没有返回执行结果" :
                result.optString("error", "所属设备未执行操作"));
        String id = result.optString("session_id");
        if (id.isEmpty()) throw new IOException("所属设备没有返回会话 ID");
        JSONObject output = new JSONObject().put("ok", true)
            .put("device_slot", slot).put("session_id", id);
        if (tool.equals("maichat_remote_session_set_workspace"))
            output.put("workspace_path", action.optString("workspace_path"));
        return output;
    }

    void acceptRemotePayload(JSONObject envelope, JSONObject payload) {
        worker.post(() -> {
            if (payload.optString("type").equals("command_result")) {
                String command = payload.optString("command_id");
                if (toolDetails.handles(command)) {
                    if (ownsModelConfiguration() && toolDetails.acceptCommandResult(envelope, payload)) publishState();
                    return; // A mismatched ACK leaves the original read pending.
                }
                OutgoingReceipt receipt = outgoingReceipts.get(command);
                if (receipt != null && AIAssistantSessionModels.Remote.matchesOwner(receipt.owner, envelope)) {
                    JSONObject result = payload.optJSONObject("result");
                    completeOutgoingReceipt(command, result == null ? new JSONObject() : result, result == null);
                    if (receipt.modelConfiguration) return;
                }
            }
            JSONObject owner = pendingRemoteSelection != null ? pendingRemoteSelection : selectedRemote;
            if (owner == null || !owner.optString("id").equals(envelope.optString("session_id")) ||
                !owner.optString("device_slot").equals(envelope.optString("from_slot")) ||
                !owner.optString("login_session_id").equals(envelope.optString("from_login_session_id")))
                return;
            if (payload.optString("type").equals("command_result")) {
                JSONObject result = payload.optJSONObject("result");
                String id = payload.optString("command_id");
                // Timed-out/unknown commands cannot clear a newer page's error or consume its
                // pending UI state. Model callbacks are handled exclusively by their ticket.
                if (!pendingRemoteCommandIds.contains(id) && !remoteDeliveryPendingIds.contains(id)) return;
                boolean ok = result != null && result.optBoolean("ok");
                boolean deliveryPending = remoteDeliveryPendingIds.contains(id);
                PendingRemoteFiles pendingFiles = pendingRemoteFilesById.remove(id);
                if (pendingFiles != null) {
                    if (ok) for (ImportedFile file : pendingFiles.files)
                        discardComposerMedia(file);
                    else if (!deliveryPending) restoreRemoteFiles(pendingFiles);
                }
                if (remoteDeliveryPendingIds.contains(id)) remoteCommandResultsById.put(id, ok);
                pendingRemoteCommandIds.remove(id);
                String draft = pendingRemoteDraftById.remove(id);
                settleRemoteOptimistic(id, result);
                if (!ok && draft != null) {
                    remoteRestoreDraft = draft;
                    remoteRestoreGeneration++;
                }
                if (pendingRemoteCommandIds.isEmpty()) remoteCommandStatus = "";
                if (!ok) error = result == null ? "所属设备未执行操作" : result.optString("error");
                else error = "";
                publishState();
                return;
            }
            JSONArray received = payload.optJSONArray("messages");
            if (!payload.optBoolean("ok") || received == null) return;
            // 群历史/状态事件比 15 秒 presence 更新更快；busy 与 turnId 必须一起更新所有者快照。
            try {
                for (int i = 0; i < remoteSessions.length(); i++) {
                    JSONObject candidate = remoteSessions.optJSONObject(i);
                    if (candidate != null && sameRemote(candidate, owner)) {
                        if (payload.has("busy")) candidate.put("busy", payload.optBoolean("busy"));
                        if (payload.has("turnId")) candidate.put("turnId", payload.optString("turnId"));
                    }
                }
                if (payload.has("busy")) owner.put("busy", payload.optBoolean("busy"));
                if (payload.has("turnId")) owner.put("turnId", payload.optString("turnId"));
            } catch (Exception ignored) { /* 原快照继续由目录更新，不猜测轮次。 */ }
            sanitizeRemoteMedia(received);
            if (pendingRemoteSelection != null) {
                if (!sameRemote(selectedRemote, pendingRemoteSelection)) {
                    pendingRemoteCommandIds.clear();
                    remoteDeliveryPendingIds.clear();
                    pendingRemoteDraftById.clear();
                    remoteOptimisticByCommandId.clear();
                    remoteCommandResultsById.clear();
                    remoteCommandStatus = "";
                }
                selectedRemote = pendingRemoteSelection;
                pendingRemoteSelection = null;
                remoteSelectionGeneration++;
                remoteData = payload;
            }
            try {
                AIAssistantMessagePages.Ticket ticket = remotePages.select(pageOwner(owner));
                remotePages.acceptSnapshot(ticket, payload);
                applyMessagePages(remoteData, remotePages);
                if (payload.has("permissions")) remoteData.put("permissions", payload.optJSONArray("permissions"));
                if (payload.has("questions")) remoteData.put("questions", payload.optJSONArray("questions"));
                if (payload.has("questionMessages")) remoteData.put("questionMessages", payload.optJSONArray("questionMessages"));
            } catch (Exception failure) { error = safeMessage(failure); publishState(); return; }
            error = "";
            publishState();
            try {
                String known = remoteModels.project(owner).optString("model");
                String observed = payload.optString("model");
                if (payload.optBoolean("initialHistoryPage")
                    || !observed.isEmpty() && !observed.equals(known))
                    requestRemoteModel(owner, null, null, null);
            } catch (Exception failure) { /* Existing history stays visible if metadata is unavailable. */ }
        });
    }

    private static void sanitizeRemoteMedia(JSONArray received) {
        if (received == null) return;
        for (int i = 0; i < received.length(); i++) {
            JSONObject message = received.optJSONObject(i);
            JSONArray parts = message == null ? null : message.optJSONArray("parts");
            if (parts == null) continue;
            int originalCount = parts.length();
            for (int j = 0; j < originalCount; j++) {
                JSONObject part = parts.optJSONObject(j);
                if (part == null) continue;
                String kind = part.optString("kind");
                if (!kind.equals("image") && !kind.equals("video")) continue;
                part.remove("path");
                if (!part.optString("attachment_id").isEmpty()) continue;
                try {
                    parts.put(new JSONObject().put("id", part.optString("id") + "-remote-placeholder")
                        .put("kind", "text").put("text", kind.equals("image")
                            ? "[图片在所属设备，暂不可跨设备预览]" :
                              "[视频在所属设备，暂不可跨设备预览]"));
                } catch (Exception ignored) { /* Already decoded JSON. */ }
            }
        }
    }

    private void beginRemoteCommand(String id, String draft) {
        beginRemoteCommand(id,draft,30000);
    }

    private void restoreRemoteFiles(PendingRemoteFiles pending) {
        if (pending == null || pending.files.isEmpty()) return;
        synchronized (remoteRestoreFiles) {
            remoteRestoreFiles.computeIfAbsent(pending.session,
                ignored -> new java.util.ArrayList<>()).addAll(pending.files);
        }
        remoteRestoreGeneration++;
    }

    Map<String, java.util.List<ImportedFile>> consumeRemoteRestoredFiles() {
        synchronized (remoteRestoreFiles) {
            Map<String, java.util.List<ImportedFile>> result =
                new HashMap<>(remoteRestoreFiles);
            remoteRestoreFiles.clear();
            return result;
        }
    }

    private void beginRemoteCommand(String id, String draft, long timeoutMs) {
        pendingRemoteCommandIds.add(id);
        remoteDeliveryPendingIds.add(id);
        if (draft != null && !draft.isEmpty()) pendingRemoteDraftById.put(id, draft);
        remoteCommandStatus = "正在送达所属设备…";
        worker.postDelayed(() -> {
            if (!pendingRemoteCommandIds.remove(id)) return;
            String saved = pendingRemoteDraftById.remove(id);
            restoreRemoteFiles(pendingRemoteFilesById.remove(id));
            settleRemoteOptimistic(id, null);
            if (saved != null) {
                remoteRestoreDraft = saved;
                remoteRestoreGeneration++;
            }
            if (pendingRemoteCommandIds.isEmpty()) remoteCommandStatus = "";
            error = "所属设备是否执行了操作尚未确定，请先查看会话记录，不要直接重复发送";
            publishState();
        }, timeoutMs);
    }

    private boolean settleRemoteDelivery(String id, String failure) {
        boolean stillPending = pendingRemoteCommandIds.contains(id);
        remoteDeliveryPendingIds.remove(id);
        Boolean confirmed = remoteCommandResultsById.remove(id);
        if (confirmed != null) return confirmed;
        if (!stillPending) return false;
        if (failure.isEmpty()) return true;
        pendingRemoteCommandIds.remove(id);
        pendingRemoteFilesById.remove(id);
        pendingRemoteDraftById.remove(id);
        settleRemoteOptimistic(id, null);
        if (pendingRemoteCommandIds.isEmpty()) remoteCommandStatus = "";
        error = "送达状态未知；再次发送相同内容会复用操作编号。" + failure;
        publishState();
        return false;
    }

    private void appendRemoteOptimistic(String temporaryId, String text) throws Exception {
        JSONArray current = remoteData.optJSONArray("messages");
        JSONArray updated = current == null ? new JSONArray() : new JSONArray(current.toString());
        long created = System.currentTimeMillis();
        updated.put(new JSONObject().put("id", temporaryId).put("role", "user")
            .put("created", created).put("completed", created).put("active", false)
            .put("parts", new JSONArray().put(new JSONObject()
                .put("id", "part_" + UUID.randomUUID()).put("kind", "text")
                .put("text", text))));
        remoteData.put("messages", updated);
    }

    private void settleRemoteOptimistic(String commandId, JSONObject result) {
        String temporaryId = remoteOptimisticByCommandId.remove(commandId);
        if (temporaryId == null) return;
        JSONArray current = remoteData.optJSONArray("messages");
        if (current == null) return;
        String canonicalId = result != null && result.optBoolean("ok")
            ? result.optString("user_message_id") : "";
        Map<String, JSONObject> ordered = new java.util.TreeMap<>();
        for (int index = 0; index < current.length(); index++) {
            JSONObject message = current.optJSONObject(index);
            if (message == null || temporaryId.equals(message.optString("id"))) continue;
            ordered.put(message.optString("id"), message);
        }
        if (!canonicalId.isEmpty() && !ordered.containsKey(canonicalId)) {
            for (int index = 0; index < current.length(); index++) {
                JSONObject message = current.optJSONObject(index);
                if (message == null || !temporaryId.equals(message.optString("id"))) continue;
                try { message.put("id", canonicalId); ordered.put(canonicalId, message); }
                catch (Exception ignored) { /* The canonical event will arrive separately. */ }
                break;
            }
        }
        JSONArray updated = new JSONArray();
        for (JSONObject message : ordered.values()) updated.put(message);
        try { remoteData.put("messages", updated); }
        catch (Exception ignored) { /* The next room event refreshes this view. */ }
    }
    void select(String id) {
        select(id, null);
    }
    void select(String id, Consumer<Boolean> completion) {
        worker.post(() -> {
            toolDetails.clearPending();
            boolean success = false;
            try {
                selectedRemote = null;
                pendingRemoteSelection = null;
                remoteSelectionGeneration++;
                remotePages.reset();
                pendingRemoteCommandIds.clear();
                remoteDeliveryPendingIds.clear();
                pendingRemoteDraftById.clear();
                remoteOptimisticByCommandId.clear();
                remoteCommandResultsById.clear();
                remoteCommandStatus = "";
                selected = id;
                error = "";
                refresh(true);
                prepareLocalModel(AIAssistantSessionModels.model(data, null, id, model));
                success = true;
            } catch (Exception failure) {
                error = safeMessage(failure);
                publishState();
            }
            if (completion != null) {
                boolean result = success;
                main.post(() -> completion.accept(result));
            }
        });
    }
    void selectRemote(JSONObject remote, Consumer<Boolean> completion) {
        worker.post(() -> {
            toolDetails.clearPending();
            if (sessionSync == null && sessionReads == null) {
                error = "设备同步暂时不可用";
                publishState();
                main.post(() -> completion.accept(false));
                return;
            }
            pendingRemoteSelection = remote;
            int generation = ++remoteSelectionGeneration;
            remotePages.reset();
            publishState();
            worker.postDelayed(() -> {
                if (generation != remoteSelectionGeneration || pendingRemoteSelection == null) return;
                pendingRemoteSelection = null;
                error = "读取所属设备会话超时，请重新选择";
                publishState();
            }, 20000);
            try {
                java.util.function.BiConsumer<JSONObject, String> readCompleted = (page, failure) -> {
                    worker.post(() -> {
                        boolean selectedByBroadcast = generation + 1 == remoteSelectionGeneration
                            && pendingRemoteSelection == null && sameRemote(selectedRemote, remote);
                        if ((generation != remoteSelectionGeneration && !selectedByBroadcast) || closed) {
                            main.post(() -> completion.accept(false)); return;
                        }
                        if (!failure.isEmpty() && sameRemote(pendingRemoteSelection, remote)) {
                            pendingRemoteSelection = null;
                            error = failure;
                            publishState();
                        }
                        if (failure.isEmpty() && page != null) {
                            try { page.put("initialHistoryPage", true); }
                            catch (Exception ignored) { /* Only the first history response sets its old-page boundary. */ }
                            JSONObject envelope = new JSONObject();
                            try { envelope.put("session_id", remote.optString("id"))
                                .put("from_slot", remote.optString("device_slot"))
                                .put("from_login_session_id", remote.optString("login_session_id")); }
                            catch (Exception ignored) { /* Fixed fields. */ }
                            acceptRemotePayload(envelope, page);
                        }
                        main.post(() -> completion.accept(failure.isEmpty()));
                    });
                };
                if (sessionReads != null) sessionReads.history(AIAssistantToolDetails.copy(remote), "",
                    AIAssistantMessagePages.LATEST_LIMIT, readCompleted);
                else sessionSync.readRemoteHistory(remote, "", AIAssistantMessagePages.LATEST_LIMIT, readCompleted);
            } catch (Exception failure) {
                pendingRemoteSelection = null;
                error = safeMessage(failure);
                publishState();
                main.post(() -> completion.accept(false));
            }
        });
    }

    private boolean hasRemoteModelOwner(JSONObject owner) {
        for (int i = 0; i < remoteSessions.length(); i++)
            if (sameRemote(owner, remoteSessions.optJSONObject(i))) return true;
        return false;
    }

    // The existing receipt map validates the command's owner slot/login/session before invoking
    // this callback. Transport delivery only acknowledges forwarding, never a model mutation.
    private void requestRemoteModel(JSONObject owner, String name, String effort,
                                    Consumer<Boolean> completion) {
        AIAssistantSessionModels.Remote.Ticket ticket = null;
        try {
            if (!ownsModelConfiguration() || (sessionSync == null && (name != null || sessionReads == null)) || !hasRemoteModelOwner(owner)
                || !owner.optBoolean("online")) throw new IOException("所属设备离线，原选择已保留");
            if (name == null && remoteModels.pending(owner)) return;
            if (name != null && (owner.optBoolean("busy") || remoteModels.changing(owner)))
                throw new IOException("原会话正在执行或等待模型确认，原选择已保留");
            JSONObject action = AIAssistantSessionModels.Remote.request(name, effort)
                .put("requestId", UUID.randomUUID().toString());
            ticket = remoteModels.begin(owner, name, effort);
            final AIAssistantSessionModels.Remote.Ticket captured = ticket;
            java.util.function.BiConsumer<String, String> sent = (message, failure) -> worker.post(() -> {
                    if (!failure.isEmpty()) completeOutgoingReceipt(message,
                        rejectedPreparation(new IOException(failure)), false);
                });
            String command = name == null ? sendReadOnlyRemote(captured.owner, action, sent)
                : sessionSync.sendToRemote(captured.owner, "command", action, sent);
            outgoingReceipts.put(command, new OutgoingReceipt(captured.owner, result -> {
                boolean accepted = false;
                try {
                    if (!ownsModelConfiguration() || !hasRemoteModelOwner(captured.owner)) {
                        remoteModels.finish(captured);
                    } else {
                        accepted = remoteModels.accept(captured, result);
                        if (accepted && sameRemote(selectedRemote, captured.owner)) error = "";
                    }
                } catch (Exception failure) {
                    if (ownsModelConfiguration() && sameRemote(selectedRemote, captured.owner))
                        error = safeMessage(failure);
                }
                if (ownsModelConfiguration()) publishState();
                if (completion != null) {
                    boolean success = accepted;
                    main.post(() -> completion.accept(success));
                }
            }, true));
            worker.postDelayed(() -> completeOutgoingReceipt(command,
                rejectedPreparation(new IOException("等待所属设备确认超时，原选择已保留")), false), 30000);
            publishState();
        } catch (Exception failure) {
            if (ticket != null) remoteModels.finish(ticket);
            if (ownsModelConfiguration() && sameRemote(selectedRemote, owner)) error = safeMessage(failure);
            if (ownsModelConfiguration()) publishState();
            if (completion != null) main.post(() -> completion.accept(false));
        }
    }
    void create() {
        create(null);
    }
    void create(Consumer<Boolean> completion) {
        actionWithResult("create", new JSONObject(), null, completion);
    }
    void retryLastRemoteInput() { retryRemoteInput(""); }
    void retryRemoteInput(String inputId) {
        worker.post(() -> {
            if (closed || outgoingQueue == null || selectedRemote == null) return;
            try {
                final String owner = outgoingOwner(selectedRemote.optString("id"), selectedRemote);
                JSONArray entries = outgoingQueue.entries();
                for (int i = 0; i < entries.length(); i++) {
                    JSONObject entry = entries.getJSONObject(i);
                    if ((!inputId.isEmpty() && !inputId.equals(entry.optString("id"))) ||
                        !owner.equals(entry.optString("owner")) ||
                        !entry.getJSONObject("payload").optString("operation").equals("enqueue")) continue;
                    String state = entry.optString("state");
                    if (!state.equals("unknown") && !state.equals("failed")) continue;
                    outgoingQueue.retryInput(entry.getString("id"));
                    error = "";
                    publishState(); drainOutgoing(owner); return;
                }
            } catch (Exception failure) { error = safeMessage(failure); publishState(); }
        });
    }

    void send(String text, Consumer<Boolean> completion) {
        send(text, java.util.Collections.emptyList(), completion);
    }
    void suggestReplies(JSONArray messages, java.util.function.BiConsumer<JSONObject, String> completion) {
        JSONArray context = messages == null ? new JSONArray() : messages;
        worker.post(() -> {
            JSONObject suggestions = null;
            String failure = null;
            try {
                suggestions = invokeNativeAgent(new JSONObject().put("op", "suggest_replies")
                    .put("messages", context));
            } catch (Exception e) {
                failure = safeMessage(e);
            }
            JSONObject result = suggestions;
            String errorMessage = failure;
            main.post(() -> completion.accept(result, errorMessage));
        });
    }
    void send(String text, java.util.List<ImportedFile> attachments, Consumer<Boolean> completion) {
        send(state.selected, text, attachments, "", completion);
    }
    // 发送目标由可见输入栏决定，不再重读可能先于面板更新的控制器选中状态。
    void send(String intendedSession, String text, java.util.List<ImportedFile> attachments,
              Consumer<Boolean> completion) {
        send(intendedSession, text, attachments, "", completion);
    }
    // 返回值只确认账号私有队列已持久接收，不是模型执行或远端接收的确认。
    boolean enqueue(String intendedSession, String text, java.util.List<ImportedFile> attachments,
                    String quotedMessageId) {
        return enqueue(state, intendedSession, text, attachments, quotedMessageId);
    }
    boolean enqueue(State visible, String intendedSession, String text, java.util.List<ImportedFile> attachments,
                    String quotedMessageId) {
        try {
            if (closed || !visible.ready || outgoingQueue == null) throw new IOException("发送队列尚未准备好");
            JSONObject remote = visible.remoteSession != null
                && intendedSession.equals(visible.remoteSession.optString("id")) ? visible.remoteSession : null;
            if (text.getBytes(StandardCharsets.UTF_8).length > 64 * 1024 || attachments.size() > 10
                || text.trim().isEmpty() && attachments.isEmpty()) throw new IOException("消息为空或过长，草稿已保留");
            JSONObject runtime = new JSONObject(visible.runtime(intendedSession).toString());
            JSONArray questions = intendedSession.equals(visible.selected) ? visible.data.optJSONArray("questions") : null;
            runtime.put("questions", questions == null ? new JSONArray() : questions)
                .put("questionAnswerEchoes", visible.data.optJSONArray("questionAnswerEchoes"));
            AIAssistantSubmission submission = AIAssistantSubmission.capture(intendedSession, remote, runtime);
            String questionId = submission.questionId();
            if (questionId.isEmpty() && remote == null && modelSettingsChange.get())
                throw new IOException("主模型正在切换，草稿已保留");
            if (!questionId.isEmpty() && (!attachments.isEmpty() || !quotedMessageId.isEmpty()))
                throw new IOException("问题回答只发送文字，附件和引用仍保留在输入栏");
            if (questionId.isEmpty() && remote == null && runtime.optBoolean("busy") && runtime.optString("turnId").isEmpty())
                throw new IOException("任务编号尚未就绪，草稿已保留");
            JSONArray files = new JSONArray();
            for (ImportedFile file : attachments) files.put(new JSONObject().put("path", file.relativePath)
                .put("mimeType", file.mimeType).put("image", file.image).put("name", file.displayName));
            JSONObject payload = new JSONObject().put("session", intendedSession).put("text", text)
                .put("quoteMessageId", quotedMessageId).put("files", files)
                .put("operation", !questionId.isEmpty() ? "answer" : remote != null ? "enqueue" : runtime.optBoolean("busy") ? "steer" : "")
                .put("turnId", remote == null ? runtime.optString("turnId") : "");
            if (!questionId.isEmpty()) payload.put("questionId", questionId).put("questionCandidates", questions)
                .put("question", new JSONObject(AIAssistantSubmission.questionToAnswer(questions,
                    visible.data.optJSONArray("questionAnswerEchoes")).toString()));
            payload.put("model", AIAssistantSessionModels.model(visible.data, remote,
                intendedSession, visible.model));
            payload.put("reasoningEffort", AIAssistantSessionModels.effort(visible.data, remote,
                intendedSession, visible.reasoningEffort));
            if (remote != null) payload.put("remote", new JSONObject(remote.toString()));
            String owner = outgoingOwner(intendedSession, remote);
            JSONObject recovery = new JSONObject(outgoingPreferences.getString("recovery", "{}"));
            if (owner.equals(recovery.optString("owner"))) payload.put("recoveryId", recovery.optString("id"));
            outgoingQueue.enqueue(owner, payload);
            worker.post(() -> { publishState(); drainOutgoing(owner); });
            return true;
        } catch (Exception failure) {
            error = safeMessage(failure); worker.post(this::publishState); return false;
        }
    }
    void send(String intendedSession, String text, java.util.List<ImportedFile> attachments,
              String quotedMessageId, Consumer<Boolean> completion) {
        boolean accepted = enqueue(intendedSession, text, attachments, quotedMessageId);
        if (completion != null) main.post(() -> completion.accept(accepted));
    }
    static String outgoingOwner(String session, JSONObject remote) {
        return remote == null ? "local:" + session : "remote:" + remote.optString("device_slot")
            + "\u001f" + remote.optString("login_session_id") + "\u001f" + session;
    }
    private void cancelPreparationsForSession(String session, JSONObject remote) {
        preparations.cancel(outgoingOwner(session, remote));
        if (remote == null) preparations.cancel("incoming:" + session);
    }
    private JSONObject outgoingRuntime(String target, JSONObject remote) throws Exception {
        if (remote == null) return invokeNativeAgent(new JSONObject().put("op", "remote_snapshot")
            .put("session", target).put("limit", 1));
        for (int i = 0; i < remoteSessions.length(); i++) {
            JSONObject candidate = remoteSessions.optJSONObject(i);
            if (candidate != null && sameRemote(candidate, remote) && candidate.optBoolean("online")) return candidate;
        }
        throw new IOException("原设备离线，内容保留在发送队列");
    }
    // Queue entries carry the clicked session's configuration. Changing another visible page
    // cannot retarget them, while an actual change on their owner leaves the input recoverable.
    private void validateOutgoingModel(JSONObject payload) throws Exception {
        requireModelOwner();
        JSONObject runtime = invokeNativeAgent(new JSONObject().put("op", "get_session")
            .put("session", payload.getString("session")));
        String actual = runtime.optString("model", model);
        String effort = runtime.optString("reasoningEffort");
        if (effort.isEmpty()) effort = reasoningEffort;
        if (!actual.equals(payload.optString("model", actual))
            || !effort.equals(payload.optString("reasoningEffort", effort)))
            throw new IOException("原会话主模型或思考强度已改变，内容保留在发送队列");
        prepareLocalModel(actual);
    }
    private void finishOutgoing(String owner, String id, JSONObject receipt) {
        try {
            JSONObject head = outgoingQueue.entry(id);
            if (head == null || !owner.equals(head.optString("owner"))) return;
            JSONObject payload = head.getJSONObject("payload");
            boolean answer = payload.optString("operation").equals("answer");
            receipt = AIAssistantSubmission.receipt(receipt, payload.optString("operation"),
                payload.optString(answer ? "questionId" : "turnId"));
            if (answer && receipt.optBoolean("ok")) {
                outgoingQueue.acceptAnswer(id, receipt.getString("question_id"));
                questionEchoes.acknowledge(owner, payload);
            } else if (receipt.optBoolean("ok") && !receipt.optString("user_message_id").isEmpty())
                outgoingQueue.accept(id, receipt.getString("user_message_id"), receipt.optString("turnId"));
            else outgoingQueue.transition(id, receipt.optBoolean("unknown") || receipt.optBoolean("ok")
                ? "unknown" : "failed", null);
        } catch (Exception failure) { error = safeMessage(failure); }
        if (id.equals(outgoingActiveInputs.get(owner))) {
            outgoingActiveOwners.remove(owner); outgoingActiveInputs.remove(owner);
        }
        refresh(true); publishState();
        drainOutgoing(owner);
    }
    private void drainOutgoing(String owner) {
        if (closed || !ready || outgoingQueue == null || outgoingActiveOwners.contains(owner)) return;
        String id = "";
        try {
            JSONObject entry = outgoingQueue.head(owner);
            if (entry == null || !entry.optString("state").equals("queued")) return;
            id = entry.getString("id");
            JSONObject payload = entry.getJSONObject("payload");
            String target = payload.optString("session");
            JSONObject remote = payload.optJSONObject("remote");
            if (remote == null) {
                if (!payload.optString("operation").equals("answer")) requireModelOwner();
                if (target.isEmpty()) {
                    if (outgoingEmptySession.isEmpty()) {
                        JSONObject created = invokeNativeAgent(sessionModelRequest("",
                            payload.optString("model", model), payload.optString("reasoningEffort", reasoningEffort)));
                        outgoingEmptySession = created.getString("id");
                        if (selected.isEmpty()) selected = outgoingEmptySession;
                    }
                    String createdOwner = outgoingOwner(outgoingEmptySession, null);
                    outgoingQueue.retarget(owner, createdOwner, outgoingEmptySession);
                    publishState(); drainOutgoing(createdOwner); return;
                }
                if (!payload.optString("operation").equals("answer")) validateOutgoingModel(payload);
            }
            final String captureId = id;
            outgoingQueue.transition(id, "preparing", payload);
            outgoingActiveOwners.add(owner);
            outgoingActiveInputs.put(owner, id);
            final boolean supportsVisual = AIAssistantMediaPolicy.supportsImages(payload.optString("model"));
            if (!preparations.start(owner, nativeEpoch, () -> nativeEpoch,
                cancel -> prepareOutgoing(payload, supportsVisual, cancel), (prepared, failure) -> {
                    if (failure != null) failOutgoingPreparation(owner, captureId, failure);
                    else dispatchPreparedOutgoing(owner, captureId, prepared);
                })) throw new IOException("原会话已有附件准备任务，内容已保留");
            publishState();
        } catch (Exception failure) { failOutgoingPreparation(owner, id, failure); }
    }
    private JSONObject prepareOutgoing(JSONObject source, boolean supportsVisual,
                                       AIAssistantPreparationJobs.Cancellation cancel) throws Exception {
        JSONObject payload = new JSONObject(source.toString());
        JSONObject remote = payload.optJSONObject("remote");
        if (remote != null && payload.has("preparedAttachments")) return payload;
        JSONArray files = payload.getJSONArray("files"), references = new JSONArray();
        MaiSessionAttachments.Scope scope = new MaiSessionAttachments.Scope();
        cancel.onCancel(scope::cancel);
        for (int i = 0; i < files.length(); i++) {
            cancel.check();
            JSONObject file = files.getJSONObject(i);
            File actual = workspaceFile(file.getString("path"));
            if (actual == null || !actual.isFile()) throw new IOException("附件已丢失，内容保留在发送队列");
            if (remote != null) {
                MaiSessionAttachments.Room room = new MaiSessionAttachments.Room(remote.optString("device_slot"),
                    remote.optString("login_session_id"), payload.getString("session"));
                JSONObject uploaded = new MaiSessionAttachments(context, accountUserId).upload(actual, file.optString("name"), room, scope);
                references.put(new JSONObject().put("attachment_id", uploaded.getString("attachment_id")));
            } else if ((file.optBoolean("image") || file.optString("mimeType").startsWith("video/")) && !supportsVisual)
                throw new IOException("当前主模型不支持图片或视频");
        }
        cancel.check();
        return payload.put("preparedAttachments", references);
    }
    private void failOutgoingPreparation(String owner, String id, Exception failure) {
        error = safeMessage(failure);
        if (!id.isEmpty()) try {
            JSONObject head = outgoingQueue.head(owner);
            if (head != null && id.equals(head.optString("id"))
                && (head.optString("state").equals("queued") || head.optString("state").equals("preparing")))
                outgoingQueue.transition(id, "failed", null);
        } catch (Exception retained) { error = safeMessage(retained); }
        if (id.equals(outgoingActiveInputs.get(owner))) {
            outgoingActiveOwners.remove(owner); outgoingActiveInputs.remove(owner);
        }
        publishState(); drainOutgoing(owner);
    }
    private void dispatchPreparedOutgoing(String owner, String id, JSONObject payload) {
        boolean dispatched = false;
        try {
            if (closed || accountAuth.accountSessionGeneration() != accountToolLoginGeneration)
                throw new IOException("原账号已改变，内容保留在发送队列");
            JSONObject head = outgoingQueue.head(owner);
            if (head == null || !id.equals(head.optString("id")) || !head.optString("state").equals("preparing"))
                throw new IOException("原发送条目已改变，未派发");
            String target = payload.getString("session");
            JSONObject remote = payload.optJSONObject("remote");
            boolean answering = payload.optString("operation").equals("answer");
            JSONObject captured = new JSONObject().put("busy", answering || payload.optString("operation").equals("steer"))
                .put("turnId", payload.optString("turnId"));
            if (answering) captured.put("questions", new JSONArray().put(new JSONObject().put("id", payload.getString("questionId"))));
            AIAssistantSubmission binding = AIAssistantSubmission.capture(target, remote, captured);
            JSONObject latest = outgoingRuntime(target, remote);
            // 远端只在 owner 执行端判定问题仍有效；本机目录摘要不能冒充权威 pending 列表。
            String operation = answering && remote != null ? "answer" : remote != null ? "enqueue" : binding.bind(latest);
            payload.put("operation", operation).put("turnId", remote == null ? binding.turnId() : "");
            JSONObject request = new JSONObject().put("text", payload.getString("text"))
                .put("requestId", id).put("quoteMessageId", payload.optString("quoteMessageId"));
            if (operation.equals("steer")) request.put("turnId", binding.turnId());
            outgoingQueue.transition(id, "dispatching", payload);
            dispatched = true; publishState();
            final String acceptedId = id;
            if (remote == null) {
                if (!answering) validateOutgoingModel(payload);
                request = AIAssistantSubmission.localRequest(payload, operation, binding.turnId(), id);
                JSONObject accepted = invokeNativeAgent(request);
                finishOutgoing(owner, id, accepted);
            } else {
                request.put("action", answering ? "answer" : operation.equals("steer") ? "steer" : "prompt");
                if (answering) request.put("id", payload.getString("questionId"));
                JSONArray references = payload.getJSONArray("preparedAttachments");
                if (references.length() > 0) request.put("attachments", references);
                JSONObject destination = new JSONObject(remote.toString());
                String envelope = sessionSync.sendToRemote(destination, "command", request, (message, failure) -> {
                    worker.post(() -> {
                        if (!failure.isEmpty()) { completeOutgoingReceipt(message, new JSONObject(), true); return; }
                        try {
                            JSONObject entry = outgoingQueue.entry(acceptedId);
                            if (entry != null && entry.optString("state").equals("dispatching"))
                                outgoingQueue.transition(acceptedId, "forwarded", null);
                            if (acceptedId.equals(outgoingActiveInputs.get(owner))) {
                                outgoingActiveOwners.remove(owner); outgoingActiveInputs.remove(owner);
                            }
                            publishState(); drainOutgoing(owner);
                        } catch (Exception retained) { error = safeMessage(retained); publishState(); }
                    });
                });
                outgoingReceipts.put(envelope, new OutgoingReceipt(destination,
                    receipt -> finishOutgoing(owner, acceptedId, receipt)));
                worker.postDelayed(() -> completeOutgoingReceipt(envelope, new JSONObject(), true),
                    payload.getJSONArray("files").length() == 0 ? 30000 : 2L * 60 * 60 * 1000);
            }
        } catch (Exception failure) {
            error = safeMessage(failure);
            if (!id.isEmpty()) try {
                outgoingQueue.transition(id, dispatched && !(failure instanceof NativeAgentFailure)
                    ? "unknown" : "failed", null);
            } catch (Exception retained) { error = safeMessage(retained); }
            if (id.equals(outgoingActiveInputs.get(owner))) {
                outgoingActiveOwners.remove(owner); outgoingActiveInputs.remove(owner);
            }
            publishState(); drainOutgoing(owner);
        }
    }
    private void completeOutgoingReceipt(String id, JSONObject result, boolean unknown) {
        OutgoingReceipt pending = outgoingReceipts.remove(id);
        if (pending == null) return;
        try { if (unknown) result.put("unknown", true); }
        catch (Exception ignored) { }
        pending.completion.accept(result);
    }
    JSONObject restoreOutgoing(String id) {
        try {
            if (outgoingPreferences == null || !outgoingPreferences.getString("recovery", "").isEmpty())
                throw new IOException("已有恢复草稿，请先处理该草稿");
            JSONObject found = null;
            JSONArray entries = outgoingQueue.entries();
            for (int i = 0; i < entries.length(); i++) {
                JSONObject item = entries.getJSONObject(i);
                if (id.equals(item.getString("id"))) found = item;
            }
            String currentOwner = outgoingOwner(state.selected, state.remoteSession);
            boolean sameOwner = found != null && (found.getString("owner").equals(currentOwner)
                || found.getString("owner").equals("local:") && state.remoteSession == null
                    && state.selected.equals(outgoingEmptySession));
            if (found == null || !found.getString("state").equals("failed") || !sameOwner)
                throw new IOException("只能恢复当前会话明确未受理的内容");
            JSONObject payload = found.getJSONObject("payload");
            if (payload.optString("session").isEmpty()) payload.put("session", state.selected);
            payload.put("recoveryId", id);
            // 先把恢复稿另存，再移除失败队首；UI 尚未回填时进程退出也不会丢失。
            if (!outgoingPreferences.edit().putString("recovery", new JSONObject().put("id", id)
                .put("owner", currentOwner).put("payload", payload).toString()).commit())
                throw new IOException("恢复稿保存失败，原内容已保留");
            outgoingQueue.restore(id);
            worker.post(this::publishState);
            return payload;
        } catch (Exception failure) { error = safeMessage(failure); worker.post(this::publishState); return null; }
    }
    void switchModel(String name, Consumer<Boolean> completion) {
        switchModel(state, name, completion);
    }
    void switchModel(State clicked, String name, Consumer<Boolean> completion) {
        if (clicked.remoteSession != null) {
            worker.post(() -> requestRemoteModel(clicked.remoteSession, name, null, completion));
            return;
        }
        if (!beginModelSettingsChange(clicked, true, completion)) return;
        worker.post(() -> {
            boolean success = false;
            publishState();
            try {
                // A model-only choice preserves the stored effort, including its empty inherit
                // value. Sending the displayed fallback here would silently make it explicit.
                applySessionModel(clicked, name, null);
                success = true;
            } catch (Exception failure) { reportModelSettingsFailure(clicked, failure); }
            finally { finishModelSettingsChange(success, completion); }
        });
    }
    void action(String operation, JSONObject values, Runnable completion) {
        actionWithResult(operation, values, completion, null);
    }
    private void actionWithResult(String operation, JSONObject values, Runnable completion,
                                  Consumer<Boolean> settled) {
        State clicked = state;
        String target = values.optString("session", clicked.selected);
        AIAssistantSubmission stopBinding = AIAssistantSubmission.capture(target,
            clicked.remoteSession, clicked.runtime(target));
        JSONObject clickedRemote = stopBinding.remoteOwner();
        worker.post(() -> {
            if (operation.equals("delete") || operation.equals("clear"))
                cancelPreparationsForSession(target, clickedRemote);
            if (operation.equals("stop")) {
                try {
                    if (!stopBinding.canStop(outgoingRuntime(target, clickedRemote)))
                        throw new IOException("原任务已结束或改变，没有停止新的任务");
                } catch (Exception failure) {
                    error = safeMessage(failure); publishState();
                    if (settled != null) main.post(() -> settled.accept(false));
                    return;
                }
                cancelPreparationsForSession(target, clickedRemote);
            }
            if (clickedRemote != null && !operation.equals("create")) {
                if (sessionSync == null || !clickedRemote.optBoolean("online") ||
                    !(operation.equals("stop") || operation.equals("permission") ||
                      operation.equals("answer") || operation.equals("rename"))) {
                    error = "所属设备离线或不支持此操作";
                    publishState();
                    if (settled != null) main.post(() -> settled.accept(false));
                    return;
                }
                JSONObject destination = clickedRemote;
                try {
                    JSONObject request = new JSONObject(values.toString()).put("action", operation);
                    if (operation.equals("stop")) request.put("turnId", stopBinding.turnId());
                    String id = sessionSync.sendToRemote(destination, "command", request,
                        (messageId, failure) -> {
                        worker.post(() -> {
                            if (!sameRemote(selectedRemote, destination)) {
                                if (settled != null) main.post(() -> settled.accept(false));
                                return;
                            }
                            boolean resultArrived = remoteCommandResultsById.containsKey(messageId);
                            boolean accepted = settleRemoteDelivery(messageId, failure);
                            if (failure.isEmpty() && !resultArrived) {
                                error = "";
                                remoteCommandStatus = "已送达所属设备，等待确认执行";
                            }
                            publishState();
                            if (settled != null) main.post(() -> settled.accept(accepted));
                            if (accepted && completion != null) main.post(completion);
                        });
                    });
                    beginRemoteCommand(id, "");
                    publishState();
                } catch (Exception failure) {
                    error = safeMessage(failure);
                    publishState();
                    if (settled != null) main.post(() -> settled.accept(false));
                }
                return;
            }
            if (operation.equals("create")) selectedRemote = null;
            boolean success = false;
            try {
                JSONObject request =
                    new JSONObject(values.toString()).put("op", operation).put("session", target);
                if (operation.equals("stop")) request.put("turnId", stopBinding.turnId());
                JSONObject result = invokeNativeAgent(request);
                if (operation.equals("clear") && selected.equals(target)) localPages.reset();
                if (operation.equals("create"))
                    selected = result.getString("id");
                if (operation.equals("delete") && selected.equals(target))
                    selected = "";
                error = "";
                refresh(true);
                success = true;
                if (completion != null)
                    main.post(completion);
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            }
            if (settled != null) {
                boolean result = success;
                main.post(() -> settled.accept(result));
            }
        });
    }
    String reasoningEffort() { return state.reasoningEffort; }
    boolean isLocalModel() { return state.model.startsWith("local:"); }
    boolean changingModelSource() { return state.remoteSession == null ? changingModelSource
        : state.data.optBoolean("remoteModelPending"); }
    String primaryModelTitle() { return isLocalModel() ? "本地模型" : state.model; }
    // Configuration availability is separate from configured/ready: a failed local restore must
    // still allow the ordinary source menu to recover by selecting a valid cloud configuration.
    boolean modelSettingsReady() { return modelConfigurationAvailable && !closed && !modelSettingsChange.get(); }


    void save(String name, String approval, Consumer<Boolean> completion) {
        save(name, approval, state.reasoningEffort, completion);
    }

    void save(String name, String approval, String effort, Consumer<Boolean> completion) {
        save(state, name, approval, effort, completion);
    }
    void save(State clicked, String name, String approval, String effort, Consumer<Boolean> completion) {
        if (clicked.remoteSession != null) {
            worker.post(() -> requestRemoteModel(clicked.remoteSession, name, effort, completion));
            return;
        }
        if (!beginModelSettingsChange(clicked, false, completion)) return;
        worker.post(() -> {
            boolean success = false;
            try {
                // Policy remains the existing account default operation. Model/effort edits use
                // the shared session mutation and never rebuild the Agent or persist defaults.
                if (!approval.equals(policy)) {
                    applyPrimarySettings(primarySettings.withOptions(primarySettings.model(), approval,
                        primarySettings.effort));
                } else {
                    applySessionModel(clicked, name, effort);
                }
                success = true;
            } catch (Exception failure) {
                reportModelSettingsFailure(clicked, failure);
            } finally {
                finishModelSettingsChange(success, completion);
            }
        });
    }

    private void applySessionModel(State clicked, String name, String effort) throws Exception {
        requireModelOwner();
        if (clicked.remoteSession != null) throw new IOException("请在所属设备修改此会话主模型");
        JSONObject request = sessionModelRequest(clicked.selected, name, effort);
        if (name.startsWith("local:")) prepareLocalModel(name);
        else if (testEndpoint == null) {
            if (cloudKey(name).isEmpty()) refreshAccountModelKeys();
            requireModelOwner();
            if (cloudKey(name).isEmpty()) throw new IOException("账号尚未配置所选供应商模型");
        }
        requireModelOwner();
        JSONObject result = invokeNativeAgent(request);
        boolean stillSelected = ownsSelectedModelPage(clicked);
        if (!clicked.selected.isEmpty())
            AIAssistantSessionModels.validateReceipt(result, clicked.selected, name);
        // A delayed blank-page choice may create its own session, but must not replace a page
        // selected while local assets or credentials were being prepared.
        else if (stillSelected) {
            selected = result.getString("id");
            outgoingEmptySession = AIAssistantSessionModels.createdDraftSession(clicked.selected,
                stillSelected, selected, outgoingEmptySession);
        }
        if (stillSelected) error = "";
        refresh(true);
    }

    private JSONObject sessionModelRequest(String target, String name, String effort) throws Exception {
        if (testEndpoint != null && "test-model".equals(name)) {
            // Preserve the pre-existing explicit Chat fixture. Four-model routing tests use
            // their real IDs and two wire formats instead of relabeling this custom model.
            JSONObject request = new JSONObject().put("op", target.isEmpty() ? "create" : "set_model")
                .put("session", target).put("model", name);
            if (effort != null) request.put("reasoningEffort", effort);
            return request;
        }
        return AIAssistantSessionModels.selection(target, name, effort);
    }

    /** Only this trusted local host resolves paths. Remote commands cannot prepare a model. */
    private void prepareLocalModel(String name) throws Exception {
        if (!name.startsWith("local:")) return;
        if (!name.equals(AIAssistantSessionModels.LOCAL))
            throw new IOException("本地模型身份不受支持");
        if (preparedLocalEpoch == nativeEpoch) return;
        File file = resolvePrimaryModel(primarySettings.selectSource(true));
        requireModelOwner();
        invokeNativeAgent(new JSONObject().put("op", "prepare_local_model").put("model", name)
            .put("localModelPath", file.getCanonicalPath()).put("localProjectorPath", "")
            .put("localContextSize", 32768).put("localMaximumTokens", 0).put("localThreads", 2));
        preparedLocalEpoch = nativeEpoch;
    }

    private boolean beginModelSettingsChange(State clicked, boolean source, Consumer<Boolean> completion) {
        if (!ownsModelConfiguration() || !modelConfigurationAvailable || clicked.remoteSession != null
                || clicked.busy() || !modelSettingsChange.compareAndSet(false, true)) {
            if (completion != null) main.post(() -> completion.accept(false));
            return false;
        }
        changingModelSource = source;
        return true;
    }

    private void applyPrimarySettings(AIAssistantModelSettings proposed) throws Exception {
        requireModelOwner();
        if (selectedRemote != null) throw new IOException("远程会话不能修改本机主模型");
        File localFile = resolvePrimaryModel(proposed);
        String key = "";
        if (!proposed.isLocal()) {
            if (testEndpoint == null && cloudKey(proposed).isEmpty()) refreshAccountModelKeys();
            requireModelOwner();
            key = cloudKey(proposed);
            if (key.isEmpty()) throw new IOException("请先登录已开通模型服务的账号");
        }
        JSONObject next = primaryModelConfiguration(proposed, localFile, key);
        JSONObject previous = committedModelConfiguration;
        // The core's configure operation rejects any running session, then validates before
        // replacing the Agent. Persist only after that gate; preserve the exact previous request
        // in memory so a disk failure rolls back local paths and provider as well as policy.
        AIAssistantModelSettingsStore.apply(new File(root, "settings.json"), proposed,
            this::ownsModelConfiguration, () -> invokeNativeAgent(next), () -> {
                requireModelOwner();
                if (previous != null) invokeNativeAgent(previous);
                else {
                    // A failed initial restore had no valid previous core. Retire the proposed
                    // instance instead of retaining an unpersisted source behind the old label.
                    nativeDestroy(handle);
                    handle = nativeCreate();
                    if (handle == 0 || !nativeSetHostToolHandler(handle, this))
                        throw new IOException("无法恢复未配置的 AI 核心");
                    ready = false;
                }
            });
        requireModelOwner();
        committedModelConfiguration = next;
        bindPrimarySettings(proposed, next);
        ready = true;
        error = "";
        refresh(true);
    }

    private boolean ownsSelectedModelPage(State clicked) {
        return clicked.remoteSession == null ? selectedRemote == null && clicked.selected.equals(selected)
            : sameRemote(selectedRemote, clicked.remoteSession);
    }

    private void reportModelSettingsFailure(State clicked, Exception failure) {
        if (!ownsModelConfiguration()) return;
        if (failure.getSuppressed().length > 0) ready = false;
        if (ownsSelectedModelPage(clicked) || !ready) error = safeMessage(failure);
    }

    private void finishModelSettingsChange(boolean success, Consumer<Boolean> completion) {
        changingModelSource = false;
        modelSettingsChange.set(false);
        if (ownsModelConfiguration()) publishState();
        boolean saved = success && ownsModelConfiguration();
        if (completion != null) main.post(() -> completion.accept(saved));
    }
    private void refreshAccountModelKeys() throws Exception {
        MaiChatAuthClient.AccountModelKeys keys = accountAuth.accountModelKeys();
        if (keys == null) return;
        glmApiKey = keys.glm;
        deepseekApiKey = keys.deepseek;
        miniMaxApiKey = keys.minimax;
    }

    private void refreshSeedanceCredential(boolean force) {
        if (testEndpoint != null) return;
        long now = System.currentTimeMillis() / 1000L;
        if (arkExpiresAt > now + 90 || (!force && now < nextArkRefreshAt)) return;
        nextArkRefreshAt = now + 30;
        if (arkExpiresAt <= now) arkApiKey = "";
        try {
            MaiChatAuthClient.ModelCredential issued = accountAuth.seedanceCredential();
            if (issued == null) return;
            arkApiKey = issued.apiKey;
            arkExpiresAt = issued.expiresAt;
        } catch (Exception failure) {
            Log.w("MaiChatAgent", "Seedance temporary credential renewal failed");
        }
    }
    private void refreshBailianCredential(boolean force) {
        // 手机号会话续领百炼短期 Key；万相和百炼可灵共用，过期后不继续暴露旧值。
        if (testEndpoint != null) return;
        long now = System.currentTimeMillis() / 1000L;
        if (wanExpiresAt > now + 90 || (!force && now < nextWanRefreshAt)) return;
        nextWanRefreshAt = now + 30;
        if (wanExpiresAt <= now) {
            wanApiKey = "";
            wanWorkspaceId = "";
        }
        try {
            MaiChatAuthClient.BailianCredential issued = accountAuth.bailianCredential();
            if (issued == null) return;
            wanWorkspaceId = issued.workspaceId;
            wanApiKey = issued.apiKey;
            wanExpiresAt = issued.expiresAt;
        } catch (Exception failure) {
            Log.w("MaiChatAgent", "Bailian temporary credential renewal failed");
        }
    }
    void importFile(Uri uri, Consumer<ImportedFile> completion) {
        worker.post(() -> {
            ImportedFile imported = null;
            try (InputStream input = context.getContentResolver().openInputStream(uri)) {
                if (input == null)
                    throw new IllegalArgumentException("无法读取文件");
                String mime = context.getContentResolver().getType(uri);
                imported = importStream(input, mime == null ? "application/octet-stream" : mime,
                    uri.getLastPathSegment());
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            }
            ImportedFile result = imported;
            main.post(() -> completion.accept(result));
        });
    }

    void importSessionFile(Uri uri, Consumer<ImportedFile> completion) {
        worker.post(() -> {
            ImportedFile imported = null;
            try (InputStream input = context.getContentResolver().openInputStream(uri)) {
                if (input == null) throw new IOException("无法读取会话附件");
                imported = copySessionFile(input,
                    sourceDisplayName(uri, uri.getLastPathSegment()));
            } catch (Exception failure) {
                error = safeMessage(failure);
                publishState();
            }
            ImportedFile result = imported;
            main.post(() -> completion.accept(result));
        });
    }

    private ImportedFile copySessionFile(InputStream input, String name) throws Exception {
        if (root == null) throw new IOException("AI 工作区尚未准备好");
        MaiCloudDrive.MediaInfo info = MaiSessionAttachments.mediaInfo(name);
        if (info == null) throw new IOException("会话附件文件名或格式无效");
        String relative = "ses-" + UUID.randomUUID().toString().substring(0,8) + "-" + name;
        File target = new File(new File(root,"Workspace"),relative);
        try (FileOutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[64 * 1024];
            long copied = 0;
            int count;
            while ((count = input.read(buffer)) >= 0) {
                copied += count;
                if (copied > info.maxBytes) throw new IOException("会话附件超过大小限制");
                output.write(buffer,0,count);
            }
            if (copied == 0) throw new IOException("会话附件为空");
        } catch (Exception failure) {
            target.delete();
            throw failure;
        }
        return new ImportedFile(relative,info.contentType,
            "image".equals(info.mediaType),name);
    }

    private ImportedFile importDownloadedSessionFile(
        MaiSessionAttachments.Downloaded downloaded) throws Exception {
        try (InputStream input = new java.io.FileInputStream(downloaded.file)) {
            return "image".equals(downloaded.mediaType)
                ? importStream(input,downloaded.contentType,downloaded.name)
                : copySessionFile(input,downloaded.name);
        }
    }

    void downloadRemoteAttachment(JSONObject part,
                                  Consumer<MaiSessionAttachments.Downloaded> completion) {
        worker.post(() -> {
            try {
                JSONObject remote = selectedRemote == null ? null : new JSONObject(selectedRemote.toString());
                String id = part == null ? "" : part.optString("attachment_id");
                if (remote == null || id.isEmpty()) throw new IOException("会话附件已不可用");
                MaiSessionAttachments.Room room = new MaiSessionAttachments.Room(
                    remote.optString("device_slot"),remote.optString("login_session_id"),
                    remote.optString("id"));
                if (!preparations.start("preview:" + outgoingOwner(remote.optString("id"), remote),
                    nativeEpoch, () -> nativeEpoch, cancel -> {
                        MaiSessionAttachments.Downloaded value = new MaiSessionAttachments(context, accountUserId)
                            .download(id, room, cancel);
                        cancel.onCancel(() -> value.file.delete()); cancel.check();
                        return new JSONObject().put("path", value.file.getAbsolutePath()).put("name", value.name)
                            .put("contentType", value.contentType).put("mediaType", value.mediaType);
                    }, (prepared, failure) -> {
                        MaiSessionAttachments.Downloaded value = null;
                        try {
                            if (failure != null) throw failure;
                            if (!sameRemote(remote, selectedRemote)) {
                                new File(prepared.getString("path")).delete();
                                throw new IOException("会话已切换");
                            }
                            value = new MaiSessionAttachments.Downloaded(new File(prepared.getString("path")),
                                prepared.getString("name"), prepared.getString("contentType"), prepared.getString("mediaType"));
                        } catch (Exception failed) { error = safeMessage(failed); publishState(); }
                        MaiSessionAttachments.Downloaded shown = value;
                        main.post(() -> completion.accept(shown));
                    })) throw new IOException("原会话附件正在准备，请稍候");
            } catch (Exception failure) {
                error = safeMessage(failure);
                publishState();
                main.post(() -> completion.accept(null));
            }
        });
    }

    // 相册输入在工作线程导入；图片转换为模型支持的格式，视频保留原件供 Agent 读取。
    void importComposerMedia(Uri uri, Consumer<ImportedFile> completion) {
        worker.post(() -> {
            ImportedFile imported = null;
            try {
                String mime = context.getContentResolver().getType(uri);
                if (mime == null) throw new IllegalArgumentException("无法识别媒体格式");
                mime = mime.toLowerCase(java.util.Locale.ROOT);
                if (mime.startsWith("video/")) {
                    imported = importOriginalMediaForHost(uri);
                } else if (mime.startsWith("image/")) {
                    try (InputStream input = context.getContentResolver().openInputStream(uri)) {
                        if (input == null) throw new IOException("无法读取图片");
                        imported = importStream(input, mime, uri.getLastPathSegment());
                    }
                } else {
                    throw new IllegalArgumentException("只能添加图片或视频");
                }
                imported = new ImportedFile(imported.relativePath, imported.mimeType,
                    imported.image, sourceDisplayName(uri, imported.relativePath));
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            }
            ImportedFile result = imported;
            main.post(() -> completion.accept(result));
        });
    }
    private String sourceDisplayName(Uri uri, String fallback) {
        try (Cursor cursor = context.getContentResolver().query(
                 uri, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                String value = cursor.getString(0);
                if (value != null && !value.trim().isEmpty()) return value.trim();
            }
        } catch (Exception ignored) {
            // 部分媒体 Provider 不提供显示名；使用工作区文件名作为回退。
        }
        return fallback;
    }

    public ImportedFile importPhotoForHost(Uri uri) throws Exception {
        if (root == null) throw new IllegalStateException("AI 工作区尚未准备好");
        if (android.os.Build.VERSION.SDK_INT >= 29) {
            Bitmap bitmap = context.getContentResolver().loadThumbnail(
                uri, new android.util.Size(2048, 2048), null);
            if (bitmap == null) throw new IllegalArgumentException("无法读取照片");
            java.io.ByteArrayOutputStream jpeg = new java.io.ByteArrayOutputStream();
            try {
                if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 88, jpeg))
                    throw new IllegalArgumentException("无法转换照片");
            } finally {
                bitmap.recycle();
            }
            try (InputStream input = new java.io.ByteArrayInputStream(jpeg.toByteArray())) {
                return importStream(input, "image/jpeg", "photo.jpg");
            }
        }
        try (InputStream input = context.getContentResolver().openInputStream(uri)) {
            if (input == null) throw new IllegalArgumentException("无法读取照片");
            return importStream(input, "image/jpeg", "photo.jpg");
        }
    }
    public ImportedFile importOriginalMediaForHost(Uri uri) throws Exception {
        if (root == null) throw new IllegalStateException("AI 工作区尚未准备好");
        String mime = context.getContentResolver().getType(uri);
        if (mime != null) mime = mime.toLowerCase(java.util.Locale.ROOT);
        if (mime == null || !(mime.startsWith("image/") || mime.startsWith("video/")))
            throw new IllegalArgumentException("只能导入系统图库中的图片或视频");
        boolean image = mime.startsWith("image/");
        String extension;
        switch (mime.toLowerCase(java.util.Locale.ROOT)) {
        case "image/jpeg": extension = "jpg"; break;
        case "image/png": extension = "png"; break;
        case "image/webp": extension = "webp"; break;
        case "image/gif": extension = "gif"; break;
        case "image/heic":
        case "image/heif": extension = "heic"; break;
        case "video/mp4": extension = "mp4"; break;
        case "video/quicktime": extension = "mov"; break;
        case "video/x-matroska": extension = "mkv"; break;
        case "video/3gpp": extension = "3gp"; break;
        case "video/webm": extension = "webm"; break;
        default: throw new IllegalArgumentException("不支持的系统媒体格式：" + mime);
        }
        String name = "gallery-" + UUID.randomUUID() + "." + extension;
        File target = new File(new File(root, "Workspace"), name);
        try (InputStream input = context.getContentResolver().openInputStream(uri);
             FileOutputStream output = new FileOutputStream(target)) {
            if (input == null) throw new IOException("无法读取系统媒体");
            byte[] buffer = new byte[8192];
            long copied = 0;
            int count;
            while ((count = input.read(buffer)) >= 0) {
                copied += count;
                if (copied > 100L * 1024 * 1024)
                    throw new IllegalArgumentException(image ? "照片超过 100 MB" : "视频超过 100 MB");
                output.write(buffer, 0, count);
            }
            if (copied == 0) throw new IOException("媒体文件为空");
        } catch (Exception error) {
            target.delete();
            throw error;
        }
        return new ImportedFile(name, mime, image);
    }
    void importCameraFile(File source, Consumer<ImportedFile> completion) {
        worker.post(() -> {
            ImportedFile imported = null;
            try (InputStream input = new java.io.FileInputStream(source)) {
                imported = importStream(input, "image/jpeg", source.getName());
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            } finally {
                if (source != null) source.delete();
            }
            if (imported != null)
                imported = new ImportedFile(imported.relativePath, imported.mimeType, true, "拍摄照片.jpg");
            ImportedFile result = imported;
            main.post(() -> completion.accept(result));
        });
    }
    void prepareCameraFile(Consumer<File> completion) {
        worker.post(() -> {
            File value = null;
            try {
                File directory = new File(context.getCacheDir(), "AIAssistantCaptures");
                if (!directory.mkdirs() && !directory.isDirectory())
                    throw new IllegalStateException("无法创建拍照目录");
                value = File.createTempFile("capture-", ".jpg", directory);
            } catch (Exception e) {
                error = safeMessage(e);
                publishState();
            }
            File result = value;
            main.post(() -> completion.accept(result));
        });
    }
    public File workspaceFile(String relativePath) {
        if (root == null || relativePath == null || relativePath.trim().isEmpty()) return null;
        try { return resolveAppFile(relativePath); }
        catch (IOException | IllegalArgumentException error) { return null; }
    }
    public File workspaceMusicForHost(String relativePath) throws Exception {
        if (root == null) throw new IllegalStateException("AI 工作目录尚未准备好");
        File workspace = new File(root, "Workspace");
        File file = AIAssistantPathPolicy.resolve(workspace, workspace, relativePath);
        if (!file.isFile() || !file.canRead())
            throw new IllegalArgumentException("音乐文件不在 Agent 工作目录或不可读取");
        return file;
    }
    public File workspaceVideoForHost(String path) throws Exception {
        File file = workspaceMediaForHost(path);
        String lower = file.getName().toLowerCase(java.util.Locale.ROOT);
        if (!(lower.endsWith(".mp4") || lower.endsWith(".mov")) ||
            file.length() <= 0 || file.length() > 2_000_000_000L)
            throw new IllegalArgumentException(
                "video must be an accessible MP4 or MOV within the Agent workspace (max 2 GB)");
        return file;
    }
    public File workspaceMediaForHost(String path) throws Exception {
        if (root == null) throw new IllegalStateException("AI workspace is unavailable");
        File workspace = new File(root, "Workspace");
        File file = AIAssistantPathPolicy.resolve(workspace, workspace, path);
        if (!file.isFile() || !file.canRead())
            throw new IllegalArgumentException("media must be a readable file in the Agent workspace");
        return file;
    }
    // 下载原件只创建工作区内的新文件；与读取媒体共用同一个路径授权策略。
    public File workspaceOutputForHost(String path) throws Exception {
        if (root == null) throw new IllegalStateException("AI workspace is unavailable");
        File workspace = new File(root, "Workspace");
        File file = AIAssistantPathPolicy.resolve(workspace, workspace, path);
        if (file.exists()) throw new IllegalArgumentException("download destination already exists");
        File parent = file.getParentFile();
        if (parent == null || (!parent.isDirectory() && !parent.mkdirs()))
            throw new IOException("download directory is unavailable");
        return file;
    }
    void discardComposerMedia(ImportedFile file) {
        if (file == null) return;
        worker.post(() -> {
            String path = file.relativePath;
            if (root == null || path == null || path.contains("/") || path.contains("\\")
                || !(path.startsWith("image-") || path.startsWith("gallery-")
                    || path.startsWith("import-") || path.startsWith("ses-"))) return;
            File workspace = new File(root, "Workspace");
            File candidate = new File(workspace, path);
            try {
                if (candidate.getCanonicalFile().getParentFile().equals(workspace.getCanonicalFile()))
                    Files.deleteIfExists(candidate.toPath());
            } catch (IOException failure) {
                Log.w("MaiChatAgent", "Could not discard unused media", failure);
            }
        });
    }
    private File resolveAppFile(String path) throws IOException {
        if (root == null)
            throw new IllegalArgumentException("需要 App 目录中的文件路径");
        return AIAssistantPathPolicy.resolve(new File(root, "Workspace"),
            context.getFilesDir().getParentFile(), path);
    }
    public File workspaceImageForHost(String relativePath) throws Exception {
        File candidate = resolveAppFile(relativePath);
        if (!candidate.isFile())
            throw new IllegalArgumentException("图片必须位于当前 App 目录内");
        return candidate;
    }
    // 作用：校验工作区图片并执行宿主图片转换。
    public ImportedFile transformImageForHost(JSONObject arguments) throws Exception {
        final File ownerRoot = root;
        if (ownerRoot == null || closed || accountToolLoginGeneration != accountAuth.accountSessionGeneration())
            throw new IOException("The portrait account or workspace changed");
        File source = workspaceImageForHost(arguments.optString("path", ""));
        boolean portrait = arguments.optString("operation").equals("beautify") || arguments.optString("operation").equals("reshape_body");
        String profile = new File(root, "PortraitPreferences").getPath();
        if (portrait) {
            byte[] original = nativePortraitRequest(profile.getBytes(StandardCharsets.UTF_8),
                source.getPath().getBytes(StandardCharsets.UTF_8), null, 1);
            if (original == null) throw new IOException("Cannot resolve portrait original");
            source = workspaceImageForHost(new String(original, StandardCharsets.UTF_8));
            if (arguments.optString("operation").equals("beautify")) {
                final File preferences = new File(ownerRoot, "PortraitPreferences");
                MaiPortraitImage.Result result = MaiPortraitImage.process(source, preferences,
                    new File(ownerRoot, "Workspace"), arguments,
                    () -> !closed && root == ownerRoot
                        && accountToolLoginGeneration == accountAuth.accountSessionGeneration(),
                    (metadata, output) -> portraitEnvelope(nativePortraitRequest(
                        preferences.getPath().getBytes(StandardCharsets.UTF_8), metadata,
                        output.getPath().getBytes(StandardCharsets.UTF_8), 2)));
                String name = result.file.getName();
                synchronized (portraitReceipts) {
                    if (portraitReceipts.size() >= 32) portraitReceipts.remove(portraitReceipts.keySet().iterator().next());
                    portraitReceipts.put(name, result.receipt);
                }
                return new ImportedFile(name, "image/png", true);
            }
            arguments = new JSONObject(arguments.toString());
            arguments.put("_face_hints", arguments.optString("operation").equals("beautify")
                ? com.kongshang.maichat.tools.MobileImageVision.detectFaces(source).getJSONArray("faces") : new JSONArray());
        }
        if (source.length() < 1 || source.length() > 50L * 1024 * 1024)
            throw new IllegalArgumentException("源图片不能超过 50 MB");
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(source.getPath(), bounds);
        if (bounds.outWidth < 1 || bounds.outHeight < 1
                || (long) bounds.outWidth * bounds.outHeight > 12_000_000)
            throw new IllegalArgumentException("图片不能超过 1200 万像素");
        BitmapFactory.Options decode = new BitmapFactory.Options();
        decode.inPreferredConfig = Bitmap.Config.ARGB_8888;
        Bitmap decoded = BitmapFactory.decodeFile(source.getPath(), decode);
        if (decoded == null) throw new IllegalArgumentException("无法解码源图片");
        Bitmap upright = decoded;
        Bitmap edited = null;
        File target = null;
        try {
            Matrix orientation = new Matrix();
            try {
                int exif = new ExifInterface(source.getPath()).getAttributeInt(
                    ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
                switch (exif) {
                case ExifInterface.ORIENTATION_FLIP_HORIZONTAL: orientation.setScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_180: orientation.setRotate(180); break;
                case ExifInterface.ORIENTATION_FLIP_VERTICAL: orientation.setScale(1, -1); break;
                case ExifInterface.ORIENTATION_TRANSPOSE:
                    orientation.setRotate(90); orientation.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_90: orientation.setRotate(90); break;
                case ExifInterface.ORIENTATION_TRANSVERSE:
                    orientation.setRotate(270); orientation.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_270: orientation.setRotate(270); break;
                default: break;
                }
            } catch (IOException ignored) { /* Images without EXIF are already upright. */ }
            if (!orientation.isIdentity())
                upright = Bitmap.createBitmap(decoded, 0, 0, decoded.getWidth(), decoded.getHeight(),
                    orientation, true);
            int[] dimensions = new int[2];
            byte[][] metadata = new byte[1][];
            JSONObject specification = new JSONObject(arguments.toString());
            specification.remove("path");
            byte[] pixels = nativeFilterBitmap(upright, specification.toString(), dimensions, metadata,
                portrait ? profile.getBytes(StandardCharsets.UTF_8) : null,
                portrait ? source.getPath().getBytes(StandardCharsets.UTF_8) : null);
            if (pixels == null || dimensions[0] < 1 || dimensions[1] < 1)
                throw new IllegalStateException("图片处理没有返回结果");
            edited = Bitmap.createBitmap(dimensions[0], dimensions[1], Bitmap.Config.ARGB_8888);
            edited.copyPixelsFromBuffer(ByteBuffer.wrap(pixels));
            String name = "edited-" + UUID.randomUUID() + ".png";
            target = new File(new File(root, "Workspace"), name);
            try (FileOutputStream output = new FileOutputStream(target)) {
                if (!edited.compress(Bitmap.CompressFormat.PNG, 100, output))
                    throw new IOException("无法写入处理结果");
            }
            if (target.length() < 1 || target.length() > 50L * 1024 * 1024)
                throw new IllegalArgumentException("处理结果超过 50 MB");
            if (metadata[0] != null) {
                JSONObject receipt = portraitEnvelope(nativePortraitRequest(profile.getBytes(StandardCharsets.UTF_8),
                    metadata[0], target.getPath().getBytes(StandardCharsets.UTF_8), 2));
                synchronized (portraitReceipts) {
                    if (portraitReceipts.size() >= 32) portraitReceipts.remove(portraitReceipts.keySet().iterator().next());
                    portraitReceipts.put(name, receipt);
                }
            }
            return new ImportedFile(name, "image/png", true);
        } catch (Exception error) {
            if (target != null) target.delete();
            throw error;
        } finally {
            if (edited != null) edited.recycle();
            if (upright != decoded) upright.recycle();
            decoded.recycle();
        }
    }
    private final java.util.Map<String, JSONObject> portraitReceipts = new java.util.LinkedHashMap<>();
    /** 普通调用与模型工具使用同一处理/登记入口，响应仅在输出已原子发布后构造。 */
    public JSONObject transformImageResultForHost(JSONObject arguments) throws Exception {
        ImportedFile file = transformImageForHost(arguments);
        File output = workspaceImageForHost(file.relativePath);
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(output.getPath(), bounds);
        // Completed files are delivered in the conversation; preview remains a user action.
        // Portrait receipts may refine this hint, and ordinary transforms need the same default.
        JSONObject response = new JSONObject().put("path", file.relativePath)
            .put("mime_type", file.mimeType).put("width", bounds.outWidth).put("height", bounds.outHeight)
            .put("next_tool", "agent_send_media");
        JSONObject receipt = portraitReceiptForHost(file.relativePath);
        java.util.Iterator<String> keys = receipt.keys();
        while (keys.hasNext()) { String key = keys.next(); response.put(key, receipt.get(key)); }
        return response;
    }
    public JSONObject portraitReceiptForHost(String path) {
        synchronized (portraitReceipts) {
            JSONObject receipt = portraitReceipts.remove(path);
            return receipt == null ? new JSONObject() : receipt;
        }
    }
    File workspaceDirectoryForPreview() {
        return root == null ? null : context.getFilesDir().getParentFile();
    }
    public File workspacePdfOutputForHost(String absolutePath) throws Exception {
        File candidate = resolveAppFile(absolutePath);
        if (!candidate.getName().toLowerCase(java.util.Locale.ROOT).endsWith(".pdf")
            || candidate.exists())
            throw new IllegalArgumentException("PDF 必须是当前 App 目录内的新文件");
        return candidate;
    }
    public File workspaceDiagnosticOutputForHost() throws Exception {
        if (root == null) throw new IllegalStateException("AI 工作区尚未准备好");
        File workspace = new File(root, "Workspace");
        if (!workspace.isDirectory() && !workspace.mkdirs())
            throw new IOException("无法创建 AI 工作区");
        return new File(workspace, "maichat-diagnostics-"
            + UUID.randomUUID().toString().toLowerCase(java.util.Locale.ROOT) + ".txt");
    }
    // 将选中的外部输入流导入 Agent 工作区，并依据实际媒体类型生成附件元数据。
    // 拷贝失败时清理临时内容，不能把只写入一部分的文件交给消息界面。
    private ImportedFile importStream(InputStream input, String sourceMime, String sourceName)
        throws Exception {
        String originalMime = sourceMime == null ? "application/octet-stream"
            : sourceMime.toLowerCase(java.util.Locale.ROOT);
        boolean image = originalMime.startsWith("image/")
            || AIAssistantMediaPolicy.looksLikeImageName(sourceName);
        boolean unknownType = originalMime.equals("application/octet-stream");
        int maximum = AIAssistantMediaPolicy.maximumBytes(image || unknownType);
        java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        int count;
        while ((count = input.read(buffer)) >= 0) {
            if (bytes.size() + count > maximum)
                throw new IllegalArgumentException(image
                    ? "请选择不超过 20 MB 的图片" : "请选择不超过 5 MB 的文本文件");
            bytes.write(buffer, 0, count);
        }
        byte[] value = bytes.toByteArray();
        String mime = originalMime;
        if (!image && unknownType) {
            BitmapFactory.Options probe = new BitmapFactory.Options();
            probe.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(value, 0, value.length, probe);
            image = probe.outWidth > 0 && probe.outHeight > 0;
        }
        if (image) {
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeByteArray(value, 0, value.length, bounds);
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0)
                throw new IllegalArgumentException("图片格式无法读取");
            if (!AIAssistantMediaPolicy.isSupportedImageMime(mime)) {
                BitmapFactory.Options decode = new BitmapFactory.Options();
                decode.inSampleSize = MessageImageDecodePolicy.sampleSize(
                    bounds.outWidth, bounds.outHeight, 4096, 4096);
                Bitmap bitmap = BitmapFactory.decodeByteArray(value, 0, value.length, decode);
                if (bitmap == null) throw new IllegalArgumentException("图片格式无法转换");
                java.io.ByteArrayOutputStream converted = new java.io.ByteArrayOutputStream();
                try {
                    if (!bitmap.compress(Bitmap.CompressFormat.JPEG, 92, converted))
                        throw new IllegalArgumentException("图片格式无法转换");
                } finally {
                    bitmap.recycle();
                }
                value = converted.toByteArray();
                mime = "image/jpeg";
                if (value.length > AIAssistantMediaPolicy.maximumBytes(true))
                    throw new IllegalArgumentException("转换后的图片超过 20 MB");
            }
        } else {
            if (value.length > AIAssistantMediaPolicy.maximumBytes(false))
                throw new IllegalArgumentException("请选择不超过 5 MB 的文本文件");
            StandardCharsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(value));
            mime = "text/plain";
        }
        String extension = image ? AIAssistantMediaPolicy.extension(mime) : "txt";
        String name = (image ? "image-" : "import-")
            + UUID.randomUUID().toString().substring(0, 8) + "." + extension;
        File target = new File(new File(root, "Workspace"), name);
        try {
            Files.write(target.toPath(), value);
        } catch (IOException failure) {
            target.delete();
            throw failure;
        }
        return new ImportedFile(name, mime, image);
    }
    private void exportSystemCertificates() throws Exception {
        SystemCertificateBundle.writeCurrentPem(new File(root, "trusted-roots.pem"));
    }
    String sessionSyncCertificatePem() throws Exception {
        if (root == null) throw new IOException("设备同步证书尚未准备好");
        byte[] bytes = Files.readAllBytes(new File(root, "trusted-roots.pem").toPath());
        if (bytes.length == 0 || bytes.length > 1024 * 1024)
            throw new IOException("设备同步证书无效");
        return new String(bytes, StandardCharsets.UTF_8);
    }
    void close() {
        if (closed) return;
        registerClosingAccountWorker(accountUserId, thread);
        closed = true;
        registerClosingAttachmentJobs(accountUserId, preparations.close());
        if (sessionSync != null) sessionSync.close();
        listener = null;
        worker.post(() -> {
            worker.removeCallbacks(poll);
            if (handle != 0)
                nativeDestroy(handle);
            handle = 0;
            thread.quitSafely();
        });
    }
}
