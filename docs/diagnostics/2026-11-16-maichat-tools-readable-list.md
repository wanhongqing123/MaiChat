# MaiChat 工具清单 · 易读版

共156个公开工具候选，每项独立列出，暂不定分组。平台按源码注册及条件标注，不代表当前全部已启用。

视频播放是 **maichat_play_video**，播放控制是 **maichat_video_command**，两项均在下方独立列出。

### 001　在创作对话中交付文件

工具：`agent_send_file`

用途：把工作区里已有的文件作为文件卡片放到当前创作中心回复中，供查看或保存。

平台：桌面、Android、iOS。

示例：把做好的报告PDF放到这段创作对话里。

---

### 002　交付图片或视频

工具：`agent_send_media`

用途：把已有工作区媒体显示为当前AI聊天卡片。

平台：桌面、Android、iOS。

示例：把工作区这张处理后的图片展示在当前聊天里。

---

### 003　扫描与预览清理存储

工具：`app_storage`

用途：扫描App受管目录并先预览后按凭据清理。

平台：桌面、Android（有条件）、iOS（有条件）。

示例：先预览App临时文件占用和可清理项。

---

### 004　关闭子Agent

工具：`close_agent`

用途：关闭不再需要的子Agent并释放并行名额。

平台：桌面。

示例：关闭已经完成的子Agent。

---

### 005　读取证书信息

工具：`crypto_certificate_info`

用途：读取工作区证书的主体、签发者、有效期和指纹。

平台：桌面。

示例：查看工作区证书的有效期。

---

### 006　计算文件或文字摘要

工具：`crypto_digest`

用途：计算SHA系列等校验摘要。

平台：桌面。

示例：计算这个文件的SHA256。

---

### 007　HTTP与文件传输

工具：`curl`

用途：运行受限libcurl命令进行网络请求或文件传输。

平台：桌面、Android、iOS。

示例：读取这个公开网页的正文。

---

### 008　查询当前时间

工具：`current_time`

用途：返回当前时间供时间相关任务使用。

平台：桌面、Android、iOS。

示例：现在当地时间是几点？

---

### 009　检测图像边缘

工具：`cv_detect_edges`

用途：分析或输出图像边缘。

平台：桌面、Android、iOS。

示例：提取这张图的边缘，另存一张结果图。

---

### 010　检测图像直线

工具：`cv_detect_lines`

用途：返回图像线段/直线信息。

平台：桌面、Android、iOS。

示例：找出照片里的直线。

---

### 011　检测文档四角

工具：`cv_document_corners`

用途：定位图像中的文档角点。

平台：桌面、Android、iOS。

示例：找出这张纸的四个角。

---

### 012　查找图像轮廓

工具：`cv_find_contours`

用途：提取现有图像轮廓信息。

平台：桌面、Android、iOS。

示例：提取图像里的封闭轮廓。

---

### 013　比较图像指标

工具：`cv_image_compare`

用途：比较两幅图像的数值差异。

平台：桌面、Android、iOS。

示例：比较两张图是否有明显变化。

---

### 014　图像质量指标

工具：`cv_image_quality`

用途：分析现有图像质量数值。

平台：桌面、Android、iOS。

示例：检查这张图片的清晰度和亮度。

---

### 015　图像模板匹配

工具：`cv_match_template`

用途：在图像中查找指定模板。

平台：桌面、Android、iOS。

示例：在大图里找出这个小图的位置。

---

### 016　检测视频运动时段

工具：`cv_motion_detect`

用途：用帧差分析可见运动并返回时间段。

平台：桌面、Android、iOS。

示例：找出视频中运动明显的时间段。

---

### 017　图像平移配准

工具：`cv_register_translation`

用途：估计图像之间的平移。

平台：桌面、Android、iOS。

示例：估算两张照片的平移偏差。

---

### 018　检测视频镜头变化

工具：`cv_scene_detect`

用途：用OpenCV返回视觉场景变化时间段而非语义理解。

平台：桌面、Android、iOS。

示例：列出这段视频的镜头切换时间。

---

### 019　创建阈值遮罩

工具：`cv_threshold_mask`

用途：按阈值生成局部处理遮罩。

平台：桌面、Android、iOS。

示例：根据亮度阈值生成一张蒙版。

---

### 020　本地视频抠像

工具：`cv_video_matting`

用途：通过RVM与ORT生成新抠像视频。

平台：桌面（有条件）、Android（有条件）、iOS（有条件）。

