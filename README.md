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

In the Qt desktop app, open **Deadlock** and click **Upload lock jobs**. Select both files from one numbered folder and click **Open**. Each file declares its two locks in opposite order. The app checksums the full contents of both submitted files under the locks and shows their CRCs and byte counts after retry. **Run bundled pair** is a shortcut for the same worker path. There are five pairs (ten files) in `live/demo-files/deadlock`. The graph keeps the detected cycle visible after recovery. Each arrow points from a waiting worker to the resource owner. A dashed red arrow marks the rejected request. The expected result is one detected cycle and two completed workers after retry. The **Timeline** tab shows queued and running intervals on a shared elapsed-time axis.

On Windows, extract the repository ZIP and double-click **Start BuildQueue.cmd**. It uses Ubuntu under WSL 2 with WSLg and the usual Qt build dependencies.
