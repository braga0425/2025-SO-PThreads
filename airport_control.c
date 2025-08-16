#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>

// configurações do aeroporto
#define NUM_PISTAS 3
#define NUM_PORTOES 5
#define MAX_TORRE_OPERACOES 2
#define TEMPO_SIMULACAO 300  // 5 minutos em segundos
#define TEMPO_CRITICO 60     // 60 segundos para estado crítico
#define TEMPO_QUEDA 90       // 90 segundos para queda
#define MAX_TENTATIVAS 10    // máximo de tentativas antes de arremeter

// estados do avião
typedef enum {
    AGUARDANDO_POUSO,
    POUSANDO,
    AGUARDANDO_DESEMBARQUE,
    DESEMBARCANDO,
    AGUARDANDO_DECOLAGEM,
    DECOLANDO,
    FINALIZADO,
    CAIU,
    ARREMETEU
} estado_aviao_t;

// tipos de voo
typedef enum {
    DOMESTICO,
    INTERNACIONAL
} tipo_voo_t;

// estrutura para requisição de recursos
typedef struct {
    int aviao_id;
    int prioridade;
    int tentativas;
    struct timeval timestamp;
    pthread_cond_t cond;
    int recursos_alocados;
    int pista_alocada;
    int portao_alocado;
    int torre_alocada;
} requisicao_t;

// estrutura do avião
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
    int prioridade;
    int tentativas_totais;
} aviao_t;

// recursos do aeroporto
sem_t pistas[NUM_PISTAS];
sem_t portoes[NUM_PORTOES];

// sistema de controle da torre com prioridade
pthread_mutex_t mutex_torre = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_torre = PTHREAD_COND_INITIALIZER;
int torre_livre = MAX_TORRE_OPERACOES;
int esperando_critico = 0;

// sistema de alocação de recursos com prioridade
pthread_mutex_t mutex_recursos = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond_recursos = PTHREAD_COND_INITIALIZER;

// fila de prioridades para requisições
#define MAX_REQUISICOES 1000
requisicao_t* fila_requisicoes[MAX_REQUISICOES];
int num_requisicoes = 0;

// mutexes para proteção
pthread_mutex_t mutex_print = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_stats = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t mutex_critico = PTHREAD_MUTEX_INITIALIZER;

// controle da simulação
volatile int simulacao_ativa = 1;
volatile int proximo_id = 1;

// estatísticas
int total_avioes_criados = 0;
int avioes_finalizados = 0;
int avioes_caidos = 0;
int avioes_arremetidos = 0;
int deadlocks_detectados = 0;
int starvation_cases = 0;

// lista de aviões para monitoramento
aviao_t *avioes[1000];
int num_avioes = 0;
pthread_mutex_t mutex_avioes = PTHREAD_MUTEX_INITIALIZER;

// função para obter tempo atual em ms
long long get_time_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// função para print thread-safe
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

// função para comparar prioridades (maior prioridade primeiro)
int comparar_prioridade(const void* a, const void* b) {
    requisicao_t* req_a = *(requisicao_t**)a;
    requisicao_t* req_b = *(requisicao_t**)b;

    // primeiro por prioridade (maior primeiro)
    if (req_a->prioridade != req_b->prioridade) {
        return req_b->prioridade - req_a->prioridade;
    }

    // se prioridades iguais, por timestamp (mais antigo primeiro)
    if (req_a->timestamp.tv_sec != req_b->timestamp.tv_sec) {
        return req_a->timestamp.tv_sec - req_b->timestamp.tv_sec;
    }

    return req_a->timestamp.tv_usec - req_b->timestamp.tv_usec;
}

// função para inserir requisição na fila de prioridades
void inserir_requisicao(requisicao_t* req) {
    pthread_mutex_lock(&mutex_recursos);

    if (num_requisicoes < MAX_REQUISICOES) {
        fila_requisicoes[num_requisicoes] = req;
        num_requisicoes++;

        // ordenar fila por prioridade
        qsort(fila_requisicoes, num_requisicoes, sizeof(requisicao_t*), comparar_prioridade);
    }

    pthread_mutex_unlock(&mutex_recursos);
}

