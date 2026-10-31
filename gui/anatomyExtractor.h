#pragma once
#include <QString>
#include <QStringList>
#include <atomic>
#include <vtkSmartPointer.h>

class vtkImageData;
struct SegmentationResult {
    vtkSmartPointer<vtkImageData> mask, distance;
};
struct SegmentationRuntime {
    QString python, script, model;
    QStringList prefixArguments;
    QString setupInstructions;
    static SegmentationRuntime defaults();
    static SegmentationRuntime skull();
};

// Runs a child process with argument vectors (no shell). All intermediate
// images are local, anonymous, and removed by QTemporaryDir on every exit.
SegmentationResult extractAnatomy(vtkImageData *volume, const SegmentationRuntime &runtime,
                                  const std::atomic_bool &cancelled,
                                  vtkImageData *secondary = nullptr);
