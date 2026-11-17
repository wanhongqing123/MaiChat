# MaiChat 全工具平面审阅清单

2026-11-16。**先列全，再由用户决定分组；这不是已更新到现装界面的工具页。**

- 主表合并156个唯一public候选ID：Mobile源并集137项，加桌面getter的19项桌面特有ID。是项目源码/条件候选全集口径，不代表用户当前全部可用；模型、权限、配置、SDK与资源条件仍需满足。
- 桌面getter117项，Mobile候选Android132/iOS133；不能把它们相加当总数。另8内部项与5个未挂载实现单列，不把“代码存在”说成“已启用”。
- 基础聊天没有工具ID，“AI问答”不计工具。正式question是向用户提问并等待回答，仍列出。
- 视频播放与控制分别为maichat_play_video和maichat_video_command；音乐桌面/移动ID分别保留。删除图片/视频介绍卡不等于停用全部供应商接口，MiniMax/Kling视频按源注册条件如实列出。
- 每个ID一行，中文名/示例为审阅稿，未替用户定组。CSV另含精确平台/模型/其他条件，可在空白分组列填写。当前模型和凭据未查询，未运行任何工具或模型；Mac按共用注册器/构建条件推导，不声称查询现装Mac实例。

## 主表

“桌面”含Windows/macOS源码条件；Windows只读getter在自有内存上下文验证，未查询stock会话。“条件/未列出”请看CSV的完整条件。主要操作是schema枚举或固定功能概述，CLI各参数不另算工具。

