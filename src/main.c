#define _GNU_SOURCE
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/signal.h>

#define log_error(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)

typedef struct {
    bool out;
    bool in;
    char * name;

    char * pipe_path;
    int pipe_out_fd;
    int pipe_in_fd;
} ctx_t;

void cleanup(ctx_t * ctx) {
    free(ctx->pipe_path);
    if (ctx->pipe_out_fd != -1) close(ctx->pipe_out_fd);
    if (ctx->pipe_in_fd != -1) close(ctx->pipe_in_fd);
    if (ctx->out) unlink(ctx->pipe_path);
}

void exit_fail(ctx_t * ctx) {
    cleanup(ctx);
    exit(1);
}

void usage(ctx_t * ctx) {
    printf("usage: pipectl [options]\n");
    printf("\n");
    printf("options:\n");
    printf("  -h, --help    show this help\n");
    printf("  -o, --out     create a pipe and print its contents to stdout\n");
    printf("  -i, --in      write stdin to an open pipe\n");
    printf("  -n, --name N  use a pipe with a custom name instead of the default\n");
    cleanup(ctx);
    exit(0);
}

void parse_opt(ctx_t * ctx, int argc, char ** argv) {
    while (argc > 0 && argv[0][0] == '-') {
        if (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
            usage(ctx);
        } else if (strcmp(argv[0], "-o") == 0 || strcmp(argv[0], "--out") == 0) {
            ctx->out = true;
        } else if (strcmp(argv[0], "-i") == 0 || strcmp(argv[0], "--in") == 0) {
            ctx->in = true;
        } else if (strcmp(argv[0], "-n") == 0 || strcmp(argv[0], "--name") == 0) {
            if (argc < 2) {
                fprintf(stderr, "error: parse_opt: option %s requires an argument\n", argv[0]);
                exit_fail(ctx);
            }

            if (strcspn(argv[1], "/") != strlen(argv[1])) {
                log_error("pipe name may not contain slashes\n");
                exit_fail(ctx);
            }

            ctx->name = argv[1];

            argv++;
            argc--;
        } else if (strcmp(argv[0], "--") == 0) {
            argv++;
            argc--;
            break;
        } else {
            log_error("error: invalid option %s\n", argv[0]);
            exit_fail(ctx);
        }

        argv++;
        argc--;
    }

    if (argc > 0 || (!ctx->in && !ctx->out)) {
        usage(ctx);
    }
}

void get_pipe_path(ctx_t * ctx) {
    int uid = getuid();

    char * path = NULL;
    int status;
    if (ctx->name == NULL) {
        status = asprintf(&path, "/tmp/pipectl.%d.pipe", uid);
    } else {
        status = asprintf(&path, "/tmp/pipectl.%d.%s.pipe", uid, ctx->name);
    }

    if (status == -1) {
        log_error("failed to format pipe path\n");
        exit_fail(ctx);
    }

    ctx->pipe_path = path;
}

ctx_t * sig_ctx;
void on_signal_sigint(int signum) {
    (void)signum;

    cleanup(sig_ctx);
    exit(0);
}

void create_out_pipe(ctx_t * ctx) {
    ctx->pipe_out_fd = mkfifo(ctx->pipe_path, 0666);
    if (ctx->pipe_out_fd == -1) {
        log_error("could not create pipe at '%s': %s\n", ctx->pipe_path, strerror(errno));
        exit_fail(ctx);
    }

    int flags = fcntl(ctx->pipe_out_fd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(ctx->pipe_out_fd, F_SETFL, flags);

    signal(SIGINT, on_signal_sigint);
}

void open_in_pipe(ctx_t * ctx) {
    ctx->pipe_in_fd = open(ctx->pipe_path, O_WRONLY);
    if (ctx->pipe_in_fd == -1) {
        log_error("could not open pipe at '%s': %s\n", ctx->pipe_path, strerror(errno));
        exit_fail(ctx);
    }

    struct stat stat;
    if (fstat(ctx->pipe_in_fd, &stat) == -1) {
        log_error("could not open pipe at '%s': %s\n", ctx->pipe_path, strerror(errno));
        exit_fail(ctx);
    }

    if (!S_ISFIFO(stat.st_mode)) {
        log_error("could not open pipe at '%s': File is not a named pipe\n", ctx->pipe_path);
        exit_fail(ctx);
    }

    int flags = fcntl(STDIN_FILENO, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(STDIN_FILENO, F_SETFL, flags);
}

#define BUFFER_SIZE 4096
bool pipe_data(ctx_t * ctx, int from_fd, int to_fd, char * label) {
    char buffer[BUFFER_SIZE];

    while (true) {
        ssize_t num = read(from_fd, buffer, sizeof buffer);
        if (num == -1 && errno == EWOULDBLOCK) {
            return true;
        } else if (num == -1) {
            log_error("error: %s: failed to read data\n", label);
            exit_fail(ctx);
        } else if (num == 0) {
            return false;
        }

        write(to_fd, buffer, num);
    }
}

void event_loop(ctx_t * ctx) {
    struct pollfd fds[2];
    fds[0].fd = ctx->out ? ctx->pipe_out_fd : -1;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = ctx->in ? STDIN_FILENO : -1;
    fds[1].events = POLLIN;
    fds[1].revents = 0;

    bool out_closed = !ctx->out;
    bool in_closed = !ctx->in;
    while (poll(fds, 2, -1) >= 0 && (!out_closed || !in_closed)) {
        if (fds[0].revents & POLLIN) {
            fprintf(stderr, "debug: reading from pipe to stdout\n");
            if (!pipe_data(ctx, ctx->pipe_out_fd, STDOUT_FILENO, "pipe output")) {
                fds[0].fd = -1;
                out_closed = true;
            }
            fds[0].revents = 0;
        }

        if (fds[1].revents & POLLIN) {
            fprintf(stderr, "debug: writing from stdin to pipe\n");
            if (!pipe_data(ctx, STDIN_FILENO, ctx->pipe_in_fd, "pipe output")) {
                fds[1].fd = -1;
                in_closed = true;
            }
            fds[1].revents = 0;
        }
    }
}

int main(int argc, char ** argv) {
    ctx_t ctx;
    ctx.out = false;
    ctx.in = false;
    ctx.name = NULL;
    ctx.pipe_path = NULL;
    ctx.pipe_out_fd = -1;
    ctx.pipe_in_fd = -1;

    parse_opt(&ctx, argc, argv);
    get_pipe_path(&ctx);

    if (ctx.out) create_out_pipe(&ctx);
    if (ctx.in) open_in_pipe(&ctx);

    event_loop(&ctx);

    cleanup(&ctx);
}
