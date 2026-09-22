/**
 * przombies.c - Daemon monitor de processos ZOMBIE
 *
 * Disciplina: TopEspSistComp (Tópicos Especiais em Sistemas Computacionais)
 * Lista 3 – Processos e comandos afins (Exercício Prático - Processos)
 *
 * Descrição:
 *   Implementa um daemon que, de n em n segundos (passado como argumento),
 *   acorda e escreve em um arquivo próprio de log informações sobre os
 *   processos ZOMBIEs do sistema.
 *
 *   Suporta os dois métodos descritos no enunciado:
 *   1. Varredura do diretório /proc (opendir, readdir, closedir e leitura de
 * status) - Padrão
 *   2. Execução do utilitário ps como filho do daemon conectado via pipe (--ps)
 *
 *   Tratamento de sinais:
 *   - SIGTERM: interceptado, encerra o daemon e grava mensagem de finalização
 * no log.
 *   - Demais sinais: ignorados (exceto SIGKILL e SIGSTOP, que não podem ser
 * bloqueados).
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define METHOD_PROC 1
#define METHOD_PS 2

#define DEFAULT_LOG_FILE "przombies.log"
#define LOG_SEPARATOR "=========================================="

#ifndef NSIG
#ifdef _NSIG
#define NSIG _NSIG
#else
#define NSIG 64
#endif
#endif

/* Estrutura para armazenar informações de um processo zumbi */
typedef struct {
  pid_t pid;
  pid_t ppid;
  char name[256];
} ZombieInfo;

/* Lista dinâmica para armazenar os zumbis encontrados */
typedef struct {
  ZombieInfo *items;
  size_t count;
  size_t capacity;
} ZombieList;

/* Variável global atômica controlada pelo manipulador de SIGTERM */
static volatile sig_atomic_t g_running = 1;

