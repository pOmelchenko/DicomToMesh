#include "anatomyExtractor.h"
#include "dicomSeries.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryDir>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vtkDataArray.h>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace {
void write(const QString &path, const char *data, qint64 size) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data, size) != size || !file.flush())
        throw std::runtime_error("Cannot write temporary segmentation data.");
}
QByteArray read(const QString &path, qint64 expected = -1) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || (expected >= 0 && file.size() != expected))
        throw std::runtime_error("Segmentation returned missing or incorrectly sized data.");
    const auto data = file.readAll();
    if (expected >= 0 && data.size() != expected)
        throw std::runtime_error("Cannot read segmentation output.");
    return data;
}
void writeVolume(vtkImageData *volume, const QString &directory) {
    d2m::validateVolume(volume);
    if (volume->GetNumberOfPoints() > 128LL * 1024 * 1024)
        throw std::runtime_error("Segmentation exceeds the 128-million-voxel limit.");
    QJsonArray dimensions, spacing, origin, direction;
    for (int a = 0; a < 3; ++a) {
        if (volume->GetExtent()[a * 2] != 0)
            throw std::runtime_error("The segmentation backend requires zero-based image extents.");
        dimensions.append(volume->GetDimensions()[a]);
        spacing.append(volume->GetSpacing()[a]);
        origin.append(volume->GetOrigin()[a]);
        for (int b = 0; b < 3; ++b)
            direction.append(volume->GetDirectionMatrix()->GetElement(a, b));
    }
    const auto json = QJsonDocument(QJsonObject{{"dimensions", dimensions},
                                                {"spacing", spacing},
                                                {"originLPS", origin},
                                                {"directionLPS", direction}})
                          .toJson(QJsonDocument::Compact);
    write(directory + "/geometry.json", json.constData(), json.size());
    std::vector<float> values(volume->GetNumberOfPoints());
    const auto scalars = volume->GetPointData()->GetScalars();
    for (size_t p = 0; p < values.size(); ++p)
        values[p] = float(scalars->GetComponent(p, 0));
    write(directory + "/input.f32", reinterpret_cast<const char *>(values.data()),
          values.size() * 4);
}
vtkSmartPointer<vtkImageData> fieldGeometry(const QString &path, vtkImageData *reference) {
    const auto object = QJsonDocument::fromJson(read(path)).object();
    const auto dimensions = object["dimensions"].toArray();
    const auto spacing = object["spacing"].toArray();
    const auto origin = object["originLPS"].toArray();
    const auto direction = object["directionLPS"].toArray();
    if (dimensions.size() != 3 || spacing.size() != 3 || origin.size() != 3 ||
        direction.size() != 9)
        throw std::runtime_error("Invalid segmentation field geometry.");
    int d[3];
    double s[3], o[3], matrix[9];
    qint64 count = 1;
    for (int a = 0; a < 3; ++a) {
        d[a] = dimensions[a].toInt();
        if (d[a] < 2 || d[a] > 128LL * 1024 * 1024 / count)
            throw std::runtime_error("Segmentation field exceeds the memory limit.");
        count *= d[a];
        s[a] = spacing[a].toDouble();
        o[a] = origin[a].toDouble();
    }
    for (int i = 0; i < 9; ++i)
        matrix[i] = direction[i].toDouble();
    auto result = vtkSmartPointer<vtkImageData>::New();
    result->SetDimensions(d);
    result->SetSpacing(s);
    result->SetOrigin(o);
    result->SetDirectionMatrix(matrix);
    result->AllocateScalars(VTK_FLOAT, 1);
    d2m::validateVolume(result);
    // Resampling may extend at most one native voxel beyond the reference.
    // Reject unrelated or corrupt output transforms before constructing a mesh.
    for (int corner = 0; corner < 8; ++corner) {
        double index[3], world[3], native[3];
        for (int a = 0; a < 3; ++a)
            index[a] = (corner & (1 << a)) ? d[a] - 1 : 0;
        result->TransformContinuousIndexToPhysicalPoint(index, world);
        reference->TransformPhysicalPointToContinuousIndex(world, native);
        for (int a = 0; a < 3; ++a)
            if (native[a] < -1.01 || native[a] > reference->GetDimensions()[a] + 0.01)
                throw std::runtime_error(
                    "Segmentation field is outside the source image geometry.");
    }
    return result;
}
void stop(QProcess &process) {
#ifdef Q_OS_UNIX
    if (process.processId() > 0)
        ::kill(-pid_t(process.processId()), SIGKILL);
#endif
    process.kill();
    process.waitForFinished(5000);
}
} // namespace

