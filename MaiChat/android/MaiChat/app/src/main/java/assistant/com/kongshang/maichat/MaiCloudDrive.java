// Android source and lifecycle adapter for the shared C++ private cloud drive.
package com.kongshang.maichat;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.os.SystemClock;
import android.provider.OpenableColumns;
import android.system.Os;
import android.system.OsConstants;
import java.io.File;
import java.io.IOException;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.security.MessageDigest;
import java.util.ArrayDeque;
import java.util.UUID;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Set;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.Consumer;
import org.json.JSONObject;

final class MaiCloudDrive {
    static final int LIST = 1, PLAY_LINK = 2, DELETE = 3, THUMBNAIL_LINK = 5;
    static final int LIST_FOLDERS = 7, CREATE_FOLDER = 8, UPDATE_FOLDER = 9,
        DELETE_FOLDER = 10, MOVE_MEDIA = 11, RENAME_MEDIA = 12, DOWNLOAD_MEDIA = 13,
        SEARCH_MEDIA = 14;
    static final int OK = 0, CANCELED = 6, INSUFFICIENT_SPACE = 11;
    static final long QUOTA_BYTES = 10_000_000_000L;
    static final int GATE_UNKNOWN = 0, GATE_AVAILABLE = 1,
        GATE_CLOSED = 2, GATE_RETRY = 3;
    private static final ExecutorService POOL = Executors.newFixedThreadPool(8,
        runnable -> new Thread(runnable, "maichat-cloud-drive"));

    // Each UI/Agent operation owner remains serialized, so a canceled call
    // finishes before its own checkpoint is reopened. Different owners can
    // still list pending uploads while a large transfer occupies one worker.
    private static final class SerialExecutor implements Executor {
        private final ArrayDeque<Runnable> waiting = new ArrayDeque<>();
        private Runnable running;
        @Override public synchronized void execute(Runnable operation) {
            waiting.offer(() -> {
                try { operation.run(); }
                finally { scheduleNext(); }
            });
            if (running == null) scheduleNext();
        }
        private synchronized void scheduleNext() {
            running = waiting.poll();
            if (running != null) POOL.execute(running);
        }
    }
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static volatile byte[] testPem;
    private static volatile String testBase;
    private static final Object gateLock = new Object();
    private static final Object orphanLock = new Object();
    private static final Set<String> scannedAccounts = new HashSet<>();
    private static String gateAccount = "";
    private static int gateState = GATE_UNKNOWN;
    private static boolean gateBusy;
    private static final ArrayList<Consumer<Integer>> gateWaiting = new ArrayList<>();

    interface Progress { void changed(long completed, long total, boolean preparing); }

    static final class Result {
        final int status;
        final long httpStatus;
        final int curlCode;
        final String body;
        Result(int status, long httpStatus, int curlCode, byte[] body) {
            this.status = status;
            this.httpStatus = httpStatus;
            this.curlCode = curlCode;
            this.body = body == null ? "" : new String(body, StandardCharsets.UTF_8);
        }
        boolean ok() { return status == OK; }
        JSONObject json() {
            try { return new JSONObject(body); }
            catch (Exception ignored) { return new JSONObject(); }
        }
    }

    static final class Source {
        final Uri uri;
        final File file;
        final String name;
        final String type;
        final long bytes;
        final PreparedImage preparedImage;
        Source(Uri uri, File file, String name, String type, long bytes) {
            this(uri, file, name, type, bytes, null);
        }
        private Source(Uri uri, File file, String name, String type, long bytes, PreparedImage preparedImage) {
            this.uri = uri;
            this.file = file;
            this.name = name;
            this.type = type;
            this.bytes = bytes;
            this.preparedImage = preparedImage;
        }
        String id() { return uri == null ? file.getAbsolutePath() : uri.toString(); }
    }

    static final class PreparationFailure extends IOException {
        final Result result;
        PreparationFailure(Result result) { super("Image preparation failed"); this.result = result; }
    }

