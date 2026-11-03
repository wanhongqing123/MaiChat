// 文件作用：普通代码与工具共用的 Android 原尺寸人脸 ROI 编辑、原子输出及结果登记。
package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import com.kongshang.maichat.tools.MobileImageVision;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import org.json.JSONArray;
import org.json.JSONObject;

/** Host-owned source/account paths never come from model fields. Work only on a background thread. */
public final class MaiPortraitImage {
    static { System.loadLibrary("maichat_agent"); }
    private static final AtomicBoolean processing = new AtomicBoolean();

    public interface Committer {
        JSONObject commit(byte[] metadata, File output) throws Exception;
    }
    public static final class Result {
        public final File file;
        public final JSONObject receipt;
        Result(File file, JSONObject receipt) { this.file = file; this.receipt = receipt; }
    }

    private static native byte[] nativeValidate(byte[] arguments);
    private static native byte[] nativeMergeFaces(byte[] hints);
    private static native byte[] nativePlan(Bitmap proxy, int width, int height, byte[] hints, int subject);
    // The source Bitmap is the single full-sized allocation. Only this bounded ROI crosses the core API;
    // returned, already-composited pixels replace it in place without a full Java RGBA array.
    private static native byte[] nativeApply(Bitmap source, int x, int y, int width, int height,
        byte[] arguments, byte[] profile, byte[] originalPath, byte[] context);
    private static native byte[] nativePublish(byte[] temporary, byte[] output);

    private MaiPortraitImage() {}

    private static byte[] utf8(String value) { return value.getBytes(StandardCharsets.UTF_8); }
    private static JSONObject parsed(byte[] bytes) throws Exception {
        if (bytes == null) throw new IOException("Portrait processing returned no result");
        return new JSONObject(new String(bytes, StandardCharsets.UTF_8));
    }
    private static JSONObject output(byte[] bytes) throws Exception {
        JSONObject result = parsed(bytes);
        if (!result.optBoolean("ok")) throw new IOException(result.opt("error") == null
            ? "Portrait processing failed" : result.get("error").toString());
        return result.getJSONObject("output");
    }
    private static void current(BooleanSupplier guard) throws IOException {
        if (guard != null && !guard.getAsBoolean())
            throw new IOException("The portrait account or workspace changed; retry in the current account");
    }

    /** Shared NMS/subject numbering for the ordinary detector and actual beautification. */
    public static JSONArray mergeFaces(JSONArray faces) throws Exception {
        return output(nativeMergeFaces(utf8(new JSONObject().put("_face_hints", faces).toString())))
            .getJSONArray("faces");
    }

    // getPixels supplies straight sRGB colors from the software Bitmap. Hash only real pixels,
    // never stride padding or a premultiplied byte buffer reinterpreted as straight RGBA.
    private static String fingerprint(Bitmap image) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        digest.update(utf8(image.getWidth() + "x" + image.getHeight() + ":"));
        int[] pixels = new int[image.getWidth()];
        byte[] rgba = new byte[image.getWidth() * 4];
        for (int y = 0; y < image.getHeight(); ++y) {
            image.getPixels(pixels, 0, pixels.length, 0, y, pixels.length, 1);
            for (int x = 0; x < pixels.length; ++x) {
                int color = pixels[x], offset = x * 4;
                rgba[offset] = (byte) (color >> 16);
                rgba[offset + 1] = (byte) (color >> 8);
                rgba[offset + 2] = (byte) color;
                rgba[offset + 3] = (byte) (color >>> 24);
            }
            digest.update(rgba);
        }
        StringBuilder result = new StringBuilder(64);
        for (byte value : digest.digest()) result.append(String.format(java.util.Locale.ROOT, "%02x", value & 255));
        return result.toString();
    }

    public static Result process(File original, File profile, File workspace, JSONObject arguments,
                                  BooleanSupplier guard, Committer committer) throws Exception {
        // The native inference mutex does not protect Java source allocations. Hold admission
        // through encoding/commit too, so parallel sessions cannot allocate multiple full photos.
        if (!processing.compareAndSet(false, true))
            throw new IOException("Another portrait edit is still running; wait for its result before retrying");
        try {
            return processExclusive(original, profile, workspace, arguments, guard, committer);
        } finally { processing.set(false); }
    }

    private static Result processExclusive(File original, File profile, File workspace, JSONObject arguments,
                                            BooleanSupplier guard, Committer committer) throws Exception {
        JSONObject specification = new JSONObject(arguments.toString());
        specification.remove("path");
        specification.remove("_face_hints");
        specification.remove("workspace_path");
        specification.remove("session_id");
        byte[] encoded = utf8(specification.toString());
        byte[] invalid = nativeValidate(encoded);
        if (invalid != null) throw new IOException(new String(invalid, StandardCharsets.UTF_8));
        current(guard);
        MaiPortraitBitmap.Source source = MaiPortraitBitmap.inspect(original);
        JSONArray hints = MobileImageVision.detectFaces(original).getJSONArray("faces");
        current(guard);
        Bitmap proxy = MaiPortraitBitmap.proxy(source, 256);
        JSONObject plan;
        try {
            plan = output(nativePlan(proxy, source.width, source.height,
                utf8(new JSONObject().put("_face_hints", hints).toString()), specification.optInt("subject_index", -1)));
        } finally { proxy.recycle(); }
        Bitmap image = null;
        File temporary = null, target = null;
        boolean published = false, committed = false;
        try {
            current(guard);
            image = MaiPortraitBitmap.decodeMutable(source);
            if (image.getWidth() != source.width || image.getHeight() != source.height)
                throw new IOException("The portrait source changed while decoding");
            plan.put("source_fingerprint", fingerprint(image));
            JSONObject roi = plan.getJSONObject("roi");
            JSONObject metadata = parsed(nativeApply(image, roi.getInt("x"), roi.getInt("y"),
                roi.getInt("width"), roi.getInt("height"), encoded, utf8(profile.getPath()),
                utf8(original.getPath()), utf8(plan.toString())));
            if (!metadata.has("source_fingerprint")) throw new IOException(metadata.toString());
            current(guard);
            temporary = File.createTempFile(".portrait-", ".png", workspace);
            target = new File(workspace, "edited-" + UUID.randomUUID() + ".png");
            try (FileOutputStream stream = new FileOutputStream(temporary)) {
                if (!image.compress(Bitmap.CompressFormat.PNG, 100, stream))
                    throw new IOException("Cannot encode the portrait output");
                stream.getFD().sync();
            }
            if (temporary.length() < 1 || temporary.length() > 50L * 1024 * 1024)
                throw new IOException("The portrait output exceeds the 50 MB file budget");
            BitmapFactory.Options bounds = new BitmapFactory.Options();
            bounds.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(temporary.getPath(), bounds);
            if (bounds.outWidth != source.width || bounds.outHeight != source.height)
                throw new IOException("The portrait output did not preserve source dimensions");
            current(guard);
            output(nativePublish(utf8(temporary.getPath()), utf8(target.getPath())));
            published = true;
            current(guard);
            JSONObject receipt = committer.commit(utf8(metadata.toString()), target);
            committed = true;
            return new Result(target, receipt);
        } catch (OutOfMemoryError failure) {
            throw new IOException("Portrait processing exceeded the current device memory budget");
        } finally {
            if (image != null) image.recycle();
            if (temporary != null) temporary.delete();
            if (published && !committed && target != null) target.delete();
        }
    }
}
