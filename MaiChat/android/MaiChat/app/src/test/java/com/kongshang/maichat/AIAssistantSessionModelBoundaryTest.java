// Exercises the ordinary host boundary and the real composer handoff with owned fixtures only.
package com.kongshang.maichat;

import static org.junit.Assert.*;
import android.app.Activity;
import java.io.IOException;
import java.lang.reflect.Field;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import org.json.JSONArray;
import org.json.JSONObject;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.Robolectric;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.LooperMode;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34, manifest = Config.NONE)
@LooperMode(LooperMode.Mode.PAUSED)
public final class AIAssistantSessionModelBoundaryTest {
    private void coldLocalOwnerInput(String operation) throws Exception {
        List<String> calls = new ArrayList<>();
        boolean[] prepared = {false};
        boolean accepted = false;
        JSONObject input = new JSONObject().put("op", operation).put("session", "local-b")
            .put("text", "owned fixture");
        try {
            accepted = AIAssistantSessionModels.executeSessionRequest(input, request -> {
                calls.add(request.getString("op") + ":" + request.optString("session"));
                if (request.getString("op").equals("get_session"))
                    return new JSONObject().put("ok", true).put("session", "local-b")
                        .put("model", AIAssistantSessionModels.LOCAL);
                if (!prepared[0]) throw new IOException("Local route is not configured");
                return new JSONObject().put("ok", true);
            }, model -> {
                assertEquals(AIAssistantSessionModels.LOCAL, model);
                calls.add("prepare:local-b");
                prepared[0] = true;
            }).optBoolean("ok");
        } catch (IOException unprepared) { /* Assert the actual old rejection below. */ }
        assertTrue("A cold local owner session must be prepared by the trusted host: " + calls, accepted);
        assertEquals(List.of("get_session:local-b", "prepare:local-b", operation + ":local-b"), calls);
        assertFalse(input.has("localModelPath"));
    }

    @Test public void remotePromptPreparesOnlyItsActualColdLocalOwner() throws Exception {
        coldLocalOwnerInput("enqueue");
    }

    @Test public void remoteSteeringUsesTheSameTrustedLocalPreparationBoundary() throws Exception {
        coldLocalOwnerInput("steer");
    }

    @Test public void cloudOwnerInputDoesNotPrepareAnyLocalHistory() throws Exception {
        List<String> calls = new ArrayList<>();
        JSONObject response = AIAssistantSessionModels.executeSessionRequest(new JSONObject()
            .put("op", "enqueue").put("session", "cloud-a"), request -> {
                calls.add(request.optString("op"));
                return new JSONObject().put("ok", true).put("session", "cloud-a")
                    .put("model", "deepseek-flash");
            }, model -> fail("A cloud request cannot stage another session's local weight"));
        assertTrue(response.optBoolean("ok"));
        assertEquals(List.of("get_session", "enqueue"), calls);
    }

    @Test public void blankAdoptionCannotMoveDraftsAfterChangingToAnotherVisiblePage() {
        assertEquals("previous", AIAssistantSessionModels.createdDraftSession("", false, "created", "previous"));
        assertEquals("previous", AIAssistantSessionModels.createdDraftSession("existing", true, "created", "previous"));
        assertEquals("previous", AIAssistantSessionModels.createdDraftSession("", true, "", "previous"));
    }

    private AIAssistantController.State state(String selected, String marker) throws Exception {
        JSONObject snapshot = new JSONObject().put("messages", new JSONArray())
            .put("sessions", new JSONArray().put(new JSONObject().put("id", selected)
                .put("model", "glm-5.3-flash").put("reasoningEffort", "max")))
            .put("emptyDraftSession", marker);
        return new AIAssistantController.State(snapshot, null, selected, "", "deepseek-flash",
            "on-request", "", "", "", 0, false, true);
    }

    @SuppressWarnings("unchecked")
    private void blankChoice(boolean checkAttachmentsFirst) throws Exception {
        Activity activity = Robolectric.buildActivity(Activity.class).setup().get();
        // The normal Controller constructor has no JNI library in this JVM; initialize rejects
        // before opening a database. The unmounted real Panel receives only our explicit State.
        AIAssistantController controller = new AIAssistantController(activity, "http://127.0.0.1:18190");
        AIAssistantPanel panel = new AIAssistantPanel(activity, controller);
        try {
            panel.onState(state("", ""));
            panel.composer.setText("owned unsent draft");
            Field field = AIAssistantPanel.class.getDeclaredField("attachments");
            field.setAccessible(true);
            Map<String, List<AIAssistantController.ImportedFile>> attachments =
                (Map<String, List<AIAssistantController.ImportedFile>>) field.get(panel);
            AIAssistantController.ImportedFile file = new AIAssistantController.ImportedFile(
                "owned-note.txt", "text/plain", false, "owned-note.txt");
            attachments.put("", new ArrayList<>(List.of(file)));
            String marker = AIAssistantSessionModels.createdDraftSession("", true, "created-session", "");
            panel.onState(state("created-session", marker));
            if (checkAttachmentsFirst) assertEquals(List.of(file), attachments.get("created-session"));
            assertEquals("owned unsent draft", panel.composer.getText().toString());
            assertEquals(List.of(file), attachments.get("created-session"));
            assertFalse("The old blank owner must retire after adoption", attachments.containsKey(""));
        } finally {
            panel.closeLocalMusic();
            controller.close();
            activity.finish();
        }
    }

    @Test public void blankModelChoicePreservesTheRealComposerAndAttachments() throws Exception {
        blankChoice(false);
    }

    @Test public void blankEffortChoicePreservesTheRealAttachmentOwnerAndComposer() throws Exception {
        blankChoice(true);
    }
}
