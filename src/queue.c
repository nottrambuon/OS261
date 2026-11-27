#include <stdio.h>
#include <stdlib.h>
#include "queue.h"

/*
 * Queue of PCB pointers backed by a fixed-size array.
 *
 * The functions below do NOT lock anything by themselves: the caller
 * (the scheduler in sched.c) holds queue_lock around every call, so a
 * queue is never touched by two CPUs at the same time.
 */

int empty(struct queue_t *q)
{
        if (q == NULL)
                return 1;
        return (q->size == 0);
}

/*
 * Priority used to order PCBs inside one queue.
 * With MLQ every queue only holds PCBs of the same prio, so this is only
 * a real choice for the legacy (non-MLQ) single ready queue.
 * A lower value means a higher priority.
 */
static uint32_t proc_priority(struct pcb_t *proc)
{
#ifdef MLQ_SCHED
        return proc->prio;
#else
        return proc->priority;
#endif
}

/* Remove the element at [idx] and shift the rest left to keep FIFO order. */
static struct pcb_t *remove_at(struct queue_t *q, int idx)
{
        struct pcb_t *proc = q->proc[idx];
        int i;

        for (i = idx; i < q->size - 1; i++)
                q->proc[i] = q->proc[i + 1];
        q->size--;
        q->proc[q->size] = NULL;
        return proc;
}

void enqueue(struct queue_t *q, struct pcb_t *proc)
{
        /* Put a new process at the tail of queue [q] */
        if (q == NULL || proc == NULL)
                return;

        if (q->size >= MAX_QUEUE_SIZE) {
                fprintf(stderr, "[queue] enqueue: queue is full (%d), drop PID %d\n",
                        MAX_QUEUE_SIZE, proc->pid);
                return;
        }

        q->proc[q->size] = proc;
        q->size++;
}

struct pcb_t *dequeue(struct queue_t *q)
{
        /*
         * Return the PCB with the highest priority (smallest value) and
         * remove it from [q]. When several PCBs have the same priority the
         * one that was enqueued first wins, so a queue whose PCBs all share
         * one priority (an MLQ level) behaves as a plain FIFO -> round robin.
         */
        int i, best;

        if (empty(q))
                return NULL;

        best = 0;
        for (i = 1; i < q->size; i++) {
                if (proc_priority(q->proc[i]) < proc_priority(q->proc[best]))
                        best = i;
        }

        return remove_at(q, best);
}

struct pcb_t *purgequeue(struct queue_t *q, struct pcb_t *proc)
{
        /*
         * Remove a specific PCB from [q] wherever it is.
         * Return that PCB, or NULL if it was not in the queue.
         */
        int i;

        if (empty(q) || proc == NULL)
                return NULL;

        for (i = 0; i < q->size; i++) {
                if (q->proc[i] == proc)
                        return remove_at(q, i);
        }

        return NULL;
}
