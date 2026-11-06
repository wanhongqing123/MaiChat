// 文件作用：桌面端共用界面；UiZoom 负责界面缩放比例和布局协调，本文件给出其实现。
// 实现边界：与 UiZoom 的公开声明配套，调用方通过相应接口进入。
#include "ui/UiZoom.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QSettings>
#include <QApplication>
#include <QWidget>
#include <QFont>
#include <QFontDatabase>
#include "markdown/TypographyTokens.h"
#include "markdown/MarkdownLabel.h"
#include "markdown/MarkdownView.h"

namespace {

constexpr qreal kMinFactor = 0.8;
constexpr qreal kMaxFactor = 2.0;
constexpr qreal kStep = 0.1;
constexpr qreal kDefaultFactor = 1.0;

bool loaded = false;
qreal current = kDefaultFactor;
QString currentTextPreset = QStringLiteral("standard");
QString currentFontFamily = QStringLiteral("system");

qreal clampFactor(qreal value) {
    return qBound(kMinFactor, value, kMaxFactor);
}

bool canPersist() {
    // 组织名未设置（单测环境）时不读写 QSettings，避免测试污染注册表。
    return !QCoreApplication::organizationName().isEmpty();
}

void ensureLoaded() {
    if (loaded) return;
    loaded = true;
    if (!canPersist()) return;
    QSettings settings;
    current = clampFactor(settings.value(QStringLiteral("ui/zoomFactor"), kDefaultFactor).toReal());
    currentFontFamily = settings.value(QString::fromLatin1(TypographyTokens::kFontFamilyPreferenceKey), QStringLiteral("system")).toString();
    currentTextPreset = settings.value(QStringLiteral("ui/textSize"), QStringLiteral("standard")).toString();
}

}  // namespace

