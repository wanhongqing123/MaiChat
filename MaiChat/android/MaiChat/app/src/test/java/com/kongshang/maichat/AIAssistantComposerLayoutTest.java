// 文件作用：用实际字体验证连续 EDIT、清空后的输入框测高与内部滚动复位。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.text.InputType;
import android.view.View;
import android.widget.EditText;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public class AIAssistantComposerLayoutTest {
    private EditText editor() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        EditText editor = new AIAssistantPanel.ComposerEditText(activity);
        editor.setLayoutParams(new android.widget.FrameLayout.LayoutParams(-1, -2));
        editor.setTextSize(16);
        editor.setMaxLines(6);
        editor.setMinHeight(88);
        editor.setPadding(26, 18, 26, 18);
        editor.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        return editor;
    }

    private void layout(EditText editor, int width) {
        editor.forceLayout();
        editor.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(500, View.MeasureSpec.AT_MOST));
        editor.layout(0, 0, width, editor.getMeasuredHeight());
    }

    @Test public void clearingPreviousEditRemovesInternalScrollBeforeTheNextRecording() {
        EditText editor = editor();
        editor.setText("你好，语音识别测试成功。");
        layout(editor, 171);
        editor.scrollTo(0, 24);
        editor.setText("");
        layout(editor, 171);
        assertEquals("Empty composer must not carry the previous edit viewport", 0, editor.getScrollY());
    }

    @Test public void secondEditReflowsAtTheRealNarrowWidthAndKeepsEveryShortLineVisible() {
        EditText editor = editor();
        for (int iteration = 0; iteration < 2; iteration++) {
            editor.setText("你好，语音识别测试成功。");
            layout(editor, 240);
            layout(editor, 125);
            editor.setSelection(editor.length());
            int lines = editor.getLayout().getLineCount();
            assertTrue("Fixture must wrap", lines > 1);
            int content = editor.getLayout().getLineTop(lines);
            assertTrue("Every line below six must fit the measured viewport",
                editor.getHeight() >= content + editor.getCompoundPaddingTop() + editor.getCompoundPaddingBottom());
            assertEquals("A short full-height draft must not be scrolled", 0, editor.getScrollY());
            editor.setText("");
            layout(editor, 240);
        }
    }
}
