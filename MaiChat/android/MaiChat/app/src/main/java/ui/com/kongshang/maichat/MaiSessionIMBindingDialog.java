// 文件作用：本机会话绑定选择器；取消只关闭界面，所有读写经同一普通绑定服务。
package com.kongshang.maichat;

import android.app.AlertDialog;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;
import android.widget.ScrollView;
import java.util.function.BooleanSupplier;
import org.json.JSONArray;
import org.json.JSONObject;

public final class MaiSessionIMBindingDialog {
    private final MaiSessionIMBindingService.Gateway gateway;
    private final BooleanSupplier current;
    private final Runnable changed;
    private final AlertDialog dialog;
    private final TextView status, retry;
    private final RadioGroup options;
    private final ScrollView scroll;
    private final Handler main = new Handler(Looper.getMainLooper());
    private String selected = "", bound = "";
    private boolean busy, closed;
    private int revision;
    private JSONObject displayed;
    private AutoCloseable subscription;

    public static AlertDialog show(Context context, MaiSessionIMBindingService.Gateway gateway,
            BooleanSupplier current, Runnable changed) {
        return new MaiSessionIMBindingDialog(context, gateway, current, changed).open();
    }

    private MaiSessionIMBindingDialog(Context context, MaiSessionIMBindingService.Gateway gateway,
            BooleanSupplier current, Runnable changed) {
        this.gateway = gateway; this.current = current; this.changed = changed;
        LinearLayout body = new LinearLayout(context); body.setOrientation(LinearLayout.VERTICAL);
        int inset = MaiChatTheme.dp(context, 20); body.setPadding(inset, inset / 2, inset, inset / 2);
        body.setTag("session-im-binding-dialog");
        status = MaiChatTheme.text(context, "正在加载本机会话…", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        status.setTag("session-im-binding-status"); body.addView(status);
        options = new RadioGroup(context); options.setOrientation(LinearLayout.VERTICAL);
        options.setTag("session-im-binding-options");
        scroll = new ScrollView(context) {
            @Override protected void onMeasure(int width, int height) {
                int maximum = MaiChatTheme.dp(getContext(), 320);
                super.onMeasure(width, View.MeasureSpec.makeMeasureSpec(
                    Math.min(maximum, View.MeasureSpec.getMode(height) == View.MeasureSpec.UNSPECIFIED ? maximum : View.MeasureSpec.getSize(height)),
                    View.MeasureSpec.AT_MOST));
            }
        };
        scroll.setTag("session-im-binding-scroll"); scroll.addView(options); body.addView(scroll);
        retry = MaiChatTheme.text(context, "重试", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.BLUE);
        retry.setTag("session-im-binding-retry"); retry.setPadding(0, inset / 2, 0, inset / 2);
        retry.setOnClickListener(view -> load()); body.addView(retry);
        dialog = new AlertDialog.Builder(context).setTitle("绑定会话").setView(body)
            .setNegativeButton("取消", null).setPositiveButton("绑定", null).setNeutralButton("解除绑定", null).create();
        dialog.setOnDismissListener(ignored -> {
            closed = true; revision++;
            if (subscription != null) try { subscription.close(); } catch (Exception ignoredClose) { }
        });
    }

    private AlertDialog open() {
        dialog.show();
        // show 返回后按钮已创建；在返回入口前进入 loading，避免首帧出现可点击的默认绑定按钮。
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> mutate(false));
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(view -> mutate(true));
        subscription = gateway.subscribe(value -> dispatch(() -> {
            if (!active()) return;
            if (value == null) {
                selected = ""; displayed = null; options.removeAllViews();
                status.setText("会话状态正在更新，请稍候…"); updateActions();
            } else if (value.optBoolean("ok")) display(value);
        }));
        load();
        return dialog;
    }
    private boolean active() {
        if (closed) return false;
        if (current.getAsBoolean()) return true;
        dialog.dismiss(); return false;
    }
    private void load() {
        if (!active() || busy) return;
        busy = true; int ticket = ++revision;
        status.setText("正在加载本机会话…"); retry.setVisibility(View.GONE); updateActions();
        gateway.load((value, error) -> dispatch(() -> {
            if (!active() || ticket != revision) return;
            busy = false;
            if (error != null || value == null || !value.optBoolean("ok")) {
                status.setText(error == null || error.isEmpty() ? "会话读取失败，请重试" : error);
                retry.setVisibility(View.VISIBLE); updateActions(); return;
            }
            display(value);
        }));
    }

