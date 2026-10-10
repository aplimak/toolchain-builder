// SPDX-License-Identifier: MIT
/* enter — portable isolated-rootfs launcher
 *
 * Place a copy of this binary at <rootfs>/enter and execute it.
 * Its parent directory is taken as the rootfs. The parent must look
 * like a real rootfs (contain /usr) or the launcher bails out.
 *
 * Usage: enter [program [args...]]
 *
 * With no arguments, the launcher searches for login binary
 * In this mode only, /etc/init.d/rcS is run before the login and
 * /etc/init.d/rcK after it. When an explicit program is given,
 * neither script runs.
 *
 * Namespaces are unshared one at a time so partial-namespace kernels
 * (Android, LXC, WSL1, older containers) keep whatever subset works.
 * Mount namespace is required.
 * Other namespaces are optional.
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
#include <sys/prctl.h>

extern char **environ;

#ifdef __ANDROID__
#define MAKEDEV(maj, min) makedev((unsigned)(maj), (unsigned)(min))
#else
#define MAKEDEV(maj, min) makedev((maj), (min))
#endif

static volatile pid_t g_wanted_pid;
static volatile sig_atomic_t g_wanted_reaped;
static volatile int g_wanted_status;

static void reap_children(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        if (pid == g_wanted_pid)
        {
            g_wanted_status = status;
            g_wanted_reaped = 1;
        }
    }
    errno = saved_errno;
}

/* ============================================================ */
/* Environment                                                   */
/* ============================================================ */

static void sanitize_env(void)
{
    const char *src_lang = getenv("LANG");
    const char *src_term = getenv("TERM");
    const char *src_tz = getenv("TZ");

    char *lang = src_lang ? strdup(src_lang) : NULL;
    char *term = src_term ? strdup(src_term) : NULL;
    char *tz = src_tz ? strdup(src_tz) : NULL;

    clearenv();

    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/root", 1);
    setenv("USER", "root", 1);
    setenv("IFS", " \t\n", 1);
    setenv("LANG", lang ? lang : "C", 1);
    setenv("TERM", term ? term : "linux", 1);
    if (tz)
        setenv("TZ", tz, 1);

    free(lang);
    free(term);
    free(tz);
}

/* ============================================================ */
/* /dev handling                                                 */
/* ============================================================ */

