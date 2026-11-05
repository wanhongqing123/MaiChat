// 文件作用：验证桌面端Markdown 选择、链接、滚动定位和内容更新。
// 运行边界：由测试进程驱动实际组件或替身，不作为正式 App 的用户入口。
#include <QLabel>
#include <QClipboard>
#include <QLineEdit>
#include <QPointer>
#include <QScrollBar>
#include <QSignalSpy>
#include <QWindow>
#include <QtTest>
#include <QtMath>

#include "markdown/MarkdownView.h"
#include "markdown/MarkdownLayout.h"

// 消息展示区的测试。
//
// 这里守的是**换掉「一条消息一个部件」之后新拿到的那些能力**：跨消息选区、
// 短气泡按内容收窄、贴底跟随、嵌进来的部件跟着滚。这几条都是回归起来最不容易
// 被发现的——看一眼界面「好像没问题」，实际已经坏了。

namespace {

MarkdownTheme testTheme() {
    return MarkdownTheme::standard(1.0);
}

}  // namespace

class MarkdownViewTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void selectsAcrossMessages();
    void shortBubbleIsNarrowerThanLongOne();
    void followsBottomUntilTheUserScrollsUp();
    void embeddedWidgetMovesWithTheContent();
    void updatingOneItemMovesTheOnesAfterIt();
    void emitsLinkActivatedOnClick();
    void clearDropsEverything();
    void readingColumnStaysCenteredAndCapped();
    void embeddedWidgetGrowingPushesTheRestDown();
    void batchedHistoryMatchesImmediateLayout();
    void movingExistingItemsPreservesWidgetsSelectionAndScroll();
    void movingItemsSupportsBatchingAndRejectsMissingIds();
    void draggingSecondTableCellCopiesVisibleText();
    void scrollingCodeKeepsSelectionAndCopiesVisibleTail();
    void streamingEarlierItemsKeepsLaterSelection_data();
    void streamingEarlierItemsKeepsLaterSelection();

private:
    void prepare(MarkdownView& view);
};

void MarkdownViewTest::initTestCase() {
    // 字体量不出来的话所有几何断言都没意义，先确认平台插件带字体。
    QVERIFY(!QGuiApplication::platformName().isEmpty());
}

void MarkdownViewTest::prepare(MarkdownView& view) {
    view.setTheme(testTheme());
    view.resize(520, 260);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));
}

void MarkdownViewTest::selectsAcrossMessages() {
    // 这是这次重写最主要的收益。原来每条消息各是一个 QTextBrowser，
    // 选区过不了界，想连着复制两条只能分两次。
    MarkdownView view;
    prepare(view);
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Bubble, QStringLiteral("第一条"));
    view.addItem(QStringLiteral("b"), MarkdownView::Style::Document, QStringLiteral("第二条"));
    view.addItem(QStringLiteral("c"), MarkdownView::Style::Document, QStringLiteral("第三条"));

    view.selectAll();
    const QString selected = view.selectedText();
    QVERIFY(selected.contains(QStringLiteral("第一条")));
    QVERIFY(selected.contains(QStringLiteral("第二条")));
    QVERIFY(selected.contains(QStringLiteral("第三条")));
    // 消息之间要断开，复制出来不能糊成一行。
    QVERIFY(selected.contains(QLatin1Char('\n')));

    view.clearSelection();
    QVERIFY(view.selectedText().isEmpty());
}

void MarkdownViewTest::streamingEarlierItemsKeepsLaterSelection_data() {
    QTest::addColumn<bool>("upsert");
    QTest::addColumn<bool>("batched");
    QTest::newRow("update") << false << false;
    QTest::newRow("upsert-existing") << true << false;
    QTest::newRow("batched-update") << false << true;
    QTest::newRow("batched-upsert-existing") << true << true;
}

