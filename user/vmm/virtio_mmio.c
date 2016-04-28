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

void virtio_mmio_set_vring_irq(struct virtio_mmio_dev *mmio_dev)
{
	mmio_dev->isr |= VIRTIO_MMIO_INT_VRING;
}

void virtio_mmio_set_cfg_irq(struct virtio_mmio_dev *mmio_dev)
{
	mmio_dev->isr |= VIRTIO_MMIO_INT_CONFIG;
}

static void virtio_mmio_reset_cfg(struct virtio_mmio_dev *mmio_dev)
{
	// TODO: Reset the device-specific configuration space.
	VIRTIO_DRI_WARNX(mmio_dev->vqdev, "Attempt to reset device configuration space, not yet implemented!");
}

static void virtio_mmio_reset(struct virtio_mmio_dev *mmio_dev)
{
	// TODO: Audit the reset function! There may be other fields we should reset.
	int i;

	if (!mmio_dev->vqdev)
		return;

	fprintf(stderr, "virtio mmio device reset: %s\n", mmio_dev->vqdev->name);

	// Clear any driver-activated feature bits
	mmio_dev->vqdev->dri_feat = 0;

	// virtio-v1.0-cs04 s2.1.2 Device Status Field
	// The device MUST initialize device status to 0 upon reset
	mmio_dev->status = 0;

	// virtio-v1.0-cs04 s4.2.2.1 MMIO Device Register Layout
	// Upon reset, the device MUST clear all bits in InterruptStatus
	mmio_dev->isr = 0;

	// virtio-v1.0-cs04 s4.2.2.1 MMIO Device Register Layout
	// Upon reset, the device MUST clear...ready bits in the QueueReady
	// register for all queues in the device.
	for (i = 0; i < mmio_dev->vqdev->num_vqs; ++i) {
		// TODO: Should probably kill the handler thread before doing
		//       anything else. MUST NOT process buffers until reinit!
		//       If we kill, what does that mean for the eventfds?
		//       If we kill, where do we launch again in the future?
		mmio_dev->vqdev->vqs[i].qready = 0;
		mmio_dev->vqdev->vqs[i].last_avail = 0;
	}

	// TODO: The device MUST NOT consume buffers or notify the driver before DRIVER_OK

	virtio_mmio_reset_cfg(mmio_dev);

}

