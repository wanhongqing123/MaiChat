// 文件作用：创作中心只读工具能力目录；不重复展示普通聊天，也不宣传已禁用的图片/视频生成。
package com.kongshang.maichat;

enum AIAssistantWelcomeFeature {
    FILE("文件处理"), SCREEN("整屏截屏"), BROWSER("浏览器操作"), SCHEDULE("会话定时任务"),
    CALENDAR("系统日历"), QR("二维码生成"), LOCATION("当前位置"),
    MEDIA("本地媒体处理"), CAMERA("拍照"), MUSIC("音乐播放"),
    GALLERY("相册管理"), DRIVE("我的云盘"), MESSAGES("MaiChat 消息"),
    CONTACTS("手机通讯录"), PHONE("拨号与发短信"), DEVICES("跨设备会话");

    final String title;
    AIAssistantWelcomeFeature(String title) { this.title = title; }
}
