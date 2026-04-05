#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include "helpa.h"

#define CACHE_FILE "tmenu_path"

typedef struct { char *dt; u64 sz; u64 cp; } StrDA;  /* DA of char*  */
typedef struct { char  dt; u64 sz; u64 cp; } CharDA;  /* unused alias */

/* DA of (char *) for the binary name list */
typedef struct { char **dt; u64 sz; u64 cp; } NameDA;

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static int is_executable(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) &&
           (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH));
}

static void build_cache(const char *cache_path)
{
    const char *path_env = getenv("PATH");
    if (!path_env) return;

    NameDA names = HELPA_DA_INIT;

    char *path_copy = strdup(path_env);
    char *dir       = strtok(path_copy, ":");

    while (dir) {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                if (ent->d_name[0] == '.') continue;

                /* build full path with HStr */
                HStr full = {0};
                hstr_printf(&full, "%s/%s", dir, ent->d_name);

                if (is_executable((char *)full.dt)) {
                    int dup = 0;
                    helpa_da_for(names, i)
                        if (strcmp(names.dt[i], ent->d_name) == 0) { dup = 1; break; }

                    if (!dup)
                        helpa_da_append(names, strdup(ent->d_name));
                }

                hstr_free(&full);
            }
            closedir(d);
        }
        dir = strtok(NULL, ":");
    }
    free(path_copy);

    qsort(names.dt, names.sz, sizeof(char *), cmp_str);

    FILE *f = fopen(cache_path, "w");
    if (f) {
        helpa_da_for(names, i)
            fprintf(f, "%s\n", names.dt[i]);
        fclose(f);
    }

    helpa_da_for(names, i) free(names.dt[i]);
    helpa_da_free(names);
}

static const char *CLI_WHITELIST[] = {
    "vim", "nvim", "nano", "ranger",
    "htop", "top", "alsamixer", "ncdu", "btop", "pipemixer",
    NULL
};

static int in_cli_whitelist(const char *name)
{
    for (int i = 0; CLI_WHITELIST[i]; i++)
        if (strcmp(name, CLI_WHITELIST[i]) == 0) return 1;
    return 0;
}

int main(void)
{
    HStr cache_path = {0};
    const char *xdg = getenv("XDG_CACHE_HOME");
    hstr_printf(&cache_path, "%s/%s", xdg ? xdg : "/tmp", CACHE_FILE);

    if (access((char *)cache_path.dt, F_OK) != 0)
        build_cache((char *)cache_path.dt);

    FILE *cache_f = fopen((char *)cache_path.dt, "r");
    if (!cache_f) {
        perror("fopen cache");
        hstr_free(&cache_path);
        return 1;
    }
    hstr_free(&cache_path);

    int pipefd[2];
    if (pipe(pipefd) != 0) { perror("pipe"); return 1; }

    pid_t writer = fork();
    if (writer == 0) {
        close(pipefd[0]);
        HStr buf = {0};
        helpa_da_reserve(buf, 4096);
        char tmp[4096];
        size_t n;
        while ((n = fread(tmp, 1, sizeof(tmp), cache_f)) > 0)
            if (!write(pipefd[1], tmp, n)) break;
        hstr_free(&buf);
        close(pipefd[1]);
        _exit(0);
    }
    close(pipefd[1]);
    fclose(cache_f);

    /* pipe: tmenu stdout → us */
    int out_pipe[2];
    if (pipe(out_pipe) != 0) { perror("pipe"); return 1; }

    pid_t tmenu_pid = fork();
    if (tmenu_pid == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(out_pipe[0]);
        close(out_pipe[1]);
        execlp("tmenu", "tmenu", (char *)NULL);
        fprintf(stderr, "Error: tmenu was not found in PATH.\n");
        _exit(1);
    }
    close(pipefd[0]);
    close(out_pipe[1]);

    HStr prog = {0};
    {
        char tmp[256];
        ssize_t n;
        while ((n = read(out_pipe[0], tmp, sizeof(tmp))) > 0)
            hstr_append_view(&prog, ((HStrView){ (u8 *)tmp, (u64)n }));
    }
    close(out_pipe[0]);

    int status;
    waitpid(writer,    &status, 0);
    waitpid(tmenu_pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        hstr_free(&prog);
        return 1;
    }

    /* trim trailing newline/CR */
    HStrView pv = hstrv_trim(hstr_view(&prog));
    /* make a NUL-terminated copy for exec */
    HStr prog_trimmed = {0};
    helpa_da_reserve(prog_trimmed, 1024);
    hstr_append_view(&prog_trimmed, pv);

    hstr_free(&prog);

    if (prog_trimmed.sz == 0) {
        hstr_free(&prog_trimmed);
        return 0;
    }

    HStrView pv2    = hstr_view(&prog_trimmed);
    const u8 *slash = NULL;
    for (u64 i = 0; i < pv2.sz; i++)
        if (pv2.dt[i] == '/') slash = pv2.dt + i;
    HStrView app_name_v = slash
        ? ((HStrView){ slash + 1, pv2.sz - (u64)(slash + 1 - pv2.dt) })
        : pv2;

    HStr app_name = {0};
    hstr_append_view(&app_name, app_name_v);

    if (in_cli_whitelist((char *)app_name.dt)) {
        pid_t pid = fork();
        if (pid == 0) {
            execlp((char *)prog_trimmed.dt,
                   (char *)prog_trimmed.dt, (char *)NULL);
            _exit(1);
        }
        waitpid(pid, NULL, 0);
    } else {
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            pid_t pid2 = fork();
            if (pid2 == 0) {
                close(STDIN_FILENO);
                close(STDOUT_FILENO);
                close(STDERR_FILENO);
                execlp((char *)prog_trimmed.dt,
                       (char *)prog_trimmed.dt, (char *)NULL);
                _exit(1);
            }
            _exit(0);
        }
        waitpid(pid, NULL, 0);
        usleep(100000);
    }

    hstr_free(&prog_trimmed);
    hstr_free(&app_name);
    return 0;
}
