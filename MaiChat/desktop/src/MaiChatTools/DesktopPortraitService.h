// 文件作用：Qt 的普通照片服务；Agent 和非模型调用共用同一授权、编码与发布路径。
#pragma once

#include <functional>
#include <QJsonObject>
#include <QString>

struct DesktopPortraitRequest {
    QString operation;
    QJsonObject arguments;
    QString workspace;
    // 必须使用调用者已经配置的文件授权策略；返回空串即拒绝，不回退原路径。
    std::function<QString(const QString&)> resolvePath;
    std::function<bool()> isCanceled;
};

// 同步后台接口，拒绝 UI 线程。实例绑定当前账号私有 profileDirectory；
// 不能让模型选择账号、模型或 ORT 路径。像素始终复用 Portrait C++ 核心，
// Qt 只持有原图、输出 RGBA 和灰度 mask，不建立整图 float 中间副本。
// 成功返回 {ok:true,output:{path,mask_path,result_id,...}}，不打开预览或发送消息。
// 所有输出都是工作区内完整的新 PNG；失败或取消会清理本次临时文件和未登记文件。
class DesktopPortraitService {
public:
    DesktopPortraitService(QString profileDirectory, QString modelDirectory, QString runtimePath);
    ~DesktopPortraitService();
    DesktopPortraitService(const DesktopPortraitService&) = delete;
    DesktopPortraitService& operator=(const DesktopPortraitService&) = delete;
    QJsonObject execute(const DesktopPortraitRequest& request) const;

private:
    QString mProfileDirectory;
    QString mModelDirectory;
    QString mRuntimePath;
};
