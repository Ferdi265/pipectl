#define _GNU_SOURCE
#include <stddef.h>
#include <stdbool.h>
#include <stdnoreturn.h>
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

#define log_debug(ctx, fmt, ...) if ((ctx)->verbose) fprintf(stderr, "debug: " fmt, ##__VA_ARGS__)
#define log_error(fmt, ...) fprintf(stderr, "error: " fmt, ##__VA_ARGS__)

#define ARRAY_SIZE(arr) ((sizeof (arr)) / (sizeof ((arr)[0])))

#define BUFFER_SIZE 4096

typedef struct {
    char * data;
    size_t size;
    size_t capacity;
} buf_t;

typedef struct {
    bool out;
    bool in;
    bool force;
    bool lock;
    bool verbose;
    char * name;

    char * pipe_path;
    buf_t pipe_out_buffer;
    buf_t pipe_in_buffer;
    int pipe_out_fd;
    int pipe_in_fd;
} ctx_t;

ctx_t * sig_ctx;

void cleanup(ctx_t * ctx) {
    if (ctx->pipe_out_fd != -1) close(ctx->pipe_out_fd);
    if (ctx->pipe_in_fd != -1) close(ctx->pipe_in_fd);
    if (ctx->out && ctx->pipe_path != NULL) unlink(ctx->pipe_path);
    free(ctx->pipe_path);
    free(ctx->pipe_out_buffer.data);
    free(ctx->pipe_in_buffer.data);
    sig_ctx = NULL;
}

noreturn void exit_fail(ctx_t * ctx) {
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
    printf("  -p, --path P  use a custom path P for the pipe created by pipectl\n");
    printf("  -f, --force   force create a pipe even if one already exists\n");
    printf("  -l, --lock    use flock(2) to synchronize writes to the pipe\n");
    printf("  -v, --verbose print debug messages on stderr\n");
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
        } else if (strcmp(argv[0], "-v") == 0 || strcmp(argv[0], "--verbose") == 0) {
            ctx->verbose = true;
        } else if (strcmp(argv[0], "-n") == 0 || strcmp(argv[0], "--name") == 0) {
            if (argc < 2) {
                log_error("option '%s' requires an argument\n", argv[0]);
                exit_fail(ctx);
            }

            if (strcspn(argv[1], "/") != strlen(argv[1])) {
                log_error("option '%s': pipe name may not contain slashes\n", argv[0]);
                exit_fail(ctx);
            }

            ctx->name = argv[1];

            argv++;
            argc--;
        } else if (strcmp(argv[0], "-p") == 0 || strcmp(argv[0], "--path") == 0) {
            if (argc < 2) {
                log_error("option '%s' requires an argument\n", argv[0]);
                exit_fail(ctx);
            }

            char * path = strdup(argv[1]);
            if (path == NULL) {
                log_error("option '%s': failed to allocate copy of custom path '%s'\n", argv[0], argv[1]);
                exit_fail(ctx);
            }

            ctx->pipe_path = path;

            argv++;
            argc--;
        } else if (strcmp(argv[0], "--") == 0) {
            argv++;
            argc--;
            break;
        } else {
            log_error("invalid option '%s'\n", argv[0]);
            exit_fail(ctx);
        }

        argv++;
        argc--;
    }

    if (argc > 0 || (!ctx->in && !ctx->out)) {
        usage(ctx);
    }
}

char * get_tmp_dir(void) {
    char * tmp_dir = NULL;

    if (tmp_dir == NULL) tmp_dir = getenv("XDG_RUNTIME_DIR");
    if (tmp_dir == NULL) tmp_dir = getenv("TMPDIR");
    if (tmp_dir == NULL) tmp_dir = "/tmp";

    return tmp_dir;
}