示例：把视频中的人物抠出并生成新文件。

---

### 021　操作桌面浏览器

工具：`desktop_browser`

用途：打开隔离的Chrome/Edge页面并查看、截图、点击或输入。

平台：桌面。

示例：打开指定网站并查看页面。

---

### 022　操作电脑

工具：`desktop_computer`

用途：读取屏幕/窗口画面并执行原生输入；具体动作受平台与依赖条件限制。

平台：桌面。

示例：查看这个窗口，并按我的要求操作。

---

### 023　读取后台进程输出

工具：`desktop_process_read`

用途：读取本会话启动的后台进程的新输出。

平台：桌面。

示例：读取刚启动进程的新输出。

---

### 024　启动后台进程

工具：`desktop_process_start`

用途：在本会话工作目录启动可持续运行的命令。

平台：桌面。

示例：在当前工作目录启动这个长时间任务。

---

### 025　停止后台进程

工具：`desktop_process_stop`

用途：停止本会话拥有的后台进程。

平台：桌面。

示例：停止刚才启动的进程。

---

### 026　向后台进程输入

工具：`desktop_process_write`

用途：向本会话运行中进程的标准输入发送文字。

平台：桌面。

示例：向刚才启动的交互进程输入这段文字。

---

### 027　检测人脸

工具：`detect_faces`

用途：检测图像中的人脸位置和关键点。

平台：桌面。

示例：检测这张图片中的人脸位置。

---

### 028　本地媒体转换

工具：`ffmpeg`

用途：对已授权媒体执行嵌入式FFmpeg处理并输出新文件。

平台：桌面、Android、iOS。

示例：把这个工作区视频转成MP4。

---

### 029　本地媒体信息

工具：`ffprobe`

用途：查询已有媒体流/时长/编码信息。

平台：桌面、Android、iOS。

示例：查看这段视频的编码、时长和分辨率。

---

### 030　新建文件

工具：`file_create`

用途：在授权路径创建新文件。

平台：桌面、Android、iOS。

示例：在工作区新建一份待办文本。

---

### 031　新建目录

工具：`file_create_directory`

用途：创建授权工作区目录。

平台：桌面、Android、iOS。

示例：在工作区建一个素材目录。

---

### 032　删除文件

工具：`file_delete`

用途：按权限删除指定文件而非目录整树。

平台：桌面、Android、iOS。

示例：删除我指定的这份临时文件。

---

### 033　定点编辑文件

工具：`file_edit`

用途：用明确旧新文本修改现有文件。

平台：桌面、Android、iOS。

示例：把文档中的这一处日期替换掉。

---

### 034　查找文件名

工具：`file_glob`

用途：按路径和模式分页查找文件。

平台：桌面、Android、iOS。

示例：列出工作区里的所有PDF文件。

---

### 035　检索文件内容

工具：`file_grep`

用途：有界文字或正则检索并返回行/文件/计数。

平台：桌面、Android、iOS。

示例：查找哪些文档提到了这个词。

---

### 036　应用文件补丁

工具：`file_patch`

用途：以补丁创建或修改指定文件。

平台：桌面、Android、iOS。

示例：把这份明确的补丁应用到工作区文件。

---

### 037　读取文件

工具：`file_read`

用途：读取工作区文本并支持有界分页。

平台：桌面、Android、iOS。

示例：读一下这份说明的前100行。

---

### 038　写入文件

工具：`file_write`

用途：写入指定文件内容。

平台：桌面、Android、iOS。

示例：把整理后的内容写入这份工作区文本。

---

### 039　生成PDF文档

工具：`generate_pdf`

用途：由宿主渲染HTML并可使用私有密码表单保护输出PDF。

平台：桌面、Android、iOS。

示例：把这段排版好的内容生成PDF。

---

### 040　百炼可灵视频任务

工具：`kling_bailian_video`

用途：通过百炼Kling创建或参考/编辑视频，查询既有任务、取消排队任务或删除本地交付记录。

平台：桌面、Android、iOS。

示例：先列出可选Kling视频型号及限制，确认后再生成。

---

### 041　查看子Agent

工具：`list_agents`

用途：查看已创建子Agent的运行状态。

平台：桌面。

示例：列出我这次任务的子Agent状态。

---

### 042　本地模型推理

工具：`llama`

用途：使用驻留本地推理引擎与已授权模型文件。

平台：桌面、Android、iOS。

示例：用已有本地GGUF模型生成一段短文。

---

### 043　群发指定联系人文字