    private void display(JSONObject projection) {
        String previous = selected;
        boolean initial = displayed == null;
        boolean authorityChanged = !initial && !MaiSessionIMBindingService.sameAuthority(displayed, projection);
        String previousWorkspace = workspace(displayed, previous);
        int scrollPosition = scroll.getScrollY();
        bound = ""; String boundTitle = "", boundPhase = "";
        JSONArray bindings = projection.optJSONArray("bindings");
        if (bindings != null) for (int index = 0; index < bindings.length(); index++) {
            JSONObject row = bindings.optJSONObject(index);
            if (row != null && gateway.peerId().equals(row.optString("peer_id"))) {
                bound = row.optString("session_id"); boundTitle = row.optString("session_title"); boundPhase = row.optString("history_state"); break;
            }
        }
        selected = initial ? bound : previous;
        boolean selectedExists = false;
        JSONArray available = projection.optJSONArray("sessions");
        if (available != null) for (int index = 0; index < available.length(); index++)
            if (available.optJSONObject(index) != null && selected.equals(available.optJSONObject(index).optString("id"))) selectedExists = true;
        boolean workspaceChanged = !initial && !previous.isEmpty() && !previousWorkspace.equals(workspace(projection, previous));
        if (authorityChanged || workspaceChanged || !selectedExists) selected = "";
        options.removeAllViews(); displayed = MaiSessionIMBindingService.copy(projection);
        JSONArray sessions = projection.optJSONArray("sessions");
        if (sessions != null) for (int index = 0; index < sessions.length(); index++) {
            JSONObject session = sessions.optJSONObject(index);
            if (session == null || session.optString("id").isEmpty()) continue;
            String id = session.optString("id");
            RadioButton option = new RadioButton(options.getContext()); option.setId(View.generateViewId());
            option.setTag("session-im-binding-option:" + id);
            String title = session.optString("title").trim();
            option.setText((title.isEmpty() ? "未命名会话" : title) + (session.optBoolean("busy") ? " · 正在处理" : ""));
            option.setChecked(id.equals(selected));
            option.setOnClickListener(view -> { selected = id; updateActions(); });
            options.addView(option);
        }
        status.setText(authorityChanged || workspaceChanged ? "会话或目录已变化，请重新选择"
            : !initial && !previous.isEmpty() && !selectedExists ? "所选会话已不存在，请重新选择"
            : options.getChildCount() == 0 ? "暂无可绑定的本机会话，请先在创作中心新建会话"
            : bound.isEmpty() ? "选择一个本机会话，新收到的好友消息将交给它处理"
            : (boundPhase.equals("ready") ? "已绑定：" : "正在导入历史：") + (boundTitle.isEmpty() ? "未命名会话" : boundTitle));
        retry.setVisibility(View.GONE); updateActions();
        scroll.post(() -> { if (!closed) scroll.scrollTo(0, scrollPosition); });
    }

    private void mutate(boolean unbind) {
        if (!active() || busy || (unbind ? bound.isEmpty() : selected.isEmpty())) return;
        busy = true; int ticket = ++revision; updateActions();
        java.util.function.BiConsumer<JSONObject, String> completion = (value, error) -> dispatch(() -> {
            if (!active() || ticket != revision) return;
            busy = false;
            if (error != null || value == null || !value.optBoolean("ok")) {
                status.setText(error == null || error.isEmpty() ? "绑定修改未确认，请重试" : error);
                retry.setVisibility(View.VISIBLE); updateActions(); return;
            }
            changed.run(); dialog.dismiss();
        });
        if (unbind) gateway.unbind(completion); else gateway.bind(selected, displayed, completion);
    }

    private void updateActions() {
        dialog.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(!busy && !selected.isEmpty() && !selected.equals(bound));
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setVisibility(bound.isEmpty() ? View.GONE : View.VISIBLE);
        dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setEnabled(!busy && !bound.isEmpty());
        for (int index = 0; index < options.getChildCount(); index++) options.getChildAt(index).setEnabled(!busy);
    }
    private void dispatch(Runnable action) {
        if (Looper.myLooper() == Looper.getMainLooper()) action.run(); else main.post(action);
    }
    private static String workspace(JSONObject projection, String id) {
        JSONArray sessions = projection == null ? null : projection.optJSONArray("sessions");
        if (sessions != null) for (int index = 0; index < sessions.length(); index++) {
            JSONObject row = sessions.optJSONObject(index);
            if (row != null && id.equals(row.optString("id"))) return row.optString("workspace_key");
        }
        return "";
    }
}
