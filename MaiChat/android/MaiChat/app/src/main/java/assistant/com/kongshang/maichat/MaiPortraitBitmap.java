// 文件作用：统一原图元数据、EXIF 坐标与有界条带解码；不修改源文件，不分配第二张全图。
package com.kongshang.maichat;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapRegionDecoder;
import android.graphics.Canvas;
import android.graphics.ColorSpace;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.media.ExifInterface;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;

/** Software sRGB pixels in oriented source coordinates, shared by analysis and final composition. */
public final class MaiPortraitBitmap {
    private static final long MAX_METADATA_PIXELS = 100_000_000L;
    private static final long STRIP_BYTES = 4L * 1024 * 1024;
    private static final int REGION_HALO = 16;

    public static final class Source {
        public final File file;
        public final int rawWidth, rawHeight, width, height, orientation;
        Source(File file, int rawWidth, int rawHeight, int orientation) {
            this.file = file;
            this.rawWidth = rawWidth;
            this.rawHeight = rawHeight;
            this.orientation = orientation;
            boolean swapped = orientation >= 5 && orientation <= 8;
            width = swapped ? rawHeight : rawWidth;
            height = swapped ? rawWidth : rawHeight;
        }
    }

    private MaiPortraitBitmap() {}

    public static Source inspect(File source) throws IOException {
        if (!source.isFile() || source.length() < 1 || source.length() > 50L * 1024 * 1024)
            throw new IOException("Portrait source must be a readable image of at most 50 MB");
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(source.getPath(), bounds);
        if (bounds.outWidth < 1 || bounds.outHeight < 1 || bounds.outWidth > 20_000 || bounds.outHeight > 20_000
                || (long) bounds.outWidth * bounds.outHeight > MAX_METADATA_PIXELS)
            throw new IOException("Portrait source dimensions exceed the metadata budget");
        int orientation = ExifInterface.ORIENTATION_NORMAL;
        try (FileInputStream input = new FileInputStream(source)) {
            orientation = new ExifInterface(input).getAttributeInt(
                ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
        } catch (IOException ignored) { /* Images without EXIF are already upright. */ }
        if (orientation < 1 || orientation > 8) orientation = ExifInterface.ORIENTATION_NORMAL;
        return new Source(source, bounds.outWidth, bounds.outHeight, orientation);
    }

    private static BitmapFactory.Options options(int sample, boolean mutable) {
        BitmapFactory.Options value = new BitmapFactory.Options();
        value.inPreferredConfig = Bitmap.Config.ARGB_8888;
        value.inPreferredColorSpace = ColorSpace.get(ColorSpace.Named.SRGB);
        value.inMutable = mutable;
        value.inScaled = false;
        value.inSampleSize = sample;
        return value;
    }

    private static Matrix orientationMatrix(int orientation) {
        Matrix matrix = new Matrix();
        switch (orientation) {
            case 2: matrix.setScale(-1, 1); break;
            case 3: matrix.setRotate(180); break;
            case 4: matrix.setScale(1, -1); break;
            case 5: matrix.setRotate(90); matrix.postScale(-1, 1); break;
            case 6: matrix.setRotate(90); break;
            case 7: matrix.setRotate(270); matrix.postScale(-1, 1); break;
            case 8: matrix.setRotate(270); break;
            default: break;
        }
        return matrix;
    }

    private static Bitmap orient(Bitmap raw, int orientation) {
        if (orientation == 1) return raw;
        try {
            Bitmap upright = Bitmap.createBitmap(raw, 0, 0, raw.getWidth(), raw.getHeight(),
                orientationMatrix(orientation), false);
            if (upright != raw) raw.recycle();
            return upright;
        } catch (RuntimeException | OutOfMemoryError failure) {
            raw.recycle();
            throw failure;
        }
    }

    public static Bitmap proxy(Source source, int maximumSide) throws IOException {
        int sample = 1;
        while (Math.max(source.rawWidth, source.rawHeight) / sample > maximumSide) sample *= 2;
        Bitmap raw = BitmapFactory.decodeFile(source.file.getPath(), options(sample, false));
        if (raw == null) throw new IOException("Cannot decode portrait analysis proxy");
        Bitmap upright = orient(raw, source.orientation);
        if (Math.max(upright.getWidth(), upright.getHeight()) <= maximumSide) return upright;
        float ratio = (float) maximumSide / Math.max(upright.getWidth(), upright.getHeight());
        Bitmap scaled = Bitmap.createScaledBitmap(upright, Math.max(1, Math.round(upright.getWidth() * ratio)),
            Math.max(1, Math.round(upright.getHeight() * ratio)), true);
        if (scaled != upright) upright.recycle();
        return scaled;
    }

    /** Half-open pixel edges; never map centers with width-1 and then round the crop. */
    public static Rect rawToUpright(Source source, Rect raw) {
        int w = source.rawWidth, h = source.rawHeight;
        switch (source.orientation) {
            case 2: return new Rect(w - raw.right, raw.top, w - raw.left, raw.bottom);
            case 3: return new Rect(w - raw.right, h - raw.bottom, w - raw.left, h - raw.top);
            case 4: return new Rect(raw.left, h - raw.bottom, raw.right, h - raw.top);
            case 5: return new Rect(raw.top, raw.left, raw.bottom, raw.right);
            case 6: return new Rect(h - raw.bottom, raw.left, h - raw.top, raw.right);
            case 7: return new Rect(h - raw.bottom, w - raw.right, h - raw.top, w - raw.left);
            case 8: return new Rect(raw.top, w - raw.right, raw.bottom, w - raw.left);
            default: return new Rect(raw);
        }
    }

    public static Rect uprightToRaw(Source source, Rect upright) {
        int w = source.rawWidth, h = source.rawHeight;
        switch (source.orientation) {
            case 2: return new Rect(w - upright.right, upright.top, w - upright.left, upright.bottom);
            case 3: return new Rect(w - upright.right, h - upright.bottom, w - upright.left, h - upright.top);
            case 4: return new Rect(upright.left, h - upright.bottom, upright.right, h - upright.top);
            case 5: return new Rect(upright.top, upright.left, upright.bottom, upright.right);
            case 6: return new Rect(upright.top, h - upright.right, upright.bottom, h - upright.left);
            case 7: return new Rect(w - upright.bottom, h - upright.right, w - upright.top, h - upright.left);
            case 8: return new Rect(w - upright.bottom, upright.left, w - upright.top, upright.right);
            default: return new Rect(upright);
        }
    }

    public static Bitmap region(Source source, BitmapRegionDecoder decoder, Rect upright) throws IOException {
        Rect rawBounds = uprightToRaw(source, upright);
        Bitmap raw = decodeRawRegion(source, decoder, rawBounds);
        Bitmap result = orient(raw, source.orientation);
        if (result.getWidth() != upright.width() || result.getHeight() != upright.height()) {
            result.recycle();
            throw new IOException("Portrait region dimensions changed while decoding");
        }
        return result;
    }

    private static Bitmap decodeRawRegion(Source source, BitmapRegionDecoder decoder, Rect bounds)
            throws IOException {
        // JPEG chroma upsampling at a cropped edge lacks its neighboring samples. Decode an
        // MCU-aligned context first, then crop exact pixels; otherwise even a flip can change
        // the first/last region column compared with a complete source decode.
        Rect context = new Rect(Math.max(0, bounds.left - REGION_HALO) / 16 * 16,
            Math.max(0, bounds.top - REGION_HALO) / 16 * 16,
            Math.min(source.rawWidth, (bounds.right + REGION_HALO + 15) / 16 * 16),
            Math.min(source.rawHeight, (bounds.bottom + REGION_HALO + 15) / 16 * 16));
        Bitmap decoded = decoder.decodeRegion(context, options(1, false));
        if (decoded == null) throw new IOException("Cannot decode a native portrait region");
        try {
            Bitmap exact = Bitmap.createBitmap(decoded, bounds.left - context.left,
                bounds.top - context.top, bounds.width(), bounds.height());
            if (exact != decoded) decoded.recycle();
            return exact;
        } catch (RuntimeException | OutOfMemoryError failure) {
            decoded.recycle();
            throw failure;
        }
    }

    /** Only the destination spans the full source. EXIF rotations allocate at most one extra strip. */
    public static Bitmap decodeMutable(Source source) throws IOException {
        long pixels = (long) source.width * source.height;
        if (pixels > 32_000_000L)
            throw new IOException("Portrait source exceeds the available full-resolution memory budget");
        Runtime runtime = Runtime.getRuntime();
        long available = runtime.maxMemory() - (runtime.totalMemory() - runtime.freeMemory());
        long required = pixels * 4 + STRIP_BYTES * 2 + 48L * 1024 * 1024;
        if (required > available) {
            // Completed ML Kit windows leave collectable conversion buffers. Give the VM the
            // same collection opportunity a large allocation would have before rejecting on
            // live heap; the source/strip reserve and hard pixel budget remain unchanged.
            runtime.gc();
            available = runtime.maxMemory() - (runtime.totalMemory() - runtime.freeMemory());
        }
        // One source allocation plus bounded strip copies and working reserve. Do not rely on
        // largeHeap or silently downsample when the current process cannot hold the source.
        if (required > available)
            throw new IOException("Portrait source exceeds the available full-resolution memory budget");
        if (source.orientation == 1) {
            Bitmap result = BitmapFactory.decodeFile(source.file.getPath(), options(1, true));
            if (result == null) throw new IOException("Cannot decode the portrait source");
            return result;
        }
        Bitmap result = Bitmap.createBitmap(source.width, source.height, Bitmap.Config.ARGB_8888,
            true, ColorSpace.get(ColorSpace.Named.SRGB));
        result.setDensity(Bitmap.DENSITY_NONE);
        BitmapRegionDecoder decoder = null;
        Canvas canvas = null;
        try {
            decoder = BitmapRegionDecoder.newInstance(source.file.getPath(), false);
            canvas = new Canvas(result);
            // Leave room for the decoding halo/alignment before the exact strip is rotated.
            // Context plus crop remains within the two-strip reserve, including very wide images.
            int rows = Math.max(1, (int) Math.min(source.rawHeight,
                STRIP_BYTES / (source.rawWidth * 4L) - REGION_HALO * 4L));
            for (int top = 0; top < source.rawHeight; top += rows) {
                Rect rawBounds = new Rect(0, top, source.rawWidth, Math.min(source.rawHeight, top + rows));
                Bitmap raw = decodeRawRegion(source, decoder, rawBounds);
                Bitmap strip = orient(raw, source.orientation);
                try {
                    Rect target = rawToUpright(source, rawBounds);
                    canvas.drawBitmap(strip, null, target, null);
                } finally { strip.recycle(); }
            }
            return result;
        } catch (IOException | RuntimeException | OutOfMemoryError failure) {
            result.recycle();
            throw failure;
        } finally {
            if (canvas != null) canvas.setBitmap(null);
            if (decoder != null) decoder.recycle();
        }
    }
}
