// 文件作用：普通界面与工具共用的头像上传/SDK 提交状态链；每个异步边界检查账号归属。
package com.kongshang.maichat;

import java.io.File;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

final class RemoteIMAvatarUpdate {
    static final class Gate {
        private final AtomicBoolean active = new AtomicBoolean();
        boolean tryAcquire() { return active.compareAndSet(false, true); }
        void release() { active.set(false); }
    }
    interface Uploader { String upload(File image, BooleanSupplier current) throws Exception; }
    interface ProfileSetter { void set(String url, Completion completion); }
    interface Completion { void finished(String error); }
    private static final String CHANGED = "登录账号已变化，请重新选择头像";
    private static final String FAILED = "头像更换尚未确认，请检查网络后重试";
    private final BooleanSupplier current;
    private final Uploader uploader;
    private final ProfileSetter setter;
    private final Consumer<String> installed;
    private final Executor callbacks;
    private final AtomicBoolean completed = new AtomicBoolean();

    RemoteIMAvatarUpdate(BooleanSupplier current, Uploader uploader, ProfileSetter setter,
                         Consumer<String> installed, Executor callbacks) {
        this.current = current; this.uploader = uploader; this.setter = setter;
        this.installed = installed; this.callbacks = callbacks;
    }
    // 调用方在 I/O worker 执行上传，SDK 提交与最终状态都在宿主回调执行器执行。
    void start(File image, Completion completion) {
        if (!current.getAsBoolean()) { callbacks.execute(() -> finish(null, CHANGED, completion)); return; }
        String url;
        try { url = uploader.upload(image, current); }
        catch (Exception failure) {
            // 不转发签名 URL、token、请求体或底层异常 message。
            callbacks.execute(() -> finish(null, FAILED, completion));
            return;
        }
        callbacks.execute(() -> {
            if (!current.getAsBoolean()) { finish(null, CHANGED, completion); return; }
            try {
                setter.set(url, error -> callbacks.execute(() -> finish(url, error, completion)));
            } catch (RuntimeException failure) { finish(null, FAILED, completion); }
        });
    }
    private void finish(String url, String error, Completion completion) {
        if (!completed.compareAndSet(false, true)) return;
        if (!current.getAsBoolean()) { completion.finished(CHANGED); return; }
        String result = error;
        if (result == null) {
            try { installed.accept(url); }
            catch (RuntimeException failure) { result = FAILED; }
        }
        completion.finished(result);
    }
}
