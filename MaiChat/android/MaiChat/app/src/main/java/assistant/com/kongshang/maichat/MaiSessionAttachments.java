// Account-scoped Session files use the shared C++ multipart and download cores.
package com.kongshang.maichat;

import android.content.Context;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
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
    private static final MaiChatAttachmentDownload.NativeTransport DOWNLOAD =
        new MaiChatAttachmentDownload.NativeTransport();

    // 与共享云盘 cancel 句柄同生命周期；cancel 可从控制线程调用，释放只在 native 返回后发生。
    static final class Scope {
        private long handle;
        private boolean canceled;
        synchronized long acquire() {
            if (canceled) return 0;
            handle = nativeCreateCancel(); return handle;
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
        this.context = context.getApplicationContext();
        auth = MaiChatAuthClient.shared(this.context);
        this.expectedAccount = expectedAccount;
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
    private static native MaiCloudDrive.Result nativeCleanupRoom(String base, String token,
        byte[] pem, String slot, String login, String session);
}
