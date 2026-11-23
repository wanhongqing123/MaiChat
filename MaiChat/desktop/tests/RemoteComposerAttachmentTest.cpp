// 文件作用：用真实远程会话输入框验证 MIME
// 图片接收；不读写系统剪贴板或调用供应商。
#include <QApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QMimeData>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QtTest>

#include "agent/RemoteAgentPanel.h"

namespace {
QJsonObject owner() {
  return {{"id", "owned-windows-session"},
          {"device_slot", "windows"},
          {"login_session_id", "owned-login"},
          {"online", true},
          {"busy", false}};
}
void drop(QTextEdit *editor, const QMimeData &mime) {
  auto *viewport = editor->viewport();
  QPointF position(viewport->width() / 2., viewport->height() / 2.);
  QDragEnterEvent enter(position.toPoint(), Qt::CopyAction, &mime,
                        Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(viewport, &enter);
  QDragMoveEvent move(position.toPoint(), Qt::CopyAction, &mime, Qt::LeftButton,
                      Qt::NoModifier);
  QApplication::sendEvent(viewport, &move);
  QDropEvent event(position, Qt::CopyAction, &mime, Qt::LeftButton,
                   Qt::NoModifier);
  QApplication::sendEvent(viewport, &event);
}
void setup(RemoteAgentPanel &panel, QJsonObject &sent) {
  panel.setHistoryHandler([&panel](const QJsonObject &remote, const QString &,
                                   int, RemoteAgentPanel::Completion done) {
    panel.receivePayload(remote, "history_page",
                         {{"ok", true}, {"messages", QJsonArray{}}});
    done({});
  });
  panel.setCommandHandler([&](const QJsonObject &remote,
                              const QJsonObject &command,
                              RemoteAgentPanel::Completion done) {
    if (command.value("action") == "get_session") {
      panel.receivePayload(
          remote, "event",
          {{"type", "command_result"},
           {"request_id", command.value("requestId")},
           {"result", QJsonObject{{"ok", true},
                                  {"session", remote.value("id")},
                                  {"model", "deepseek-flash"},
                                  {"reasoningEffort", "high"},
                                  {"wire", "responses"}}}});
    } else
      sent = command;
    done({});
  });
  panel.resize(900, 700);
  panel.show();
  panel.openSession(owner());
}
} // namespace

class RemoteComposerAttachmentTest : public QObject {
  Q_OBJECT
private slots:
  void receivesImageMimeAndSendsWithoutText();
  void receivesLocalFileDropAndKeepsText();
  void pastedImageDraftSurvivesPanelReopen();
  void clearedImagesDoNotReappear();
  void droppedFilesRespectAttachmentLimit();
};

void RemoteComposerAttachmentTest::receivesImageMimeAndSendsWithoutText() {
  QTemporaryDir data;
  QVERIFY(data.isValid());
  QJsonObject sent;
  RemoteAgentPanel panel(nullptr, data.filePath("outbox.json"));
  setup(panel, sent);
  QVERIFY(QTest::qWaitForWindowExposed(&panel));
  auto *editor = panel.findChild<QTextEdit *>("remoteAgentComposer");
  auto *send = panel.findChild<QPushButton *>("remoteAgentSendButton");
  QVERIFY(editor && send);
  QImage image(80, 50, QImage::Format_RGBA8888);
  image.fill(Qt::green);
  QMimeData mime;
  mime.setImageData(image);
  drop(editor, mime);
  QVERIFY2(
      send->isEnabled(),
      "Image MIME must become a sendable attachment, not be silently consumed");
  const auto evidence =
      qEnvironmentVariable("MAICHAT_REMOTE_ATTACHMENT_CAPTURE");
  if (!evidence.isEmpty())
    QVERIFY(panel.grab().save(evidence));
  QTest::mouseClick(send, Qt::LeftButton);
  const auto files = sent.value("local_files").toArray();
  QCOMPARE(files.size(), 1);
  QVERIFY(QFileInfo(files[0].toString()).isFile());
  QCOMPARE(QImage(files[0].toString()).size(), image.size());
  QCOMPARE(sent.value("action").toString(), QString("prompt"));
  QCOMPARE(sent.value("text").toString(), QString());
}

void RemoteComposerAttachmentTest::receivesLocalFileDropAndKeepsText() {
  QTemporaryDir data;
  QVERIFY(data.isValid());
  QJsonObject sent;
  QImage image(80, 50, QImage::Format_RGBA8888);
  image.fill(Qt::blue);
  const auto path = data.filePath("owned-image.png");
  QVERIFY(image.save(path));
  RemoteAgentPanel panel(nullptr, data.filePath("outbox.json"));
  setup(panel, sent);
  auto *editor = panel.findChild<QTextEdit *>("remoteAgentComposer");
  QVERIFY(editor);
  editor->setPlainText("Owned accompanying text");
  QMimeData mime;
  mime.setUrls({QUrl::fromLocalFile(path)});
  drop(editor, mime);
  drop(editor, mime);
  auto *send = panel.findChild<QPushButton *>("remoteAgentSendButton");
  QVERIFY(send);
  QTest::mouseClick(send, Qt::LeftButton);
  const auto files = sent.value("local_files").toArray();
  QCOMPARE(files.size(), 1);
  QCOMPARE(files[0].toString(), QFileInfo(path).absoluteFilePath());
  QCOMPARE(sent.value("text").toString(), QString("Owned accompanying text"));
}

void RemoteComposerAttachmentTest::pastedImageDraftSurvivesPanelReopen() {
  QTemporaryDir data;
  QVERIFY(data.isValid());
  QJsonObject sent;
  {
    RemoteAgentPanel panel(nullptr, data.filePath("outbox.json"));
    setup(panel, sent);
    auto *editor = panel.findChild<QTextEdit *>("remoteAgentComposer");
    QVERIFY(editor);
    QImage image(30, 20, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    QMimeData mime;
    mime.setImageData(image);
    drop(editor, mime);
    editor->setPlainText("Owned retained text");
  }
  RemoteAgentPanel panel(nullptr, data.filePath("outbox.json"));
  setup(panel, sent);
  auto *send = panel.findChild<QPushButton *>("remoteAgentSendButton");
  QVERIFY(send);
  QTest::mouseClick(send, Qt::LeftButton);
  const auto files = sent.value("local_files").toArray();
  QCOMPARE(files.size(), 1);
  QVERIFY(QFileInfo(files[0].toString()).isFile());
  QCOMPARE(sent.value("text").toString(), QString("Owned retained text"));
}
void RemoteComposerAttachmentTest::clearedImagesDoNotReappear() {
  QTemporaryDir data;
  QVERIFY(data.isValid());
  QJsonObject sent;
  {
    RemoteAgentPanel panel(nullptr, data.filePath("outbox.json"));
    setup(panel, sent);
    auto *editor = panel.findChild<QTextEdit *>("remoteAgentComposer");
    QVERIFY(editor);
    QImage image(30, 20, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    QMimeData mime;
    mime.setImageData(image);
    drop(editor, mime);
    auto *send = panel.findChild<QPushButton *>("remoteAgentSendButton");
    QVERIFY(send && send->isEnabled());
    QPushButton *clear = nullptr;
    for (auto *button : panel.findChildren<QPushButton *>())
      if (button->text() == QStringLiteral("清除附件"))
        clear = button;
    QVERIFY(clear && clear->isVisibleTo(&panel));
    QTest::mouseClick(clear, Qt::LeftButton);
    QVERIFY(!send->isEnabled());
  }
  RemoteAgentPanel panel(nullptr, data.filePath("outbox.json"));
  setup(panel, sent);
  auto *send = panel.findChild<QPushButton *>("remoteAgentSendButton");
  QVERIFY(send && !send->isEnabled());
}

void RemoteComposerAttachmentTest::droppedFilesRespectAttachmentLimit() {
  QTemporaryDir data;
  QVERIFY(data.isValid());
  QJsonObject sent;
  QImage image(10, 10, QImage::Format_RGBA8888);
  image.fill(Qt::blue);
  QList<QUrl> files;
  for (int i = 0; i < 12; ++i) {
    const auto path = data.filePath(QString("owned-%1.png").arg(i));
    QVERIFY(image.save(path));
    files.append(QUrl::fromLocalFile(path));
  }
  RemoteAgentPanel panel(nullptr, data.filePath("outbox.json"));
  setup(panel, sent);
  auto *editor = panel.findChild<QTextEdit *>("remoteAgentComposer");
  QVERIFY(editor);
  QMimeData mime;
  mime.setUrls(files);
  drop(editor, mime);
  auto *send = panel.findChild<QPushButton *>("remoteAgentSendButton");
  QVERIFY(send && send->isEnabled());
  QTest::mouseClick(send, Qt::LeftButton);
  QCOMPARE(sent.value("local_files").toArray().size(), 10);
}

QTEST_MAIN(RemoteComposerAttachmentTest)
#include "RemoteComposerAttachmentTest.moc"