工具：`maichat_broadcast_text`

用途：仅向指定联系人集合发送文字。

平台：桌面、Android、iOS。

示例：把这段通知发送给我指定的这些联系人。

---

### 044　新建系统日历事件

工具：`maichat_calendar_create_event`

用途：通过授权日历能力创建指定时间事件。

平台：桌面、Android、iOS。

示例：为明天下午的会议创建系统日历事件。

---

### 045　控制自动相机采样

工具：`maichat_camera_command`

用途：iOS仅按用户明确要求每两秒保存图像并查询/停止。

平台：桌面、iOS。

示例：打开本轮会话的相机预览，等我拍摄。

---

### 046　打开安全修改密码流程

工具：`maichat_change_password`

用途：用系统安全表单修改密码且模型不接触密码。

平台：桌面、Android、iOS。

示例：打开安全修改密码表单，由我在设备上填写。

---

### 047　中止云盘上传

工具：`maichat_cloud_drive_abort_upload`

用途：明确中止上传并按契约回收分片。

平台：桌面、Android、iOS。

示例：终止这一项云盘上传。

---

### 048　新建云盘文件夹

工具：`maichat_cloud_drive_create_folder`

用途：创建指定父目录下的新文件夹。

平台：桌面、Android、iOS。

示例：在云盘创建一个资料文件夹。

---

### 049　删除空云盘文件夹

工具：`maichat_cloud_drive_delete_folder`

用途：只删除指定空文件夹而非递归内容。

平台：桌面、Android、iOS。

示例：删除这个空的云盘文件夹。

---

### 050　删除个人云盘文件

工具：`maichat_cloud_drive_delete_media`

用途：删除用户明确指定的个人云盘对象。

平台：桌面、Android、iOS。

示例：删除我指定的这一项云盘文件。

---

### 051　下载云盘文件到工作区

工具：`maichat_cloud_drive_download_media`

用途：把选定私人文件下载到本机工作区。

平台：桌面、Android、iOS。

示例：把这个云盘文件下载到当前工作区。

---

### 052　列出云盘文件夹

工具：`maichat_cloud_drive_list_folders`

用途：查看私人云盘目录元数据。

平台：桌面、Android、iOS。

示例：列出当前云盘文件夹。

---

### 053　列出个人云盘文件

工具：`maichat_cloud_drive_list_media`

用途：查询当前账号个人云盘已有文件。

平台：桌面、Android、iOS。

示例：看看我的云盘有哪些文件。

---

### 054　列出云盘上传断点

工具：`maichat_cloud_drive_list_pending_uploads`

用途：查看可恢复的本机上传任务。

平台：桌面、Android、iOS。

示例：列出尚未完成的上传任务。

---

### 055　移动云盘文件

工具：`maichat_cloud_drive_move_media`

用途：改变已有文件所属文件夹。

平台：桌面、Android、iOS。

示例：把这个云盘文件移到指定文件夹。

---

### 056　打开云盘媒体

工具：`maichat_cloud_drive_open_media`

用途：在App中打开已有云盘媒体。

平台：桌面、Android、iOS。

示例：打开这项云盘素材供当前会话使用。

---

### 057　暂停云盘上传

工具：`maichat_cloud_drive_pause_upload`

用途：暂停指定上传并保留恢复状态。

平台：桌面、Android、iOS。

示例：暂停这一项上传。

---

### 058　重命名云盘文件

工具：`maichat_cloud_drive_rename_media`

用途：更新显示名称而不改远端对象身份。

平台：桌面、Android、iOS。

示例：把这个云盘文件改成指定名称。

---

### 059　继续云盘上传

工具：`maichat_cloud_drive_resume_upload`

用途：继续指定未完成上传。

平台：桌面、Android、iOS。

示例：继续刚才暂停的上传。

---

### 060　搜索个人云盘文件

工具：`maichat_cloud_drive_search_media`

用途：按条件搜索当前账号云盘媒体。

平台：桌面、Android、iOS。

示例：在云盘中查找名称包含合同的文件。

---

### 061　修改云盘文件夹

工具：`maichat_cloud_drive_update_folder`

用途：重命名或移动文件夹元数据。

平台：桌面、Android、iOS。

示例：给这个云盘文件夹改名。

---

### 062　上传到个人云盘

工具：`maichat_cloud_drive_upload_media`

用途：把用户选定的受支持文件上传到私人空间。

平台：桌面、Android、iOS。

示例：把工作区这份文件上传到云盘。

