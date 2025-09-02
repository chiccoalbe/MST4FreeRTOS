# MST4FreeRTOS
### v 0.1
---
> ⚠️ **Warning:** THIS IS AN EARLY ALPHA RELEASE. ALL FUNCTIONALITY IS IMPLEMENTED BUT NOT THOROUGHLY TESTED. USE IN PRODUCTION IS NOT RECOMMENDED.

**MST4FreeRTOS** is a FreeRTOS extension that implements the **Standard Model (MST)** for real-time task scheduling.  
It adds support for **periodic tasks**, **sporadic tasks**, and optionally a **sporadic server**, using either **Rate Monotonic Scheduling (RMS)** or **Earliest Deadline First (EDF)**.

The library provides a higher-level API to declare task timing parameters such as **period**, **deadline**, **phase**, and **WCET**, while still leveraging the FreeRTOS kernel underneath.

---

## Features
- **Periodic tasks** with explicit period, deadline, phase, and WCET.
- **Sporadic tasks** with minimum interarrival time, deadline, and WCET.
- **Sporadic server** support (when enabled) to schedule sporadic tasks under RMS.
- Configurable scheduling policies:
  - **RMS** (Rate Monotonic Scheduling)
  - **EDF** (Earliest Deadline First)

---

## Configuration
Edit [`MSTScheduler.h`](MSTLibrary/MSTScheduler.h) to configure:
- **Scheduling policy**:
  ```c
  #define mst_schedSCHEDULING_POLICY mst_schedSCHEDULING_RMS   
  ```
- **Periodic task release mechanism**:
  ```c
  #define mst_test_PERIODIC_METHOD 2   // 1 = vTaskDelayUntil, 2 = FreeRTOS timers
  ```
- **Sporadic server usage**:
  ```c
  #define mst_USE_SPORADIC_SERVER 1   // 1 = enabled, 0 = disabled
  ```

---

## 🚀 Usage

### 1. Include the header
```c
#include "MSTScheduler.h"
```

### 2. Create periodic tasks
```c
TaskHandle_t Task1Handle;

void MyPeriodicTask(void *pvParameters) {
    // user code here
}

vMSTPeriodicTaskCreate(
    MyPeriodicTask,            // function to execute
    "Task1",                   // task name
    configMINIMAL_STACK_SIZE,  // stack size
    NULL,                      // parameters
    2,                         // priority (used only if DEFAULT scheduling)
    &Task1Handle,              // task handle
    1000,                      // period (ms)
    300,                       // deadline (ms)
    0,                         // phase (ms)
    100                        // WCET (ms)
);
```

### 3. Create sporadic tasks
```c
TaskHandle_t Task2Handle;

void MySporadicTask(void *pvParameters) {
    // user code here
}

vMSTSporadicTaskCreate(
    MySporadicTask,            // function to execute
    "Task2",                   // task name
    configMINIMAL_STACK_SIZE,  // stack size
    NULL,                      // parameters
    1,                         // priority (used only if DEFAULT scheduling)
    &Task2Handle,              // task handle
    400,                       // minimum interarrival time (ms)
    150,                       // deadline (ms)
    100                        // WCET (ms)
);
```

To run a sporadic job (after respecting interarrival time):
```c
vMSTSporadicTaskRun(&Task2Handle);
```

### 3. - Configure runtime stats
For accurate and non-preemption task timing detection and WCET tracking, ensure `configGENERATE_RUN_TIME_STATS` is properly configured in FreeRTOS.  
You can find an example for STM32 in the project
repository

### 4. Start the scheduler
> ⚠️  Make sure to call `vMSTSchedulerStart()` instead of `vTaskStartScheduler()`.
```c
vMSTSchedulerStart();   // Use this instead of vTaskStartScheduler()
```

---

## ⚠️ Known Issues

- **Sporadic server budget watchdog**: The performed sporadic job acceptance test should assure that a new job will not go over-budget. If it happens the library integrates a watchdog timer, which will suspend and later resume the job. This watchdog has a known limitation: If FreeRTOS is running with preemption enabled, a sporadic task might get preempted after being activated by the server, the watchdog timer will consider the preemption time as time used by the job, reducing the actual budget.
If a task runs over budget the watchdog that checks **AS SUCH THE SPORADIC SERVER WATCHDOG IS PRESENT BUT INHIBITED (COMMENTED OUT) IN V.0.1**

- **Overhead too high for EDF acceptance**


---

## 📝 Notes
- Deadlines and WCET are used for **admission control** and deadline-miss detection.
- When using **RMS + sporadic server**, sporadic jobs are admitted according to slack and budget rules.
- When using **EDF**, the scheduler dynamically recomputes priorities.
- Requires `configNUM_THREAD_LOCAL_STORAGE_POINTERS >= 1` in FreeRTOS.

