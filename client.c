#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

#define DEVICE_PATH "/dev/mq"
#define CMD_BUF_SIZE 256
#define READ_BUF_SIZE 512 // Buffer fixo para /read

void limpar_espacos(char *str) {
    char *dst = str;
    int in_space = 1;
    while (*str) {
        if (isspace(*str)) {
            if (!in_space) {
                *dst++ = ' ';
                in_space = 1;
            }
        } else {
            *dst++ = *str;
            in_space = 0;
        }
        str++;
    }
    if (dst > str && isspace(*(dst - 1)))
        dst--;
    *dst = '\0';
}

void mostrar_prompt() {
    printf("\n🌌 Comandos disponíveis no terminal de mensagens:\n");
    printf("  /reg <nome>        - Registrar processo\n");
    printf("  /unr               - Desregistrar processo (ignora extras)\n");
    printf("  /msg <dest> <msg>  - Enviar mensagem\n");
    printf("  /all <msg>         - Enviar mensagem a todos\n");
    printf("  /read              - Ler do dispositivo\n");
    printf("  /exit              - Sair do programa\n");
    printf("--------------------------------------------------\n");
}

int main() {
    int fd = open(DEVICE_PATH, O_RDWR);  // Precisa de leitura e escrita
    if (fd < 0) {
        perror("Erro ao abrir o dispositivo");
        return 1;
    }

    char input[CMD_BUF_SIZE];
    char read_buf[READ_BUF_SIZE];
    mostrar_prompt();

    while (1) {
        printf("\n> ");
        if (!fgets(input, sizeof(input), stdin)) break;

        input[strcspn(input, "\n")] = '\0';
        limpar_espacos(input);

        if (strncmp(input, "/reg ", 5) == 0) {
            char *nome = input + 5;
            char cmd[CMD_BUF_SIZE];
            snprintf(cmd, sizeof(cmd), "/reg %s", nome);
            write(fd, cmd, strlen(cmd) + 1);

        } else if (strncmp(input, "/unr", 4) == 0) {
            write(fd, "/unr", 5);

        } else if (strncmp(input, "/msg ", 5) == 0) {
            write(fd, input, strlen(input) + 1);

        } else if (strncmp(input, "/all ", 5) == 0) {
            write(fd, input, strlen(input) + 1);

        } else if (strcmp(input, "/read") == 0) {
            ssize_t bytes = read(fd, read_buf, READ_BUF_SIZE - 1);
            if (bytes > 0) {
                read_buf[bytes] = '\0';  // Garantir terminação
                printf("Mensagem recebida:\n%s\n", read_buf);
            } else if (bytes == 0) {
                printf("Nenhuma mensagem disponível no momento.\n");
            } else {
                perror("Erro ao ler do dispositivo");
            }

        } else if (strcmp(input, "/exit") == 0) {
            printf("Encerrando...\n");
            break;

        } else {
            printf("Comando não reconhecido.\n");
        }
    }

    close(fd);
    return 0;
}