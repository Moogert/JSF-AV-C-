"""HTTP smoke tests for the optional local browser interface."""
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

BINARY = Path(sys.argv.pop(1)).resolve()


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="jsfav web test ")
        self.root = Path(self.temp.name)
        self.source = self.root / "simple.cpp"
        self.source.write_text("#define F(x) (x)\nint f(){ return F(1); }\n", encoding="utf-8")
        (self.root / "compile_commands.json").write_text(json.dumps([{
            "directory": str(self.root), "file": str(self.source),
            "arguments": ["clang++", "-std=c++17", "-c", str(self.source)]
        }]), encoding="utf-8")
        env = os.environ.copy()
        env["JSFAV_NO_BROWSER"] = "1"
        self.proc = subprocess.Popen([str(BINARY), "serve"], cwd=self.root, env=env,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, encoding="utf-8")
        line = self.proc.stdout.readline().strip()
        match = re.search(r"(http://127\.0\.0\.1:\d+/\?token=[^\s]+)", line)
        if not match:
            err = self.proc.stderr.read() if self.proc.poll() is not None else ""
            self.fail(f"server did not start: {line} {err}")
        self.url = match.group(1)
        self.base = self.url.split("/?", 1)[0]
        self.token = self.url.split("token=", 1)[1]
        self.wait_http()

    def tearDown(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        self.temp.cleanup()

    def request(self, path, method="GET", body=None, authorized=True):
        headers = {}
        if authorized:
            headers["X-JSFAV-Token"] = self.token
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            headers["Content-Type"] = "application/json"
        req = urllib.request.Request(self.base + path, data=data, headers=headers, method=method)
        try:
            with urllib.request.urlopen(req, timeout=20) as response:
                return response.status, response.headers, response.read()
        except urllib.error.HTTPError as e:
            return e.code, e.headers, e.read()

    def wait_http(self):
        for _ in range(50):
            try:
                code, _, _ = self.request("/api/v1/status", authorized=False)
                if code == 403:
                    return
            except Exception:
                time.sleep(.1)
        self.fail("local server did not become ready")

    def test_protected_ui_and_catalog(self):
        self.assertEqual(self.request("/api/v1/status", authorized=False)[0], 403)
        self.assertEqual(self.request("/")[0], 200)
        status, _, payload = self.request("/api/v1/rules")
        self.assertEqual(status, 200)
        self.assertEqual(len(json.loads(payload)["rules"]), 233)

    def test_scan_progress_source_and_exports(self):
        status, _, payload = self.request("/api/v1/scans", method="POST", body={
            "projectPath": str(self.root), "compdbPath": str(self.root), "profile": "literal"
        })
        self.assertEqual(status, 202)
        scan_id = json.loads(payload)["id"]
        result = None
        for _ in range(200):
            status, _, payload = self.request(f"/api/v1/scans/{scan_id}")
            body = json.loads(payload)
            if body["state"] == "completed":
                result = body["result"]
                break
            if body["state"] == "failed":
                self.fail(body.get("error"))
            time.sleep(.1)
        self.assertIsNotNone(result, "scan did not complete")
        self.assertEqual(result["scanId"], scan_id)
        finding = next(r for r in result["results"] if r["ruleId"] == "29" and r["classification"] == "violation")
        source_path = urllib.parse.quote(finding["location"]["path"])
        status, _, payload = self.request(f"/api/v1/source?path={source_path}&line={finding['location']['line']}")
        self.assertEqual(status, 200, payload)
        self.assertTrue(json.loads(payload)["lines"])
        self.assertEqual(self.request(f"/api/v1/scans/{scan_id}/export?format=json")[0], 200)
        sarif_status, headers, payload = self.request(f"/api/v1/scans/{scan_id}/export?format=sarif")
        self.assertEqual(sarif_status, 200)
        self.assertEqual(json.loads(payload)["version"], "2.1.0")
        self.assertIn("sarif", headers.get("Content-Type", ""))
        escape = urllib.parse.quote(str(self.root.parent / "outside.cpp"))
        self.assertEqual(self.request(f"/api/v1/source?path={escape}&line=1")[0], 404)

    def test_reject_invalid_project(self):
        status, _, payload = self.request("/api/v1/scans", method="POST", body={"projectPath": "missing"})
        self.assertEqual(status, 400)
        self.assertIn("folder", json.loads(payload)["error"])


if __name__ == "__main__":
    unittest.main()
