// 文件作用：既有 native HTTP 核心的有界 JSON POST / 图片 PUT 适配，不实现另一套网络栈。
// 系统证书、代理与取消代际由 Android 提供；native 与账号/头像 GET 共用同一 performCurl。
package com.kongshang.maichat;

import java.io.IOException;
import java.net.URL;
import java.util.function.BooleanSupplier;

final class MaiChatBinaryHttpTransport {
    static final class Result {
        final int status; final byte[] body; final int curlCode, failureKind;
        Result(int status, byte[] body, int curlCode, int failureKind) {
            this.status = status; this.body = body; this.curlCode = curlCode; this.failureKind = failureKind;
        }
    }
    private MaiChatBinaryHttpTransport() { }

    static Result perform(String endpoint, String method, byte[] body, String bearer,
                          String contentType, BooleanSupplier current) throws Exception {
        if (current == null || !current.getAsBoolean()) throw new IOException("登录账号已变化");
        URL url = new URL(endpoint);
        if (!"https".equals(url.getProtocol()) || url.getHost().isEmpty() || url.getUserInfo() != null
            || url.getRef() != null || endpoint.length() > 8192 || bearer == null
            || !bearer.matches("[!-~]{0,8192}") || body == null || body.length == 0
            || !("POST".equals(method) && "application/json".equals(contentType) && body.length <= 32 * 1024
                || "PUT".equals(method) && bearer.isEmpty() && "image/jpeg".equals(contentType)
                    && body.length <= 2_000_000))
            throw new IOException("上传网络参数无效");
        MaiChatAuthTransport.ProxyRoute proxy = MaiChatAuthTransport.selectedProxy(url);
        byte[] ca = SystemCertificateBundle.currentPem();
        try { System.loadLibrary("maichat_agent"); }
        catch (LinkageError failure) { throw new IOException("头像上传网络组件不可用"); }
        Result result = nativeExecute(endpoint, method, body, bearer, contentType, ca,
            proxy.type, proxy.host, proxy.port, current);
        if (!current.getAsBoolean()) throw new IOException("登录账号已变化");
        if (result == null || result.curlCode != 0) throw new IOException("头像上传连接失败，请稍后重试");
        return result;
    }

    private static native Result nativeExecute(String url, String method, byte[] body, String bearer,
        String contentType, byte[] ca, int proxyType, String proxyHost, int proxyPort, BooleanSupplier current);
}