---

### 063　创建MaiChat群聊

工具：`maichat_create_group`

用途：按用户要求创建指定成员的群聊。

平台：桌面、Android、iOS。

示例：用这些MaiChat联系人创建一个群。

---

### 064　生成二维码图片

工具：`maichat_generate_qr_code`

用途：将文字或URL生成可扫描二维码PNG。

平台：桌面、Android、iOS。

示例：把这个链接做成二维码图片。

---

### 065　查看当前账号资料

工具：`maichat_get_current_account`

用途：读取当前登录资料快照且不返回令牌/密码。

平台：桌面、Android、iOS。

示例：查看我当前登录的MaiChat账号资料。

---

### 066　读取群聊历史

工具：`maichat_get_group_messages`

用途：读取指定群聊的已有消息。

平台：桌面、Android、iOS。

示例：查看这个MaiChat群最近的消息。

---

### 067　读取私聊消息

工具：`maichat_get_messages`

用途：按所属联系人/会话读取已有消息。

平台：桌面、Android、iOS。

示例：读取这个MaiChat会话最近的消息。

---

### 068　查看未读概况

工具：`maichat_get_unread_summary`

用途：按联系人汇总未读消息。

平台：桌面、Android、iOS。

示例：汇总MaiChat当前未读消息数量。

---

### 069　查看MaiChat联系人

工具：`maichat_list_contacts`

用途：读取当前IM联系人列表而非手机通讯录。

平台：桌面、Android、iOS。

示例：查找我的MaiChat联系人。

---

### 070　列出IM会话

工具：`maichat_list_conversations`

用途：查询本机可访问的IM会话列表。

平台：桌面、Android、iOS。

示例：列出最近的MaiChat聊天会话。

---

### 071　查看MaiChat群聊

工具：`maichat_list_groups`

用途：查询当前账号群聊。

平台：桌面、Android、iOS。

示例：列出我加入的MaiChat群。

---

### 072　查看安全举报记录

工具：`maichat_list_safety_reports`

用途：查询当前账号的举报记录。

平台：Android。

示例：查看我提交的安全举报记录。

---

### 073　播放桌面本地音乐

工具：`maichat_play_local_music`

用途：在MaiChat播放器打开工作区音乐，或由用户选择文件。

平台：桌面。

示例：播放工作区里的音乐文件。

---

### 074　打开工作区视频播放

工具：`maichat_play_video`

用途：在同一MaiChat播放器打开已有视频并可指定起播秒数。

平台：桌面、Android、iOS。

示例：播放工作区的视频，从第8秒开始。

---

### 075　创建远端会话

工具：`maichat_remote_session_create`

用途：向所属设备请求创建Session。

平台：桌面、Android、iOS。

示例：在指定在线设备创建一个Agent会话。

---

### 076　删除远端会话

工具：`maichat_remote_session_delete`

用途：向所属设备请求删除指定Session。

平台：桌面、Android、iOS。

示例：删除指定远端设备上的这个Agent会话。

---

### 077　列出跨设备会话

工具：`maichat_remote_session_list`

用途：查看当前账号设备和远端Session目录。

平台：桌面、Android、iOS。

示例：看看我的其他设备有哪些Agent会话。

---

### 078　修改远端工作目录

工具：`maichat_remote_session_set_workspace`

用途：请求所属设备切换指定Session工作目录。

平台：桌面、Android、iOS。

示例：为指定远端Agent会话设置工作目录。

---

### 079　回复MaiChat消息

工具：`maichat_reply_message`

用途：以明确的引用关系回复已有消息。

平台：桌面、Android、iOS。

示例：在MaiChat中回复这条指定消息。

---

### 080　申请注销账号

工具：`maichat_request_account_deletion`

用途：经安全身份与确认流程申请账号注销。

平台：Android。

示例：为我的当前账号提交注销申请。

---

### 081　申请添加好友

工具：`maichat_request_friend`

用途：向明确联系人发送好友申请。

平台：桌面、Android、iOS。

示例：向这个MaiChat用户发送好友申请。

---

### 082　检索本机聊天记录

工具：`maichat_search_messages`

用途：在授权本机历史中按条件检索消息。

平台：桌面、Android、iOS。

示例：在指定MaiChat会话中搜索这段关键词。

---

### 083　发送MaiChat群聊文字

工具：`maichat_send_group_text`

用途：向明确群聊发送文字。

平台：桌面、Android、iOS。

示例：把这段文字发到指定MaiChat群。

