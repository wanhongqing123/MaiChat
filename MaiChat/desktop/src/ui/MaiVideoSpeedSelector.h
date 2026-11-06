// 文件作用：共用播放器的倍速入口；保留 QComboBox 的索引和信号，使用向上展开的浮层呈现已有倍率。
#pragma once

#include "ui/UiZoom.h"

#include <QAction>
#include <QComboBox>
#include <QGuiApplication>
#include <QHideEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <vector>

class MaiVideoSpeedSelector final : public QComboBox {
public:
    explicit MaiVideoSpeedSelector(QWidget* parent = nullptr) : QComboBox(parent) {
        setCursor(Qt::PointingHandCursor);
        menu_ = new SpeedMenu(this);
        menu_->setObjectName(QStringLiteral("ffplaySpeedMenu"));
        connect(menu_, &QMenu::aboutToHide, this, [this] {
            // 自定义浮层关闭后也清理 QComboBox 的按压状态，避免下一次点击被当成收起操作。
            QComboBox::hidePopup();
            update();
        });
        connect(this, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { updateRateDescription(); });
        updateRateDescription();
    }

    void showPopup() override {
        if (!isEnabled() || count() == 0 || menu_->isVisible()) return;

        // 只改变展示顺序；动作仍回传原索引，不影响宿主已有的倍率数据和播放切换路径。
        std::vector<int> indices;
        indices.reserve(static_cast<std::size_t>(count()));
        for (int index = 0; index < count(); ++index) indices.push_back(index);
        std::stable_sort(indices.begin(), indices.end(), [this](int left, int right) {
            return itemData(left).toDouble() > itemData(right).toDouble();
        });
        menu_->clear();
        QAction* currentAction = nullptr;
        for (const int index : indices) {
            auto* action = menu_->addAction(rateText(index));
            action->setData(index);
            action->setCheckable(true);
            action->setChecked(index == currentIndex());
            if (action->isChecked()) currentAction = action;
            connect(action, &QAction::triggered, this, [this, index] {
                if (index < 0 || index >= count()) return;
                QPointer<MaiVideoSpeedSelector> alive(this);
                if (currentIndex() != index) setCurrentIndex(index);
                if (!alive) return;
                updateRateDescription();
                // 与普通 QComboBox 一样，重复选择当前档不发 currentIndexChanged，但仍算一次用户选择。
                emit activated(index);
                if (alive) emit textActivated(itemText(index));
            });
        }

        updateRateDescription();
        menu_->ensurePolished();
        menu_->adjustSize();
        const QSize popupSize = menu_->size();
        const QPoint anchor = mapToGlobal(QPoint(width() / 2, 0));
        QPoint position(anchor.x() - popupSize.width() / 2, anchor.y() - popupSize.height() - 8);
        QScreen* screen = QGuiApplication::screenAt(anchor);
        if (!screen) screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect bounds = screen->availableGeometry();
            position.setX(
                qBound(bounds.left(), position.x(), qMax(bounds.left(), bounds.right() - popupSize.width() + 1)));
            position.setY(qMax(bounds.top(), position.y()));
        }
        menu_->popup(position);
        menu_->setActiveAction(currentAction);
        update();
    }

    void hidePopup() override {
        if (menu_->isVisible()) {
            menu_->hide();
            return;
        }
        QComboBox::hidePopup();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        // 宿主会在 QSignalBlocker 内同步倍率，因此重绘时也更新文字描述，防止无障碍名落后于实际倍率。
        updateRateDescription();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF bounds = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (isEnabled() && (underMouse() || menu_->isVisible())) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 18));
            painter.drawRoundedRect(bounds, 5, 5);
        }
        if (hasFocus() && window()->testAttribute(Qt::WA_KeyboardFocusChange)) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QColor(QStringLiteral("#737373")));
            painter.drawRoundedRect(bounds, 5, 5);
        }
        painter.setPen(QColor(isEnabled() ? QStringLiteral("#f4f4f4") : QStringLiteral("#777777")));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("倍速"));
    }

    void keyPressEvent(QKeyEvent* event) override {
        const int key = event->key();
        if (key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_F4 ||
            ((key == Qt::Key_Down || key == Qt::Key_Up) && event->modifiers().testFlag(Qt::AltModifier))) {
            showPopup();
            event->accept();
            return;
        }
        QComboBox::keyPressEvent(event);
    }

    void hideEvent(QHideEvent* event) override {
        hidePopup();
        QComboBox::hideEvent(event);
    }

private:
    class SpeedMenu final : public QMenu {
    public:
        explicit SpeedMenu(QWidget* parent) : QMenu(parent) {
            // 使用 Qt popup 保留键盘、Esc 和点外部关闭语义；透明窗口避免圆角外围出现系统白底。
            setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
            setAttribute(Qt::WA_TranslucentBackground);
            setFixedWidth(80);
            UiZoom::setStyle(this,
                             QStringLiteral("QMenu {background:transparent;border:0;padding:6px 0;font-size:@detail;}"
                                            "QMenu::item {min-height:30px;padding:0 8px;margin:1px 6px;}"
                                            "QMenu::indicator {width:0;height:0;}"));
        }

    protected:
        void paintEvent(QPaintEvent*) override {
            // 菜单几何和选择交给 QMenu；只统一绘制圆角、居中的倍率和当前档，避开各系统默认勾选栏。
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(QStringLiteral("#292929")));
            painter.drawRoundedRect(QRectF(rect()), 8, 8);
            for (auto* action : actions()) {
                if (!action->isVisible()) continue;
                const QRect item = actionGeometry(action).adjusted(6, 1, -6, -1);
                if (action == activeAction() || action->isChecked()) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(
                        QColor(action == activeAction() ? QStringLiteral("#505050") : QStringLiteral("#404040")));
                    painter.drawRoundedRect(QRectF(item), 6, 6);
                }
                painter.setPen(QColor(QStringLiteral("#f4f4f4")));
                painter.drawText(item, Qt::AlignCenter, action->text());
            }
        }
    };

    QString rateText(int index) const {
        bool valid = false;
        const double rate = itemData(index).toDouble(&valid);
        if (!valid || !std::isfinite(rate)) return itemText(index);
        const QString value = std::floor(rate) == rate ? QString::number(rate, 'f', 1) : QString::number(rate, 'g', 3);
        return value + QStringLiteral("x");
    }

    void updateRateDescription() {
        const QString description = currentIndex() >= 0 ? QStringLiteral("播放倍速：%1").arg(rateText(currentIndex()))
                                                        : QStringLiteral("播放倍速");
        if (toolTip() != description) setToolTip(description);
        if (accessibleName() != description) setAccessibleName(description);
    }

    SpeedMenu* menu_ = nullptr;
};
