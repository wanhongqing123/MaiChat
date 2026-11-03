#pragma once

#include "auth/DesktopVideoCircleAccess.h"

#include <QWidget>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>

class QCloseEvent;
class QLabel;
class QPushButton;
class QProgressBar;
class QScrollArea;
class QTemporaryDir;
class QVBoxLayout;
struct MaiCloudDriveCancel;
struct MaiDownloadCancel;
struct MaiCloudVod;
struct MaiCloudVodCancel;

// The desktop host owns only presentation and local paths. HTTP, multipart
// upload, checkpoint verification, and download live in shared C++ libraries.
class CloudDriveDialog final : public QWidget {
public:
    using AccessProvider = std::function<DesktopVideoCircleAccess()>;
    using AccountIdProvider = std::function<QString()>;
    using PlayCallback = std::function<void(std::shared_ptr<MaiCloudVod>, const QString&)>;
    using RelocateSelector = std::function<QString(const QString&)>;

    CloudDriveDialog(AccessProvider access, AccountIdProvider accountId,
                     PlayCallback play, QWidget* parent = nullptr,
                     RelocateSelector relocate = {}, bool embedded = false);
    ~CloudDriveDialog() override;

    // 1: account cloud API returned 200; 0: feature is disabled; -1: retryable.
    static int availability(const DesktopVideoCircleAccess& access);
    void openDrive(const QString& suggestedVideo = {});
    void cancelPending();
    // 错误交给宿主现有轻提示；云盘页面不再为请求状态占用底部区域。
    void setErrorHandler(std::function<void(const QString&)> handler);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Reply;
    struct ImageReply;
    void refresh(bool append = false);
    void refreshCheckpoints();
    void renderVideos(const QJsonArray& videos, bool append);
    void renderFolders();
    void updateFolderNavigation();
    void navigateToFolder(const QString& folderId);
    QString folderPath(const QString& folderId) const;
    void createFolder();
    void deleteFolder(const QString& folderId, const QString& name);
    void mutateFolder(int action, const QJsonObject& payload);
    void renderCheckpoints(const QJsonArray& uploads);
    void loadThumbnail(const QString& mediaId, QLabel* target);
    void resetThumbnails();
    void startUpload(const QString& sourcePath, const QString& checkpointPath = {},
                     const QString& originalName = {});
    void startPlayback(const QString& videoId, const QString& name);
    void saveVideo(const QString& videoId, const QString& name);
    void startImagePreview(const QString& mediaId);
    void deleteVideo(const QString& videoId, const QString& name);
    void discardUpload(const QString& checkpointPath, const QString& name);
    void setBusy(bool busy, const QString& message = {});
    void setStatus(const QString& message, bool error = false);
    bool sameAccount() const;
    static Reply request(const DesktopVideoCircleAccess& access, int action,
                         const QJsonObject& payload,
                         const MaiCloudDriveCancel* cancel);

    AccessProvider accessProvider_;
    AccountIdProvider accountIdProvider_;
    PlayCallback play_;
    RelocateSelector relocate_;
    QString openedAccountId_;
    QString nextCursor_;
    QString currentFolderId_;
    QJsonArray folders_;
    QString refreshNotice_;
    unsigned generation_ = 0;
    unsigned thumbnailGeneration_ = 0;
    bool busy_ = false;
    QLabel* quota_ = nullptr;
    QProgressBar* quotaBar_ = nullptr;
    QScrollArea* uploadScroll_ = nullptr;
    QPushButton* uploadButton_ = nullptr;
    QPushButton* newFolderButton_ = nullptr;
    QPushButton* parentFolderButton_ = nullptr;
    QPushButton* rootFolderButton_ = nullptr;
    QPushButton* refreshButton_ = nullptr;
    QLabel* folderPath_ = nullptr;
    QPushButton* moreButton_ = nullptr;
    QVBoxLayout* uploadRows_ = nullptr;
    QVBoxLayout* videoRows_ = nullptr;
    std::shared_ptr<MaiCloudDriveCancel> requestCancel_;
    std::shared_ptr<MaiDownloadCancel> downloadCancel_;
    std::shared_ptr<MaiCloudVodCancel> vodCancel_;
    std::shared_ptr<MaiCloudDriveCancel> thumbnailRequestCancel_;
    std::shared_ptr<MaiDownloadCancel> thumbnailDownloadCancel_;
    std::shared_ptr<QTemporaryDir> thumbnailDirectory_;
    bool embedded_ = false;
    std::function<void(const QString&)> errorHandler_;
};
