/** @file

  Copyright (c) 2022, Arm Limited. All rights reserved.

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <Base.h>
#include <libfdt.h>
#include <Library/ArmLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>

RETURN_STATUS
EFIAPI
CloudHvVirtMemInfoPeiLibConstructor (
  VOID
  )
{
  VOID           *DeviceTreeBase;
  INT32          Node, Prev;
  UINT64         NewBase, CurBase;
  UINT64         NewSize, CurSize;
  CONST CHAR8    *Type;
  INT32          Len;
  CONST UINT64   *RegProp;
  RETURN_STATUS  PcdStatus;

  NewBase = 0;
  NewSize = 0;

  DeviceTreeBase = (VOID *)(UINTN)PcdGet64 (PcdDeviceTreeInitialBaseAddress);
  ASSERT (DeviceTreeBase != NULL);

  //
  // Make sure we have a valid device tree blob
  //
  ASSERT (fdt_check_header (DeviceTreeBase) == 0);

  //
  // Look for the lowest memory node
  //
  for (Prev = 0; ; Prev = Node) {
    Node = fdt_next_node (DeviceTreeBase, Prev, NULL);
    if (Node < 0) {
      break;
    }

    //
    // Check for memory node
    //
    Type = fdt_getprop (DeviceTreeBase, Node, "device_type", &Len);
    if (Type && (AsciiStrnCmp (Type, "memory", Len) == 0)) {
      //
      // Get the 'reg' property of this node. For now, we will assume
      // two 8 byte quantities for base and size, respectively.
      //
      RegProp = fdt_getprop (DeviceTreeBase, Node, "reg", &Len);
      if ((RegProp != 0) && (Len == (2 * sizeof (UINT64)))) {
        CurBase = fdt64_to_cpu (ReadUnaligned64 (RegProp));
        CurSize = fdt64_to_cpu (ReadUnaligned64 (RegProp + 1));

        DEBUG ((
          DEBUG_INFO,
          "%a: System RAM @ 0x%lx - 0x%lx\n",
          __FUNCTION__,
          CurBase,
          CurBase + CurSize - 1
          ));

        if ((NewBase > CurBase) || (NewBase == 0)) {
          NewBase = CurBase;
          NewSize = CurSize;
        }
      } else {
        DEBUG ((
          DEBUG_ERROR,
          "%a: Failed to parse FDT memory node\n",
          __FUNCTION__
          ));
      }
    }
  }

  //
  // Make sure the start of DRAM matches our expectation
  //
  ASSERT (FixedPcdGet64 (PcdSystemMemoryBase) == NewBase);
  PcdStatus = PcdSet64S (PcdSystemMemorySize, NewSize);
  ASSERT_RETURN_ERROR (PcdStatus);

  ASSERT (
    (((UINT64)PcdGet64 (PcdFdBaseAddress) +
      (UINT64)PcdGet32 (PcdFdSize)) <= NewBase) ||
    ((UINT64)PcdGet64 (PcdFdBaseAddress) >= (NewBase + NewSize))
    );

  return RETURN_SUCCESS;
}

// Number of Virtual Memory Map Descriptors
#define MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS  6

//
// Core peripherals such as the UART, the GIC and the RTC are
// all mapped in the 'miscellaneous device I/O' region, which we just map
// in its entirety rather than device by device. Note that it does not
// cover any of the NOR flash banks or PCI resource windows.
//
#define MACH_VIRT_PERIPH_BASE  0x00400000
#define MACH_VIRT_PERIPH_SIZE  0x0FC00000

//
// The top of the 64M memory region under 4GB reserved for device
//
#define TOP_32BIT_DEVICE_BASE  0xFC000000
#define TOP_32BIT_DEVICE_SIZE  0x04000000
#define SECOND_PART_MEM_BASE  0x100000000

/**
  Return the Virtual Memory Map of your platform

  This Virtual Memory Map is used by MemoryInitPei Module to initialize the MMU
  on your platform.

  @param[out]   VirtualMemoryMap    Array of ARM_MEMORY_REGION_DESCRIPTOR
                                    describing a Physical-to-Virtual Memory
                                    mapping. This array must be ended by a
                                    zero-filled entry. The allocated memory
                                    will not be freed.

**/
VOID
ArmVirtGetMemoryMap (
  OUT ARM_MEMORY_REGION_DESCRIPTOR  **VirtualMemoryMap
  )
{
  ARM_MEMORY_REGION_DESCRIPTOR  *VirtualMemoryTable;
  INT64 RamBase = PcdGet64 (PcdSystemMemoryBase);
  INT64 RamSize = PcdGet64 (PcdSystemMemorySize);

  ASSERT (VirtualMemoryMap != NULL);

  VirtualMemoryTable = AllocatePool (
                         sizeof (ARM_MEMORY_REGION_DESCRIPTOR) *
                         MAX_VIRTUAL_MEMORY_MAP_DESCRIPTORS
                         );

  if (VirtualMemoryTable == NULL) {
    DEBUG ((DEBUG_ERROR, "%a: Error: Failed AllocatePool()\n", __FUNCTION__));
    return;
  }

  if (RamBase + RamSize > TOP_32BIT_DEVICE_BASE) {
    RamSize = TOP_32BIT_DEVICE_BASE - RamBase;
  }
    // System DRAM
  VirtualMemoryTable[0].PhysicalBase = RamBase;
  VirtualMemoryTable[0].VirtualBase  = RamBase;
  VirtualMemoryTable[0].Length       = RamSize;
  VirtualMemoryTable[0].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;

  DEBUG ((
    DEBUG_INFO,
    "%a: Dumping System DRAM Memory Map:\n"
    "\tPhysicalBase: 0x%lX\n"
    "\tVirtualBase: 0x%lX\n"
    "\tLength: 0x%lX\n",
    __FUNCTION__,
    VirtualMemoryTable[0].PhysicalBase,
    VirtualMemoryTable[0].VirtualBase,
    VirtualMemoryTable[0].Length
    ));

  // Memory mapped peripherals (UART, RTC, GIC, virtio-mmio, etc)
  VirtualMemoryTable[1].PhysicalBase = MACH_VIRT_PERIPH_BASE;
  VirtualMemoryTable[1].VirtualBase  = MACH_VIRT_PERIPH_BASE;
  VirtualMemoryTable[1].Length       = MACH_VIRT_PERIPH_SIZE;
  VirtualMemoryTable[1].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // Map the FV region as normal executable memory
  VirtualMemoryTable[2].PhysicalBase = PcdGet64 (PcdFvBaseAddress);
  VirtualMemoryTable[2].VirtualBase  = VirtualMemoryTable[2].PhysicalBase;
  VirtualMemoryTable[2].Length       = FixedPcdGet32 (PcdFvSize);
  VirtualMemoryTable[2].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_WRITE_BACK;

  // Memory mapped for 32bit device (like TPM)
  VirtualMemoryTable[3].PhysicalBase = TOP_32BIT_DEVICE_BASE;
  VirtualMemoryTable[3].VirtualBase  = TOP_32BIT_DEVICE_BASE;
  VirtualMemoryTable[3].Length       = TOP_32BIT_DEVICE_SIZE;
  VirtualMemoryTable[3].Attributes   = ARM_MEMORY_REGION_ATTRIBUTE_DEVICE;

  // End of Table
  ZeroMem (&VirtualMemoryTable[4], sizeof (ARM_MEMORY_REGION_DESCRIPTOR));

  *VirtualMemoryMap = VirtualMemoryTable;
}
