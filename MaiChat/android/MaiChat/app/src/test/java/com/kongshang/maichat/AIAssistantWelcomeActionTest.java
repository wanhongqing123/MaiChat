// 文件作用：验证欢迎卡片保留跨端能力目录，并且只产生可编辑草稿。
// 运行边界：纯 JVM 测试，不连接模型、账号、系统权限或设备服务。
package com.kongshang.maichat;

import java.util.HashSet;
import java.util.Set;
import org.junit.Test;
import static org.junit.Assert.*;

public class AIAssistantWelcomeActionTest {
    @Test public void everyWelcomeCapabilityHasADistinctEditableDraft() {
        Set<String> titles = new HashSet<>();
        Set<String> drafts = new HashSet<>();
        for (AIAssistantWelcomeAction action : AIAssistantWelcomeAction.values()) {
            assertTrue(titles.add(action.title));
            assertTrue(drafts.add(action.draft));
            assertFalse(action.draft.trim().isEmpty());
        }
        assertEquals(19, titles.size());
        assertTrue(titles.contains("会话定时任务"));
        assertTrue(titles.contains("本地媒体处理"));
        assertTrue(titles.contains("拨号与发短信"));
        assertTrue(titles.contains("跨设备会话"));
    }

    @Test public void selectingASystemCapabilityAsksForDetailsBeforeSubmission() {
        assertEquals("请帮我设置会话定时任务：", AIAssistantWelcomeAction.SCHEDULE.draft);
        assertEquals("请帮我拨号或发送短信：", AIAssistantWelcomeAction.PHONE.draft);
        assertEquals("请帮我处理本地媒体：", AIAssistantWelcomeAction.MEDIA.draft);
    }
}
