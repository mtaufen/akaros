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

#include <stdio.h>
#include <sys/eventfd.h>
#include <vmm/virtio_config.h>
#include <vmm/virtio_mmio.h>

#define VIRT_MAGIC 0x74726976 /* 'virt' */

#define VIRT_MMIO_VERSION 0x2

#define VIRT_MMIO_VENDOR 0x52414B41 /* 'AKAR' */

static void virtio_mmio_reset(struct virtio_mmio_dev *mmio_dev)
{
	// TODO: Audit the reset function!
	int i;

	if (!mmio_dev->vqdev)
		return;

	fprintf(stderr, "virtio mmio device reset: %s\n", mmio_dev->vqdev->name);

	mmio_dev->vqdev->dri_feat = 0;
	mmio_dev->status = 0;
	mmio_dev->isr = 0;

	for (i = 0; i < mmio_dev->vqdev->num_vqs; ++i) {
		// TODO: Should probably kill the handler thread before doing
		//       anything else. MUST NOT process buffers until reinit!
		// TODO: If we kill, what does that mean for the eventfds?
		// TODO: If we kill, where do we launch again in the future?
		mmio_dev->vqdev->vqs[i].qready = 0;
		mmio_dev->vqdev->vqs[i].last_avail = 0;
	}

// TODO: Put parts of the virito spec relating to reset in here

	virtio_mmio_reset_cfg(mmio_dev);

}

static void virtio_mmio_reset_cfg(struct virtio_mmio_dev *mmio_dev)
{
	// TODO: Reset the device-specific configuration space.
	VIRTIO_DRI_WARNX(mmio_dev->vqdev, "Attempt to reset device configuration space, not yet implemented!");
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
		// TODO: Implement reading the device config space
		VIRTIO_DRI_ERRX(mmio_dev->vqdev, "Attempt to read the device configuration space! Not yet implemented!");
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
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
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
			VIRTIO_DRI_WARNX(mmio_dev->vqdev, "Attempt to read write-only device register offset 0x%x.", offset);
			return 0;
		default:
			// Bad register offset
			VIRTIO_DRI_WARNX(mmio_dev->vqdev, "Attempt to read invalid device register offset 0x%x.", offset);
			return 0;
	}

	return 0;
}

