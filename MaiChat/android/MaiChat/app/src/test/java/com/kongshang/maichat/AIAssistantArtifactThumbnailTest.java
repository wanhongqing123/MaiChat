// Owned document fixtures exercise real Agent file cards without JNI, models or user files.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.graphics.Bitmap;
import android.graphics.Color;
import android.graphics.drawable.BitmapDrawable;
import android.os.Looper;
import android.view.View;
import android.view.ViewGroup;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.function.BooleanSupplier;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import org.robolectric.shadows.ShadowAlertDialog;
import org.robolectric.shadows.ShadowDialog;
import org.robolectric.util.ReflectionHelpers;

@RunWith(RobolectricTestRunner.class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = 34, qualifiers = "w400dp-h800dp-mdpi", shadows = {
    AIAssistantSessionDrawerTest.OwnedController.class,
    AIAssistantSessionDrawerTest.OwnedImageLoader.class})
public final class AIAssistantArtifactThumbnailTest {
    private Activity activity;
    private AIAssistantController controller;
    private AIAssistantPanel panel;
    private File workspace;

    @Before public void setup() throws Exception {
        // Android's data root is stable for one app process. Robolectric gives each test
        // a new data root but retains AndroidX's static authority/path strategy cache.
        // Reset that fixture-only cache before this sandbox creates its provider.
        ((java.util.Map<?, ?>) ReflectionHelpers.getStaticField(androidx.core.content.FileProvider.class,
            "sCache")).clear();
        activity = Robolectric.buildActivity(Activity.class).setup().get();
        controller = new AIAssistantController(activity, "http://owned.invalid");
        File owner = new File(activity.getCacheDir(), "owned-artifact-" + System.nanoTime());
        workspace = new File(owner, "Workspace");
        assertTrue(workspace.mkdirs());
        ReflectionHelpers.setField(controller, "root", owner);
        panel = new AIAssistantPanel(activity, controller);
    }

    @After public void close() {
        panel.setForeground(false);
        controller.close();
        activity.finish();
    }

    @Test public void markdownCardShowsRenderedFirstScreenAndClickOpensTheWholeDocument() throws Exception {
        File markdown = new File(workspace, "sample.md");
        String source = "# Owned first-screen heading\n\n**Bold content** and a list:\n\n- first\n- second\n\n"
            + "Repeated paragraph.\n\n".repeat(100) + "OWNED_DOCUMENT_END";
        Files.write(markdown.toPath(), source.getBytes(StandardCharsets.UTF_8));
        Button card = delivered(markdown, "text/markdown", "owned-markdown");
        await(() -> card.getCompoundDrawables()[1] instanceof BitmapDrawable);
        assertTrue("A real Markdown first screen must appear above the filename",
            card.getCompoundDrawables()[1] instanceof BitmapDrawable);
        Bitmap thumbnail = ((BitmapDrawable) card.getCompoundDrawables()[1]).getBitmap();
        assertTrue("The first screen must contain rendered document ink", distinctPixels(thumbnail) > 40);
        assertTrue(card.getText().toString().contains("sample.md"));
        card.performClick();
        await(() -> ShadowDialog.getLatestDialog() != null
            && findText(ShadowDialog.getLatestDialog().getWindow().getDecorView(),
                "OWNED_DOCUMENT_END") != null);
        assertNotNull("Click must open the whole Markdown, including content beyond its thumbnail",
            findText(ShadowDialog.getLatestDialog().getWindow().getDecorView(), "OWNED_DOCUMENT_END"));
        assertMarkdownShare("sample.md");
    }

    @Test public void pdfCardRetainsItsPaginatedPreviewActionWhenJvmHasNoNativePdfBackend() throws Exception {
        File pdf = new File(workspace, "sample.pdf");
        Files.write(pdf.toPath(), coloredPdf());
        Button card = delivered(pdf, "application/pdf", "owned-pdf");
        assertEquals("文件 sample.pdf", card.getContentDescription());
        card.performClick();
        assertNotNull("PDF click retains the complete paginated preview", ShadowAlertDialog.getLatestAlertDialog());
        // Real first-page pixel assertions live in DocumentThumbnailInstrumentedTest:
        // Robolectric 4.12.2 has no PdfRenderer native implementation (page count is zero).
    }

