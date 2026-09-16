#include "ui/VoiceGroups.h"

#include <QComboBox>
#include <QFont>
#include <QStandardItemModel>

namespace gbtts {

namespace {

bool isEngine(const QString &engine, const char *key)
{
    return engine == QLatin1String(key);
}

} // namespace

QString VoiceText::languageName(const QString &code)
{
    if (code == QLatin1String("it")) return QStringLiteral("Italiano");
    if (code == QLatin1String("en")) return QStringLiteral("English");
    if (code == QLatin1String("es")) return QStringLiteral("Español");
    if (code == QLatin1String("fr")) return QStringLiteral("Français");
    if (code == QLatin1String("de")) return QStringLiteral("Deutsch");
    if (code == QLatin1String("pt")) return QStringLiteral("Português");
    if (code == QLatin1String("ru")) return QStringLiteral("Русский");
    if (code == QLatin1String("ja")) return QStringLiteral("日本語");
    if (code == QLatin1String("ko")) return QStringLiteral("한국어");
    if (code == QLatin1String("zh")) return QStringLiteral("中文");
    if (code == QLatin1String("hi")) return QStringLiteral("हिन्दी");
    if (code == QLatin1String("auto")) return tr("Automatic (Italian/English)");
    if (code == QLatin1String("multi") || code.isEmpty()) return tr("Multilingual");
    return code;
}

QString VoiceText::displayName(const Controller::Voice &v)
{
    QString name = v.name;
    if (v.gender == QLatin1String("f")) name = QStringLiteral("♀ ") + name;
    else if (v.gender == QLatin1String("m")) name = QStringLiteral("♂ ") + name;
    if (v.kind == QLatin1String("builtin_download")) name += QStringLiteral("  ⬇");
    return name;
}

QList<VoiceText::Group> VoiceText::groupVoices(const QString &engine, const QList<Controller::Voice> &voices)
{
    QList<Group> out;
    auto add = [&out](const QString &title, const QList<Controller::Voice> &list) {
        if (!list.isEmpty()) out.append(Group{title, list});
    };

    if (isEngine(engine, "qwen")) {
        QList<Controller::Voice> italian, mine, builtin;
        for (const Controller::Voice &v : voices) {
            if (v.kind != QLatin1String("clone")) builtin.append(v);
            else if (v.source == QLatin1String("stock")) italian.append(v);
            else mine.append(v);
        }
        add(tr("Italian voices (included)"), italian);
        add(tr("Your voices (clone / design)"), mine);
        add(tr("Qwen built-in voices (non-Italian accent)"), builtin);
    } else if (isEngine(engine, "kokoro")) {
        QStringList order = {QStringLiteral("it"), QStringLiteral("en"), QStringLiteral("es"),
                             QStringLiteral("fr"), QStringLiteral("pt"), QStringLiteral("hi")};
        for (const Controller::Voice &v : voices)
            if (!order.contains(v.lang)) order.append(v.lang);
        for (const QString &lang : order) {
            QList<Controller::Voice> list;
            for (const Controller::Voice &v : voices)
                if (v.lang == lang) list.append(v);
            add(languageName(lang), list);
        }
    } else if (isEngine(engine, "supertonic")) {
        QList<Controller::Voice> women, men, other;
        for (const Controller::Voice &v : voices) {
            if (v.gender == QLatin1String("f")) women.append(v);
            else if (v.gender == QLatin1String("m")) men.append(v);
            else other.append(v);
        }
        add(tr("Women"), women);
        add(tr("Men"), men);
        add(tr("Other"), other);
    } else {
        add(tr("Voices"), voices);
    }
    return out;
}

QString VoiceText::engineTagline(const QString &engine)
{
    if (isEngine(engine, "kokoro")) return tr("light, CPU only");
    if (isEngine(engine, "supertonic")) return tr("CPU only, many languages");
    return tr("most natural, NVIDIA GPU");
}

QString VoiceText::engineDescription(const QString &engine)
{
    if (isEngine(engine, "kokoro"))
        return tr("Light and fast, runs on the processor: ideal while gaming, the GPU stays free. "
                  "Native Italian voices Sara and Nicola, plus English, Spanish, French and Portuguese voices.");
    if (isEngine(engine, "supertonic"))
        return tr("Runs on the processor, 30 languages with ten generic voices. "
                  "Useful as a fallback when neither Qwen nor Kokoro can run.");
    return tr("The most natural voice: sentence-level intonation, speaking style instructions, "
              "your own cloned or designed voices. Needs an NVIDIA GPU.");
}

QString VoiceText::engineResources(const QString &engine)
{
    if (isEngine(engine, "kokoro"))
        return tr("~520 MB RAM · no VRAM · max 8 threads, 0% CPU when idle · first word in ~0.3 s");
    if (isEngine(engine, "supertonic"))
        return tr("~500 MB RAM · no VRAM · slower than Kokoro");
    return tr("4.7 GB VRAM (1.7B) or 2.7 GB (0.6B) + ~2.2 GB RAM · first word in ~0.3 s");
}

QString VoiceText::voiceCountText(const QString &engine, int count)
{
    return tr("%n voice(s) of %1", nullptr, count).arg(Controller::engineLabel(engine));
}

void VoiceText::fillLanguages(QComboBox *combo, const QString &engine, const QString &current)
{
    const QSignalBlocker block(combo);
    combo->clear();
    for (const QString &code : Controller::engineLanguages(engine)) combo->addItem(languageName(code), code);
    const int idx = combo->findData(current);
    combo->setCurrentIndex(idx < 0 ? 0 : idx);
}

void VoiceText::fillVoices(QComboBox *combo, const QString &engine, const QList<Controller::Voice> &voices,
                           const QString &current, bool loaded)
{
    const QSignalBlocker block(combo);
    combo->clear();
    auto *model = qobject_cast<QStandardItemModel *>(combo->model());
    auto header = [&](const QString &text) {
        combo->addItem(text);
        if (!model) return;
        QStandardItem *it = model->item(combo->count() - 1);
        it->setFlags(it->flags() & ~(Qt::ItemIsSelectable | Qt::ItemIsEnabled));
        QFont f = it->font();
        f.setBold(true);
        it->setFont(f);
    };

    if (voices.isEmpty()) {
        header(loaded ? tr("(no voices)") : tr("Loading the %1 voices…").arg(Controller::engineLabel(engine)));
        return;
    }
    const QList<Group> groups = groupVoices(engine, voices);
    for (const Group &g : groups) {
        if (groups.size() > 1) header(g.title);
        for (const Controller::Voice &v : g.voices) {
            combo->addItem(QStringLiteral("   ") + displayName(v), v.id);
            QString tip = v.description;
            if (v.kind == QLatin1String("builtin_download"))
                tip += QStringLiteral("\n") + tr("The first message downloads this model (a few GB).");
            combo->setItemData(combo->count() - 1, tip, Qt::ToolTipRole);
        }
    }
    int idx = combo->findData(current);
    if (idx < 0) {
        for (int i = 0; i < combo->count(); ++i)
            if (!combo->itemData(i).toString().isEmpty()) { idx = i; break; }
    }
    if (idx >= 0) combo->setCurrentIndex(idx);
}

} // namespace gbtts
