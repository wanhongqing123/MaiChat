// 文件作用：桌面端Markdown 内容解析与呈现；MarkdownView 负责界面呈现、交互与状态更新，本文件给出其实现。
// 主要入口：setTheme、applyScrollBarStyle、theme、setMaxContentWidth。
// 直接依赖：markdown/MarkdownDocument.h、markdown/MarkdownLayout.h；实现与这些接口协作。
#include "markdown/MarkdownView.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEvent>
#include <QElapsedTimer>
#include <QDebug>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QThread>
#include <QWheelEvent>
#include <algorithm>
#include <memory>
#include <vector>

#include "markdown/MarkdownDocument.h"
#include "markdown/MarkdownLayout.h"
#include "markdown/MarkdownCodeScrollState.h"

// 一条内容。位置都在**内容坐标系**里：y 从整片的顶上算起，和滚动位置无关。
struct MarkdownViewItem {
    QString id;
    MarkdownView::Style style = MarkdownView::Style::Document;
    QString source;
    MarkdownDocument document;
    MarkdownLayout layout;
    MarkdownCodeScrollState codeScroll;

    // Positions change when cards expand or rows move, while unchanged text
    // keeps its expensive QTextLayouts. Theme revisions are per owning view;
    // custom Agent themes must never share IM cache keys based only on zoom.
    bool layoutValid = false;
    qreal layoutAvailableWidth = 0;
    quint64 layoutThemeRevision = 0;

    // 外框（气泡的底、整行的范围）和里面文字的左上角。
    QRectF frame;
    QPointF origin;

    // 选区用：这条内容的文字在整片里的起点。
    int textStart = 0;

    // 嵌进来的部件（工具卡那种）。非空时这条不走 Markdown 那套。
    QWidget* widget = nullptr;
    // 上次给它留的高度。部件自己变高时要认出来，但 setGeometry 又会回弹
    // 一个 Resize 事件——比高度才能分清「真变了」和「我自己刚摆的」。
    int widgetHeight = 0;

    int textLength() const {
        return widget != nullptr ? 0 : layout.textLength();
    }
};

namespace {

using Item = MarkdownViewItem;

// 给嵌进来的部件留多高。
//
// **不能只看 sizeHint。** QLabel 之类的 sizeHint 给的是文字的自然高度，
// 调用方 setFixedHeight 之后它一点不变——工具卡展开了这儿还按旧高度留位，
// 卡片就会盖住下一条消息。这里按 Qt 布局的规矩来：先问 heightForWidth，
// 没有就用 sizeHint，最后夹到 [minimumHeight, maximumHeight] 之间。
// 给它留多宽。不想撑满的部件（工具卡：横向策略 Fixed / Maximum）按自己的
// sizeHint 走，靠左摆——撑满会让一条注记看起来比回答还重要。
int widgetSlotWidth(const QWidget* widget, int available) {
    const QSizePolicy::Policy policy = widget->sizePolicy().horizontalPolicy();
    if (policy != QSizePolicy::Fixed && policy != QSizePolicy::Maximum) return available;
    return qBound(1, widget->sizeHint().width(), available);
}

int widgetSlotHeight(const QWidget* widget, int width) {
    int height = widget->sizeHint().height();
    if (widget->hasHeightForWidth() && width > 0) {
        // A natural hint may assume a narrow default width. A supported HFW
        // result already measures wrapping at our actual column width; taking
        // max(hint, HFW) leaves large blank tails in expanded sub-agent cards.
        // Negative HFW means unsupported, so only that case keeps the hint.
        const int wrappedHeight = widget->heightForWidth(width);
        if (wrappedHeight >= 0) height = wrappedHeight;
    }
    return qBound(widget->minimumHeight(), qMax(height, 1), widget->maximumHeight());
}

}  // namespace

struct MarkdownView::Runtime {
    MarkdownTheme theme = MarkdownTheme::standard(1.0);
    quint64 themeRevision = 1;
    std::vector<std::unique_ptr<Item>> items;
    qreal contentHeight = 0;
    int maxContentWidth = 0;
    int batchDepth = 0;
    bool layoutDirty = false;
    QString draggingCodeItem;

    // 贴底跟随。用户自己往上滚过就停掉——正在看历史却被拽回底部很烦。
    bool stickToBottom = true;
    bool preservingScrollState = false;

