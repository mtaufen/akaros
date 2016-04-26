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

static void virtio_mmio_reset_cfg(struct virtio_mmio_dev *mmio_dev)
{
	// TODO: Reset the device-specific configuration space.
	VIRTIO_DRI_WARNX(mmio_dev->vqdev, "Attempt to reset device configuration space, not yet implemented!");
}

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

	// Warn if FAILED status bit is set.
	if (mmio_dev->status & VIRTIO_CONFIG_S_FAILED) {
		VIRTIO_DRI_WARNX(mmio_dev->vqdev,
			"The FAILED status bit is set."
			" The driver should probably reset the device before continuing.");
	}

	if (offset >= VIRTIO_MMIO_CONFIG) {
		// TODO: Implement reading the device config space
		VIRTIO_DRI_ERRX(mmio_dev->vqdev,
			"Attempt to read the device configuration space! Not yet implemented!");
	}


// TODO: the spec I am referencing is: http://docs.oasis-open.org/virtio/virtio/v1.0/cs04/virtio-v1.0-cs04.html
	switch(offset) {
		// Magic value
		// 0x74726976 (a Little Endian equivalent of the “virt” string).
		case VIRTIO_MMIO_MAGIC_VALUE:
			return VIRT_MAGIC;

		// Device version number
		// 0x2. Note: Legacy devices (see 4.2.4 Legacy interface) used 0x1.
		case VIRTIO_MMIO_VERSION:
			return VIRT_MMIO_VERSION;

		// Virtio Subsystem Device ID (see virtio-v1.0-cs04 sec. 5 for values)
		// Value 0x0 is used to define a system memory map with placeholder
		// devices at static, well known addresses.
		case VIRTIO_MMIO_DEVICE_ID:
			return mmio_dev->vqdev->dev_id;

		// Virtio Subsystem Vendor ID
		case VIRTIO_MMIO_VENDOR_ID:
			return VIRT_MMIO_VENDOR;

		// Flags representing features the device supports
		case VIRTIO_MMIO_DEVICE_FEATURES:
			if (!(mmio_dev->status & VIRTIO_CONFIG_S_DRIVER))
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
				         "Attempt to read device features before setting the DRIVER status bit."
				         " See virtio-v1.0-cs04 s3.1.1."
				         " 0x%x", mmio_dev->status);
			if (mmio_dev->dev_feat_sel) // high 32 bits requested
				return mmio_dev->vqdev->dev_feat >> 32;
			return mmio_dev->vqdev->dev_feat; // low 32 bits requested

		// Maximum virtual queue size
		// Returns the maximum size (number of elements) of the queue the device
		// is ready to process or zero (0x0) if the queue is not available.
		// Applies to the queue selected by writing to QueueSel.
		case VIRTIO_MMIO_QUEUE_NUM_MAX:
		// TODO: Are there other cases that count as "queue not available"
		// NOTE: Returning 0 here if !qready causes linux's driver
		//       to fail to initialize the vqs.
			if (mmio_dev->qsel >= mmio_dev->vqdev->num_vqs)
				return 0;
			return mmio_dev->vqdev->vqs[mmio_dev->qsel].qnum_max;

		// Virtual queue ready bit
		// Applies to the queue selected by writing to QueueSel.
		case VIRTIO_MMIO_QUEUE_READY:
			if (mmio_dev->qsel >= mmio_dev->vqdev->num_vqs)
				return 0;
			return mmio_dev->vqdev->vqs[mmio_dev->qsel].qready;

		// Interrupt status
		// Bit mask of events that caused the device interrupt to be asserted.
		// bit 0: Used Ring Update
		// bit 1: Configuration Change
		case VIRTIO_MMIO_INTERRUPT_STATUS:
			return mmio_dev->isr;

		// Device status
		case VIRTIO_MMIO_STATUS:
			return mmio_dev->status;

		// Configuration atomicity value
		// Contains a version for the device-specific configuration space
		// The driver checks this version before and after accessing the config
		// space, and if the values don't match it repeats the access.
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
			VIRTIO_DRI_ERRX(mmio_dev->vqdev, "Attempt to read write-only device register offset 0x%x.", offset);
			return 0;
		default:
			// Bad register offset
			VIRTIO_DRI_ERRX(mmio_dev->vqdev, "Attempt to read invalid device register offset 0x%x.", offset);
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

	// Warn if FAILED and trying to do something that is definitely not a reset.
	if (offset != VIRTIO_MMIO_STATUS
		&& (mmio_dev->status & VIRTIO_CONFIG_S_FAILED)) {
		VIRTIO_DRI_WARNX(mmio_dev->vqdev,
			"The FAILED status bit is set."
			" The driver should probably reset the device before continuing.");
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
			if (mmio_dev->status & VIRTIO_CONFIG_S_FEATURES_OK)
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"The driver may not accept new feature bits offered by"
					" the device after setting FEATURES_OK."); // TODO: But is it allowed to toggle already-accepted feature bits?
			else if (mmio_dev->dri_feat_sel) {
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
		// NOTE: We must allow the driver to write whatever they want to
		//       QueueSel, because QueueNumMax contians 0x0 for invalid
		//       QueueSel incices.
			mmio_dev->qsel = *value;
			break;

		// Virtual queue size
		// The queue size is the number of elements in the queue, thus in the
		// Descriptor Table, the Available Ring and the Used Ring. Writes
		// notify the device what size queue the driver will use.
		// This applies to the queue selected by writing to QueueSel.
		case VIRTIO_MMIO_QUEUE_NUM:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				// virtio-v1.0-cs04 4.2.2.2 MMIO Device Register Layout
				if (*value <= mmio_dev->vqdev->vqs[mmio_dev->qsel].qnum_max)
					mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.num = *value;
				else
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to write value greater than QueueNumMax"
						" to QueueNum register.");
			}
			break;

		// Virtual queue ready bit
		// Writing one (0x1) to this register notifies the device that it can
		// execute requests from the virtual queue selected by QueueSel.
		// TODO: If the driver writes 0x0 to queue ready, we probably have to make sure we
		// 		 stop processing the queue.
		case VIRTIO_MMIO_QUEUE_READY:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs)
				mmio_dev->vqdev->vqs[mmio_dev->qsel].qready = *value;
			break;

		// Queue notifier
		// Writing a queue index to this register notifies the device that
		// there are new buffers to process in that queue.
		case VIRTIO_MMIO_QUEUE_NOTIFY:
			if (!(mmio_dev->status & VIRTIO_CONFIG_S_DRIVER_OK))
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"Attempt to notify the device before setting"
					" the DRIVER_OK status bit.");
			else if (*value < mmio_dev->vqdev->num_vqs) {
				notified_queue = &mmio_dev->vqdev->vqs[*value];

				// kick the queue's service thread
				if (notified_queue->eventfd > 0)
					eventfd_write(notified_queue->eventfd, 1);
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
			if (*value & ~0b11)
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"Attempt to set undefined bits in InterruptACK register.");
			mmio_dev->isr &= ~(*value);
			break;

		// Device status
		// Writing non-zero values to this register sets the status flags.
		// Writing zero (0x0) to this register triggers a device reset.
		case VIRTIO_MMIO_STATUS:
			if (*value == 0)
				virtio_mmio_reset(mmio_dev);
			// virtio-v1.0-cs04 s2.1.1. driver must NOT clear a status bit
			else if (mmio_dev->status & ~(*value)) {
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"The driver must not clear any device status bits,"
					" except as a result of resetting the device.");
			}
			// virtio-v1.0-cs04 s2.1.1. MUST reset before re-init if FAILED set
			else if (mmio_dev->status & VIRTIO_CONFIG_S_FAILED
				&&   mmio_dev->status != *value) { // allow them to set the same status value again, though
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"The driver must reset the device after setting the FAILED"
					" status bit, before attempting to re-initialize the device.");
			}

			// NOTE: If a bit is not set in value, then at this point it
			//       CANNOT be set in status either, because if it were
			//       set in status, we would have just crashed with an
			//       error due to the attempt to clear a status bit.

			// Now we check that status bits are set in the correct
			// sequence during device initialization as described
			// in virtio-v1.0-cs04 s3.1.1.

			else if ((*value & VIRTIO_CONFIG_S_DRIVER)
			          && !(*value & VIRTIO_CONFIG_S_ACKNOWLEDGE)) {
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"Tried to set DRIVER status bit before setting"
					" ACKNOWLEDGE feature bit.");
			}
			else if ((*value & VIRTIO_CONFIG_S_FEATURES_OK)
			         && !((*value & VIRTIO_CONFIG_S_ACKNOWLEDGE)
				           && (*value & VIRTIO_CONFIG_S_DRIVER))) {
				// All those parentheses... Lisp must be making a comeback.
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"Tried to set FEATURES_OK status bit before setting both"
					" ACKNOWLEDGE and DRIVER status bits.");
			}
			else if ((*value & VIRTIO_CONFIG_S_DRIVER_OK)
			         && !((*value & VIRTIO_CONFIG_S_ACKNOWLEDGE)
				           && (*value & VIRTIO_CONFIG_S_DRIVER)
				           && (*value & VIRTIO_CONFIG_S_FEATURES_OK))) {
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"Tried to set DRIVER_OK status bit before setting all of"
					" ACKNOWLEDGE, DRIVER, and FEATURES_OK status bits.");
			}

			// NOTE: For now, we allow the driver to set all status bits up
			//       through FEATURES_OK in one fell swoop. The driver is, however,
			//       required to re-read FEATURES_OK after setting it to be sure
			//       that the driver-activated features are a subset of those
			//       supported by the device, so it must make an additional write
			//       to set DRIVER_OK.

			else if ((*value & VIRTIO_CONFIG_S_DRIVER_OK)
			         && !(mmio_dev->status & VIRTIO_CONFIG_S_FEATURES_OK)) {
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"The driver may not set FEATURES_OK and DRIVER_OK status"
					" bits simultaneously."
					" It must read back FEATURES_OK after setting it to ensure"
					" that its activated features are supported by the device"
					" before setting DRIVER_OK.");
			}
			else {
				// NOTE: Don't set the FEATURES_OK bit unless the driver
				//       activated a subset of the supported features prior to
				//       attempting to set FEATURES_OK.
				if (!(mmio_dev->status & VIRTIO_CONFIG_S_FEATURES_OK)
				    && (*value & VIRTIO_CONFIG_S_FEATURES_OK)
				    && (mmio_dev->vqdev->dri_feat
				    	& ~mmio_dev->vqdev->dev_feat)) {
					*value &= ~VIRTIO_CONFIG_S_FEATURES_OK;
				}
				// Device status is only a byte wide.
				mmio_dev->status = *value & 0xff;
			}
			break;

		// Queue's Descriptor Table 64 bit long physical address, low 32
		case VIRTIO_MMIO_QUEUE_DESC_LOW:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueDescLow on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc
				                  & ((uint64_t)0xffffffff << 32)); // clear low bits
				temp_ptr = (void *) ((uint64_t)temp_ptr | *value); // write low bits

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 16)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's descriptor table (%p) is"
						" misaligned. Address should be a multiple of 16.");

				// assign the new value to the queue desc
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc = temp_ptr;
			}
			break;

		// Queue's Descriptor Table 64 bit long physical address, high 32
		case VIRTIO_MMIO_QUEUE_DESC_HIGH:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueDescHigh on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc
				                  & ((uint64_t)0xffffffff)); // clear high bits
				temp_ptr = (void *) ((uint64_t)temp_ptr
				                  | ((uint64_t)(*value) << 32)); // write high bits

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 16)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's descriptor table (%p) is"
						" misaligned. Address should be a multiple of 16.");

				// assign the new value to the queue desc
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc = temp_ptr;
			}
			break;

		// Queue's Available Ring 64 bit long physical address, low 32
		case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueAvailLow on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail
				                  & ((uint64_t)0xffffffff << 32)); // clear low bits
				temp_ptr = (void *) ((uint64_t)temp_ptr | *value); // write low bits

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 2)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's available ring (%p) is"
						" misaligned. Address should be a multiple of 2.");

				// assign the new value to the queue avail
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail = temp_ptr;
			}
			break;

		// Queue's Available Ring 64 bit long physical address, high 32
		case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueAvailHigh on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail
				                  & ((uint64_t)0xffffffff)); // clear high bits
				temp_ptr = (void *) ((uint64_t)temp_ptr
				                  | ((uint64_t)(*value) << 32)); // write high bits

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 2)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's available ring (%p) is"
						" misaligned. Address should be a multiple of 2.");

				// assign the new value to the queue avail
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail = temp_ptr;
			}
			break;

		// Queue's Used Ring 64 bit long physical address, low 32
		case VIRTIO_MMIO_QUEUE_USED_LOW:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueUsedLow on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used
				                  & ((uint64_t)0xffffffff << 32)); // clear low bits
				temp_ptr = (void *) ((uint64_t)temp_ptr | *value); // write low bits

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 4)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's used ring (%p) is"
						" misaligned. Address should be a multiple of 4.");

				// assign the new value to the queue used
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used = temp_ptr;
			}
			break;

		// Queue's Used Ring 64 bit long physical address, high 32
		case VIRTIO_MMIO_QUEUE_USED_HIGH:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueUsedHigh on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				temp_ptr = (void *) ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used
				                  & ((uint64_t)0xffffffff)); // clear high bits
				temp_ptr = (void *) ((uint64_t)temp_ptr
				                  | ((uint64_t)(*value) << 32)); // write high bits

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 4)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's used ring (%p) is"
						" misaligned. Address should be a multiple of 4.");

				// assign the new value to the queue used
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used = temp_ptr;
			}
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
			VIRTIO_DRI_ERRX(mmio_dev->vqdev, "Attempt to write read-only device register offset 0x%x.", offset);
			break;
		default:
			// Bad register offset
			VIRTIO_DRI_ERRX(mmio_dev->vqdev, "Attempt to write invalid device register offset 0x%x.", offset);
			break;
	}
}


void virtio_mmio_set_vring_irq(struct virtio_mmio_dev *mmio_dev)
{
	mmio_dev->isr |= VIRTIO_MMIO_INT_VRING;
}

