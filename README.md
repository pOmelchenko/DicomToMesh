![Pixi build and tests](https://github.com/pOmelchenko/DicomToMesh/actions/workflows/cppVTK9Dicom.yml/badge.svg) ![Qt 6 native OpenGL preview](https://github.com/pOmelchenko/DicomToMesh/actions/workflows/cppVTK9DicomGUI.yml/badge.svg)

# DicomToMesh

DicomToMesh is a handy command line tool, which enables the user to automatically create a 3D mesh from a set of 2D DICOM images, a common image format used in medicine. The supported 3D mesh formats are STL, OBJ and PLY. DicomToMesh works on Linux, OSX and Windows.

<p align="center"><img alt="dicom2mesh" src="docs/img/dicomtomesh.png" width="80%"></p>

# Mesh Creation

The 3D surface mesh is computed by the marching cubes algorithm. As an input, this algorithm requires a threshold which indicates what range of voxel values should be considered. This threshold is also known as iso-value. In CT scans, the iso-value depends on the tissue (density). In Dicom2Mesh you can specify the iso-value with the parameter <code>-t X</code>, where X is an integer.

# Mesh Post-Processing Options

**Mesh reduction:** The mesh of a medical DICOM image can easily exceed 1 GB of data. Reducing the number of polygons, therefore, is a crucial feature. Reduction can be enabled by the option <code>-r X</code> where X is a floating-point value between 0.0 and 1.0. 

<p align="center"><img alt="reduction" src="docs/img/mesh-reduction.png" width="60%"></p>

**Mesh smoothing:** Acquired medical images contain often heavy noise. This is visible in the extracted 3D surface. Smoothing the mesh leads often to a better result. Smoothing can be enabled with the argument <code>-s</code>.

<p align="center"><img alt="smoothing" src="docs/img/mesh-smoothed.png" width="60%"></p>

**Remove small objects:** The resulting 3D mesh contains often parts which are not of interest, such as for example the screws of the table on which a CT scan of a patient was acquired. With DicomToMesh, you can remove objects below a certain size by adding the option <code>-e X</code> where X is a floating-point value between 0.0 and 1.0. <code>X</code> is a size threshold relative to the connected object with the most vertices. It is easy understandable with an example: The biggest connected object of the mesh has 1000 vertices. Then <code>-e 0.25</code> removes all connected objects with less than 250 vertices.    

<p align="center"><img alt="filter" src="docs/img/mesh-filter.png" width="80%"></p>


# Visualisation 

By passing the command line option <code>-v</code> the resulting mesh is visualised in a 3D environment. By double clicking the mesh's  surface, the corresponding 3D coordinate is printed to the shell.

<p align="center"><img alt="filter" src="docs/img/mesh-visualization.png" width="50%"></p>

The option <code>-vo</code> shows the input DICOM data in a volume renderer (vtkFixedPointVolumeRayCastMapper). The color map can be set with multiple parameters like <code>(Red,Green,Blue,Alpha,Iso-Value)</code>. As an example, the parameters <code>-vo (255,0,0,0,0) (255,0,0,60,700) (200,200,200,90,2000)</code> color voxels with values from 0-700 reddish and 700-2000 whitish. 

<p align="center"><img alt="filter" src="docs/img/volumerendering.png" width="50%"></p>

# Installation

Ubuntu 25.04, 24.04, 22.04, 18.04
```
> sudo add-apt-repository ppa:eidelen/d2m
> sudo apt-get update
> sudo apt-get install dicom2mesh
```
Note: Running dicom2mesh from PPA installation may throw an error about missing shared libraries.
If that happens, please let me know — I’ll update the RPATH configuration accordingly.
Also, if there are any Launchpad or PPA experts among you, I’d really appreciate your support!

On OSX and Windows it is required to build DicomToMesh yourself. 

### Using via Docker

There is a [docker file](docker/Dockerfile) ready for use. If you have docker installed you can use the generated image without installing anything locally.

#### Usage

```bash
# build image and tag it as `dicom2mesh`
docker build docker -t dicom2mesh
# use generated image to build an stl file
docker run -v `pwd`/my/dicom/files/:/data dicom2mesh -i /data -t 557 -o /data/mesh.stl
```

Your mesh will be created alongside your dicom images and named `mesh.stl`

# Building

The software is written in modern C++ on top of the powerful VTK library and is using CMake as a build-system.

## Building with Pixi (macOS and Linux)

Install [Pixi](https://pixi.sh/latest/installation/), then run these commands from the repository root:

```bash
pixi install --locked
pixi run build
pixi run test
pixi run gui
```

Pixi installs CMake, Ninja, pkg-config, a C++ compiler, DCMTK, VTK, Qt 6 and GoogleTest from
conda-forge. The committed `pixi.lock` records the exact packages for Apple
Silicon macOS, Intel macOS and x86-64 Linux (macOS 13+ or Linux with glibc
2.28+). On macOS, Apple's Xcode Command Line Tools (or Xcode) must also be
installed to provide the system SDK.

The environment uses VTK 9.5+ and Qt 6 together. Both the CLI and GUI use
DCMTK to inspect and decode DICOM, with the same geometry validation.
`USE_VTK_DICOM` is obsolete; enabling it no longer selects the old interactive loader.

The build includes the CLI, GUI and library, CLI and Qt GUI test suites. CMake finds
GoogleTest in the Pixi environment instead of downloading it. Build files
are kept in `build/pixi/`; binaries should be run through Pixi so their
runtime dependencies and Qt plugins are available. `gui`, `cli` and `test`
automatically configure and build the project before running. Tests copy
their fixtures into the build directory and keep generated files there.
On macOS the GUI is built as `build/pixi/gui/dicom2meshgui.app`.

```bash
pixi run cli -h
pixi run cli -i /path/to/dicom --list-series
pixi run cli -i /path/to/dicom --series SERIES_UID -t 557 -o mesh.stl
```

For paths containing apostrophes, Pixi 0.71 task argument quoting can fail.
After `pixi run build`, bypass its task shell and pass the arguments directly:

```bash
pixi run --executable ./build/pixi/dicom2mesh/dicom2mesh -i "/path/with an apostrophe's/DICOM" --list-series
```

## Building with system dependencies

Install CMake 3.24+, a C++17 compiler, VTK 9 and DCMTK with CMake package
configuration files. The GUI additionally requires Qt 6 and a VTK build
whose `GUISupportQt` module uses Qt 6. The Pixi lock file provides a tested
combination; arbitrary system VTK/Qt builds may use different Qt versions.

```bash
cmake -S . -B build/system -DCMAKE_BUILD_TYPE=Release -DBUILD_GUI=ON
cmake --build build/system --parallel
```

Set `CMAKE_PREFIX_PATH` or `VTK_DIR`, `DCMTK_DIR` and `Qt6_DIR` when the
dependencies are outside the default search paths. Set `BUILD_GUI=OFF`
for the CLI alone. The managed Pixi workflow targets macOS and Linux;
Windows packaging is not covered by its lock file.

## DICOM import and geometry

The directory is scanned recursively and grouped by Series Instance UID.
Opening a mixed folder never silently chooses its first series. The GUI
lists every series and explains why a localizer, report or inconsistent
acquisition is unavailable. The CLI loads automatically only when there
is exactly one valid 3D series; otherwise use `--list-series` and `--series`.

Slices are sorted by Image Position Patient projected onto the normal
from Image Orientation Patient. Pixel spacing, physical origin and LPS
orientation are preserved in the volume and exported surface. Zero or
irregular spacing, inconsistent dimensions/orientation, repeated positions
and sheared stacks are rejected with an explanation. A repeated-position
diffusion or time series must first be separated into individual volumes.

Single-frame monochrome 8/16-bit integer images are supported, including
signed stored bits, pixel rescale and the DCMTK JPEG, JPEG-LS and RLE
decoders. Enhanced multi-frame images, RGB reports and JPEG 2000 require
an appropriate external conversion. There is a 256-million-voxel volume
limit and a 128-million-voxel interpolation limit to bound memory use.

# How to use Dicom2Mesh

Command line arguments can be combined and passed in arbitrary order.

**Input and output:** The path to the DICOM directory is passed by the argument <code>-i dicomPath</code>. The file name of the resulting 3D mesh is specified by the parameter <code>-o meshPath</code>. This simple example transforms a DICOM data set into a 3D mesh file called mesh.stl, by using an iso-value of 557 <code>-t 557</code>. 

<code>> dicom2mesh -i pathToDicomDirectory -t 557 -o mesh.stl</code>

Alternatively, one can use an existing 3d mesh as input. This is useful if you want to apply only mesh post-processing routines and bypass the time consuming mesh creation step. This example imports the former mesh.stl, centers it and exports it in the OBJ mesh format.

<code>> dicom2mesh -i mesh.stl -c -o newMesh.obj</code>

**Mesh post-processing:** The following example shows different mesh post-processing methods applied to resulting surface mesh out of the marching cubes algorithm. In particular, a mesh is reduced by 90% of its original number faces <code>-r 0.9</code>. In addition, the mesh is smoothed <code>-s</code> and centred at the coordinate system's origin <code>-c</code>.  Another helpful function is the removal of small objects - here the removal of every object smaller than 5% of the biggest object <code>-e 0.05</code>.

<code>> dicom2mesh -i pathToDicomDirectory -r 0.9 -s -c -e 0.05 -o mesh.stl</code>

# GUI

Run `pixi run gui`, open a DICOM folder, choose an anatomical series and
click **Load selected series**. Inspect the three orthogonal image planes.
They keep the physical pixel aspect ratio; labels indicate the positive
image-axis directions in DICOM LPS coordinates. These are acquisition-axis
views, not a resliced anatomical atlas. Brightness/window controls affect
display only. A sampled histogram and Otsu suggestion help choose an
intensity range; the green overlay shows the actual segmentation mask.

Four methods are available:

- **Threshold:** an intensity range for CT or manual selection. A CT bone
  threshold is not a universal MRI threshold.
- **Head outline:** an approximate MRI outline with configurable gap
  closing in millimetres, largest-region selection and hole filling in
  acquisition slices. It does not automatically isolate brain or skull.
- **Region from seeds:** connected growth within the intensity range from
  manually painted foreground seeds. Right-button erasing can create
  barriers against leakage. This provides a manual-assisted tool for
  selecting a region such as brain tissue; all boundaries need review.
- **Brain · SynthStrip:** dedicated local brain extraction with the no-CSF
  version 1 model. This needs the optional environment described below.
  Selecting the method starts CPU inference; use **Cancel brain extraction**
  to stop it. The result is cached for the loaded series, so painting and
  undo do not run the network again. Threshold and morphology controls are
  disabled for this method.

Enable **Paint corrections / place seeds** to add with the left mouse
button and erase with the right mouse button in any plane. Brush size is
in millimetres; strokes can be undone or cleared. Corrections take
precedence over automatic hole filling. Masks and edits remain in the
current session; export the surface before loading another series.

Click **Build surface** after reviewing the mask. The 3D view is embedded
in the application. Optional interpolation reduces stair steps but does
not recover anatomical detail between thick slices. Smoothing, polygon
reduction and centering are disabled by default. **Apply processing**
always starts from a copy of the original surface. Switch **View / export**
between original and processed to compare triangle counts and geometry,
then export the visible version as STL, OBJ or PLY. Changing the mask
invalidates the old surface so it cannot be exported accidentally.

Surface extraction follows the selected mask inside the scanned field of
view. A region cut by a scan boundary can produce an open surface; this
workflow does not invent missing anatomy or automatically repair it for
printing. Coarse MRI with thick/gapped slices yields only an approximate
surface, regardless of interpolation or smoothing.

The mask coverage message lists scan faces touched by the segmentation.
Inspect those end slices: contact can mean truncated anatomy and an open
surface. Lack of contact alone does not establish segmentation accuracy.

## Optional brain extraction

From the repository root, install the separate CPU environment and download
the verified 29 MB model once:

```bash
pixi install -e brain --locked
pixi run -e brain brain-setup
pixi run gui
```

Load an anatomical MR series, select **Brain · SynthStrip (local CPU)**,
then review the green overlay throughout all three image planes before
building. Inference can take a few minutes. The original scan is not
modified or uploaded; temporary anonymous voxel/geometry files are deleted
after inference. Network access is used only by the explicit model setup
command. Intermediate volumes, masks, surfaces and weights stay in ignored
build/cache directories.

SynthStrip predicts a signed distance to the brain boundary using a
network trained on synthetic image contrasts and resolutions. The unedited
surface uses that distance field (border 1 mm); largest-component selection
and hole filling are reconciled with the reviewed binary mask. After brush
edits, extraction follows the edited binary mask. Optional interpolation
operates inside the acquired field of view and cannot recover missing
brain or accurate fine cortical folds from thick slices.

The brain Python environment is independent of the C++/VTK environment.
For a system build, `D2M_BRAIN_PYTHON` can point to an equivalent Python
interpreter with PyTorch, Surfa and NumPy; the build uses the helper and
verified model under its source tree. This is a source-tree workflow,
not a standalone distributable application bundle.

Method: [Hoopes et al., SynthStrip (2022)](https://doi.org/10.1016/j.neuroimage.2022.119474).
All or portions of this licensed product (such portions are the "Software")
have been obtained under license from The General Hospital Corporation "MGH"
and are subject to the terms and conditions in
[the FreeSurfer license](scripts/vendor/synthstrip/LICENSE.txt).
See [source revision, modifications and model attribution](scripts/vendor/synthstrip/NOTICE.md).
The separately downloaded weights use the authors' CC BY 4.0 option.

The CLI also supports approximate head outlines and optional interpolation:

```bash
pixi run cli -i /path/to/dicom --series SERIES_UID --head-outline \
  -t LOWER_INTENSITY -tu UPPER_INTENSITY --close-mm 1 --resample-mm 1 -o head.stl
```

## Verification

`pixi run test` runs synthetic DICOM geometry/decoding tests, segmentation
and coordinate tests, the existing mesh/CLI tests and headless Qt workflow
tests. Fixtures are generated at runtime and contain no patient data.
On a machine with a display, the native OpenGL check additionally verifies
that the actual 3D framebuffer contains a surface:

```bash
pixi run --executable ./build/pixi/gui/runGuiTests nativeSurfacePreview
```

The optional environment also has geometry and model-integrity tests:

```bash
pixi run -e brain brain-test
```

For a local end-to-end inference check with an existing study, set
`D2M_BRAIN_TEST_DICOM` to its directory and `D2M_BRAIN_TEST_SERIES` to the
selected UID, then run `runGuiTests realBrainExtraction` through Pixi.
`D2M_BRAIN_TEST_OUTPUT` optionally exports its original surface as STL.
These variables are unset in CI; no patient data is included in fixtures.

The icons in the gui were made by <a href="https://www.flaticon.com/authors/smashicons" title="Smashicons">Smashicons</a> from <a href="https://www.flaticon.com/" title="Flaticon">www.flaticon.com</a>. The license is <a href="http://creativecommons.org/licenses/by/3.0/" title="Creative Commons BY 3.0" target="_blank">CC 3.0 BY</a>

# Issues

## Unsupported DICOM Image Format
There exists many different DICOM Image formats and it happens often that Dicom2Mesh is unable to understand these data. 
With the following bash command, you can try to preprocess your DICOM images first before applying Dicom2Mesh.
```
for f in original-dicoms/*.dcm; do
    gdcmconv --raw "$f" "raw-dicoms/$(basename "$f")"
done  
```

# Contributors

DicomToMesh was an in-house product of AOT AG, a former robotic medtech startup from Basel. Since the software is based on several open-source projects and multiple Stack Overflow entries, we decided to make our code public as well. We hope somebody can use parts of it. Participants are most welcome.

Have fun :)

