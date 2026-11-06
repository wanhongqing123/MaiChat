// 文件作用：普通存储设置。只清理共享核心预览的缓存/临时普通文件，工作区始终只读。
package com.kongshang.maichat;

import android.app.Activity;
import android.app.AlertDialog;
import android.view.Gravity;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import org.json.JSONArray;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MaiChatStoragePanel extends LinearLayout implements AutoCloseable {
    private final Activity activity;
    private final AIAssistantController.StorageRoots roots;
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final List<MaiAppStorageClient> previews = new java.util.concurrent.CopyOnWriteArrayList<>();
    private volatile boolean closed;
    private boolean busy;
    private AlertDialog confirmation;
    private TextView operationStatus;
    private String resultText = "";
    public MaiChatStoragePanel(Activity activity, AIAssistantController.StorageRoots roots) {
        super(activity); this.activity = activity; this.roots = roots;
        setOrientation(VERTICAL);
        refresh();
    }
    private boolean current() { return !closed && roots.current.getAsBoolean(); }
    private void text(String value, int color, MaiChatTypographyTokens.Role role) {
        TextView label = MaiChatTheme.text(activity, value, role, color);
        label.setPadding(0, MaiChatTheme.dp(activity, 8), 0, MaiChatTheme.dp(activity, 8));
        addView(label, new LayoutParams(-1, -2));
    }
    private void refresh() {
        if (!current()) return;
        busy = true; removeAllViews(); text("正在统计可管理空间…", MaiChatTheme.SECONDARY, MaiChatTypographyTokens.Role.DETAIL);
        worker.execute(() -> {
            try (MaiAppStorageClient client = new MaiAppStorageClient(roots)) {
                JSONObject output = client.request(new JSONObject().put("action", "preview").put("target", "app"));
                activity.runOnUiThread(() -> { if (current()) render(output); });
            } catch (Exception error) { fail(error); }
        });
    }
    private void render(JSONObject output) {
        busy = false; removeAllViews();
        JSONArray summaries = output.optJSONArray("summaries");
        long total = 0;
        if (summaries != null) for (int i = 0; i < summaries.length(); i++) total += summaries.optJSONObject(i).optLong("bytes");
        LinearLayout header = new LinearLayout(activity); header.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout summary = new LinearLayout(activity); summary.setOrientation(VERTICAL);
        summary.addView(MaiChatTheme.text(activity, "所列目录占用", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY));
        TextView amount = MaiChatTheme.label(activity, size(total), MaiChatTypographyTokens.Role.DISPLAY, MaiChatTheme.TEXT);
        amount.setPadding(0, dp(8), 0, 0); summary.addView(amount);
        header.addView(summary, new LayoutParams(0, -2, 1));
        Button all = action("全部清理", true, output.optLong("candidate_files") > 0);
        all.setOnClickListener(v -> preview("all")); header.addView(all, new LayoutParams(-2, dp(44)));
        addView(header, new LayoutParams(-1, -2));
        operationStatus = MaiChatTheme.text(activity, "", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        operationStatus.setVisibility(GONE); addView(operationStatus, new LayoutParams(-1, -2));
        if (!resultText.isEmpty()) text(resultText, MaiChatTheme.SECONDARY, MaiChatTypographyTokens.Role.DETAIL);
        text("仅统计缓存、临时文件和当前工作区，不代表安装包或全 App 占用。工作区、聊天记录、模型和原片成品不开放清理。", MaiChatTheme.SECONDARY, MaiChatTypographyTokens.Role.DETAIL);
        if (output.optBoolean("limited")) text("统计已达到安全上限，以下数字可能不完整。", MaiChatTheme.SECONDARY, MaiChatTypographyTokens.Role.DETAIL);
        if (summaries != null) for (int i = 0; i < summaries.length(); i++) {
            JSONObject item = summaries.optJSONObject(i);
            String target = item.optString("target");
            boolean workspace = "workspace".equals(target);
            LinearLayout card = new LinearLayout(activity); card.setOrientation(VERTICAL);
            card.setPadding(dp(16), dp(12), dp(16), dp(12));
            card.setBackground(MaiChatTheme.rounded(MaiChatTheme.PANEL, 18, activity));
            LinearLayout.LayoutParams cardParams = new LayoutParams(-1, -2); cardParams.topMargin = dp(16);
            addView(card, cardParams);
            LinearLayout cardHeader = new LinearLayout(activity); cardHeader.setGravity(Gravity.CENTER_VERTICAL);
            LinearLayout details = new LinearLayout(activity); details.setOrientation(VERTICAL);
            TextView title = MaiChatTheme.label(activity, workspace ? "工作区（只读）" : "cache".equals(target) ? "缓存" : "临时文件", MaiChatTypographyTokens.Role.BODY, MaiChatTheme.TEXT);
            details.addView(title);
            TextView bytes = MaiChatTheme.text(activity, size(item.optLong("bytes")), MaiChatTypographyTokens.Role.HEADING1, MaiChatTheme.TEXT);
            bytes.setPadding(0, dp(8), 0, 0); details.addView(bytes);
            cardHeader.addView(details, new LayoutParams(0, -2, 1));
            if (!workspace) {
                Button clear = action("清理", false, item.optLong("eligible_files") > 0);
                clear.setContentDescription("清理" + ("cache".equals(target) ? "缓存" : "临时文件"));
                clear.setOnClickListener(v -> preview(target));
                cardHeader.addView(clear, new LayoutParams(-2, dp(42)));
            }
            card.addView(cardHeader, new LayoutParams(-1, -2));
            TextView note = MaiChatTheme.text(activity, workspace ? "保留原片、成品及聊天引用文件。"
                : "可清理 " + size(item.optLong("eligible_bytes")) + " · " + item.optLong("eligible_files") + " 个文件\n"
                    + ("cache".equals(target) ? "缓存可由对应功能重新生成或下载。" : "只处理过期中间产物，需要时重新执行对应任务。")
                    + "最近24小时及受保护文件保留。", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
            note.setPadding(0, dp(10), 0, dp(6)); card.addView(note);
        }
        Button rescan = action("重新统计", false, true);
        rescan.setOnClickListener(v -> { if (!busy) refresh(); });
        LayoutParams rescanParams = new LayoutParams(-1, dp(44)); rescanParams.topMargin = dp(16);
        addView(rescan, rescanParams);
    }
    // 复用产品主题的圆角和边框，避免系统 Button 的默认灰底与阴影混入设置卡片。
    private Button action(String title, boolean primary, boolean enabled) {
        Button button = MaiChatTypography.button(activity); button.setText(title); button.setAllCaps(false);
        MaiChatTypography.apply(button, MaiChatTypographyTokens.Role.DETAIL); button.setTextColor(primary ? android.graphics.Color.WHITE : MaiChatTheme.TEXT);
        button.setPadding(dp(16), 0, dp(16), 0);
        button.setMinWidth(0); button.setMinimumWidth(0); button.setMinHeight(0); button.setMinimumHeight(0);
        button.setBackgroundTintList(null); button.setStateListAnimator(null); button.setElevation(0);
        button.setBackground(primary ? MaiChatTheme.rounded(MaiChatTheme.BLUE, 12, activity)
            : MaiChatTheme.bordered(MaiChatTheme.PANEL, MaiChatTheme.BORDER, 12, activity));
        button.setEnabled(enabled); button.setAlpha(enabled ? 1f : 0.4f);
        return button;
    }
    private void preview(String target) {
        if (busy || !current()) return;
        busy = true;
        operationStatus.setText("正在准备清理预览…"); operationStatus.setVisibility(VISIBLE);
        worker.execute(() -> {
            try {
                releasePreviews();
                String[] targets = "all".equals(target) ? new String[]{"cache", "temporary"} : new String[]{target};
                List<String> tokens = new ArrayList<>(); long bytes = 0, files = 0;
                for (String item : targets) {
                    if (!current()) throw new IllegalStateException("账号已切换，请重新打开存储空间");
                    MaiAppStorageClient client = new MaiAppStorageClient(roots); previews.add(client);
                    if (!current()) throw new IllegalStateException("页面已关闭");
                    JSONObject output = client.request(new JSONObject().put("action", "preview").put("target", item));
                    if (!output.optBoolean("cleanup_allowed")) throw new IllegalStateException("该空间不允许清理");
                    tokens.add(output.getString("preview_id")); bytes += output.optLong("candidate_bytes"); files += output.optLong("candidate_files");
                }
                final long count = files, amount = bytes;
                activity.runOnUiThread(() -> {
                    if (!current()) return;
                    confirmation = new AlertDialog.Builder(activity).setTitle("确认清理")
                        .setMessage("将删除本次预览的 " + count + " 个文件（" + size(amount) + "）。\n缓存可重新生成或下载；过期临时文件无法直接恢复，需要重新执行对应任务。最近24小时、聊天记录、工作区和受保护模型保留。")
                        .setNegativeButton("取消", (d, w) -> cancelPreview())
                        .setPositiveButton("清理", (d, w) -> cleanup(tokens)).create();
                    confirmation.setOnCancelListener(d -> cancelPreview()); confirmation.show();
                });
            } catch (Exception error) { releasePreviews(); fail(error); }
        });
    }
    private void cancelPreview() {
        if (!current()) return; busy = false;
        if (operationStatus != null) operationStatus.setVisibility(GONE);
        worker.execute(this::releasePreviews);
    }
    private void cleanup(List<String> tokens) {
        if (!current()) return;
        operationStatus.setText("正在清理…");
        worker.execute(() -> {
            long removed = 0, bytes = 0, skipped = 0; String failure = "";
            try {
                for (int i = 0; i < tokens.size(); i++) {
                    if (!current()) throw new IllegalStateException("账号已切换，未继续清理");
                    JSONObject output = previews.get(i).request(new JSONObject().put("action", "cleanup").put("preview_id", tokens.get(i)));
                    removed += output.optLong("removed_files"); bytes += output.optLong("reclaimed_bytes"); skipped += output.optLong("skipped_files");
                }
            } catch (Exception error) { failure = "；部分清理未完成：" + error.getMessage(); }
            finally { releasePreviews(); }
            resultText = "已清理 " + removed + " 个文件，释放 " + size(bytes) + (skipped > 0 ? "，跳过 " + skipped + " 个已变化或受保护文件" : "") + failure;
            activity.runOnUiThread(() -> { if (current()) refresh(); });
        });
    }
    private void releasePreviews() {
        for (MaiAppStorageClient client : previews) if (previews.remove(client)) client.close();
    }
    private void fail(Exception error) {
        activity.runOnUiThread(() -> {
            if (!current()) return;
            busy = false; removeAllViews(); text("存储操作未完成：" + error.getMessage(), MaiChatTheme.SECONDARY, MaiChatTypographyTokens.Role.DETAIL);
            Button retry = action("重新统计", false, true); retry.setOnClickListener(v -> refresh());
            addView(retry, new LayoutParams(-1, dp(44)));
        });
    }
    private int dp(int value) { return MaiChatTheme.dp(activity, value); }
    public static String size(long bytes) {
        if (bytes < 1024) return bytes + " B";
        if (bytes < 1024L * 1024) return String.format(java.util.Locale.ROOT, "%.1f KB", bytes / 1024.0);
        if (bytes < 1024L * 1024 * 1024) return String.format(java.util.Locale.ROOT, "%.1f MB", bytes / (1024.0 * 1024));
        return String.format(java.util.Locale.ROOT, "%.2f GB", bytes / (1024.0 * 1024 * 1024));
    }
    @Override public void close() {
        if (closed) return;
        closed = true; if (confirmation != null) confirmation.dismiss();
        // 独立队列向执行中的核心请求发取消，不等扫描/清理排完，也不阻塞 UI。
        worker.shutdown();
        new Thread(this::releasePreviews, "MaiChat-storage-cancel").start();
    }
}
