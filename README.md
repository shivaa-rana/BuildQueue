name: Live checksum tests
on: [push, pull_request]
permissions:
  contents: read
jobs:
  linux:
    runs-on: ubuntu-latest
    defaults:
      run:
        working-directory: live
    steps:
      - uses: actions/checkout@v4
      - name: Build and test
        run: make test
      - name: Check dashboard script
        run: node --check web/js/app.js
      - name: Build Qt desktop app
        run: |
          sudo apt-get update
          sudo apt-get install -y qt6-base-dev
          sh -n run.sh
          ./run.sh --build-only
      - name: Smoke-test Qt window
        run: |
          status=0
          QT_QPA_PLATFORM=offscreen timeout 5s ./build-qt/buildqueue-qt || status=$?
          test "$status" -eq 124

### Deadlock recovery demonstration

Open **Deadlock → Upload deadlock file** and select any one of the ten files under `live/demo-files/deadlock`.

The Deadlock workspace opens one case file at a time. Cases 01–07 run two jobs. Cases 08, 09 and 10 run 3, 4 and 8 jobs respectively. Each job requests the first lock from its case entry and then the requested second lock. With **Wait for timeout (10 s)** enabled, the detected cycle stays active until the lock deadline. Each waiting job then shows Timed out, and the graph stays available. Leave the option unchecked to reject and retry the job that closes the cycle.

**Stop all** holds queued jobs, ends active checksum children and releases waiting demo locks. An in-process Round Robin turn reaches its next turn boundary before stopping. **Resume** resumes dispatch, restarts interrupted child jobs from the beginning, and replays any interrupted deadlock case or policy comparison.


On Windows, extract the repository ZIP and double-click **Start BuildQueue.cmd**. It uses Ubuntu under WSL 2 with WSLg and the usual Qt build dependencies.
