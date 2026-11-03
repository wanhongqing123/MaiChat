// 文件作用：Android 移动端 Agent 工具与宿主能力；MobileImageVision 负责移动端本地图片视觉分析，本文件给出其实现。
// 主要入口：detectFaces、segmentPerson、prepare。
package com.kongshang.maichat.tools;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.BitmapRegionDecoder;
import android.graphics.Matrix;
import android.graphics.PointF;
import android.graphics.Rect;
import android.media.ExifInterface;
import com.google.android.gms.tasks.Tasks;
import com.google.android.gms.tasks.Task;
import com.google.mlkit.vision.common.InputImage;
import com.google.mlkit.vision.common.internal.MobileVisionBase;
import com.google.mlkit.vision.face.Face;
import com.google.mlkit.vision.face.FaceDetection;
import com.google.mlkit.vision.face.FaceDetector;
import com.google.mlkit.vision.face.FaceDetectorOptions;
import com.google.mlkit.vision.face.FaceLandmark;
import com.google.mlkit.vision.segmentation.Segmentation;
import com.google.mlkit.vision.segmentation.SegmentationMask;
import com.google.mlkit.vision.segmentation.Segmenter;
import com.google.mlkit.vision.segmentation.selfie.SelfieSegmenterOptions;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import org.json.JSONArray;
import org.json.JSONObject;
import com.kongshang.maichat.MaiPortraitBitmap;
import com.kongshang.maichat.MaiPortraitImage;

/** On-device face localization and person masks for Agent workspace images. */
public final class MobileImageVision {
    // The pinned SDK retains its native working set after close/reload. Reusing one anonymous
    // model client across photos keeps that allocation bounded; no Bitmap/account is cached here.
    private static FaceDetector sharedDetector;
    private static boolean detectorUnavailable;
    private static Task<Void> detectorRelease;
    private static final Executor DIRECT_COMPLETION = Runnable::run;
    private static final ConcurrentHashMap<Bitmap, Task<Void>> deferredFrames = new ConcurrentHashMap<>();
    private MobileImageVision() {}

    public static synchronized JSONObject detectFaces(File source) throws Exception {
        if (detectorUnavailable)
            throw new IOException("Face analysis stopped after an unfinished operation; reopen the app before retrying");
        MaiPortraitBitmap.Source metadata = MaiPortraitBitmap.inspect(source);
        FaceDetectorOptions options = new FaceDetectorOptions.Builder()
            .setPerformanceMode(FaceDetectorOptions.PERFORMANCE_MODE_ACCURATE)
            .setLandmarkMode(FaceDetectorOptions.LANDMARK_MODE_ALL)
            .setMinFaceSize(0.02f)
            .build();
        if (sharedDetector == null) sharedDetector = FaceDetection.getClient(options);
        FaceDetector detector = sharedDetector;
        BitmapRegionDecoder regions = null;
        try {
            JSONArray faces = new JSONArray();
            Bitmap proxy = MaiPortraitBitmap.proxy(metadata, 2048);
            try {
                appendFaces(detector, proxy, 0, 0, (double) metadata.width / proxy.getWidth(),
                    (double) metadata.height / proxy.getHeight(), metadata, faces);
            } finally { recycleFrame(proxy); }
            JSONArray globalFaces = new JSONArray(faces.toString());
            // 原图的小脸可能在全图代理中消失。原生 2048 窗口保留脸部细节；检测候选
            // 统一交共享 planner 合并，避免 detect_faces 与实际编辑得到两套主体编号。
            if (Math.max(metadata.width, metadata.height) > 2048) {
                int columns = Math.max(1, (int) Math.ceil(Math.max(0, metadata.width - 2048) / 1536.0) + 1);
                int rows = Math.max(1, (int) Math.ceil(Math.max(0, metadata.height - 2048) / 1536.0) + 1);
                if ((long) columns * rows > 24)
                    throw new IllegalArgumentException("Face analysis exceeds the bounded window budget");
                regions = BitmapRegionDecoder.newInstance(source.getPath(), false);
                for (int row = 0; row < rows; ++row) {
                    for (int column = 0; column < columns; ++column) {
                        int left = Math.min(column * 1536, Math.max(0, metadata.width - 2048));
                        int top = Math.min(row * 1536, Math.max(0, metadata.height - 2048));
                        Rect window = new Rect(left, top, Math.min(metadata.width, left + 2048),
                            Math.min(metadata.height, top + 2048));
                        Bitmap pixels = MaiPortraitBitmap.region(metadata, regions, window);
                        try { appendFaces(detector, pixels, left, top, 1, 1, metadata, faces); }
                        finally { recycleFrame(pixels); }
                    }
                }
            }
            JSONArray merged = mergeFacesPreservingAngles(faces);
            if (regions != null)
                merged = corroborateLocalFaces(detector, metadata, regions, merged, globalFaces);
            return new JSONObject().put("source_width", metadata.width)
                .put("source_height", metadata.height)
                .put("coordinate_origin", "top_left").put("faces", merged);
        } finally {
            if (regions != null) regions.recycle();
            // Process-owned model only. Closing after each photo and recreating it retained an
            // additional ~229 MB per photo in this SDK; successful inputs are released per frame.
        }
    }

