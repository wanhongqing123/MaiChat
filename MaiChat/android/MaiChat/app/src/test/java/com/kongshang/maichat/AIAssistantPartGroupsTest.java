// 文件作用：无 Android 依赖的分组回归；直接由 javac/java 运行，验证工具不会越过正文或媒体。
package com.kongshang.maichat;

import java.util.Arrays;

public final class AIAssistantPartGroupsTest {
    private static void expect(int[] expected, String... kinds) {
        int[] actual = AIAssistantPartGroups.starts(kinds);
        if (!Arrays.equals(expected, actual))
            throw new AssertionError(Arrays.toString(kinds) + ": expected "
                + Arrays.toString(expected) + ", got " + Arrays.toString(actual));
    }

    public static void main(String[] args) {
        expect(new int[] {-1, 1, -1, 3, -1}, "text", "tool", "text", "tool", "text");
        expect(new int[] {-1, 1, 1, -1}, "text", "tool", "tool", "text");
        expect(new int[] {0, -1, 2}, "tool", "media", "tool");
        expect(new int[] {0, -1, 0, -1}, "tool", "reasoning", "tool", "text");
        expect(new int[] {-1, -1}, "text", "reasoning");
        expect(new int[] {});
        // Appending streamed text or tools must retain the first tool's group identity.
        expect(new int[] {-1, 1, 1, -1}, "text", "tool", "tool", "text");
        System.out.println("AIAssistantPartGroupsTest passed");
    }
}
