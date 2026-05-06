/*
 * Questão 1 – Comparação RR vs SRTF
 *
 * Compila:  gcc -o sched main.c -lm
 * Executa:  ./sched input.json
 *
 * Regras implementadas:
 *  - Custo de 1 tick sempre que a CPU muda de processo (incluindo preempções).
 *  - Empates resolvidos aleatoriamente com semente fixa (SEED 42).
 *  - Métricas: tempo médio de resposta (±std), tempo médio de retorno (±std), vazão.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ── tunables ─────────────────────────────────────────────────────────────── */
#define MAX_PROC      64
#define MAX_TIMELINE  100000
#define CONTEXT_COST  1
#define SEED          42
#define THROUGHPUT_T  100

/* ── simple JSON helpers (hand-rolled, no external deps) ──────────────────── */
static int parse_int_field(const char *buf, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    while (*p == ':' || *p == ' ') p++;
    return atoi(p);
}

static void parse_string_field(const char *buf, const char *key, char *out, int sz) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(buf, pat);
    if (!p) { out[0] = '\0'; return; }
    p = strchr(p, ':');
    if (!p) { out[0] = '\0'; return; }
    while (*p == ':' || *p == ' ') p++;
    if (*p == '"') p++;
    int i = 0;
    while (*p && *p != '"' && i < sz-1) out[i++] = *p++;
    out[i] = '\0';
}

/* ── process descriptor ───────────────────────────────────────────────────── */
typedef struct {
    char pid[16];
    int  arrival;
    int  burst;
    /* runtime fields (reset per simulation) */
    int  remaining;
    int  first_run;   /* tick when first scheduled (-1 = not yet) */
    int  finish;
    int  done;
} Process;

/* ── timeline entry ───────────────────────────────────────────────────────── */
typedef struct { int tick; char pid[16]; } TLEntry;

/* ── RNG helpers ──────────────────────────────────────────────────────────── */
static unsigned rng_state;
static void rng_seed(unsigned s) { rng_state = s; }
/* LCG */
static unsigned rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}
/* pick random index in [0, n) */
static int rng_pick(int n) { return (int)(rng_next() % (unsigned)n); }

/* ── reset runtime fields ─────────────────────────────────────────────────── */
static void reset_procs(Process *procs, int n) {
    for (int i = 0; i < n; i++) {
        procs[i].remaining = procs[i].burst;
        procs[i].first_run = -1;
        procs[i].finish    = -1;
        procs[i].done      = 0;
    }
}

/* ── metrics ──────────────────────────────────────────────────────────────── */
typedef struct {
    double avg_response, std_response;
    double avg_turnaround, std_turnaround;
    double throughput;          /* processes finished in [0, THROUGHPUT_T] */
} Metrics;

static Metrics compute_metrics(Process *procs, int n) {
    double sum_r = 0, sum_r2 = 0;
    double sum_t = 0, sum_t2 = 0;
    int    tp = 0;
    for (int i = 0; i < n; i++) {
        double r = procs[i].first_run - procs[i].arrival;
        double t = procs[i].finish    - procs[i].arrival;
        sum_r  += r;  sum_r2 += r*r;
        sum_t  += t;  sum_t2 += t*t;
        if (procs[i].finish <= THROUGHPUT_T) tp++;
    }
    Metrics m;
    m.avg_response   = sum_r  / n;
    m.std_response   = sqrt(sum_r2/n - m.avg_response*m.avg_response);
    m.avg_turnaround = sum_t  / n;
    m.std_turnaround = sqrt(sum_t2/n - m.avg_turnaround*m.avg_turnaround);
    m.throughput     = (double)tp / THROUGHPUT_T;
    return m;
}

/* ── print timeline ───────────────────────────────────────────────────────── */
static void print_timeline(TLEntry *tl, int len) {
    printf("  Sequência de execução:\n  ");
    for (int i = 0; i < len; i++) {
        if (i > 0 && tl[i].tick != tl[i-1].tick + 1)
            printf(" ");
        /* collapse consecutive same-pid entries */
        if (i == 0 || strcmp(tl[i].pid, tl[i-1].pid) != 0)
            printf("[t%d %s", tl[i].tick, tl[i].pid);
        if (i+1 == len || strcmp(tl[i].pid, tl[i+1].pid) != 0)
            printf("]");
    }
    printf("\n");
}

