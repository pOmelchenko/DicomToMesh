#include "dicomConverter.h"
#include "meshData.h"
#include "meshRoutines.h"
#include <QFileInfo>
#include <filesystem>
#include <stdexcept>

void DicomConverter::scan(const QString &directory) {
    try {
        emit catalogReady(d2m::scanDicomDirectory(directory.toStdString()));
    } catch (const std::exception &e) {
        emit failed("scan", QString::fromUtf8(e.what()));
    }
}
void DicomConverter::load(const d2m::DicomSeries &series) {
    try {
        m_volume = nullptr;
        m_brainMask = nullptr;
        m_brainDistance = nullptr;
        m_useBrainDistance = false;
        m_mask = nullptr;
        m_original = nullptr;
        m_processed = nullptr;
        m_volume = d2m::loadDicomSeries(series, [this](double p) { emit progress(int(p * 100)); });
        emit volumeReady(m_volume, d2m::volumeHistogram(m_volume));
    } catch (const std::exception &e) {
        emit failed("load", QString::fromUtf8(e.what()));
    }
}
void DicomConverter::segment(const d2m::SegmentationOptions &options) {
    m_mask = nullptr;
    m_original = nullptr;
    m_processed = nullptr;
    m_useBrainDistance = false;
    try {
        if (options.mode == d2m::SegmentationMode::Brain) {
            if (!m_brainMask) {
                const auto brain = extractBrain(m_volume, m_brainRuntime, m_cancelled);
                m_brainMask = brain.mask;
                m_brainDistance = brain.distance;
            }
            m_mask = d2m::correctMask(m_brainMask, options.strokes);
            m_useBrainDistance = options.strokes.empty();
        } else
            m_mask = d2m::segmentVolume(m_volume, options);
        emit maskReady(m_mask);
    } catch (const std::exception &e) {
        emit failed("segment", QString::fromUtf8(e.what()));
    }
}
void DicomConverter::processMesh(const MeshOptions &options) {
    if (!m_original || !m_original->GetNumberOfCells())
        throw std::runtime_error("Build a non-empty surface first.");
    if (options.reduction < 0 || options.reduction > 0.95 || options.smoothingIterations < 0 ||
        options.smoothingIterations > 100)
        throw std::runtime_error("Invalid mesh processing settings.");
    auto result = MeshPtr::New();
    result->DeepCopy(m_original);
    VTKMeshRoutines routines;
    if (options.reduction > 0)
        routines.meshReduction(result, options.reduction);
    if (options.smoothingIterations > 0)
        routines.smoothMesh(result, options.smoothingIterations);
    if (options.center)
        routines.moveMeshToCOSCenter(result);
    m_processed = result;
    emit meshReady(m_original, m_processed);
}
void DicomConverter::build(double spacing, const MeshOptions &options) {
    try {
        if (!m_mask || !d2m::summarizeMask(m_mask).foreground)
            throw std::runtime_error("Review a non-empty mask before building a surface.");
        m_original = m_useBrainDistance ? d2m::surfaceFromDistance(m_brainDistance, spacing)
                                      : d2m::surfaceFromMask(m_mask, spacing);
        if (!m_original->GetNumberOfCells())
            throw std::runtime_error(
                "The mask has no surface. Adjust the thresholds or paint the target region.");
        processMesh(options);
    } catch (const std::exception &e) {
        emit failed("build", QString::fromUtf8(e.what()));
    }
}
void DicomConverter::process(const MeshOptions &options) {
    try {
        processMesh(options);
    } catch (const std::exception &e) {
        emit failed("process", QString::fromUtf8(e.what()));
    }
}
void DicomConverter::save(const QString &path, bool original) {
    try {
        const auto mesh = original ? m_original : m_processed;
        if (!mesh || !mesh->GetNumberOfCells())
            throw std::runtime_error("There is no current surface to export.");
        VTKMeshData writer;
        const QString suffix = QFileInfo(path).suffix().toLower();
        if (suffix == "stl")
            writer.exportAsStlFile(mesh, path.toStdString(), true);
        else if (suffix == "obj")
            writer.exportAsObjFile(mesh, path.toStdString());
        else if (suffix == "ply")
            writer.exportAsPlyFile(mesh, path.toStdString());
        else
            throw std::runtime_error("Choose an STL, OBJ or PLY filename.");
        if (!std::filesystem::exists(path.toStdString()) ||
            !std::filesystem::file_size(path.toStdString()))
            throw std::runtime_error(
                "The model could not be written. Check the destination and available disk space.");
        emit saved(path);
    } catch (const std::exception &e) {
        emit failed("save", QString::fromUtf8(e.what()));
    }
}