void MarkdownViewTest::streamingEarlierItemsKeepsLaterSelection() {
    QFETCH(bool, upsert);
    QFETCH(bool, batched);
    MarkdownView view;
    prepare(view);
    const QString firstId = QStringLiteral("streaming-assistant");
    const QString selectedId = QStringLiteral("submitted-user");
    const QString original = QStringLiteral("Earlier assistant");
    const QString later = QStringLiteral("Later selected text");
    view.addItem(firstId, MarkdownView::Style::Document, original);
    view.addItem(selectedId, MarkdownView::Style::Document, later);
    auto* editor = new QLineEdit(QStringLiteral("Retained widget draft"));
    editor->setFixedHeight(30);
    view.addWidget(QStringLiteral("quote-footer"), editor);
    QPointer<QLineEdit> retained = editor;
    QSignalSpy destroyed(editor, &QObject::destroyed);

    MarkdownLayout reference;
    reference.layout(MarkdownDocument::parse(later), testTheme(), view.itemRect(selectedId).width());
    const QString expected = QStringLiteral("selected");
    const int start = reference.selectableText().indexOf(expected);
    QVERIFY(start >= 0);
    const auto pointerForPosition = [&](int position) {
        for (int y = 0; y <= qCeil(reference.height()); ++y)
            for (int x = 0; x <= qCeil(reference.width()); ++x)
                if (reference.positionAt(QPointF(x, y)) == position) return QPoint(x, y);
        return QPoint(-1, -1);
    };
    const QPoint first = pointerForPosition(start), last = pointerForPosition(start + expected.size());
    QVERIFY(first.x() >= 0 && last.x() >= 0);
    auto* window = view.windowHandle();
    QVERIFY(window);
    const auto windowPoint = [&](QPoint local) {
        const QPoint viewportPoint = (QPointF(local) + view.itemRect(selectedId).topLeft()
            - QPointF(0, view.verticalScrollBar()->value())).toPoint();
        return window->mapFromGlobal(view.viewport()->mapToGlobal(viewportPoint));
    };
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, windowPoint(first));
    QTest::mouseMove(window, windowPoint(last));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, windowPoint(last));
    QCOMPARE(view.selectedText(), expected);

    // Busy input can already be below a still-streaming assistant. Growing an
    // earlier item changes the later item's global textStart, not its selection.
    const auto append = [&](const QString& source) {
        if (upsert) view.addItem(firstId, MarkdownView::Style::Document, source);
        else view.updateItem(firstId, source);
    };
    if (batched) view.beginBatchUpdates();
    append(original + QStringLiteral(" appended context").repeated(12));
    if (batched) {
        append(original + QStringLiteral(" appended context").repeated(16));
        view.endBatchUpdates();
    }
    QCOMPARE(view.selectedText(), expected);
    QTest::keySequence(&view, QKeySequence(QKeySequence::Copy));
    QCOMPARE(QApplication::clipboard()->text(), expected);
    QVERIFY(retained && retained == editor);
    QCOMPARE(destroyed.count(), 0);
    QCOMPARE(editor->text(), QStringLiteral("Retained widget draft"));
    QCOMPARE(editor->parentWidget(), view.viewport());
}

