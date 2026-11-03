// Account-scoped directory selection for the Android cloud-drive page.
package com.kongshang.maichat;

import java.util.ArrayList;
import java.util.HashSet;
import org.json.JSONException;
import org.json.JSONArray;
import org.json.JSONObject;

final class MaiCloudDriveDirectory {
    private String account = "", current = "";
    private final ArrayList<JSONObject> folders = new ArrayList<>();
    private final ArrayList<JSONObject> path = new ArrayList<>();

    void activate(String account) {
        if (this.account.equals(account)) return;
        this.account = account;
        current = "";
        folders.clear();
        path.clear();
    }

    void updateFolders(JSONArray listed) {
        folders.clear();
        if (listed != null) for (int i = 0; i < listed.length(); i++) {
            JSONObject folder = listed.optJSONObject(i);
            if (folder != null && !folder.optString("id").isEmpty()) folders.add(folder);
        }
        // A folder deleted on another device keeps its last known path until
        // the user returns; a failed refresh must never silently show root files.
        if (currentExists()) rebuildPath();
    }

    String currentId() { return current; }

    String currentName() {
        JSONObject folder = find(current);
        if (folder == null && !path.isEmpty()) folder = path.get(path.size() - 1);
        return folder == null ? "我的云盘" : folder.optString("name", "文件夹");
    }

    boolean currentExists() { return current.isEmpty() || find(current) != null; }

    ArrayList<JSONObject> children() {
        ArrayList<JSONObject> children = new ArrayList<>();
        for (JSONObject folder : folders)
            if (current.equals(folder.optString("parent_id", ""))) children.add(folder);
        return children;
    }

    ArrayList<JSONObject> folders() { return new ArrayList<>(folders); }

    boolean browse(String id) {
        if (id == null) id = "";
        if (!id.isEmpty() && find(id) == null) return false;
        current = id;
        rebuildPath();
        return true;
    }

    boolean back() {
        if (current.isEmpty()) return false;
        String parent = "";
        for (int i = path.size() - 2; i >= 0; i--) {
            String id = path.get(i).optString("id");
            if (find(id) != null) { parent = id; break; }
        }
        browse(parent);
        return true;
    }

    boolean accepts(JSONObject media) {
        return current.equals(media.optString("folder_id", ""));
    }

    JSONObject payload(String cursor) {
        JSONObject payload = new JSONObject();
        try {
            // Omitting folder_id means all folders in the service API. Root
            // is explicitly an empty string, so nested files never leak here.
            payload.put("folder_id", current);
            if (cursor != null && !cursor.isEmpty()) payload.put("before", cursor);
        } catch (JSONException impossible) { throw new IllegalStateException(impossible); }
        return payload;
    }

    private JSONObject find(String id) {
        for (JSONObject folder : folders) if (id.equals(folder.optString("id"))) return folder;
        return null;
    }

    private void rebuildPath() {
        path.clear();
        HashSet<String> visited = new HashSet<>();
        String id = current;
        while (!id.isEmpty()) {
            // Corrupt parent chains fall back to a root return instead of
            // creating an endless back-navigation loop.
            if (!visited.add(id)) { path.clear(); return; }
            JSONObject folder = find(id);
            if (folder == null) return;
            path.add(0, folder);
            id = folder.optString("parent_id", "");
        }
    }
}
