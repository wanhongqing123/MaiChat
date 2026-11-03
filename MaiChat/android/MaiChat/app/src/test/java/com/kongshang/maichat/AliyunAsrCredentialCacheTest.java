// 文件作用：验证阿里凭据内存缓存、并发合并、提前刷新和账号代际隔离。
package com.kongshang.maichat;

import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.LongSupplier;
import org.junit.Test;
import static org.junit.Assert.*;

public class AliyunAsrCredentialCacheTest {
    private final List<Runnable> queued = new ArrayList<>();
    private final AtomicLong clock = new AtomicLong(AliyunAsrCredentialTest.NOW);
    private final AtomicInteger loads = new AtomicInteger();
    private Class<?> type, loaderType;
    private Object cache;
    private boolean fail, switchDuringLoad;
    private void setup() throws Exception {
        try { type = Class.forName("com.kongshang.maichat.AliyunAsrCredentialCache"); }
        catch (ClassNotFoundException missing) { throw new AssertionError("Aliyun tokens need an account-scoped memory cache", missing); }
        loaderType = Class.forName(type.getName() + "$Loader");
        cache = type.getDeclaredConstructor(Executor.class, LongSupplier.class).newInstance((Executor) queued::add, (LongSupplier) clock::get);
        activate("alice:1");
    }
    private void activate(String identity) throws Exception { type.getDeclaredMethod("activate", String.class).invoke(cache, identity); }
    private CompletableFuture<?> get(String identity) throws Exception {
        Object loader = Proxy.newProxyInstance(loaderType.getClassLoader(), new Class<?>[] {loaderType}, (p,m,a) -> {
            loads.incrementAndGet();
            if (fail) throw new java.io.IOException("fixture network failure");
            if (switchDuringLoad) activate("bob:1");
            return AliyunAsrCredentialTest.parse(AliyunAsrCredentialTest.fixture(clock.get() + 36 * 3600));
        });
        return (CompletableFuture<?>) type.getDeclaredMethod("get", String.class, loaderType).invoke(cache, identity, loader);
    }
    @Test public void coalescesConcurrentLoadsAndRefreshesAtNinetySeconds() throws Exception {
        setup(); CompletableFuture<?> first = get("alice:1"); assertSame(first, get("alice:1"));
        assertEquals(1, queued.size()); queued.remove(0).run(); Object credential = first.get();
        assertSame(credential, get("alice:1").get()); assertEquals(1, loads.get());
        clock.addAndGet(36 * 3600 - 90); CompletableFuture<?> refreshed = get("alice:1");
        assertFalse(refreshed.isDone()); queued.remove(0).run(); assertEquals(2, loads.get());
    }
    @Test public void aLateOldAccountLoadCannotPopulateTheNewAccountsCache() throws Exception {
        setup(); switchDuringLoad = true; CompletableFuture<?> old = get("alice:1"); queued.remove(0).run();
        assertTrue(old.isCompletedExceptionally()); switchDuringLoad = false;
        CompletableFuture<?> next = get("bob:1"); queued.remove(0).run(); assertNotNull(next.get());
        assertTrue(get("alice:1").isCompletedExceptionally());
    }
    @Test public void failedLoadCanRetryAndReloginInvalidatesTheCachedToken() throws Exception {
        setup(); fail = true; CompletableFuture<?> broken = get("alice:1"); queued.remove(0).run();
        assertTrue(broken.isCompletedExceptionally()); fail = false;
        CompletableFuture<?> good = get("alice:1"); queued.remove(0).run(); assertNotNull(good.get());
        activate("alice:2"); assertTrue(get("alice:1").isCompletedExceptionally());
        CompletableFuture<?> relogin = get("alice:2"); assertFalse(relogin.isDone()); queued.remove(0).run(); assertNotNull(relogin.get());
    }
}
