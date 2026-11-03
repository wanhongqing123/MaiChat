// 文件作用：验证公开账号资料使用独立 ID、可空脱敏字段，且不读取手机号原文。
package com.kongshang.maichat;

import org.json.JSONObject;
import org.junit.Test;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

public class AccountProfileSnapshotTest {
    @Test public void distinguishesPublicAccountIdAndImIdentity() throws Exception {
        MaiChatAuthClient.AccountProfile profile = new MaiChatAuthClient.AccountProfile(
            new JSONObject().put("id", "public-account-42").put("im_user_id", "im-user-73")
                .put("nickname", "A nickname").put("phone_masked", "138****1234")
                .put("phone", "13800131234").put("access_token", "not-a-profile-field"));
        assertEquals("public-account-42", profile.accountId);
        assertEquals("im-user-73", profile.imUserId);
        assertEquals("A nickname", profile.nickname);
        assertEquals("138****1234", profile.phoneMasked);
    }

    @Test public void missingMaskedPhoneNeverFallsBackToRawPhone() throws Exception {
        MaiChatAuthClient.AccountProfile profile = new MaiChatAuthClient.AccountProfile(
            new JSONObject().put("id", "public-account-42").put("im_user_id", "im-user-73")
                .put("nickname", JSONObject.NULL).put("phone", "13800131234"));
        assertNull(profile.nickname);
        assertNull(profile.phoneMasked);
    }
}
