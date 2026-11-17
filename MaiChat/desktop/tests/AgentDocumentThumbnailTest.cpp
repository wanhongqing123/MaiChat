// 自有文档的普通首屏渲染合同；不启动产品窗口、不使用模型、真实用户文件或持久目录。
#include "agent/AgentDocumentThumbnail.h"
#include "agent/AgentDocumentThumbnailView.h"
#include "ui/PdfPreviewDialog.h"

#include <QCoreApplication>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QPageLayout>
#include <QPainter>
#include <QPdfWriter>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <future>

namespace {
int checks = 0;
int failures = 0;
void check(bool value, const char* message) {
    ++checks;
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", message);
    }
}
bool write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
AgentDocumentThumbnailResult render(const AgentDocumentThumbnailRequest& request) {
    auto pending = std::async(std::launch::async, [request] { return prepareAgentDocumentThumbnail(request); });
    return renderAgentDocumentThumbnail(pending.get());
}
int coloredPixels(const QImage& source, int channel) {
    const QImage image = source.convertToFormat(QImage::Format_RGB32);
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int values[] = {qRed(row[x]), qGreen(row[x]), qBlue(row[x])};
            if (values[channel] > 170 && values[(channel + 1) % 3] < 110 && values[(channel + 2) % 3] < 110) ++count;
        }
    }
    return count;
}
int inkPixels(const QImage& source) {
    const QImage image = source.convertToFormat(QImage::Format_RGB32);
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            if (qRed(row[x]) < 160 && qGreen(row[x]) < 160 && qBlue(row[x]) < 160) ++count;
    }
    return count;
}
bool waitUntil(const std::function<bool()>& condition, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    return condition();
}
}  // namespace

