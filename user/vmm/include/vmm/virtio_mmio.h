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

// TODO: using the ros one for now becasue the vmm one still tries to include linux/types.
 //      I'll convert the types to stuff in stdint when I re-import all these headers from linux
#include <vmm/virtio_ring.h>

 // TODO: move comments to a block above each struct so that it's easy to quickly
 //       read all the fields on the struct

// TODO: This "struct vq" is all ours. So I can clean it up and change it around however I want (Mike)
// A vq defines on queue attached to a device. It has a function, started as a thread;
// an arg, for arbitrary use; qnum, which is an indicator of how much memory is given
// to the queue; a pointer to the thread that gets started when the queue is notified;
// a physical frame number, which is process virtual to the vmm; an isr (not used yet);
// status; and a pointer to the virtio struct.
// struct vqdev;
struct vq {
	// The name of the vq e.g. for printing errors
	char *name;

	// The vqdev that contains this vq
	struct vqdev *vqdev;

	// The vring contains pointers to the descriptor table and available and used rings
	struct vring vring;

	// TODO: Figure out exactly what maxqnum is for
	int maxqnum; // how many things the q gets? or something.

	// TODO: comment this
	uint32_t isr; // not used yet but ... // TODO: If it's not used then what is it!?

	// TODO: comment this
	uint32_t status;

	// The driver writes 0x1 to qready to tell the device
	// that it can execute requests from this vq
	uint32_t qready; // TODO do we prevent access to the queue before this is written?

	// The last vq.vring.avail->idx that the service function saw while processing the queue
	uint16_t last_avail;

	// The service function that processes buffers for this queue
	void *(*srv_fn)(void *arg);

	// The thread that the service function is running in
	pthread_t srv_th;

	// We write eventfd to wake up the service function; it blocks on eventfd read
	int eventfd;
};

// a vqdev has a name; magic number; features ( we MUST have features);
// and an array of vqs.
struct vqdev {
	// The name of the device e.g. for printing errors
	char *name;

	// The type of the device e.g. VIRTIO_ID_CONSOLE for a console device
	uint32_t dev_id;

	// The features supported by this device
	uint64_t dev_feat;

	// The features supported by the driver (these are set by the guest)
	uint64_t dri_feat;

	// The VIRTIO transport that contains this vqdev. i.e. struct virtio_mmio_dev
	void *transport_dev;

	// The number of vqs on this device
	int numvqs;

	// Flexible array of vqs on this device TODO document that you usually just init this with a struct literal
	struct vq vqs[]; // TODO: QEMU macros a fixed-length in here, that they just make the max number of queues
	// TODO: Is there a way to do a compile time check that someone actually put as many vqs in here as they said they would?
};

// The mmio device that wraps the vqdev. Holds things like the base
// address of the device, the device status register, queue selectors, etc.
// TODO: Remove fields that I just shim out or don't need, or that are already on the vqdev
// this is a NON LEGACY DEVICE!
struct virtio_mmio_dev {
	// The base address of the virtio mmio device
	// we save the same value here as we report to guest via kernel cmd line
	uint64_t addr;

	// Reads from vqdev.dev_feat are performed starting at bit 32 * dev_feat_sel
	uint32_t dev_feat_sel;

	// Writes to vqdev.dri_feat are performed starting at bit 32 * dri_feat_sel
	uint32_t dri_feat_sel;

	// Reads and writes to queue-specific registers target vqdev->vqs[qsel]
	uint32_t qsel;

	// Interrupt status register
	uint32_t isr;

	// Status register for the device
	uint32_t status;

	// ConfigGeneration, used to check that access to device-specific config space was atomic
	uint32_t cfg_gen;

	// The generic vq device contained by this mmio transport
	struct vqdev *vqdev;

	// TODO: What to do about the device-specific configuration space?
};

// virtio_mmio_rd_reg and virtio_mmio_wr_reg are used to process the guest's driver's
// reads and writes to the mmio device registers. gpa is the guest physical address
// that the driver tried to write to; this is used to calculate the target register
uint32_t virtio_mmio_rd_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa);
void     virtio_mmio_wr_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa, uint32_t *value);

// Sets the VIRTIO_MMIO_INT_VRING bit in the interrupt status register for the device
void virtio_mmio_set_vring_irq(struct virtio_mmio_dev *mmio_dev);

// Mike: This file is from Linux. Ok.
