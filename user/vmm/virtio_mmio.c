// Mike's implementation of virtio mmio device

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



void virtio_mmio_rd_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa)
{

}


void virtio_mmio_wr_reg(struct virtio_mmio_dev *mmio_dev, uint64_t gpa, uint32_t *value)
{

}


void virtio_mmio_set_vring_irq(struct virtio_mmio_dev *mmio_dev)
{
	mmio_dev->int_status |= VIRTIO_MMIO_INT_VRING;
}

