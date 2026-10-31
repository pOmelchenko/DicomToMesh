#include "d2mWidget.h"
#include "sliceView.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QVTKOpenGLNativeWidget.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkMatrix3x3.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>

namespace {
QDoubleSpinBox *spin(const QString &name, double low, double high, double value, int decimals = 2) {
    auto *result = new QDoubleSpinBox;
    result->setObjectName(name);
    result->setRange(low, high);
    result->setDecimals(decimals);
    result->setValue(value);
    return result;
}
QPushButton *button(const QString &text, const QString &name) {
    auto *result = new QPushButton(text);
    result->setObjectName(name);
    return result;
}
QString directionLabel(vtkImageData *image, int axis, bool increasing = true) {
    int dominant = 0;
    double largest = 0;
    for (int i = 0; i < 3; ++i)
        if (std::abs(image->GetDirectionMatrix()->GetElement(i, axis)) > largest) {
            dominant = i;
            largest = std::abs(image->GetDirectionMatrix()->GetElement(i, axis));
        }
    const bool positive = (image->GetDirectionMatrix()->GetElement(dominant, axis) > 0) == increasing;
    const char *names[3][2] = {{"R", "L"}, {"A", "P"}, {"I", "S"}};
    return names[dominant][positive ? 1 : 0];
}
} // namespace

