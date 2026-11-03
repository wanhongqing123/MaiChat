// 文件作用：录音开始固定服务选择；直接语音始终 AAC，不受转写设置改变影响。
package com.kongshang.maichat;

import org.junit.Test;
import static org.junit.Assert.*;

public class SpeechRecordingPlanTest {
    @Test public void providerRoutingPreservesDirectVoiceAndDefaultsAndUsesPcmForAliyun() throws Exception {
        Class<?> provider;
        try { provider = Class.forName("com.kongshang.maichat.SpeechRecognitionProvider"); }
        catch (ClassNotFoundException missing) { throw new AssertionError("Recordings need a fixed Tencent or Aliyun provider selection", missing); }
        Object tencent = provider.getDeclaredMethod("fromId", String.class).invoke(null, "tencent");
        Object aliyun = provider.getDeclaredMethod("fromId", String.class).invoke(null, "aliyun");
        assertEquals(tencent, provider.getDeclaredMethod("fromId", String.class).invoke(null, "unknown"));
        Class<?> plan = Class.forName("com.kongshang.maichat.SpeechRecordingPlan");
        Object aliCapture = plan.getDeclaredConstructor(provider, boolean.class).newInstance(aliyun, true);
        assertEquals(true, plan.getDeclaredMethod("usesPcm").invoke(aliCapture));
        Object direct = plan.getDeclaredConstructor(provider, boolean.class).newInstance(aliyun, false);
        assertEquals(false, plan.getDeclaredMethod("usesPcm").invoke(direct));
        assertEquals(tencent, plan.getDeclaredMethod("provider").invoke(direct));
        Object next = plan.getDeclaredConstructor(provider, boolean.class).newInstance(tencent, true);
        assertEquals(false, plan.getDeclaredMethod("usesPcm").invoke(next));
        assertEquals(aliyun, plan.getDeclaredMethod("provider").invoke(aliCapture));
    }
}
