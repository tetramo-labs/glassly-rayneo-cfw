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


def test_diag_head_metrics_and_truncated_extension():
    prefix = bytes([0x15, 2]) + bytes(49)
    v13 = prefix + struct.pack("<IBBB", 300, 2, 45, 1)
    out = parse_diag(v13)
    assert out["head_events"] == 300
    assert out["head_direction"] == 2
    assert out["head_angle"] == 45
    assert out["head_config"] == 1
    for length in range(51, 58):
        assert "head_events" not in parse_diag(v13[:length])
