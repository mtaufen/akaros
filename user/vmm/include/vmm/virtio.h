#pragma once

#include <stdint.h>
#include <err.h>
#include <pthread.h>
#include <sys/uio.h>
#include <vmm/virtio_ring.h>

// This file contains the core virtio structs, functions, and macros for Akaros

// Print errors caused by incorrect driver behavior
#define VIRTIO_DRI_ERRX(dev, fmt, ...) \
	errx(1, "Virtio Device: %s: Error, driver misbehaved. " fmt, (dev)->name, ## __VA_ARGS__)

// Print warnings caused by incorrect driver behavior
#define VIRTIO_DRI_WARNX(dev, fmt, ...) \
	warnx("Virtio Device: %s: Warning, driver misbehaved. " fmt, (dev)->name, ## __VA_ARGS__)


struct virtio_vq {
	// The name of the vq e.g. for printing errors
	char *name;

	// The vqdev that contains this vq
	struct virtio_vq_dev *vqdev;

	// The vring contains pointers to the descriptor table and available and used rings
	// as well as the number of elements in the queue.
	struct vring vring;

	// The maximum number of elements in the queue that the device is ready to process
	// Reads from the register corresponding to this value return 0x0 if the queue is
	// not available.
	int qnum_max;

	// The driver writes 0x1 to qready to tell the device
	// that it can execute requests from this vq
	uint32_t qready;

	// The last vq.vring.avail->idx that the service function saw while processing the queue
	uint16_t last_avail;

	// The service function that processes buffers for this queue
	void *(*srv_fn)(void *arg);

	// The thread that the service function is running in
	pthread_t srv_th;

	// We write eventfd to wake up the service function; it blocks on eventfd read
	int eventfd;
};

struct virtio_vq_dev {
	// The name of the device e.g. for printing errors
	char *name;

	// The type of the device e.g. VIRTIO_ID_CONSOLE for a console device
	uint32_t dev_id;

	// The features supported by this device
	uint64_t dev_feat;

	// The features supported by the driver (these are set by the guest)
	uint64_t dri_feat;

	// The virtio transport dev that contains this vqdev. i.e. struct virtio_mmio_dev
	void *transport_dev;

	// The number of vqs on this device. You MUST set this to the same number of
	// virtio_vqs that you put in the vqs array on the virtio_vq_dev.
	int num_vqs; // TODO: can we make this unsigned?

	// Flexible array of vqs on this device
	struct virtio_vq vqs[]; // TODO: QEMU macros a fixed-length in here, that they just make the max number of queues
};

// TODO: Rename this fn
// Based on wait_for_vq_desc in Linux lguest.c
uint32_t virtio_next_avail_vq_desc(struct virtio_vq *vq, struct iovec iov[],
                            uint32_t *olen, uint32_t *ilen);

// TODO: Rename this to something more succinct and understandable!
// Based on the add_used function in lguest.c
// Adds descriptor chain to the used ring of the vq
void virtio_add_used_desc(struct virtio_vq *vq, uint32_t head, uint32_t len);