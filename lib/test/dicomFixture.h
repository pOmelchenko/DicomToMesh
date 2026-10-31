#pragma once
#include <array>
#include <chrono>
#include <dcmtk/config/osconfig.h>
#include <dcmtk/dcmdata/dctk.h>
#include <dcmtk/dcmdata/dcuid.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace fixture {
struct Slice {
    std::string uid = "1.2.826.0.1.3680043.10.999.1", description = "Synthetic T1", number = "1";
    std::array<double, 3> position{0, 0, 0};
    std::array<double, 6> orientation{1, 0, 0, 0, 1, 0};
    std::array<double, 2> spacing{2, 1};
    int rows = 8, columns = 8, bitsStored = 16, highBit = 15;
    bool signedPixels = true;
    double slope = 1, intercept = 0;
    std::string modality = "MR", type = "ORIGINAL\\PRIMARY", photo = "MONOCHROME2";
    std::vector<Uint16> pixels;
};
template <size_t N> std::string joined(const std::array<double, N> &values) {
    std::string result;
    for (double v : values) {
        if (!result.empty())
            result += "\\";
        result += std::to_string(v);
    }
    return result;
}
inline void write(const std::filesystem::path &path, const Slice &s,
                  E_TransferSyntax syntax = EXS_LittleEndianExplicit) {
    DcmFileFormat file;
    auto &ds = *file.getDataset();
    ds.putAndInsertString(DCM_SOPClassUID,
                          s.modality == "CT" ? UID_CTImageStorage : UID_MRImageStorage);
    char uid[100];
    dcmGenerateUniqueIdentifier(uid, SITE_INSTANCE_UID_ROOT);
    ds.putAndInsertString(DCM_SOPInstanceUID, uid);
    ds.putAndInsertString(DCM_StudyInstanceUID, "1.2.826.0.1.3680043.10.999.100");
    ds.putAndInsertString(DCM_SeriesInstanceUID, s.uid.c_str());
    ds.putAndInsertString(DCM_SeriesDescription, s.description.c_str());
    ds.putAndInsertString(DCM_SeriesNumber, s.number.c_str());
    ds.putAndInsertString(DCM_Modality, s.modality.c_str());
    ds.putAndInsertString(DCM_ImageType, s.type.c_str());
    ds.putAndInsertString(DCM_ImagePositionPatient, joined(s.position).c_str());
    ds.putAndInsertString(DCM_ImageOrientationPatient, joined(s.orientation).c_str());
    ds.putAndInsertString(DCM_PixelSpacing, joined(s.spacing).c_str());
    ds.putAndInsertUint16(DCM_Rows, s.rows);
    ds.putAndInsertUint16(DCM_Columns, s.columns);
    ds.putAndInsertUint16(DCM_SamplesPerPixel, 1);
    ds.putAndInsertString(DCM_PhotometricInterpretation, s.photo.c_str());
    ds.putAndInsertUint16(DCM_BitsAllocated, 16);
    ds.putAndInsertUint16(DCM_BitsStored, s.bitsStored);
    ds.putAndInsertUint16(DCM_HighBit, s.highBit);
    ds.putAndInsertUint16(DCM_PixelRepresentation, s.signedPixels ? 1 : 0);
    ds.putAndInsertString(DCM_RescaleSlope, std::to_string(s.slope).c_str());
    ds.putAndInsertString(DCM_RescaleIntercept, std::to_string(s.intercept).c_str());
    auto pixels = s.pixels;
    if (pixels.empty())
        pixels.assign(size_t(s.rows) * s.columns, 100);
    ds.putAndInsertUint16Array(DCM_PixelData, pixels.data(), pixels.size());
    const auto status = file.saveFile(path.string().c_str(), syntax);
    if (status.bad())
        throw std::runtime_error(status.text());
}
struct Directory {
    std::filesystem::path path;
    Directory() {
        static unsigned counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("d2m-synthetic-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(++counter));
        std::filesystem::create_directories(path);
    }
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};
inline void stack(const std::filesystem::path &folder, Slice slice = {}, int count = 3,
                  double step = 5.5) {
    for (int i = 0; i < count; ++i) {
        slice.position[2] = i * step;
        write(folder / (std::to_string(count - i) + ".dcm"), slice);
    }
}
} // namespace fixture
