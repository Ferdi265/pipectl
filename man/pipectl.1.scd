pipectl(1)

# NAME

pipectl - a simple named pipe management utility

# SYNOPSIS

*pipectl* [-h,-o,-i,-n N,-p P,-f,-l]

# DESCRIPTION

*pipectl* is a tool to create and manage short-lived named pipes that can be
used to feed stdin to a longer-lived program from elsewhere in the system
without needing a complex IPC mechanism such as UNIX domain sockets.

*pipectl* is intended to be used in pairs, one call to *pipectl* opens the pipe
(*-o*) and feeds it to a process, and all others with the same name or path will
write to the stdin of that process:

	$ pipectl -o | daemon-process

	$ echo "do-the-thing --color red" | pipectl -i

	$ echo "quit" | pipectl -i

The daemon process process will never observe an EOF on its stdin. *pipectl*
will instead block until another input pipe (*-i*) is openend and will then feed
the stdin of that process to the daemon process asynchronously.

When the daemon process exits or closes its stdin, *pipectl* will clean up and
remove the created named pipe automatically.

# OPTIONS

*-h, --help*
	Show help message and exit.

*-o, --out*
	Create a named pipe and print its contents to stdout.

*-i, --in*
	Write stdin to an open named pipe created with *-o*.

*-n N, --name N*
	Create named pipe with a custom name. See *PATHS*.

*-p P, --path P*
	Create a names pipe at a custom path. See *PATHS*.

*-f, --force*
	Force overwrite an existing named pipe event if it already exists (only usable with *-o*).

*-l, --lock*
	Use *flock*(2) to synchronize writes to the pipe (only usable with *-i*).

# PATHS

By default, *pipectl* will create the named pipe in a temporary directory
(*TMP*). The first available directory of these will be used:

- *$XDG_RUNTIME_DIR*
- *$TMPDIR*
- /tmp

The resulting path of the named pipe is as follows:

*default*
	*TMP*/pipectl.*UID*.pipe

*with --name NAME*
	*TMP*/pipectl.*UID*.*NAME*.pipe

*with --path PATH*
	*PATH*

where *UID* is the real user id of the *pipectl* process (see *getuid*(3)).

# AUTHORS

Maintained by Ferdinand Bachmann <ferdinand.bachmann@yrlf.at>. More information on *pipectl* can be found at <https://github.com/Ferdi265/pipectl>.

# SEE ALSO

*pipectl*(1) *mkfifo*(1) *flock*(2) *getuid*(3)
