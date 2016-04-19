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


#include <vmm/virtio_mmio.h>


#define VIRT_MAGIC 0x74726976 /* 'virt' */

#define VIRT_MMIO_VERSION 0x2

#define VIRT_MMIO_VENDOR 0x52414B41 /* 'AKAR' */


// Just need to implement read, write, and set irq,
 // maybe register_virtio_mmio... but that's pretty trivial (2 lines)

static void virtio_mmio_reset(struct virtio_mmio_dev *mmio_dev)
{
	// TODO: Actually reset the device!
}


// TODO: Prevent device from accessing virtual queue contents when QueueReady is 0x0

// Reads are ALWAYS 32 bit at a time
uint32_t virtio_mmio_rd_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa)
{
	uint64_t offset = gpa - mmio_dev->base_address;

	// TODO: Are there any more static fields to return here in a non-legacy device?
	// TODO: Is there a use case where you would want to read registers from the
	//       mmio_dev that would make sense even if vqdev.numvqs == 0?
/*	If there is no vqdev registered with this mmio device,
	or if there are no vqs on the device, we
	return all registers as 0 except for the virtio magic
	number, the mmio version, and the device vendor.
	*/
	if (!mmio_dev->vqdev || mmio_dev->vqdev.numvqs == 0) {
		switch(offset) {
		case VIRTIO_MMIO_MAGIC:
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
		printf("Tried to read the virtio mmio device configuration space, aborting.\n");
		abort();
	}


// TODO: note that some comments are direct from the virtio mmio spec, and some of my notes too.
	// the spec I am referencing is: http://docs.oasis-open.org/virtio/virtio/v1.0/cs04/virtio-v1.0-cs04.html
	switch(offset) {
/*
Magic value
0x74726976 (a Little Endian equivalent of the “virt” string).
*/
		case VIRTIO_MMIO_MAGIC:
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
			return mmio_dev->vqdev->dev;
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
			if (mmio_dev->device_features_sel) // high 32 bits requested
				return mmio_dev->vqdev->device_features >> 32;
			return mmio_dev->vqdev->device_features; // low 32 bits requested
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
			//       I am going to guard against the queue_sel being
			//       greater than the numvqs on the vq_def, however.
			//       Since queues above this number don't exist, they
			//       definitely are not available.
			// Queue indices start at 0
		// TODO: Is not checking mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qready
		//       the right thing to do here?
			if (mmio_dev->queue_sel >= mmio_dev->vqdev.numvqs)
				return 0;
			return mmio_dev->vqdev->vqs[mmio_dev->queue_sel].maxqnum;
/*
Virtual queue ready bit
Writing one (0x1) to this register notifies the device that it can execute requests from
this virtual queue. Reading from this register returns the last value written to it. Both
read and write accesses apply to the queue selected by writing to QueueSel.
*/
		case VIRTIO_MMIO_QUEUE_READY:
			if (mmio_dev->queue_sel >= mmio_dev->vqdev.numvqs)
				return 0;
			return mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qready;
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
			return mmio_dev->int_status;
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
			return 0;
		default:
			// Bad register offset
			return 0;
	}

	return 0;
}

// Writes are always 32 bits at a time! As far as I care and for the time being anyway,
// this (TODO) might change when we get to the device-specific config space
void virtio_mmio_wr_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa, uint32_t *value)
{
	uint64_t offset = gpa - mmio_dev->base_address;
	struct virtio_threadarg *qnotify_arg;

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
		printf("Tried to write the virtio mmio device configuration space, aborting.\n");
		abort();
	}


