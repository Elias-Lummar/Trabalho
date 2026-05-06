#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <semaphore.h>

#define N 5

sem_t compilador;   
sem_t banco_dados;     

void* programador(void* arg) {
    int id = *((int*) arg);

    while (1) {
        printf("Programador %d está pensando...\n", id);
        sleep(rand() % 3 + 1);

        printf("Programador %d quer compilar.\n", id);

        sem_wait(&banco_dados);
        printf("Programador %d acessando banco de dados.\n", id);

        sem_wait(&compilador);
        printf("Programador %d compilando...\n", id);

        sleep(2);

        printf("Programador %d terminou de compilar.\n", id);

        sem_post(&compilador);
        sem_post(&banco_dados);
    }
}

int main() {
    pthread_t threads[N];
    int ids[N];

    srand(time(NULL));

    sem_init(&compilador, 0, 1);   
    sem_init(&banco_dados, 0, 2);

    for (int i = 0; i < N; i++) {
        ids[i] = i + 1;
        pthread_create(&threads[i], NULL, programador, &ids[i]);
    }

    for (int i = 0; i < N; i++) {
        pthread_join(threads[i], NULL);
    }

    return 0;
}