    @Test public void unchangedRebindKeepsVisibleContentWhileFileIdentityIsCheckedOffMain() throws Exception {
        File file = new File(workspace, "stable.md");
        Files.write(file.toPath(), "# Owned stable heading\n\nDocument content".getBytes(StandardCharsets.UTF_8));
        Button button = new Button(activity);
        activity.setContentView(button);
        AIAssistantArtifactThumbnail thumbnails = new AIAssistantArtifactThumbnail();
        thumbnails.bind(button, "same-owner", file.getName(), "text/markdown", file.length(), workspace,
            () -> file, () -> true, available -> { });
        await(() -> button.getCompoundDrawables()[1] instanceof BitmapDrawable);
        android.graphics.drawable.Drawable first = button.getCompoundDrawables()[1];
        assertNotNull(first);
        java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
        try {
            thumbnails.bind(button, "same-owner", file.getName(), "text/markdown", file.length(), workspace,
                () -> { release.await(); return file; }, () -> true, available -> { });
            assertSame("Streaming row rebinding must not flash the same thumbnail blank", first,
                button.getCompoundDrawables()[1]);
        } finally { release.countDown(); }
    }

    @Test public void recycledBindingAndRevokedOwnerRejectDelayedResultsWithoutChangingClick() throws Exception {
        File oldFile = new File(workspace, "old.md"), newFile = new File(workspace, "new.md");
        Files.write(oldFile.toPath(), "# Old document".getBytes(StandardCharsets.UTF_8));
        Files.write(newFile.toPath(), "# New document".getBytes(StandardCharsets.UTF_8));
        Button button = new Button(activity);
        activity.setContentView(button);
        java.util.concurrent.atomic.AtomicInteger clicks = new java.util.concurrent.atomic.AtomicInteger();
        button.setOnClickListener(ignored -> clicks.incrementAndGet());
        java.util.concurrent.ExecutorService io = java.util.concurrent.Executors.newSingleThreadExecutor();
        AIAssistantArtifactThumbnail thumbnails = new AIAssistantArtifactThumbnail(io);
        java.util.concurrent.CountDownLatch started = new java.util.concurrent.CountDownLatch(1);
        java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
        try {
            thumbnails.bind(button, "old-session", oldFile.getName(), "text/markdown", oldFile.length(), workspace,
                () -> { assertNotEquals(Looper.getMainLooper().getThread(), Thread.currentThread());
                    started.countDown(); release.await(); return oldFile; }, () -> true, available -> { });
            assertTrue(started.await(2, java.util.concurrent.TimeUnit.SECONDS));
            thumbnails.bind(button, "new-session", newFile.getName(), "text/markdown", newFile.length(), workspace,
                () -> newFile, () -> true, available -> { });
            release.countDown();
            await(() -> button.getCompoundDrawables()[1] instanceof BitmapDrawable);
            android.graphics.drawable.Drawable current = button.getCompoundDrawables()[1];
            assertNotNull(current);
            io.submit(() -> { }).get();
            Shadows.shadowOf(Looper.getMainLooper()).idle();
            assertSame("Old session completion must never replace the new card", current,
                button.getCompoundDrawables()[1]);
            java.util.concurrent.atomic.AtomicBoolean owner = new java.util.concurrent.atomic.AtomicBoolean(false);
            thumbnails.bind(button, "revoked-owner", oldFile.getName(), "text/markdown", oldFile.length(), workspace,
                () -> oldFile, owner::get, available -> fail("Revoked owner received file availability"));
            io.submit(() -> { }).get();
            ((java.util.concurrent.ExecutorService) ReflectionHelpers.getStaticField(MarkdownRenderer.class, "worker"))
                .submit(() -> { }).get();
            Shadows.shadowOf(Looper.getMainLooper()).idle();
            assertNull("A revoked owner cannot receive a thumbnail", button.getCompoundDrawables()[1]);
            button.performClick();
            assertEquals("Thumbnail cancellation must retain the original click action", 1, clicks.get());
        } finally { release.countDown(); io.shutdownNow(); }
    }

