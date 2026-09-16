#pragma once

#include "core/Controller.h"

#include <QCoreApplication>
#include <QList>
#include <QString>
#include <QStringList>

class QComboBox;

namespace gbtts {

// Presentation of engines and voices shared by the main window, the voice
// library and the settings dialog: one wording, one grouping, everywhere.
class VoiceText
{
    Q_DECLARE_TR_FUNCTIONS(gbtts::VoiceText)
public:
    struct Group {
        QString title;
        QList<Controller::Voice> voices;
    };

    // Qwen: your voices / stock voices. Kokoro: by language, Italian first.
    // Supertonic: women / men. Groups are never empty.
    static QList<Group> groupVoices(const QString &engine, const QList<Controller::Voice> &voices);
    static QString displayName(const Controller::Voice &v);     // "♀ Sara", "Vivian ⬇"
    static QString languageName(const QString &code);          // native name
    static QString engineTagline(const QString &engine);       // one line for badges
    static QString engineDescription(const QString &engine);   // settings card body
    static QString engineResources(const QString &engine);     // measured cost
    static QString voiceCountText(const QString &engine, int count);

    // Fills a combo with the engine's languages (data = code), keeping `current`.
    static void fillLanguages(QComboBox *combo, const QString &engine, const QString &current);
    // Fills a combo with grouped voices (headers disabled), selecting `current`.
    static void fillVoices(QComboBox *combo, const QString &engine, const QList<Controller::Voice> &voices,
                           const QString &current, bool loaded);
};

} // namespace gbtts
