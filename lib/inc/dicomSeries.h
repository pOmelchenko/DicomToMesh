#pragma once

#include <array>
#include <functional>
#include <string>
#include <vector>
#include <vtkSmartPointer.h>

class vtkImageData;

namespace d2m {
struct DicomSlice {
    std::string path;
    std::array<double, 3> position{};
    std::array<double, 6> orientation{};
    std::array<double, 2> pixelSpacing{};
    int rows = 0, columns = 0, bitsAllocated = 0, bitsStored = 0, highBit = 0;
    bool signedPixels = false;
    double slope = 1.0, intercept = 0.0;
    std::string error;
};

struct DicomSeries {
    std::string uid, number, description, modality, studyUid;
    std::vector<DicomSlice> slices;
    std::array<double, 3> spacing{};
    std::array<double, 9> direction{}; // columns are the image axes in DICOM LPS
    std::string error;
    bool localizer = false;
    bool usable() const { return error.empty(); }
};

struct DicomCatalog {
    std::vector<DicomSeries> series;
    size_t ignoredFiles = 0;
};

// All operations throw std::runtime_error with a user-facing explanation.
DicomCatalog scanDicomDirectory(const std::string &directory);
vtkSmartPointer<vtkImageData> loadDicomSeries(const DicomSeries &series,
                                              const std::function<void(double)> &progress = {});
void validateVolume(vtkImageData *image);
} // namespace d2m