/* Inicialização da lista de zumbis */
static void zombie_list_init(ZombieList *list) {
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

/* Adiciona um zumbi à lista */
static int zombie_list_add(ZombieList *list, pid_t pid, pid_t ppid,
                           const char *name) {
  if (list->count >= list->capacity) {
    size_t new_cap = (list->capacity == 0) ? 16 : list->capacity * 2;
    ZombieInfo *new_items =
        (ZombieInfo *)realloc(list->items, new_cap * sizeof(ZombieInfo));
    if (!new_items) {
      return -1;
    }
    list->items = new_items;
    list->capacity = new_cap;
  }
  list->items[list->count].pid = pid;
  list->items[list->count].ppid = ppid;
  strncpy(list->items[list->count].name, name,
          sizeof(list->items[list->count].name) - 1);
  list->items[list->count].name[sizeof(list->items[list->count].name) - 1] =
      '\0';
  list->count++;
  return 0;
}

/* Libera memória da lista de zumbis */
static void zombie_list_free(ZombieList *list) {
  free(list->items);
  list->items = NULL;
  list->count = 0;
  list->capacity = 0;
}

/* Função de ordenação por PID crescente */
static int compare_zombies(const void *a, const void *b) {
  const ZombieInfo *za = (const ZombieInfo *)a;
  const ZombieInfo *zb = (const ZombieInfo *)b;
  if (za->pid < zb->pid)
    return -1;
  if (za->pid > zb->pid)
    return 1;
  return 0;
}

/* Verifica se uma string contém apenas dígitos (representa um PID) */
static int is_numeric_str(const char *str) {
  if (!str || !*str)
    return 0;
  for (int i = 0; str[i] != '\0'; i++) {
    if (!isdigit((unsigned char)str[i])) {
      return 0;
    }
  }
  return 1;
}

/* Manipulador para SIGTERM */
static void handle_sigterm(int sig) {
  (void)sig;
  g_running = 0;
}

/* Configura tratamento de sinais: intercepta SIGTERM e ignora os demais */
static void configure_signals(void) {
  struct sigaction sa_term;
  memset(&sa_term, 0, sizeof(sa_term));
  sa_term.sa_handler = handle_sigterm;
  sigemptyset(&sa_term.sa_mask);
  /* Não definimos SA_RESTART para que chamadas bloqueantes como sleep() sejam
   * interrompidas imediatamente */
  sa_term.sa_flags = 0;

  struct sigaction sa_ign;
  memset(&sa_ign, 0, sizeof(sa_ign));
  sa_ign.sa_handler = SIG_IGN;
  sigemptyset(&sa_ign.sa_mask);
  sa_ign.sa_flags = 0;

  for (int sig = 1; sig < NSIG; sig++) {
    if (sig == SIGTERM) {
      sigaction(sig, &sa_term, NULL);
    } else if (sig == SIGKILL || sig == SIGSTOP) {
      /* Sinais não capturáveis nem ignoráveis pelo kernel */
      continue;
    } else if (sig == SIGCHLD) {
      /* Mantém o comportamento padrão para colher processos filhos via waitpid
       * sem conflito */
      signal(sig, SIG_DFL);
    } else {
      /* Daemon invulnerável aos demais sinais */
      sigaction(sig, &sa_ign, NULL);
    }
  }
}

/**
 * Método 1: Varredura de /proc usando opendir, readdir, closedir e
 * /proc/[pid]/status
 */
static int scan_zombies_via_proc(ZombieList *list) {
  DIR *proc_dir = opendir("/proc");
  if (!proc_dir) {
    return -1;
  }

  struct dirent *entry;
  while ((entry = readdir(proc_dir)) != NULL) {
    /* Verifica se o nome da entrada é numérico (subdiretório de processo) */
    if (!is_numeric_str(entry->d_name)) {
      continue;
    }

    char status_path[PATH_MAX];
    snprintf(status_path, sizeof(status_path), "/proc/%s/status",
             entry->d_name);

    FILE *status_fp = fopen(status_path, "r");
    if (!status_fp) {
      /* Processo encerrou entre readdir e fopen ou sem permissão */
      continue;
    }

    char line[512];
    char comm[256] = "";
    char state = '\0';
    pid_t ppid = 0;
    int found_name = 0, found_state = 0, found_ppid = 0;

    while (fgets(line, sizeof(line), status_fp)) {
      if (strncmp(line, "Name:", 5) == 0) {
        char *p = line + 5;
        while (*p == ' ' || *p == '\t')
          p++;
        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r')) {
          p[len - 1] = '\0';
          len--;
        }
        strncpy(comm, p, sizeof(comm) - 1);
        comm[sizeof(comm) - 1] = '\0';
        found_name = 1;
      } else if (strncmp(line, "State:", 6) == 0) {
        char *p = line + 6;
        while (*p == ' ' || *p == '\t')
          p++;
        state = *p;
        found_state = 1;
      } else if (strncmp(line, "PPid:", 5) == 0) {
        char *p = line + 5;
        while (*p == ' ' || *p == '\t')
          p++;
        ppid = (pid_t)atoi(p);
        found_ppid = 1;
      }

      if (found_name && found_state && found_ppid) {
        break;
      }
    }
    fclose(status_fp);

    /* Estado 'Z' indica processo ZOMBIE */
    if (state == 'Z') {
      pid_t pid = (pid_t)atoi(entry->d_name);
      zombie_list_add(list, pid, ppid, comm);
    }
  }

  closedir(proc_dir);
  return 0;
}

/**
 * Método 2: Execução de ps como filho do daemon com comunicação via pipe
 */
static int scan_zombies_via_ps(ZombieList *list) {
  int pipefd[2];
  if (pipe(pipefd) == -1) {
    return -1;
  }

  pid_t child_pid = fork();
  if (child_pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (child_pid == 0) {
    /* Processo filho: redireciona stdout para a ponta de escrita do pipe */
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);

    int dev_null = open("/dev/null", O_WRONLY);
    if (dev_null >= 0) {
      dup2(dev_null, STDERR_FILENO);
      close(dev_null);
    }

    /* Executa ps formatado: PID PPID STAT COMMAND */
    execlp("ps", "ps", "-eo", "pid,ppid,state,comm", (char *)NULL);
    _exit(127);
  }

  /* Processo pai (daemon): lê da ponta de leitura do pipe */
  close(pipefd[1]);
  FILE *pipe_fp = fdopen(pipefd[0], "r");
  if (!pipe_fp) {
    close(pipefd[0]);
    waitpid(child_pid, NULL, 0);
    return -1;
  }

  char line[512];
  /* Descarta o cabeçalho "PID PPID S COMMAND" */
  if (fgets(line, sizeof(line), pipe_fp) != NULL) {
    /* cabeçalho lido */
  }

  while (fgets(line, sizeof(line), pipe_fp)) {
    pid_t pid = 0, ppid = 0;
    char state[32] = "";
    char comm[256] = "";

    if (sscanf(line, "%d %d %31s %255s", &pid, &ppid, state, comm) >= 4) {
      if (state[0] == 'Z') {
        zombie_list_add(list, pid, ppid, comm);
      }
    }
  }

  fclose(pipe_fp);
  waitpid(child_pid, NULL, 0);
  return 0;
}

