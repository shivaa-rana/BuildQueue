# BuildQueue

BuildQueue Live is a C++17 file checksum queue for Linux and Ubuntu under WSL2 with WSLg. The Qt desktop app runs real checksum jobs and shows which jobs ran, when they started, and how long they waited.

## Run the Qt app

Install the dependencies once on Ubuntu:

```sh
sudo apt update
sudo apt install -y build-essential cmake qt6-base-dev coreutils util-linux
```

From the repository root, build and open the app:

```sh
sh run.sh
```

## Compare scheduling algorithms

1. Use **Add files** or drag files into the window. In WSL, Windows files are under `/mnt/c`.
2. Start with **FCFS**, the default. Choose at least three files of different sizes so at least one job waits for a slot.
3. After the batch finishes, choose **SJF** or **Priority** and click **Rerun batch**. For a Priority comparison, first submit groups with different priorities. `0` is highest.
4. Use **Show previous batch** to compare dispatch order, the timeline, and average wait. SJF estimates job length from file size.

Open **Compare policies** to run the same batch under all three schedulers. Use **Memory** to inspect the file-streaming pool, fragmentation, and coalescing.

See the [BuildQueue Live documentation](live/README.md) for the browser and CLI, the deadlock detector, and how to run the tests.

## Original scheduler and simulation

BuildQueue connects CPU scheduling, contiguous memory allocation, semaphore admission, persistent worker threads, POSIX processes, and performance reporting in one local application. Its deterministic coordinator makes scheduling experiments reproducible; the offline Control Center makes each resource decision visible. A second, explicitly live mode executes real worker tasks and a bounded, predefined file checksum process.

## Quick start

On Ubuntu/Debian, install the compiler and optional verification tools once:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake python3
make -j4
make test
./buildqueue --demo fcfs
./buildqueue --serve --port 8081
```

Open **http://127.0.0.1:8081**. No Node.js, npm, CDN, database, or cloud service is needed. Approved headers and MIT license notices are included under `third_party/`. Python is used only by the optional HTTP smoke tests.

On macOS, install Command Line Tools with `xcode-select --install` if needed, then run `make -j4` and `make test` from this directory. CMake is optional; Linux/container verification provides CMake when it is unavailable on the host. Linux was separately verified in Debian ARM64 containers; macOS builds are not presented as Linux verification. Run from the project root so `logs/buildqueue.log` is created there. `--root /absolute/path/to/BuildQueue` overrides the compiled resource path if a binary is moved.

## Features

- FCFS, non-preemptive SJF, non-preemptive Priority, and preemptive simulated Round Robin, with stable ties and future arrivals.
- First Fit, Best Fit, Worst Fit, exact partition splitting, owner checks, coalescing, and measurable external fragmentation.
- Explicit NEW, READY, RUNNING, WAITING, COMPLETED and CANCELLED states. Running jobs cannot be cancelled.
- One to sixteen persistent workers; one to thirty-two semaphore build slots. Memory and slot admission precede every dispatch.
- Integer-tick simulation with pause, resume, step, replay and per-worker Gantt lanes, including idle intervals.
- Controlled live `fork → execve → waitpid` file processing, fixed argv, a three-second timeout, and process exit reporting.
- A shared CLI/API engine, completed-job metrics, JSON Lines execution logs, and isolated four-policy comparison.
- Dark/light dashboard with searchable and sortable jobs, Kanban queues, memory partitions, resource slots, filtered events, explanations and CSV/JSON exports.
- Eleven demo datasets and a self-contained C++ test runner covering the ten prescribed lab tests and additional regressions.

## Architecture and source organization

```text
CLI / REST API / offline dashboard
                 ↓
BuildQueueEngine — transaction lock and logical-tick coordinator
  JobManager → SchedulerEngine → MemoryManager → SyncController
                                      ↓
                         persistent WorkerManager pool
                                      ↓
                        MonitoringEngine + POSIX Logger

