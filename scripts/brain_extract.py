#!/usr/bin/env python3
"""Local SynthStrip bridge. Only geometry and voxel values cross this interface."""
import argparse
import hashlib
import json
from pathlib import Path
import os
import runpy
import sys
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MODEL = ROOT / ".pixi/models/synthstrip.nocsf.1.pt"
MODEL_URL = "https://surfer.nmr.mgh.harvard.edu/docs/synthstrip/requirements/synthstrip.nocsf.1.pt"
MODEL_SHA256 = "62bf01137c45b5f0cc04d59dbaed5b9ac138b3f25b766c062a7c1a0d696ecb28"


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def download_model(destination):
    if destination.is_file() and digest(destination) == MODEL_SHA256:
        print("SynthStrip no-CSF v1 weights already verified.")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
            temporary = Path(output.name)
            print("Downloading SynthStrip no-CSF v1 weights (29 MB)...", flush=True)
            with urllib.request.urlopen(MODEL_URL, timeout=30) as response:
                while chunk := response.read(1024 * 1024):
                    output.write(chunk)
        if digest(temporary) != MODEL_SHA256:
            raise ValueError("SynthStrip model checksum mismatch; download discarded.")
        os.replace(temporary, destination)
        print("Weights verified. Inference runs locally; DICOM files are never uploaded.")
        print("Weights: SynthStrip authors, CC BY 4.0. See scripts/vendor/synthstrip/NOTICE.md.")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def load_input(work):
    import nibabel as nib
    import numpy as np

    metadata = json.loads((work / "geometry.json").read_text())
    shape = tuple(metadata["dimensions"])
    if len(shape) != 3 or any(type(d) is not int or d < 2 for d in shape):
        raise ValueError("Expected a three-dimensional volume.")
    count = int(np.prod(shape, dtype=np.int64))
    if count > 128 * 1024 * 1024:
        raise ValueError("Brain extraction exceeds the 128-million-voxel limit.")
    spacing = np.asarray(metadata["spacing"], dtype=float)
    origin = np.asarray(metadata["originLPS"], dtype=float)
    direction = np.asarray(metadata["directionLPS"], dtype=float).reshape(3, 3)
    if (spacing.shape != (3,) or origin.shape != (3,) or
            not np.all(np.isfinite([spacing, origin])) or np.any(spacing <= 0) or
            not np.allclose(direction.T @ direction, np.eye(3), atol=1e-5)):
        raise ValueError("Invalid image geometry.")
    values = np.fromfile(work / "input.f32", dtype="<f4")
    if values.size != count or not np.all(np.isfinite(values)) or np.ptp(values) <= 0:
        raise ValueError("Input intensities must be finite and non-constant.")
    affine = np.eye(4)
    affine[:3, :3] = direction @ np.diag(spacing)
    affine[:3, 3] = origin
    affine = np.diag([-1, -1, 1, 1]) @ affine  # DICOM LPS -> NIfTI RAS
    image = nib.Nifti1Image(values.reshape(shape, order="F"), affine)
    image.set_qform(affine, code=1)
    image.set_sform(affine, code=1)
    return image


def extract(work, model, threads):
    import nibabel as nib
    import numpy as np

    if not model.is_file() or digest(model) != MODEL_SHA256:
        raise ValueError("Verified SynthStrip weights are missing. Run: pixi run -e brain brain-setup")
    image = load_input(work)
    input_path = work / "input.nii.gz"
    mask_path = work / "mask.nii.gz"
    distance_path = work / "distance.nii.gz"
    nib.save(image, input_path)
    original_args = sys.argv
    try:
        sys.argv = ["mri_synthstrip", "-i", str(input_path), "-m", str(mask_path),
                    "-d", str(distance_path), "--model", str(model), "--no-csf",
                    "-t", str(threads)]
        runpy.run_path(str(ROOT / "scripts/vendor/synthstrip/mri_synthstrip.py"), run_name="__main__")
    finally:
        sys.argv = original_args
    outputs = [nib.load(mask_path), nib.load(distance_path)]
    for output in outputs:
        if output.shape != image.shape or not np.allclose(output.affine, image.affine, atol=1e-3):
            raise ValueError("SynthStrip output does not match the source geometry.")
    mask = np.asarray(outputs[0].dataobj, dtype=np.uint8)
    distance = np.asarray(outputs[1].dataobj, dtype=np.float32)
    if not np.any(mask) or not np.all(np.isin(mask, [0, 1])) or not np.all(np.isfinite(distance)):
        raise ValueError("SynthStrip returned an empty or invalid brain mask.")
    # Match the reviewed largest-component/filled mask even in removed islands
    # or filled cavities. The distance-field is used only while edits are absent.
    distance = np.where(mask != 0, np.minimum(distance, 0.999), np.maximum(distance, 1.001))
    (work / "mask.u8").write_bytes(mask.tobytes(order="F"))
    (work / "distance.f32").write_bytes(distance.astype("<f4").tobytes(order="F"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--download-model", action="store_true")
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--model", type=Path, default=MODEL)
    parser.add_argument("--threads", type=int, default=4)
    args = parser.parse_args()
    if args.download_model:
        download_model(args.model)
    elif args.work_dir:
        if not 1 <= args.threads <= 32:
            parser.error("--threads must be between 1 and 32")
        extract(args.work_dir, args.model, args.threads)
    else:
        parser.error("choose --download-model or --work-dir")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"Brain extraction failed: {error}", file=sys.stderr)
        sys.exit(1)