// função para remover requisição da fila
void remover_requisicao(int aviao_id) {
    pthread_mutex_lock(&mutex_recursos);

    for (int i = 0; i < num_requisicoes; i++) {
        if (fila_requisicoes[i]->aviao_id == aviao_id) {
            // remover da fila
            for (int j = i; j < num_requisicoes - 1; j++) {
                fila_requisicoes[j] = fila_requisicoes[j + 1];
            }
            num_requisicoes--;
            break;
        }
    }

    pthread_mutex_unlock(&mutex_recursos);
}

// função para calcular tempo de espera
int calcular_tempo_espera(aviao_t *aviao) {
    struct timeval agora;
    gettimeofday(&agora, NULL);
    return (agora.tv_sec - aviao->inicio_espera.tv_sec);
}

// função para verificar estado crítico
int verificar_estado_critico(aviao_t *aviao) {
    int tempo_espera = calcular_tempo_espera(aviao);

    if (tempo_espera >= TEMPO_QUEDA) {
        pthread_mutex_lock(&mutex_stats);
        avioes_caidos++;
        starvation_cases++;
        pthread_mutex_unlock(&mutex_stats);

        aviao->estado = CAIU;
        safe_print("💥 AVIÃO %d (%s) CAIU por starvation após %d segundos!\n",
                  aviao->id, (aviao->tipo == INTERNACIONAL) ? "INT" : "DOM", tempo_espera);

        return 0; // caiu
    } else if (tempo_espera >= TEMPO_CRITICO && !aviao->em_estado_critico) {
        pthread_mutex_lock(&mutex_critico);
        aviao->em_estado_critico = 1;
        aviao->prioridade += 5; // incremento maior para estado crítico
        pthread_mutex_unlock(&mutex_critico);

        safe_print("⚠️ AVIÃO %d (%s) entrou em ESTADO CRÍTICO após %d segundos! Prioridade: %d\n",
                  aviao->id, (aviao->tipo == INTERNACIONAL) ? "INT" : "DOM",
                  tempo_espera, aviao->prioridade);
    }

    return 1; // ok
}

// função para resetar cronômetro
void resetar_cronometro(aviao_t *aviao) {
    gettimeofday(&aviao->inicio_espera, NULL);
    aviao->em_estado_critico = 0;
}

// função para verificar disponibilidade de recursos
int verificar_recursos_disponiveis(int precisa_pista, int precisa_portao, int precisa_torre,
                                   int* pista_disponivel, int* portao_disponivel) {
    int recursos_ok = 1;

    if (precisa_pista) {
        *pista_disponivel = -1;
        for (int i = 0; i < NUM_PISTAS; i++) {
            int val;
            sem_getvalue(&pistas[i], &val);
            if (val > 0) {
                *pista_disponivel = i;
                break;
            }
        }
        if (*pista_disponivel == -1) recursos_ok = 0;
    }

    if (precisa_portao) {
        *portao_disponivel = -1;
        for (int i = 0; i < NUM_PORTOES; i++) {
            int val;
            sem_getvalue(&portoes[i], &val);
            if (val > 0) {
                *portao_disponivel = i;
                break;
            }
        }
        if (*portao_disponivel == -1) recursos_ok = 0;
    }

    if (precisa_torre && torre_livre <= 0) {
        recursos_ok = 0;
    }

    return recursos_ok;
}