/* Transforma o processo atual em um daemon UNIX padrão */
static void daemonize(const char *log_path, int interval, int method) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("Erro ao executar fork inicial");
    exit(EXIT_FAILURE);
  }

  if (pid > 0) {
    /* Processo pai exibe resumo e encerra */
    printf("Daemon przombies iniciado com sucesso.\n");
    printf("  PID: %d\n", pid);
    printf("  Intervalo: %d segundo(s)\n", interval);
    printf("  Metodo de varredura: %s\n",
           (method == METHOD_PROC) ? "/proc (opendir/readdir)" : "ps via pipe");
    printf("  Arquivo de log: %s\n", log_path);
    printf("Para encerrar o daemon:\n");
    printf("  kill -TERM %d\n", pid);
    exit(EXIT_SUCCESS);
  }

  /* Processo filho torna-se líder de uma nova sessão */
  if (setsid() < 0) {
    perror("Erro em setsid");
    exit(EXIT_FAILURE);
  }

  /* Ajusta a máscara de criação de arquivos */
  umask(0);

  /* Muda o diretório raiz para não travar sistemas de arquivos montados */
  if (chdir("/") < 0) {
    /* Aviso silencioso */
  }

  /* Redireciona descritores de entrada e saída padrão para /dev/null */
  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);

  int dev_null = open("/dev/null", O_RDWR);
  if (dev_null >= 0) {
    dup2(dev_null, STDIN_FILENO);
    dup2(dev_null, STDOUT_FILENO);
    dup2(dev_null, STDERR_FILENO);
    if (dev_null > STDERR_FILENO) {
      close(dev_null);
    }
  }
}

/* Exibe mensagem de ajuda */
static void show_help(const char *prog_name) {
  printf("Uso: %s <intervalo_em_segundos> [opcoes] [arquivo_de_log]\n\n",
         prog_name);
  printf("Parametros:\n");
  printf("  <intervalo_em_segundos>  Intervalo de tempo 'n' entre varreduras "
         "(inteiro > 0).\n\n");
  printf("Opcoes:\n");
  printf("  --proc                   Usa varredura no /proc (metodo padrao "
         "recomendado).\n");
  printf(
      "  --ps                     Usa execucao do utilitario ps via pipe.\n");
  printf(
      "  --log <caminho>          Caminho especifico para o arquivo de log.\n");
  printf("  -h, --help               Exibe esta mensagem de ajuda.\n\n");
  printf("Exemplos:\n");
  printf("  %s 3\n", prog_name);
  printf("  %s 5 meulog.log\n", prog_name);
  printf("  %s 2 --ps\n", prog_name);
}

