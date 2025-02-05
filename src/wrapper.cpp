/*
  This file is part of the KDE libraries
  Copyright (c) 1999 Waldo Bastian <bastian@kde.org>
            (c) 1999 Mario Weilguni <mweilguni@sime.com>
            (c) 2001 Lubos Lunak <l.lunak@kde.org>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Library General Public
  License version 2 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Library General Public License for more details.

  You should have received a copy of the GNU Library General Public License
  along with this library; see the file COPYING.LIB.  If not, write to
  the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301, USA.
*/

#include "klauncher_cmds.h"

#include "config-kdeinit.h"

#include <qstandardpaths.h>
#include <qfile.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <signal.h>

extern char **environ;

// copied from kdeinit/kinit.cpp
// Can't use QGuiApplication::platformName() here, there is no app instance.
#if HAVE_X11 || HAVE_XCB
static const char* displayEnvVarName_c()
{
    return "DISPLAY";
}
#endif

// adapted from kdeinit/kinit.cpp
// WARNING, if you change the socket name, adjust kinit.cpp too
static const QString generate_socket_file_name()
{

#if HAVE_X11 || HAVE_XCB // qt5: see displayEnvVarName_c()
    QByteArray display = qgetenv(displayEnvVarName_c());
    if (display.isEmpty()) {
        fprintf(stderr, "Error: could not determine $%s.\n", displayEnvVarName_c());
        return QString();
    }
    int i;
    if ((i = display.lastIndexOf('.')) > display.lastIndexOf(':') && i >= 0) {
        display.truncate(i);
    }

    display.replace(':', '_');
#ifdef __APPLE__
    // not entirely impossible, so let's leave it
    display.replace('/', '_');
#endif
#else
    // not using a DISPLAY variable; use an empty string instead
    QByteArray display = "";
#endif
    // WARNING, if you change the socket name, adjust kwrapper too
    const QString socketFileName = QStringLiteral("kdeinit5_%1").arg(QLatin1String(display));
    return socketFileName;
}

/*
 * Write 'len' bytes from 'buffer' into 'sock'.
 * returns 0 on success, -1 on failure.
 */
static int write_socket(int sock, char *buffer, int len)
{
    ssize_t result;
    int bytes_left = len;
    while (bytes_left > 0) {
        result = write(sock, buffer, bytes_left);
        if (result > 0) {
            buffer += result;
            bytes_left -= result;
        } else if (result == 0) {
            return -1;
        } else if ((result == -1) && (errno != EINTR) && (errno != EAGAIN)) {
            return -1;
        }
    }
    return 0;
}

/*
 * Read 'len' bytes from 'sock' into 'buffer'.
 * returns 0 on success, -1 on failure.
 */
static int read_socket(int sock, char *buffer, int len)
{
    ssize_t result;
    int bytes_left = len;
    while (bytes_left > 0) {
        result = read(sock, buffer, bytes_left);
        if (result > 0) {
            buffer += result;
            bytes_left -= result;
        } else if (result == 0) {
            return -1;
        } else if ((result == -1) && (errno != EINTR) && (errno != EAGAIN)) {
            return -1;
        }
    }
    return 0;
}

static int openSocket()
{
    const QString socketFileName = generate_socket_file_name();
    if (socketFileName.isEmpty()) {
        return -1;
    }
    QByteArray socketName = QFile::encodeName(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
                            QLatin1Char('/') + socketFileName);
    const char *sock_file = socketName.constData();

    struct sockaddr_un server;
    if (strlen(sock_file) >= sizeof(server.sun_path)) {
        fprintf(stderr, "Warning: Path of socketfile exceeds UNIX_PATH_MAX.\n");
        return -1;
    }

    /*
     * create the socket stream
     */
    int s = socket(PF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        perror("Warning: socket() failed: ");
        return -1;
    }

    server.sun_family = AF_UNIX;
    strcpy(server.sun_path, sock_file);
    kde_socklen_t socklen = sizeof(server);
    if (connect(s, (struct sockaddr *)&server, socklen) == -1) {
        fprintf(stderr, "kdeinit5_wrapper: Warning: connect(%s) failed:", sock_file);
        perror(" ");
        close(s);
        return -1;
    }
    return s;
}

static pid_t kwrapper_pid;

static void sig_pass_handler(int signo);
static void setup_signals(void);

static void setup_signal_handler(int signo, int clean)
{
    struct sigaction sa;
    if (clean) {
        sa.sa_handler = SIG_DFL;
    } else {
        sa.sa_handler = sig_pass_handler;
    }
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, signo);
    sa.sa_flags = 0; /* don't use SA_RESTART */
    sigaction(signo, &sa, nullptr);
}

