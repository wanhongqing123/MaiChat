// 文件作用：Android 普通头像服务的图片准备与共享 C++ 上传适配；不依赖模型工具协议。
package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Rect;
import android.media.ExifInterface;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.LinkOption;

final class MaiAccountAvatar {
    private static final int MAX_INPUT_BYTES = 20 * 1024 * 1024;
    private static final int AVATAR_SIDE = 512;

    private MaiAccountAvatar() { }

    private static final class NativeLibrary {
        static { System.loadLibrary("maichat_agent"); }
        static void load() { }
    }

    /** 句柄直到同步上传返回后才销毁；切号线程只发取消，不释放正在使用的原生对象。 */
    static final class Scope implements AutoCloseable {
        private long handle;
        private boolean canceled;

        Scope() throws IOException {
            NativeLibrary.load();
            handle = nativeCreateCancel();
            if (handle == 0) throw new IOException("无法创建头像上传任务");
        }

        synchronized void cancel() {
            canceled = true;
            if (handle != 0) nativeRequestCancel(handle);
        }

        synchronized long handle() throws IOException {
            if (canceled || handle == 0) throw new IOException("头像上传已取消");
            return handle;
        }

        @Override public synchronized void close() {
            if (handle != 0) nativeDestroyCancel(handle);
            handle = 0;
        }
    }

    static String upload(MaiChatAuthClient.MediaServiceAccess access, String accountId,
                         byte[] jpeg, Scope scope) throws Exception {
        byte[] result = nativeUpload(access.baseUrl.getBytes(StandardCharsets.UTF_8),
            access.accountToken.getBytes(StandardCharsets.UTF_8),
            accountId.getBytes(StandardCharsets.UTF_8), SystemCertificateBundle.currentPem(),
            jpeg, scope.handle());
        if (result == null || result.length == 0) throw new IOException("头像上传尚未确认");
        return new String(result, StandardCharsets.UTF_8);
    }

    /** 先限制压缩输入，再按采样解码；极大尺寸图片不会先按原始像素分配内存。 */
    static byte[] prepare(File source) throws IOException {
        if (source == null || !source.isAbsolute() || !source.isFile() ||
                source.length() < 1 || source.length() > MAX_INPUT_BYTES)
            throw new IOException("头像必须是工作区内不超过 20 MB 的图片");
        ByteArrayOutputStream input = new ByteArrayOutputStream();
        try (InputStream stream = Files.newInputStream(source.toPath(), LinkOption.NOFOLLOW_LINKS)) {
            byte[] buffer = new byte[32 * 1024];
            int count;
            while ((count = stream.read(buffer)) != -1) {
                if (input.size() + count > MAX_INPUT_BYTES)
                    throw new IOException("头像图片超过 20 MB");
                input.write(buffer, 0, count);
            }
        }
        byte[] bytes = input.toByteArray();
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(bytes, 0, bytes.length, bounds);
        if (bounds.outWidth < 1 || bounds.outHeight < 1)
            throw new IOException("头像图片无法解码");
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inPreferredConfig = Bitmap.Config.ARGB_8888;
        options.inSampleSize = 1;
        int longest = Math.max(bounds.outWidth, bounds.outHeight);
        while (longest / options.inSampleSize > AVATAR_SIDE * 2)
            options.inSampleSize *= 2;
        Bitmap decoded = BitmapFactory.decodeByteArray(bytes, 0, bytes.length, options);
        if (decoded == null) throw new IOException("头像图片无法解码");
        Bitmap oriented = decoded;
        Bitmap output = null;
        try {
            Matrix orientation = exifOrientation(bytes);
            if (!orientation.isIdentity()) oriented = Bitmap.createBitmap(decoded, 0, 0,
                decoded.getWidth(), decoded.getHeight(), orientation, true);
            output = Bitmap.createBitmap(AVATAR_SIDE, AVATAR_SIDE, Bitmap.Config.ARGB_8888);
            Canvas canvas = new Canvas(output);
            canvas.drawColor(Color.WHITE);
            int side = Math.min(oriented.getWidth(), oriented.getHeight());
            int left = (oriented.getWidth() - side) / 2;
            int top = (oriented.getHeight() - side) / 2;
            canvas.drawBitmap(oriented, new Rect(left, top, left + side, top + side),
                new Rect(0, 0, AVATAR_SIDE, AVATAR_SIDE), new Paint(Paint.FILTER_BITMAP_FLAG));
            ByteArrayOutputStream jpeg = new ByteArrayOutputStream();
            if (!output.compress(Bitmap.CompressFormat.JPEG, 90, jpeg) ||
                    jpeg.size() < 1 || jpeg.size() > 2_000_000)
                throw new IOException("头像图片处理失败");
            return jpeg.toByteArray();
        } finally {
            if (output != null) output.recycle();
            if (oriented != decoded) oriented.recycle();
            decoded.recycle();
        }
    }

    private static Matrix exifOrientation(byte[] bytes) {
        Matrix matrix = new Matrix();
        try {
            int orientation = new ExifInterface(new ByteArrayInputStream(bytes)).getAttributeInt(
                ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
            switch (orientation) {
                case ExifInterface.ORIENTATION_FLIP_HORIZONTAL: matrix.setScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_180: matrix.setRotate(180); break;
                case ExifInterface.ORIENTATION_FLIP_VERTICAL: matrix.setScale(1, -1); break;
                case ExifInterface.ORIENTATION_TRANSPOSE:
                    matrix.setRotate(90); matrix.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_90: matrix.setRotate(90); break;
                case ExifInterface.ORIENTATION_TRANSVERSE:
                    matrix.setRotate(-90); matrix.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_270: matrix.setRotate(-90); break;
                default: break;
            }
        } catch (IOException ignored) {
            // 没有 EXIF 的 PNG/WebP 仍是有效图片；像素解码已单独验证。
        }
        return matrix;
    }

    @SuppressWarnings("unused") // 同步 JNI 上传按每个 URL 复用系统代理选择。
    private static String proxyForUrl(String url) throws IOException {
        MaiChatAuthTransport.ProxyRoute route =
            MaiChatAuthTransport.selectedProxy(new URL(url));
        if (route.type == MaiChatAuthTransport.ProxyRoute.DIRECT) return "";
        String host = route.host.indexOf(':') >= 0 ? "[" + route.host + "]" : route.host;
        return (route.type == MaiChatAuthTransport.ProxyRoute.HTTP ? "http://" : "socks5h://")
            + host + ":" + route.port;
    }

    private static native long nativeCreateCancel();
    private static native void nativeRequestCancel(long handle);
    private static native void nativeDestroyCancel(long handle);
    private static native byte[] nativeUpload(byte[] base, byte[] token, byte[] accountId,
                                             byte[] pem, byte[] jpeg, long cancel);
}
