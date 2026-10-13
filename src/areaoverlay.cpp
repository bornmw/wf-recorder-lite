#include "areaoverlay.h"

#include <QFont>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>

AreaOverlay::AreaOverlay(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
    setAttribute(Qt::WA_TranslucentBackground);
    setCursor(Qt::CrossCursor);
}

void AreaOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    const QRect full = rect();

    // Dim everything outside the selection.
    p.fillRect(full, QColor(0, 0, 0, 110));
    if (!m_sel.isNull())
    {
        p.fillRect(m_sel, Qt::transparent);
        p.setPen(QPen(QColor(0, 220, 130), 3));
        p.drawRect(m_sel.adjusted(1, 1, -1, -1));
    }

    p.setPen(QColor(255, 255, 255, 220));
    QFont hintFont = font();
    hintFont.setBold(true);
    p.setFont(hintFont);
    p.drawText(full.adjusted(0, 14, 0, 0), Qt::AlignHCenter,
               "Drag to select area — Esc to cancel");
}

void AreaOverlay::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
    {
        m_anchor = e->pos();
        m_sel = QRect(m_anchor, m_anchor);
        m_dragging = true;
        update();
    }
}

void AreaOverlay::mouseMoveEvent(QMouseEvent *e)
{
    if (m_dragging)
    {
        m_sel = QRect(m_anchor, e->pos()).normalized();
        update();
    }
}

void AreaOverlay::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_dragging || e->button() != Qt::LeftButton)
        return;
    m_dragging = false;

    if (m_sel.width() < 8 || m_sel.height() < 8)
    {
        // Tiny click — treat as a cancel, keep the overlay up.
        m_sel = QRect();
        update();
        return;
    }

    // Emit coordinates local to the screen (output), in physical pixels:
    // map window-local selection to global, then subtract the screen's
    // virtual origin (robust if the compositor does not place the window
    // exactly at the screen origin).
    const QPoint g = mapToGlobal(m_sel.topLeft());
    const QRect geo = screen()->geometry();
    const qreal dpr = devicePixelRatio();
    const int x = qRound((g.x() - geo.x()) * dpr);
    const int y = qRound((g.y() - geo.y()) * dpr);
    const int w = qRound(m_sel.width() * dpr);
    const int h = qRound(m_sel.height() * dpr);
    emit selected(QRect(x, y, w, h));
    hide();
}

void AreaOverlay::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape)
    {
        m_sel = QRect();
        m_dragging = false;
        hide();
        return;
    }
    QWidget::keyPressEvent(e);
}
