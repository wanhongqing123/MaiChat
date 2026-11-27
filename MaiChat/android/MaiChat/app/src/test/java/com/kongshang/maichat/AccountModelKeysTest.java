// Credential parsing uses synthetic responses only; no account, network or persistent storage.
package com.kongshang.maichat;

import static org.junit.Assert.*;
import org.json.JSONObject;
import org.junit.Test;

public final class AccountModelKeysTest {
    private JSONObject legacy(String glm, String deepseek, String minimax) throws Exception {
        return new JSONObject().put("glm", glm).put("deepseek", deepseek).put("minimax", minimax);
    }

    @Test public void openAiOnlyAccountIsAcceptedAndOldResponsesDefaultToEmpty() throws Exception {
        MaiChatAuthClient.AccountModelKeys keys = MaiChatAuthClient.parseAccountModelKeys(
            legacy("", "", "").put("openai", "synthetic-openai"));
        assertEquals("synthetic-openai", keys.openai);
        assertEquals("", keys.openrouter);
        assertEquals("", MaiChatAuthClient.parseAccountModelKeys(legacy("synthetic-glm", "", "")).openai);
        for (Object invalid : new Object[]{42, true, JSONObject.NULL, "bad\nkey", "x".repeat(4097)}) {
            try {
                MaiChatAuthClient.parseAccountModelKeys(legacy("synthetic-glm", "", "").put("openai", invalid));
                fail("Malformed OpenAI credential must reject the entire response");
            } catch (Exception expected) { }
        }
    }

    @Test public void oldResponseKeepsExistingKeysAndDefaultsOpenRouterToEmpty() throws Exception {
        MaiChatAuthClient.AccountModelKeys keys = MaiChatAuthClient.parseAccountModelKeys(
            legacy("synthetic-glm", "synthetic-deepseek", "synthetic-minimax"));
        assertEquals("synthetic-glm", keys.glm);
        assertEquals("synthetic-deepseek", keys.deepseek);
        assertEquals("synthetic-minimax", keys.minimax);
        assertEquals("", keys.openrouter);
    }

    @Test public void openRouterOnlyAccountIsValidWithoutAnotherProvidersKey() throws Exception {
        MaiChatAuthClient.AccountModelKeys keys = MaiChatAuthClient.parseAccountModelKeys(
            legacy("", "", "").put("openrouter", "synthetic-openrouter"));
        assertEquals("synthetic-openrouter", keys.openrouter);
        assertEquals("", keys.glm);
        assertEquals("", keys.deepseek);
        assertEquals("", keys.minimax);
    }

    @Test public void optionalKeyMustBeAValidStringAndAllEmptyResponseIsRejected() throws Exception {
        for (Object invalid : new Object[]{42, true, JSONObject.NULL, "bad\nkey", "bad\u0000key",
                "x".repeat(4097)}) {
            try {
                MaiChatAuthClient.parseAccountModelKeys(legacy("synthetic-glm", "", "")
                    .put("openrouter", invalid));
                fail("An invalid optional key must reject the credential response");
            } catch (Exception expected) { }
        }
        try {
            MaiChatAuthClient.parseAccountModelKeys(legacy("", "", "").put("openrouter", ""));
            fail("An all-empty response must not grant model access");
        } catch (Exception expected) { }
    }
}
