// Remote tool details use the real panel and an owned synthetic command transport.
// No command is executed: captured reads are answered through receivePayload.
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>

#include "agent/RemoteAgentPanel.h"
#include "agent/DesktopSessionSyncCoordinator.h"
#include "markdown/MarkdownView.h"

namespace {
const QString kRevision(64, QLatin1Char('a'));
const QString kNewRevision(64, QLatin1Char('b'));

QJsonObject owner(const QString& id = QStringLiteral("session-detail"),
                  const QString& login = QStringLiteral("login-detail")) {
    return {{QStringLiteral("id"), id}, {QStringLiteral("device_slot"), QStringLiteral("owned-mac")},
            {QStringLiteral("login_session_id"), login}, {QStringLiteral("online"), true},
            {QStringLiteral("account_id"), QStringLiteral("owned-account")},
            {QStringLiteral("generation"), 7}};
}

QJsonObject part(const QString& revision = kRevision) {
    return {{QStringLiteral("id"), QStringLiteral("part-detail")}, {QStringLiteral("kind"), QStringLiteral("tool")},
            {QStringLiteral("tool"), QStringLiteral("file_grep")}, {QStringLiteral("state"), QStringLiteral("completed")},
            {QStringLiteral("callId"), QStringLiteral("call-detail")},
            {QStringLiteral("detailsAvailable"), true}, {QStringLiteral("detailRevision"), revision}};
}

void snapshot(RemoteAgentPanel& panel, const QJsonObject& current, const QJsonObject& tool = part(),
              const QString& kind = QStringLiteral("history_page")) {
    // The visual fragment must not become the owner ID sent to the read API.
    const QJsonObject message{{QStringLiteral("id"), QStringLiteral("msg-visual-fragment")},
                              {QStringLiteral("source_message_id"), QStringLiteral("msg-source")},
                              {QStringLiteral("role"), QStringLiteral("assistant")},
                              {QStringLiteral("created"), 1000}, {QStringLiteral("parts"), QJsonArray{tool}}};
    panel.receivePayload(current, kind, {{QStringLiteral("ok"), true},
                                        {QStringLiteral("messages"), QJsonArray{message}}});
}

struct Read {
    QJsonObject owner;
    QJsonObject command;
};

struct Fixture {
    QTemporaryDir directory;
    RemoteAgentPanel panel{nullptr, directory.filePath(QStringLiteral("outbox.json"))};
    QJsonObject current = owner();
    QList<Read> reads;
    Fixture() {
        panel.resize(1000, 800);
        panel.setToolDetailScope(QStringLiteral("owned-account"), 7);
        panel.setCommandHandler([this](const QJsonObject& remote, const QJsonObject& command,
                                       RemoteAgentPanel::Completion done) {
            if (command.value(QStringLiteral("action")) == QStringLiteral("get_tool_detail"))
                reads.append({remote, command});
            done({});
        });
        panel.openSession(current);
        snapshot(panel, current);
        panel.show();
        QCoreApplication::processEvents();
    }
    QAbstractButton* groupButton() {
        return panel.findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolGroupDisclosure"));
    }
    QAbstractButton* itemButton() {
        return panel.findChild<QAbstractButton*>(QStringLiteral("remoteAgentToolDisclosure"));
    }
    QLabel* text(const QString& field) {
        for (auto* label : panel.findChildren<QLabel*>(QStringLiteral("remoteAgentToolFieldText")))
            if (label->property("detailField").toString() == field) return label;
        return nullptr;
    }
    QAbstractButton* more(const QString& field) {
        for (auto* button : panel.findChildren<QAbstractButton*>(QStringLiteral("remoteAgentToolFieldMore")))
            if (button->property("detailField").toString() == field) return button;
        return nullptr;
    }
    Read read(const QString& field, int last = 0) const {
        for (int index = reads.size() - 1 - last; index >= 0; --index)
            if (reads[index].command.value(QStringLiteral("field")) == field) return reads[index];
        return {};
    }
};

QJsonObject page(const Read& read, const QString& text, qint64 total = -1) {
    const auto& command = read.command;
    const qint64 offset = command.value(QStringLiteral("offset")).toInteger();
    const qint64 count = text.toUtf8().size();
    if (total < 0) total = offset + count;
    const bool more = offset + count < total;
    return {{QStringLiteral("ok"), true}, {QStringLiteral("requestId"), command.value(QStringLiteral("requestId"))},
            {QStringLiteral("sessionId"), read.owner.value(QStringLiteral("id"))},
            {QStringLiteral("messageId"), command.value(QStringLiteral("messageId"))},
            {QStringLiteral("partId"), command.value(QStringLiteral("partId"))},
            {QStringLiteral("callId"), QStringLiteral("call-detail")},
            {QStringLiteral("tool"), QStringLiteral("file_grep")}, {QStringLiteral("state"), QStringLiteral("completed")},
            {QStringLiteral("field"), command.value(QStringLiteral("field"))},
            {QStringLiteral("detailRevision"), command.value(QStringLiteral("detailRevision"))},
            {QStringLiteral("offset"), offset}, {QStringLiteral("sourceBytesRead"), count},
            {QStringLiteral("totalBytes"), total}, {QStringLiteral("hasMore"), more},
            {QStringLiteral("nextOffset"), more ? offset + count : 0},
            {QStringLiteral("textIsValidUtf8"), true}, {QStringLiteral("text"), text}};
}

void deliver(RemoteAgentPanel& panel, const Read& read, const QJsonObject& result,
             const QJsonObject& remote = {}) {
    const auto id = read.command.value(QStringLiteral("requestId"));
    panel.receivePayload(remote.isEmpty() ? read.owner : remote, QStringLiteral("event"),
                         {{QStringLiteral("type"), QStringLiteral("command_result")},
                          {QStringLiteral("command_id"), id}, {QStringLiteral("request_id"), id},
                          {QStringLiteral("result"), result}});
}
}