    // 选区，整片的字符偏移。
    int anchor = 0;
    int cursor = 0;
    bool selecting = false;
    QPoint pressPosition;

    // Global character offsets change when items move or earlier text grows.
    // Keep endpoints by stable item ID until the next real layout, including
    // batched changes; selection in a later item must not migrate into a stream.
    struct SelectionPoint {
        QString id;
        int offset = 0;
        bool valid = false;
    };
    bool selectionReorderPending = false;
    bool reorderedSelectionWasAll = false;
    bool reorderedSelectionReversed = false;
    SelectionPoint reorderedAnchor;
    SelectionPoint reorderedCursor;

    Item* find(const QString& id) const {
        for (const std::unique_ptr<Item>& item : items) {
            if (item->id == id) return item.get();
        }
        return nullptr;
    }

    void rememberSelectionForReorder() {
        if (selectionReorderPending || anchor == cursor) return;
        int total = 0;
        for (const auto& item : items) total += item->textLength();
        reorderedSelectionWasAll = qMin(anchor, cursor) == 0 && qMax(anchor, cursor) == total;
        reorderedSelectionReversed = anchor > cursor;
        const auto locate = [this](int position, bool preferPrevious) {
            SelectionPoint point;
            int start = 0;
            for (const auto& item : items) {
                const int length = item->textLength();
                if (length <= 0) continue;
                point = {item->id, qBound(0, position - start, length), true};
                if (position < start + length || (preferPrevious && position == start + length)) break;
                start += length;
            }
            return point;
        };
        reorderedAnchor = locate(anchor, anchor > cursor);
        reorderedCursor = locate(cursor, cursor > anchor);
        selectionReorderPending = true;
    }

    void restoreSelectionAfterReorder(int total) {
        if (!selectionReorderPending) return;
        selectionReorderPending = false;
        if (reorderedSelectionWasAll) {
            anchor = reorderedSelectionReversed ? total : 0;
            cursor = reorderedSelectionReversed ? 0 : total;
            return;
        }
        const auto locate = [this, total](const SelectionPoint& point, int fallback) {
            const Item* item = point.valid ? find(point.id) : nullptr;
            return item ? item->textStart + qBound(0, point.offset, item->textLength()) : qBound(0, fallback, total);
        };
        anchor = locate(reorderedAnchor, anchor);
        cursor = locate(reorderedCursor, cursor);
    }
};

MarkdownView::MarkdownView(QWidget* parent) : QAbstractScrollArea(parent), runtime_(std::make_unique<Runtime>()) {
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setAutoFillBackground(false);
    // 文字上要显示 I 形光标，链接上要显示手形——两种都在 mouseMoveEvent 里切。
    viewport()->setCursor(Qt::IBeamCursor);
    setFocusPolicy(Qt::StrongFocus);

    applyScrollBarStyle();
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int value) {
                if (!runtime_->preservingScrollState)
                    runtime_->stickToBottom = value >= verticalScrollBar()->maximum() - 2;
            });
}

MarkdownView::~MarkdownView() = default;

void MarkdownView::setTheme(const MarkdownTheme& theme) {
    runtime_->theme = theme;
    ++runtime_->themeRevision;
    applyScrollBarStyle();
    // A detached page keeps its old revision, so it also reflows if the common
    // theme changes while another session is visible.
    relayoutAll();
}

// 细滚动条，和 MaiChat 会话列表那两条一致。
//
// 样式表**只挂在自己的滚动条上**，不往上挂：挂在视图上的话会顺着层级
// 落到嵌进来的工具卡里去。上下箭头按钮要置零，8px 宽度下它们会挤成两个色块——
// 原生那两个箭头正是这么来的。
void MarkdownView::applyScrollBarStyle() {
    const MarkdownTheme& theme = runtime_->theme;
    verticalScrollBar()->setStyleSheet(QStringLiteral("QScrollBar:vertical{background:transparent;width:%1px;margin:0;}"
                                                      "QScrollBar::handle:vertical{background:%2;border-radius:%3px;"
                                                      "min-height:%4px;}"
                                                      "QScrollBar::handle:vertical:hover{background:%5;}"
                                                      "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{"
                                                      "height:0;border:0;background:transparent;}"
                                                      "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{"
                                                      "background:transparent;}")
                                           .arg(theme.scrollBarWidth)
                                           .arg(theme.scrollBarHandle.name())
                                           .arg(theme.scrollBarWidth / 2)
                                           .arg(theme.scrollBarWidth * 3)
                                           .arg(theme.scrollBarHandleHover.name()));
}