void MarkdownViewTest::scrollingCodeKeepsSelectionAndCopiesVisibleTail() {
    MarkdownView view;
    prepare(view);
    const QString source = QStringLiteral("Body before\n\n```text\nHEAD_")
        + QStringLiteral("0123456789").repeated(16) + QStringLiteral("_TAIL_MARKER\n```\n\nBody after");
    view.addItem(QStringLiteral("code"), MarkdownView::Style::Document, source);
    MarkdownLayout reference;
    reference.layout(MarkdownDocument::parse(source), testTheme(), view.itemRect(QStringLiteral("code")).width());
    QCOMPARE(reference.codeBlocks().size(), 1);
    const auto code = reference.codeBlocks().first();
    QVERIFY(code.maximumOffset() > 0);
    const auto viewportPoint = [&](QPointF local) {
        return (local + view.itemRect(QStringLiteral("code")).topLeft()
            - QPointF(0, view.verticalScrollBar()->value())).toPoint();
    };
    auto* window = view.windowHandle();
    QVERIFY(window);
    const auto windowPoint = [&](QPointF local) {
        return window->mapFromGlobal(view.viewport()->mapToGlobal(viewportPoint(local)));
    };
    view.selectAll();
    const QString all = view.selectedText();
    const QPointF track(qFloor(code.scrollTrack().right()), code.scrollTrack().center().y());
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, windowPoint(track));
    QCOMPARE(view.selectedText(), all);
    const QHash<int, qreal> offsets{{0, code.maximumOffset()}};
    const int start = reference.selectableText().indexOf(QStringLiteral("TAIL_MARKER"));
    const auto pointFor = [&](int position) {
        for (int y = qCeil(code.viewport.top()); y <= qFloor(code.viewport.bottom()); ++y)
            for (int x = qCeil(code.viewport.left()); x <= qFloor(code.viewport.right()); ++x)
                if (reference.positionAt(QPointF(x, y), offsets) == position) return QPoint(x, y);
        return QPoint(-1, -1);
    };
    const QPoint from = pointFor(start), to = pointFor(start + 11);
    QVERIFY(from.x() >= 0 && to.x() >= 0);
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, windowPoint(from));
    QTest::mouseMove(window, windowPoint(to));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, windowPoint(to));
    QCOMPARE(view.selectedText(), QStringLiteral("TAIL_MARKER"));
    QTest::keySequence(&view, QKeySequence(QKeySequence::Copy));
    QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("TAIL_MARKER"));
    view.updateItem(QStringLiteral("code"), source + QStringLiteral("\n\nStreaming body"));
    QCOMPARE(view.selectedText(), QStringLiteral("TAIL_MARKER"));
    QVERIFY(view.horizontalScrollBar()->maximum() == 0); // Only code moves, never the message column.
}

void MarkdownViewTest::movingExistingItemsPreservesWidgetsSelectionAndScroll() {
    MarkdownView view;
    prepare(view);
    view.addItem(QStringLiteral("start"), MarkdownView::Style::Document, QStringLiteral("Beginning"));
    auto* editor = new QLineEdit(QStringLiteral("Unsent widget draft"));
    editor->setFixedHeight(36);
    view.addWidget(QStringLiteral("footer"), editor);
    QPointer<QLineEdit> kept = editor;
    QSignalSpy destroyed(editor, &QObject::destroyed);
    for (int index = 0; index < 20; ++index)
        view.addItem(QStringLiteral("body-%1").arg(index), MarkdownView::Style::Document,
                     QStringLiteral("Body paragraph %1").arg(index));
    editor->setSelection(7, 6);
    view.selectAll();
    const QString selected = view.selectedText();
    view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum() / 2);
    const int offset = view.verticalScrollBar()->value();
    QVERIFY(!view.isAtBottom());

    QVERIFY(view.moveItemAfter(QStringLiteral("footer"), QStringLiteral("body-19")));
    QVERIFY(kept && kept == editor);
    QCOMPARE(destroyed.count(), 0);
    QCOMPARE(editor->parentWidget(), view.viewport());
    QCOMPARE(editor->text(), QStringLiteral("Unsent widget draft"));
    QCOMPARE(editor->selectedText(), QStringLiteral("widget"));
    QCOMPARE(view.selectedText(), selected);
    QCOMPARE(view.verticalScrollBar()->value(), offset);
    QVERIFY(!view.isAtBottom());
    QVERIFY(view.itemRect(QStringLiteral("footer")).top() > view.itemRect(QStringLiteral("body-19")).bottom());
    QCOMPARE(view.itemCount(), 22);

    view.scrollToBottom();
    QVERIFY(view.moveItemAfter(QStringLiteral("footer"), QStringLiteral("body-18")));
    QVERIFY(view.isAtBottom());
    QVERIFY(view.itemRect(QStringLiteral("footer")).bottom() < view.itemRect(QStringLiteral("body-19")).top());
}