class RemoteToolDetailTest final : public QObject {
    Q_OBJECT
private slots:
    void disclosureReadsOnlySelectedToolAndPagesOriginalSource();
    void expandedToolDetailsUseTheReadingColumn();
    void wrongBindingAndWrongPageDoNotConsumeTheCorrectRead();
    void androidCommandOnlyEnvelopeKeepsTheSameRequestBinding();
    void busyRefreshLocksCurrentStateBeforeTheNextSnapshot();
    void sameStateRevisionRefreshRejectsOldPages();
    void historyStartedBeforeLiveUpdateCannotRollToolDetailsBack();
    void coordinatorHistoryGenerationKeepsTheCurrentLiveGuard();
    void accountAndGenerationChangesDiscardOldDetails();
    void transportReplacementDiscardsPendingReads();
    void switchOwnerAndReconnectNeverReplayOldRead();
    void offlineCacheAndLegacyInlineRemainReadable();
    void busyAndInvalidUtf8PagesRemainExplicit();
};

void RemoteToolDetailTest::disclosureReadsOnlySelectedToolAndPagesOriginalSource() {
    Fixture f;
    QCOMPARE(f.reads.size(), 0);
    QVERIFY(f.groupButton());
    QTest::mouseClick(f.groupButton(), Qt::LeftButton);
    QCOMPARE(f.reads.size(), 0);
    QVERIFY2(f.itemButton(), "An individual tool disclosure must expose its own lazy read.");
    QTest::mouseClick(f.itemButton(), Qt::LeftButton);
    QCOMPARE(f.reads.size(), 3);
    for (const Read& read : f.reads) {
        QCOMPARE(read.command.value(QStringLiteral("messageId")).toString(), QStringLiteral("msg-source"));
        QCOMPARE(read.command.value(QStringLiteral("partId")).toString(), QStringLiteral("part-detail"));
        QCOMPARE(read.command.value(QStringLiteral("limit")).toInt(), 4096);
        QCOMPARE(read.command.value(QStringLiteral("offset")).toInt(), 0);
    }
    const QString first(4096, QLatin1Char('x'));
    const QString full = QString(70 * 1024, QLatin1Char('x')) + QStringLiteral("TAIL");
    const Read initial = f.read(QStringLiteral("output"));
    deliver(f.panel, initial, page(initial, first, full.toUtf8().size()));
    QVERIFY(f.text(QStringLiteral("output")));
    QCOMPARE(f.text(QStringLiteral("output"))->text(), first);
    QVERIFY(f.text(QStringLiteral("output"))->textInteractionFlags().testFlag(Qt::TextSelectableByMouse));
    auto* more = f.more(QStringLiteral("output"));
    QVERIFY(more && more->isVisible());
    for (int offset = 4096; offset < full.size(); offset += 4096) {
        more->click();
        const Read next = f.read(QStringLiteral("output"));
        QCOMPARE(next.command.value(QStringLiteral("offset")).toInt(), offset);
        QCOMPARE(next.command.value(QStringLiteral("detailRevision")).toString(), kRevision);
        deliver(f.panel, next, page(next, full.mid(offset, 4096), full.toUtf8().size()));
    }
    QCOMPARE(f.text(QStringLiteral("output"))->text(), full);
    QVERIFY(!f.more(QStringLiteral("output"))->isVisible());
    const auto* view = f.panel.findChild<MarkdownView*>();
    QVERIFY(view);
    QVERIFY(!view->selectedText().contains(QStringLiteral("TAIL")));
    QFile outbox(f.directory.filePath(QStringLiteral("outbox.json")));
    if (outbox.open(QIODevice::ReadOnly)) QVERIFY(!outbox.readAll().contains("get_tool_detail"));
    const QString capture = qEnvironmentVariable("MAICHAT_REMOTE_DETAIL_CAPTURE_DIR");
    if (!capture.isEmpty()) {
        // Use an additional owned record for the visual receipt so a 70 KiB
        // fixture does not obscure the three ordinary first-page sections.
        Fixture visual;
        QVERIFY(visual.groupButton()); QTest::mouseClick(visual.groupButton(), Qt::LeftButton);
        QVERIFY(visual.itemButton()); QTest::mouseClick(visual.itemButton(), Qt::LeftButton);
        const Read input = visual.read(QStringLiteral("input"));
        const Read output = visual.read(QStringLiteral("output"));
        const Read error = visual.read(QStringLiteral("error"));
        deliver(visual.panel, input, page(input, QStringLiteral("{\"pattern\":\"owned proof\",\"path\":\"fixture.txt\"}")));
        deliver(visual.panel, output, page(output, QStringLiteral("fixture.txt:12: owned proof line\nfixture.txt:27: another owned result\nOnly synthetic fixture data is displayed.")));
        deliver(visual.panel, error, page(error, {}));
        QCoreApplication::processEvents();
        QVERIFY(QDir().mkpath(capture));
        QVERIFY(visual.panel.grab().save(QDir(capture).filePath(QStringLiteral("remote-tool-detail-owned.png"))));
    }
}

