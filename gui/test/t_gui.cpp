#include "d2mWidget.h"
#include "dicomFixture.h"
#include "sliceView.h"
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
#include <QVTKOpenGLNativeWidget.h>
#include <QtTest>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkRenderWindow.h>

class GuiTest : public QObject {
    Q_OBJECT
  private slots:
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
    }
    void defaultProcessingPreservesDetails() {
        D2MWidget widget;
        QCOMPARE(widget.findChild<QDoubleSpinBox *>("meshReduction")->value(), 0.0);
        QCOMPARE(widget.findChild<QSpinBox *>("smoothingIterations")->value(), 0);
        QVERIFY(!widget.findChild<QCheckBox *>("centerMesh")->isChecked());
        QVERIFY(!widget.findChild<QPushButton *>("buildSurface")->isEnabled());
        QVERIFY(!widget.findChild<QPushButton *>("exportSurface")->isEnabled());
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
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication application(argc, argv);
    GuiTest tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "t_gui.moc"
