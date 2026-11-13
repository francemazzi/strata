"""Export and play a synthetic MP4 using the Python runtime in the macOS app."""

import platform
import sys
from pathlib import Path

from qgis.core import Qgis, QgsVideoExporter
from qgis.PyQt.QtCore import QEventLoop, QSize, QTimer, QUrl
from qgis.PyQt.QtGui import QColor, QImage
from qgis.PyQt.QtMultimedia import (
    QMediaFormat,
    QMediaPlayer,
    QMediaRecorder,
    QVideoSink,
)
from qgis.PyQt.QtWidgets import QApplication


def wait_for_result(connect, start):
    loop = QEventLoop()
    timer = QTimer()
    timer.setSingleShot(True)
    timer.timeout.connect(loop.quit)
    completed = False

    def finish(*args):
        nonlocal completed
        completed = True
        loop.quit()

    connect(finish)
    timer.start(15_000)
    start()
    if not completed:
        loop.exec()
    timer.stop()
    return completed


def main():
    expected_arch, source_sha, directory = sys.argv[1:]
    arch = platform.machine()
    if arch != expected_arch:
        raise RuntimeError(f"Expected {expected_arch}, running as {arch}")
    build_sha = Qgis.devVersion()
    if len(build_sha) < 7 or not source_sha.startswith(build_sha):
        raise RuntimeError(
            f"Expected source {source_sha}, packaged PyQGIS is {build_sha}"
        )
    app = QApplication([])
    size = QSize(320, 240)
    paths = []
    for index, color in enumerate(("red", "green", "blue")):
        image = QImage(size, QImage.Format.Format_RGB32)
        image.fill(QColor(color))
        path = str(Path(directory) / f"{index}.png")
        if not image.save(path):
            raise RuntimeError(f"Could not write frame {path}")
        paths.append(path)

    output = Path(directory) / "export.mp4"
    exporter = QgsVideoExporter(str(output), size, 2)
    exporter.setInputFiles(paths)
    exporter.setFileFormat(QMediaFormat.FileFormat.MPEG4)
    exporter.setVideoCodec(QMediaFormat.VideoCodec.H264)
    if not wait_for_result(exporter.finished.connect, exporter.writeVideo):
        raise RuntimeError(f"Video export timed out: {exporter.errorString()}")
    if exporter.error() != QMediaRecorder.Error.NoError or exporter.errorString():
        raise RuntimeError(f"Video export failed: {exporter.errorString()}")
    if not output.is_file() or output.stat().st_size == 0:
        raise RuntimeError("Video export did not create a nonempty MP4")

    sink = QVideoSink()
    player = QMediaPlayer()
    player.setVideoSink(sink)
    frames = []

    def record_frame(frame):
        if frame.isValid():
            image = frame.toImage()
            color = image.pixelColor(image.width() // 2, image.height() // 2)
            channels = (color.red(), color.green(), color.blue())
            frames.append((image.size(), max(range(3), key=channels.__getitem__)))

    def connect_playback(finish):
        def finish_at_end(status):
            if status == QMediaPlayer.MediaStatus.EndOfMedia:
                finish()

        player.mediaStatusChanged.connect(finish_at_end)
        player.errorOccurred.connect(finish)

    def play():
        player.setSource(QUrl.fromLocalFile(str(output)))
        player.play()

    sink.videoFrameChanged.connect(record_frame)
    if not wait_for_result(connect_playback, play):
        raise RuntimeError(f"Video playback timed out: {player.errorString()}")
    if player.error() != QMediaPlayer.Error.NoError or (
        player.mediaStatus() != QMediaPlayer.MediaStatus.EndOfMedia
    ):
        raise RuntimeError(f"Video playback failed: {player.errorString()}")
    if frames != [(size, 0), (size, 1), (size, 2)]:
        raise RuntimeError(f"Unexpected decoded frames: {frames}")
    print(f"{arch} ({build_sha}): exported and decoded {len(frames)} MP4/H.264 frames")
    app.quit()


if __name__ == "__main__":
    main()
