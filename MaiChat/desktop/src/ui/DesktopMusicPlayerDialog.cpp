// 文件作用：选择并播放一首本地音频，关闭窗口时立即停止声音。
#include "ui/UiZoom.h"
#include "ui/DesktopMusicPlayerDialog.h"
#include "ui/DesktopAudioPlayback.h"

#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace {
QPointer<DesktopMusicPlayerDialog> sActiveMusic;

QString timeLabel(qint64 microseconds) {
    const qint64 seconds = qMax<qint64>(0, microseconds / 1000000);
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

bool supportedMusic(const QString& path) {
    const QFileInfo file(path);
    return file.isFile() && file.isReadable() && file.size() > 0 &&
           QStringList{QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("wav")}.contains(
               file.suffix().toLower());
}
}  // namespace

bool DesktopMusicPlayerDialog::openFile(QWidget* parent, const QString& path, QString* error) {
    if (error) error->clear();
#if !defined(MAICHAT_HAS_FFPLAY)
    (void)parent;
    (void)path;
    if (error) *error = QStringLiteral("当前构建未包含 FFplay 音频播放器");
    return false;
#else
    if (!supportedMusic(path)) {
        if (error) *error = QStringLiteral("请选择可读取的 MP3、M4A 或 WAV 文件");
        return false;
    }
    if (sActiveMusic) sActiveMusic->close();
    auto* dialog = new DesktopMusicPlayerDialog(parent, path);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    sActiveMusic = dialog;
    dialog->show();
    dialog->raise();
    return true;
#endif
}

void DesktopMusicPlayerDialog::closeActive() {
    if (sActiveMusic) sActiveMusic->close();
}

DesktopMusicPlayerDialog::DesktopMusicPlayerDialog(QWidget* parent, const QString& path) : QDialog(parent) {
    setWindowTitle(QStringLiteral("音乐播放器"));
    setFixedWidth(440);
    setObjectName(QStringLiteral("desktopMusicPlayer"));
    setStyleSheet(QStringLiteral("QDialog#desktopMusicPlayer{background:#ffffff;}"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 22, 28, 22);
    layout->setSpacing(12);
    auto* source = new QLabel(QStringLiteral("正在播放  ·  本地文件"), this);
    UiZoom::setStyle(source, QStringLiteral("color:#667085;font-size: @metadata;"));
    layout->addWidget(source);
    auto* artwork = new QLabel(QStringLiteral("♫"), this);
    artwork->setAlignment(Qt::AlignCenter);
    artwork->setFixedSize(104, 104);
    artwork->setStyleSheet(
        QStringLiteral("color:#ffffff;font-size:48px;border-radius:24px;"
                       "background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #2193ee,stop:1 #6350c2);"));
    layout->addWidget(artwork, 0, Qt::AlignHCenter);
    title_ = new QLabel(this);
    title_->setWordWrap(true);
    title_->setAlignment(Qt::AlignCenter);
    UiZoom::setStyle(title_, QStringLiteral("color:#101828;font-size: @body;font-weight:600;"));
    layout->addWidget(title_);
    progress_ = new QSlider(Qt::Horizontal, this);
    progress_->setObjectName(QStringLiteral("desktopMusicProgress"));
    progress_->setRange(0, 1000);
    progress_->setStyleSheet(
        QStringLiteral("QSlider::groove:horizontal{height:4px;background:#d6e2ee;border-radius:2px;}"
                       "QSlider::sub-page:horizontal{background:#168fe0;border-radius:2px;}"
                       "QSlider::handle:horizontal{background:#168fe0;width:14px;margin:-5px 0;"
                       "border-radius:7px;}"));
    layout->addWidget(progress_);
    time_ = new QLabel(QStringLiteral("0:00 / 0:00"), this);
    UiZoom::setStyle(time_, QStringLiteral("color:#667085;font-size: @metadata;"));
    layout->addWidget(time_);
    status_ = new QLabel(this);
    UiZoom::setStyle(status_, QStringLiteral("color:#667085;font-size: @metadata;"));
    layout->addWidget(status_);
    auto* controls = new QHBoxLayout;
    play_ = new QPushButton(QStringLiteral("暂停"), this);
    play_->setObjectName(QStringLiteral("desktopMusicPlayPause"));
    auto* stop = new QPushButton(QStringLiteral("停止"), this);
    auto* choose = new QPushButton(QStringLiteral("选择其他音乐"), this);
    play_->setFixedSize(88, 36);
    stop->setFixedSize(88, 36);
    play_->setStyleSheet(
        QStringLiteral("QPushButton{color:white;background:#168fe0;border:0;border-radius:10px;"
                       "font-weight:600;}QPushButton:hover{background:#0d76bd;}"));
    stop->setStyleSheet(QStringLiteral("QPushButton{color:#0d76bd;background:#e8f4fc;border:0;border-radius:10px;}"));
    choose->setStyleSheet(QStringLiteral("QPushButton{color:#344054;background:transparent;border:0;padding:8px;}"));
    controls->addWidget(play_);
    controls->addWidget(stop);
    controls->addStretch();
    controls->addWidget(choose);
    layout->addLayout(controls);

    // 音乐和聊天音频卡共用同一 owner 协调，不再各自持有能操作全局 FFplay 的旧 handle。
    playback_ = new DesktopAudioPlayback(this);
    connect(playback_, &DesktopAudioPlayback::stateChanged, this, [this] { refreshPlayback(); });
    connect(playback_, &DesktopAudioPlayback::positionChanged, this, [this] { refreshPlayback(); });
    connect(playback_, &DesktopAudioPlayback::durationChanged, this, [this] { refreshPlayback(); });
    connect(playback_, &DesktopAudioPlayback::errorOccurred, status_,
            [this](const QString& error) { status_->setText(error); });
    connect(progress_, &QSlider::sliderReleased, this, [this] {
        if (playback_->durationUs() > 0)
            playback_->seek(static_cast<qint64>(playback_->durationUs() * (progress_->value() / 1000.0)));
    });
    connect(play_, &QPushButton::clicked, this, [this] {
        if (playback_->state() == DesktopAudioPlayback::State::Playing ||
            playback_->state() == DesktopAudioPlayback::State::Starting)
            playback_->pause();
        else
            playback_->play();
    });
    connect(stop, &QPushButton::clicked, this, [this] {
        stopPlayback();
        progress_->setValue(0);
        time_->setText(QStringLiteral("0:00 / 0:00"));
        play_->setText(QStringLiteral("播放"));
    });
    connect(choose, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择本地音乐"), QStandardPaths::writableLocation(QStandardPaths::MusicLocation),
            QStringLiteral("音频文件 (*.mp3 *.m4a *.wav)"));
        if (path.isEmpty()) return;
        if (!supportedMusic(path)) {
            status_->setText(QStringLiteral("请选择可读取的 MP3、M4A 或 WAV 文件"));
            return;
        }
        setFile(path);
    });
    setFile(path);
}