---

### 084　发送MaiChat媒体

工具：`maichat_send_media`

用途：向明确联系人发送现有图片/视频等受支持媒体。

平台：桌面、Android、iOS。

示例：把这个工作区媒体发给指定MaiChat会话。

---

### 085　发送MaiChat私聊文字

工具：`maichat_send_text`

用途：向明确联系人发送文字。

平台：桌面、Android、iOS。

示例：给这个MaiChat联系人发一段文字。

---

### 086　打开所属Session附件

工具：`maichat_session_attachment_open`

用途：核对房间归属后下载并导入本地Session工作区。

平台：桌面、Android、iOS。

示例：把这份会话附件导入当前工作区。

---

### 087　绑定本机Session与好友会话

工具：`maichat_session_im_binding`

用途：只为新的Human消息建立/解除当前账号本地绑定。

平台：桌面、Android、iOS。

示例：查看这个Agent会话的IM绑定关系。

---

### 088　取消会话定时任务

工具：`maichat_session_task_cancel`

用途：取消指定会话任务。

平台：桌面、Android、iOS。

示例：取消这条会话定时任务。

---

### 089　创建会话定时任务

工具：`maichat_session_task_create`

用途：为指定Session建立本机定时任务。

平台：桌面、Android、iOS。

示例：明天上午在这个会话提醒我检查报告。

---

### 090　查看会话定时任务

工具：`maichat_session_task_list`

用途：列出当前范围的定时任务。

平台：桌面、Android、iOS。

示例：列出本会话的定时任务。

---

### 091　修改会话定时任务

工具：`maichat_session_task_update`

用途：更新已有任务的时间/启用等配置。

平台：桌面、Android、iOS。

示例：把这条会话任务改到明天下午。

---

### 092　更换当前账号头像

工具：`maichat_set_avatar`

用途：从现有图片准备新头像并等待资料确认。

平台：桌面、Android、iOS。

示例：打开头像选择流程，让我确认新的头像。

---

### 093　更改当前账号昵称

工具：`maichat_set_nickname`

用途：按用户要求更新账号昵称。

平台：桌面、Android、iOS。

示例：把我的MaiChat昵称改成这个名称。

---

### 094　控制当前视频播放

工具：`maichat_video_command`

用途：控制已打开的播放器而非另存转码文件。

平台：桌面、Android、iOS。

示例：把当前播放的视频暂停并跳到第8秒。

---

### 095　本地视频水印修复

工具：`media_remove_watermark`

用途：使用LaMa与邻近干净帧修复视频水印区域。

平台：Android（有条件）、iOS（有条件）。

示例：修复我指定视频中这一块水印区域，另存结果。

---

### 096　MiniMax视频任务

工具：`minimax_video`

用途：通过MiniMax生成视频、校验方案及查询既有任务；delete仅删已交付本地记录。

平台：桌面、Android、iOS。

示例：先核对MiniMax视频生成方案和费用条件，等我确认。

---

### 097　本地人像美颜

工具：`mobile_beautify_image`

用途：基于可靠人物/皮肤范围创建新美颜图像。

平台：桌面（有条件）、Android、iOS。

示例：对这张照片轻度磨皮、美白，另存新图。

---

### 098　观察和操作内置浏览器

工具：`mobile_browser`

用途：观察和操作当前Agent会话拥有的MaiChat内置网页；不控制外部浏览器。

平台：Android、iOS。

示例：打开这个网页，先观察页面再填写指定的普通字段。

---

### 099　查看已保存相机图像

工具：`mobile_camera_frames`

用途：iOS列出或打开自动相机拍摄批次里用户选择的图像。

平台：iOS。

示例：列出本轮相机拍到的帧，让我选一张查看。

---

### 100　读取屏幕图像

工具：`mobile_capture_screen`

用途：Android读已授权整屏帧；iOS可读当前App或最近外部帧。

平台：Android、iOS（有条件）。

示例：读取本次已授权屏幕画面。

---

### 101　打开系统短信编辑器

工具：`mobile_compose_sms`

用途：iOS预填收件人与正文供用户自己点击发送。

平台：iOS。

示例：打开系统短信编辑器，内容由我检查后发送。

---

### 102　检测人脸与关键点

工具：`mobile_detect_faces`

用途：端侧返回人脸边框/关键点而不识别人身份。

平台：Android、iOS。

示例：找出这张照片里的人脸位置。

---

### 103　拨打明确电话号码

工具：`mobile_dial_phone`