SegmentationRuntime SegmentationRuntime::defaults() {
    const QString root = QString::fromUtf8(D2M_PROJECT_ROOT);
    return {qEnvironmentVariable("D2M_BRAIN_PYTHON", root + "/.pixi/envs/brain/bin/python"),
            root + "/scripts/brain_extract.py",
            root + "/.pixi/models/synthstrip.nocsf.1.pt",
            {},
            "pixi install -e brain --locked; pixi run -e brain brain-setup"};
}
SegmentationRuntime SegmentationRuntime::skull() {
    const QString root = QString::fromUtf8(D2M_PROJECT_ROOT);
    return {qEnvironmentVariable("D2M_SKULL_PYTHON",
                                 root + "/environments/skull/.pixi/envs/default/bin/python"),
            root + "/scripts/skull_extract.py",
            {},
            {},
            "pixi run skull-setup (Apple Silicon/macOS 14+), "
            "or set D2M_SKULL_PYTHON to a SimNIBS 4.6.0 interpreter"};
}

SegmentationResult extractAnatomy(vtkImageData *volume, const SegmentationRuntime &runtime,
                                  const std::atomic_bool &cancelled, vtkImageData *secondary) {
    if (cancelled.load())
        throw std::runtime_error("Segmentation cancelled.");
    if (!QFileInfo::exists(runtime.python) || !QFileInfo::exists(runtime.script) ||
        (!runtime.model.isEmpty() && !QFileInfo::exists(runtime.model)))
        throw std::runtime_error(
            ("Install the optional segmentation environment from the project folder: " +
             runtime.setupInstructions)
                .toStdString());
    if (QSysInfo::ByteOrder != QSysInfo::LittleEndian)
        throw std::runtime_error("Segmentation currently requires a little-endian host.");
    QTemporaryDir temporary;
    if (!temporary.isValid())
        throw std::runtime_error("Cannot create a temporary segmentation directory.");
    writeVolume(volume, temporary.path());
    auto arguments = runtime.prefixArguments;
    arguments << runtime.script << "--work-dir" << temporary.path();
    if (!runtime.model.isEmpty())
        arguments << "--model" << runtime.model;
    if (secondary) {
        if (!QDir(temporary.path()).mkdir("secondary"))
            throw std::runtime_error("Cannot create the supporting image directory.");
        writeVolume(secondary, temporary.filePath("secondary"));
        arguments << "--secondary" << temporary.filePath("secondary");
    }
    QProcess process;
#ifdef Q_OS_UNIX
    process.setChildProcessModifier([] { ::setsid(); });
#endif
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(runtime.python, arguments);
    if (!process.waitForStarted(10000))
        throw std::runtime_error("Cannot start the segmentation Python process.");
    QElapsedTimer timer;
    timer.start();
    QByteArray log;
    for (;;) {
        const bool finished = process.waitForFinished(100);
        log += process.readAll();
        log = log.right(8192);
        if (cancelled.load() || timer.elapsed() > 30 * 60 * 1000) {
            stop(process);
            throw std::runtime_error(cancelled.load() ? "Segmentation cancelled."
                                                      : "Segmentation timed out (30 minutes).");
        }
        if (finished)
            break;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        throw std::runtime_error(("Segmentation failed. " + QString::fromUtf8(log)).toStdString());
    const qint64 count = volume->GetNumberOfPoints();
    const auto maskBytes = read(temporary.filePath("mask.u8"), count);
    const bool separateField = QFileInfo::exists(temporary.filePath("distance-geometry.json"));
    SegmentationResult result{vtkSmartPointer<vtkImageData>::New(),
                              vtkSmartPointer<vtkImageData>::New()};
    result.mask->CopyStructure(volume);
    result.mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    std::memcpy(result.mask->GetScalarPointer(), maskBytes.constData(), count);
    if (separateField)
        result.distance = fieldGeometry(temporary.filePath("distance-geometry.json"), volume);
    else {
        result.distance->CopyStructure(volume);
        result.distance->AllocateScalars(VTK_FLOAT, 1);
    }
    const auto fieldCount = result.distance->GetNumberOfPoints();
    const auto distanceBytes = read(temporary.filePath("distance.f32"), fieldCount * 4);
    std::memcpy(result.distance->GetScalarPointer(), distanceBytes.constData(), fieldCount * 4);
    const auto distance = static_cast<float *>(result.distance->GetScalarPointer());
    for (qint64 p = 0; p < fieldCount; ++p)
        if (!std::isfinite(distance[p]))
            throw std::runtime_error("Segmentation returned a non-finite distance field.");
    const auto mask = static_cast<unsigned char *>(result.mask->GetScalarPointer());
    qint64 foreground = 0;
    for (qint64 p = 0; p < count; ++p) {
        foreground += mask[p] != 0;
        if (mask[p] > 1 || (!separateField && ((distance[p] < 1) != bool(mask[p]))))
            throw std::runtime_error("Segmentation returned an invalid mask/distance field.");
    }
    if (!foreground)
        throw std::runtime_error("Segmentation returned an empty mask.");
    return result;
}
