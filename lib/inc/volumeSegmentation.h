#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include <vtkSmartPointer.h>

class vtkImageData;
class vtkPolyData;

namespace d2m {
enum class SegmentationMode { Threshold, HeadOutline, SeededRegion, Brain, SkullMRI };
struct MaskStroke {
    std::array<int, 3> center{};
    double radiusMm = 2;
    bool foreground = true;
};
struct SegmentationOptions {
    SegmentationMode mode = SegmentationMode::Threshold;
    double lower = 400, upper = 3000;
    double closingRadiusMm = 0;
    bool largestComponent = false;
    bool fillSliceHoles = false;
    std::vector<MaskStroke> strokes;
};
struct VolumeHistogram {
    std::array<uint64_t, 256> bins{};
    double minimum = 0, maximum = 1;
    double suggestedLower = 0;
};

VolumeHistogram volumeHistogram(vtkImageData *image);
struct MaskSummary {
    uint64_t foreground = 0;
    std::array<uint64_t, 6> boundary{}; // I-, I+, J-, J+, K-, K+
};
MaskSummary summarizeMask(vtkImageData *mask);
vtkSmartPointer<vtkImageData> correctMask(vtkImageData *mask,
                                          const std::vector<MaskStroke> &strokes);
vtkSmartPointer<vtkImageData> segmentVolume(vtkImageData *image,
                                            const SegmentationOptions &options);
// Extraction happens in image-local coordinates, followed by an explicit LPS
// transform, so orientation does not depend on filter-specific VTK support.
vtkSmartPointer<vtkPolyData> surfaceFromMask(vtkImageData *mask, double isotropicSpacing = 0);
// Signed distance in millimetres, negative inside. SynthStrip uses border=1.
vtkSmartPointer<vtkPolyData> surfaceFromDistance(vtkImageData *distance,
                                                 double isotropicSpacing = 0);
vtkSmartPointer<vtkImageData> correctDistance(vtkImageData *distance, vtkImageData *reference,
                                              const std::vector<MaskStroke> &strokes);
vtkSmartPointer<vtkImageData> maskFromDistance(vtkImageData *distance, vtkImageData *reference);
vtkSmartPointer<vtkPolyData> intensitySurface(vtkImageData *image, double lower,
                                              bool useUpper = false, double upper = 0);
} // namespace d2m
