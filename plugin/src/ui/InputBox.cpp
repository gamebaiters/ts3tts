#include "ui/InputBox.h"

#include <QKeyEvent>
#include <QTextBlock>
#include <QTextCursor>

namespace gbtts {

InputBox::InputBox(QWidget *parent) : QPlainTextEdit(parent)
{
    setTabChangesFocus(true);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
}

void InputBox::setHistory(const QStringList &history)
{
    m_history = history;
    m_histPos = -1;
}

QSize InputBox::sizeHint() const
{
    const int line = fontMetrics().lineSpacing();
    return {400, line * 3 + 16};
}

QSize InputBox::minimumSizeHint() const
{
    const int line = fontMetrics().lineSpacing();
    return {120, line * 2 + 14};
}

void InputBox::keyPressEvent(QKeyEvent *e)
{
    const bool plainEnter = (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) &&
                            !(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier));
    if (plainEnter) {
        const QString text = toPlainText().trimmed();
        if (!text.isEmpty()) {
            m_histPos = -1;
            m_draft.clear();
            emit submitted(text);
        }
        e->accept();
        return;
    }
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::ControlModifier)) {
        insertPlainText(QStringLiteral("\n"));
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Escape) {
        emit stopRequested();
        e->accept();
        return;
    }

    const QTextCursor c = textCursor();
    const bool onFirstLine = c.block() == document()->firstBlock();
    const bool onLastLine = c.block() == document()->lastBlock();
    if (e->key() == Qt::Key_Up && onFirstLine && !m_history.isEmpty() && !(e->modifiers() & Qt::ShiftModifier)) {
        if (m_histPos == -1) {
            m_draft = toPlainText();
            m_histPos = m_history.size();
        }
        if (m_histPos > 0) {
            --m_histPos;
            setPlainText(m_history.at(m_histPos));
            moveCursor(QTextCursor::End);
        }
        e->accept();
        return;
    }
    if (e->key() == Qt::Key_Down && onLastLine && m_histPos != -1 && !(e->modifiers() & Qt::ShiftModifier)) {
        ++m_histPos;
        if (m_histPos >= m_history.size()) {
            m_histPos = -1;
            setPlainText(m_draft);
        } else {
            setPlainText(m_history.at(m_histPos));
        }
        moveCursor(QTextCursor::End);
        e->accept();
        return;
    }
    QPlainTextEdit::keyPressEvent(e);
}

} // namespace gbtts
