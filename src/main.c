// main process

#include "common.h"

int             pipe_fd[2];
int             shmid, msgid;
shared_state_t *shm;
pid_t           pids[4];      /* [0]=sensor [1]=ctrl [2]=logger [3]=alert */
volatile int    running = 1;

/* ── Signal handlers ─────────────────────────────── */

void sigint_handler(int sig) {
    (void)sig;
    printf("\n[MAIN] SIGINT - shutting down all children\n");
    running = 0;
    for (int i = 0; i < 4; i++)
        if (pids[i] > 0) kill(pids[i], SIGTERM);
}

/* Also triggered by alert process when a CRITICAL fires */
void sigusr1_handler(int sig) {
    (void)sig;
    printf("\n[MAIN] === SYSTEM SNAPSHOT (Street %d) ===\n", STREET_ID);
    printf("  Nodes ON    : %d/%d\n", shm->total_on, NUM_NODES);
    printf("  Total faults: %d\n",    shm->total_faults);
    printf("  CRITICAL    : %d\n",    shm->alert_count[3]);
    printf("  HIGH        : %d\n",    shm->alert_count[2]);
    printf("  MEDIUM      : %d\n",    shm->alert_count[1]);
    printf("  LOW         : %d\n",    shm->alert_count[0]);
    for (int i = 0; i < NUM_NODES; i++)
        printf("  Node %d: %s  brightness=%3d  fault=%d\n",
               i + 1,
               shm->light_on[i] ? "ON " : "OFF",
               shm->brightness[i],
               shm->fault[i]);
    printf("  Last critical: %ld\n", (long)shm->last_critical_time);
    printf("==========================================\n");
}

void sigalrm_handler(int sig) {
    (void)sig;
    sigusr1_handler(0);
    alarm(10);
}

void sigchld_handler(int sig) {
    (void)sig;
    int   status;
    pid_t dead;
    while ((dead = waitpid(-1, &status, WNOHANG)) > 0)
        printf("[MAIN] Child pid=%d exited (status=%d)\n",
               dead, WEXITSTATUS(status));
}

/* ── IPC setup ──────────────────────────────────── */
void setup_ipc(void) {
    if (pipe(pipe_fd) == -1) { perror("pipe");   exit(1); }

    unlink(FIFO_PATH);
    if (mkfifo(FIFO_PATH, 0666) == -1) { perror("mkfifo"); exit(1); }

    msgid = msgget(MSG_KEY, IPC_CREAT | 0666);
    if (msgid == -1) { perror("msgget"); exit(1); }

    shmid = shmget(SHM_KEY, sizeof(shared_state_t), IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); exit(1); }

    shm = (shared_state_t *)shmat(shmid, NULL, 0);
    if (shm == (shared_state_t *)-1) { perror("shmat"); exit(1); }

    memset(shm, 0, sizeof(shared_state_t));
    sem_init(&shm->sem, 1, 1);
}

/* ── fork + exec one child binary ─────────────── */
pid_t spawn_child(const char *exe, char *const args[]) {
    pid_t pid = fork();
    if (pid < 0)  { perror("fork"); exit(1); }
    if (pid == 0) {
        execv(exe, args);
        perror("execv");
        exit(1);
    }
    return pid;
}

/* ── main ──────────────────────────────────────── */
int main(void) {
    setup_ipc();

    signal(SIGINT,  sigint_handler);
    signal(SIGUSR1, sigusr1_handler);
    signal(SIGALRM, sigalrm_handler);
    signal(SIGCHLD, sigchld_handler);
    alarm(10);

    char pw[16], pr[16], sh[16], mq[16], ppid_s[16];
    snprintf(pw,     sizeof(pw),     "%d", pipe_fd[1]);
    snprintf(pr,     sizeof(pr),     "%d", pipe_fd[0]);
    snprintf(sh,     sizeof(sh),     "%d", shmid);
    snprintf(mq,     sizeof(mq),     "%d", msgid);
    snprintf(ppid_s, sizeof(ppid_s), "%d", (int)getpid());

    char *sensor_args[] = {"./sensor",     pw, sh,         NULL};
    char *ctrl_args[]   = {"./controller", pr, sh, mq,     NULL};
    char *logger_args[] = {"./logger",     sh,             NULL};
    char *alert_args[]  = {"./alert",      mq, sh, ppid_s, NULL};

    pids[0] = spawn_child("./sensor",     sensor_args);
    pids[1] = spawn_child("./controller", ctrl_args);
    pids[2] = spawn_child("./logger",     logger_args);
    pids[3] = spawn_child("./alert",      alert_args);

    printf("[MAIN] Smart Street Light System started\n");
    printf("[MAIN] Monitoring: Street %d with %d lamp nodes\n",
           STREET_ID, NUM_NODES);
    printf("[MAIN] PIDs: sensor=%d ctrl=%d logger=%d alert=%d\n",
           pids[0], pids[1], pids[2], pids[3]);

    const char  *exes[]     = {"./sensor","./controller","./logger","./alert"};
    char *const *arglists[] = {
        (char *const *)sensor_args,
        (char *const *)ctrl_args,
        (char *const *)logger_args,
        (char *const *)alert_args
    };

    /* Supervisor loop: restart any child that exits unexpectedly */
    while (running) {
        sleep(2);
        for (int i = 0; i < 4; i++) {
            int   status;
            pid_t ret = waitpid(pids[i], &status, WNOHANG);
            if (ret == pids[i]) {
                printf("[MAIN] %s (pid=%d) died - restarting\n",
                       exes[i], pids[i]);
                pids[i] = spawn_child(exes[i], arglists[i]);
            }
        }
    }

    for (int i = 0; i < 4; i++)
        waitpid(pids[i], NULL, 0);

    sem_destroy(&shm->sem);
    shmdt(shm);
    shmctl(shmid, IPC_RMID, NULL);
    msgctl(msgid, IPC_RMID, NULL);
    unlink(FIFO_PATH);
    printf("[MAIN] Cleanup complete. Goodbye.\n");
    return 0;
}
