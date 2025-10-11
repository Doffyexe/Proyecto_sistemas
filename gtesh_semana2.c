// gtesh_semana2.c — Semana 2: comandos integrados cd y path + manejo del PATH interno
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>

// Mensaje de error único, según el enunciado
static const char ERROR_MSG[] = "An error has occurred\n";

// Imprime el mensaje de error en stderr
static void print_error(void) {
    (void)write(STDERR_FILENO, ERROR_MSG, sizeof(ERROR_MSG) - 1);
}

// ---------- Utilidades de texto ----------

// Quita espacios en blanco al inicio y al final de la cadena
static char *trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len-1])) s[--len] = '\0';
    return s;
}

// Tokeniza una línea separando por espacios y tabuladores.
// Devuelve un arreglo argv[] terminado en NULL.
static char **tokenize(char *line, int *argc_out) {
    const char *delim = " \t";  // separadores
    int cap = 8, argc = 0;      // capacidad inicial 8
    char **argv = malloc(sizeof(char *) * cap);
    if (!argv) return NULL;

    char *token = NULL;
    while ((token = strsep(&line, delim)) != NULL) {
        token = trim(token);
        if (*token == '\0') continue; // ignora tokens vacíos
        if (argc == cap) {            // si se llena, duplica tamaño
            cap *= 2;
            char **tmp = realloc(argv, sizeof(char *) * cap);
            if (!tmp) { free(argv); return NULL; }
            argv = tmp;
        }
        argv[argc++] = strdup(token); // guarda una copia del token
        if (!argv[argc-1]) { // si falla strdup, libera todo
            for (int i = 0; i < argc-1; i++) free(argv[i]);
            free(argv);
            return NULL;
        }
    }
    argv[argc] = NULL;         // marca el final
    if (argc_out) *argc_out = argc;
    return argv;
}

// Libera el arreglo argv
static void free_argv(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; i++) free(argv[i]);
    free(argv);
}

// ---------- PATH interno ----------

static char *path_dirs[64];   // arreglo con directorios del PATH
static size_t path_count = 0; // cantidad de directorios en uso

// Limpia todo el PATH interno liberando memoria
static void clear_path(void) {
    for (size_t i = 0; i < path_count; i++) {
        free(path_dirs[i]);
        path_dirs[i] = NULL;
    }
    path_count = 0;
}

// Asigna un nuevo PATH según los argumentos recibidos
static int set_path_from_args(int argc_cmd, char **argv_cmd) {
    clear_path(); // borra el PATH anterior

    for (int i = 1; i < argc_cmd; i++) {
        if (path_count >= (sizeof(path_dirs)/sizeof(path_dirs[0]))) {
            print_error();
            clear_path();
            return -1;
        }
        char *copy = strdup(argv_cmd[i]);
        if (!copy) {
            print_error();
            clear_path();
            return -1;
        }
        path_dirs[path_count++] = copy;
    }
    return 0;
}

// Busca un ejecutable en el PATH interno usando access()
static char *find_executable(const char *cmd) {
    if (!cmd || !*cmd) return NULL;

    // Si el comando contiene '/', probar la ruta directamente
    if (strchr(cmd, '/')) {
        if (access(cmd, X_OK) == 0) return strdup(cmd);
        else return NULL;
    }

    // Si no contiene '/', buscar en cada directorio del PATH
    for (size_t i = 0; i < path_count; i++) {
        const char *dir = path_dirs[i];
        size_t len = strlen(dir) + strlen(cmd) + 2;
        char *full = malloc(len);
        if (!full) return NULL;
        snprintf(full, len, "%s/%s", dir, cmd);
        if (access(full, X_OK) == 0) return full; // encontrado
        free(full);
    }
    return NULL; // no encontrado
}

int main(int argc, char *argv[]) {
    // Solo modo interactivo (sin archivos)
    if (argc != 1) {
        print_error();
        exit(1);
    }

    // PATH inicial: /bin
    path_dirs[path_count++] = strdup("/bin");
    if (!path_dirs[0]) { print_error(); exit(1); }

    setvbuf(stdout, NULL, _IONBF, 0); // desactiva buffer para mostrar prompt inmediato

    char *line = NULL;
    size_t cap = 0;

    for (;;) {
        printf("gtesh> "); // muestra el prompt

        ssize_t nread = getline(&line, &cap, stdin);
        if (nread == -1) break; // EOF o error (Ctrl+D)

        if (nread > 0 && line[nread-1] == '\n')
            line[nread-1] = '\0';
        char *clean = trim(line);
        if (*clean == '\0') continue; // línea vacía

        // Tokeniza la línea
        int argc_cmd = 0;
        char **argv_cmd = tokenize(clean, &argc_cmd);
        if (!argv_cmd || argc_cmd == 0) {
            free_argv(argv_cmd);
            continue;
        }

        // ======= Comandos internos =======

        // exit
        if (strcmp(argv_cmd[0], "exit") == 0) {
            if (argc_cmd != 1) {
                print_error(); // exit no acepta argumentos
            } else {
                free_argv(argv_cmd);
                free(line);
                clear_path();
                exit(0);
            }
            free_argv(argv_cmd);
            continue;
        }

        // cd <dir>
        if (strcmp(argv_cmd[0], "cd") == 0) {
            if (argc_cmd != 2) {
                print_error(); // cd requiere 1 argumento
            } else {
                if (chdir(argv_cmd[1]) != 0)
                    print_error(); // error si no existe el directorio
            }
            free_argv(argv_cmd);
            continue;
        }

        // path <dir1> <dir2> ...
        if (strcmp(argv_cmd[0], "path") == 0) {
            set_path_from_args(argc_cmd, argv_cmd);
            free_argv(argv_cmd);
            continue;
        }

        // ======= Ejecución de comandos externos =======
        char *exe = find_executable(argv_cmd[0]);
        if (!exe) {
            print_error(); // comando no encontrado
            free_argv(argv_cmd);
            continue;
        }

        pid_t pid = fork(); // crea un proceso hijo
        if (pid < 0) {
            print_error(); // error al crear proceso
        } else if (pid == 0) {
            // Proceso hijo: reemplaza su código por el programa a ejecutar
            execv(exe, argv_cmd);
            // Si execv devuelve, algo falló
            print_error();
            _exit(1);
        } else {
            // Proceso padre: espera a que el hijo termine
            waitpid(pid, NULL, 0);
        }

        free(exe);
        free_argv(argv_cmd);
    }

    // Limpieza final
    free(line);
    clear_path();
    return 0;
}
