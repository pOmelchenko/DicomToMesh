# SynthStrip attribution

All or portions of this licensed product (such portions are the "Software")
have been obtained under license from The General Hospital Corporation "MGH"
and are subject to the terms and conditions in [LICENSE.txt](LICENSE.txt).

`mri_synthstrip.py` comes from FreeSurfer commit
`cf4bccf24875a47245b4df9fc9372d0f1c3d784f`:
https://github.com/freesurfer/freesurfer/blob/cf4bccf24875a47245b4df9fc9372d0f1c3d784f/mri_synthstrip/mri_synthstrip

The original file has SHA-256
`291c253ab2f7c0cbcb84afa769ae10909549537e727db5b92d268754b7cc7871`.
The local version explicitly uses `torch.load(..., weights_only=True)`.

Reference: A. Hoopes, J. S. Mora, A. V. Dalca, B. Fischl, M. Hoffmann,
*SynthStrip: Skull-Stripping for Any Brain Image*, NeuroImage 260 (2022), 119474.
https://doi.org/10.1016/j.neuroimage.2022.119474

The separately downloaded no-CSF version 1 model is available under the
MIT or CC BY 4.0 license, as stated by the authors:
https://surfer.nmr.mgh.harvard.edu/docs/synthstrip/
DicomToMesh uses the CC BY 4.0 option; the weights are not part of this repository.
