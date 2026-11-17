#pragma once

#include "MaiCloudDriveClient.h"

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include <memory>

class DesktopCloudDriveImageReservation;

// The image snapshot is bounded by the shared cloud-image policy and remains
// immutable while the shared upload borrows it. It makes the thumbnail and
// original refer to the same bytes, without buffering a video in memory.
struct DesktopCloudDrivePreparedImage {
    QByteArray encodedOriginal;
    QString thumbnailPath;
    QString error;
    // Copies borrow one shared process-wide slot until the final prepared
    // value leaves its synchronous upload/queued worker lifetime.
    std::shared_ptr<const DesktopCloudDriveImageReservation> reservation;
    bool image = false;
    bool resourceBusy = false;
};

// Worker-thread helper shared by the ordinary UI and Agent adapter. The JPEG
// and its source/thumbnail fingerprint metadata live beside the existing
// account-owned checkpoint. A legacy checkpoint without a thumbnail keeps its
// original transport contract. Missing local auxiliaries on a matching new
// checkpoint return an empty path so the core can recover authoritative ready
// state; corrupt/mismatched existing bytes fail instead of being regenerated.
// No remote request or model protocol is used.
DesktopCloudDrivePreparedImage prepareDesktopCloudDriveImage(const QString& sourcePath, const QString& checkpointPath,
                                                             const QString& displayName);

// Keep auxiliary files after pause/failure while a core checkpoint exists.
// Once the shared core confirms completion/discard, or returns without any
// durable checkpoint, remove only this helper's matching adjacent files.
// Never remove the user's original media or follow a path stored in metadata.
bool finishDesktopCloudDriveImage(const QString& checkpointPath, bool verifiedCompletionOrDiscard,
                                  QString* error = nullptr);

// Ordinary desktop adapter: prepare once, borrow option strings for the whole
// synchronous core call, and apply the file ownership rule above. The caller
// must serialize calls for one checkpoint and run this on a worker thread.
// preparedImage, when supplied, is an immutable result of the ordinary helper
// above and remains borrowed only for this synchronous call.
void uploadDesktopCloudDriveMedia(const MaiCloudDriveOptions& options, const QString& sourcePath,
                                  const QString& checkpointPath, const QString& displayName,
                                  const MaiCloudDriveCancel* cancel, MaiCloudDriveResult* result,
                                  QString* localError = nullptr,
                                  const DesktopCloudDrivePreparedImage* preparedImage = nullptr);

quint64 desktopCloudDriveMaximumBytes(bool image);
int desktopCloudDriveDownloadTimeoutMs(bool image);
