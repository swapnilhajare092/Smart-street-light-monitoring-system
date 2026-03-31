
# Smart Street Light Monitoring System

Smart Street Light Monitoring System built using C and Linux system programming. Implements multi-process architecture with IPC (pipe, FIFO, message queue, shared memory) and pthreads. Simulates sensors, controls lights (OFF/DIM/FULL), logs events, and detects faults with alerts, demonstrating real-time embedded system concepts.


## Project overview

A Linux-based Smart Street Light Monitoring System that uses simulated lux and motion inputs to automatically control lights. It employs multiple processes and IPC mechanisms (pipes/shared memory/message queues) to exchange data, demonstrating process management, synchronization, and file handling while optimizing energy usage.
    
The objective is to demonstrate key operating system concepts:  
      
1.Automate street light operation using sensor inputs (lux and motion).

2.Demonstrate Linux process creation and management.

3.Implement inter-process communication (IPC) between processes.

4.Use system calls for file handling and synchronization.

5.Optimize energy consumption by switching lights ON/OFF dynamically.
## scenario desciption

* Street lights are monitored in a simulated environment using Linux processes.
* Lux sensor input determines ambient light conditions (day/night).
* Motion sensor input detects presence of vehicles or pedestrians.
* Controller process evaluates sensor data and decides light status.
* Lights turn ON when it is dark and motion is detected; otherwise remain OFF or dim.
* Processes communicate using IPC mechanisms (pipes/shared memory/message queues).
* System logs sensor data and decisions using file I/O for monitoring and analysis.

## System Architecture

https://github.com/user-attachments/assets/1f463489-1dda-4383-929a-51ffd86e56df" 


---

## Process 1 — `main.c` (Parent Controller)

This is the root process. It runs first and owns the entire system lifetime. It calls `setup_ipc()` to create the pipe, FIFO, message queue, and shared memory before any child exists. Then it installs four signal handlers — `SIGINT` for shutdown, `SIGUSR1` for snapshot, `SIGALRM` for 10-second periodic prints, and `SIGCHLD` to reap zombie children. After that it calls `fork()` four times followed by `execv()` to launch each child binary as a separate process. The supervisor loop at the bottom runs `waitpid(..., WNOHANG)` every 2 seconds to detect any child that crashed and restarts it automatically. On Ctrl+C it sends `SIGTERM` to all children, waits for them, then destroys every IPC resource cleanly.

**Linux calls used:** `pipe`, `mkfifo`, `shmget`, `shmat`, `msgget`, `sem_init`, `fork`, `execv`, `waitpid`, `signal`, `alarm`, `kill`, `shmctl`, `msgctl`, `unlink`

---

## Process 2 — `sensor.c` (Sensor Simulator)

This child simulates the hardware sensors on all 5 lamp nodes of Street 1. It runs two `pthreads` internally. Thread 1 generates random `light_intensity` values (0–100 lux) for each node every second, occasionally injecting edge values like 0 or >80 to trigger different alert types. Thread 2 generates `motion_detected` (0 or 1) every 2 seconds and signals the main thread via `pthread_cond_signal` that data is ready. The main thread waits on `pthread_cond_wait`, then builds a `sensor_data_t` struct for each node and writes it into the anonymous pipe with `write()`. The controller reads from the other end of that pipe.

**Linux calls used:** `shmat`, `pthread_create`, `pthread_mutex_lock/unlock`, `pthread_cond_wait/signal`, `write` (pipe), `close`

---

## Process 3 — `controller.c` (Light Controller)

This child is the brain of the system. It also runs two `pthreads`. Thread 1 sits in a blocking `read()` on the pipe, waiting for sensor data from the sensor process. When data arrives it runs the `decide()` function — if motion is detected or lux is below 30 the light goes FULL, if lux is 30–60 it goes DIM, otherwise OFF — and stores the result in a shared `decisions[]` array, then signals Thread 2 via `pthread_cond_signal`. Thread 2 wakes up, copies the decisions, then does three things for each node: updates the shared memory state under `sem_wait/sem_post`, writes a log line into the FIFO for the logger, and evaluates the four priority alert conditions. If a fault is detected it builds an `alert_msg_t` and calls `msgsnd()` with `mtype` set to the priority level so the alert process can fetch by priority order.

**Linux calls used:** `shmat`, `pthread_create`, `pthread_mutex_lock/unlock`, `pthread_cond_wait/signal`, `read` (pipe), `open` (FIFO), `write` (FIFO), `sem_wait/post`, `msgsnd`

---

## Process 4 — `logger.c` (File Logger)

This child handles all file I/O. Thread 1 opens the FIFO in read mode — this call blocks until the controller opens the write end, which is how FIFO rendezvous works. Once open it reads log lines from the FIFO and writes them into `system.log`. The key technique here is `lseek`: after every new entry it seeks back to byte offset 0 and overwrites the binary `log_header_t` struct in-place so the header always reflects the current entry count and alert counters without growing the file. Thread 2 wakes every 30 seconds, reads a summary from shared memory, and appends a line to `daily_report.txt`.