用途：Android直接请求拨号/呼叫权限；iOS打开系统电话确认。

平台：Android、iOS（有条件）。

示例：拨打我选定的这个电话号码。

---

### 104　导出当前App诊断

工具：`mobile_export_diagnostics`

用途：由宿主把受限诊断信息导出到受管文件。

平台：Android、iOS。

示例：导出本机MaiChat诊断文件供我查看。

---

### 105　导出相册原媒体

工具：`mobile_export_media_original`

用途：按资产ID导出原始照片或视频到工作区。

平台：Android、iOS。

示例：把我选中的相册视频原文件导入工作区。

---

### 106　导出相册原文件

工具：`mobile_export_photo_original`

用途：按相册资产ID导出原图或配对视频并保留格式。

平台：Android、iOS。

示例：导出这张已授权照片的原文件再做编辑。

---

### 107　读取通讯录联系人

工具：`mobile_get_contact`

用途：获取指定系统联系人信息。

平台：Android、iOS。

示例：查看我选中的系统联系人的电话号码。

---

### 108　获取当前前台位置

工具：`mobile_get_location`

用途：按系统授权读取一次前台位置。

平台：Android、iOS。

示例：取得我当前的一次前台位置。

---

### 109　读取本机运营商号码

工具：`mobile_get_phone_number`

用途：Android读取默认订阅号码且不视作验证身份。

平台：Android。

示例：读取当前默认SIM卡提供的本机号码。

---

### 110　列出系统相册

工具：`mobile_list_albums`

用途：查询当前权限允许的相册列表。

平台：Android、iOS。

示例：列出我允许MaiChat访问的系统相册。

---

### 111　列出手机通讯录

工具：`mobile_list_contacts`

用途：读取系统授权的通讯录联系人。

平台：Android、iOS。

示例：在手机系统通讯录查找这个姓名。

---

### 112　列出授权相册素材

工具：`mobile_list_photos`

用途：分页列出当前系统照片授权可见的照片/视频。

平台：Android、iOS。

示例：列出我授权相册最近的照片和视频。

---

### 113　把照片加入iOS相册

工具：`mobile_photos_add_to_album`

用途：创建或复用Photos相册并加入已有ID而不复制/删除原件。

平台：iOS。

示例：把这些已有系统照片加入指定相册。

---

### 114　复制照片到Android相册目录

工具：`mobile_photos_copy_to_album`

用途：复制所选现有素材到Pictures/MaiChat相册目录并保留原件。

平台：Android。

示例：把这些照片复制到指定系统相册。

---

### 115　播放本地音乐

工具：`mobile_play_local_music`

用途：播放工作区音乐或让用户选择系统音乐/文件。

平台：Android、iOS。

示例：播放工作区这首音乐，或让我选择一首。

---

### 116　记录明确人像偏好

工具：`mobile_portrait_feedback`

用途：保存用户对同一原图两结果的实际评价。

平台：桌面（有条件）、Android、iOS。

示例：我更喜欢这次磨皮结果，请只记录这个明确偏好。

---

### 117　全屏查看已有图片

工具：`mobile_preview_image`

用途：仅在用户明确要求全屏/放大时打开查看器。

平台：Android、iOS。

示例：把这张工作区图片全屏放大给我看。

---

### 118　读取相册预览图

工具：`mobile_read_photo`

用途：把指定素材的有界JPEG预览复制到工作区。

平台：Android、iOS。

示例：读取我选中的这张相册照片预览。

---

### 119　读取用户授权短信内容

工具：`mobile_read_sms`

用途：Android读受限收件箱；iOS仅读用户快捷指令导入内容。

平台：Android、iOS。

示例：读取我明确授权或导入的最近几条短信。

---

### 120　申请所需系统权限

工具：`mobile_request_permission`

用途：只为当前任务请求相关系统能力并报告授权范围。

平台：Android、iOS。

示例：申请本次读取相册所需的照片权限。

---

### 121　本地身体比例塑形

工具：`mobile_reshape_body`

用途：用姿态与分割做有界腰部/腿部轻度几何调整。

平台：桌面（有条件）、Android、iOS。

示例：对这张站姿照片轻微收腰，另存新图。

---

### 122　保存新图片到相册

工具：`mobile_save_image`

用途：把现有工作区图片新增到系统图库而非覆盖原图。

平台：Android、iOS。

示例：把这张处理后的图片另存到系统相册。

---

### 123　保存新视频到相册

