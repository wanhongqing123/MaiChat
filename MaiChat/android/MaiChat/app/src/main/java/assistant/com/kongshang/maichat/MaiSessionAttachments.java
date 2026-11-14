// Account-scoped Session files use the shared C++ multipart and download cores.
package com.kongshang.maichat;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.security.MessageDigest;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.CancellationException;
import org.json.JSONObject;

final class MaiSessionAttachments {
    static final class Room {
        final String slot;
        final String login;
        final String session;
        Room(String slot, String login, String session) {
            this.slot = slot;
            this.login = login;
            this.session = session;
        }
        String key() { return slot + ":" + session; }
    }

    static final class Downloaded {
        final File file;
        final String name;
        final String contentType;
        final String mediaType;
        Downloaded(File file, String name, String contentType, String mediaType) {
            this.file = file;
            this.name = name;
            this.contentType = contentType;
            this.mediaType = mediaType;
        }
    }

    private final Context context;
    private final MaiChatAuthClient auth;
    private final String expectedAccount;
    private final long expectedGeneration;
    private static final MaiChatAttachmentDownload.NativeTransport DOWNLOAD =
        new MaiChatAttachmentDownload.NativeTransport();

    private static final class NativeLibrary {
        static { System.loadLibrary("maichat_agent"); }
        static void ensureLoaded() { }
    }

    private static long createCancel() {
        NativeLibrary.ensureLoaded();
        return nativeCreateCancel();
    }

    // 与共享云盘 cancel 句柄同生命周期；cancel 可从控制线程调用，释放只在 native 返回后发生。
    static final class Scope {
        private long handle;
        private boolean canceled;
        synchronized long acquire() {
            if (canceled) return 0;
            handle = createCancel(); return handle;
        }
        synchronized void cancel() {
            canceled = true;
            if (handle != 0) nativeRequestCancel(handle);
        }
        synchronized void release(long value) {
            if (value != 0 && handle == value) { handle = 0; nativeDestroyCancel(value); }
        }
    }

    MaiSessionAttachments(Context context) {
        this(context, MaiChatAuthClient.shared(context.getApplicationContext()).accountId());
    }

    MaiSessionAttachments(Context context, String expectedAccount) {
        this(context, expectedAccount,
            MaiChatAuthClient.shared(context.getApplicationContext()).accountSessionGeneration());
    }

    // Callers with an existing media lease must pass its original epoch, rather than
    // capturing a replacement login while an older Controller is still unwinding.
    MaiSessionAttachments(Context context, String expectedAccount, long expectedGeneration) {
        this.context = context.getApplicationContext();
        auth = MaiChatAuthClient.shared(this.context);
        this.expectedAccount = expectedAccount;
        this.expectedGeneration = expectedGeneration;
    }

    static MaiCloudDrive.MediaInfo mediaInfo(String name) {
        try {
            System.loadLibrary("maichat_agent");
            return nativeMediaInfo(name);
        } catch (LinkageError failure) { return null; }
    }

    JSONObject upload(File file, String displayName, Room room) throws Exception {
        return upload(file, displayName, room, new Scope());
    }

    JSONObject upload(File file, String displayName, Room room, Scope scope) throws Exception {
        if (file == null || !file.isFile()) throw new IOException("会话附件文件已丢失");
        MaiCloudDrive.MediaInfo info = mediaInfo(displayName);
        if (info == null || file.length() < 1 || file.length() > info.maxBytes)
            throw new IOException("会话附件格式或大小无效");
        String account = auth.accountId();
        if (!account.equals(expectedAccount)) throw new IOException("原附件账号已切换");
        if (account.isEmpty()) throw new IOException("请先登录 MaiChat");
        File directory = new File(context.getFilesDir(),
            "session-attachments/" + MaiCloudDrive.accountKey(account));
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("无法准备会话附件目录");
        File checkpoint = new File(directory,
            sha256(room.key() + ":" + file.getCanonicalPath()) + ".maicloud.json");
        MaiChatAuthClient.CloudDriveAccess access = auth.cloudDriveAccess();
        if (access == null) throw new IOException("会话附件空间不可用");
        if (!account.equals(auth.accountId())) throw new IOException("原附件账号已切换");
        long cancellation = scope.acquire();
        if (cancellation == 0) throw new IOException("会话附件准备已取消");
        MaiCloudDrive.Result result;
        try {
            result = nativeUploadFile(access.authBaseUrl,
                access.accountToken,SystemCertificateBundle.currentPem(),
                file.getAbsolutePath(),checkpoint.getAbsolutePath(),displayName,
                info.contentType,room.slot,room.login,room.session,cancellation);
        } finally { scope.release(cancellation); }
        if (result == null || !result.ok())
            throw new IOException(MaiCloudDrive.errorText(result));
        if (!account.equals(auth.accountId())) throw new IOException("账号已切换");
        JSONObject receipt = result.json();
        if (!room.key().equals(receipt.optString("room_key")) ||
            !displayName.equals(receipt.optString("name")) ||
            !info.contentType.equals(receipt.optString("content_type")) ||
            !info.mediaType.equals(receipt.optString("media_type")) ||
            file.length() != receipt.optLong("size_bytes"))
            throw new IOException("会话附件上传回执不匹配");
        return receipt;
    }

