import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import nibabel as nib
import numpy as np

spec = importlib.util.spec_from_file_location("brain_extract", Path(__file__).parents[1] / "brain_extract.py")
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)


class BrainBridgeTest(unittest.TestCase):
    def test_dicom_lps_to_nifti_ras_preserves_oblique_physical_coordinates(self):
        angle = np.deg2rad(17)
        direction = np.array([[np.cos(angle), -np.sin(angle), 0],
                              [np.sin(angle), np.cos(angle), 0], [0, 0, 1]])
        spacing, origin = [0.7, 0.9, 6.5], [100, -40, 20]
        metadata = dict(dimensions=[2, 3, 4], spacing=spacing, originLPS=origin,
                        directionLPS=direction.ravel().tolist())
        with tempfile.TemporaryDirectory(prefix="brain test's ") as directory:
            work = Path(directory)
            (work / "geometry.json").write_text(json.dumps(metadata))
            np.arange(24, dtype="<f4").tofile(work / "input.f32")
            image = bridge.load_input(work)
            nib.save(image, work / "input.nii.gz")
            reloaded = nib.load(work / "input.nii.gz")
            self.assertEqual(reloaded.dataobj[1, 2, 3], 23)
            lps = direction @ (np.array([1, 2, 3]) * spacing) + origin
            ras = reloaded.affine @ [1, 2, 3, 1]
            np.testing.assert_allclose(ras[:3], lps * [-1, -1, 1], atol=1e-4)

    def test_nonfinite_input_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            metadata = dict(dimensions=[2, 2, 2], spacing=[1, 1, 1], originLPS=[0, 0, 0],
                            directionLPS=np.eye(3).ravel().tolist())
            (work / "geometry.json").write_text(json.dumps(metadata))
            values = np.arange(8, dtype="<f4")
            values[3] = np.nan
            values.tofile(work / "input.f32")
            with self.assertRaises(ValueError):
                bridge.load_input(work)

    def test_corrupt_download_preserves_existing_file(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "model.pt"
            target.write_bytes(b"old")
            with patch.object(bridge.urllib.request, "urlopen", return_value=io.BytesIO(b"bad")):
                with self.assertRaises(ValueError):
                    bridge.download_model(target)
            self.assertEqual(target.read_bytes(), b"old")
            self.assertEqual(list(target.parent.iterdir()), [target])

    def test_verified_model_does_not_download_again(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "model.pt"
            target.write_bytes(b"verified")
            with patch.object(bridge, "MODEL_SHA256", hashlib.sha256(b"verified").hexdigest()):
                with patch.object(bridge.urllib.request, "urlopen") as request:
                    bridge.download_model(target)
                    request.assert_not_called()


if __name__ == "__main__":
    unittest.main()
