#pragma once

#include "core/UpdateFeed.h"

#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QPointer>

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QPlainTextEdit;
class QPushButton;

namespace gbtts {

class ProgressBar;

// Progress window of an update (the Soundboard's UpdaterWindow, same stages):
// download the package to %TEMP%\GameBaitersTTS-update (redirects followed),
// verify it (zip + SHA-256 from the feed), write and start the helper script.
class UpdaterWindow : public QDialog
{
    Q_OBJECT
public:
    explicit UpdaterWindow(const update::Info &info, QWidget *parent = nullptr);
    ~UpdaterWindow() override;

    void start();

signals:
    // ok: the helper is running (or written, in a dry run). !ok with an empty error: cancelled.
    void downloadFinished(bool ok, const QString &error);

protected:
    void reject() override;   // Cancel, Esc and the close button

private:
    void onProgress(qint64 bytes, qint64 total);
    void onReplyFinished();
    void finish(bool ok, const QString &error);
    void addLog(const QString &line);

    update::Info            m_info;
    QNetworkAccessManager  *m_net = nullptr;
    QPointer<QNetworkReply> m_reply;
    QFile                   m_file;
    QString                 m_dir;
    QString                 m_target;
    QElapsedTimer           m_speedTimer;
    qint64                  m_lastBytes = 0;
    double                  m_bytesPerSecond = 0.0;
    bool                    m_cancelled = false;
    bool                    m_finished = false;

    QLabel         *m_status = nullptr;
    ProgressBar    *m_bar = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QPushButton    *m_cancel = nullptr;
};

} // namespace gbtts
