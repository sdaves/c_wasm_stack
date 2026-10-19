#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>

#define TARGET_IP "127.0.0.1"
#define TARGET_PORT 8080
#define THREAD_COUNT 8
#define REQUESTS_PER_THREAD 5000

static const char *target_ip = TARGET_IP;
static int target_port = TARGET_PORT;
static int requests_per_thread = REQUESTS_PER_THREAD;

void *run_benchmark(void *arg) {
    (void)arg;
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(target_port);
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr);

    char request[] = "GET /api/bench HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    char buffer[256];
    long successful_hits = 0;

    for (int i = 0; i < requests_per_thread; i++) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;
        if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) == 0) {
            send(sock, request, strlen(request), MSG_NOSIGNAL);
            if (recv(sock, buffer, sizeof(buffer), 0) > 0) successful_hits++;
        }
        close(sock);
    }
    return (void *)successful_hits;
}

static int env_int(const char *name, int fallback) {
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    int parsed = atoi(value);
    return parsed > 0 ? parsed : fallback;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    const char *ip = getenv("BENCH_IP");
    if (ip && *ip) target_ip = ip;
    target_port = env_int("BENCH_PORT", TARGET_PORT);
    requests_per_thread = env_int("BENCH_REQUESTS", REQUESTS_PER_THREAD);
    int thread_count = env_int("BENCH_THREADS", THREAD_COUNT);

    pthread_t *threads = calloc((size_t)thread_count, sizeof(*threads));
    if (!threads) return EXIT_FAILURE;

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&threads[i], NULL, run_benchmark, NULL) != 0) {
            threads[i] = 0;
        }
    }

    long total_requests = 0;
    for (int i = 0; i < thread_count; i++) {
        if (!threads[i]) continue;
        void *hits;
        pthread_join(threads[i], &hits);
        total_requests += (long)hits;
    }
    free(threads);

    clock_gettime(CLOCK_MONOTONIC, &end);
    double elapsed = (double)(end.tv_sec - start.tv_sec) +
                     (double)(end.tv_nsec - start.tv_nsec) / 1e9;
    long attempted = (long)thread_count * requests_per_thread;

    printf("\n--- API BENCHMARK RESULTS ---\n");
    printf("Target:               %s:%d\n", target_ip, target_port);
    printf("Threads:              %d\n", thread_count);
    printf("Requests:             %ld (%ld failed)\n", attempted, attempted - total_requests);
    printf("Total Execution Time: %.4f seconds\n", elapsed);
    printf("Throughput Rate:      %.2f TPS\n", elapsed > 0.0 ? (double)total_requests / elapsed : 0.0);
    return total_requests > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