uint32_t virtio_mmio_rd(struct virtio_mmio_dev *mmio_dev,
                        uint64_t gpa, uint8_t size)
{
	uint64_t offset = gpa - mmio_dev->addr;
	uint8_t *target; // target of read from device-specific config space

	// virtio-v1.0-cs04 s4.2.3.1.1 Device Initialization (MMIO section)
	if (mmio_dev->vqdev->dev_id == 0
		&& offset != VIRTIO_MMIO_MAGIC_VALUE
		&& offset != VIRTIO_MMIO_VERSION
		&& offset != VIRTIO_MMIO_DEVICE_ID)
		VIRTIO_DRI_ERRX(mmio_dev->vqdev,
			"Attempt to read from a register not MagicValue, Version, or"
			" DeviceID on a device whose DeviceID is 0x0");

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

	// TODO: I could only do a limited amount of testing on the device-
	//       specific config space, because I was limited to seeing what
	//       the guest driver for the console device would do. You may
	//       run into issues when you implement virtio-net, since that
	//       does more with the device-specific config.
	if (offset >= VIRTIO_MMIO_CONFIG) {
		offset -= VIRTIO_MMIO_CONFIG;

		if (!mmio_dev->vqdev->cfg || mmio_dev->vqdev->cfg_sz == 0) {
			VIRTIO_DEV_ERRX(mmio_dev->vqdev,
				"Driver attempted to read the device-specific configuration"
				" space, but the device failed to provide it.");
		}

		// virtio-v1.0-cs04 s3.1.1 Device Initialization
		if (!(mmio_dev->status & VIRTIO_CONFIG_S_DRIVER)) {
			VIRTIO_DRI_ERRX(mmio_dev->vqdev,
				"Driver attempted to read the device-specific configuration"
				" space before setting the DRIVER status bit.");
		}

		if ((offset + size) > mmio_dev->vqdev->cfg_sz
			|| (offset + size) < offset) {
			VIRTIO_DRI_ERRX(mmio_dev->vqdev,
				"Attempt to read invalid offset of the device specific"
				" configuration space, or (offset + read width)"
				" wrapped around.");
		}

		target = (uint8_t*)((uint64_t)mmio_dev->vqdev->cfg + offset);

		// TODO: Check that size matches the size of the field at offset
		//       for the given device? i.e. virtio_console_config.rows
		//       should only be accessible via a 16 bit read or write.
		//       I haven't done this yet, it will be a significant
		//       undertaking and maintainence commitment, because you
		//       will have to do it for every virtio device you
		//       want to use in the future.
		switch(size) {
			case 1:
				return *((uint8_t*)target);
			case 2:
				if ((uint64_t)target % 2 != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"The driver must use 16 bit aligned reads for"
						" reading from 16 bit values in the device-specific"
						" configuration space.");
				return *((uint16_t*)target);
			case 4:
				if ((uint64_t)target % 4 != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"The driver must use 32 bit aligned reads for"
						" reading from 32 or 64 bit values in the"
						" device-specific configuration space.");
				return *((uint32_t*)target);
			default:
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"The driver must use 8, 16, or 32 bit wide and aligned"
					" reads for reading from the device-specific"
					" configuration space.");
		}
	}

	// virtio-v1.0-cs04 4.2.2.2 MMIO Device Register Layout
	if (size != 4 || (offset % 4) != 0) {
		VIRTIO_DRI_ERRX(mmio_dev->vqdev,
			"The driver must only use 32 bit wide and aligned reads for"
			" reading the control registers on the MMIO transport.",
			size, offset, offset%32);
	}

	// virtio-v1.0-cs04 Table 4.1
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
				         "Attempt to read device features before setting"
				         " the DRIVER status bit."
				         " See virtio-v1.0-cs04 s3.1.1."
				         " Current status is 0x%x", mmio_dev->status);

			// virtio-v1.0-cs04 s6.2 Reserved Feature Bits
			if (!(mmio_dev->vqdev->dev_feat & ((uint64_t)1<<VIRTIO_F_VERSION_1)))
				VIRTIO_DEV_ERRX(mmio_dev->vqdev,
					"The device must offer the VIRTIO_F_VERSION_1"
					" feature bit (bit 32). You didn't."
					" You must be a bad person.");

			if (mmio_dev->dev_feat_sel) // high 32 bits requested
				return mmio_dev->vqdev->dev_feat >> 32;
			return mmio_dev->vqdev->dev_feat; // low 32 bits requested

		// Maximum virtual queue size
		// Returns the maximum size (number of elements) of the queue the device
		// is ready to process or zero (0x0) if the queue is not available.
		// Applies to the queue selected by writing to QueueSel.
		case VIRTIO_MMIO_QUEUE_NUM_MAX:
		// TODO: Are there other cases that count as "queue not available"
		// NOTE: !qready does not count as queue not available.
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
			VIRTIO_DRI_ERRX(mmio_dev->vqdev,
				"Attempt to read write-only device register offset 0x%x.",
				offset);
			return 0;
		default:
			// Bad register offset
			VIRTIO_DRI_ERRX(mmio_dev->vqdev,
				"Attempt to read invalid device register offset 0x%x.",
				offset);
			return 0;
	}

	return 0;
}

// Based on check_virtqueue from lguest.c
// We call this when the driver writes 0x1 to QueueReady
static void check_vring(struct virtio_vq *vq) {
	// First make sure that the pointers on the vring are all valid:
	virtio_check_pointer(vq, (uint64_t)vq->vring.desc,
	                     sizeof(*vq->vring.desc) * vq->vring.num,
	                     __FILE__, __LINE__);
	virtio_check_pointer(vq, (uint64_t)vq->vring.avail,
	                     sizeof(*vq->vring.avail) * vq->vring.num,
	                     __FILE__, __LINE__);
	virtio_check_pointer(vq, (uint64_t)vq->vring.used,
	                     sizeof(*vq->vring.used) * vq->vring.num,
	                     __FILE__, __LINE__);


	// virtio-v1.0-cs04 s2.4.9.1 Virtqueue Notification Suppression
	// The driver MUST initialize flags in the used ring to 0 when
	// allocating the used ring.
	if (vq->vring.used->flags != 0)
		VIRTIO_DRI_ERRX(vq->vqdev,
			"The driver must initialize the flags field of the used ring"
			" to 0 when allocating the used ring.");
}

