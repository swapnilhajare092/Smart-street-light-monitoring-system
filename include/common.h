#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <time.h>
#include <errno.h>

/* ── IPC Keys & Paths ─────────────────────────────── */
#define SHM_KEY      0x1234
#define MSG_KEY      0x5678
#define FIFO_PATH    "/tmp/log_fifo"
#define LOG_FILE     "system.log"
#define REPORT_FILE  "daily_report.txt"
#define ALERT_LOG    "alerts.log"
#define ALERT_REPORT "alert_report.txt"

/* ── System Layout ────────────────────────────────── */
#define NUM_NODES    5          /* lamp posts on the street   */
#define STREET_ID    1          /* this system monitors Street 1 */

/* ── Fault Codes ──────────────────────────────────── */
#define FAULT_NONE        0     /* no fault                          */
#define FAULT_SENSOR      1     /* lux=0 AND motion=0 -> sensor dead */
#define FAULT_LIGHT       2     /* light OFF when very dark           */
#define FAULT_STUCK_ON    3     /* light ON in bright daylight        */
#define FAULT_NO_MOTION   4     /* no motion in dark  (info only)     */

/* ── Priority Levels (= mtype in message queue) ───── */
#define PRIORITY_LOW       1
#define PRIORITY_MEDIUM    2
#define PRIORITY_HIGH      3
#define PRIORITY_CRITICAL  4

/* ── Sensor Data (pipe: sensor -> controller) ──────── */
typedef struct {
    int    light_intensity;   /* 0-100 lux                    */
    int    motion_detected;   /* 0 or 1                       */
    int    street_id;         /* always STREET_ID (1)         */
    int    node_id;           /* lamp post number 1-NUM_NODES */
    time_t timestamp;
} sensor_data_t;

/* ── Alert Message (msg queue: controller -> alert) ── */
typedef struct {
    long mtype;               /* set = priority (1-4)         */
    int  street_id;           /* always STREET_ID (1)         */
    int  node_id;             /* which lamp post              */
    int  fault_code;          /* FAULT_* constant             */
    int  priority;            /* same as mtype, for display   */
    char description[80];     /* human-readable message       */
} alert_msg_t;

/* ── Shared Memory (global state, all processes) ─────  */
typedef struct {
    int    light_on[NUM_NODES];    /* 1=ON  0=OFF per node    */
    int    brightness[NUM_NODES];  /* 0 / 50 / 100            */
    int    fault[NUM_NODES];       /* fault flag per node     */
    int    fault_code[NUM_NODES];  /* last fault code         */
    int    total_on;               /* how many lights are ON  */
    int    total_faults;           /* cumulative fault count  */
    int    alert_count[4];         /* [0]=LOW [1]=MED [2]=HIGH [3]=CRIT */
    time_t last_critical_time;     /* timestamp of last P4    */
    sem_t  sem;                    /* protects this struct    */
} shared_state_t;

/* ── Light Decision Codes ─────────────────────────── */
#define LIGHT_OFF   0
#define LIGHT_DIM   1
#define LIGHT_FULL  2

#endif /* COMMON_H */
