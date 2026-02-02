/*
 * monitor.c - CPU Usage Monitor
 *
 * Monitors CPU usage of all processes, aggregates per user,
 * and produces a ranked list by total CPU consumption.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <time.h>
#include <ctype.h>

/* Maximum number of users to track */
#define MAX_USERS 1024

/* Maximum number of processes to track */
#define MAX_PROCS 65536

/* Structure to store per-user CPU time */
typedef struct {
    uid_t uid;
    char username[64];
    unsigned long long cpu_time;  /* in clock ticks */
} user_cpu_t;

/* Structure to store CPU time tracking for each process */
typedef struct {
    pid_t pid;
    uid_t uid;
    unsigned long long last_utime;
    unsigned long long last_stime;
    int valid;
} proc_info_t;

/* Global arrays */
static user_cpu_t users[MAX_USERS];
static int num_users = 0;

static proc_info_t proc_info[MAX_PROCS];
static int num_procs = 0;

/* Clock ticks per second */
static long clk_tck;

/*
 * Check if a string is a valid PID (all digits)
 */
static int is_pid_dir(const char *name)
{
    while (*name) {
        if (!isdigit(*name))
            return 0;
        name++;
    }
    return 1;
}

/*
 * Get the UID of a process by reading /proc/<pid>/status
 */
static uid_t get_process_uid(pid_t pid)
{
    char path[256];
    FILE *fp;
    char line[256];
    uid_t uid = (uid_t)-1;

    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    fp = fopen(path, "r");
    if (!fp)
        return (uid_t)-1;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Uid:", 4) == 0) {
            /* Format: Uid: real effective saved filesystem */
            sscanf(line + 4, "%u", &uid);
            break;
        }
    }

    fclose(fp);
    return uid;
}

/*
 * Read CPU time (utime + stime) from /proc/<pid>/stat
 * Returns 0 on success, -1 on failure
 */
static int read_proc_cpu_time(pid_t pid, unsigned long long *utime, unsigned long long *stime)
{
    char path[256];
    FILE *fp;
    char comm[256];
    char state;
    int ppid, pgrp, session, tty_nr, tpgid;
    unsigned int flags;
    unsigned long minflt, cminflt, majflt, cmajflt;
    unsigned long utime_val, stime_val;

    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    fp = fopen(path, "r");
    if (!fp)
        return -1;

    /*
     * /proc/<pid>/stat format:
     * pid (comm) state ppid pgrp session tty_nr tpgid flags
     * minflt cminflt majflt cmajflt utime stime ...
     *
     * Fields 14 and 15 are utime and stime (in clock ticks)
     */

    /* Read pid */
    int pid_read;
    if (fscanf(fp, "%d ", &pid_read) != 1) {
        fclose(fp);
        return -1;
    }

    /* Read comm - handle parentheses and possible spaces in name */
    int c;
    int paren_depth = 0;
    int comm_idx = 0;

    while ((c = fgetc(fp)) != EOF) {
        if (c == '(') {
            paren_depth++;
            if (paren_depth == 1)
                continue;  /* Skip first '(' */
        }
        if (c == ')') {
            paren_depth--;
            if (paren_depth == 0)
                break;  /* End of comm */
        }
        if (paren_depth > 0 && comm_idx < (int)sizeof(comm) - 1) {
            comm[comm_idx++] = c;
        }
    }
    comm[comm_idx] = '\0';

    /* Read remaining fields */
    if (fscanf(fp, " %c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu",
               &state, &ppid, &pgrp, &session, &tty_nr, &tpgid, &flags,
               &minflt, &cminflt, &majflt, &cmajflt,
               &utime_val, &stime_val) != 13) {
        fclose(fp);
        return -1;
    }

    fclose(fp);

    *utime = utime_val;
    *stime = stime_val;
    return 0;
}

/*
 * Get username from UID
 */
static const char *get_username(uid_t uid)
{
    struct passwd *pw = getpwuid(uid);
    if (pw)
        return pw->pw_name;
    return NULL;
}

/*
 * Find or create user entry
 */
static user_cpu_t *find_or_create_user(uid_t uid)
{
    int i;
    const char *name;

    /* Search existing users */
    for (i = 0; i < num_users; i++) {
        if (users[i].uid == uid)
            return &users[i];
    }

    /* Create new user entry */
    if (num_users >= MAX_USERS)
        return NULL;

    users[num_users].uid = uid;
    users[num_users].cpu_time = 0;

    name = get_username(uid);
    if (name) {
        strncpy(users[num_users].username, name, sizeof(users[num_users].username) - 1);
        users[num_users].username[sizeof(users[num_users].username) - 1] = '\0';
    } else {
        snprintf(users[num_users].username, sizeof(users[num_users].username), "%u", uid);
    }

    return &users[num_users++];
}

/*
 * Find process entry by PID
 */
static proc_info_t *find_proc_info(pid_t pid)
{
    int i;
    for (i = 0; i < num_procs; i++) {
        if (proc_info[i].pid == pid && proc_info[i].valid)
            return &proc_info[i];
    }
    return NULL;
}

