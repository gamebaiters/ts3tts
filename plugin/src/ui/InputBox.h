#pragma once

#include <QPlainTextEdit>
#include <QStringList>

namespace gbtts {

// The message box. Enter speaks, Shift+Enter inserts a new line, Esc stops,
// Up/Down walk the sent-message history like a terminal (only when the caret
// is on the first/last line, so multi-line editing keeps working).
class InputBox : public QPlainTextEdit
{
    Q_OBJECT
public:
    explicit InputBox(QWidget *parent = nullptr);

    void setHistory(const QStringList &history);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

signals:
    void submitted(const QString &text);
    void stopRequested();

protected:
    void keyPressEvent(QKeyEvent *e) override;

private:
    QStringList m_history;
    int         m_histPos = -1;
    QString     m_draft;
};

} // namespace gbtts
