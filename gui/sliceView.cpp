#include "sliceView.h"
#include <QMouseEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <vtkDataArray.h>
#include <vtkPointData.h>

SliceView::SliceView(int axis, QWidget *parent) : QWidget(parent), m_axis(axis) {
    m_u = axis == 0 ? 1 : 0;
    m_v = axis == 2 ? 1 : 2;
    setMinimumSize(180, 200);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setObjectName(QString("sliceView%1").arg(axis));
}
void SliceView::setVolume(ImagePtr volume) {
    m_volume = volume;
    m_mask = nullptr;
    m_slice = volume ? volume->GetDimensions()[m_axis] / 2 : 0;
    refresh();
}
void SliceView::setMask(ImagePtr mask) {
    m_mask = mask;
    refresh();
}
void SliceView::setSlice(int slice) {
    m_slice = slice;
    refresh();
}
void SliceView::setWindow(double low, double high) {
    m_low = low;
    m_high = high;
    refresh();
}
void SliceView::setOverlay(bool visible) {
    m_overlay = visible;
    refresh();
}
void SliceView::setEditing(bool enabled) {
    m_editing = enabled;
    setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
}
void SliceView::refresh() {
    if (!m_volume) {
        m_image = {};
        update();
        return;
    }
    const auto *d = m_volume->GetDimensions();
    m_slice = std::clamp(m_slice, 0, d[m_axis] - 1);
    m_image = QImage(d[m_u], d[m_v], QImage::Format_RGB32);
    auto *scalars = m_volume->GetPointData()->GetScalars();
    const auto *mask =
        m_mask ? static_cast<const unsigned char *>(m_mask->GetScalarPointer()) : nullptr;
    for (int v = 0; v < d[m_v]; ++v) {
        auto *row = reinterpret_cast<QRgb *>(m_image.scanLine(d[m_v] - 1 - v));
        for (int u = 0; u < d[m_u]; ++u) {
            int c[3];
            c[m_axis] = m_slice;
            c[m_u] = u;
            c[m_v] = v;
            const size_t p = (size_t(c[2]) * d[1] + c[1]) * d[0] + c[0];
            const double value = scalars->GetComponent(p, 0);
            const int gray =
                std::isfinite(value)
                    ? std::clamp(int((value - m_low) * 255 / std::max(1e-9, m_high - m_low)), 0,
                                 255)
                    : 0;
            row[u] = m_overlay && mask && mask[p]
                         ? qRgb(int(gray * 0.6), int(gray * 0.6 + 102), int(gray * 0.6 + 55))
                         : qRgb(gray, gray, gray);
        }
    }
    update();
}
void SliceView::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#111820"));
    if (m_image.isNull()) {
        painter.setPen(Qt::gray);
        painter.drawText(rect(), Qt::AlignCenter, "Load an anatomical series");
        return;
    }
    const auto *spacing = m_volume->GetSpacing();
    QSizeF physical(m_image.width() * spacing[m_u], m_image.height() * spacing[m_v]);
    const double factor =
        std::min((width() - 12) / physical.width(), (height() - 12) / physical.height());
    const QSizeF size = physical * factor;
    m_target = QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(),
                      size.height());
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(m_target, m_image);
}
void SliceView::paintAt(QMouseEvent *event) {
    if (!m_editing || !m_volume || !m_target.contains(event->position()))
        return;
    int c[3];
    c[m_axis] = m_slice;
    const auto *d = m_volume->GetDimensions();
    c[m_u] = std::clamp(int((event->position().x() - m_target.left()) / m_target.width() * d[m_u]),
                        0, d[m_u] - 1);
    c[m_v] = std::clamp(
        d[m_v] - 1 - int((event->position().y() - m_target.top()) / m_target.height() * d[m_v]), 0,
        d[m_v] - 1);
    emit brushAt(c[0], c[1], c[2], !(event->buttons() & Qt::RightButton));
}
void SliceView::mousePressEvent(QMouseEvent *e) { paintAt(e); }
void SliceView::mouseMoveEvent(QMouseEvent *e) {
    if (e->buttons() & (Qt::LeftButton | Qt::RightButton))
        paintAt(e);
}
void HistogramView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), QColor("#16212b"));
    const auto max = *std::max_element(m_hist.bins.begin(), m_hist.bins.end());
    if (!max)
        return;
    p.setPen(QColor("#7895a8"));
    for (int i = 0; i < 256; ++i) {
        const double h =
            std::log1p(double(m_hist.bins[i])) / std::log1p(double(max)) * (height() - 4);
        const double x = (i + 0.5) * width() / 256;
        p.drawLine(QPointF(x, height()), QPointF(x, height() - h));
    }
    p.setPen(QPen(QColor("#52dfab"), 2));
    for (double value : {m_low, m_high}) {
        const double x =
            std::clamp((value - m_hist.minimum) / std::max(1e-9, m_hist.maximum - m_hist.minimum),
                       0.0, 1.0) *
            width();
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
    }
}