/*
 * Add new process entry
 */
static proc_info_t *add_proc_info(pid_t pid, uid_t uid,
                                   unsigned long long utime,
                                   unsigned long long stime)
{
    if (num_procs >= MAX_PROCS)
        return NULL;

    proc_info[num_procs].pid = pid;
    proc_info[num_procs].uid = uid;
    proc_info[num_procs].last_utime = utime;
    proc_info[num_procs].last_stime = stime;
    proc_info[num_procs].valid = 1;

    return &proc_info[num_procs++];
}

/*
 * Scan all processes and accumulate CPU time
 */
static void scan_processes(void)
{
    DIR *proc_dir;
    struct dirent *entry;
    pid_t pid;
    uid_t uid;
    unsigned long long utime, stime;
    proc_info_t *pi;
    user_cpu_t *user;

    proc_dir = opendir("/proc");
    if (!proc_dir) {
        perror("opendir /proc");
        return;
    }

    while ((entry = readdir(proc_dir)) != NULL) {
        if (!is_pid_dir(entry->d_name))
            continue;

        pid = atoi(entry->d_name);

        uid = get_process_uid(pid);
        if (uid == (uid_t)-1)
            continue;

        if (read_proc_cpu_time(pid, &utime, &stime) < 0)
            continue;

        pi = find_proc_info(pid);
        if (!pi) {
            /* New process - record initial values */
            add_proc_info(pid, uid, utime, stime);
            find_or_create_user(uid);
        } else {
            /* Existing process - accumulate delta */
            unsigned long long delta_utime = 0;
            unsigned long long delta_stime = 0;

            if (utime >= pi->last_utime)
                delta_utime = utime - pi->last_utime;
            if (stime >= pi->last_stime)
                delta_stime = stime - pi->last_stime;

            user = find_or_create_user(uid);
            if (user)
                user->cpu_time += delta_utime + delta_stime;

            /* Update last seen values */
            pi->last_utime = utime;
            pi->last_stime = stime;
        }
    }

    closedir(proc_dir);
}

/*
 * Comparison function for sorting users by CPU time (descending)
 */
static int compare_users(const void *a, const void *b)
{
    const user_cpu_t *ua = (const user_cpu_t *)a;
    const user_cpu_t *ub = (const user_cpu_t *)b;

    if (ub->cpu_time > ua->cpu_time)
        return 1;
    if (ub->cpu_time < ua->cpu_time)
        return -1;
    return 0;
}

/*
 * Convert clock ticks to milliseconds
 */
static unsigned long long ticks_to_ms(unsigned long long ticks)
{
    return (ticks * 1000) / clk_tck;
}

/*
 * Print the final summary
 */
static void print_summary(void)
{
    int i;
    int rank = 0;

    /* Sort users by CPU time (descending) */
    qsort(users, num_users, sizeof(user_cpu_t), compare_users);

    /* Print header */
    printf("Rank User           CPU Time (milliseconds)\n");
    printf("----------------------------------------\n");

    /* Print each user with non-zero CPU time */
    for (i = 0; i < num_users; i++) {
        if (users[i].cpu_time > 0) {
            rank++;
            printf("%-4d %-14s %llu\n",
                   rank,
                   users[i].username,
                   ticks_to_ms(users[i].cpu_time));
        }
    }

    /* If no users had CPU time, print a message */
    if (rank == 0) {
        printf("(No CPU usage recorded)\n");
    }
}

/*
 * Initialize - record initial CPU times for all existing processes
 */
static void initialize(void)
{
    DIR *proc_dir;
    struct dirent *entry;
    pid_t pid;
    uid_t uid;
    unsigned long long utime, stime;

    proc_dir = opendir("/proc");
    if (!proc_dir) {
        perror("opendir /proc");
        return;
    }

    while ((entry = readdir(proc_dir)) != NULL) {
        if (!is_pid_dir(entry->d_name))
            continue;

        pid = atoi(entry->d_name);

        uid = get_process_uid(pid);
        if (uid == (uid_t)-1)
            continue;

        if (read_proc_cpu_time(pid, &utime, &stime) < 0)
            continue;

        add_proc_info(pid, uid, utime, stime);
        find_or_create_user(uid);
    }

    closedir(proc_dir);
}

int main(int argc, char *argv[])
{
    int duration;
    int elapsed;

    /* Check command line arguments */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <duration_seconds>\n", argv[0]);
        return 1;
    }

    duration = atoi(argv[1]);
    if (duration <= 0) {
        fprintf(stderr, "Error: duration must be a positive integer\n");
        return 1;
    }

    /* Get clock ticks per second */
    clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) {
        clk_tck = 100;  /* Default fallback */
    }

    /* Initialize - record initial CPU times */
    initialize();

    /* Monitor loop - scan every second */
    for (elapsed = 0; elapsed < duration; elapsed++) {
        sleep(1);
        scan_processes();
    }

    /* Print summary */
    print_summary();

    return 0;
}
