/*
 * Virtio platform device driver
 *
 * Copyright 2011, ARM Ltd.
 *
 * Based on Virtio PCI driver by Anthony Liguori, copyright IBM Corp. 2007
 *
 * This header is BSD licensed so anyone can use the definitions to implement
 * compatible drivers/servers.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of IBM nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

#include <pthread.h>
#include <vmm/sched.h>
/*
 * Control registers
 */

/* Magic value ("virt" string) - Read Only */
#define VIRTIO_MMIO_MAGIC_VALUE		0x000

/* Virtio device version - Read Only */
#define VIRTIO_MMIO_VERSION		0x004

/* Virtio device ID - Read Only */
#define VIRTIO_MMIO_DEVICE_ID		0x008

/* Virtio vendor ID - Read Only */
#define VIRTIO_MMIO_VENDOR_ID		0x00c

/* Bitmask of the features supported by the device (host)
 * (32 bits per set) - Read Only */
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010

/* Device (host) features set selector - Write Only */
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL	0x014

/* Bitmask of features activated by the driver (guest)
 * (32 bits per set) - Write Only */
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020

/* Activated features set selector - Write Only */
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL	0x024


#ifndef VIRTIO_MMIO_NO_LEGACY /* LEGACY DEVICES ONLY! */

/* Guest's memory page size in bytes - Write Only */
#define VIRTIO_MMIO_GUEST_PAGE_SIZE	0x028

#endif


/* Queue selector - Write Only */
#define VIRTIO_MMIO_QUEUE_SEL		0x030

/* Maximum size of the currently selected queue - Read Only */
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034

/* Queue size for the currently selected queue - Write Only */
#define VIRTIO_MMIO_QUEUE_NUM		0x038


#ifndef VIRTIO_MMIO_NO_LEGACY /* LEGACY DEVICES ONLY! */

/* Used Ring alignment for the currently selected queue - Write Only */
#define VIRTIO_MMIO_QUEUE_ALIGN		0x03c

/* Guest's PFN for the currently selected queue - Read Write */
#define VIRTIO_MMIO_QUEUE_PFN		0x040

#endif


/* Ready bit for the currently selected queue - Read Write */
#define VIRTIO_MMIO_QUEUE_READY		0x044

/* Queue notifier - Write Only */
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050

/* Interrupt status - Read Only */
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060

/* Interrupt acknowledge - Write Only */
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064

/* Device status register - Read Write */
#define VIRTIO_MMIO_STATUS		0x070

/* Selected queue's Descriptor Table address, 64 bits in two halves */
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084

/* Selected queue's Available Ring address, 64 bits in two halves */
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW	0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH	0x094

/* Selected queue's Used Ring address, 64 bits in two halves */
#define VIRTIO_MMIO_QUEUE_USED_LOW	0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH	0x0a4

/* Configuration atomicity value */
#define VIRTIO_MMIO_CONFIG_GENERATION	0x0fc

/* The config space is defined by each driver as
 * the per-driver configuration space - Read Write */
#define VIRTIO_MMIO_CONFIG		0x100

/*
 * Interrupt flags (re: interrupt status & acknowledge registers)
 */

#define VIRTIO_MMIO_INT_VRING		(1 << 0)
#define VIRTIO_MMIO_INT_CONFIG		(1 << 1)

// A vq defines on queue attached to a device. It has a function, started as a thread;
// an arg, for arbitrary use; qnum, which is an indicator of how much memory is given
// to the queue; a pointer to the thread that gets started when the queue is notified;
// a physical frame number, which is process virtual to the vmm; an isr (not used yet);
// status; and a pointer to the virtio struct.
struct vq {
	char *name;
	void *(*f)(void *arg); // Start this as a thread when a matching virtio is discovered.
	void *arg;
	int maxqnum; // how many things the q gets? or something.
	int qnum;
	int qalign;
	pthread_t thread;
	/* filled in by virtio probing. */
	uint64_t pfn;
	uint32_t isr; // not used yet but ... // TODO: If it's not used then what is it!?
	uint32_t status;
	uint64_t qdesc;
	uint64_t qavail;
	uint64_t qused;
	void *virtio;

	uint32_t qready;

};

// a vqdev has a name; magic number; features ( we MUST have features);
// and an array of vqs.
struct vqdev {
	/* Set up usually as a static initializer */
	char *name;
	uint32_t dev; // e.g. VIRTIO_ID_CONSOLE);
	uint64_t device_features;
	uint64_t driver_features;
	int numvqs;
	struct vq vqs[]; // TODO: QEMU macros a fixed-length in here, that they just make the max number of queues
	// TODO: Is there a way to do a compile time check that someone actually put as many vqs in here as they said they would?
};



/* This struct is passed to a virtio thread when it is started. It includes
 * needed info and the vqdev arg. This seems overkill but we may need to add to it.
 */
struct virtio_threadarg {
	struct vq *arg;
};

// The mmio device that wraps the vqdev. Holds things like the base
// address of the device, the device status register, queue selectors, etc.
// TODO: Remove fields that I just shim out or don't need, or that are already on the vqdev
// this is a NON LEGACY DEVICE!
struct virtio_mmio_dev {
	uint64_t base_address;

	uint32_t device_features_sel;
	uint32_t driver_features_sel;
	uint32_t queue_sel; // is this actually 32 bits? definitely not any bigger in the spec (next offest 4 bytes away)
	// Stuff like queue num max, etc is on the vq.
	// TODO: Change some of the names on the vq so that they match the virtio spec
	uint32_t int_status; // InterruptStatus
	uint32_t status; // Status
	uint32_t cfg_gen; //ConfigGeneration, used to check that access to device-specific config space was atomic

	struct vqdev *vqdev;
	// TODO: What to do about the device-specific configuration space?
};


// gpa is guest physical address
// these are my "version 2" functions
uint32_t virtio_mmio_rd_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa);
void virtio_mmio_wr_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa, uint32_t *value);
void virtio_mmio_set_vring_irq(struct virtio_mmio_dev *mmio_dev);

// better name for this.... it sets the vqdev pointer on the virtio_mmio_dev
// this function does two tiny things... basically unnecessary
// would it be a good hook for "I plugged this thing in" though?
void virtio_mmio_register_vqdev(struct virtio_mmio_dev *mmio_dev, struct vqdev *vqdev, uint64_t mmio_ba);



void dumpvirtio_mmio(FILE *f, uint64_t gpa);
void register_virtio_mmio(struct vqdev *v, uint64_t virtio_base);
int virtio_mmio(struct guest_thread *vm_thread, uint64_t gpa, int destreg,
                uint64_t *regp, int store);
//void virtio_mmio_set_vring_irq(void);


// Mike: This file is from Linux. Ok.
