# BuildQueue

BuildQueue is a C++17 Linux application for calculating checksums of batches of local files. Select files in the Qt desktop app or browser dashboard, or submit them through the command line. A persistent thread pool schedules each file, and a counting semaphore limits how many checksum processes can run at once.

## Browser dashboard

From the `live/` directory on Linux or Ubuntu under WSL2, build and start the server:

```sh
make -j2
mkdir -p input
printf 'Hello from BuildQueue\n' > input/example.txt
./buildqueue --input input --serve --workers 4 --slots 2
```

Open [http://127.0.0.1:8081](http://127.0.0.1:8081), select files and choose **Queue files**. Copy additional files into the input folder and use **Refresh** to list them. The dashboard updates job states twice a second and shows the checksum, byte count, elapsed wait and run times, process ID and exit code.

## Qt desktop app

On Ubuntu (including WSL2 with WSLg), install Qt 6 Widgets and CMake, then build from the project root:

```sh
sudo apt install build-essential cmake qt6-base-dev coreutils util-linux
sh run.sh
```

Run `sh run.sh` from the project folder each time you want to open the app. The script builds any changed files and launches the Qt window. An executable Git checkout also supports `./run.sh`.

Click **Add files** and choose several files, or drag local files into the window. The desktop app queues the selected paths directly and reads each file when its worker starts. The file picker sidebar includes mounted Windows drives under `/mnt` and available shortcuts for your Windows profile. You can switch between **FCFS**, **SJF**, and **Priority** while the picker is open. In Priority mode, use the batch priority control to set a number from 0 (highest) to 9 before opening the selected files. The table shows size estimates, priorities, job states, checksums, process IDs, and elapsed times. The timeline uses amber for waiting and teal for execution; overlapping teal bars show concurrent jobs. The deadlock panel reports wait-for graph detections.

The left navigation groups the desktop app into three workspaces:

- **Jobs** contains scheduling controls, queue actions, live metrics, and the timeline. Use **Show technical columns** for file size, priority, PID, and peak RSS.
- **Compare policies** runs all three schedulers and keeps the results in the main window.
- **Memory** contains the allocation policy, process memory limit, buffer map, block table, and deadlock status.

The desktop app shares the C++ scheduler, semaphore, process launcher, and deadlock detector with the CLI and browser dashboard. It opens its own queue and does not require an HTTP server. The browser dashboard still uses its configured input folder.

To compare algorithms, add files of varied sizes, let the batch finish, choose a different scheduling policy, and click **Rerun batch**. BuildQueue runs the same files again with their original priorities. Use **Show previous batch** to compare the timelines and average wait times. The **Dispatch** column shows the actual start order and policy used for each job. With two execution slots, add at least three files so some jobs have to wait. For Priority comparisons, submit groups with different batch priorities before rerunning them.

**Compare all policies** runs the current batch under FCFS, SJF, Priority, and Round Robin in three separate queues and shows measured average wait, total elapsed time, completed count, and dispatch order side by side. Choose one comparison slot to see a sequential dispatch order, or two to observe concurrent scheduling. Each run uses the same paths, priorities, slot count, memory limit, and buffer allocation policy. Timings can vary with filesystem caching and system load.

## Command line

From the `live/` directory, run a batch using the binary built with `make`:

```sh
./buildqueue --input input --file example.txt
cksum input/example.txt
```

Add multiple `--file` arguments to submit a batch. A batch exits with code 1 if any job fails. A custom dashboard port can be set with `--port`.

## Process memory management

Linux enforces a **64 MiB virtual address-space limit per checksum child** by default. Set **Process memory limit** in Qt to change the limit for jobs when their workers start. In the CLI or browser server, pass `--memory-mib 32` at startup. The accepted range is 8 to 4096 MiB.

BuildQueue sets both the soft and hard limits with `prlimit --as` before executing `cksum`. On exit, Linux reports the child's **peak resident set size (RSS)** in KiB. The Qt and browser tables show that reading and the configured limit in a tooltip. The CLI and API report `peak_rss_kib` and `memory_limit_bytes`. The address-space limit controls virtual memory; peak RSS measures resident memory. These figures cover each checksum child.

The checksum workers also stream the actual input bytes through a shared **512 KiB buffer pool** before sending them to the child process. Each job uses a contiguous buffer sized from 4 to 256 KiB and reuses it for successive reads. A job's timeout includes time spent waiting for a buffer. In Qt, choose **First Fit**, **Best Fit**, or **Worst Fit**. The main queue shows free and used block ranges, largest free block, peak usage, external fragmentation and coalescing count. Released buffers merge with adjacent free blocks. Peak fragmentation and the coalescing count remain visible after fast jobs finish. Browser and CLI use First Fit.

## How it works

- **Job submission** creates one job per selected file. A mutex guards submission, state transitions and snapshots.
- **Job scheduling** defaults to First Come, First Served (FCFS). Non-preemptive Priority chooses the lowest priority number first. Non-preemptive Shortest Job First (SJF) chooses the smallest file by its byte size at submission, since hashing reads the entire file. Equal sizes and equal priorities keep submission order, and SJF places files with unknown sizes after known sizes. Policy changes affect queued jobs. Select SJF in either interface or run the CLI with `--policy sjf`.
- **Synchronization** uses a condition variable to wake workers and a counting semaphore to enforce the active-job limit. A freed slot lets a queued file start immediately.
- **Deadlock detection** tracks which threads own or are waiting for the queue and semaphore mutexes. A blocking lock request that would close a wait-for cycle is rejected with a diagnostic. The dashboard and CLI report the number of detected cycles and the most recent cycle. Normal semaphore contention is expected and leaves the counter unchanged.
- **Process execution** opens a regular file in the configured directory and feeds it to `/usr/bin/cksum` through standard input. Each process returns a CRC checksum and byte count. The worker records the real PID and exit status, and reaps the child. A job fails if its process exceeds two minutes.
- **Monitoring** reports Queued, Running, Completed, Failed and Cancelled. The live timeline shows each job's waiting and processing intervals on a shared elapsed-time axis measured from the first job shown. Overlapping processing bars indicate concurrent jobs. The browser refreshes every 500 ms.
- **Queue management** allows cancelling queued jobs and clearing terminal jobs. Up to 100 files can be submitted per batch and up to 500 job records are kept in memory. Job history starts fresh when the program restarts.

The dashboard binds to `127.0.0.1`. Files must be regular entries directly inside the input directory. File names with spaces work, and relative paths that leave the directory and symbolic links are rejected. Files are read when their jobs run. A checksum can be compared with a known value to check file integrity.

## Lab demonstration

1. Put five local files in the input folder and start BuildQueue with four workers and two slots.
2. Queue the five files together and show the peak active count of two. Use the job timeline to point out overlapping processing intervals and the time each file spent waiting for a slot.
3. Switch to Priority and submit lower- and higher-priority batches during a running batch to show how the queue selects the next job when a slot opens.
4. Compare a completed checksum and byte count with the result of `cksum input/example.txt`.

## Tests

```sh
make test
```

The C++ tests cover real checksum subprocesses, path validation, deterministic scheduling order, slot limits, buffer placement, fragmentation, coalescing and multi-chunk streaming. The HTTP test checks the CLI, served dashboard, job submission, live results and failure reporting. CMake and CTest are also supported on Linux.

Run `make test-deadlock` to demonstrate detection with two real threads and two tracked mutexes. Both threads acquire their first lock, then request the other thread's lock. The detector rejects the request that closes the cycle. Stack unwinding releases the rejected thread's first lock, letting the other thread finish. Ordinary checksum jobs acquire the queue and semaphore locks in a consistent order, so the dashboard should normally show zero cycles. The detector covers instrumented mutexes inside BuildQueue. The checksum process timeout separately handles stalled child processes.

## Team

- Arihant Jain (992401030014)
- Yuvraj Rana (992401030021)
- Siddharth Singh (992401030026)

Operating Systems Lab, Batch F1, Jaypee Institute of Information Technology.

### Round Robin and the desktop dashboard

Round Robin reads up to 1 MiB per turn and returns unfinished checksum jobs to the ready queue. Its work runs inside the application with a bounded read buffer. The process memory limit and child PID/RSS apply to the policies that spawn `cksum`.

The Jobs table shows arrival, burst, completion, turnaround, and waiting times. Finished jobs are listed by completion time. Open Analysis to compare all four policies and inspect the checksum buffer pool.

### Deadlock recovery demonstration

In the Qt desktop app, open **Deadlock** and click **Upload lock jobs**. Select both files from one numbered folder and click **Open**. Each file declares its two locks in opposite order. The app checksums the full contents of both submitted files under the locks and shows their CRCs and byte counts after retry. **Run bundled pair** is a shortcut for the same worker path. There are five pairs (ten files) in `demo-files/deadlock`. The graph keeps the detected cycle visible after recovery. Each arrow points from a waiting worker to the resource owner. A dashed red arrow marks the rejected request. The expected result is one detected cycle and two completed workers after retry. The **Timeline** tab shows queued and running intervals on a shared elapsed-time axis.

On Windows, extract the repository ZIP and double-click **Start BuildQueue.cmd**. It uses Ubuntu under WSL 2 with WSLg and the usual Qt build dependencies.