void get_pipe_path(ctx_t * ctx) {
    int uid = getuid();

    char * tmp_dir = get_tmp_dir();

    char * path = NULL;
    int status;
    if (ctx->name == NULL) {
        status = asprintf(&path, "%s/pipectl.%d.pipe", tmp_dir, uid);
    } else {
        status = asprintf(&path, "%s/pipectl.%d.%s.pipe", tmp_dir, uid, ctx->name);
    }

    if (status == -1) {
        log_error("failed to allocate formatted pipe path\n");
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

void buf_allocate(ctx_t * ctx, buf_t * buffer, char * label) {
    buffer->data = malloc(sizeof (char) * BUFFER_SIZE);
    if (buffer->data == NULL) {
        log_error("failed to allocate %s buffer\n", label);
        exit_fail(ctx);
    }

    buffer->capacity = BUFFER_SIZE;
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

    // make reading from and writing to pipe nonblocking
    int flags = fcntl(ctx->pipe_out_fd, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(ctx->pipe_out_fd, F_SETFL, flags);

    return fd;
}

void create_out_pipe(ctx_t * ctx) {
    if (ctx->force) {
        if (unlink(ctx->pipe_path) == -1 && errno != ENOENT) {
            log_error("could not remove old pipe at '%s': %s\n", ctx->pipe_path, strerror(errno));
            exit_fail(ctx);
        }
    }

    if (mkfifo(ctx->pipe_path, 0666) == -1) {
        log_error("could not create pipe at '%s': %s\n", ctx->pipe_path, strerror(errno));
        exit_fail(ctx);
    }

    ctx->pipe_out_fd = open_pipe(ctx, O_RDWR);

    buf_allocate(ctx, &ctx->pipe_out_buffer, "output");

    // make writing to stdout nonblocking
    int flags = fcntl(STDOUT_FILENO, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(STDOUT_FILENO, F_SETFL, flags);
}

void open_in_pipe(ctx_t * ctx) {
    ctx->pipe_in_fd = open_pipe(ctx, O_WRONLY);

    if (ctx->lock) flock(ctx->pipe_in_fd, LOCK_EX);

    buf_allocate(ctx, &ctx->pipe_in_buffer, "input");

    // make reading from stdin nonblocking
    int flags = fcntl(STDIN_FILENO, F_GETFL);
    flags |= O_NONBLOCK;
    fcntl(STDIN_FILENO, F_SETFL, flags);
}

bool buf_can_read(buf_t * buffer) {
    return buffer->size < buffer->capacity;
}

bool buf_can_write(buf_t * buffer) {
    return buffer->size > 0;
}

ssize_t pipe_to_buffer(ctx_t * ctx, int from_fd, buf_t * to_buffer, char * label) {
    ssize_t num = read(from_fd, to_buffer->data + to_buffer->size, to_buffer->capacity - to_buffer->size);
    if (num == -1 && errno == EWOULDBLOCK) {
        log_debug(ctx, "reading from %s would block\n", label);
    } else if (num == -1) {
        log_error("failed to read data from %s: %s\n", label, strerror(errno));
        exit_fail(ctx);
    } else {
        to_buffer->size += num;
    }

    return num;
}

ssize_t pipe_from_buffer(ctx_t * ctx, buf_t * from_buffer, int to_fd, char * label) {
    ssize_t num = write(to_fd, from_buffer->data, from_buffer->size);
    if (num == -1 && errno == EWOULDBLOCK) {
        log_debug(ctx, "writing to %s would block\n", label);
    } else if (num == -1) {
        log_error("failed to write data to %s: %s\n", label, strerror(errno));
        exit_fail(ctx);
    } else {
        from_buffer->size -= num;
        memmove(from_buffer->data, from_buffer->data + num, from_buffer->size);
    }

    return num;
}

void event_loop(ctx_t * ctx) {
    struct pollfd pollfds[5];
    struct pollfd * poll_stdin = &pollfds[0];
    struct pollfd * poll_pipe_in = &pollfds[1];
    struct pollfd * poll_pipe_out = &pollfds[2];
    struct pollfd * poll_stdout = &pollfds[3];
    struct pollfd * poll_stdout_closed = &pollfds[4];

    poll_stdin->revents = 0;
    poll_pipe_in->revents = 0;
    poll_pipe_out->revents = 0;
    poll_stdout->revents = 0;
    poll_stdout_closed->revents = 0;

    bool out_closed = !ctx->out;
    bool in_closed = !ctx->in;
    bool in_pending = false;
    bool closing = false;
    do {
        // --- stdin -----------------------------------------------------------
        if (poll_stdin->revents & (POLLERR | POLLHUP)) {
            log_debug(ctx, "stdin closed\n");
            in_closed = true;
        }

        if (poll_stdin->revents & POLLIN) {
            log_debug(ctx, "reading from stdin\n");
            if (pipe_to_buffer(ctx, poll_stdin->fd, &ctx->pipe_in_buffer, "stdin") == 0) {
                log_debug(ctx, "stdin closed due to empty read\n");
                in_closed = true;
            }
        }

        poll_stdin->fd = !in_closed && buf_can_read(&ctx->pipe_in_buffer) ? STDIN_FILENO : -1;
        poll_stdin->events = POLLIN;
        poll_stdin->revents = 0;

        // --- pipe_in ---------------------------------------------------------
        if (poll_pipe_in->revents & POLLERR) {
            log_error("failed to write to pipe: POLLERR\n");
            exit_fail(ctx);
        }

        if (poll_pipe_in->revents & POLLOUT) {
            log_debug(ctx, "writing to pipe\n");
            pipe_from_buffer(ctx, &ctx->pipe_in_buffer, poll_pipe_in->fd, "pipe");
        }

        in_pending = buf_can_write(&ctx->pipe_in_buffer);
        poll_pipe_in->fd = in_pending ? ctx->pipe_in_fd : -1;
        poll_pipe_in->events = POLLOUT;
        poll_pipe_in->revents = 0;

        // --- pipe_out --------------------------------------------------------
        if (poll_pipe_out->revents & POLLERR) {
            log_error("failed to read from pipe: POLLERR\n");
            exit_fail(ctx);
        }

        if (poll_pipe_out->revents & POLLIN) {
            log_debug(ctx, "reading from pipe\n");
            pipe_to_buffer(ctx, poll_pipe_out->fd, &ctx->pipe_out_buffer, "pipe");
        }

        poll_pipe_out->fd = !out_closed && buf_can_read(&ctx->pipe_out_buffer) ? ctx->pipe_out_fd : -1;
        poll_pipe_out->events = POLLIN;
        poll_pipe_out->revents = 0;

        // --- stdout ----------------------------------------------------------
        if (poll_stdout->revents & POLLOUT) {
            log_debug(ctx, "writing to stdout\n");
            pipe_from_buffer(ctx, &ctx->pipe_out_buffer, poll_stdout->fd, "stdout");
        }

        poll_stdout->fd = !out_closed && buf_can_write(&ctx->pipe_out_buffer) ? STDOUT_FILENO : -1;
        poll_stdout->events = POLLOUT;
        poll_stdout->revents = 0;

        // --- stdout_closed ---------------------------------------------------
        if (poll_stdout_closed->revents & POLLERR) {
            log_debug(ctx, "stdout closed\n");
            out_closed = true;
        }

        poll_stdout_closed->fd = !out_closed ? STDOUT_FILENO : -1;
        poll_stdout_closed->events = 0;
        poll_stdout_closed->revents = 0;

        // --- closing flag ----------------------------------------------------
        if (ctx->out) {
            closing = out_closed;
        } else {
            closing = in_closed && !in_pending;
        }

        log_debug(ctx, "polling\n");
    } while (!closing && poll(pollfds, ARRAY_SIZE(pollfds), -1) >= 0);
}

void init(ctx_t * ctx) {
    ctx->out = false;
    ctx->in = false;
    ctx->force = false;
    ctx->lock = false;
    ctx->verbose = false;
    ctx->name = NULL;
    ctx->pipe_path = NULL;
    ctx->pipe_out_fd = -1;
    ctx->pipe_in_fd = -1;

    register_signal_handlers(ctx);
}


int main(int argc, char ** argv) {
    ctx_t ctx;
    init(&ctx);

    parse_opt(&ctx, argc, argv);

    if (ctx.pipe_path == NULL) get_pipe_path(&ctx);
    if (ctx.out) create_out_pipe(&ctx);
    if (ctx.in) open_in_pipe(&ctx);

    event_loop(&ctx);

    cleanup(&ctx);
}
