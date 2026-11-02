// 文件作用：保存当前账号的本人 IM 资料，避免本人头像与好友列表共用关系状态。
// 运行边界：调用方在切换账号时 activate；SDK 回调还须由调用方校验账号代际。
package com.kongshang.maichat;

final class RemoteIMSelfProfileState {
    private String ownerUserId = "";
    private String accountNickname = "";
    private RemoteIMContact profile;

    void activate(String ownerUserId, String accountNickname) {
        String owner = clean(ownerUserId);
        if (owner.isEmpty()) throw new IllegalArgumentException("ownerUserId is required");
        // 切号立即清空头像；同一账号更新昵称则保留已经取得的 IM 资料。
        if (!owner.equals(this.ownerUserId)) profile = null;
        this.ownerUserId = owner;
        this.accountNickname = clean(accountNickname);
    }

    boolean accept(RemoteIMContact candidate) {
        if (candidate == null || !ownerUserId.equals(candidate.userId())) return false;
        profile = candidate;
        return true;
    }

    RemoteIMContact current() {
        String displayName = accountNickname.isEmpty()
            ? (profile == null ? ownerUserId : profile.displayName()) : accountNickname;
        // 空头像继续使用 AvatarImageLoader 的正常 fallback，不虚构远端头像地址。
        return new RemoteIMContact(ownerUserId, displayName, profile == null ? "" : profile.avatarUrl());
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }
}
