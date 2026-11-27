// 视频时间轴：点击/拖动定位与悬停时间共用轨道坐标映射，悬停不改变播放位置。
#pragma once

#include <QSlider>
#include <cstdint>

class MaiVideoSeekSlider final : public QSlider {
  public:
    explicit MaiVideoSeekSlider(QWidget *parent = nullptr);
    void setDurationUs(int64_t duration);

  protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

  private:
    int valueAt(int x, int offset) const;
    void moveTo(int x);
    int64_t durationUs_ = 0;
    int dragOffset_ = 0;
};
