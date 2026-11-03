// 文件作用：录音器共用实时流生命周期；各服务实现各自协议，不修改上层三态交付语义。
package com.kongshang.maichat;

interface RealtimeSpeechStream {
    interface Listener {
        void partial(String text);
        void finished(String text);
        void failed();
        default void diagnostic(String event) { }
    }
    void endAudio();
    void cancel();
}
