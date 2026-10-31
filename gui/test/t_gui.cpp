#include "d2mWidget.h"
#include "dicomFixture.h"
#include "sliceView.h"
#include "anatomyExtractor.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QSurfaceFormat>
#include <QTabWidget>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <future>
#include <cstring>
#include <QVTKOpenGLNativeWidget.h>
#include <QtTest>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkRenderWindow.h>

namespace {
// A separate process exercises the same binary protocol without neural weights.
int brainFixture(const QStringList &args) {
    const auto work = args.value(args.indexOf("--work-dir") + 1);
    const auto model = args.value(args.indexOf("--model") + 1);
    const auto mode = args.value(2);
    if (mode == "sleep")
        QThread::sleep(30);
    QFile geometry(work + "/geometry.json");
    QFile copy(model + ".geometry");
    if (!geometry.open(QIODevice::ReadOnly) || !copy.open(QIODevice::WriteOnly))
        return 1;
    const auto metadata = geometry.readAll();
    copy.write(metadata);
    if (args.contains("--secondary")) {
        QFile auxiliary(args.value(args.indexOf("--secondary") + 1) + "/geometry.json");
        QFile auxiliaryCopy(model + ".secondary");
        if (!auxiliary.open(QIODevice::ReadOnly) || !auxiliaryCopy.open(QIODevice::WriteOnly))
            return 5;
        auxiliaryCopy.write(auxiliary.readAll());
    }
    QFile count(model + ".calls");
    if (!count.open(QIODevice::Append))
        return 2;
    count.write("1");
    QFile input(work + "/input.f32");
    if (!input.open(QIODevice::ReadOnly))
        return 3;
    const auto values = input.readAll();
    QByteArray mask(values.size() / 4, 0), distance(values.size(), 0);
    for (qsizetype p = 0; p < mask.size(); ++p) {
        float value;
        std::memcpy(&value, values.constData() + p * 4, 4);
        mask[p] = value > 50;
        const float d = mask[p] ? -1 : 100;
        std::memcpy(distance.data() + p * 4, &d, 4);
    }
    if (mode == "truncate")
        mask.chop(1);
    if (mode == "field" || mode == "misaligned") {
        auto field = QJsonDocument::fromJson(metadata).object();
        auto dims = field["dimensions"].toArray();
        auto spacing = field["spacing"].toArray();
        int original[3], d[3];
        for (int a = 0; a < 3; ++a) {
            original[a] = dims[a].toInt();
            d[a] = original[a] * 2;
            dims[a] = d[a];
            spacing[a] = spacing[a].toDouble() / 2;
        }
        field["dimensions"] = dims;
        field["spacing"] = spacing;
        if (mode == "misaligned") {
            auto origin = field["originLPS"].toArray();
            origin[0] = 1000000;
            field["originLPS"] = origin;
        }
        QFile header(work + "/distance-geometry.json");
        if (!header.open(QIODevice::WriteOnly))
            return 6;
        header.write(QJsonDocument(field).toJson());
        distance.resize(d[0] * d[1] * d[2] * 4);
        for (int z = 0; z < d[2]; ++z)
            for (int y = 0; y < d[1]; ++y)
                for (int x = 0; x < d[0]; ++x) {
                    const int p = ((z / 2) * original[1] + y / 2) * original[0] + x / 2;
                    const float value = mask[p] ? -1 : 100;
                    std::memcpy(distance.data() + ((z * d[1] + y) * d[0] + x) * 4, &value, 4);
                }
    }
    QFile m(work + "/mask.u8"), d(work + "/distance.f32");
    if (!m.open(QIODevice::WriteOnly) || !d.open(QIODevice::WriteOnly))
        return 4;
    m.write(mask);
    d.write(distance);
    return 0;
}
SegmentationRuntime fixtureRuntime(const QString &model, const QString &mode = "normal") {
    QFile file(model);
    if (!file.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot create test model placeholder.");
    file.write("fixture");
    const auto executable = QCoreApplication::applicationFilePath();
    return {executable, executable, model, {"--brain-worker-fixture", mode}};
}
} // namespace

class GuiTest : public QObject {
    Q_OBJECT
  private slots:
    void brainBridgePreservesGeometryAndRejectsBrokenOutput() {
        QTemporaryDir directory;
        auto volume = ImagePtr::New();
        volume->SetDimensions(4, 5, 6);
        volume->SetSpacing(0.7, 1.2, 4);
        volume->SetOrigin(12, 24, 36);
        volume->SetDirectionMatrix(0, -1, 0, 1, 0, 0, 0, 0, 1);
        volume->AllocateScalars(VTK_FLOAT, 1);
        std::fill_n(static_cast<float *>(volume->GetScalarPointer()), 120, 0);
        *static_cast<float *>(volume->GetScalarPointer(1, 2, 3)) = 100;
        auto runtime = fixtureRuntime(directory.filePath("model with an apostrophe's.pt"));
        std::atomic_bool cancelled{false};
        const auto result = extractAnatomy(volume, runtime, cancelled);
        QCOMPARE(result.mask->GetScalarComponentAsDouble(1, 2, 3, 0), 1.0);
        QCOMPARE(result.mask->GetScalarComponentAsDouble(2, 1, 3, 0), 0.0);
        QCOMPARE(result.mask->GetOrigin()[1], 24.0);
        QCOMPARE(result.mask->GetSpacing()[2], 4.0);
        QFile geometry(runtime.model + ".geometry");
        QVERIFY(geometry.open(QIODevice::ReadOnly));
        const auto metadata = QJsonDocument::fromJson(geometry.readAll()).object();
        QCOMPARE(metadata["originLPS"].toArray()[1].toDouble(), 24.0);
        QCOMPARE(metadata["directionLPS"].toArray()[1].toDouble(), -1.0);
        runtime.prefixArguments[1] = "field";
        const auto upsampled = extractAnatomy(volume, runtime, cancelled, volume);
        QCOMPARE(upsampled.distance->GetDimensions()[0], 8);
        QCOMPARE(upsampled.distance->GetSpacing()[0], .35);
        QVERIFY(QFileInfo::exists(runtime.model + ".secondary"));
        runtime.prefixArguments[1] = "misaligned";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, extractAnatomy(volume, runtime, cancelled));
        runtime.prefixArguments[1] = "truncate";
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, extractAnatomy(volume, runtime, cancelled));
        runtime.prefixArguments[1] = "sleep";
        QElapsedTimer timer;
        timer.start();
        auto cancel = std::async(std::launch::async, [&] {
            QThread::msleep(250);
            cancelled.store(true);
        });
        QVERIFY_THROWS_EXCEPTION(std::runtime_error, extractAnatomy(volume, runtime, cancelled));
        QVERIFY(timer.elapsed() < 5000);
    }
    void brainCorrectionsUseCacheAndReloadInvalidatesIt() {
        fixture::Directory directory;
        fixture::Slice slice;
        slice.pixels.assign(64, 0);
        slice.pixels[4 * 8 + 4] = 100;
        fixture::stack(directory.path, slice, 5, 1);
        const auto runtime =
            fixtureRuntime(QString::fromStdString((directory.path / "model").string()));
        DicomConverter converter(runtime);
        QSignalSpy masks(&converter, &DicomConverter::maskReady);
        QSignalSpy failures(&converter, &DicomConverter::failed);
        const auto series = d2m::scanDicomDirectory(directory.path.string()).series[0];
        converter.load(series);
        d2m::SegmentationOptions options;
        options.mode = d2m::SegmentationMode::Brain;
        converter.segment(options);
        QCOMPARE(failures.count(), 0);
        QCOMPARE(masks.count(), 1);
        const auto first = qvariant_cast<ImagePtr>(masks[0][0]);
        options.strokes = {{{4, 4, 2}, 0.1, false}};
        converter.segment(options);
        QCOMPARE(qvariant_cast<ImagePtr>(masks[1][0])->GetScalarComponentAsDouble(4, 4, 2, 0), 0.0);
        QCOMPARE(first->GetScalarComponentAsDouble(4, 4, 2, 0), 1.0);
        QFile calls(runtime.model + ".calls");
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll(), QByteArray("1"));
        calls.close();
        converter.load(series);
        converter.segment(options);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll(), QByteArray("11"));
        QCOMPARE(failures.count(), 0);
    }
    void realBrainExtraction() {
        const auto path = qEnvironmentVariable("D2M_BRAIN_TEST_DICOM");
        if (path.isEmpty())
            QSKIP(
                "Set D2M_BRAIN_TEST_DICOM and D2M_BRAIN_TEST_SERIES for optional local inference.");
        const auto uid = qEnvironmentVariable("D2M_BRAIN_TEST_SERIES");
        const auto catalog = d2m::scanDicomDirectory(path.toStdString());
        const auto series = std::find_if(catalog.series.begin(), catalog.series.end(),
                                         [&](const auto &s) { return s.uid == uid.toStdString(); });
        QVERIFY(series != catalog.series.end());
        DicomConverter converter;
        QSignalSpy masks(&converter, &DicomConverter::maskReady);
        QSignalSpy failures(&converter, &DicomConverter::failed);
        QSignalSpy meshes(&converter, &DicomConverter::meshReady);
        converter.load(*series);
        d2m::SegmentationOptions options;
        options.mode = d2m::SegmentationMode::Brain;
        converter.segment(options);
        QVERIFY2(failures.empty(),
                 failures.empty() ? "" : qPrintable(failures.last()[1].toString()));
        QCOMPARE(masks.count(), 1);
        const auto mask = qvariant_cast<ImagePtr>(masks[0][0]);
        QVERIFY(d2m::summarizeMask(mask).foreground > 0);
        converter.build(1, {});
        QVERIFY2(failures.empty(),
                 failures.empty() ? "" : qPrintable(failures.last()[1].toString()));
        QCOMPARE(meshes.count(), 1);
        const auto mesh = qvariant_cast<MeshPtr>(meshes[0][0]);
        QVERIFY(mesh->GetNumberOfCells() > 0);
        const auto destination = qEnvironmentVariable("D2M_BRAIN_TEST_OUTPUT");
        if (!destination.isEmpty()) {
            converter.save(destination, true);
            QVERIFY(failures.empty());
        }
    }
    void skullCachesBySupportingSeriesAndRejectsOtherStudies() {
        fixture::Directory directory;
        fixture::Slice slice;
        slice.pixels.assign(64, 0);
        slice.pixels[4 * 8 + 4] = 100;
        fixture::stack(directory.path, slice, 5, 1);
        for (int i = 2; i <= 3; ++i) {
            const auto folder = directory.path / std::to_string(i);
            std::filesystem::create_directory(folder);
            auto auxiliary = slice;
            auxiliary.uid += "." + std::to_string(i);
            auxiliary.number = std::to_string(i);
            fixture::stack(folder, auxiliary, 5, 1);
        }
        const auto series = d2m::scanDicomDirectory(directory.path.string()).series;
        const auto backend =
            fixtureRuntime(QString::fromStdString((directory.path / "model").string()));
        DicomConverter converter(backend, backend);
        QSignalSpy masks(&converter, &DicomConverter::maskReady);
        QSignalSpy failures(&converter, &DicomConverter::failed);
        converter.load(series[0]);
        d2m::SegmentationOptions options;
        options.mode = d2m::SegmentationMode::SkullMRI;
        converter.segment(options, series[1]);
        QCOMPARE(failures.count(), 0);
        options.strokes = {{{4, 4, 2}, 0.1, false}};
        converter.segment(options, series[1]);
        QCOMPARE(masks.count(), 2);
        QCOMPARE(qvariant_cast<ImagePtr>(masks[1][0])->GetScalarComponentAsDouble(4, 4, 2, 0), 0.0);
        QFile calls(backend.model + ".calls");
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll(), QByteArray("1"));
        calls.close();
        converter.segment(options, series[2]);
        QVERIFY(calls.open(QIODevice::ReadOnly));
        QCOMPARE(calls.readAll(), QByteArray("11"));
        auto unrelated = series[2];
        unrelated.studyUid += ".999";
        converter.segment(options, unrelated);
        QCOMPARE(failures.count(), 1);
        QVERIFY(failures.last()[1].toString().contains("same DICOM study"));
        converter.load(series[0]);
        converter.segment(options, series[0]);
        QCOMPARE(failures.count(), 2);
    }
    void realSkullExtraction() {
        const auto path = qEnvironmentVariable("D2M_SKULL_TEST_DICOM");
        if (path.isEmpty())
            QSKIP(
                "Set D2M_SKULL_TEST_DICOM and D2M_SKULL_TEST_SERIES for optional local inference.");
        const auto catalog = d2m::scanDicomDirectory(path.toStdString());
        auto find = [&](const QString &uid) {
            for (const auto &s : catalog.series)
                if (s.uid == uid.toStdString())
                    return s;
            return d2m::DicomSeries{};
        };
        const auto primary = find(qEnvironmentVariable("D2M_SKULL_TEST_SERIES"));
        const auto secondary = find(qEnvironmentVariable("D2M_SKULL_TEST_T2"));
        QVERIFY(!primary.uid.empty());
        auto backend = SegmentationRuntime::skull();
        const auto driver = qEnvironmentVariable("D2M_SKULL_TEST_DRIVER");
        if (!driver.isEmpty())
            backend.prefixArguments << driver;
        DicomConverter converter(SegmentationRuntime::defaults(), backend);
        QSignalSpy failures(&converter, &DicomConverter::failed);
        QSignalSpy masks(&converter, &DicomConverter::maskReady);
        QSignalSpy meshes(&converter, &DicomConverter::meshReady);
        converter.load(primary);
        d2m::SegmentationOptions options;
        options.mode = d2m::SegmentationMode::SkullMRI;
        converter.segment(options, secondary);
        QVERIFY2(failures.empty(),
                 failures.empty() ? "" : qPrintable(failures.last()[1].toString()));
        QCOMPARE(masks.count(), 1);
        converter.build(1, {});
        QVERIFY2(failures.empty(),
                 failures.empty() ? "" : qPrintable(failures.last()[1].toString()));
        QCOMPARE(meshes.count(), 1);
        const auto output = qEnvironmentVariable("D2M_SKULL_TEST_OUTPUT");
        if (!output.isEmpty()) {
            converter.save(output, true);
            QVERIFY(failures.empty());
        }
    }
    void nativeSurfacePreview() {
        if (QGuiApplication::platformName() == "offscreen")
            QSKIP("Native OpenGL rendering is checked separately on a display.");
        fixture::Directory directory;
        fixture::Slice slice;
        slice.pixels.assign(64, 0);
        for (int y = 2; y < 6; ++y)
            for (int x = 2; x < 6; ++x)
                slice.pixels[y * 8 + x] = 100;
        fixture::stack(directory.path, slice, 5, 1);
        D2MWidget widget;
        widget.show();
        QVERIFY(QTest::qWaitForWindowExposed(&widget));
        auto *panel = widget.findChild<QScrollArea *>();
        QVERIFY(panel);
        QCOMPARE(panel->horizontalScrollBar()->maximum(), 0);
        QSignalSpy listed(&widget, &D2MWidget::seriesListed);
        QSignalSpy masks(&widget, &D2MWidget::maskUpdated);
        widget.openDirectory(QString::fromStdString(directory.path.string()));
        QTRY_COMPARE_WITH_TIMEOUT(listed.count(), 1, 10000);
        widget.findChild<QPushButton *>("loadSeries")->click();
        QTRY_VERIFY_WITH_TIMEOUT(masks.count() > 0, 10000);
        widget.findChild<QPushButton *>("buildSurface")->click();
        QTRY_VERIFY_WITH_TIMEOUT(widget.findChild<QPushButton *>("exportSurface")->isEnabled(),
                                 10000);
        QCOMPARE(widget.findChild<QTabWidget *>("previewTabs")->currentIndex(), 1);
        auto *view = widget.findChild<QVTKOpenGLNativeWidget *>("surfaceView");
        QTRY_VERIFY_WITH_TIMEOUT(view->isValid(), 10000);
        view->renderWindow()->Render();
        const auto image = view->grabFramebuffer();
        QVERIFY(!image.isNull());
        int foreground = 0;
        for (int y = 0; y < image.height(); y += 8)
            for (int x = 0; x < image.width(); x += 8)
                if (image.pixelColor(x, y).red() > 100)
                    ++foreground;
        QVERIFY2(foreground > 10, "The 3D preview framebuffer contains no visible surface.");
        widget.findChild<QComboBox *>("segmentationMode")->setCurrentIndex(4);
        QTest::qWait(50);
        QVERIFY(widget.findChild<QPushButton *>("extractSkull")->isVisible());
        QCOMPARE(panel->horizontalScrollBar()->maximum(), 0);
        const auto cleared = view->grabFramebuffer();
        int stale = 0;
        for (int y = 0; y < cleared.height(); y += 8)
            for (int x = 0; x < cleared.width(); x += 8)
                stale += cleared.pixelColor(x, y).red() > 100;
        QCOMPARE(stale, 0);
    }
    void defaultProcessingPreservesDetails() {
        D2MWidget widget;
        QCOMPARE(widget.findChild<QDoubleSpinBox *>("meshReduction")->value(), 0.0);
        QCOMPARE(widget.findChild<QSpinBox *>("smoothingIterations")->value(), 0);
        QVERIFY(!widget.findChild<QCheckBox *>("centerMesh")->isChecked());
        QVERIFY(!widget.findChild<QPushButton *>("buildSurface")->isEnabled());
        QVERIFY(!widget.findChild<QPushButton *>("exportSurface")->isEnabled());
    }
    void brainModeDisablesThresholdsAndExplainsMissingRuntime() {
        fixture::Directory directory;
        fixture::Slice slice;
        slice.pixels.assign(64, 0);
        slice.pixels[4 * 8 + 4] = 100;
        fixture::stack(directory.path, slice, 5, 1);
        const auto previous = qgetenv("D2M_BRAIN_PYTHON");
        qputenv("D2M_BRAIN_PYTHON", "/nonexistent/d2m-brain-python");
        D2MWidget widget;
        if (previous.isNull())
            qunsetenv("D2M_BRAIN_PYTHON");
        else
            qputenv("D2M_BRAIN_PYTHON", previous);
        QSignalSpy listed(&widget, &D2MWidget::seriesListed);
        QSignalSpy masks(&widget, &D2MWidget::maskUpdated);
        widget.openDirectory(QString::fromStdString(directory.path.string()));
        QTRY_COMPARE_WITH_TIMEOUT(listed.count(), 1, 10000);
        widget.findChild<QPushButton *>("loadSeries")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!masks.empty(), 10000);
        auto *method = widget.findChild<QComboBox *>("segmentationMode");
        method->setCurrentIndex(3);
        QVERIFY(!widget.findChild<QDoubleSpinBox *>("lowerThreshold")->isEnabled());
        QVERIFY(!widget.findChild<QPushButton *>("autoThreshold")->isEnabled());
        QVERIFY(!widget.findChild<QCheckBox *>("fillHoles")->isEnabled());
        QTRY_VERIFY_WITH_TIMEOUT(
            widget.findChild<QLabel *>("statusMessage")->text().contains("pixi install -e brain"),
            10000);
        QVERIFY(!widget.findChild<QPushButton *>("buildSurface")->isEnabled());
        const int count = masks.count();
        method->setCurrentIndex(0);
        QTRY_VERIFY_WITH_TIMEOUT(masks.count() > count, 10000);
        QVERIFY(widget.findChild<QDoubleSpinBox *>("lowerThreshold")->isEnabled());
    }
    void skullModeWaitsForExplicitEstimateAndListsSupportingSeries() {
        fixture::Directory directory;
        fixture::Slice slice;
        fixture::stack(directory.path, slice, 3, 1);
        std::filesystem::create_directory(directory.path / "t2");
        slice.uid += ".2";
        slice.number = "2";
        slice.description = "Synthetic T2";
        fixture::stack(directory.path / "t2", slice, 3, 1);
        const auto previous = qgetenv("D2M_SKULL_PYTHON");
        qputenv("D2M_SKULL_PYTHON", "/nonexistent/d2m-skull-python");
        D2MWidget widget;
        if (previous.isNull())
            qunsetenv("D2M_SKULL_PYTHON");
        else
            qputenv("D2M_SKULL_PYTHON", previous);
        QSignalSpy listed(&widget, &D2MWidget::seriesListed);
        QSignalSpy masks(&widget, &D2MWidget::maskUpdated);
        widget.openDirectory(QString::fromStdString(directory.path.string()));
        QTRY_COMPARE_WITH_TIMEOUT(listed.count(), 1, 10000);
        widget.findChild<QPushButton *>("loadSeries")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!masks.empty(), 10000);
        widget.findChild<SliceView *>("sliceView2")->brushAt(3, 3, 1, true);
        const int beforePaint = masks.count();
        QTRY_VERIFY_WITH_TIMEOUT(masks.count() > beforePaint, 10000);
        QVERIFY(widget.findChild<QPushButton *>("undoStroke")->isEnabled());
        widget.findChild<QComboBox *>("segmentationMode")->setCurrentIndex(4);
        QVERIFY(!widget.findChild<QPushButton *>("undoStroke")->isEnabled());
        widget.findChild<QComboBox *>("segmentationMode")->setCurrentIndex(1);
        QVERIFY(widget.findChild<QPushButton *>("undoStroke")->isEnabled());
        widget.findChild<QComboBox *>("segmentationMode")->setCurrentIndex(4);
        QVERIFY(widget.findChild<QPushButton *>("extractSkull")->isEnabled());
        QVERIFY(!widget.findChild<QPushButton *>("buildSurface")->isEnabled());
        QVERIFY(widget.findChild<QLabel *>("statusMessage")->text().contains("click Estimate"));
        QCOMPARE(widget.findChild<QComboBox *>("skullSupportingSeries")->count(), 2);
        QVERIFY(!widget.findChild<QDoubleSpinBox *>("lowerThreshold")->isEnabled());
        widget.findChild<QPushButton *>("extractSkull")->click();
        QTRY_VERIFY_WITH_TIMEOUT(
            widget.findChild<QLabel *>("statusMessage")->text().contains("pixi run skull-setup"),
            10000);
    }
    void listsAllSeriesAndRejectsLocalizerWithoutAutoLoading() {
        fixture::Directory directory;
        fixture::Slice slice;
        slice.description = "Locator 3Axis";
        fixture::stack(directory.path, slice);
        std::filesystem::create_directory(directory.path / "valid");
        slice.description = "Synthetic T2";
        slice.uid += ".2";
        slice.number = "2";
        fixture::stack(directory.path / "valid", slice);
        D2MWidget widget;
        QSignalSpy listed(&widget, &D2MWidget::seriesListed);
        QSignalSpy loaded(&widget, &D2MWidget::volumeLoaded);
        widget.openDirectory(QString::fromStdString(directory.path.string()));
        QTRY_COMPARE_WITH_TIMEOUT(listed.count(), 1, 10000);
        auto *selector = widget.findChild<QComboBox *>("seriesSelector");
        QCOMPARE(selector->count(), 2);
        QCOMPARE(loaded.count(), 0);
        QCOMPARE(selector->currentIndex(), 1);
        QVERIFY(widget.findChild<QPushButton *>("loadSeries")->isEnabled());
        selector->setCurrentIndex(0);
        QVERIFY(!widget.findChild<QPushButton *>("loadSeries")->isEnabled());
        QVERIFY(widget.findChild<QLabel *>("seriesDetails")->text().contains("Localizer"));
    }
    void loadsVolumeUpdatesThreePlanesAndInvalidatesStaleMask() {
        fixture::Directory directory;
        fixture::Slice slice;
        slice.pixels.assign(64, 0);
        for (int y = 2; y < 6; ++y)
            for (int x = 2; x < 6; ++x)
                slice.pixels[y * 8 + x] = 100;
        fixture::stack(directory.path, slice, 3);
        D2MWidget widget;
        QSignalSpy listed(&widget, &D2MWidget::seriesListed);
        QSignalSpy loaded(&widget, &D2MWidget::volumeLoaded);
        QSignalSpy masks(&widget, &D2MWidget::maskUpdated);
        widget.openDirectory(QString::fromStdString(directory.path.string()));
        QTRY_COMPARE_WITH_TIMEOUT(listed.count(), 1, 10000);
        widget.findChild<QPushButton *>("loadSeries")->click();
        QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(masks.count() > 0, 10000);
        for (int axis = 0; axis < 3; ++axis)
            QVERIFY(
                !widget.findChild<SliceView *>(QString("sliceView%1").arg(axis))->image().isNull());
        QVERIFY(widget.findChild<QPushButton *>("buildSurface")->isEnabled());
        widget.findChild<QDoubleSpinBox *>("lowerThreshold")->setValue(80);
        QVERIFY(!widget.findChild<QPushButton *>("buildSurface")->isEnabled());
        const int previous = masks.count();
        QTRY_VERIFY_WITH_TIMEOUT(masks.count() > previous, 10000);
        widget.findChild<QComboBox *>("segmentationMode")->setCurrentIndex(2);
        QTRY_VERIFY_WITH_TIMEOUT(
            widget.findChild<QLabel *>("statusMessage")->text().contains("foreground seeds"),
            10000);
        QVERIFY(!widget.findChild<QPushButton *>("buildSurface")->isEnabled());
    }
    void previewDisplaysCorrectPlaneAndMaskOverlay() {
        auto volume = ImagePtr::New();
        volume->SetDimensions(3, 4, 5);
        volume->AllocateScalars(VTK_FLOAT, 1);
        for (int z = 0; z < 5; ++z)
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 3; ++x)
                    *static_cast<float *>(volume->GetScalarPointer(x, y, z)) = x + 10 * y + 100 * z;
        SliceView view(2);
        view.setWindow(0, 500);
        view.setVolume(volume);
        view.setSlice(1);
        QCOMPARE(view.image().size(), QSize(3, 4));
        const auto gray = view.image().pixelColor(2, 0);
        QCOMPARE(gray.red(), int(132.0 * 255 / 500));
        auto mask = ImagePtr::New();
        mask->CopyStructure(volume);
        mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        std::fill_n(static_cast<unsigned char *>(mask->GetScalarPointer()), 60, 0);
        *static_cast<unsigned char *>(mask->GetScalarPointer(2, 3, 1)) = 1;
        view.setMask(mask);
        QVERIFY(view.image().pixelColor(2, 0).green() > gray.green());
        view.setOverlay(false);
        QCOMPARE(view.image().pixelColor(2, 0), gray);
    }
    void paintingMapsWidgetCoordinatesBackToVoxels() {
        auto volume = ImagePtr::New();
        volume->SetDimensions(10, 10, 10);
        volume->AllocateScalars(VTK_FLOAT, 1);
        std::fill_n(static_cast<float *>(volume->GetScalarPointer()), 1000, 0);
        SliceView view(2);
        view.resize(240, 240);
        view.setVolume(volume);
        view.setSlice(4);
        view.setEditing(true);
        view.show();
        QTest::qWait(20);
        QSignalSpy brush(&view, &SliceView::brushAt);
        QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, QPoint(120, 120));
        QCOMPARE(brush.count(), 1);
        const auto args = brush.takeFirst();
        QCOMPARE(args[2].toInt(), 4);
        QVERIFY(args[0].toInt() >= 4 && args[0].toInt() <= 5);
        QVERIFY(args[3].toBool());
        QTest::mouseClick(&view, Qt::RightButton, Qt::NoModifier, QPoint(120, 120));
        QVERIFY(!brush.takeFirst()[3].toBool());
    }
    void processingIsNonCumulativeAndExportsTheSelectedCopy() {
        fixture::Directory directory;
        fixture::Slice slice;
        slice.rows = 20;
        slice.columns = 20;
        slice.pixels.assign(400, 0);
        for (int y = 3; y < 17; ++y)
            for (int x = 3; x < 17; ++x)
                slice.pixels[y * 20 + x] = 100;
        fixture::stack(directory.path, slice, 8, 1);
        DicomConverter converter;
        QSignalSpy meshes(&converter, &DicomConverter::meshReady);
        QSignalSpy failures(&converter, &DicomConverter::failed);
        QSignalSpy saved(&converter, &DicomConverter::saved);
        converter.load(d2m::scanDicomDirectory(directory.path.string()).series[0]);
        d2m::SegmentationOptions segmentation;
        segmentation.lower = 50;
        segmentation.upper = 150;
        converter.segment(segmentation);
        converter.build(0, {});
        QCOMPARE(failures.count(), 0);
        QCOMPARE(meshes.count(), 1);
        const auto original = qvariant_cast<MeshPtr>(meshes[0][0]);
        const auto originalFaces = original->GetNumberOfCells();
        QVERIFY(originalFaces > 0);
        converter.process({0.5, 0, false});
        QCOMPARE(meshes.count(), 2);
        const auto reduced = qvariant_cast<MeshPtr>(meshes[1][1])->GetNumberOfCells();
        QVERIFY(reduced < originalFaces);
        converter.process({0.5, 0, false});
        QCOMPARE(qvariant_cast<MeshPtr>(meshes[2][1])->GetNumberOfCells(), reduced);
        QCOMPARE(original->GetNumberOfCells(), originalFaces);
        converter.save(QString::fromStdString((directory.path / "original.stl").string()), true);
        converter.save(QString::fromStdString((directory.path / "processed.stl").string()), false);
        QCOMPARE(saved.count(), 2);
        QCOMPARE(failures.count(), 0);
        QVERIFY(std::filesystem::file_size(directory.path / "original.stl") >
                std::filesystem::file_size(directory.path / "processed.stl"));
    }
};
int main(int argc, char **argv) {
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--brain-worker-fixture") {
        QCoreApplication application(argc, argv);
        return brainFixture(application.arguments());
    }
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication application(argc, argv);
    GuiTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "t_gui.moc"
