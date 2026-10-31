#pragma once
#include "dicomConverter.h"
#include <QThread>
#include <QTimer>
#include <QWidget>

class QComboBox;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QProgressBar;
class QSlider;
class QTabWidget;
class QGroupBox;
class QVTKOpenGLNativeWidget;
class vtkRenderer;
class vtkPolyDataMapper;
class vtkActor;
class SliceView;
class HistogramView;

class D2MWidget : public QWidget {
    Q_OBJECT
  public:
    explicit D2MWidget(QWidget *parent = nullptr);
    ~D2MWidget() override;
  public slots:
    void openDirectory(const QString &directory);
  signals:
    void doScan(const QString &directory);
    void doLoad(const d2m::DicomSeries &series);
    void doSegment(const d2m::SegmentationOptions &options, const d2m::DicomSeries &secondary);
    void doBuild(double spacing, const MeshOptions &options);
    void doProcess(const MeshOptions &options);
    void doSave(const QString &path, bool original);
    void seriesListed(int count);
    void volumeLoaded();
    void maskUpdated();

  private:
    void selectSeries();
    void loadSelected();
    void receivedVolume(ImagePtr image, const d2m::VolumeHistogram &histogram);
    void requestMask();
    void startMask();
    void modeChanged();
    void updateActions();
    void setBusy(const QString &message);
    void invalidateMesh();
    void updateWindow();
    void showMesh(bool resetCamera = true);
    MeshOptions meshOptions() const;
    d2m::SegmentationOptions segmentationOptions() const;
    void applyProcessing();
    void saveModel();
    void fail(const QString &operation, const QString &message);
    void resetEdits();

    QThread m_thread;
    DicomConverter *m_worker;
    d2m::DicomCatalog m_catalog;
    d2m::VolumeHistogram m_histogram;
    std::vector<d2m::MaskStroke> m_strokes;
    std::array<std::vector<d2m::MaskStroke>, 5> m_modeStrokes;
    int m_previousMode = 0;
    ImagePtr m_volume, m_mask;
    MeshPtr m_original, m_processed;
    bool m_busy = false, m_segmentRunning = false, m_segmentPending = false, m_initializing = false,
         m_maskCurrent = false, m_processingDirty = false, m_skullRequested = false;
    QTimer m_debounce;
    QPushButton *m_open, *m_load, *m_build, *m_process, *m_save, *m_undo, *m_clear, *m_cancel,
        *m_extractSkull;
    QComboBox *m_series, *m_mode, *m_comparison, *m_secondary;
    QWidget *m_skullControls;
    QLabel *m_details, *m_status, *m_hint, *m_meshStats, *m_maskInfo;
    QProgressBar *m_progress;
    QDoubleSpinBox *m_lower, *m_upper, *m_closing, *m_windowLow, *m_windowHigh, *m_brush,
        *m_reduction, *m_isotropicSpacing;
    QSpinBox *m_smoothing;
    QCheckBox *m_largest, *m_fill, *m_overlay, *m_edit, *m_center, *m_isotropic;
    QGroupBox *m_segmentationGroup;
    QTabWidget *m_tabs;
    SliceView *m_views[3];
    QSlider *m_sliders[3];
    HistogramView *m_histogramView;
    QVTKOpenGLNativeWidget *m_meshView;
    vtkSmartPointer<vtkRenderer> m_renderer;
    vtkSmartPointer<vtkPolyDataMapper> m_mapper;
    vtkSmartPointer<vtkActor> m_actor;
};
