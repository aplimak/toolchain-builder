# enter

`enter` is a Linux rootfs launcher. Install a copy at the root of a filesystem
tree and run it from there. The executable's containing directory is the target
root; it must contain `/usr` and `/tmp`.

## Building

Autoconf's generated `configure` script and `Makefile.in` are included, so a
normal standalone build needs a C compiler and GNU make, not Autoconf or
Automake:

```sh
mkdir build
cd build
../configure --bindir=/ --enable-static
make
make install DESTDIR=/tmp/enter-root
```

Compiler diagnostics are always enabled (`-Wall -Wextra -Wformat=2`). Runtime
warnings for recoverable failures are enabled by default; use
`../configure --disable-runtime-warnings` to suppress those messages. Errors
remain visible. Use `--enable-static` to request a static executable. Static
linking is enabled by default for Buildroot, where the launcher must not depend
on libraries inside the target rootfs.

To regenerate the checked-in Autoconf/Automake files after editing
`configure.ac` or `Makefile.am`, run `autoreconf -fi` from this directory.

## Tests

Tests are opt-in and are not part of the default build:

```sh
mkdir build-tests
cd build-tests
../configure --enable-tests
make check
```

The suite runs natively on the build machine. It tests filesystem helpers using
temporary directories, environment sanitization in a child process, diagnostic
filtering, process exit/signal handling, process naming, and PTY attachment and
I/O. PTY checks are skipped when the host cannot allocate a PTY. Tests do not
mount filesystems, create namespaces, change the root, or modify host device
nodes. Buildroot explicitly configures `--disable-tests`; use the native suite
for behavior checks because a cross-built target test executable cannot be run
by the Buildroot host build.

```sh
/path/to/rootfs/enter /bin/sh
```

An explicit program receives the remaining command-line arguments. With no
program, `enter` searches `/bin/login`, `/usr/bin/login`, `/sbin/login`, and
`/usr/sbin/login`. If `/etc/init.d/rcS` is executable, it runs before login
and `/etc/init.d/rcK` runs after login.
The child environment is cleared and rebuilt with a minimal `PATH`, root home
and user, and the caller's `LANG`, `TERM`, and `TZ` when available.

## Isolation and compatibility

The mount namespace is required. PID, UTS, and IPC namespaces are attempted
independently and produce warnings when unavailable. Non-root execution
requires host support for unprivileged user namespaces and working
`uid_map`/`gid_map` files; otherwise startup is fatal. Network namespaces are
not enabled by default.

Root entry tries `pivot_root`, then a move-mount/chroot fallback, then plain
`chroot`. Read-only roots use OverlayFS when available; older kernels and
kernels without OverlayFS continue on the original read-only root. This keeps
the launcher usable on older Linux 3.x systems as well as newer kernels,
subject to the host's namespace and mount policy.

`/dev` must provide a usable character `/dev/null`. A proc filesystem is required.
`sysfs`, private `devpts`, and tmpfs mounts for `/tmp`, `/run`, and `/dev/shm`
are optional conveniences; failures are warnings and leave the underlying
rootfs paths available where possible. Private devpts first requests
`ptmxmode=0666`, then retries without that option for kernels that reject it.
`/dev/ptmx` is linked to `/dev/pts/ptmx` only after the latter is verified as
character device 5:2, adjusted to mode `0666`, and successfully opened; an
unusable or missing target leaves the direct `/dev/ptmx` device node in place.
Standard device nodes are checked against their expected major/minor numbers
and permissions. Resolver setup writes default nameservers into the ephemeral
`/run/resolv.conf`.

When private devpts is mounted and its PTY multiplexer is usable, `enter`
allocates a PTY for the requested program, makes the slave its controlling
terminal, and relays console input, output, and window-size changes. If PTY
allocation fails, it warns and uses the inherited console descriptors. The
launcher process labels fit Linux's 15-character process-name limit.

Diagnostics use `enter: warning:` for recoverable issues and `enter: error:`
for failures that stop startup or execution. Successful optional operations
and deliberately ignored cleanup failures are quiet.