---

## 📝 Future work
- Use microseconds as timing standard

---

# Technical notes

## EDF: the approach
2 ways have been explored to implement EDF:
1. Block all tasks and use xTaskNotifyGive() at job release. Our scheduler decides which job releases, not freeRTOS
2. Map deadlines of FreeRTOS dinamically, on each job release, compute the relative priority based on the job’s deadline. The pro is that multiple jobs can be active at once, hence, preemption is possible. We decided to go for this option.

## Sporadic server
The sporadic server serves one sporadic task at a time. It has one known limitation: 
If FreeRTOS is running with preemption enabled, a sporadic task might get preempted after being activated by the server.
If a task runs over budget the watchdog that checks 

## Calculation of Sporadic Server’s Period and WCET

When using **RMS with Sporadic Server** (`mst_USE_SPORADIC_SERVER == 1`), the scheduler computes the **sporadic server period (Tss)** and **worst-case execution time (Css)** based on utilization bounds.

From Liu & Layland (1973), the utilization bound for `m` tasks is:

$$
U = m \cdot \left( 2^{\tfrac{1}{m}} - 1 \right)
$$

where `m = n + 1` (number of periodic tasks `n` plus the sporadic server).

The system utilization is:

$$
U = \sum_{i=1}^{n} \frac{C_i}{T_i} + \frac{C_{ss}}{T_{ss}}
$$

Thus the sporadic server utilization is:

$$
U_{ss} = U - \sum_{i=1}^{n} \frac{C_i}{T_i}
$$

Given a candidate `C_ss` (WCET), the period is chosen as:

$$
T_{ss} = \frac{C_{ss}}{U_{ss}}
$$

To optimally choose the values we put the following constraints in the choice:
- `C_ss` must be between `max(C_i)` and `Σ C_i` of all periodic tasks.  
- `T_ss` must be **≤ minimum interarrival time** of all sporadic tasks.  

This is implementing by iterating from highest to lowest possible period.

---
## Acceptance Test for Sporadic Jobs under EDF

**Theorem (EDF Schedulability for Sporadic Jobs):**  
A system of independent, preemptable **sporadic jobs** is schedulable under the EDF algorithm if the **total density** of all active jobs in the system never exceeds 1:

$$
\sum \text{density of active jobs} \leq 1
$$

This condition is **sufficient** (but not necessary). It applies to both **periodic** and **sporadic** jobs.

---

### Admission Control
When a new sporadic job $S(t, d, e)$ arrives, with:
- $t$: current time  
- $e$: execution time (WCET)  
- $d$: relative deadline  

the EDF scheduler partitions the timeline $[t, \infty)$ into **discrete intervals** based on the deadlines of active jobs:

$$
I_1, I_2, \dots, I_{n+1}
$$

- $I_1$ starts at $t$ and ends at the earliest deadline.  
- $I_{k+1}$ starts when $I_k$ ends and finishes at the next deadline in the list.  
- Each interval $I_k$ has a **total sporadic density** $\Delta_{s,k}$.  

---

### Acceptance Rule
Let $I_l$ be the interval containing the new job’s deadline.  
The job is **accepted** if for every $k = 1, 2, \dots, l$:

$$
\frac{e}{d - t} + \Delta_{s,k} \leq 1 - \Delta
$$

Where:
- $\Delta_{s,k}$ = density of active sporadic jobs overlapping interval $I_k$  
- $\Delta$ = density of periodic tasks (computed at kernel start):

$$
\Delta = \sum_{i=1}^{n} \frac{C_i}{T_i}
$$

---

- If the inequality holds, the new job can be scheduled without exceeding utilization $1$ in any relevant interval.  
- If the inequality fails, admitting the job would overload the processor, the job is then **rejected**.  

---

## Acceptance Test for Sporadic Jobs under RMS with Sporadic Server Active

When using RMS + Sporadic Server, acceptance is evaluated via **slack**:

$$
\sigma(t) = \left\lfloor \frac{d_{s1} - t}{p_s} \right\rfloor e_s - e_{s1}
$$

Where:
- $ d_{s1} $ : absolute deadline of the sporadic job  
- $ p_s $ : sporadic server period  
- $ e_s $ : sporadic server execution budget (WCET)  
- $ e_{s1} $ : requested execution time of sporadic job $ S_1 $

Condition:  
$$

\sigma(t) \geq 0

$$

If slack is non-negative, the sporadic job is **accepted**; otherwise, it is **rejected**.