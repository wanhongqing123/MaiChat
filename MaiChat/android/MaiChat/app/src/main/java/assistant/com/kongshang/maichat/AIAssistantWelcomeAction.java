// 文件作用：创作中心欢迎卡片的能力目录和可编辑提示语。
// 边界：选中卡片只提供草稿，不在此处调用模型或申请系统权限。
package com.kongshang.maichat;

enum AIAssistantWelcomeAction {
    ASK("AI 问答", "请帮我解答："),
    IMAGE("图片创作", "请帮我创作图片："),
    VIDEO("视频创作", "请帮我创作视频："),
    FILE("文件处理", "请帮我处理文件："),
    SCREEN("整屏截屏", "请帮我截取屏幕："),
    BROWSER("浏览器操作", "请帮我在浏览器中："),
    SCHEDULE("会话定时任务", "请帮我设置会话定时任务："),
    CALENDAR("系统日历", "请帮我管理系统日历："),
    QR("二维码生成", "请帮我生成二维码："),
    LOCATION("当前位置", "请帮我查看当前位置："),
    MEDIA("本地媒体处理", "请帮我处理本地媒体："),
    CAMERA("拍照", "请帮我拍照："),
    MUSIC("音乐播放", "请帮我播放音乐："),
    GALLERY("相册管理", "请帮我管理相册："),
    DRIVE("我的云盘", "请帮我管理我的云盘："),
    MESSAGES("MaiChat 消息", "请帮我处理 MaiChat 消息："),
    CONTACTS("手机通讯录", "请帮我查找手机通讯录："),
    PHONE("拨号与发短信", "请帮我拨号或发送短信："),
    DEVICES("跨设备会话", "请帮我查看跨设备会话：");

    final String title;
    final String draft;

    AIAssistantWelcomeAction(String title, String draft) {
        this.title = title;
        this.draft = draft;
    }
}
