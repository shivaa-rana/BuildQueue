Deadlock demonstration

Open BuildQueue and select Deadlock. Click Upload deadlock file.
Select any one of the ten .txt files directly inside demo-files/deadlock.
Each file declares two jobs with opposite lock orders. Both jobs process
the selected file. Expected result: one detected cycle and two completed jobs.

Timeline demonstration

Click Add files and select several files. Open Timeline to see the
first dispatch order, queued intervals and processing intervals.
For Round Robin, choose the policy before adding files. Use files
larger than 1 MiB to get more than one processing turn.