void RemoteToolDetailTest::expandedToolDetailsUseTheReadingColumn() {
    Fixture f;
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read output = f.read(QStringLiteral("output"));
    deliver(f.panel, output, page(output, QStringLiteral("A readable owned tool result, with sufficient space for ordinary text.")));
    QCoreApplication::processEvents();
    auto* text = f.text(QStringLiteral("output"));
    QVERIFY(text);
    QVERIFY2(text->width() >= f.panel.width() / 2,
             "Expanded tool details must use the reading column, not a narrow metadata-width strip.");
    f.panel.resize(480, 800);
    QCoreApplication::processEvents();
    QVERIFY(text->width() <= f.panel.width());
}

void RemoteToolDetailTest::wrongBindingAndWrongPageDoNotConsumeTheCorrectRead() {
    Fixture f;
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read requested = f.read(QStringLiteral("output"));
    QVERIFY(!requested.command.isEmpty());
    const auto correct = page(requested, QStringLiteral("OWNED_RESULT"));
    const auto requestId = requested.command.value(QStringLiteral("requestId"));
    for (const QString& outer : {QStringLiteral("request_id"), QStringLiteral("command_id")}) {
        QJsonObject envelope{{QStringLiteral("type"), QStringLiteral("command_result")},
                             {QStringLiteral("command_id"), requestId}, {QStringLiteral("request_id"), requestId},
                             {QStringLiteral("result"), correct}};
        envelope.insert(outer, QStringLiteral("wrong"));
        f.panel.receivePayload(requested.owner, QStringLiteral("event"), envelope);
    }
    for (const QString& field : {QStringLiteral("requestId"), QStringLiteral("sessionId"),
                                 QStringLiteral("messageId"), QStringLiteral("partId"),
                                 QStringLiteral("callId"), QStringLiteral("tool"), QStringLiteral("state"),
                                 QStringLiteral("field"), QStringLiteral("detailRevision")}) {
        auto wrong = correct; wrong.insert(field, QStringLiteral("wrong"));
        deliver(f.panel, requested, wrong);
    }
    auto wrong = correct; wrong.insert(QStringLiteral("offset"), 1); deliver(f.panel, requested, wrong);
    for (const QString& field : {QStringLiteral("device_slot"), QStringLiteral("login_session_id")}) {
        auto remote = requested.owner; remote.insert(field, QStringLiteral("wrong"));
        deliver(f.panel, requested, correct, remote);
    }
    QVERIFY(!f.text(QStringLiteral("output"))->text().contains(QStringLiteral("OWNED_RESULT")));
    deliver(f.panel, requested, correct);
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("OWNED_RESULT"));
}