// função para alocar todos os recursos de uma vez (evita deadlock)
int alocar_recursos_atomicos(aviao_t *aviao, int precisa_pista, int precisa_portao,
                            int precisa_torre, int* pista_alocada, int* portao_alocado) {

    requisicao_t req;
    req.aviao_id = aviao->id;
    req.prioridade = aviao->prioridade;
    req.tentativas = aviao->tentativas_totais;
    gettimeofday(&req.timestamp, NULL);
    pthread_cond_init(&req.cond, NULL);
    req.recursos_alocados = 0;
    req.pista_alocada = -1;
    req.portao_alocado = -1;
    req.torre_alocada = 0;

    inserir_requisicao(&req);

    pthread_mutex_lock(&mutex_recursos);

    int tentativas_locais = 0;

    while (!req.recursos_alocados && tentativas_locais < MAX_TENTATIVAS) {
        // verificar se é a vez desta requisição (primeira na fila)
        if (num_requisicoes > 0 && fila_requisicoes[0]->aviao_id == aviao->id) {

            int pista_disp = -1, portao_disp = -1;

            // verificar se todos os recursos necessários estão disponíveis
            if (verificar_recursos_disponiveis(precisa_pista, precisa_portao, precisa_torre,
                                             &pista_disp, &portao_disp)) {

                // alocar todos os recursos atomicamente
                int sucesso = 1;

                if (precisa_pista && sem_trywait(&pistas[pista_disp]) != 0) {
                    sucesso = 0;
                }

                if (sucesso && precisa_portao && sem_trywait(&portoes[portao_disp]) != 0) {
                    if (precisa_pista) sem_post(&pistas[pista_disp]);
                    sucesso = 0;
                }

                if (sucesso && precisa_torre) {
                    if (torre_livre > 0) {
                        torre_livre--;
                    } else {
                        if (precisa_pista) sem_post(&pistas[pista_disp]);
                        if (precisa_portao) sem_post(&portoes[portao_disp]);
                        sucesso = 0;
                    }
                }

                if (sucesso) {
                    req.recursos_alocados = 1;
                    req.pista_alocada = pista_disp;
                    req.portao_alocado = portao_disp;
                    req.torre_alocada = precisa_torre;

                    *pista_alocada = pista_disp;
                    *portao_alocado = portao_disp;

                    safe_print("🔒 Avião %d ALOCOU recursos atomicamente: Pista=%d, Portão=%d, Torre=%d\n",
                              aviao->id, pista_disp, portao_disp, precisa_torre);
                    break;
                }
            }
        }

        // se não conseguiu recursos, incrementar prioridade e tentativas
        if (!req.recursos_alocados) {
            tentativas_locais++;
            aviao->tentativas_totais++;
            aviao->prioridade++;
            req.prioridade = aviao->prioridade;
            req.tentativas = aviao->tentativas_totais;

            // reordenar fila com nova prioridade
            qsort(fila_requisicoes, num_requisicoes, sizeof(requisicao_t*), comparar_prioridade);

            safe_print("🔄 Avião %d FALHOU em obter recursos (tentativa %d/%d). Nova prioridade: %d\n",
                      aviao->id, tentativas_locais, MAX_TENTATIVAS, aviao->prioridade);

            // verificar se ainda está dentro dos limites de tempo
            if (!verificar_estado_critico(aviao)) {
                break; // caiu
            }

            // esperar um pouco antes de tentar novamente
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 2;

            pthread_cond_timedwait(&cond_recursos, &mutex_recursos, &timeout);
        }
    }

    pthread_mutex_unlock(&mutex_recursos);

    // remover da fila
    remover_requisicao(aviao->id);
    pthread_cond_destroy(&req.cond);

    // verificar se desistiu após muitas tentativas
    if (tentativas_locais >= MAX_TENTATIVAS && !req.recursos_alocados) {
        aviao->estado = ARREMETEU;
        pthread_mutex_lock(&mutex_stats);
        avioes_arremetidos++;
        pthread_mutex_unlock(&mutex_stats);

        safe_print("✈️ Avião %d ARREMETEU após %d tentativas sem sucesso!\n",
                  aviao->id, tentativas_locais);
        return 0;
    }

    // notificar outras threads que recursos podem estar disponíveis
    pthread_cond_broadcast(&cond_recursos);

    return req.recursos_alocados;
}

// função para liberar recursos atomicamente
void liberar_recursos_atomicos(int pista, int portao, int torre) {
    pthread_mutex_lock(&mutex_recursos);

    if (pista >= 0) {
        sem_post(&pistas[pista]);
    }

    if (portao >= 0) {
        sem_post(&portoes[portao]);
    }

    if (torre) {
        torre_livre++;
    }

    // acordar threads esperando por recursos
    pthread_cond_broadcast(&cond_recursos);

    pthread_mutex_unlock(&mutex_recursos);
}

