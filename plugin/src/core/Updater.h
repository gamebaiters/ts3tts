#pragma once

#include "core/UpdateFeed.h"

#include <QObject>
#include <QPointer>

class QMessageBox;
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace gbtts {

class Controller;
class UpdaterWindow;

// Automatic updates, same behaviour as the GameBaiters Soundboard (Windows only):
//  - a few seconds after TeamSpeak starts, at most once a day, read version.xml;
//  - newer build -> ask (with the release notes of every newer version); "Later"
//    asks again in 3 days, "up to date" checks again in 1 day;
//  - "Update now" -> UpdaterWindow downloads + verifies the .ts3_plugin, starts the
//    helper script and TeamSpeak is asked to close so the installer can run.
//
// Crash safety: nothing here blocks or runs a nested event loop (every message box
// is non-modal and owned through a QPointer); the parentless boxes/window are
// deleted in the destructor, which gbtts::shutdown() runs before the DLL unloads.
class Updater : public QObject
{
    Q_OBJECT
public:
    explicit Updater(Controller *ctrl, QObject *parent = nullptr);
    ~Updater() override;

    void checkAutomatically();   // honours "check automatically" and the next-check time
    void checkNow();             // explicit: always reports the outcome

private:
    void startCheck(bool explicitCheck);
    void onFeedFinished(QNetworkReply *reply);
    void onNotesFinished(QNetworkReply *reply);
    void offer();
    void startDownload();
    void onDownloadFinished(bool ok, const QString &error);
    void postpone(qint64 seconds);
    void message(bool warning, const QString &title, const QString &text);
    QNetworkRequest request(const QUrl &url) const;
    QNetworkAccessManager *net();

    Controller              *m_ctrl;
    QNetworkAccessManager   *m_net = nullptr;
    QPointer<QNetworkReply>  m_reply;
    bool                     m_explicit = false;
    update::Info             m_info;
    QString                  m_notes;
    QPointer<QMessageBox>    m_box;
    QPointer<UpdaterWindow>  m_window;
};

} // namespace gbtts
