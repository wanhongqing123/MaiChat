// Account storage preparation keeps startup file I/O off the GUI thread and
// hands the already-open backend to the ordinary controller without reopening.
#pragma once

#include <QString>
#include <functional>
#include <memory>

#include "MaiSqliteStore.h"

class QWidget;

QString accountAgentDirectory(const QString& ownerUserId);
QString agentDatabasePath(const QString& ownerUserId);

struct DesktopAgentStoragePreparationResult {
    std::unique_ptr<MaiSessionStore> store;
    QString error;
    bool canceled = false;
};

using DesktopAgentStorageOpener = std::function<MaiResult<std::unique_ptr<MaiSessionStore>>(
    const QString&, const MaiSqliteStoreOpenOptions&)>;

// Runs a nested Qt event loop while the existing QtConcurrent pool opens the
// real backend. Slow opens show progress; quick opens avoid a flashing dialog.
DesktopAgentStoragePreparationResult prepareDesktopAgentStorage(
    const QString& databasePath, QWidget* parent = nullptr,
    DesktopAgentStorageOpener opener = {}, int progressDelayMs = 200);
