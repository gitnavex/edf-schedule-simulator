# EDF Scheduler

Ein Echtzeit-Scheduler in C auf Basis des Earliest Deadline First (EDF) Algorithmus nach Liu & Layland.
Das Projekt simuliert preemptives Scheduling periodischer Tasks mit POSIX-Threads und Signalen.


## Kompilierung

Voraussetzungen: GCC, libreadline

    sudo apt install gcc libreadline-dev
    make
    make clean


## Verwendung

    ./scheduler [-n <tasks>] [-t <ticks>] [-u] [-f]

    -n <tasks>   Anzahl initial erstellter Tasks (Standard: 10)
    -t <ticks>   Simulationsdauer in Ticks (Standard: 100)
    -u           Manuelle Eingabe von period und wcet per Readline
    -f           Auch nicht planbare Tasks aufnehmen (force)

Beispiele:

    ./scheduler
    ./scheduler -n 5 -t 50
    ./scheduler -n 3 -u
    ./scheduler -n 15 -f


## Funktionsweise

Der Scheduler verwaltet zwei Queues:

- taskset: bereite Tasks, sortiert nach aufsteigender Deadline (EDF-Prinzip)
- waitset: fertige Tasks, die auf ihre naechste Periode warten

Ein Timer-Thread inkrementiert die globale Zeit alle 500 us und sendet SIGALRM
an den Hauptprozess. Die Hauptschleife schlaeft in sigsuspend() und wacht auf:

- SIGALRM:  Ein Tick ist vergangen, naechste Scheduling-Entscheidung
- SIGUSR1:  Preemption, neuer Task mit kuerzerer Deadline ist bereit
- SIGUSR2:  Neuen Task zur Laufzeit erstellen

Aufnahmetest nach Liu & Layland: Ein neuer Task wird nur aufgenommen wenn
die Gesamtauslastung 100% nicht ueberschreitet: sum(wcet_i / period_i) <= 1.0


## Projektstruktur

    scheduler.c   main(), Argument-Parsing, Scheduling-Schleife
    edf.c         Task-Verwaltung, Queue-Operationen, Timer-Thread, Statistik
    edf.h         Typdefinitionen, Funktionsprototypen
    makefile      Build-Konfiguration


## Task-Lebenszyklus

    task_create() -> taskset (READY)
                        | task_run() pro Tick
                     task_done() -> waitset (WAITING)
                                       | nach einer Periode
                                    task_respawn() -> taskset (READY)
