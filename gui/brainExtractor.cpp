#include "brainExtractor.h"
#include "dicomSeries.h"
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
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkDataArray.h>

namespace {
void write(const QString &path, const char *data, qint64 size) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data, size) != size || !file.flush())
        throw std::runtime_error("Cannot write temporary brain-extraction data.");
}
QByteArray read(const QString &path, qint64 expected) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() != expected)
        throw std::runtime_error("Brain extraction returned missing or incorrectly sized data.");
    const auto data = file.readAll();
    if (data.size() != expected)
        throw std::runtime_error("Cannot read brain-extraction output.");
    return data;
}
} // namespace

BrainRuntime BrainRuntime::defaults() {
    const QString root = QString::fromUtf8(D2M_PROJECT_ROOT);
    return {qEnvironmentVariable("D2M_BRAIN_PYTHON", root + "/.pixi/envs/brain/bin/python"),
            root + "/scripts/brain_extract.py",
            root + "/.pixi/models/synthstrip.nocsf.1.pt",
            {}};
}

BrainResult extractBrain(vtkImageData *volume, const BrainRuntime &runtime,
                         const std::atomic_bool &cancelled) {
    d2m::validateVolume(volume);
    if (cancelled.load())
        throw std::runtime_error("Brain extraction cancelled.");
    if (!QFileInfo::exists(runtime.python) || !QFileInfo::exists(runtime.script) ||
        !QFileInfo::exists(runtime.model))
        throw std::runtime_error("Brain extraction needs its optional environment and model. "
                                 "From the project folder run: pixi install -e brain --locked; "
                                 "pixi run -e brain brain-setup");
    if (QSysInfo::ByteOrder != QSysInfo::LittleEndian)
        throw std::runtime_error("Brain extraction currently requires a little-endian host.");
    const qint64 count = volume->GetNumberOfPoints();
    if (count > 128LL * 1024 * 1024)
        throw std::runtime_error("Brain extraction exceeds the 128-million-voxel limit.");
    QTemporaryDir temporary;
    if (!temporary.isValid())
        throw std::runtime_error("Cannot create a temporary brain-extraction directory.");
    QJsonArray dimensions, spacing, origin, direction;
    for (int a = 0; a < 3; ++a) {
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
    write(temporary.filePath("geometry.json"), json.constData(), json.size());
    std::vector<float> values(count);
    const auto scalars = volume->GetPointData()->GetScalars();
    for (qint64 p = 0; p < count; ++p)
        values[p] = float(scalars->GetComponent(p, 0));
    write(temporary.filePath("input.f32"), reinterpret_cast<const char *>(values.data()),
          count * 4);
    values.clear();
    values.shrink_to_fit();
    QProcess process;
    auto arguments = runtime.prefixArguments;
    arguments << runtime.script << "--work-dir" << temporary.path() << "--model" << runtime.model;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(runtime.python, arguments);
    if (!process.waitForStarted(10000))
        throw std::runtime_error("Cannot start the brain-extraction Python process.");
    QElapsedTimer timer;
    timer.start();
    QByteArray log;
    for (;;) {
        const bool finished = process.waitForFinished(100);
        log += process.readAll();
        log = log.right(8192);
        if (cancelled.load() || timer.elapsed() > 20 * 60 * 1000) {
            process.kill();
            process.waitForFinished(5000);
            throw std::runtime_error(cancelled.load() ? "Brain extraction cancelled."
                                                      : "Brain extraction timed out (20 minutes).");
        }
        if (finished)
            break;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        throw std::runtime_error(
            ("Brain extraction failed. " + QString::fromUtf8(log)).toStdString());
    const auto maskBytes = read(temporary.filePath("mask.u8"), count);
    const auto distanceBytes = read(temporary.filePath("distance.f32"), count * 4);
    BrainResult result{vtkSmartPointer<vtkImageData>::New(), vtkSmartPointer<vtkImageData>::New()};
    result.mask->CopyStructure(volume);
    result.distance->CopyStructure(volume);
    result.mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    result.distance->AllocateScalars(VTK_FLOAT, 1);
    std::memcpy(result.mask->GetScalarPointer(), maskBytes.constData(), count);
    std::memcpy(result.distance->GetScalarPointer(), distanceBytes.constData(), count * 4);
    const auto mask = static_cast<unsigned char *>(result.mask->GetScalarPointer());
    const auto distance = static_cast<float *>(result.distance->GetScalarPointer());
    qint64 foreground = 0;
    for (qint64 p = 0; p < count; ++p) {
        foreground += mask[p] != 0;
        if (mask[p] > 1 || !std::isfinite(distance[p]) || ((distance[p] < 1) != bool(mask[p])))
            throw std::runtime_error("Brain extraction returned an invalid mask/distance field.");
    }
    if (!foreground)
        throw std::runtime_error("Brain extraction returned an empty mask.");
    return result;
}
