// 文件作用：转文字服务标识；设置持久化由宿主负责，每次录音固定选择。
package com.kongshang.maichat;

public enum SpeechRecognitionProvider {
    TENCENT("tencent", "腾讯云"), ALIYUN("aliyun", "阿里云");
    private final String id, title;
    SpeechRecognitionProvider(String id, String title) { this.id = id; this.title = title; }
    public String id() { return id; }
    public String title() { return title; }
    public static SpeechRecognitionProvider fromId(String id) {
        return "aliyun".equals(id) ? ALIYUN : TENCENT;
    }
}
