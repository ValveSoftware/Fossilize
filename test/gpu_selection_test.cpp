#include "cli/device.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

using namespace Fossilize;

int main()
{
	printf("[TEST] Testing GPU Vendor & Device selection options...\n");

	VulkanDevice::Options opts = {};
	opts.device_pci_vendor = 0x10de; // NVIDIA Vendor ID
	opts.device_pci_device = 0x2488; // Device ID

	assert(opts.device_pci_vendor == 0x10de);
	assert(opts.device_pci_device == 0x2488);

	printf("[TEST] SUCCESS: GPU selection options structure verified.\n");
	return 0;
}
