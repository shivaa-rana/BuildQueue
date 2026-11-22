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

Open **Deadlock → Upload deadlock file**, select any one of the ten `.txt` files directly in `live/demo-files/deadlock`, then click **Open**. Each file contains two jobs that request the same resources in opposite order. BuildQueue captures the wait-for cycle, retries one job and reports two checksums of the selected file. The dashed red edge remains visible after recovery.

On Windows, extract the repository ZIP and double-click **Start BuildQueue.cmd**. It uses Ubuntu under WSL 2 with WSLg and the usual Qt build dependencies.
