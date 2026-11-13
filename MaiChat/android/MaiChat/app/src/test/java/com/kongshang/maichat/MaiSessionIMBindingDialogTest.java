// 文件作用：只用普通 Gateway 替身验证绑定选择、取消、解除、失败恢复和草稿隔离。
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.app.Activity;
import android.app.AlertDialog;
import android.view.View;
import android.widget.EditText;
import android.widget.RadioGroup;
import android.widget.TextView;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class MaiSessionIMBindingDialogTest {
    @Test public void dynamicSessionChangesPreserveSelectionAndDisableADeletedRoot() {
        Activity activity = activity(); Gateway gateway = new Gateway();
        AlertDialog dialog = MaiSessionIMBindingDialog.show(activity, gateway, () -> true, () -> {});
        assertNotNull("The ordinary picker must subscribe to Session changes", gateway.observer);
        find(dialog, "session-im-binding-option:root-two").performClick();
        gateway.titleTwo = "Renamed"; gateway.publish();
        assertTrue(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        assertTrue(((android.widget.RadioButton) find(dialog, "session-im-binding-option:root-two")).isChecked());
        assertTrue(((TextView) find(dialog, "session-im-binding-option:root-two")).getText().toString().contains("Renamed"));
        gateway.includeTwo = false; gateway.publish();
        assertNull(find(dialog, "session-im-binding-option:root-two"));
        assertFalse(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        dialog.dismiss();
    }

    @Test public void changedControllerOrWorkspaceClearsSelectionUntilExplicitlyReselected() {
        Activity activity = activity(); Gateway gateway = new Gateway();
        AlertDialog dialog = MaiSessionIMBindingDialog.show(activity, gateway, () -> true, () -> {});
        assertNotNull(gateway.observer);
        find(dialog, "session-im-binding-option:root-two").performClick();
        gateway.controller = "6"; gateway.publish();
        assertFalse(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        find(dialog, "session-im-binding-option:root-two").performClick();
        gateway.workspaceTwo = "/new-workspace"; gateway.publish();
        assertFalse(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        find(dialog, "session-im-binding-option:root-two").performClick();
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        assertEquals("6", gateway.expected.optJSONObject("scope").optString("controller_generation"));
        assertEquals("/new-workspace", gateway.expected.optJSONArray("sessions").optJSONObject(1).optString("workspace_key"));
    }
    @Test public void cancelDoesNotBindUnbindOrModifyTheOrdinaryComposerDraft() {
        Activity activity = activity(); EditText draft = new EditText(activity); draft.setText("未发送的草稿"); activity.setContentView(draft);
        Gateway gateway = new Gateway();
        AlertDialog dialog = MaiSessionIMBindingDialog.show(activity, gateway, () -> true, () -> fail("Cancel has no mutation"));
        assertFalse(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        dialog.getButton(AlertDialog.BUTTON_NEGATIVE).performClick();
        org.robolectric.Shadows.shadowOf(android.os.Looper.getMainLooper()).idle();
        assertFalse(dialog.isShowing()); assertEquals(0, gateway.binds); assertEquals(0, gateway.unbinds);
        assertEquals("未发送的草稿", draft.getText().toString());
    }

    @Test public void selectingAndConfirmingUsesTheExactLocalRootSession() {
        Activity activity = activity(); Gateway gateway = new Gateway(); AtomicInteger changed = new AtomicInteger();
        AlertDialog dialog = MaiSessionIMBindingDialog.show(activity, gateway, () -> true, changed::incrementAndGet);
        View option = find(dialog, "session-im-binding-option:root-two"); option.performClick();
        assertTrue(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        assertEquals(0, gateway.binds);
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        assertEquals(1, gateway.binds); assertEquals("root-two", gateway.selected); assertEquals(1, changed.get());
        assertFalse(dialog.isShowing());
    }

    @Test public void existingBindingShowsItsTitleAndUnbindUsesTheSameOrdinaryGateway() {
        Activity activity = activity(); Gateway gateway = new Gateway(); gateway.bound = "root-one";
        AtomicInteger changed = new AtomicInteger();
        AlertDialog dialog = MaiSessionIMBindingDialog.show(activity, gateway, () -> true, changed::incrementAndGet);
        assertTrue(((TextView) find(dialog, "session-im-binding-status")).getText().toString().contains("One"));
        assertEquals(View.VISIBLE, dialog.getButton(AlertDialog.BUTTON_NEUTRAL).getVisibility());
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).performClick();
        assertEquals(1, gateway.unbinds); assertEquals(0, gateway.binds); assertEquals(1, changed.get());
        assertFalse(dialog.isShowing());
    }

    @Test public void loadingFailureAndRetryKeepTheSameDialogAndNeverMutateBindings() {
        Activity activity = activity(); Gateway gateway = new Gateway(); gateway.deferLoad = true;
        AlertDialog dialog = MaiSessionIMBindingDialog.show(activity, gateway, () -> true, () -> fail("No mutation"));
        View body = find(dialog, "session-im-binding-dialog");
        assertFalse(dialog.getButton(AlertDialog.BUTTON_POSITIVE).isEnabled());
        gateway.loading.accept(null, "读取暂时失败");
        assertTrue(dialog.isShowing()); assertSame(body, find(dialog, "session-im-binding-dialog"));
        assertEquals("读取暂时失败", ((TextView) find(dialog, "session-im-binding-status")).getText().toString());
        gateway.deferLoad = false; find(dialog, "session-im-binding-retry").performClick();
        assertEquals(2, ((RadioGroup) find(dialog, "session-im-binding-options")).getChildCount());
        assertEquals(0, gateway.binds); assertEquals(0, gateway.unbinds);
        dialog.dismiss();
    }

    @Test public void accountOrPeerChangeDismissesInsteadOfSubmittingToAnotherConversation() {
        Activity activity = activity(); Gateway gateway = new Gateway(); AtomicBoolean current = new AtomicBoolean(true);
        AlertDialog dialog = MaiSessionIMBindingDialog.show(activity, gateway, current::get, () -> fail("Stale dialog"));
        find(dialog, "session-im-binding-option:root-two").performClick(); current.set(false);
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).performClick();
        assertFalse(dialog.isShowing()); assertEquals(0, gateway.binds); assertEquals(0, gateway.unbinds);
    }

    private static Activity activity() { return Robolectric.buildActivity(Activity.class).setup().get(); }
    private static View find(AlertDialog dialog, String tag) { return dialog.getWindow().getDecorView().findViewWithTag(tag); }
    private static final class Gateway implements MaiSessionIMBindingService.Gateway {
        String bound = "", selected;
        int binds, unbinds;
        boolean deferLoad;
        boolean includeTwo = true;
        String titleTwo = "Two", workspaceTwo = "/workspace", controller = "5";
        Consumer<JSONObject> observer;
        JSONObject expected;
        BiConsumer<JSONObject, String> loading;
        @Override public String peerId() { return "peer"; }
        @Override public void load(BiConsumer<JSONObject, String> completion) {
            if (deferLoad) loading = completion; else completion.accept(projection(), null);
        }
        @Override public void bind(String id, BiConsumer<JSONObject, String> completion) {
            binds++; selected = id; bound = id; completion.accept(projection(), null);
        }
        @Override public void bind(String id, JSONObject displayed, BiConsumer<JSONObject, String> completion) {
            expected = MaiSessionIMBindingService.copy(displayed); bind(id, completion);
        }
        @Override public AutoCloseable subscribe(Consumer<JSONObject> listener) {
            observer = listener; return () -> { if (observer == listener) observer = null; };
        }
        void publish() { if (observer != null) observer.accept(projection()); }
        @Override public void unbind(BiConsumer<JSONObject, String> completion) {
            unbinds++; bound = ""; completion.accept(projection(), null);
        }
        JSONObject projection() {
            JSONObject value = new JSONObject(); put(value, "ok", true); put(value, "bridge_id", "bridge");
            JSONObject scope = new JSONObject(); put(scope, "sdk_app_id", "123"); put(scope, "owner_id", "owner");
            put(scope, "auth_generation", "4"); put(scope, "controller_generation", controller); put(value, "scope", scope);
            JSONArray sessions = new JSONArray();
            for (String[] pair : new String[][]{{"root-one", "One"}, {"root-two", "Two"}}) {
                if (pair[0].equals("root-two") && !includeTwo) continue;
                JSONObject row = new JSONObject(); put(row, "id", pair[0]); put(row, "title", pair[0].equals("root-two") ? titleTwo : pair[1]);
                put(row, "workspace_key", pair[0].equals("root-two") ? workspaceTwo : "/workspace"); sessions.put(row);
            }
            put(value, "sessions", sessions); JSONArray bindings = new JSONArray();
            if (!bound.isEmpty()) {
                JSONObject binding = new JSONObject(); put(binding, "peer_id", "peer"); put(binding, "session_id", bound);
                put(binding, "session_title", bound.equals("root-one") ? "One" : "Two"); bindings.put(binding);
            }
            put(value, "bindings", bindings); return value;
        }
        private static void put(JSONObject target, String key, Object value) { MaiSessionIMBindingService.put(target, key, value); }
    }
}
