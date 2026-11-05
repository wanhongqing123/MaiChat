// 文件作用：验证折叠工具的重复 bind 复用解析结果，同时有明确内存上限。
package com.kongshang.maichat;
import static org.junit.Assert.*;
import java.util.concurrent.atomic.AtomicInteger;
import org.json.JSONObject;
import org.junit.Test;

public final class AIAssistantToolJsonCacheTest {
    @Test public void unchangedToolOutputIsParsedOnceAndChangedOutputGetsANewValue() throws Exception {
        AtomicInteger count = new AtomicInteger();
        AIAssistantToolJsonCache cache = new AIAssistantToolJsonCache(64, value -> {
            count.incrementAndGet(); return new JSONObject(value);
        });
        assertEquals(1, cache.read("{\"step\":1}").getInt("step"));
        assertSame(cache.read("{\"step\":1}"), cache.read("{\"step\":1}"));
        assertEquals(1, count.get());
        assertEquals(2, cache.read("{\"step\":2}").getInt("step"));
        assertEquals(2, count.get());
    }
    @Test public void cacheEvictsOldBytesAndDoesNotRetainOversizedPayloads() throws Exception {
        AtomicInteger count = new AtomicInteger();
        AIAssistantToolJsonCache cache = new AIAssistantToolJsonCache(24, value -> {
            count.incrementAndGet(); return new JSONObject(value);
        });
        cache.read("{\"a\":1}"); cache.read("{\"b\":2}"); cache.read("{\"a\":1}");
        assertEquals(3, count.get());
        String large = "{\"large\":\"abcdefghijklmnop\"}";
        cache.read(large); cache.read(large);
        assertEquals(5, count.get());
        assertTrue(cache.retainedBytes() <= 24);
    }
}
