import importlib.util
import pathlib
import struct
import unittest


MODULE_PATH = pathlib.Path(__file__).resolve().parents[1] / "pc_video_viewer.py"
SPEC = importlib.util.spec_from_file_location("pc_video_viewer", MODULE_PATH)
VIEWER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VIEWER)


class ChunkedConnection:
    def __init__(self, data, chunk=7):
        self.data = memoryview(data)
        self.offset = 0
        self.chunk = chunk

    def recv_into(self, destination):
        count = min(len(destination), self.chunk, len(self.data) - self.offset)
        if count:
            destination[:count] = self.data[self.offset : self.offset + count]
            self.offset += count
        return count


class VideoViewerTests(unittest.TestCase):
    def test_frame_survives_partial_tcp_reads(self):
        pixels = bytes(range(18))
        message = struct.pack("!4sHH4sII", b"SBV1", 3, 2, b"BGR3", 99, 18) + pixels
        width, height, sequence, decoded = VIEWER.read_frame(
            ChunkedConnection(message)
        )
        self.assertEqual((width, height, sequence), (3, 2, 99))
        self.assertEqual(decoded, pixels)

    def test_rejects_bad_length(self):
        message = struct.pack("!4sHH4sII", b"SBV1", 3, 2, b"BGR3", 0, 17)
        with self.assertRaisesRegex(ValueError, "length"):
            VIEWER.read_frame(ChunkedConnection(message))

    def test_rejects_short_frame(self):
        message = struct.pack("!4sHH4sII", b"SBV1", 3, 2, b"BGR3", 0, 18)
        with self.assertRaises(EOFError):
            VIEWER.read_frame(ChunkedConnection(message))


if __name__ == "__main__":
    unittest.main()