/* ── RR simulation ────────────────────────────────────────────────────────── */
/*
 * ready queue: circular array of process indices
 * context-switch tick: when current != next, burn 1 tick doing nothing
 */
static int rr_simulate(Process *procs, int n, int quantum,
                        TLEntry *tl, int *tl_len)
{
    rng_seed(SEED);
    reset_procs(procs, n);

    /* simple queue */
    int queue[MAX_PROC * 200];
    int qhead = 0, qtail = 0;
#define QPUSH(x) (queue[qtail++ % (MAX_PROC*200)] = (x))
#define QPOP()   (queue[qhead++ % (MAX_PROC*200)])
#define QSIZE()  (qtail - qhead)

    int tick = 0, done = 0, tl_idx = 0;
    int in_queue[MAX_PROC] = {0};  /* avoid duplicate enqueue */
    int current = -1;              /* index of running process */
    int time_in_slice = 0;
    int context_switching = 0;     /* ticks remaining in context switch */
    int next_proc = -1;            /* process that will run after ctx switch */

    /* enqueue all processes that arrive at tick 0 */
    for (int i = 0; i < n; i++) {
        if (procs[i].arrival == 0) {
            QPUSH(i);
            in_queue[i] = 1;
        }
    }

    while (done < n) {
        /* admit newly arrived processes */
        for (int i = 0; i < n; i++) {
            if (!procs[i].done && !in_queue[i] && procs[i].arrival == tick) {
                QPUSH(i);
                in_queue[i] = 1;
            }
        }

        if (context_switching > 0) {
            /* burn context-switch tick */
            tl[tl_idx].tick = tick;
            strcpy(tl[tl_idx].pid, "CTX");
            tl_idx++;
            context_switching--;
            tick++;
            /* admit arrivals during ctx switch */
            for (int i = 0; i < n; i++) {
                if (!procs[i].done && !in_queue[i] && procs[i].arrival == tick) {
                    QPUSH(i);
                    in_queue[i] = 1;
                }
            }
            if (context_switching == 0) {
                current       = next_proc;
                time_in_slice = 0;
                next_proc     = -1;
            }
            continue;
        }

        if (current == -1) {
            /* pick from queue */
            if (QSIZE() == 0) {
                /* idle */
                tl[tl_idx].tick = tick;
                strcpy(tl[tl_idx].pid, "IDLE");
                tl_idx++;
                tick++;
                continue;
            }
            current       = QPOP();
            in_queue[current] = 0;
            time_in_slice = 0;
        }

        /* record first run */
        if (procs[current].first_run == -1)
            procs[current].first_run = tick;

        /* execute one tick */
        tl[tl_idx].tick = tick;
        strcpy(tl[tl_idx].pid, procs[current].pid);
        tl_idx++;
        procs[current].remaining--;
        time_in_slice++;
        tick++;

        /* admit new arrivals */
        for (int i = 0; i < n; i++) {
            if (!procs[i].done && !in_queue[i] && procs[i].arrival == tick) {
                QPUSH(i);
                in_queue[i] = 1;
            }
        }

        if (procs[current].remaining == 0) {
            /* finished */
            procs[current].finish = tick;
            procs[current].done   = 1;
            done++;
            int old = current;
            current = -1;
            /* if there is something in queue, pay ctx switch */
            if (QSIZE() > 0) {
                next_proc         = QPOP();
                in_queue[next_proc] = 0;
                context_switching = CONTEXT_COST;
            }
            (void)old;
        } else if (time_in_slice == quantum) {
            /* quantum expired – preempt */
            int old = current;
            QPUSH(current);
            in_queue[current] = 1;
            current = -1;
            if (QSIZE() > 0) {
                next_proc         = QPOP();
                in_queue[next_proc] = 0;
                if (next_proc != old) {
                    context_switching = CONTEXT_COST;
                } else {
                    /* same process comes back – no ctx switch cost */
                    current       = next_proc;
                    next_proc     = -1;
                    time_in_slice = 0;
                }
            }
        }

        if (tl_idx >= MAX_TIMELINE - 2) break;
    }
    *tl_len = tl_idx;
    return tick;
}

