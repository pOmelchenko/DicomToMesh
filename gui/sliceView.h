#pragma once
#include "dicomConverter.h"
#include <QImage>
#include <QRectF>
#include <QWidget>

class SliceView : public QWidget {
    Q_OBJECT
  public:
    explicit SliceView(int axis, QWidget *parent = nullptr);
    void setVolume(ImagePtr volume);
    void setMask(ImagePtr mask);
    void setSlice(int slice);
    void setWindow(double low, double high);
    void setOverlay(bool visible);
    void setEditing(bool enabled);
    int slice() const { return m_slice; }
    int axis() const { return m_axis; }
    QImage image() const { return m_image; }
  signals:
    void brushAt(int x, int y, int z, bool foreground);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;

  private:
    void refresh();
    void paintAt(QMouseEvent *event);
    ImagePtr m_volume, m_mask;
    QImage m_image;
    QRectF m_target;
    int m_axis, m_slice = 0, m_u = 0, m_v = 1;
    double m_low = 0, m_high = 1;
    bool m_overlay = true, m_editing = false;
};

class HistogramView : public QWidget {
    Q_OBJECT
  public:
    explicit HistogramView(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(65);
        setMaximumHeight(90);
    }
    void setHistogram(const d2m::VolumeHistogram &h) {
        m_hist = h;
        update();
    }
    void setThresholds(double low, double high) {
        m_low = low;
        m_high = high;
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override;

  private:
    d2m::VolumeHistogram m_hist;
    double m_low = 0, m_high = 1;
};
