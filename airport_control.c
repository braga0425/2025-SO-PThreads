#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <sys/time.h>
#include <stdarg.h>

// Configurações do aeroporto (podem ser ajustadas)
#define NUM_PISTAS 3
#define NUM_PORTOES 5
#define MAX_TORRE_OPERACOES 2
#define TEMPO_SIMULACAO 300  // 5 minutos em segundos
#define TEMPO_CRITICO 60     // 60 segundos para estado crítico
#define TEMPO_QUEDA 90       // 90 segundos para queda

// Experimentos sugeridos:
// Cenário de stress: NUM_PISTAS=1, NUM_PORTOES=2, MAX_TORRE_OPERACOES=1
// Cenário equilibrado: NUM_PISTAS=4, NUM_PORTOES=6, MAX_TORRE_OPERACOES=3
// Cenário de alta capacidade: NUM_PISTAS=5, NUM_PORTOES=8, MAX_TORRE_OPERACOES=4

// Estados do avião
typedef enum {
    AGUARDANDO_POUSO,
    POUSANDO,
    AGUARDANDO_DESEMBARQUE,
    DESEMBARCANDO,
    AGUARDANDO_DECOLAGEM,
    DECOLANDO,
    FINALIZADO,
    CRASHED
} estado_aviao_t;

// Tipos de voo
typedef enum {
    DOMESTICO,
    INTERNACIONAL
} tipo_voo_t;

// Estrutura do avião
typedef struct {
    int id;
    tipo_voo_t tipo;
    estado_aviao_t estado;
    pthread_t thread;
    struct timeval inicio_espera;
    int tempo_espera_total;
    int pista_alocada;
    int portao_alocado;
    int operacoes_concluidas;
    int em_estado_critico;
} aviao_t;

// Recursos do aeroporto
sem_t pistas[NUM_PISTAS];
sem_t portoes[NUM_PORTOES];
sem_t torre;
sem_t torre_priority;  // Para voos em estado crítico

// Mutexes para proteção
pthread_mutex_t mutex_print = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_stats = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_critico = PTHREAD_MUTEX_INITIALIZER;

// Controle da simulação
volatile int simulacao_ativa = 1;
volatile int proximo_id = 1;

// Estatísticas
int total_avioes_criados = 0;
int avioes_finalizados = 0;
int avioes_crashed = 0;
int deadlocks_detectados = 0;
int starvation_cases = 0;

// Lista de aviões para monitoramento
aviao_t *avioes[1000];
int num_avioes = 0;
pthread_mutex_t mutex_avioes = PTHREAD_MUTEX_INITIALIZER;

