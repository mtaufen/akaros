#pragma once

#include <ros/arch/membar.h>
#include <ros/arch/mmu.h>
#include <ros/virtio_ring.h> // TODO: Is the best virtio_ring even still in ros/? there is one in vmm/include/vmm now...
#include <ros/common.h>

#include <stddef.h>
#include <stdbool.h>