    private static void recycleFrame(Bitmap bitmap) {
        if (!deferredFrames.containsKey(bitmap) && !bitmap.isRecycled()) bitmap.recycle();
    }

    private static JSONArray corroborateLocalFaces(FaceDetector detector, MaiPortraitBitmap.Source source,
                                                   BitmapRegionDecoder regions, JSONArray candidates,
                                                   JSONArray globalFaces) throws Exception {
        if (candidates.length() > 32)
            throw new IllegalArgumentException("Face analysis exceeds the bounded verification budget");
        JSONArray confirmed = new JSONArray();
        for (int index = 0; index < candidates.length(); ++index) {
            JSONObject candidate = candidates.getJSONObject(index);
            if (matchesAnyFace(candidate, globalFaces)) {
                confirmed.put(candidate);
                continue;
            }
            JSONObject box = candidate.getJSONObject("box");
            int side = Math.min(2048, Math.max(128, (int) Math.ceil(
                Math.max(box.getDouble("width"), box.getDouble("height")) * 2.5)));
            int width = Math.min(source.width, side), height = Math.min(source.height, side);
            int left = Math.max(0, Math.min(source.width - width, (int) Math.floor(
                box.getDouble("x") + box.getDouble("width") * .5 - width * .5)));
            int top = Math.max(0, Math.min(source.height - height, (int) Math.floor(
                box.getDouble("y") + box.getDouble("height") * .5 - height * .5)));
            Bitmap pixels = MaiPortraitBitmap.region(source, regions,
                new Rect(left, top, left + width, top + height));
            JSONArray localFaces = new JSONArray();
            try { appendFaces(detector, pixels, left, top, 1, 1, source, localFaces); }
            finally { recycleFrame(pixels); }
            // Window edges can turn clothing into an isolated false face. A centered native
            // recheck must locate the same face; another person elsewhere in the crop cannot
            // validate it. Globally supported faces retain their original detections unchanged.
            if (matchesAnyFace(candidate, localFaces)) confirmed.put(candidate);
        }
        return confirmed;
    }

