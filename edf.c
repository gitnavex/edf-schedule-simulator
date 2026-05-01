#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "edf.h"

/* -- Globale Variablen ------------------------------------------------ */
pthread_mutex_t mutex_t = PTHREAD_MUTEX_INITIALIZER;
zeit_t          zeit    = 0;
int             taskno  = 0;
int             id      = 0;
taskset_t       taskset    = { NULL, NULL, PTHREAD_MUTEX_INITIALIZER, 0 };
taskset_t       waitset    = { NULL, NULL, PTHREAD_MUTEX_INITIALIZER, 0 };
double          util_total = 0.0;  /* laufende Summe wcet/period aller Tasks  */
volatile bool   auto_flag  = true; /* automatische Vergabe der TaskParameters */   
volatile bool   task_enable= false;/* Task annehmen auch wenn nicht planbar   */
/* -- time_thread ------------------------------------------------------ */
static void cleanup_mutex(void *arg) {
    pthread_mutex_unlock((pthread_mutex_t *)arg);
}

void *time_thread(void *arg __attribute__((unused))) {
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    while (1) {
        usleep(500);  /* cancellation point */

        pthread_mutex_lock(&mutex_t);
        pthread_cleanup_push(cleanup_mutex, &mutex_t);
        zeit++;
        pthread_cleanup_pop(1);

        waitset_check();

        /* Tick-Signal an den Hauptprozess: weckt sigsuspend() auf      */
        kill(getpid(), SIGALRM);
    }
    return NULL;
}

/* -- Signalhandler (in edf.c definiert, in edf.h deklariert) ---------- */
void sig_task_create(int signo __attribute__((unused))) {
    task_t *T = task_create(&zeit);
    if (T)
        set_add_task(&taskset, T, READY);
}

/* sig_task_run und sig_tick werden in scheduler.c definiert            */

/* -- task_create ------------------------------------------------------ */
task_t *task_create(zeit_t *t) {
    task_t *new_task = malloc(sizeof(task_t));
    if (!new_task) {
        fprintf(stderr, "Fehler bei malloc in task_create\n");
        return NULL;
    }

    pthread_mutex_lock(&mutex_t);
    new_task->arrival = *t;
    pthread_mutex_unlock(&mutex_t);

    new_task->id        = id++;
    if(auto_flag) {
	    new_task->period    = rand() % 12 + 6;
	    new_task->wcet      = rand() % (new_task->period/3) + 1;
    } else {
	    printf("Konfiguierung Task %d:\n", new_task->id);
	    char *line = NULL;
	    line = readline("period> ");
	    add_history(line);
	    new_task->period = atoi(line);
	    free(line);
	    line = readline("wcet> ");
	    add_history(line);
	    new_task->wcet = atoi(line);
	    free(line);
    }
    new_task->execution = new_task->wcet;
    new_task->deadline  = new_task->arrival + new_task->period;
    new_task->orig_deadline = new_task->deadline;
    new_task->response_time  = 0;
    new_task->deadline_missed = 0;
    new_task->inst_done = 1;
    new_task->next      = NULL;
    new_task->prev      = NULL;

    /* Aufnahmetest: Task nur hinzufügen wenn planbar */
    if (!schedulable(new_task)) {
        //printf("[EDF] Task %d abgelehnt  (nicht planbar)\n", new_task->id);
	if(!task_enable) {
        	id--;   /* ID-Zaehler zuruecksetzen */
	        free(new_task);
        	return NULL;
	}
    }

    /* util_total unter mutex_t aktualisieren */
    pthread_mutex_lock(&mutex_t);
    util_total += (double)new_task->wcet / (double)new_task->period;
    pthread_mutex_unlock(&mutex_t);

    taskno++;
    printf("[EDF] Task %d erstellt  (arrival=%d  wcet=%d  period=%d  deadline=%d)\n",
           new_task->id, new_task->arrival, new_task->wcet,
           new_task->period, new_task->deadline);
    return new_task;
}

