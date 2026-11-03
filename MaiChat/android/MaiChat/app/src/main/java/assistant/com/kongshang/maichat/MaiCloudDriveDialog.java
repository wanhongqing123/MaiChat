// Private media cloud presentation; network and multipart state live in MaiCloudDriveClient.
package com.kongshang.maichat;

import android.app.AlertDialog;
import android.content.res.ColorStateList;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.net.Uri;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.PopupMenu;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Locale;
import java.util.UUID;
import java.util.function.Consumer;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONArray;
import org.json.JSONObject;

final class MaiCloudDriveDialog {
    private static final long MAX_THUMBNAIL_BYTES = 2L * 1024 * 1024;
    private static final int PAGE_COLOR = Color.rgb(242, 242, 247);
    private static final ExecutorService VOD_WORKERS = Executors.newFixedThreadPool(2,
        runnable -> new Thread(runnable, "maichat-cloud-vod"));
    private static final class CappedScrollView extends ScrollView {
        private final int maximumHeight;
        CappedScrollView(MainActivity activity, int maximumHeight) {
            super(activity);
            this.maximumHeight = maximumHeight;
        }
        @Override protected void onMeasure(int widthSpec, int heightSpec) {
            int allowed = View.MeasureSpec.getMode(heightSpec) == View.MeasureSpec.UNSPECIFIED
                ? maximumHeight : Math.min(View.MeasureSpec.getSize(heightSpec), maximumHeight);
            super.onMeasure(widthSpec,
                View.MeasureSpec.makeMeasureSpec(allowed, View.MeasureSpec.AT_MOST));
        }
    }

    private final MainActivity activity;
    private final MaiCloudDrive client;
    private final MaiCloudDrive folderClient;
    private final MaiCloudDriveDirectory directory;
    private final MaiChatAttachmentDownload.NativeTransport downloads;
    private MaiChatAttachmentDownload.Scope downloadScope = new MaiChatAttachmentDownload.Scope();
    private MaiCloudVod.Opening vodOpening;
    private final ArrayList<JSONObject> media = new ArrayList<>();
    private AlertDialog dialog;
    private boolean embedded;
    private LinearLayout page, uploads, library;
    private TextView quota, quotaNote, status;
    private View upload, back;
    private Button next;
    private ProgressBar progress, quotaProgress;
    private String cursor = "";
    private String account = "";
    private final String thumbnailInstance = UUID.randomUUID().toString();
    private File thumbnailDirectory;
    private boolean uploading, loading, folderLoading, playing, saving;
    private String listError = "";
    private int generation;
    private File requestedCheckpoint;

    MaiCloudDriveDialog(MainActivity activity) {
        this(activity, new MaiCloudDriveDirectory());
    }

    MaiCloudDriveDialog(MainActivity activity, MaiCloudDriveDirectory directory) {
        this.activity = activity;
        this.client = new MaiCloudDrive(activity);
        this.folderClient = new MaiCloudDrive(activity);
        this.directory = directory;
        this.downloads = new MaiChatAttachmentDownload.NativeTransport(client::trustPem);
    }

    boolean isShowing() {
        return embedded ? page != null && page.getParent() != null
            : dialog != null && dialog.isShowing();
    }

    boolean isEmbedded() { return embedded && isShowing(); }

    void dismiss() {
        if (!isShowing()) return;
        if (!embedded) { dialog.dismiss(); return; }
        release();
        if (page.getParent() instanceof ViewGroup)
            ((ViewGroup) page.getParent()).removeView(page);
        embedded = false;
    }

    void show() {
        embedded = false;
        showIn(null);
    }

    void showEmbedded(ViewGroup host) {
        embedded = true;
        showIn(host);
    }

