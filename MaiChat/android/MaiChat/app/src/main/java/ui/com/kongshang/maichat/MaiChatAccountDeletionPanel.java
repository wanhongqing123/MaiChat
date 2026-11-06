// 文件作用：账号注销的本地安全表单；密码/验证码不经过模型协议，不保存表单草稿。
// 边界：网络只在 worker 运行；回执先保存到私有 no-backup 目录，用户确认回执后才退出账号。
package com.kongshang.maichat;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Application;
import android.content.Context;
import android.graphics.Color;
import android.os.Handler;
import android.os.Bundle;
import android.os.Looper;
import android.os.SystemClock;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.AtomicFile;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.text.DateFormat;
import java.util.Date;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.BooleanSupplier;
import org.json.JSONObject;

public final class MaiChatAccountDeletionPanel extends LinearLayout {
    private final Activity activity;
    private final MaiChatAccountSettingsService service;
    private final Runnable onAccepted;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService worker = Executors.newSingleThreadExecutor(r -> new Thread(r, "account-deletion"));
    private final LinearLayout form;
    private final TextView message;
    private final Button submit, retry;
    private EditText confirmation, password, code;
    private Button sendCode;
    private MaiChatAccountSettingsService.Options options;
    private String challengeId;
    private long resendAt, challengeExpiresAt;
    private boolean usePassword, busy, submitting, closed, receiptAccepted;
    private interface AcceptedRecorder { void record(MaiChatAccountSettingsService.Receipt receipt) throws Exception; }
    private AcceptedRecorder acceptedRecorder = receipt -> { };
    private BooleanSupplier receiptVisible;
    private AlertDialog danger;
    private final Runnable cooldownTick = new Runnable() {
        @Override public void run() {
            if (closed || !service.isCurrent()) return;
            updateControls();
            if (SystemClock.elapsedRealtime() < resendAt) main.postDelayed(this, 1000);
        }
    };

    public MaiChatAccountDeletionPanel(Activity activity, RemoteIMSessionController session, Runnable onAccepted) {
        this(activity, accountService(activity, session), onAccepted);
        String owner = session.chatState().ownerUserId(), account = session.accountId();
        String authIdentity = MaiChatAuthClient.shared(activity).accountSettingsIdentitySnapshot();
        acceptedRecorder = receipt -> session.rememberAcceptedAccountDeletion(owner, account, authIdentity, receipt.requestId);
        // 202 后会话可能先被定时校验撤销；仍可展示原回执，但不能覆盖另一个已登录账号。
        receiptVisible = () -> service.isCurrent() || session.requiresLogin();
    }

    private static MaiChatAccountSettingsService accountService(Activity activity, RemoteIMSessionController session) {
        String account = session.accountId(), identity = session.passwordChangeIdentity();
        BooleanSupplier current = () -> !activity.isDestroyed() && !session.requiresLogin()
            && identity.equals(session.passwordChangeIdentity());
        return new MaiChatAccountSettingsService(account, current,
            (path, method, body) -> MaiChatAuthClient.shared(activity).accountSettingsRequest(path, method, body, account, current));
    }