// função para operação de pouso
int realizar_pouso(aviao_t *aviao) {
    safe_print("🛬 Avião %d (%s) iniciando procedimento de POUSO!\n",
              aviao->id, (aviao->tipo == INTERNACIONAL) ? "INT" : "DOM");

    aviao->estado = AGUARDANDO_POUSO;
    resetar_cronometro(aviao);

    int pista_alocada = -1, portao_alocado = -1;

    // alocar recursos necessários para pouso (pista + torre)
    if (!alocar_recursos_atomicos(aviao, 1, 0, 1, &pista_alocada, &portao_alocado)) {
        return 0; // falhou ou arremeteu
    }

    if (aviao->estado == CAIU || aviao->estado == ARREMETEU) {
        return 0;
    }

    aviao->pista_alocada = pista_alocada;

    // realizar pouso
    aviao->estado = POUSANDO;
    safe_print("🛬 Avião %d POUSANDO na pista %d!\n", aviao->id, pista_alocada);

    sleep(2); // tempo de pouso

    // liberar recursos do pouso
    liberar_recursos_atomicos(pista_alocada, -1, 1);
    aviao->pista_alocada = -1;

    safe_print("✅  Avião %d POUSOU com sucesso! Pista %d e torre liberadas!\n",
              aviao->id, pista_alocada);

    return 1;
}

// função para operação de desembarque
int realizar_desembarque(aviao_t *aviao) {
    safe_print("💺 Avião %d iniciando procedimento de DESEMBARQUE!\n", aviao->id);

    aviao->estado = AGUARDANDO_DESEMBARQUE;
    resetar_cronometro(aviao);

    int pista_alocada = -1, portao_alocado = -1;

    // alocar recursos necessários para desembarque (portão + torre)
    if (!alocar_recursos_atomicos(aviao, 0, 1, 1, &pista_alocada, &portao_alocado)) {
        return 0; // falhou ou arremeteu
    }

    if (aviao->estado == CAIU || aviao->estado == ARREMETEU) {
        return 0;
    }

    aviao->portao_alocado = portao_alocado;

    // realizar desembarque
    aviao->estado = DESEMBARCANDO;
    safe_print("💺 Avião %d DESEMBARCANDO no portão %d!\n", aviao->id, portao_alocado);

    sleep(3); // tempo de desembarque

    // liberar apenas a torre, manter portão para decolagem
    liberar_recursos_atomicos(-1, -1, 1);

    safe_print("✅  Avião %d DESEMBARCOU com sucesso! Mantendo portão %d para decolagem!\n",
              aviao->id, portao_alocado);

    return 1;
}

// função para operação de decolagem
int realizar_decolagem(aviao_t *aviao) {
    safe_print("🛫 Avião %d iniciando procedimento de DECOLAGEM!\n", aviao->id);

    aviao->estado = AGUARDANDO_DECOLAGEM;
    resetar_cronometro(aviao);

    int pista_alocada = -1, portao_dummy = -1;

    // alocar recursos necessários para decolagem (pista + torre)
    // portão já está alocado do desembarque
    if (!alocar_recursos_atomicos(aviao, 1, 0, 1, &pista_alocada, &portao_dummy)) {
        // liberar portão se falhou
        if (aviao->portao_alocado >= 0) {
            liberar_recursos_atomicos(-1, aviao->portao_alocado, 0);
        }
        return 0; // falhou ou arremeteu
    }

    if (aviao->estado == CAIU || aviao->estado == ARREMETEU) {
        if (aviao->portao_alocado >= 0) {
            liberar_recursos_atomicos(-1, aviao->portao_alocado, 0);
        }
        return 0;
    }

    aviao->pista_alocada = pista_alocada;

    // realizar decolagem
    aviao->estado = DECOLANDO;
    safe_print("🛫 Avião %d DECOLANDO da pista %d, partindo do portão %d!\n",
              aviao->id, pista_alocada, aviao->portao_alocado);

    sleep(2); // tempo de decolagem

    // liberar todos os recursos
    liberar_recursos_atomicos(pista_alocada, aviao->portao_alocado, 1);
    aviao->portao_alocado = -1;
    aviao->pista_alocada = -1;

    safe_print("🎉 Avião %d DECOLOU com sucesso! Todos os recursos liberados!\n", aviao->id);

    return 1;
}

