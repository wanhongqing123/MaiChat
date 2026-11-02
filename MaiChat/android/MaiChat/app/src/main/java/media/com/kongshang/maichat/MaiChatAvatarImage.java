// 文件作用：头像选择器与普通/工具上传共用的有界图片解码、EXIF 方向和方形 JPEG 编码。
// 运行边界：在 worker 调用；Bitmap 由取得它的调用方回收，返回的私有缓存文件由宿主删除。
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.RectF;
import android.media.ExifInterface;
import android.net.Uri;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;

public final class MaiChatAvatarImage {
    static final int MAX_SOURCE_BYTES = 32 * 1024 * 1024;
    static final int MAX_UPLOAD_BYTES = 2_000_000;
    private MaiChatAvatarImage() { }

    public static Bitmap decodeSelectedImage(Context context, Uri uri) throws IOException {
        if (context == null || uri == null) throw new IOException("请选择头像图片");
        File source = File.createTempFile("avatar-source-", ".image", context.getCacheDir());
        try {
            try (InputStream input = context.getContentResolver().openInputStream(uri);
                 FileOutputStream output = new FileOutputStream(source)) {
                if (input == null) throw new IOException("无法读取头像图片");
                byte[] buffer = new byte[8192];
                int count, total = 0;
                while ((count = input.read(buffer)) != -1) {
                    if (count > MAX_SOURCE_BYTES - total) throw new IOException("原始头像图片不能超过 32 MB");
                    output.write(buffer, 0, count); total += count;
                }
            }
            return decode(source);
        } finally { source.delete(); }
    }

    static Bitmap decode(File source) throws IOException {
        if (source == null || !source.isFile() || source.length() <= 0 || source.length() > MAX_SOURCE_BYTES)
            throw new IOException("头像图片无效或超过 32 MB");
        BitmapFactory.Options bounds = new BitmapFactory.Options(); bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(source.getAbsolutePath(), bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || bounds.outWidth > 100_000 || bounds.outHeight > 100_000)
            throw new IOException("无法读取头像图片尺寸");
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inSampleSize = 1; options.inPreferredConfig = Bitmap.Config.ARGB_8888;
        // 解码前采样；无论原图像素多少，正常解码输出最长边不超过 1024，避免全尺寸分配。
        while ((Math.max(bounds.outWidth, bounds.outHeight) + options.inSampleSize - 1) / options.inSampleSize > 1024)
            options.inSampleSize *= 2;
        Bitmap decoded = BitmapFactory.decodeFile(source.getAbsolutePath(), options);
        if (decoded == null) throw new IOException("无法解码头像图片");
        try {
            int orientation = ExifInterface.ORIENTATION_NORMAL;
            try { orientation = new ExifInterface(source.getAbsolutePath()).getAttributeInt(
                ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL); }
            catch (IOException ignored) { /* 无 EXIF 的 PNG 等格式按正常方向处理。 */ }
            Matrix transform = new Matrix();
            switch (orientation) {
                case ExifInterface.ORIENTATION_FLIP_HORIZONTAL: transform.setScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_180: transform.setRotate(180); break;
                case ExifInterface.ORIENTATION_FLIP_VERTICAL: transform.setScale(1, -1); break;
                case ExifInterface.ORIENTATION_TRANSPOSE: transform.setRotate(90); transform.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_90: transform.setRotate(90); break;
                case ExifInterface.ORIENTATION_TRANSVERSE: transform.setRotate(270); transform.postScale(-1, 1); break;
                case ExifInterface.ORIENTATION_ROTATE_270: transform.setRotate(270); break;
                default: return decoded;
            }
            Bitmap oriented = Bitmap.createBitmap(decoded, 0, 0, decoded.getWidth(), decoded.getHeight(), transform, true);
            if (oriented != decoded) decoded.recycle();
            return oriented;
        } catch (RuntimeException failure) { decoded.recycle(); throw new IOException("无法处理头像图片方向"); }
    }

    static byte[] encode(File source) throws IOException {
        Bitmap image = decode(source);
        try { return encodeCrop(image, centerSquare(image)); }
        finally { image.recycle(); }
    }

    static RectF centerSquare(Bitmap image) {
        float side = Math.min(image.getWidth(), image.getHeight());
        float left = (image.getWidth() - side) / 2, top = (image.getHeight() - side) / 2;
        return new RectF(left, top, left + side, top + side);
    }

    public static File saveCrop(Context context, Bitmap oriented, RectF crop) throws IOException {
        byte[] encoded = encodeCrop(oriented, crop);
        File result = File.createTempFile("avatar-prepared-", ".jpg", context.getCacheDir());
        boolean complete = false;
        try {
            try (FileOutputStream output = new FileOutputStream(result)) { output.write(encoded); }
            complete = true; return result;
        } finally { if (!complete) result.delete(); }
    }

    private static byte[] encodeCrop(Bitmap image, RectF crop) throws IOException {
        if (image == null || image.isRecycled() || crop == null || !Float.isFinite(crop.left)
            || !Float.isFinite(crop.top) || !Float.isFinite(crop.right) || !Float.isFinite(crop.bottom)
            || crop.width() <= 0 || crop.height() <= 0 || Math.abs(crop.width() - crop.height()) > 1
            || crop.left < 0 || crop.top < 0 || crop.right > image.getWidth() || crop.bottom > image.getHeight())
            throw new IOException("头像裁剪区域无效");
        Bitmap result = Bitmap.createBitmap(512, 512, Bitmap.Config.ARGB_8888);
        try {
            Canvas canvas = new Canvas(result); canvas.drawColor(Color.WHITE);
            Matrix matrix = new Matrix(); matrix.setRectToRect(crop, new RectF(0, 0, 512, 512), Matrix.ScaleToFit.FILL);
            // 直接画到固定画布，不分配极窄长原图按 aspect-fill 放大后的中间 Bitmap。
            canvas.drawBitmap(image, matrix, new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG));
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            if (!result.compress(Bitmap.CompressFormat.JPEG, 88, output)) throw new IOException("无法编码头像图片");
            byte[] bytes = output.toByteArray();
            if (bytes.length == 0 || bytes.length > MAX_UPLOAD_BYTES) throw new IOException("头像图片不能超过 2 MB");
            return bytes;
        } finally { result.recycle(); }
    }
}
