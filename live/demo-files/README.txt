Deadlock cases

The Deadlock workspace opens one case file at a time. Cases 01–07 run two jobs. Cases 08, 09 and 10 run 3, 4 and 8 jobs respectively. Each job requests the first lock from its case entry and then the requested second lock. With **Wait for timeout (10 s)** enabled, the detected cycle stays active until the lock deadline. Each waiting job then shows Timed out, and the graph stays available. Leave the option unchecked to reject and retry the job that closes the cycle.

**Stop all** holds queued jobs, ends active checksum children and releases waiting demo locks. An in-process Round Robin turn reaches its next turn boundary before stopping. **Resume** resumes dispatch, restarts interrupted child jobs from the beginning, and replays any interrupted deadlock case or policy comparison.

Timeline demonstration

Open Timeline to inspect first-dispatch order and measured job intervals.