static void sig_pass_handler(int signo)
{
    int save_errno = errno;
    if (signo == SIGTSTP) {
        kill(kwrapper_pid, SIGSTOP);    /* pass the signal to the real process */
    } else {                           /* SIGTSTP wouldn't work ... I don't think is much */
        kill(kwrapper_pid, signo);    /* of a problem */
    }

    if (signo == SIGCONT) {
        setup_signals();    /* restore signals */
    } else if (signo == SIGCHLD)
        ; /* nothing, ignore */
    else { /* do the default action ( most of them quit the app ) */
        setup_signal_handler(signo, 1);
        raise(signo);   /* handle the signal again */
    }

    errno = save_errno;
}

static void setup_signals()
{
    setup_signal_handler(SIGHUP, 0);
    setup_signal_handler(SIGINT, 0);
    setup_signal_handler(SIGQUIT, 0);
    setup_signal_handler(SIGILL, 0);    /* e.g. this one is probably doesn't make sense to pass */
    setup_signal_handler(SIGABRT, 0);   /* but anyway ... */
    setup_signal_handler(SIGFPE, 0);
    /*   SIGKILL   can't be handled :( */
    setup_signal_handler(SIGSEGV, 0);
    setup_signal_handler(SIGPIPE, 0);
    setup_signal_handler(SIGALRM, 0);
    setup_signal_handler(SIGTERM, 0);
    setup_signal_handler(SIGUSR1, 0);
    setup_signal_handler(SIGUSR2, 0);
    setup_signal_handler(SIGCHLD, 0);   /* is this a good idea ??? */
    setup_signal_handler(SIGCONT, 0);   /* SIGSTOP can't be handled, but SIGTSTP and SIGCONT can */
    /* SIGSTOP */                       /* which should be enough */
    setup_signal_handler(SIGTSTP, 0);
    setup_signal_handler(SIGTTIN, 0);   /* is this a good idea ??? */
    setup_signal_handler(SIGTTOU, 0);   /* is this a good idea ??? */
    /* some more ? */
}

static int kwrapper_run(pid_t wrapped, int sock)
{
    klauncher_header header;
    char *buffer;
    long pid, status;

    kwrapper_pid = wrapped;
    setup_signals();

    read_socket(sock, (char *)&header, sizeof(header));

    if (header.cmd != LAUNCHER_CHILD_DIED) {
        fprintf(stderr, "Unexpected response from KInit (response = %ld).\n", header.cmd);
        exit(255);
    }

    buffer = (char *) malloc(header.arg_length);
    if (buffer == nullptr) {
        perror("Error: malloc() failed\n");
        exit(255);
    }

    read_socket(sock, buffer, header.arg_length);
    pid = ((long *) buffer)[0];
    if (pid !=  kwrapper_pid) {
        fprintf(stderr, "Unexpected LAUNCHER_CHILD_DIED from KInit - pid = %ld\n", pid);
        exit(255);
    }

    status = ((long *) buffer)[1];
    free(buffer);
    return (int) status;
}