int main(int argc, char** argv) {
    const QDir fonts(QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath(QStringLiteral("../resources/fonts")));
    qputenv("QT_QPA_FONTDIR", fonts.absolutePath().toUtf8());
    QApplication app(argc, argv);
    for (const auto& file : fonts.entryList({QStringLiteral("*.ttf")}, QDir::Files))
        check(QFontDatabase::addApplicationFont(fonts.filePath(file)) >= 0, "load product font in offscreen fixture");
    check(QFontDatabase::addApplicationFont(
              fonts.filePath(QStringLiteral("../../../Presentation/fonts/NotoSansSC-Regular.otf"))) >= 0,
          "load product Chinese fallback font");
    QTemporaryDir workspace;
    check(workspace.isValid(), "owned temporary workspace");
    const QString artifacts = app.arguments().size() > 1 ? app.arguments()[1] : QString();
    if (!artifacts.isEmpty()) QDir().mkpath(artifacts);

    AgentDocumentThumbnailRequest markdown;
    markdown.path = workspace.filePath(QStringLiteral("report.md"));
    markdown.allowedRoot = workspace.path();
    markdown.mimeType = QStringLiteral("text/markdown");
    const QByteArray first =
        "# Owned FIRST report\n\n**Actual document content**, with a [reference][later].\n\n"
        "> [!NOTE]\n> First screen keeps its quote styling.\n\n"
        "- [x] Read screen\n- [ ] Verify document\n\n"
        "```cpp\nint owned = 42;\n```\n\n";
    const QByteArray tail(4000, 'x');
    check(write(markdown.path, first + tail + "\n\n[later]: https://example.com\n"), "write Markdown");
    check(agentDocumentThumbnailKind(markdown.path, markdown.mimeType) == AgentDocumentThumbnailKind::Markdown,
          "Markdown recognized");
    auto md = render(markdown);
    check(!md.image.isNull(), "Markdown has real content pixels");
    check(md.image.size() == markdown.logicalSize, "Markdown logical size at DPR1");
    check(inkPixels(md.image) > 500, "Markdown first screen contains text, not a blank icon");
    check(md.canonicalPath == QFileInfo(markdown.path).canonicalFilePath(), "source remains original document");
    check(md.contentDigest.size() == 32, "SHA256 content identity");
    const auto cached = render(markdown);
    check(cached.fromCache && cached.cacheKey == md.cacheKey && cached.image == md.image, "memory cache hit");
    if (!artifacts.isEmpty()) md.image.save(QDir(artifacts).filePath(QStringLiteral("markdown-first.png")));

    QFile timestamp(markdown.path);
    check(timestamp.open(QIODevice::ReadOnly), "open timestamp");
    const QDateTime originalTime = timestamp.fileTime(QFileDevice::FileModificationTime);
    timestamp.close();
    QByteArray changed = first;
    changed.replace("FIRST", "OTHER");
    check(write(markdown.path, changed + tail + "\n\n[later]: https://example.com\n"), "same-size Markdown rewrite");
    check(timestamp.open(QIODevice::ReadWrite), "reopen timestamp");
    check(timestamp.setFileTime(originalTime, QFileDevice::FileModificationTime), "restore mtime");
    timestamp.close();
    const auto updated = render(markdown);
    check(updated.contentDigest != md.contentDigest && updated.cacheKey != md.cacheKey,
          "content change invalidates cache");
    check(updated.image != md.image, "changed first heading changes thumbnail");
    if (!artifacts.isEmpty()) updated.image.save(QDir(artifacts).filePath(QStringLiteral("markdown-changed.png")));
    QByteArray changedTail = tail;
    changedTail.replace(3900, 100, QByteArray(100, 'y'));
    check(write(markdown.path, changed + changedTail + "\n\n[later]: https://example.com\n"),
          "change only document tail");
    const auto tailUpdated = render(markdown);
    check(tailUpdated.contentDigest != updated.contentDigest, "offscreen document change invalidates content identity");
    check(tailUpdated.image == updated.image, "tail below viewport does not shrink or alter the first screen");

    for (int width : {144, 312, 520}) {
        markdown.logicalSize = QSize(width, 192);
        for (qreal dpr : {1.0, 1.5, 2.0}) {
            markdown.devicePixelRatio = dpr;
            const auto sized = render(markdown);
            check(sized.image.size() == QSize(qRound(width * dpr), qRound(192 * dpr)),
                  "physical pixels match width and DPR");
            check(qFuzzyCompare(sized.image.devicePixelRatio(), dpr), "image retains requested DPR");
            check(inkPixels(sized.image) > 250, "resized image still contains actual document");
        }
    }
    markdown.logicalSize = QSize(312, 192);
    markdown.devicePixelRatio = 1;
    const auto fontBase = render(markdown);
    markdown.bodyFamily = QStringLiteral("Consolas");
    const auto otherFont = render(markdown);
    check(otherFont.cacheKey != fontBase.cacheKey, "font is part of image cache identity");
    markdown.bodyFamily = QStringLiteral("Inter");

    AgentDocumentThumbnailRequest pdf;
    pdf.path = workspace.filePath(QStringLiteral("two-pages.pdf"));
    pdf.allowedRoot = workspace.path();
    pdf.mimeType = QStringLiteral("application/pdf");
    {
        QPdfWriter writer(pdf.path);
        writer.setResolution(72);
        writer.setPageSize(QPageSize(QPageSize::A4));
        writer.setPageMargins(QMarginsF(), QPageLayout::Point);
        QPainter painter(&writer);
        painter.fillRect(QRect(30, 30, 450, 210), QColor(220, 20, 20));
        painter.fillRect(QRect(30, 650, 450, 140), QColor(20, 220, 20));
        check(writer.newPage(), "owned PDF second page");
        painter.fillRect(QRect(0, 0, 590, 840), QColor(20, 20, 220));
    }
    const auto page = render(pdf);
    check(!page.image.isNull(), "PDF first page rendered");
    check(page.image.size() == pdf.logicalSize, "PDF crop dimensions");
    check(coloredPixels(page.image, 0) > 15000, "PDF contains real red first-page content");
    check(coloredPixels(page.image, 1) < 50, "PDF shows first viewport, not shrunken whole page");
    check(coloredPixels(page.image, 2) < 50, "PDF never shows second page");
    if (!artifacts.isEmpty()) page.image.save(QDir(artifacts).filePath(QStringLiteral("pdf-first.png")));
    pdf.devicePixelRatio = 2;
    const auto retina = render(pdf);
    check(retina.image.size() == pdf.logicalSize * 2 && qFuzzyCompare(retina.image.devicePixelRatio(), 2.0),
          "PDF DPR2");
#ifdef Q_OS_WIN
    QFile originalPdf(pdf.path);
    check(originalPdf.open(QIODevice::ReadOnly), "read owned immutable PDF snapshot");
    const QByteArray snapshot = originalPdf.readAll();
    originalPdf.close();
    {
        QPdfWriter replacement(pdf.path);
        replacement.setResolution(72);
        replacement.setPageMargins(QMarginsF(), QPageLayout::Point);
        QPainter painter(&replacement);
        painter.fillRect(QRect(0, 0, 590, 840), QColor(20, 20, 220));
    }
    const auto bytesPage =
        std::async(std::launch::async, [snapshot] { return renderWindowsPdfPageBytes(snapshot, 0); }).get();
    check(bytesPage.pageCount == 2 && coloredPixels(bytesPage.image, 0) > 15000,
          "immutable PDF bytes retain original first-page content after path replacement");
    const auto pathPage = std::async(std::launch::async, [pdf] { return renderWindowsPdfPage(pdf.path, 0); }).get();
    check(pathPage.pageCount == 1 && coloredPixels(pathPage.image, 2) > 15000,
          "ordinary full-path renderer still reads current replacement document");
    check(write(pdf.path, snapshot), "restore owned PDF snapshot");
#endif

    auto missing = markdown;
    missing.path = workspace.filePath(QStringLiteral("missing.md"));
    check(render(missing).image.isNull() && !render(missing).error.isEmpty(), "missing document falls back");
    auto bad = pdf;
    bad.path = workspace.filePath(QStringLiteral("bad.pdf"));
    check(write(bad.path, "not a PDF"), "write malformed owned PDF");
    check(render(bad).image.isNull(), "PDF failure does not fabricate content");
    auto invalid = markdown;
    invalid.devicePixelRatio = 1000;
    check(render(invalid).image.isNull(), "unbounded pixel allocation rejected");
    invalid = markdown;
    invalid.path = workspace.filePath(QStringLiteral("archive.zip"));
    invalid.mimeType = QStringLiteral("application/zip");
    check(write(invalid.path, "PK-owned") && render(invalid).image.isNull(), "other files keep ordinary card");
    invalid = markdown;
    invalid.path = workspace.filePath(QStringLiteral("too-large.md"));
    check(write(invalid.path, QByteArray(512 * 1024 + 1, 'a')), "write bounded-input fixture");
    check(render(invalid).image.isNull(), "oversized Markdown falls back before GUI layout");
    invalid.path = workspace.filePath(QStringLiteral("invalid-utf8.md"));
    check(write(invalid.path, QByteArray::fromHex("c080eda080")) && render(invalid).image.isNull(),
          "invalid UTF-8 falls back");
    // This is the complete file, not a stream chunk: an unfinished final code
    // point must fall back instead of silently dropping the remaining bytes.
    for (const auto& suffix : {QByteArray::fromHex("e2"), QByteArray::fromHex("e282"),
                               QByteArray::fromHex("f09f98")}) {
        invalid.path =
            workspace.filePath(QStringLiteral("truncated-utf8-%1.md").arg(QString::fromLatin1(suffix.toHex())));
        const QByteArray completeBody("# Complete visible body\n\nOwned text before the incomplete character.\n");
        check(write(invalid.path, completeBody + suffix),
              "write owned Markdown with incomplete UTF-8 at EOF");
        const auto truncated = render(invalid);
        check(truncated.image.isNull() && !truncated.error.isEmpty(),
              (QByteArray("incomplete UTF-8 EOF falls back: ") + suffix.toHex()).constData());
    }
    auto completeUtf8 = markdown;
    completeUtf8.path = workspace.filePath(QStringLiteral("complete-utf8.md"));
    const QByteArray completeText = QByteArray("# Complete UTF-8 document\n\nOwned text and complete Unicode: ") +
                                    QByteArray::fromHex("e282acf09f9880") + "\n";
    check(write(completeUtf8.path, completeText), "write complete BMP and supplementary UTF-8 fixture");
    const auto completeUnicode = render(completeUtf8);
    check(!completeUnicode.image.isNull() && completeUnicode.error.isEmpty(),
          "complete BMP and supplementary UTF-8 still render");
    completeUtf8.path = workspace.filePath(QStringLiteral("complete-utf8-bom.md"));
    check(write(completeUtf8.path, QByteArray::fromHex("efbbbf") + completeText), "write complete UTF-8 BOM fixture");
    const auto completeBom = render(completeUtf8);
    check(!completeBom.image.isNull() && completeBom.error.isEmpty() && completeBom.image == completeUnicode.image,
          "UTF-8 BOM preserves the complete document thumbnail");
    const QString differentRoot = workspace.filePath(QStringLiteral("restricted"));
    check(QDir().mkpath(differentRoot), "create owned restricted root");
    auto escaped = markdown;
    escaped.allowedRoot = differentRoot;
    check(render(escaped).image.isNull(), "each background read rejects a resolved source outside its workspace");
    auto table = markdown;
    table.path = workspace.filePath(QStringLiteral("long-table.md"));
    QByteArray tableText = "| A | B |\n|---|---|\n";
    for (int row = 0; row < 200; ++row)
        tableText += row == 160 ? "| a very wide late cell changes the real table geometry | b |\n" : "| a | b |\n";
    check(write(table.path, tableText), "write table whose late row affects first-screen columns");
    check(render(table).image.isNull(),
          "truncated table geometry falls back rather than fabricating first-screen columns");

    // Observe the public prepared snapshot before GUI delivery, retaining only
    // a weak reference. A finished binding must release it on every path: the
    // snapshot can own a full PDF page, outside the final-image cache budget.
    const auto verifyPreparedLifetime = [&](const QString& name, bool clearBeforeDelivery, bool invalidPdf) {
        auto lifetimeRequest = pdf;
        lifetimeRequest.path = workspace.filePath(QStringLiteral("lifetime-%1.pdf").arg(name));
        check(invalidPdf ? write(lifetimeRequest.path, "not a PDF") : QFile::copy(pdf.path, lifetimeRequest.path),
              "create distinct owned PDF lifetime fixture");
        int completions = 0;
        int finishedSignals = 0;
        bool imageDelivered = false;
        bool errorDelivered = false;
        AgentDocumentThumbnailBinding lifetimeBinding(&app, [&](const auto& result) {
            ++completions;
            imageDelivered = !result.image.isNull();
            errorDelivered = !result.error.isEmpty();
        });
        lifetimeBinding.request(lifetimeRequest);
        const auto watchers = lifetimeBinding.findChildren<QFutureWatcherBase*>(QString(), Qt::FindDirectChildrenOnly);
        check(watchers.size() == 1, "lifetime fixture has one public future watcher");
        if (watchers.size() != 1) return;
        auto* future = dynamic_cast<QFutureWatcher<AgentDocumentThumbnailPrepared>*>(watchers.first());
        check(future != nullptr, "lifetime fixture exposes the prepared result type");
        if (!future) return;
        QObject::connect(future, &QFutureWatcherBase::finished, &lifetimeBinding, [&] { ++finishedSignals; });
        // Do not pump GUI events yet: capture the completed worker snapshot
        // before the binding's queued finished callback can consume it.
        QElapsedTimer workerWait;
        workerWait.start();
        while (!future->isFinished() && workerWait.elapsed() < 10000) QThread::msleep(2);
        check(future->isFinished(), "owned PDF worker finishes before GUI delivery");
        if (!future->isFinished()) return;
        std::weak_ptr<const AgentDocumentThumbnailPrepared::Data> snapshot;
        {
            const auto prepared = future->future().result();
            snapshot = prepared.data;
        }
        check(!snapshot.expired(), "worker snapshot is alive before GUI delivery");
        if (clearBeforeDelivery) lifetimeBinding.clear();
        check(waitUntil([&] { return finishedSignals == 1; }), "owned PDF finished callback is delivered");
        const QByteArray description = QByteArray("finished binding releases prepared snapshot: ") + name.toUtf8();
        check(snapshot.expired(), description.constData());
        check(completions == (clearBeforeDelivery ? 0 : 1), "clear suppresses only its owned stale completion");
        if (!clearBeforeDelivery)
            check(imageDelivered == !invalidPdf && errorDelivered == invalidPdf,
                  "lifetime fixture preserves successful pixels and failure fallback");
    };
    verifyPreparedLifetime(QStringLiteral("normal"), false, false);
    verifyPreparedLifetime(QStringLiteral("cleared"), true, false);
    verifyPreparedLifetime(QStringLiteral("failed"), false, true);

    // A queued completion from the old document may not paint into a rebound view.
    QVector<AgentDocumentThumbnailResult> delivered;
    AgentDocumentThumbnailBinding binding(&app, [&](const auto& result) { delivered.push_back(result); });
    binding.request(pdf);
    binding.request(markdown);
    check(waitUntil([&] { return !delivered.isEmpty(); }), "async binding delivers latest document");
    QCoreApplication::processEvents();
    check(delivered.size() == 1 && delivered.first().canonicalPath == QFileInfo(markdown.path).canonicalFilePath(),
          "stale document completion rejected");
    delivered.clear();
    binding.request(pdf);
    binding.clear();
    QElapsedTimer cancellation;
    cancellation.start();
    while (cancellation.elapsed() < 150) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    check(delivered.isEmpty(), "cleared binding never publishes old result");
    binding.request(markdown);
    check(waitUntil([&] { return !delivered.isEmpty(); }), "watch own document");
    const auto watched = delivered.last();
    QSaveFile replacement(markdown.path);
    check(replacement.open(QIODevice::WriteOnly) &&
              replacement.write("# Replaced owned document\n\nDifferent first screen.\n") > 0 && replacement.commit(),
          "atomically replace owned document");
    check(waitUntil([&] { return !delivered.isEmpty() && delivered.last().contentDigest != watched.contentDigest; }),
          "file replacement refreshes memory cache");
    check(!delivered.last().image.isNull() && delivered.last().image != watched.image,
          "file watcher publishes new document pixels");
    binding.clear();
    int lateAfterDestruction = 0;
    {
        AgentDocumentThumbnailBinding disposable(&app, [&](const auto&) { ++lateAfterDestruction; });
        disposable.request(pdf);
    }
    cancellation.restart();
    while (cancellation.elapsed() < 100) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    check(lateAfterDestruction == 0, "destroyed card binding never publishes late completion");
    QVector<AgentDocumentThumbnailResult> floodResults;
    AgentDocumentThumbnailBinding flood(&app, [&](const auto& result) { floodResults.push_back(result); });
    for (int i = 0; i < 500; ++i) flood.request(markdown);
    check(flood.findChildren<QFutureWatcherBase*>(QString(), Qt::FindDirectChildrenOnly).size() <= 1,
          "one in-flight watcher per binding under request flood");
    check(waitUntil([&] { return !floodResults.isEmpty(); }), "request flood delivers coalesced latest request");
    check(floodResults.size() == 1, "request flood only publishes current binding");
    flood.clear();
    bool mayRead = false;
    int accessChecks = 0;
    QVector<AgentDocumentThumbnailResult> guardedResults;
    AgentDocumentThumbnailBinding guarded(
        &app, [&](const auto& result) { guardedResults.push_back(result); },
        [&] {
            ++accessChecks;
            return mayRead;
        });
    guarded.request(markdown);
    check(waitUntil([&] { return !guardedResults.isEmpty(); }), "rejected source reports fallback");
    check(accessChecks > 0 && guardedResults.last().image.isNull(), "source ownership checked before read");
    mayRead = true;
    guarded.request(markdown);
    mayRead = false;
    check(waitUntil([&] { return guardedResults.size() >= 2; }), "owner invalidation reports fallback");
    check(guardedResults.last().image.isNull(), "owner checked again before publishing pixels");
    AgentDocumentThumbnailView view(nullptr, [] { return true; });
    view.resize(180, view.heightForWidth(180));
    view.setDocument(markdown);
    view.show();  // Offscreen QPA only: no OS window or foreground switch.
    check(waitUntil([&] { return !view.thumbnail().isNull(); }), "content view displays real pixels");
    check(view.sourcePath() == markdown.path, "content view retains full original source path");
    check(view.thumbnail().width() == qRound(view.width() * view.devicePixelRatioF()),
          "view width maps to actual DPR pixels");
    view.resize(290, view.heightForWidth(290));
    check(waitUntil([&] { return view.thumbnail().width() == qRound(290 * view.devicePixelRatioF()); }),
          "content view rebinds after width change");
    view.clear();
    check(view.thumbnail().isNull() && view.sourcePath().isEmpty(), "cleared view never retains prior content");
    std::fprintf(stderr, "Agent document thumbnail: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
