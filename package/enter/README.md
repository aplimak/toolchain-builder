# enter

`enter` is a statically linked Linux rootfs launcher. Install a copy at the
root of a filesystem tree and run it from there. The executable's containing
directory is the target root; it must contain `/usr` and `/tmp`.

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

`/dev` must provide a usable character `/dev/null`: `devtmpfs` is preferred,
with a `tmpfs` plus device-node fallback. A proc filesystem is required.
`sysfs`, private `devpts`, and tmpfs mounts for `/tmp`, `/run`, and `/dev/shm`
are optional conveniences; failures are warnings and leave the underlying
rootfs paths available where possible. Resolver setup writes default
nameservers into the ephemeral `/run/resolv.conf`.

Diagnostics use `enter: warning:` for recoverable issues and `enter: error:`
for failures that stop startup or execution. Successful optional operations
and deliberately ignored cleanup failures are quiet.
