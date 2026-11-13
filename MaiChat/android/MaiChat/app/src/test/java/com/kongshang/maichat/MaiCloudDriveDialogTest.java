// Cloud-drive header visibility uses the actual component and directory state without network or device data.
package com.kongshang.maichat;

import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.util.ReflectionHelpers;

import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 29)
public final class MaiCloudDriveDialogTest {
    @Test public void embeddedRootHeaderHasNoAddActionAndKeepsItsTitle() throws Exception {
        Fixture fixture = fixture(true, false);
        assertFalse(visible(byDescription(fixture.bar, "云盘更多操作")));
        assertTrue(hasText(fixture.bar, "我的云盘"));
        assertFalse(visible(byDescription(fixture.bar, "关闭云盘")));
    }

    @Test public void nestedFolderHasNoManualAddActionAndKeepsBackNavigation() throws Exception {
        Fixture fixture = fixture(true, true);
        View add = byDescription(fixture.bar, "云盘更多操作");
        assertFalse(visible(add));
        assertTrue(visible(byDescription(fixture.bar, "返回上级文件夹")));
    }

    @Test public void returningToRootCannotRestoreAManualAddAction() throws Exception {
        Fixture fixture = fixture(true, true);
        View add = byDescription(fixture.bar, "云盘更多操作");
        assertFalse(visible(add));
        assertTrue(fixture.directory.back());
        ReflectionHelpers.callInstanceMethod(fixture.dialog, "updateNavigation");
        assertFalse(visible(add));
        assertFalse(visible(byDescription(fixture.bar, "关闭云盘")));
    }

    @Test public void modalRootStillHasCloseWithoutAnAddAction() throws Exception {
        Fixture fixture = fixture(false, false);
        assertTrue(visible(byDescription(fixture.bar, "关闭云盘")));
        assertFalse(visible(byDescription(fixture.bar, "云盘更多操作")));
    }

    private static Fixture fixture(boolean embedded, boolean nested) throws Exception {
        // Only attach the normal Activity context; its application/network lifecycle is not started.
        MainActivity activity = Robolectric.buildActivity(MainActivity.class).get();
        MaiCloudDriveDirectory directory = new MaiCloudDriveDirectory();
        directory.activate("owned-cloud-header-test");
        directory.updateFolders(new JSONArray().put(new JSONObject()
            .put("id", "owned-folder").put("name", "Owned folder")
            .put("parent_id", JSONObject.NULL)));
        if (nested) assertTrue(directory.browse("owned-folder"));
        MaiCloudDriveDialog dialog = new MaiCloudDriveDialog(activity, directory);
        ReflectionHelpers.setField(dialog, "embedded", embedded);
        View bar = ReflectionHelpers.callInstanceMethod(dialog, "navigationBar");
        return new Fixture(dialog, directory, bar);
    }

    private static boolean visible(View view) {
        return view != null && view.getVisibility() == View.VISIBLE;
    }

    private static View byDescription(View view, String description) {
        if (description.contentEquals(view.getContentDescription() == null ? "" : view.getContentDescription()))
            return view;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++) {
                View found = byDescription(group.getChildAt(index), description);
                if (found != null) return found;
            }
        }
        return null;
    }

    private static boolean hasText(View view, String text) {
        if (view instanceof TextView && text.contentEquals(((TextView) view).getText())) return true;
        if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int index = 0; index < group.getChildCount(); index++)
                if (hasText(group.getChildAt(index), text)) return true;
        }
        return false;
    }

    private static final class Fixture {
        final MaiCloudDriveDialog dialog;
        final MaiCloudDriveDirectory directory;
        final View bar;
        Fixture(MaiCloudDriveDialog dialog, MaiCloudDriveDirectory directory, View bar) {
            this.dialog = dialog;
            this.directory = directory;
            this.bar = bar;
        }
    }
}
