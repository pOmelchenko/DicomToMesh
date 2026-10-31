#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Optional CHARM backend; see licenses/GPL-3.0.txt and licenses/CHARM.md.
"""Experimental local MRI bone estimation with SimNIBS CHARM 4.6.0."""
import argparse
import gc
import json
import os
from pathlib import Path
import sys

from brain_extract import load_input


def runtime():
    import simnibs
    if simnibs.__version__ != "4.6.0":
        raise ValueError("This backend requires the locked SimNIBS 4.6.0 environment.")
    return simnibs


def export_result(subject, source, work):
    import nibabel as nib
    from nibabel.processing import resample_from_to
    import numpy as np
    from scipy.ndimage import distance_transform_edt

    label = nib.load(subject / "label_prep/tissue_labeling_upsampled.nii.gz")
    bone = np.isin(np.asanyarray(label.dataobj), [7, 8])  # compact and spongy bone, v4.6 atlas
    if not bone.any() or bone.all():
        raise ValueError("CHARM did not return a valid bone region.")
    spacing = nib.affines.voxel_sizes(label.affine)
    # Keep the upsampled field for the surface; a native-grid mask alone can
    # lose thin bone between thick acquisition slices. This remains an estimate.
    distance = distance_transform_edt(~bone, sampling=spacing).astype(np.float32)
    distance -= distance_transform_edt(bone, sampling=spacing).astype(np.float32)
    distance += 1  # common bridge convention: inside < 1, boundary at 1
    native = resample_from_to(nib.Nifti1Image(distance, label.affine), source, order=1, cval=1000)
    mask = (np.asanyarray(native.dataobj) < 1).astype(np.uint8)
    if not mask.any():
        raise ValueError("The estimated bone does not intersect the source image.")
    lps = np.diag([-1, -1, 1, 1]) @ label.affine
    geometry = dict(dimensions=list(bone.shape), spacing=spacing.tolist(),
                    originLPS=lps[:3, 3].tolist(),
                    directionLPS=(lps[:3, :3] / spacing).ravel().tolist())
    (work / "distance-geometry.json").write_text(json.dumps(geometry))
    (work / "distance.f32").write_bytes(distance.astype("<f4").tobytes(order="F"))
    (work / "mask.u8").write_bytes(mask.tobytes(order="F"))


def extract(work, secondary):
    import nibabel as nib
    simnibs = runtime()
    from simnibs.cli import charm

    source = load_input(work)
    nib.save(source, work / "source-t1.nii.gz")
    argv = ["charm", "skull", str(work / "source-t1.nii.gz")]
    if secondary is not None:
        nib.save(load_input(secondary), work / "source-t2.nii.gz")
        argv += [str(work / "source-t2.nii.gz"), "--registerT2"]
    settings = (Path(simnibs.SIMNIBSDIR) / "charm.ini").read_text()
    if 'init_type = "trega"' not in settings or "threads = 8" not in settings:
        raise ValueError("Unexpected CHARM configuration; use the locked environment.")
    # Atlas initialization avoids automatic network/model downloads. Cortical
    # surface fitting and tetrahedral meshing are unnecessary for bone extraction.
    settings = settings.replace('init_type = "trega"', 'init_type = "atlas"').replace("threads = 8", "threads = 4")
    (work / "settings.ini").write_text(settings)
    argv += ["--initatlas", "--segment", "--forcesform", "--usesettings", str(work / "settings.ini")]
    original_args, original_directory = sys.argv, Path.cwd()
    try:
        os.chdir(work)
        sys.argv = argv
        charm.main()
    finally:
        sys.argv = original_args
        os.chdir(original_directory)
    gc.collect()
    export_result(work / "m2m_skull", source, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--secondary", type=Path)
    args = parser.parse_args()
    if args.check:
        runtime()
        print("CHARM 4.6.0 ready. MRI bone estimation is experimental and requires anatomical review.")
    elif args.work_dir:
        extract(args.work_dir.resolve(), args.secondary.resolve() if args.secondary else None)
    else:
        parser.error("choose --check or --work-dir")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"MRI skull estimation failed: {error}", file=sys.stderr)
        sys.exit(1)
