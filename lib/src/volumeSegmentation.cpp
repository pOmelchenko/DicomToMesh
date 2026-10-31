#include "volumeSegmentation.h"
#include "dicomSeries.h"

#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkImageResample.h>
#include <vtkMarchingCubes.h>
#include <vtkMatrix3x3.h>
#include <vtkMatrix4x4.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkTransform.h>
#include <vtkTransformFilter.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace d2m {
namespace {
using Mask = std::vector<unsigned char>;
size_t voxelCount(const int *d) { return size_t(d[0]) * d[1] * d[2]; }
size_t offset(int x, int y, int z, const int *d) { return (size_t(z) * d[1] + y) * d[0] + x; }
template <class F> void neighbors(size_t p, const int *d, F visit) {
    const size_t xy = size_t(d[0]) * d[1];
    const int x = p % d[0], y = (p / d[0]) % d[1], z = p / xy;
    if (x)
        visit(p - 1);
    if (x + 1 < d[0])
        visit(p + 1);
    if (y)
        visit(p - d[0]);
    if (y + 1 < d[1])
        visit(p + d[0]);
    if (z)
        visit(p - xy);
    if (z + 1 < d[2])
        visit(p + xy);
}
Mask connected(const Mask &mask, const int *d, const std::vector<size_t> &seeds) {
    Mask result(mask.size(), 0);
    std::vector<size_t> queue;
    for (size_t seed : seeds)
        if (seed < mask.size() && mask[seed] && !result[seed]) {
            result[seed] = 1;
            queue.push_back(seed);
        }
    for (size_t i = 0; i < queue.size(); ++i)
        neighbors(queue[i], d, [&](size_t n) {
            if (mask[n] && !result[n]) {
                result[n] = 1;
                queue.push_back(n);
            }
        });
    return result;
}
void keepLargest(Mask &mask, const int *d) {
    Mask visited(mask.size(), 0);
    std::vector<size_t> queue, best;
    for (size_t start = 0; start < mask.size(); ++start)
        if (mask[start] && !visited[start]) {
            queue.clear();
            queue.push_back(start);
            visited[start] = 1;
            for (size_t i = 0; i < queue.size(); ++i)
                neighbors(queue[i], d, [&](size_t n) {
                    if (mask[n] && !visited[n]) {
                        visited[n] = 1;
                        queue.push_back(n);
                    }
                });
            if (queue.size() > best.size())
                best = queue;
        }
    std::fill(mask.begin(), mask.end(), 0);
    for (size_t p : best)
        mask[p] = 1;
}
// A separable box closing in physical millimetres. A prefix-sum window keeps
// runtime linear even for thin in-plane pixels and thick MR slices.
void morphology(Mask &mask, const int *d, const double *spacing, double radius, bool dilate) {
    for (int axis = 0; axis < 3; ++axis) {
        const int r = std::min(d[axis] - 1, int(std::floor(radius / spacing[axis])));
        if (r <= 0)
            continue;
        const int a = (axis + 1) % 3, b = (axis + 2) % 3;
        Mask output(mask.size());
        std::vector<int> prefix(d[axis] + 1);
        for (int j = 0; j < d[b]; ++j)
            for (int i = 0; i < d[a]; ++i) {
                std::array<int, 3> c{};
                c[a] = i;
                c[b] = j;
                prefix[0] = 0;
                for (int k = 0; k < d[axis]; ++k) {
                    c[axis] = k;
                    prefix[k + 1] = prefix[k] + mask[offset(c[0], c[1], c[2], d)];
                }
                for (int k = 0; k < d[axis]; ++k) {
                    const int lo = std::max(0, k - r), hi = std::min(d[axis], k + r + 1);
                    const int sum = prefix[hi] - prefix[lo];
                    c[axis] = k;
                    output[offset(c[0], c[1], c[2], d)] = dilate ? sum > 0 : sum == hi - lo;
                }
            }
        mask.swap(output);
    }
}
void fillHoles(Mask &mask, const int *d) {
    const size_t plane = size_t(d[0]) * d[1];
    Mask exterior(plane);
    std::vector<size_t> queue;
    for (int z = 0; z < d[2]; ++z) {
        std::fill(exterior.begin(), exterior.end(), 0);
        queue.clear();
        auto add = [&](int x, int y) {
            const size_t p = size_t(y) * d[0] + x;
            if (!mask[z * plane + p] && !exterior[p]) {
                exterior[p] = 1;
                queue.push_back(p);
            }
        };
        for (int x = 0; x < d[0]; ++x) {
            add(x, 0);
            add(x, d[1] - 1);
        }
        for (int y = 0; y < d[1]; ++y) {
            add(0, y);
            add(d[0] - 1, y);
        }
        for (size_t i = 0; i < queue.size(); ++i) {
            const int x = queue[i] % d[0], y = queue[i] / d[0];
            if (x)
                add(x - 1, y);
            if (x + 1 < d[0])
                add(x + 1, y);
            if (y)
                add(x, y - 1);
            if (y + 1 < d[1])
                add(x, y + 1);
        }
        for (size_t p = 0; p < plane; ++p)
            if (!exterior[p])
                mask[z * plane + p] = 1;
    }
}
std::vector<size_t> applyStrokes(Mask &mask, const int *d, const double *spacing,
                                 const std::vector<MaskStroke> &strokes) {
    Mask paintedForeground(mask.size(), 0);
    for (const auto &stroke : strokes) {
        if (!std::isfinite(stroke.radiusMm) || stroke.radiusMm <= 0 || stroke.radiusMm > 50)
            throw std::runtime_error("Brush radius must be between 0 and 50 mm.");
        std::array<int, 3> r;
        for (int a = 0; a < 3; ++a)
            r[a] = int(std::ceil(stroke.radiusMm / spacing[a]));
        for (int z = std::max(0, stroke.center[2] - r[2]);
             z <= std::min(d[2] - 1, stroke.center[2] + r[2]); ++z)
            for (int y = std::max(0, stroke.center[1] - r[1]);
                 y <= std::min(d[1] - 1, stroke.center[1] + r[1]); ++y)
                for (int x = std::max(0, stroke.center[0] - r[0]);
                     x <= std::min(d[0] - 1, stroke.center[0] + r[0]); ++x) {
                    double distance = 0;
                    int c[3]{x, y, z};
                    for (int a = 0; a < 3; ++a) {
                        const double v = (c[a] - stroke.center[a]) * spacing[a];
                        distance += v * v;
                    }
                    if (distance <= stroke.radiusMm * stroke.radiusMm) {
                        const size_t p = offset(x, y, z, d);
                        mask[p] = stroke.foreground;
                        paintedForeground[p] = stroke.foreground;
                    }
                }
    }
    std::vector<size_t> seeds;
    for (size_t i = 0; i < paintedForeground.size(); ++i)
        if (paintedForeground[i])
            seeds.push_back(i);
    return seeds;
}
vtkSmartPointer<vtkPolyData> extract(vtkImageData *image, double value, double resample) {
    validateVolume(image);
    auto local = vtkSmartPointer<vtkImageData>::New();
    local->ShallowCopy(image);
    local->SetOrigin(0, 0, 0);
    local->SetDirectionMatrix(1, 0, 0, 0, 1, 0, 0, 0, 1);
    if (resample > 0) {
        if (!std::isfinite(resample) || resample < 0.1)
            throw std::runtime_error("Resampling spacing must be at least 0.1 mm.");
        double count = 1;
        for (int a = 0; a < 3; ++a)
            count *=
                std::ceil((local->GetDimensions()[a] - 1) * local->GetSpacing()[a] / resample) + 1;
        if (count > 128ULL * 1024 * 1024)
            throw std::runtime_error("Requested interpolation exceeds the 128-million-voxel limit; "
                                     "choose a larger spacing.");
        auto sampler = vtkSmartPointer<vtkImageResample>::New();
        sampler->SetInputData(local);
        sampler->SetInterpolationModeToLinear();
        sampler->SetOutputScalarType(VTK_FLOAT);
        for (int a = 0; a < 3; ++a)
            sampler->SetAxisOutputSpacing(a, resample);
        sampler->Update();
        auto sampled = vtkSmartPointer<vtkImageData>::New();
        sampled->ShallowCopy(sampler->GetOutput());
        local = sampled;
    }
    auto cubes = vtkSmartPointer<vtkMarchingCubes>::New();
    cubes->SetInputData(local);
    cubes->SetValue(0, value);
    cubes->ComputeNormalsOn();
    cubes->Update();
    auto matrix = vtkSmartPointer<vtkMatrix4x4>::New();
    matrix->Identity();
    for (int i = 0; i < 3; ++i) {
        matrix->SetElement(i, 3, image->GetOrigin()[i]);
        for (int j = 0; j < 3; ++j)
            matrix->SetElement(i, j, image->GetDirectionMatrix()->GetElement(i, j));
    }
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(matrix);
    auto transformed = vtkSmartPointer<vtkTransformFilter>::New();
    transformed->SetTransform(transform);
    transformed->SetInputConnection(cubes->GetOutputPort());
    transformed->Update();
    auto result = vtkSmartPointer<vtkPolyData>::New();
    result->DeepCopy(transformed->GetOutput());
    return result;
}
} // namespace