void MarkdownViewTest::movingItemsSupportsBatchingAndRejectsMissingIds() {
    MarkdownView view;
    prepare(view);
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Document, QStringLiteral("Alpha"));
    view.addItem(QStringLiteral("b"), MarkdownView::Style::Document, QStringLiteral("Bravo"));
    view.addItem(QStringLiteral("c"), MarkdownView::Style::Document, QStringLiteral("Charlie"));
    const QRectF before = view.itemRect(QStringLiteral("b"));
    QVERIFY(!view.moveItemAfter(QStringLiteral("missing"), QStringLiteral("a")));
    QVERIFY(!view.moveItemAfter(QStringLiteral("b"), QStringLiteral("missing")));
    QVERIFY(view.moveItemAfter(QStringLiteral("b"), QStringLiteral("b")));
    QVERIFY(view.moveItemAfter(QStringLiteral("b"), QStringLiteral("a")));
    QCOMPARE(view.itemRect(QStringLiteral("b")), before);

    view.selectAll();
    view.beginBatchUpdates();
    QVERIFY(view.moveItemAfter(QStringLiteral("a"), QStringLiteral("c")));
    QCOMPARE(view.itemRect(QStringLiteral("b")), before);
    view.endBatchUpdates();
    QVERIFY(view.itemRect(QStringLiteral("b")).top() < view.itemRect(QStringLiteral("c")).top());
    QVERIFY(view.itemRect(QStringLiteral("c")).top() < view.itemRect(QStringLiteral("a")).top());
    const QString selected = view.selectedText();
    QVERIFY(selected.contains(QStringLiteral("Alpha")) && selected.contains(QStringLiteral("Bravo")) &&
            selected.contains(QStringLiteral("Charlie")));
    QVERIFY(selected.indexOf(QStringLiteral("Bravo")) < selected.indexOf(QStringLiteral("Charlie")));
    QVERIFY(selected.indexOf(QStringLiteral("Charlie")) < selected.indexOf(QStringLiteral("Alpha")));
}

void MarkdownViewTest::draggingSecondTableCellCopiesVisibleText() {
    MarkdownView view;
    prepare(view);
    view.resize(1220, 260);
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    const QString markdown = QStringLiteral(
        "| 尝试 | 结果 |\n| --- | --- |\n"
        "| PowerShell + `System.Drawing` | 失败：PowerShell处于受限语言模式，`Add-Type`被拒绝 |\n");
    const QString expected = QStringLiteral("失败：PowerShell处于受限语言模式，Add-Type被拒绝");
    view.addItem(QStringLiteral("table"), MarkdownView::Style::Document, markdown);
    const QRectF frame = view.itemRect(QStringLiteral("table"));
    MarkdownLayout reference;
    reference.layout(MarkdownDocument::parse(markdown), testTheme(), frame.width());
    const int start = reference.selectableText().indexOf(expected);
    QVERIFY(start >= 0);
    const auto pointerForPosition = [&](int position) {
        for (int y = 0; y <= qCeil(reference.height()); ++y)
            for (int x = 0; x <= qFloor(frame.width()); ++x)
                if (reference.positionAt(QPointF(x, y)) == position)
                    return QPoint(qRound(frame.left()) + x,
                                  qRound(frame.top()) + y - view.verticalScrollBar()->value());
        return QPoint(-1, -1);
    };
    const QPoint first = pointerForPosition(start);
    const QPoint last = pointerForPosition(start + expected.size());
    QVERIFY(first.x() >= 0 && last.x() >= 0);
    for (bool reverse : {false, true}) {
        // Keep press/move/release in the same QPA event stream. The QWidget
        // mouseMove overload merely moves the OS cursor on Qt 5 and is not a
        // reliable drag when another owned fixture is in the foreground.
        auto* window = view.windowHandle();
        QVERIFY(window);
        const QPoint from = reverse ? last : first;
        const QPoint to = reverse ? first : last;
        const QPoint windowFrom = window->mapFromGlobal(view.viewport()->mapToGlobal(from));
        const QPoint windowTo = window->mapFromGlobal(view.viewport()->mapToGlobal(to));
        qInfo() << "table-view drag" << view.viewport()->size() << frame << from << to
                << windowFrom << windowTo;
        QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier, windowFrom);
        QTest::mouseMove(window, windowTo);
        QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier, windowTo);
        QCOMPARE(view.selectedText(), expected);
        QTest::keySequence(&view, QKeySequence(QKeySequence::Copy));
        QCOMPARE(QApplication::clipboard()->text(), expected);
    }
}