void RemoteToolDetailTest::androidCommandOnlyEnvelopeKeepsTheSameRequestBinding() {
    Fixture f;
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read read = f.read(QStringLiteral("output"));
    const auto correct = page(read, QStringLiteral("ANDROID_OWNER_RESULT"));
    const auto id = read.command.value(QStringLiteral("requestId"));
    // AndroidSessionSyncCoordinator publishes exactly type/command_id/result;
    // Desktop's prompt-only request_id projection does not apply to this read.
    QJsonObject envelope{{QStringLiteral("type"), QStringLiteral("command_result")},
                          {QStringLiteral("command_id"), id}, {QStringLiteral("result"), correct}};
    auto conflict = envelope;
    conflict.insert(QStringLiteral("request_id"), QStringLiteral("different-optional-request"));
    f.panel.receivePayload(read.owner, QStringLiteral("event"), conflict);
    QVERIFY(!f.text(QStringLiteral("output"))->text().contains(QStringLiteral("ANDROID_OWNER_RESULT")));
    f.panel.receivePayload(read.owner, QStringLiteral("event"), envelope);
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("ANDROID_OWNER_RESULT"));
}

void RemoteToolDetailTest::busyRefreshLocksCurrentStateBeforeTheNextSnapshot() {
    Fixture f;
    auto running = part(); running.insert(QStringLiteral("state"), QStringLiteral("running"));
    snapshot(f.panel, f.current, running);
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read original = f.read(QStringLiteral("output"));
    // An ordinary first read still requires the snapshot's Running identity.
    deliver(f.panel, original, page(original, QStringLiteral("UNCONFIRMED_COMPLETED")));
    QVERIFY(!f.text(QStringLiteral("output"))->text().contains(QStringLiteral("UNCONFIRMED_COMPLETED")));
    auto busy = page(original, {});
    busy.insert(QStringLiteral("ok"), false);
    busy.insert(QStringLiteral("errorCode"), QStringLiteral("busy"));
    busy.insert(QStringLiteral("error"), QStringLiteral("detail_changed"));
    deliver(f.panel, original, busy);
    QVERIFY(f.more(QStringLiteral("output"))); f.more(QStringLiteral("output"))->click();
    const Read output = f.read(QStringLiteral("output"));
    auto malformed = page(output, QStringLiteral("MALFORMED_FIRST_PAGE"));
    malformed.insert(QStringLiteral("detailRevision"), QString(64, QLatin1Char('c')));
    malformed.insert(QStringLiteral("state"), QStringLiteral("error"));
    malformed.insert(QStringLiteral("sourceBytesRead"), 4000);
    deliver(f.panel, output, malformed);
    QVERIFY(!f.text(QStringLiteral("output"))->text().contains(QStringLiteral("MALFORMED_FIRST_PAGE")));
    auto completed = page(output, QStringLiteral("COMPLETED_BEFORE_SNAPSHOT"));
    completed.insert(QStringLiteral("detailRevision"), kNewRevision);
    deliver(f.panel, output, completed);
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("COMPLETED_BEFORE_SNAPSHOT"));
    auto* metadata = f.panel.findChild<QLabel*>(QStringLiteral("remoteAgentToolDetail"));
    QVERIFY(metadata && metadata->text().contains(QStringLiteral("已完成")));
    QVERIFY(f.groupButton()->text().contains(QStringLiteral("已完成")));

    const Read input = f.read(QStringLiteral("input"));
    auto wrongState = page(input, QStringLiteral("WRONG_REFRESH_STATE"));
    wrongState.insert(QStringLiteral("detailRevision"), kNewRevision);
    wrongState.insert(QStringLiteral("state"), QStringLiteral("running"));
    deliver(f.panel, input, wrongState);
    QVERIFY(!f.text(QStringLiteral("input"))->text().contains(QStringLiteral("WRONG_REFRESH_STATE")));
    auto wrongRevision = page(input, QStringLiteral("WRONG_REFRESH_REVISION"));
    wrongRevision.insert(QStringLiteral("detailRevision"), kRevision);
    deliver(f.panel, input, wrongRevision);
    QVERIFY(!f.text(QStringLiteral("input"))->text().contains(QStringLiteral("WRONG_REFRESH_REVISION")));
    auto correct = page(input, QStringLiteral("CURRENT_INPUT"));
    correct.insert(QStringLiteral("detailRevision"), kNewRevision);
    deliver(f.panel, input, correct);
    QCOMPARE(f.text(QStringLiteral("input"))->text(), QStringLiteral("CURRENT_INPUT"));
}

