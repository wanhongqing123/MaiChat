// 文件作用：注销表单与举报列表共用的固定账号合同；同步请求由宿主 worker 执行。
// 边界：敏感输入只存在于普通请求参数；safeSummary 永不含密码、验证码或回执令牌。
package com.kongshang.maichat;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import org.json.JSONArray;
import org.json.JSONObject;

public final class MaiChatAccountSettingsService {
    public interface Request { JSONObject call(String path, String method, JSONObject body) throws Exception; }
    public static final class Options {
        public final boolean hasPassword;
        public final String phoneMasked;
        public final int processingDays;
        private Options(boolean password, String phone, int days) { hasPassword = password; phoneMasked = phone; processingDays = days; }
    }
    public static final class Challenge {
        public final String challengeId;
        public final int expiresIn, resendAfter;
        private Challenge(String id, int expires, int resend) { challengeId = id; expiresIn = expires; resendAfter = resend; }
    }
    public static final class Report {
        public final String id, status, reason, resolution;
        public final long createdAt;
        private Report(JSONObject value) throws Exception {
            id = required(value, "id", 128); status = required(value, "status", 40);
            reason = value.optString("reason", ""); resolution = optional(value, "resolution");
            createdAt = number(value, "created_at");
        }
    }
    public static final class Receipt {
        public final String requestId, status, supportEmail;
        public final long dueAt;
        private final String token;
        private Receipt(String id, String status, long due, String support, String token) {
            requestId = id; this.status = status; dueAt = due; supportEmail = support; this.token = token;
        }
        public JSONObject safeSummary() throws Exception {
            return new JSONObject().put("request_id", requestId).put("status", status)
                .put("due_at", dueAt).put("support_email", supportEmail);
        }
        // 仅供 App 私有回执存储与无登录状态查询；绝不用于工具输出、日志或测试产物。
        String privateStorage() throws Exception { return safeSummary().put("receipt_token", token).toString(); }
        JSONObject statusBody() throws Exception { return new JSONObject().put("request_id", requestId).put("receipt_token", token); }
    }

    private final String account;
    private final BooleanSupplier current;
    private final Request request;
    private final AtomicBoolean writing = new AtomicBoolean();
    private volatile Options loadedOptions;
    private volatile boolean accepted;

    public MaiChatAccountSettingsService(String accountId, BooleanSupplier current, Request request) {
        if (accountId == null || accountId.isEmpty() || current == null || request == null)
            throw new IllegalArgumentException("Account settings require an authenticated identity");
        account = accountId; this.current = current; this.request = request;
    }
    public String accountId() { return account; }
    public boolean isCurrent() { return current.getAsBoolean(); }
    private void requireCurrent() throws IOException {
        if (!isCurrent()) throw new IOException("登录账号已变化，请重新打开设置页");
    }

    public Options options() throws Exception {
        requireCurrent();
        if (loadedOptions != null) return loadedOptions;
        JSONObject result = request.call("auth/account/deletion/options", "GET", null);
        requireCurrent();
        if (!(result.opt("has_password") instanceof Boolean)) throw invalid();
        long days = number(result, "processing_days");
        String phone = optional(result, "phone_masked");
        if (days < 1 || days > 365 || phone != null && (phone.isEmpty() || phone.length() > 64)) throw invalid();
        return loadedOptions = new Options(result.getBoolean("has_password"), phone, (int) days);
    }

    public Challenge requestCode() throws Exception {
        requireCurrent();
        if (loadedOptions == null || loadedOptions.phoneMasked == null || accepted || !writing.compareAndSet(false, true))
            throw new IOException("当前无法发送注销验证码，请稍后重试");
        try {
            requireCurrent();
            JSONObject result = request.call("auth/account/deletion/challenge", "POST", new JSONObject());
            requireCurrent();
            String id = required(result, "challenge_id", 128);
            long expires = number(result, "expires_in"), resend = number(result, "resend_after");
            if (expires < 1 || expires > 3600 || resend < 0 || resend > 3600) throw invalid();
            return new Challenge(id, (int) expires, (int) resend);
        } finally { writing.set(false); }
    }

