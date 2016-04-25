// NOTE:
// Mike's implementation of virtio mmio device.
// Mike's additions are Copyright (c) 2016 Google, Inc.

// Partially based on the qemu implementation (although that was for the legacy spec)
// That implementation was licensed as:

/*
 * Virtio MMIO bindings
 *
 * Copyright (c) 2011 Linaro Limited
 *
 * Author:
 *  Peter Maydell <peter.maydell@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h> // TODO: remove this when all printfs have been replaced with actual errors
#include <err.h>
#include <sys/eventfd.h>
#include <vmm/virtio_config.h>
#include <vmm/virtio_mmio.h>

// Print errors caused by incorrect driver behavior
#define DRI_ERRX(dev, fmt, ...) \
	errx(1, "Virtio Device: %s: Error, driver misbehaved. " fmt, (dev)->name, ## __VA_ARGS__)

// Print warnings caused by incorrect driver behavior
#define DRI_WARNX(dev, fmt, ...) \
	warnx("Virtio Device: %s: Warning, driver misbehaved. " fmt, (dev)->name, ## __VA_ARGS__)

#define VIRT_MAGIC 0x74726976 /* 'virt' */

#define VIRT_MMIO_VERSION 0x2

#define VIRT_MMIO_VENDOR 0x52414B41 /* 'AKAR' */

static void virtio_mmio_reset(struct virtio_mmio_dev *mmio_dev)
{
	// TODO: Actually reset the device!
}

// TODO: Prevent device from accessing virtual queue contents when QueueReady is 0x0
// TODO: MAKE SURE WE DO THIS FOR qready!!!!!

