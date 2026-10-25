/* Process wall-clock benchmark helper. Uses only C/POSIX facilities. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int timestamp(struct timespec* now)
{
    if(clock_gettime(CLOCK_MONOTONIC, now) == 0) return 0;
    perror("benchmark clock");
    return -1;
}

int main(int argc, char** argv)
{
    if(argc < 3) { fprintf(stderr, "usage: %s <samples> <command> [args...]\n", argv[0]); return 1; }
    char* end;
    errno = 0;
    long count = strtol(argv[1], &end, 10);
    if(errno || !*argv[1] || *end || count < 1 || count > 10000)
    { fprintf(stderr, "benchmark samples must be 1..10000\n"); return 1; }
    for(long i = 0; i < count; ++i)
    {
        struct timespec start, finish;
        if(timestamp(&start)) return 1;
        pid_t child = fork();
        if(child < 0) { perror("benchmark fork"); return 1; }
        if(child == 0)
        {
            int sink = open("/dev/null", O_WRONLY);
            if(sink < 0 || dup2(sink, STDOUT_FILENO) < 0 || dup2(sink, STDERR_FILENO) < 0) _exit(126);
            if(sink > STDERR_FILENO) close(sink);
            execvp(argv[2], &argv[2]);
            _exit(127);
        }
        int status;
        pid_t waited;
        do { waited = waitpid(child, &status, 0); } while(waited < 0 && errno == EINTR);
        if(waited < 0) { perror("benchmark wait"); return 1; }
        if(timestamp(&finish)) return 1;
        if(!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        { fprintf(stderr, "benchmark command failed at sample %ld (wait status %d)\n", i + 1, status); return 1; }
        double ms = (double)(finish.tv_sec - start.tv_sec) * 1000.0 +
                    (double)(finish.tv_nsec - start.tv_nsec) / 1000000.0;
        printf("%.6f\n", ms);
        fflush(stdout); /* Keep earlier samples from being flushed by children. */
    }
    return 0;
}