const MarkdownTheme& MarkdownView::theme() const {
    return runtime_->theme;
}

void MarkdownView::setMaxContentWidth(int width) {
    if (runtime_->maxContentWidth == width) return;
    runtime_->maxContentWidth = qMax(0, width);
    relayoutAll();
}

void MarkdownView::addItem(const QString& id, Style style, const QString& markdown) {
    if (Item* existing = runtime_->find(id)) {
        if (existing->style == style && existing->source == markdown) return;
        runtime_->rememberSelectionForReorder();
        if (!markdown.startsWith(existing->source)) existing->codeScroll.clear();
        existing->style = style;
        existing->source = markdown;
        existing->document = MarkdownDocument::parse(markdown);
        existing->layoutValid = false;
        relayoutAll();
        return;
    }
    auto item = std::make_unique<Item>();
    item->id = id;
    item->style = style;
    item->source = markdown;
    item->document = MarkdownDocument::parse(markdown);
    runtime_->items.push_back(std::move(item));
    relayoutAll();
}

void MarkdownView::updateItem(const QString& id, const QString& markdown) {
    Item* item = runtime_->find(id);
    if (item == nullptr) return;
    if (item->source == markdown) return;
    runtime_->rememberSelectionForReorder();
    if (!markdown.startsWith(item->source)) item->codeScroll.clear();
    item->source = markdown;
    item->document = MarkdownDocument::parse(markdown);
    item->layoutValid = false;
    relayoutAll();
}

bool MarkdownView::contains(const QString& id) const {
    return runtime_->find(id) != nullptr;
}

void MarkdownView::removeItem(const QString& id) {
    for (auto it = runtime_->items.begin(); it != runtime_->items.end(); ++it) {
        if ((*it)->id != id) continue;
        if ((*it)->widget != nullptr) (*it)->widget->deleteLater();
        runtime_->items.erase(it);
        relayoutAll();
        return;
    }
}

bool MarkdownView::moveItemAfter(const QString& id, const QString& anchorId) {
    auto& items = runtime_->items;
    const auto item = std::find_if(items.begin(), items.end(), [&](const auto& value) { return value->id == id; });
    const auto anchor =
        std::find_if(items.begin(), items.end(), [&](const auto& value) { return value->id == anchorId; });
    if (item == items.end() || anchor == items.end()) return false;
    if (item == anchor || item == anchor + 1) return true;

    runtime_->rememberSelectionForReorder();
    // Rotate unique_ptr entries only: the item, widget parent, child controls,
    // draft and signal connections all keep their original lifetime.
    if (item < anchor)
        std::rotate(item, item + 1, anchor + 1);
    else
        std::rotate(anchor + 1, item, item + 1);
    relayoutAll();
    return true;
}

bool MarkdownView::moveItemBefore(const QString& id, const QString& anchorId) {
    auto& items = runtime_->items;
    const auto item = std::find_if(items.begin(), items.end(), [&](const auto& value) { return value->id == id; });
    const auto anchor =
        std::find_if(items.begin(), items.end(), [&](const auto& value) { return value->id == anchorId; });
    if (item == items.end() || anchor == items.end()) return false;
    if (item == anchor || item + 1 == anchor) return true;
    runtime_->rememberSelectionForReorder();
    // Prepending older pages preserves original widgets and their interaction
    // state. Recreating the view here would lose selection and reading position.
    if (item < anchor)
        std::rotate(item, item + 1, anchor);
    else
        std::rotate(anchor, item, item + 1);
    relayoutAll();
    return true;
}

void MarkdownView::addWidget(const QString& id, QWidget* widget) {
    if (widget == nullptr) return;
    widget->setParent(viewport());
    widget->installEventFilter(this);
    auto item = std::make_unique<Item>();
    item->id = id;
    item->widget = widget;
    runtime_->items.push_back(std::move(item));
    relayoutAll();
}

void MarkdownView::beginBatchUpdates() {
    ++runtime_->batchDepth;
}

