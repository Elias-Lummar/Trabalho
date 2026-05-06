/*
 * myshell.c — Shell simplificado
 * Baseado no pseudocódigo da Figura 1.18 (Sistemas Operacionais Modernos, p. 38)
 *
 * Chamadas de sistema utilizadas:
 *   fork()     — cria processo filho idêntico ao pai
 *   waitpid()  — espera que um processo filho seja concluído
 *   execve()   — substitui a imagem do núcleo de um processo
 *   exit()     — conclui a execução do processo
 *   chdir()    — altera o diretório de trabalho (builtin cd)
 *   getcwd()   — obtém o diretório de trabalho atual (para o prompt)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ------------------------------------------------------------------ */
/* Constantes                                                           */
/* ------------------------------------------------------------------ */
#define TRUE          1
#define MAX_LINE      1024   /* tamanho máximo de uma linha de comando  */
#define MAX_ARGS      64     /* número máximo de argumentos             */

/* ------------------------------------------------------------------ */
/* type_prompt — exibe o prompt com o diretório atual                  */
/* ------------------------------------------------------------------ */
void type_prompt(void)
{
    char cwd[MAX_LINE];

    if (getcwd(cwd, sizeof(cwd)) != NULL)
        printf("\033[1;32mmyshell\033[0m:\033[1;34m%s\033[0m$ ", cwd);
    else
        printf("myshell$ ");

    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* read_command — lê a linha do terminal e divide em tokens            */
/*                                                                     */
/* Parâmetros:                                                         */
/*   command    — recebe o nome do executável (argv[0])                */
/*   parameters — vetor de ponteiros terminado em NULL (argv completo) */
/*                                                                     */
/* Retorno: 0 se EOF (Ctrl-D), 1 se há comando para executar          */
/* ------------------------------------------------------------------ */
int read_command(char *command, char **parameters)
{
    static char line[MAX_LINE];
    int argc = 0;

    /* Lê a linha; retorna 0 em EOF */
    if (fgets(line, sizeof(line), stdin) == NULL)
        return 0;

    /* Remove a newline final */
    line[strcspn(line, "\n")] = '\0';

    /* Ignora linhas vazias */
    if (line[0] == '\0') {
        parameters[0] = NULL;
        command[0]    = '\0';
        return 1;
    }

    /* Tokeniza usando espaços/tabs como delimitadores */
    char *token = strtok(line, " \t");
    while (token != NULL && argc < MAX_ARGS - 1) {
        parameters[argc++] = token;
        token = strtok(NULL, " \t");
    }
    parameters[argc] = NULL;   /* execve exige NULL no final */

    /* command é o primeiro token (nome do executável) */
    strncpy(command, parameters[0], MAX_LINE - 1);
    command[MAX_LINE - 1] = '\0';

    return 1;
}

/* ------------------------------------------------------------------ */
/* handle_builtin — trata comandos internos do shell                   */
/*                                                                     */
/* Retorno: 1 se o comando foi tratado aqui, 0 caso contrário         */
/* ------------------------------------------------------------------ */
int handle_builtin(const char *command, char **parameters)
{
    /* exit / quit — encerra o shell */
    if (strcmp(command, "exit") == 0 || strcmp(command, "quit") == 0) {
        printf("Encerrando myshell. Até logo!\n");
        exit(EXIT_SUCCESS);
    }

    /* cd — altera diretório (chdir não pode ser feito no filho) */
    if (strcmp(command, "cd") == 0) {
        const char *dir = parameters[1] ? parameters[1] : getenv("HOME");
        if (chdir(dir) != 0)
            perror("cd");
        return 1;
    }

    return 0;   /* não é um builtin */
}

/* ------------------------------------------------------------------ */
/* main — loop principal do shell (Figura 1.18)                        */
/* ------------------------------------------------------------------ */
int main(void)
{
    char  command[MAX_LINE];
    char *parameters[MAX_ARGS];
    int   status;
    pid_t pid;

    printf("=== myshell — Shell Simplificado ===\n");
    printf("Digite 'exit' ou 'quit' para sair.\n\n");

    while (TRUE) {                          /* repita para sempre */

        type_prompt();                      /* mostra prompt na tela */

        if (!read_command(command, parameters))  /* lê entrada do terminal */
            break;                          /* EOF — encerra o loop */

        /* Ignora linha vazia */
        if (command[0] == '\0')
            continue;

        /* Verifica comandos internos ANTES de fazer fork */
        if (handle_builtin(command, parameters))
            continue;

        /* ---------------------------------------------------------- */
        pid = fork();                       /* cria processo filho    */
        /* ---------------------------------------------------------- */

        if (pid < 0) {
            /* fork falhou */
            perror("fork");
            continue;
        }

        if (pid != 0) {
            /* ------------------------------------------------------- */
            /* Código do processo PAI                                   */
            /* ------------------------------------------------------- */
            waitpid(pid, &status, 0);       /* aguarda o filho acabar  */

            if (WIFEXITED(status))
                printf("[filho encerrou com código %d]\n", WEXITSTATUS(status));
            else if (WIFSIGNALED(status))
                printf("[filho encerrou por sinal %d]\n", WTERMSIG(status));

        } else {
            /* ------------------------------------------------------- */
            /* Código do processo FILHO                                 */
            /* ------------------------------------------------------- */
            /*
             * execve(command, parameters, envp)
             *   command    — caminho completo OU nome a ser buscado no PATH
             *   parameters — argv (vetor terminado em NULL)
             *   environ    — herda as variáveis de ambiente do pai
             *
             * execvp é igual ao execve mas resolve o PATH automaticamente,
             * assim como um shell real faria.
             */
            execvp(command, parameters);    /* executa o comando        */

            /* Se execvp retornou, houve erro */
            perror(command);
            exit(EXIT_FAILURE);             /* encerra apenas o filho   */
        }
    }

    printf("\nSaindo do myshell.\n");
    return EXIT_SUCCESS;
}