    // 依赖注入也用于本地 fixture 验证；生产构造始终绑定当前 Session 的认证代际。
    MaiChatAccountDeletionPanel(Activity activity, MaiChatAccountSettingsService service, Runnable onAccepted) {
        super(activity);
        this.activity = activity; this.onAccepted = onAccepted;
        this.service = service;
        this.receiptVisible = service::isCurrent;
        setOrientation(VERTICAL); setBackgroundColor(MaiChatTheme.GROUPED_PAGE);
        ScrollView scroll = new ScrollView(activity); scroll.setFillViewport(true);
        addView(scroll, new LayoutParams(-1, -1));
        form = new LinearLayout(activity); form.setOrientation(VERTICAL);
        form.setPadding(dp(18), dp(18), dp(18), dp(28)); scroll.addView(form);
        addText("提交后立即停用 MaiChat 账号并撤销登录。好友关系、云端会话和可识别素材会进入清理流程；依法必须保留的记录按期限处理。", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        message = addText("正在读取注销确认方式…", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        retry = button("重新加载", () -> loadOptions()); form.addView(retry, row());
        submit = button("提交注销申请", this::confirmSubmission);
        submit.setTextColor(MaiChatTheme.RED); submit.setEnabled(false);
        loadOptions();
    }

    public void refreshIdentity() { if (!service.isCurrent()) close(); }
    public boolean isSubmitting() { return submitting; }
    /** 工具适配器只能读取这份脱敏状态；不提供密码、验证码、确认 ID 或提交方法。 */
    public JSONObject safeStatus() throws Exception {
        String status = !service.isCurrent() ? "identity_changed" : receiptAccepted ? "accepted"
            : submitting ? "submitting" : closed ? "closed" : options == null ? "loading" : "awaiting_user";
        return new JSONObject().put("opened", !closed && isAttachedToWindow())
            .put("status", status).put("accepted", receiptAccepted);
    }

    private void loadOptions() {
        if (closed || busy || options != null) return;
        busy = true; retry.setEnabled(false); message.setText("正在读取注销确认方式…");
        worker.execute(() -> {
            try {
                MaiChatAccountSettingsService.Options loaded = service.options();
                main.post(() -> {
                    if (!active()) return;
                    options = loaded; busy = false; retry.setVisibility(GONE);
                    message.setText("预计在 " + options.processingDays + " 天内完成。完成后不能恢复账号及资料。");
                    buildFields(); updateControls();
                });
            } catch (Exception failure) {
                main.post(() -> { if (active()) { busy = false; retry.setEnabled(true);
                    message.setText("无法读取注销确认方式，请检查网络后重试。"); } });
            }
        });
    }

    private void buildFields() {
        addText("确认身份", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        usePassword = options.hasPassword;
        if (options.hasPassword && options.phoneMasked != null) {
            RadioGroup modes = new RadioGroup(activity); modes.setOrientation(HORIZONTAL);
            RadioButton passwordMode = new RadioButton(activity); passwordMode.setId(View.generateViewId()); passwordMode.setText("登录密码");
            RadioButton smsMode = new RadioButton(activity); smsMode.setId(View.generateViewId()); smsMode.setText("短信验证码");
            modes.addView(passwordMode); modes.addView(smsMode); modes.check(passwordMode.getId()); form.addView(modes, row());
            modes.setOnCheckedChangeListener((group, selected) -> {
                usePassword = selected == passwordMode.getId();
                password.setText(""); code.setText(""); updateControls();
            });
        }
        password = input("输入登录密码", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        password.setSaveEnabled(false); password.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
        if (options.phoneMasked != null) addText("已绑定手机号：" + options.phoneMasked, MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        code = input("输入 6 位验证码", InputType.TYPE_CLASS_NUMBER);
        code.setSaveEnabled(false); code.setFilters(new InputFilter[] {new InputFilter.LengthFilter(6)});
        sendCode = button("获取验证码", this::requestCode); form.addView(sendCode, row());
        addText("确认 MaiChat ID", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.SECONDARY);
        TextView account = addText(service.accountId(), MaiChatTypographyTokens.Role.BODY, MaiChatTheme.TEXT); account.setTextIsSelectable(true);
        confirmation = input("输入上方 MaiChat ID", InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_VISIBLE_PASSWORD);
        confirmation.setSaveEnabled(false);
        if (!options.hasPassword && options.phoneMasked == null)
            addText("当前账号没有可用的核验方式，请联系支持处理。", MaiChatTypographyTokens.Role.DETAIL, MaiChatTheme.RED);
        form.addView(submit, row());
    }

    private void requestCode() {
        if (!active() || busy || options == null || options.phoneMasked == null || SystemClock.elapsedRealtime() < resendAt) return;
        busy = true; updateControls(); message.setText("正在发送注销验证码…");
        worker.execute(() -> {
            try {
                MaiChatAccountSettingsService.Challenge issued = service.requestCode();
                main.post(() -> { if (active()) {
                    busy = false; challengeId = issued.challengeId; code.setText("");
                    long now = SystemClock.elapsedRealtime(); resendAt = now + issued.resendAfter * 1000L;
                    challengeExpiresAt = now + issued.expiresIn * 1000L;
                    message.setText("验证码已发送，请填写后再次确认 MaiChat ID。"); cooldownTick.run();
                } });
            } catch (Exception failure) { main.post(() -> { if (active()) {
                busy = false; message.setText("验证码发送失败，请稍后重试。"); updateControls();
            } }); }
        });
    }

    private boolean canSubmit() {
        return !busy && confirmation != null && (usePassword || SystemClock.elapsedRealtime() < challengeExpiresAt)
            && service.canSubmit(confirmation.getText().toString(), usePassword ? password.getText().toString() : "",
                usePassword ? null : challengeId, usePassword ? null : code.getText().toString());
    }

    private void confirmSubmission() {
        if (!active() || !canSubmit()) return;
        danger = new AlertDialog.Builder(activity).setTitle("确认永久注销这个账号？")
            .setMessage("提交后立即停止账号使用，并开始清理关联数据。完成后不能恢复账号及资料。")
            .setNegativeButton("取消", null).setPositiveButton("确认提交", null).create();
        danger.setOnShowListener(ignored -> {
            danger.getButton(AlertDialog.BUTTON_POSITIVE).setTextColor(MaiChatTheme.RED);
            danger.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> submit());
        });
        danger.setOnDismissListener(ignored -> { if (!submitting) danger = null; }); danger.show();
    }

    private void submit() {
        if (!active() || submitting || !canSubmit()) return;
        String confirmed = confirmation.getText().toString();
        String secret = usePassword ? password.getText().toString() : "";
        String challenge = usePassword ? null : challengeId, digits = usePassword ? null : code.getText().toString();
        busy = submitting = true;
        danger.setCancelable(false); danger.getButton(AlertDialog.BUTTON_NEGATIVE).setEnabled(false);
        danger.getButton(AlertDialog.BUTTON_POSITIVE).setEnabled(false); danger.setMessage("正在提交注销申请，请等待明确回执…");
        password.setText(""); code.setText(""); updateControls();
        worker.execute(() -> {
            try {
                MaiChatAccountSettingsService.Receipt receipt = service.submitDeletion(confirmed, secret, challenge, digits);
                boolean saved;
                try { saveReceipt(activity.getApplicationContext(), receipt); saved = true; }
                catch (Exception storageFailure) { saved = false; }
                try { acceptedRecorder.record(receipt); }
                catch (Exception pendingStorageFailure) { saved = false; }
                boolean stored = saved;
                main.post(() -> {
                    submitting = busy = false; dismissDanger();
                    if (activity.isFinishing() || activity.isDestroyed() || !receiptVisible.getAsBoolean()) return;
                    receiptAccepted = true;
                    showReceipt(activity, receipt, onAccepted, stored);
                });
            } catch (Exception failure) { main.post(() -> {
                submitting = busy = false; dismissDanger();
                if (active()) { message.setText("注销申请尚未确认，请核对密码或验证码后重试。未收到回执前请勿认为注销已完成。"); updateControls(); }
            }); }
        });
    }

    private void updateControls() {
        if (password == null) return;
        password.setVisibility(usePassword ? VISIBLE : GONE); password.setEnabled(!busy);
        code.setVisibility(!usePassword && options.phoneMasked != null ? VISIBLE : GONE); code.setEnabled(!busy);
        sendCode.setVisibility(!usePassword && options.phoneMasked != null ? VISIBLE : GONE);
        long seconds = Math.max(0, (resendAt - SystemClock.elapsedRealtime() + 999) / 1000);
        sendCode.setText(seconds > 0 ? seconds + " 秒后重发" : challengeId == null ? "获取验证码" : "重新获取");
        sendCode.setEnabled(!busy && seconds == 0); confirmation.setEnabled(!busy); submit.setEnabled(canSubmit());
    }

    private boolean active() { return !closed && !activity.isFinishing() && !activity.isDestroyed() && service.isCurrent(); }
    public void close() {
        if (closed) return;
        closed = true; main.removeCallbacks(cooldownTick);
        if (password != null) password.setText(""); if (code != null) code.setText("");
        dismissDanger();
        // 已提交的 HTTP 不是可撤回操作；在途结果仍保存回执，但不能写入别的账号页面。
        worker.shutdown();
    }
    @Override protected void onDetachedFromWindow() { close(); super.onDetachedFromWindow(); }
    private void dismissDanger() { AlertDialog previous = danger; danger = null; if (previous != null) previous.dismiss(); }

    private EditText input(String hint, int inputType) {
        EditText view = MaiChatTypography.input(activity); view.setSingleLine(); view.setHint(hint); view.setInputType(inputType);
        view.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) { if (confirmation != null) updateControls(); }
            @Override public void afterTextChanged(Editable value) { }
        });
        form.addView(view, row()); return view;
    }
    private TextView addText(String value, MaiChatTypographyTokens.Role role, int color) {
        TextView view = MaiChatTheme.text(activity, value, role, color); form.addView(view, row()); return view;
    }
    private Button button(String title, Runnable action) { Button button = MaiChatTypography.button(activity); button.setText(title);
        button.setOnClickListener(view -> action.run()); return button; }
    private LayoutParams row() { LayoutParams params = new LayoutParams(-1, -2); params.bottomMargin = dp(12); return params; }
    private int dp(int value) { return MaiChatTheme.dp(activity, value); }

    private static File receiptFile(Context context) { return new File(context.getNoBackupFilesDir(), "account-deletion-receipt.json"); }
    public static boolean hasSavedReceipt(Context context) {
        File file = receiptFile(context);
        return file.isFile() || new File(file.getPath() + ".bak").isFile();
    }
    private static void saveReceipt(Context context, MaiChatAccountSettingsService.Receipt receipt) throws Exception {
        AtomicFile file = new AtomicFile(receiptFile(context)); FileOutputStream output = file.startWrite();
        try { output.write(receipt.privateStorage().getBytes(StandardCharsets.UTF_8)); file.finishWrite(output); }
        catch (Exception failure) { file.failWrite(output); throw failure; }
    }
    public static void showSavedReceipt(Activity activity, Runnable onAcknowledged) {
        new Thread(() -> {
            try {
                ByteArrayOutputStream bytes = new ByteArrayOutputStream();
                try (FileInputStream input = new AtomicFile(receiptFile(activity)).openRead()) {
                    byte[] buffer = new byte[1024]; int count;
                    while ((count = input.read(buffer)) != -1) { if (bytes.size() + count > 8192) throw new java.io.IOException(); bytes.write(buffer, 0, count); }
                }
                MaiChatAccountSettingsService.Receipt receipt = MaiChatAccountSettingsService.parseReceipt(
                    new JSONObject(bytes.toString(StandardCharsets.UTF_8.name())));
                activity.runOnUiThread(() -> { if (!activity.isFinishing() && !activity.isDestroyed()) showReceipt(activity, receipt, onAcknowledged, true); });
            } catch (Exception failure) { activity.runOnUiThread(() -> { if (!activity.isDestroyed())
                Toast.makeText(activity, "无法读取注销回执，请稍后重试", Toast.LENGTH_SHORT).show(); }); }
        }, "account-deletion-receipt").start();
    }
    private static void showReceipt(Activity activity, MaiChatAccountSettingsService.Receipt receipt,
                                    Runnable acknowledged, boolean stored) {
        String description = receiptText(receipt) + (stored ? "" : "\n本机回执保存失败，请记录申请编号后联系支持。");
        AlertDialog dialog = new AlertDialog.Builder(activity).setTitle("注销申请回执").setMessage(description)
            .setCancelable(false).setPositiveButton("知道了", (ignored, which) -> { if (acknowledged != null) acknowledged.run(); })
            .setNeutralButton("查询进度", null).create();
        // 回执可跨表单离页保留，但不能跨 Activity 销毁保留窗口。
        Application.ActivityLifecycleCallbacks lifetime = new Application.ActivityLifecycleCallbacks() {
            @Override public void onActivityDestroyed(Activity owner) { if (owner == activity) dialog.dismiss(); }
            @Override public void onActivityCreated(Activity owner, Bundle state) { }
            @Override public void onActivityStarted(Activity owner) { }
            @Override public void onActivityResumed(Activity owner) { }
            @Override public void onActivityPaused(Activity owner) { }
            @Override public void onActivityStopped(Activity owner) { }
            @Override public void onActivitySaveInstanceState(Activity owner, Bundle state) { }
        };
        activity.getApplication().registerActivityLifecycleCallbacks(lifetime);
        dialog.setOnDismissListener(ignored -> activity.getApplication().unregisterActivityLifecycleCallbacks(lifetime));
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setOnClickListener(view -> {
            dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setEnabled(false);
            new Thread(() -> {
                try {
                    MaiChatAccountSettingsService.Receipt updated = MaiChatAccountSettingsService.refreshReceipt(receipt,
                        (path, method, body) -> MaiChatAuthClient.shared(activity).accountSettingsRequest(path, method, body, null, () -> true));
                    saveReceipt(activity.getApplicationContext(), updated);
                    activity.runOnUiThread(() -> { if (dialog.isShowing() && !activity.isDestroyed()) {
                        dialog.setMessage(receiptText(updated)); dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setEnabled(true);
                    } });
                } catch (Exception failure) { activity.runOnUiThread(() -> { if (dialog.isShowing() && !activity.isDestroyed()) {
                    dialog.getButton(AlertDialog.BUTTON_NEUTRAL).setEnabled(true);
                    Toast.makeText(activity, "查询失败，请稍后重试", Toast.LENGTH_SHORT).show();
                } }); }
            }, "account-deletion-status").start();
        })); dialog.show();
    }
    private static String receiptText(MaiChatAccountSettingsService.Receipt receipt) {
        return (receipt.status.equals("completed") ? "账号注销已处理完成。" : "注销申请已受理，账号已停用；关联数据清理尚在处理中。")
            + "\n申请编号：" + receipt.requestId + "\n预计处理期限：" + DateFormat.getDateTimeInstance().format(new Date(receipt.dueAt))
            + "\n支持邮箱：" + receipt.supportEmail;
    }
}
