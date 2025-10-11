#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

int main() {
    pid_t pid = fork();

    if (pid < 0) {
        perror("El proceso fork falló");
        exit(EXIT_FAILURE);
    } else if (pid == 0) {
        printf("Soy el proceso hijo con PID %d\n", getpid());
        sleep(2); 
        exit(42);
    } else {
        int status;
        pid_t wpid = waitpid(pid, &status, 0);
        printf("Soy el proceso padre con PID %d, esperando a que el hijo termine...\n", getpid());

        if (wpid == -1) {
            perror("Waitpid falló");
            exit(EXIT_FAILURE);
        }

        if (WIFEXITED(status)) {
            int exit_status = WEXITSTATUS(status);
            printf("El proceso hijo con PID %d terminó con código de salida %d\n", wpid, exit_status);
        } else {
            printf("El proceso hijo no terminó normalmente\n");
        }
    }

    return 0;
}