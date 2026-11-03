// 文件作用：Android 设备集成测试；ImageFilterInstrumentedTest 负责覆盖对应界面或服务的设备行为，本文件给出其验证。
// 运行边界：依赖 debug 专属 AIAssistantTestActivity，不能对 Release 目标执行本类。
// 覆盖场景：agentTransformsWorkspaceImageWithoutChangingSource；纯 native 用例在独立测试类。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import androidx.test.core.app.ActivityScenario;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import java.io.File;
import java.io.FileOutputStream;
import java.util.concurrent.atomic.AtomicReference;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;

@RunWith(AndroidJUnit4.class)
public final class ImageFilterInstrumentedTest {
    @Test
    public void agentTransformsWorkspaceImageWithoutChangingSource() throws Exception {
        try (ActivityScenario<AIAssistantTestActivity> scenario =
                 ActivityScenario.launch(AIAssistantTestActivity.class)) {
            AtomicReference<AIAssistantController> controller = new AtomicReference<>();
            scenario.onActivity(activity -> controller.set(activity.panel.controller));
            long deadline = System.currentTimeMillis() + 15_000;
            while (!controller.get().state.ready && System.currentTimeMillis() < deadline)
                Thread.sleep(50);
            assertTrue(controller.get().state.ready);
            File sourceFile = controller.get().workspaceFile("image-filter-original.png");
            assertNotNull(sourceFile);
            Bitmap source = Bitmap.createBitmap(2, 1, Bitmap.Config.ARGB_8888);
            source.setPixel(0, 0, Color.RED);
            source.setPixel(1, 0, Color.GREEN);
            try (FileOutputStream output = new FileOutputStream(sourceFile)) {
                assertTrue(source.compress(Bitmap.CompressFormat.PNG, 100, output));
            }
            AIAssistantController.ImportedFile edited = controller.get().transformImageForHost(
                new JSONObject().put("path", sourceFile.getName())
                    .put("operation", "flip_horizontal"));
            File editedFile = controller.get().workspaceImageForHost(edited.relativePath);
            Bitmap transformed = BitmapFactory.decodeFile(editedFile.getPath());
            Bitmap unchanged = BitmapFactory.decodeFile(sourceFile.getPath());
            assertNotNull(transformed);
            assertNotNull(unchanged);
            assertEquals(Color.GREEN, transformed.getPixel(0, 0));
            assertEquals(Color.RED, transformed.getPixel(1, 0));
            assertEquals(Color.RED, unchanged.getPixel(0, 0));
            assertEquals(Color.GREEN, unchanged.getPixel(1, 0));
            transformed.recycle();
            unchanged.recycle();
            source.recycle();
        }
    }
}