// função principal da thread do avião
void* thread_aviao(void* arg) {
    aviao_t* aviao = (aviao_t*)arg;

    aviao->pista_alocada = -1;
    aviao->portao_alocado = -1;
    aviao->operacoes_concluidas = 0;
    aviao->em_estado_critico = 0;
    aviao->prioridade = 0;
    aviao->tentativas_totais = 0;

    safe_print("🆕 Avião %d (%s) chegou ao aeroporto!\n",
              aviao->id, (aviao->tipo == INTERNACIONAL) ? "INTERNACIONAL" : "DOMÉSTICO");

    // operação 1: pouso
    if (realizar_pouso(aviao)) {
        aviao->operacoes_concluidas++;

        // operação 2: desembarque
        if (realizar_desembarque(aviao)) {
            aviao->operacoes_concluidas++;

            // operação 3: decolagem
            if (realizar_decolagem(aviao)) {
                aviao->operacoes_concluidas++;
                aviao->estado = FINALIZADO;

                pthread_mutex_lock(&mutex_stats);
                avioes_finalizados++;
                pthread_mutex_unlock(&mutex_stats);

                safe_print("🏆 Avião %d CONCLUIU todas as operações com sucesso!\n", aviao->id);
            }
        }
    }

    return NULL;
}

// thread para criar aviões periodicamente
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

            // criar thread do avião
            pthread_create(&novo_aviao->thread, NULL, thread_aviao, novo_aviao);
            pthread_detach(novo_aviao->thread);
        }

        // intervalo randômico entre 1 e 5 segundos
        sleep(1 + rand() % 5);
    }

    return NULL;
}

// thread para monitorar sistema
void* thread_monitor(void* arg) {
    while (simulacao_ativa) {
        sleep(10); // monitora a cada 10 segundos

        pthread_mutex_lock(&mutex_stats);
        safe_print("================================\n");
        safe_print("📊 RELATÓRIO INTERMEDIÁRIO\n");
        safe_print("Aviões criados: %d\n", total_avioes_criados);
        safe_print("Aviões finalizados: %d\n", avioes_finalizados);
        safe_print("Aviões caídos: %d\n", avioes_caidos);
        safe_print("Aviões que arremeteram: %d\n", avioes_arremetidos);
        safe_print("Casos de starvation: %d\n", starvation_cases);
        safe_print("Requisições na fila: %d\n", num_requisicoes);
        safe_print("================================\n");
        pthread_mutex_unlock(&mutex_stats);
    }

    return NULL;
}

// função para inicializar recursos
void inicializar_recursos() {
    // inicializar semáforos das pistas
    for (int i = 0; i < NUM_PISTAS; i++) {
        sem_init(&pistas[i], 0, 1);
    }

    // inicializar semáforos dos portões
    for (int i = 0; i < NUM_PORTOES; i++) {
        sem_init(&portoes[i], 0, 1);
    }
}

