// 文件作用：验证本人资料的账号边界、显示回退和好友资料隔离，不依赖 Android 或 IM 网络。
package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class RemoteIMSelfProfileStateTest {
    @Test
    public void fallsBackToCurrentImIdentityAndAccountNickname() {
        RemoteIMSelfProfileState state = new RemoteIMSelfProfileState();
        state.activate(" im-alice ", " Account Alice ");

        assertEquals("im-alice", state.current().userId());
        assertEquals("Account Alice", state.current().displayName());
        assertEquals("", state.current().avatarUrl());

        state.activate("im-alice", "");
        assertEquals("im-alice", state.current().displayName());
    }

    @Test
    public void accountNicknameTakesPriorityWhileKeepingCurrentAvatar() {
        RemoteIMSelfProfileState state = new RemoteIMSelfProfileState();
        state.activate("im-alice", "Account Alice");
        assertTrue(state.accept(new RemoteIMContact("im-alice", "IM Alice", "https://example.test/alice.png")));

        assertEquals("Account Alice", state.current().displayName());
        state.activate("im-alice", "Renamed Alice");
        assertEquals("Renamed Alice", state.current().displayName());
        assertEquals("https://example.test/alice.png", state.current().avatarUrl());

        state.activate("im-alice", "");
        assertEquals("IM Alice", state.current().displayName());
    }

    @Test
    public void accountSwitchClearsAvatarAndRejectsLatePreviousProfile() {
        RemoteIMSelfProfileState state = new RemoteIMSelfProfileState();
        state.activate("im-alice", "Alice");
        state.accept(new RemoteIMContact("im-alice", "Alice", "https://example.test/alice.png"));
        state.activate("im-bob", "Bob");

        assertFalse(state.accept(new RemoteIMContact("im-alice", "Late Alice", "https://example.test/late.png")));
        assertEquals(new RemoteIMContact("im-bob", "Bob"), state.current());

        state.activate("im-alice", "Alice");
        assertEquals("", state.current().avatarUrl());
    }

    @Test
    public void friendProfilesDoNotReplaceSelfOrCarryContactGroups() {
        RemoteIMSelfProfileState state = new RemoteIMSelfProfileState();
        state.activate("im-alice", "Alice");

        assertFalse(state.accept(new RemoteIMContact("im-bob", "Bob", "https://example.test/bob.png")));
        assertEquals(new RemoteIMContact("im-alice", "Alice"), state.current());
        assertTrue(state.accept(new RemoteIMContact("im-alice", "IM Alice", "", "Friends")));
        assertEquals("", state.current().groupName());
    }
}
