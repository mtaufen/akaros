// Implementations of common virtio functions for Akaros
// Code in here is based on lguest.c by Rusty Russel, which is
// distributed under the GNU General Public License.

#include <sys/eventfd.h>
#include <sys/uio.h>
#include <vmm/virtio.h>
#include <vmm/virtio_ids.h>

// based on _check_pointer in Linux's lguest.c
void *virtio_check_pointer(struct virtio_vq *vq, uint64_t addr,
                           uint32_t size, char *file, uint32_t line)
{
	// TODO: Right now, we just check that the pointer + the size doesn't wrap
	//       around. We can also check that the pointer isn't outside
	//       the region of memory allocated to the guest. However, we need to
	//       get the bounds of that region from somewhere. I don't know what
	//       they are off the top of my head.

	if ((addr + size) < addr)
		VIRTIO_DRI_ERRX(vq->vqdev,
			"Driver provided an invalid address or size (addr:0x%x sz:%u)."
			" Location: %s:%d", addr, size, file, line);

	return (void *)addr;
}

// For traversing the chain of descriptors
// based on next_desc Linux's lguest.c
static uint32_t next_desc(struct vring_desc *desc, uint32_t i, uint32_t max,
		struct virtio_vq *vq) // The vq is just for the error message.
{
	uint32_t next;

	if (!(desc[i].flags & VRING_DESC_F_NEXT)) {
		// No more in the chain, so return max to signal that we reached the end
		return max;
	}

	next = desc[i].next;

	// TODO: Figure out why lguest had the memory barrier here.
	//       DO NOT REMOVE UNLESS YOU KNOW WHY!
	wmb_f();

	if (next >= max) {
		VIRTIO_DRI_ERRX(vq->vqdev,
			"The next descriptor index in the chain provided by the driver is"
			" outside the bounds of the maximum allowed size of its queue.");
	}

	return next;
}

// Adds descriptor chain to the used ring of the vq
// based on add_used in Linux's lguest.c
void virtio_add_used_desc(struct virtio_vq *vq, uint32_t head, uint32_t len)
{
	if (!vq->qready)
		VIRTIO_DEV_ERRX(vq->vqdev,
			"The device may not process queues with QueueReady set to 0x0.");

	// NOTE: len is the total length of the descriptor chain (in bytes)
	//       that was written to.
	//       So you should pass 0 if you didn't write anything, and pass
	//       the number of bytes you wrote otherwise.
	vq->vring.used->ring[vq->vring.used->idx % vq->vring.num].id = head;
	vq->vring.used->ring[vq->vring.used->idx % vq->vring.num].len = len;

	// virtio-v1.0-cs04 s2.4.8.2 The Virtqueue Used Ring
	wmb_f(); // The device MUST set len prior to updating the used idx (sfence)
	vq->vring.used->idx++;
}

