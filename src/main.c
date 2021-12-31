#define _GNU_SOURCE
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/file.h>

#define log_error(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)

typedef struct {
    bool out;
    bool in;
    bool force;
    bool lock;
    char * name;

    char * pipe_path;
    int pipe_out_fd;
    int pipe_in_fd;
} ctx_t;

ctx_t * sig_ctx;

void cleanup(ctx_t * ctx) {
    if (ctx->pipe_out_fd != -1) close(ctx->pipe_out_fd);
    if (ctx->pipe_in_fd != -1) close(ctx->pipe_in_fd);
    if (ctx->out && ctx->pipe_path != NULL) unlink(ctx->pipe_path);
    free(ctx->pipe_path);
    sig_ctx = NULL;
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
    printf("  -f, --force   force create a pipe even if one already exists\n");
    printf("  -l, --lock    use flock(2) to synchronize writes to the pipe\n");
    cleanup(ctx);
    exit(0);
}

void parse_opt(ctx_t * ctx, int argc, char ** argv) {
    if (argc > 0) {
        // skip program name
        argv++;
        argc--;
    }

    while (argc > 0 && argv[0][0] == '-') {
        if (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
            usage(ctx);
        } else if (strcmp(argv[0], "-o") == 0 || strcmp(argv[0], "--out") == 0) {
            ctx->out = true;
        } else if (strcmp(argv[0], "-i") == 0 || strcmp(argv[0], "--in") == 0) {
            ctx->in = true;
        } else if (strcmp(argv[0], "-f") == 0 || strcmp(argv[0], "--force") == 0) {
            ctx->force = true;
        } else if (strcmp(argv[0], "-l") == 0 || strcmp(argv[0], "--lock") == 0) {
            ctx->lock = true;
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

void cleanup_on_signal(int signum) {
    (void)signum;

    cleanup(sig_ctx);
    exit(0);
}

void register_signal_handlers(ctx_t * ctx) {
    sig_ctx = ctx;
    signal(SIGINT, cleanup_on_signal);
    signal(SIGHUP, cleanup_on_signal);
    signal(SIGTERM, cleanup_on_signal);
    signal(SIGPIPE, cleanup_on_signal);
}

int open_pipe(ctx_t * ctx, int mode) {
    int fd = open(ctx->pipe_path, mode);
    if (fd == -1) {
        log_error("could not open pipe at '%s': %s\n", ctx->pipe_path, strerror(errno));
        exit_fail(ctx);
    }

    struct stat stat;
    if (fstat(fd, &stat) == -1) {
        log_error("could not open pipe at '%s': %s\n", ctx->pipe_path, strerror(errno));
        close(fd);
        exit_fail(ctx);
    }

    if (!S_ISFIFO(stat.st_mode)) {
        log_error("could not open pipe at '%s': File is not a named pipe\n", ctx->pipe_path);
        close(fd);
        exit_fail(ctx);
    }

    return fd;
}

void create_out_pipe(ctx_t * ctx) {
    if (mkfifo(ctx->pipe_path, 0666) == -1) {
        log_error("could not create pipe at '%s': %s\n", ctx->pipe_path, strerror(errno));
        exit_fail(ctx);
    }

    ctx->pipe_out_fd = open_pipe(ctx, O_RDWR);

    int flags = fcntl(ctx->pipe_out_fd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(ctx->pipe_out_fd, F_SETFL, flags);
}

void open_in_pipe(ctx_t * ctx) {
    ctx->pipe_in_fd = open_pipe(ctx, O_WRONLY);

    int flags = fcntl(STDIN_FILENO, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(STDIN_FILENO, F_SETFL, flags);

    if (ctx->lock) flock(ctx->pipe_in_fd, LOCK_EX);
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

        num = write(to_fd, buffer, num);
        if (num == -1) {
            return false;
        }
    }
}

void event_loop(ctx_t * ctx) {
    struct pollfd fds[3];
    fds[0].fd = ctx->out ? ctx->pipe_out_fd : -1;
    fds[0].events = POLLIN;
    fds[0].revents = 0;
    fds[1].fd = ctx->out ? STDOUT_FILENO : -1;
    fds[1].events = 0;
    fds[1].revents = 0;
    fds[2].fd = ctx->in ? STDIN_FILENO : -1;
    fds[2].events = POLLIN;
    fds[2].revents = 0;

    bool out_closed = !ctx->out;
    bool in_closed = !ctx->in;
    while ((!out_closed || !in_closed) && poll(fds, 3, -1) >= 0) {
        if (fds[0].revents & POLLIN) {
            pipe_data(ctx, ctx->pipe_out_fd, STDOUT_FILENO, "pipe output");
        }

        if (fds[1].revents & POLLERR) {
            out_closed = true;
            fds[0].fd = -1;
            fds[1].fd = -1;
        }

        if (fds[2].revents & POLLIN) {
            if (!pipe_data(ctx, STDIN_FILENO, ctx->pipe_in_fd, "pipe output")) {
                in_closed = true;
                fds[2].fd = -1;
            }
            fds[2].revents = 0;
        }

        if (fds[2].revents & POLLHUP) {
            in_closed = true;
            fds[2].fd = -1;
        }

        fds[0].revents = 0;
        fds[1].revents = 0;
        fds[2].revents = 0;
    }
}

int main(int argc, char ** argv) {
    ctx_t ctx;
    ctx.out = false;
    ctx.in = false;
    ctx.force = false;
    ctx.lock = false;
    ctx.name = NULL;
    ctx.pipe_path = NULL;
    ctx.pipe_out_fd = -1;
    ctx.pipe_in_fd = -1;

    parse_opt(&ctx, argc, argv);
    get_pipe_path(&ctx);
    register_signal_handlers(&ctx);

    if (ctx.force) unlink(ctx.pipe_path);
    if (ctx.out) create_out_pipe(&ctx);
    if (ctx.in) open_in_pipe(&ctx);

    event_loop(&ctx);

    cleanup(&ctx);
}