    @Test public void cacheReusesUnchangedFileButAtomicReplacementWithSameSizeAndTimeGetsFreshPixels() throws Exception {
        File file = new File(workspace, "cached.md");
        Files.write(file.toPath(), "# AAA\n\nOwned".getBytes(StandardCharsets.UTF_8));
        Button button = new Button(activity);
        activity.setContentView(button);
        java.util.concurrent.ExecutorService io = java.util.concurrent.Executors.newSingleThreadExecutor();
        AIAssistantArtifactThumbnail thumbnails = new AIAssistantArtifactThumbnail(io);
        try {
            Runnable bind = () -> thumbnails.bind(button, "cache-owner", file.getName(), "text/markdown",
                file.length(), workspace, () -> file, () -> true, available -> { });
            bind.run(); await(() -> button.getCompoundDrawables()[1] instanceof BitmapDrawable);
            Bitmap first = ((BitmapDrawable) button.getCompoundDrawables()[1]).getBitmap();
            bind.run(); io.submit(() -> { }).get(); Shadows.shadowOf(Looper.getMainLooper()).idle();
            assertSame("The bounded memory cache should reuse unchanged document pixels", first,
                ((BitmapDrawable) button.getCompoundDrawables()[1]).getBitmap());
            java.nio.file.attribute.FileTime modified = Files.getLastModifiedTime(file.toPath());
            File replacement = new File(workspace, "replacement.md");
            Files.write(replacement.toPath(), "# BBB\n\nOwned".getBytes(StandardCharsets.UTF_8));
            Files.setLastModifiedTime(replacement.toPath(), modified);
            Files.move(replacement.toPath(), file.toPath(), java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            bind.run();
            await(() -> button.getCompoundDrawables()[1] instanceof BitmapDrawable
                && ((BitmapDrawable) button.getCompoundDrawables()[1]).getBitmap() != first);
            assertNotSame("Same pathname/size/mtime must not adopt a replacement inode's old pixels", first,
                ((BitmapDrawable) button.getCompoundDrawables()[1]).getBitmap());
        } finally { io.shutdownNow(); }
    }

    @Test public void corruptOrUnsupportedThumbnailKeepsTheOriginalFileCardAndAction() throws Exception {
        File file = new File(workspace, "broken.pdf");
        Files.write(file.toPath(), "%PDF-not-a-valid-document".getBytes(StandardCharsets.US_ASCII));
        Button card = delivered(file, "application/pdf", "owned-broken");
        assertTrue(card.getText().toString().contains("broken.pdf"));
        assertTrue(card.getText().toString().contains("Owned fixture caption"));
        card.performClick();
        assertNotNull(ShadowAlertDialog.getLatestAlertDialog());
        assertTrue(card.isEnabled());
    }

    @Test public void overLimitMarkdownShowsAnExplicitErrorAndRetainsTheOriginalFileCard() throws Exception {
        File file = new File(workspace, "oversize.md");
        try (java.io.RandomAccessFile output = new java.io.RandomAccessFile(file, "rw")) {
            output.setLength(MarkdownPreviewDialog.MAX_BYTES + 1L);
        }
        Button card = delivered(file, "text/markdown", "owned-large");
        card.performClick();
        await(() -> ShadowDialog.getLatestDialog() != null
            && findText(ShadowDialog.getLatestDialog().getWindow().getDecorView(), "文件过大，无法预览") != null);
        assertNotNull(findText(ShadowDialog.getLatestDialog().getWindow().getDecorView(), "文件过大，无法预览"));
        assertTrue(card.getText().toString().contains("oversize.md"));
        assertTrue(card.getText().toString().contains("Owned fixture caption"));
        assertTrue(card.isEnabled());
        assertEquals(MarkdownPreviewDialog.MAX_BYTES + 1L, file.length());
        assertMarkdownShare("oversize.md");
    }

    @Test public void sharedPreviewCannotShareAFileOutsideItsCanonicalAllowedRoot() throws Exception {
        File outside = new File(workspace.getParentFile(), "outside.md");
        Files.write(outside.toPath(), "Owned but outside allowed workspace".getBytes(StandardCharsets.UTF_8));
        MarkdownPreviewDialog.show(activity, new File(workspace, "../outside.md"), workspace, "outside.md");
        TextView share = findText(ShadowDialog.getLatestDialog().getWindow().getDecorView(), "分享");
        assertNotNull(share);
        share.performClick();
        ((java.util.concurrent.ExecutorService) ReflectionHelpers.getStaticField(MarkdownPreviewDialog.class, "worker"))
            .submit(() -> { }).get();
        Shadows.shadowOf(Looper.getMainLooper()).idle();
        assertNull("Even a FileProvider-accessible cache file must pass this preview's root boundary",
            Shadows.shadowOf(activity).getNextStartedActivity());
        assertEquals("文件暂不可分享", org.robolectric.shadows.ShadowToast.getTextOfLatestToast());
    }

    @Test public void floodRebindingKeepsOnlyLatestPendingTaskInARealBoundedQueue() throws Exception {
        File file = new File(workspace, "flood.md");
        Files.write(file.toPath(), "# Owned flood document".getBytes(StandardCharsets.UTF_8));
        AIAssistantArtifactThumbnail thumbnails = new AIAssistantArtifactThumbnail();
        java.util.concurrent.ThreadPoolExecutor io = ReflectionHelpers.getStaticField(
            AIAssistantArtifactThumbnail.class, "worker");
        java.util.concurrent.CountDownLatch started = new java.util.concurrent.CountDownLatch(2);
        java.util.concurrent.CountDownLatch release = new java.util.concurrent.CountDownLatch(1);
        Button first = new Button(activity), second = new Button(activity), target = new Button(activity);
        try {
            for (Button blocker : new Button[]{first, second}) thumbnails.bind(blocker, "blocking", file.getName(),
                "text/markdown", file.length(), workspace,
                () -> { started.countDown(); release.await(); return file; }, () -> true, available -> { });
            assertTrue(started.await(2, java.util.concurrent.TimeUnit.SECONDS));
            for (int i = 0; i < 500; i++) thumbnails.bind(target, "latest-owner-" + i, file.getName(),
                "text/markdown", file.length(), workspace, () -> file, () -> true, available -> { });
            assertTrue("Canceled requests must leave only the latest pending work; queued=" + io.getQueue().size(),
                io.getQueue().size() <= 1);
            assertTrue("Worker count alone is not a queue bound", io.getQueue().remainingCapacity() < 64);
        } finally {
            thumbnails.clear(target); thumbnails.clear(first); thumbnails.clear(second);
            release.countDown(); io.purge();
        }
    }

    @Test public void abaRewriteCannotRenderDifferentBytesUnderTheOriginalSnapshotIdentity() throws Exception {
        File file = new File(workspace, "aba.md");
        byte[] original = "# AAA\n\nOwned snapshot".getBytes(StandardCharsets.UTF_8);
        byte[] replacement = "# BBB\n\nOwned snapshot".getBytes(StandardCharsets.UTF_8);
        Files.write(file.toPath(), original);
        Button reference = new Button(activity), target = new Button(activity);
        AIAssistantArtifactThumbnail ordinary = new AIAssistantArtifactThumbnail();
        ordinary.bind(reference, "reference", file.getName(), "text/markdown", file.length(), workspace,
            () -> file, () -> true, available -> { });
        await(() -> reference.getCompoundDrawables()[1] instanceof BitmapDrawable);
        Bitmap originalImage = ((BitmapDrawable) reference.getCompoundDrawables()[1]).getBitmap();
        java.nio.file.attribute.FileTime modified = Files.getLastModifiedTime(file.toPath());
        java.util.concurrent.ExecutorService io = java.util.concurrent.Executors.newSingleThreadExecutor();
        AIAssistantArtifactThumbnail racing = new AIAssistantArtifactThumbnail(io,
            new AIAssistantArtifactThumbnail.ReadObserver() {
                @Override public void beforeRender(File ignored) throws java.io.IOException {
                    Files.write(file.toPath(), replacement);
                }
                @Override public void afterRender(File ignored) throws java.io.IOException {
                    Files.write(file.toPath(), original);
                    Files.setLastModifiedTime(file.toPath(), modified);
                }
            });
        try {
            racing.bind(target, "race-owner", file.getName(), "text/markdown", file.length(), workspace,
                () -> file, () -> true, available -> { });
            await(() -> target.getCompoundDrawables()[1] instanceof BitmapDrawable);
            assertTrue("An immutable A snapshot cannot display the transient B document",
                ((BitmapDrawable) target.getCompoundDrawables()[1]).getBitmap().sameAs(originalImage));
            assertArrayEquals(original, Files.readAllBytes(file.toPath()));
        } finally { io.shutdownNow(); }
    }

    private void assertMarkdownShare(String filename) throws Exception {
        assertNotNull("The fixture must be addressable by the existing FileProvider",
            androidx.core.content.FileProvider.getUriForFile(activity, activity.getPackageName() + ".files",
                new File(workspace, filename).getCanonicalFile()));
        TextView share = findText(ShadowDialog.getLatestDialog().getWindow().getDecorView(), "分享");
        assertNotNull("Full/error preview must retain the original file sharing action", share);
        assertTrue("The preview's share button must invoke its ordinary action", share.performClick());
        ((java.util.concurrent.ExecutorService) ReflectionHelpers.getStaticField(MarkdownPreviewDialog.class, "worker"))
            .submit(() -> { }).get();
        Shadows.shadowOf(Looper.getMainLooper()).idle();
        Intent chooser = Shadows.shadowOf(activity).getNextStartedActivity();
        assertNotNull("Share must reach the system chooser; toast="
            + org.robolectric.shadows.ShadowToast.getTextOfLatestToast(), chooser);
        assertEquals(Intent.ACTION_CHOOSER, chooser.getAction());
        Intent send = chooser.getParcelableExtra(Intent.EXTRA_INTENT);
        assertEquals(Intent.ACTION_SEND, send.getAction());
        assertEquals("text/markdown", send.getType());
        Uri uri = send.getParcelableExtra(Intent.EXTRA_STREAM);
        assertNotNull(uri);
        assertEquals("content", uri.getScheme());
        assertEquals(activity.getPackageName() + ".files", uri.getAuthority());
        assertEquals(uri, send.getClipData().getItemAt(0).getUri());
        assertTrue((send.getFlags() & Intent.FLAG_GRANT_READ_URI_PERMISSION) != 0);
        assertEquals(0, send.getFlags() & Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
        assertTrue(uri.toString(), uri.getLastPathSegment().endsWith(filename));
    }

    private Button delivered(File file, String mime, String session) throws Exception {
        JSONObject artifact = new JSONObject().put("delivery", "current_ai_session").put("type", "file")
            .put("path", file.getName()).put("name", file.getName()).put("mime_type", mime)
            .put("bytes", file.length()).put("caption", "Owned fixture caption");
        JSONObject part = new JSONObject().put("id", "owned-file-part").put("kind", "tool")
            .put("tool", "agent_send_file").put("state", "completed").put("output", artifact.toString());
        JSONObject message = new JSONObject().put("id", "owned-message").put("role", "assistant")
            .put("parts", new JSONArray().put(part));
        JSONObject data = new JSONObject().put("sessions", new JSONArray())
            .put("devices", new JSONArray()).put("messages", new JSONArray().put(message));
        panel.onState(new AIAssistantController.State(data, null, session, "", "deepseek-flash",
            "on-request", "", "", "", 0, false, true));
        BaseAdapter adapter = ReflectionHelpers.getField(panel, "adapter");
        LinearLayout container = new LinearLayout(activity);
        container.setOrientation(LinearLayout.VERTICAL);
        View row = adapter.getView(0, null, container);
        container.addView(row);
        activity.setContentView(container);
        container.measure(View.MeasureSpec.makeMeasureSpec(400, View.MeasureSpec.EXACTLY),
            View.MeasureSpec.makeMeasureSpec(2000, View.MeasureSpec.AT_MOST));
        container.layout(0, 0, 400, container.getMeasuredHeight());
        Button card = findFileButton(container, file.getName());
        assertNotNull("The existing file delivery card must be present", card);
        return card;
    }

    private static Button findFileButton(View view, String name) {
        if (view instanceof Button && ("文件 " + name).equals(view.getContentDescription()))
            return (Button) view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                Button found = findFileButton(group.getChildAt(i), name);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static TextView findText(View view, String value) {
        if (view instanceof TextView && ((TextView) view).getText().toString().contains(value)) return (TextView) view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) {
                TextView found = findText(group.getChildAt(i), value);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static void await(BooleanSupplier done) throws Exception {
        long deadline = System.nanoTime() + 5_000_000_000L;
        while (!done.getAsBoolean() && System.nanoTime() < deadline) {
            Shadows.shadowOf(Looper.getMainLooper()).idle();
            Thread.sleep(5);
        }
    }

    private static int distinctPixels(Bitmap bitmap) {
        int background = bitmap.getPixel(0, 0), ink = 0;
        for (int y = 0; y < bitmap.getHeight(); y += 2)
            for (int x = 0; x < bitmap.getWidth(); x += 2)
                if (bitmap.getPixel(x, y) != background) ++ink;
        return ink;
    }

    static byte[] coloredPdf() {
        String[] objects = {"<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R 5 0 R] /Count 2 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Contents 4 0 R >>",
            stream("0 0 1 rg 0 0 600 800 re f\n"),
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Contents 6 0 R >>",
            stream("1 0 0 rg 0 0 600 800 re f\n")};
        StringBuilder pdf = new StringBuilder("%PDF-1.4\n");
        int[] offsets = new int[objects.length];
        for (int i = 0; i < objects.length; i++) {
            offsets[i] = pdf.length();
            pdf.append(i + 1).append(" 0 obj\n").append(objects[i]).append("\nendobj\n");
        }
        int xref = pdf.length();
        pdf.append("xref\n0 7\n0000000000 65535 f \n");
        for (int offset : offsets) pdf.append(String.format(java.util.Locale.ROOT, "%010d 00000 n \n", offset));
        pdf.append("trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n").append(xref).append("\n%%EOF\n");
        return pdf.toString().getBytes(StandardCharsets.US_ASCII);
    }

    private static String stream(String body) { return "<< /Length " + body.length() + " >>\nstream\n" + body + "endstream"; }
}
