// SPDX-License-Identifier: MIT
/* enter — portable isolated-rootfs launcher
 *
 * Place a copy of this binary at <rootfs>/enter and execute it.
 * Its parent directory is taken as the rootfs. The parent must look
 * like a real rootfs (contain /usr) or the launcher bails out.
 *
 * Usage: enter [program [args...]]
 *
 * With no arguments, the launcher searches (in order):
 *   /sbin/init /etc/init /bin/init /init /linuxrc
 * and falls back to a shell:
 *   /bin/sh /bin/ash /bin/bash /usr/bin/sh
 *
 * Isolation is attempted in this order, degrading gracefully:
 *   1. mount+pid+uts+ipc+net namespaces + pivot_root
 *   2. chroot with whatever namespaces were available
 *   3. chdir only (no filesystem isolation)
 *
 * Namespaces are unshared one at a time so partial-namespace kernels
 * (Android, LXC, WSL1, older containers) keep whatever subset works.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <errno.h>

extern char **environ;

#ifdef __ANDROID__
# define MAKEDEV(maj, min) makedev((unsigned)(maj), (unsigned)(min))
#else
# define MAKEDEV(maj, min) makedev((maj), (min))
#endif

/* ============================================================ */
/* Environment                                                   */
/* ============================================================ */

static void sanitize_env(void)
{
    char *lang = getenv("LANG");
    char *term = getenv("TERM");
    char *tz   = getenv("TZ");

    clearenv();

    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/root", 1);
    setenv("USER", "root", 1);
    setenv("IFS",  " \t\n", 1);
    setenv("LANG", lang ? lang : "C", 1);
    setenv("TERM", term ? term : "linux", 1);
    if (tz)
        setenv("TZ", tz, 1);
}

/* ============================================================ */
/* /dev handling                                                 */
/* ============================================================ */