| 序号 | 工具ID | 中文名 | 用途 | 平台候选 | 桌面主要操作 | 移动主要操作 | 简短示例 | 用户分组 |
|---|---|---|---|---|---|---|---|---|
| 1 | agent_send_file | 交付文件 | 把工作区已有文件作为卡片放到当前创作中心回复中，供查看或保存 | 桌面 / Android / iOS | 固定功能/详参数 | send_file | 把报告PDF作为本轮聊天附件发给我。 |  |
| 2 | agent_send_media | 交付图片或视频 | 把已有工作区媒体显示为当前AI聊天卡片 | 桌面 / Android / iOS | 固定功能/详参数 | image / video | 把工作区这张处理后的图片展示在当前聊天里。 |  |
| 3 | app_storage | 扫描与预览清理存储 | 扫描App受管目录并先预览后按凭据清理 | 桌面 / Android（条件） / iOS（条件） | scan / cleanup | scan / cleanup | 先预览App临时文件占用和可清理项。 |  |
| 4 | close_agent | 关闭子Agent | 关闭不再需要的子Agent并释放并行名额 | 桌面 | 固定功能/详参数 | — | 关闭已经完成的子Agent。 |  |
| 5 | crypto_certificate_info | 读取证书信息 | 读取工作区证书的主体、签发者、有效期和指纹 | 桌面 | 固定功能/详参数 | — | 查看工作区证书的有效期。 |  |
| 6 | crypto_digest | 计算文件或文字摘要 | 计算SHA系列等校验摘要 | 桌面 | 固定功能/详参数 | — | 计算这个文件的SHA256。 |  |
| 7 | curl | HTTP与文件传输 | 运行受限libcurl命令进行网络请求或文件传输 | 桌面 / Android / iOS | 固定功能/详参数 | curl参数 | 读取这个公开网页的正文。 |  |
| 8 | current_time | 查询当前时间 | 返回当前时间供时间相关任务使用 | 桌面 / Android / iOS | 固定功能/详参数 | query | 现在当地时间是几点？ |  |
| 9 | cv_detect_edges | 检测图像边缘 | 分析或输出图像边缘 | 桌面 / Android / iOS | 固定功能/详参数 | edges | 提取这张图的边缘，另存一张结果图。 |  |
| 10 | cv_detect_lines | 检测图像直线 | 返回图像线段/直线信息 | 桌面 / Android / iOS | 固定功能/详参数 | lines | 找出照片里的直线。 |  |
| 11 | cv_document_corners | 检测文档四角 | 定位图像中的文档角点 | 桌面 / Android / iOS | 固定功能/详参数 | document_corners | 找出这张纸的四个角。 |  |
| 12 | cv_find_contours | 查找图像轮廓 | 提取现有图像轮廓信息 | 桌面 / Android / iOS | 固定功能/详参数 | contours | 提取图像里的封闭轮廓。 |  |
| 13 | cv_image_compare | 比较图像指标 | 比较两幅图像的数值差异 | 桌面 / Android / iOS | 固定功能/详参数 | compare | 比较两张图是否有明显变化。 |  |
| 14 | cv_image_quality | 图像质量指标 | 分析现有图像质量数值 | 桌面 / Android / iOS | 固定功能/详参数 | quality | 检查这张图片的清晰度和亮度。 |  |
| 15 | cv_match_template | 图像模板匹配 | 在图像中查找指定模板 | 桌面 / Android / iOS | 固定功能/详参数 | template_match | 在大图里找出这个小图的位置。 |  |
| 16 | cv_motion_detect | 检测视频运动时段 | 用帧差分析可见运动并返回时间段 | 桌面 / Android / iOS | 固定功能/详参数 | motion_detect | 找出视频中运动明显的时间段。 |  |
| 17 | cv_register_translation | 图像平移配准 | 估计图像之间的平移 | 桌面 / Android / iOS | 固定功能/详参数 | register_translation | 估算两张照片的平移偏差。 |  |
| 18 | cv_scene_detect | 检测视频镜头变化 | 用OpenCV返回视觉场景变化时间段而非语义理解 | 桌面 / Android / iOS | 固定功能/详参数 | scene_detect | 列出这段视频的镜头切换时间。 |  |
| 19 | cv_threshold_mask | 创建阈值遮罩 | 按阈值生成局部处理遮罩 | 桌面 / Android / iOS | 固定功能/详参数 | threshold | 根据亮度阈值生成一张蒙版。 |  |
| 20 | cv_video_matting | 本地视频抠像 | 通过RVM与ORT生成新抠像视频 | 桌面（条件） / Android（条件） / iOS（条件） | 同共享功能，条件注册 | matting | 把视频中的人物抠出并生成新文件。 |  |
| 21 | desktop_browser | 操作桌面浏览器 | 打开隔离的Chrome/Edge页面并查看、截图、点击或输入 | 桌面 | open / observe / screenshot / click / type / close | — | 打开指定网站并查看页面。 |  |
| 22 | desktop_computer | 操作电脑 | 读取屏幕/窗口画面并执行原生输入；具体动作受平台与依赖条件限制 | 桌面 | list / list_displays / list_keys / observe / focus / move / click / double_click / right_click / drag / mouse_down / mouse_up / type / replace_text / key / key_down / key_up / scroll / input_state / release_all / verify / clear / input_events / input_capabilities / feedback | — | 查看这个窗口，并按我的要求操作。 |  |
| 23 | desktop_process_read | 读取后台进程输出 | 读取本会话启动的后台进程的新输出 | 桌面 | 固定功能/详参数 | — | 读取刚启动进程的新输出。 |  |
| 24 | desktop_process_start | 启动后台进程 | 在本会话工作目录启动可持续运行的命令 | 桌面 | 固定功能/详参数 | — | 在当前工作目录启动这个长时间任务。 |  |
| 25 | desktop_process_stop | 停止后台进程 | 停止本会话拥有的后台进程 | 桌面 | 固定功能/详参数 | — | 停止刚才启动的进程。 |  |
| 26 | desktop_process_write | 向后台进程输入 | 向本会话运行中进程的标准输入发送文字 | 桌面 | 固定功能/详参数 | — | 向刚才启动的交互进程输入这段文字。 |  |
| 27 | detect_faces | 检测人脸 | 检测图像中的人脸位置和关键点 | 桌面 | 固定功能/详参数 | — | 检测这张图片中的人脸位置。 |  |
| 28 | ffmpeg | 本地媒体转换 | 对已授权媒体执行嵌入式FFmpeg处理并输出新文件 | 桌面 / Android / iOS | 固定功能/详参数 | ffmpeg参数 | 把这个工作区视频转成MP4。 |  |
| 29 | ffprobe | 本地媒体信息 | 查询已有媒体流/时长/编码信息 | 桌面 / Android / iOS | 固定功能/详参数 | ffprobe参数 | 查看这段视频的编码、时长和分辨率。 |  |
| 30 | file_create | 新建文件 | 在授权路径创建新文件 | 桌面 / Android / iOS | 固定功能/详参数 | create | 在工作区新建一份待办文本。 |  |
| 31 | file_create_directory | 新建目录 | 创建授权工作区目录 | 桌面 / Android / iOS | 固定功能/详参数 | mkdir | 在工作区建一个素材目录。 |  |
| 32 | file_delete | 删除文件 | 按权限删除指定文件而非目录整树 | 桌面 / Android / iOS | 固定功能/详参数 | delete | 删除我指定的这份临时文件。 |  |
| 33 | file_edit | 定点编辑文件 | 用明确旧新文本修改现有文件 | 桌面 / Android / iOS | 固定功能/详参数 | replace | 把文档中的这一处日期替换掉。 |  |
| 34 | file_glob | 查找文件名 | 按路径和模式分页查找文件 | 桌面 / Android / iOS | 固定功能/详参数 | glob | 列出工作区里的所有PDF文件。 |  |
| 35 | file_grep | 检索文件内容 | 有界文字或正则检索并返回行/文件/计数 | 桌面 / Android / iOS | 固定功能/详参数 | literal / regex;lines / files / count | 查找哪些文档提到了这个词。 |  |
| 36 | file_patch | 应用文件补丁 | 以补丁创建或修改指定文件 | 桌面 / Android / iOS | 固定功能/详参数 | patch | 把这份明确的补丁应用到工作区文件。 |  |
| 37 | file_read | 读取文件 | 读取工作区文本并支持有界分页 | 桌面 / Android / iOS | 固定功能/详参数 | read / 分页 | 读一下这份说明的前100行。 |  |
| 38 | file_write | 写入文件 | 写入指定文件内容 | 桌面 / Android / iOS | 固定功能/详参数 | write | 把整理后的内容写入这份工作区文本。 |  |
| 39 | generate_pdf | 生成PDF文档 | 由宿主渲染HTML并可使用私有密码表单保护输出PDF | 桌面 / Android / iOS | 固定功能/详参数 | generate / encrypt | 把这段排版好的内容生成PDF。 |  |
| 40 | kling_bailian_video | 百炼可灵视频任务 | 通过百炼Kling创建或参考/编辑视频，查询既有任务、取消排队任务或删除本地交付记录 | 桌面 / Android / iOS | discover / diagnose / delegate / continue / cancel / delete | discover / diagnose / delegate / continue / cancel / delete | 先列出可选Kling视频型号及限制，确认后再生成。 |  |
| 41 | list_agents | 查看子Agent | 查看已创建子Agent的运行状态 | 桌面 | 固定功能/详参数 | — | 列出我这次任务的子Agent状态。 |  |
| 42 | llama | 本地模型推理 | 使用驻留本地推理引擎与已授权模型文件 | 桌面 / Android / iOS | 固定功能/详参数 | llama参数 / 推理 | 用已有本地GGUF模型生成一段短文。 |  |
| 43 | maichat_broadcast_text | 群发指定联系人文字 | 仅向指定联系人集合发送文字 | 桌面 / Android / iOS | 固定功能/详参数 | broadcast | 把这段通知发送给我指定的这些联系人。 |  |
| 44 | maichat_calendar_create_event | 新建系统日历事件 | 通过授权日历能力创建指定时间事件 | 桌面 / Android / iOS | 固定功能/详参数 | create_event | 为明天下午的会议创建系统日历事件。 |  |
| 45 | maichat_camera_command | 控制自动相机采样 | iOS仅按用户明确要求每两秒保存图像并查询/停止 | 桌面 / iOS | open / status / close | open / status / close | 打开本轮会话的相机预览，等我拍摄。 |  |
| 46 | maichat_change_password | 打开安全修改密码流程 | 用系统安全表单修改密码且模型不接触密码 | 桌面 / Android / iOS | open / status | open / status | 打开安全修改密码表单，由我在设备上填写。 |  |
| 47 | maichat_cloud_drive_abort_upload | 中止云盘上传 | 明确中止上传并按契约回收分片 | 桌面 / Android / iOS | 固定功能/详参数 | abort | 终止这一项云盘上传。 |  |
| 48 | maichat_cloud_drive_create_folder | 新建云盘文件夹 | 创建指定父目录下的新文件夹 | 桌面 / Android / iOS | 固定功能/详参数 | create | 在云盘创建一个资料文件夹。 |  |
| 49 | maichat_cloud_drive_delete_folder | 删除空云盘文件夹 | 只删除指定空文件夹而非递归内容 | 桌面 / Android / iOS | 固定功能/详参数 | delete_empty | 删除这个空的云盘文件夹。 |  |
| 50 | maichat_cloud_drive_delete_media | 删除个人云盘文件 | 删除用户明确指定的个人云盘对象 | 桌面 / Android / iOS | 固定功能/详参数 | delete | 删除我指定的这一项云盘文件。 |  |
| 51 | maichat_cloud_drive_download_media | 下载云盘文件到工作区 | 把选定私人文件下载到本机工作区 | 桌面 / Android / iOS | 固定功能/详参数 | download | 把这个云盘文件下载到当前工作区。 |  |
| 52 | maichat_cloud_drive_list_folders | 列出云盘文件夹 | 查看私人云盘目录元数据 | 桌面 / Android / iOS | 固定功能/详参数 | list | 列出当前云盘文件夹。 |  |
| 53 | maichat_cloud_drive_list_media | 列出个人云盘文件 | 查询当前账号个人云盘已有文件 | 桌面 / Android / iOS | 固定功能/详参数 | list | 看看我的云盘有哪些文件。 |  |
| 54 | maichat_cloud_drive_list_pending_uploads | 列出云盘上传断点 | 查看可恢复的本机上传任务 | 桌面 / Android / iOS | 固定功能/详参数 | list_pending | 列出尚未完成的上传任务。 |  |
| 55 | maichat_cloud_drive_move_media | 移动云盘文件 | 改变已有文件所属文件夹 | 桌面 / Android / iOS | 固定功能/详参数 | move | 把这个云盘文件移到指定文件夹。 |  |
| 56 | maichat_cloud_drive_open_media | 打开云盘媒体 | 在App中打开已有云盘媒体 | 桌面 / Android / iOS | 固定功能/详参数 | open | 打开这项云盘素材供当前会话使用。 |  |
| 57 | maichat_cloud_drive_pause_upload | 暂停云盘上传 | 暂停指定上传并保留恢复状态 | 桌面 / Android / iOS | 固定功能/详参数 | pause | 暂停这一项上传。 |  |
| 58 | maichat_cloud_drive_rename_media | 重命名云盘文件 | 更新显示名称而不改远端对象身份 | 桌面 / Android / iOS | 固定功能/详参数 | rename | 把这个云盘文件改成指定名称。 |  |
| 59 | maichat_cloud_drive_resume_upload | 继续云盘上传 | 继续指定未完成上传 | 桌面 / Android / iOS | 固定功能/详参数 | resume | 继续刚才暂停的上传。 |  |
| 60 | maichat_cloud_drive_search_media | 搜索个人云盘文件 | 按条件搜索当前账号云盘媒体 | 桌面 / Android / iOS | 固定功能/详参数 | search | 在云盘中查找名称包含合同的文件。 |  |
| 61 | maichat_cloud_drive_update_folder | 修改云盘文件夹 | 重命名或移动文件夹元数据 | 桌面 / Android / iOS | 固定功能/详参数 | rename / move | 给这个云盘文件夹改名。 |  |
| 62 | maichat_cloud_drive_upload_media | 上传到个人云盘 | 把用户选定的受支持文件上传到私人空间 | 桌面 / Android / iOS | 固定功能/详参数 | upload | 把工作区这份文件上传到云盘。 |  |
| 63 | maichat_create_group | 创建MaiChat群聊 | 按用户要求创建指定成员的群聊 | 桌面 / Android / iOS | 固定功能/详参数 | create | 用这些MaiChat联系人创建一个群。 |  |
| 64 | maichat_generate_qr_code | 生成二维码图片 | 将文字或URL生成可扫描二维码PNG | 桌面 / Android / iOS | 固定功能/详参数 | generate | 把这个链接做成二维码图片。 |  |
| 65 | maichat_get_current_account | 查看当前账号资料 | 读取当前登录资料快照且不返回令牌/密码 | 桌面 / Android / iOS | 固定功能/详参数 | read | 查看我当前登录的MaiChat账号资料。 |  |
| 66 | maichat_get_group_messages | 读取群聊历史 | 读取指定群聊的已有消息 | 桌面 / Android / iOS | 固定功能/详参数 | read | 查看这个MaiChat群最近的消息。 |  |
| 67 | maichat_get_messages | 读取私聊消息 | 按所属联系人/会话读取已有消息 | 桌面 / Android / iOS | 固定功能/详参数 | read / 分页 | 读取这个MaiChat会话最近的消息。 |  |
| 68 | maichat_get_unread_summary | 查看未读概况 | 按联系人汇总未读消息 | 桌面 / Android / iOS | 固定功能/详参数 | summary | 汇总MaiChat当前未读消息数量。 |  |
| 69 | maichat_list_contacts | 查看MaiChat联系人 | 读取当前IM联系人列表而非手机通讯录 | 桌面 / Android / iOS | 固定功能/详参数 | list | 查找我的MaiChat联系人。 |  |
| 70 | maichat_list_conversations | 列出IM会话 | 查询本机可访问的IM会话列表 | 桌面 / Android / iOS | 固定功能/详参数 | list | 列出最近的MaiChat聊天会话。 |  |
| 71 | maichat_list_groups | 查看MaiChat群聊 | 查询当前账号群聊 | 桌面 / Android / iOS | 固定功能/详参数 | list | 列出我加入的MaiChat群。 |  |
| 72 | maichat_list_safety_reports | 查看安全举报记录 | 查询当前账号的举报记录 | Android | — | list | 查看我提交的安全举报记录。 |  |
| 73 | maichat_play_local_music | 播放桌面本地音乐 | 在MaiChat播放器打开工作区音乐，或由用户选择文件 | 桌面 | 固定功能/详参数 | — | 播放工作区里的音乐文件。 |  |
| 74 | maichat_play_video | 打开工作区视频播放 | 在同一MaiChat播放器打开已有视频并可指定起播秒数 | 桌面 / Android / iOS | 固定功能/详参数 | open / start_seconds | 播放工作区的视频，从第8秒开始。 |  |
| 75 | maichat_remote_session_create | 创建远端会话 | 向所属设备请求创建Session | 桌面 / Android / iOS | 固定功能/详参数 | create | 在指定在线设备创建一个Agent会话。 |  |
| 76 | maichat_remote_session_delete | 删除远端会话 | 向所属设备请求删除指定Session | 桌面 / Android / iOS | 固定功能/详参数 | delete | 删除指定远端设备上的这个Agent会话。 |  |
| 77 | maichat_remote_session_list | 列出跨设备会话 | 查看当前账号设备和远端Session目录 | 桌面 / Android / iOS | 固定功能/详参数 | list | 看看我的其他设备有哪些Agent会话。 |  |
| 78 | maichat_remote_session_set_workspace | 修改远端工作目录 | 请求所属设备切换指定Session工作目录 | 桌面 / Android / iOS | 固定功能/详参数 | set_workspace | 为指定远端Agent会话设置工作目录。 |  |
| 79 | maichat_reply_message | 回复MaiChat消息 | 以明确的引用关系回复已有消息 | 桌面 / Android / iOS | 固定功能/详参数 | reply | 在MaiChat中回复这条指定消息。 |  |
| 80 | maichat_request_account_deletion | 申请注销账号 | 经安全身份与确认流程申请账号注销 | Android | — | request | 为我的当前账号提交注销申请。 |  |
| 81 | maichat_request_friend | 申请添加好友 | 向明确联系人发送好友申请 | 桌面 / Android / iOS | 固定功能/详参数 | request | 向这个MaiChat用户发送好友申请。 |  |
| 82 | maichat_search_messages | 检索本机聊天记录 | 在授权本机历史中按条件检索消息 | 桌面 / Android / iOS | 固定功能/详参数 | search / 分页 | 在指定MaiChat会话中搜索这段关键词。 |  |
| 83 | maichat_send_group_text | 发送MaiChat群聊文字 | 向明确群聊发送文字 | 桌面 / Android / iOS | 固定功能/详参数 | send | 把这段文字发到指定MaiChat群。 |  |
| 84 | maichat_send_media | 发送MaiChat媒体 | 向明确联系人发送现有图片/视频等受支持媒体 | 桌面 / Android / iOS | 固定功能/详参数 | send | 把这个工作区媒体发给指定MaiChat会话。 |  |
| 85 | maichat_send_text | 发送MaiChat私聊文字 | 向明确联系人发送文字 | 桌面 / Android / iOS | 固定功能/详参数 | send | 给这个MaiChat联系人发一段文字。 |  |
| 86 | maichat_session_attachment_open | 打开所属Session附件 | 核对房间归属后下载并导入本地Session工作区 | 桌面 / Android / iOS | 固定功能/详参数 | open | 把这份会话附件导入当前工作区。 |  |
| 87 | maichat_session_im_binding | 绑定本机Session与好友会话 | 只为新的Human消息建立/解除当前账号本地绑定 | 桌面 / Android / iOS | list / bind / unbind | list / bind / unbind | 查看这个Agent会话的IM绑定关系。 |  |
| 88 | maichat_session_task_cancel | 取消会话定时任务 | 取消指定会话任务 | 桌面 / Android / iOS | 固定功能/详参数 | cancel | 取消这条会话定时任务。 |  |
| 89 | maichat_session_task_create | 创建会话定时任务 | 为指定Session建立本机定时任务 | 桌面 / Android / iOS | 固定功能/详参数 | create | 明天上午在这个会话提醒我检查报告。 |  |
| 90 | maichat_session_task_list | 查看会话定时任务 | 列出当前范围的定时任务 | 桌面 / Android / iOS | 固定功能/详参数 | list | 列出本会话的定时任务。 |  |
| 91 | maichat_session_task_update | 修改会话定时任务 | 更新已有任务的时间/启用等配置 | 桌面 / Android / iOS | 固定功能/详参数 | update | 把这条会话任务改到明天下午。 |  |
| 92 | maichat_set_avatar | 更换当前账号头像 | 从现有图片准备新头像并等待资料确认 | 桌面 / Android / iOS | 固定功能/详参数 | set_avatar | 打开头像选择流程，让我确认新的头像。 |  |
| 93 | maichat_set_nickname | 更改当前账号昵称 | 按用户要求更新账号昵称 | 桌面 / Android / iOS | 固定功能/详参数 | set | 把我的MaiChat昵称改成这个名称。 |  |
| 94 | maichat_video_command | 控制当前视频播放 | 控制已打开的播放器而非另存转码文件 | 桌面 / Android / iOS | status / play / pause / toggle_pause / step / replay / previous_item / next_item / seek_forward / seek_backward / seek_minute_forward / seek_minute_backward / next_chapter / previous_chapter / seek_percent / seek_seconds / set_speed / next_audio / next_video / next_subtitle / next_program / next_filter / mute / volume_up / volume_down / fullscreen / close | play / pause / toggle_pause / replay / step / seek_forward / seek_backward / seek_minute_forward / seek_minute_backward / next_chapter / previous_chapter / seek_percent / seek_seconds / set_speed / next_audio / next_video / next_subtitle / next_program / next_filter / mute / volume_up / volume_down / fullscreen / close | 把当前播放的视频暂停并跳到第8秒。 |  |
| 95 | media_remove_watermark | 本地视频水印修复 | 使用LaMa与邻近干净帧修复视频水印区域 | Android（条件） / iOS（条件） | — | repair | 修复我指定视频中这一块水印区域，另存结果。 |  |
| 96 | minimax_video | MiniMax视频任务 | 通过MiniMax生成视频、校验方案及查询既有任务；delete仅删已交付本地记录 | 桌面 / Android / iOS | discover / validate / delegate / continue / delete | discover / validate / delegate / continue / delete | 先核对MiniMax视频生成方案和费用条件，等我确认。 |  |
| 97 | mobile_beautify_image | 本地人像美颜 | 基于可靠人物/皮肤范围创建新美颜图像 | 桌面（条件） / Android / iOS | 同共享功能，条件注册 | smoothing / brightening / whitening / warmth / slim_face / enlarge_eyes / remove_body_hair | 对这张照片轻度磨皮、美白，另存新图。 |  |
| 98 | mobile_browser | 观察和操作内置浏览器 | 观察和操作当前Agent会话拥有的MaiChat内置网页；不控制外部浏览器 | Android / iOS | — | request.open / request.observe / request.screenshot / request.click / request.type / request.close | 打开这个网页，先观察页面再填写指定的普通字段。 |  |
| 99 | mobile_camera_frames | 查看已保存相机图像 | iOS列出或打开自动相机拍摄批次里用户选择的图像 | iOS | — | request.list / request.open / request.delete_batch | 列出本轮相机拍到的帧，让我选一张查看。 |  |
| 100 | mobile_capture_screen | 读取屏幕图像 | Android读已授权整屏帧；iOS可读当前App或最近外部帧 | Android / iOS（条件） | — | capture; iOS current_app / last_external / delay_ms | 读取本次已授权屏幕画面。 |  |
| 101 | mobile_compose_sms | 打开系统短信编辑器 | iOS预填收件人与正文供用户自己点击发送 | iOS | — | compose | 打开系统短信编辑器，内容由我检查后发送。 |  |
| 102 | mobile_detect_faces | 检测人脸与关键点 | 端侧返回人脸边框/关键点而不识别人身份 | Android / iOS | — | detect | 找出这张照片里的人脸位置。 |  |
| 103 | mobile_dial_phone | 拨打明确电话号码 | Android直接请求拨号/呼叫权限；iOS打开系统电话确认 | Android / iOS（条件） | — | call / confirm | 拨打我选定的这个电话号码。 |  |
| 104 | mobile_export_diagnostics | 导出当前App诊断 | 由宿主把受限诊断信息导出到受管文件 | Android / iOS | — | export | 导出本机MaiChat诊断文件供我查看。 |  |
| 105 | mobile_export_media_original | 导出相册原媒体 | 按资产ID导出原始照片或视频到工作区 | Android / iOS | — | photo / video | 把我选中的相册视频原文件导入工作区。 |  |
| 106 | mobile_export_photo_original | 导出相册原文件 | 按相册资产ID导出原图或配对视频并保留格式 | Android / iOS | — | photo / video | 导出这张已授权照片的原文件再做编辑。 |  |
| 107 | mobile_get_contact | 读取通讯录联系人 | 获取指定系统联系人信息 | Android / iOS | — | read | 查看我选中的系统联系人的电话号码。 |  |
| 108 | mobile_get_location | 获取当前前台位置 | 按系统授权读取一次前台位置 | Android / iOS | — | locate | 取得我当前的一次前台位置。 |  |
| 109 | mobile_get_phone_number | 读取本机运营商号码 | Android读取默认订阅号码且不视作验证身份 | Android | — | read | 读取当前默认SIM卡提供的本机号码。 |  |
| 110 | mobile_list_albums | 列出系统相册 | 查询当前权限允许的相册列表 | Android / iOS | — | list | 列出我允许MaiChat访问的系统相册。 |  |
| 111 | mobile_list_contacts | 列出手机通讯录 | 读取系统授权的通讯录联系人 | Android / iOS | — | list | 在手机系统通讯录查找这个姓名。 |  |
| 112 | mobile_list_photos | 列出授权相册素材 | 分页列出当前系统照片授权可见的照片/视频 | Android / iOS | — | list / offset / limit / album_id | 列出我授权相册最近的照片和视频。 |  |
| 113 | mobile_photos_add_to_album | 把照片加入iOS相册 | 创建或复用Photos相册并加入已有ID而不复制/删除原件 | iOS | — | add | 把这些已有系统照片加入指定相册。 |  |
| 114 | mobile_photos_copy_to_album | 复制照片到Android相册目录 | 复制所选现有素材到Pictures/MaiChat相册目录并保留原件 | Android | — | copy | 把这些照片复制到指定系统相册。 |  |
| 115 | mobile_play_local_music | 播放本地音乐 | 播放工作区音乐或让用户选择系统音乐/文件 | Android / iOS | — | choose / play | 播放工作区这首音乐，或让我选择一首。 |  |
| 116 | mobile_portrait_feedback | 记录明确人像偏好 | 保存用户对同一原图两结果的实际评价 | 桌面（条件） / Android / iOS | 同共享功能，条件注册 | record_explicit_feedback | 我更喜欢这次磨皮结果，请只记录这个明确偏好。 |  |
| 117 | mobile_preview_image | 全屏查看已有图片 | 仅在用户明确要求全屏/放大时打开查看器 | Android / iOS | — | preview | 把这张工作区图片全屏放大给我看。 |  |
| 118 | mobile_read_photo | 读取相册预览图 | 把指定素材的有界JPEG预览复制到工作区 | Android / iOS | — | read_preview | 读取我选中的这张相册照片预览。 |  |
| 119 | mobile_read_sms | 读取用户授权短信内容 | Android读受限收件箱；iOS仅读用户快捷指令导入内容 | Android / iOS | — | read / filter | 读取我明确授权或导入的最近几条短信。 |  |
| 120 | mobile_request_permission | 申请所需系统权限 | 只为当前任务请求相关系统能力并报告授权范围 | Android / iOS | — | photos / camera / microphone / location / contacts / calendar / notifications;Android另phone_call / phone_number / sms | 申请本次读取相册所需的照片权限。 |  |
| 121 | mobile_reshape_body | 本地身体比例塑形 | 用姿态与分割做有界腰部/腿部轻度几何调整 | 桌面（条件） / Android / iOS | 同共享功能，条件注册 | slim_waist / leg_length | 对这张站姿照片轻微收腰，另存新图。 |  |
| 122 | mobile_save_image | 保存新图片到相册 | 把现有工作区图片新增到系统图库而非覆盖原图 | Android / iOS | — | save_new | 把这张处理后的图片另存到系统相册。 |  |
| 123 | mobile_save_video | 保存新视频到相册 | 把受支持工作区视频新增到系统图库 | Android / iOS | — | save_new | 把工作区这个视频保存为相册新项目。 |  |
| 124 | mobile_screen_frames | 查看已保存屏幕截图 | iOS列出/打开用户选定的共享截图或清理停止的批次 | iOS（条件） | — | request.list / request.open / request.delete_batch | 列出本轮已停止共享后保存的屏幕帧。 |  |
| 125 | mobile_segment_person | 生成人物分割遮罩 | 端侧生成白人物黑背景的新灰度PNG | Android / iOS | — | segment | 为这张图片生成独立的人物蒙版。 |  |
| 126 | mobile_start_screen_share | 开始系统屏幕共享 | 由用户通过系统选择器启动整屏共享 | Android / iOS（条件） | — | start | 开始本轮由我授权的整屏共享。 |  |
| 127 | mobile_stop_screen_share | 停止系统屏幕共享 | 结束当前共享且iOS保留已保存截图 | Android / iOS（条件） | — | stop | 停止本轮整屏共享并保存当前批次。 |  |
| 128 | mobile_take_photo | 打开相机拍照 | 等待用户拍照并保存新工作区JPEG | Android / iOS | — | take_photo | 打开相机，等我拍一张照片。 |  |
| 129 | mobile_transform_image | 本地图像变换 | 生成新文件进行一种端侧图像编辑 | Android / iOS | — | crop / rotate / resize / flip_horizontal / flip_vertical / grayscale / sharpen / adjust / beautify | 把这张图片顺时针旋转90度，保留原图。 |  |
| 130 | model_continue_training | 训练本地偏好模型 | 训练/检查/回滚用户的小型人像偏好排名权重 | 桌面（条件） / Android / iOS | 同共享功能，条件注册 | train / status / versions / rollback | 用我已明确反馈的偏好训练并报告是否验证生效。 |  |
| 131 | network_ip | 查看网络地址 | 查询网络IP信息 | 桌面 / Android / iOS | 固定功能/详参数 | query | 查看设备可见的网络IP信息。 |  |
| 132 | openssl | 加密与证书命令 | 调用受限OpenSSL命令入口 | 桌面（条件） / Android / iOS | — | openssl参数 | 查看这份本地证书的有效期。 |  |
| 133 | question | 向用户确认信息 | 向用户提出聚焦问题并等待真实回答 | 桌面 / Android / iOS | 固定功能/详参数 | 提问 / 等待回答 | 请我确认海报用竖版还是方版，再继续制作。 |  |
| 134 | screenshot | 截取桌面屏幕 | 截取鼠标所在显示器的完整画面；不是窗口裁剪或浏览器视口 | 桌面 | capture / stop | — | 截取当前显示器的画面。 |  |
| 135 | segment_person | 分割图中人像 | 生成新的人像灰度蒙版，保留原图 | 桌面 | 固定功能/详参数 | — | 为这张照片生成人像蒙版。 |  |
| 136 | send_input | 向子Agent补充指令 | 向已创建子Agent补充信息或调整任务 | 桌面 | 固定功能/详参数 | — | 给这个子Agent补充以下要求。 |  |
| 137 | session_create | 创建会话 | 在移动默认工作目录创建新会话 | 桌面 / Android / iOS | 固定功能/详参数 | create | 创建一个新的本地Agent会话。 |  |
| 138 | session_delete | 删除会话 | 删除指定本地会话 | 桌面 / Android / iOS | 固定功能/详参数 | delete | 删除我指定的这个本地Agent会话。 |  |
| 139 | session_history | 读取会话历史 | 分页读取指定会话历史 | 桌面 / Android / iOS | 固定功能/详参数 | history | 查看这个Agent会话的已有历史。 |  |
| 140 | session_list | 列出本机会话 | 查询当前Agent可访问的会话 | 桌面 / Android / iOS | 固定功能/详参数 | list | 列出本地Agent会话。 |  |
| 141 | session_rename | 重命名会话 | 为已有会话设置名称 | 桌面 / Android / iOS | 固定功能/详参数 | rename | 把这个Agent会话重命名。 |  |
| 142 | session_send_input | 发送会话输入 | 向指定会话提交输入并保留接收语义 | 桌面 / Android / iOS | 固定功能/详参数 | send | 把这段任务发送给指定Agent会话。 |  |
| 143 | session_set_workspace | 更改会话工作目录 | 修改已有会话的工作目录 | 桌面 | 固定功能/详参数 | — | 将这个会话的工作目录改到指定目录。 |  |
| 144 | shell | 执行终端命令 | 在工作目录执行终端命令并返回输出 | 桌面 | 固定功能/详参数 | — | 在当前工作目录执行这条命令。 |  |
| 145 | spawn_agent | 创建子Agent | 把一项独立子任务交给子Agent并行处理 | 桌面 | 固定功能/详参数 | — | 创建一个子Agent检查这部分资料。 |  |
| 146 | ssh_exec | 远端SSH命令 | 经宿主密码/主机信任确认执行SSH命令 | 桌面 / Android / iOS | 固定功能/详参数 | exec | 在我指定并已配置的SSH主机执行这条命令。 |  |
| 147 | system_resources | 查看系统资源 | 返回本机CPU/内存及可查询GPU状态 | 桌面 / Android / iOS | 固定功能/详参数 | inspect | 查看当前内存、CPU等资源信息。 |  |
| 148 | todowrite | 维护本轮任务清单 | 更新任务步骤及状态 | 桌面 / Android / iOS | 固定功能/详参数 | pending / in_progress / completed | 把这项工作拆成待办并标记当前进度。 |  |
| 149 | tool_output_read | 补读工具输出 | 按已保存输出句柄有界读取完整原始结果 | 桌面 / Android / iOS | 固定功能/详参数 | read / 分页 | 继续读取刚才过长的工具输出。 |  |
| 150 | video_stretch_lower | 视频下部比例延展 | 按专用几何流程生成新视频并保留源 | 桌面 / Android / iOS | 固定功能/详参数 | stretch | 调整视频下部画面的拉伸比例并另存。 |  |
| 151 | view_image | 向主模型提供图像观察 | 准备已有图片的有界预览供支持图像结果的主模型分析 | 桌面 / Android / iOS | 固定功能/详参数 | view | 用支持图像输入的当前模型分析这张工作区图片。 |  |
| 152 | wait_agent | 等待子Agent | 等待指定子Agent完成或返回当前进度 | 桌面 | 固定功能/详参数 | — | 等待刚才的子Agent返回结果。 |  |
| 153 | web_fetch | 供应商网页读取 | 按当前主模型供应商读取网页参考信息 | 桌面 / Android / iOS | 固定功能/详参数 | fetch | 用当前云模型供应商检索这个网页正文。 |  |
| 154 | web_search | 供应商网页搜索 | 按当前主模型供应商调用其原生搜索 | 桌面 / Android / iOS | 固定功能/详参数 | search | 用当前云模型供应商搜索这个主题。 |  |
| 155 | zlib_compress | 压缩文件 | 生成新的gzip/zlib/deflate压缩文件 | 桌面 / Android / iOS | 固定功能/详参数 | gzip / zlib / deflate | 把这份工作区文件压缩成gzip。 |  |
| 156 | zlib_decompress | 解压文件 | 生成新的解压文件且保留输入 | 桌面 / Android / iOS | 固定功能/详参数 | auto / gzip / zlib / deflate | 把这个gzip文件解压到指定工作区文件。 |  |