include/       public C++17 interfaces and typed model
src/           original implementation, CLI and HTTP adapter
web/           index.html, css/styles.css, js/{app,api,charts,components}.js
tests/         T1–T10 and scheduling/memory/concurrency/API regressions
demos/         eleven CSV workloads and controlled process input
third_party/   cpp-httplib 0.18.3, nlohmann/json 3.11.3, licenses
docs/          architecture, design, testing, API, demo and viva guides
scripts/       demo, sanitizer, Linux and end-to-end verification helpers
logs/          runtime JSON Lines log (ignored by Git)
```

The default experiment has one logical worker, two build slots, 1024 MB of **simulated** memory, FCFS, First Fit, and a two-tick RR quantum. This is an application scheduler, not a replacement for Linux's kernel scheduler. See [DESIGN](docs/DESIGN.md) and [ARCHITECTURE](docs/ARCHITECTURE.md) for concept-to-code mapping and lock ordering.

## CLI

```sh
./buildqueue --help
./buildqueue                        # interactive shell
./buildqueue --demo fcfs
./buildqueue --demo rr
./buildqueue --demo priority
./buildqueue --load demos/fcfs.csv --policy sjf
./buildqueue --demo mixed_workload --json
./buildqueue --demo synchronization --interactive
./buildqueue --serve --port 8081
```

The interactive shell supports:

```text
help
submit "Compile core" --burst 5 --prio 2 --mem 192 --arrival 0
list                         status [id]               cancel id
policy fcfs                  policy sjf                policy priority
policy rr 2                  mem-policy first|best|worst
workers 4                    slots 2                   memmap
run                          pause                     resume
step                         finish                    reset
report                       gantt                     log
load demos/fcfs.csv           demo fragmentation        compare
mode simulation|live         delay 350                 exit
```

`run` and `resume` execute asynchronously; `finish` blocks until all unfinished jobs complete. One-shot `--demo` and `--load` run to completion unless `--interactive` or `--serve` is supplied. CSV imports append atomically after validating every row. Demo loading resets jobs and applies the documented scenario settings. CLI names and paths containing spaces use double quotes.

A controlled process demonstration:

```text
reset
policy fcfs
mode live
workers 2
submit "Checksum fixture" --type process --burst 1 --prio 1 --mem 64
finish
status 1
```

Use the actual returned ID after resets: IDs stay unique for the engine lifetime. Live process jobs execute `/usr/bin/cksum` on `demos/process_input.txt`; arbitrary commands and shell execution are not exposed. Choose a non-preemptive policy before entering live mode.

## Expected FCFS output

The fixture `demos/fcfs.csv` has arrivals 0,1,2 and bursts 4,3,2:

```text
W1: J1 [0–4] J2 [4–7] J3 [7–9]
Ready waiting times: 0, 3, 5
Average ready wait: 8/3 = 2.6666666666666665 ticks
Average turnaround: 17/3 = 5.666666666666667 ticks
CPU utilization: 100%; throughput: 3/9 jobs per tick
```

Tests assert unrounded values. READY waiting and resource-blocked waiting are separate counters. Averages include completed jobs only; unfinished and cancelled jobs have no completion-derived metrics.

## Dashboard and demonstrations

**Load demo** opens the scenario library. **Step** advances exactly one logical tick; **Run/Pause** controls asynchronous playback. CPU Scheduler changes policies and delay; Memory Manager displays real partition offsets; Resource Monitor sets workers, slots and mode. Algorithm Comparison replays the same workload from tick zero with fresh engines and exports CSV or JSON. Event Logs filters and exports the real engine stream. Clearing the log view retains the on-disk audit log.

Keyboard: **Space** toggles playback and **.** single-steps outside inputs/dialogs. The interface polls one coherent C++ snapshot every 500 ms. Browser rendering never schedules jobs.

| Scenario | Observation |
|---|---|
| `fcfs` | Hand-calculated arrival-order trace |
| `sjf` | Shortest eligible original burst; no mid-job interruption |
| `priority` | Lowest numeric priority, then arrival and ID |
| `round_robin` / `rr` | Two-tick slices and arrival-before-requeue ordering |
| `memory_first_fit` | Probe allocated at offset 0 MB |
| `memory_best_fit` | Probe allocated at offset 280 MB |
| `memory_worst_fit` | Probe allocated at offset 724 MB |
| `fragmentation` | At tick 3: 400 MB request WAITING, 580 MB free, largest hole 300 MB |
| `memory_exhaustion` | A 512 MB job waits while a 768 MB job holds memory |
| `synchronization` | Four workers compete for two slots; counter reaches 18 |
| `mixed_workload` | RR, future arrivals, memory pressure and slot contention |

See [DEMO_GUIDE](docs/DEMO_GUIDE.md) for a reproducible 3–5 minute practical evaluation and every dataset's expected outcome.

## Screenshots and visual verification

The baseline dashboard was observed in native Chrome during the independent audit. The refined frontend has API, rendering-logic and export checks; reliable browser input/viewport control was unavailable, so full interaction and five-width visual QA remain open. No fabricated screenshot is included. See [AUDIT_REPORT](docs/AUDIT_REPORT.md) and [TESTING](docs/TESTING.md).

## Build and test

```sh
make -j4
make test
make debug                    # isolated .build-debug/buildqueue executable
make asan                    # AddressSanitizer + UndefinedBehaviorSanitizer
make tsan                    # separate ThreadSanitizer binary
python3 scripts/smoke_test.py # temporary server, real HTTP + CLI + shutdown
python3 scripts/audit_test.py # five demos, exports, concurrent clients and repeated lifecycle checks
node scripts/test_frontend.mjs docs/evidence/comparison.json # optional developer-only JS checks
make clean

cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j4
ctest --test-dir build --output-on-failure

cmake -S . -B build-asan -DBQ_ASAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j4
ctest --test-dir build-asan --output-on-failure
```

Linux verification from macOS can use `docker build -t buildqueue-lab .` and `sh scripts/verify_linux.sh`. Docker is optional; the native application itself needs only a C++17 compiler, Make and pthread/POSIX support. The Docker image primarily provides a Linux lab environment; the default loopback binding is local to that container, so a published container port alone does not expose the dashboard.

Observed test results, sanitizer support and verification limits are recorded in [TESTING](docs/TESTING.md). See [API](docs/API.md), [VIVA_GUIDE](docs/VIVA_GUIDE.md) and [IMPLEMENTATION_STATUS](docs/IMPLEMENTATION_STATUS.md) for the remaining academic deliverables.

## Scope and limitations

- Logical CPU utilization measures simulation lane occupancy, not host CPU load. Live process wall time and exit code are reported separately. Estimated process bursts remain logical accounting units; use burst 1 for the checksum example.
- The coordinator waits for each batch before publishing a tick, including in live mode. Real workers/processes run concurrently within that batch; their wall timing is nondeterministic. A slow process can delay pause/status until its bounded operation returns.
- SJF assumes declared bursts. Priority aging is not implemented; a stream of higher-priority jobs can starve lower-priority work.
- RR preempts simulated assignments; it does not suspend real Linux processes. Live mode deliberately requires non-preemptive scheduling.
- At most 500 jobs per experiment and 500000 ticks. Logs expose the latest 4000 events in memory; persistent logs retain every event. Long Gantt histories are held in memory for this local educational workload.
- State is in memory; the execution log persists, but restarting does not restore a queue. The API is loopback-only and has no arbitrary filesystem imports or command execution.
- Resize workers/slots while paused and with no running lane owners; capacity or execution mode changes require an empty experiment. Reset preserves configuration and globally increasing IDs.
- macOS sanitizer runtime support and restricted-container TSan support may vary. A runtime failure is not counted as a sanitizer pass.

## Final audit and troubleshooting

The independent audit preserved the original 34 tests and all eleven scenarios. It added regressions for tick limits, maximum capacity, malformed identifiers, repeated resets, process timeout/reaping and log-write failures. Read [AUDIT_REPORT](docs/AUDIT_REPORT.md) for changes, evidence and the submission quality gate.

- **Port in use:** stop the previous instance with Ctrl+C or use `./buildqueue --serve --port 8081`, then open `http://127.0.0.1:8081`. The server now rejects a second listener on the same port.
- **Dashboard missing/disconnected:** run from the project root, check `curl http://127.0.0.1:8081/api/health`, and reload. If assets moved, use `--root /absolute/path/to/BuildQueue`.
- **Cannot resize or change mode:** pause and finish/reset active jobs. Mode/capacity changes need an empty experiment; choose FCFS before entering live mode.
- **Arrival is in the past:** dashboard defaults and CLI submissions without `--arrival` use the current tick atomically. Editing arrival supplies an explicit schedule; choose the current or a future tick. API clients can opt into `arrival_now:true`.
- **Persistent log warning:** events remain in memory and resource accounting continues. Restore writable storage and restart to resume on-disk logging; the warning is exposed in `/api/snapshot` and the dashboard.
- **Sanitizer runtime failure on macOS:** use the documented Linux verification helpers rather than treating compilation as a sanitizer pass.

## Original implementation and licenses

BuildQueue is original C++17 code, licensed under MIT. The educational reference, [Operating-System-TimeSharing](https://github.com/KevinFu12/Operating-System-TimeSharing), was absent locally and was neither copied nor modified. [PROJECT_COMPARISON](docs/PROJECT_COMPARISON.md) distinguishes the supplied reference description from BuildQueue's observed implementation. [cpp-httplib](https://github.com/yhirose/cpp-httplib/tree/v0.18.3) and [nlohmann/json](https://github.com/nlohmann/json/tree/v3.11.3) retain their upstream MIT notices in `third_party/LICENSES/`.

### Round Robin and the desktop dashboard

Round Robin reads up to 1 MiB per turn and returns unfinished checksum jobs to the ready queue. Its work runs inside the application with a bounded read buffer. The process memory limit and child PID/RSS apply to the policies that spawn `cksum`.

The Jobs table shows arrival, burst, completion, turnaround, and waiting times. Finished jobs are listed by completion time. Open Analysis to compare all four policies and inspect the checksum buffer pool.
