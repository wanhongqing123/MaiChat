// Actual Android PdfRenderer pixel contract; does not start MaiChat or read any user document.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.pdf.PdfDocument;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import java.io.File;
import java.io.FileOutputStream;
import java.util.UUID;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class DocumentThumbnailInstrumentedTest {
    @Test public void pdfThumbnailIsTheActualFirstPageAndNativeResourcesCanBeReopened() throws Exception {
        Context context = InstrumentationRegistry.getInstrumentation().getTargetContext();
        File file = new File(context.getCacheDir(), "owned-document-" + UUID.randomUUID() + ".pdf");
        try {
            PdfDocument document = new PdfDocument();
            try {
                for (int index = 0; index < 2; index++) {
                    PdfDocument.Page page = document.startPage(new PdfDocument.PageInfo.Builder(600, 800, index + 1).create());
                    page.getCanvas().drawColor(index == 0 ? Color.BLUE : Color.RED);
                    document.finishPage(page);
                }
                try (FileOutputStream output = new FileOutputStream(file)) { document.writeTo(output); }
            } finally { document.close(); }
            for (int attempt = 0; attempt < 3; attempt++) {
                Bitmap bitmap = PdfPreviewDialog.thumbnail(file, context.getCacheDir(), 320, 180, context.getCacheDir());
                assertEquals(320, bitmap.getWidth());
                assertEquals(180, bitmap.getHeight());
                assertEquals("The second page is red; page zero must be blue", Color.BLUE, bitmap.getPixel(160, 90));
                bitmap.recycle();
            }
        } finally { file.delete(); }
    }
}
