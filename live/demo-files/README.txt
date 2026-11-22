Deadlock demonstration

Open BuildQueue and select Deadlock. Choose one of the five resource
pairs and click Run recovery demo. Each pair uses two files from its
own folder under demo-files/deadlock. No file copying is necessary.

Two threads take the resource locks in opposite orders. In the graph,
arrows point from the requester to the owner. The dashed red arrow is
the rejected cycle-closing request. The snapshot stays visible after
the rejected worker releases its lock and its work is retried.
Expected result: one detected cycle and two completed workers.

Timeline demonstration

Click Add files and select several files. Open Timeline to see the
first dispatch order, queued intervals and processing intervals.
For Round Robin, choose the policy before adding files. Use files
larger than 1 MiB to get more than one processing turn.
