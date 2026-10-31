#include "dicomFixture.h"
#include "dicomRoutines.h"
#include "dicomSeries.h"
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>

TEST(DicomSeries, SeparatesSeriesAndNeverSilentlySelectsOne) {
    fixture::Directory folder;
    fixture::Slice slice;
    fixture::stack(folder.path, slice);
    std::filesystem::create_directory(folder.path / "second");
    slice.uid += ".2";
    slice.number = "2";
    slice.description = "Synthetic T2";
    fixture::stack(folder.path / "second", slice);
    std::ofstream(folder.path / "readme.txt") << "not a DICOM file";
    const auto catalog = d2m::scanDicomDirectory(folder.path.string());
    ASSERT_EQ(catalog.series.size(), 2);
    EXPECT_EQ(catalog.ignoredFiles, 1);
    EXPECT_TRUE(catalog.series[0].usable());
    EXPECT_TRUE(catalog.series[1].usable());
    VTKDicomRoutines routines;
    EXPECT_EQ(routines.loadDicomImage(folder.path.string()), nullptr);
    EXPECT_NE(routines.lastError().find("Multiple"), std::string::npos);
}
TEST(DicomSeries, SortsByPositionAndPreservesSpacingAndPixels) {
    fixture::Directory folder;
    fixture::Slice slice;
    for (int i = 0; i < 3; ++i) {
        slice.position = {10, 20, double(30 + i * 5.5)};
        slice.pixels.assign(64, i * 10);
        fixture::write(folder.path / (std::to_string(3 - i) + ".dcm"), slice);
    }
    const auto catalog = d2m::scanDicomDirectory(folder.path.string());
    ASSERT_TRUE(catalog.series[0].usable()) << catalog.series[0].error;
    const auto image = d2m::loadDicomSeries(catalog.series[0]);
    EXPECT_EQ(image->GetDimensions()[2], 3);
    EXPECT_DOUBLE_EQ(image->GetSpacing()[0], 1);
    EXPECT_DOUBLE_EQ(image->GetSpacing()[1], 2);
    EXPECT_NEAR(image->GetSpacing()[2], 5.5, 1e-6);
    EXPECT_DOUBLE_EQ(image->GetOrigin()[2], 30);
    EXPECT_EQ(image->GetScalarComponentAsDouble(0, 0, 0, 0), 0);
    EXPECT_EQ(image->GetScalarComponentAsDouble(0, 0, 2, 0), 20);
}
TEST(DicomSeries, PreservesObliqueOrientationAndPhysicalOrigin) {
    fixture::Directory folder;
    fixture::Slice slice;
    const double c = std::sqrt(0.5);
    slice.orientation = {c, c, 0, -c, c, 0};
    slice.position = {50, 60, 70};
    fixture::write(folder.path / "a.dcm", slice);
    slice.position[2] += 4;
    fixture::write(folder.path / "b.dcm", slice);
    const auto series = d2m::scanDicomDirectory(folder.path.string()).series[0];
    ASSERT_TRUE(series.usable()) << series.error;
    const auto image = d2m::loadDicomSeries(series);
    double point[3];
    image->TransformIndexToPhysicalPoint(1, 1, 1, point);
    EXPECT_NEAR(point[0], 50 - c, 1e-4);
    EXPECT_NEAR(point[1], 60 + 3 * c, 1e-4);
    EXPECT_NEAR(point[2], 74, 1e-4);
}
TEST(DicomSeries, DecodesSignedStoredBitsAndRescaleWithoutClipping) {
    fixture::Directory folder;
    fixture::Slice slice;
    slice.bitsStored = 12;
    slice.highBit = 11;
    slice.slope = 2;
    slice.intercept = -100;
    slice.pixels.assign(64, 0x0fff);
    fixture::stack(folder.path, slice);
    auto image = d2m::loadDicomSeries(d2m::scanDicomDirectory(folder.path.string()).series[0]);
    EXPECT_EQ(image->GetScalarComponentAsDouble(0, 0, 0, 0), -102);
}
TEST(DicomSeries, ReadsExplicitBigEndianPixels) {
    fixture::Directory folder;
    fixture::Slice slice;
    slice.signedPixels = false;
    slice.pixels.assign(64, 40000);
    fixture::write(folder.path / "a.dcm", slice, EXS_BigEndianExplicit);
    slice.position[2] = 2;
    fixture::write(folder.path / "b.dcm", slice, EXS_BigEndianExplicit);
    auto image = d2m::loadDicomSeries(d2m::scanDicomDirectory(folder.path.string()).series[0]);
    EXPECT_EQ(image->GetScalarComponentAsDouble(0, 0, 0, 0), 40000);
}
TEST(DicomSeries, RejectsLocalizersEvenWithValidPositions) {
    fixture::Directory folder;
    fixture::Slice slice;
    slice.description = "Locator 3Axis";
    fixture::stack(folder.path, slice);
    const auto series = d2m::scanDicomDirectory(folder.path.string()).series[0];
    EXPECT_TRUE(series.localizer);
    EXPECT_FALSE(series.usable());
    EXPECT_THROW(d2m::loadDicomSeries(series), std::runtime_error);
}
TEST(DicomSeries, RejectsDuplicatePositionsInsteadOfProducingZeroDepth) {
    fixture::Directory folder;
    fixture::stack(folder.path, {}, 3, 0);
    const auto series = d2m::scanDicomDirectory(folder.path.string()).series[0];
    EXPECT_FALSE(series.usable());
    EXPECT_NE(series.error.find("Repeated"), std::string::npos);
    EXPECT_THROW(d2m::loadDicomSeries(series), std::runtime_error);
}
TEST(DicomSeries, RejectsMixedOrientationsWithinOneSeries) {
    fixture::Directory folder;
    fixture::Slice slice;
    fixture::write(folder.path / "a.dcm", slice);
    slice.position[2] = 5;
    slice.orientation = {1, 0, 0, 0, 0, 1};
    fixture::write(folder.path / "b.dcm", slice);
    EXPECT_FALSE(d2m::scanDicomDirectory(folder.path.string()).series[0].usable());
}
TEST(DicomSeries, RejectsMissingSliceGapsAndInPlaneDrift) {
    fixture::Directory folder;
    fixture::Slice slice;
    fixture::stack(folder.path, slice, 3, 2);
    slice.position[2] = 9;
    fixture::write(folder.path / "gap.dcm", slice);
    auto series = d2m::scanDicomDirectory(folder.path.string()).series[0];
    EXPECT_NE(series.error.find("Irregular"), std::string::npos);
    std::filesystem::remove(folder.path / "gap.dcm");
    slice.position = {1, 0, 6};
    fixture::write(folder.path / "drift.dcm", slice);
    series = d2m::scanDicomDirectory(folder.path.string()).series[0];
    EXPECT_NE(series.error.find("Sheared"), std::string::npos);
}
TEST(DicomSeries, RejectsMixedDimensionsAndInvalidPixelSpacing) {
    fixture::Directory folder;
    fixture::Slice slice;
    fixture::write(folder.path / "a.dcm", slice);
    slice.position[2] = 5;
    slice.columns = 9;
    fixture::write(folder.path / "b.dcm", slice);
    EXPECT_FALSE(d2m::scanDicomDirectory(folder.path.string()).series[0].usable());
    slice.columns = 8;
    slice.spacing = {0, 1};
    fixture::write(folder.path / "b.dcm", slice);
    EXPECT_FALSE(d2m::scanDicomDirectory(folder.path.string()).series[0].usable());
}
TEST(DicomSeries, RejectsTruncatedPixelDataAtLoad) {
    fixture::Directory folder;
    fixture::Slice slice;
    slice.pixels = {10};
    fixture::stack(folder.path, slice);
    const auto series = d2m::scanDicomDirectory(folder.path.string()).series[0];
    ASSERT_TRUE(series.usable());
    EXPECT_THROW(d2m::loadDicomSeries(series), std::runtime_error);
}