工具：`mobile_save_video`

用途：把受支持工作区视频新增到系统图库。

平台：Android、iOS。

示例：把工作区这个视频保存为相册新项目。

---

### 124　查看已保存屏幕截图

工具：`mobile_screen_frames`

用途：iOS列出/打开用户选定的共享截图或清理停止的批次。

平台：iOS（有条件）。

示例：列出本轮已停止共享后保存的屏幕帧。

---

### 125　生成人物分割遮罩

工具：`mobile_segment_person`

用途：端侧生成白人物黑背景的新灰度PNG。

平台：Android、iOS。

示例：为这张图片生成独立的人物蒙版。

---

### 126　开始系统屏幕共享

工具：`mobile_start_screen_share`

用途：由用户通过系统选择器启动整屏共享。

平台：Android、iOS（有条件）。

示例：开始本轮由我授权的整屏共享。

---

### 127　停止系统屏幕共享

工具：`mobile_stop_screen_share`

用途：结束当前共享且iOS保留已保存截图。

平台：Android、iOS（有条件）。

示例：停止本轮整屏共享并保存当前批次。

---

### 128　打开相机拍照

工具：`mobile_take_photo`

用途：等待用户拍照并保存新工作区JPEG。

平台：Android、iOS。

示例：打开相机，等我拍一张照片。

---

### 129　本地图像变换

工具：`mobile_transform_image`

用途：生成新文件进行一种端侧图像编辑。

平台：Android、iOS。

示例：把这张图片顺时针旋转90度，保留原图。

---

### 130　训练本地偏好模型

工具：`model_continue_training`

用途：训练/检查/回滚用户的小型人像偏好排名权重。

平台：桌面（有条件）、Android、iOS。

示例：用我已明确反馈的偏好训练并报告是否验证生效。

---

### 131　查看网络地址

工具：`network_ip`

用途：查询网络IP信息。

平台：桌面、Android、iOS。

示例：查看设备可见的网络IP信息。

---

### 132　加密与证书命令

工具：`openssl`

用途：调用受限OpenSSL命令入口。

平台：桌面（有条件）、Android、iOS。

示例：查看这份本地证书的有效期。

---

### 133　向用户确认信息

工具：`question`

用途：向用户提出聚焦问题并等待真实回答。

平台：桌面、Android、iOS。

示例：请我确认海报用竖版还是方版，再继续制作。

---

### 134　截取桌面屏幕

工具：`screenshot`

用途：截取鼠标所在显示器的完整画面；不是窗口裁剪或浏览器视口。

平台：桌面。

示例：截取当前显示器的画面。

---

### 135　分割图中人像

工具：`segment_person`

用途：生成新的人像灰度蒙版，保留原图。

平台：桌面。

示例：为这张照片生成人像蒙版。

---

### 136　向子Agent补充指令

工具：`send_input`

用途：向已创建子Agent补充信息或调整任务。

平台：桌面。

示例：给这个子Agent补充以下要求。

---

### 137　创建会话

工具：`session_create`

用途：在移动默认工作目录创建新会话。

平台：桌面、Android、iOS。

示例：创建一个新的本地Agent会话。

---

### 138　删除会话

工具：`session_delete`

用途：删除指定本地会话。

平台：桌面、Android、iOS。

示例：删除我指定的这个本地Agent会话。

---

### 139　读取会话历史

工具：`session_history`

用途：分页读取指定会话历史。

平台：桌面、Android、iOS。

示例：查看这个Agent会话的已有历史。

---

### 140　列出本机会话

工具：`session_list`

用途：查询当前Agent可访问的会话。

平台：桌面、Android、iOS。

示例：列出本地Agent会话。

---

### 141　重命名会话

工具：`session_rename`

用途：为已有会话设置名称。

平台：桌面、Android、iOS。

示例：把这个Agent会话重命名。

---

### 142　发送会话输入

工具：`session_send_input`

用途：向指定会话提交输入并保留接收语义。

平台：桌面、Android、iOS。

示例：把这段任务发送给指定Agent会话。

---

### 143　更改会话工作目录

工具：`session_set_workspace`

用途：修改已有会话的工作目录。

平台：桌面。

示例：将这个会话的工作目录改到指定目录。

---

### 144　执行终端命令

工具：`shell`

用途：在工作目录执行终端命令并返回输出。

平台：桌面。

示例：在当前工作目录执行这条命令。

---

### 145　创建子Agent

工具：`spawn_agent`

用途：把一项独立子任务交给子Agent并行处理。