**Linux calls used:** `shmat`, `pthread_create`, `open` (FIFO read), `read` (FIFO), `open` (log files), `write`, `lseek` (SEEK_SET and SEEK_END), `close`, `sem_wait/post`

---

## Process 5 — `alert.c` (Alert Manager)

This child handles all fault detection and priority dispatch. Thread 1 polls the message queue in strict priority order — it tries `msgrcv(..., PRIORITY_CRITICAL, IPC_NOWAIT)` first, then HIGH, MEDIUM, LOW. Because `IPC_NOWAIT` is used it never blocks on one priority level, so a queue full of LOW messages cannot delay a CRITICAL. Each priority has its own handler function: `handle_critical` prints bold red, writes to both log files, marks the fault in shared memory, and sends `SIGUSR1` to the parent process so the snapshot prints immediately. `handle_high` and `handle_medium` log and mark faults. `handle_low` is informational only — it increments the counter but does not set a fault flag. Thread 2 wakes every 20 seconds and prints a summary of all counters to the console and `alert_report.txt`.

**Linux calls used:** `shmat`, `pthread_create`, `msgrcv`, `open` (log files), `write`, `lseek`, `sem_wait/post`, `kill` (SIGUSR1 to parent)

---

## How the 5 processes connect — data flow in one sentence each

| Connection | Mechanism | Data |
|---|---|---|
| sensor → controller | anonymous `pipe` | `sensor_data_t` (lux, motion, node_id) |
| controller → logger | named `FIFO` | plain text log lines |
| controller → alert | `message queue` | `alert_msg_t` with mtype = priority |
| all ↔ all | `shared memory + semaphore` | lights ON/OFF, brightness, fault flags, alert counts |
| alert → main | `SIGUSR1` signal | triggers snapshot on CRITICAL fault |
| main → children | `SIGTERM` signal | graceful shutdown |
## Process Design

Main Process:
- Creates IPC
- Spawns all child processes
- Monitors and restarts

Child Processes:
 1. Sensor → generates data
                 2. Controller → decision making
                 3. Logger → writes logs
                 4. Alert → handles faults
## Inter Process Communication (IPC)

IPC Mechanisms Used

* Pipes
* Shared Memory
* Semaphores
* Message Queues
* Signals

---

## Purpose of Each IPC

* **Pipes**

  * Used for communication between parent and child processes (sensor → controller)
  * One-way data transfer

* **Shared Memory**

  * Used to store and share sensor data (lux, motion) between processes
  * Fastest IPC method

* **Semaphores**

  * Used for synchronization
  * Prevent race conditions while accessing shared memory

* **Message Queues**

  * Used to send structured messages between processes
  * Useful for control and monitoring communication

* **Signals**

  * Used for event notifications (e.g., motion detected, threshold exceeded)
  * Lightweight interrupt-like communication

---

## Functions Used

* **Pipes**

  * `pipe()`, `fork()`, `read()`, `write()`, `close()`

* **Shared Memory**

  * `shmget()`, `shmat()`, `shmdt()`, `shmctl()`

* **Semaphores**

  * `semget()`, `semop()`, `semctl()`

* **Message Queues**

  * `msgget()`, `msgsnd()`, `msgrcv()`, `msgctl()`

* **Signals**

  * `signal()`, `sigaction()`, `kill()`

## Synchronization

Synchronization → Ensures safe access to shared data and avoids conflicts

functions: semop() (semaphores), mutex APIs (pthread_mutex_lock(), pthread_mutex_unlock())

* condition variables are useful for synchronization between threads
## Synchronization

Synchronization → Ensures safe access to shared data and avoids conflicts

functions: semop() (semaphores), mutex APIs (pthread_mutex_lock(), pthread_mutex_unlock())

* condition variables are useful for synchronization between threads
## Signal Handling

Signal are used for system control
 * SIGINT  → graceful shutdown
 * SIGUSR1 → system snapshot
 * SIGALRM → periodic status
 * SIGCHLD → child termination handling
## File Handling

| File             | Purpose       |
| ---------------- | ------------- |
| system.log       | Event logging |
| daily_report.txt | Summary       |
| alerts.log       | Fault records |
| alert_report.txt | alert Summary |
## Build Steps

* Build

make

* Run

./main

* Sample Outputs
    
    1.Alert log output :
    https://github.com/user-attachments/assets/4b556fa1-c31f-4417-9c71-11d886b71165

    2.system log output :
    https://github.com/user-attachments/assets/f63aca5b-99d3-4769-908e-490e4f4d000b

    3.Daily_report.txt :
    https://github.com/user-attachments/assets/d0ee7665-4144-4de4-816e-af9f7a275edf"