namespace UiZoom {

QString textPreset() { ensureLoaded(); return currentTextPreset; }

QStringList availableFontFamilyIds() {
    QStringList result;
    const QStringList installed = QFontDatabase().families();
    for (const auto& entry : TypographyTokens::kFontFamilies) {
#ifndef Q_OS_WIN
        if (entry.windowsOnly) continue;
#endif
        if (!entry.family[0] || installed.contains(QString::fromUtf8(entry.family), Qt::CaseInsensitive))
            result.append(QString::fromLatin1(entry.id));
    }
    return result;
}

QString fontFamilyId() {
    ensureLoaded();
    return availableFontFamilyIds().contains(currentFontFamily) ? currentFontFamily : QStringLiteral("system");
}

void setFontFamilyId(const QString& family) {
    ensureLoaded();
    currentFontFamily = availableFontFamilyIds().contains(family) ? family : QStringLiteral("system");
    if (canPersist()) QSettings().setValue(QString::fromLatin1(TypographyTokens::kFontFamilyPreferenceKey), currentFontFamily);
}

QString fontFamilyTitle(const QString& id) {
    for (const auto& entry : TypographyTokens::kFontFamilies)
        if (id == QString::fromLatin1(entry.id)) return QString::fromUtf8(entry.title);
    return QString::fromUtf8(TypographyTokens::kFontFamilies[0].title);
}

QStringList fontFamilies() {
    const QString choice = fontFamilyId();
    for (const auto& entry : TypographyTokens::kFontFamilies) {
        if (choice != QString::fromLatin1(entry.id)) continue;
        if (entry.family[0]) return {QString::fromUtf8(entry.family)};
    }
    return {QFontDatabase::systemFont(QFontDatabase::GeneralFont).family(),
        QStringLiteral("Noto Sans SC"), QStringLiteral("Microsoft YaHei UI"), QStringLiteral("PingFang SC")};
}


void setTextPreset(const QString& preset) {
    ensureLoaded();
    currentTextPreset = preset == QStringLiteral("large") || preset == QStringLiteral("extraLarge")
        ? preset : QStringLiteral("standard");
    if (canPersist()) QSettings().setValue(QStringLiteral("ui/textSize"), currentTextPreset);
}

qreal textFactor() {
    ensureLoaded();
    return currentTextPreset == QStringLiteral("large") ? TypographyTokens::kLargeScale
        : currentTextPreset == QStringLiteral("extraLarge") ? TypographyTokens::kExtraLargeScale : TypographyTokens::kStandardScale;
}

qreal markdownScale() { return factor() * textFactor(); }

int textPixels(TextRole role) {
    using namespace TypographyTokens;
    qreal ratio = kBodyRatio;
    switch (role) {
        case TextRole::Body: break;
        case TextRole::Detail: ratio = kDetailRatio; break;
        case TextRole::Metadata: ratio = kMetadataRatio; break;
        case TextRole::Heading1: ratio = kHeading1Ratio; break;
        case TextRole::Heading2: ratio = kHeading2Ratio; break;
        case TextRole::Heading3: ratio = kHeading3Ratio; break;
        case TextRole::Title: ratio = kTitleRatio; break;
        case TextRole::Section: ratio = kSectionRatio; break;
        case TextRole::Display: ratio = kDisplayRatio; break;
        case TextRole::Hero: ratio = kHeroRatio; break;
    }
    return qMax(1, qRound(kBody * ratio * markdownScale()));
}

void setStyle(QWidget* widget, const QString& source) {
    widget->setProperty("maichatStyleSource", source);
    widget->setStyleSheet(scaleQss(source));
}

void setFont(QWidget* widget, TextRole role, const QFont& source) {
    widget->setProperty("maichatTextRole", static_cast<int>(role));
    QFont font = source;
    if (font.styleHint() != QFont::TypeWriter) font.setFamilies(fontFamilies());
    font.setPixelSize(textPixels(role));
    widget->setFont(font);
}

void refreshText() {
    QFont base = QApplication::font();
    base.setFamilies(fontFamilies());
    base.setPixelSize(textPixels(TextRole::Body));
    QApplication::setFont(base);
    for (QWidget* widget : QApplication::allWidgets()) {
        const QVariant style = widget->property("maichatStyleSource");
        if (style.isValid()) widget->setStyleSheet(scaleQss(style.toString()));
        const QVariant role = widget->property("maichatTextRole");
        if (role.isValid()) setFont(widget, static_cast<TextRole>(role.toInt()), widget->font());
        // Markdown 的解析结果保留，只使字号相关布局缓存失效。
        if (auto* label = qobject_cast<MarkdownLabel*>(widget)) {
            label->setTheme(MarkdownTheme::standard(markdownScale(), fontFamilies().first()));
        } else if (auto* view = qobject_cast<MarkdownView*>(widget)) {
            MarkdownTheme theme = MarkdownTheme::standard(markdownScale(), fontFamilies().first());
            theme.viewBackground = view->theme().viewBackground;
            theme.viewMargin = view->theme().viewMargin;
            view->setTheme(theme);
        }
    }
}

qreal factor() {
    ensureLoaded();
    return current;
}

qreal setFactor(qreal value) {
    ensureLoaded();
    current = clampFactor(value);
    if (canPersist()) {
        QSettings settings;
        settings.setValue(QStringLiteral("ui/zoomFactor"), current);
    }
    return current;
}

qreal minFactor() { return kMinFactor; }
qreal maxFactor() { return kMaxFactor; }
qreal step() { return kStep; }

int s(int px) {
    if (px == 0) return 0;
    const int scaled = qRound(px * factor());
    return px > 0 ? qMax(1, scaled) : qMin(-1, scaled);
}

QString scaleQss(const QString& qss) {
    ensureLoaded();
    // 先解析语义字号，再统一缩放几何像素；字体倍率只参与前一步。
    QString semantic = qss;
    semantic.replace(QStringLiteral("@font"), QStringLiteral("\"") + fontFamilies().first() + QStringLiteral("\""));
    const char* names[] = {"body", "detail", "metadata", "heading1", "heading2", "heading3", "title", "section", "display", "hero"};
    const TextRole roles[] = {TextRole::Body, TextRole::Detail, TextRole::Metadata, TextRole::Heading1, TextRole::Heading2,
        TextRole::Heading3, TextRole::Title, TextRole::Section, TextRole::Display, TextRole::Hero};
    for (int i = 0; i < 10; ++i) {
        semantic.replace(QStringLiteral("@") + QString::fromLatin1(names[i]),
            QString::number(textPixels(roles[i]) / current, 'f', 3) + QStringLiteral("px"));
    }
    static const QRegularExpression pxPattern(QStringLiteral("(\\d+(?:\\.\\d+)?)px"));
    QString result;
    result.reserve(semantic.size() + 16);
    int last = 0;
    QRegularExpressionMatchIterator it = pxPattern.globalMatch(semantic);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        result += semantic.mid(last, match.capturedStart() - last);
        const qreal value = match.captured(1).toDouble();
        const int scaled = qFuzzyIsNull(value) ? 0 : qMax(1, qRound(value * current));
        result += QString::number(scaled);
        result += QStringLiteral("px");
        last = match.capturedEnd();
    }
    result += semantic.mid(last);
    return result;
}

}  // namespace UiZoom