平台：桌面。

示例：创建一个子Agent检查这部分资料。

---

### 146　远端SSH命令

工具：`ssh_exec`

用途：经宿主密码/主机信任确认执行SSH命令。

平台：桌面、Android、iOS。

示例：在我指定并已配置的SSH主机执行这条命令。

---

### 147　查看系统资源

工具：`system_resources`

用途：返回本机CPU/内存及可查询GPU状态。

平台：桌面、Android、iOS。

示例：查看当前内存、CPU等资源信息。

---

### 148　维护本轮任务清单

工具：`todowrite`

用途：更新任务步骤及状态。

平台：桌面、Android、iOS。

示例：把这项工作拆成待办并标记当前进度。

---

### 149　补读工具输出

工具：`tool_output_read`

用途：按已保存输出句柄有界读取完整原始结果。

平台：桌面、Android、iOS。

示例：继续读取刚才过长的工具输出。

---

### 150　视频下部比例延展

工具：`video_stretch_lower`

用途：按专用几何流程生成新视频并保留源。

平台：桌面、Android、iOS。

示例：调整视频下部画面的拉伸比例并另存。

---

### 151　向主模型提供图像观察

工具：`view_image`

用途：准备已有图片的有界预览供支持图像结果的主模型分析。

平台：桌面、Android、iOS。

示例：用支持图像输入的当前模型分析这张工作区图片。

---

### 152　等待子Agent

工具：`wait_agent`

用途：等待指定子Agent完成或返回当前进度。

平台：桌面。

示例：等待刚才的子Agent返回结果。

---

### 153　供应商网页读取

工具：`web_fetch`

用途：按当前主模型供应商读取网页参考信息。

平台：桌面、Android、iOS。

示例：用当前云模型供应商检索这个网页正文。

---

### 154　供应商网页搜索

工具：`web_search`

用途：按当前主模型供应商调用其原生搜索。

平台：桌面、Android、iOS。

示例：用当前云模型供应商搜索这个主题。

---

### 155　压缩文件

工具：`zlib_compress`

用途：生成新的gzip/zlib/deflate压缩文件。

平台：桌面、Android、iOS。

示例：把这份工作区文件压缩成gzip。

---

### 156　解压文件

工具：`zlib_decompress`

用途：生成新的解压文件且保留输入。

平台：桌面、Android、iOS。

示例：把这个gzip文件解压到指定工作区文件。

---

## 内部或不对主模型公开的实现

以下单列备查，不混进上面的公开工具候选，也不当作已启用功能。

### Seedance视频任务

`seedance_video`

仅保留内部后台轮询/旧任务恢复，不在public specs

### Ark素材管理

`ark_assets`

内部素材准备/旧任务关联，不在public specs

### Seedream图像生成

`seedream_image`

内部实例，不在public specs

### GLM图像生成

`glm_image`

付费图像入口暂停，内部恢复实例，不在public specs

### MiniMax图像生成

`minimax_image`

付费图像入口暂停，内部保留，不在public specs

### Wan视频生成

`wan_video`

该付费视频入口暂停，内部查询/恢复既有任务，不在public specs

### Wan图像生成

`wan_image`

内部实例，不在public specs

### 百炼Kling图像生成

`kling_bailian_image`

内部实例，不在public specs

## 代码存在但未挂入当前公开注册表

- `curl_download`：独立下载工厂，当前注册器统一公开的是 `curl`。
- `curl_fetch`：网页取文本独立实现，当前没有另公开登记。
- `curl_request`：HTTP请求独立实现，当前没有另公开登记。
- `curl_upload`：上传独立实现，当前没有另公开登记。
- `list_windows`：普通窗口枚举工厂，当前没有作为独立模型工具登记；电脑操作的list可列窗口。

## 清单口径

- AI问答是基础聊天，不计工具。`question`是向用户提问并等待回答的实际工具。
- “有条件”包括编译、平台版本、资源、模型、权限与宿主配置。CSV技术版保留详细条件及各端操作差异。
- 桌面与移动端同名工具的动作可能不同，不能把一端的动作擅自当成全端支持。
- 当前欢迎界面还没有按这份完整清单逐项显示；这份先供你决定分组。

最终交叉核对：桌面117个getter条目+6个源码条件项，与移动端137个源码public候选合并，仍为156个唯一ID；缺项0、额外项0。内部8项和未挂载5项另列。这里不声称当前所有实例均已启用或所有能力均已实机验收。