    private void showIn(ViewGroup host) {
        account = MaiChatAuthClient.shared(activity).accountId();
        if (account.isEmpty()) {
            Toast.makeText(activity, "请先登录 MaiChat", Toast.LENGTH_SHORT).show();
            return;
        }
        directory.activate(account);
        downloadScope = new MaiChatAttachmentDownload.Scope();
        uploading = loading = folderLoading = playing = saving = false;
        thumbnailDirectory = new File(activity.getCacheDir(),
            "cloud-drive-thumbnails/" + MaiCloudDrive.accountKey(account));
        clearThumbnails();
        page = new LinearLayout(activity);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setBackgroundColor(PAGE_COLOR);
        page.setContentDescription("云盘页面");
        page.addView(navigationBar(), new LinearLayout.LayoutParams(-1, dp(56)));
        ScrollView scroll = embedded ? new ScrollView(activity) : new CappedScrollView(activity,
            (int) (activity.getResources().getDisplayMetrics().heightPixels * .68f));
        scroll.setFillViewport(true);
        scroll.setClipToPadding(false);
        scroll.setPadding(dp(18), dp(18), dp(18), dp(24));
        LinearLayout list = new LinearLayout(activity);
        list.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(list);
        list.addView(quotaCard(), row());
        status = label("", 13, MaiChatTheme.SECONDARY);
        status.setContentDescription("云盘状态");
        status.setPadding(dp(12), dp(10), dp(12), dp(10));
        status.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE_SOFT, 12, activity));
        status.setVisibility(View.GONE);
        list.addView(status, row());
        progress = new ProgressBar(activity, null,
            android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        progress.setVisibility(View.GONE);
        LinearLayout.LayoutParams progressParams = row();
        progressParams.topMargin = dp(8);
        list.addView(progress, progressParams);
        uploads = new LinearLayout(activity);
        uploads.setOrientation(LinearLayout.VERTICAL);
        uploads.setVisibility(View.GONE);
        list.addView(uploads, row());
        library = new LinearLayout(activity);
        library.setOrientation(LinearLayout.VERTICAL);
        list.addView(library, row());
        next = button("加载更多", () -> { if (listError.isEmpty()) loadNext(); else refresh(); });
        next.setVisibility(View.GONE);
        list.addView(next, row());
        page.addView(scroll, embedded ? new LinearLayout.LayoutParams(-1, 0, 1) : row());
        if (embedded) {
            host.addView(page, new ViewGroup.LayoutParams(-1, -1));
        } else {
            dialog = new AlertDialog.Builder(activity).setView(page).create();
            dialog.setOnDismissListener(ignored -> release());
            dialog.show();
        }
        cleanupIdlePlaybackCache();
        activity.cleanupIdleCloudDriveImages();
        Window window = embedded ? null : dialog.getWindow();
        if (window != null) {
            window.setBackgroundDrawableResource(android.R.color.transparent);
            window.setLayout(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT);
            window.setGravity(Gravity.BOTTOM);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
        }
        refresh();
    }

    private View navigationBar() {
        FrameLayout bar = new FrameLayout(activity);
        bar.setBackgroundColor(Color.WHITE);
        TextView title = label("我的云盘", 17, MaiChatTheme.TEXT);
        title.setTypeface(MaiChatTypography.semibold());
        title.setGravity(Gravity.CENTER);
        FrameLayout.LayoutParams titleParams = new FrameLayout.LayoutParams(-1, -1);
        titleParams.leftMargin = dp(64);
        titleParams.rightMargin = dp(64);
        bar.addView(title, titleParams);
        back = symbolButton(MaiChatSymbolView.Symbol.CHEVRON_LEFT, "返回上级文件夹",
            () -> { if (!navigateBack() && !embedded) dismiss(); });
        FrameLayout.LayoutParams backParams = new FrameLayout.LayoutParams(dp(44), dp(44),
            Gravity.START | Gravity.CENTER_VERTICAL);
        backParams.leftMargin = dp(10);
        bar.addView(back, backParams);
        upload = symbolButton(MaiChatSymbolView.Symbol.PLUS, "云盘更多操作", this::showAddMenu);
        upload.setBackground(MaiChatTheme.bordered(Color.WHITE,
            Color.rgb(232, 232, 236), 22, activity));
        FrameLayout.LayoutParams addParams = new FrameLayout.LayoutParams(dp(44), dp(44),
            Gravity.END | Gravity.CENTER_VERTICAL);
        addParams.rightMargin = dp(18);
        bar.addView(upload, addParams);
        updateNavigation();
        return bar;
    }

    private View quotaCard() {
        LinearLayout card = card();
        card.setPadding(dp(17), dp(17), dp(17), dp(17));
        card.setBackground(MaiChatTheme.rounded(Color.WHITE, 16, activity));
        card.setContentDescription("云盘空间");
        LinearLayout heading = new LinearLayout(activity);
        heading.setGravity(Gravity.CENTER_VERTICAL);
        heading.addView(new CloudIconView(CloudIconView.CLOUD, MaiChatTheme.TEXT),
            new LinearLayout.LayoutParams(dp(23), dp(23)));
        TextView title = label("云盘空间", 17, MaiChatTheme.TEXT);
        title.setTypeface(MaiChatTypography.semibold());
        LinearLayout.LayoutParams titleParams = new LinearLayout.LayoutParams(0, -2, 1);
        titleParams.leftMargin = dp(9);
        heading.addView(title, titleParams);
        quota = label("正在读取…", 14, MaiChatTheme.SECONDARY);
        quota.setGravity(Gravity.END);
        heading.addView(quota, new LinearLayout.LayoutParams(-2, -2));
        card.addView(heading, new LinearLayout.LayoutParams(-1, -2));
        quotaProgress = new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        quotaProgress.setMax(1000);
        quotaProgress.setProgressTintList(ColorStateList.valueOf(MaiChatTheme.BLUE));
        quotaProgress.setProgressBackgroundTintList(ColorStateList.valueOf(Color.rgb(229, 229, 234)));
        LinearLayout.LayoutParams meterParams = new LinearLayout.LayoutParams(-1, dp(4));
        meterParams.topMargin = dp(14);
        meterParams.bottomMargin = dp(13);
        card.addView(quotaProgress, meterParams);
        quotaNote = label("仅自己可见 · 视频 2 GB / 图片 20 MB", 13, MaiChatTheme.SECONDARY);
        card.addView(quotaNote, new LinearLayout.LayoutParams(-1, -2));
        return card;
    }

    private View symbolButton(MaiChatSymbolView.Symbol symbol, String description, Runnable action) {
        FrameLayout target = new FrameLayout(activity);
        MaiChatSymbolView view = new MaiChatSymbolView(activity, symbol);
        view.setSymbolColor(MaiChatTheme.TEXT);
        view.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        target.addView(view, new FrameLayout.LayoutParams(dp(32), dp(32), Gravity.CENTER));
        target.setContentDescription(description);
        target.setOnClickListener(ignored -> action.run());
        target.setFocusable(true);
        return target;
    }

    private void updateNavigation() {
        if (back == null) return;
        back.setVisibility(!directory.currentId().isEmpty() || !embedded ? View.VISIBLE : View.GONE);
        back.setContentDescription(directory.currentId().isEmpty() ? "关闭云盘" : "返回上级文件夹");
    }

    boolean navigateBack() {
        if (directory.currentId().isEmpty()) return false;
        if (uploading || playing || saving) return true;
        directory.back();
        refresh();
        return true;
    }

    private void setStatus(String message) {
        status.setText(message);
        status.setVisibility(message == null || message.isEmpty() ? View.GONE : View.VISIBLE);
    }

    private void release() {
        ++generation;
        client.cancel();
        folderClient.cancel();
        downloadScope.cancel();
        if (vodOpening != null) vodOpening.cancel();
        uploading = loading = folderLoading = playing = saving = false;
        requestedCheckpoint = null;
        cleanupIdlePlaybackCache();
        clearThumbnails();
        if (activity.currentCloudDriveDialog() == this) activity.clearCloudDriveDialog(this);
    }

    void confirmAgentFile(File file) {
        try {
            MaiCloudDrive.Source source = client.inspect(file);
            new AlertDialog.Builder(activity).setTitle("保存到我的云盘")
                .setMessage(source.name + "\n" + sizeText(source.bytes) + " · 仅自己可见")
                .setNegativeButton("取消", null)
                .setPositiveButton("保存", (ignored, which) -> upload(source, null))
                .show();
        } catch (IOException failure) { setStatus(failure.getMessage()); }
    }

    void onPicked(Uri uri) {
        if (!isShowing() || uri == null) return;
        try {
            MaiCloudDrive.Source source = client.inspect(uri);
            File checkpoint = requestedCheckpoint;
            requestedCheckpoint = null;
            new AlertDialog.Builder(activity).setTitle(checkpoint == null ?
                    "上传到我的云盘" : "继续上传")
                .setMessage(source.name + "\n" + sizeText(source.bytes) + " · 仅自己可见")
                .setNegativeButton("取消", null)
                .setPositiveButton("上传", (ignored, which) -> upload(source, checkpoint))
                .show();
        } catch (IOException failure) {
            requestedCheckpoint = null;
            setStatus(failure.getMessage());
        }
    }

    void onPickCanceled() { requestedCheckpoint = null; }

    void onPickPermissionFailure() {
        requestedCheckpoint = null;
        setStatus("该文件来源不支持续传授权，请从系统文件管理器选择");
    }

    private void refresh() {
        if (!isShowing() || uploading || playing || saving) return;
        client.cancel();
        folderClient.cancel();
        loading = false;
        folderLoading = true;
        listError = "";
        media.clear();
        cursor = "";
        library.removeAllViews();
        next.setVisibility(View.GONE);
        updateNavigation();
        loadNext();
        int requestGeneration = generation;
        folderClient.listFolders(result -> {
            if (!live(requestGeneration)) return;
            folderLoading = false;
            next.setEnabled(!loading);
            if (result.ok()) {
                directory.updateFolders(result.json().optJSONArray("folders"));
                if (!directory.currentExists()) {
                    listError = "文件夹已不存在，请返回上级或根目录";
                    setStatus(listError);
                    media.clear();
                    cursor = "";
                    next.setVisibility(View.GONE);
                }
            } else if (listError.isEmpty()) {
                listError = "暂时无法读取文件夹，请重试";
                setStatus(listError);
                next.setText("重试");
                next.setVisibility(View.VISIBLE);
            }
            renderMedia(requestGeneration);
        });
    }

    private void loadNext() {
        if (loading || uploading) return;
        loading = true;
        int requestGeneration = ++generation;
        String requestedCursor = cursor;
        setStatus(media.isEmpty() && directory.children().isEmpty() ? "" : "正在读取云盘…");
        next.setEnabled(false);
        renderMedia(requestGeneration);
        JSONObject payload = directory.payload(cursor);
        client.request(MaiCloudDrive.LIST, payload, result -> {
            if (!live(requestGeneration)) return;
            loading = false;
            next.setEnabled(!folderLoading);
            if (!result.ok()) {
                listError = !directory.currentId().isEmpty() && result.httpStatus == 404 ?
                    "文件夹已不存在，请返回上级或根目录" : MaiCloudDrive.errorText(result);
                setStatus(listError);
                if ("正在读取…".contentEquals(quota.getText())) quota.setText("暂不可用");
                upload.setEnabled(result.httpStatus != 404 || !directory.currentId().isEmpty());
                next.setText("重试");
                // 根目录尚未开通时仍保留重试；加号会被禁用，不能依赖其中的刷新菜单恢复。
                next.setVisibility(View.VISIBLE);
                renderMedia(requestGeneration);
                loadUploads(requestGeneration);
                return;
            }
            upload.setEnabled(true);
            JSONObject body = result.json();
            JSONObject usage = body.optJSONObject("quota");
            if (usage != null) {
                long used = usage.optLong("used_bytes");
                long reserved = usage.optLong("reserved_bytes");
                long limit = usage.optLong("limit_bytes", MaiCloudDrive.QUOTA_BYTES);
                long occupied = Math.max(0, used) + Math.max(0, reserved);
                quota.setText(sizeText(occupied) + " / " + sizeText(limit));
                quotaProgress.setProgress((int) Math.max(0, Math.min(1000,
                    (double) occupied * 1000 / Math.max(1, limit))));
                quotaNote.setText(reserved > 0 ? "其中 " + sizeText(reserved) +
                    " 已预留 · 视频 2 GB / 图片 20 MB" :
                    "仅自己可见 · 视频 2 GB / 图片 20 MB");
            }
            JSONArray page = body.optJSONArray("videos");
            if (page != null) for (int i = 0; i < page.length(); ++i) {
                JSONObject item = page.optJSONObject(i);
                if (item != null && directory.accepts(item) && !containsMedia(item)) media.add(item);
            }
            cursor = body.optString("next_cursor", "");
            if ("null".equals(cursor)) cursor = "";
            if (!cursor.isEmpty() && cursor.equals(requestedCursor)) {
                listError = "云盘分页未前进，请重试";
                cursor = "";
            }
            next.setText(listError.isEmpty() ? "加载更多" : "重试");
            next.setVisibility(cursor.isEmpty() && listError.isEmpty() ? View.GONE : View.VISIBLE);
            renderMedia(requestGeneration);
            setStatus(listError);
            loadUploads(requestGeneration);
        });
    }

    private boolean containsMedia(JSONObject candidate) {
        String id = candidate.optString("id", candidate.optString("video_id", ""));
        for (JSONObject item : media)
            if (id.equals(item.optString("id", item.optString("video_id", "")))) return true;
        return false;
    }

    private void browse(String id) {
        if (uploading || playing || saving) return;
        if (!directory.browse(id)) {
            setStatus("文件夹已不存在，请刷新云盘");
            return;
        }
        refresh();
    }

    private void showAddMenu() {
        PopupMenu menu = new PopupMenu(activity, upload);
        menu.getMenu().add(directory.currentId().isEmpty() ? "上传图片或视频" :
            "上传图片或视频到根目录").setOnMenuItemClickListener(item -> {
                activity.pickCloudDriveMedia(this);
                return true;
            });
        menu.getMenu().add("新建文件夹").setEnabled(!loading && !folderLoading)
            .setOnMenuItemClickListener(item -> {
                promptName("新建文件夹", "", name -> mutate("正在创建文件夹…",
                    done -> folderClient.createFolder(name, directory.currentId(), done)));
                return true;
            });
        menu.getMenu().add("刷新云盘").setOnMenuItemClickListener(item -> {
            refresh(); return true;
        });
        menu.show();
    }

    private View folderRow(JSONObject folder) {
        String id = folder.optString("id");
        String name = folder.optString("name", "文件夹");
        LinearLayout card = card();
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        FrameLayout icon = new FrameLayout(activity);
        icon.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE_SOFT, 12, activity));
        icon.addView(new CloudIconView(CloudIconView.FOLDER, MaiChatTheme.BLUE),
            new FrameLayout.LayoutParams(-1, -1));
        card.addView(icon, new LinearLayout.LayoutParams(dp(56), dp(56)));
        TextView title = label(name, 16, MaiChatTheme.TEXT);
        title.setTypeface(MaiChatTypography.semibold());
        title.setMaxLines(2);
        title.setEllipsize(android.text.TextUtils.TruncateAt.END);
        LinearLayout.LayoutParams titleParams = new LinearLayout.LayoutParams(0, -2, 1);
        titleParams.leftMargin = dp(14);
        titleParams.rightMargin = dp(8);
        card.addView(title, titleParams);
        card.addView(new CloudIconView(CloudIconView.CHEVRON, Color.rgb(142, 142, 147)),
            new LinearLayout.LayoutParams(dp(20), dp(24)));
        card.setContentDescription(name + "，文件夹");
        card.setOnClickListener(ignored -> browse(id));
        card.setOnLongClickListener(ignored -> { showFolderMenu(folder); return true; });
        return card;
    }

    private boolean canEdit() {
        return isShowing() && !loading && !folderLoading && !uploading && !playing && !saving;
    }

    private void showFolderMenu(JSONObject folder) {
        if (!canEdit()) return;
        String id = folder.optString("id");
        String name = folder.optString("name", "文件夹");
        new AlertDialog.Builder(activity).setTitle(name)
            .setItems(new String[]{"重命名", "移动文件夹", "删除文件夹"}, (menu, which) -> {
                if (which == 0) promptName("重命名文件夹", name,
                    changed -> mutate("正在重命名…",
                        done -> folderClient.updateFolder(id, changed, null, done)));
                else if (which == 1) chooseDestination(id, target ->
                    mutate("正在移动文件夹…",
                        done -> folderClient.updateFolder(id, null, target, done)));
                else new AlertDialog.Builder(activity).setTitle("删除文件夹")
                    .setMessage("删除「" + name + "」？只能删除空文件夹。")
                    .setNegativeButton("取消", null)
                    .setPositiveButton("删除", (ignored, button) ->
                        mutate("正在删除文件夹…", done -> folderClient.deleteFolder(id, done)))
                    .show();
            }).show();
    }

    private void showMediaMenu(JSONObject item) {
        if (!canEdit()) return;
        String id = item.optString("id", item.optString("video_id", ""));
        String name = item.optString("name", "文件");
        new AlertDialog.Builder(activity).setTitle(name)
            .setItems(new String[]{"重命名", "移动到文件夹", "删除文件"}, (menu, which) -> {
                if (which == 0) promptName("重命名文件", name,
                    changed -> mutate("正在重命名…",
                        done -> folderClient.renameMedia(id, changed, done)));
                else if (which == 1) chooseDestination("", target ->
                    mutate("正在移动文件…", done -> folderClient.moveMedia(id, target, done)));
                else new AlertDialog.Builder(activity)
                    .setMessage("删除「" + name + "」？此操作无法撤销。")
                    .setNegativeButton("取消", null)
                    .setPositiveButton("删除", (ignored, button) -> delete(id)).show();
            }).show();
    }

    private void promptName(String title, String initial, Consumer<String> submit) {
        if (!canEdit()) return;
        EditText input = new EditText(activity);
        input.setText(initial);
        input.setSingleLine(true);
        input.setSelectAllOnFocus(true);
        LinearLayout wrapper = new LinearLayout(activity);
        wrapper.setPadding(dp(20), dp(8), dp(20), 0);
        wrapper.addView(input, new LinearLayout.LayoutParams(-1, -2));
        AlertDialog editor = new AlertDialog.Builder(activity).setTitle(title).setView(wrapper)
            .setNegativeButton("取消", null).setPositiveButton("确定", null).create();
        editor.setOnShowListener(ignored -> {
            input.requestFocus();
            editor.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
            editor.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(button -> {
                String value = input.getText().toString().trim();
                if (value.isEmpty()) { input.setError("请输入名称"); return; }
                if (!canEdit()) return;
                editor.dismiss();
                submit.accept(value);
            });
        });
        editor.show();
    }

    private void chooseDestination(String excludedFolder, Consumer<String> submit) {
        if (!canEdit()) return;
        ArrayList<JSONObject> destinations = directory.folders();
        ArrayList<String> ids = new ArrayList<>(), names = new ArrayList<>();
        ids.add("");
        names.add("我的云盘（根目录）");
        for (JSONObject folder : destinations) {
            String id = folder.optString("id");
            if (id.equals(excludedFolder)) continue;
            ids.add(id);
            names.add(folder.optString("name", "文件夹"));
        }
        // Parent-cycle and non-empty-folder checks remain in the shared client
        // and service; this picker only selects the requested destination.
        new AlertDialog.Builder(activity).setTitle("移动到")
            .setItems(names.toArray(new String[0]), (ignored, which) -> submit.accept(ids.get(which)))
            .setNegativeButton("取消", null).show();
    }

    private void mutate(String message, Consumer<Consumer<MaiCloudDrive.Result>> operation) {
        if (!canEdit()) return;
        int expected = generation;
        saving = true;
        upload.setEnabled(false);
        setStatus(message);
        operation.accept(result -> {
            if (!live(expected)) return;
            saving = false;
            upload.setEnabled(true);
            if (!result.ok()) { setStatus(MaiCloudDrive.errorText(result)); return; }
            refresh();
        });
    }

    private void loadUploads(int requestGeneration) {
        client.checkpoints(result -> {
            if (!live(requestGeneration)) return;
            uploads.removeAllViews();
            uploads.setVisibility(View.GONE);
            if (!result.ok()) return;
            JSONArray paused = result.json().optJSONArray("uploads");
            if (paused == null || paused.length() == 0) return;
            uploads.setVisibility(View.VISIBLE);
            uploads.addView(sectionTitle("待续传"), row());
            for (int i = 0; i < paused.length(); ++i) {
                JSONObject upload = paused.optJSONObject(i);
                if (upload != null) uploads.addView(uploadRow(upload), row());
            }
        });
    }

    private View uploadRow(JSONObject item) {
        String name = item.optString("name", "未完成文件");
        File checkpoint = new File(item.optString("checkpoint_path", ""));
        String sourceId = item.optString("source_id", "");
        LinearLayout card = card();
        card.addView(label(name, 15, MaiChatTheme.TEXT), row());
        card.addView(label(sizeText(item.optLong("size_bytes")) + " · 上传尚未完成",
            12, MaiChatTheme.SECONDARY), row());
        LinearLayout actions = new LinearLayout(activity);
        card.addView(actions, row());
        Button resume = button("继续上传", () -> {
            if (sourceId.startsWith("content://")) {
                try {
                    upload(client.inspect(Uri.parse(sourceId)), checkpoint);
                } catch (Exception unavailable) {
                    requestedCheckpoint = checkpoint;
                    setStatus("原文件授权已失效，请重新选择同一文件");
                    activity.pickCloudDriveMedia(this);
                }
            } else {
                File sourceFile = new File(sourceId);
                if (sourceFile.isFile() && sourceFile.length() == item.optLong("size_bytes")) {
                    upload(new MaiCloudDrive.Source(null, sourceFile,
                        item.optString("name", ""),
                        item.optString("content_type", ""),
                        item.optLong("size_bytes")), checkpoint);
                } else {
                    requestedCheckpoint = checkpoint;
                    setStatus("原文件已移走，请重新选择同一文件");
                    activity.pickCloudDriveMedia(this);
                }
            }
        });
        actions.addView(resume, new LinearLayout.LayoutParams(0, dp(42), 1));
        Button discard = button("放弃", () -> new AlertDialog.Builder(activity)
            .setMessage("放弃后会删除未完成的上传分片。原文件不会删除。")
            .setNegativeButton("取消", null)
            .setPositiveButton("放弃上传", (ignored, which) -> {
                setStatus("正在取消上传…");
                client.abort(checkpoint, result -> {
                    setStatus(result.ok() ? "已放弃上传" : MaiCloudDrive.errorText(result));
                    if (result.ok()) {
                        new File(checkpoint.getAbsolutePath() + ".source").delete();
                        refresh();
                    }
                });
            }).show());
        LinearLayout.LayoutParams discardParams = new LinearLayout.LayoutParams(dp(72), dp(42));
        discardParams.leftMargin = dp(8);
        actions.addView(discard, discardParams);
        return card;
    }

    private void upload(MaiCloudDrive.Source source, File resumeCheckpoint) {
        if (!isShowing() || uploading || playing || saving ||
            !account.equals(MaiChatAuthClient.shared(activity).accountId())) return;
        try {
            File checkpoint = resumeCheckpoint == null ? client.newCheckpoint() : resumeCheckpoint;
            uploading = true;
            upload.setEnabled(false);
            progress.setVisibility(View.VISIBLE);
            progress.setProgress(0);
            setStatus("准备上传 " + source.name + "…");
            Button pause = button("暂停上传", () -> {
                client.cancel();
                setStatus("已暂停，可稍后续传");
                uploading = false;
                upload.setEnabled(true);
                progress.setVisibility(View.GONE);
                refresh();
            });
            uploads.removeAllViews();
            uploads.setVisibility(View.VISIBLE);
            uploads.addView(pause, row());
            client.upload(source, checkpoint, (done, total, preparing) -> {
                if (!isShowing() || !account.equals(MaiChatAuthClient.shared(activity).accountId()))
                    return;
                progress.setProgress(total <= 0 ? 0 :
                    (int) Math.min(1000, done * 1000 / total));
                setStatus((preparing ? "正在准备文件 " : "正在上传 ") +
                    sizeText(done) + " / " + sizeText(total));
            }, result -> {
                if (!isShowing()) return;
                uploading = false;
                progress.setVisibility(View.GONE);
                upload.setEnabled(true);
                if (result.ok()) {
                    new File(checkpoint.getAbsolutePath() + ".source").delete();
                    setStatus("已保存到我的云盘");
                    Toast.makeText(activity, "上传完成", Toast.LENGTH_SHORT).show();
                } else setStatus(MaiCloudDrive.errorText(result));
                refresh();
            });
        } catch (IOException failure) { setStatus(failure.getMessage()); }
    }

    private void renderMedia(int requestGeneration) {
        library.removeAllViews();
        if (!directory.currentId().isEmpty()) {
            LinearLayout path = new LinearLayout(activity);
            path.setGravity(Gravity.CENTER_VERTICAL);
            TextView root = label("我的云盘", 14, MaiChatTheme.BLUE);
            root.setPadding(0, dp(10), dp(8), dp(10));
            root.setContentDescription("返回云盘根目录");
            root.setOnClickListener(ignored -> browse(""));
            path.addView(root, new LinearLayout.LayoutParams(-2, dp(44)));
            path.addView(new CloudIconView(CloudIconView.CHEVRON, MaiChatTheme.SECONDARY),
                new LinearLayout.LayoutParams(dp(18), dp(24)));
            TextView current = label(directory.currentName(), 14, MaiChatTheme.TEXT);
            current.setSingleLine(true);
            current.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
            LinearLayout.LayoutParams currentParams = new LinearLayout.LayoutParams(0, -2, 1);
            currentParams.leftMargin = dp(8);
            path.addView(current, currentParams);
            library.addView(path, row());
        }
        for (JSONObject folder : directory.children()) library.addView(folderRow(folder), row());
        for (JSONObject item : media) library.addView(mediaRow(item, requestGeneration), row());
        // 空列表按用户要求留白；请求期间仍显示真实加载状态。
        if (media.isEmpty() && directory.children().isEmpty() && listError.isEmpty()
                && (loading || folderLoading)) {
            LinearLayout empty = new LinearLayout(activity);
            empty.setOrientation(LinearLayout.VERTICAL);
            empty.setGravity(Gravity.CENTER);
            empty.setPadding(dp(12), dp(38), dp(12), dp(38));
            ProgressBar spinner = new ProgressBar(activity);
            spinner.setIndeterminateTintList(ColorStateList.valueOf(MaiChatTheme.BLUE));
            empty.addView(spinner, new LinearLayout.LayoutParams(dp(30), dp(30)));
            TextView message = label("正在加载云盘", 14, MaiChatTheme.SECONDARY);
            message.setPadding(0, dp(14), 0, 0);
            empty.addView(message);
            library.addView(empty, new LinearLayout.LayoutParams(-1, -2));
        }
    }

    private View mediaRow(JSONObject item, int requestGeneration) {
        String id = item.optString("id", item.optString("video_id", ""));
        String name = item.optString("name", "文件");
        long bytes = item.optLong("size_bytes");
        boolean image = "image".equals(item.optString("media_type", "video"));
        LinearLayout card = card();
        card.setOrientation(LinearLayout.HORIZONTAL);
        card.setGravity(Gravity.CENTER_VERTICAL);
        card.setContentDescription((image ? "查看图片：" : "播放视频：") + name);
        card.setOnClickListener(ignored -> open(id, name, bytes, image));
        card.setOnLongClickListener(ignored -> { showMediaMenu(item); return true; });
        FrameLayout cover = new FrameLayout(activity);
        cover.setBackground(MaiChatTheme.rounded(MaiChatTheme.BLUE_SOFT, 12, activity));
        cover.setClipToOutline(true);
        View placeholder = new CloudIconView(image ? CloudIconView.IMAGE : CloudIconView.PLAY,
            MaiChatTheme.BLUE);
        cover.addView(placeholder, new FrameLayout.LayoutParams(dp(28), dp(28), Gravity.CENTER));
        ImageView thumbnail = new ImageView(activity);
        thumbnail.setScaleType(ImageView.ScaleType.CENTER_CROP);
        thumbnail.setContentDescription(name + "缩略图");
        cover.addView(thumbnail, new FrameLayout.LayoutParams(-1, -1));
        if (!image) {
            FrameLayout play = new FrameLayout(activity);
            play.setBackground(MaiChatTheme.rounded(Color.argb(150, 0, 0, 0), 12, activity));
            play.addView(new CloudIconView(CloudIconView.PLAY, Color.WHITE),
                new FrameLayout.LayoutParams(-1, -1));
            cover.addView(play, new FrameLayout.LayoutParams(dp(24), dp(24), Gravity.CENTER));
        }
        card.addView(cover, new LinearLayout.LayoutParams(dp(72), dp(72)));
        LinearLayout details = new LinearLayout(activity);
        details.setOrientation(LinearLayout.VERTICAL);
        LinearLayout.LayoutParams detailParams = new LinearLayout.LayoutParams(0, -2, 1);
        detailParams.leftMargin = dp(12);
        card.addView(details, detailParams);
        TextView filename = label(name, 15, MaiChatTheme.TEXT);
        filename.setTypeface(MaiChatTypography.semibold());
        filename.setSingleLine(true);
        filename.setEllipsize(android.text.TextUtils.TruncateAt.MIDDLE);
        details.addView(filename, new LinearLayout.LayoutParams(-1, -2));
        TextView size = label(sizeText(bytes), 12, Color.rgb(142, 142, 147));
        LinearLayout.LayoutParams sizeParams = new LinearLayout.LayoutParams(-1, -2);
        sizeParams.topMargin = dp(5);
        details.addView(size, sizeParams);
        // Folder and media metadata can finish in either order. Start cover
        // work only after both settle so one refresh never downloads twice to
        // the same account-owned thumbnail path while replacing a row view.
        if (!loading && !folderLoading)
            loadThumbnail(id, thumbnail, placeholder, requestGeneration);
        if (!image) {
            View save = new CloudIconView(CloudIconView.DOWNLOAD, MaiChatTheme.TEXT);
            save.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);
            save.setContentDescription("保存到相册");
            save.setOnClickListener(ignored -> saveVideo(id, name, bytes));
            save.setFocusable(true);
            LinearLayout.LayoutParams saveParams = new LinearLayout.LayoutParams(dp(44), dp(44));
            saveParams.leftMargin = dp(6);
            card.addView(save, saveParams);
        }
        return card;
    }

    private void loadThumbnail(String id, ImageView thumbnail, View placeholder,
                               int requestGeneration) {
        if (id.isEmpty()) return;
        if (thumbnailDirectory == null) {
            thumbnailDirectory = new File(activity.getCacheDir(),
                "cloud-drive-thumbnails/" + MaiCloudDrive.accountKey(account));
        }
        if (!thumbnailDirectory.isDirectory() && !thumbnailDirectory.mkdirs()) return;
        File target = new File(thumbnailDirectory,
            UUID.nameUUIDFromBytes(id.getBytes(StandardCharsets.UTF_8)) +
                // 旧目录请求的清理回调不能删除刷新后同一媒体的新封面。
                "-" + thumbnailInstance + "-" + requestGeneration + ".jpg");
        if (target.isFile()) {
            Bitmap cached = decodeThumbnail(target);
            if (cached != null) {
                thumbnail.setImageBitmap(cached);
                placeholder.setVisibility(View.GONE);
                return;
            }
            target.delete();
        }
        JSONObject payload = new JSONObject();
        try { payload.put("video_id", id); } catch (Exception ignored) { return; }
        // A thumbnail has its own request owner. Listing, uploading, or opening
        // another row must not cancel this request's short-lived signed link.
        new MaiCloudDrive(activity).request(MaiCloudDrive.THUMBNAIL_LINK, payload, result -> {
            if (!live(requestGeneration) || !result.ok()) return;
            String url = result.json().optString("thumbnail_url", "");
            if (!url.startsWith("https://")) return;
            MaiChatAttachmentDownload.Scope scope = downloadScope;
            downloads.download(url, target, MAX_THUMBNAIL_BYTES, scope, downloaded -> {
                Bitmap bitmap = downloaded.ok() ? decodeThumbnail(target) : null;
                activity.runOnUiThread(() -> {
                    if (!live(requestGeneration) || scope != downloadScope ||
                        thumbnail.getParent() == null) {
                        if (bitmap != null) bitmap.recycle();
                        target.delete();
                        return;
                    }
                    if (bitmap == null) { target.delete(); return; }
                    thumbnail.setImageBitmap(bitmap);
                    placeholder.setVisibility(View.GONE);
                });
            });
        });
    }

    private static Bitmap decodeThumbnail(File file) {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(file.getAbsolutePath(), bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0 ||
            bounds.outWidth > 8192 || bounds.outHeight > 8192) return null;
        BitmapFactory.Options sampled = new BitmapFactory.Options();
        sampled.inSampleSize = 1;
        while (bounds.outWidth / sampled.inSampleSize > 320 ||
               bounds.outHeight / sampled.inSampleSize > 320)
            sampled.inSampleSize *= 2;
        return BitmapFactory.decodeFile(file.getAbsolutePath(), sampled);
    }

    private void clearThumbnails() {
        if (thumbnailDirectory == null) return;
        File[] files = thumbnailDirectory.listFiles();
        if (files != null) for (File file : files) if (file.isFile()) file.delete();
        thumbnailDirectory.delete();
    }

    private void open(String id, String name, long bytes, boolean image) {
        if (id.isEmpty() || playing || uploading || saving) return;
        if (loading || folderLoading) { setStatus("正在加载云盘，请稍候"); return; }
        MaiCloudDrive.MediaInfo info = MaiCloudDrive.mediaInfo(name);
        if (info == null || !info.mediaType.equals(image ? "image" : "video") ||
            bytes < 1 || bytes > info.maxBytes) {
            setStatus("云盘文件信息无效，无法预览");
            return;
        }
        if (!image) {
            startVideoVod(id, name);
            return;
        }
        File directory = new File(activity.getCacheDir(), "cloud-drive-images");
        if (!directory.isDirectory() && !directory.mkdirs()) {
            setStatus("无法准备预览缓存"); return;
        }
        activity.cleanupIdleCloudDriveImages();
        if (!MaiCloudDrivePlaybackCache.hasSpace(directory, bytes)) {
            setStatus("设备剩余空间不足，无法缓存文件"); return;
        }
        playing = true;
        setStatus("正在获取预览链接…");
        JSONObject payload = new JSONObject();
        try { payload.put("video_id", id); } catch (Exception ignored) { }
        client.request(MaiCloudDrive.PLAY_LINK, payload, result -> {
            if (!result.ok()) {
                playing = false;
                setStatus(MaiCloudDrive.errorText(result));
                return;
            }
            String url = result.json().optString("read_url", "");
            if (!url.startsWith("https://")) {
                playing = false; setStatus("预览链接无效"); return;
            }
            String extension = name.substring(name.lastIndexOf('.')).toLowerCase(Locale.ROOT);
            File target = new File(directory, UUID.randomUUID() + extension);
            setStatus("正在下载图片…");
            MaiChatAttachmentDownload.Scope scope = downloadScope;
            downloads.download(url, target, info.maxBytes,
                scope, downloaded -> activity.runOnUiThread(() -> {
                    if (!isShowing() || scope != downloadScope ||
                        !account.equals(MaiChatAuthClient.shared(activity).accountId())) {
                        target.delete();
                        return;
                    }
                    playing = false;
                    if (!downloaded.ok()) { setStatus("下载失败，请重试"); return; }
                    setStatus("");
                    if (!activity.openCloudDriveImage(target.getAbsolutePath())) target.delete();
                }));
        });
    }

    private void startVideoVod(String id, String name) {
        final MaiCloudVod.Opening opening;
        try { opening = new MaiCloudVod.Opening(); }
        catch (IOException | LinkageError failure) {
            setStatus("无法启动云盘点播");
            return;
        }
        vodOpening = opening;
        playing = true;
        setStatus("正在缓冲视频…");
        int requestGeneration = generation;
        String expectedAccount = account;
        String extension = name.toLowerCase(Locale.ROOT).endsWith(".mov") ? "mov" : "mp4";
        VOD_WORKERS.execute(() -> {
            MaiCloudVod.Session source = null;
            String error = null;
            try { source = client.openVodBlocking(id, extension, opening); }
            catch (Exception | LinkageError failure) { error = failure.getMessage(); }
            MaiCloudVod.Session ready = source;
            String message = error;
            activity.runOnUiThread(() -> {
                if (vodOpening == opening) vodOpening = null;
                if (!isShowing() || generation != requestGeneration || opening.isCanceled() ||
                    !expectedAccount.equals(MaiChatAuthClient.shared(activity).accountId())) {
                    if (ready != null) { ready.cancel(); ready.close(); }
                    else opening.release();
                    if (isShowing()) playing = false;
                    return;
                }
                playing = false;
                if (ready == null) {
                    opening.release();
                    setStatus(message == null ? "视频点播失败，请重试" : message);
                    return;
                }
                setStatus("");
                if (!activity.openCloudDriveVideo(ready)) {
                    ready.cancel();
                    ready.close();
                    setStatus("视频无法播放");
                }
            });
        });
    }

    private void saveVideo(String id, String name, long bytes) {
        if (id.isEmpty() || saving || playing || uploading) return;
        if (loading || folderLoading) { setStatus("正在加载云盘，请稍候"); return; }
        MaiCloudDrive.MediaInfo info = MaiCloudDrive.mediaInfo(name);
        if (info == null || !"video".equals(info.mediaType) ||
            bytes < 1 || bytes > info.maxBytes) {
            setStatus("云盘视频信息无效，无法保存");
            return;
        }
        File directory = new File(activity.getCacheDir(), "cloud-drive-explicit-saves");
        if (!directory.isDirectory() && !directory.mkdirs()) {
            setStatus("无法准备保存缓存");
            return;
        }
        saving = true;
        setStatus("正在下载以保存到相册…");
        int requestGeneration = generation;
        String expectedAccount = account;
        JSONObject payload = new JSONObject();
        try { payload.put("video_id", id); }
        catch (Exception ignored) { saving = false; return; }
        client.request(MaiCloudDrive.PLAY_LINK, payload, result -> {
            if (!live(requestGeneration) || !expectedAccount.equals(account)) {
                saving = false;
                return;
            }
            if (!result.ok()) {
                saving = false;
                setStatus(MaiCloudDrive.errorText(result));
                return;
            }
            String url = result.json().optString("read_url", "");
            if (!url.startsWith("https://")) {
                saving = false;
                setStatus("云盘保存链接无效");
                return;
            }
            String extension = name.toLowerCase(Locale.ROOT).endsWith(".mov") ? ".mov" : ".mp4";
            File target = new File(directory, UUID.randomUUID() + extension);
            MaiChatAttachmentDownload.Scope scope = downloadScope;
            downloads.download(url, target, info.maxBytes, scope,
                downloaded -> activity.runOnUiThread(() -> {
                    if (!downloaded.ok() || !live(requestGeneration) ||
                        !expectedAccount.equals(account) || scope != downloadScope) {
                        saving = false;
                        target.delete();
                        if (isShowing()) setStatus("保存下载失败，请重试");
                        return;
                    }
                    setStatus("正在保存到相册…");
                    activity.saveCloudDriveVideo(target.getAbsolutePath(), () -> {
                        target.delete();
                        activity.runOnUiThread(() -> {
                            saving = false;
                            if (isShowing()) setStatus("");
                        });
                    });
                }));
        });
    }

    private void cleanupIdlePlaybackCache() {
        MaiCloudDrivePlaybackCache.cleanupIdle(new File(activity.getCacheDir(),
            "cloud-drive-playback"));
    }

    private void delete(String id) {
        JSONObject payload = new JSONObject();
        try { payload.put("video_id", id); } catch (Exception ignored) { }
        mutate("正在删除…", done -> client.request(MaiCloudDrive.DELETE, payload, done));
    }

    private boolean live(int expected) {
        return isShowing() && expected == generation &&
            account.equals(MaiChatAuthClient.shared(activity).accountId());
    }

    private LinearLayout card() {
        LinearLayout card = new LinearLayout(activity);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(12), dp(12), dp(12), dp(12));
        card.setBackground(MaiChatTheme.rounded(Color.WHITE, 14, activity));
        return card;
    }

    private TextView sectionTitle(String text) {
        TextView heading = label(text, 16, MaiChatTheme.TEXT);
        heading.setTypeface(MaiChatTypography.semibold());
        heading.setPadding(0, dp(16), 0, dp(10));
        return heading;
    }

    private TextView label(String text, int sp, int color) {
        return MaiChatTheme.text(activity, text, sp, color);
    }

    private Button button(String title, Runnable action) {
        Button button = new Button(activity);
        button.setText(title);
        button.setAllCaps(false);
        button.setTextSize(14);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setElevation(0);
        button.setStateListAnimator(null);
        boolean primary = title.equals("上传图片或视频") || title.equals("继续上传");
        boolean destructive = title.equals("删除") || title.equals("放弃");
        button.setTextColor(primary ? Color.WHITE :
            destructive ? MaiChatTheme.RED : MaiChatTheme.BLUE_DARK);
        button.setBackground(primary ? MaiChatTheme.rounded(MaiChatTheme.BLUE, 10, activity) :
            MaiChatTheme.bordered(destructive ? Color.WHITE : MaiChatTheme.BLUE_SOFT,
                MaiChatTheme.BORDER, 10, activity));
        button.setOnClickListener(ignored -> action.run());
        return button;
    }

    private LinearLayout.LayoutParams row() {
        LinearLayout.LayoutParams layout = new LinearLayout.LayoutParams(-1, -2);
        layout.bottomMargin = dp(18);
        return layout;
    }

    private int dp(int value) {
        return (int) (value * activity.getResources().getDisplayMetrics().density + .5f);
    }

    // Use the same vector resources as the root navigation; the surrounding
    // container defines the card-specific size and tint without font glyphs.
    private final class CloudIconView extends ImageView {
        static final int CLOUD = 0, FOLDER = 1, IMAGE = 2, PLAY = 3, DOWNLOAD = 4,
            CHEVRON = 5;

        CloudIconView(int shape, int color) {
            super(activity);
            int resource;
            switch (shape) {
                case CLOUD: resource = R.drawable.ic_ui_cloud_filled; break;
                case FOLDER: resource = R.drawable.ic_ui_folder_filled; break;
                case IMAGE: resource = R.drawable.ic_ui_photo; break;
                case PLAY: resource = R.drawable.ic_ui_play; break;
                case DOWNLOAD: resource = R.drawable.ic_ui_download; break;
                default: resource = R.drawable.ic_ui_chevron_right; break;
            }
            setImageResource(resource);
            setImageTintList(ColorStateList.valueOf(color));
            setScaleType(ImageView.ScaleType.FIT_CENTER);
            int edgePadding = shape == CLOUD ? 0 : shape == FOLDER ? 14 :
                shape == PLAY ? 6 : shape == DOWNLOAD ? 10 : 4;
            setPadding(dp(edgePadding), dp(edgePadding), dp(edgePadding), dp(edgePadding));
            setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        }
    }

    private static String sizeText(long bytes) {
        if (bytes >= 1_000_000_000) return String.format(Locale.ROOT,
            bytes % 1_000_000_000 == 0 ? "%.0f GB" : "%.1f GB", bytes / 1e9);
        if (bytes >= 1_000_000) return String.format(Locale.ROOT, "%.1f MB", bytes / 1e6);
        if (bytes >= 1_000) return String.format(Locale.ROOT, "%.0f KB", bytes / 1e3);
        return Math.max(0, bytes) + " B";
    }
}