void MarkdownViewTest::shortBubbleIsNarrowerThanLongOne() {
    // 气泡按内容收窄。「好」和一整段话占一样宽的话，界面会很空。
    MarkdownView view;
    prepare(view);
    view.addItem(QStringLiteral("short"), MarkdownView::Style::Bubble, QStringLiteral("好"));
    view.addItem(QStringLiteral("long"), MarkdownView::Style::Bubble,
                 QStringLiteral("这是一条长得多的消息，长到需要占满允许的最大宽度还要折行"));

    const QRectF shortRect = view.itemRect(QStringLiteral("short"));
    const QRectF longRect = view.itemRect(QStringLiteral("long"));
    QVERIFY(shortRect.width() > 0);
    QVERIFY(shortRect.width() < longRect.width());

    // 两个都贴右边。气泡是「我发的」，靠右是这个语义的全部表达。
    QVERIFY(qAbs(shortRect.right() - longRect.right()) < 1.5);

    // 再长也不能占满整行。
    QVERIFY(longRect.width() < view.viewport()->width());
}

void MarkdownViewTest::followsBottomUntilTheUserScrollsUp() {
    MarkdownView view;
    prepare(view);
    for (int index = 0; index < 20; ++index) {
        view.addItem(QStringLiteral("m%1").arg(index), MarkdownView::Style::Document,
                     QStringLiteral("第 %1 条消息").arg(index));
    }
    QVERIFY(view.contentHeight() > view.viewport()->height());
    // 内容在长的时候默认贴底，新消息才看得见。
    QVERIFY(view.isAtBottom());

    // 用户往上翻之后就不许再被拽回去——正在看历史时被拉到底是很烦的。
    view.verticalScrollBar()->setValue(0);
    QVERIFY(!view.isAtBottom());
    view.addItem(QStringLiteral("new"), MarkdownView::Style::Document, QStringLiteral("又一条"));
    QVERIFY(!view.isAtBottom());

    view.scrollToBottom();
    QVERIFY(view.isAtBottom());
}

void MarkdownViewTest::embeddedWidgetMovesWithTheContent() {
    // 工具卡那种带按钮的东西画不出来，得真嵌一个部件进去。
    // 视图要负责摆位置、跟着滚、滚出去藏起来。
    MarkdownView view;
    prepare(view);
    for (int index = 0; index < 12; ++index) {
        view.addItem(QStringLiteral("m%1").arg(index), MarkdownView::Style::Document,
                     QStringLiteral("第 %1 条消息").arg(index));
    }
    auto* card = new QLabel(QStringLiteral("工具卡"));
    card->setFixedHeight(40);
    view.addWidget(QStringLiteral("card"), card);

    QCOMPARE(card->parentWidget(), view.viewport());
    view.scrollToBottom();
    QVERIFY(card->isVisible());

    // 位置必须真的按内容坐标减滚动量算出来，不能只是「碰巧可见」。
    const QRectF slot = view.itemRect(QStringLiteral("card"));
    QVERIFY(slot.height() > 0);
    const int offset = view.verticalScrollBar()->value();
    QCOMPARE(card->y(), qRound(slot.top()) - offset);
    QCOMPARE(card->height(), qRound(slot.height()));

    // 滚到顶上之后卡片在很远的下面，应该被藏起来而不是糊在别的内容上。
    view.verticalScrollBar()->setValue(0);
    QVERIFY(slot.top() > view.viewport()->height());
    QVERIFY(!card->isVisible());

    // 再滚回去还得回到原位。
    view.scrollToBottom();
    QVERIFY(card->isVisible());
    QCOMPARE(card->y(), qRound(slot.top()) - view.verticalScrollBar()->value());
}

