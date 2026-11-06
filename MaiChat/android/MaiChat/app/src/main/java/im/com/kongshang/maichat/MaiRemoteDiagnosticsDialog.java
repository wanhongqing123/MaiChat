// User confirmation page for the same request/merge/forward workflow as iOS.
// Opening the page performs no IM write; only the explicit confirmation starts collection.
package com.kongshang.maichat;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.res.ColorStateList;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

final class MaiRemoteDiagnosticsDialog {
    private final Activity activity;
    private final RemoteIMSessionController session;
    private final RemoteIMContact peer;
    private final List<RemoteIMContact> recipients = new ArrayList<>();
    private AlertDialog dialog;
    private Spinner picker;
    private TextView status;
    private ProgressBar spinner;
    private MaiRemoteDiagnosticsCoordinator coordinator;
    private String confirmationIdentity;
    private volatile boolean visible;
    private boolean running, sending;

    MaiRemoteDiagnosticsDialog(Activity activity, RemoteIMSessionController session, RemoteIMContact peer) {
        this.activity = activity; this.session = session; this.peer = peer;
    }

    void show() {
        if (activity.isFinishing() || activity.isDestroyed()) return;
        visible = true;
        confirmationIdentity = new MaiRemoteDiagnosticsTransport(activity, session,
            peer.userId(), () -> visible).identity();
        String owner = session.chatState().ownerUserId();
        for (RemoteIMContact contact : session.chatState().contacts())
            if (!owner.equals(contact.userId())) recipients.add(contact);
        LinearLayout body = new LinearLayout(activity);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(dp(20), dp(8), dp(20), dp(12));
        heading(body, "故障会话");
        body.addView(text(peer.displayName(), MaiChatTypographyTokens.Role.BODY, MaiChatTheme.TEXT), row());
        body.addView(text("收集 Android 本地现场，并请求该好友的 MultiAICode 回传近期诊断记录。" +
            "不需要选择问题类型或操作远端电脑。", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY), row());
        heading(body, "发送给排查好友");
        picker = new Spinner(activity);
        picker.setContentDescription("接收排障报告的好友");
        List<String> labels = new ArrayList<>();
        labels.add("请选择好友");
        int preferred = 0;
        for (int i = 0; i < recipients.size(); i++) {
            RemoteIMContact contact = recipients.get(i);
            labels.add(contact.displayName().equals(contact.userId()) ? contact.userId() :
                contact.displayName() + " (" + contact.userId() + ")");
            if (contact.userId().equals("mac-multi-ai-code") ||
                (preferred == 0 && contact.userId().equals("house-multi-ai-code"))) preferred = i + 1;
        }
        ArrayAdapter<String> choices = new ArrayAdapter<>(activity, android.R.layout.simple_spinner_item, labels);
        choices.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        picker.setAdapter(choices);
        picker.setSelection(preferred);
        body.addView(picker, new LinearLayout.LayoutParams(-1, dp(48)));
        body.addView(text("确认后将自动合并并发送给所选好友。报告仅含设备版本、消息方向、状态、时间及稳定标签，" +
            "不含聊天正文、登录凭据或环境变量。当前未采集延迟、界面性能或原始日志，缺失证据会明确标注。" +
            "采集请求会作为一条消息留在当前聊天。", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY), row());
        spinner = new ProgressBar(activity);
        spinner.setIndeterminateTintList(ColorStateList.valueOf(MaiChatTheme.BLUE));
        spinner.setContentDescription("正在采集远程排障报告");
        spinner.setVisibility(View.GONE);
        body.addView(spinner, new LinearLayout.LayoutParams(dp(28), dp(28)));
        status = text(session.connectionState() == TencentIMClient.ConnectionState.CONNECTED ?
            "选择接收报告的好友后，确认收集并发送。" : "本机未连接 IM，连接后才能发送请求与报告。", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        status.setContentDescription("远程排障状态");
        body.addView(status, row());
        ScrollView scroll = new ScrollView(activity);
        scroll.addView(body);
        dialog = new AlertDialog.Builder(activity).setTitle("远程排障").setView(scroll)
            .setNegativeButton("关闭", null).setPositiveButton("确认收集并发送", null).create();
        dialog.setOnDismissListener(ignored -> {
            visible = false;
            if (coordinator != null) coordinator.cancel();
        });
        dialog.setOnShowListener(ignored -> {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(button -> confirm());
            dialog.getButton(AlertDialog.BUTTON_NEGATIVE).setOnClickListener(button -> {
                if (running && coordinator != null) coordinator.cancel();
                else dialog.dismiss();
            });
            updateControls();
        });
        picker.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                updateControls();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) { updateControls(); }
        });
        dialog.show();
        dialog.getWindow().setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private void confirm() {
        int selected = picker.getSelectedItemPosition() - 1;
        if (running || selected < 0 || selected >= recipients.size()) return;
        String recipient = recipients.get(selected).userId();
        String identity = confirmationIdentity;
        MaiRemoteDiagnosticsTransport operationTransport = new MaiRemoteDiagnosticsTransport(
            activity, session, peer.userId(), () -> visible);
        // This check runs synchronously with the user's confirmation. The
        // transport repeats it immediately before each later IM write.
        try {
            if (!operationTransport.mayContinue(identity, peer.userId(), recipient)) {
                status.setText("账号、连接或好友已变更，请关闭后重新确认。");
                return;
            }
        } catch (Exception invalid) {
            status.setText("当前排障上下文已失效，请关闭后重新确认。");
            return;
        }
        running = true;
        sending = false;
        updateControls();
        coordinator = new MaiRemoteDiagnosticsCoordinator(operationTransport, (message, active, submitting) ->
            activity.runOnUiThread(() -> {
                if (!visible || dialog == null || !dialog.isShowing()) return;
                running = active; sending = submitting;
                status.setText(message);
                updateControls();
            }));
        MaiRemoteDiagnosticsCoordinator operation = coordinator;
        ExecutorService worker = Executors.newSingleThreadExecutor(task -> new Thread(task, "maichat-remote-diagnostics"));
        worker.execute(() -> {
            try { operation.run(peer.userId(), recipient, identity, UUID.randomUUID().toString()); }
            finally { worker.shutdown(); }
        });
    }

    private void updateControls() {
        if (dialog == null || !dialog.isShowing()) return;
        Button confirm = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
        confirm.setEnabled(!running && picker.getSelectedItemPosition() > 0 &&
            session.connectionState() == TencentIMClient.ConnectionState.CONNECTED);
        picker.setEnabled(!running);
        spinner.setVisibility(running ? View.VISIBLE : View.GONE);
        Button cancel = dialog.getButton(AlertDialog.BUTTON_NEGATIVE);
        cancel.setText(running ? "取消回传" : "关闭");
        cancel.setEnabled(!sending);
        // Once the report is submitted, wait for its receipt or the bounded
        // timeout; dismissing cannot retract the normal IM attachment.
        dialog.setCancelable(!sending);
        dialog.setCanceledOnTouchOutside(!running);
    }

    private void heading(LinearLayout body, String title) {
        TextView heading = text(title, MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        heading.setTypeface(MaiChatTypography.semibold());
        heading.setPadding(0, dp(16), 0, dp(6));
        body.addView(heading, row());
    }

    private TextView text(String value, MaiChatTypographyTokens.Role role, int color) {
        return MaiChatTheme.text(activity, value, role, color);
    }

    private LinearLayout.LayoutParams row() {
        LinearLayout.LayoutParams row = new LinearLayout.LayoutParams(-1, -2);
        row.bottomMargin = dp(10);
        return row;
    }

    private int dp(int value) { return MaiChatTheme.dp(activity, value); }
}