    Downloaded download(String id, Room room) throws Exception {
        return download(id, room, null);
    }

    Downloaded download(String id, Room room, AIAssistantPreparationJobs.Cancellation cancellation) throws Exception {
        if (cancellation != null) cancellation.check();
        String account = auth.accountId();
        if (!account.equals(expectedAccount)) throw new IOException("原附件账号已切换");
        if (account.isEmpty()) throw new IOException("请先登录 MaiChat");
        MaiChatAuthClient.CloudDriveAccess access = auth.cloudDriveAccess();
        if (access == null) throw new IOException("会话附件空间不可用");
        if (!account.equals(auth.accountId())) throw new IOException("原附件账号已切换");
        MaiCloudDrive.Result result = nativeReadLink(access.authBaseUrl,
            access.accountToken,SystemCertificateBundle.currentPem(),id,
            room.slot,room.login,room.session);
        if (result == null || !result.ok())
            throw new IOException(MaiCloudDrive.errorText(result));
        JSONObject link = result.json();
        String name = link.optString("name");
        MaiCloudDrive.MediaInfo info = mediaInfo(name);
        long bytes = link.optLong("size_bytes");
        if (info == null || bytes < 1 || bytes > info.maxBytes ||
            !room.key().equals(link.optString("room_key")) ||
            !info.contentType.equals(link.optString("content_type")))
            throw new IOException("会话附件归属或文件信息无效");
        File directory = new File(context.getCacheDir(), "session-attachments");
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("无法准备附件下载目录");
        String extension = name.substring(name.lastIndexOf('.'));
        File target = new File(directory, UUID.randomUUID() + extension);
        CompletableFuture<MaiChatAttachmentDownload.Result> completed =
            new CompletableFuture<>();
        MaiChatAttachmentDownload.Scope scope = new MaiChatAttachmentDownload.Scope();
        try {
            if (cancellation != null) { cancellation.onCancel(scope::cancel); cancellation.check(); }
            DOWNLOAD.download(link.getString("read_url"),target,bytes,scope,completed::complete);
            MaiChatAttachmentDownload.Result downloaded = completed.get(2,TimeUnit.HOURS);
            if (downloaded == null || !downloaded.ok() || target.length() != bytes)
                throw new IOException("会话附件下载失败");
            if (!account.equals(auth.accountId())) throw new IOException("账号已切换");
            return new Downloaded(target,name,info.contentType,info.mediaType);
        } catch (Exception failure) {
            target.delete();
            throw failure;
        } finally { scope.cancel(); }
    }