void MarkdownViewTest::updatingOneItemMovesTheOnesAfterIt() {
    // 流式输出就是这个形状：中间那条一直在长，后面的要跟着往下挪。
    MarkdownView view;
    prepare(view);
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Document, QStringLiteral("开头"));
    view.addItem(QStringLiteral("b"), MarkdownView::Style::Document, QStringLiteral("短"));
    view.addItem(QStringLiteral("c"), MarkdownView::Style::Document, QStringLiteral("结尾"));

    const QRectF beforeA = view.itemRect(QStringLiteral("a"));
    const qreal beforeC = view.itemRect(QStringLiteral("c")).top();

    view.updateItem(QStringLiteral("b"),
                    QStringLiteral("变长了很多很多的一段内容，长到一定会折成好几行，"
                                   "这样后面那条的位置必然要往下挪。再补一句让它更长一点。"));

    // 前面那条不该动。
    QCOMPARE(view.itemRect(QStringLiteral("a")), beforeA);
    // 后面那条要往下挪。
    QVERIFY(view.itemRect(QStringLiteral("c")).top() > beforeC);

    QVERIFY(view.contains(QStringLiteral("b")));
    QCOMPARE(view.itemCount(), 3);
}

void MarkdownViewTest::emitsLinkActivatedOnClick() {
    MarkdownView view;
    prepare(view);
    // **链接放在行尾**，这一条才测得到东西：xToCursor 会把行尾右边的空白
    // 一律映射到最后一个字符上，链接如果在行尾，少了行宽判断的话
    // 整行右边一大片空白都会变成可点的。链接放行首的话这条断言是白写的。
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Document,
                 QStringLiteral("前面是普通文字 [点我](https://example.com)"));

    QSignalSpy spy(&view, &MarkdownView::linkActivated);
    const QRectF rect = view.itemRect(QStringLiteral("a"));
    const int row = static_cast<int>(rect.top()) + 8;

    // 先确认这条内容确实没占满整行，右边真的有空白可点。
    QVERIFY(view.itemRect(QStringLiteral("a")).width() > 200);

    // 行尾右边的空白不是链接。
    const QPoint blank(static_cast<int>(rect.right()) - 6, row);
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), blank);
    QCOMPARE(spy.count(), 0);

    // 链接本身要点得中。文字末尾往左找一点，落在「点我」上。
    bool activated = false;
    for (int x = static_cast<int>(rect.left()) + 40; x < static_cast<int>(rect.right()); x += 4) {
        QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::KeyboardModifiers(), QPoint(x, row));
        if (spy.count() > 0) {
            activated = true;
            break;
        }
    }
    QVERIFY(activated);
    QCOMPARE(spy.first().first().toString(), QStringLiteral("https://example.com"));
}

void MarkdownViewTest::clearDropsEverything() {
    MarkdownView view;
    prepare(view);
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Document, QStringLiteral("内容"));
    view.addWidget(QStringLiteral("card"), new QLabel(QStringLiteral("卡片")));
    QCOMPARE(view.itemCount(), 2);

    view.clear();
    QVERIFY(view.isEmpty());
    QCOMPARE(view.itemCount(), 0);
    QVERIFY(view.selectedText().isEmpty());
    QVERIFY(!view.contains(QStringLiteral("a")));
}