void RemoteToolDetailTest::sameStateRevisionRefreshRejectsOldPages() {
    Fixture f;
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read old = f.read(QStringLiteral("output"));
    auto* button = f.itemButton();
    snapshot(f.panel, f.current, part(kNewRevision), QStringLiteral("event"));
    QCOMPARE(f.itemButton(), button);
    QCOMPARE(f.reads.size(), 6);
    const Read current = f.read(QStringLiteral("output"));
    QCOMPARE(current.command.value(QStringLiteral("detailRevision")).toString(), kNewRevision);
    deliver(f.panel, old, page(old, QStringLiteral("OLD_LATE_PAGE")));
    QVERIFY(!f.text(QStringLiteral("output"))->text().contains(QStringLiteral("OLD_LATE_PAGE")));
    deliver(f.panel, current, page(current, QStringLiteral("NEW_SAME_STATE")));
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("NEW_SAME_STATE"));
}

void RemoteToolDetailTest::historyStartedBeforeLiveUpdateCannotRollToolDetailsBack() {
    Fixture f;
    auto running = part(); running.insert(QStringLiteral("state"), QStringLiteral("running"));
    snapshot(f.panel, f.current, running);
    int historyReads = 0;
    f.panel.setHistoryHandler([&](const QJsonObject&, const QString&, int, RemoteAgentPanel::Completion done) {
        ++historyReads;
        done({});
    });
    auto offline = f.current; offline.insert(QStringLiteral("online"), false);
    f.panel.updateSession(offline);
    f.panel.updateSession(f.current);
    QCOMPARE(historyReads, 1);
    // The prior SQL/history request is still in flight when the completed
    // point update arrives. Its later Running snapshot must not retire this.
    snapshot(f.panel, f.current, part(kNewRevision), QStringLiteral("event"));
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read current = f.read(QStringLiteral("output"));
    deliver(f.panel, current, page(current, QStringLiteral("LIVE_COMPLETED_RESULT")));
    const int commands = f.reads.size();
    snapshot(f.panel, f.current, running);
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("LIVE_COMPLETED_RESULT"));
    QCOMPARE(f.reads.size(), commands);
    auto* metadata = f.panel.findChild<QLabel*>(QStringLiteral("remoteAgentToolDetail"));
    QVERIFY(metadata && metadata->text().contains(QStringLiteral("已完成")));

    // Another explicit reconnect starts a fresh request. That newer snapshot
    // remains authoritative; this is not a permanent ban on history pages.
    f.panel.updateSession(offline);
    f.panel.updateSession(f.current);
    QCOMPARE(historyReads, 2);
    const QString thirdRevision(64, QLatin1Char('c'));
    snapshot(f.panel, f.current, part(thirdRevision));
    const Read fresh = f.read(QStringLiteral("output"));
    QCOMPARE(fresh.command.value(QStringLiteral("detailRevision")).toString(), thirdRevision);
    deliver(f.panel, fresh, page(fresh, QStringLiteral("FRESH_RECONNECT_RESULT")));
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("FRESH_RECONNECT_RESULT"));
}

