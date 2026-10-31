#include "dicomSeries.h"
#include "volumeSegmentation.h"
#include <algorithm>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>
#include <vtkImageData.h>
#include <vtkPolyData.h>

namespace {
auto volume(int size = 9) {
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(size, size, size);
    image->AllocateScalars(VTK_FLOAT, 1);
    std::fill_n(static_cast<float *>(image->GetScalarPointer()), size * size * size, 0);
    return image;
}
void set(vtkImageData *image, int x, int y, int z, float value) {
    *static_cast<float *>(image->GetScalarPointer(x, y, z)) = value;
}
double get(vtkImageData *image, int x, int y, int z) {
    return image->GetScalarComponentAsDouble(x, y, z, 0);
}
} // namespace
TEST(Segmentation, ThresholdIsNonDestructiveAndSupportsUpperBound) {
    auto image = volume();
    set(image, 4, 4, 4, 100);
    set(image, 5, 4, 4, 200);
    d2m::SegmentationOptions options;
    options.lower = 90;
    options.upper = 110;
    auto mask = d2m::segmentVolume(image, options);
    EXPECT_EQ(get(mask, 4, 4, 4), 1);
    EXPECT_EQ(get(mask, 5, 4, 4), 0);
    EXPECT_EQ(get(image, 4, 4, 4), 100);
    EXPECT_THROW(d2m::segmentVolume(image, {d2m::SegmentationMode::Threshold, 200, 100}),
                 std::runtime_error);
}
TEST(Segmentation, RemovesDisconnectedNoiseAndFillsEnclosedSliceHoles) {
    auto image = volume();
    for (int z = 2; z < 7; ++z)
        for (int y = 2; y < 7; ++y)
            for (int x = 2; x < 7; ++x)
                set(image, x, y, z, 100);
    set(image, 4, 4, 4, 0);
    set(image, 0, 0, 0, 100);
    d2m::SegmentationOptions options;
    options.lower = 50;
    options.upper = 150;
    options.largestComponent = true;
    options.fillSliceHoles = true;
    auto mask = d2m::segmentVolume(image, options);
    EXPECT_EQ(get(mask, 0, 0, 0), 0);
    EXPECT_EQ(get(mask, 4, 4, 4), 1);
    EXPECT_EQ(get(mask, 0, 4, 4), 0);
}
TEST(Segmentation, SeededGrowthKeepsOnlySeededRegionAndHonorsPaintedBarriers) {
    auto image = volume();
    for (int x = 1; x < 8; ++x)
        set(image, x, 4, 4, 100);
    d2m::SegmentationOptions options;
    options.mode = d2m::SegmentationMode::SeededRegion;
    options.lower = 50;
    options.upper = 150;
    EXPECT_THROW(d2m::segmentVolume(image, options), std::runtime_error);
    options.strokes = {{{1, 4, 4}, 0.1, true}, {{4, 4, 4}, 0.1, false}};
    auto mask = d2m::segmentVolume(image, options);
    EXPECT_EQ(get(mask, 2, 4, 4), 1);
    EXPECT_EQ(get(mask, 4, 4, 4), 0);
    EXPECT_EQ(get(mask, 6, 4, 4), 0);
}
TEST(Segmentation, ManualErasureSurvivesHoleFilling) {
    auto image = volume();
    for (int y = 1; y < 8; ++y)
        for (int x = 1; x < 8; ++x)
            set(image, x, y, 4, 100);
    d2m::SegmentationOptions options;
    options.lower = 50;
    options.upper = 150;
    options.fillSliceHoles = true;
    options.strokes = {{{4, 4, 4}, 0.1, false}};
    auto mask = d2m::segmentVolume(image, options);
    EXPECT_EQ(get(mask, 4, 4, 4), 0);
}
TEST(Segmentation, ClosingUsesPhysicalSpacingAndDoesNotJoinThickSlices) {
    auto image = volume();
    image->SetSpacing(1, 1, 6);
    for (int z : {2, 4})
        for (int y = 1; y < 8; ++y)
            for (int x = 1; x < 8; ++x)
                set(image, x, y, z, 100);
    d2m::SegmentationOptions options;
    options.lower = 50;
    options.upper = 150;
    options.closingRadiusMm = 1;
    auto mask = d2m::segmentVolume(image, options);
    EXPECT_EQ(get(mask, 4, 4, 3), 0);
}
TEST(Segmentation, SurfaceUsesPatientOrientationAndOrigin) {
    auto image = volume();
    image->SetSpacing(1, 2, 3);
    image->SetOrigin(100, 200, 300);
    image->SetDirectionMatrix(0, -1, 0, 1, 0, 0, 0, 0, 1);
    for (int z = 2; z <= 6; ++z)
        for (int y = 2; y <= 6; ++y)
            for (int x = 2; x <= 6; ++x)
                set(image, x, y, z, 100);
    d2m::SegmentationOptions options;
    options.lower = 50;
    options.upper = 150;
    auto mask = d2m::segmentVolume(image, options);
    auto mesh = d2m::surfaceFromMask(mask);
    ASSERT_GT(mesh->GetNumberOfCells(), 0);
    const auto *bounds = mesh->GetBounds();
    EXPECT_NEAR(bounds[0], 87, 1e-5);
    EXPECT_NEAR(bounds[1], 97, 1e-5);
    EXPECT_NEAR(bounds[2], 201.5, 1e-5);
    EXPECT_NEAR(bounds[5], 319.5, 1e-5);
    auto interpolated = d2m::surfaceFromMask(mask, 1);
    EXPECT_GT(interpolated->GetNumberOfCells(), 0);
    EXPECT_NEAR(interpolated->GetBounds()[0], 87, 1e-5);
    EXPECT_EQ(get(image, 4, 4, 4), 100);
    EXPECT_DOUBLE_EQ(image->GetSpacing()[2], 3);
}
TEST(Segmentation, RejectsInvalidGeometryAndExcessiveResampling) {
    auto image = volume();
    image->SetSpacing(1, 1, 0);
    EXPECT_THROW(d2m::segmentVolume(image, {}), std::runtime_error);
    image->SetSpacing(100, 100, 100);
    EXPECT_THROW(d2m::surfaceFromMask(image, 0.1), std::runtime_error);
}
TEST(Segmentation, HistogramSuggestsAThresholdBetweenTwoPopulations) {
    auto image = volume();
    for (int z = 2; z <= 6; ++z)
        for (int y = 2; y <= 6; ++y)
            for (int x = 2; x <= 6; ++x)
                set(image, x, y, z, 100);
    const auto histogram = d2m::volumeHistogram(image);
    EXPECT_EQ(histogram.minimum, 0);
    EXPECT_EQ(histogram.maximum, 100);
    EXPECT_GT(histogram.suggestedLower, 0);
    EXPECT_LT(histogram.suggestedLower, 100);
}

TEST(Segmentation, RejectsInvalidOriginAndDirection) {
    auto image = volume();
    image->SetOrigin(std::numeric_limits<double>::infinity(), 0, 0);
    EXPECT_THROW(d2m::segmentVolume(image, {}), std::runtime_error);
    image->SetOrigin(0, 0, 0);
    image->SetDirectionMatrix(1, 1, 0, 0, 1, 0, 0, 0, 1);
    EXPECT_THROW(d2m::surfaceFromMask(image), std::runtime_error);
}