    private static boolean matchesAnyFace(JSONObject candidate, JSONArray alternatives) throws Exception {
        JSONObject a = candidate.getJSONObject("box");
        double ax = a.getDouble("x"), ay = a.getDouble("y");
        double aw = a.getDouble("width"), ah = a.getDouble("height");
        for (int index = 0; index < alternatives.length(); ++index) {
            JSONObject b = alternatives.getJSONObject(index).getJSONObject("box");
            double bx = b.getDouble("x"), by = b.getDouble("y");
            double bw = b.getDouble("width"), bh = b.getDouble("height");
            double intersection = Math.max(0, Math.min(ax + aw, bx + bw) - Math.max(ax, bx))
                * Math.max(0, Math.min(ay + ah, by + bh) - Math.max(ay, by));
            double first = aw * ah, second = bw * bh;
            if (intersection / (first + second - intersection) > .3
                    || intersection / Math.min(first, second) > .72) return true;
        }
        return false;
    }

    private static void retireDetector(FaceDetector detector, Bitmap input) {
        detectorUnavailable = true;
        sharedDetector = null;
        if (detectorRelease == null)
            detectorRelease = ((MobileVisionBase<?>) detector).closeWithTask();
        Task<Void> release = detectorRelease;
        deferredFrames.put(input, release);
        // A timed-out Tasks.await does not stop native inference. Keep that input alive until
        // the pinned SDK's close task confirms its queue is drained and release has completed.
        release.addOnCompleteListener(DIRECT_COMPLETION, completed -> {
            if (completed.isSuccessful()) {
                if (!input.isRecycled()) input.recycle();
                deferredFrames.remove(input);
            }
        });
        try { Tasks.await(release, 60, TimeUnit.SECONDS); }
        catch (Exception ignored) { /* Failed release remains closed to new batches in this process. */ }
    }

    private static void appendFaces(FaceDetector detector, Bitmap bitmap, int offsetX, int offsetY,
                                    double scaleX, double scaleY, MaiPortraitBitmap.Source source,
                                    JSONArray output) throws Exception {
        final List<Face> detected;
        try {
            detected = Tasks.await(detector.process(InputImage.fromBitmap(bitmap, 0)),
                60, TimeUnit.SECONDS);
        } catch (Exception failure) {
            retireDetector(detector, bitmap);
            throw failure;
        } catch (Error failure) {
            retireDetector(detector, bitmap);
            throw failure;
        }
        for (Face face : detected) {
            Rect bounds = face.getBoundingBox();
            int left = Math.max(0, (int) Math.round(offsetX + bounds.left * scaleX));
            int top = Math.max(0, (int) Math.round(offsetY + bounds.top * scaleY));
            int right = Math.min(source.width, (int) Math.round(offsetX + bounds.right * scaleX));
            int bottom = Math.min(source.height, (int) Math.round(offsetY + bounds.bottom * scaleY));
            if (right - left < 8 || bottom - top < 8) continue;
            JSONObject points = new JSONObject();
            landmark(points, "left_eye", face.getLandmark(FaceLandmark.LEFT_EYE), scaleX, scaleY, offsetX, offsetY);
            landmark(points, "right_eye", face.getLandmark(FaceLandmark.RIGHT_EYE), scaleX, scaleY, offsetX, offsetY);
            output.put(new JSONObject().put("box", new JSONObject().put("x", left).put("y", top)
                .put("width", right - left).put("height", bottom - top)).put("landmarks", points)
                .put("yaw_degrees", face.getHeadEulerAngleY()).put("roll_degrees", face.getHeadEulerAngleZ()));
            if (output.length() > 256) throw new IllegalArgumentException("Face detection exceeded its candidate budget");
        }
    }