/* ── SRTF simulation ──────────────────────────────────────────────────────── */
static int srtf_simulate(Process *procs, int n,
                         TLEntry *tl, int *tl_len)
{
    rng_seed(SEED);
    reset_procs(procs, n);

    int tick = 0, done = 0, tl_idx = 0;
    int current = -1;
    int context_switching = 0;
    int next_proc = -1;

    while (done < n) {
        /* admit new arrivals */
        for (int i = 0; i < n; i++) {
            if (!procs[i].done && procs[i].arrival == tick) {
                /* check preemption if something is running */
                if (current != -1 && !context_switching) {
                    if (procs[i].remaining < procs[current].remaining) {
                        /* preempt: start ctx switch to new process */
                        int old = current;
                        next_proc         = i;
                        context_switching = CONTEXT_COST;
                        current           = -1;
                        (void)old;
                    }
                }
            }
        }

        if (context_switching > 0) {
            tl[tl_idx].tick = tick;
            strcpy(tl[tl_idx].pid, "CTX");
            tl_idx++;
            context_switching--;
            tick++;
            /* admit during ctx */
            for (int i = 0; i < n; i++) {
                if (!procs[i].done && procs[i].arrival == tick) {
                    if (next_proc != -1 &&
                        procs[i].remaining < procs[next_proc].remaining) {
                        next_proc = i;
                    }
                }
            }
            if (context_switching == 0) {
                current   = next_proc;
                next_proc = -1;
            }
            continue;
        }

        if (current == -1) {
            /* pick process with shortest remaining time */
            int best = -1;
            for (int i = 0; i < n; i++) {
                if (procs[i].done || procs[i].arrival > tick) continue;
                if (best == -1 || procs[i].remaining < procs[best].remaining)
                    best = i;
                else if (procs[i].remaining == procs[best].remaining) {
                    /* tie-break randomly */
                    if (rng_pick(2) == 0) best = i;
                }
            }
            if (best == -1) {
                /* idle */
                tl[tl_idx].tick = tick;
                strcpy(tl[tl_idx].pid, "IDLE");
                tl_idx++;
                tick++;
                continue;
            }
            current = best;
        }

        if (procs[current].first_run == -1)
            procs[current].first_run = tick;

        tl[tl_idx].tick = tick;
        strcpy(tl[tl_idx].pid, procs[current].pid);
        tl_idx++;
        procs[current].remaining--;
        tick++;

        /* admit new arrivals after this tick */
        for (int i = 0; i < n; i++) {
            if (!procs[i].done && procs[i].arrival == tick) {
                if (procs[i].remaining < procs[current].remaining) {
                    int old = current;
                    next_proc         = i;
                    context_switching = CONTEXT_COST;
                    current           = -1;
                    (void)old;
                    break;
                }
            }
        }

        if (current != -1 && procs[current].remaining == 0) {
            procs[current].finish = tick;
            procs[current].done   = 1;
            done++;
            current = -1;
            /* find next best; pay ctx switch if non-idle */
            int best = -1;
            for (int i = 0; i < n; i++) {
                if (procs[i].done || procs[i].arrival > tick) continue;
                if (best == -1 || procs[i].remaining < procs[best].remaining)
                    best = i;
                else if (procs[i].remaining == procs[best].remaining) {
                    if (rng_pick(2) == 0) best = i;
                }
            }
            if (best != -1) {
                next_proc         = best;
                context_switching = CONTEXT_COST;
            }
        }

        if (tl_idx >= MAX_TIMELINE - 2) break;
    }
    *tl_len = tl_idx;
    return tick;
}

/* ── print metrics ────────────────────────────────────────────────────────── */
static void print_metrics(Metrics *m) {
    printf("  Tempo médio de resposta  : %.2f (±%.2f)\n",
           m->avg_response,   m->std_response);
    printf("  Tempo médio de retorno   : %.2f (±%.2f)\n",
           m->avg_turnaround, m->std_turnaround);
    printf("  Vazão (T=%d)             : %.4f proc/tick\n",
           THROUGHPUT_T, m->throughput);
}

/* ── print per-process table ──────────────────────────────────────────────── */
static void print_table(Process *procs, int n) {
    printf("  %-6s %8s %8s %8s %10s %11s\n",
           "PID","Chegada","Burst","1ªExec","Retorno","Resposta");
    for (int i = 0; i < n; i++) {
        int turnaround = procs[i].finish - procs[i].arrival;
        int response   = procs[i].first_run - procs[i].arrival;
        printf("  %-6s %8d %8d %8d %10d %11d\n",
               procs[i].pid, procs[i].arrival, procs[i].burst,
               procs[i].first_run, turnaround, response);
    }
}

