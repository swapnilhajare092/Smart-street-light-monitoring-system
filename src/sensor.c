//sensor process

#include "common.h"

int             pipe_w_fd;
int             shmid;
shared_state_t *shm;
volatile int    running = 1;

/* Shared buffer between the two sensor threads */
typedef struct {
    int intensity[NUM_NODES];
    int motion[NUM_NODES];
} sensor_buf_t;

sensor_buf_t    sbuf;
pthread_mutex_t buf_lock   = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  data_ready = PTHREAD_COND_INITIALIZER;
int             data_available = 0;

void sig_handler(int sig) { (void)sig; running = 0; }

/* Thread 1: generate light intensity for each lamp post.
   Injects edge values occasionally to trigger all 4 alert types. */
void *intensity_thread(void *arg) {
    (void)arg;
    srand((unsigned)time(NULL) ^ ((unsigned)getpid() << 1));
    while (running) {
        pthread_mutex_lock(&buf_lock);
        for (int i = 0; i < NUM_NODES; i++) {
            int r = rand() % 100;
            if (r < 5)
                sbuf.intensity[i] = 0;           /* -> FAULT_SENSOR or FAULT_LIGHT */
            else if (r < 10)
                sbuf.intensity[i] = 85 + rand() % 16; /* -> FAULT_STUCK_ON (>80)  */
            else
                sbuf.intensity[i] = rand() % 101;     /* normal 0-100 lux         */
        }
        pthread_mutex_unlock(&buf_lock);
        sleep(1);
    }
    return NULL;
}

/* Thread 2: generate motion detection for each lamp post.
   30% chance of motion. Signals main thread when data is ready. */
void *motion_thread(void *arg) {
    (void)arg;
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    while (running) {
        pthread_mutex_lock(&buf_lock);
        for (int i = 0; i < NUM_NODES; i++)
            sbuf.motion[i] = (rand() % 10 < 3) ? 1 : 0;
        data_available = 1;
        pthread_cond_signal(&data_ready);
        pthread_mutex_unlock(&buf_lock);
        sleep(2);
    }
    return NULL;
}

/* Main: waits for a combined reading, then sends one
   sensor_data_t per lamp post through the pipe.        */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: sensor <pipe_write_fd> <shmid>\n");
        exit(1);
    }
    pipe_w_fd = atoi(argv[1]);
    shmid     = atoi(argv[2]);
    shm       = (shared_state_t *)shmat(shmid, NULL, 0);
    if (shm == (shared_state_t *)-1) { perror("sensor: shmat"); exit(1); }
    signal(SIGTERM, sig_handler);

    pthread_t t1, t2;
    pthread_create(&t1, NULL, intensity_thread, NULL);
    pthread_create(&t2, NULL, motion_thread,    NULL);

    while (running) {
        pthread_mutex_lock(&buf_lock);
        while (!data_available && running)
            pthread_cond_wait(&data_ready, &buf_lock);
        if (!running) { pthread_mutex_unlock(&buf_lock); break; }

        for (int i = 0; i < NUM_NODES; i++) {
            sensor_data_t sd;
            sd.street_id       = STREET_ID;    /* always Street 1          */
            sd.node_id         = i + 1;        /* lamp post 1 through 5    */
            sd.light_intensity = sbuf.intensity[i];
            sd.motion_detected = sbuf.motion[i];
            sd.timestamp       = time(NULL);

            if (write(pipe_w_fd, &sd, sizeof(sd)) == -1) {
                if (errno == EPIPE) { running = 0; break; }
                perror("sensor: write pipe");
            }
            printf("[SENSOR] Street %d Node %d: lux=%3d  motion=%d\n",
                   sd.street_id, sd.node_id,
                   sd.light_intensity, sd.motion_detected);
        }
        data_available = 0;
        pthread_mutex_unlock(&buf_lock);
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    close(pipe_w_fd);
    shmdt(shm);
    printf("[SENSOR] Exiting.\n");
    return 0;
}