void virtio_mmio_wr(struct virtio_mmio_dev *mmio_dev, uint64_t gpa,
                    uint8_t size, uint32_t *value)
{
	uint64_t offset = gpa - mmio_dev->addr;
	struct virtio_vq *notified_queue;
	uint8_t *target; // target of write to device-specific config space
	void *temp_ptr; // for facilitating bitwise ops on pointers

	if (!mmio_dev->vqdev) {
		// If there is no vqdev on the mmio_dev,
		// we just make all registers write-ignored.
		return;
	}

	// virtio-v1.0-cs04 s4.2.3.1.1 Device Initialization (MMIO)
	if (mmio_dev->vqdev->dev_id == 0)
		VIRTIO_DRI_ERRX(mmio_dev->vqdev,
			"Attempt to write to a device whose DeviceID is 0x0");

	// Warn if FAILED and trying to do something that is definitely not a reset.
	if (offset != VIRTIO_MMIO_STATUS
		&& (mmio_dev->status & VIRTIO_CONFIG_S_FAILED)) {
		VIRTIO_DRI_WARNX(mmio_dev->vqdev,
			"The FAILED status bit is set."
			" The driver should probably reset the device before continuing.");
	}

	// TODO: I could only do a limited amount of testing on the device-
	//       specific config space, because I was limited to seeing what
	//       the guest driver for the console device would do. You may
	//       run into issues when you implement virtio-net, since that
	//       does more with the device-specific config. (In fact, I don't think
	//       the guest driver ever even tried to write the device-specific
	//       config space for the console, so this section is entirely untested)
	if (offset >= VIRTIO_MMIO_CONFIG) {
		offset -= VIRTIO_MMIO_CONFIG;

		if (!mmio_dev->vqdev->cfg || mmio_dev->vqdev->cfg_sz == 0) {
			VIRTIO_DEV_ERRX(mmio_dev->vqdev,
				"Driver attempted to write to the device-specific configuration"
				" space, but the device failed to provide it.");
		}

		// virtio-v1.0-cs04 s3.1.1 Device Initialization
		if (!(mmio_dev->status & VIRTIO_CONFIG_S_FEATURES_OK)) {
			VIRTIO_DRI_ERRX(mmio_dev->vqdev,
				"Driver attempted to write the device-specific configuration"
				" space before setting the FEATURES_OK status bit.");
		}

		if ((offset + size) > mmio_dev->vqdev->cfg_sz
			|| (offset + size) < offset) {
			VIRTIO_DRI_ERRX(mmio_dev->vqdev,
				"Attempt to write invalid offset of the device specific"
				" configuration space, or (offset + write width)"
				" wrapped around.");
		}

		target = (uint8_t*)((uint64_t)mmio_dev->vqdev->cfg + offset);

		// TODO: Check that size matches the size of the field at offset
		//       for the given device? i.e. virtio_console_config.rows
		//       should only be accessible via a 16 bit read or write.
		//       I haven't done this yet, it will be a significant
		//       undertaking and maintainence commitment, because you
		//       will have to do it for every virtio device you
		//       want to use in the future.
		switch(size) {
			case 1:
				*((uint8_t*)target) = *((uint8_t*)value);
			case 2:
				if ((uint64_t)target % 2 != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"The driver must use 16 bit aligned writes for"
						" writing to 16 bit values in the device-specific"
						" configuration space.");
				*((uint16_t*)target) = *((uint16_t*)value);
			case 4:
				if ((uint64_t)target % 4 != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"The driver must use 32 bit aligned writes for"
						" writing to 32 or 64 bit values in the device-specific"
						" configuration space.");
				*((uint32_t*)target) = *((uint32_t*)value);
			default:
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"The driver must use 8, 16, or 32 bit wide and aligned"
					" writes for writing to the device-specific"
					" configuration space.");
		}

		// Increment cfg_gen because the device-specific config changed
		mmio_dev->cfg_gen++;

		// Notify the driver that the device-specific config changed
		virtio_mmio_set_cfg_irq(mmio_dev);
		if (mmio_dev->poke_guest)
			mmio_dev->poke_guest();

		return;
	}

	// virtio-v1.0-cs04 4.2.2.2 MMIO Device Register Layout
	if (size != 4 || (offset % 4) != 0) {
		VIRTIO_DRI_ERRX(mmio_dev->vqdev,
			"The driver must only use 32 bit wide and aligned writes for"
			" writing the control registers on the MMIO transport.");
	}

	// virtio-v1.0-cs04 Table 4.1
	switch(offset) {

		// Device (host) features word selection.
		case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
			mmio_dev->dev_feat_sel = *value;
			break;

		// Device feature flags activated by the driver
		case VIRTIO_MMIO_DRIVER_FEATURES:
			// virtio-v1.0-cs04 s3.1.1 Device Initialization
			if (mmio_dev->status & VIRTIO_CONFIG_S_FEATURES_OK) {
				// NOTE: The spec just says the driver isn't allowed to accept
				//       NEW feature bits after setting FEATURES_OK. Although
				//       the language makes it seem like it might be fine to
				//       let the driver un-accept features after it sets
				//       FEATURES_OK, this would require very careful handling,
				//       so for now we just don't allow the driver to write to
				//       the DriverFeatures register after FEATURES_OK is set.
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"The driver may not accept (i.e. activate) new feature bits"
					" offered by the device after setting FEATURES_OK.");
			}
			else if (mmio_dev->dri_feat_sel) {
				// clear high 32 bits
				mmio_dev->vqdev->dri_feat &= 0xffffffff;
				// write high 32 bits
				mmio_dev->vqdev->dri_feat |= ((uint64_t)(*value) << 32);
			} else {
				// clear low 32 bits
				mmio_dev->vqdev->dri_feat &= ((uint64_t)0xffffffff << 32);
				// write low 32 bits
				mmio_dev->vqdev->dri_feat |= *value;
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
		//       QueueSel indices.
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
			else {
				VIRTIO_DRI_WARNX(mmio_dev->vqdev,
					"Attempt to write QueueNum register for invalid QueueSel."
					" QueueSel was %u, but the number of queues is %u.",
					mmio_dev->qsel, mmio_dev->vqdev->num_vqs);
			}
			break;

		// Virtual queue ready bit
		// Writing one (0x1) to this register notifies the device that it can
		// execute requests from the virtual queue selected by QueueSel.
		case VIRTIO_MMIO_QUEUE_READY:
		// TODO: If the driver writes 0x0 to queue ready, we probably have to make sure we
		// 		 stop processing the queue, which probably means we cancel processing of buffers,
		//       or just wait for everything in flight to finish, before we finally write 0 here.
		//       I wonder if I can put a signal handler on a thread for a signal that means
		//       "stop/finish up your processing and then set your queue's qready to 0x0"?
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				check_vring(&mmio_dev->vqdev->vqs[mmio_dev->qsel]);
				mmio_dev->vqdev->vqs[mmio_dev->qsel].qready = *value;
			}
			else {
				VIRTIO_DRI_WARNX(mmio_dev->vqdev,
					"Attempt to write QueueReady register for invalid QueueSel."
					" QueueSel was %u, but the number of queues is %u.",
					mmio_dev->qsel, mmio_dev->vqdev->num_vqs);
			}
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
				else
					VIRTIO_DEV_ERRX(mmio_dev->vqdev,
						"You need to provide a valid eventfd on your virtio_vq"
						" so that it can be kicked when the driver writes to"
						" QueueNotify.");
			}
			break;

		// Interrupt acknowledge
		// Writing a value with bits set as defined in InterruptStatus to this
		// register notifies the device that events causing the interrupt have
		// been handled.
		case VIRTIO_MMIO_INTERRUPT_ACK:
			// TODO: Is there anything the device actually has to DO on an
			//       ack other than clear the acked interrupt bits in isr?
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
				&&   mmio_dev->status != *value) {
				// NOTE: This fails if the driver tries to *change* the status
				//       after the FAILED bit is set. The driver can set the
				//       same status again all it wants.
				VIRTIO_DRI_ERRX(mmio_dev->vqdev,
					"The driver must reset the device after setting the FAILED"
					" status bit, before attempting to re-initialize "
					" the device.");
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

				// TODO: We still need to check that no feature is accepted which
				//       depends on a not-accepted feature! This will be a lot of work...
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

				// clear low bits
				temp_ptr = (void *)
				    ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc
				  & ((uint64_t)0xffffffff << 32));
				// write low bits
				temp_ptr = (void *) ((uint64_t)temp_ptr | *value);

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 16)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's descriptor table (%p) is"
						" misaligned. Address should be a multiple of 16.");

				// assign the new value to the queue desc
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc = temp_ptr;
			}
			else {
				VIRTIO_DRI_WARNX(mmio_dev->vqdev,
					"Attempt to write QueueDescLow register for invalid"
					" QueueSel."
					" QueueSel was %u, but the number of queues is %u.",
					mmio_dev->qsel, mmio_dev->vqdev->num_vqs);
			}
			break;

		// Queue's Descriptor Table 64 bit long physical address, high 32
		case VIRTIO_MMIO_QUEUE_DESC_HIGH:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueDescHigh on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				// clear high bits
				temp_ptr = (void *)
				    ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc
				  & ((uint64_t)0xffffffff));
				// write high bits
				temp_ptr = (void *) ((uint64_t)temp_ptr
				                  | ((uint64_t)(*value) << 32));

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 16)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's descriptor table (%p) is"
						" misaligned. Address should be a multiple of 16.");

				// assign the new value to the queue desc
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.desc = temp_ptr;
			}
			else {
				VIRTIO_DRI_WARNX(mmio_dev->vqdev,
					"Attempt to write QueueDescHigh register for invalid"
					" QueueSel."
					" QueueSel was %u, but the number of queues is %u.",
					mmio_dev->qsel, mmio_dev->vqdev->num_vqs);
			}
			break;

		// Queue's Available Ring 64 bit long physical address, low 32
		case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueAvailLow on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				// clear low bits
				temp_ptr = (void *)
				    ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail
				  & ((uint64_t)0xffffffff << 32));
				// write low bits
				temp_ptr = (void *) ((uint64_t)temp_ptr | *value);

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 2)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's available ring (%p) is"
						" misaligned. Address should be a multiple of 2.");

				// assign the new value to the queue avail
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail = temp_ptr;
			}
			else {
				VIRTIO_DRI_WARNX(mmio_dev->vqdev,
					"Attempt to write QueueAvailLow register for invalid"
					" QueueSel."
					" QueueSel was %u, but the number of queues is %u.",
					mmio_dev->qsel, mmio_dev->vqdev->num_vqs);
			}
			break;

		// Queue's Available Ring 64 bit long physical address, high 32
		case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueAvailHigh on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				// clear high bits
				temp_ptr = (void *)
				    ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail
				 &  ((uint64_t)0xffffffff));
				// write high bits
				temp_ptr = (void *) ((uint64_t)temp_ptr
				                  | ((uint64_t)(*value) << 32));

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 2)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's available ring (%p) is"
						" misaligned. Address should be a multiple of 2.");

				// assign the new value to the queue avail
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.avail = temp_ptr;
			}
			else {
				VIRTIO_DRI_WARNX(mmio_dev->vqdev,
					"Attempt to write QueueAvailHigh register for invalid"
					" QueueSel."
					" QueueSel was %u, but the number of queues is %u.",
					mmio_dev->qsel, mmio_dev->vqdev->num_vqs);
			}
			break;

		// Queue's Used Ring 64 bit long physical address, low 32
		case VIRTIO_MMIO_QUEUE_USED_LOW:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueUsedLow on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				// clear low bits
				temp_ptr = (void *)
				    ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used
				  & ((uint64_t)0xffffffff << 32));
				// write low bits
				temp_ptr = (void *) ((uint64_t)temp_ptr | *value);

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 4)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's used ring (%p) is"
						" misaligned. Address should be a multiple of 4.");

				// assign the new value to the queue used
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used = temp_ptr;
			}
			else {
				VIRTIO_DRI_WARNX(mmio_dev->vqdev,
					"Attempt to write QueueUsedLow register for invalid"
					" QueueSel."
					" QueueSel was %u, but the number of queues is %u.",
					mmio_dev->qsel, mmio_dev->vqdev->num_vqs);
			}
			break;

		// Queue's Used Ring 64 bit long physical address, high 32
		case VIRTIO_MMIO_QUEUE_USED_HIGH:
			if (mmio_dev->qsel < mmio_dev->vqdev->num_vqs) {
				if (mmio_dev->vqdev->vqs[mmio_dev->qsel].qready != 0)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Attempt to access QueueUsedHigh on queue %d,"
						" which has nonzero QueueReady.", mmio_dev->qsel);

				// clear high bits
				temp_ptr = (void *)
				    ((uint64_t)mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used
				  & ((uint64_t)0xffffffff));
				// write high bits
				temp_ptr = (void *) ((uint64_t)temp_ptr
				                  | ((uint64_t)(*value) << 32));

				// virtio-v1.0-cs04 s2.4 Virtqueues
				if ((uint64_t)temp_ptr % 4)
					VIRTIO_DRI_ERRX(mmio_dev->vqdev,
						"Physical address of guest's used ring (%p) is"
						" misaligned. Address should be a multiple of 4.");

				// assign the new value to the queue used
				mmio_dev->vqdev->vqs[mmio_dev->qsel].vring.used = temp_ptr;
			}
			else {
				VIRTIO_DRI_WARNX(mmio_dev->vqdev,
					"Attempt to write QueueUsedHigh register for invalid"
					" QueueSel."
					" QueueSel was %u, but the number of queues is %u.",
					mmio_dev->qsel, mmio_dev->vqdev->num_vqs);
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
			VIRTIO_DRI_ERRX(mmio_dev->vqdev,
				"Attempt to write read-only device register offset 0x%x.",
				offset);
			break;
		default:
			// Bad register offset
			VIRTIO_DRI_ERRX(mmio_dev->vqdev,
				"Attempt to write invalid device register offset 0x%x.",
				offset);
			break;
	}
}




