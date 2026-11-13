// 文件作用：已授权 Machine 交付的普通媒体准备；只产出 IM 管理目录里的消息和附件，不执行路由或重试。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.BitmapFactory;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import org.json.JSONObject;

final class MaiSessionIMOutgoing {
    final RemoteIMMessage message;
    final String exactBody;
    private final List<File> owned;
    private MaiSessionIMOutgoing(RemoteIMMessage message, String exactBody, List<File> owned) {
        this.message = message; this.exactBody = exactBody; this.owned = owned;
    }

    static MaiSessionIMOutgoing prepare(Context context, JSONObject delivery, BooleanSupplier current) throws IOException {
        String kind = delivery.optString("kind");
        String body = delivery.optString(kind.equals("text") ? "text" : "caption");
        JSONObject scope = delivery.optJSONObject("scope");
        if (scope == null || !current.getAsBoolean()) throw new IOException("交付账号已变化");
        List<File> owned = new ArrayList<>();
        RemoteIMImageAttachment image = null; RemoteIMVideoAttachment video = null; RemoteIMFileAttachment file = null;
        try {
            if (!kind.equals("text")) {
                if (!List.of("image", "video", "file").contains(kind)) throw new IOException("不支持的交付媒体");
                File source = new File(delivery.optString("path"));
                if (!source.isAbsolute() || !source.isFile() || source.length() < 1) throw new IOException("交付媒体尚未准备好");
                RemoteIMMediaPaths paths = RemoteIMMediaPaths.forApp(context);
                String category = kind.equals("image") ? RemoteIMMediaPaths.IMAGES
                    : kind.equals("video") ? RemoteIMMediaPaths.VIDEOS : RemoteIMMediaPaths.FILES;
                String name = source.getName(); int dot = name.lastIndexOf('.');
                String extension = dot < 0 ? ".bin" : name.substring(dot);
                if (!extension.matches("\\.[A-Za-z0-9]{1,12}")) extension = ".bin";
                File target = new File(paths.directory(RemoteIMMediaPaths.OUTGOING, category), "machine-" + UUID.randomUUID() + extension);
                owned.add(target);
                long maximum = kind.equals("image") ? MaiChatAttachmentDownload.MAX_IMAGE_BYTES
                    : kind.equals("video") ? MaiChatAttachmentDownload.MAX_VIDEO_BYTES : MaiChatAttachmentDownload.MAX_FILE_BYTES;
                try (FileInputStream input = new FileInputStream(source); FileOutputStream output = new FileOutputStream(target)) {
                    byte[] bytes = new byte[64 * 1024]; int count; long copied = 0;
                    while ((count = input.read(bytes)) >= 0) {
                        if (!current.getAsBoolean()) throw new IOException("交付准备已取消");
                        copied += count; if (copied > maximum) throw new IOException("交付媒体超过大小限制");
                        output.write(bytes, 0, count);
                    }
                    if (copied == 0) throw new IOException("交付媒体为空");
                }
                if (kind.equals("image")) {
                    BitmapFactory.Options bounds = new BitmapFactory.Options(); bounds.inJustDecodeBounds = true;
                    BitmapFactory.decodeFile(target.getAbsolutePath(), bounds);
                    if (bounds.outWidth <= 0 || bounds.outHeight <= 0) throw new IOException("交付图片无法读取");
                    image = new RemoteIMImageAttachment(target.getAbsolutePath(), bounds.outWidth, bounds.outHeight, target.length());
                } else if (kind.equals("video")) {
                    video = new RemoteIMMediaStore(paths).prepareVideo(target);
                    owned.add(new File(video.coverPath()));
                } else file = new RemoteIMFileAttachment(target.getAbsolutePath(), source.getName(),
                    delivery.optString("mime_type", "application/octet-stream"), target.length());
            }
            // Native ticket 只在当前进程有效，不能成为持久主键；同次派发及回执始终保留这个本地 UUID。
            String id = UUID.randomUUID().toString();
            RemoteIMMessage message = new RemoteIMMessage(id, null, scope.optString("owner_id"), delivery.optString("peer_id"),
                body, RemoteIMMessage.Direction.OUTGOING, RemoteIMMessage.Status.PENDING, System.currentTimeMillis(),
                image, null, file, video, RemoteIMOrigin.MACHINE);
            return new MaiSessionIMOutgoing(message, body, owned);
        } catch (Exception failure) {
            for (File item : owned) item.delete();
            if (failure instanceof IOException) throw (IOException) failure;
            throw new IOException("交付准备失败", failure);
        }
    }

    /** 准备失败同样结束 Native 交接，并向当前本地宿主报告；绝不制造远端任务状态消息。 */
    static MaiSessionIMOutgoing prepareOrReport(Context context, JSONObject delivery, BooleanSupplier current,
            Runnable acknowledge, Consumer<String> localError) {
        try { return prepare(context, delivery, current); }
        catch (Exception failure) {
            acknowledge.run();
            if (current.getAsBoolean()) {
                String reason = failure.getMessage() == null ? "" : failure.getMessage();
                localError.accept(reason.contains("大小") ? "绑定回复附件超过大小限制，未自动重发"
                    : reason.contains("图片") ? "绑定回复图片无法读取，未自动重发" : "绑定回复附件准备失败，未自动重发");
            }
            return null;
        }
    }

    static void prepareAndDispatch(Context context, JSONObject delivery, BooleanSupplier current,
            Runnable acknowledge, Consumer<String> localError, Consumer<MaiSessionIMOutgoing> dispatch) {
        MaiSessionIMOutgoing prepared = prepareOrReport(context, delivery, current, acknowledge, localError);
        if (prepared != null) dispatch.accept(prepared);
    }

    void discard() { for (File item : owned) item.delete(); }
}
