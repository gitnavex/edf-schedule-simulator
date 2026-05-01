#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#include "edf.h"

#define INIT_TASKS 10
#define SIM_TICKS  100

static volatile sig_atomic_t preempt_flag = 0;
static volatile sig_atomic_t tick_flag    = 0;

void sig_task_run(int signo __attribute__((unused))) {
    preempt_flag = 1;
}

void sig_tick(int signo __attribute__((unused))) {
    tick_flag = 1;
}

long str2l(char *str, char **endptr, char *msg) {
	errno = 0;
	long val = strtol(str, endptr, 10);
	if(errno == EINVAL)
		return -1;
	if(*endptr == str) {
		fprintf(stderr, "%s\n", msg);
               	return -1;
       	}
	return val;
}

/* -- parse_args ------------------------------------------------------- */
static void parse_args(int argc, char *argv[], int *n_tasks, int *n_ticks) {
    char *endptr = NULL;
 
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            fprintf(stderr, "Unbekannte Option: %s\n", argv[i]);
            fprintf(stderr, "Verwendung: %s [-n <tasks>] [-t <ticks>] [-u] [-f]\n", argv[0]);
            exit(EXIT_FAILURE);
        }
 
        for (int j = 1; argv[i][j] != '\0'; j++) {
            switch (argv[i][j]) {
                case 'u':
                    auto_flag = false;
                    break;
		case 'f':
		    task_enable= true;
		    break;
                case 'n':
                    if (i + 1 >= argc) {
                        fprintf(stderr, "Anzahl der Tasks nicht spezifiziert nach -n\n");
                        exit(EXIT_FAILURE);
                    }
                    *n_tasks = (int)str2l(argv[++i], &endptr,
                                          "Ungueltige Taskanzahl nach -n");
                    goto next_arg;
                case 't':
                    if (i + 1 >= argc) {
                        fprintf(stderr, "Anzahl der Ticks nicht spezifiziert nach -t\n");
                        exit(EXIT_FAILURE);
                    }
                    *n_ticks = (int)str2l(argv[++i], &endptr,
                                          "Ungueltige Tickanzahl nach -t");
                    goto next_arg;
                default:
                    fprintf(stderr, "Unbekannte Option: -%c\n", argv[i][j]);
                    fprintf(stderr, "Verwendung: %s [-n <tasks>] [-t <ticks>] [-u]\n", argv[0]);
                    exit(EXIT_FAILURE);
            }
        }
        next_arg:;
    }
}

int main(int argc, char*argv[]) {
    srand((unsigned)time(NULL));

    int n_tasks = INIT_TASKS;
    int n_ticks = SIM_TICKS;
   

    parse_args(argc, argv, &n_tasks, &n_ticks);


    sigset_t full;
    sigfillset(&full);

    struct sigaction sa_preemption = {0};
    sa_preemption.sa_mask    = full;
    sa_preemption.sa_handler = sig_task_run;
    sigaction(SIGUSR1, &sa_preemption, NULL);

    struct sigaction sa_creation = {0};
    sa_creation.sa_mask    = full;
    sa_creation.sa_handler = sig_task_create;
    sigaction(SIGUSR2, &sa_creation, NULL);

    struct sigaction sa_tick = {0};
    sa_tick.sa_mask    = full;
    sa_tick.sa_handler = sig_tick;
    sigaction(SIGALRM, &sa_tick, NULL);

    /* Alle Signale blockieren (Init-Phase) */
    sigset_t all_sigs, old_mask, wait_mask;
    sigfillset(&all_sigs);
    sigprocmask(SIG_BLOCK, &all_sigs, &old_mask);

    /* Initiale Tasks erstellen */
    for (int i = 0; i < n_tasks; i++) {
	task_t *T = NULL;
	int retry = 0;
	do {
	        T = task_create(&zeit);
        	if (T)
	            set_add_task(&taskset, T, READY);
		retry++;
	} while (!T && retry < 100);
	if(!T) printf("¬> Kein planbarer Task %d konnte nach 100 Versuche erstellt werden\n", i);
    }

    /* Timer-Thread starten */
    pthread_t timer;
    if (pthread_create(&timer, NULL, time_thread, NULL) != 0) {
        fprintf(stderr, "Fehler: pthread_create timer\n");
        return EXIT_FAILURE;
    }

    /* wait_mask: nur SIGUSR1, SIGUSR2, SIGALRM durchlassen */
    sigfillset(&wait_mask);
    sigdelset(&wait_mask, SIGUSR1);
    sigdelset(&wait_mask, SIGUSR2);
    sigdelset(&wait_mask, SIGALRM);

    sigprocmask(SIG_SETMASK, &old_mask, NULL);

    /* -- Haupt-Scheduling-Schleife ------------------------------------- */
    /*
     * t=0 wird sofort ausgefuehrt (kein Tick abwarten).
     * Ab t=1 wartet sigsuspend auf SIGALRM (Tick) oder SIGUSR1 (Preemption).
     */
    int tick = 0;
    while (tick < n_ticks) {

        if (tick > 0) {
            preempt_flag = 0;
            sigsuspend(&wait_mask);  /* schlaeft bis SIGALRM oder SIGUSR1 */
        }

        sigprocmask(SIG_BLOCK, &all_sigs, NULL);

        check_deadlines();

        pthread_mutex_lock(&taskset.mutex_set);
        task_t *T = taskset.first;
        pthread_mutex_unlock(&taskset.mutex_set);

        pthread_mutex_lock(&mutex_t);
        zeit_t now = zeit;
        pthread_mutex_unlock(&mutex_t);

        if (T) {
            task_run(T);
            printf("t=%d : T%d laeuft (Deadline: %d, verbleibend: %d)\n",
                   now, T->id, T->orig_deadline, T->execution);

            if (task_done(T)) {
                task_t *done = task_remove(&taskset);
                if (done) {
                    pthread_mutex_lock(&mutex_t);
                    done->response_time += zeit - done->arrival;
                    pthread_mutex_unlock(&mutex_t);
                    set_add_task(&waitset, done, WAITING);
                }
            }
        } else {
            printf("t=%d : Leerlauf\n", now);
        }

        /* Tick nur bei SIGALRM zaehlen, nicht bei reiner Preemption     */
        if (tick_flag) {
            tick_flag = 0;
            tick++;
        } else if (tick == 0) {
            /* t=0: kein Signal, einmalig manuell zaehlen                */
            tick++;
        }

        sigprocmask(SIG_SETMASK, &old_mask, NULL);
    }

    pthread_cancel(timer);
    pthread_join(timer, NULL);

    print_stats();

    free_set(&taskset);
    free_set(&waitset);
    return EXIT_SUCCESS;
}