void MarkdownView::endBatchUpdates() {
    if (runtime_->batchDepth <= 0) {
        Q_ASSERT(false);
        return;
    }
    if (--runtime_->batchDepth == 0 && runtime_->layoutDirty) {
        runtime_->layoutDirty = false;
        relayoutAll();
    }
}

// A saved page owns its explicitly hidden widget roots through QPointer.
// Keeping their viewport parent avoids repolishing every tool-card descendant
// on session switches. The pointers also become null if the view dies first.
class MarkdownView::PageState : public QObject {
public:
    ~PageState() override {
        for (const QPointer<QWidget>& widget : widgets)
            if (widget) delete widget.data();
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        // Queued callbacks may still update a dormant card or call show().
        // Its children retain their new state, but its root stays hidden until
        // restore removes this filter and makes the page active again.
        if (event->type() == QEvent::Show) {
            if (auto* widget = qobject_cast<QWidget*>(watched)) widget->hide();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    friend class MarkdownView;
    QPointer<MarkdownView> owner;
    std::vector<QPointer<QWidget>> widgets;
    std::unique_ptr<Runtime> runtime;
    int scrollPosition = 0;
    QSize viewSize;
};

std::shared_ptr<MarkdownView::PageState> MarkdownView::takePage() {
    Q_ASSERT(thread() == QThread::currentThread());
    Q_ASSERT(runtime_->batchDepth == 0);
    const bool profile = qEnvironmentVariableIsSet("MAICHAT_MARKDOWN_PAGE_PROFILE");
    QElapsedTimer timer;
    if (profile) timer.start();
    const int itemCount = static_cast<int>(runtime_->items.size());
    int widgetCount = 0;
    auto page = std::make_shared<PageState>();
    page->owner = this;
    page->scrollPosition = verticalScrollBar()->value();
    page->viewSize = size();
    // Swapping can occur between mouse events. Preserve offsets and selection,
    // but an abandoned drag must never continue onto another session's items.
    runtime_->selecting = false;
    runtime_->draggingCodeItem.clear();
    for (const auto& item : runtime_->items) {
        item->codeScroll.release();
        if (!item->widget) continue;
        ++widgetCount;
        item->widget->hide();
        item->widget->removeEventFilter(this);
        page->widgets.push_back(item->widget);
        item->widget->installEventFilter(page.get());
    }
    const qint64 controlsUs = profile ? timer.nsecsElapsed() / 1000 : 0;
    auto empty = std::make_unique<Runtime>();
    empty->theme = runtime_->theme;
    empty->themeRevision = runtime_->themeRevision;
    empty->maxContentWidth = runtime_->maxContentWidth;
    page->runtime = std::move(runtime_);
    runtime_ = std::move(empty);
    viewport()->setCursor(Qt::IBeamCursor);
    relayoutAll();
    if (profile) qInfo("page_take items=%d widgets=%d controls_us=%lld total_us=%lld", itemCount,
                       widgetCount, controlsUs, timer.nsecsElapsed() / 1000);
    return page;
}

void MarkdownView::restorePage(const std::shared_ptr<PageState>& page) {
    Q_ASSERT(thread() == QThread::currentThread());
    Q_ASSERT(runtime_->batchDepth == 0);
    // Retained controls and their signal connections belong to one view.
    // Wrong-owner and already-consumed states must leave its active page alone.
    if (!page || page->owner != this || !page->runtime) return;
    const bool profile = qEnvironmentVariableIsSet("MAICHAT_MARKDOWN_PAGE_PROFILE");
    QElapsedTimer timer;
    if (profile) timer.start();
    const int itemCount = static_cast<int>(page->runtime->items.size());
    int widgetCount = 0;
    const MarkdownTheme currentTheme = runtime_->theme;
    const quint64 currentRevision = runtime_->themeRevision;
    const int currentMaxWidth = runtime_->maxContentWidth;
    const Qt::ScrollBarPolicy configuredScrollPolicy = verticalScrollBarPolicy();
    const bool sameLayoutSettings = page->viewSize == size() &&
        page->runtime->themeRevision == currentRevision && page->runtime->maxContentWidth == currentMaxWidth;
    clear();
    runtime_ = std::move(page->runtime);
    runtime_->theme = currentTheme;
    runtime_->themeRevision = currentRevision;
    runtime_->maxContentWidth = currentMaxWidth;
    const bool followsBottom = runtime_->stickToBottom;
    for (const auto& item : runtime_->items) {
        if (!item->widget) continue;
        ++widgetCount;
        item->widget->removeEventFilter(page.get());
        item->widget->installEventFilter(this);
    }
    page->widgets.clear(); // Ownership returns to this view's active page.
    const qint64 controlsUs = profile ? timer.nsecsElapsed() / 1000 : 0;
    // Ignore transient values only in our follow-state observer. Qt still
    // needs rangeChanged to show/hide the scrollbar and resize the viewport;
    // blocking the scrollbar object would leave retained column geometry stale.
    {
        QScopedValueRollback<bool> preserving(runtime_->preservingScrollState, true);
        // The empty placeholder can hide an AsNeeded scrollbar. Reserve the
        // saved column width synchronously before measuring an unchanged page;
        // otherwise every warm restore reshapes all text at +scrollbarWidth and
        // then again when overflow brings the bar back. Changed size/theme uses
        // normal reflow. The caller's policy is restored before returning.
        if (sameLayoutSettings && configuredScrollPolicy == Qt::ScrollBarAsNeeded) {
            const bool needsBar = runtime_->contentHeight > viewport()->height();
            setVerticalScrollBarPolicy(needsBar ? Qt::ScrollBarAlwaysOn : Qt::ScrollBarAlwaysOff);
        }
        relayoutAll();
        setVerticalScrollBarPolicy(configuredScrollPolicy);
        verticalScrollBar()->setValue(followsBottom ? verticalScrollBar()->maximum() : page->scrollPosition);
        runtime_->stickToBottom = followsBottom;
    }
    positionWidgets();
    viewport()->update();
    if (profile) qInfo("page_restore items=%d widgets=%d controls_us=%lld total_us=%lld", itemCount,
                       widgetCount, controlsUs, timer.nsecsElapsed() / 1000);
}

void MarkdownView::clear() {
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget == nullptr) continue;
        // deleteLater 要等下一轮事件循环才真正销毁；不先 hide 的话，这段窗口期里
        // 旧部件还盖在新画的内容上（切会话后立刻看到上一会话的残影/遮挡），
        // 而且点击会落在这些死部件上，怎么点都没反应。hide 是同步的，立刻出视野。
        item->widget->hide();
        item->widget->deleteLater();
    }
    runtime_->items.clear();
    runtime_->draggingCodeItem.clear();
    runtime_->anchor = 0;
    runtime_->cursor = 0;
    runtime_->selectionReorderPending = false;
    runtime_->stickToBottom = true;
    relayoutAll();
}

