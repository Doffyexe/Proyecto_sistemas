// gtesh_semana5.c — Semana 5: '&' en paralelo + redirección '>' + batch + cd/path/exit
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/stat.h>

static const char ERROR_MSG[] = "An error has occurred\n";

static void print_error(void) {
    (void)write(STDERR_FILENO, ERROR_MSG, sizeof(ERROR_MSG) - 1);
}

// ---------------- utils de texto ----------------
static char *trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len-1])) s[--len] = '\0';
    return s;
}

// tokeniza por espacio/tab y devuelve argv[] NULL-terminado
static char **tokenize(char *line, int *argc_out) {
    const char *delim = " \t";
    int cap = 8, argc = 0;
    char **argv = malloc(sizeof(char *) * cap);
    if (!argv) return NULL;

    char *token = NULL;
    while ((token = strsep(&line, delim)) != NULL) {
        token = trim(token);
        if (*token == '\0') continue;
        if (argc == cap) {
            cap *= 2;
            char **tmp = realloc(argv, sizeof(char *) * cap);
            if (!tmp) { for (int i = 0; i < argc; i++) free(argv[i]); free(argv); return NULL; }
            argv = tmp;
        }
        argv[argc] = strdup(token);
        if (!argv[argc]) { for (int i = 0; i < argc; i++) free(argv[i]); free(argv); return NULL; }
        argc++;
    }
    argv[argc] = NULL;
    if (argc_out) *argc_out = argc;
    return argv;
}

static void free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

// inserta espacios alrededor de '>' para tokenizarlo aunque esté pegado
static char *expand_redirection(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(3*n + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '>') { out[j++]=' '; out[j++]='>'; out[j++]=' '; }
        else out[j++] = s[i];
    }
    out[j] = '\0';
    return out;
}

// divide una línea en subcomandos separados por '&'
static char **split_by_ampersand(char *line, int *count_out) {
    // permitimos cosas como "cmd1 &  cmd2  &cmd3"
    int cap = 4, count = 0;
    char **parts = malloc(sizeof(char*) * cap);
    if (!parts) return NULL;

    char *p = line;
    char *seg;
    while ((seg = strsep(&p, "&")) != NULL) {
        seg = trim(seg);
        if (*seg == '\0') {
            // segmento vacío (p.ej., "cmd1 && cmd2" o "& cmd")
            // lo trataremos como error luego si no hay ningún comando válido
            continue;
        }
        if (count == cap) {
            cap *= 2;
            char **tmp = realloc(parts, sizeof(char*) * cap);
            if (!tmp) { free(parts); return NULL; }
            parts = tmp;
        }
        parts[count++] = strdup(seg);
        if (!parts[count-1]) { for (int i=0;i<count-1;i++) free(parts[i]); free(parts); return NULL; }
    }
    if (count_out) *count_out = count;
    parts = realloc(parts, sizeof(char*) * (count>0 ? count : 1));
    return parts;
}

static void free_parts(char **parts, int count) {
    if (!parts) return;
    for (int i=0;i<count;i++) free(parts[i]);
    free(parts);
}

// ---------------- PATH interno ----------------
static char *path_dirs[64];
static size_t path_count = 0;

static void clear_path(void) {
    for (size_t i = 0; i < path_count; i++) { free(path_dirs[i]); path_dirs[i] = NULL; }
    path_count = 0;
}

static int set_path_from_args(int argc_cmd, char **argv_cmd) {
    clear_path();
    for (int i = 1; i < argc_cmd; i++) {
        if (path_count >= (sizeof(path_dirs)/sizeof(path_dirs[0]))) { print_error(); clear_path(); return -1; }
        char *copy = strdup(argv_cmd[i]);
        if (!copy) { print_error(); clear_path(); return -1; }
        path_dirs[path_count++] = copy;
    }
    return 0;
}

static char *find_executable(const char *cmd) {
    if (!cmd || !*cmd) return NULL;
    if (strchr(cmd, '/')) {
        if (access(cmd, X_OK) == 0) return strdup(cmd);
        else return NULL;
    }
    for (size_t i = 0; i < path_count; i++) {
        const char *dir = path_dirs[i];
        size_t len = strlen(dir) + strlen(cmd) + 2;
        char *full = malloc(len);
        if (!full) return NULL;
        snprintf(full, len, "%s/%s", dir, cmd);
        if (access(full, X_OK) == 0) return full;
        free(full);
    }
    return NULL;
}

// ---------------- Redirección '>' ----------------
// valida y “corta” argv si hay '>' → deja argv solo con el comando y argumentos;
// out_file recibe el archivo destino. Devuelve 0 OK, -1 error de forma.
static int parse_redirection(char **argv_cmd, int *argc_cmd, char **out_file) {
    int gt_pos = -1;
    for (int i = 0; i < *argc_cmd; i++) {
        if (strcmp(argv_cmd[i], ">") == 0) {
            if (gt_pos != -1) return -1; // más de un '>'
            gt_pos = i;
        }
    }
    if (gt_pos == -1) { *out_file = NULL; return 0; } // no hay redirección

    if (gt_pos == 0) return -1;                         // '>' no puede ir antes del comando
    if (gt_pos + 1 >= *argc_cmd) return -1;             // falta archivo
    if (gt_pos + 2 != *argc_cmd) return -1;             // sobran tokens después del archivo

    *out_file = argv_cmd[gt_pos + 1];
    argv_cmd[gt_pos] = NULL;                            // “corta” argv para execv
    *argc_cmd = gt_pos;
    return 0;
}