/* -- task_respawn ----------------------------------------------------- */
task_t *task_respawn(task_t *T, zeit_t *t) {
    if (!T) return NULL;
    task_t *new = malloc(sizeof(task_t));
    if (!new) {
        fprintf(stderr, "Fehler bei malloc in task_respawn\n");
        return NULL;
    }

    pthread_mutex_lock(&mutex_t);
    new->arrival = *t;
    pthread_mutex_unlock(&mutex_t);

    new->id          = T->id;
    new->period      = T->period;
    new->wcet        = T->wcet;
    new->execution   = rand() % T->wcet + 1;
    new->deadline    = new->arrival + new->period;
    new->orig_deadline = new->deadline;
    new->response_time   = T->response_time;
    new->inst_done       = T->inst_done;
    new->deadline_missed = T->deadline_missed;
    new->inst_done++; 
    new->next        = NULL;
    new->prev        = NULL;

    printf("[EDF] Task %d respawn   (arrival=%d  exec=%d  deadline=%d)\n",
           new->id, new->arrival, new->execution, new->deadline);
    return new;
}

/* -- waitset_check ---------------------------------------------------- */
void waitset_check(void) {
    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGUSR2);
    sigaddset(&block, SIGALRM);
    sigprocmask(SIG_BLOCK, &block, &old);

    pthread_mutex_lock(&waitset.mutex_set);

    task_t *best = NULL;
    for (task_t *T = waitset.first; T; T = T->next) {
        if (zeit >= T->arrival + T->period) {
            if (!best || T->id < best->id)
                best = T;
        }
    }

    if (best) {
        if (best->prev)
            best->prev->next = best->next;
        else
            waitset.first = best->next;

        if (best->next)
            best->next->prev = best->prev;
        else
            waitset.last = best->prev;

        best->prev = NULL;
        best->next = NULL;
        waitset.size--;
    }

    pthread_mutex_unlock(&waitset.mutex_set);

    if (best) {
        task_t *next = task_respawn(best, &zeit);
        free(best);
        if (next)
            set_add_task(&taskset, next, READY);
    }

    sigprocmask(SIG_SETMASK, &old, NULL);
}

/* -- set_add_task ----------------------------------------------------- */
void set_add_task(taskset_t *Taskset, task_t *T, qtype st) {
    if (!T || !Taskset) return;

    bool preempt = false;

    pthread_mutex_lock(&Taskset->mutex_set);

    if (!Taskset->first) {
        Taskset->first = T;
        Taskset->last  = T;
        T->prev = NULL;
        T->next = NULL;
        Taskset->size = 1;
        preempt = (st == READY);
        pthread_mutex_unlock(&Taskset->mutex_set);
        if (preempt)
            raise(SIGUSR1);
        return;
    }

    task_t *current = Taskset->first;
    task_t *prev    = NULL;

    if (st == READY) {
        while (current && current->deadline <= T->deadline) {
            prev    = current;
            current = current->next;
        }
    } else {
        while (current && current->arrival <= T->arrival) {
            prev    = current;
            current = current->next;
        }
    }

    if (!prev) {
        T->next = Taskset->first;
        T->prev = NULL;
        Taskset->first->prev = T;
        Taskset->first = T;
        preempt = (st == READY);   /* neuer Task hat kuerzeste Deadline  */
    } else if (!current) {
        prev->next = T;
        T->prev    = prev;
        T->next    = NULL;
        Taskset->last = T;
    } else {
        prev->next    = T;
        T->prev       = prev;
        T->next       = current;
        current->prev = T;
    }

    Taskset->size++;
    pthread_mutex_unlock(&Taskset->mutex_set);

    if (preempt && st == READY)
        raise(SIGUSR1);  /* Hauptschleife aus sigsuspend wecken          */
}

/* -- time_run --------------------------------------------------------- */
void time_run(zeit_t *t) {
    pthread_mutex_lock(&mutex_t);
    (*t)++;
    pthread_mutex_unlock(&mutex_t);
}

/* -- task_done -------------------------------------------------------- */
bool task_done(task_t *T) {
    return (T && T->execution == 0);
}

/* -- task_run --------------------------------------------------------- */
void task_run(task_t *T) {
    if (!T || !T->execution) return;
    T->execution--;
}

