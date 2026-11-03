// Cloud-drive directory navigation and request scoping are shared by the visible page.
package com.kongshang.maichat;

import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import static org.junit.Assert.*;

public class MaiCloudDriveDirectoryTest {
    private MaiCloudDriveDirectory directory() throws Exception {
        MaiCloudDriveDirectory directory = new MaiCloudDriveDirectory();
        directory.activate("account-a");
        directory.updateFolders(new JSONArray()
            .put(new JSONObject().put("id", "trip").put("name", "Trip")
                .put("parent_id", JSONObject.NULL))
            .put(new JSONObject().put("id", "day").put("name", "Day one")
                .put("parent_id", "trip"))
            .put(new JSONObject().put("id", "work").put("name", "Work")
                .put("parent_id", JSONObject.NULL)));
        return directory;
    }

    @Test public void rootRequestExplicitlyExcludesFilesInsideFolders() throws Exception {
        MaiCloudDriveDirectory directory = directory();
        assertEquals("", directory.payload("").getString("folder_id"));
        assertEquals(2, directory.children().size());
        assertTrue(directory.accepts(new JSONObject().put("folder_id", JSONObject.NULL)));
        assertFalse(directory.accepts(new JSONObject().put("folder_id", "trip")));
    }

    @Test public void nestedRequestAndBackFollowTheActualParent() throws Exception {
        MaiCloudDriveDirectory directory = directory();
        assertTrue(directory.browse("trip"));
        assertEquals("day", directory.children().get(0).getString("id"));
        assertTrue(directory.browse("day"));
        assertEquals("day", directory.payload("older").getString("folder_id"));
        assertEquals("older", directory.payload("older").getString("before"));
        assertTrue(directory.back());
        assertEquals("trip", directory.currentId());
        assertTrue(directory.back());
        assertEquals("", directory.currentId());
        assertFalse(directory.back());
    }

    @Test public void sameAccountKeepsFolderAndAccountChangeClearsIt() throws Exception {
        MaiCloudDriveDirectory directory = directory();
        directory.browse("day");
        directory.activate("account-a");
        assertEquals("day", directory.currentId());
        directory.activate("account-b");
        assertEquals("", directory.currentId());
        assertTrue(directory.children().isEmpty());
    }

    @Test public void missingFolderRetainsContextUntilUserReturns() throws Exception {
        MaiCloudDriveDirectory directory = directory();
        directory.browse("day");
        directory.updateFolders(new JSONArray()
            .put(new JSONObject().put("id", "trip").put("name", "Trip")));
        assertFalse(directory.currentExists());
        assertEquals("day", directory.currentId());
        assertEquals("Day one", directory.currentName());
        assertTrue(directory.back());
        assertEquals("trip", directory.currentId());
    }

    @Test public void invalidTargetDoesNotDiscardCurrentFolder() throws Exception {
        MaiCloudDriveDirectory directory = directory();
        directory.browse("trip");
        assertFalse(directory.browse("unknown"));
        assertEquals("trip", directory.currentId());
    }

    @Test public void cyclicParentsNeverTrapBackNavigation() throws Exception {
        MaiCloudDriveDirectory directory = directory();
        directory.updateFolders(new JSONArray()
            .put(new JSONObject().put("id", "trip").put("name", "Trip").put("parent_id", "day"))
            .put(new JSONObject().put("id", "day").put("name", "Day").put("parent_id", "trip")));
        directory.browse("trip");
        assertTrue(directory.back());
        assertEquals("", directory.currentId());
    }
}
