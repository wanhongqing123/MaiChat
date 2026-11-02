// 文件作用：用真实字体排版验证窄屏转写预览保留尾句、短文不裁和窗口变宽恢复。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.text.Layout;
import android.text.StaticLayout;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class VoiceTranscriptionOverlayTest {
    private ViewGroup layer;
    private VoiceTranscriptionOverlay overlay;

    private TextView preview(String text, int width) {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        overlay = new VoiceTranscriptionOverlay(activity, false, 100, 700, action -> { });
        overlay.show();
        ViewGroup decor = (ViewGroup) activity.getWindow().getDecorView();
        layer = (ViewGroup) decor.getChildAt(decor.getChildCount() - 1);
        overlay.updateText(text);
        layout(width);
        return (TextView) ((LinearLayout) layer.getChildAt(0)).getChildAt(1);
    }

    private void layout(int width) {
        // 首次布局确定卡片宽度，后续布局处理文本高度；有限次数也覆盖重排稳定性。
        for (int count = 0; count < 3; count++) {
            layer.forceLayout();
            layer.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(900, View.MeasureSpec.EXACTLY));
            layer.layout(0, 0, width, 900);
        }
    }

    private int lines(TextView text) {
        return StaticLayout.Builder.obtain(text.getText(), 0, text.length(), text.getPaint(),
            text.getWidth() - text.getCompoundPaddingLeft() - text.getCompoundPaddingRight())
            .setIncludePad(false).setBreakStrategy(Layout.BREAK_STRATEGY_SIMPLE)
            .setHyphenationFrequency(Layout.HYPHENATION_FREQUENCY_NONE).build().getLineCount();
    }

    @Test public void narrowChinesePreviewKeepsTheSpokenTailWithinEightRealLines() {
        String source = "这是一段需要在窄屏真实字体下换行的中文转写内容。".repeat(7) + "最后一句不能丢。";
        TextView text = preview(source, 260);
        assertTrue(text.getText().toString().startsWith("…"));
        assertTrue(text.getText().toString().endsWith("最后一句不能丢。"));
        assertTrue("Rendered text must fit without TextView clipping its tail", lines(text) <= 8);
        overlay.dismiss();
    }

    @Test public void shortPreviewIsNotCroppedOrPrefixed() {
        TextView text = preview("今天开会，请准时。", 260);
        assertEquals("今天开会，请准时。", text.getText().toString());
        assertTrue(lines(text) <= 8);
        overlay.dismiss();
    }

    @Test public void widerWindowRecomputesFromTheOriginalTailWithoutRepeatedCropping() {
        String source = "中文排版验证".repeat(19) + "结尾。";
        TextView text = preview(source, 180);
        String narrow = text.getText().toString();
        assertTrue(narrow.startsWith("…"));
        layout(600);
        String wider = text.getText().toString();
        assertTrue(wider.endsWith("结尾。"));
        assertTrue("Narrow length=" + narrow.length() + ", wide length=" + wider.length()
            + ", actual width=" + text.getWidth(), wider.length() > narrow.length());
        assertTrue(lines(text) <= 8);
        layout(600);
        assertEquals("Stable width must not crop an already cropped preview again", wider, text.getText().toString());
        overlay.dismiss();
    }
}
