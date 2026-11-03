// 文件作用：创作中心欢迎页的只读能力目录，不生成草稿或触发操作。
package com.kongshang.maichat;

enum AIAssistantWelcomeFeature {
    ASK("AI 问答"), IMAGE("图片创作"), VIDEO("视频创作"), FILE("文件处理"),
    SCREEN("整屏截屏"), BROWSER("浏览器操作"), SCHEDULE("会话定时任务"),
    CALENDAR("系统日历"), QR("二维码生成"), LOCATION("当前位置"),
    MEDIA("本地媒体处理"), CAMERA("拍照"), MUSIC("音乐播放"),
    GALLERY("相册管理"), DRIVE("我的云盘"), MESSAGES("MaiChat 消息"),
    CONTACTS("手机通讯录"), PHONE("拨号与发短信"), DEVICES("跨设备会话");

    final String title;
    AIAssistantWelcomeFeature(String title) { this.title = title; }
}
