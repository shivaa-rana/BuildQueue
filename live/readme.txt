BuildQueue | Live batch file-checksum manager

Start on Windows
- Extract the ZIP to a local folder and open Start BuildQueue.cmd.
- Requires the Ubuntu WSL 2 distribution with Linux graphical app support
  and the build packages below. The first launch builds the Qt application.
- Open Deadlock, click Upload deadlock file and select any one .txt file
  directly inside demo-files/deadlock.
- Open Timeline for the live graph in first-dispatch order.

Requirements
- Linux, or Ubuntu under WSL2 on Windows
- C++17 compiler, Make and GNU coreutils (/usr/bin/cksum)
- util-linux (/usr/bin/prlimit) to set per-process memory limits
- Python 3 for integration tests only

Install on Debian or Ubuntu
  sudo apt-get install build-essential coreutils python3 util-linux
  make -j2
  make test

Demonstrate deadlock detection with two real threads
  make test-deadlock

Build and run the optional Qt desktop app
  sudo apt install cmake qt6-base-dev
  sh run.sh
  Use Add files to queue files directly. On WSL2, browse /mnt/c for Windows files.

Start the local dashboard
  mkdir -p input
  printf 'Hello from BuildQueue\n' > input/example.txt
  ./buildqueue --input input --serve --workers 4 --slots 2
  Open http://127.0.0.1:8081 in a browser.
  Select files from the list and choose Queue files.
  Copy more files into the input directory and use Refresh to list them.

Run from the command line
  ./buildqueue --input input --file example.txt
  cksum input/example.txt

See README.md for scheduling policies and lab demonstration steps.
