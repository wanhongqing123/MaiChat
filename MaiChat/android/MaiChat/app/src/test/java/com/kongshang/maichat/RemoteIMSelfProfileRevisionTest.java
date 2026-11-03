// 文件作用：头像写入成功后，旧的本人资料响应不能回滚新头像；好友资料继续交付。
package com.kongshang.maichat;

import org.junit.Test;
import static org.junit.Assert.*;

public class RemoteIMSelfProfileRevisionTest {
    @Test public void inFlightOwnProfileIsRejectedAfterAvatarCommitButFriendsStillApply() throws Exception {
        Class<?> type;
        try { type = Class.forName("com.kongshang.maichat.RemoteIMSelfProfileRevision"); }
        catch (ClassNotFoundException missing) { throw new AssertionError("Avatar commit must invalidate old self-profile reads", missing); }
        Object revision = type.getDeclaredConstructor().newInstance();
        long before = (Long) type.getDeclaredMethod("snapshot").invoke(revision);
        type.getDeclaredMethod("avatarCommitted").invoke(revision);
        java.lang.reflect.Method accepts = type.getDeclaredMethod("accepts", String.class, String.class, long.class);
        assertEquals(false, accepts.invoke(revision, "alice", "alice", before));
        assertEquals(true, accepts.invoke(revision, "alice", "bob", before));
        long after = (Long) type.getDeclaredMethod("snapshot").invoke(revision);
        assertEquals(true, accepts.invoke(revision, "alice", "alice", after));
    }
}