    /** 调用方只传自有的新缓存路径；共享核心负责完整临时身份、期限、HTTPS 和原子下载。 */
    JSONObject downloadTemporaryImage(JSONObject request, File target,
            AIAssistantPreparationJobs.Cancellation cancellation) throws Exception {
        if (cancellation != null) cancellation.check();
        if (target == null || !target.isAbsolute() || target.getPath().indexOf(0) >= 0
                || Files.exists(target.toPath(), LinkOption.NOFOLLOW_LINKS)
                || target.getParentFile() == null || !target.getParentFile().isDirectory())
            throw new IOException("图片缓存必须是调用方准备的新路径");
        String account = imageString(request, "account_id");
        requireImageAccount(account);
        Room room = new Room(imageString(request, "owner_slot"),
            imageString(request, "owner_login_session_id"), imageString(request, "session_id"));
        String message = imageString(request, "message_id"), part = imageString(request, "part_id");
        String id = imageString(request, "attachment_id"), name = imageString(request, "name");
        String type = imageString(request, "content_type");
        long bytes = imageInteger(request, "size_bytes"), expiry = imageInteger(request, "expires_at");
        JSONObject payload = new JSONObject().put("attachment_id", id).put("name", name)
            .put("content_type", type).put("media_type", "image").put("size_bytes", bytes)
            .put("expires_at", expiry).put("output_path", target.getAbsolutePath());
        MaiChatAuthClient.CloudDriveAccess access = auth.cloudDriveAccess();
        if (access == null) throw new IOException("会话附件空间不可用");
        // 取凭据可能刷新登录态；房间原发布者 tuple 不能替代当前读者账号。
        requireImageAccount(account);
        if (cancellation != null) cancellation.check();
        Scope scope = new Scope();
        long handle = 0;
        boolean delivered = false;
        try {
            if (cancellation != null) { cancellation.onCancel(scope::cancel); cancellation.check(); }
            handle = scope.acquire();
            if (handle == 0) {
                if (cancellation != null) cancellation.check();
                throw new IOException("无法准备图片下载");
            }
            MaiCloudDrive.Result result = nativeDownloadTemporaryImage(access.authBaseUrl, access.accountToken,
                SystemCertificateBundle.currentPem(), payload.toString(), room.slot, room.login, room.session,
                message, part, handle);
            if (cancellation != null) cancellation.check();
            requireImageAccount(account);
            if (result == null || !result.ok()) {
                if (result != null && result.status == MaiCloudDrive.CANCELED)
                    throw new CancellationException("图片下载已取消");
                throw new IOException(result != null && result.httpStatus == 410
                    ? "图片已过期" : MaiCloudDrive.errorText(result));
            }
            JSONObject receipt = result.json();
            if (receipt.length() != 7 || !Boolean.TRUE.equals(receipt.opt("downloaded"))
                    || !id.equals(receipt.opt("attachment_id"))
                    || !target.getAbsolutePath().equals(receipt.opt("path"))
                    || imageInteger(receipt, "size_bytes") != bytes
                    || imageInteger(receipt, "expires_at") != expiry
                    || !type.equals(receipt.opt("content_type")) || !"image".equals(receipt.opt("media_type"))
                    || !target.isFile() || target.length() != bytes)
                throw new IOException("临时图片下载回执不匹配");
            delivered = true;
            return receipt;
        } catch (LinkageError unavailable) {
            throw new IOException("本地图片下载组件不可用", unavailable);
        } finally {
            // Native 借用 cancel 到同步返回；取消/失败只丢弃本调用独占的新暂存文件。
            scope.release(handle);
            if (!delivered) target.delete();
        }
    }

    private void requireImageAccount(String account) throws IOException {
        // Called before credentials, after a possible token refresh and after Native returns.
        // A same-account login replacement is a new lease; ordinary token refresh is not.
        if (account.isEmpty() || !account.equals(expectedAccount) || !account.equals(auth.accountId())
                || expectedGeneration != auth.accountSessionGeneration())
            throw new IOException("原图片账号已切换");
    }

    private static String imageString(JSONObject value, String key) throws IOException {
        Object raw = value == null ? null : value.opt(key);
        if (!(raw instanceof String) || ((String) raw).isEmpty() || ((String) raw).indexOf(0) >= 0)
            throw new IOException("临时图片身份或元数据无效");
        return (String) raw;
    }

    private static long imageInteger(JSONObject value, String key) throws IOException {
        Object raw = value.opt(key);
        if (!(raw instanceof Integer) && !(raw instanceof Long)) throw new IOException("临时图片数值无效");
        return ((Number) raw).longValue();
    }

    void cleanupRoom(Room room) throws Exception {
        MaiChatAuthClient.CloudDriveAccess access = auth.cloudDriveAccess();
        if (access == null) throw new IOException("会话附件空间不可用");
        MaiCloudDrive.Result result = nativeCleanupRoom(access.authBaseUrl,
            access.accountToken,SystemCertificateBundle.currentPem(),
            room.slot,room.login,room.session);
        if (result == null || !result.ok())
            throw new IOException(MaiCloudDrive.errorText(result));
    }

    private static String sha256(String value) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256")
            .digest(value.getBytes(StandardCharsets.UTF_8));
        StringBuilder hex = new StringBuilder(digest.length * 2);
        for (byte item : digest)
            hex.append(String.format(java.util.Locale.ROOT,"%02x",item & 0xff));
        return hex.toString();
    }

    private static native MaiCloudDrive.MediaInfo nativeMediaInfo(String name);
    private static native MaiCloudDrive.Result nativeUploadFile(String base, String token,
        byte[] pem, String source, String checkpoint, String name, String type,
        String slot, String login, String session, long cancel);
    private static native long nativeCreateCancel();
    private static native void nativeRequestCancel(long handle);
    private static native void nativeDestroyCancel(long handle);
    private static native MaiCloudDrive.Result nativeReadLink(String base, String token,
        byte[] pem, String attachmentId, String slot, String login, String session);
    private static native MaiCloudDrive.Result nativeDownloadTemporaryImage(String base, String token,
        byte[] pem, String payload, String slot, String login, String session,
        String message, String part, long cancel);
    private static native MaiCloudDrive.Result nativeCleanupRoom(String base, String token,
        byte[] pem, String slot, String login, String session);
}