D2MWidget::D2MWidget(QWidget *parent) : QWidget(parent), m_worker(new DicomConverter) {
    setWindowTitle("DicomToMesh — series, mask and surface");
    resize(1260, 880);
    qRegisterMetaType<d2m::DicomCatalog>();
    qRegisterMetaType<d2m::DicomSeries>();
    qRegisterMetaType<d2m::SegmentationOptions>();
    qRegisterMetaType<d2m::VolumeHistogram>();
    qRegisterMetaType<ImagePtr>();
    qRegisterMetaType<MeshPtr>();
    qRegisterMetaType<MeshOptions>();
    auto *root = new QVBoxLayout(this);
    auto *source = new QHBoxLayout;
    m_open = button("Open DICOM folder…", "openFolder");
    source->addWidget(m_open);
    m_series = new QComboBox;
    m_series->setObjectName("seriesSelector");
    m_series->setMinimumContentsLength(35);
    source->addWidget(m_series, 1);
    m_load = button("Load selected series", "loadSeries");
    source->addWidget(m_load);
    root->addLayout(source);
    m_details =
        new QLabel("Open a folder to inspect its DICOM series. No series is loaded automatically.");
    m_details->setObjectName("seriesDetails");
    m_details->setWordWrap(true);
    root->addWidget(m_details);
    auto *splitter = new QSplitter;
    root->addWidget(splitter, 1);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(320);
    scroll->setMaximumWidth(410);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *controls = new QWidget;
    auto *left = new QVBoxLayout(controls);
    scroll->setWidget(controls);
    splitter->addWidget(scroll);
    m_segmentationGroup = new QGroupBox("1 · Segmentation");
    auto *segmentation = new QFormLayout(m_segmentationGroup);
    left->addWidget(m_segmentationGroup);
    m_mode = new QComboBox;
    m_mode->setObjectName("segmentationMode");
    m_mode->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_mode->setMinimumContentsLength(12);
    m_mode->addItems({"Threshold · CT / manual", "Head outline · MRI, approximate",
                      "Region from seeds · MRI / manual", "Brain · SynthStrip (local CPU)",
                      "Skull · MRI estimate (experimental)"});
    segmentation->addRow("Method", m_mode);
    m_skullControls = new QWidget;
    auto *skullLayout = new QVBoxLayout(m_skullControls);
    skullLayout->setContentsMargins(0, 0, 0, 0);
    auto *skullHelp = new QLabel("Load a T1-weighted MR series above. Choose a T2-weighted series "
                                 "from the same study to support the estimate (recommended).");
    skullHelp->setWordWrap(true);
    skullLayout->addWidget(skullHelp);
    m_secondary = new QComboBox;
    m_secondary->setObjectName("skullSupportingSeries");
    m_secondary->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_secondary->setMinimumContentsLength(12);
    m_secondary->addItem("No supporting T2 · T1 only", -1);
    skullLayout->addWidget(m_secondary);
    m_extractSkull = button("Estimate skull from MRI", "extractSkull");
    skullLayout->addWidget(m_extractSkull);
    segmentation->addRow(m_skullControls);
    m_lower = spin("lowerThreshold", -1e9, 1e9, 400);
    m_upper = spin("upperThreshold", -1e9, 1e9, 3000);
    segmentation->addRow("Lower intensity", m_lower);
    segmentation->addRow("Upper intensity", m_upper);
    auto *automatic = button("Suggest threshold (Otsu)", "autoThreshold");
    segmentation->addRow(automatic);
    m_histogramView = new HistogramView;
    m_histogramView->setObjectName("histogram");
    segmentation->addRow(m_histogramView);
    m_largest = new QCheckBox("Keep largest region");
    m_largest->setObjectName("largestRegion");
    segmentation->addRow(m_largest);
    m_fill = new QCheckBox("Fill holes in each slice");
    m_fill->setObjectName("fillHoles");
    segmentation->addRow(m_fill);
    m_closing = spin("closingRadius", 0, 20, 0);
    m_closing->setSuffix(" mm");
    segmentation->addRow("Close small gaps", m_closing);
    m_hint = new QLabel;
    m_hint->setWordWrap(true);
    segmentation->addRow(m_hint);
    m_cancel = button("Cancel extraction", "cancelBrain");
    segmentation->addRow(m_cancel);
    m_maskInfo = new QLabel("No current mask.");
    m_maskInfo->setObjectName("maskCoverage");
    m_maskInfo->setWordWrap(true);
    segmentation->addRow(m_maskInfo);
    m_overlay = new QCheckBox("Show mask overlay");
    m_overlay->setChecked(true);
    m_overlay->setObjectName("maskOverlay");
    segmentation->addRow(m_overlay);
    m_edit = new QCheckBox("Paint corrections / place seeds");
    m_edit->setObjectName("editMask");
    segmentation->addRow(m_edit);
    m_brush = spin("brushRadius", 0.1, 20, 2);
    m_brush->setSuffix(" mm");
    segmentation->addRow("Brush radius", m_brush);
    auto *undoRow = new QHBoxLayout;
    m_undo = button("Undo stroke", "undoStroke");
    m_clear = button("Clear edits", "clearStrokes");
    undoRow->addWidget(m_undo);
    undoRow->addWidget(m_clear);
    segmentation->addRow(undoRow);
    auto *paintHelp = new QLabel(
        "Left mouse: add / seed. Right mouse: erase / barrier. Review every slice before export.");
    paintHelp->setWordWrap(true);
    segmentation->addRow(paintHelp);
    auto *display = new QGroupBox("Slice display");
    auto *displayLayout = new QFormLayout(display);
    left->addWidget(display);
    m_windowLow = spin("windowLow", -1e9, 1e9, 0);
    m_windowHigh = spin("windowHigh", -1e9, 1e9, 1000);
    displayLayout->addRow("Black level", m_windowLow);
    displayLayout->addRow("White level", m_windowHigh);
    auto *surface = new QGroupBox("2 · Surface and optional processing");
    auto *mesh = new QFormLayout(surface);
    left->addWidget(surface);
    m_isotropic = new QCheckBox("Interpolate mask");
    m_isotropic->setObjectName("interpolateMask");
    mesh->addRow(m_isotropic);
    m_isotropicSpacing = spin("interpolationSpacing", 0.1, 10, 1);
    m_isotropicSpacing->setSuffix(" mm");
    mesh->addRow("Target spacing", m_isotropicSpacing);
    auto *interpolationHint = new QLabel("Interpolation reduces stair steps. It does not recover "
                                         "details between the original slices.");
    interpolationHint->setWordWrap(true);
    mesh->addRow(interpolationHint);
    m_build = button("Build surface", "buildSurface");
    mesh->addRow(m_build);
    m_reduction = spin("meshReduction", 0, 95, 0, 0);
    m_reduction->setSuffix(" %");
    mesh->addRow("Remove polygons", m_reduction);
    m_smoothing = new QSpinBox;
    m_smoothing->setObjectName("smoothingIterations");
    m_smoothing->setRange(0, 100);
    mesh->addRow("Smoothing iterations", m_smoothing);
    m_center = new QCheckBox("Move surface to its center");
    m_center->setObjectName("centerMesh");
    mesh->addRow(m_center);
    m_process = button("Apply processing", "applyProcessing");
    m_process->setToolTip("Always starts from the original surface; changes do not accumulate.");
    mesh->addRow(m_process);
    m_comparison = new QComboBox;
    m_comparison->setObjectName("surfaceComparison");
    m_comparison->addItems({"Original surface", "Processed surface"});
    mesh->addRow("View / export", m_comparison);
    m_meshStats = new QLabel("No surface built.");
    m_meshStats->setObjectName("meshStatistics");
    m_meshStats->setWordWrap(true);
    mesh->addRow(m_meshStats);
    m_save = button("Export visible surface…", "exportSurface");
    mesh->addRow(m_save);
    left->addStretch();
    for (auto *form : {segmentation, displayLayout, mesh}) {
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    }
    controls->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_tabs = new QTabWidget;
    m_tabs->setObjectName("previewTabs");
    splitter->addWidget(m_tabs);
    splitter->setStretchFactor(1, 1);
    auto *slicePanel = new QWidget;
    auto *views = new QVBoxLayout(slicePanel);
    auto *orientation =
        new QLabel("Orthogonal image planes · physical aspect ratio · DICOM LPS coordinates");
    orientation->setWordWrap(true);
    views->addWidget(orientation);
    auto *viewRow = new QHBoxLayout;
    views->addLayout(viewRow, 1);
    for (int axis = 0; axis < 3; ++axis) {
        auto *panel = new QWidget;
        auto *layout = new QVBoxLayout(panel);
        auto *label = new QLabel(axis == 0   ? "YZ · position along I"
                                 : axis == 1 ? "XZ · position along J"
                                             : "XY · acquisition slices");
        label->setObjectName(QString("planeLabel%1").arg(axis));
        label->setWordWrap(true);
        layout->addWidget(label);
        m_views[axis] = new SliceView(axis);
        layout->addWidget(m_views[axis], 1);
        m_sliders[axis] = new QSlider(Qt::Horizontal);
        m_sliders[axis]->setObjectName(QString("sliceSlider%1").arg(axis));
        layout->addWidget(m_sliders[axis]);
        viewRow->addWidget(panel, 1);
        connect(m_sliders[axis], &QSlider::valueChanged, m_views[axis], &SliceView::setSlice);
        connect(m_views[axis], &SliceView::brushAt, this, [this](int x, int y, int z, bool fg) {
            if (m_busy || !m_volume)
                return;
            m_strokes.push_back({{x, y, z}, m_brush->value(), fg});
            requestMask();
        });
        connect(m_overlay, &QCheckBox::toggled, m_views[axis], &SliceView::setOverlay);
        connect(m_edit, &QCheckBox::toggled, m_views[axis], &SliceView::setEditing);
    }
    m_tabs->addTab(slicePanel, "Slices and mask");
    m_meshView = new QVTKOpenGLNativeWidget;
    m_meshView->setObjectName("surfaceView");
    auto window = vtkSmartPointer<vtkGenericOpenGLRenderWindow>::New();
    m_meshView->setRenderWindow(window);
    m_renderer = vtkSmartPointer<vtkRenderer>::New();
    m_renderer->SetBackground(0.055, 0.08, 0.11);
    window->AddRenderer(m_renderer);
    m_mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
    m_mapper->ScalarVisibilityOff();
    m_actor = vtkSmartPointer<vtkActor>::New();
    m_actor->SetMapper(m_mapper);
    m_actor->GetProperty()->SetColor(0.82, 0.88, 0.91);
    m_actor->GetProperty()->SetAmbient(0.15);
    m_actor->SetVisibility(false);
    m_renderer->AddActor(m_actor);
    // LPS: look from the anterior/left side, with superior at the top.
    // Looking straight down the slice axis can hide open, cropped surfaces.
    m_renderer->GetActiveCamera()->SetPosition(1, -1, 0.6);
    m_renderer->GetActiveCamera()->SetViewUp(0, 0, 1);
    m_tabs->addTab(m_meshView, "3D surface");
    m_status = new QLabel("Ready");
    m_status->setObjectName("statusMessage");
    m_status->setWordWrap(true);
    root->addWidget(m_status);
    m_progress = new QProgressBar;
    m_progress->setMaximumHeight(14);
    m_progress->setValue(0);
    root->addWidget(m_progress);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(180);
    connect(&m_debounce, &QTimer::timeout, this, &D2MWidget::startMask);
    connect(m_open, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getExistingDirectory(this, "Choose DICOM directory");
        if (!path.isEmpty())
            openDirectory(path);
    });
    connect(m_series, &QComboBox::currentIndexChanged, this, &D2MWidget::selectSeries);
    connect(m_load, &QPushButton::clicked, this, &D2MWidget::loadSelected);
    connect(m_mode, &QComboBox::currentIndexChanged, this, &D2MWidget::modeChanged);
    connect(m_secondary, &QComboBox::currentIndexChanged, this, [this] {
        m_skullRequested = false;
        requestMask();
    });
    connect(m_extractSkull, &QPushButton::clicked, this, [this] {
        m_skullRequested = true;
        requestMask();
    });
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        m_debounce.stop();
        m_segmentPending = false;
        m_worker->cancel();
        m_status->setText("Cancelling extraction…");
    });
    for (auto *field : {m_lower, m_upper, m_closing})
        connect(field, &QDoubleSpinBox::valueChanged, this, [this] { requestMask(); });
    for (auto *field : {m_largest, m_fill})
        connect(field, &QCheckBox::toggled, this, [this] { requestMask(); });
    for (auto *field : {m_windowLow, m_windowHigh})
        connect(field, &QDoubleSpinBox::valueChanged, this, [this] { updateWindow(); });
    connect(automatic, &QPushButton::clicked, this,
            [this] { m_lower->setValue(m_histogram.suggestedLower); });
    connect(m_undo, &QPushButton::clicked, this, [this] {
        if (!m_strokes.empty()) {
            m_strokes.pop_back();
            requestMask();
        }
    });
    connect(m_clear, &QPushButton::clicked, this, [this] {
        m_strokes.clear();
        requestMask();
    });
    connect(m_build, &QPushButton::clicked, this, [this] {
        setBusy("Building surface…");
        emit doBuild(m_isotropic->isChecked() ? m_isotropicSpacing->value() : 0, meshOptions());
    });
    connect(m_process, &QPushButton::clicked, this, &D2MWidget::applyProcessing);
    connect(m_save, &QPushButton::clicked, this, &D2MWidget::saveModel);
    connect(m_comparison, &QComboBox::currentIndexChanged, this, [this] {
        showMesh();
        updateActions();
    });
    auto processingChanged = [this] {
        m_processingDirty = true;
        updateActions();
        if (m_original)
            m_meshStats->setText(
                "Processing settings changed. Apply them to update the processed copy.");
    };
    connect(m_reduction, &QDoubleSpinBox::valueChanged, this, processingChanged);
    connect(m_smoothing, &QSpinBox::valueChanged, this, processingChanged);
    connect(m_center, &QCheckBox::toggled, this, processingChanged);
    connect(m_isotropic, &QCheckBox::toggled, this, [this] {
        invalidateMesh();
        updateActions();
    });
    connect(m_isotropicSpacing, &QDoubleSpinBox::valueChanged, this, [this] {
        invalidateMesh();
        updateActions();
    });
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 1 && m_original)
            showMesh(false);
    });
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &D2MWidget::doScan, m_worker, &DicomConverter::scan);
    connect(this, &D2MWidget::doLoad, m_worker, &DicomConverter::load);
    connect(this, &D2MWidget::doSegment, m_worker, &DicomConverter::segment);
    connect(this, &D2MWidget::doBuild, m_worker, &DicomConverter::build);
    connect(this, &D2MWidget::doProcess, m_worker, &DicomConverter::process);
    connect(this, &D2MWidget::doSave, m_worker, &DicomConverter::save);
    connect(m_worker, &DicomConverter::progress, this, [this](int p) {
        m_progress->setRange(0, 100);
        m_progress->setValue(p);
    });
    connect(m_worker, &DicomConverter::catalogReady, this,
            [this](const d2m::DicomCatalog &catalog) {
                m_catalog = catalog;
                m_busy = false;
                {
                    QSignalBlocker block(m_series);
                    m_series->clear();
                    for (const auto &s : catalog.series)
                        m_series->addItem(QString("%1 · %2 | %3 | %4 slices%5")
                                              .arg(QString::fromStdString(s.number),
                                                   QString::fromStdString(s.description),
                                                   QString::fromStdString(s.modality))
                                              .arg(s.slices.size())
                                              .arg(s.usable() ? "" : " | unavailable"));
                }
                int first = 0;
                for (size_t i = 0; i < catalog.series.size(); ++i)
                    if (catalog.series[i].usable()) {
                        first = int(i);
                        break;
                    }
                m_series->setCurrentIndex(first);
                selectSeries();
                m_status->setText(QString("Found %1 series. Select and load an anatomical volume; "
                                          "ignored %2 non-image files.")
                                      .arg(catalog.series.size())
                                      .arg(catalog.ignoredFiles));
                updateActions();
                emit seriesListed(int(catalog.series.size()));
            });
    connect(m_worker, &DicomConverter::volumeReady, this, &D2MWidget::receivedVolume);
    connect(m_worker, &DicomConverter::maskReady, this, [this](ImagePtr mask) {
        m_segmentRunning = false;
        if (m_segmentPending) {
            m_segmentPending = false;
            startMask();
            updateActions();
            return;
        }
        m_mask = mask;
        const auto summary = d2m::summarizeMask(mask);
        m_maskCurrent = summary.foreground > 0;
        for (auto *view : m_views)
            view->setMask(mask);
        QStringList touched;
        for (int face = 0; face < 6; ++face)
            if (summary.boundary[face])
                touched << QString("%1 (%2 voxels)")
                               .arg(directionLabel(mask, face / 2, face % 2 != 0))
                               .arg(summary.boundary[face]);
        m_maskInfo->setText(
            !summary.foreground ? "The mask is empty. Adjust the method or paint the target."
            : !touched.empty() ? "Mask reaches the scan boundary: " + touched.join(", ") +
                                     ". Check these end slices: the surface may be open or truncated."
                               : "Mask stays inside the scan boundaries. Review its anatomical border.");
        m_status->setText(
            "Mask updated. Review the overlay in all three planes before building a surface.");
        updateActions();
        emit maskUpdated();
    });
    connect(
        m_worker, &DicomConverter::meshReady, this, [this](MeshPtr original, MeshPtr processed) {
            m_busy = false;
            m_processingDirty = false;
            m_original = original;
            m_processed = processed;
            m_meshStats->setText(QString("Original: %1 triangles\nProcessed: %2 triangles\nBoth "
                                         "use millimetres; original geometry is retained.")
                                     .arg(original->GetNumberOfCells())
                                     .arg(processed->GetNumberOfCells()));
            m_status->setText(
                "Surface ready. Switch between original and processed before exporting.");
            updateActions();
            m_tabs->setCurrentIndex(1);
            showMesh();
        });
    connect(m_worker, &DicomConverter::saved, this, [this](const QString &path) {
        m_busy = false;
        m_status->setText("Saved " + path);
        updateActions();
    });
    connect(m_worker, &DicomConverter::failed, this, &D2MWidget::fail);
    m_thread.start();
    modeChanged();
    updateActions();
}
D2MWidget::~D2MWidget() {
    m_debounce.stop();
    m_worker->cancel();
    m_thread.quit();
    m_thread.wait();
}