VolumeHistogram volumeHistogram(vtkImageData *image) {
    validateVolume(image);
    VolumeHistogram result;
    double range[2];
    image->GetScalarRange(range);
    result.minimum = range[0];
    result.maximum = range[1];
    if (!std::isfinite(range[0]) || !std::isfinite(range[1]))
        throw std::runtime_error("Image intensity range is not finite.");
    const double width = std::max(1.0, range[1] - range[0]);
    const size_t count = voxelCount(image->GetDimensions());
    // The histogram is sampled uniformly, keeping UI latency bounded.
    const size_t stride = std::max<size_t>(1, count / 1000000);
    for (size_t p = 0; p < count; p += stride) {
        const double value = image->GetPointData()->GetScalars()->GetComponent(p, 0);
        if (std::isfinite(value))
            ++result.bins[std::clamp(int((value - range[0]) * 255 / width), 0, 255)];
    }
    const double total = std::accumulate(result.bins.begin(), result.bins.end(), 0.0);
    double sum = 0;
    for (int i = 0; i < 256; ++i)
        sum += i * result.bins[i];
    double weight = 0, partial = 0, best = -1;
    int threshold = 0;
    for (int i = 0; i < 255; ++i) {
        weight += result.bins[i];
        partial += i * result.bins[i];
        if (weight == 0 || total == weight)
            continue;
        const double delta = partial / weight - (sum - partial) / (total - weight);
        const double score = weight * (total - weight) * delta * delta;
        if (score > best) {
            best = score;
            threshold = i;
        }
    }
    result.suggestedLower = range[0] + (threshold + 0.5) * width / 256;
    return result;
}