uint32_t virtio_mmio_rd_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa)
{
	uint64_t offset = gpa - mmio_dev->addr;

	// Return 0 for all registers except the magic number,
	// the mmio version, and the device vendor when either
	// there is no vqdev or no vqs on the vqdev.
	if (!mmio_dev->vqdev || mmio_dev->vqdev->num_vqs == 0) {
		switch(offset) {
		case VIRTIO_MMIO_MAGIC_VALUE:
			return VIRT_MAGIC;
		case VIRTIO_MMIO_VERSION:
			return VIRT_MMIO_VERSION;
		case VIRTIO_MMIO_VENDOR_ID:
			return VIRT_MMIO_VENDOR;
		default:
			return 0;
		}
	}

	if (offset >= VIRTIO_MMIO_CONFIG) {
		// TODO: Figure out what to do for reading the device config space
		// TODO: Will also probably have to set the ConfigGeneration stuff
		//       when I finally get to writing the device configuration space.
		//       And will definitely have to read that value twice to make sure
		//       the read was atomic.
		// TODO: ConfigGenreation is a read only register, so it is probably set by the device,
		//       and not by these handlers for the driver to touch the device through.
		DRI_ERRX(mmio_dev->vqdev, "Attempt to read the device configuration space! Not yet implemented!");
	}


// TODO: the spec I am referencing is: http://docs.oasis-open.org/virtio/virtio/v1.0/cs04/virtio-v1.0-cs04.html
	switch(offset) {
/*
Magic value
0x74726976 (a Little Endian equivalent of the “virt” string).
*/
		case VIRTIO_MMIO_MAGIC_VALUE:
			return VIRT_MAGIC;
/*
Device version number
0x2. Note: Legacy devices (see 4.2.4 Legacy interface) used 0x1.
*/
		case VIRTIO_MMIO_VERSION:
			return VIRT_MMIO_VERSION;
/*
Virtio Subsystem Device ID
See 5 Device Types for possible values. Value zero (0x0) is used to define a
system memory map with placeholder devices at static, well known addresses,
assigning functions to them depending on user’s needs.
*/
		case VIRTIO_MMIO_DEVICE_ID:
			return mmio_dev->vqdev->dev_id;
/*
Virtio Subsystem Vendor ID
*/
		case VIRTIO_MMIO_VENDOR_ID:
			return VIRT_MMIO_VENDOR;
/*
Flags representing features the device supports
Reading from this register returns 32 consecutive flag bits, the least significant
bit depending on the last value written to DeviceFeaturesSel. Access to this register
returns bits DeviceFeaturesSel ∗ 32 to (DeviceFeaturesSel ∗ 32) + 31, eg. feature
bits 0 to 31 if DeviceFeaturesSel is set to 0 and features bits 32 to 63 if
DeviceFeaturesSel is set to 1. Also see 2.2 Feature Bits.
*/
		case VIRTIO_MMIO_DEVICE_FEATURES:
			if (!(mmio_dev->status & VIRTIO_CONFIG_S_DRIVER))
				DRI_ERRX(mmio_dev->vqdev,
				         "Attempt to read device features before setting the DRIVER status bit. See virtio-v1.0-cs04 sec. 3.1.1.");

			if (mmio_dev->dev_feat_sel) // high 32 bits requested
				return mmio_dev->vqdev->dev_feat >> 32;

			return mmio_dev->vqdev->dev_feat; // low 32 bits requested
/*
Maximum virtual queue size
Reading from the register returns the maximum size (number of elements) of the queue
the device is ready to process or zero (0x0) if the queue is not available. This applies
to the queue selected by writing to QueueSel.
*/
		case VIRTIO_MMIO_QUEUE_NUM_MAX:
			// TODO: Spec says to return 0 if the queue is not available
			// TODO: Exactly what do they mean by "available"?
			// TODO: For now, we are assuming that if you gave a vqdev
			//       to the mmio_dev, the queues on it are "available."
			//       I am going to guard against the qsel being
			//       greater than the num_vqs on the vq_def, however.
			//       Since queues above this number don't exist, they
			//       definitely are not available.
			// Queue indices start at 0
		// TODO: Is not checking mmio_dev->vqdev->vqs[mmio_dev->qsel].qready
		//       the right thing to do here?
			if (mmio_dev->qsel >= mmio_dev->vqdev->num_vqs)
				return 0;
			return mmio_dev->vqdev->vqs[mmio_dev->qsel].qnum_max;
/*
Virtual queue ready bit
Writing one (0x1) to this register notifies the device that it can execute requests from
this virtual queue. Reading from this register returns the last value written to it. Both
read and write accesses apply to the queue selected by writing to QueueSel.
*/
		case VIRTIO_MMIO_QUEUE_READY:
			if (mmio_dev->qsel >= mmio_dev->vqdev->num_vqs)
				return 0;
			return mmio_dev->vqdev->vqs[mmio_dev->qsel].qready;
/*
Interrupt status
Reading from this register returns a bit mask of events that caused the
device interrupt to be asserted. The following events are possible:

Used Ring Update
- bit 0: the interrupt was asserted because the device has updated the Used Ring in
         at least one of the active virtual queues.

Configuration Change
- bit 1: the interrupt was asserted because the configuration of
         the device has changed.
*/
		case VIRTIO_MMIO_INTERRUPT_STATUS:
			return mmio_dev->isr;
/*
Device status
Reading from this register returns the current device status flags. Writing non-zero values
to this register sets the status flags, indicating the driver progress. Writing zero (0x0)
to this register triggers a device reset. See also p. 4.2.3.1 Device Initialization.
*/
		case VIRTIO_MMIO_STATUS:
			return mmio_dev->status;
/*
Configuration atomicity value
Reading from this register returns a value describing a version of the device-specific
configuration space (see Config). The driver can then access the configuration space and,
when finished, read ConfigGeneration again. If no part of the configuration space has changed
between these two ConfigGeneration reads, the returned values are identical. If the values are
different, the configuration space accesses were not atomic and the driver has to perform the
operations again. See also 2.3.
*/
		case VIRTIO_MMIO_CONFIG_GENERATION:
			return mmio_dev->cfg_gen;

		// Write-only register offsets:
		case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
		case VIRTIO_MMIO_DRIVER_FEATURES:
		case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
		case VIRTIO_MMIO_QUEUE_SEL:
		case VIRTIO_MMIO_QUEUE_NUM:
		case VIRTIO_MMIO_QUEUE_NOTIFY:
		case VIRTIO_MMIO_INTERRUPT_ACK:
		case VIRTIO_MMIO_QUEUE_DESC_LOW:
		case VIRTIO_MMIO_QUEUE_DESC_HIGH:
		case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
		case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
		case VIRTIO_MMIO_QUEUE_USED_LOW:
		case VIRTIO_MMIO_QUEUE_USED_HIGH:
			// Read of write-only register
			DRI_WARNX(mmio_dev->vqdev, "Attempt to read write-only device register offset 0x%x.", offset);
			return 0;
		default:
			// Bad register offset
			DRI_WARNX(mmio_dev->vqdev, "Attempt to read invalid device register offset 0x%x.", offset);
			return 0;
	}

	return 0;
}