bool MarkdownView::isEmpty() const {
    return runtime_->items.empty();
}

int MarkdownView::itemCount() const {
    return static_cast<int>(runtime_->items.size());
}

QRectF MarkdownView::itemRect(const QString& id) const {
    const Item* item = runtime_->find(id);
    return item == nullptr ? QRectF() : item->frame;
}

qreal MarkdownView::contentHeight() const {
    return runtime_->contentHeight;
}

void MarkdownView::scrollToBottom() {
    runtime_->stickToBottom = true;
    verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

bool MarkdownView::isAtBottom() const {
    return verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 2;
}

// 作用：在宽度或内容变化后重新计算文档排版并更新滚动区域。
void MarkdownView::relayoutAll() {
    if (runtime_->batchDepth > 0) {
        runtime_->layoutDirty = true;
        return;
    }
    const MarkdownTheme& theme = runtime_->theme;
    const qreal margin = theme.viewMargin;
    const qreal full = qMax<qreal>(viewport()->width() - 2 * margin, 1);
    const qreal available = runtime_->maxContentWidth > 0 ? qMin<qreal>(full, runtime_->maxContentWidth) : full;
    // 列宽不够占满时居中。原来是靠 addStretch(1)/addWidget(列,20)/addStretch(1)
    // 那种权重摆出来的，权重给错一次列就只剩三分之一宽。
    const qreal left = margin + (full - available) / 2;

    qreal y = margin;
    int textStart = 0;
    bool first = true;
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (!first) y += theme.itemSpacing;
        first = false;
        item->textStart = textStart;

        if (item->widget != nullptr) {
            const int slotWidth = widgetSlotWidth(item->widget, qRound(available));
            const int height = widgetSlotHeight(item->widget, slotWidth);
            item->widgetHeight = height;
            item->frame = QRectF(left, y, slotWidth, height);
            item->origin = item->frame.topLeft();
            y = item->frame.bottom();
            continue;
        }

        const bool needsLayout = !item->layoutValid || item->layoutAvailableWidth != available ||
            item->layoutThemeRevision != runtime_->themeRevision;
        switch (item->style) {
            case Style::Document: {
                if (needsLayout) item->layout.layout(item->document, theme, available);
                item->frame = QRectF(left, y, available, item->layout.height());
                item->origin = item->frame.topLeft();
                break;
            }
            case Style::Bubble: {
                // 先按最大宽度排一遍，再按实际需要的宽度收窄。短消息不该占满一行，
                // 而**宽度只能量出来不能估**——估窄了字会被提前折断。
                const qreal maximum = qMax<qreal>(available * theme.bubbleMaxWidthRatio - 2 * theme.bubblePadding, 1);
                if (needsLayout) {
                    item->layout.layout(item->document, theme, maximum);
                    const qreal natural = qMin(item->layout.naturalWidth(), maximum);
                    if (natural > 0 && natural < maximum) item->layout.layout(item->document, theme, natural);
                }
                const qreal width = item->layout.width() + 2 * theme.bubblePadding;
                const qreal height = item->layout.height() + 2 * theme.bubblePadding;
                item->frame = QRectF(left + available - width, y, width, height);
                item->origin = item->frame.topLeft() + QPointF(theme.bubblePadding, theme.bubblePadding);
                break;
            }
            case Style::Notice:
            case Style::Error: {
                // 提示行要用自己的颜色，不能和正文一个色——它不是内容，
                // 是「刚才发生了什么」，读的时候应该一眼掠过去。
                if (needsLayout) {
                    MarkdownTheme muted = theme;
                    muted.text = item->style == Style::Error ? theme.errorText : theme.noticeText;
                    muted.bodyPixelSize = qMax(1, theme.bodyPixelSize - 2);
                    item->layout.layout(item->document, muted, available);
                    const qreal width = qMin(item->layout.naturalWidth(), available);
                    item->layout.layout(item->document, muted, qMax<qreal>(width, 1));
                }
                item->frame = QRectF(left + (available - item->layout.width()) / 2, y, item->layout.width(),
                                     item->layout.height());
                item->origin = item->frame.topLeft();
                break;
            }
        }
        item->layoutValid = true;
        item->layoutAvailableWidth = available;
        item->layoutThemeRevision = runtime_->themeRevision;
        item->codeScroll.synchronize(item->layout);
        y = item->frame.bottom();
        textStart += item->textLength();
    }

    runtime_->contentHeight = y + margin;
    runtime_->restoreSelectionAfterReorder(textStart);
    updateScrollRange();
    applySelectionToItems();
    positionWidgets();
    viewport()->update();
}

