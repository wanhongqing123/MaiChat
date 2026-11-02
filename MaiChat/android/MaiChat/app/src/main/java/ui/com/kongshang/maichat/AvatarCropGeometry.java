// 文件作用：把头像预览中的拖动、缩放转换成源图内的正方形裁剪范围。
// 边界：坐标以裁剪框左上角为原点；图片始终覆盖裁剪框，缩放保留手指下的源图位置。
package com.kongshang.maichat;

final class AvatarCropGeometry {
    private static final float MAX_ZOOM = 6;
    private final int imageWidth, imageHeight;
    private float cropSide, scale, left, top;

    AvatarCropGeometry(int imageWidth, int imageHeight, float cropSide) {
        if (imageWidth < 1 || imageHeight < 1 || !positiveFinite(cropSide))
            throw new IllegalArgumentException("Invalid avatar crop dimensions");
        this.imageWidth = imageWidth;
        this.imageHeight = imageHeight;
        this.cropSide = cropSide;
        scale = minimumScale();
        left = (cropSide - imageWidth * scale) / 2;
        top = (cropSide - imageHeight * scale) / 2;
        clampPosition();
    }

    float scale() { return scale; }
    float left() { return left; }
    float top() { return top; }
    float sourceSide() { return Math.min(Math.min(imageWidth, imageHeight), cropSide / scale); }
    float sourceLeft() { return clamp(-left / scale, 0, imageWidth - sourceSide()); }
    float sourceTop() { return clamp(-top / scale, 0, imageHeight - sourceSide()); }

    void pan(float deltaX, float deltaY) {
        if (!Float.isFinite(deltaX) || !Float.isFinite(deltaY)) return;
        left += deltaX;
        top += deltaY;
        clampPosition();
    }

    void zoom(float factor, float focusX, float focusY) {
        if (!positiveFinite(factor) || !Float.isFinite(focusX) || !Float.isFinite(focusY)) return;
        float nextScale = clamp(scale * factor, minimumScale(), minimumScale() * MAX_ZOOM);
        float ratio = nextScale / scale;
        left = focusX + (left - focusX) * ratio;
        top = focusY + (top - focusY) * ratio;
        scale = nextScale;
        clampPosition();
    }

    // 尺寸变化只重排预览，不改变用户已经选好的源图区域。
    void resize(float nextCropSide) {
        if (!positiveFinite(nextCropSide)) return;
        float ratio = nextCropSide / cropSide;
        scale *= ratio;
        left *= ratio;
        top *= ratio;
        cropSide = nextCropSide;
        clampPosition();
    }

    private float minimumScale() { return cropSide / Math.min(imageWidth, imageHeight); }

    private void clampPosition() {
        left = clamp(left, Math.min(0, cropSide - imageWidth * scale), 0);
        top = clamp(top, Math.min(0, cropSide - imageHeight * scale), 0);
    }

    private static float clamp(float value, float minimum, float maximum) {
        return Math.max(minimum, Math.min(maximum, value));
    }

    private static boolean positiveFinite(float value) {
        return Float.isFinite(value) && value > 0;
    }
}