    private static JSONArray mergeFacesPreservingAngles(JSONArray candidates) throws Exception {
        JSONArray merged = MaiPortraitImage.mergeFaces(candidates);
        for (int index = 0; index < merged.length(); ++index) {
            JSONObject face = merged.getJSONObject(index), selected = null;
            double best = Double.POSITIVE_INFINITY;
            JSONObject bounds = face.getJSONObject("box"), eyes = face.getJSONObject("landmarks");
            for (int candidate = 0; candidate < candidates.length(); ++candidate) {
                JSONObject value = candidates.getJSONObject(candidate), box = value.getJSONObject("box");
                if (box.getDouble("x") != bounds.getDouble("x") || box.getDouble("y") != bounds.getDouble("y")
                        || box.getDouble("width") != bounds.getDouble("width")
                        || box.getDouble("height") != bounds.getDouble("height")) continue;
                double distance = 0;
                for (String eye : new String[]{"left_eye", "right_eye"}) {
                    JSONObject point = value.getJSONObject("landmarks").optJSONObject(eye);
                    if (point == null) { distance += 1e12; continue; }
                    JSONObject target = eyes.getJSONObject(eye);
                    distance += Math.abs(point.getDouble("x") - target.getDouble("x"))
                        + Math.abs(point.getDouble("y") - target.getDouble("y"));
                }
                if (distance < best) { best = distance; selected = value; }
            }
            // Native NMS selects an actual host candidate. Preserve that candidate's existing
            // upright-image Euler output without teaching the portable planner Android fields.
            if (selected != null) {
                face.put("yaw_degrees", selected.get("yaw_degrees"));
                face.put("roll_degrees", selected.get("roll_degrees"));
            }
        }
        return merged;
    }

    public static JSONObject segmentPerson(File source, File target) throws Exception {
        PreparedImage image = prepare(source);
        SelfieSegmenterOptions options = new SelfieSegmenterOptions.Builder()
            .setDetectorMode(SelfieSegmenterOptions.SINGLE_IMAGE_MODE).build();
        Segmenter segmenter = Segmentation.getClient(options);
        Bitmap mask = null;
        try {
            SegmentationMask result = Tasks.await(
                segmenter.process(InputImage.fromBitmap(image.bitmap, 0)), 60, TimeUnit.SECONDS);
            int width = result.getWidth(), height = result.getHeight();
            if (width < 1 || height < 1 || (long) width * height > 12_000_000)
                throw new IllegalStateException("人物遮罩尺寸无效");
            ByteBuffer confidences = result.getBuffer();
            confidences.order(ByteOrder.nativeOrder());
            confidences.rewind();
            int[] pixels = new int[width * height];
            for (int index = 0; index < pixels.length; index++) {
                float confidence = confidences.getFloat();
                int value = Float.isFinite(confidence)
                    ? Math.round(Math.max(0, Math.min(1, confidence)) * 255) : 0;
                pixels[index] = 0xff000000 | value << 16 | value << 8 | value;
            }
            mask = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            mask.setPixels(pixels, 0, width, 0, 0, width, height);
            try (FileOutputStream output = new FileOutputStream(target)) {
                if (!mask.compress(Bitmap.CompressFormat.PNG, 100, output))
                    throw new IOException("无法写入人物遮罩");
            }
            if (target.length() < 1 || target.length() > 50L * 1024 * 1024)
                throw new IllegalStateException("人物遮罩文件大小无效");
            return new JSONObject().put("path", target.getName())
                .put("mime_type", "image/png").put("kind", "person_mask")
                .put("source_width", image.sourceWidth).put("source_height", image.sourceHeight)
                .put("mask_width", width).put("mask_height", height)
                .put("next_tool", "mobile_preview_image");
        } catch (Exception error) {
            target.delete();
            throw error;
        } finally {
            if (mask != null) mask.recycle();
            segmenter.close();
            image.bitmap.recycle();
        }
    }

    private static void landmark(JSONObject output, String name, FaceLandmark landmark,
                                 double scaleX, double scaleY) throws Exception {
        landmark(output, name, landmark, scaleX, scaleY, 0, 0);
    }
    private static void landmark(JSONObject output, String name, FaceLandmark landmark,
                                 double scaleX, double scaleY, int offsetX, int offsetY) throws Exception {
        if (landmark == null) return;
        PointF point = landmark.getPosition();
        output.put(name, new JSONObject()
            .put("x", Math.round(offsetX + point.x * scaleX))
            .put("y", Math.round(offsetY + point.y * scaleY)));
    }