static int is_usable_dev(const char *devdir)
{
    char nullpath[4096];
    struct stat st;
    int fd;

    snprintf(nullpath, sizeof(nullpath), "%s/null", devdir);
    fd = open(nullpath, O_RDWR);
    if (fd < 0)
        return 0;
    if (fstat(fd, &st) != 0 || !S_ISCHR(st.st_mode))
    {
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

static void ensure_dev(const char *devdir, const char *name,
                       unsigned int maj, unsigned int min, mode_t mode)
{
    char path[4096];
    struct stat st;

    snprintf(path, sizeof(path), "%s/%s", devdir, name);

    if (stat(path, &st) == 0)
    {
        if (S_ISCHR(st.st_mode))
            return;
        (void)unlink(path);
    }
    (void)mknod(path, S_IFCHR | mode, MAKEDEV(maj, min));
}

static void fill_standard_links(const char *devdir)
{
    char path[4096];

    snprintf(path, sizeof(path), "%s/fd", devdir);
    unlink(path);
    symlink("/proc/self/fd", path);

    snprintf(path, sizeof(path), "%s/stdin", devdir);
    unlink(path);
    symlink("/proc/self/fd/0", path);

    snprintf(path, sizeof(path), "%s/stdout", devdir);
    unlink(path);
    symlink("/proc/self/fd/1", path);

    snprintf(path, sizeof(path), "%s/stderr", devdir);
    unlink(path);
    symlink("/proc/self/fd/2", path);
}

static void fill_vital_devices(const char *devdir)
{
    ensure_dev(devdir, "null", 1, 3, 0666);
    ensure_dev(devdir, "zero", 1, 5, 0666);
    ensure_dev(devdir, "full", 1, 7, 0666);
    ensure_dev(devdir, "random", 1, 8, 0666);
    ensure_dev(devdir, "urandom", 1, 9, 0666);
    ensure_dev(devdir, "tty", 5, 0, 0666);
    ensure_dev(devdir, "console", 5, 1, 0600);
    ensure_dev(devdir, "ptmx", 5, 2, 0666);

    fill_standard_links(devdir);
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

static void setup_pseudo_fs(const char *root)
{
    char path[4096];
    char devdir[4096];

    /* /dev — preserve an existing usable tree */
    snprintf(devdir, sizeof(devdir), "%s/dev", root);
    (void)mkdir(devdir, 0755);
    int dev_usable = is_usable_dev(devdir);

    if (dev_usable)
    {
        fill_vital_devices(devdir);
    }
    else if (mount("devtmpfs", devdir, "devtmpfs", MS_NOSUID, NULL) == 0)
    {
        /* kernel-provided, all good */
    }
    else if (try_mount("tmpfs", devdir, "tmpfs",
                       MS_NOSUID, "mode=0755") == 0)
    {
        fill_vital_devices(devdir);
    }

    snprintf(path, sizeof(path), "%s/proc", root);
    (void)mkdir(path, 0555);
    try_mount("proc", path, "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

    snprintf(path, sizeof(path), "%s/sys", root);
    (void)mkdir(path, 0555);
    try_mount("sysfs", path, "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);

    /* Give the rootfs a private devpts instance. */
    snprintf(path, sizeof(path), "%s/dev/pts", root);
    (void)mkdir(path, 0755);
    if (try_mount("devpts", path, "devpts", MS_NOSUID | MS_NOEXEC,
                  "mode=0620,ptmxmode=0666,newinstance") == 0)
    {
        ensure_dev(path, "ptmx", 5, 2, 0000);
    }

    snprintf(path, sizeof(path), "%s/dev/shm", root);
    (void)mkdir(path, 1777);
    try_mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");

    snprintf(path, sizeof(path), "%s/tmp", root);
    (void)mkdir(path, 1777);
    try_mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");

    snprintf(path, sizeof(path), "%s/run", root);
    (void)mkdir(path, 0755);
    try_mount("tmpfs", path, "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");

    /* Create /run/resolv.conf with default nameservers */
    snprintf(path, sizeof(path), "%s/run/resolv.conf", root);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0)
    {
        const char *resolv = "nameserver 1.1.1.1\nnameserver 8.8.8.8\n";
        ssize_t n = write(fd, resolv, strlen(resolv));
        (void)n; /* ignore short writes for simplicity */
        close(fd);
    }
    else
    {
        fprintf(stderr, "enter: cannot create %s: %s\n",
                path, strerror(errno));
    }

    snprintf(path, sizeof(path), "%s/run/lock", root);
    (void)mkdir(path, 0755);
}

/* ============================================================ */
/* pivot_root                                                    */
/* ============================================================ */

static int do_pivot_root(void)
{
    static const char oldroot[] = ".enter-oldroot";

    if (mkdir(oldroot, 0700) != 0 && errno != EEXIST)
    {
        perror("mkdir oldroot");
        return -1;
    }

    if (syscall(SYS_pivot_root, ".", oldroot) != 0)
    {
        perror("pivot_root");
        (void)rmdir(oldroot);
        return -1;
    }

    if (chdir("/") != 0)
        return -1;

    (void)umount2(oldroot, MNT_DETACH);
    (void)rmdir(oldroot);
    return 0;
}

static int do_switch_root(void)
{
    /*
     * Move our current directory to /.
     *
     * This is useful as a fallback when pivot_root() cannot
     * operate on the current root setup.
     */

    if (mount(".", "/", NULL, MS_MOVE, NULL) != 0)
    {
        fprintf(stderr, "enter: move root: %s\n",
                strerror(errno));
        return -1;
    }

    /*
     * The process still has the old root directory semantics,
     * so explicitly change its root.
     */
    if (chroot(".") != 0)
    {
        fprintf(stderr, "enter: chroot new root: %s\n",
                strerror(errno));
        return -1;
    }

    if (chdir("/") != 0)
    {
        fprintf(stderr, "enter: chdir /: %s\n",
                strerror(errno));
        return -1;
    }

    return 0;
}

/* ============================================================ */
/* Enter the rootfs                                              */
/* ============================================================ */

enum
{
    MODE_FAIL = 0,
    MODE_PIVOT,
    MODE_SHROOT,
    MODE_CHROOT
};

static int enter_rootfs()
{
    if (do_pivot_root() == 0)
    {
        setup_pseudo_fs("");
        return MODE_PIVOT;
    }

    fprintf(stderr, "enter: pivot_root failed, trying switch_root\n");

    if (do_switch_root() == 0)
    {
        setup_pseudo_fs("");
        return MODE_SHROOT;
    }

    fprintf(stderr, "enter: switch_root failed (%s)\n",
            strerror(errno));

    if (chroot(".") == 0 && chdir("/") == 0)
    {
        setup_pseudo_fs("");
        return MODE_CHROOT;
    }

    fprintf(stderr, "enter: chroot failed (%s)\n",
            strerror(errno));

    return MODE_FAIL;
}

/* ============================================================ */
/* Namespaces                                                    */
/* ============================================================ */

static int write_str_file(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, data, strlen(data));
    close(fd);
    return (n == (ssize_t)strlen(data)) ? 0 : -1;
}

/* Check if the current directory (rootfs) is writable */
static int is_rootfs_readonly(void)
{
    char testfile[4096];
    snprintf(testfile, sizeof(testfile), ".enter-write-test-%d", getpid());
    int fd = open(testfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0)
    {
        close(fd);
        unlink(testfile);
        return 0; /* writable */
    }
    /* EROFS or EACCES means read-only; other errors treat as read-only */
    return 1;
}

/*
 * Called only when we are not already root.
 * Creates a user namespace and maps our uid/gid to 0 inside it,
 * so the rest of the launcher can rely on CAP_SYS_ADMIN etc.
 * Returns 0 on success, -1 on failure (caller bails out).
 */
static int setup_userns(void)
{
    uid_t uid = getuid();
    gid_t gid = getgid();
    char buf[64];

    if (unshare(CLONE_NEWUSER) != 0)
    {
        fprintf(stderr, "enter: unshare(CLONE_NEWUSER): %s\n", strerror(errno));
        return -1;
    }

    /* Required before writing gid_map when not privileged in the parent. */
    (void)write_str_file("/proc/self/setgroups", "deny");

    snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)uid);
    if (write_str_file("/proc/self/uid_map", buf) != 0)
    {
        fprintf(stderr, "enter: uid_map: %s\n", strerror(errno));
        return -1;
    }

    snprintf(buf, sizeof(buf), "0 %u 1", (unsigned)gid);
    if (write_str_file("/proc/self/gid_map", buf) != 0)
    {
        fprintf(stderr, "enter: gid_map: %s\n", strerror(errno));
        return -1;
    }

    /* gid first: setresuid(0,0,0) would drop our ability to change gid. */
    if (setresgid(0, 0, 0) != 0 || setresuid(0, 0, 0) != 0)
    {
        fprintf(stderr, "enter: setresuid/setresgid: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static int try_ns(int flag, const char *name)
{
    if (unshare(flag) == 0)
    {
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
    char path[4096];
    ssize_t n;
    char *slash;

    n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0)
    {
        path[n] = '\0';
    }
    else
    {
        if (!argv0 || !strchr(argv0, '/'))
            return -1;
        snprintf(path, sizeof(path), "%s", argv0);
    }

    slash = strrchr(path, '/');
    if (!slash)
        return -1;
    if (slash == path)
        snprintf(buf, sz, "/");
    else
    {
        *slash = '\0';
        snprintf(buf, sz, "%s", path);
    }
    return 0;
}

/* ============================================================ */
/* Init / shell search                                           */
/* ============================================================ */

static const char *const login_candidates[] = {
    "/bin/login", "/usr/bin/login", "/sbin/login", "/usr/sbin/login", NULL};

static const char *find_in_rootfs(const char *root,
                                  const char *const *cands,
                                  char *out, size_t outsz)
{
    size_t i;
    for (i = 0; cands[i]; i++)
    {
        char full[4096];
        snprintf(full, sizeof(full), "%s%s", root, cands[i]);
        if (access(full, X_OK) == 0)
        {
            snprintf(out, outsz, "%s", cands[i]);
            return out;
        }
    }
    return NULL;
}

static int run_if_executable(const char *path)
{
    if (access(path, X_OK) != 0)
        return 1;

    sigset_t chld_mask, old_mask;
    sigemptyset(&chld_mask);
    sigaddset(&chld_mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &chld_mask, &old_mask);

    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork");
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return -1;
    }

    if (pid == 0)
    {
        /* ---- CHILD process ---- */
        sigprocmask(SIG_SETMASK, &old_mask, NULL);

        char *argv[] = {(char *)path, NULL};
        execv(path, argv);

        perror("execv");
        _exit(127);
    }

    /* ---- PARENT process ---- */
    int status;
    if (waitpid(pid, &status, 0) < 0)
    {
        perror("waitpid");
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return -1;
    }
    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    if (WIFEXITED(status))
    {
        int code = WEXITSTATUS(status);
        if (code != 0)
        {
            fprintf(stderr, "%s exited with status %d\n", path, code);
            return -1;
        }
    }
    else if (WIFSIGNALED(status))
    {
        fprintf(stderr, "%s killed by signal %d\n", path, WTERMSIG(status));
        return -1;
    }

    return 0; /* success */
}

static int mount_overlay(void)
{
    /* Make /tmp writable with tmpfs */
    if (mount("tmpfs", "./tmp", "tmpfs",
              MS_NOSUID | MS_NODEV, "mode=1777") != 0)
    {
        fprintf(stderr, "enter: mount tmpfs on /tmp: %s\n",
                strerror(errno));
        return 1;
    }
    if ((mkdir("./tmp/upper", 0755) != 0 && errno != EEXIST) ||
        (mkdir("./tmp/work", 0755) != 0 && errno != EEXIST) ||
        (mkdir("./tmp/overlay", 0755) != 0 && errno != EEXIST))
    {
        perror("mkdir");
        umount2("./tmp", MNT_DETACH);
        return 1;
    }

    char overlay_opts[4096];
    snprintf(overlay_opts, sizeof(overlay_opts),
             "lowerdir=.,upperdir=./tmp/upper,workdir=./tmp/work");
    if (mount("overlay", "./tmp/overlay", "overlay",
              MS_NOSUID | MS_NODEV, overlay_opts) != 0)
    {
        fprintf(stderr, "enter: mount overlay: %s\n",
                strerror(errno));
        umount2("./tmp", MNT_DETACH);
        return 1;
    }
    if (chdir("./tmp/overlay") != 0)
    {
        perror("chdir overlay");
        umount2("./tmp/overlay", MNT_DETACH);
        umount2("./tmp", MNT_DETACH);
        return 1;
    }

    return 0;
}

/* ============================================================ */
/* main                                                          */
/* ============================================================ */

#define DEF_HOSTNAME "buildroot"

int main(int argc, char *argv[])
{
    char rootfs[4096];
    char resolved[4096];
    char usrpath[4096];
    char tmppath[4096];
    char devpath[4096];
    char prog_buf[4096];
    char *auto_argv[4];
    char **child_argv;
    const char *prog;
    struct stat st;
    pid_t child_pid;
    int status;
    int do_init = 0;

    /* find our own directory = candidate rootfs */
    if (get_self_dir(rootfs, sizeof(rootfs), argv[0]) != 0)
    {
        fprintf(stderr, "enter: cannot determine own directory\n");
        return 1;
    }

    if (realpath(rootfs, resolved) == NULL)
    {
        fprintf(stderr, "enter: realpath(%s): %s\n", rootfs, strerror(errno));
        return 1;
    }
    snprintf(rootfs, sizeof(rootfs), "%s", resolved);

    /* sanity check: it must look like a real rootfs */
    snprintf(usrpath, sizeof(usrpath), "%s/usr", rootfs);
    if (stat(usrpath, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "enter: %s is not a rootfs (no /usr)\n", rootfs);
        return 1;
    }

    snprintf(tmppath, sizeof(tmppath), "%s/tmp", rootfs);
    if (stat(tmppath, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        fprintf(stderr, "enter: %s is not a rootfs (no /tmp)\n", rootfs);
        return 1;
    }

    if (chdir(rootfs) != 0)
    {
        fprintf(stderr, "enter: chdir(%s): %s\n",
                rootfs, strerror(errno));
        return 1;
    }

    /* determine program to run */
    if (argc >= 2)
    {
        prog = argv[1];
        child_argv = &argv[1];
    }
    else
    {
        const char *found;

        found = find_in_rootfs(rootfs, login_candidates,
                               prog_buf, sizeof(prog_buf));
        if (!found)
        {
            fprintf(stderr,
                    "enter: no login binary found in %s\n", rootfs);
            return 1;
        }
        do_init = 1;
        prog = found;
        auto_argv[0] = prog_buf;
        auto_argv[1] = "-f"; // auto login
        auto_argv[2] = "root";
        auto_argv[3] = NULL;
        child_argv = auto_argv;
    }

    /* If we're not root, we need a user namespace. Everything after
     * this point (mount ns, pid ns, mount calls, pivot_root, chroot,
     * setuid, sethostname, mknod in /dev) relies on CAP_SYS_ADMIN
     * inside the namespace, which uid 0 in a fresh userns has. */
    if (geteuid() != 0)
    {
        if (setup_userns() != 0)
        {
            fprintf(stderr, "enter: cannot enter user namespace, "
                            "rerun as root or enable unprivileged userns\n");
            return 1;
        }
    }

    /* unshare PID */
    try_ns(CLONE_NEWPID, "pid"); /* fork below activates it */

    /* fork: with CLONE_NEWPID the child becomes PID 1 */
    child_pid = fork();
    if (child_pid < 0)
    {
        perror("fork");
        return 1;
    }

    // Child
    if (child_pid == 0)
    {
        int is_init = getpid() == 1;
        int is_readonly = is_rootfs_readonly();

        /* Rename child process */
        if (is_init)
        {
            prctl(PR_SET_NAME, "[enter: init]", 0, 0, 0);
            char *init_name = strdup("[enter: init]");
            if (init_name)
                argv[0] = init_name;
        }
        else
        {
            prctl(PR_SET_NAME, "[enter: container]", 0, 0, 0);
            char *cont_name = strdup("[enter: container]");
            if (cont_name)
                argv[0] = cont_name;
        }

        struct sigaction sa;
        sa.sa_handler = reap_children;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
        sigaction(SIGCHLD, &sa, NULL);

        if (!try_ns(CLONE_NEWNS, "mount"))
        {
            _exit(1);
        }

        if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        {
            fprintf(stderr,
                    "enter: make / private failed: %s\n",
                    strerror(errno));
            _exit(1);
        }

        int root_mountpoint = 0;
        if (is_readonly)
        {
            root_mountpoint = mount_overlay() == 0;
        }

        if (!root_mountpoint)
        {
            if (mount(".", ".", NULL, MS_BIND, NULL) != 0)
            {
                fprintf(stderr, "enter: bind self: %s\n",
                        strerror(errno));
                _exit(1);
            }
            if (chdir(".") != 0)
            {
                perror("chdir self");
                _exit(1);
            }
        }

        if (try_ns(CLONE_NEWUTS, "uts"))
        {
            if (sethostname(DEF_HOSTNAME, strlen(DEF_HOSTNAME)) != 0)
                perror("sethostname");
        }

        try_ns(CLONE_NEWIPC, "ipc");
        // try_ns(CLONE_NEWNET, "net");

        int mode = enter_rootfs();

        if (mode == MODE_FAIL)
        {
            fprintf(stderr, "enter: cannot enter %s\n", rootfs);
            _exit(126);
        }

        /* sanitise env */
        sanitize_env();

        int do_deinit = 0;
        if (do_init && run_if_executable("/etc/init.d/rcS") != 1)
        {
            do_deinit = 1;
        }

        pid_t launcher_pid = fork();

        if (launcher_pid < 0)
        {
            perror("fork");
            _exit(1);
        }

        if (launcher_pid == 0)
        {
            if (strchr(prog, '/'))
                execve(prog, child_argv, environ);
            else
                execvp(prog, child_argv);

            fprintf(stderr, "enter: execve %s: %s\n",
                    prog, strerror(errno));
            _exit(127);
        }

        /* Block SIGCHLD while we install the wanted pid, so the handler
         * cannot reap launcher_pid before we record its status. */
        sigset_t chld_mask, old_mask;
        sigemptyset(&chld_mask);
        sigaddset(&chld_mask, SIGCHLD);
        sigprocmask(SIG_BLOCK, &chld_mask, &old_mask);

        g_wanted_pid = launcher_pid;
        g_wanted_reaped = 0;

        while (!g_wanted_reaped)
            sigsuspend(&old_mask); /* atomically unblocks CHLD + waits */

        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        status = g_wanted_status;

        // We must cleanup before exit
        if (do_deinit)
        {
            run_if_executable("/etc/init.d/rcK");
        }

        if (WIFEXITED(status))
            _exit(WEXITSTATUS(status));
        if (WIFSIGNALED(status))
            _exit(128 + WTERMSIG(status));
        _exit(1);
    }

    // Parent
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    /* Rename parent process */
    prctl(PR_SET_NAME, "[enter: host]", 0, 0, 0);
    char *host_name = strdup("[enter: host]");
    if (host_name)
        argv[0] = host_name;

    for (;;)
    {
        if (waitpid(child_pid, &status, 0) >= 0)
            break;

        if (errno == EINTR)
            continue;

        perror("waitpid");
        return 1;
    }

    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}
