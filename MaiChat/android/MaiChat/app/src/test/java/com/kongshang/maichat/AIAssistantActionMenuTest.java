// Real View interactions for the creation action popup; no Agent, network or account is started.
package com.kongshang.maichat;

import static org.junit.Assert.*;

import android.app.Activity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import java.util.ArrayList;
import java.util.List;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.shadows.ShadowAlertDialog;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
public final class AIAssistantActionMenuTest {
    private final List<String> efforts = new ArrayList<>(), policies = new ArrayList<>();
    private final List<Boolean> modelSelections = new ArrayList<>();
    private int clears;

    private AIAssistantActionMenu menu() {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        FrameLayout host = new FrameLayout(activity);
        activity.setContentView(host);
        AIAssistantActionMenu menu = new AIAssistantActionMenu(activity,
            new AIAssistantActionMenu.Listener() {
                @Override public void selectModelSource(boolean local) { modelSelections.add(local); }
                @Override public void selectEffort(String effort) { efforts.add(effort); }
                @Override public void selectPolicy(String policy) { policies.add(policy); }
                @Override public void clearConversation() { clears++; }
            }, () -> { });
        host.addView(menu);
        menu.update(false, false, "high", "on-request", true, true, true);
        return menu;
    }

    @Test public void primaryModelIsTheFirstSectionAndSharesTheExistingInlineMenu() {
        AIAssistantActionMenu menu = menu();
        View primary = heading(menu, "主模型");
        assertNotNull("The first action must be the real primary-model source", primary);
        assertSame(menu.getChildAt(0), primary);
        click(menu, "主模型");
        assertNotNull(option(menu, "云端模型"));
        assertNotNull(option(menu, "本地模型"));
        assertTrue(option(menu, "云端模型").isSelected());
        option(menu, "云端模型").performClick();
        assertTrue("Selecting the current source must be a no-op", modelSelections.isEmpty());
        option(menu, "本地模型").performClick();
        assertEquals(List.of(true), modelSelections);
        assertNotNull("Choosing a source must keep its options in this menu", option(menu, "云端模型"));
        click(menu, "思考强度");
        assertNull(option(menu, "本地模型"));
        assertNotNull(option(menu, "简单"));
    }

    @Test public void preparingKeepsTheCommittedSourceAndFailedRestoreStillAllowsRecovery() {
        AIAssistantActionMenu menu = menu();
        click(menu, "主模型");
        menu.update(false, true, "high", "on-request", false, false, false);
        assertTrue(option(menu, "云端模型").isSelected());
        assertFalse(option(menu, "本地模型").isSelected());
        assertFalse(option(menu, "本地模型").isEnabled());
        option(menu, "本地模型").performClick();
        assertTrue(modelSelections.isEmpty());

        menu.update(true, false, "high", "on-request", false, true, false);
        assertTrue(option(menu, "本地模型").isSelected());
        assertTrue(heading(menu, "主模型").isEnabled());
        assertFalse(heading(menu, "思考强度").isEnabled());
        option(menu, "云端模型").performClick();
        assertEquals(List.of(false), modelSelections);
        assertNotNull(option(menu, "本地模型"));
    }

    @Test public void headingsExpandInsideOneContentAndOnlyOneSectionRemainsOpen() {
        AIAssistantActionMenu menu = menu();
        Object parent = menu.getParent();
        click(menu, "思考强度");
        assertNotNull("Effort options must appear inside the original popup", option(menu, "简单"));
        assertNotNull(option(menu, "标准"));
        assertNotNull(option(menu, "深度"));
        assertSame(parent, menu.getParent());
        assertNull("Expanding must not create a centered system dialog", ShadowAlertDialog.getLatestAlertDialog());
        click(menu, "操作权限");
        assertNull(option(menu, "简单"));
        assertNotNull(option(menu, "请求批准"));
        assertNotNull(option(menu, "帮我批准"));
        assertNotNull(option(menu, "完全访问"));
        click(menu, "操作权限");
        assertNull(option(menu, "完全访问"));
    }

    @Test public void choosingAnOptionKeepsTheSectionOpenAndSameValueDoesNotSaveAgain() {
        AIAssistantActionMenu menu = menu();
        click(menu, "思考强度");
        assertTrue(option(menu, "标准").isSelected());
        option(menu, "标准").performClick();
        assertTrue(efforts.isEmpty());
        option(menu, "深度").performClick();
        assertEquals(List.of("max"), efforts);
        assertNotNull("Selection must keep this popup section open", option(menu, "简单"));
        menu.update(false, false, "max", "on-request", true, true, true);
        assertTrue(option(menu, "深度").isSelected());
        assertFalse(option(menu, "标准").isSelected());
        click(menu, "操作权限");
        assertTrue(option(menu, "请求批准").isSelected());
        option(menu, "请求批准").performClick();
        assertTrue(policies.isEmpty());
        option(menu, "完全访问").performClick();
        assertEquals(List.of("never"), policies);
        assertNotNull(option(menu, "帮我批准"));
    }

    @Test public void stateChangesDisableOpenOptionsAndDoNotApplyAStaleSelection() {
        AIAssistantActionMenu menu = menu();
        click(menu, "操作权限");
        menu.update(false, false, "high", "on-request", false, false, false);
        View choice = option(menu, "完全访问");
        assertNotNull(choice);
        assertFalse(choice.isEnabled());
        choice.performClick();
        assertTrue(policies.isEmpty());
        assertFalse(heading(menu, "思考强度").isEnabled());
        click(menu, "清空当前对话");
        assertEquals(0, clears);
        menu.update(false, false, "high", "unless-trusted", true, true, true);
        assertTrue(option(menu, "帮我批准").isSelected());
        option(menu, "请求批准").performClick();
        assertEquals(List.of("on-request"), policies);
    }

    private static void click(View root, String title) {
        View heading = heading(root, title);
        assertNotNull("Missing popup heading: " + title, heading);
        heading.performClick();
    }

    private static View heading(View root, String title) {
        if (root.getVisibility() != View.VISIBLE) return null;
        if (title.contentEquals(root.getContentDescription() == null ? "" : root.getContentDescription()))
            return root;
        if (root instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) root;
            for (int i = 0; i < group.getChildCount(); i++) {
                View found = heading(group.getChildAt(i), title);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static View option(View root, String title) {
        // Match a visible choice's accessible label, not a heading subtitle that happens
        // to contain the same current value. GONE groups remain in the stable View tree.
        View found = heading(root, title);
        return found != null && found.isClickable() ? found : null;
    }
}