void virtio_mmio_wr_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa, uint32_t *value)
{
	uint64_t offset = gpa - mmio_dev->addr;
	struct virtio_vq *notified_queue;
	void *temp_ptr; // for facilitating bitwise ops on pointers

	if (!mmio_dev->vqdev) {
		// If there is no vqdev on the mmio_dev, we just make all registers write-ignored.
		// TODO: Is there a case where we want to provide an mmio transport with no vqdev backend?
		return;
	}

	if (offset >= VIRTIO_MMIO_CONFIG) {
		// TODO: Implement writing the device config space
		VIRTIO_DRI_ERRX(mmio_dev->vqdev, "Attempt to write the device configuration space! Not yet implemented!");
	}


// TODO: note that some comments are direct from the virtio mmio spec, and some of my notes too.
	// the spec I am referencing is: http://docs.oasis-open.org/virtio/virtio/v1.0/cs04/virtio-v1.0-cs04.html
	// TODO: Mention Table 4.1 in the spec somewhere. And that the names of registers
	//       used in the comments are taken from that table.
	switch(offset) {

		// Device (host) features word selection.
		case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
			mmio_dev->dev_feat_sel = *value;
			break;

		// Device feature flags activated by the driver
		case VIRTIO_MMIO_DRIVER_FEATURES:
			if (mmio_dev->dri_feat_sel) {
				mmio_dev->vqdev->dri_feat &= 0xffffffff; // clear high 32 bits
				mmio_dev->vqdev->dri_feat |= ((uint64_t)(*value) << 32); // write high 32 bits
			} else {
				mmio_dev->vqdev->dri_feat &= ((uint64_t)0xffffffff << 32); // clear low 32 bits
				mmio_dev->vqdev->dri_feat |= *value; // write low 32 bits
			}
			break;

		// Activated (guest) features word selection
		case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
			mmio_dev->dri_feat_sel = *value;
			break;

		// Virtual queue index
		// Selects the virtual queue that QueueNumMax, QueueNum, QueueReady,
		// QueueDescLow, QueueDescHigh, QueueAvailLow, QueueAvailHigh,
		// QueueUsedLow and QueueUsedHigh apply to. The index number of the
 		// first queue is zero (0x0).
		case VIRTIO_MMIO_QUEUE_SEL:
		// TODO: If we make sure it's less than num_vqs, we probably don't need to bounds-check
		//       in the read reg function.
			if (*value < mmio_dev->vqdev->num_vqs) {
				mmio_dev->qsel = *value;
			}
			break;

		// Virtual queue size
		// Queue size is the number of elements in the queue, thus in the
		// Descriptor Table, the Available Ring and the Used Ring. Writes
		// notify the device what size queue the driver will use.
		// This applies to the queue selected by writing to QueueSel.
		case VIRTIO_MMIO_QUEUE_NUM:
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.num = *value;
			break;

		// Virtual queue ready bit
		// Writing one (0x1) to this register notifies the device that it can
		// execute requests from the virtual queue selected by QueueSel.
		case VIRTIO_MMIO_QUEUE_READY:
			mmio_dev->vqdev->vqs[mmio_dev->qsel].qready = *value;
			break;

		// Queue notifier
		// Writing a queue index to this register notifies the device that
		// there are new buffers to process in that queue.
		case VIRTIO_MMIO_QUEUE_NOTIFY:
			if (*value < mmio_dev->vqdev->num_vqs) {
				notified_queue = &mmio_dev->vqdev->vqs[*value];

				if (notified_queue->eventfd > 0) {
					// kick the queue's service thread
					eventfd_write(notified_queue->eventfd, 1);
				}
				// TODO: Should we panic if there's no valid eventfd?
			}
			break;

		// Interrupt acknowledge
		// Writing a value with bits set as defined in InterruptStatus to this
		// register notifies the device that events causing the interrupt have
		// been handled.
		case VIRTIO_MMIO_INTERRUPT_ACK:
			// TODO: Is there anything the device actually has to DO on an interrupt ack
			//       other than clear the acked interrupt bits in isr?
			// TODO: If the driver MUST NOT set anything else, should we fail here if they try?
			*value &= 0b11; // only the lower two bits matter, driver MUST NOT set anything else
			mmio_dev->isr &= ~(*value);
			break;

		// Device status
		// Writing non-zero values to this register sets the status flags.
		// Writing zero (0x0) to this register triggers a device reset.
		case VIRTIO_MMIO_STATUS:
			// NOTE: The status field is only one byte wide.
			// See section 2.1 of virtio-v1.0-cs04
			if (mmio_dev->status == 0) virtio_mmio_reset(mmio_dev);
			else mmio_dev->status = *value & 0xff;
			break;


		// Queue's Descriptor Table 64 bit long physical address, low 32
		case VIRTIO_MMIO_QUEUE_DESC_LOW:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc
			                  & ((uint64_t)0xffffffff << 32)); // clear low bits
			temp_ptr = (void *) ((uint64_t)temp_ptr | *value); // write low bits
			// assign the new value to the queue desc
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc = temp_ptr;
			break;

		// Queue's Descriptor Table 64 bit long physical address, high 32
		case VIRTIO_MMIO_QUEUE_DESC_HIGH:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc
			                  & ((uint64_t)0xffffffff)); // clear high bits
			temp_ptr = (void *) ((uint64_t)temp_ptr
			                  | ((uint64_t)(*value) << 32)); // write high bits
			// assign the new value to the queue desc
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc = temp_ptr;
			break;

		// Queue's Available Ring 64 bit long physical address, low 32
		case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail
			                  & ((uint64_t)0xffffffff << 32)); // clear low bits
			temp_ptr = (void *) ((uint64_t)temp_ptr | *value); // write low bits
			// assign the new value to the queue avail
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail = temp_ptr;
			break;

		// Queue's Available Ring 64 bit long physical address, high 32
		case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail
			                  & ((uint64_t)0xffffffff)); // clear high bits
			temp_ptr = (void *) ((uint64_t)temp_ptr
			                  | ((uint64_t)(*value) << 32)); // write high bits
			// assign the new value to the queue avail
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail = temp_ptr;
			break;

		// Queue's Used Ring 64 bit long physical address, low 32
		case VIRTIO_MMIO_QUEUE_USED_LOW:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used
			                  & ((uint64_t)0xffffffff << 32)); // clear low bits
			temp_ptr = (void *) ((uint64_t)temp_ptr | *value); // write low bits
			// assign the new value to the queue used
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used = temp_ptr;
			break;

		// Queue's Used Ring 64 bit long physical address, high 32
		case VIRTIO_MMIO_QUEUE_USED_HIGH:
			temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used
			                  & ((uint64_t)0xffffffff)); // clear high bits
			temp_ptr = (void *) ((uint64_t)temp_ptr
			                  | ((uint64_t)(*value) << 32)); // write high bits
			// assign the new value to the queue used
			mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used = temp_ptr;
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