    // 先按模型输入限制缩放与旋转源图，再构造像素缓冲；解码失败直接抛错，不传空图给推理器。
    private static PreparedImage prepare(File source) throws Exception {
        if (source.length() < 1 || source.length() > 50L * 1024 * 1024)
            throw new IllegalArgumentException("源图片不能超过 50 MB");
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(source.getPath(), bounds);
        if (bounds.outWidth < 1 || bounds.outHeight < 1
                || bounds.outWidth > 20_000 || bounds.outHeight > 20_000)
            throw new IllegalArgumentException("图片尺寸无效");
        int orientation = ExifInterface.ORIENTATION_NORMAL;
        try {
            orientation = new ExifInterface(source.getPath()).getAttributeInt(
                ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL);
        } catch (IOException ignored) { /* Images without EXIF are already upright. */ }
        boolean swapped = orientation == ExifInterface.ORIENTATION_ROTATE_90
            || orientation == ExifInterface.ORIENTATION_ROTATE_270
            || orientation == ExifInterface.ORIENTATION_TRANSPOSE
            || orientation == ExifInterface.ORIENTATION_TRANSVERSE;
        int sourceWidth = swapped ? bounds.outHeight : bounds.outWidth;
        int sourceHeight = swapped ? bounds.outWidth : bounds.outHeight;
        BitmapFactory.Options decode = new BitmapFactory.Options();
        decode.inPreferredConfig = Bitmap.Config.ARGB_8888;
        decode.inSampleSize = 1;
        while (Math.max(bounds.outWidth, bounds.outHeight) / decode.inSampleSize > 2048)
            decode.inSampleSize *= 2;
        Bitmap raw = BitmapFactory.decodeFile(source.getPath(), decode);
        if (raw == null) throw new IllegalArgumentException("无法解码源图片");
        Matrix matrix = new Matrix();
        switch (orientation) {
        case ExifInterface.ORIENTATION_FLIP_HORIZONTAL: matrix.setScale(-1, 1); break;
        case ExifInterface.ORIENTATION_ROTATE_180: matrix.setRotate(180); break;
        case ExifInterface.ORIENTATION_FLIP_VERTICAL: matrix.setScale(1, -1); break;
        case ExifInterface.ORIENTATION_TRANSPOSE:
            matrix.setRotate(90); matrix.postScale(-1, 1); break;
        case ExifInterface.ORIENTATION_ROTATE_90: matrix.setRotate(90); break;
        case ExifInterface.ORIENTATION_TRANSVERSE:
            matrix.setRotate(270); matrix.postScale(-1, 1); break;
        case ExifInterface.ORIENTATION_ROTATE_270: matrix.setRotate(270); break;
        default: break;
        }
        Bitmap upright = matrix.isIdentity() ? raw
            : Bitmap.createBitmap(raw, 0, 0, raw.getWidth(), raw.getHeight(), matrix, true);
        if (upright != raw) raw.recycle();
        if (Math.max(upright.getWidth(), upright.getHeight()) > 2048) {
            double scale = 2048.0 / Math.max(upright.getWidth(), upright.getHeight());
            Bitmap smaller = Bitmap.createScaledBitmap(upright,
                Math.max(1, (int) Math.round(upright.getWidth() * scale)),
                Math.max(1, (int) Math.round(upright.getHeight() * scale)), true);
            if (smaller != upright) upright.recycle();
            upright = smaller;
        }
        return new PreparedImage(upright, sourceWidth, sourceHeight);
    }

    private static final class PreparedImage {
        final Bitmap bitmap;
        final int sourceWidth, sourceHeight;
        PreparedImage(Bitmap bitmap, int sourceWidth, int sourceHeight) {
            this.bitmap = bitmap;
            this.sourceWidth = sourceWidth;
            this.sourceHeight = sourceHeight;
        }
    }
}
