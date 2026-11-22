Deadlock demonstration

Open BuildQueue and select Deadlock. Click Upload lock jobs.
Open one of the five numbered folders under demo-files/deadlock,
select both .txt files, then click Open. Run bundled pair is a shortcut
that uses the same files and the same checksum worker path.

Two threads take the resource locks in opposite orders. In the graph,
arrows point from the requester to the owner. The dashed red arrow is
the rejected cycle-closing request. The snapshot stays visible after
the rejected worker releases its lock and its work is retried.
Expected result: one detected cycle, two completed jobs, and a CRC
checksum and byte count for the full contents of each selected file.
Each file declares its lock order in the first three lines.

Timeline demonstration

Click Add files and select several files. Open Timeline to see the
first dispatch order, queued intervals and processing intervals.
For Round Robin, choose the policy before adding files. Use files
larger than 1 MiB to get more than one processing turn.