## 内部/不对主模型公开的实现

此表不放进普通欢迎页；按所核Mobile源码实例单列，并非从项目里删除实现。

| ID | 中文名 | 原因 |
|---|---|---|
| seedance_video | Seedance视频任务 | 仅保留内部后台轮询/旧任务恢复，不在public specs |
| ark_assets | Ark素材管理 | 内部素材准备/旧任务关联，不在public specs |
| seedream_image | Seedream图像生成 | 内部实例，不在public specs |
| glm_image | GLM图像生成 | 付费图像入口暂停，内部恢复实例，不在public specs |
| minimax_image | MiniMax图像生成 | 付费图像入口暂停，内部保留，不在public specs |
| wan_video | Wan视频生成 | 该付费视频入口暂停，内部查询/恢复既有任务，不在public specs |
| wan_image | Wan图像生成 | 内部实例，不在public specs |
| kling_bailian_image | 百炼Kling图像生成 | 内部实例，不在public specs |

## 存在实现、但未挂入当前标准产品public注册的项

| ID | 归因 |
|---|---|
| curl_download | 下载普通工厂可供媒体模块复用；当前curl注册器只公开统一curl入口 |
| curl_fetch | 抓网页取文本的独立实现；类存在不等于当前单独注册 |
| curl_request | HTTP请求独立实现；当前标准注册器未将其另公开 |
| curl_upload | 上传独立实现；可复用工厂不等于公开工具 |
| list_windows | 普通窗口枚举工厂存在；builtin未公开注册，desktop_computer的list提供相应能力 |