    private static final class PreparedImage implements AutoCloseable {
        final File checkpoint, snapshot, thumbnail;
        final String account, checkpointKey;
        final MaiCloudDriveThumbnail.Lease lease;
        private boolean claimed, closed;
        PreparedImage(File checkpoint, File snapshot, File thumbnail, String account,
                      MaiCloudDriveThumbnail.Lease lease) throws IOException {
            this.checkpoint = checkpoint; this.snapshot = snapshot; this.thumbnail = thumbnail;
            this.account = account; this.lease = lease; checkpointKey = checkpoint.getCanonicalPath();
        }
        synchronized boolean claim(File requested, String owner) throws IOException {
            if (claimed || closed || !account.equals(owner) || !checkpointKey.equals(requested.getCanonicalPath()))
                return false;
            claimed = true;
            return true;
        }
        @Override public synchronized void close() {
            if (closed) return;
            closed = true;
            if (!checkpoint.exists()) deleteOwnedSnapshot(snapshot);
            lease.close();
        }
    }

    static final class MediaInfo {
        final String contentType;
        final String mediaType;
        final long maxBytes;
        MediaInfo(String contentType, String mediaType, long maxBytes) {
            this.contentType = contentType;
            this.mediaType = mediaType;
            this.maxBytes = maxBytes;
        }
    }

    private static final class NativeLibrary {
        static { System.loadLibrary("maichat_agent"); }
        static void ensureLoaded() { }
    }

    private final Context context;
    private final MaiChatAuthClient auth;
    private final SerialExecutor worker = new SerialExecutor();
    private final Object lock = new Object();
    private long activeCancel;
    private volatile long operation;

    MaiCloudDrive(Context context) {
        this.context = context.getApplicationContext();
        this.auth = MaiChatAuthClient.shared(context);
    }

    static int availability(String account) {
        synchronized (gateLock) {
            return account != null && account.equals(gateAccount) ? gateState : GATE_UNKNOWN;
        }
    }

    static void probe(Context context, Consumer<Integer> completion) {
        MaiCloudDrive client = new MaiCloudDrive(context);
        String account = client.auth.accountId();
        if (account.isEmpty()) { completion.accept(GATE_CLOSED); return; }
        synchronized (gateLock) {
            if (!account.equals(gateAccount)) {
                gateAccount = account;
                gateState = GATE_UNKNOWN;
                gateBusy = false;
                gateWaiting.clear();
            }
            if (gateState == GATE_AVAILABLE || gateState == GATE_CLOSED) {
                completion.accept(gateState);
                return;
            }
            gateWaiting.add(completion);
            if (gateBusy) return;
            gateBusy = true;
        }
        client.request(LIST, new JSONObject(), result -> {
            int state = result.ok() ? GATE_AVAILABLE :
                (result.httpStatus == 404 || result.httpStatus == 401 ?
                    GATE_CLOSED : GATE_RETRY);
            ArrayList<Consumer<Integer>> waiting;
            synchronized (gateLock) {
                if (!account.equals(gateAccount)) return;
                gateState = state;
                gateBusy = false;
                waiting = new ArrayList<>(gateWaiting);
                gateWaiting.clear();
            }
            for (Consumer<Integer> listener : waiting) listener.accept(state);
        });
    }

    static void useLocalFixtureForTest(String base, byte[] pem) {
        if (BuildConfig.DEBUG && BuildConfig.USE_LOCAL_UI_AUTH) {
            testBase = base;
            testPem = pem;
            synchronized (gateLock) {
                gateState = GATE_UNKNOWN;
                gateBusy = false;
                gateWaiting.clear();
            }
        }
    }

    static String errorText(Result result) {
        if (result == null) return "操作失败，请重试";
        if (result.status == CANCELED) return "已暂停，可稍后续传";
        if (result.status == INSUFFICIENT_SPACE) return "设备空间不足，无法暂存这个文件";
        if ("thumbnail_prepare_failed".equals(result.json().optString("error")))
            return "无法生成或恢复图片缩略图，请重新选择可解码的原图";
        if (result.status == 8) return "暂存文件与上传记录不一致，请放弃旧上传后重选";
        if (result.httpStatus == 401) return "请重新登录 MaiChat";
        if (result.httpStatus == 404) return "云盘暂未开放";
        if (result.httpStatus == 410) return "上传期限已过，请重新选择文件";
        if (result.httpStatus == 413) return "文件超过大小限制或云盘剩余容量不足";
        if (result.httpStatus == 429) return "请求太频繁，请稍后重试";
        return "网络或服务暂不可用，请重试";
    }

