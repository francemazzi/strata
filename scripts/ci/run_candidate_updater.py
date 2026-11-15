"""Drive the packaged updater with signed draft assets on loopback."""

import base64
import hashlib
import http.server
import json
import os
import pathlib
import threading
import time
import traceback
from datetime import datetime, timezone

from qgis.core import QgsNetworkAccessManager, QgsProject, QgsSettings
from qgis.PyQt.QtCore import QTimer, QUrl
from qgis.PyQt.QtGui import QAction
from qgis.PyQt.QtNetwork import QNetworkRequest
from qgis.PyQt.QtWidgets import QLabel, QMessageBox, QPushButton
from qgis.utils import iface

root = pathlib.Path(os.environ["STRATA_QA_ROOT"])
package = root / "candidate" / os.environ["STRATA_QA_ASSET"]
metadata = root / "candidate"
manifest_url_base = "https://github.com/francemazzi/strata/releases/download/strata-v1.9.1/"
feed_url = "https://api.github.com/repos/francemazzi/strata/releases/latest"
endpoint_names = ("update-manifest.json", "update-manifest.sig", package.name)
state = {"scenario": "invalid-signature", "phase": "waiting", "started": time.monotonic()}
log = root / os.environ.get("STRATA_QA_LOG", "evidence.jsonl")


def record(event, **data):
    with log.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps({"event": event, "at": datetime.now(timezone.utc).isoformat(), **data}) + "\n")


class Feed(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send_content(self, data):
        self.send_response(200)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        name = self.path.lstrip("/")
        scenario = state["scenario"]
        record("http", name=name, scenario=scenario)
        if name == "latest":
            self.send_content(json.dumps({
                "draft": False, "prerelease": False, "tag_name": "strata-v1.9.1",
                "assets": [{"name": name, "browser_download_url": manifest_url_base + name}
                           for name in endpoint_names[:2]]
            }).encode())
        elif name == "update-manifest.json":
            self.send_content((metadata / name).read_bytes())
        elif name == "update-manifest.sig":
            data = (metadata / name).read_bytes()
            if scenario == "invalid-signature":
                raw = bytearray(base64.b64decode(data))
                raw[0] ^= 1
                data = base64.b64encode(raw) + b"\n"
            self.send_content(data)
        elif name == package.name:
            with package.open("rb") as stream:
                self.send_response(200)
                self.send_header("Content-Length", str(package.stat().st_size))
                self.end_headers()
                first = True
                try:
                    while chunk := stream.read(1024 * 1024):
                        if first and scenario == "checksum-failure":
                            chunk = bytes([chunk[0] ^ 1]) + chunk[1:]
                        self.wfile.write(chunk)
                        first = False
                        if scenario == "cancel":
                            time.sleep(0.15)
                except (BrokenPipeError, ConnectionResetError):
                    if scenario != "cancel":
                        raise
        else:
            self.send_error(404)


def redirect_qa_request(request):
    url = request.url().toString()
    name = "latest" if url == feed_url else urls.get(url)
    if name:
        request.setUrl(QUrl(f"http://127.0.0.1:{server.server_port}/{name}"))
        request.setAttribute(QNetworkRequest.Attribute.CacheLoadControlAttribute,
                             QNetworkRequest.CacheLoadControl.AlwaysNetwork)
    return ""


def button(text):
    return next(control for control in dialog.findChildren(QPushButton) if control.text() == text)


def next_scenario(name):
    state.update(scenario=name, phase="waiting")
    button("Check for updates").click()


def tick():
    try:
        if time.monotonic() - state["started"] > 420:
            raise RuntimeError("Timed out")
        status = " | ".join(label.text() for label in dialog.findChildren(QLabel))
        if status != state.get("status"):
            record("status", text=status, scenario=state["scenario"])
            state["status"] = status
        scenario = state["scenario"]
        if scenario == "invalid-signature" and "signature is invalid" in status:
            record("passed", scenario=scenario)
            next_scenario("cancel")
        elif scenario == "cancel" and "is available" in status and state["phase"] == "waiting":
            state["phase"] = "downloading"
            state["download_at"] = time.monotonic()
            button("Update").click()
        elif scenario == "cancel" and "Downloading Strata" in status and time.monotonic() - state["download_at"] > 0.5:
            button("Cancel download").click()
        elif scenario == "cancel" and "Update cancelled" in status:
            record("passed", scenario=scenario)
            next_scenario("checksum-failure")
        elif scenario in ("checksum-failure", "upgrade") and "is available" in status and state["phase"] == "waiting":
            state["phase"] = "downloading"
            button("Update").click()
        elif scenario == "checksum-failure" and "checksum verification failed" in status:
            record("passed", scenario=scenario)
            next_scenario("upgrade")
        elif scenario == "upgrade" and "Update verified" in status:
            record("passed", scenario="verified-download")
            record("install-requested")
            state["scenario"] = "install-requested"
            button("Install and restart").click()
        elif scenario == "install-requested":
            for message in iface.mainWindow().findChildren(QMessageBox):
                if message.isVisible():
                    raise RuntimeError(message.text())
    except Exception:
        record("failure", details=traceback.format_exc())
        timer.stop()


urls = {manifest_url_base + name: name for name in endpoint_names}
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Feed)
threading.Thread(target=server.serve_forever, daemon=True).start()
QgsNetworkAccessManager.setRequestPreprocessor(redirect_qa_request)
settings = QgsSettings()
settings.setValue("strata/updates/checkAutomatically", False)
settings.setValue("strata/qa-upgrade-marker", "retained")
settings.sync()
project = root / (log.stem + ".qgz")
assert QgsProject.instance().write(str(project))
record("qa-start", package=package.name, config=os.environ["STRATA_CUSTOM_CONFIG_PATH"],
       project_sha256=hashlib.sha256(project.read_bytes()).hexdigest())
window = iface.mainWindow()
action = window.findChild(QAction, "mActionCheckQgisVersion")
assert action
action.trigger()
dialog = next(widget for widget in window.children() if hasattr(widget, "windowTitle") and widget.windowTitle() == "Strata updates")
timer = QTimer(dialog)
timer.setInterval(150)
timer.timeout.connect(tick)
timer.start()
