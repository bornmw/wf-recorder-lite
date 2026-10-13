#pragma once

#include <QRect>
#include <QWidget>

// Fullscreen transparent overlay for selecting a screen region:
// drag a rectangle, Esc cancels. Emits the rectangle in PHYSICAL
// (device) pixels — the space wf-recorder's -g geometry consumes,
// independent of the fractional scale factor Qt reports.
class AreaOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit AreaOverlay(QWidget *parent = nullptr);

signals:
    void selected(const QRect &physRect);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;

private:
    QPoint m_anchor;
    QRect m_sel; // logical pixels
    bool m_dragging = false;
};
