import struct
from probe import parse_diag


def test_diag_old_prefix_and_v11_metrics():
    prefix = bytes([0x15, 2]) + struct.pack("<IIBbbII", 1, 2, 5, 0, 0, 3, 4)
    assert len(prefix) == 21
    assert parse_diag(prefix)["tick"] == 4
    v10 = prefix + struct.pack("<IIIIH", 1000, 500, 400, 500, 256)
    assert parse_diag(v10)["heap_free"] == 500
    assert "scene_renders" not in parse_diag(v10)
    v11 = v10 + struct.pack("<IIHH", 12, 10, 5, 99)
    out = parse_diag(v11)
    assert out["scene_renders"] == 12
    assert out["scene_copies"] == 10
    assert out["scene_last_ms"] == 5
    assert out["scene_max_ms"] == 99