static int is_usable_dev(const char *devdir)
{
    char        nullpath[4096];
    struct stat st;
    int         fd;

    snprintf(nullpath, sizeof(nullpath), "%s/null", devdir);
    fd = open(nullpath, O_RDWR);
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode)) {
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

static void ensure_dev(const char *devdir, const char *name,
                       unsigned int maj, unsigned int min, mode_t mode)
{
    char        path[4096];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", devdir, name);

    if (stat(path, &st) == 0) {
        if (S_ISCHR(st.st_mode))
            return;
        (void)unlink(path);
    }
    (void)mknod(path, S_IFCHR | mode, MAKEDEV(maj, min));
}

static void fill_vital_devices(const char *devdir)
{
    char pts[4096];

    ensure_dev(devdir, "null",    1, 3, 0666);
    ensure_dev(devdir, "zero",    1, 5, 0666);
    ensure_dev(devdir, "full",    1, 7, 0666);
    ensure_dev(devdir, "random",  1, 8, 0666);
    ensure_dev(devdir, "urandom", 1, 9, 0666);
    ensure_dev(devdir, "tty",     5, 0, 0666);
    ensure_dev(devdir, "console", 5, 1, 0600);
    ensure_dev(devdir, "ptmx",    5, 2, 0666);

    snprintf(pts, sizeof(pts), "%s/pts", devdir);
    (void)mkdir(pts, 0755);
}

/* ============================================================ */
/* Mounts                                                        */
/* ============================================================ */

static int try_mount(const char *src, const char *tgt, const char *fstype,
                     unsigned long flags, const char *data)
{
    if (mount(src, tgt, fstype, flags, data) == 0)
        return 0;
    fprintf(stderr, "enter: mount %s on %s: %s\n",
            fstype ? fstype : src, tgt, strerror(errno));
    return -1;
}

static void setup_pseudo_fs(const char *root, int dev_usable)
{
    char path[4096];

    /* /dev — never shadow a working one */
    snprintf(path, sizeof(path), "%s/dev", root);
    (void)mkdir(path, 0755);

    if (dev_usable) {
        fill_vital_devices(path);
    } else if (mount("devtmpfs", path, "devtmpfs", MS_NOSUID, NULL) == 0) {
        /* kernel-provided, all good */
    } else if (try_mount("tmpfs", path, "tmpfs",
                         MS_NOSUID, "mode=0755") == 0) {
        fill_vital_devices(path);
    }

    snprintf(path, sizeof(path), "%s/proc", root);
    (void)mkdir(path, 0555);
    try_mount("proc", path, "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

    snprintf(path, sizeof(path), "%s/sys", root);
    (void)mkdir(path, 0555);
    try_mount("sysfs", path, "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

    /* devpts creates char devices → do NOT add MS_NODEV here */
    snprintf(path, sizeof(path), "%s/dev/pts", root);
    (void)mkdir(path, 0755);
    try_mount("devpts", path, "devpts", MS_NOSUID | MS_NOEXEC,
              "mode=0620,ptmxmode=0666,newinstance");

    snprintf(path, sizeof(path), "%s/dev/shm", root);
    (void)mkdir(path, 1777);
    try_mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");

    snprintf(path, sizeof(path), "%s/tmp", root);
    (void)mkdir(path, 1777);
    try_mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");

    snprintf(path, sizeof(path), "%s/run", root);
    (void)mkdir(path, 0755);
    try_mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
}

/* ============================================================ */
/* pivot_root                                                    */
/* ============================================================ */

static int do_pivot_root(const char *root)
{
    static const char put_old_rel[] = "/.enter-oldroot";
    char put_old[4096];
    int  n;

    n = snprintf(put_old, sizeof(put_old), "%s%s", root, put_old_rel);
    if (n < 0 || (size_t)n >= sizeof(put_old))
        return -1;

    (void)rmdir(put_old);
    if (mkdir(put_old, 0700) != 0)
        return -1;

    if (mount(root, root, NULL, MS_BIND | MS_REC, NULL) != 0) {
        (void)rmdir(put_old);
        return -1;
    }

    if (syscall(SYS_pivot_root, root, put_old) != 0) {
        (void)umount2(root, MNT_DETACH);
        (void)rmdir(put_old);
        return -1;
    }

    if (chdir("/") != 0)
        return -1;

    (void)umount2(put_old_rel, MNT_DETACH);
    (void)rmdir(put_old_rel);
    return 0;
}

/* ============================================================ */
/* Enter the rootfs                                              */
/* ============================================================ */

enum { MODE_FAIL = 0, MODE_PIVOT, MODE_CHROOT, MODE_CHDIR };

static int enter_rootfs(const char *root, int dev_usable)
{
    setup_pseudo_fs(root, dev_usable);

    if (do_pivot_root(root) == 0)
        return MODE_PIVOT;

    fprintf(stderr, "enter: pivot_root failed (%s), trying chroot\n",
            strerror(errno));

    if (chroot(root) == 0 && chdir("/") == 0) {
        setup_pseudo_fs("/", dev_usable);
        return MODE_CHROOT;
    }

    fprintf(stderr, "enter: chroot failed (%s), last-resort chdir\n",
            strerror(errno));

    if (chdir(root) == 0)
        return MODE_CHDIR;

    return MODE_FAIL;
}

/* ============================================================ */
/* Namespaces                                                    */
/* ============================================================ */

static int try_ns(int flag, const char *name)
{
    if (unshare(flag) == 0) {
        fprintf(stderr, "enter: namespace %s ok\n", name);
        return 1;
    }
    fprintf(stderr, "enter: namespace %s unavailable: %s\n",
            name, strerror(errno));
    return 0;
}

/* ============================================================ */
/* Locate self                                                   */
/* ============================================================ */

static int get_self_dir(char *buf, size_t sz, const char *argv0)
{
    char    path[4096];
    ssize_t n;
    char   *slash;

    n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0) {
        path[n] = '\0';
    } else {
        if (!argv0 || !strchr(argv0, '/'))
            return -1;
        snprintf(path, sizeof(path), "%s", argv0);
    }

    slash = strrchr(path, '/');
    if (!slash)
        return -1;
    if (slash == path)
        snprintf(buf, sz, "/");
    else {
        *slash = '\0';
        snprintf(buf, sz, "%s", path);
    }
    return 0;
}

/* ============================================================ */
/* Init / shell search                                           */
/* ============================================================ */

static const char *const init_candidates[] = {
    "/sbin/init", "/etc/init", "/bin/init", "/init", "/linuxrc", NULL
};

static const char *const shell_candidates[] = {
    "/bin/sh", "/bin/ash", "/bin/bash", "/usr/bin/sh", NULL
};

static const char *find_in_rootfs(const char *root,
                                  const char *const *cands,
                                  char *out, size_t outsz)
{
    size_t i;
    for (i = 0; cands[i]; i++) {
        char full[4096];
        snprintf(full, sizeof(full), "%s%s", root, cands[i]);
        if (access(full, X_OK) == 0) {
            snprintf(out, outsz, "%s", cands[i]);
            return out;
        }
    }
    return NULL;
}

/* ============================================================ */
/* main                                                          */
/* ============================================================ */

int main(int argc, char *argv[])
{
    char        rootfs[4096];
    char        resolved[4096];
    char        usrpath[4096];
    char        devpath[4096];
    char        prog_buf[4096];
    char       *auto_argv[2];
    char      **child_argv;
    const char *prog;
    struct stat st;
    int         dev_usable;
    int         have_mnt_ns;
    pid_t       pid;
    int         status;

    /* 1. find our own directory = candidate rootfs */
    if (get_self_dir(rootfs, sizeof(rootfs), argv[0]) != 0) {
        fprintf(stderr, "enter: cannot determine own directory\n");
        return 1;
    }

    if (realpath(rootfs, resolved) == NULL) {
        fprintf(stderr, "enter: realpath(%s): %s\n", rootfs, strerror(errno));
        return 1;
    }
    snprintf(rootfs, sizeof(rootfs), "%s", resolved);

    /* 2. sanity check: it must look like a real rootfs */
    snprintf(usrpath, sizeof(usrpath), "%s/usr", rootfs);
    if (stat(usrpath, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "enter: %s is not a rootfs (no /usr)\n", rootfs);
        return 1;
    }

    /* 3. determine program to run */
    if (argc >= 2) {
        prog       = argv[1];
        child_argv = &argv[1];
    } else {
        const char *found;

        found = find_in_rootfs(rootfs, init_candidates,
                               prog_buf, sizeof(prog_buf));
        if (!found)
            found = find_in_rootfs(rootfs, shell_candidates,
                                   prog_buf, sizeof(prog_buf));
        if (!found) {
            fprintf(stderr,
                    "enter: no init, linuxrc or shell found in %s\n", rootfs);
            return 1;
        }
        prog         = found;
        auto_argv[0] = prog_buf;
        auto_argv[1] = NULL;
        child_argv   = auto_argv;
    }

    /* 4. pre-flight /dev probe */
    snprintf(devpath, sizeof(devpath), "%s/dev", rootfs);
    dev_usable = is_usable_dev(devpath);
    fprintf(stderr, "enter: rootfs=%s dev_usable=%d prog=%s\n",
            rootfs, dev_usable, prog);

    /* 5. sanitise env */
    sanitize_env();

    /* 6. unshare namespaces one at a time */
    have_mnt_ns = try_ns(CLONE_NEWNS, "mount");
    if (have_mnt_ns)
        (void)mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
    try_ns(CLONE_NEWUTS, "uts");
    try_ns(CLONE_NEWIPC, "ipc");
    try_ns(CLONE_NEWNET, "net");
    try_ns(CLONE_NEWPID, "pid");   /* fork below activates it */

    /* 7. fork: with CLONE_NEWPID the child becomes PID 1 */
    pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }

    if (pid == 0) {
        int  mode = enter_rootfs(rootfs, dev_usable);
        char final_prog[4096];

        if (mode == MODE_FAIL) {
            fprintf(stderr, "enter: cannot enter %s\n", rootfs);
            _exit(126);
        }

        if (mode == MODE_CHDIR && prog[0] == '/')
            snprintf(final_prog, sizeof(final_prog), "%s%s", rootfs, prog);
        else
            snprintf(final_prog, sizeof(final_prog), "%s", prog);

        execve(final_prog, child_argv, environ);
        fprintf(stderr, "enter: execve %s: %s\n",
                final_prog, strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        perror("waitpid");
        return 1;
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}
