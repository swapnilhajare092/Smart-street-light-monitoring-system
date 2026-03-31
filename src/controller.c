// controller process

#include "common.h"

int             pipe_r_fd;
int             shmid;
int             msgid;
shared_state_t *shm;
volatile int    running = 1;

/* Decision result shared between the two controller threads */
typedef struct {
    int street_id;
    int node_id;    /* lamp post 1-NUM_NODES */
    int decision;   /* LIGHT_OFF / LIGHT_DIM / LIGHT_FULL */
    int intensity;
    int motion;
} decision_t;

decision_t      decisions[NUM_NODES];
pthread_mutex_t dec_lock      = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  dec_ready     = PTHREAD_COND_INITIALIZER;
int             dec_available = 0;

void sig_handler(int sig) { (void)sig; running = 0; }

/* Choose light mode from sensor reading */
int decide(int intensity, int motion) {
    if (motion == 1)    return LIGHT_FULL;
    if (intensity < 30) return LIGHT_FULL;
    if (intensity < 60) return LIGHT_DIM;
    return LIGHT_OFF;
}

/* Thread 1: read pipe, compute decision for each node */
void *decision_thread(void *arg) {
    (void)arg;
    sensor_data_t sd;
    while (running) {
        ssize_t n = read(pipe_r_fd, &sd, sizeof(sd));
        if (n <= 0) break;

        int idx = sd.node_id - 1;          /* node 1 -> index 0 */
        if (idx < 0 || idx >= NUM_NODES) continue;

        pthread_mutex_lock(&dec_lock);
        decisions[idx].street_id = sd.street_id;
        decisions[idx].node_id   = sd.node_id;
        decisions[idx].decision  = decide(sd.light_intensity,
                                          sd.motion_detected);
        decisions[idx].intensity = sd.light_intensity;
        decisions[idx].motion    = sd.motion_detected;
        dec_available = 1;
        pthread_cond_signal(&dec_ready);
        pthread_mutex_unlock(&dec_lock);
    }
    return NULL;
}

/* Thread 2: apply decisions, write FIFO log, send priority alerts */
void *broadcast_thread(void *arg) {
    (void)arg;
    int fifo_fd = open(FIFO_PATH, O_WRONLY);
    if (fifo_fd == -1) { perror("controller: open fifo"); return NULL; }

    while (running) {
        pthread_mutex_lock(&dec_lock);
        while (!dec_available && running)
            pthread_cond_wait(&dec_ready, &dec_lock);
        if (!running) { pthread_mutex_unlock(&dec_lock); break; }

        decision_t local[NUM_NODES];
        memcpy(local, decisions, sizeof(decisions));
        dec_available = 0;
        pthread_mutex_unlock(&dec_lock);

        for (int i = 0; i < NUM_NODES; i++) {
            if (local[i].node_id == 0) continue;
            int idx = local[i].node_id - 1;

            /* ── 1. Update shared memory ─────────────── */
            sem_wait(&shm->sem);
            shm->light_on[idx]   = (local[i].decision != LIGHT_OFF);
            shm->brightness[idx] = (local[i].decision == LIGHT_FULL) ? 100
                                 : (local[i].decision == LIGHT_DIM)  ?  50 : 0;
            shm->total_on = 0;
            for (int j = 0; j < NUM_NODES; j++)
                shm->total_on += shm->light_on[j];
            sem_post(&shm->sem);

            /* ── 2. Write log line to FIFO for logger ── */
            char logbuf[200];
            int  lblen = snprintf(logbuf, sizeof(logbuf),
                "[%ld] Street %d Node %d: lux=%3d motion=%d -> %s\n",
                (long)time(NULL),
                local[i].street_id, local[i].node_id,
                local[i].intensity, local[i].motion,
                local[i].decision == LIGHT_FULL ? "FULL"
              : local[i].decision == LIGHT_DIM  ? "DIM" : "OFF");
            if (write(fifo_fd, logbuf, (size_t)lblen) == -1)
                perror("controller: write fifo");

            /* ── 3. Priority-based Alert Logic ──────────
               One alert per node per cycle, highest severity wins. */
            alert_msg_t amsg;
            memset(&amsg, 0, sizeof(amsg));
            int fault_detected = 0;

            /* CRITICAL (4): sensor completely dead */
            if (local[i].intensity == 0 && local[i].motion == 0) {
                amsg.fault_code = FAULT_SENSOR;
                amsg.priority   = PRIORITY_CRITICAL;
                snprintf(amsg.description, sizeof(amsg.description),
                    "Street %d Node %d: Sensor failure (lux=0, motion=0)",
                    local[i].street_id, local[i].node_id);
                fault_detected = 1;
            }
            /* HIGH (3): light OFF but it is very dark */
            else if (local[i].intensity < 10 &&
                     local[i].decision == LIGHT_OFF) {
                amsg.fault_code = FAULT_LIGHT;
                amsg.priority   = PRIORITY_HIGH;
                snprintf(amsg.description, sizeof(amsg.description),
                    "Street %d Node %d: Light OFF in dark (lux=%d)",
                    local[i].street_id, local[i].node_id,
                    local[i].intensity);
                fault_detected = 1;
            }
            /* MEDIUM (2): light ON in bright daylight */
            else if (local[i].intensity > 80 &&
                     local[i].decision != LIGHT_OFF) {
                amsg.fault_code = FAULT_STUCK_ON;
                amsg.priority   = PRIORITY_MEDIUM;
                snprintf(amsg.description, sizeof(amsg.description),
                    "Street %d Node %d: Light ON in daylight (lux=%d)",
                    local[i].street_id, local[i].node_id,
                    local[i].intensity);
                fault_detected = 1;
            }
            /* LOW (1): no motion in a dark area */
            else if (local[i].motion == 0 && local[i].intensity < 20) {
                amsg.fault_code = FAULT_NO_MOTION;
                amsg.priority   = PRIORITY_LOW;
                snprintf(amsg.description, sizeof(amsg.description),
                    "Street %d Node %d: No motion in dark (lux=%d)",
                    local[i].street_id, local[i].node_id,
                    local[i].intensity);
                fault_detected = 1;
            }

            if (fault_detected) {
                amsg.street_id = local[i].street_id;
                amsg.node_id   = local[i].node_id;
                amsg.mtype     = amsg.priority;
                if (msgsnd(msgid, &amsg,
                           sizeof(amsg) - sizeof(long),
                           IPC_NOWAIT) == -1) {
                    if (errno != EAGAIN)
                        perror("controller: msgsnd");
                }
            }
        }
    }

    close(fifo_fd);
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr,
            "Usage: controller <pipe_read_fd> <shmid> <msgid>\n");
        exit(1);
    }
    pipe_r_fd = atoi(argv[1]);
    shmid     = atoi(argv[2]);
    msgid     = atoi(argv[3]);
    shm = (shared_state_t *)shmat(shmid, NULL, 0);
    if (shm == (shared_state_t *)-1) {
        perror("controller: shmat"); exit(1);
    }
    signal(SIGTERM, sig_handler);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, decision_thread,  NULL);
    pthread_create(&t2, NULL, broadcast_thread, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    close(pipe_r_fd);
    shmdt(shm);
    return 0;
}
