# Optional MRI skull backend

`skull_extract.py` integrates SimNIBS CHARM 4.6.0 in a separate Python process
and is provided under GPL-3.0-only; see [GPL-3.0.txt](GPL-3.0.txt).
The C++ application communicates with this optional backend using local
voxel/geometry files. The managed build and brain environments are separate.

SimNIBS is developed by the SimNIBS contributors:
https://github.com/simnibs/simnibs/tree/v4.6.0

Method: Puonti et al. (2020), *Accurate and robust whole-head segmentation
from magnetic resonance images for individualized head modeling*,
NeuroImage 219, 117044. https://doi.org/10.1016/j.neuroimage.2020.117044

The optional environment downloads upstream SimNIBS, SAMSEG, Cortech and
other dependencies with their respective licenses. Wheel hashes and Git
revisions are recorded in `environments/skull/pixi.lock`; model/atlas files
and dependency binaries are not redistributed in this repository.
