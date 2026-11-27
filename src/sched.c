/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "queue.h"
#include "sched.h"
#include <pthread.h>

#include <stdlib.h>
#include <stdio.h>
static struct queue_t ready_queue;
static struct queue_t run_queue;
static pthread_mutex_t queue_lock;

static struct queue_t running_list;
#ifdef MLQ_SCHED
static struct queue_t mlq_ready_queue[MAX_PRIO];
/* slot[prio] = number of dispatches the queue [prio] still has in this round */
static int slot[MAX_PRIO];
/* The ready queue served by the last get_mlq_proc() (for tracing) */
static int curr_prio = 0;
#endif

/* Return 1 when no process is waiting in any ready queue, 0 otherwise */
int queue_empty(void) {
	int is_empty;

	pthread_mutex_lock(&queue_lock);
	is_empty = empty(&ready_queue) && empty(&run_queue);
#ifdef MLQ_SCHED
	unsigned long prio;
	for (prio = 0; prio < MAX_PRIO && is_empty; prio++)
		if (!empty(&mlq_ready_queue[prio]))
			is_empty = 0;
#endif
	pthread_mutex_unlock(&queue_lock);
	return is_empty;
}

void init_scheduler(void) {
#ifdef MLQ_SCHED
    int i ;

	for (i = 0; i < MAX_PRIO; i ++) {
		mlq_ready_queue[i].size = 0;
		slot[i] = MAX_PRIO - i;
	}
	curr_prio = 0;
#endif
	ready_queue.size = 0;
	run_queue.size = 0;
	running_list.size = 0;
	pthread_mutex_init(&queue_lock, NULL);
}

/*
 * A process has finished: take it out of running_list so that nobody
 * (e.g. a syscall looking processes up by PID) keeps a pointer to a PCB
 * that the CPU is about to free.
 */
void finish_proc(struct pcb_t * proc) {
	if (proc == NULL)
		return;
	pthread_mutex_lock(&queue_lock);
	purgequeue(&running_list, proc);
	pthread_mutex_unlock(&queue_lock);
}

#ifdef MLQ_SCHED
/* Keep a prio read from the input file inside the valid range */
static unsigned long valid_prio(struct pcb_t * proc) {
	if (proc->prio >= MAX_PRIO)
		proc->prio = MAX_PRIO - 1;
	return proc->prio;
}

/*
 *  Stateful design for routine calling
 *  based on the priority and our MLQ policy
 *  We implement stateful here using transition technique
 *  State representation   prio = 0 .. MAX_PRIO, curr_slot = 0..(MAX_PRIO - prio)
 *
 *  MLQ policy:
 *  - Every call scans the queues from prio 0 (highest) to MAX_PRIO-1 and
 *    takes the first non-empty queue that still has slot left.
 *  - Queue [prio] may hand out at most slot = MAX_PRIO - prio processes in
 *    one round. When it is used up the CPU goes to the next queue, even if
 *    [prio] still has work: that work waits for the next round.
 *  - When every non-empty queue has used up its slot, the round is over:
 *    all slots are refilled and the scan starts again.
 */
struct pcb_t * get_mlq_proc(void) {
	struct pcb_t * proc = NULL;
	int prio, round;

	pthread_mutex_lock(&queue_lock);
	/* round 0: use what is left of the slots; round 1: after a refill */
	for (round = 0; round < 2 && proc == NULL; round++) {
		for (prio = 0; prio < MAX_PRIO; prio++) {
			if (!empty(&mlq_ready_queue[prio]) && slot[prio] > 0) {
				proc = dequeue(&mlq_ready_queue[prio]);
				slot[prio]--;
				curr_prio = prio;
				break;
			}
		}
		if (proc == NULL) {
			/* Nothing left in this round: start a new one */
			for (prio = 0; prio < MAX_PRIO; prio++)
				slot[prio] = MAX_PRIO - prio;
		}
	}

	if (proc != NULL)
		enqueue(&running_list, proc);
	pthread_mutex_unlock(&queue_lock);
	return proc;
}

void put_mlq_proc(struct pcb_t * proc) {
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->mlq_ready_queue = mlq_ready_queue;
	proc->krnl->running_list = &running_list;

	/* The process leaves the CPU: move it from running_list
	 * back to the ready queue of its priority */
	pthread_mutex_lock(&queue_lock);
	purgequeue(&running_list, proc);
	enqueue(&mlq_ready_queue[valid_prio(proc)], proc);
	pthread_mutex_unlock(&queue_lock);
}

void add_mlq_proc(struct pcb_t * proc) {
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->mlq_ready_queue = mlq_ready_queue;
	proc->krnl->running_list = &running_list;

	/* A new process from the loader: it has never run,
	 * so it only goes to the ready queue of its priority */
	pthread_mutex_lock(&queue_lock);
	enqueue(&mlq_ready_queue[valid_prio(proc)], proc);
	pthread_mutex_unlock(&queue_lock);
}

struct pcb_t * get_proc(void) {
	return get_mlq_proc();
}

void put_proc(struct pcb_t * proc) {
	return put_mlq_proc(proc);
}

void add_proc(struct pcb_t * proc) {
	return add_mlq_proc(proc);
}
#else
struct pcb_t * get_proc(void) {
	struct pcb_t * proc = NULL;

	pthread_mutex_lock(&queue_lock);
	/* Legacy scheduler: when ready_queue runs dry, the processes that
	 * already used their time slice (run_queue) become ready again */
	if (empty(&ready_queue)) {
		while (!empty(&run_queue))
			enqueue(&ready_queue, dequeue(&run_queue));
	}
	proc = dequeue(&ready_queue);
	if (proc != NULL)
		enqueue(&running_list, proc);
	pthread_mutex_unlock(&queue_lock);

	return proc;
}

void put_proc(struct pcb_t * proc) {
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;

	/* Leave the CPU: out of running_list, into run_queue */
	pthread_mutex_lock(&queue_lock);
	purgequeue(&running_list, proc);
	enqueue(&run_queue, proc);
	pthread_mutex_unlock(&queue_lock);
}

void add_proc(struct pcb_t * proc) {
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;

	/* A new process only goes to ready_queue */
	pthread_mutex_lock(&queue_lock);
	enqueue(&ready_queue, proc);
	pthread_mutex_unlock(&queue_lock);
}
#endif