/* -- task_remove ------------------------------------------------------ */
task_t *task_remove(taskset_t *Taskset) {
    if (!Taskset || !Taskset->first) return NULL;

    pthread_mutex_lock(&Taskset->mutex_set);

    task_t *T      = Taskset->first;
    Taskset->first = T->next;

    if (Taskset->first)
        Taskset->first->prev = NULL;
    else
        Taskset->last = NULL;

    Taskset->size--;
    T->prev = NULL;
    T->next = NULL;

    pthread_mutex_unlock(&Taskset->mutex_set);
    return T;
}

/* -- free_set --------------------------------------------------------- */
void free_set(taskset_t *Taskset) {
    if (!Taskset) return;

    pthread_mutex_lock(&Taskset->mutex_set);
    task_t *current = Taskset->first;
    while (current) {
        task_t *next = current->next;
        free(current);
        current = next;
    }
    Taskset->first = NULL;
    Taskset->last  = NULL;
    Taskset->size  = 0;
    pthread_mutex_unlock(&Taskset->mutex_set);
}

/* -- check_deadlines -------------------------------------------------- */
void check_deadlines(void) {
    pthread_mutex_lock(&mutex_t);
    zeit_t now = zeit;
    pthread_mutex_unlock(&mutex_t);

    pthread_mutex_lock(&taskset.mutex_set);
    task_t *T = taskset.first;
    while (T) {
        if (now > T->deadline) {
            printf("  T%d hat Deadline %d verpasst! (tc=%d)\n",
                   T->id, T->orig_deadline, now);
            T->deadline_missed++;
            T->deadline = INT_MAX;
        }
        T = T->next;
    }
    pthread_mutex_unlock(&taskset.mutex_set);
}

/* -- print_stats ------------------------------------------------------ */
void print_stats(void) {
    int total_missed = 0;
    printf("\n=== Scheduling Statistik ===\n");
    printf("Utilization : U=%.3f\n", util_total);
    taskset_t *sets[2] = { &taskset, &waitset };
    for (int s = 0; s < 2; s++) {
        pthread_mutex_lock(&sets[s]->mutex_set);
        task_t *T = sets[s]->first;
        while (T) {
	    double avg_tr = (T->inst_done > 0) ? (double)T->response_time / T->inst_done: 0.0;
	    char avg_str[6];
	    (avg_tr)? snprintf(avg_str, sizeof(avg_str), "%.5f", avg_tr): snprintf(avg_str, sizeof(avg_str), "-"); 
            if (T->deadline_missed == 0)
                printf("T%d : tr=%s, Deadline eingehalten\n",
                       T->id, avg_str);
            else
                printf("T%d : tr=%s, Deadline %dx verpasst\n",
                       T->id, avg_str, T->deadline_missed);
            total_missed += T->deadline_missed;
            T = T->next;
        }
        pthread_mutex_unlock(&sets[s]->mutex_set);
    }
    printf("Deadline-Verletzungen: %d\n", total_missed);
}

/* -- schedulable ------------------------------------------------------ */
/*
 * EDF-Aufnahmetest nach Liu & Layland:
 *
 *   sum (wcet_i / period_i)  <=  1.0
 *
 * Berechnet die aktuelle CPU-Auslastung aller bekannten Tasks
 * (taskset + waitset) und prueft ob das Hinzufügen von T die
 * Grenze von 100% ueberschreiten wuerde.
 *
 * Gibt true zurueck wenn der neue Task aufgenommen werden kann,
 * false wenn das System dadurch nicht mehr planbar waere.
 */
bool schedulable(task_t *T) {
    if (!T || T->period == 0) return false;

    /* O(1): util_total wird bei jeder task_create aktualisiert,        */
    /* kein Durchlaufen der Liste noetig.                                */
    pthread_mutex_lock(&mutex_t);
    double util_now = util_total;
    pthread_mutex_unlock(&mutex_t);

    double util_new = (double)T->wcet / (double)T->period;
    bool   ok       = (util_now + util_new <= 1.0);
    if(ok)
	    printf("[EDF] Aufnahmetest: U_aktuell=%.3f  U_neu=%.3f  U_gesamt=%.3f  %s\n",
			    util_now, util_new, util_now + util_new, "PLANBAR");
    else if(!auto_flag || task_enable)
	    printf("[EDF] Aufnahmetest: U_aktuell=%.3f  U_neu=%.3f  U_gesamt=%.3f  %s\n",
                            util_now, util_new, util_now + util_new, "NICHT PLANBAR");


    return ok;
}