void D2MWidget::openDirectory(const QString &directory) {
    if (m_busy || m_segmentRunning)
        return;
    m_volume = nullptr;
    m_mask = nullptr;
    m_maskCurrent = false;
    resetEdits();
    invalidateMesh();
    for (auto *view : m_views)
        view->setVolume(nullptr);
    m_catalog = {};
    m_series->clear();
    setBusy("Inspecting DICOM series…");
    emit doScan(directory);
}
void D2MWidget::selectSeries() {
    if (m_busy)
        return;
    const int index = m_series->currentIndex();
    if (index < 0 || index >= int(m_catalog.series.size())) {
        updateActions();
        return;
    }
    m_volume = nullptr;
    m_mask = nullptr;
    m_maskCurrent = false;
    resetEdits();
    invalidateMesh();
    for (auto *view : m_views)
        view->setVolume(nullptr);
    const auto &s = m_catalog.series[index];
    const auto &slice = s.slices.front();
    QString details = QString("%1 · %2 × %3 × %4 · ")
                          .arg(QString::fromStdString(s.modality))
                          .arg(slice.columns)
                          .arg(slice.rows)
                          .arg(s.slices.size());
    if (s.usable())
        details += QString("spacing %1 × %2 × %3 mm. Select this series explicitly to load it.")
                       .arg(s.spacing[0], 0, 'f', 3)
                       .arg(s.spacing[1], 0, 'f', 3)
                       .arg(s.spacing[2], 0, 'f', 3);
    else
        details += QString::fromStdString(s.error);
    m_details->setText(details);
    updateActions();
}
void D2MWidget::loadSelected() {
    const int index = m_series->currentIndex();
    if (index < 0 || index >= int(m_catalog.series.size()) || !m_catalog.series[index].usable())
        return;
    m_volume = nullptr;
    m_mask = nullptr;
    m_maskCurrent = false;
    resetEdits();
    invalidateMesh();
    for (auto *view : m_views)
        view->setVolume(nullptr);
    setBusy("Loading selected series…");
    emit doLoad(m_catalog.series[index]);
}
void D2MWidget::receivedVolume(ImagePtr image, const d2m::VolumeHistogram &histogram) {
    m_busy = false;
    m_volume = image;
    m_skullRequested = false;
    m_mask = nullptr;
    m_maskCurrent = false;
    resetEdits();
    m_histogram = histogram;
    m_initializing = true;
    m_lower->setValue(histogram.suggestedLower);
    m_upper->setValue(histogram.maximum);
    m_histogramView->setHistogram(histogram);
    const auto total = std::accumulate(histogram.bins.begin(), histogram.bins.end(), uint64_t(0));
    uint64_t sum = 0;
    double windowHigh = histogram.maximum;
    for (int i = 0; i < 256; ++i) {
        sum += histogram.bins[i];
        if (sum >= total * 0.995) {
            windowHigh =
                histogram.minimum + (i + 1) * (histogram.maximum - histogram.minimum) / 256;
            break;
        }
    }
    m_windowLow->setValue(histogram.minimum);
    m_windowHigh->setValue(std::max(histogram.minimum + 1, windowHigh));
    for (int axis = 0; axis < 3; ++axis) {
        m_views[axis]->setVolume(image);
        m_sliders[axis]->setRange(0, image->GetDimensions()[axis] - 1);
        m_sliders[axis]->setValue(image->GetDimensions()[axis] / 2);
        auto *label = findChild<QLabel *>(QString("planeLabel%1").arg(axis));
        label->setText(QString("%1 · %2 slices toward %3")
                           .arg(axis == 2   ? "XY acquisition"
                                : axis == 1 ? "XZ orthogonal"
                                            : "YZ orthogonal")
                           .arg(image->GetDimensions()[axis])
                           .arg(directionLabel(image, axis)));
    }
    const auto &series = m_catalog.series[m_series->currentIndex()];
    {
        QSignalBlocker block(m_secondary);
        m_secondary->clear();
        m_secondary->addItem("No supporting T2 · T1 only", -1);
        for (size_t i = 0; i < m_catalog.series.size(); ++i) {
            const auto &s = m_catalog.series[i];
            if (s.usable() && s.modality == "MR" && s.uid != series.uid &&
                !series.studyUid.empty() && s.studyUid == series.studyUid)
                m_secondary->addItem(QString::fromStdString(s.number + " · " + s.description), int(i));
        }
    }
    // Brain extraction is explicit: selecting it starts a potentially long inference.
    m_mode->setCurrentIndex(series.modality == "MR" ? 1 : 0);
    modeChanged();
    if (series.modality == "CT")
        m_lower->setValue(std::clamp(400.0, histogram.minimum, histogram.maximum));
    const double *spacing = image->GetSpacing();
    const double anisotropy =
        *std::max_element(spacing, spacing + 3) / *std::min_element(spacing, spacing + 3);
    if (anisotropy > 3)
        m_details->setText(m_details->text() +
                           QString(" Thick/anisotropic slices (%1 mm through-plane): small details "
                                   "cannot be recovered by smoothing.")
                               .arg(spacing[2], 0, 'f', 2));
    m_initializing = false;
    updateWindow();
    m_tabs->setCurrentIndex(0);
    requestMask();
    emit volumeLoaded();
}
void D2MWidget::resetEdits() {
    m_strokes.clear();
    for (auto &strokes : m_modeStrokes)
        strokes.clear();
}
void D2MWidget::modeChanged() {
    const bool initializing = m_initializing;
    m_initializing = true;
    const int mode = m_mode->currentIndex();
    if (mode != m_previousMode) {
        m_modeStrokes[m_previousMode] = m_strokes;
        m_strokes = m_modeStrokes[mode];
        m_previousMode = mode;
    }
    const bool brain = mode == 3;
    const bool skull = mode == 4;
    const bool anatomical = brain || skull;
    m_skullControls->setVisible(skull);
    m_brush->setMinimum(skull ? 1.0 : 0.1);
    if (m_segmentRunning)
        m_worker->cancel();
    m_largest->setChecked(mode == 1);
    m_fill->setChecked(mode == 1);
    m_closing->setValue(mode == 1 ? 1.0 : 0.0);
    m_largest->setEnabled(mode != 2 && !anatomical);
    m_lower->setEnabled(!anatomical);
    m_upper->setEnabled(!anatomical);
    m_fill->setEnabled(!anatomical);
    m_closing->setEnabled(!anatomical);
    findChild<QPushButton *>("autoThreshold")->setEnabled(!anatomical);
    m_hint->setText(
        mode == 0 ? "Intensity range only. CT thresholds are not universal for MRI."
        : mode == 1
            ? "Approximate head outline: threshold, gap closing, largest region and slice-hole "
              "filling. Check the mask; this is not automatic brain or skull extraction."
        : skull
            ? "Experimental CHARM bone estimate from T1 and optional T2 MRI (can take 15–30 minutes). "
              "Atlas-based boundaries need review. Includes bone in the scanned neck and jaw. "
              "Thick or cropped scans produce incomplete, approximate surfaces. This does not recover "
              "missing facial bones. Paint corrections after estimation."
        : brain
            ? "SynthStrip no-CSF v1 extracts the brain locally on CPU (usually a few minutes). "
              "Review all planes; paint corrections after extraction. Edits use the cached mask. "
              "Model weights and the optional Pixi brain environment must be installed once."
            : "Place foreground seeds inside the target. Growth follows connected voxels within "
              "the intensity range. Erase/paint barriers to stop leakage; brain boundaries require "
              "manual review.");
    if (mode == 2)
        m_edit->setChecked(true);
    m_initializing = initializing;
    requestMask();
}
d2m::SegmentationOptions D2MWidget::segmentationOptions() const {
    d2m::SegmentationOptions o;
    o.mode = static_cast<d2m::SegmentationMode>(m_mode->currentIndex());
    o.lower = m_lower->value();
    o.upper = m_upper->value();
    o.closingRadiusMm = m_closing->value();
    o.largestComponent = m_largest->isChecked();
    o.fillSliceHoles = m_fill->isChecked();
    o.strokes = m_strokes;
    return o;
}
MeshOptions D2MWidget::meshOptions() const {
    return {m_reduction->value() / 100, m_smoothing->value(), m_center->isChecked()};
}
void D2MWidget::requestMask() {
    if (m_initializing || !m_volume)
        return;
    m_maskCurrent = false;
    m_mask = nullptr;
    for (auto *view : m_views)
        view->setMask(nullptr);
    invalidateMesh();
    m_maskInfo->setText("Mask is being updated.");
    m_histogramView->setThresholds(m_lower->value(), m_upper->value());
    if (m_segmentRunning)
        m_segmentPending = true;
    else
        m_debounce.start();
    m_status->setText("Updating mask…");
    if (m_mode->currentIndex() == 4 && !m_skullRequested) {
        m_debounce.stop();
        m_status->setText("Choose the supporting T2 series, then click Estimate skull from MRI. "
                          "A T1-weighted MR series must be loaded as the primary image.");
        m_maskInfo->setText("MRI skull estimation is experimental; no current mask.");
        updateActions();
        return;
    }
    updateActions();
}
void D2MWidget::startMask() {
    if (!m_volume || m_busy)
        return;
    if (m_mode->currentIndex() == 4 && !m_skullRequested)
        return;
    if (m_segmentRunning) {
        m_segmentPending = true;
        return;
    }
    m_segmentRunning = true;
    m_segmentPending = false;
    m_worker->resetCancellation();
    if (m_mode->currentIndex() >= 3) {
        m_status->setText(m_mode->currentIndex() == 3
            ? "Extracting brain locally with SynthStrip, or applying cached corrections…"
            : "Estimating MRI bone locally with CHARM, or applying cached corrections…");
        m_progress->setRange(0, 0);
    }
    updateActions();
    const int auxiliary = m_secondary->currentData().toInt();
    emit doSegment(segmentationOptions(), m_mode->currentIndex() == 4 && auxiliary >= 0
        ? m_catalog.series[auxiliary] : d2m::DicomSeries{});
}
void D2MWidget::invalidateMesh() {
    m_original = nullptr;
    m_processed = nullptr;
    m_actor->SetVisibility(false);
    if (m_tabs->currentIndex() == 1 && isVisible())
        m_meshView->renderWindow()->Render();
    m_meshStats->setText("Mask changed. Build the surface to update the 3D preview.");
}
void D2MWidget::setBusy(const QString &message) {
    m_busy = true;
    m_status->setText(message);
    m_progress->setRange(0, 0);
    updateActions();
}
void D2MWidget::updateActions() {
    const bool working = m_busy || m_segmentRunning;
    const int index = m_series->currentIndex();
    const bool valid =
        index >= 0 && index < int(m_catalog.series.size()) && m_catalog.series[index].usable();
    m_open->setEnabled(!working);
    m_series->setEnabled(!working && !m_catalog.series.empty());
    m_load->setEnabled(!working && valid);
    m_segmentationGroup->setEnabled(!m_busy && bool(m_volume));
    m_cancel->setEnabled(m_segmentRunning && m_mode->currentIndex() >= 3);
    m_extractSkull->setEnabled(!working && bool(m_volume) && valid &&
                               m_catalog.series[index].modality == "MR");
    m_secondary->setEnabled(!working);
    m_build->setEnabled(!working && m_maskCurrent);
    m_process->setEnabled(!working && bool(m_original));
    m_save->setEnabled(!working && bool(m_original) &&
                       (m_comparison->currentIndex() == 0 || !m_processingDirty));
    m_comparison->setEnabled(!working && bool(m_original));
    m_undo->setEnabled(!m_strokes.empty());
    m_clear->setEnabled(!m_strokes.empty());
    m_reduction->setEnabled(!working);
    m_smoothing->setEnabled(!working);
    m_center->setEnabled(!working);
    m_isotropic->setEnabled(!working);
    m_isotropicSpacing->setEnabled(!working && m_isotropic->isChecked());
    if (!working) {
        m_progress->setRange(0, 100);
        m_progress->setValue(m_volume ? 100 : 0);
    }
}
void D2MWidget::updateWindow() {
    for (auto *view : m_views)
        view->setWindow(m_windowLow->value(), m_windowHigh->value());
}
void D2MWidget::applyProcessing() {
    setBusy("Processing a copy of the original surface…");
    emit doProcess(meshOptions());
}
void D2MWidget::showMesh(bool resetCamera) {
    const auto mesh = m_comparison->currentIndex() == 0 ? m_original : m_processed;
    if (!mesh)
        return;
    m_mapper->SetInputData(mesh);
    m_actor->SetVisibility(true);
    if (resetCamera)
        m_renderer->ResetCamera();
    if (m_tabs->currentIndex() == 1 && isVisible())
        m_meshView->renderWindow()->Render();
}
void D2MWidget::saveModel() {
    QString path = QFileDialog::getSaveFileName(this, "Export visible surface",
                                                m_mode->currentIndex() == 4 ? "skull-mri-estimate.stl" : "surface.stl",
                                                "STL (*.stl);;OBJ (*.obj);;PLY (*.ply)");
    if (path.isEmpty())
        return;
    setBusy("Exporting visible surface…");
    emit doSave(path, m_comparison->currentIndex() == 0);
}
void D2MWidget::fail(const QString &operation, const QString &message) {
    m_busy = false;
    if (operation == "segment") {
        m_segmentRunning = false;
        if (m_segmentPending) {
            m_segmentPending = false;
            startMask();
            updateActions();
            return;
        }
        m_maskInfo->setText("No current mask. Check the extraction settings before retrying.");
    }
    m_status->setText(message);
    updateActions();
}
