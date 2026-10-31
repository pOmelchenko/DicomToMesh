#pragma once
#include "dicomSeries.h"
#include "volumeSegmentation.h"
#include "brainExtractor.h"
#include <QObject>
#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <utility>

using ImagePtr = vtkSmartPointer<vtkImageData>;
using MeshPtr = vtkSmartPointer<vtkPolyData>;
struct MeshOptions {
    double reduction = 0;
    int smoothingIterations = 0;
    bool center = false;
};
Q_DECLARE_METATYPE(d2m::DicomCatalog)
Q_DECLARE_METATYPE(d2m::DicomSeries)
Q_DECLARE_METATYPE(d2m::SegmentationOptions)
Q_DECLARE_METATYPE(d2m::VolumeHistogram)
Q_DECLARE_METATYPE(ImagePtr)
Q_DECLARE_METATYPE(MeshPtr)
Q_DECLARE_METATYPE(MeshOptions)

class DicomConverter : public QObject {
    Q_OBJECT
  public:
    explicit DicomConverter(BrainRuntime runtime = BrainRuntime::defaults())
        : m_brainRuntime(std::move(runtime)) {}
    // These two methods touch only an atomic flag and may be called by the UI.
    void cancel() { m_cancelled.store(true); }
    void resetCancellation() { m_cancelled.store(false); }
  public slots:
    void scan(const QString &directory);
    void load(const d2m::DicomSeries &series);
    void segment(const d2m::SegmentationOptions &options);
    void build(double isotropicSpacing, const MeshOptions &options);
    void process(const MeshOptions &options);
    void save(const QString &path, bool original);
  signals:
    void catalogReady(const d2m::DicomCatalog &catalog);
    void volumeReady(ImagePtr image, const d2m::VolumeHistogram &histogram);
    void maskReady(ImagePtr mask);
    void meshReady(MeshPtr original, MeshPtr processed);
    void saved(const QString &path);
    void failed(const QString &operation, const QString &message);
    void progress(int percent);

  private:
    ImagePtr m_volume, m_mask, m_brainMask, m_brainDistance;
    MeshPtr m_original, m_processed;
    BrainRuntime m_brainRuntime;
    std::atomic_bool m_cancelled{false};
    bool m_useBrainDistance = false;
    void processMesh(const MeshOptions &options);
};