/* ── load JSON ────────────────────────────────────────────────────────────── */
static int load_json(const char *path, Process *procs, int *np) {
    FILE *f = fopen(path, "r");
    if (!f) { perror("fopen"); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f); buf[sz] = '\0';
    fclose(f);

    *np = 0;
    const char *p = buf;
    while ((p = strstr(p, "\"pid\"")) != NULL) {
        Process *pr = &procs[*np];
        parse_string_field(p, "pid", pr->pid, sizeof(pr->pid));
        pr->arrival = parse_int_field(p, "arrival_time");
        pr->burst   = parse_int_field(p, "burst_time");
        (*np)++;
        p += 5;
        if (*np >= MAX_PROC) break;
    }
    free(buf);
    return 0;
}

/* ── main ─────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    const char *jsonfile = (argc > 1) ? argv[1] : "input.json";

    Process procs[MAX_PROC];
    int     n = 0;

    if (load_json(jsonfile, procs, &n) != 0 || n == 0) {
        fprintf(stderr, "Erro ao carregar %s\n", jsonfile);
        return 1;
    }

    printf("=== Simulação RR vs SRTF ===\n");
    printf("Processos carregados: %d   Custo de ctx-switch: %d tick\n\n",
           n, CONTEXT_COST);

    /* print process list */
    printf("%-6s %8s %8s\n", "PID","Chegada","Burst");
    for (int i = 0; i < n; i++)
        printf("%-6s %8d %8d\n",
               procs[i].pid, procs[i].arrival, procs[i].burst);
    printf("\n");

    TLEntry *tl = malloc(MAX_TIMELINE * sizeof(TLEntry));
    int tl_len;

    /* ── RR for each quantum ── */
    int quantums[] = {1, 2, 4, 8, 16};
    int nq = (int)(sizeof(quantums)/sizeof(quantums[0]));

    for (int qi = 0; qi < nq; qi++) {
        int q = quantums[qi];
        printf("──────────────────────────────────────\n");
        printf("Round Robin  (quantum = %d)\n", q);
        rr_simulate(procs, n, q, tl, &tl_len);
        print_timeline(tl, tl_len);
        print_table(procs, n);
        Metrics m = compute_metrics(procs, n);
        print_metrics(&m);
        printf("\n");
    }

    /* ── SRTF ── */
    printf("──────────────────────────────────────\n");
    printf("SRTF (Shortest Remaining Time First)\n");
    srtf_simulate(procs, n, tl, &tl_len);
    print_timeline(tl, tl_len);
    print_table(procs, n);
    Metrics ms = compute_metrics(procs, n);
    print_metrics(&ms);
    printf("\n");

    /* ── Discussion ── */
    printf("══════════════════════════════════════\n");
    printf("Análise e Discussão\n");
    printf("══════════════════════════════════════\n\n");

    printf("Round Robin:\n");
    printf("  Vantagens:\n");
    printf("  + Previsibilidade: cada processo recebe CPU a cada (quantum + ctx) ticks.\n");
    printf("  + Justiça: nenhum processo sofre starvation.\n");
    printf("  + Bom tempo de resposta com quantum pequeno.\n");
    printf("  Desvantagens:\n");
    printf("  - Quantum pequeno → muitos ctx-switches → alta sobrecarga e pior vazão.\n");
    printf("  - Quantum grande → degrada ao FCFS → alto tempo de retorno para processos curtos.\n");
    printf("  - Ignora burst time: processos curtos e longos competem igualmente.\n\n");

    printf("SRTF:\n");
    printf("  Vantagens:\n");
    printf("  + Minimiza o tempo médio de retorno (ótimo entre algoritmos preemptivos).\n");
    printf("  + Processos curtos terminam rapidamente; boa vazão geral.\n");
    printf("  Desvantagens:\n");
    printf("  - Starvation: processos com burst longo podem esperar indefinidamente.\n");
    printf("  - Requer conhecimento do burst time restante (impraticável sem estimativa).\n");
    printf("  - Cada chegada de processo pode causar um ctx-switch → overhead imprevisível.\n\n");

    printf("Conclusão:\n");
    printf("  RR é preferível em sistemas interativos/tempo compartilhado (quantum moderado).\n");
    printf("  SRTF é preferível quando o objetivo é minimizar o turnaround médio e os\n");
    printf("  burst times são conhecidos ou estimáveis (ex.: sistemas batch).\n");

    free(tl);
    return 0;
}