来源：MaiAgent/AgentTools/MaiCurlTools.cpp:9–10；MaiTool.cpp:221–255；MaiWindowListTool.cpp。宿主凭据回调、帧release、测试helper、ABI configure/local_model_probe等不冒充正式工具。local_model_probe会实际运行模型，本次未调用。

## 条件和证据

- Windows纯getter快照参数为unfiltered/deepseek-flash/deepseek-v4-pro/glm-5.3/glm-5.3-flash，数量117/117/117/115/117。glm-5.3缺view_image+screenshot；不是用户live模型快照。
- getter故意用内存会话、无账号目录；DesktopPortrait四项和RVM资源条件由源补充：mobile_beautify_image、mobile_reshape_body、mobile_portrait_feedback、model_continue_training、cv_video_matting。它们已在Mobile并集中，所以不重复增加ID。openssl与crypto fallback还受编译分支影响。
- Mobile源行137项均有source证据、ID唯一；8 internal另列。缺无副作用当前会话spec桥，所以运行时已启用数为unknown，不以configure/probe代替。
- 独立102个直接name/helper注册候选与public并集及internal交叉后，5个未挂载项已在附表解释。变量name工厂由注册器和getter补证，不用字符串grep自称全。
- 桌面/移动maichat_video_command动作有差异：Desktop的status、previous_item、next_item等不能擅自算作Mobile也支持；原始schema分别保留。权限和新输入依赖未据此宣称全部实机验收。
- 当前欢迎页尚未按全部独立条目展示。目录归整任务仍只方案，此审阅表也不代表已重组UI。

Mobile来源MD58093B/SHA256 9b201872a0da54bd38ad88896ddcb386d37f306855aaa181cf09df9185c71348；JSON323249B/SHA256 edc66f8b9b824e38e3fa47b5d7d8d6919b9580ad7a592d6f925242fe6c2740e1。Desktop getter raw1997257B/SHA256 c901cd35d1c792ccf1c9a5b122a57581d19b7bf77668e4ce3fc2a49d5854649a。源码基线与平台条件详原报告；后续source-condition peer差异会同步补充。
