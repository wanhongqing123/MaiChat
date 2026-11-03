// 文件作用：合并阿里 NLS WebSocket 实时识别结果，不访问麦克风、网络或 SDK。
// 协议边界：固定 task_id；句子按 index 替换；SentenceEnd 不结束整轮，只有
// TranscriptionCompleted 才交付完整文本。取消、完成和失败后不再接受迟到消息。
package com.kongshang.maichat;

import java.util.TreeMap;
import org.json.JSONObject;

final class AliyunSpeechTranscript {
    private static final long SUCCESS = 20_000_000;
    private final String taskId;
    private final TreeMap<Integer, Sentence> sentences = new TreeMap<>();
    private boolean started, complete, failed, closed;
    private String text = "";

    private static final class Sentence {
        final String text;
        final boolean finished;
        final Long time;

        Sentence(String text, boolean finished, Long time) {
            this.text = text;
            this.finished = finished;
            this.time = time;
        }
    }

    AliyunSpeechTranscript(String taskId) {
        if (taskId == null || taskId.isEmpty())
            throw new IllegalArgumentException("An NLS task ID is required");
        this.taskId = taskId;
    }

    // 返回 true 表示消费了当前任务的有效事件，包括开始、完成、失败和无文字的句子开始。
    // 异任务、格式不符、过时中间态与终态之后的事件返回 false，不能驱动宿主交付文字。
    synchronized boolean accept(JSONObject message) {
        if (closed || message == null) return false;
        JSONObject header = message.optJSONObject("header");
        if (header == null || !taskId.equals(header.opt("task_id"))
                || !(header.opt("name") instanceof String)) return false;
        String name = (String) header.opt("name");
        if (name.equals("TaskFailed")) {
            failed = true;
            closed = true;
            return true;
        }
        Long status = integer(header.opt("status"));
        if (status == null) return false;
        if (status != SUCCESS) {
            failed = true;
            closed = true;
            return true;
        }
        switch (name) {
            case "TranscriptionStarted":
                started = true;
                return true;
            case "SentenceBegin":
                // 官方此事件没有 result；不能按空句子处理并擦除已经收到的内容。
                return true;
            case "TranscriptionResultChanged":
                return acceptSentence(message.optJSONObject("payload"), false);
            case "SentenceEnd":
                return acceptSentence(message.optJSONObject("payload"), true);
            case "TranscriptionCompleted":
                complete = true;
                closed = true;
                return true;
            default:
                return false;
        }
    }

    private boolean acceptSentence(JSONObject payload, boolean finished) {
        if (payload == null || !(payload.opt("result") instanceof String)) return false;
        Long index = integer(payload.opt("index"));
        if (index == null || index <= 0 || index > Integer.MAX_VALUE) return false;
        Long time = payload.has("time") ? integer(payload.opt("time")) : null;
        if (payload.has("time") && (time == null || time < 0)) return false;
        Sentence previous = sentences.get(index.intValue());
        if (previous != null && !finished) {
            if (previous.finished) return false;
            // 与 iOS 解析器一致：只拦截倒退的中间态，服务端最终句仍可修正已有文字。
            if (previous.time != null && time != null && time < previous.time) return false;
        }
        // 某些 fixture 没有 time；不能因此遗忘已经观察到的中间态时间边界。
        Long rememberedTime = time != null ? time : previous == null ? null : previous.time;
        sentences.put(index.intValue(), new Sentence((String) payload.opt("result"), finished, rememberedTime));
        StringBuilder joined = new StringBuilder();
        for (Sentence sentence : sentences.values()) joined.append(sentence.text);
        text = joined.toString().trim();
        return true;
    }

    synchronized String text() { return text; }
    synchronized boolean started() { return started; }
    synchronized boolean complete() { return complete; }
    synchronized boolean failed() { return failed; }

    synchronized void cancel() {
        closed = true;
        sentences.clear();
        text = "";
        started = false;
        complete = false;
        failed = false;
    }

    private static Long integer(Object value) {
        if (!(value instanceof Number)) return null;
        Number number = (Number) value;
        double numeric = number.doubleValue();
        long integral = number.longValue();
        return Double.isFinite(numeric) && numeric == integral ? integral : null;
    }
}
