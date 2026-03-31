
//alert process

#include "common.h"

volatile int    running    = 1;
int             msgid;
int             shmid;
pid_t           parent_pid;
shared_state_t *shm;
int             alert_fd;
int             report_fd;

pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

void sig_handler(int sig) { (void)sig; running = 0; }

/* Write one timestamped entry to alerts.log (thread-safe) */
void write_log_line(const alert_msg_t *a, const char *label) {
    char line[300];
    int  len = snprintf(line, sizeof(line),
        "[%ld] PRIORITY=%d [%-8s] Street=%d Node=%d code=%d msg=%s\n",
        (long)time(NULL), a->priority, label,
        a->street_id, a->node_id, a->fault_code, a->description);
    pthread_mutex_lock(&log_lock);
    if (lseek(alert_fd, 0, SEEK_END) != -1)
        if (write(alert_fd, line, (size_t)len) == -1)
            perror("alert: write log");
    pthread_mutex_unlock(&log_lock);
}

/* Write one formatted entry to alert_report.txt (thread-safe) */
void write_report_entry(const alert_msg_t *a, const char *label) {
    char line[320];
    int  len = snprintf(line, sizeof(line),
        "[%ld] %-8s | Street=%d Node=%d | FaultCode=%d | Priority=%d | %s\n",
        (long)time(NULL), label,
        a->street_id, a->node_id,
        a->fault_code, a->priority, a->description);
    pthread_mutex_lock(&log_lock);
    if (lseek(report_fd, 0, SEEK_END) != -1)
        if (write(report_fd, line, (size_t)len) == -1)
            perror("alert: write report");
    pthread_mutex_unlock(&log_lock);
}

/* Mark fault in shared memory and bump the priority counter */
void update_shm_fault(const alert_msg_t *a) {
    sem_wait(&shm->sem);
    int idx = a->node_id - 1;
    if (idx >= 0 && idx < NUM_NODES) {
        shm->fault[idx]      = 1;
        shm->fault_code[idx] = a->fault_code;
        shm->total_faults++;
    }
    int pidx = a->priority - 1;   /* priority 1-4 -> index 0-3 */
    if (pidx >= 0 && pidx < 4)
        shm->alert_count[pidx]++;
    sem_post(&shm->sem);
}

/* ── Priority handlers ─────────────────────────────── */

/* CRITICAL (4): sensor dead — bold red, signal parent */
void handle_critical(const alert_msg_t *a) {
    printf("\033[1;31m[CRITICAL] Street %d Node %d: %s\033[0m\n",
           a->street_id, a->node_id, a->description);
    write_log_line(a, "CRITICAL");
    write_report_entry(a, "CRITICAL");
    update_shm_fault(a);
    if (parent_pid > 0 && kill(parent_pid, SIGUSR1) == -1)
        perror("alert: kill SIGUSR1 parent");
    sem_wait(&shm->sem);
    shm->last_critical_time = time(NULL);
    sem_post(&shm->sem);
}

/* HIGH (3): light off in dark — red */
void handle_high(const alert_msg_t *a) {
    printf("\033[0;31m[HIGH]     Street %d Node %d: %s\033[0m\n",
           a->street_id, a->node_id, a->description);
    write_log_line(a, "HIGH");
    write_report_entry(a, "HIGH");
    update_shm_fault(a);
}

/* MEDIUM (2): light on in daylight — yellow */
void handle_medium(const alert_msg_t *a) {
    printf("\033[0;33m[MEDIUM]   Street %d Node %d: %s\033[0m\n",
           a->street_id, a->node_id, a->description);
    write_log_line(a, "MEDIUM");
    write_report_entry(a, "MEDIUM");
    update_shm_fault(a);
}

/* LOW (1): no motion in dark — cyan, informational only */
void handle_low(const alert_msg_t *a) {
    printf("\033[0;36m[LOW]      Street %d Node %d: %s\033[0m\n",
           a->street_id, a->node_id, a->description);
    write_log_line(a, "LOW");
    sem_wait(&shm->sem);
    shm->alert_count[0]++;
    sem_post(&shm->sem);
}

/* ── Thread 1: poll queue in priority order ──────────
   CRITICAL checked first every iteration so it is never
   delayed behind a backlog of LOW messages.            */
