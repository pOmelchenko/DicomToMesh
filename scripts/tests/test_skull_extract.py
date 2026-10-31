import json
from pathlib import Path
import sys
import tempfile
import unittest

import nibabel as nib
import numpy as np

sys.path.insert(0, str(Path(__file__).parents[1]))
from skull_extract import export_result


class SkullBridgeTest(unittest.TestCase):
    def test_preserves_fine_field_and_native_overlay_with_lps_geometry(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            labels = np.zeros((10, 12, 14), dtype=np.uint8)
            labels[2:8, 2:10, 2:12] = 7
            labels[4:6, 4:8, 4:10] = 8
            labels[0, 0, 0] = 2  # gray matter must not become bone
            affine = np.array([[0, -.5, 0, 10], [.5, 0, 0, 20], [0, 0, .5, 30], [0, 0, 0, 1]])
            folder = work / "label_prep"
            folder.mkdir()
            nib.save(nib.Nifti1Image(labels, affine), folder / "tissue_labeling_upsampled.nii.gz")
            native_affine = affine.copy()
            native_affine[:3, :3] *= 2
            source = nib.Nifti1Image(np.zeros((5, 6, 7)), native_affine)
            export_result(work, source, work)
            mask = np.fromfile(work / "mask.u8", dtype=np.uint8).reshape(source.shape, order="F")
            self.assertEqual(mask[1, 1, 1], 1)
            self.assertEqual(mask[4, 1, 1], 0)
            self.assertEqual(mask[0, 0, 0], 0)
            field = np.fromfile(work / "distance.f32", dtype="<f4").reshape(labels.shape, order="F")
            self.assertTrue(np.all(np.isfinite(field)))
            self.assertTrue(np.all((field < 1) == np.isin(labels, [7, 8])))
            geometry = json.loads((work / "distance-geometry.json").read_text())
            self.assertEqual(geometry["dimensions"], list(labels.shape))
            np.testing.assert_allclose(geometry["originLPS"], [-10, -20, 30])
            np.testing.assert_allclose(np.array(geometry["directionLPS"]).reshape(3, 3),
                                       [[0, 1, 0], [-1, 0, 0], [0, 0, 1]])

    def test_rejects_empty_bone_estimate(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "label_prep").mkdir()
            image = nib.Nifti1Image(np.zeros((4, 4, 4), dtype=np.uint8), np.eye(4))
            nib.save(image, work / "label_prep/tissue_labeling_upsampled.nii.gz")
            with self.assertRaises(ValueError):
                export_result(work, image, work)


if __name__ == "__main__":
    unittest.main()