void MarkdownViewTest::readingColumnStaysCenteredAndCapped() {
    // 窗口很宽时正文不能跟着拉宽：一行太长，眼睛回扫会丢行。
    // 超出的部分留白，内容**居中**。
    MarkdownView view;
    prepare(view);
    view.setMaxContentWidth(300);
    view.resize(900, 260);
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    view.addItem(QStringLiteral("a"), MarkdownView::Style::Document,
                 QStringLiteral("一段会被限制在阅读列宽度之内的正文"));

    const QRectF rect = view.itemRect(QStringLiteral("a"));
    QCOMPARE(qRound(rect.width()), 300);
    // 两边留白要一样宽。原来是靠布局权重摆的，权重给错一次列就只剩三分之一宽。
    const qreal leftGap = rect.left();
    const qreal rightGap = view.viewport()->width() - rect.right();
    QVERIFY(qAbs(leftGap - rightGap) < 1.5);

    // 窗口比列还窄时就该占满，不能反过来溢出去。
    view.resize(220, 260);
    QVERIFY(QTest::qWaitForWindowExposed(&view));
    const QRectF narrow = view.itemRect(QStringLiteral("a"));
    QVERIFY(narrow.width() < 300);
    QVERIFY(narrow.right() <= view.viewport()->width());
}

void MarkdownViewTest::embeddedWidgetGrowingPushesTheRestDown() {
    // 工具卡展开、思考条收起之后高度变了，后面所有东西都得往下挪。
    // 不跟的话卡片会盖住下一条消息。
    MarkdownView view;
    prepare(view);
    auto* card = new QLabel(QStringLiteral("工具卡"));
    card->setFixedHeight(30);
    view.addWidget(QStringLiteral("card"), card);
    view.addItem(QStringLiteral("after"), MarkdownView::Style::Document,
                 QStringLiteral("卡片后面的一条"));

    const qreal before = view.itemRect(QStringLiteral("after")).top();
    QCOMPARE(qRound(view.itemRect(QStringLiteral("card")).height()), 30);

    card->setFixedHeight(120);
    QCOMPARE(qRound(view.itemRect(QStringLiteral("card")).height()), 120);
    QCOMPARE(view.itemRect(QStringLiteral("after")).top(), before + 90);
}

void MarkdownViewTest::batchedHistoryMatchesImmediateLayout() {
    MarkdownView immediate;
    MarkdownView batched;
    prepare(immediate);
    prepare(batched);
    batched.beginBatchUpdates();
    for (int index = 0; index < 30; ++index) {
        const QString id = QStringLiteral("message-%1").arg(index);
        const QString body = QStringLiteral("# 标题\n\n正文 %1").arg(index);
        immediate.addItem(id, MarkdownView::Style::Document, body);
        batched.addItem(id, MarkdownView::Style::Document, body);
    }
    auto* immediateCard = new QLabel(QStringLiteral("工具卡"));
    auto* batchedCard = new QLabel(QStringLiteral("工具卡"));
    immediateCard->setFixedHeight(48);
    batchedCard->setFixedHeight(48);
    immediate.addWidget(QStringLiteral("card"), immediateCard);
    batched.addWidget(QStringLiteral("card"), batchedCard);
    batched.endBatchUpdates();

    QCOMPARE(batched.itemCount(), immediate.itemCount());
    QCOMPARE(batched.contentHeight(), immediate.contentHeight());
    QCOMPARE(batched.itemRect(QStringLiteral("message-20")), immediate.itemRect(QStringLiteral("message-20")));
    QCOMPARE(batched.itemRect(QStringLiteral("card")), immediate.itemRect(QStringLiteral("card")));
    batched.scrollToBottom();
    QCOMPARE(batchedCard->y(),
             qRound(batched.itemRect(QStringLiteral("card")).top()) - batched.verticalScrollBar()->value());
}

QTEST_MAIN(MarkdownViewTest)
#include "MarkdownViewTest.moc"
