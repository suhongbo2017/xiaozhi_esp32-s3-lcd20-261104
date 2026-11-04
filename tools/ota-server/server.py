#!/usr/bin/env python3
"""XIAOZHI_ESP32-S3_LCD20 self-hosted OTA server (stdlib only, no deps).

Implements the contract reverse-engineered from main/ota.cc of xiaozhi 2.4.2:

  POST/GET  /ota/            version check + optional mqtt/websocket config push
  GET       /fw/<file>       firmware binary download (Content-Length required)

Device behaviour that this server must satisfy
----------------------------------------------
* ota_url comes from settings["wifi"]["ota_url"] (NVS) or CONFIG_OTA_URL.
* Request headers: Device-Id (MAC), Client-Id (board UUID), User-Agent,
  Accept-Language, Activation-Version, Content-Type: application/json.
* Body: board.GetSystemInfoJson() (POST) when system info is available.
* Response MUST be HTTP 200 (other codes are logged as failure).
* Response JSON keys:  firmware {version, url, force?}, mqtt {...}, websocket {...}
* Version compare on device (Ota::ParseVersion + IsNewVersionAvailable):
  split on '.', std::stoi each segment (leading digits only), lexicographic,
  longer wins on tie. So "2.5.0" > "2.4.2" but "2.4.2-lcd20" == "2.4.2".
  => Keep version numbers purely numeric-dotted (2.5.0, 2.5.1 ...).

Run
---
    python tools/ota-server/server.py --port 8080 --root tools/ota-server
    # then on the device:
    #   settings["wifi"]["ota_url"] = "http://<pc-ip>:8080/ota/"
    # or build with CONFIG_OTA_URL="http://<pc-ip>:8080/ota/"
"""

import argparse
import datetime
import json
import os
import re
import shutil
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, unquote

LOCK = threading.Lock()
ROOT = os.path.dirname(os.path.abspath(__file__))


# --------------------------------------------------------------------------- #
# helpers
# --------------------------------------------------------------------------- #
def parse_version(version):
    """Mirror of Ota::ParseVersion: split on '.', keep leading digits per part."""
    out = []
    for seg in str(version).split("."):
        m = re.match(r"\d+", seg.strip())
        if not m:
            break
        out.append(int(m.group(0)))
    return out


def is_newer(current, candidate):
    """Mirror of Ota::IsNewVersionAvailable."""
    cur, new = parse_version(current), parse_version(candidate)
    for a, b in zip(cur, new):
        if b > a:
            return True
        if b < a:
            return False
    return len(new) > len(cur)


def load_json(path, default):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return default


def log_request(record):
    line = json.dumps(record, ensure_ascii=False)
    with LOCK:
        with open(os.path.join(ROOT, "device_requests.jsonl"), "a", encoding="utf-8") as f:
            f.write(line + "\n")
    print("[REQ] " + line, flush=True)


