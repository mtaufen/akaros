/* Copyright (c) 2009, 2012, 2015 The Regents of the University of California
 * Barret Rhoden <brho@cs.berkeley.edu>
 * Valmon Leymarie <leymariv@berkeley.edu>
 * Kevin Klues <klueska@cs.berkeley.edu>
 * See LICENSE for details.
 */

#pragma once

/* The core request algorithm maintains an internal array of these: the
 * global pcore map. Note the prov_proc and alloc_proc are weak (internal)
 * references, and should only be used as a ref source while the ksched has a
 * valid kref. */
struct sched_pcore {
	TAILQ_ENTRY(sched_pcore)   prov_next;    /* on a proc's prov list */
	TAILQ_ENTRY(sched_pcore)   alloc_next;   /* on an alloc list (idle)*/
	struct proc                *prov_proc;   /* who this is prov to */
	struct proc                *alloc_proc;  /* who this is alloc to */
};
TAILQ_HEAD(sched_pcore_tailq, sched_pcore);

struct core_request_data {
	struct sched_pcore_tailq  prov_alloc_me;      /* prov cores alloced us */
	struct sched_pcore_tailq  prov_not_alloc_me;  /* maybe alloc to others */
};