void RemoteToolDetailTest::coordinatorHistoryGenerationKeepsTheCurrentLiveGuard() {
    Fixture f;
    auto running = part(); running.insert(QStringLiteral("state"), QStringLiteral("running"));
    snapshot(f.panel, f.current, running);
    struct HistoryRead {
        QJsonObject request;
        DesktopSessionSyncCoordinator::Completion complete;
    };
    QList<HistoryRead> history;
    QHash<QString, int> delivered;
    QStringList completed;
    DesktopSessionSyncCoordinator coordinator(
        [&](const QString& path, const QByteArray&, const QJsonObject& request,
            DesktopSessionSyncCoordinator::Completion complete) {
            if (path == QStringLiteral("session-sync/history")) history.append({request, std::move(complete)});
            else complete({}, {});
        }, []() -> AgentController* { return nullptr; }, nullptr,
        QStringLiteral("owned-history-viewer"), QStringLiteral("Owned history viewer"));
    coordinator.setPayloadHandler([&](const QJsonObject& remote, const QString& kind, const QJsonObject& payload) {
        if (kind == QStringLiteral("history_page")) ++delivered[remote.value(QStringLiteral("id")).toString()];
        f.panel.receivePayload(remote, kind, payload);
    });
    coordinator.setCatalogHandler([&](const QJsonArray& catalog) {
        for (const auto& value : catalog)
            if (value.toObject().value(QStringLiteral("id")) == f.current.value(QStringLiteral("id")))
                f.panel.updateSession(value.toObject());
    });
    f.panel.setHistoryHandler([&](const QJsonObject& remote, const QString& before, int limit,
                                 RemoteAgentPanel::Completion done) {
        coordinator.requestRemoteHistory(remote, before, limit,
            [&, done](const QJsonObject&, const QString& error) { completed.append(error); done(error); });
    });
    coordinator.acceptCatalog(QJsonArray{f.current});
    auto offline = f.current; offline.insert(QStringLiteral("online"), false);
    coordinator.acceptCatalog(QJsonArray{offline});
    coordinator.acceptCatalog(QJsonArray{f.current});
    QCOMPARE(history.size(), 1); // H1 has not replied.
    coordinator.connectionLost();
    coordinator.acceptCatalog(QJsonArray{f.current});
    QCOMPARE(history.size(), 2); // H2 belongs to the new selection/connection.
    snapshot(f.panel, f.current, part(kNewRevision), QStringLiteral("event"));
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read output = f.read(QStringLiteral("output"));
    deliver(f.panel, output, page(output, QStringLiteral("CURRENT_H2_LIVE_RESULT")));
    const auto historyPage = [](const QJsonObject& tool) {
        return QJsonObject{{QStringLiteral("ok"), true},
            {QStringLiteral("messages"), QJsonArray{QJsonObject{
                {QStringLiteral("id"), QStringLiteral("msg-visual-fragment")},
                {QStringLiteral("source_message_id"), QStringLiteral("msg-source")},
                {QStringLiteral("role"), QStringLiteral("assistant")}, {QStringLiteral("created"), 1000},
                {QStringLiteral("parts"), QJsonArray{tool}}}}}};
    };
    // Preserve actual serial WS response order. H1 arrives first; it must not
    // finish H2's pending read or clear H2's live-part protection before H2 arrives.
    history[0].complete(historyPage(running), {});
    history[1].complete(historyPage(running), {});
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("CURRENT_H2_LIVE_RESULT"));
    QCOMPARE(delivered.value(f.current.value(QStringLiteral("id")).toString()), 1);
    QCOMPARE(completed.size(), 2);
    QVERIFY(completed[0].contains(QStringLiteral("superseded")));
    QVERIFY(completed[1].isEmpty());

    // A newer H3 can update normally, while another room's read is independent.
    auto other = owner(QStringLiteral("other-history-room"));
    coordinator.acceptCatalog(QJsonArray{f.current, other});
    coordinator.requestRemoteHistory(f.current, {}, 10, [](const QJsonObject&, const QString&) {});
    coordinator.requestRemoteHistory(other, {}, 10, [](const QJsonObject&, const QString&) {});
    QCOMPARE(history.size(), 4);
    const QString freshRevision(64, QLatin1Char('c'));
    history[2].complete(historyPage(part(freshRevision)), {});
    history[3].complete({{QStringLiteral("ok"), true}, {QStringLiteral("messages"), QJsonArray{}}}, {});
    QCOMPARE(delivered.value(f.current.value(QStringLiteral("id")).toString()), 2);
    QCOMPARE(delivered.value(other.value(QStringLiteral("id")).toString()), 1);
    const Read fresh = f.read(QStringLiteral("output"));
    QCOMPARE(fresh.command.value(QStringLiteral("detailRevision")).toString(), freshRevision);
    deliver(f.panel, fresh, page(fresh, QStringLiteral("CURRENT_H3_RESULT")));
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("CURRENT_H3_RESULT"));
}