# --------------------------------------------------------------------------- #
# HTTP handler
# --------------------------------------------------------------------------- #
class Handler(BaseHTTPRequestHandler):
    server_version = "xiaozhi-ota/1.0"
    protocol_version = "HTTP/1.1"

    # -- low level -------------------------------------------------------- #
    def _send(self, code, body=b"", ctype="application/json", extra_headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra_headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _read_body(self):
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length else b""

    def log_message(self, fmt, *args):  # keep the console readable
        return

    # -- routing ---------------------------------------------------------- #
    def do_GET(self):
        path = urlparse(self.path).path
        if path.rstrip("/") in ("/ota", ""):
            return self.handle_ota(b"")
        if path.startswith("/fw/"):
            return self.handle_firmware(unquote(path[len("/fw/"):]))
        if path == "/healthz":
            return self._send(200, '{"ok":true}')
        return self._send(404, '{"error":"not found"}')

    def do_POST(self):
        path = urlparse(self.path).path
        body = self._read_body()
        if path.rstrip("/") in ("/ota", ""):
            return self.handle_ota(body)
        return self._send(404, '{"error":"not found"}')

    # -- /ota/ ------------------------------------------------------------ #
    def handle_ota(self, raw_body):
        try:
            info = json.loads(raw_body) if raw_body else {}
        except Exception:
            info = {}

        app = info.get("application") or {}
        device = {
            "time": datetime.datetime.now().isoformat(timespec="seconds"),
            "client_id": self.headers.get("Client-Id", ""),
            "device_id": self.headers.get("Device-Id", ""),
            "user_agent": self.headers.get("User-Agent", ""),
            "accept_language": self.headers.get("Accept-Language", ""),
            "activation_version": self.headers.get("Activation-Version", ""),
            "app_name": app.get("name", ""),
            "app_version": app.get("version", ""),
            "idf_version": app.get("idf_version", ""),
            "chip": info.get("chip_model_name", ""),
            "flash_size": info.get("flash_size"),
            "psram_size": info.get("psram_size"),
            "mac": info.get("mac_address", ""),
            "uuid": info.get("uuid", ""),
        }

        releases = load_json(os.path.join(ROOT, "releases.json"), {})
        devices = load_json(os.path.join(ROOT, "devices.json"), {})

        latest = releases.get("latest") or {}
        target_version = latest.get("version", "")
        target_file = latest.get("file", "")
        force = int(latest.get("force", 0) or 0)

        # per-device override: devices.json {"<client_id>": {"version":..., "file":..., "force":1}}
        ov = devices.get(device["client_id"]) or devices.get(device["device_id"]) or {}
        if ov:
            target_version = ov.get("version", target_version)
            target_file = ov.get("file", target_file)
            force = int(ov.get("force", force) or 0)

        if latest.get("enabled", True) is False:
            target_version = ""

        fw_section = None
        if target_version and target_file:
            current = device["app_version"]
            newer = is_newer(current, target_version) or force
            device["current"] = current
            device["target"] = target_version
            device["update"] = bool(newer)
            if newer:
                host = self.headers.get("Host") or f"127.0.0.1:{self.server.server_port}"
                scheme = "https" if latest.get("https") else "http"
                base = latest.get("base_url") or f"{scheme}://{host}"
                fw_section = {
                    "version": target_version,
                    "url": f"{base}/fw/{target_file}",
                    "force": force,
                }
            else:
                device["note"] = "already latest"

        payload = {}
        if fw_section:
            payload["firmware"] = fw_section
        # optional endpoint config push (writes into device NVS settings)
        for key in ("mqtt", "websocket"):
            section = releases.get(key)
            if isinstance(section, dict) and section:
                payload[key] = section

        log_request({**device, "response_keys": list(payload.keys())})
        name = device["app_name"] or "device"
        ver = device["app_version"] or "?"
        if fw_section:
            print(f"[OTA ] {name} v{ver} -> {target_version} ({target_file})", flush=True)
        else:
            print(f"[OTA ] {name} v{ver} : no update", flush=True)
        return self._send(200, json.dumps(payload, ensure_ascii=False))

    # -- /fw/<file> ------------------------------------------------------- #
    def handle_firmware(self, name):
        if "/" in name or "\\" in name or name.startswith("."):
            return self._send(400, '{"error":"bad name"}')
        path = os.path.join(ROOT, "firmware", name)
        if not os.path.isfile(path):
            print(f"[FW  ] MISS {name}", flush=True)
            return self._send(404, '{"error":"no such firmware"}')
        size = os.path.getsize(path)
        print(f"[FW  ] serving {name} ({size} bytes) to {self.headers.get('Device-Id','?')}", flush=True)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(size))
        self.end_headers()
        with open(path, "rb") as f:
            shutil.copyfileobj(f, self.wfile, 64 * 1024)


def main():
    global ROOT
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--root", default=ROOT)
    args = ap.parse_args()
    ROOT = os.path.abspath(args.root)
    os.makedirs(os.path.join(ROOT, "firmware"), exist_ok=True)
    print(f"OTA server on http://{args.host}:{args.port}/ota/  (root={ROOT})", flush=True)
    ThreadingHTTPServer((args.host, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
