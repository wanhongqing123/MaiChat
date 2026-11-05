// Verifies the bundled code face through Android's actual text engine, not a host font approximation.
package com.kongshang.maichat;

import android.content.Context;
import android.graphics.Typeface;
import android.text.TextPaint;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class MarkdownCodeFontInstrumentedTest {
    @Test public void codeFaceUsesOneCellAsciiAndBoxDrawingAndTwoCellCjk() {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        Typeface face = MarkdownRenderer.codeTypeface(context);
        assertNotNull(face);
        TextPaint paint = new TextPaint(TextPaint.ANTI_ALIAS_FLAG);
        paint.setTypeface(face);
        paint.setTextSize(20f);
        float cell = paint.measureText("M");
        assertTrue(cell > 0f);
        for (String glyph : new String[] {" ", "-", "|", "┌", "─", "│", "┐", "┴", "┘", "▼", "►", "◄"}) {
            assertTrue("Missing bundled code glyph: " + glyph, paint.hasGlyph(glyph));
            assertEquals("Code advance for " + glyph, cell, paint.measureText(glyph), 0.25f);
        }
        assertTrue(paint.hasGlyph("中"));
        assertEquals(2f * cell, paint.measureText("中"), 0.25f);
        assertEquals(4f * cell, paint.measureText("路径"), 0.25f);
    }
}
