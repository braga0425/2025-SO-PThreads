# Air Traffic Control Simulation at an International Airport

This project implements a simulation of air traffic control at an
international airport, using C threads (POSIX Threads) and semaphores.
The system concurrently manages limited resources (runways, gates, and
the control tower), handling domestic and international flights with
different operating rules and priority mechanisms, preventing deadlocks
and starvation.

## Features

-   Dynamic creation of airplanes (threads) at random intervals;
-   Management of shared resources:
    -   **3 runways** for landings and takeoffs;
    -   **5 gates** for boarding/disembarking;
    -   **1 control tower** with up to 2 simultaneous operations.
-   **Deadlock Prevention:** resources allocated atomically;
-   **Starvation Prevention:** adaptive priority system and critical state handling;
-   **Intermediate** and **final** reports with simulation statistics;
-   Handling of extreme cases: go-arounds and crashes.

## Simulation Rules

Each airplane performs 3 sequential operations:
1. **Landing;**
2. **Disembarking;**
3. **Takeoff.**
\
Flight differences:
- **International flights:** receive higher initial priority;
- **Domestic flights:** priority increases with waiting time.

Possible airplane states:
- Waiting / Landing / Disembarking / Taking off;
- Finished;
- Go-around;
- Crashed (due to starvation).

## Code Structure

-   **Main threads:**
    -   `thread_criador_avioes`: periodically creates new airplanes;
    -   `thread_aviao`: executes the sequence of operations for each
        airplane;
    -   `thread_monitor`: generates intermediate reports.
-   **Resource modules:**
    -   Semaphores for runways and gates;
    -   Tower control via mutex + condition variable.
-   **Concurrency management:**
    -   Priority queue (`fila_requisicoes`);
    -   Function `alocar_recursos_atomicos`: ensures safe allocation;
    -   Timeout mechanisms, critical state, and crash handling.

## Compilation and Execution

Prerequisites:
- **C Compiler (GCC);**
- **POSIX Threads and Semaphore libraries** (already included in most
Linux systems).

### Compilation

``` bash
gcc -o airport_control airport_control.c -pthread
```

### Execution

``` bash
./airport_control
```

During the simulation, the terminal will display:
- Real-time logs about each airplane operation;
- Critical event messages (critical state, crashes, go-arounds);
- Periodic performance reports.

## Reports

### Intermediate (every 10s):

-   Number of airplanes created, finished, crashed, and go-arounds;
-   Starvation cases;
-   Size of the request queue.

### Final:

-   Global simulation statistics;
-   Success rate;
-   Final state of each airplane;
-   Summary of resources and policies used.

# [PT-BR] Simulação de Controle de Tráfego Aéreo em Aeroporto Internacional

Este projeto implementa uma simulação de controle de tráfego aéreo em um aeroporto internacional, utilizando threads em C (POSIX Threads) e semáforos. O sistema gerencia de forma concorrente recursos limitados (pistas, portões e torre de controle), tratando voos domésticos e internacionais com diferentes regras de operação e mecanismos de prioridade, prevenindo deadlocks e starvation.

## Funcionalidades

- Criação dinâmica de aviões (threads) em intervalos aleatórios;
- Gerenciamento de recursos compartilhados:
  - **3 pistas** para pousos e decolagens;
  - **5 portões** para embarque/desembarque;
  - **1 torre de controle** com até 2 operações simultâneas.
- **Prevenção de Deadlock:** recursos alocados de forma atômica;
- **Prevenção de Starvation:** sistema de prioridades adaptativas e estado crítico;
- Relatórios **intermediários** e **final** com estatísticas da simulação;
- Tratamento de casos extremos: arremetida e queda de aviões.

## Regras da Simulação

Cada avião executa 3 operações sequenciais:
1. **Pouso;**
2. **Desembarque;**
3. **Decolagem.**
\
Diferença entre voos:
- **Internacionais:** recebem prioridade inicial maior;
- **Domésticos:** prioridade cresce ao longo do tempo de espera.

Estados possíveis de um avião:
- Aguardando / Pousando / Desembarcando / Decolando;
- Finalizado;
- Arremeteu;
- Caiu (por starvation).

## Estrutura do Código

- **Threads principais:**
  - `thread_criador_avioes`: cria novos aviões periodicamente;
  - `thread_aviao`: executa a sequência de operações de cada avião;
  - `thread_monitor`: gera relatórios intermediários.
- **Módulos de recurso:**
  - Semáforos para pistas e portões;
  - Controle da torre via mutex + variável de condição.
- **Gerenciamento de concorrência:**
  - Fila de prioridades (`fila_requisicoes`);
  - Função `alocar_recursos_atomicos`: garante alocação segura;
  - Mecanismos de timeout, estado crítico e queda.

## Compilação e Execução

Pré-requisitos:
- **Compilador C (GCC);**  
- **Bibliotecas POSIX Threads e Semáforos** (já presentes na maioria dos Linux)

### Compilação
```bash
gcc -o airport_control airport_control.c -pthread
```

### Execução
```bash
./airport_control
```

Durante a simulação, o terminal exibirá:
- Logs em tempo real sobre cada operação dos aviões;
- Mensagens de eventos críticos (estado crítico, quedas, arremetidas);
- Relatórios periódicos de desempenho.

## Relatórios

### Intermediário (a cada 10s):
- Número de aviões criados, finalizados, caídos e arremetidos;
- Casos de starvation;
- Tamanho da fila de requisições.

### Final:
- Estatísticas globais da simulação;
- Taxa de sucesso;
- Estado final de cada avião;
- Resumo dos recursos e políticas usadas.