void RemoteToolDetailTest::accountAndGenerationChangesDiscardOldDetails() {
    Fixture f;
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    Read old = f.read(QStringLiteral("output"));
    deliver(f.panel, old, page(old, QStringLiteral("ACCOUNT_A_CACHE")));
    for (const auto& scope : {qMakePair(QStringLiteral("other-account"), quint64(8)),
                              qMakePair(QStringLiteral("other-account"), quint64(9))}) {
        f.panel.setToolDetailScope(scope.first, scope.second);
        snapshot(f.panel, f.current);
        QVERIFY(!f.text(QStringLiteral("output"))->text().contains(QStringLiteral("ACCOUNT_A_CACHE")));
        QVERIFY(f.itemButton()); f.itemButton()->click();
        const Read current = f.read(QStringLiteral("output"));
        QVERIFY(current.command.value(QStringLiteral("requestId")) != old.command.value(QStringLiteral("requestId")));
        deliver(f.panel, old, page(old, QStringLiteral("STALE_SCOPE")));
        QVERIFY(!f.text(QStringLiteral("output"))->text().contains(QStringLiteral("STALE_SCOPE")));
        deliver(f.panel, current, page(current, QStringLiteral("CURRENT_SCOPE")));
        QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("CURRENT_SCOPE"));
        old = current;
    }
}

void RemoteToolDetailTest::transportReplacementDiscardsPendingReads() {
    Fixture f;
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read old = f.read(QStringLiteral("output"));
    const int count = f.reads.size();
    f.panel.setCommandHandler([&](const QJsonObject& current, const QJsonObject& command,
                                  RemoteAgentPanel::Completion done) {
        if (command.value(QStringLiteral("action")) == QStringLiteral("get_tool_detail"))
            f.reads.append({current, command});
        done({});
    });
    snapshot(f.panel, f.current);
    deliver(f.panel, old, page(old, QStringLiteral("OLD_TRANSPORT")));
    QVERIFY(f.itemButton()); f.itemButton()->click();
    QCOMPARE(f.reads.size(), count + 3);
    QVERIFY(!f.text(QStringLiteral("output"))->text().contains(QStringLiteral("OLD_TRANSPORT")));
    const Read current = f.read(QStringLiteral("output"));
    deliver(f.panel, current, page(current, QStringLiteral("NEW_TRANSPORT")));
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("NEW_TRANSPORT"));
}

void RemoteToolDetailTest::switchOwnerAndReconnectNeverReplayOldRead() {
    Fixture f;
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read old = f.read(QStringLiteral("output"));
    f.current = owner(QStringLiteral("session-B"));
    f.panel.openSession(f.current); snapshot(f.panel, f.current);
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read current = f.read(QStringLiteral("output"));
    deliver(f.panel, old, page(old, QStringLiteral("A_MUST_NOT_REACH_B")));
    QVERIFY(!f.text(QStringLiteral("output"))->text().contains(QStringLiteral("A_MUST_NOT_REACH_B")));
    auto replacement = f.current; replacement.insert(QStringLiteral("login_session_id"), QStringLiteral("new-login"));
    f.panel.updateSession(replacement);
    deliver(f.panel, current, page(current, QStringLiteral("OLD_LOGIN_MUST_NOT_APPLY")));
    QVERIFY(!f.text(QStringLiteral("output"))->text().contains(QStringLiteral("OLD_LOGIN_MUST_NOT_APPLY")));
    const int count = f.reads.size();
    f.panel.openSession(replacement); snapshot(f.panel, replacement);
    QCOMPARE(f.reads.size(), count);
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    QCOMPARE(f.reads.size(), count + 3);
}

