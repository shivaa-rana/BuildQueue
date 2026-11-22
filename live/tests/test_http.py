"""Exercise the CLI and localhost dashboard API with real files."""

import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def request(base, path, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    req = Request(base + path, data=data, headers={"Content-Type": "application/json"})
    with urlopen(req, timeout=2) as response:
        return response.read()


def reject(base, path, payload, status):
    try:
        request(base, path, payload)
    except HTTPError as error:
        assert error.code == status, error
    else:
        raise AssertionError(f"{path} accepted an invalid input")


def main():
    binary = Path(sys.argv[1]).resolve()
    web = Path(sys.argv[2]).resolve()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        file = root / "example file.txt"
        file.write_bytes(b"Hello from BuildQueue!\n")
        (root / "alias").symlink_to(file)
        (root / "another.txt").write_bytes(b"Another file\n")
        expected = subprocess.check_output(["cksum"], input=file.read_bytes()).decode().split()

        command = [str(binary), "--input", directory]
        result = subprocess.run(command + ["--file", file.name], capture_output=True, check=True, timeout=10)
        cli_job = json.loads(result.stdout)["jobs"][0]
        assert cli_job["memory_limit_bytes"] == 64 * 1024 * 1024
        assert isinstance(cli_job["peak_rss_kib"], int) and cli_job["peak_rss_kib"] >= 0
        assert json.loads(result.stdout)["policy"] == "fcfs"
        assert cli_job["state"] == "Completed"
        assert cli_job["checksum"] == int(expected[0])
        large = root / "multichunk.bin"
        large.write_bytes(bytes(range(256)) * 8800)
        expected_large = subprocess.check_output(["cksum", str(large)]).split()
        streamed = subprocess.run(command + ["--file", large.name], capture_output=True,
                                  check=True, timeout=10)
        streamed_job = json.loads(streamed.stdout)["jobs"][0]
        assert streamed_job["checksum"] == int(expected_large[0])
        assert streamed_job["bytes"] == int(expected_large[1])
        rr = subprocess.run(command + ["--policy", "rr", "--file", large.name, "--file", file.name],
                            capture_output=True, check=True, timeout=10)
        rr_jobs = json.loads(rr.stdout)["jobs"]
        assert rr_jobs[0]["checksum"] == int(expected_large[0])
        assert len(rr_jobs[0]["slices"]) == 3
        assert rr_jobs[1]["checksum"] == int(expected[0])
        assert rr_jobs[0]["pid"] is None
        for job in rr_jobs:
            assert abs(job["turnaround_ms"] - job["wait_ms"] - job["run_ms"]) < 0.01
        large.unlink()

        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        base = f"http://127.0.0.1:{port}"
        with subprocess.Popen(command + ["--serve", "--port", str(port), "--web", str(web)]) as server:
            try:
                for attempt in range(80):
                    try:
                        page = request(base, "/")
                        break
                    except URLError:
                        if server.poll() is not None or attempt == 79:
                            raise
                        time.sleep(0.05)
                assert b"File checksum queue" in page
                assert b"Job timeline" in page and b"Deadlock solver" in page
                assert b"/api/jobs" in request(base, "/js/app.js")
                files = json.loads(request(base, "/api/files"))["files"]
                assert [file["name"] for file in files] == ["another.txt", "example file.txt"]

                reject(base, "/api/jobs", {"files": ["../outside"]}, 400)
                reject(base, "/api/jobs", {"files": [file.name], "priority": 2.1}, 400)
                assert json.loads(request(base, "/api/jobs"))["policy"] == "fcfs"
                request(base, "/api/policy", {"policy": "sjf"})
                batch = {"files": [file.name, "another.txt", "missing", "alias"], "priority": 1}
                ids = json.loads(request(base, "/api/jobs", batch))["ids"]
                assert len(ids) == 4

                for _ in range(80):
                    snapshot = json.loads(request(base, "/api/jobs"))
                    jobs = snapshot["jobs"]
                    if all(job["state"] in ("Completed", "Failed") for job in jobs):
                        break
                    time.sleep(0.05)

                assert snapshot["policy"] == "sjf"
                solver = snapshot["deadlocks"]
                assert solver["detected"] == 0 and solver["last_cycle"] == ""
                assert solver["type"] == "Wait-for graph cycle detection"
                assert solver["recovery"]
                assert 1 <= snapshot["peak"] <= snapshot["slots"] == 2
                assert [job["state"] for job in jobs] == ["Completed", "Completed", "Failed", "Failed"]
                assert jobs[0]["checksum"] == int(expected[0])
                assert jobs[0]["bytes"] == int(expected[1])
                assert jobs[0]["estimated_bytes"] == int(expected[1])
                assert jobs[2]["estimated_bytes"] is None
                assert jobs[0]["pid"] > 0 and jobs[0]["exit_code"] == 0
                assert jobs[0]["memory_limit_bytes"] == 64 * 1024 * 1024
                assert isinstance(jobs[0]["peak_rss_kib"], int)
                assert jobs[0]["wait_ms"] >= 0 and jobs[0]["run_ms"] >= 0
                assert jobs[0]["submitted_ms"] == 0
                assert all(job["submitted_ms"] >= 0 for job in jobs)
                reject(base, "/api/cancel", {"id": ids[0]}, 409)
                request(base, "/api/clear", {})
                assert not json.loads(request(base, "/api/jobs"))["jobs"]
            finally:
                server.terminate()
                try:
                    server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait()
    print("PASS: CLI, dashboard, live HTTP jobs, checksum results and errors")


if __name__ == "__main__":
    main()
