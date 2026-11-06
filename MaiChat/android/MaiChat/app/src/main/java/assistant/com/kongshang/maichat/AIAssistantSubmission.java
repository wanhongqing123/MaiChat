// 文件作用：捕获 Android 点击时的所有者与补充编号；工具排队和执行仍由共享 Agent 核心负责。
package com.kongshang.maichat;

import java.util.UUID;
import org.json.JSONObject;
import org.json.JSONArray;

final class AIAssistantSubmission {
    final String session;
    final String requestId = UUID.randomUUID().toString();
    private final String remote;
    private String turn;
    private boolean bound;
    private final String question;

    private AIAssistantSubmission(String session, JSONObject remote, JSONObject runtime) {
        this.session = session;
        this.remote = remote == null ? null : remote.toString();
        JSONObject first = questionToAnswer(runtime.optJSONArray("questions"), runtime.optJSONArray("questionAnswerEchoes"));
        question = first == null ? "" : first.optString("id");
        bound = runtime.optBoolean("busy");
        turn = bound ? runtime.optString("turnId") : "";
    }

    static AIAssistantSubmission capture(String session, JSONObject remote, JSONObject runtime) {
        return new AIAssistantSubmission(session, remote, runtime);
    }

    static JSONObject questionToAnswer(JSONArray questions, JSONArray echoes) {
        if (questions == null) return null;
        for (int i = 0; i < questions.length(); i++) {
            JSONObject question = questions.optJSONObject(i);
            if (question == null || question.optString("id").isEmpty()) continue;
            boolean answered = false;
            if (echoes != null) for (int j = 0; j < echoes.length(); j++) {
                JSONObject echo = echoes.optJSONObject(j);
                if (echo != null && question.optString("id").equals(echo.optString("id"))) { answered = true; break; }
            }
            if (!answered) return question;
        }
        return null;
    }

    JSONObject remoteOwner() {
        try { return remote == null ? null : new JSONObject(remote); }
        catch (Exception impossible) { throw new IllegalStateException("Invalid captured owner", impossible); }
    }

    String turnId() { return turn; }
    String questionId() { return question; }

    // 空闲点击尚未选定模型轮次，前一条 FIFO 请求受理后可加入该轮。一次绑定后永不改发 send。
    String bind(JSONObject runtime) {
        // 普通输入只回答点击时看到的那一个问题；它若已失效，不能改绑其他问题或新任务。
        if (!question.isEmpty()) {
            JSONArray pending = runtime.optJSONArray("questions");
            if (pending != null) for (int i = 0; i < pending.length(); i++) {
                JSONObject item = pending.optJSONObject(i);
                if (item != null && question.equals(item.optString("id"))) return "answer";
            }
            throw new IllegalStateException("这个问题已回答或取消，内容已保留，请核对后再发送");
        }
        if (bound) {
            if (turn.isEmpty() || !runtime.optBoolean("busy") || !turn.equals(runtime.optString("turnId")))
                throw new IllegalStateException("原任务已结束或正在停止，补充内容未发送，已保留草稿");
            return "steer";
        }
        if (!runtime.optBoolean("busy")) return "send";
        turn = runtime.optString("turnId");
        if (turn.isEmpty())
            throw new IllegalStateException("任务编号尚未就绪，内容未发送，已保留草稿");
        bound = true;
        return "steer";
    }

    // Stop 与输入不是同一动作；异步派发前再次核对，不能停止已经替换的新轮次。
    boolean canStop(JSONObject runtime) {
        return bound && !turn.isEmpty() && runtime.optBoolean("busy")
            && turn.equals(runtime.optString("turnId"));
    }

    // 回答用 question_id；send/steer 用 user 与原 turn。最后参数是该操作捕获的 question/turn 绑定。
    static JSONObject receipt(JSONObject accepted, String operation, String expectedTurn) throws Exception {
        JSONObject result = accepted == null ? new JSONObject() : new JSONObject(accepted.toString());
        if (operation.equals("answer")) {
            if (result.optBoolean("ok") && (expectedTurn.isEmpty()
                    || !expectedTurn.equals(result.optString("question_id"))))
                result.put("ok", false).put("unknown", true)
                    .put("error", "回答回执缺少原问题编号，内容已保留，请先核对历史");
            return result;
        } else if (operation.equals("enqueue")) {
            if (result.optBoolean("ok") && (result.optBoolean("unknown") || result.optBoolean("delivery_unknown") ||
                !(result.opt("user_message_id") instanceof String) || result.optString("user_message_id").isEmpty()))
                result.put("ok", false).put("unknown", true);
            return result;
        }
        if (!result.optBoolean("ok") || !(operation.equals("send") || operation.equals("prompt")
            || operation.equals("steer"))) return result;
        Object user = result.opt("user_message_id"), turn = result.opt("turnId");
        boolean complete = user instanceof String && !((String) user).isEmpty()
            && turn instanceof String && !((String) turn).isEmpty();
        if (operation.equals("steer")) complete &= !expectedTurn.isEmpty() && expectedTurn.equals(turn);
        if (!complete) result.put("ok", false).put("unknown", true)
            .put("error", "受理回执缺少原消息或轮次，内容已保留，请先核对历史");
        return result;
    }

    // 普通模型入口与 steer 复用一个最终请求构造，附件路径不能只保存在待发卡片中。
    static JSONObject localRequest(JSONObject payload, String operation, String turnId, String requestId) throws Exception {
        if (operation.equals("answer")) {
            if (payload.getJSONArray("files").length() > 0 || !payload.optString("quoteMessageId").isEmpty())
                throw new IllegalStateException("问题回答只发送文字，附件和引用必须保留在输入栏");
            return new JSONObject().put("op", "answer").put("session", payload.getString("session"))
                .put("id", payload.getString("questionId")).put("text", payload.getString("text"))
                .put("requestId", requestId);
        }
        JSONArray images = new JSONArray(), videos = new JSONArray();
        StringBuilder documents = new StringBuilder();
        JSONArray files = payload.getJSONArray("files");
        for (int i = 0; i < files.length(); i++) {
            JSONObject file = files.getJSONObject(i);
            if (file.optBoolean("image") || file.optString("mimeType").startsWith("video/"))
                (file.optBoolean("image") ? images : videos).put(new JSONObject().put("path", file.getString("path"))
                    .put("mimeType", file.getString("mimeType")));
            else documents.append("\n- ").append(file.getString("path"));
        }
        String text = payload.getString("text");
        if (documents.length() > 0) text += "\n\n会话附件已保存到本机工作目录：" + documents;
        JSONObject request = new JSONObject().put("op", operation).put("session", payload.getString("session"))
            .put("text", text).put("images", images).put("videos", videos)
            .put("requestId", requestId).put("quoteMessageId", payload.optString("quoteMessageId"));
        if (operation.equals("steer")) request.put("turnId", turnId);
        return request;
    }
}
