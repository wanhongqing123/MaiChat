// 通过现有 IM 发现可共享的电脑；必须明确选中可用设备才返回，不自动连接首个回复者。
#pragma once

#include <QDialog>
#include <QPointer>
#include <QString>

class QLabel;
class QListWidget;
class QPushButton;
class RemoteDesktopController;

class RemoteDesktopDeviceDialog final : public QDialog {
  public:
    RemoteDesktopDeviceDialog(RemoteDesktopController &controller, QString peerId,
                              QWidget *parent = nullptr);
    ~RemoteDesktopDeviceDialog() override;
    QString selectedDeviceId() const;
    QString selectedPeerId() const;

  private:
    void refresh();
    void renderDevices();
    QPointer<RemoteDesktopController> controller_;
    QString peerId_;
    QLabel *status_ = nullptr;
    QListWidget *devices_ = nullptr;
    QPushButton *connect_ = nullptr;
    bool searching_ = false;
};
