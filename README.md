# Exercício Prático - Monitor de Processos ZOMBIEs (`przombies`)

**Disciplina:** Tópicos Especiais em Sistemas Computacionais (`TopEspSistComp`)  
**Lista 3:** Processos e comandos afins  

---

## 1. Descrição do Problema

O objetivo deste projeto é implementar um **daemon** em C (`przombies`) que, periodicamente (a cada `n` segundos, informado na linha de comando), acorda e registra em um arquivo de log informações sobre os processos em estado **ZOMBIE** (`defunct`) presentes no sistema.

Exemplo de formato de log gerado:
```text
PID PPID Nome do Programa
==========================================
223 220 prog1
321 220 xcount
==========================================
223 220 prog1
321 220 xcount
400 105 xpto
==========================================
...
```

### Requisitos Atendidos:
1. **Daemonização Completa:** O programa cria um processo filho (`fork()`), o pai finaliza, o filho se desacopla do terminal controlador via `setsid()`, redefine a máscara com `umask(0)`, altera o diretório de trabalho para a raiz `/` e redireciona os descritores padrão (`stdin`, `stdout`, `stderr`) para `/dev/null`.
2. **Tratamento de Sinais:**
   - O sinal `SIGTERM` é interceptado de forma segura via `sigaction`. Ao ser recebido, interrompe o `sleep()`, escreve no arquivo de log a mensagem de encerramento do daemon e finaliza a execução de forma limpa.
   - O daemon é **invulnerável** aos demais sinais (como `SIGINT`, `SIGHUP`, `SIGQUIT`, `SIGUSR1`, etc.), mantendo-os configurados com `SIG_IGN` (com exceção de `SIGKILL` e `SIGSTOP`, que o kernel não permite interceptar nem ignorar).
3. **Métodos de Identificação de Zumbis:**
   - **Método 1 (Padrão):** Varredura direta do sistema de arquivos virtual `/proc` utilizando as chamadas `opendir`, `readdir`, `closedir` e leitura do arquivo `/proc/[pid]/status` (extraindo os campos `Name:`, `State:`, `Pid:` e `PPid:`).
   - **Método 2 (Opcional com `--ps`):** Execução do utilitário `ps` como processo filho conectado ao daemon via `pipe()`, `fork()` e `dup2()`.

---

## 2. Estrutura dos Arquivos

- **[`przombies.c`](przombies.c):** Código-fonte do daemon monitor de zumbis.
- **[`mkzombies.c`](mkzombies.c):** Programa gerador de processos zumbis (fornecido no enunciado da lista para testes).
- **[`Makefile`](Makefile):** Script de compilação automatizada com flags rigorosas (`-Wall -Wextra -O2 -std=c99`).
- **[`README.md`](README.md):** Esta documentação.

---

## 3. Compilação

Para compilar os programas, execute:

```bash
make
```

Isso gerará os executáveis `przombies` e `mkzombies`. Para limpar os binários e arquivos de log:

```bash
make clean
```

---

## 4. Como Executar

### Sintaxe Básica:
```bash
./przombies <n>
```
Onde `<n>` é o intervalo em segundos entre as varreduras.

### Opções Disponíveis:
```text
Uso: ./przombies <intervalo_em_segundos> [opcoes] [arquivo_de_log]

Parametros:
  <intervalo_em_segundos>  Intervalo de tempo 'n' entre varreduras (inteiro > 0).

Opcoes:
  --proc                   Usa varredura no /proc (metodo padrao).
  --ps                     Usa execucao do utilitario ps via pipe.
  --log <caminho>          Caminho especifico para o arquivo de log.
  -h, --help               Exibe a mensagem de ajuda.
```

### Exemplos:
1. Iniciar daemon a cada 3 segundos (grava em `./przombies.log` por padrão):
   ```bash
   ./przombies 3
   ```

2. Iniciar daemon usando o método alternativo de `ps` via pipe:
   ```bash
   ./przombies 3 --ps
   ```

3. Iniciar especificando um arquivo de log personalizado:
   ```bash
   ./przombies 5 --log /tmp/meus_zumbis.log
   ```

---

## 5. Roteiro de Teste Prático

Siga os passos abaixo para verificar o funcionamento completo:

### Passo 1: Iniciar o daemon
```bash
./przombies 2
```
A saída exibirá o PID do daemon e o caminho do arquivo de log:
```text
Daemon przombies iniciado com sucesso.
  PID: 12345
  Intervalo: 2 segundo(s)
  Metodo de varredura: /proc (opendir/readdir)
  Arquivo de log: .../przombies.log
Para encerrar o daemon:
  kill -TERM 12345
```

### Passo 2: Acompanhar o log em tempo real
Abra outro terminal ou execute:
```bash
tail -f przombies.log
```

### Passo 3: Criar processos zumbis com `mkzombies`
Em outro terminal (ou na mesma sessão), invoque o gerador de zumbis:
```bash
./mkzombies 3
```
Em até 2 segundos, o daemon detectará os 3 processos zumbis e os registrará no log:
```text
PID PPID Nome do Programa
==========================================
12401 12400 mkzombies
12402 12400 mkzombies
12403 12400 mkzombies
==========================================
```

### Passo 4: Testar a invulnerabilidade a outros sinais
Envie sinais como `SIGINT`, `SIGHUP` e `SIGUSR1` para o daemon:
```bash
kill -INT 12345
kill -HUP 12345
kill -USR1 12345
```
Verifique que o daemon continua em execução (`ps aux | grep przombies`).

### Passo 5: Finalizar o daemon com `SIGTERM`
Envie o sinal `SIGTERM`:
```bash
kill -TERM 12345
```
Verifique o encerramento no final do arquivo de log:
```text
Daemon finalizado (SIGTERM recebido).
==========================================
```

### Passo 6: Limpar os processos zumbis
Para eliminar os zumbis criados pelo teste, finalize o pai deles:
```bash
killall -9 mkzombies
```