void *detect_thread(void *arg) {
    (void)arg;
    alert_msg_t amsg;

    while (running) {
        if (msgrcv(msgid, &amsg, sizeof(amsg) - sizeof(long),
                   PRIORITY_CRITICAL, IPC_NOWAIT) != -1) {
            handle_critical(&amsg); continue;
        }
        if (msgrcv(msgid, &amsg, sizeof(amsg) - sizeof(long),
                   PRIORITY_HIGH, IPC_NOWAIT) != -1) {
            handle_high(&amsg); continue;
        }
        if (msgrcv(msgid, &amsg, sizeof(amsg) - sizeof(long),
                   PRIORITY_MEDIUM, IPC_NOWAIT) != -1) {
            handle_medium(&amsg); continue;
        }
        if (msgrcv(msgid, &amsg, sizeof(amsg) - sizeof(long),
                   PRIORITY_LOW, IPC_NOWAIT) != -1) {
            handle_low(&amsg); continue;
        }
        usleep(200000);   /* queue empty — avoid busy spin */
    }
    return NULL;
}

/* ── Thread 2: periodic summary every 20 s ──────────  */
void *notify_thread(void *arg) {
    (void)arg;
    while (running) {
        sleep(20);

        sem_wait(&shm->sem);
        int on     = shm->total_on;
        int faults = shm->total_faults;
        int ac[4];
        for (int i = 0; i < 4; i++) ac[i] = shm->alert_count[i];
        sem_post(&shm->sem);

        printf("[ALERT SUMMARY] Street %d | Nodes ON: %d/%d  "
               "Faults: %d | CRIT=%d HIGH=%d MED=%d LOW=%d\n",
               STREET_ID, on, NUM_NODES, faults,
               ac[3], ac[2], ac[1], ac[0]);

        char line[250];
        int  len = snprintf(line, sizeof(line),
            "[%ld] SUMMARY | Street=%d Nodes ON=%d/%d Faults=%d "
            "CRIT=%d HIGH=%d MED=%d LOW=%d\n",
            (long)time(NULL), STREET_ID, on, NUM_NODES, faults,
            ac[3], ac[2], ac[1], ac[0]);
        pthread_mutex_lock(&log_lock);
        if (lseek(report_fd, 0, SEEK_END) != -1)
            if (write(report_fd, line, (size_t)len) == -1)
                perror("alert: write summary");
        pthread_mutex_unlock(&log_lock);
    }
    return NULL;
}

/* ── main ─────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: alert <msgid> <shmid> <parent_pid>\n");
        exit(1);
    }
    msgid      = atoi(argv[1]);
    shmid      = atoi(argv[2]);
    parent_pid = (pid_t)atoi(argv[3]);
    shm = (shared_state_t *)shmat(shmid, NULL, 0);
    if (shm == (shared_state_t *)-1) { perror("alert: shmat"); exit(1); }
    signal(SIGTERM, sig_handler);

    alert_fd = open(ALERT_LOG,    O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (alert_fd  == -1) { perror("alert: open alerts.log");       exit(1); }
    report_fd = open(ALERT_REPORT, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (report_fd == -1) { perror("alert: open alert_report.txt"); exit(1); }

    /* Write report header at offset 0 */
    char hdr[256];
    int  hlen = snprintf(hdr, sizeof(hdr),
        "=== Smart Street Light Alert Report ===\n"
        "Street  : %d\n"
        "Nodes   : %d lamp posts\n"
        "Started : %ld\n"
        "========================================\n",
        STREET_ID, NUM_NODES, (long)time(NULL));
    if (lseek(report_fd, 0, SEEK_SET) == -1) perror("alert: lseek hdr");
    if (write(report_fd, hdr, (size_t)hlen) == -1)
        perror("alert: write hdr");
    if (lseek(report_fd, 0, SEEK_END) == -1)
        perror("alert: lseek end");

    pthread_t t1, t2;
    pthread_create(&t1, NULL, detect_thread,  NULL);
    pthread_create(&t2, NULL, notify_thread,  NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    close(alert_fd);
    close(report_fd);
    shmdt(shm);
    printf("[ALERT] Exiting.\n");
    return 0;
}