// Writes are always 32 bits at a time! As far as I care and for the time being anyway,
// this (TODO) might change when we get to the device-specific config space
void virtio_mmio_wr_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa, uint32_t *value)
{
	uint64_t offset = gpa - mmio_dev->addr;
	struct virtio_vq *notified_queue;
	void *temp_ptr; // for facilitating bitwise ops on pointers

	// printf("in wr reg\n");

	if (!mmio_dev->vqdev) {
		// If there is no vqdev on the mmio_dev, we just make all registers write-ignored.
		// TODO: Is there a case where we want to provide an mmio transport with no vqdev backend?
		return;
	}

	if (offset >= VIRTIO_MMIO_CONFIG) {
		// TODO: Figure out what to do for writing the device config space
		// TODO: Will also probably have to set the ConfigGeneration stuff
		//       when I finally get to writing the device configuration space.
		//       And will definitely have to read that value twice to make sure
		//       the read was atomic.
		// TODO: ConfigGenreation is a read only register, so it is probably set by the device,
		//       and not by these handlers for the driver to touch the device through.
		DRI_ERRX(mmio_dev->vqdev, "Attempt to write the device configuration space! Not yet implemented!");
	}


// TODO: note that some comments are direct from the virtio mmio spec, and some of my notes too.
	// the spec I am referencing is: http://docs.oasis-open.org/virtio/virtio/v1.0/cs04/virtio-v1.0-cs04.html
	switch(offset) {
		case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
			mmio_dev->dev_feat_sel = *value;
			break;

		case VIRTIO_MMIO_DRIVER_FEATURES: // TODO: Test this one, make sure it works right
			if (mmio_dev->dri_feat_sel) {
				mmio_dev->vqdev->dri_feat &= 0xffffffff; // clear high 32 bits
				mmio_dev->vqdev->dri_feat |= ((uint64_t)(*value) << 32); // write high 32 bits
			} else {
				mmio_dev->vqdev->dri_feat &= ((uint64_t)0xffffffff << 32); // clear low 32 bits
				mmio_dev->vqdev->dri_feat |= *value; // write low 32 bits
			}
			break;

		case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
			mmio_dev->dri_feat_sel = *value;
			break;

		case VIRTIO_MMIO_QUEUE_SEL:
		// TODO: For now, if the value is above the number of vqs, we just won't set it.
		//       This may or may not be the right thing to do. QEMU just decides not to set
		//       it if it is greater than or equal to 1024 (their VIRTIO_QUEUE_MAX macro)
		// TODO: If we make sure it's less than num_vqs, we probably don't need to bounds-check
		//       in the read reg function.
			if (*value < mmio_dev->vqdev->num_vqs) {
				mmio_dev->qsel = *value;
			}
			break;

		case VIRTIO_MMIO_QUEUE_NUM:
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.num = *value;
			break;

		case VIRTIO_MMIO_QUEUE_READY:
			mmio_dev->vqdev->vqs[mmio_dev->qsel].qready = *value;
			break;

		case VIRTIO_MMIO_QUEUE_NOTIFY:
		// printf("in queue notify\n");
		// TODO: Ron was just setting the qsel here... is that the right thing?
		//       The spec is pretty clear that qsel is a different register than this.
		// TODO: Bounds check the value against num_vqs, first, obviously
		// TODO: It looks like QEMU would actually do some sort of notification handling
		//       when you would write to this register.
		// bounds check -> virtio_queue_notify -> virtio_queue_notify_vq ->
		//  if (vq->vring.desc && vq->handle_output) { vq->handle_output(vq->vdev, vq); }
		//  and handle_output is a method on QEMU's VirtQueue.
		// seems like when you add a queue to a vdev in qemu, you pass a handle_output function
		// pointer with it.
		// our version of vq->vring.desc is probably vq->qdesc but I have to make sure...
		// rather, qdesc might be the driver saying "hey, here's the address of the qdesc"
		// TODO: The driver tells the device that there are new buffers available in a queue
		//       by writing the index of the updated queue to this register. We'll have to figure
		//       out what to do with this information later.
		// TODO: Our model is to have spinning IO threads, because we just want to see stuff show up
		//       in the queues in memory. VIRTIO spec says to catch the write to this register, and use
		//       that as the trigger to process a queue.
		//       But that will cause a VM exit due to EPT violation, which makes things slow.
		//       So we will have to prevent the Linux virtio mmio driver from trying to write to
		//       queue notify. So we're deviating a little bit from the spec for this in order to make
		//       things faster.
		//       But for now, we're going to stay single threaded, and just call the handler function
		//       for the queue directly here, so we can tell if things work or not.
			if (*value < mmio_dev->vqdev->num_vqs) {
				// TODO: The arg is just for arbitrary use?
				// TODO: I'm passing 0 for now and just using my own custom handlers
				// TODO: Since we're just using the console right now I think this only ever calls consout
				// TODO: consin might stop working when we switch the rd/wr reg functions in vmrunkernel...
				// TODO: And this stuff did originally work...... so what did we screw with in the Linux driver?
				//qnotify_arg = &mmio_dev->vqdev->vqs[mmio_dev->qsel];

				// TODO: Can't use the original handlers in here, since they have (intended) infinite loops
				// TODO: Akaros vmrunkernel model would have done a pthread create, but this one will just call
				//       the handlers for now. We'll figure out how to do the spinning threads, and where the best
				//       place to spawn them, later on.

				notified_queue = &mmio_dev->vqdev->vqs[*value];


				//qnotify_arg->arg = qnotify_arg; // TODO: This makes the most sense to me right now, probably unnecessary though
				// TODO: Gotta figure out what that virtio pointer on the vq struct is for though...
				//       Ron treats it like a struct virtqueue (def in include/vmm/virtio.h) in his
				//       handler fns, and calls wait_for_vq_desc on it.

				//mmio_dev->vqdev->vqs[*value].f(notified_queue);
				if (notified_queue->eventfd > 0) {
					eventfd_write(notified_queue->eventfd, 1); // kick the queue's service thread
				}
				// TODO: Should we panic if there's no valid eventfd?

				/*
					What do we do about arg...
					before, Ron was passing a pointer to the queue selected in the QUEUE_PFN
					mmio handler. He sets the virtio pointer on the vq to a new struct virtqueue
					created as follows:
			va->arg->virtio = vring_new_virtqueue(mmio.qsel,
							  mmio.vqdev->vqs[mmio.qsel].qnum,
							  mmio.vqdev->vqs[mmio.qsel].qalign,
							  false, // weak_barriers
							  (void *)(mmio.vqdev->vqs[mmio.qsel].pfn * mmio.vqdev->vqs[mmio.qsel].qalign),
							  NULL, NULL, // callbacks
 							  mmio.vqdev->vqs[mmio.qsel].name);

 					I think the only reason to do this is so that you have the right type to use the
 					wait_for_vq_desc function on the queue. But there is a lot of magic going on here...

 					well, that, and when we eventually do want to do no-exit io, we'll need to make sure
 					that our queue uses exactly the same layout in memory as Linux's, because that's the
 					layout Linux's driver will use when it puts stuff in the queues.



				What our hacked up version of wait_for_vq_desc does:
				takes a pointer to a virtqueue,
				a pointer to a scatterlist,
				a pointer to an out len,
				and a pointer to an in len

				Note: sometimes I refer to the vring_virtqueue as the vq

				converts the virtqueue to a vring_virtqueue by calling to_vvq with the virtqueue
				a vring_virtqueue is a wrapper around the virtqueue that has the vring memory layout
				for the queue, some configuration bools, the head of the free buffer list, the number
				of (buffers?) added "since the last sync", the last used index "seen", some stuff to
				"figure out if their kicks are too delayed" and a data pointer (what data idk,
				it says "Tokens for callbacks.").

				sets uint16 last_avail to the ret of lg_last_avail( the vring_virtqueue ), which
				is actually just a macro that returns the vring_virtqueue's last_avail_idx.

				sets the in len and out len to 0

				then, while last_avail equals the vring_virtqueue's vring.avail->idx, it spins, continually doing:
					return 0 if the _vq is broken (_vq is the virtqueue inside the vring_virtqueue)
					// TODO: What are the conditions where a virtqueue can be broken?
					*1*
					clear the VRING_USED_F_NO_NOTIFY bit on the vq->vring.used->flags
					mb() // memory barrier
					if (last_avail != vq->vring.avail->idx)
						set the vq->vring.used->flags VRING_USED_F_NO_NOTIFY bit
					*2*
					set the vq->vring.used->flags VRING_USED_F_NO_NOTIFY_BIT

				Note: The original one would trigger_irq(vq) at *1*
					  and do an eventfd read (thus waiting) at *2*

				So the end result of that while loop is that the VRING_USED_F_NO_NOTIFY bit is
				set when we finaly exit the loop (unless the virtqueue was broken)

				Then we error if vq->vring.avail->idx - last_avail > vq->vring.num
				// TODO: Figure out what that error means.

				rmb(); // read memory barrier
				the purpose of that barrier is to "make sure we read the descriptor number *after*
				we read the ring update; don't let the cpu or compiler change the order"
				// TODO: Figure out and document why this is important

				Now comes the real meat of the function.

				first we set head = vq->vring.avail->ring[last_avail % vq->vring.num]
				then we increment vq->last_avail_idx

				then set the in len and out len to 0 again. I think this is the one that actually matters

				then:
				max = vq->vring.num;
				desc = vq->vring.desc;
				i = head;

				Ah, so it looks like num is maybe like our qnum_max?

				then they check desc[i].flags for VRING_DESC_F_INDIRECT to see if index i
				is an indirect entry. If it is an indirect entry, then the buffer contains
				a descriptor table and, after validating that the len of the buffer (i.e. the
				size of the table) is evenly divisible by the size of a vring_desc, they set
				`max` to the number of vring_descs that can fit in the buffer. Finally, they
				check that the desc[i].addr + desc[i].len neither exceeds the limit on guest
				memory nor wraps around. That memory limit is hardcoded as
				#define guest_limit ((1ULL<<48)-1)

				Then they have a loop that builds the iov (array of scatterlists), while
				checking that the addr and len of each desc that it's putting in the scatterlist
				neither exceeds the limit on guest memory nor wraps around.
				They also check that no output descriptors come after the input descriptors.
				// TODO: We'll probably have to do that.
				It knows to stop building the iov when (i = next_desc(desc, i, max)) != max

				Note: The static unsigned next_desc(struct vring_desc *desc, unsigned int i, unsigned int max)
				function does the following:
					return max if the descriptor says it doesn't chain (VRING_DESC_F_NEXT bit set on the desc[i].flags)
					otherwise set next to desc[i].next
					wmb() // write memory barrier // TODO: I'm not really sure that this is necessary?
					check that next is not greater or equal to max (cause error if so)
					return next


				So basically, the goal of our wait_for_vq_desc is to spin until the available index is
				incremented, and then

				The old one





				*/
			}
			break;

		case VIRTIO_MMIO_INTERRUPT_ACK:
		// TODO: It seems like you are supposed to write the same value as in isr
		//       here to indicate the event causing the interrupt was handled
		// TODO: Ron was just doing mmio.isr &= ~value, which would clear the isr register,
		//       if you wrote the right thing... if you didn't you'd have a really messed
		//       up looking interrupt in there...
		// QEMU does the same thing as Ron, but then they also call this virtio_update_irq(vdev) function
		// which calls virtio_notify_vector(vdev, VIRTIO_NO_VECTOR)
		// which gets the parent bus of the queue, checks if the bus has a notify function, and
		// then calls the notify function on the bus. I don't think we really want to model a bus yet...
		/*
			VIRTIO_NO_VECTOR is 0xffff

			The virtio_update_irq function was added in the "Separate virtio PCI code" commit, back in 2009
			At the time it looked for an update_irq binding on vdev->binding and called it with vdev->binding_opaque
			as an argument.

			The question is, why does qemu dive into the irq handler (ultimately) here?


		*/

		/*
			There are only two bits that matter in the interrupt status register
			0: interrupt because used ring updated in active vq
			1: interrupt because device config changed

		*/
			// TODO: If the driver MUST NOT set anything else, should we fail here?
			*value &= 0b11; // only the lower two bits matter, spec says driver MUST NOT set anything else
			mmio_dev->isr &= ~(*value);
			break;

		case VIRTIO_MMIO_STATUS:
			// NOTE: The status field is only one byte wide. See section 2.1 of virtio-v1.0-cs04
			mmio_dev->status = *value & 0xff;
			if (mmio_dev->status == 0) virtio_mmio_reset(mmio_dev);
			break;

		case VIRTIO_MMIO_QUEUE_DESC_LOW:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc
			                  & ((uint64_t)0xffffffff << 32)); // clear low bits
			temp_ptr = (void *) ((uint64_t)temp_ptr | *value); // write low bits
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc = temp_ptr; // assign the new value to the queue desc
			break;

		case VIRTIO_MMIO_QUEUE_DESC_HIGH:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc
			                  & ((uint64_t)0xffffffff)); // clear high bits
			temp_ptr = (void *) ((uint64_t)temp_ptr | ((uint64_t)(*value) << 32)); // write high bits
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc = temp_ptr; // assign the new value to the queue desc
			break;

		case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail
			                  & ((uint64_t)0xffffffff << 32)); // clear low bits
			temp_ptr = (void *) ((uint64_t)temp_ptr | *value); // write low bits
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail = temp_ptr; // assign the new value to the queue avail
			break;

		case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail
			                  & ((uint64_t)0xffffffff)); // clear high bits
			temp_ptr = (void *) ((uint64_t)temp_ptr | ((uint64_t)(*value) << 32)); // write high bits
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail = temp_ptr; // assign the new value to the queue avail
			break;

		case VIRTIO_MMIO_QUEUE_USED_LOW:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used
			                  & ((uint64_t)0xffffffff << 32)); // clear low bits
			temp_ptr = (void *) ((uint64_t)temp_ptr | *value); // write low bits
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used = temp_ptr; // assign the new value to the queue used
			break;

		case VIRTIO_MMIO_QUEUE_USED_HIGH:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used
			                  & ((uint64_t)0xffffffff)); // clear high bits
			temp_ptr = (void *) ((uint64_t)temp_ptr | ((uint64_t)(*value) << 32)); // write high bits
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used = temp_ptr; // assign the new value to the queue used
			break;

		// Read-only register offsets:
		case VIRTIO_MMIO_MAGIC_VALUE:
		case VIRTIO_MMIO_VERSION:
		case VIRTIO_MMIO_DEVICE_ID:
		case VIRTIO_MMIO_VENDOR_ID:
		case VIRTIO_MMIO_DEVICE_FEATURES:
		case VIRTIO_MMIO_QUEUE_NUM_MAX:
		case VIRTIO_MMIO_INTERRUPT_STATUS:
		case VIRTIO_MMIO_CONFIG_GENERATION:
			// Write to read-only register
			break;
		default:
			// Bad register offset
			break;
	}
}


void virtio_mmio_set_vring_irq(struct virtio_mmio_dev *mmio_dev)
{
	mmio_dev->isr |= VIRTIO_MMIO_INT_VRING;
}