// Função para obter tempo atual em ms
long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// Função para print thread-safe
void safe_print(const char* format, ...) {
    pthread_mutex_lock(&mutex_print);
    va_list args;
    va_start(args, format);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    printf("[%02d:%02d:%02d] ", tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    vprintf(format, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&mutex_print);
}

// Função para calcular tempo de espera
int calcular_tempo_espera(aviao_t *aviao) {
    struct timeval agora;
    gettimeofday(&agora, NULL);
    return (agora.tv_sec - aviao->inicio_espera.tv_sec);
}

// Função para verificar estado crítico
void verificar_estado_critico(aviao_t *aviao) {
    int tempo_espera = calcular_tempo_espera(aviao);

    if (tempo_espera >= TEMPO_QUEDA) {
        pthread_mutex_lock(&mutex_stats);
        avioes_crashed++;
        starvation_cases++;
        pthread_mutex_unlock(&mutex_stats);

        aviao->estado = CRASHED;
        safe_print("✈️💥 AVIÃO %d (%s) CRASHED por starvation após %d segundos!\n",
                  aviao->id, (aviao->tipo == INTERNACIONAL) ? "INT" : "DOM", tempo_espera);

        // Liberar recursos se alocados
        if (aviao->pista_alocada >= 0) {
            sem_post(&pistas[aviao->pista_alocada]);
            safe_print("🛫 Pista %d liberada devido ao crash do avião %d\n",
                      aviao->pista_alocada, aviao->id);
        }
        if (aviao->portao_alocado >= 0) {
            sem_post(&portoes[aviao->portao_alocado]);
            safe_print("🚪 Portão %d liberado devido ao crash do avião %d\n",
                      aviao->portao_alocado, aviao->id);
        }

        pthread_exit(NULL);
    } else if (tempo_espera >= TEMPO_CRITICO && !aviao->em_estado_critico) {
        pthread_mutex_lock(&mutex_critico);
        aviao->em_estado_critico = 1;
        pthread_mutex_unlock(&mutex_critico);

        safe_print("⚠️ AVIÃO %d (%s) entrou em ESTADO CRÍTICO após %d segundos!\n",
                  aviao->id, (aviao->tipo == INTERNACIONAL) ? "INT" : "DOM", tempo_espera);
    }
}

// Função para resetar cronômetro
void resetar_cronometro(aviao_t *aviao) {
    gettimeofday(&aviao->inicio_espera, NULL);
    aviao->em_estado_critico = 0;
}

// Função para alocar pista
int alocar_pista(aviao_t *aviao) {
    for (int i = 0; i < NUM_PISTAS; i++) {
        if (sem_trywait(&pistas[i]) == 0) {
            aviao->pista_alocada = i;
            return i;
        }
    }

    // Se não conseguiu alocar, espera com verificação de timeout
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 5; // timeout de 5 segundos

    for (int i = 0; i < NUM_PISTAS; i++) {
        if (sem_timedwait(&pistas[i], &timeout) == 0) {
            aviao->pista_alocada = i;
            return i;
        }
    }

    return -1; // Não conseguiu alocar
}

// Função para alocar portão
int alocar_portao(aviao_t *aviao) {
    for (int i = 0; i < NUM_PORTOES; i++) {
        if (sem_trywait(&portoes[i]) == 0) {
            aviao->portao_alocado = i;
            return i;
        }
    }

    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 5;

    for (int i = 0; i < NUM_PORTOES; i++) {
        if (sem_timedwait(&portoes[i], &timeout) == 0) {
            aviao->portao_alocado = i;
            return i;
        }
    }

    return -1;
}

// Função para operação de pouso
int realizar_pouso(aviao_t *aviao) {
    safe_print("✈️ Avião %d (%s) iniciando procedimento de POUSO\n",
              aviao->id, (aviao->tipo == INTERNACIONAL) ? "INT" : "DOM");

    aviao->estado = AGUARDANDO_POUSO;
    resetar_cronometro(aviao);

    int pista = -1, torre_ok = 0;

    if (aviao->tipo == INTERNACIONAL) {
        // Internacional: Pista -> Torre
        while (pista == -1 && aviao->estado != CRASHED) {
            verificar_estado_critico(aviao);
            pista = alocar_pista(aviao);
            if (pista == -1) usleep(100000); // 100ms
        }

        if (aviao->estado == CRASHED) return 0;

        safe_print("🛫 Avião %d alocou pista %d\n", aviao->id, pista);

        // Aviões em estado crítico têm prioridade na torre
        if (aviao->em_estado_critico) {
            sem_wait(&torre_priority);
        } else {
            while (!torre_ok && aviao->estado != CRASHED) {
                verificar_estado_critico(aviao);
                if (sem_trywait(&torre) == 0) {
                    torre_ok = 1;
                } else {
                    usleep(100000);
                }
            }
        }

    } else {
        // Doméstico: Torre -> Pista
        if (aviao->em_estado_critico) {
            sem_wait(&torre_priority);
            torre_ok = 1;
        } else {
            while (!torre_ok && aviao->estado != CRASHED) {
                verificar_estado_critico(aviao);
                if (sem_trywait(&torre) == 0) {
                    torre_ok = 1;
                } else {
                    usleep(100000);
                }
            }
        }

        if (aviao->estado == CRASHED) return 0;

        safe_print("🗼 Avião %d obteve autorização da torre\n", aviao->id);

        while (pista == -1 && aviao->estado != CRASHED) {
            verificar_estado_critico(aviao);
            pista = alocar_pista(aviao);
            if (pista == -1) usleep(100000);
        }
    }

    if (aviao->estado == CRASHED) return 0;

    // Realizar pouso
    aviao->estado = POUSANDO;
    safe_print("🛬 Avião %d POUSANDO na pista %d\n", aviao->id, pista);

    sleep(2); // Tempo de pouso

    // Liberar recursos
    sem_post(&pistas[pista]);
    if (aviao->em_estado_critico) {
        sem_post(&torre_priority);
    } else {
        sem_post(&torre);
    }

    aviao->pista_alocada = -1;
    safe_print("✅ Avião %d POUSOU com sucesso! Pista %d e torre liberadas\n", aviao->id, pista);

    return 1;
}

// Função para operação de desembarque
int realizar_desembarque(aviao_t *aviao) {
    safe_print("🚶 Avião %d iniciando procedimento de DESEMBARQUE\n", aviao->id);

    aviao->estado = AGUARDANDO_DESEMBARQUE;
    resetar_cronometro(aviao);

    int portao = -1, torre_ok = 0;

    if (aviao->tipo == INTERNACIONAL) {
        // Internacional: Portão -> Torre
        while (portao == -1 && aviao->estado != CRASHED) {
            verificar_estado_critico(aviao);
            portao = alocar_portao(aviao);
            if (portao == -1) usleep(100000);
        }

        if (aviao->estado == CRASHED) return 0;

        safe_print("🚪 Avião %d alocou portão %d\n", aviao->id, portao);

        if (aviao->em_estado_critico) {
            sem_wait(&torre_priority);
            torre_ok = 1;
        } else {
            while (!torre_ok && aviao->estado != CRASHED) {
                verificar_estado_critico(aviao);
                if (sem_trywait(&torre) == 0) {
                    torre_ok = 1;
                } else {
                    usleep(100000);
                }
            }
        }

    } else {
        // Doméstico: Torre -> Portão
        if (aviao->em_estado_critico) {
            sem_wait(&torre_priority);
            torre_ok = 1;
        } else {
            while (!torre_ok && aviao->estado != CRASHED) {
                verificar_estado_critico(aviao);
                if (sem_trywait(&torre) == 0) {
                    torre_ok = 1;
                } else {
                    usleep(100000);
                }
            }
        }

        if (aviao->estado == CRASHED) return 0;

        while (portao == -1 && aviao->estado != CRASHED) {
            verificar_estado_critico(aviao);
            portao = alocar_portao(aviao);
            if (portao == -1) usleep(100000);
        }
    }

    if (aviao->estado == CRASHED) return 0;

    // Realizar desembarque
    aviao->estado = DESEMBARCANDO;
    safe_print("🚶‍♂️ Avião %d DESEMBARCANDO no portão %d\n", aviao->id, portao);

    sleep(3); // Tempo de desembarque

    // Liberar torre primeiro
    if (aviao->em_estado_critico) {
        sem_post(&torre_priority);
    } else {
        sem_post(&torre);
    }

    safe_print("🗼 Torre liberada pelo avião %d\n", aviao->id);

    sleep(1); // Tempo adicional no portão

    // Manter portão para decolagem - não liberar ainda
    safe_print("✅ Avião %d DESEMBARCOU com sucesso! Mantendo portão %d para decolagem\n",
              aviao->id, portao);

    return 1;
}

// Função para operação de decolagem
int realizar_decolagem(aviao_t *aviao) {
    safe_print("🚀 Avião %d iniciando procedimento de DECOLAGEM\n", aviao->id);

    aviao->estado = AGUARDANDO_DECOLAGEM;
    resetar_cronometro(aviao);

    int pista = -1, torre_ok = 0;
    // Portão já está alocado do desembarque

    if (aviao->tipo == INTERNACIONAL) {
        // Internacional: Portão (já tem) -> Pista -> Torre
        while (pista == -1 && aviao->estado != CRASHED) {
            verificar_estado_critico(aviao);
            pista = alocar_pista(aviao);
            if (pista == -1) usleep(100000);
        }

        if (aviao->estado == CRASHED) return 0;

        if (aviao->em_estado_critico) {
            sem_wait(&torre_priority);
            torre_ok = 1;
        } else {
            while (!torre_ok && aviao->estado != CRASHED) {
                verificar_estado_critico(aviao);
                if (sem_trywait(&torre) == 0) {
                    torre_ok = 1;
                } else {
                    usleep(100000);
                }
            }
        }

    } else {
        // Doméstico: Torre -> Portão (já tem) -> Pista
        if (aviao->em_estado_critico) {
            sem_wait(&torre_priority);
            torre_ok = 1;
        } else {
            while (!torre_ok && aviao->estado != CRASHED) {
                verificar_estado_critico(aviao);
                if (sem_trywait(&torre) == 0) {
                    torre_ok = 1;
                } else {
                    usleep(100000);
                }
            }
        }

        if (aviao->estado == CRASHED) return 0;

        while (pista == -1 && aviao->estado != CRASHED) {
            verificar_estado_critico(aviao);
            pista = alocar_pista(aviao);
            if (pista == -1) usleep(100000);
        }
    }

    if (aviao->estado == CRASHED) return 0;

    // Realizar decolagem
    aviao->estado = DECOLANDO;
    safe_print("🛫 Avião %d DECOLANDO da pista %d, partindo do portão %d\n",
              aviao->id, pista, aviao->portao_alocado);

    sleep(2); // Tempo de decolagem

    // Liberar todos os recursos
    sem_post(&portoes[aviao->portao_alocado]);
    sem_post(&pistas[pista]);
    if (aviao->em_estado_critico) {
        sem_post(&torre_priority);
    } else {
        sem_post(&torre);
    }

    aviao->portao_alocado = -1;
    aviao->pista_alocada = -1;

    safe_print("🎉 Avião %d DECOLOU com sucesso! Todos os recursos liberados\n", aviao->id);

    return 1;
}

// Função principal da thread do avião
void* thread_aviao(void* arg) {
    aviao_t* aviao = (aviao_t*)arg;

    aviao->pista_alocada = -1;
    aviao->portao_alocado = -1;
    aviao->operacoes_concluidas = 0;
    aviao->em_estado_critico = 0;

    safe_print("🆕 Avião %d (%s) chegou ao aeroporto\n",
              aviao->id, (aviao->tipo == INTERNACIONAL) ? "INTERNACIONAL" : "DOMÉSTICO");

    // Operação 1: Pouso
    if (realizar_pouso(aviao)) {
        aviao->operacoes_concluidas++;

        // Operação 2: Desembarque
        if (realizar_desembarque(aviao)) {
            aviao->operacoes_concluidas++;

            // Operação 3: Decolagem
            if (realizar_decolagem(aviao)) {
                aviao->operacoes_concluidas++;
                aviao->estado = FINALIZADO;

                pthread_mutex_lock(&mutex_stats);
                avioes_finalizados++;
                pthread_mutex_unlock(&mutex_stats);

                safe_print("🏁 Avião %d concluiu TODAS as operações com sucesso!\n", aviao->id);
            }
        }
    }

    return NULL;
}

// Thread para criar aviões periodicamente
void* thread_criador_avioes(void* arg) {
    srand(time(NULL));

    while (simulacao_ativa) {
        if (num_avioes < 1000) {
            pthread_mutex_lock(&mutex_avioes);

            aviao_t* novo_aviao = malloc(sizeof(aviao_t));
            novo_aviao->id = proximo_id++;
            novo_aviao->tipo = (rand() % 2) ? INTERNACIONAL : DOMESTICO;
            novo_aviao->estado = AGUARDANDO_POUSO;

            avioes[num_avioes] = novo_aviao;
            num_avioes++;
            total_avioes_criados++;

            pthread_mutex_unlock(&mutex_avioes);

            // Criar thread do avião
            pthread_create(&novo_aviao->thread, NULL, thread_aviao, novo_aviao);
            pthread_detach(novo_aviao->thread);
        }

        // Intervalo randômico entre 1 e 5 segundos
        sleep(1 + rand() % 5);
    }

    return NULL;
}

// Thread para monitorar sistema
void* thread_monitor(void* arg) {
    while (simulacao_ativa) {
        sleep(10); // Monitora a cada 10 segundos

        pthread_mutex_lock(&mutex_stats);
        safe_print("\n📊 === RELATÓRIO INTERMEDIÁRIO ===\n");
        safe_print("Aviões criados: %d\n", total_avioes_criados);
        safe_print("Aviões finalizados: %d\n", avioes_finalizados);
        safe_print("Aviões crashed: %d\n", avioes_crashed);
        safe_print("Casos de starvation: %d\n", starvation_cases);
        safe_print("=====================================\n\n");
        pthread_mutex_unlock(&mutex_stats);
    }

    return NULL;
}

// Função para inicializar recursos
void inicializar_recursos() {
    // Inicializar semáforos das pistas
    for (int i = 0; i < NUM_PISTAS; i++) {
        sem_init(&pistas[i], 0, 1);
    }

    // Inicializar semáforos dos portões
    for (int i = 0; i < NUM_PORTOES; i++) {
        sem_init(&portoes[i], 0, 1);
    }

    // Inicializar semáforo da torre (máximo 2 operações simultâneas)
    sem_init(&torre, 0, MAX_TORRE_OPERACOES);
    sem_init(&torre_priority, 0, MAX_TORRE_OPERACOES);
}

// Função para gerar relatório final
void gerar_relatorio_final() {
    safe_print("\n🎯 ================= RELATÓRIO FINAL =================\n");
    safe_print("⏰ Tempo total de simulação: %d segundos\n", TEMPO_SIMULACAO);
    safe_print("✈️ Total de aviões criados: %d\n", total_avioes_criados);
    safe_print("✅ Aviões que completaram todas operações: %d\n", avioes_finalizados);
    safe_print("💥 Aviões que crasharam (starvation): %d\n", avioes_crashed);
    safe_print("⚠️ Total de casos de starvation: %d\n", starvation_cases);

    double taxa_sucesso = (total_avioes_criados > 0) ?
        (double)avioes_finalizados / total_avioes_criados * 100.0 : 0.0;
    safe_print("📈 Taxa de sucesso: %.2f%%\n", taxa_sucesso);

    safe_print("\n📋 CONFIGURAÇÃO DO AEROPORTO:\n");
    safe_print("🛫 Pistas disponíveis: %d\n", NUM_PISTAS);
    safe_print("🚪 Portões disponíveis: %d\n", NUM_PORTOES);
    safe_print("🗼 Operações simultâneas na torre: %d\n", MAX_TORRE_OPERACOES);

    safe_print("\n📊 ESTADO FINAL DOS AVIÕES:\n");
    pthread_mutex_lock(&mutex_avioes);

    int aguardando = 0, operando = 0;
    for (int i = 0; i < num_avioes; i++) {
        aviao_t* aviao = avioes[i];
        const char* tipo_str = (aviao->tipo == INTERNACIONAL) ? "INT" : "DOM";
        const char* estado_str;

        switch (aviao->estado) {
            case AGUARDANDO_POUSO: estado_str = "Aguardando Pouso"; aguardando++; break;
            case POUSANDO: estado_str = "Pousando"; operando++; break;
            case AGUARDANDO_DESEMBARQUE: estado_str = "Aguardando Desembarque"; aguardando++; break;
            case DESEMBARCANDO: estado_str = "Desembarcando"; operando++; break;
            case AGUARDANDO_DECOLAGEM: estado_str = "Aguardando Decolagem"; aguardando++; break;
            case DECOLANDO: estado_str = "Decolando"; operando++; break;
            case FINALIZADO: estado_str = "Finalizado"; break;
            case CRASHED: estado_str = "CRASHED"; break;
        }

        safe_print("Avião %d (%s): %s - Operações concluídas: %d/3\n",
                  aviao->id, tipo_str, estado_str, aviao->operacoes_concluidas);
    }

    pthread_mutex_unlock(&mutex_avioes);

    safe_print("\n📈 RESUMO DE ESTADOS:\n");
    safe_print("✅ Finalizados: %d\n", avioes_finalizados);
    safe_print("⏳ Aguardando recursos: %d\n", aguardando);
    safe_print("🔄 Em operação: %d\n", operando);
    safe_print("💥 Crashed: %d\n", avioes_crashed);

    if (avioes_crashed == 0) {
        safe_print("\n🎉 PARABÉNS! Nenhum avião crashou durante a simulação!\n");
    } else {
        safe_print("\n⚠️ ATENÇÃO: %d avião(ões) crasharam devido a starvation.\n", avioes_crashed);
        safe_print("💡 Considere ajustar os recursos ou implementar melhores políticas de prioridade.\n");
    }

    safe_print("=====================================================\n");
}

int main() {
    printf("🛫 === SISTEMA DE CONTROLE DE TRÁFEGO AÉREO ===\n");
    printf("Configuração: %d pistas, %d portões, %d operações simultâneas na torre\n",
           NUM_PISTAS, NUM_PORTOES, MAX_TORRE_OPERACOES);
    printf("Tempo de simulação: %d segundos\n", TEMPO_SIMULACAO);
    printf("Tempo crítico: %d segundos, Tempo de crash: %d segundos\n\n",
           TEMPO_CRITICO, TEMPO_QUEDA);

    // Inicializar recursos
    inicializar_recursos();

    // Criar threads auxiliares
    pthread_t thread_criador, thread_monitor_id;
    pthread_create(&thread_criador, NULL, thread_criador_avioes, NULL);
    pthread_create(&thread_monitor_id, NULL, thread_monitor, NULL);

    // Aguardar tempo de simulação
    sleep(TEMPO_SIMULACAO);

    // Parar criação de novos aviões
    simulacao_ativa = 0;
    safe_print("\n🛑 Tempo de simulação encerrado. Parando criação de novos aviões...\n");

    // Aguardar threads auxiliares
    pthread_join(thread_criador, NULL);
    pthread_join(thread_monitor_id, NULL);

    // Aguardar aviões em operação terminarem (máximo 30 segundos adicionais)
    safe_print("⏳ Aguardando aviões em operação terminarem...\n");
    sleep(30);

    // Gerar relatório final
    gerar_relatorio_final();

    // Limpar recursos
    pthread_mutex_lock(&mutex_avioes);
    for (int i = 0; i < num_avioes; i++) {
        free(avioes[i]);
    }
    pthread_mutex_unlock(&mutex_avioes);

    // Destruir semáforos
    for (int i = 0; i < NUM_PISTAS; i++) {
        sem_destroy(&pistas[i]);
    }
    for (int i = 0; i < NUM_PORTOES; i++) {
        sem_destroy(&portoes[i]);
    }
    sem_destroy(&torre);
    sem_destroy(&torre_priority);

    return 0;
}