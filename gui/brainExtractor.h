#pragma once
#include <QString>
#include <QStringList>
#include <atomic>
#include <vtkSmartPointer.h>

class vtkImageData;
struct BrainResult {
    vtkSmartPointer<vtkImageData> mask, distance;
};
struct BrainRuntime {
    QString python, script, model;
    QStringList prefixArguments;
    static BrainRuntime defaults();
};

// Runs a child process with argument vectors (no shell). All intermediate
// images are local, anonymous, and removed by QTemporaryDir on every exit.
BrainResult extractBrain(vtkImageData *volume, const BrainRuntime &runtime,
                         const std::atomic_bool &cancelled);