// função para gerar relatório final
void gerar_relatorio_final() {
    safe_print("================================\n");
    safe_print("🎯 RELATÓRIO FINAL\n");
    safe_print("⏰  Tempo total de simulação: %d segundos\n", TEMPO_SIMULACAO);
    safe_print("✈️ Total de aviões criados: %d\n", total_avioes_criados);
    safe_print("✅  Aviões que completaram todas operações: %d\n", avioes_finalizados);
    safe_print("💥 Aviões que caíram (starvation): %d\n", avioes_caidos);
    safe_print("🛫 Aviões que arremetaram: %d\n", avioes_arremetidos);
    safe_print("⚠️ Total de casos de starvation: %d\n", starvation_cases);
    safe_print("================================\n");

    double taxa_sucesso = (total_avioes_criados > 0) ?
        (double)avioes_finalizados / total_avioes_criados * 100.0 : 0.0;
    safe_print("📈 Taxa de sucesso: %.2f%%\n", taxa_sucesso);

    safe_print("================================\n");
    safe_print("📋 CONFIGURAÇÃO DO AEROPORTO:\n");
    safe_print("🛣️Pistas disponíveis: %d\n", NUM_PISTAS);
    safe_print("🚪 Portões disponíveis: %d\n", NUM_PORTOES);
    safe_print("🗼 Operações simultâneas na torre: %d\n", MAX_TORRE_OPERACOES);
    safe_print("🔄 Máximo de tentativas por avião: %d\n", MAX_TENTATIVAS);
    safe_print("================================\n");

    safe_print("📊 ESTADO FINAL DOS AVIÕES:\n");
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
            case CAIU: estado_str = "Caiu"; break;
            case ARREMETEU: estado_str = "Arremeteu"; break;
        }

        safe_print("Avião %d (%s): %s - Operações concluídas: %d/3 - Prioridade: %d - Tentativas: %d\n",
                  aviao->id, tipo_str, estado_str, aviao->operacoes_concluidas,
                  aviao->prioridade, aviao->tentativas_totais);
    }

    pthread_mutex_unlock(&mutex_avioes);

    safe_print("================================\n");
    safe_print("📈 RESUMO DE ESTADOS:\n");
    safe_print("✅  Finalizados: %d\n", avioes_finalizados);
    safe_print("⏳  Aguardando recursos: %d\n", aguardando);
    safe_print("🔄 Em operação: %d\n", operando);
    safe_print("💥 Caídos: %d\n", avioes_caidos);
    safe_print("🛫 Arremeteram: %d\n", avioes_arremetidos);

    int total_problemas = avioes_caidos + avioes_arremetidos;
    if (total_problemas == 0) {
        safe_print("🎉 PARABÉNS! Nenhum avião caiu ou arremeteu durante a simulação!\n");
    } else {
        safe_print("⚠️ ATENÇÃO: %d avião(ões) tiveram problemas (%d caídos + %d arremetidos).\n",
                  total_problemas, avioes_caidos, avioes_arremetidos);
        safe_print("================================\n");
    }
}

int main() {
    printf("🛫 Simulação de Controle de Tráfego Aéreo em Aeroporto Internacional");
    printf("\n\nConfiguração: %d Pistas, %d Portões, %d Operações Simultâneas na Torre\n",
           NUM_PISTAS, NUM_PORTOES, MAX_TORRE_OPERACOES);
    printf("Tempo de Simulação: %d segundos\n", TEMPO_SIMULACAO);
    printf("Tempo Crítico: %d segundos, Tempo para Queda: %d segundos\n",
           TEMPO_CRITICO, TEMPO_QUEDA);
    printf("Máximo de tentativas por Avião: %d\n\n", MAX_TENTATIVAS);

    // inicializar recursos
    inicializar_recursos();

    // criar threads auxiliares
    pthread_t thread_criador, thread_monitor_id;
    pthread_create(&thread_criador, NULL, thread_criador_avioes, NULL);
    pthread_create(&thread_monitor_id, NULL, thread_monitor, NULL);

    // aguardar tempo de simulação
    sleep(TEMPO_SIMULACAO);

    // parar criação de novos aviões
    simulacao_ativa = 0;
    safe_print("\n🛑 Tempo de simulação encerrado. Parando criação de novos aviões...\n");

    // aguardar threads auxiliares
    pthread_join(thread_criador, NULL);
    pthread_join(thread_monitor_id, NULL);

    // aguardar aviões em operação terminarem (máximo 30 segundos adicionais)
    safe_print("⏳ Aguardando aviões em operação terminarem...\n");
    sleep(30);

    // gerar relatório final
    gerar_relatorio_final();

    // limpar recursos
    pthread_mutex_lock(&mutex_avioes);
    for (int i = 0; i < num_avioes; i++) {
        free(avioes[i]);
    }
    pthread_mutex_unlock(&mutex_avioes);

    // destruir semáforos
    for (int i = 0; i < NUM_PISTAS; i++) {
        sem_destroy(&pistas[i]);
    }
    for (int i = 0; i < NUM_PORTOES; i++) {
        sem_destroy(&portoes[i]);
    }

    // destruir mutexes
    pthread_mutex_destroy(&mutex_torre);
    pthread_mutex_destroy(&mutex_recursos);
    pthread_mutex_destroy(&mutex_print);
    pthread_mutex_destroy(&mutex_stats);
    pthread_mutex_destroy(&mutex_critico);
    pthread_mutex_destroy(&mutex_avioes);

    // destruir condition variables
    pthread_cond_destroy(&cond_torre);
    pthread_cond_destroy(&cond_recursos);

    printf("\n🎯 Simulação concluída com sucesso!\n");

    return 0;
}