// TODO: note that some comments are direct from the virtio mmio spec, and some of my notes too.
	// the spec I am referencing is: http://docs.oasis-open.org/virtio/virtio/v1.0/cs04/virtio-v1.0-cs04.html
	switch(offset) {
		case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
			mmio_dev->device_features_sel = *value;
			break;

		case VIRTIO_MMIO_DRIVER_FEATURES: // TODO: Test this one, make sure it works right
			if (mmio_dev->driver_features_sel) {
				mmio_dev->vqdev->driver_features &= 0xffffffff; // clear high 32 bits
				mmio_dev->vqdev->driver_features |= ((uint64_t)(*value) << 32); // write high 32 bits
			} else {
				mmio_dev->vqdev->driver_features &= ((uint64_t)0xffffffff << 32); // clear low 32 bits
				mmio_dev->vqdev->driver_features |= *value; // write low 32 bits
			}
			break;

		case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
			mmio->driver_features_sel = *value;
			break;

		case VIRTIO_MMIO_QUEUE_SEL:
		// TODO: For now, if the value is above the number of vqs, we just won't set it.
		//       This may or may not be the right thing to do. QEMU just decides not to set
		//       it if it is greater than or equal to 1024 (their VIRTIO_QUEUE_MAX macro)
		// TODO: If we make sure it's less than numvqs, we probably don't need to bounds-check
		//       in the read reg function.
			if (*value < mmio_dev->vqdev.numvqs) {
				mmio_dev->vqdev->queue_sel = *value;
			}
			break;

		case VIRTIO_MMIO_QUEUE_NUM:
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qnum = *value;
			break;

		case VIRTIO_MMIO_QUEUE_READY:
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qready = *value;
			break;

		case VIRTIO_MMIO_QUEUE_NOTIFY:
		// TODO: Ron was just setting the qsel here... is that the right thing?
		//       The spec is pretty clear that qsel is a different register than this.
		// TODO: Bounds check the value against numvqs, first, obviously
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
			if (*value < mmio_dev->vqdev.numvqs) {
				// TODO: The arg is just for arbitrary use?
				// TODO: I'm passing 0 for now and just using my own custom handlers
				// TODO: Since we're just using the console right now I think this only ever calls consout
				// TODO: consin might stop working when we switch the rd/wr reg functions in vmrunkernel...
				// TODO: And this stuff did originally work...... so what did we screw with in the Linux driver?
				//qnotify_arg = &mmio_dev->vqdev->vqs[mmio_dev->qsel];
				mmio_dev->vqdev->vqs[*value].f(0);
			}
			break;

		case VIRTIO_MMIO_INTERRUPT_ACK:
		// TODO: It seems like you are supposed to write the same value as in int_status
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
			1: interrupt because dev config changed

			the device sets the interrupt status reg
			the driver acks handling by writing the same flags for the events it handled to the ack reg
			The spec says that the driver MUST NOT set any of the undefined bits in this value
			so we need to protect against that.

		*/
			*value &= 0b11; // only the lower two bits matter, spec says driver MUST NOT set anything else
			mmio_dev->int_status &= ~(*value);
			break;

		case VIRTIO_MMIO_STATUS:
		// TODO: Ron was doing mmio.status |= value & 0xff;
		//       why only letting the last byte through?
		//       I guess this register is only a byte?
		// TODO: Should we also check the DRIVER_OK bit of the status reg here?
		//       qemu starts/stops their virtio mmio ioeventfd stuff if the
		//       VIRTIO_CONFIG_S_DRIVER_OK bit is set/not set
		// TODO: I guess status is only a byte? Kind of seems that way in the spec...
			mmio_dev->status = *value & 0xff;
			if (mmio_dev->status == 0) virtio_mmio_reset(mmio_dev);
			break;

		case VIRTIO_MMIO_QUEUE_DESC_LOW: // TODO: Test this
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qdesc &= ((uint64_t)0xffffffff << 32); // clear low bits
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qdesc |= *value; // write low bits
			break;

		case VIRTIO_MMIO_QUEUE_DESC_HIGH: // TODO: Test this
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qdesc &= 0xffffffff; // clear high bits
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qdesc |= ((uint64_t)(*value) << 32); // write high bits
			break;

		case VIRTIO_MMIO_QUEUE_AVAIL_LOW: // TODO: Test this
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qavail &= ((uint64_t)0xffffffff << 32); // clear low bits
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qavail |= *value; // write low bits
			break;

		case VIRTIO_MMIO_QUEUE_AVAIL_HIGH: // TODO: Test this
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qavail &= 0xffffffff; // clear high bits
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qavail |= ((uint64_t)(*value) << 32); // write high bits
			break;

		case VIRTIO_MMIO_QUEUE_USED_LOW: // TODO: Test this
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qused &= ((uint64_t)0xffffffff << 32); // clear low bits
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qused |= *value; // write low bits
			break;

		case VIRTIO_MMIO_QUEUE_USED_HIGH: // TODO: Test this
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qused &= 0xffffffff; // clear high bits
			mmio_dev->vqdev->vqs[mmio_dev->queue_sel].qused |= ((uint64_t)(*value) << 32); // write high bits
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

	return 0;
}


void virtio_mmio_set_vring_irq(struct virtio_mmio_dev *mmio_dev)
{
	mmio_dev->int_status |= VIRTIO_MMIO_INT_VRING;
}

