// 文件作用：验证缩略图保留完整 Bitmap、等比尺寸及父容器边距，不依赖原生解码。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.drawable.BitmapDrawable;
import android.widget.ImageView;
import android.widget.LinearLayout;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, qualifiers = "mdpi", manifest = Config.NONE)
public class MaiImageThumbnailViewTest {
    @Test public void fitsPortraitAndLandscapeWithoutCroppingOrReplacingTheBitmap() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        MaiImageThumbnailView view = new MaiImageThumbnailView(activity);
        Bitmap portrait = Bitmap.createBitmap(100, 200, Bitmap.Config.ARGB_8888);
        view.setImageBitmap(portrait);
        assertEquals(90, view.getLayoutParams().width);
        assertEquals(180, view.getLayoutParams().height);
        assertSame(portrait, ((BitmapDrawable) view.getDrawable()).getBitmap());
        assertEquals(ImageView.ScaleType.FIT_CENTER, view.getScaleType());
        assertTrue(view.getClipToOutline());
        Bitmap landscape = Bitmap.createBitmap(400, 100, Bitmap.Config.ARGB_8888);
        view.setImageBitmap(landscape);
        assertEquals(220, view.getLayoutParams().width);
        assertEquals(55, view.getLayoutParams().height);
        assertSame(landscape, ((BitmapDrawable) view.getDrawable()).getBitmap());
    }

    @Test public void customBoundsAndLaterParentParamsKeepTheFitAndExistingMargins() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        MaiImageThumbnailView view = new MaiImageThumbnailView(activity, 280, 340);
        view.setImageBitmap(Bitmap.createBitmap(100, 200, Bitmap.Config.ARGB_8888));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, 190);
        params.setMargins(3, 7, 5, 4);
        view.setLayoutParams(params);
        assertSame(params, view.getLayoutParams());
        assertEquals(170, params.width);
        assertEquals(340, params.height);
        assertEquals(7, params.topMargin);
        assertEquals(4, params.bottomMargin);
        view.setImageBitmap(null);
        assertEquals(280, params.width);
        assertEquals(340, params.height);
    }
}
