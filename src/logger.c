// logger process

#include "common.h"

volatile int running = 1;
int log_fd, report_fd;
int shmid;
shared_state_t *shm;

// ── Log file header ─────────────────────────────
typedef struct {
    char  magic[8];
    int   total_entries;
    long  last_update;
} log_header_t;

// ── Signal Handler ──────────────────────────────
void sig_handler(int sig) {
    (void)sig;   // suppress unused warning
    running = 0;
}

// ── Thread 1: FIFO → File (line-based) ──────────
void *write_thread(void *arg) {
    (void)arg;   // suppress unused warning

    int fifo_fd = open(FIFO_PATH, O_RDONLY);
    if (fifo_fd == -1) {
        perror("open fifo");
        return NULL;
    }

    log_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    strcpy(hdr.magic, "SSLMS");
    hdr.total_entries = 0;

    // Initialize header
    lseek(log_fd, 0, SEEK_SET);
    write(log_fd, &hdr, sizeof(hdr));

    char buf[256];
    char line[256];
    size_t idx = 0;

    while (running) {

        ssize_t n = read(fifo_fd, buf, sizeof(buf));

        // Writer closed FIFO → reopen
        if (n == 0) {
            close(fifo_fd);
            fifo_fd = open(FIFO_PATH, O_RDONLY);
            continue;
        }

        if (n < 0) {
            perror("read fifo");
            usleep(100000);
            continue;
        }

        for (ssize_t i = 0; i < n; i++) {

            if (buf[i] == '\n') {
                line[idx] = '\0';

                // Write single log entry
                lseek(log_fd, 0, SEEK_END);
                write(log_fd, line, strlen(line));
                write(log_fd, "\n", 1);

                // Update header
                hdr.total_entries++;
                hdr.last_update = time(NULL);

                lseek(log_fd, 0, SEEK_SET);
                write(log_fd, &hdr, sizeof(hdr));

                printf("[LOGGER] Entry #%d written\n", hdr.total_entries);

                idx = 0;  // reset buffer
            }
            else {
                if (idx < sizeof(line) - 1) {
                    line[idx++] = buf[i];
                }
                // else: discard extra characters (overflow protection)
            }
        }
    }

    close(fifo_fd);
    return NULL;
}

// ── Thread 2: Periodic Report ───────────────────
void *report_thread(void *arg) {
    (void)arg;   // suppress unused warning

    while (running) {
        sleep(30);   // change to 5 for testing

        sem_wait(&shm->sem);
        int on = shm->total_on;
        int faults = shm->total_faults;
        sem_post(&shm->sem);

        char rline[128];
        int len = snprintf(rline, sizeof(rline),
            "[%ld] Lights ON: %d/5  Faults: %d\n",
            (long)time(NULL), on, faults);

        lseek(report_fd, 0, SEEK_END);
        write(report_fd, rline, len);

        printf("[LOGGER] Report written\n");
    }

    return NULL;
}

// ── Main Function ───────────────────────────────
int main(int argc, char *argv[]) {

    if (argc < 2) {
        fprintf(stderr, "Usage: logger <shmid>\n");
        exit(1);
    }

    shmid = atoi(argv[1]);
    shm = (shared_state_t *)shmat(shmid, NULL, 0);

    signal(SIGTERM, sig_handler);

    // Open log file
    log_fd = open(LOG_FILE, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (log_fd == -1) {
        perror("open log");
        exit(1);
    }

    // Open report file
    report_fd = open(REPORT_FILE, O_CREAT | O_RDWR | O_APPEND, 0644);
    if (report_fd == -1) {
        perror("open report");
        exit(1);
    }

    pthread_t t1, t2;

    if (pthread_create(&t1, NULL, write_thread, NULL) != 0) {
        perror("pthread_create write_thread");
        exit(1);
    }

    if (pthread_create(&t2, NULL, report_thread, NULL) != 0) {
        perror("pthread_create report_thread");
        exit(1);
    }

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    close(log_fd);
    close(report_fd);
    shmdt(shm);

    return 0;
}