// TODO: Need to make sure we don't overflow iov. Right now we're just kind of
//       trusting that whoever provided the iov made it at least as big as
//       qnum_max, but maybe we shouldn't be that trusting.
// Based on wait_for_vq_desc in Linux's'lguest.c
uint32_t virtio_next_avail_vq_desc(struct virtio_vq *vq, struct iovec iov[],
                            uint32_t *olen, uint32_t *ilen)
{
	uint32_t i, head, max;
	struct vring_desc *desc;
	eventfd_t event;

	// The first thing we do is read from the eventfd. If nothing has been
	// written to it yet, then the driver isn't done setting things up and we
	// want to wait for it to finish.
	// For example, dereferencing the vq->vring.avail pointer could segfault if
	// the driver has not yet written a valid address to it.
	if (eventfd_read(vq->eventfd, &event))
		VIRTIO_DEV_ERRX(vq->vqdev,
			"eventfd read failed while waiting for available descriptors\n");

	// Make sure vring.avail->idx has had a chance to update before our read
	// The mfence instruction is invoked via mb_f in Akaros.
	mb_f();

	// Since the device is not supposed to access queues when QueueReady is 0x0,
	// if that's the case we'll stay in the loop.
	while (vq->last_avail == vq->vring.avail->idx
	       || vq->qready == 0) {
		// We know the ring has updated when idx advances. We check == because
		// idx is allowed to wrap around eventually.

		// NOTE: I do not kick the guest with an irq here. I do that in
		//       the queue service functions when it is necessary.

		// NOTE: If you look at the comments in virtio_ring.h, the
		//       VRING_DESC_F_NO_NOTIFY flag is set by the host to say to the
		//       guest "Don't kick me when you add a buffer." But this comment
		//       also says that it is an optimization, is not always reliable,
		//       and that the guest will still kick the host when out of
		//       buffers. So I'm leaving that out for now, and we can revisit
		//       why it might improve performance sometime in the future.

		if (eventfd_read(vq->eventfd, &event))
			VIRTIO_DEV_ERRX(vq->vqdev,
				"eventfd read failed while waiting for"
				" available descriptors\n");

		// Make sure vring.avail->idx has had a chance to update before our read
		// The mfence instruction is invoked via mb_f in Akaros.
		mb_f();
	}

	// Read the desc num into head after we detect the ring update
	// The lfence instruction is invoked via rmb_f in Akaros.
	rmb_f();


	// NOTE: lgeust is a bit cryptic about why they check for this. I added
	//       the reason I believe they do it in the comment and error message.
	// the guest can't have incremented idx more than vring.num times since
	// we last incremented vq->last_avail, because it would have run out of
	// places to put descriptors after incrementing exactly vring.num times
	// (prior to our next vq->last_avail++)
	if ((vq->vring.avail->idx - vq->last_avail) > vq->vring.num)
		VIRTIO_DRI_ERRX(vq->vqdev,
			"The driver advanced vq->vring.avail->idx from %u to %u,"
			" which have a difference greater than the capacity of a queue."
			" The idx is supposed to increase by 1 for each descriptor chain"
			" added to the available ring; the driver should have run out of"
			" room and thus been forced to wait for us to catch up!",
			vq->last_avail, vq->vring.avail);

	// Mod because it's a *ring*
	head = vq->vring.avail->ring[vq->last_avail % vq->vring.num];
	vq->last_avail++;

	if (head >= vq->vring.num)
		VIRTIO_DRI_ERRX(vq->vqdev,
			"The index of the head of the descriptor chain provided by the"
			" driver is after the end of the queue.");

	// Don't know how many output buffers or input buffers there are yet,
	// this depends on the descriptor chain.
	*olen = *ilen = 0;

	// Since vring.num is the size of the queue, max is the max descriptors
	// that should be in a descriptor chain. If we find more than that, the
	// driver is doing something wrong.
	max = vq->vring.num;
	desc = vq->vring.desc;
	i = head;

	// NOTE: (from lguest)
	//       We have to read the descriptor after we read the descriptor number,
	//       but there's a data dependency there so the CPU shouldn't reorder
	//       that: no rmb() required.
	//       Mike: The descriptor number is stored in i; what lguest means is
	//             that data must flow from avail_ring to head to i before i
	//             is used to index into desc.

	do {
		// If it's an indirect descriptor, it points at a table of descriptors
		// provided by the guest driver. The descriptors in that table are
		// still chained, so we can ultimately handle them the same way as
		// direct descriptors.
		if (desc[i].flags & VRING_DESC_F_INDIRECT) {

			// virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors
			if (!(vq->vqdev->dri_feat & (1<<VIRTIO_RING_F_INDIRECT_DESC)))
				VIRTIO_DRI_ERRX(vq->vqdev,
					"The driver must not set the INDIRECT flag on a descriptor"
					" if the INDIRECT_DESC feature was not negotiated.");

			// NOTE: desc is only modified when we detect an indirect
			//       descriptor, so our implementation works whether there is an
			//       indirect descriptor at the very beginning OR at the very
			//       end of the chain (virtio-v1.0-cs04 s2.4.5.3.2 compliant)
			// virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors
			if (desc != vq->vring.desc)
				VIRTIO_DRI_ERRX(vq->vqdev,
					"The driver must not set the INDIRECT flag on a descriptor"
					" within an indirect descriptor."
					" See virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors");

			// virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors
			if (desc[i].flags & VRING_DESC_F_NEXT)
				VIRTIO_DRI_ERRX(vq->vqdev,
					"The driver must not set both the INDIRECT and NEXT flags"
					" on a descriptor."
					" See virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors");

			// nonzero mod indicates wrong table size
			if (desc[i].len % sizeof(struct vring_desc))
				VIRTIO_DRI_ERRX(vq->vqdev,
					"The size of a vring descriptor does not evenly divide the"
					" length of the indirect table provided by the driver."
					" Bad table size.");

			// NOTE: virtio-v1.0-cs04 s2.4.5.3.2 Indirect Descriptors
			//       says that the device MUST ignore the write-only flag in the
			//       descriptor that refers to an indirect table. So we ignore.

			max = desc[i].len / sizeof(struct vring_desc);
			desc = virtio_check_pointer(vq, desc[i].addr, desc[i].len,
			                            __FILE__, __LINE__);

			// Now that desc is pointing at the table of indirect descriptors,
			// we set i to 0 so that we can start walking that table
			i = 0;

			// virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors
			if (max > vq->vring.num) {
				VIRTIO_DRI_ERRX(vq->vqdev,
					"The driver must not create a descriptor chain longer"
					" than the queue size of the device."
					" See virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors");
			}
		}

		// Now build the scatterlist of buffers for the device to process
		iov[*olen + *ilen].iov_len = desc[i].len;
		iov[*olen + *ilen].iov_base = virtio_check_pointer(vq, desc[i].addr,
		                                                   desc[i].len,
		                                                   __FILE__, __LINE__);

		if (desc[i].flags & VRING_DESC_F_WRITE) {
			// input descriptor, increment *ilen
			(*ilen)++;
		}
		else {
			// virtio-v1.0-cs04 s2.4.4.2 Message Framing
			if (*ilen) {
				VIRTIO_DRI_ERRX(vq->vqdev,
					"Device detected an output descriptor after an input"
					" descriptor. The driver must place any device-writeable"
					" descriptor elements after all device-readable descriptor"
					" elements.");
			}

			(*olen)++;
		}

		// virtio-v1.0-cs04 s2.4.5.2 The Virtqueue Descriptor Table
		if (*olen + *ilen > max) {
			VIRTIO_DRI_ERRX(vq->vqdev,
				"The driver must ensure that there are no loops in the"
				" descriptor chain it provides! The combined length of"
				" readable and writeable buffers was greater than the"
				" number of elements in the queue.");
		}


	} while ((i = next_desc(desc, i, max, vq)) != max);

	return head;

}

// Returns NULL if the features are valid, otherwise returns
// an error string describing what part of validation failed
const char *virtio_validate_feat(uint32_t dev_id, uint64_t feat) {

	// First validate device-specific features. We want to tell someone
	// when they forgot to implement validation code for a new device
	// as soon as possible, so that they don't skip this when they
	// implement new devices.
	switch(dev_id) {
		// case VIRTIO_ID_CONSOLE:
		// 	break;
		case 0:
			return "Invalid device id (0x0)! On the MMIO transport,"
			       " this value indicates that the device is a system memory"
			       " map with placeholder devices at static, well known"
			       " addresses. In any case, this is not something you"
			       " validate features for.";
		default:
			return "Validation not implemented for this device type!"
			       " You MUST implement validation for this device!"
			       " You should add your new code to the virtio_validate_feat"
			       " function in vmm/virtio.c.";
			break;
	}

	// Validate that general feature set is valid

	return NULL;
}
