// 文件作用：用内存上传器/SDK 回调验证头像写入每个异步边界的账号归属，不连接网络。
package com.kongshang.maichat;

import java.io.File;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import org.junit.Test;
import static org.junit.Assert.*;

public class RemoteIMAvatarUpdateTest {
    private final AtomicBoolean current = new AtomicBoolean(true);
    private BooleanSupplier identity = current::get;
    private int uploads, commits, installs, completions;
    private String result, installed;
    private Object pending;
    private Class<?> completionType;
    private boolean switchDuringUpload, failUpload, failInstall;

    private void start(Executor callbacks) throws Exception {
        Class<?> type;
        try { type = Class.forName("com.kongshang.maichat.RemoteIMAvatarUpdate"); }
        catch (ClassNotFoundException error) { throw new AssertionError("Avatar upload must guard account identity at upload and SDK boundaries", error); }
        Class<?> uploadType = Class.forName(type.getName() + "$Uploader");
        Class<?> setterType = Class.forName(type.getName() + "$ProfileSetter");
        completionType = Class.forName(type.getName() + "$Completion");
        Object uploader = Proxy.newProxyInstance(uploadType.getClassLoader(), new Class<?>[] {uploadType}, (p,m,a) -> {
            uploads++;
            if (failUpload) throw new java.io.IOException("private transport detail must not escape");
            if (switchDuringUpload) current.set(false);
            return "https://example.test/avatar.jpg";
        });
        Object setter = Proxy.newProxyInstance(setterType.getClassLoader(), new Class<?>[] {setterType}, (p,m,a) -> {
            commits++; pending = a[1]; return null;
        });
        Consumer<String> apply = url -> {
            installs++;
            if (failInstall) throw new IllegalStateException("private install detail");
            installed = url;
        };
        Object operation = type.getDeclaredConstructor(BooleanSupplier.class, uploadType, setterType, Consumer.class, Executor.class)
            .newInstance(identity, uploader, setter, apply, callbacks);
        Object completion = Proxy.newProxyInstance(completionType.getClassLoader(), new Class<?>[] {completionType}, (p,m,a) -> {
            completions++; result = (String) a[0]; return null;
        });
        type.getDeclaredMethod("start", File.class, completionType).invoke(operation, new File("synthetic-avatar.jpg"), completion);
    }
    private void sdkFinished(String error) throws Exception {
        completionType.getDeclaredMethod("finished", String.class).invoke(pending, new Object[] {error});
    }
    @Test public void successUpdatesProfileOnlyAfterSdkConfirmation() throws Exception {
        start(Runnable::run);
        assertEquals(1, uploads); assertEquals(1, commits); assertEquals(0, installs);
        sdkFinished(null);
        assertEquals(1, installs); assertEquals("https://example.test/avatar.jpg", installed);
        assertEquals(1, completions); assertNull(result);
    }
    @Test public void changedIdentityBeforeUploadDoesNotUploadOrCommit() throws Exception {
        current.set(false); start(Runnable::run);
        assertEquals(0, uploads); assertEquals(0, commits); assertEquals(0, installs);
        assertNotNull(result);
    }
    @Test public void accountSwitchDuringUploadNeverWritesTheNewAccountsProfile() throws Exception {
        switchDuringUpload = true; start(Runnable::run);
        assertEquals(1, uploads); assertEquals(0, commits); assertEquals(0, installs);
        assertNotNull(result);
    }
    @Test public void accountSwitchWhileCommitIsQueuedRejectsTheSdkWrite() throws Exception {
        List<Runnable> queued = new ArrayList<>(); start(queued::add);
        current.set(false); queued.remove(0).run();
        assertEquals(0, commits); assertEquals(0, installs); assertNotNull(result);
    }
    @Test public void accountSwitchBeforeSdkCallbackDoesNotChangeCurrentProfile() throws Exception {
        start(Runnable::run); current.set(false); sdkFinished(null);
        assertEquals(0, installs); assertNotNull(result);
    }
    @Test public void uploadFailureHidesTransportDetailsAndDoesNotCommit() throws Exception {
        failUpload = true; start(Runnable::run);
        assertEquals(0, commits); assertEquals(0, installs);
        assertNotNull(result); assertFalse(result.contains("private transport"));
    }
    @Test public void sameAccountReloginRejectsOldCallbackExactlyOnce() throws Exception {
        AtomicReference<String> login = new AtomicReference<>("alice|generation-1");
        String expected = login.get(); identity = () -> expected.equals(login.get());
        start(Runnable::run); login.set("alice|generation-2");
        sdkFinished(null); sdkFinished(null);
        assertEquals(0, installs); assertEquals(1, completions); assertNotNull(result);
    }
    @Test public void duplicateSdkCallbackCannotInstallOrCompleteTwice() throws Exception {
        start(Runnable::run); sdkFinished(null); sdkFinished(null);
        assertEquals(1, installs); assertEquals(1, completions);
    }
    @Test public void aSharedGateRejectsConcurrentUiAndToolUpdatesUntilCompletion() throws Exception {
        Class<?> type;
        try { type = Class.forName("com.kongshang.maichat.RemoteIMAvatarUpdate$Gate"); }
        catch (ClassNotFoundException missing) { throw new AssertionError("UI and tools need one shared avatar transaction gate", missing); }
        Object gate = type.getDeclaredConstructor().newInstance();
        assertEquals(true, type.getDeclaredMethod("tryAcquire").invoke(gate));
        assertEquals(false, type.getDeclaredMethod("tryAcquire").invoke(gate));
        type.getDeclaredMethod("release").invoke(gate);
        assertEquals(true, type.getDeclaredMethod("tryAcquire").invoke(gate));
    }
    @Test public void localProfileFailureStillCompletesOnceWithSafeError() throws Exception {
        failInstall = true; start(Runnable::run);
        try { sdkFinished(null); } catch (java.lang.reflect.InvocationTargetException ignored) { }
        assertEquals(1, completions);
        assertNotNull(result); assertFalse(result.contains("private install"));
        sdkFinished(null); assertEquals(1, completions);
    }
}