    public boolean canSubmit(String confirmation, String password, String challenge, String code) {
        Options options = loadedOptions;
        if (!isCurrent() || options == null || accepted || writing.get() || !account.equals(confirmation)) return false;
        if (password != null && !password.isEmpty())
            return options.hasPassword && password.codePointCount(0, password.length()) <= 128
                && password.getBytes(StandardCharsets.UTF_8).length <= 512;
        return options.phoneMasked != null && challenge != null && !challenge.isEmpty()
            && challenge.length() <= 128 && code != null && code.matches("[0-9]{6}");
    }

    public Receipt submitDeletion(String confirmation, String password, String challenge, String code) throws Exception {
        requireCurrent();
        if (!canSubmit(confirmation, password, challenge, code) || !writing.compareAndSet(false, true))
            throw new IOException("请确认当前 MaiChat ID，并填写密码或六位验证码");
        try {
            requireCurrent();
            JSONObject body = new JSONObject().put("confirm_account_id", account);
            if (password != null && !password.isEmpty()) body.put("password", password);
            else body.put("challenge_id", challenge).put("code", code);
            // 已发送的注销不能因离页/切号伪称取消。明确回执仍交给原调用方保存；UI 再核对身份。
            Receipt receipt = parseReceipt(request.call("auth/account/deletion", "POST", body));
            if (!"pending".equals(receipt.status)) throw invalid();
            accepted = true;
            return receipt;
        } finally { writing.set(false); }
    }

    public List<Report> reports() throws Exception {
        requireCurrent();
        JSONObject result = request.call("safety/reports", "GET", null);
        requireCurrent();
        JSONArray records = result.getJSONArray("reports");
        if (records.length() > 1000) throw invalid();
        List<Report> reports = new ArrayList<>();
        for (int i = 0; i < records.length(); i++) reports.add(new Report(records.getJSONObject(i)));
        return reports;
    }

    static Receipt parseReceipt(JSONObject value) throws Exception {
        String id = required(value, "request_id", 36), status = required(value, "status", 20);
        String token = required(value, "receipt_token", 43), support = required(value, "support_email", 254);
        long due = number(value, "due_at");
        if (!id.matches("[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")
            || !token.matches("[A-Za-z0-9_-]{43}") || !(status.equals("pending") || status.equals("completed"))
            || due <= 0 || !support.matches("[^\\s@]+@[^\\s@]+")) throw invalid();
        return new Receipt(id, status, due, support, token);
    }

    static Receipt refreshReceipt(Receipt receipt, Request request) throws Exception {
        JSONObject updated = request.call("auth/account/deletion/status", "POST", receipt.statusBody());
        if (!receipt.requestId.equals(updated.optString("request_id"))) throw invalid();
        updated.put("receipt_token", receipt.token);
        return parseReceipt(updated);
    }

    private static String required(JSONObject value, String field, int max) throws Exception {
        Object raw = value.opt(field);
        if (!(raw instanceof String) || ((String) raw).isEmpty() || ((String) raw).length() > max) throw invalid();
        return (String) raw;
    }
    private static String optional(JSONObject value, String field) throws Exception {
        Object raw = value.opt(field);
        if (raw == null || raw == JSONObject.NULL) return null;
        if (!(raw instanceof String)) throw invalid();
        return (String) raw;
    }
    private static long number(JSONObject value, String field) throws Exception {
        Object raw = value.opt(field);
        if (!(raw instanceof Number) || ((Number) raw).doubleValue() != ((Number) raw).longValue()) throw invalid();
        return ((Number) raw).longValue();
    }
    private static IOException invalid() { return new IOException("账号服务返回的数据无效，请重试"); }
}
