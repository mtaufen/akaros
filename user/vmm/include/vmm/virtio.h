#pragma once

#include <stdint.h>
#include <err.h>
#include <pthread.h>
#include <sys/uio.h>
#include <vmm/virtio_ring.h>

// This file contains the core virtio structs, functions, and macros for Akaros

// Print errors caused by incorrect driver behavior
#define VIRTIO_DRI_ERRX(dev, fmt, ...) \
	errx(1, "\n  Virtio Device: %s: Error, driver misbehaved.\n  "\
		fmt, (dev)->name, ## __VA_ARGS__)

// Print warnings caused by incorrect driver behavior
#define VIRTIO_DRI_WARNX(dev, fmt, ...) \
	warnx("\n  Virtio Device: %s: Warning, driver behaved suspiciously.\n  "\
		fmt, (dev)->name, ## __VA_ARGS__)

// Print errors caused by incorrect device behavior
#define VIRTIO_DEV_ERRX(dev, fmt, ...) \
	errx(1, "\n  Virtio Device: %s: Error, device misbehaved.\n  "\
		fmt, (dev)->name, ## __VA_ARGS__)

// Print warnings caused by incorrect device behavior
#define VIRTIO_DEV_WARNX(dev, fmt, ...) \
	warnx("\n  Virtio Device: %s: Warning, device behaved suspiciously.\n  "\
		fmt, (dev)->name, ## __VA_ARGS__)



struct virtio_vq {
	// The name of the vq e.g. for printing errors
	char *name;

	// The vqdev that contains this vq
	struct virtio_vq_dev *vqdev;

	// The vring contains pointers to the descriptor table and available and
	// used rings as well as the number of elements in the queue.
	struct vring vring;

	// The maximum number of elements in the queue that the device is ready to
	// process. Reads from the register corresponding to this value return 0x0
	// if the queue is not available. A queue's size is always a power of 2.
	int qnum_max;

	// The driver writes 0x1 to qready to tell the device
	// that it can execute requests from this vq
	uint32_t qready;

	// The last vq.vring.avail->idx that the service function saw while
	// processing the queue
	uint16_t last_avail;

	// The service function that processes buffers for this queue
	void *(*srv_fn)(void *arg);

	// The thread that the service function is running in
	pthread_t srv_th;

	// Write eventfd to wake up the service function; it blocks on eventfd read
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

	// The number of virtio_vqs on the device
	uint32_t num_vqs;

	// A pointer to the device-specific config space
	void *cfg;

	// A pointer to a default device-specific config space
	// If set, cfg_sz bytes, starting at cfg_d, will be
	// copied to cfg.
	void *cfg_d;

	// The size, in bytes, of the device-specific config space
	// Used by the device to bounds-check driver access
	uint64_t cfg_sz;

	// The virtio transport dev that contains this vqdev
	// i.e. struct virtio_mmio_dev
	void *transport_dev;

	// Flexible array of vqs on this device
	struct virtio_vq vqs[];
};

// Validates memory regions provided by the guest's virtio driver
void *virtio_check_pointer(struct virtio_vq *vq, uint64_t addr,
                           uint32_t size, char *file, uint32_t line);

// Adds descriptor chain to the used ring of the vq
// Based on add_used in Linux's lguest.c
void virtio_add_used_desc(struct virtio_vq *vq, uint32_t head, uint32_t len);

// Waits for the next available descriptor chain and writes the addresses
// and sizes of the buffers it describes to an iovec to make them easy to use.
// Based on wait_for_vq_desc in Linux lguest.c
uint32_t virtio_next_avail_vq_desc(struct virtio_vq *vq, struct iovec iov[],
                            uint32_t *olen, uint32_t *ilen);

// Returns NULL if the features are valid, otherwise returns
// an error string describing what part of validation failed
// We pass the vqdev instead of just the dev_id in case we
// also want to validate the device-specific config space.
// feat is the feature vector that you want to validate for the vqdev
const char *virtio_validate_feat(struct virtio_vq_dev *vqdev, uint64_t feat);