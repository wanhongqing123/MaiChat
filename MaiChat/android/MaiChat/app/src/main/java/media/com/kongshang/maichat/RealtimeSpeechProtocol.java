// 文件作用：解析腾讯普通实时 ASR 的逐段结果；不依赖麦克风、网络或界面。
// 主要入口：accept、text、complete、failed；每次录音单独创建实例，客户端使用临时 STS 签名。
package com.kongshang.maichat;

import org.json.JSONObject;
import java.util.TreeMap;

/** Standard 16k_zh wire contract: replace a segment by index and join ordered segments. */
final class RealtimeSpeechProtocol {
    private final String voiceId;
    private final TreeMap<Integer, String> sentences = new TreeMap<>();
    private final java.util.Set<Integer> committed = new java.util.HashSet<>();
    private boolean complete, failed;
    private String text = "";

    RealtimeSpeechProtocol(String voiceId) { this.voiceId = voiceId; }

    // 迟到的旧流消息不能结束当前流；结束或失败后也不能再改写已交付的结果。
    boolean accept(JSONObject message) {
        if (complete || failed || !voiceId.equals(message.optString("voice_id", voiceId)))
            return false;
        if (message.optInt("code", -1) != 0) {
            failed = true;
            return true;
        }
        JSONObject result = message.optJSONObject("result");
        if (acceptSentence(result)) text = joinedSentences();
        // final 通常没有 result，不能清空已收到的段落，也不能提前把 partial 当作最终结果。
        if (message.optInt("final", 0) == 1) complete = true;
        return true;
    }

    private boolean acceptSentence(JSONObject sentence) {
        if (sentence == null) return false;
        int index = sentence.optInt("index", -1);
        int type = sentence.optInt("slice_type", -1);
        if (index < 0 || index >= 10000 || type < 0 || type > 2 ||
            !(sentence.opt("voice_text_str") instanceof String) ||
            (committed.contains(index) && type != 2)) return false;
        sentences.put(index, sentence.optString("voice_text_str", ""));
        if (type == 2) committed.add(index);
        return true;
    }

    boolean complete() { return complete; }
    boolean failed() { return failed; }
    String text() { return text.trim(); }
    private String joinedSentences() {
        StringBuilder text = new StringBuilder();
        for (String sentence : sentences.values()) text.append(sentence);
        return text.toString();
    }
}
