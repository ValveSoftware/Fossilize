#include "cli/device.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

using namespace Fossilize;

// Synthetic GPU catalogue. Real systems can have multiple iGPUs + dGPUs,
// so the selection loop must tolerate mixed vendor IDs and not assume
// the matching GPU is at index 0.
struct FakeGpu
{
	uint32_t vendor_id;
	uint32_t device_id;
	const char *name;
};

static const FakeGpu kCatalogue[] = {
	{ 0x10de, 0x2488, "NVIDIA RTX 3070" },
	{ 0x1002, 0x73bf, "AMD Radeon RX 6700 XT" },
	{ 0x8086, 0x4907, "Intel UHD 770" },
};

static int select_first_match(VulkanDevice::Options opts)
{
	for (size_t i = 0; i < sizeof(kCatalogue) / sizeof(kCatalogue[0]); i++)
	{
		if (VulkanDevice::pci_filter_matches(opts,
		                                     kCatalogue[i].vendor_id,
		                                     kCatalogue[i].device_id))
			return int(i);
	}
	return -1;
}

int main()
{
	printf("[TEST] Testing GPU PCI filter selection logic...\n");

	// Case 1: vendor-only filter matches the first NVIDIA dGPU.
	{
		VulkanDevice::Options opts = {};
		opts.device_pci_vendor = 0x10de;
		int idx = select_first_match(opts);
		printf("  vendor=0x10de -> index %d (%s)\n", idx,
		       idx >= 0 ? kCatalogue[idx].name : "(no match)");
		assert(idx == 0);
	}

	// Case 2: vendor+device matches NVIDIA RTX 3070 exactly.
	{
		VulkanDevice::Options opts = {};
		opts.device_pci_vendor = 0x10de;
		opts.device_pci_device = 0x2488;
		int idx = select_first_match(opts);
		printf("  vendor=0x10de device=0x2488 -> index %d (%s)\n", idx,
		       idx >= 0 ? kCatalogue[idx].name : "(no match)");
		assert(idx == 0);
	}

	// Case 3: vendor matches but specific device does not.
	{
		VulkanDevice::Options opts = {};
		opts.device_pci_vendor = 0x10de;
		opts.device_pci_device = 0x9999;
		int idx = select_first_match(opts);
		printf("  vendor=0x10de device=0x9999 -> index %d\n", idx);
		assert(idx == -1);
	}

	// Case 4: no filter (vendor == 0) means "do not filter".
	{
		VulkanDevice::Options opts = {};
		int idx = select_first_match(opts);
		printf("  no filter -> index %d\n", idx);
		assert(idx == -1);
	}

	// Case 5: vendor-only filter selects AMD in the middle slot.
	{
		VulkanDevice::Options opts = {};
		opts.device_pci_vendor = 0x1002;
		int idx = select_first_match(opts);
		printf("  vendor=0x1002 -> index %d (%s)\n", idx,
		       idx >= 0 ? kCatalogue[idx].name : "(no match)");
		assert(idx == 1);
	}

	// Case 6: vendor+device matches the last entry (Intel iGPU).
	{
		VulkanDevice::Options opts = {};
		opts.device_pci_vendor = 0x8086;
		opts.device_pci_device = 0x4907;
		int idx = select_first_match(opts);
		printf("  vendor=0x8086 device=0x4907 -> index %d (%s)\n", idx,
		       idx >= 0 ? kCatalogue[idx].name : "(no match)");
		assert(idx == 2);
	}

	// Case 7: AMD vendor but with NVIDIA device id -- must NOT match.
	{
		VulkanDevice::Options opts = {};
		opts.device_pci_vendor = 0x1002;
		opts.device_pci_device = 0x2488;
		int idx = select_first_match(opts);
		printf("  vendor=0x1002 device=0x2488 -> index %d\n", idx);
		assert(idx == -1);
	}

	printf("[TEST] SUCCESS: GPU PCI filter selection logic verified.\n");
	return 0;
}