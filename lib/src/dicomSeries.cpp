#include "dicomSeries.h"

#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dcrledrg.h>
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmjpeg/djdecode.h>
#include <dcmtk/dcmjpls/djdecode.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>

namespace d2m {
namespace {
using Vec = std::array<double, 3>;
double dot(const Vec &a, const Vec &b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec subtract(const Vec &a, const Vec &b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec cross(const Vec &a, const Vec &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
bool close(double a, double b, double tolerance = 1e-4) { return std::abs(a - b) <= tolerance; }
std::string text(DcmItem &ds, const DcmTagKey &key) {
    OFString value;
    ds.findAndGetOFStringArray(key, value);
    return value.c_str();
}
double number(DcmItem &ds, const DcmTagKey &key, unsigned long index,
              double fallback = std::numeric_limits<double>::quiet_NaN()) {
    Float64 value;
    return ds.findAndGetFloat64(key, value, index).good() ? value : fallback;
}
int integer(DcmItem &ds, const DcmTagKey &key, int fallback = -1) {
    Uint16 value;
    return ds.findAndGetUint16(key, value).good() ? value : fallback;
}
void validateSeries(DicomSeries &series) {
    auto fail = [&](const std::string &error) { series.error = error; };
    if (series.localizer)
        return fail("Localizer/scout contains positioning images, not a 3D anatomical volume.");
    if (series.slices.size() < 2)
        return fail("At least two distinct parallel slices are required for a 3D surface.");
    const auto first = series.slices.front();
    Vec x{first.orientation[0], first.orientation[1], first.orientation[2]};
    Vec y{first.orientation[3], first.orientation[4], first.orientation[5]};
    if (!std::isfinite(dot(x, x)) || !close(dot(x, x), 1, 1e-3) || !close(dot(y, y), 1, 1e-3) ||
        !close(dot(x, y), 0, 1e-3))
        return fail("Missing or invalid ImageOrientationPatient.");
    const Vec z = cross(x, y);
    for (int axis = 0; axis < 3; ++axis) {
        series.direction[axis * 3] = x[axis];
        series.direction[axis * 3 + 1] = y[axis];
        series.direction[axis * 3 + 2] = z[axis];
    }
    for (const auto &slice : series.slices) {
        if (!slice.error.empty())
            return fail(slice.error);
        if (slice.rows != first.rows || slice.columns != first.columns)
            return fail("Inconsistent image dimensions within the series.");
        for (int axis = 0; axis < 6; ++axis)
            if (!std::isfinite(slice.orientation[axis]) ||
                !close(slice.orientation[axis], first.orientation[axis], 1e-3))
                return fail("The series contains different slice orientations; it cannot form one "
                            "3D volume.");
        for (int axis = 0; axis < 2; ++axis)
            if (!std::isfinite(slice.pixelSpacing[axis]) || slice.pixelSpacing[axis] <= 0 ||
                !close(slice.pixelSpacing[axis], first.pixelSpacing[axis]))
                return fail("Missing, non-positive or inconsistent PixelSpacing.");
        for (double value : slice.position)
            if (!std::isfinite(value))
                return fail("Missing or invalid ImagePositionPatient.");
    }
    std::sort(series.slices.begin(), series.slices.end(), [&](const auto &a, const auto &b) {
        return dot(a.position, z) < dot(b.position, z);
    });
    std::vector<double> distances;
    for (size_t i = 1; i < series.slices.size(); ++i) {
        const Vec delta = subtract(series.slices[i].position, series.slices[i - 1].position);
        const double distance = dot(delta, z);
        if (distance < 1e-3)
            return fail("Repeated slice positions: select one acquisition/temporal/diffusion "
                        "volume before loading.");
        const Vec drift{delta[0] - distance * z[0], delta[1] - distance * z[1],
                        delta[2] - distance * z[2]};
        if (std::sqrt(dot(drift, drift)) > 0.05)
            return fail("Sheared/tilted slice positions require resampling before loading as a "
                        "regular volume.");
        distances.push_back(distance);
    }
    auto sorted = distances;
    std::sort(sorted.begin(), sorted.end());
    const double step = sorted[sorted.size() / 2];
    for (double distance : distances)
        if (!close(distance, step, std::max(0.05, step * 0.01)))
            return fail(
                "Irregular slice spacing or missing slices; refusing to distort the volume.");
    series.spacing = {first.pixelSpacing[1], first.pixelSpacing[0], step};
}
} // namespace

DicomCatalog scanDicomDirectory(const std::string &directory) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(directory))
        throw std::runtime_error("Choose an existing DICOM directory.");
    DicomCatalog result;
    std::map<std::string, DicomSeries> groups;
    std::vector<fs::path> paths;
    for (const auto &entry : fs::recursive_directory_iterator(directory))
        if (entry.is_regular_file())
            paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    for (const auto &path : paths) {
        auto extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        std::ifstream header(path, std::ios::binary);
        char prefix[132]{};
        header.read(prefix, sizeof(prefix));
        if (extension != ".dcm" && extension != ".ima" && !extension.empty() &&
            !(header.gcount() == 132 && std::string(prefix + 128, 4) == "DICM")) {
            ++result.ignoredFiles;
            continue;
        }
        DcmFileFormat file;
        if (file.loadFile(path.string().c_str()).bad()) {
            ++result.ignoredFiles;
            continue;
        }
        auto &ds = *file.getDataset();
        const std::string uid = text(ds, DCM_SeriesInstanceUID);
        if (uid.empty() || !ds.tagExists(DCM_PixelData)) {
            ++result.ignoredFiles;
            continue;
        }
        auto &series = groups[uid];
        series.uid = uid;
        const auto description = text(ds, DCM_SeriesDescription);
        const auto modality = text(ds, DCM_Modality);
        if (series.slices.empty()) {
            series.description = description;
            series.modality = modality;
            series.number = text(ds, DCM_SeriesNumber);
        }
        auto type = text(ds, DCM_ImageType) + " " + description;
        std::transform(type.begin(), type.end(), type.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        series.localizer = series.localizer || type.find("LOCALIZER") != std::string::npos ||
                           type.find("LOCATOR") != std::string::npos ||
                           type.find("SCOUT") != std::string::npos;
        DicomSlice slice;
        slice.path = path.string();
        slice.rows = integer(ds, DCM_Rows);
        slice.columns = integer(ds, DCM_Columns);
        slice.bitsAllocated = integer(ds, DCM_BitsAllocated);
        slice.bitsStored = integer(ds, DCM_BitsStored);
        slice.highBit = integer(ds, DCM_HighBit);
        slice.signedPixels = integer(ds, DCM_PixelRepresentation) == 1;
        slice.slope = number(ds, DCM_RescaleSlope, 0, 1.0);
        slice.intercept = number(ds, DCM_RescaleIntercept, 0, 0.0);
        for (int i = 0; i < 3; ++i)
            slice.position[i] = number(ds, DCM_ImagePositionPatient, i);
        for (int i = 0; i < 6; ++i)
            slice.orientation[i] = number(ds, DCM_ImageOrientationPatient, i);
        for (int i = 0; i < 2; ++i)
            slice.pixelSpacing[i] = number(ds, DCM_PixelSpacing, i);
        const auto photo = text(ds, DCM_PhotometricInterpretation);
        if (integer(ds, DCM_SamplesPerPixel, 1) != 1 ||
            (photo != "MONOCHROME1" && photo != "MONOCHROME2"))
            slice.error = "Only monochrome anatomical images can form a scalar volume; RGB/derived "
                          "reports are unsupported.";
        if (slice.rows <= 0 || slice.columns <= 0)
            slice.error = "Invalid Rows/Columns.";
        if ((slice.bitsAllocated != 8 && slice.bitsAllocated != 16) || slice.bitsStored <= 0 ||
            slice.bitsStored > slice.bitsAllocated || slice.highBit < slice.bitsStored - 1 ||
            slice.highBit >= slice.bitsAllocated)
            slice.error = "Unsupported or inconsistent pixel bit layout (8/16-bit integer images "
                          "are supported).";
        if (!std::isfinite(slice.slope) || slice.slope == 0 || !std::isfinite(slice.intercept))
            slice.error = "Invalid pixel rescale slope/intercept.";
        Sint32 frames = 1;
        if (ds.tagExists(DCM_NumberOfFrames) &&
            (ds.findAndGetSint32(DCM_NumberOfFrames, frames).bad() || frames != 1))
            slice.error = "Enhanced/multi-frame DICOM requires a dedicated frame importer; export "
                          "a single-frame series.";
        if (modality != series.modality)
            slice.error = "Inconsistent modality within a series.";
        series.slices.push_back(std::move(slice));
    }
    for (auto &group : groups) {
        validateSeries(group.second);
        result.series.push_back(std::move(group.second));
    }
    std::sort(result.series.begin(), result.series.end(), [](const auto &a, const auto &b) {
        auto numeric = [](const std::string &value) {
            try {
                return std::stoi(value);
            } catch (...) {
                return 0;
            }
        };
        const int an = numeric(a.number), bn = numeric(b.number);
        return an == bn ? a.uid < b.uid : an < bn;
    });
    if (result.series.empty())
        throw std::runtime_error("No DICOM image series were found in this directory.");
    return result;
}

vtkSmartPointer<vtkImageData> loadDicomSeries(const DicomSeries &input,
                                              const std::function<void(double)> &progress) {
    auto series = input;
    validateSeries(series); // callers cannot bypass geometry checks with a modified catalog
    if (!series.usable())
        throw std::runtime_error(series.error);
    const auto &first = series.slices.front();
    constexpr size_t maximumVoxels = 256ULL * 1024 * 1024;
    const size_t plane = size_t(first.rows) * size_t(first.columns);
    if (plane > maximumVoxels / series.slices.size())
        throw std::runtime_error("Volume exceeds the 256-million-voxel memory limit.");
    static std::once_flag codecs;
    std::call_once(codecs, [] {
        DcmRLEDecoderRegistration::registerCodecs();
        DJDecoderRegistration::registerCodecs();
        DJLSDecoderRegistration::registerCodecs();
    });
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(first.columns, first.rows, static_cast<int>(series.slices.size()));
    image->SetSpacing(series.spacing.data());
    image->SetOrigin(first.position.data());
    image->SetDirectionMatrix(series.direction.data());
    image->AllocateScalars(VTK_FLOAT, 1);
    auto *output = static_cast<float *>(image->GetScalarPointer());
    for (size_t z = 0; z < series.slices.size(); ++z) {
        const auto &slice = series.slices[z];
        DcmFileFormat file;
        if (file.loadFile(slice.path.c_str()).bad())
            throw std::runtime_error("A selected DICOM file can no longer be read.");
        auto &ds = *file.getDataset();
        if (integer(ds, DCM_Rows) != slice.rows || integer(ds, DCM_Columns) != slice.columns ||
            text(ds, DCM_SeriesInstanceUID) != series.uid ||
            integer(ds, DCM_BitsAllocated) != slice.bitsAllocated ||
            integer(ds, DCM_BitsStored) != slice.bitsStored ||
            integer(ds, DCM_HighBit) != slice.highBit ||
            (integer(ds, DCM_PixelRepresentation) == 1) != slice.signedPixels ||
            !close(number(ds, DCM_RescaleSlope, 0, 1), slice.slope) ||
            !close(number(ds, DCM_RescaleIntercept, 0, 0), slice.intercept))
            throw std::runtime_error(
                "DICOM metadata changed after the series was inspected; reopen the directory.");
        for (int a = 0; a < 3; ++a)
            if (!close(number(ds, DCM_ImagePositionPatient, a), slice.position[a]))
                throw std::runtime_error(
                    "DICOM slice positions changed after inspection; reopen the directory.");
        for (int a = 0; a < 6; ++a)
            if (!close(number(ds, DCM_ImageOrientationPatient, a), slice.orientation[a]))
                throw std::runtime_error(
                    "DICOM orientation changed after inspection; reopen the directory.");
        for (int a = 0; a < 2; ++a)
            if (!close(number(ds, DCM_PixelSpacing, a), slice.pixelSpacing[a]))
                throw std::runtime_error(
                    "DICOM pixel spacing changed after inspection; reopen the directory.");
        if (ds.chooseRepresentation(EXS_LittleEndianExplicit, nullptr).bad() ||
            !ds.canWriteXfer(EXS_LittleEndianExplicit))
            throw std::runtime_error("Cannot decode the DICOM transfer syntax (JPEG 2000 and some "
                                     "proprietary formats need preprocessing).");
        const Uint16 *words = nullptr;
        const Uint8 *bytes = nullptr;
        unsigned long count = 0;
        const bool ok = slice.bitsAllocated == 16
                            ? ds.findAndGetUint16Array(DCM_PixelData, words, &count).good()
                            : ds.findAndGetUint8Array(DCM_PixelData, bytes, &count).good();
        if (!ok || count < plane || count > plane + (slice.bitsAllocated == 8 && plane % 2 ? 1 : 0))
            throw std::runtime_error("Truncated or invalid DICOM PixelData.");
        const unsigned mask = (1u << slice.bitsStored) - 1;
        const unsigned sign = 1u << (slice.bitsStored - 1);
        const int shift = slice.highBit + 1 - slice.bitsStored;
        for (size_t p = 0; p < plane; ++p) {
            const unsigned raw = ((words ? words[p] : bytes[p]) >> shift) & mask;
            const int value = slice.signedPixels && (raw & sign)
                                  ? int(raw) - int(1u << slice.bitsStored)
                                  : int(raw);
            const double scaled = value * slice.slope + slice.intercept;
            if (!std::isfinite(scaled) || std::abs(scaled) > std::numeric_limits<float>::max())
                throw std::runtime_error(
                    "Rescaled image intensity is outside the supported range.");
            output[z * plane + p] = static_cast<float>(scaled);
        }
        if (progress)
            progress(double(z + 1) / series.slices.size());
    }
    validateVolume(image);
    return image;
}

void validateVolume(vtkImageData *image) {
    if (!image)
        throw std::runtime_error("No image volume is loaded.");
    const int *dims = image->GetDimensions();
    const double *spacing = image->GetSpacing();
    size_t count = 1;
    for (int i = 0; i < 3; ++i) {
        if (dims[i] < 2)
            throw std::runtime_error("A 3D surface requires at least two voxels along every axis.");
        if (!std::isfinite(spacing[i]) || spacing[i] <= 0)
            throw std::runtime_error("Volume spacing must be finite and strictly positive.");
        if (!std::isfinite(image->GetOrigin()[i]))
            throw std::runtime_error("Volume origin must be finite.");
        if (size_t(dims[i]) > 256ULL * 1024 * 1024 / count)
            throw std::runtime_error("Volume exceeds the 256-million-voxel memory limit.");
        count *= dims[i];
        for (int j = 0; j < 3; ++j) {
            double product = 0;
            for (int k = 0; k < 3; ++k)
                product += image->GetDirectionMatrix()->GetElement(k, i) *
                           image->GetDirectionMatrix()->GetElement(k, j);
            if (!std::isfinite(product) || !close(product, i == j ? 1 : 0, 1e-3))
                throw std::runtime_error("Volume directions must be finite, orthonormal axes.");
        }
    }
    if (image->GetNumberOfScalarComponents() != 1 || !image->GetScalarPointer())
        throw std::runtime_error("A scalar image volume is required.");
}
} // namespace d2m
