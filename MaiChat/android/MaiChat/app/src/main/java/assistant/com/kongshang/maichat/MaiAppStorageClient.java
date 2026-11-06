// 文件作用：设置页调用共享存储普通 C ABI；不经过 Agent/tool 请求。
package com.kongshang.maichat;

import java.nio.charset.StandardCharsets;
import org.json.JSONObject;

public final class MaiAppStorageClient implements AutoCloseable {
    static { System.loadLibrary("maichat_agent"); }
    private final java.util.concurrent.atomic.AtomicLong handle = new java.util.concurrent.atomic.AtomicLong();
    public MaiAppStorageClient(AIAssistantController.StorageRoots roots) throws Exception {
        if (!roots.current.getAsBoolean()) throw new IllegalStateException("账号已切换，请重新打开存储空间");
        handle.set(nativeCreate(roots.temporary.getPath().getBytes(StandardCharsets.UTF_8),
            roots.cache.getPath().getBytes(StandardCharsets.UTF_8),
            roots.workspace.getPath().getBytes(StandardCharsets.UTF_8)));
        if (handle.get() == 0) throw new IllegalStateException("无法打开存储空间");
    }
    public JSONObject request(JSONObject request) throws Exception {
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper())
            throw new IllegalStateException("存储扫描不能阻塞界面");
        byte[] response = nativeRequest(handle.get(), request.toString().getBytes(StandardCharsets.UTF_8));
        if (response == null) throw new IllegalStateException("存储空间暂不可用");
        JSONObject envelope = new JSONObject(new String(response, StandardCharsets.UTF_8));
        if (!envelope.optBoolean("ok")) throw new IllegalStateException(envelope.optString("error", "存储操作失败"));
        return envelope.getJSONObject("output");
    }
    // C ABI 允许 destroy 与 request 并发；不得加同一 Java 锁挡住核心取消信号。
    @Override public void close() { long previous = handle.getAndSet(0); if (previous != 0) nativeDestroy(previous); }
    private static native long nativeCreate(byte[] temporary, byte[] cache, byte[] workspace);
    private static native byte[] nativeRequest(long handle, byte[] request);
    private static native void nativeDestroy(long handle);
}