vtkSmartPointer<vtkImageData> segmentVolume(vtkImageData *image,
                                            const SegmentationOptions &options) {
    validateVolume(image);
    if (!std::isfinite(options.lower) || !std::isfinite(options.upper) ||
        options.lower > options.upper)
        throw std::runtime_error("The lower threshold must not exceed the upper threshold.");
    if (!std::isfinite(options.closingRadiusMm) || options.closingRadiusMm < 0 ||
        options.closingRadiusMm > 20)
        throw std::runtime_error("Closing radius must be between 0 and 20 mm.");
    const int *d = image->GetDimensions();
    const double *spacing = image->GetSpacing();
    Mask mask(voxelCount(d));
    auto *scalars = image->GetPointData()->GetScalars();
    for (size_t p = 0; p < mask.size(); ++p) {
        const double value = scalars->GetComponent(p, 0);
        mask[p] = std::isfinite(value) && value >= options.lower && value <= options.upper;
    }
    if (options.closingRadiusMm > 0) {
        morphology(mask, d, spacing, options.closingRadiusMm, true);
        morphology(mask, d, spacing, options.closingRadiusMm, false);
    }
    const auto seeds = applyStrokes(mask, d, spacing, options.strokes);
    if (options.mode == SegmentationMode::SeededRegion) {
        if (seeds.empty())
            throw std::runtime_error(
                "Place foreground seeds with the left mouse button inside the target region.");
        mask = connected(mask, d, seeds);
    } else if (options.largestComponent)
        keepLargest(mask, d);
    if (options.fillSliceHoles)
        fillHoles(mask, d);
    // Manual corrections are authoritative, including after hole filling.
    applyStrokes(mask, d, spacing, options.strokes);
    auto result = vtkSmartPointer<vtkImageData>::New();
    result->CopyStructure(image);
    result->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::copy(mask.begin(), mask.end(), static_cast<unsigned char *>(result->GetScalarPointer()));
    return result;
}

vtkSmartPointer<vtkPolyData> surfaceFromMask(vtkImageData *mask, double isotropicSpacing) {
    return extract(mask, 0.5, isotropicSpacing);
}
vtkSmartPointer<vtkPolyData> intensitySurface(vtkImageData *image, double lower, bool useUpper,
                                              double upper) {
    if (!useUpper)
        return extract(image, lower, 0);
    SegmentationOptions options;
    options.lower = lower;
    options.upper = upper;
    return surfaceFromMask(segmentVolume(image, options));
}
} // namespace d2m
