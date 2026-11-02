// 文件作用：普通界面与宿主工具共用头像归一化、签发、直传和稳定地址校验。
// 运行边界：仅在 worker 调用；账号服务只签发元数据，JPEG 字节直接 PUT 至私有 OSS。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Bitmap;
import android.net.Uri;
import java.io.File;
import java.io.IOException;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.UUID;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;

public final class MaiChatAvatarUploader {
    interface Transport {
        Response request(String url, String method, byte[] body, String bearer,
                         String contentType, BooleanSupplier current) throws Exception;
    }
    static final class Response {
        final int status; final byte[] body;
        Response(int status, byte[] body) { this.status = status; this.body = body; }
    }
    private MaiChatAvatarUploader() { }

    /** 返回私有 512 JPEG 缓存供宿主预览/上传；宿主在成功、取消或失败后删除文件。 */
    public static File prepareSelectedImage(Context context, Uri uri) throws IOException {
        Bitmap image = MaiChatAvatarImage.decodeSelectedImage(context, uri);
        try { return MaiChatAvatarImage.saveCrop(context, image, MaiChatAvatarImage.centerSquare(image)); }
        finally { image.recycle(); }
    }

    /** 同步 worker API；current 在各边界及原生传输中拒绝旧账号继续执行或返回成功。 */
    public static String upload(File image, MaiChatAuthClient.MediaServiceAccess access,
                                BooleanSupplier current) throws IOException {
        return upload(image, access, current, (url, method, body, bearer, contentType, valid) -> {
            MaiChatBinaryHttpTransport.Result response = MaiChatBinaryHttpTransport.perform(
                url, method, body, bearer, contentType, valid);
            return new Response(response.status, response.body);
        });
    }

    static String upload(File image, MaiChatAuthClient.MediaServiceAccess access,
                         BooleanSupplier current, Transport transport) throws IOException {
        checkCurrent(current);
        if (access == null || transport == null || !validMediaBase(access.baseUrl))
            throw new IOException("请先登录 MaiChat 账号");
        byte[] jpeg = MaiChatAvatarImage.encode(image);
        checkCurrent(current);
        try {
            JSONObject request = new JSONObject().put("filename", "avatar-" + UUID.randomUUID() + ".jpg")
                .put("size_bytes", jpeg.length).put("purpose", "im_avatar");
            Response signed = transport.request(access.baseUrl + "/sign-upload", "POST",
                request.toString().getBytes(StandardCharsets.UTF_8), access.accountToken,
                "application/json", current);
            checkCurrent(current);
            if (signed == null || signed.status != 200 || signed.body == null || signed.body.length > 8192)
                throw new IOException("头像上传地址领取失败，请稍后重试");
            JSONObject metadata = new JSONObject(new String(signed.body, StandardCharsets.UTF_8));
            String upload = metadata.optString("upload_url", ""), avatar = metadata.optString("avatar_url", "");
            JSONObject headers = metadata.optJSONObject("upload_headers");
            if (!validSignedUpload(upload, avatar) || headers == null || headers.length() != 1
                || !"image/jpeg".equals(headers.optString("Content-Type", "")))
                throw new IOException("头像上传地址无效");
            checkCurrent(current);
            // OSS 请求没有账号 Bearer，也不跟随重定向或把图片字节送回账号服务器。
            Response uploaded = transport.request(upload, "PUT", jpeg, "", "image/jpeg", current);
            checkCurrent(current);
            if (uploaded == null || uploaded.status < 200 || uploaded.status >= 300)
                throw new IOException("头像上传失败，请稍后重试");
            return avatar;
        } catch (IOException failure) { throw failure; }
        catch (Exception failure) { throw new IOException("头像上传失败，请稍后重试"); }
    }

    private static void checkCurrent(BooleanSupplier current) throws IOException {
        if (current == null || !current.getAsBoolean()) throw new IOException("登录账号已变化，请重新选择头像");
    }

    private static boolean validMediaBase(String value) {
        try {
            URI uri = new URI(value);
            return secure(uri) && "ichat.life".equalsIgnoreCase(uri.getHost())
                && "/maichat".equals(uri.getRawPath()) && uri.getRawQuery() == null;
        } catch (Exception failure) { return false; }
    }

    private static boolean validSignedUpload(String upload, String avatar) {
        try {
            if (upload.length() > 8192 || avatar.length() > 2048) return false;
            URI put = new URI(upload), stable = new URI(avatar);
            String path = stable.getRawPath();
            return secure(put) && put.getHost().toLowerCase(java.util.Locale.ROOT).endsWith(".aliyuncs.com")
                && put.getRawQuery() != null && secure(stable)
                && "ichat.life".equalsIgnoreCase(stable.getHost()) && stable.getRawQuery() == null
                && path != null && path.startsWith("/maichat/avatars/")
                && path.matches("/maichat/avatars/[A-Za-z0-9_/-]+\\.jpg")
                && !path.contains("//") && !path.contains("..")
                && put.getRawPath().equals(path.substring("/maichat".length()));
        } catch (Exception failure) { return false; }
    }

    private static boolean secure(URI uri) {
        return "https".equals(uri.getScheme()) && uri.getHost() != null
            && (uri.getPort() == -1 || uri.getPort() == 443) && uri.getRawUserInfo() == null
            && uri.getRawFragment() == null && uri.toString().equals(uri.toASCIIString());
    }
}
