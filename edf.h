#ifndef EDF_H
#define EDF_H

#include <stdbool.h>
#include <pthread.h>
#include <signal.h>

typedef volatile int zeit_t;

/* -- Queue Type ------------------------------------------------------- */
typedef enum qtype {
    READY,
    WAITING
} qtype;

typedef struct task_t {
    int    id;
    zeit_t arrival;
    zeit_t wcet;            /* Worst-Case Execution Time, unveränderlich */
    zeit_t execution;       /* aktuelle Instanz, wird runtergezählt      */
    zeit_t period;
    zeit_t deadline;
    zeit_t orig_deadline;   /* unveraenderliche Original-Deadline        */
    zeit_t response_time;   /* zeit bei Fertigstellung - arrival         */
    int    deadline_missed; /* Anzahl verpasster Deadlines               */
    int    inst_done;       /* Anzahl der abgeschlossenen Instanzen      */ 
    struct task_t *prev;
    struct task_t *next;
} task_t;

typedef struct {
    task_t         *first;
    task_t         *last;
    pthread_mutex_t mutex_set;
    int             size;
} taskset_t;

extern pthread_mutex_t mutex_t;   /* Schutz für globale Zeit           */
extern zeit_t          zeit;      /* Globale Zeit                      */
extern int             taskno;    /* Anzahl erstellter Tasks           */
extern int             id;        /* Nächste Task-ID                   */
extern taskset_t       taskset;   /* Aktive Tasks (sortiert: deadline) */
extern taskset_t       waitset;   /* Wartende Tasks (sortiert: arrival)*/
extern double          util_total;/* Laufende CPU-Auslastung aller Tasks*/
extern volatile bool   auto_flag; /* automatische Vergabe der Tasks Parameters*/
extern volatile bool   task_enable;/* Task annehmen auch wenn nicht planbar */

/* -- Funktionsprototypen ---------------------------------------------- */
task_t *task_create(zeit_t *);
task_t *task_respawn(task_t *, zeit_t *);  /* neue Instanz, gleiche Periode */
void    set_add_task(taskset_t *, task_t *, qtype);
void    time_run(zeit_t *);
void    task_run(task_t *);
void    free_set(taskset_t *);
bool    task_done(task_t *);
task_t *task_remove(taskset_t *);
void    waitset_check(void);    /* fällige Tasks von wait → ready verschieben */
void    check_deadlines(void);  /* Deadline-Verletzungen prüfen               */
void    print_stats(void);      /* Statistik am Ende ausgeben                 */
bool    schedulable(task_t *);  /* EDF-Aufnahmetest fuer neuen Task           */

/* Signalhandler */
void sig_task_create(int);
void sig_task_run(int);
void sig_tick(int);       /* SIGALRM: ein Tick ist abgelaufen */

/* Timer-Thread */
void *time_thread(void *arg);

#endif /* EDF_H */