void RemoteToolDetailTest::offlineCacheAndLegacyInlineRemainReadable() {
    Fixture f;
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read output = f.read(QStringLiteral("output"));
    deliver(f.panel, output, page(output, QStringLiteral("CACHED_WHILE_OFFLINE")));
    auto offline = f.current; offline.insert(QStringLiteral("online"), false); f.panel.updateSession(offline);
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QStringLiteral("CACHED_WHILE_OFFLINE"));
    const int count = f.reads.size();
    QVERIFY(f.more(QStringLiteral("input"))); f.more(QStringLiteral("input"))->click();
    QCOMPARE(f.reads.size(), count);
    f.panel.updateSession(f.current);
    QCOMPARE(f.reads.size(), count);
    f.more(QStringLiteral("input"))->click();
    QCOMPARE(f.reads.size(), count + 1);

    Fixture legacy;
    auto inlinePart = part();
    inlinePart.remove(QStringLiteral("detailRevision")); inlinePart.remove(QStringLiteral("detailsAvailable"));
    inlinePart.insert(QStringLiteral("input"), QStringLiteral("LEGACY_INPUT"));
    inlinePart.insert(QStringLiteral("output"), QStringLiteral("LEGACY_OUTPUT"));
    snapshot(legacy.panel, legacy.current, inlinePart);
    QVERIFY(legacy.groupButton()); legacy.groupButton()->click();
    QVERIFY(legacy.itemButton()); legacy.itemButton()->click();
    QCOMPARE(legacy.reads.size(), 0);
    QCOMPARE(legacy.text(QStringLiteral("input"))->text(), QStringLiteral("LEGACY_INPUT"));
    QCOMPARE(legacy.text(QStringLiteral("output"))->text(), QStringLiteral("LEGACY_OUTPUT"));
}

void RemoteToolDetailTest::busyAndInvalidUtf8PagesRemainExplicit() {
    Fixture f;
    QVERIFY(f.groupButton()); f.groupButton()->click();
    QVERIFY(f.itemButton()); f.itemButton()->click();
    const Read input = f.read(QStringLiteral("input"));
    auto result = page(input, QStringLiteral("unused"));
    result.insert(QStringLiteral("ok"), false); result.insert(QStringLiteral("errorCode"), QStringLiteral("busy"));
    result.insert(QStringLiteral("error"), QStringLiteral("Tool detail revision changed."));
    deliver(f.panel, input, result);
    QVERIFY(f.more(QStringLiteral("input"))->isVisible());
    const int count = f.reads.size();
    f.more(QStringLiteral("input"))->click();
    QCOMPARE(f.reads.size(), count + 3);
    for (int index = count; index < f.reads.size(); ++index)
        QCOMPARE(f.reads[index].command.value(QStringLiteral("offset")).toInt(), 0);
    const Read output = f.read(QStringLiteral("output"));
    const QByteArray bytes("bad\xff", 4);
    auto invalid = page(output, QString::fromUtf8(bytes));
    // A first-page refresh deliberately omitted the retired revision in its
    // request. The owner still returns the actual current SHA256 in its reply.
    invalid.insert(QStringLiteral("detailRevision"), kNewRevision);
    invalid.insert(QStringLiteral("sourceBytesRead"), 4); invalid.insert(QStringLiteral("totalBytes"), 4);
    invalid.insert(QStringLiteral("textIsValidUtf8"), false);
    invalid.insert(QStringLiteral("rawBase64"), QString::fromLatin1(bytes.toBase64()));
    deliver(f.panel, output, invalid);
    QCOMPARE(f.text(QStringLiteral("output"))->text(), QString::fromUtf8(bytes));
    QVERIFY(f.text(QStringLiteral("output"))->toolTip().contains(QStringLiteral("UTF-8")));
}

QTEST_MAIN(RemoteToolDetailTest)
#include "RemoteToolDetailTest.moc"
