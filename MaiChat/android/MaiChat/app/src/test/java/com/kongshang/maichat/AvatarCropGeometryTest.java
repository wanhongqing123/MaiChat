// 文件作用：验证头像裁剪的真实源图范围，防止拖动、缩放或尺寸变化后露出空白。
// 运行边界：纯 Java 几何回归，不依赖 Android 渲染或设备。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class AvatarCropGeometryTest {
    private static final float EPSILON = 0.002f;

    @Test public void landscapeStartsWithCenteredSquareInSourcePixels() {
        AvatarCropGeometry crop = new AvatarCropGeometry(2000, 1000, 300);
        assertEquals(500, crop.sourceLeft(), EPSILON);
        assertEquals(0, crop.sourceTop(), EPSILON);
        assertEquals(1000, crop.sourceSide(), EPSILON);
    }

    @Test public void portraitStartsWithCenteredSquareInSourcePixels() {
        AvatarCropGeometry crop = new AvatarCropGeometry(1000, 2000, 300);
        assertEquals(0, crop.sourceLeft(), EPSILON);
        assertEquals(500, crop.sourceTop(), EPSILON);
        assertEquals(1000, crop.sourceSide(), EPSILON);
    }

    @Test public void dragStopsAtSourceEdgesWithoutExposingBlankPixels() {
        AvatarCropGeometry crop = new AvatarCropGeometry(2000, 1000, 300);
        crop.pan(5000, -5000);
        assertEquals(0, crop.sourceLeft(), EPSILON);
        assertEquals(0, crop.sourceTop(), EPSILON);
        crop.pan(-5000, 5000);
        assertEquals(1000, crop.sourceLeft(), EPSILON);
        assertEquals(0, crop.sourceTop(), EPSILON);
    }

    @Test public void pinchKeepsSourcePointUnderTheFingerWhenItFits() {
        AvatarCropGeometry crop = new AvatarCropGeometry(1000, 1000, 300);
        float beforeX = (75 - crop.left()) / crop.scale();
        float beforeY = (120 - crop.top()) / crop.scale();
        crop.zoom(2, 75, 120);
        assertEquals(beforeX, (75 - crop.left()) / crop.scale(), EPSILON);
        assertEquals(beforeY, (120 - crop.top()) / crop.scale(), EPSILON);
        assertEquals(500, crop.sourceSide(), EPSILON);
    }

    @Test public void zoomOutCannotMakeTheImageSmallerThanTheCrop() {
        AvatarCropGeometry crop = new AvatarCropGeometry(2000, 1000, 300);
        crop.zoom(4, 150, 150);
        crop.pan(900, -900);
        crop.zoom(0.001f, 150, 150);
        assertEquals(1000, crop.sourceSide(), EPSILON);
        assertInside(crop, 2000, 1000);
    }

    @Test public void resizeKeepsTheSameSelectedSourceArea() {
        AvatarCropGeometry crop = new AvatarCropGeometry(1800, 1200, 300);
        crop.zoom(2, 120, 100);
        crop.pan(-40, -25);
        float left = crop.sourceLeft(), top = crop.sourceTop(), side = crop.sourceSide();
        crop.resize(480);
        assertEquals(left, crop.sourceLeft(), EPSILON);
        assertEquals(top, crop.sourceTop(), EPSILON);
        assertEquals(side, crop.sourceSide(), EPSILON);
    }

    @Test public void malformedGestureValuesDoNotPoisonTheTransform() {
        AvatarCropGeometry crop = new AvatarCropGeometry(1200, 800, 300);
        float left = crop.sourceLeft(), top = crop.sourceTop(), side = crop.sourceSide();
        crop.zoom(Float.NaN, 150, 150);
        crop.zoom(2, Float.POSITIVE_INFINITY, 150);
        crop.zoom(0, 150, 150);
        crop.pan(Float.NaN, 100);
        crop.resize(Float.POSITIVE_INFINITY);
        assertEquals(left, crop.sourceLeft(), EPSILON);
        assertEquals(top, crop.sourceTop(), EPSILON);
        assertEquals(side, crop.sourceSide(), EPSILON);
    }

    @Test public void longDragAndPinchSequencesRemainInsideVeryWideAndTallImages() {
        int[][] sizes = {{2048, 1}, {1, 2048}, {2048, 1536}, {900, 1600}};
        for (int[] size : sizes) {
            AvatarCropGeometry crop = new AvatarCropGeometry(size[0], size[1], 320);
            for (int index = 0; index < 100; index++) {
                crop.zoom(index % 3 == 0 ? 0.75f : 1.6f, index * 5 % 320, index * 9 % 320);
                crop.pan(index * 31 % 900 - 450, index * 37 % 900 - 450);
                assertInside(crop, size[0], size[1]);
            }
        }
    }

    private static void assertInside(AvatarCropGeometry crop, int width, int height) {
        assertTrue(crop.sourceSide() > 0);
        assertTrue(crop.sourceLeft() >= -EPSILON);
        assertTrue(crop.sourceTop() >= -EPSILON);
        assertTrue(crop.sourceLeft() + crop.sourceSide() <= width + EPSILON);
        assertTrue(crop.sourceTop() + crop.sourceSide() <= height + EPSILON);
    }
}