void DesktopMusicPlayerDialog::setFile(const QString& path) {
    stopPlayback();
    path_ = path;
    title_->setText(QFileInfo(path).fileName());
    status_->clear();
    progress_->setValue(0);
    time_->setText(QStringLiteral("0:00 / 0:00"));
#if defined(MAICHAT_HAS_FFPLAY)
    playback_->setSource(path);
    playback_->play();
#else
    status_->setText(QStringLiteral("当前构建未包含 FFplay 音频播放器"));
#endif
}

void DesktopMusicPlayerDialog::refreshPlayback() {
    if (!playback_) return;
    const qint64 duration = playback_->durationUs();
    const qint64 position = duration > 0 ? qBound<qint64>(0, playback_->positionUs(), duration) : 0;
    if (!progress_->isSliderDown())
        progress_->setValue(duration > 0 ? static_cast<int>(position * 1000.0 / duration) : 0);
    time_->setText(timeLabel(position) + QStringLiteral(" / ") + timeLabel(duration));
    const auto state = playback_->state();
    playing_ = state == DesktopAudioPlayback::State::Playing || state == DesktopAudioPlayback::State::Starting;
    play_->setText(playing_                                      ? QStringLiteral("暂停")
                   : state == DesktopAudioPlayback::State::Ended ? QStringLiteral("重播")
                                                                 : QStringLiteral("播放"));
    if (state == DesktopAudioPlayback::State::Error)
        status_->setText(playback_->errorString());
    else if (state == DesktopAudioPlayback::State::Starting)
        status_->setText(QStringLiteral("正在载入…"));
    else
        status_->clear();
}

void DesktopMusicPlayerDialog::stopPlayback() {
    if (playback_) playback_->stop();
    playing_ = false;
}

void DesktopMusicPlayerDialog::closeEvent(QCloseEvent* event) {
    stopPlayback();
    if (sActiveMusic == this) sActiveMusic.clear();
    QDialog::closeEvent(event);
}

DesktopMusicPlayerDialog::~DesktopMusicPlayerDialog() {
    stopPlayback();
}
