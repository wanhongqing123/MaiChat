// 文件作用：读取当前账号的举报记录；错误、空态和刷新均保留在本页。
// 生命周期：页面关闭或账号代际变化后不展示迟到结果，不发起任何举报写操作。
package com.kongshang.maichat;

import android.app.Activity;
import android.graphics.Color;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public final class MaiChatSafetyReportsPanel extends LinearLayout {
    private final Activity activity;
    private final MaiChatAccountSettingsService service;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor();
    private final TextView status;
    private final Button refresh;
    private final LinearLayout rows;
    private boolean closed;
    private int generation;

    public MaiChatSafetyReportsPanel(Activity activity, RemoteIMSessionController session) {
        super(activity);
        this.activity = activity;
        String identity = session.passwordChangeIdentity();
        String account = session.accountId();
        service = new MaiChatAccountSettingsService(account,
            () -> !session.requiresLogin() && identity.equals(session.passwordChangeIdentity()),
            (path, method, body) -> MaiChatAuthClient.shared(activity).accountSettingsRequest(
                path, method, body, account,
                () -> !session.requiresLogin() && identity.equals(session.passwordChangeIdentity())));
        setOrientation(VERTICAL);
        setPadding(dp(18), dp(16), dp(18), 0);
        setBackgroundColor(MaiChatTheme.GROUPED_PAGE);
        LinearLayout header = new LinearLayout(activity);
        header.setGravity(Gravity.CENTER_VERTICAL);
        status = MaiChatTheme.text(activity, "", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        header.addView(status, new LayoutParams(0, -2, 1));
        refresh = MaiChatTypography.button(activity);
        refresh.setText("刷新");
        refresh.setTextColor(MaiChatTheme.BLUE);
        refresh.setOnClickListener(view -> load());
        header.addView(refresh, new LayoutParams(-2, dp(44)));
        addView(header, new LayoutParams(-1, -2));
        ScrollView scroll = new ScrollView(activity);
        scroll.setFillViewport(true);
        rows = new LinearLayout(activity);
        rows.setOrientation(VERTICAL);
        rows.setPadding(0, dp(12), 0, dp(24));
        scroll.addView(rows);
        addView(scroll, new LayoutParams(-1, 0, 1));
        load();
    }

    private void load() {
        if (closed || !service.isCurrent()) return;
        int request = ++generation;
        refresh.setEnabled(false);
        status.setText("正在读取举报记录…");
        worker.execute(() -> {
            try {
                List<MaiChatAccountSettingsService.Report> reports = service.reports();
                main.post(() -> {
                    if (!current(request)) return;
                    refresh.setEnabled(true);
                    status.setText(reports.isEmpty() ? "暂无举报记录" : "");
                    rows.removeAllViews();
                    for (MaiChatAccountSettingsService.Report report : reports) addReport(report);
                });
            } catch (Exception failure) {
                main.post(() -> {
                    if (!current(request)) return;
                    refresh.setEnabled(true);
                    status.setText("无法读取举报记录，请重试");
                });
            }
        });
    }

    private boolean current(int request) {
        return !closed && request == generation && service.isCurrent()
            && !activity.isFinishing() && !activity.isDestroyed();
    }

    private void addReport(MaiChatAccountSettingsService.Report report) {
        LinearLayout card = new LinearLayout(activity);
        card.setOrientation(VERTICAL);
        card.setPadding(dp(16), dp(16), dp(16), dp(16));
        card.setBackground(MaiChatTheme.rounded(Color.WHITE, 16, activity));
        card.addView(MaiChatTheme.label(activity,
            "resolved".equals(report.status) ? "已处理" : "处理中", MaiChatTypographyTokens.Role.BODY, MaiChatTheme.TEXT));
        TextView detail = MaiChatTheme.text(activity,
            report.resolution == null || report.resolution.isEmpty()
                ? "我们会尽快核查并处理。" : report.resolution, MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        detail.setPadding(0, dp(8), 0, dp(8));
        card.addView(detail);
        TextView id = MaiChatTheme.text(activity, "编号：" + report.id, MaiChatTypographyTokens.Role.METADATA, MaiChatTheme.SECONDARY);
        id.setTextIsSelectable(true);
        card.addView(id);
        LayoutParams params = new LayoutParams(-1, -2);
        params.bottomMargin = dp(12);
        rows.addView(card, params);
    }

    public void close() {
        if (closed) return;
        closed = true;
        generation++;
        worker.shutdownNow();
    }
    @Override protected void onDetachedFromWindow() { close(); super.onDetachedFromWindow(); }
    private int dp(float value) { return MaiChatTheme.dp(getContext(), value); }
}