TEST(DicomSeries, RejectsMetadataChangedAfterInspection) {
    fixture::Directory folder;
    fixture::Slice slice;
    fixture::stack(folder.path, slice);
    const auto series = d2m::scanDicomDirectory(folder.path.string()).series[0];
    slice.position = {0, 0, 11};
    slice.spacing = {3, 1};
    fixture::write(folder.path / "1.dcm", slice);
    EXPECT_THROW(d2m::loadDicomSeries(series), std::runtime_error);
}

TEST(DicomSeries, RejectsRgbAndMultiFrameSeries) {
    fixture::Directory folder;
    fixture::Slice slice;
    slice.photo = "RGB";
    fixture::stack(folder.path, slice);
    EXPECT_FALSE(d2m::scanDicomDirectory(folder.path.string()).series[0].usable());
    slice.photo = "MONOCHROME2";
    fixture::stack(folder.path, slice);
    DcmFileFormat file;
    ASSERT_TRUE(file.loadFile((folder.path / "1.dcm").string().c_str()).good());
    file.getDataset()->putAndInsertString(DCM_NumberOfFrames, "2");
    ASSERT_TRUE(
        file.saveFile((folder.path / "1.dcm").string().c_str(), EXS_LittleEndianExplicit).good());
    EXPECT_FALSE(d2m::scanDicomDirectory(folder.path.string()).series[0].usable());
}
TEST(DicomSeries, PreservesStudyIdentityAndRejectsChangesAfterInspection) {
    fixture::Directory folder;
    fixture::Slice slice;
    fixture::stack(folder.path, slice);
    const auto series = d2m::scanDicomDirectory(folder.path.string()).series[0];
    EXPECT_EQ(series.studyUid, slice.studyUid);
    DcmFileFormat file;
    ASSERT_TRUE(file.loadFile((folder.path / "1.dcm").string().c_str()).good());
    file.getDataset()->putAndInsertString(DCM_StudyInstanceUID, "1.2.826.0.1.3680043.10.999.999");
    ASSERT_TRUE(file.saveFile((folder.path / "1.dcm").string().c_str(), EXS_LittleEndianExplicit).good());
    EXPECT_THROW(d2m::loadDicomSeries(series), std::runtime_error);
    EXPECT_FALSE(d2m::scanDicomDirectory(folder.path.string()).series[0].usable());
}
