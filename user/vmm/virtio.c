// Implementations of common virtio functions for Akaros
// Code in here is based on lguest.c by Rusty Russel, which is
// distributed under the GNU General Public License.

#include <sys/eventfd.h>
#include <sys/uio.h>
#include <vmm/virtio.h>

// TODO: Rename this to something more succinct and understandable!
// Based on the add_used function in lguest.c
// Adds descriptor chain to the used ring of the vq
void virtio_add_used_desc(struct virtio_vq *vq, uint32_t head, uint32_t len)
{
	// NOTE: len is the total length of the descriptor chain (in bytes)
	//       that was written to.
	//       So you should pass 0 if you didn't write anything, and pass
	//       the number of bytes you wrote otherwise.
	vq->vring.used->ring[vq->vring.used->idx % vq->vring.num].id = head;
	vq->vring.used->ring[vq->vring.used->idx % vq->vring.num].len = len;
	// TODO: what does this wmb actually end up compiling as now that we're out of linux?
	wmb(); // So the values get written to the used buffer before we update idx
	vq->vring.used->idx++;
}

// TODO: Cleanup. maybe rename too
// For traversing the linked list of descriptors
// Also based on Linux's lguest.c
static uint32_t get_next_desc(struct vring_desc *desc, uint32_t i, uint32_t max)
{
	uint32_t next;

	if (!(desc[i].flags & VRING_DESC_F_NEXT)) {
		// No more in the chain, so return max to signal that we reached the end
		return max;
	}

	next = desc[i].next;

	// TODO: what does this wmb actually end up compiling as now that we're out of linux?
	wmb(); // just because lguest put it here. not sure why they did that yet.

	// TODO: Make this an actual error with a real error message
	// TODO: Figure out what lguest.c's "bad_driver" function does
	if (next >= max) {
		printf("NONONONONONO. NO!\nYou can not tell me I have a desc at an index outside the queue.\nYou liar!\nvmrunkernel.c-get_next_desc\n");
	}

	return next;
}