void MarkdownView::updateScrollRange() {
    const int overflow = qMax(0, qRound(runtime_->contentHeight) - viewport()->height());
    QScrollBar* bar = verticalScrollBar();
    const bool wasAtBottom = runtime_->stickToBottom;
    bar->setRange(0, overflow);
    bar->setPageStep(viewport()->height());
    bar->setSingleStep(qMax(1, runtime_->theme.bodyPixelSize * 3));
    if (wasAtBottom) bar->setValue(overflow);
}

void MarkdownView::positionWidgets() {
    const int offset = verticalScrollBar()->value();
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget == nullptr) continue;
        const QRectF frame = item->frame.translated(0, -offset);
        // 滚出可见区就藏起来：Qt 不会自己裁部件，留着会盖在别的东西上面。
        const bool visible = frame.bottom() >= 0 && frame.top() <= viewport()->height();
        item->widget->setVisible(visible);
        if (!visible) continue;
        item->widget->setGeometry(frame.toRect());
    }
}

void MarkdownView::paintEvent(QPaintEvent* event) {
    const MarkdownTheme& theme = runtime_->theme;
    QPainter painter(viewport());
    painter.fillRect(event->rect(), theme.viewBackground);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int offset = verticalScrollBar()->value();
    const QRectF visible(0, 0, viewport()->width(), viewport()->height());

    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget != nullptr) continue;
        const QRectF frame = item->frame.translated(0, -offset);
        if (!visible.intersects(frame)) continue;

        if (item->style == Style::Bubble) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.bubbleBackground);
            painter.drawRoundedRect(frame, theme.bubbleRadius, theme.bubbleRadius);
        }
        item->layout.paint(&painter, item->origin - QPointF(0, offset), visible, item->codeScroll.offsets);
    }
}

