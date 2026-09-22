/**
 * mkzombies.c - Gerador de processos zumbis
 *
 * Programa fornecido na Lista 3 de Tópicos Especiais em Sistemas Computacionais
 * para produzir processos ZOMBIEs de teste.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define EVER ;;

int main(int argc, char **argv)
{
    int n;
    if ((n = (argc == 1) ? 1 : atol(argv[1])) <= 0)
    {
        fprintf(stderr, "Use: %s [<n>]\n", argv[0]);
        return 1;
    }
    if (fork()) /* Passa para background */
        exit(0);
    while (n-- > 0)
    {
        if (fork() == 0)
            exit(0); /* Filhos falecem ... */
    }
    for (EVER)
        pause(); /* ... e o pai nem liga */
    return 0;
}
