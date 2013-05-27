/*
** Copyright 2010, Adam Shanks (@ChainsDD)
** Copyright 2008, Zinx Verituse (@zinxv)
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#define LOG_TAG "lewa_su"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <endian.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdint.h>
#include <pwd.h>

#include <private/android_filesystem_config.h>
#include <cutils/log.h>

#include <sqlite3.h>

#include "su.h"

//extern char* _mktemp(char*); /* mktemp doesn't link right.  Don't ask me why. */

static struct su_initiator su_from = {
    .pid = -1,
    .uid = 0,
    .bin = "",
    .args = "",
};

static struct su_request su_to = {
    .uid = AID_ROOT,
    .command = DEFAULT_COMMAND,
};

static int from_init(struct su_initiator *from)
{
    char path[PATH_MAX], exe[PATH_MAX];
    char args[4096], *argv0, *argv_rest;
    int fd;
    ssize_t len;
    int i;

    from->uid = getuid();
    from->pid = getppid();

    /* Get the command line */
    snprintf(path, sizeof(path), "/proc/%u/cmdline", from->pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        PLOGE("Opening command line");
        return -1;
    }
    len = read(fd, args, sizeof(args));
    close(fd);
    if (len < 0 || len == sizeof(args)) {
        PLOGE("Reading command line");
        return -1;
    }

    argv0 = args;
    argv_rest = NULL;
    for (i = 0; i < len; i++) {
        if (args[i] == '\0') {
            if (!argv_rest) {
                argv_rest = &args[i+1];
            } else {
                args[i] = ' ';
            }
        }
    }
    args[len] = '\0';

    if (argv_rest) {
        strncpy(from->args, argv_rest, sizeof(from->args));
        from->args[sizeof(from->args)-1] = '\0';
    } else {
        from->args[0] = '\0';
    }

    /* If this isn't app_process, use the real path instead of argv[0] */
    snprintf(path, sizeof(path), "/proc/%u/exe", from->pid);
    len = readlink(path, exe, sizeof(exe));
    if (len < 0) {
        PLOGE("Getting exe path");
        return -1;
    }
    exe[len] = '\0';
    if (strcmp(exe, "/system/bin/app_process")) {
        argv0 = exe;
    }

    strncpy(from->bin, argv0, sizeof(from->bin));
    from->bin[sizeof(from->bin)-1] = '\0';

    return 0;
}



static void usage(void)
{
    printf("Usage: su0 [options] [LOGIN]\n\n");
    printf("Options:\n");
    printf("  -c, --command COMMAND         pass COMMAND to the invoked shell\n");
    printf("  -h, --help                    display this help message and exit\n");
    printf("  -, -l, --login                make the shell a login shell\n");
    // I'll look more into this to figure out what it's about,
    // maybe implement it later
//    printf("  -m, -p,\n");
//    printf("  --preserve-environment        do not reset environment variables, and\n");
//    printf("                                keep the same shell\n");
    printf("  -s, --shell SHELL             use SHELL instead of the default in passwd\n");
    printf("  -v, --version                 display version number and exit\n");
    printf("  -V                            display version code and exit. this is\n");
    printf("                                used almost exclusively by Superuser.apk\n");
    exit(EXIT_SUCCESS);
}

static void deny(void)
{
    struct su_initiator *from = &su_from;
    struct su_request *to = &su_to;

    //send_intent(&su_from, &su_to, "", 0, 1);
    LOGW("request rejected (%u->%u %s)", from->uid, to->uid, to->command);
    fprintf(stderr, "%s\n", strerror(EACCES));
    exit(EXIT_FAILURE);
}

static void allow(char *shell,int ignore)
{
    struct su_initiator *from = &su_from;
    struct su_request *to = &su_to;
    char *exe = NULL;

    //send_intent(&su_from, &su_to, "", 1, 1);

    if (!strcmp(shell, "")) {
        strcpy(shell , "/system/bin/sh");
    }
    exe = strrchr (shell, '/') + 1;
    setgroups(0, NULL);
    setresgid(to->uid, to->uid, to->uid);
    setresuid(to->uid, to->uid, to->uid);
    LOGD("%u %s executing %u %s using shell %s : %s", from->uid, from->bin,
            to->uid, to->command, shell, exe);
    if (strcmp(to->command, DEFAULT_COMMAND)) {
        execl(shell, exe, "-c", to->command, (char*)NULL);
    } else {
        execl(shell, exe, "-", (char*)NULL);
    }
    PLOGE("exec");
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[])
{
    struct stat st;
    static int socket_serv_fd = -1;
    char buf[64], shell[PATH_MAX], *result;
    int i, dballow;
    unsigned req_uid;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") || !strcmp(argv[i], "--command")) {
            if (++i < argc) {
                su_to.command = argv[i];
            } else {
                usage();
            }
        } else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--shell")) {
            if (++i < argc) {
                strcpy(shell, argv[i]);
            } else {
                usage();
            }
        } else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            printf("%s\n", VERSION);
            exit(EXIT_SUCCESS);
        } else if (!strcmp(argv[i], "-V")) {
            printf("%d\n", VERSION_CODE);
            exit(EXIT_SUCCESS);
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
        } else if (!strcmp(argv[i], "-") || !strcmp(argv[i], "-l") ||
                !strcmp(argv[i], "--login")) {
            ++i;
            break;
        } else {
            break;
        }
    }
    if (i < argc-1) {
        usage();
    }
    if (i == argc-1) {
        struct passwd *pw;
        pw = getpwnam(argv[i]);
        if (!pw) {
            su_to.uid = atoi(argv[i]);
        } else {
            su_to.uid = pw->pw_uid;
        }
    }

    if (from_init(&su_from) < 0) {
        deny();
    }
   
    if (su_from.uid == AID_ROOT || su_from.uid == AID_SHELL 
    || su_from.uid == AID_SYSTEM || su_from.uid == AID_RADIO){
        allow(shell,1);
    }
    
    LOGD("%u %s executing su0", su_from.uid, su_from.bin);
    if (NULL != strstr(su_from.bin, "com.lewa")) {
        allow(shell,1);
    }

    if (st.st_gid != st.st_uid)
    {
        LOGE("Bad uid/gid %d/%d for Superuser Requestor application",
                (int)st.st_uid, (int)st.st_gid);
        deny();
    }

    deny();
    return -1;
}