void MarkdownView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    relayoutAll();
}

bool MarkdownView::eventFilter(QObject* watched, QEvent* event) {
    // 工具卡展开或初始空卡填入文字后，宽高都可能变化。只比较高度会让
    // Maximum 策略的子任务卡一直占用空标签时的窄宽度，标题/状态被裁掉。
    // 按实际分配尺寸比较，避免 setGeometry 回弹的 Resize 导致无限递归。
    if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize) {
        for (const std::unique_ptr<Item>& item : runtime_->items) {
            if (item->widget != watched) continue;
            const qreal full = qMax<qreal>(viewport()->width() - 2 * runtime_->theme.viewMargin, 1);
            const qreal available = runtime_->maxContentWidth > 0 ? qMin<qreal>(full, runtime_->maxContentWidth) : full;
            const int width = widgetSlotWidth(item->widget, qRound(available));
            if (width == qRound(item->frame.width()) && widgetSlotHeight(item->widget, width) == item->widgetHeight)
                break;
            relayoutAll();
            break;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

void MarkdownView::scrollContentsBy(int dx, int dy) {
    QAbstractScrollArea::scrollContentsBy(dx, dy);
    positionWidgets();
    viewport()->update();
}

// ── 选区与命中 ──────────────────────────────────────────────────

const MarkdownViewItem* MarkdownView::itemAt(const QPoint& viewportPoint, QPointF* local) const {
    const int offset = verticalScrollBar()->value();
    const QPointF content(viewportPoint.x(), viewportPoint.y() + offset);

    const Item* best = nullptr;
    qreal bestDistance = 0;
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget != nullptr) continue;
        const qreal top = item->frame.top();
        const qreal bottom = item->frame.bottom();
        if (content.y() >= top && content.y() <= bottom) {
            best = item.get();
            break;
        }
        // 拖到两条消息之间的空档时要挑最近的，不然选区会在那儿断掉。
        const qreal distance = content.y() < top ? top - content.y() : content.y() - bottom;
        if (best == nullptr || distance < bestDistance) {
            best = item.get();
            bestDistance = distance;
        }
    }
    if (best != nullptr && local != nullptr) *local = content - best->origin;
    return best;
}

int MarkdownView::positionAt(const QPoint& viewportPoint) const {
    QPointF local;
    const Item* item = itemAt(viewportPoint, &local);
    if (item == nullptr) return 0;
    return item->textStart + item->layout.positionAt(local, item->codeScroll.offsets);
}

void MarkdownView::applySelectionToItems() {
    const int from = qMin(runtime_->anchor, runtime_->cursor);
    const int to = qMax(runtime_->anchor, runtime_->cursor);
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget != nullptr) continue;
        const int start = item->textStart;
        const int end = start + item->textLength();
        if (to <= start || from >= end) {
            item->layout.clearSelection();
            continue;
        }
        item->layout.setSelection(qMax(from, start) - start, qMin(to, end) - start);
    }
}

void MarkdownView::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mousePressEvent(event);
        return;
    }
    runtime_->pressPosition = event->pos();
    QPointF local;
    const Item* hit = itemAt(event->pos(), &local);
    Item* codeItem = hit == nullptr ? nullptr : runtime_->find(hit->id);
    if (codeItem != nullptr && codeItem->codeScroll.press(codeItem->layout, local)) {
        runtime_->draggingCodeItem = codeItem->id;
        runtime_->selecting = false;
        viewport()->update();
        event->accept();
        return;
    }
    runtime_->selectionReorderPending = false;
    runtime_->anchor = positionAt(event->pos());
    runtime_->cursor = runtime_->anchor;
    runtime_->selecting = true;
    applySelectionToItems();
    viewport()->update();
}

