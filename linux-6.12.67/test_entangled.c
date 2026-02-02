/*
 * Test program for Task 1: Entangled CPU mutual exclusion
 *
 * Compile: gcc -o test_entangled test_entangled.c -lpthread
 * Run as root: ./test_entangled
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pwd.h>
#include <string.h>
#include <time.h>

#define CPU1 1
#define CPU2 3
#define TEST_DURATION 10  /* seconds */

volatile int running = 1;

/* Set CPU affinity for current process */
int set_cpu_affinity(int cpu) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(cpu, &cpuset);
    return sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

/* Get current CPU */
int get_current_cpu() {
    return sched_getcpu();
}

/* CPU-intensive work */
void do_work() {
    volatile long sum = 0;
    for (long i = 0; i < 10000000 && running; i++) {
        sum += i;
    }
}

/* Worker function that runs on a specific CPU */
void* worker(void* arg) {
    int target_cpu = *(int*)arg;
    int actual_cpu;
    int mismatch_count = 0;
    int total_checks = 0;

    set_cpu_affinity(target_cpu);

    printf("[PID %d, UID %d] Starting on CPU %d\n",
           getpid(), getuid(), target_cpu);

    while (running) {
        do_work();
        actual_cpu = get_current_cpu();
        total_checks++;

        if (actual_cpu != target_cpu) {
            mismatch_count++;
        }
    }

    printf("[PID %d, UID %d] Target CPU %d, Mismatch count: %d/%d\n",
           getpid(), getuid(), target_cpu, mismatch_count, total_checks);

    return NULL;
}

/* Set entangled CPUs via procfs */
int set_entangled_cpus(int cpu1, int cpu2) {
    FILE *f1, *f2;

    f1 = fopen("/proc/sys/kernel/entangled_cpus_1", "w");
    if (!f1) {
        perror("Cannot open entangled_cpus_1");
        return -1;
    }
    fprintf(f1, "%d", cpu1);
    fclose(f1);

    f2 = fopen("/proc/sys/kernel/entangled_cpus_2", "w");
    if (!f2) {
        perror("Cannot open entangled_cpus_2");
        return -1;
    }
    fprintf(f2, "%d", cpu2);
    fclose(f2);

    printf("Set entangled CPUs: %d <-> %d\n", cpu1, cpu2);
    return 0;
}

/* Read current entangled CPU settings */
void print_entangled_cpus() {
    FILE *f1, *f2;
    int cpu1 = -1, cpu2 = -1;

    f1 = fopen("/proc/sys/kernel/entangled_cpus_1", "r");
    if (f1) {
        fscanf(f1, "%d", &cpu1);
        fclose(f1);
    }

    f2 = fopen("/proc/sys/kernel/entangled_cpus_2", "r");
    if (f2) {
        fscanf(f2, "%d", &cpu2);
        fclose(f2);
    }

    printf("Current entangled CPUs: %d <-> %d\n", cpu1, cpu2);
}

void test_same_user() {
    printf("\n=== TEST 1: Same user on both entangled CPUs ===\n");
    printf("Expected: Both processes should run normally\n\n");

    pthread_t t1, t2;
    int cpu1 = CPU1, cpu2 = CPU2;

    running = 1;
    pthread_create(&t1, NULL, worker, &cpu1);
    pthread_create(&t2, NULL, worker, &cpu2);

    sleep(TEST_DURATION);
    running = 0;

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    printf("TEST 1 complete.\n");
}

void test_different_users() {
    printf("\n=== TEST 2: Different users on entangled CPUs ===\n");
    printf("Expected: One CPU should go idle when different user tries to run\n");
    printf("(Run this test with two different user accounts)\n\n");

    /* Fork a child that runs on CPU2 */
    pid_t pid = fork();

    if (pid == 0) {
        /* Child process */
        int cpu = CPU2;
        set_cpu_affinity(cpu);

        printf("[Child PID %d, UID %d] Running on CPU %d\n",
               getpid(), getuid(), cpu);

        time_t start = time(NULL);
        int iterations = 0;

        while (time(NULL) - start < TEST_DURATION) {
            do_work();
            iterations++;
        }

        printf("[Child] Completed %d iterations on CPU %d\n",
               iterations, get_current_cpu());
        exit(0);
    } else {
        /* Parent process */
        int cpu = CPU1;
        set_cpu_affinity(cpu);

        printf("[Parent PID %d, UID %d] Running on CPU %d\n",
               getpid(), getuid(), cpu);

        time_t start = time(NULL);
        int iterations = 0;

        while (time(NULL) - start < TEST_DURATION) {
            do_work();
            iterations++;
        }

        printf("[Parent] Completed %d iterations on CPU %d\n",
               iterations, get_current_cpu());

        wait(NULL);
    }

    printf("TEST 2 complete.\n");
}

int main(int argc, char *argv[]) {
    printf("=== Entangled CPU Test Program ===\n");
    printf("Testing CPUs %d and %d\n\n", CPU1, CPU2);

    /* Check if running as root */
    if (geteuid() != 0) {
        printf("Warning: Not running as root. Cannot modify procfs.\n");
        printf("Run with: sudo ./test_entangled\n\n");
    }

    /* Print current settings */
    print_entangled_cpus();

    /* Set entangled CPUs */
    if (set_entangled_cpus(CPU1, CPU2) < 0) {
        printf("Failed to set entangled CPUs. Check if kernel supports this feature.\n");
        return 1;
    }

    /* Verify settings */
    print_entangled_cpus();

    /* Run tests */
    test_same_user();
    test_different_users();

    /* Reset entangled CPUs (disable) */
    printf("\n=== Resetting entangled CPUs ===\n");
    set_entangled_cpus(0, 0);
    print_entangled_cpus();

    printf("\n=== All tests complete ===\n");
    return 0;
}
