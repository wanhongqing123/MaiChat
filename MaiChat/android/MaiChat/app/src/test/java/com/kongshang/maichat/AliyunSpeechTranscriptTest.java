// 文件作用：用本地 NLS 协议 fixture 验证逐句替换、任务归属与完成/失败/取消边界。
// 来源：iOS test-aliyun-asr.swift 的 task_id、status、index/result/time 合同；不访问 SDK 或网络。
package com.kongshang.maichat;

import static org.junit.Assert.*;

import org.json.JSONObject;
import org.junit.Test;

public class AliyunSpeechTranscriptTest {
    private static final String TASK = "task-a";

    private JSONObject event(String name) throws Exception {
        return new JSONObject().put("header", new JSONObject().put("task_id", TASK)
            .put("name", name).put("status", 20_000_000));
    }

    private JSONObject sentence(String name, int index, String result, Integer time) throws Exception {
        JSONObject payload = new JSONObject().put("index", index).put("result", result);
        if (time != null) payload.put("time", time);
        return event(name).put("payload", payload);
    }

    @Test public void onlyTheStartedEventAcknowledgesStartupAndDuplicatesKeepTheTranscript() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        assertFalse(transcript.started());
        assertTrue(transcript.accept(event("TranscriptionStarted")));
        assertTrue(transcript.started());
        assertFalse(transcript.complete());
        transcript.accept(sentence("TranscriptionResultChanged", 1, "你好", null));
        assertTrue(transcript.accept(event("TranscriptionStarted")));
        assertEquals("你好", transcript.text());
        assertFalse(transcript.complete());
    }

    @Test public void revisedPartialReplacesItsSentenceEvenWhenTheNewTextIsShorter() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        assertTrue(transcript.accept(sentence("TranscriptionResultChanged", 1, "你好世界", 1000)));
        assertTrue(transcript.accept(sentence("TranscriptionResultChanged", 1, "你好", 1100)));
        assertEquals("你好", transcript.text());
        assertFalse(transcript.started());
        assertFalse(transcript.complete());
    }

    @Test public void sentencesAreJoinedByIndexAndDuplicateFinalsAreNotAppended() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        transcript.accept(sentence("SentenceEnd", 2, "世界。", 2000));
        transcript.accept(sentence("SentenceEnd", 1, "你好，", 900));
        transcript.accept(sentence("SentenceEnd", 2, "世界。", 2000));
        assertEquals("你好，世界。", transcript.text());
        assertFalse(transcript.complete());
    }

    @Test public void completedSentenceRejectsLatePartialButAcceptsAFinalCorrection() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        transcript.accept(sentence("SentenceEnd", 1, "你好，", 1000));
        assertFalse(transcript.accept(sentence("TranscriptionResultChanged", 1, "过时结果", 1100)));
        assertTrue(transcript.accept(sentence("SentenceEnd", 1, "您好，", 1000)));
        assertEquals("您好，", transcript.text());
    }

    @Test public void partialWithEarlierAudioTimeCannotMoveTheSentenceBackwards() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        transcript.accept(sentence("TranscriptionResultChanged", 1, "新结果", 2000));
        assertFalse(transcript.accept(sentence("TranscriptionResultChanged", 1, "过时结果", 1000)));
        assertEquals("新结果", transcript.text());
        assertTrue(transcript.accept(sentence("TranscriptionResultChanged", 1, "同时间修正", 2000)));
        assertEquals("同时间修正", transcript.text());
    }

    @Test public void aPartialWithoutTimeDoesNotEraseTheKnownAudioTimeBoundary() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        transcript.accept(sentence("TranscriptionResultChanged", 1, "较新结果", 2000));
        assertTrue(transcript.accept(sentence("TranscriptionResultChanged", 1, "没有时间的修正", null)));
        assertFalse(transcript.accept(sentence("TranscriptionResultChanged", 1, "较旧回包", 1000)));
        assertEquals("没有时间的修正", transcript.text());
    }

    @Test public void emptyPartialOnlyClearsItsOwnSentence() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        transcript.accept(sentence("SentenceEnd", 1, "已确认。", 1000));
        transcript.accept(sentence("TranscriptionResultChanged", 2, "背景噪音", 2000));
        assertTrue(transcript.accept(sentence("TranscriptionResultChanged", 2, "", 2100)));
        assertEquals("已确认。", transcript.text());
    }

    @Test public void completionPreservesAllTextAndRejectsLateEvents() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        transcript.accept(sentence("SentenceEnd", 1, "你好，", 1000));
        transcript.accept(sentence("SentenceEnd", 2, "世界。", 2000));
        assertFalse(transcript.complete());
        assertTrue(transcript.accept(event("TranscriptionCompleted")));
        assertTrue(transcript.complete());
        assertFalse(transcript.failed());
        assertEquals("你好，世界。", transcript.text());
        assertFalse(transcript.accept(sentence("SentenceEnd", 3, "迟到", 3000)));
        assertFalse(transcript.accept(event("TaskFailed")));
        assertFalse(transcript.failed());
        assertEquals("你好，世界。", transcript.text());
    }

    @Test public void noSpeechCanCompleteSuccessfullyWithoutASentencePayload() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        assertTrue(transcript.accept(event("TranscriptionCompleted")));
        assertTrue(transcript.complete());
        assertFalse(transcript.failed());
        assertEquals("", transcript.text());
    }

    @Test public void aTaskFailureWithoutTextCannotBeRevivedByACompletion() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        transcript.accept(sentence("TranscriptionResultChanged", 1, "中间态", 1000));
        assertTrue(transcript.accept(event("TaskFailed")));
        assertTrue(transcript.failed());
        assertFalse(transcript.complete());
        assertEquals("中间态", transcript.text());
        assertFalse(transcript.accept(event("TranscriptionCompleted")));
        assertFalse(transcript.complete());
    }

    @Test public void nonSuccessStatusFailsWithoutApplyingItsPayload() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        JSONObject failed = sentence("SentenceEnd", 1, "错误回包正文", 1000);
        failed.getJSONObject("header").put("status", 40_000_000);
        assertTrue(transcript.accept(failed));
        assertTrue(transcript.failed());
        assertFalse(transcript.complete());
        assertEquals("", transcript.text());
        assertFalse(transcript.accept(event("TranscriptionStarted")));
    }

    @Test public void anotherTaskCannotStartChangeFinishOrFailThisTranscript() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        for (String name : new String[] {"TranscriptionStarted", "TranscriptionResultChanged", "SentenceEnd",
                "TranscriptionCompleted", "TaskFailed"}) {
            JSONObject foreign = sentence(name, 1, "异流", 1000);
            foreign.getJSONObject("header").put("task_id", "task-b").put("status", 40_000_000);
            assertFalse(transcript.accept(foreign));
        }
        assertEquals("", transcript.text());
        assertFalse(transcript.started());
        assertFalse(transcript.complete());
        assertFalse(transcript.failed());
    }

    @Test public void nullMissingHeaderAndMissingTaskIdentityAreIgnored() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        assertFalse(transcript.accept(null));
        assertFalse(transcript.accept(new JSONObject()));
        JSONObject unidentified = event("TranscriptionCompleted");
        unidentified.getJSONObject("header").remove("task_id");
        assertFalse(transcript.accept(unidentified));
        assertFalse(transcript.complete());
        assertFalse(transcript.failed());
    }

    @Test public void sentenceBeginIsConsumedWithoutResultAndUnknownEventsDoNotInventText() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        transcript.accept(sentence("SentenceEnd", 1, "已完成。", 1000));
        assertTrue(transcript.accept(event("SentenceBegin").put("payload",
            new JSONObject().put("index", 2).put("time", 1100))));
        assertFalse(transcript.accept(sentence("UnknownEvent", 1, "不是结果", 1000)));
        assertEquals("已完成。", transcript.text());
        assertFalse(transcript.complete());
        assertFalse(transcript.failed());
    }

    @Test public void invalidSentenceFieldsCannotReplaceEarlierValidText() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        transcript.accept(sentence("TranscriptionResultChanged", 1, "有效结果", 1000));
        assertFalse(transcript.accept(sentence("SentenceEnd", 0, "零编号", 1000)));
        assertFalse(transcript.accept(sentence("SentenceEnd", -1, "负编号", 1000)));
        assertFalse(transcript.accept(event("SentenceEnd")));
        JSONObject invalid = sentence("TranscriptionResultChanged", 1, "错误类型", 1100);
        invalid.getJSONObject("payload").put("result", 123);
        assertFalse(transcript.accept(invalid));
        invalid.getJSONObject("payload").put("result", "错误编号").put("index", 1.5);
        assertFalse(transcript.accept(invalid));
        invalid.getJSONObject("payload").put("index", 1).put("time", -1);
        assertFalse(transcript.accept(invalid));
        invalid.getJSONObject("payload").put("time", 1100.5);
        assertFalse(transcript.accept(invalid));
        assertEquals("有效结果", transcript.text());
        assertFalse(transcript.failed());
    }

    @Test public void cancelClearsTheTextAndRejectsEveryLateResult() throws Exception {
        AliyunSpeechTranscript transcript = new AliyunSpeechTranscript(TASK);
        transcript.accept(event("TranscriptionStarted"));
        transcript.accept(sentence("TranscriptionResultChanged", 1, "取消", 1000));
        transcript.cancel();
        assertEquals("", transcript.text());
        assertFalse(transcript.complete());
        assertFalse(transcript.failed());
        assertFalse(transcript.accept(sentence("SentenceEnd", 1, "取消后完成", 1100)));
        assertFalse(transcript.accept(event("TranscriptionCompleted")));
        assertEquals("", transcript.text());
    }
}
