#pragma once
/* Core virtio definitions for Akaros
	For example: The definitions of our virtqueue and
	             generic virtio device structures.
*/

#include <stdint.h>
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