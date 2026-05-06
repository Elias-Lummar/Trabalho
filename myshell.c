
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>

#define EMPTY 0
#define DOGS 1
#define CATS 2

pthread_mutex_t mutex;
pthread_cond_t cond;

int estadoSala = EMPTY;
int qtdDentro = 0;

typedef struct {
    char id[10];
    int especie;
    int chegada;
    int descanso;
} Animal;

void* entrarSala(void* arg) {
    Animal* a = (Animal*) arg;

    sleep(a->chegada);

    pthread_mutex_lock(&mutex);

    while (estadoSala != EMPTY && estadoSala != a->especie) {
        pthread_cond_wait(&cond, &mutex);
    }

    if (estadoSala == EMPTY) {
        estadoSala = a->especie;
    }

    qtdDentro++;

    printf("%s entrou na sala. Total: %d\n", a->id, qtdDentro);

    pthread_mutex_unlock(&mutex);

    sleep(a->descanso);

    pthread_mutex_lock(&mutex);

    qtdDentro--;

    printf("%s saiu da sala. Total: %d\n", a->id, qtdDentro);

    if (qtdDentro == 0) {
        estadoSala = EMPTY;
        pthread_cond_broadcast(&cond);
    }

    pthread_mutex_unlock(&mutex);

    return NULL;
}

int main() {

    pthread_t threads[5];

    Animal animais[5] = {
        {"D01", DOGS, 0, 5},
        {"C01", CATS, 1, 4},
        {"D02", DOGS, 2, 6},
        {"C02", CATS, 3, 2},
        {"D03", DOGS, 4, 3}
    };

    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);

    for (int i = 0; i < 5; i++) {
        pthread_create(&threads[i], NULL, entrarSala, &animais[i]);
    }

    for (int i = 0; i < 5; i++) {
        pthread_join(threads[i], NULL);
    }

    pthread_mutex_destroy(&mutex);
    pthread_cond_destroy(&cond);

    return 0;
}
