// 文件作用：确定相邻可折叠工具的边界，避免后续工具越过中间正文或媒体。
package com.kongshang.maichat;

import java.util.Arrays;

final class AIAssistantPartGroups {
    private AIAssistantPartGroups() {}

    // "tool" 表示可折叠工具；媒体等可见内容断组，隐藏思考不创建空组。
    // 返回首工具的索引，宿主据其稳定 part.id 保持流式更新期间的折叠身份。
    static int[] starts(String[] kinds) {
        int[] starts = new int[kinds.length];
        Arrays.fill(starts, -1);
        int first = -1;
        for (int i = 0; i < kinds.length; i++) {
            if ("reasoning".equals(kinds[i])) continue;
            if ("tool".equals(kinds[i])) {
                if (first < 0) first = i;
                starts[i] = first;
            } else {
                first = -1;
            }
        }
        return starts;
    }
}
