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
    @Test public void mainModelMenuOffersExactCloudModelsAndKeepsEffortSeparate() {
        AIAssistantActionMenu menu = menu();
        click(menu, "主模型");
        for (String model : new String[]{"glm-5.3", "glm-5.3-flash", "deepseek-flash", "deepseek-v4-pro"})
            assertNotNull("Missing main-model choice: " + model, option(menu, model));
        assertNotNull(option(menu, "本地模型"));
        assertNull(option(menu, "简单"));
        click(menu, "思考强度");
        assertNull(option(menu, "glm-5.3"));
        assertNotNull(option(menu, "简单"));
    }

    private final List<String> efforts = new ArrayList<>(), policies = new ArrayList<>();
    private final List<String> modelSelections = new ArrayList<>();
    private int clears;

    private AIAssistantActionMenu menu() { return menu(false); }
    private AIAssistantActionMenu menu(boolean remote) {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        FrameLayout host = new FrameLayout(activity);
        activity.setContentView(host);
        AIAssistantActionMenu menu = new AIAssistantActionMenu(activity,
            new AIAssistantActionMenu.Listener() {
                @Override public void selectModel(String model) { modelSelections.add(model); }
                @Override public void selectEffort(String effort) { efforts.add(effort); }
                @Override public void selectPolicy(String policy) { policies.add(policy); }
                @Override public void clearConversation() { clears++; }
            }, () -> { }, remote);
        host.addView(menu);
        menu.update("deepseek-flash", false, "high", "on-request", true, true, true);
        return menu;
    }

    @Test public void remoteModelMenuKeepsCloudAndEffortButNeverOffersLocalPathsOrAccountPolicy() {
        AIAssistantActionMenu menu = menu(true);
        click(menu, "主模型");
        for (String model : new String[]{"glm-5.3", "glm-5.3-flash", "deepseek-flash", "deepseek-v4-pro"})
            assertNotNull(option(menu, model));
        assertNull(option(menu, "本地模型"));
        assertNull(heading(menu, "操作权限"));
        assertNull(heading(menu, "清空当前对话"));
        option(menu, "glm-5.3").performClick();
        assertEquals(List.of("glm-5.3"), modelSelections);
        menu.update("deepseek-flash", true, "high", "on-request", false, false, false);
        assertTrue(option(menu, "deepseek-flash").isSelected());
        assertFalse(option(menu, "glm-5.3").isSelected());
        assertFalse(option(menu, "glm-5.3").isEnabled());
        menu.update("glm-5.3", false, "max", "on-request", true, true, false);
        assertTrue(option(menu, "glm-5.3").isSelected());
        click(menu, "思考强度");
        assertNull(option(menu, "glm-5.3"));
        assertNotNull(option(menu, "深度"));
    }

    @Test public void primaryModelIsTheFirstSectionAndSharesTheExistingInlineMenu() {
        AIAssistantActionMenu menu = menu();
        View primary = heading(menu, "主模型");
        assertNotNull("The first action must be the real primary-model source", primary);
        assertSame(menu.getChildAt(0), primary);
        click(menu, "主模型");
        assertNotNull(option(menu, "deepseek-flash"));
        assertNotNull(option(menu, "本地模型"));
        assertTrue(option(menu, "deepseek-flash").isSelected());
        option(menu, "deepseek-flash").performClick();
        assertTrue("Selecting the current source must be a no-op", modelSelections.isEmpty());
        option(menu, "本地模型").performClick();
        assertEquals(List.of(AIAssistantSessionModels.LOCAL), modelSelections);
        assertNotNull("Choosing a source must keep its options in this menu", option(menu, "deepseek-flash"));
        click(menu, "思考强度");
        assertNull(option(menu, "本地模型"));
        assertNotNull(option(menu, "简单"));
    }

    @Test public void preparingKeepsTheCommittedSourceAndFailedRestoreStillAllowsRecovery() {
        AIAssistantActionMenu menu = menu();
        click(menu, "主模型");
        menu.update("deepseek-flash", true, "high", "on-request", false, false, false);
        assertTrue(option(menu, "deepseek-flash").isSelected());
        assertFalse(option(menu, "本地模型").isSelected());
        assertFalse(option(menu, "本地模型").isEnabled());
        option(menu, "本地模型").performClick();
        assertTrue(modelSelections.isEmpty());

        menu.update(AIAssistantSessionModels.LOCAL, false, "high", "on-request", false, true, false);
        assertTrue(option(menu, "本地模型").isSelected());
        assertTrue(heading(menu, "主模型").isEnabled());
        assertFalse(heading(menu, "思考强度").isEnabled());
        option(menu, "deepseek-flash").performClick();
        assertEquals(List.of("deepseek-flash"), modelSelections);
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
        menu.update("deepseek-flash", false, "max", "on-request", true, true, true);
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
        menu.update("deepseek-flash", false, "high", "on-request", false, false, false);
        View choice = option(menu, "完全访问");
        assertNotNull(choice);
        assertFalse(choice.isEnabled());
        choice.performClick();
        assertTrue(policies.isEmpty());
        assertFalse(heading(menu, "思考强度").isEnabled());
        click(menu, "清空当前对话");
        assertEquals(0, clears);
        menu.update("deepseek-flash", false, "high", "unless-trusted", true, true, true);
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
