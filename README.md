# MST4FreeRTOS
## Goals
- [x]  Periodic task create with delay
- [x]  Periodic task create with timers
- [x]  Sporadic task create and fire with timers
  - [ ]  Bug: Now need at least 1ms interarrival, lets remove this
- [x]  Rate monotonic scheduling
- [x]  Earliest deadline first scheduling
- [x]  Fixed priority sporadic server
- [ ]  Sporadic tasks with acceptance in static priority (90% done)
- [ ]  Sporadic tasks with acceptance in dynamic priority 



## EDF: the approach
2 ways have been explored to implement EDF:
1. Block all tasks and use xTaskNotifyGive() at job release. Our scheduler decides which job releases, not freeRTOS
2. Map deadlines of FreeRTOS dinamically, on each job release, compute the relative priority based on the job’s deadline. The pro is that multiple jobs can be active at once, hence, preemption is possible. We decided to go for this option.