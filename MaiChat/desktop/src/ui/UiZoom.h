// 文件作用：桌面端共用界面；UiZoom 负责界面缩放比例和布局协调，本文件给出其声明。
// 对外边界：本文件给出 UiZoom 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>
class QWidget;
class QFont;

// 全局界面缩放（飞书式 Ctrl+= / Ctrl+- / Ctrl+0）。
// factor 范围 0.8~2.0、步进 0.1，持久化到 QSettings("ui/zoomFactor")（组织名未设置
// 时——如单测环境——只存内存，不碰注册表）。s(px) 缩放单个像素值；scaleQss 把样式表
// 字符串里的所有 "<n>px" 按当前倍率缩放（0px 保持 0，非零值最小 1px 不被缩没）。
namespace UiZoom {

// 字号独立于窗口缩放；同类文字使用角色，不能把文字大小当作几何尺寸。
enum class TextRole { Body, Detail, Metadata, Heading1, Heading2, Heading3, Title, Section, Display, Hero };
QString textPreset();
QString fontFamilyId();
void setFontFamilyId(const QString& family);
QStringList availableFontFamilyIds();
QString fontFamilyTitle(const QString& id);
QStringList fontFamilies();
void setTextPreset(const QString& preset);
qreal textFactor();
qreal markdownScale();
int textPixels(TextRole role);
// 保存语义源值，偏好变化后刷新现有控件；不替换编辑器或清除输入状态。
void setStyle(QWidget* widget, const QString& source);
void setFont(QWidget* widget, TextRole role, const QFont& source);
void refreshText();

qreal factor();
// 设置倍率并返回实际生效值（越界被夹紧到 [minFactor, maxFactor]）。
qreal setFactor(qreal value);
qreal minFactor();
qreal maxFactor();
qreal step();

int s(int px);
QString scaleQss(const QString& qss);

}  // namespace UiZoom
