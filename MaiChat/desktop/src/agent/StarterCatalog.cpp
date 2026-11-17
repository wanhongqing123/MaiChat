// 文件作用：依据模型可用的公开工具声明生成桌面欢迎页能力目录。
#include "agent/StarterCatalog.h"

#include <QSet>

QList<StarterEntry> desktopStarterCatalog(const std::vector<MaiToolSpec>& tools) {
    struct Group {
        QString name;
        QString symbol;
        QString purpose;
        // Names define presentation families only. The registry remains the
        // authority for visibility, model support, dispatch and approval.
        QString toolNames;
        QString goal;
    };
    const QList<Group> groups = {
        {QStringLiteral("视频创作"), QStringLiteral("视"),
         QStringLiteral("了解当前可用的视频模型；先确认模型、素材、时长和画幅，再按次批准生成。"),
         QStringLiteral("kling_bailian_video minimax_video seedance_video wan_video"),
         QStringLiteral("先了解它支持的视频创作方式和需要的素材，暂不提交生成任务。")},
        {QStringLiteral("文件处理"), QStringLiteral("文"),
         QStringLiteral("查找、读取、新建、编辑和删除本地文件与目录。"),
         QStringLiteral("file_glob file_read file_grep file_create file_create_directory file_write file_edit file_patch file_delete"),
         QStringLiteral("查找工作目录中的会议记录，列出文件路径。")},
        {QStringLiteral("文件与媒体交付"), QStringLiteral("传"),
         QStringLiteral("将已有文件、图片、视频或音频展示为当前对话中的持久卡片。"),
         QStringLiteral("agent_send_file agent_send_media"),
         QStringLiteral("把工作目录中指定的已有文件展示到当前对话，保留原文件。")},
        {QStringLiteral("图片查看"), QStringLiteral("图"),
         QStringLiteral("让支持图片的当前主模型查看已有图片，说明画面内容。"),
         QStringLiteral("view_image"), QStringLiteral("查看工作目录中的指定图片，说明画面内容。")},
        {QStringLiteral("电脑操作"), QStringLiteral("控"),
         QStringLiteral("像人一样查看屏幕，使用鼠标和键盘操作电脑。"),
         QStringLiteral("desktop_computer screenshot"), QStringLiteral("在电脑上完成我指定的操作。")},
        {QStringLiteral("网页检索"), QStringLiteral("搜"),
         QStringLiteral("使用当前主模型供应商的搜索与网页读取能力查找资料。"),
         QStringLiteral("web_search web_fetch"), QStringLiteral("搜索指定主题的公开资料，并给出来源。")},
        {QStringLiteral("网络请求"), QStringLiteral("网"),
         QStringLiteral("按指定网址发起 HTTP 请求、读取响应或传输文件；先确认目标和要发送的内容。"),
         QStringLiteral("curl curl_fetch curl_request curl_download curl_upload"),
         QStringLiteral("查看指定 HTTPS 网页的响应头，不发送账号凭据。")},
        {QStringLiteral("浏览器操作"), QStringLiteral("览"),
         QStringLiteral("打开网页、读取页面内容并操作网页。"), QStringLiteral("desktop_browser"),
         QStringLiteral("打开指定网页并找到下载入口，先不要下载。")},
        {QStringLiteral("终端与进程"), QStringLiteral("终"),
         QStringLiteral("运行终端命令，启动、读取、输入和停止本会话管理的进程。"),
         QStringLiteral("shell desktop_process_start desktop_process_read desktop_process_write desktop_process_stop"),
         QStringLiteral("列出当前运行的进程名称和进程 ID。")},
        {QStringLiteral("SSH 远程命令"), QStringLiteral("连"),
         QStringLiteral("连接指定 SSH 主机执行命令；密码和主机指纹交给本机确认。"),
         QStringLiteral("ssh_exec"), QStringLiteral("连接我指定的主机查看系统版本，先确认主机、端口和用户名。")},
        {QStringLiteral("会话定时任务"), QStringLiteral("时"),
         QStringLiteral("安排、查看、修改或取消本会话未来执行的任务。"),
         QStringLiteral("maichat_session_task_list maichat_session_task_create maichat_session_task_update maichat_session_task_cancel"),
         QStringLiteral("查看这个会话中尚未执行的定时任务。")},
        {QStringLiteral("日历导入"), QStringLiteral("历"),
         QStringLiteral("准备日历事件，交给系统日历确认导入。"),
         QStringLiteral("maichat_calendar_create_event"),
         QStringLiteral("准备明天下午三点的项目评审日程，交给我确认导入。")},
        {QStringLiteral("二维码生成"), QStringLiteral("码"),
         QStringLiteral("将文字或网址生成可保存的二维码图片。"),
         QStringLiteral("maichat_generate_qr_code"), QStringLiteral("把这个网址制作成 PNG 二维码。")},
        {QStringLiteral("PDF 文档"), QStringLiteral("页"),
         QStringLiteral("将整理好的 HTML 内容生成为新的 PDF 文件。"),
         QStringLiteral("generate_pdf"), QStringLiteral("把整理好的会议纪要排版为新的 PDF 文件，先确认内容。")},
        {QStringLiteral("媒体信息"), QStringLiteral("媒"),
         QStringLiteral("查看已有音视频的时长、分辨率、音轨和格式信息。"),
         QStringLiteral("ffprobe"), QStringLiteral("查看指定本地视频的时长、分辨率和音轨。")},
        {QStringLiteral("本地媒体转换"), QStringLiteral("剪"),
         QStringLiteral("转换、裁剪和组合已有本地音视频，按要求另存结果。"),
         QStringLiteral("ffmpeg video_stretch_lower"),
         QStringLiteral("把指定本地视频裁剪为选定时间范围，另存新文件并保留原片。")},
        {QStringLiteral("视频变化分析"), QStringLiteral("帧"),
         QStringLiteral("测量镜头切换与可见运动的时间片段，不将数值分析当作剧情理解。"),
         QStringLiteral("cv_scene_detect cv_motion_detect"),
         QStringLiteral("分析指定本地视频的画面变化，列出时间片段并说明分析边界。")},
        {QStringLiteral("图像测量与几何分析"), QStringLiteral("测"),
         QStringLiteral("测量清晰度与像素差异，提取轮廓、边缘、直线和文档角点；结果不代表语义识别。"),
         QStringLiteral("cv_image_quality cv_image_compare cv_find_contours cv_detect_edges cv_match_template cv_register_translation cv_detect_lines cv_document_corners cv_threshold_mask"),
         QStringLiteral("测量指定图片的清晰度、亮度和对比度，说明这些数值的含义。")},
        {QStringLiteral("人脸与人像区域"), QStringLiteral("人"),
         QStringLiteral("定位已有图片中的人脸关键点，或另存人物区域遮罩；不识别人物身份。"),
         QStringLiteral("detect_faces segment_person"),
         QStringLiteral("分析指定图片中的人物区域，说明检测能力和返回结果。")},
        {QStringLiteral("视频人物抠像"), QStringLiteral("抠"),
         QStringLiteral("对已有视频执行本地人物抠像，保留原视频并另存结果。"),
         QStringLiteral("cv_video_matting"), QStringLiteral("为指定本地视频准备人物抠像，先确认输出和背景要求。")},
        {QStringLiteral("本地人像处理"), QStringLiteral("像"),
         QStringLiteral("对已有照片做本地肤色、皮肤或轻度体形调整；保留原图和可说明的处理范围。"),
         QStringLiteral("mobile_beautify_image mobile_reshape_body"),
         QStringLiteral("为指定人像照片做轻度皮肤调整，先确认参数，另存结果并保留原图。")},
        {QStringLiteral("人像偏好"), QStringLiteral("选"),
         QStringLiteral("记录你明确选择的同源人像结果，查看或训练本机个人偏好模型。"),
         QStringLiteral("model_continue_training mobile_portrait_feedback"),
         QStringLiteral("查看当前本机人像偏好模型的状态，不开始训练。")},
        {QStringLiteral("拍照"), QStringLiteral("拍"),
         QStringLiteral("打开本机相机，由你点击快门拍照。"),
         QStringLiteral("maichat_camera_command"), QStringLiteral("打开相机，我来点击快门拍照。")},
        {QStringLiteral("音乐播放"), QStringLiteral("音"),
         QStringLiteral("在 MaiChat 播放器中打开已有本地音乐。"),
         QStringLiteral("maichat_play_local_music"), QStringLiteral("播放工作目录中的指定音乐文件。")},
        {QStringLiteral("视频播放"), QStringLiteral("播"),
         QStringLiteral("打开已有本地视频，查看或控制播放进度与播放状态。"),
         QStringLiteral("maichat_play_video maichat_video_command"), QStringLiteral("打开工作目录中的指定视频。")},
        {QStringLiteral("我的云盘"), QStringLiteral("云"),
         QStringLiteral("查看私有云盘文件与文件夹，搜索、上传、下载、打开或整理已有媒体。"),
         QStringLiteral("maichat_cloud_drive_list_media maichat_cloud_drive_search_media maichat_cloud_drive_list_pending_uploads maichat_cloud_drive_upload_media maichat_cloud_drive_pause_upload maichat_cloud_drive_resume_upload maichat_cloud_drive_abort_upload maichat_cloud_drive_open_media maichat_cloud_drive_download_media maichat_cloud_drive_delete_media maichat_cloud_drive_list_folders maichat_cloud_drive_create_folder maichat_cloud_drive_update_folder maichat_cloud_drive_delete_folder maichat_cloud_drive_move_media maichat_cloud_drive_rename_media"),
         QStringLiteral("查看我的云盘是否可用，并列出已有文件。")},
        {QStringLiteral("会话附件"), QStringLiteral("附"),
         QStringLiteral("打开已分享到当前会话的附件，将已有附件取回本会话工作区。"),
         QStringLiteral("maichat_session_attachment_open"), QStringLiteral("打开这个会话中我指定的已有附件。")},
        {QStringLiteral("MaiChat 消息"), QStringLiteral("聊"),
         QStringLiteral("查找本机已保存的聊天记录、查看未读概况，按要求发送、引用回复或群发消息。"),
         QStringLiteral("maichat_search_messages maichat_list_conversations maichat_get_messages maichat_get_unread_summary maichat_send_text maichat_send_media maichat_reply_message maichat_broadcast_text"),
         QStringLiteral("查找本周提到“项目资料”的消息，列出匹配结果。")},
        {QStringLiteral("联系人与群聊"), QStringLiteral("友"),
         QStringLiteral("查找联系人和已加入的群聊，按要求申请好友、建群或发送群消息。"),
         QStringLiteral("maichat_list_contacts maichat_request_friend maichat_list_groups maichat_create_group maichat_get_group_messages maichat_send_group_text"),
         QStringLiteral("查找名字中包含“小明”的好友，列出匹配联系人。")},
        {QStringLiteral("账号资料与头像"), QStringLiteral("账"),
         QStringLiteral("查看当前账号资料，按要求修改昵称或头像。"),
         QStringLiteral("maichat_get_current_account maichat_set_nickname maichat_set_avatar"),
         QStringLiteral("查看当前账号的昵称和头像。")},
        {QStringLiteral("修改密码"), QStringLiteral("密"),
         QStringLiteral("打开本机私有密码表单，由你输入密码并确认修改。"),
         QStringLiteral("maichat_change_password"), QStringLiteral("打开修改密码表单，我自己填写密码。")},
        {QStringLiteral("会话与历史"), QStringLiteral("史"),
         QStringLiteral("查看会话与原始历史，创建、改名、删除会话，切换工作区或向已有会话补充输入。"),
         QStringLiteral("session_list session_history session_create session_rename session_delete session_set_workspace session_send_input"),
         QStringLiteral("列出本机已有会话及其名称。")},
        {QStringLiteral("会话消息绑定"), QStringLiteral("绑"),
         QStringLiteral("查看、绑定或解除好友聊天与本机创作会话的自动转发关系。"),
         QStringLiteral("maichat_session_im_binding"), QStringLiteral("查看当前好友聊天与创作会话的绑定关系，先不修改。")},
        {QStringLiteral("跨设备会话"), QStringLiteral("跨"),
         QStringLiteral("查看其他在线设备的会话，按要求创建、删除会话或调整工作区。"),
         QStringLiteral("maichat_remote_session_list maichat_remote_session_create maichat_remote_session_delete maichat_remote_session_set_workspace"),
         QStringLiteral("查看其他在线设备上的会话，列出设备和会话名称。")},
        {QStringLiteral("任务清单"), QStringLiteral("单"),
         QStringLiteral("将当前任务拆成清单，更新执行进度。"), QStringLiteral("todowrite"),
         QStringLiteral("把这项工作拆成待办清单，先不要执行。")},
        {QStringLiteral("并行助手"), QStringLiteral("并"),
         QStringLiteral("查看、创建、补充、等待或关闭本会话的子助手。"),
         QStringLiteral("list_agents spawn_agent send_input wait_agent close_agent"),
         QStringLiteral("查看这个会话中已有子助手的状态。")},
        {QStringLiteral("补充需求"), QStringLiteral("问"),
         QStringLiteral("在目标、参数或选择不明确时，用结构化问题收集你的补充。"),
         QStringLiteral("question"), QStringLiteral("先确认这项任务的目标、约束和完成条件。")},
        {QStringLiteral("工具结果补读"), QStringLiteral("续"),
         QStringLiteral("分段读取之前保存的长工具结果，沿用已有输出而不重复执行原操作。"),
         QStringLiteral("tool_output_read"), QStringLiteral("继续读取上一项工具返回的长结果，沿用已有输出编号。")},
        {QStringLiteral("本地模型推理"), QStringLiteral("本"),
         QStringLiteral("使用已有 GGUF 模型执行有长度限制的本地文本或图像任务。"),
         QStringLiteral("llama"), QStringLiteral("用已有 GGUF 模型总结指定文字，先确认模型路径和上下文长度。")},
        {QStringLiteral("压缩与解压"), QStringLiteral("压"),
         QStringLiteral("将文件压缩为 gzip、zlib 或 deflate，或解压到新文件，保留原文件。"),
         QStringLiteral("zlib_compress zlib_decompress"), QStringLiteral("将指定文件压缩为新的 gzip 文件，保留原文件。")},
        {QStringLiteral("摘要与证书检查"), QStringLiteral("验"),
         QStringLiteral("计算文件摘要或查看已有证书的信息，不修改输入文件。"),
         QStringLiteral("crypto_digest crypto_certificate_info openssl"),
         QStringLiteral("计算指定文件的 SHA-256 摘要，保留原文件。")},
        {QStringLiteral("应用资源状态"), QStringLiteral("机"),
         QStringLiteral("查看 MaiChat 进程的 CPU、内存等资源状态和当前接通的 GPU 信息。"),
         QStringLiteral("system_resources"), QStringLiteral("查看 MaiChat 当前的内存和 CPU 使用情况。")},
        {QStringLiteral("网络地址"), QStringLiteral("址"),
         QStringLiteral("查看本机网络接口地址，按要求查询公网出口地址。"),
         QStringLiteral("network_ip"), QStringLiteral("查看本机网络接口地址，先不查询公网地址。")},
        {QStringLiteral("当前时间"), QStringLiteral("钟"),
         QStringLiteral("读取当前时间与时区，为计划和日程提供时间依据。"),
         QStringLiteral("current_time"), QStringLiteral("查看当前时间和时区。")},
        {QStringLiteral("应用存储整理"), QStringLiteral("存"),
         QStringLiteral("预览应用缓存与可再生临时文件，确认后再清理指定范围。"),
         QStringLiteral("app_storage"), QStringLiteral("预览可以清理的应用缓存，先不要删除。")},
    };

    QSet<QString> available;
    for (const auto& tool : tools) available.insert(QString::fromStdString(tool.name));
    QSet<QString> covered;
    // 普通聊天仍使用原输入框；欢迎页只列当前声明可用的工具能力，不另设问答卡片。
    QList<StarterEntry> entries;
    for (const auto& group : groups) {
        QStringList names;
        const QStringList candidates = group.toolNames.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        for (const QString& name : candidates) {
            if (available.contains(name) && !covered.contains(name)) names.append(name);
        }
        if (names.isEmpty()) continue;
        // The first currently available member supplies the example's exact ID.
        // A partial host registration must not inherit another member's recipe.
        QString goal = names.first() == candidates.first()
                           ? group.goal
                           : QStringLiteral("先说明这项能力支持的操作和所需参数，缺少目标时先问我。");
        QString purpose = group.purpose;
        QString symbol = group.symbol;
        // 截图与电脑输入属于同一张卡；部分注册时只描述真实可用的成员，
        // 不能因卡片合并就向仅有截图的模型承诺鼠标或键盘操作。
        if (candidates.contains(QStringLiteral("desktop_computer")) &&
            !names.contains(QStringLiteral("desktop_computer"))) {
            purpose = QStringLiteral("查看当前电脑屏幕，说明画面内容。");
            symbol = QStringLiteral("屏");
            goal = QStringLiteral("查看当前屏幕上的报错，说明报错内容。");
        }
        entries.append({group.name, symbol, purpose,
                        QStringLiteral("使用%1工具（%2）帮我%3").arg(group.name, names.first(), goal), names});
        for (const QString& name : names) covered.insert(name);
    }
    // New public host tools stay discoverable even before a curated family is
    // added. Internal/disabled tools can never enter via this fallback because
    // the caller passes the same model-filtered specs used by real dispatch.
    for (const auto& tool : tools) {
        const QString name = QString::fromStdString(tool.name);
        if (covered.contains(name)) continue;
        entries.append({name, QStringLiteral("具"), QString::fromStdString(tool.description),
                        QStringLiteral("使用工具（%1）帮我处理指定任务，先确认目标和所需参数。").arg(name), {name}});
        covered.insert(name);
    }
    return entries;
}