// ejecuta un subcomando (con posible '>').
// Devuelve: pid del hijo si lanzó proceso externo, 0 si fue built-in manejado en el padre, -1 si error previo.
static pid_t run_one_command(char *subcmd) {
    // expandir '>' pegado (ls>out.txt)
    char *expanded = expand_redirection(subcmd);
    if (!expanded) { print_error(); return -1; }

    int argc_cmd = 0;
    char **argv_cmd = tokenize(expanded, &argc_cmd);
    free(expanded);
    if (!argv_cmd || argc_cmd == 0) { free_argv(argv_cmd); print_error(); return -1; }

    // built-in: exit (solo válido si es el único en la línea en esta implementación)
    if (strcmp(argv_cmd[0], "exit") == 0) {
        if (argc_cmd != 1) { print_error(); free_argv(argv_cmd); return -1; }
        // señalamos al caller que debe terminar el shell inmediatamente:
        free_argv(argv_cmd);
        // usamos _exit(0) aquí? no, dejamos que el caller decida. Devolvemos pid= -2 para indicar EXIT.
        return -2;
    }

    // built-in: cd
    if (strcmp(argv_cmd[0], "cd") == 0) {
        if (argc_cmd != 2) { print_error(); free_argv(argv_cmd); return 0; }
        if (chdir(argv_cmd[1]) != 0) print_error();
        free_argv(argv_cmd);
        return 0;
    }

    // built-in: path
    if (strcmp(argv_cmd[0], "path") == 0) {
        (void)set_path_from_args(argc_cmd, argv_cmd);
        free_argv(argv_cmd);
        return 0;
    }

    // comandos externos con posible redirección
    char *out_file = NULL;
    int visible_argc = argc_cmd;
    if (parse_redirection(argv_cmd, &visible_argc, &out_file) != 0) {
        print_error();
        free_argv(argv_cmd);
        return -1;
    }

    char *exe = find_executable(argv_cmd[0]);
    if (!exe) {
        print_error();
        free_argv(argv_cmd);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        print_error();
        free(exe);
        free_argv(argv_cmd);
        return -1;
    } else if (pid == 0) {
        if (out_file) {
            int fd = open(out_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0) { print_error(); _exit(1); }
            if (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
                print_error(); close(fd); _exit(1);
            }
            close(fd);
        }
        execv(exe, argv_cmd);      // argv_cmd ya “cortado” si había '>'
        print_error();
        _exit(1);
    }

    // padre
    free(exe);
    free_argv(argv_cmd);
    return pid;
}

// ---------------- main ----------------
int main(int argc, char *argv[]) {
    // Interactivo (sin args) o batch (1 arg). >1 args → error
    FILE *in = NULL;
    int batch_mode = 0;

    if (argc == 1) {
        in = stdin; batch_mode = 0;
    } else if (argc == 2) {
        in = fopen(argv[1], "r");
        if (!in) { print_error(); exit(1); }
        batch_mode = 1;
    } else {
        print_error();
        exit(1);
    }

    // PATH inicial
    path_dirs[path_count++] = strdup("/bin");
    if (!path_dirs[0]) { print_error(); if (in && in != stdin) fclose(in); exit(1); }

    if (!batch_mode) setvbuf(stdout, NULL, _IONBF, 0);

    char *line = NULL;
    size_t cap = 0;

    for (;;) {
        if (!batch_mode) printf("gtesh> ");

        ssize_t nread = getline(&line, &cap, in);
        if (nread == -1) break;

        if (nread > 0 && line[nread-1] == '\n') line[nread-1] = '\0';
        char *clean = trim(line);
        if (*clean == '\0') continue;

        // 1) separar por '&' (subcomandos en paralelo)
        char **parts = NULL;
        int parts_count = 0;
        parts = split_by_ampersand(clean, &parts_count);
        if (!parts) { print_error(); continue; }
        if (parts_count == 0) { // solo '&' o vacíos
            print_error();
            free_parts(parts, parts_count);
            continue;
        }

        // 2) lanzar cada subcomando
        pid_t *children = calloc(parts_count, sizeof(pid_t));
        if (!children) { print_error(); free_parts(parts, parts_count); continue; }

        int need_exit = 0;
        int launched = 0;
        for (int i = 0; i < parts_count; i++) {
            pid_t p = run_one_command(parts[i]);
            if (p == -2) { // 'exit' detectado como único comando en subcmd
                need_exit = 1;
                // si hay más subcomandos en la misma línea, mostramos error para mantener reglas simples
                if (parts_count > 1) print_error();
                break;
            }
            if (p > 0) children[launched++] = p; // externo lanzado
            // p == 0 → built-in ya ejecutado en el padre; p == -1 → error ya informado
        }

        // 3) esperar por todos los hijos lanzados
        for (int i = 0; i < launched; i++) {
            (void)waitpid(children[i], NULL, 0);
        }

        free(children);
        free_parts(parts, parts_count);

        if (need_exit) {
            free(line);
            clear_path();
            if (in && in != stdin) fclose(in);
            exit(0);
        }
    }

    free(line);
    clear_path();
    if (in && in != stdin) fclose(in);
    return 0;
}