int main(int argc, char **argv)
{
    int i;
    int wrapper = 0;
    int ext_wrapper = 0;
    int kwrapper = 0;
    long arg_count;
    long env_count;
    klauncher_header header;
    char *start, *p, *buffer;
    char cwd[8192];
    const char *tty = nullptr;
    long avoid_loops = 0;
    const char *startup_id = nullptr;
    int sock;

    long size = 0;

    start = argv[0];
    p = start + strlen(argv[0]);
    while (--p > start) {
        if (*p == '/') {
            break;
        }
    }
    if (p > start) {
        p++;
    }
    start = p;

    if (strcmp(start, "kdeinit5_wrapper") == 0) {
        wrapper = 1;
    } else if (strcmp(start, "kshell5") == 0) {
        ext_wrapper = 1;
    } else if (strcmp(start, "kwrapper5") == 0) {
        kwrapper = 1;
    } else if (strcmp(start, "kdeinit5_shutdown") == 0) {
        if (argc > 1) {
            fprintf(stderr, "Usage: %s\n\n", start);
            fprintf(stderr, "Shuts down kdeinit5 master process and terminates all processes spawned from it.\n");
            exit(255);
        }
        sock = openSocket();
        if (sock < 0) {
            fprintf(stderr, "Error: Can not contact kdeinit5!\n");
            exit(255);
        }
        header.cmd = LAUNCHER_TERMINATE_KDE;
        header.arg_length = 0;
        write_socket(sock, (char *) &header, sizeof(header));
        read_socket(sock, (char *) &header, 1); /* wait for the socket to close */
        return 0;
    }

    if (wrapper || ext_wrapper || kwrapper) {
        argv++;
        argc--;
        if (argc < 1) {
            fprintf(stderr, "Usage: %s <application> [<args>]\n", start);
            exit(255); /* usage should be documented somewhere ... */
        }
        start = argv[0];
    }

    sock = openSocket();
    if (sock < 0) { /* couldn't contact kdeinit5, start argv[ 0 ] directly */
        execvp(argv[ 0 ], argv);
        fprintf(stderr, "Error: Can not run %s !\n", argv[ 0 ]);
        exit(255);
    }

    if (!wrapper && !ext_wrapper && !kwrapper) {
        /* was called as a symlink */
        avoid_loops = 1;
#if defined(WE_ARE_KWRAPPER)
        kwrapper = 1;
#elif defined(WE_ARE_KSHELL)
        ext_wrapper = 1;
#else
        wrapper = 1;
#endif
    }

    arg_count = argc;
    env_count = 0;

    size += sizeof(long); /* Number of arguments*/

    size += strlen(start) + 1; /* Size of first argument. */

    for (i = 1; i < argc; i++) {
        size += strlen(argv[i]) + 1;
    }
    if (wrapper) {
        size += sizeof(long); /* empty envs */
    }
    if (ext_wrapper || kwrapper) {
        if (!getcwd(cwd, 8192)) {
            cwd[0] = '\0';
        }
        size += strlen(cwd) + 1;

        size += sizeof(long); /* Number of env.vars. */

        for (; environ[env_count]; env_count++) {
            int l = strlen(environ[env_count]) + 1;
            size += l;
        }

        if (kwrapper) {
            tty = ttyname(1);
            if (!tty || !isatty(2)) {
                tty = "";
            }
            size += strlen(tty) + 1;
        }
    }

    size += sizeof(avoid_loops);

    if (!wrapper) {
        startup_id = getenv("DESKTOP_STARTUP_ID");
        if (startup_id == nullptr) {
            startup_id = "";
        }
        size += strlen(startup_id) + 1;
    }

    if (wrapper) {
        header.cmd = LAUNCHER_EXEC_NEW;
    } else if (kwrapper) {
        header.cmd = LAUNCHER_KWRAPPER;
    } else {
        header.cmd = LAUNCHER_SHELL;
    }
    header.arg_length = size;
    write_socket(sock, (char *) &header, sizeof(header));

    buffer = (char *) malloc(size);
    if (buffer == nullptr) {
        perror("Error: malloc() failed.");
        exit(255);
    }
    p = buffer;

    memcpy(p, &arg_count, sizeof(arg_count));
    p += sizeof(arg_count);

    memcpy(p, start, strlen(start) + 1);
    p += strlen(start) + 1;

    for (i = 1; i < argc; i++) {
        memcpy(p, argv[i], strlen(argv[i]) + 1);
        p += strlen(argv[i]) + 1;
    }

    if (wrapper) {
        long dummy = 0;
        memcpy(p, &dummy, sizeof(dummy)); /* empty envc */
        p += sizeof(dummy);
    }
    if (ext_wrapper || kwrapper) {
        memcpy(p, cwd, strlen(cwd) + 1);
        p += strlen(cwd) + 1;

        memcpy(p, &env_count, sizeof(env_count));
        p += sizeof(env_count);

        for (i = 0; i < env_count; i++) {
            int l = strlen(environ[i]) + 1;
            memcpy(p, environ[i], l);
            p += l;
        }

        if (kwrapper) {
            memcpy(p, tty, strlen(tty) + 1);
            p += strlen(tty) + 1;
        }
    }

    memcpy(p, &avoid_loops, sizeof(avoid_loops));
    p += sizeof(avoid_loops);

    if (!wrapper) {
        memcpy(p, startup_id, strlen(startup_id) + 1);
        p += strlen(startup_id) + 1;
    }

    if (p - buffer != size)  /* should fail only if you change this source and do */
        /* a stupid mistake, it should be assert() actually */
    {
        fprintf(stderr, "Oops. Invalid format.\n");
        exit(255);
    }

    write_socket(sock, buffer, size);
    free(buffer);

    if (read_socket(sock, (char *) &header, sizeof(header)) == -1) {
        fprintf(stderr, "Communication error with KInit.\n");
        exit(255);
    }

    if (header.cmd == LAUNCHER_OK) {
        long pid;
        buffer = (char *) malloc(header.arg_length);
        if (buffer == nullptr) {
            perror("Error: malloc() failed\n");
            exit(255);
        }
        read_socket(sock, buffer, header.arg_length);
        pid = *((long *) buffer);
        if (!kwrapper) { /* kwrapper shouldn't print any output */
            printf("Launched ok, pid = %ld\n", pid);
        } else {
            exit(kwrapper_run(pid, sock));
        }
    } else if (header.cmd == LAUNCHER_ERROR) {
        fprintf(stderr, "KInit could not launch '%s'.\n", start);
        exit(255);
    } else {
        fprintf(stderr, "Unexpected response from KInit (response = %ld).\n", header.cmd);
        exit(255);
    }
    exit(0);
}