int main(int argc, char *argv[]) {
  int interval = 0;
  int method = METHOD_PROC;
  const char *custom_log = NULL;

  if (argc < 2) {
    fprintf(stderr, "Uso: %s <intervalo_em_segundos> [opcoes]\n", argv[0]);
    fprintf(stderr, "Execute '%s --help' para mais informacoes.\n", argv[0]);
    return EXIT_FAILURE;
  }

  /* Analise de argumentos de linha de comando */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      show_help(argv[0]);
      return EXIT_SUCCESS;
    } else if (strcmp(argv[i], "--proc") == 0) {
      method = METHOD_PROC;
    } else if (strcmp(argv[i], "--ps") == 0) {
      method = METHOD_PS;
    } else if (strcmp(argv[i], "--log") == 0) {
      if (i + 1 < argc) {
        custom_log = argv[++i];
      } else {
        fprintf(stderr, "Erro: A opcao --log requer o caminho do arquivo.\n");
        return EXIT_FAILURE;
      }
    } else if (interval == 0 && is_numeric_str(argv[i])) {
      interval = atoi(argv[i]);
    } else if (!custom_log) {
      custom_log = argv[i];
    } else {
      fprintf(stderr, "Argumento desconhecido: %s\n", argv[i]);
      fprintf(stderr, "Execute '%s --help' para mais informacoes.\n", argv[0]);
      return EXIT_FAILURE;
    }
  }

  if (interval <= 0) {
    fprintf(stderr, "Erro: O intervalo deve ser um numero inteiro positivo.\n");
    return EXIT_FAILURE;
  }

  /* Determina o caminho absoluto do arquivo de log antes de mudar de diretorio
   */
  char resolved_log_path[PATH_MAX];
  const char *target_log = custom_log ? custom_log : DEFAULT_LOG_FILE;

  if (target_log[0] == '/') {
    strncpy(resolved_log_path, target_log, sizeof(resolved_log_path) - 1);
    resolved_log_path[sizeof(resolved_log_path) - 1] = '\0';
  } else {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
      size_t cwd_len = strlen(cwd);
      size_t tgt_len = strlen(target_log);
      if (cwd_len + 1 + tgt_len < sizeof(resolved_log_path)) {
        memcpy(resolved_log_path, cwd, cwd_len);
        resolved_log_path[cwd_len] = '/';
        memcpy(resolved_log_path + cwd_len + 1, target_log, tgt_len);
        resolved_log_path[cwd_len + 1 + tgt_len] = '\0';
      } else {
        fprintf(stderr, "Erro: Caminho do arquivo de log muito longo.\n");
        return EXIT_FAILURE;
      }
    } else {
      strncpy(resolved_log_path, target_log, sizeof(resolved_log_path) - 1);
      resolved_log_path[sizeof(resolved_log_path) - 1] = '\0';
    }
  }

  /* Testa abertura previa do arquivo de log para reportar erros ao usuario */
  FILE *test_fp = fopen(resolved_log_path, "a");
  if (!test_fp) {
    fprintf(stderr, "Erro ao abrir/criar arquivo de log '%s': %s\n",
            resolved_log_path, strerror(errno));
    return EXIT_FAILURE;
  }
  fclose(test_fp);

  /* Daemoniza o processo */
  daemonize(resolved_log_path, interval, method);

  /* Configura os sinais no processo daemon */
  configure_signals();

  /* Abre o arquivo de log para gravacao continua */
  FILE *log_fp = fopen(resolved_log_path, "a");
  if (!log_fp) {
    exit(EXIT_FAILURE);
  }

  /* Escreve o cabecalho padrao exigido no exercicio */
  fprintf(log_fp, "PID PPID Nome do Programa\n%s\n", LOG_SEPARATOR);
  fflush(log_fp);

  /* Loop principal do daemon */
  while (g_running) {
    ZombieList zombies;
    zombie_list_init(&zombies);

    if (method == METHOD_PROC) {
      scan_zombies_via_proc(&zombies);
    } else {
      scan_zombies_via_ps(&zombies);
    }

    /* Se houver processos zumbis, escreve no log conforme especificado */
    if (zombies.count > 0) {
      qsort(zombies.items, zombies.count, sizeof(ZombieInfo), compare_zombies);

      for (size_t i = 0; i < zombies.count; i++) {
        fprintf(log_fp, "%d %d %s\n", zombies.items[i].pid,
                zombies.items[i].ppid, zombies.items[i].name);
      }
      fprintf(log_fp, "%s\n", LOG_SEPARATOR);
      fflush(log_fp);
    }

    zombie_list_free(&zombies);

    /* Dorme pelo intervalo 'n' especificado; acorda imediatamente se receber
     * SIGTERM */
    unsigned int remaining = (unsigned int)interval;
    while (remaining > 0 && g_running) {
      remaining = sleep(remaining);
    }
  }

  /* Mensagem de finalizacao solicitada ao receber SIGTERM */
  fprintf(log_fp, "Daemon finalizado (SIGTERM recebido).\n%s\n", LOG_SEPARATOR);
  fflush(log_fp);
  fclose(log_fp);

  return EXIT_SUCCESS;
}