// TODO: Rename this fn
// Based on wait_for_vq_desc in Linux lguest.c
uint32_t virtio_next_avail_vq_desc(struct virtio_vq *vq, struct iovec iov[],
                            uint32_t *olen, uint32_t *ilen)
{
	uint32_t i, head, max;
	struct vring_desc *desc;
	eventfd_t event;

	// The first thing we do is read from the eventfd. If nothing has been written to it yet,
	// then the driver isn't done setting things up and we want to wait for it to finish.
	// For example, dereferencing the vq->vring.avail pointer could segfault if the driver
	// has not yet written a valid address to it.
	if (eventfd_read(vq->eventfd, &event))
		printf("next_avail_vq_desc event read failed?\n");
	// TODO: I think I want a memory barrier here? In case the event fd gets written but the avail idx hasn't yet?
	mb();

	while (vq->last_avail == vq->vring.avail->idx) {
		// If we got poked but there are no queues available, then just return 0
		// this will work for now because we're only in these handlers during an
		// exit due to EPT viol. due to driver write to QUEUE_NOTIFY register on dev.
		// return 0;



		// TODO: We will move from just returning zero to just sleeping
		//       again if we were kicked but nothing is available.
		// NOTE: I do not kick the guest with an irq here. I do that in
		//       the individual service functions when it is necessary.

		// TODO: What to do here about VRING_DESC_F_NO_NOTIFY flag?
		// NOTE: If you look at the comments in virtio_ring.h, the VRING_DESC_F_NO_NOTIFY
		//       flag is set by the host to say to the guest "Don't kick me when you add
		//       a buffer." But this comment also says that it is an optimization, is not
		//       always reliable, and that the guest will still kick the host when out of
		//       buffers. So I'm leaving that out for now, and we can revisit why it might
		//       improve performance sometime in the future.
		//       TODO: That said, I might still need to unset the bit. It should be unset
		//             by default, because it is only supposed to be set by the host and
		//             I never set it. But this is worth double-checking.

// TODO: make this a real error
		if (eventfd_read(vq->eventfd, &event))
			printf("next_avail_vq_desc event read failed?\n");

		// TODO: Do I want a memory barrier here? In case the event fd gets written but the avail idx hasn't yet?
		mb();
	}

	// Read the desc num (head) after we detect the ring update (vq->last_avail != vq->vring.avail->idx)
	rmb();

	/* TODO: lguest also checks for this:
	// Check it isn't doing very strange things with descriptor numbers.
	if ((u16)(vq->vring.avail->idx - last_avail) > vq->vring.num)
		bad_driver_vq(vq, "Guest moved used index from %u to %u",
			      last_avail, vq->vring.avail->idx);
	*/

	// Mod because it's a *ring*
	// TODO: maybe switch to using vq->vring.num for ours too
	head = vq->vring.avail->ring[vq->last_avail % vq->vring.num];
	vq->last_avail++;

	// TODO: make this an actual error
	if (head >= vq->vring.num)
		printf("dumb dumb dumb driver. head >= vq->vring.num in next_avail_vq_desc in vmrunkernel\n");

	// Don't know how many output buffers or input buffers there are yet, depends on desc chain.
	*olen = *ilen = 0;

	max = vq->vring.num; // Since vring.num is the size of the queue, max is the most buffers we could possibly find
	desc = vq->vring.desc; // qdesc is the address of the descriptor (array?TODO) set by the driver
	i = head;

	// TODO: lguest manually checks that the pointers on the vring fields aren't goofy when the driver
	//       initally says they are ready, we should probably do that somewhere too.

	/*NOTE: (from lguest)
	 * We have to read the descriptor after we read the descriptor number,
	 * but there's a data dependency there so the CPU shouldn't reorder
	 * that: no rmb() required.
	 */


	do {

		// If it's an indirect descriptor, we travel through the layer of indirection and then
		// we're at table of descriptors chained by `next`, and since they are chained we can
		// handle them the same way as direct descriptors once we're through that indirection.
		if (desc[i].flags & VRING_DESC_F_INDIRECT) {
			// virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors
			if (!(vq->vqdev & VIRTIO_RING_F_INDIRECT_DESC))
				VIRTIO_DRI_ERRX(vq->vqdev,
					"The driver must not set the INDIRECT flag on a descriptor"
					" if the INDIRECT_DESC feature was not negotiated.");

			// TODO: Should also error if we find an indirect within an indirect (only one table per desc)
			//       lguest seems to interpret this as "the only indirect desc can be the first in the chain"
			//       I trust Rusty on that interpretation. (desc != vq->vring.desc is a bad_driver)
			// TODO: Only the first in the chain! is not the correct interpretation. s2.4.5.3.2 says
			//       you MUST handle the case where you have normal chained descriptors before a single
			//       indirect descriptor
			// virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors
			if (desc != vq->vring.desc)
				VIRTIO_DRI_ERRX(vq->vqdev,
					"The driver must not set the INDIRECT flag on a descriptor"
					" within an indirect descriptor."
					" See virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors");

			// virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors
			if (desc[i].flags & VRING_DESC_F_NEXT)
				VIRTIO_DRI_ERRX(vq->vqdev,
					"The driver must not set both the INDIRECT and NEXT flags on a descriptor."
					" See virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors");

			// TODO: Better error message, more descriptive of what this error indicates.
			if (desc[i].len % sizeof(struct vring_desc)) // nonzero mod indicates wrong table size
				printf("virtio Error; bad size for indirect table\n");

			// NOTE: Virtio spec says the device MUST ignore the write-only flag in the
			//       descriptor for an indirect table. So we ignore it.

			max = desc[i].len / sizeof(struct vring_desc);
			desc = (void *)desc[i].addr; // TODO: check that this pointer isn't goofy
			i = 0;

			// virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors
			if (max > vq->vring.num) {
				VIRTIO_DRI_ERRX(vq->vqdev,
					"The driver must not create a descriptor chain longer"
					" than the queue size of the device."
					" See virtio-v1.0-cs04 s2.4.5.3.1 Indirect Descriptors");
			}
		}

		// Now build the scatterlist of descriptors
		// TODO: And, you know, we ought to check the pointers on these descriptors too!
		// TODO: You better make sure you pass a big enough scatterlist to this function
		//       for whatever the eventual value of *olen + *ilen will be!
		iov[*olen + *ilen].iov_len = desc[i].len;
		iov[*olen + *ilen].iov_base = (void *)desc[i].addr; // NOTE: .v is basically our scatterlist/iovec's iov_base

		if (desc[i].flags & VRING_DESC_F_WRITE) {
			// input descriptor, increment *ilen
			(*ilen)++;
		}
		else {
			// output descriptor, check that this is *before* we read any input descriptors
			// and then increment *olen if we're ok

			// virtio-v1.0-cs04 s2.4.4.2 Message Framing
			if (*ilen) {
				VIRTIO_DRI_ERRX(vq->vqdev,
					"Device detected an output descriptor after an input descriptor."
					" The driver must place any device-writeable descriptor elements"
					" after all device-readable descriptor elements.");
			}

			(*olen)++;
		}

		// virtio-v1.0-cs04 s2.4.5.2 The Virtqueue Descriptor Table
		if (*olen + *ilen > max) {
			VIRTIO_DRI_ERRX(vq->vqdev,
				"The driver must ensure that there are no loops in the"
				" descriptor chain it provides!");
		}


	} while ((i = get_next_desc(desc, i, max)) != max);

	return head;

}
