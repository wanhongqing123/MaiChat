// 文件作用：普通代码与工具共用的 Android 原尺寸皮肤分块读写、原子输出及结果登记。
package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapRegionDecoder;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import com.kongshang.maichat.tools.MobileImageVision;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.DeflaterOutputStream;
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
    // The opaque context, parameters, subject ownership and tile order belong to the shared C++ ABI.
    // Java reads only its requested rectangles and copies each already-composited write interior once.
    private static native byte[] nativeAnalyze(Bitmap proxy, Bitmap global, int width, int height,
        byte[] hints, byte[] arguments, byte[] profile, byte[] originalPath, byte[] fingerprint, long[] context);
    private static native byte[] nativeRefine(long context, Bitmap proxy);
    private static native byte[] nativeProtect(long context, Bitmap face);
    private static native byte[] nativeTile(long context, int index, Bitmap sample, Bitmap destination,
        byte[] mask, int sampleX, int sampleY, int writeX, int writeY, int writeWidth, int writeHeight);
    private static native byte[] nativeFinish(long context);
    private static native void nativeFree(long context);
    // Geometry uses the core-provided parameters/face plan after skin processing, without a second color pass.
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
        if (!result.has("ok")) throw new IOException(result.toString());
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
    private static String fingerprint(Bitmap image, BooleanSupplier guard) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        digest.update(utf8(image.getWidth() + "x" + image.getHeight() + ":"));
        int[] pixels = new int[image.getWidth()];
        byte[] rgba = new byte[image.getWidth() * 4];
        for (int y = 0; y < image.getHeight(); ++y) {
            current(guard);
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
        long sourceBytes = original.length(), sourceModified = original.lastModified();
        MaiPortraitBitmap.Source source = MaiPortraitBitmap.inspect(original);
        JSONArray hints = MobileImageVision.detectFaces(original).getJSONArray("faces");
        current(guard);
        Bitmap image = null;
        BitmapRegionDecoder decoder = null;
        long[] context = new long[1];
        File temporary = null, target = null, maskTemporary = null, maskTarget = null;
        boolean imagePublished = false, maskPublished = false, committed = false;
        try {
            current(guard);
            image = MaiPortraitBitmap.decodeMutable(source);
            if (image.getWidth() != source.width || image.getHeight() != source.height)
                throw new IOException("The portrait source changed while decoding");
            String fingerprint = fingerprint(image, guard);
            Bitmap proxy = null, global = null;
            JSONObject plan;
            try {
                proxy = MaiPortraitBitmap.proxy(source, 512);
                global = scaledRegion(image, new Rect(0, 0, source.width, source.height), 64, 64);
                plan = output(nativeAnalyze(proxy, global, source.width, source.height,
                    utf8(new JSONObject().put("_face_hints", hints).toString()), encoded,
                    utf8(profile.getPath()), utf8(original.getPath()), utf8(fingerprint), context));
            } finally {
                if (proxy != null) proxy.recycle();
                if (global != null) global.recycle();
            }
            if (context[0] == 0) throw new IOException("Portrait service returned no skin context");
            current(guard);
            JSONObject refinement = plan.optJSONObject("skin_refinement_request");
            if (refinement != null) {
                Rect region = rectangle(refinement.getJSONArray("source_rect"), source, 20_000);
                int width = refinement.getInt("proxy_width"), height = refinement.getInt("proxy_height");
                // Refined body material is shared by all tiles; initial analysis stays 512.
                if (width < 1 || height < 1 || width > 1024 || height > 1024
                        || width > region.width() || height > region.height())
                    throw new IOException("Portrait service requested an invalid refinement image");
                Bitmap local = scaledRegion(image, region, width, height);
                try { output(nativeRefine(context[0], local)); }
                finally { local.recycle(); }
                current(guard);
            }
            decoder = BitmapRegionDecoder.newInstance(original.getPath(), false);
            JSONObject face = plan.optJSONObject("face_roi");
            Bitmap facePixels = face == null ? null : MaiPortraitBitmap.region(source, decoder,
                rectangle(face, source, 2048));
            try { output(nativeProtect(context[0], facePixels)); }
            finally { if (facePixels != null) facePixels.recycle(); }
            // The only full-size color allocation is the mutable destination. Samples always
            // come from the original file, so an earlier write never contaminates a later halo.
            byte[] mask = new byte[Math.multiplyExact(source.width, source.height)];
            JSONArray tiles = plan.getJSONArray("tiles");
            for (int index = 0; index < tiles.length(); ++index) {
                current(guard);
                JSONObject tile = tiles.getJSONObject(index);
                Rect sample = rectangle(tile.getJSONArray("sample_rect"), source, 1040);
                Rect write = rectangle(tile.getJSONArray("write_rect"), source, 1024);
                Bitmap pixels = MaiPortraitBitmap.region(source, decoder, sample);
                try {
                    output(nativeTile(context[0], index, pixels, image, mask, sample.left, sample.top,
                        write.left, write.top, write.width(), write.height()));
                } finally { pixels.recycle(); }
            }
            current(guard);
            JSONObject effective = plan.getJSONObject("effective_parameters");
            boolean geometry = effective.getDouble("slim_face") > 0 || effective.getDouble("enlarge_eyes") > 0;
            Rect geometryRegion = null;
            if (geometry) {
                JSONObject facePlan = plan.getJSONObject("face_plan");
                geometryRegion = rectangle(facePlan.getJSONObject("roi"), source, 2048);
                JSONObject applied = parsed(nativeApply(image, geometryRegion.left, geometryRegion.top,
                    geometryRegion.width(), geometryRegion.height(),
                    utf8(plan.getJSONObject("geometry_parameters").toString()), utf8(profile.getPath()),
                    utf8(original.getPath()), utf8(facePlan.toString())));
                if (!applied.has("source_fingerprint")) throw new IOException(applied.toString());
            }
            JSONObject metadata = output(nativeFinish(context[0]));
            metadata.put("geometry_applied", geometry);
            if (geometryRegion != null) {
                metadata.put("geometry_processing_bbox", new JSONArray().put(geometryRegion.left)
                    .put(geometryRegion.top).put(geometryRegion.width()).put(geometryRegion.height()));
                metadata.put("metrics_scope", "skin_color_and_smoothing_before_optional_face_geometry");
            }
            current(guard);
            temporary = File.createTempFile(".portrait-", ".png", workspace);
            maskTemporary = File.createTempFile(".portrait-mask-", ".png", workspace);
            String id = UUID.randomUUID().toString();
            target = new File(workspace, "edited-" + id + ".png");
            maskTarget = new File(workspace, "skin-mask-" + id + ".png");
            try (FileOutputStream stream = new FileOutputStream(temporary)) {
                if (!image.compress(Bitmap.CompressFormat.PNG, 100, stream))
                    throw new IOException("Cannot encode the portrait output");
                stream.getFD().sync();
            }
            writeMask(maskTemporary, mask, source.width, source.height, guard);
            for (File complete : new File[] {temporary, maskTemporary}) {
                if (complete.length() < 1 || complete.length() > 50L * 1024 * 1024)
                    throw new IOException("The portrait output exceeds the 50 MB file budget");
                BitmapFactory.Options bounds = new BitmapFactory.Options();
                bounds.inJustDecodeBounds = true;
                BitmapFactory.decodeFile(complete.getPath(), bounds);
                if (bounds.outWidth != source.width || bounds.outHeight != source.height)
                    throw new IOException("The portrait output did not preserve source dimensions");
            }
            if (original.length() != sourceBytes || original.lastModified() != sourceModified)
                throw new IOException("The portrait source changed while processing");
            current(guard);
            output(nativePublish(utf8(maskTemporary.getPath()), utf8(maskTarget.getPath())));
            maskPublished = true;
            current(guard);
            output(nativePublish(utf8(temporary.getPath()), utf8(target.getPath())));
            imagePublished = true;
            current(guard);
            JSONObject receipt = committer.commit(utf8(metadata.toString()), target);
            committed = true;
            receipt.put("mask_path", maskTarget.getName()).put("mask_width", source.width)
                .put("mask_height", source.height).put("mask_coordinate_origin", "top_left");
            return new Result(target, receipt);
        } catch (OutOfMemoryError failure) {
            throw new IOException("Portrait processing exceeded the current device memory budget");
        } finally {
            if (context[0] != 0) nativeFree(context[0]);
            if (decoder != null) decoder.recycle();
            if (image != null) image.recycle();
            if (temporary != null) temporary.delete();
            if (maskTemporary != null) maskTemporary.delete();
            if (!committed && imagePublished && target != null) target.delete();
            if (!committed && maskPublished && maskTarget != null) maskTarget.delete();
        }
    }

    private static Rect rectangle(JSONObject value, MaiPortraitBitmap.Source source, int maximum) throws Exception {
        return rectangle(new JSONArray().put(value.getInt("x")).put(value.getInt("y"))
            .put(value.getInt("width")).put(value.getInt("height")), source, maximum);
    }
    private static Rect rectangle(JSONArray value, MaiPortraitBitmap.Source source, int maximum) throws Exception {
        if (value.length() != 4) throw new IOException("Portrait service returned invalid bounds");
        int x = value.getInt(0), y = value.getInt(1), width = value.getInt(2), height = value.getInt(3);
        if (x < 0 || y < 0 || width < 1 || height < 1 || width > maximum || height > maximum
                || x > source.width - width || y > source.height - height)
            throw new IOException("Portrait service returned invalid bounds");
        return new Rect(x, y, x + width, y + height);
    }
    private static Bitmap scaledRegion(Bitmap source, Rect region, int width, int height) {
        Bitmap result = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888, true, source.getColorSpace());
        result.setDensity(Bitmap.DENSITY_NONE);
        Canvas canvas = new Canvas(result);
        try { canvas.drawBitmap(source, region, new Rect(0, 0, width, height), new Paint(Paint.FILTER_BITMAP_FLAG)); }
        catch (RuntimeException | OutOfMemoryError failure) { result.recycle(); throw failure; }
        finally { canvas.setBitmap(null); }
        return result;
    }

    // Android's ALPHA_8 Bitmap encoder is not a grayscale PNG contract. Emit a standard
    // 8-bit grayscale PNG directly from the diagnostic L8 bytes with bounded compressed chunks.
    // Color output has already been composited by the core.
    static void writeMask(File target, byte[] mask, int width, int height, BooleanSupplier guard) throws IOException {
        if (width < 1 || height < 1 || (long) width * height != mask.length)
            throw new IOException("Portrait mask dimensions are invalid");
        try (FileOutputStream file = new FileOutputStream(target); DataOutputStream png = new DataOutputStream(file)) {
            png.write(new byte[] {(byte) 137, 80, 78, 71, 13, 10, 26, 10});
            byte[] header = ByteBuffer.allocate(13).putInt(width).putInt(height)
                .put((byte) 8).put((byte) 0).put((byte) 0).put((byte) 0).put((byte) 0).array();
            pngChunk(png, "IHDR", header, header.length);
            MaskChunks chunks = new MaskChunks(png);
            Deflater compressor = new Deflater();
            try (DeflaterOutputStream compressed = new DeflaterOutputStream(chunks, compressor, 65536)) {
                for (int y = 0; y < height; ++y) {
                    current(guard);
                    compressed.write(0); // PNG filter None; bytes are the core's exact diagnostic alpha.
                    compressed.write(mask, y * width, width);
                }
                compressed.finish();
                chunks.finish();
            } finally { compressor.end(); }
            pngChunk(png, "IEND", new byte[0], 0);
            png.flush();
            file.getFD().sync();
        }
    }
    private static void pngChunk(DataOutputStream png, String type, byte[] bytes, int length) throws IOException {
        byte[] name = type.getBytes(StandardCharsets.US_ASCII);
        CRC32 crc = new CRC32();
        crc.update(name); crc.update(bytes, 0, length);
        png.writeInt(length); png.write(name); png.write(bytes, 0, length); png.writeInt((int) crc.getValue());
    }
    private static final class MaskChunks extends OutputStream {
        private final DataOutputStream output;
        private final byte[] buffer = new byte[65536];
        private int count;
        MaskChunks(DataOutputStream output) { this.output = output; }
        @Override public void write(int value) throws IOException {
            buffer[count++] = (byte) value;
            if (count == buffer.length) finish();
        }
        @Override public void write(byte[] bytes, int offset, int length) throws IOException {
            while (length > 0) {
                int copied = Math.min(length, buffer.length - count);
                System.arraycopy(bytes, offset, buffer, count, copied);
                count += copied; offset += copied; length -= copied;
                if (count == buffer.length) finish();
            }
        }
        void finish() throws IOException {
            if (count > 0) { pngChunk(output, "IDAT", buffer, count); count = 0; }
        }
    }
}
