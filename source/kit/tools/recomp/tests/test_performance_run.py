"""Do not admit a renderer's compatibility label as a measured gameplay phase."""
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "performance_run", Path(__file__).resolve().parents[1] / "performance_run.py")
performance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(performance)


def test_phase_samples_require_validity_and_unique_gameplay():
    rows = [
        {"frame_id": "1", "screen_class": "2", "repeat": "0"},
        {"frame_id": "2", "screen_class": "2", "repeat": "0", "screen_class_valid": "0"},
        {"frame_id": "3", "screen_class": "2", "repeat": "1", "screen_class_valid": "1"},
        {"frame_id": "4", "screen_class": "0", "repeat": "0", "screen_class_valid": "1"},
        {"frame_id": "5", "screen_class": "1", "repeat": "0", "screen_class_valid": "1"},
        {"frame_id": "6", "screen_class": "2", "repeat": "0", "screen_class_valid": "1"},
    ]
    assert performance.classified_gameplay_rows(rows) == [rows[-1]]
    assert performance.classified_gameplay_rows(rows[:-1]) == []
