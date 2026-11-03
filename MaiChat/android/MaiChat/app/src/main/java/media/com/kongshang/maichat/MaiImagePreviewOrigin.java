// A gallery keeps the original image's return target while browsing neighboring media.
package com.kongshang.maichat;

import android.graphics.RectF;
import android.view.View;
import java.lang.ref.WeakReference;

final class MaiImagePreviewOrigin {
    private final String path;
    private final String owner;
    private final int rotation;
    private final WeakReference<View> source;
    private final RectF screenFrame;
    private boolean snapshotValid = true;

    MaiImagePreviewOrigin(String path, View source, String owner, int rotation) {
        this.path = path;
        this.owner = owner;
        this.rotation = rotation;
        this.source = new WeakReference<>(source);
        screenFrame = MaiImagePreviewTransition.sourceFrame(source, null);
    }

    RectF frame(String currentPath, String currentOwner, int currentRotation, View viewport) {
        if (!path.equals(currentPath) || !owner.equals(currentOwner)) return null;
        // Save the pre-presentation frame like iOS: hiding Android system bars may relayout
        // the covered activity. Using that temporary row position would miss the original bubble.
        if (!snapshotValid || rotation != currentRotation)
            return MaiImagePreviewTransition.sourceFrame(source.get(), viewport);
        if (screenFrame == null) return null;
        int[] host = new int[2];
        viewport.getLocationOnScreen(host);
        RectF result = new RectF(screenFrame);
        result.offset(-host[0], -host[1]);
        return result;
    }

    void configurationChanged() { snapshotValid = false; }
}
