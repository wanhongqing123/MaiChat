// 文件作用：实时流只依赖有界 WebSocket 收发，正式实现复用既有 native curl。
package com.kongshang.maichat;

interface RealtimeSpeechSocket {
    boolean sendBinary(byte[] data);
    boolean sendText(String text);
    void cancel();
}
