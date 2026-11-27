// 发现与刷新均由远控控制器通过 IM 完成；选择器只管理本次选择，不修改共享权限。
#include "ui/RemoteDesktopDeviceDialog.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "remote/RemoteDesktopController.h"

RemoteDesktopDeviceDialog::RemoteDesktopDeviceDialog(RemoteDesktopController &controller,
                                                     QString peerId, QWidget *parent)
    : QDialog(parent), controller_(&controller), peerId_(std::move(peerId)) {
    setObjectName(QStringLiteral("remoteDesktopDeviceDialog"));
    setWindowTitle(QStringLiteral("选择远程电脑"));
    setModal(true);
    resize(440, 360);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);
    auto *title = new QLabel(QStringLiteral("选择要连接的电脑"), this);
    layout->addWidget(title);
    auto *scope = new QComboBox(this);
    scope->setObjectName(QStringLiteral("remoteDeviceAccountScope"));
    scope->addItem(QStringLiteral("对方的电脑"), peerId_);
    if (peerId_ != controller_->localUserId())
        scope->addItem(QStringLiteral("我的电脑"), controller_->localUserId());
    layout->addWidget(scope);
    connect(scope, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, scope](int) {
        peerId_ = scope->currentData().toString();
        refresh();
    });
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setObjectName(QStringLiteral("remoteDeviceStatus"));
    layout->addWidget(status_);
    devices_ = new QListWidget(this);
    devices_->setObjectName(QStringLiteral("remoteDeviceList"));
    devices_->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(devices_, 1);
    auto *buttons = new QHBoxLayout;
    auto *retry = new QPushButton(QStringLiteral("刷新"), this);
    retry->setObjectName(QStringLiteral("remoteDeviceRefresh"));
    auto *cancel = new QPushButton(QStringLiteral("取消"), this);
    connect_ = new QPushButton(QStringLiteral("连接"), this);
    connect_->setObjectName(QStringLiteral("remoteDeviceConnect"));
    connect_->setDefault(true);
    connect_->setEnabled(false);
    buttons->addWidget(retry);
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(connect_);
    layout->addLayout(buttons);
    connect(retry, &QPushButton::clicked, this, [this] { refresh(); });
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(connect_, &QPushButton::clicked, this, [this] {
        if (!selectedDeviceId().isEmpty())
            accept();
    });
    connect(devices_, &QListWidget::currentItemChanged, this,
            [this] { connect_->setEnabled(!selectedDeviceId().isEmpty()); });
    connect(controller_.data(), &RemoteDesktopController::devicesChanged, this,
            [this] { renderDevices(); });
    connect(controller_.data(), &RemoteDesktopController::discoveryFinished, this, [this] {
        searching_ = false;
        renderDevices();
    });
    QTimer::singleShot(0, this, [this] { refresh(); });
}

RemoteDesktopDeviceDialog::~RemoteDesktopDeviceDialog() {
    if (controller_)
        controller_->cancelDiscovery();
}

QString RemoteDesktopDeviceDialog::selectedDeviceId() const {
    const auto *item = devices_->currentItem();
    return item && (item->flags() & Qt::ItemIsEnabled) ? item->data(Qt::UserRole).toString()
                                                       : QString{};
}

void RemoteDesktopDeviceDialog::refresh() {
    if (!controller_)
        return;
    searching_ = true;
    controller_->discoverDevices(peerId_);
    renderDevices();
}

void RemoteDesktopDeviceDialog::renderDevices() {
    if (!controller_) {
        connect_->setEnabled(false);
        return;
    }
    const QString selected = selectedDeviceId();
    devices_->clear();
    for (const auto &device : controller_->discoveredDevices()) {
        QString label = device.deviceName.isEmpty() ? QStringLiteral("电脑") : device.deviceName;
        if (!device.platform.isEmpty())
            label += QStringLiteral(" · ") + device.platform;
        if (!device.available)
            label += QStringLiteral(" · 暂不可连接");
        auto *item = new QListWidgetItem(label, devices_);
        item->setData(Qt::UserRole, device.sourceDeviceId);
        if (!device.available)
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        if (device.available && device.sourceDeviceId == selected)
            devices_->setCurrentItem(item);
    }
    // 不预选首个回复，避免 IM 到达顺序替用户决定目标；刷新保留仍有效的显式选择。
    connect_->setEnabled(!selectedDeviceId().isEmpty());
    status_->setText(
        searching_ ? QStringLiteral("正在查找在线的电脑…")
        : devices_->count() == 0
            ? QStringLiteral("未找到支持的电脑。请确认电脑在线并已更新 MaiChat，再刷新。")
            : QStringLiteral("选择一台可用电脑，连接后仍按该电脑的授权设置确认。"));
}

QString RemoteDesktopDeviceDialog::selectedPeerId() const { return peerId_; }