    static String accountKey(String account) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256")
                .digest(account.getBytes(StandardCharsets.UTF_8));
            StringBuilder hex = new StringBuilder();
            for (int i = 0; i < 12; ++i)
                hex.append(String.format(java.util.Locale.ROOT, "%02x", digest[i] & 0xff));
            return hex.toString();
        } catch (Exception impossible) { return "invalid-account"; }
    }

    File accountDirectory() throws IOException {
        String account = auth.accountId();
        if (account.isEmpty()) throw new IOException("请先登录 MaiChat");
        File directory = new File(context.getFilesDir(), "cloud-drive/" + accountKey(account));
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("无法准备云盘目录");
        synchronized (orphanLock) {
            if (!scannedAccounts.contains(directory.getAbsolutePath())) {
                File[] files = directory.listFiles();
                if (files == null) throw new IOException("无法读取云盘上传记录");
                for (File file : files) {
                    String name = file.getName();
                    String suffix = name.endsWith(".maicloud.json.source") ? ".source" :
                        name.endsWith(".maicloud.json.thumbnail.jpg") ? ".thumbnail.jpg" :
                        name.endsWith(".maicloud.json.thumbnail.meta") ? ".thumbnail.meta" : null;
                    // A process may die after publishing an auxiliary image but
                    // before the native uploader creates its durable checkpoint.
                    int preparing = name.indexOf(".maicloud.json.thumbnail.");
                    if (suffix == null && preparing > 0 && name.contains(".prepare-"))
                        suffix = name.substring(preparing + ".maicloud.json".length());
                    if (suffix == null || name.length() <= ".maicloud.json".length() + suffix.length() ||
                        !Files.isRegularFile(file.toPath(), LinkOption.NOFOLLOW_LINKS)) continue;
                    File checkpoint = new File(directory, name.substring(0, name.length() - suffix.length()));
                    if (!checkpoint.exists()) file.delete();
                }
                // Upload creation also enters through accountDirectory() before staging/thumbnail creation.
                // Later callers in this process must not race the live stage→checkpoint gap.
                scannedAccounts.add(directory.getAbsolutePath());
            }
        }
        return directory;
    }

    Source inspect(Uri uri) throws IOException {
        if (uri == null || !"content".equals(uri.getScheme()))
            throw new IOException("请选择文件管理器中的图片或视频");
        String name = "";
        long bytes = -1;
        try (Cursor cursor = context.getContentResolver().query(uri,
                new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE},
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int nameColumn = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                int sizeColumn = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (nameColumn >= 0 && !cursor.isNull(nameColumn)) name = cursor.getString(nameColumn);
                if (sizeColumn >= 0 && !cursor.isNull(sizeColumn)) bytes = cursor.getLong(sizeColumn);
            }
        } catch (RuntimeException failure) { throw new IOException("无法读取文件信息", failure); }
        if (bytes < 0) {
            try (ParcelFileDescriptor descriptor =
                     context.getContentResolver().openFileDescriptor(uri, "r")) {
                if (descriptor != null) bytes = descriptor.getStatSize();
            }
        }
        return validate(new Source(uri, null, name, null, bytes));
    }

    Source inspect(File file) throws IOException {
        return validate(new Source(null, file, file.getName(), null,
            file.length()));
    }

    private static Source validate(Source source) throws IOException {
        MediaInfo info = mediaInfo(source.name);
        if (info == null)
            throw new IOException("仅支持 MP4、MOV、JPEG、PNG、WebP 或 HEIC 文件");
        if (source.bytes <= 0 || source.bytes > info.maxBytes)
            throw new IOException(source.bytes < 0 ? "文件提供方没有给出文件大小，请从文件管理器重新选择" :
                fileLimitError(info));
        if (source.file != null && !source.file.isFile()) throw new IOException("原文件已不存在");
        return new Source(source.uri, source.file, source.name, info.contentType, source.bytes);
    }

    static MediaInfo mediaInfo(String name) {
        if (name == null) return null;
        try {
            NativeLibrary.ensureLoaded();
            return nativeMediaInfo(name);
        } catch (LinkageError failure) { return null; }
    }

    // The shared classifier owns cloud size policy; error copy must not retain
    // old limits or confuse the account's configured quota with a file ceiling.
    static String fileLimitError(MediaInfo info) {
        long bytes = info.maxBytes;
        String amount = bytes % 1_000_000_000L == 0 ? bytes / 1_000_000_000L + " GB" :
            bytes % 1_000_000L == 0 ? bytes / 1_000_000L + " MB" : bytes + " B";
        return ("image".equals(info.mediaType) ? "单张图片" : "单个视频") + "不能超过 " + amount;
    }

    File newCheckpoint() throws IOException {
        return new File(accountDirectory(), UUID.randomUUID() + ".maicloud.json");
    }

    private boolean matchingStagedSource(File checkpoint, File staged, long bytes) {
        if (!checkpoint.isFile() || !staged.isFile() || staged.length() != bytes) return false;
        Result list = nativeListCheckpoints(checkpoint.getParent());
        if (list == null || !list.ok()) return false;
        org.json.JSONArray uploads = list.json().optJSONArray("uploads");
        if (uploads == null) return false;
        for (int i = 0; i < uploads.length(); ++i) {
            JSONObject item = uploads.optJSONObject(i);
            if (item != null && checkpoint.getAbsolutePath().equals(
                    item.optString("checkpoint_path")) &&
                staged.getAbsolutePath().equals(item.optString("source_id")) &&
                bytes == item.optLong("size_bytes")) return true;
        }
        return false;
    }

    void cancel() {
        synchronized (lock) {
            operation++;
            if (activeCancel != 0) nativeRequestCancel(activeCancel);
        }
    }

    private long begin() {
        synchronized (lock) {
            operation++;
            if (activeCancel != 0) nativeRequestCancel(activeCancel);
            return operation;
        }
    }

    private void finish(long handle) {
        synchronized (lock) {
            if (activeCancel == handle) activeCancel = 0;
        }
        if (handle != 0) nativeDestroyCancel(handle);
    }

    private boolean register(long version, long handle) {
        synchronized (lock) {
            if (operation != version) return false;
            activeCancel = handle;
            return true;
        }
    }

    void listFolders(Consumer<Result> completion) {
        request(LIST_FOLDERS, new JSONObject(), completion);
    }

    void createFolder(String name, String parentId, Consumer<Result> completion) {
        JSONObject payload = new JSONObject();
        try {
            payload.put("name", name);
            if (parentId != null) payload.put("parent_id", parentId);
        } catch (org.json.JSONException impossible) { throw new IllegalArgumentException(impossible); }
        request(CREATE_FOLDER, payload, completion);
    }

    // null keeps the parent unchanged; an empty string moves to the root.
    void updateFolder(String id, String name, String parentId, Consumer<Result> completion) {
        JSONObject payload = new JSONObject();
        try {
            payload.put("folder_id", id);
            if (name != null) payload.put("name", name);
            if (parentId != null) payload.put("parent_id", parentId);
        } catch (org.json.JSONException impossible) { throw new IllegalArgumentException(impossible); }
        request(UPDATE_FOLDER, payload, completion);
    }

    void deleteFolder(String id, Consumer<Result> completion) {
        JSONObject payload = new JSONObject();
        try { payload.put("folder_id", id); }
        catch (org.json.JSONException impossible) { throw new IllegalArgumentException(impossible); }
        request(DELETE_FOLDER, payload, completion);
    }

    void moveMedia(String id, String folderId, Consumer<Result> completion) {
        JSONObject payload = new JSONObject();
        try { payload.put("media_id", id); payload.put("folder_id", folderId); }
        catch (org.json.JSONException impossible) { throw new IllegalArgumentException(impossible); }
        request(MOVE_MEDIA, payload, completion);
    }

    void renameMedia(String id, String name, Consumer<Result> completion) {
        JSONObject payload = new JSONObject();
        try { payload.put("media_id", id); payload.put("name", name); }
        catch (org.json.JSONException impossible) { throw new IllegalArgumentException(impossible); }
        request(RENAME_MEDIA, payload, completion);
    }

    void request(int action, JSONObject payload, Consumer<Result> completion) {
        long version = begin();
        String account = auth.accountId();
        worker.execute(() -> {
            Result result = new Result(2, 0, 0, null);
            long handle = 0;
            try {
                NativeLibrary.ensureLoaded();
                MaiChatAuthClient.CloudDriveAccess access = auth.cloudDriveAccess();
                if (access == null) result = new Result(1, 401, 0, null);
                else {
                    handle = nativeCreateCancel();
                    if (register(version, handle))
                        result = nativeRequest(testBase == null ? access.authBaseUrl : testBase,
                            access.accountToken, trustPem(), action,
                            payload == null ? "{}" : payload.toString(),
                            testBase == null ? null : "localhost", handle);
                }
            } catch (Exception | LinkageError ignored) { }
            finally { finish(handle); }
            Result delivered = result;
            MAIN.post(() -> {
                if (operation == version && account.equals(auth.accountId())) completion.accept(delivered);
            });
        });
    }

    void checkpoints(Consumer<Result> completion) {
        long version = begin();
        String account = auth.accountId();
        worker.execute(() -> {
            Result result;
            try {
                NativeLibrary.ensureLoaded();
                result = nativeListCheckpoints(accountDirectory().getAbsolutePath());
            } catch (Exception | LinkageError failure) { result = new Result(5, 0, 0, null); }
            Result delivered = result;
            MAIN.post(() -> {
                if (operation == version && account.equals(auth.accountId())) completion.accept(delivered);
            });
        });
    }

    void upload(Source source, File checkpoint, Progress progress, Consumer<Result> completion) {
        upload(source, checkpoint, progress, completion, () -> { });
    }

    // Agent calls this ordinary, local-only preparation on its existing tool
    // worker before acknowledging a task. UI uses the same preparation from
    // its serial worker. A successful handoff owns one immutable source/JPEG
    // pair until the ordinary upload settles; no HTTP logic is duplicated.
    Source prepareImageForUploadBlocking(Source source, File checkpoint) throws IOException {
        if (source.type == null || !source.type.startsWith("image/")) return source;
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new IOException("Image preparation requires a worker thread");
        if (!checkpoint.getCanonicalFile().getParentFile().equals(accountDirectory().getCanonicalFile()))
            throw new IOException("Upload checkpoint is outside this account");
        if (MaiCloudDriveThumbnail.legacyCheckpoint(checkpoint)) return source;
        NativeLibrary.ensureLoaded();
        String account = auth.accountId();
        long version = begin(), handle = nativeCreateCancel();
        try {
            if (handle == 0 || !register(version, handle))
                throw new PreparationFailure(new Result(CANCELED, 0, 0, null));
            PreparedImage prepared = prepareImage(source, checkpoint, account, version, handle,
                (done, total) -> operation == version && account.equals(auth.accountId()) ? 0 : 1);
            return new Source(source.uri, source.file, source.name, source.type, source.bytes, prepared);
        } finally { finish(handle); }
    }

    static void discardPrepared(Source source) {
        PreparedImage prepared = source.preparedImage;
        if (prepared == null) return;
        synchronized (prepared) { if (!prepared.claimed) prepared.close(); }
    }

    private static void deleteOwnedSnapshot(File snapshot) {
        if (Files.isRegularFile(snapshot.toPath(), LinkOption.NOFOLLOW_LINKS)) snapshot.delete();
    }

    private PreparedImage prepareImage(Source source, File checkpoint, String account, long version,
                                       long handle, NativeProgress stageBridge) throws IOException {
        MaiCloudDriveThumbnail.Lease lease = MaiCloudDriveThumbnail.borrow(checkpoint);
        File snapshot = new File(checkpoint + ".source");
        try {
            if (Files.isRegularFile(snapshot.toPath(), LinkOption.NOFOLLOW_LINKS) && !checkpoint.exists())
                snapshot.delete();
            if (snapshot.exists()) {
                if (!matchingStagedSource(checkpoint, snapshot, source.bytes))
                    throw new IOException("The retained image source is inconsistent");
            } else {
                try (ParcelFileDescriptor descriptor = source.file == null ?
                        context.getContentResolver().openFileDescriptor(source.uri, "r") :
                        ParcelFileDescriptor.open(source.file, ParcelFileDescriptor.MODE_READ_ONLY)) {
                    if (descriptor == null) throw new IOException("The image is unavailable");
                    Result copied = nativeStageFd(descriptor.getFd(), source.bytes,
                        snapshot.getAbsolutePath(), handle, stageBridge);
                    if (copied == null || !copied.ok())
                        throw new PreparationFailure(copied == null ? new Result(5, 0, 0, null) : copied);
                }
            }
            File thumbnail;
            try {
                thumbnail = MaiCloudDriveThumbnail.prepareFile(checkpoint, snapshot,
                    source.name, () -> operation != version || !account.equals(auth.accountId()));
            } catch (MaiCloudDriveThumbnail.Canceled canceled) {
                throw new PreparationFailure(new Result(CANCELED, 0, 0, null));
            } catch (IOException invalid) {
                throw new PreparationFailure(new Result(1, 0, 0,
                    "{\"error\":\"thumbnail_prepare_failed\"}".getBytes(StandardCharsets.UTF_8)));
            }
            return new PreparedImage(checkpoint, snapshot, thumbnail, account, lease);
        } catch (IOException | RuntimeException | Error failure) {
            if (!checkpoint.exists()) deleteOwnedSnapshot(snapshot);
            lease.close();
            throw failure;
        }
    }

    // Agent task tracking needs a worker-side terminal signal even when
    // cancel() suppresses the public UI callback for an obsolete operation.
    void upload(Source source, File checkpoint, Progress progress,
                Consumer<Result> completion, Runnable settled) {
        long version = begin();
        String account = auth.accountId();
        worker.execute(() -> {
            long handle = 0;
            Result result = new Result(2, 0, 0, null);
            PreparedImage preparedImage = source.preparedImage;
            boolean preparedOwned = false;
            try {
                NativeLibrary.ensureLoaded();
                if (!checkpoint.getCanonicalFile().getParentFile().equals(
                        accountDirectory().getCanonicalFile()))
                    throw new IOException("上传记录目录不属于当前账号");
                MaiChatAuthClient.CloudDriveAccess access = auth.cloudDriveAccess();
                if (access == null) result = new Result(1, 401, 0, null);
                else {
                    handle = nativeCreateCancel();
                    if (register(version, handle)) {
                        final long[] lastProgressAt = {0};
                        NativeProgress bridge = (done, total) -> {
                            long now = SystemClock.uptimeMillis();
                            if (done == total || now - lastProgressAt[0] >= 150) {
                                lastProgressAt[0] = now;
                                MAIN.post(() -> {
                                    if (operation == version && account.equals(auth.accountId()))
                                        progress.changed(done, total, false);
                                });
                            }
                            return operation == version ? 0 : 1;
                        };
                        String base = testBase == null ? access.authBaseUrl : testBase;
                        String suffix = testBase == null ? null : "localhost";
                        byte[] pem = trustPem();
                        boolean image = source.type != null && source.type.startsWith("image/");
                        if (image && (preparedImage != null || !MaiCloudDriveThumbnail.legacyCheckpoint(checkpoint))) {
                            if (preparedImage == null) {
                                NativeProgress stageBridge = (done, total) -> {
                                    long now = SystemClock.uptimeMillis();
                                    if (done == total || now - lastProgressAt[0] >= 150) {
                                        lastProgressAt[0] = now;
                                        MAIN.post(() -> {
                                            if (operation == version && account.equals(auth.accountId()))
                                                progress.changed(done, total, true);
                                        });
                                    }
                                    return operation == version ? 0 : 1;
                                };
                                preparedImage = prepareImage(source, checkpoint, account, version, handle, stageBridge);
                            }
                            if (!preparedImage.claim(checkpoint, account))
                                throw new IOException("Prepared image handoff is no longer owned");
                            preparedOwned = true;
                            result = nativeUploadFile(base, access.accountToken, pem, suffix,
                                preparedImage.snapshot.getAbsolutePath(), checkpoint.getAbsolutePath(),
                                source.name, source.type,
                                preparedImage.thumbnail == null ? null : preparedImage.thumbnail.getAbsolutePath(),
                                handle, bridge);
                        } else if (source.file != null) {
                            File managedStage = new File(checkpoint.getAbsolutePath() + ".source");
                            File canonicalStage = managedStage.getCanonicalFile();
                            boolean discardManagedStage = canonicalStage.getParentFile().equals(
                                accountDirectory().getCanonicalFile()) &&
                                source.file.getCanonicalFile().equals(canonicalStage);
                            result = nativeUploadFile(base, access.accountToken, pem, suffix,
                                source.file.getAbsolutePath(), checkpoint.getAbsolutePath(),
                                source.name, source.type, null, handle, bridge);
                            // Native success has already verified the checkpoint SHA;
                            // its completion removes the checkpoint before returning.
                            // Never retain this app-owned copy after completion, even
                            // if a canonicalized source_id differs in string form.
                            if (discardManagedStage && result != null &&
                                (result.ok() || !checkpoint.exists()))
                                managedStage.delete();
                        } else {
                            File staged = new File(checkpoint.getParentFile(),
                                checkpoint.getName() + ".source");
                            // A crash after staging but before the first upload-session
                            // checkpoint leaves only an app-owned copy. Recreate it from
                            // the currently selected URI; never touch the provider file.
                            if (staged.isFile() && !checkpoint.exists()) staged.delete();
                            if (staged.exists()) {
                                if (matchingStagedSource(checkpoint, staged, source.bytes)) {
                                    result = nativeUploadFile(base, access.accountToken, pem,
                                        suffix, staged.getAbsolutePath(), checkpoint.getAbsolutePath(),
                                        source.name, source.type, null, handle, bridge);
                                    if (result != null && result.ok()) staged.delete();
                                } else result = new Result(8, 0, 0, null);
                                // Never replace a durable source: the C++ core checks its
                                // fingerprint against the checkpoint before resuming.
                            } else {
                                try (ParcelFileDescriptor descriptor =
                                         context.getContentResolver().openFileDescriptor(source.uri, "r")) {
                                    if (descriptor == null) throw new IOException("视频无法读取");
                                    boolean seekable;
                                    try {
                                        Os.lseek(descriptor.getFileDescriptor(), 0,
                                            OsConstants.SEEK_CUR);
                                        seekable = true;
                                    } catch (android.system.ErrnoException notSeekable) {
                                        seekable = false;
                                    }
                                    if (seekable) {
                                        result = nativeUploadFd(base, access.accountToken, pem,
                                            suffix, descriptor.getFd(), source.id(), source.bytes,
                                            checkpoint.getAbsolutePath(), source.name, source.type, null,
                                            handle, bridge);
                                    } else {
                                        Progress staging = (done, total, ignored) ->
                                            progress.changed(done, total, true);
                                        NativeProgress stageBridge = (done, total) -> {
                                            long now = SystemClock.uptimeMillis();
                                            if (done == total || now - lastProgressAt[0] >= 150) {
                                                lastProgressAt[0] = now;
                                                MAIN.post(() -> {
                                                    if (operation == version &&
                                                        account.equals(auth.accountId()))
                                                        staging.changed(done, total, true);
                                                });
                                            }
                                            return operation == version ? 0 : 1;
                                        };
                                        result = nativeStageFd(descriptor.getFd(), source.bytes,
                                            staged.getAbsolutePath(), handle, stageBridge);
                                        if (result != null && result.ok())
                                            result = nativeUploadFile(base, access.accountToken,
                                                pem, suffix, staged.getAbsolutePath(),
                                                checkpoint.getAbsolutePath(), source.name,
                                                source.type, null, handle, bridge);
                                        if (result != null && result.ok()) staged.delete();
                                    }
                                }
                            }
                        }
                    }
                }
            } catch (PreparationFailure failure) { result = failure.result; }
            catch (Exception | LinkageError failure) { result = new Result(5, 0, 0, null); }
            finally {
                finish(handle);
                if (preparedOwned) preparedImage.close();
                else if (source.preparedImage != null) discardPrepared(source);
                settled.run();
            }
            Result delivered = result == null ? new Result(2, 0, 0, null) : result;
            MAIN.post(() -> {
                if (operation == version && account.equals(auth.accountId())) completion.accept(delivered);
            });
        });
    }

    void abort(File checkpoint, Consumer<Result> completion) {
        long version = begin();
        String account = auth.accountId();
        worker.execute(() -> {
            long handle = 0;
            Result result = new Result(2, 0, 0, null);
            try {
                if (!checkpoint.getCanonicalFile().getParentFile().equals(
                        accountDirectory().getCanonicalFile()))
                    throw new IOException("Upload checkpoint is outside this account");
                NativeLibrary.ensureLoaded();
                MaiChatAuthClient.CloudDriveAccess access = auth.cloudDriveAccess();
                if (access == null) result = new Result(1, 401, 0, null);
                else {
                    handle = nativeCreateCancel();
                    if (register(version, handle))
                        result = nativeAbort(testBase == null ? access.authBaseUrl : testBase,
                            access.accountToken, trustPem(),
                            testBase == null ? null : "localhost",
                            checkpoint.getAbsolutePath(), handle);
                    if (result != null && result.ok())
                        MaiCloudDriveThumbnail.cleanupIfNoCheckpoint(checkpoint);
                }
            } catch (Exception | LinkageError ignored) { }
            finally { finish(handle); }
            Result delivered = result;
            MAIN.post(() -> {
                if (operation == version && account.equals(auth.accountId())) completion.accept(delivered);
            });
        });
    }

    byte[] trustPem() throws Exception {
        return testPem == null ? SystemCertificateBundle.currentPem() : testPem;
    }

    MaiCloudVod.Session openVodBlocking(String mediaID, String extension,
                                       MaiCloudVod.Opening opening) throws Exception {
        MaiChatAuthClient.CloudDriveAccess access = auth.cloudDriveAccess();
        if (access == null || auth.accountId().isEmpty())
            throw new IOException("请先登录 MaiChat");
        return MaiCloudVod.openBlocking(
            testBase == null ? access.authBaseUrl : testBase,
            access.accountToken, trustPem(),
            testBase == null ? null : "localhost",
            mediaID, extension, opening);
    }

    private static String proxyForUrl(String url) throws IOException {
        MaiChatAuthTransport.ProxyRoute route =
            MaiChatAuthTransport.selectedProxy(new URL(url));
        if (route.type == MaiChatAuthTransport.ProxyRoute.DIRECT) return "";
        String host = route.host.indexOf(':') >= 0 ? "[" + route.host + "]" : route.host;
        return (route.type == MaiChatAuthTransport.ProxyRoute.HTTP ? "http://" : "socks5h://")
            + host + ":" + route.port;
    }

    private interface NativeProgress { int update(long done, long total); }
    private static native MediaInfo nativeMediaInfo(String name);
    private static native long nativeCreateCancel();
    private static native void nativeRequestCancel(long handle);
    private static native void nativeDestroyCancel(long handle);
    private static native Result nativeRequest(String base, String token, byte[] pem,
        int action, String payload, String hostSuffix, long cancel);
    private static native Result nativeListCheckpoints(String directory);
    private static native Result nativeUploadFile(String base, String token, byte[] pem,
        String hostSuffix, String source, String checkpoint, String name, String type,
        String thumbnail, long cancel, NativeProgress progress);
    private static native Result nativeUploadFd(String base, String token, byte[] pem,
        String hostSuffix, int fd, String stableId, long bytes, String checkpoint,
        String name, String type, String thumbnail, long cancel, NativeProgress progress);
    private static native Result nativeStageFd(int fd, long bytes, String target,
        long cancel, NativeProgress progress);
    private static native Result nativeAbort(String base, String token, byte[] pem,
        String hostSuffix, String checkpoint, long cancel);
}