void MarkdownView::mouseMoveEvent(QMouseEvent* event) {
    if (!runtime_->draggingCodeItem.isEmpty()) {
        Item* item = runtime_->find(runtime_->draggingCodeItem);
        if (item != nullptr) {
            const QPointF local = QPointF(event->pos()) + QPointF(0, verticalScrollBar()->value()) - item->origin;
            item->codeScroll.move(item->layout, local);
        }
        viewport()->update();
        event->accept();
        return;
    }
    if (runtime_->selecting) {
        runtime_->cursor = positionAt(event->pos());
        applySelectionToItems();
        viewport()->update();
        return;
    }
    QPointF local;
    const Item* item = itemAt(event->pos(), &local);
    const bool onLink = item != nullptr && !item->layout.linkAt(local).isEmpty();
    viewport()->setCursor(onLink ? Qt::PointingHandCursor : Qt::IBeamCursor);
}

void MarkdownView::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseReleaseEvent(event);
        return;
    }
    if (!runtime_->draggingCodeItem.isEmpty()) {
        if (Item* item = runtime_->find(runtime_->draggingCodeItem)) item->codeScroll.release();
        runtime_->draggingCodeItem.clear();
        event->accept();
        return;
    }
    // A press may belong to a detached page. Only a matching live press in
    // this page can finish a text drag or activate its link.
    if (!runtime_->selecting) return;
    runtime_->selecting = false;
    // 拖过一段距离的是在选文字，不是在点链接。
    const int moved = (event->pos() - runtime_->pressPosition).manhattanLength();
    if (moved > QApplication::startDragDistance()) return;

    QPointF local;
    const Item* item = itemAt(event->pos(), &local);
    if (item == nullptr) return;
    const QString href = item->layout.linkAt(local);
    if (!href.isEmpty()) emit linkActivated(href);
}

void MarkdownView::wheelEvent(QWheelEvent* event) {
    QPointF local;
    const Item* hit = itemAt(event->position().toPoint(), &local);
    Item* item = hit == nullptr ? nullptr : runtime_->find(hit->id);
    if (item != nullptr && item->codeScroll.wheel(item->layout, local, event->pixelDelta(),
                                                 event->angleDelta(), event->modifiers())) {
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void MarkdownView::keyPressEvent(QKeyEvent* event) {
    if (event->matches(QKeySequence::Copy)) {
        copySelection();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void MarkdownView::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    QAction* copy = menu.addAction(QStringLiteral("复制"));
    copy->setEnabled(!selectedText().isEmpty());
    QAction* all = menu.addAction(QStringLiteral("全选"));

    QPointF local;
    const Item* item = itemAt(event->pos(), &local);
    QAction* copySource = nullptr;
    if (item != nullptr && !item->source.isEmpty()) {
        // 渲染是有损的：标题、代码围栏、链接目标都还原不回来，
        // 所以「复制原始数据」复制的是 Markdown 原文。IM 那边一直有这一条。
        copySource = menu.addAction(QStringLiteral("复制原始数据"));
    }

    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == nullptr) return;
    if (chosen == copy) {
        copySelection();
    } else if (chosen == all) {
        selectAll();
    } else if (chosen == copySource) {
        QApplication::clipboard()->setText(item->source);
    }
}

QString MarkdownView::selectedText() const {
    QString text;
    for (const std::unique_ptr<Item>& item : runtime_->items) {
        if (item->widget != nullptr) continue;
        const QString piece = item->layout.selectedText();
        if (piece.isEmpty()) continue;
        if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n'))) text += QLatin1Char('\n');
        text += piece;
    }
    return text;
}

void MarkdownView::selectAll() {
    runtime_->selectionReorderPending = false;
    int total = 0;
    for (const std::unique_ptr<Item>& item : runtime_->items) total += item->textLength();
    runtime_->anchor = 0;
    runtime_->cursor = total;
    applySelectionToItems();
    viewport()->update();
}

void MarkdownView::clearSelection() {
    runtime_->selectionReorderPending = false;
    runtime_->anchor = 0;
    runtime_->cursor = 0;
    applySelectionToItems();
    viewport()->update();
}

void MarkdownView::copySelection() {
    const QString text = selectedText();
    if (text.isEmpty()) return;
    QApplication::clipboard()->setText(text);
}
