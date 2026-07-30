#!/usr/bin/env python3

import sys
import argparse
import json

class Hasher:
    def __init__(self):
        self._h = 0xcbf29ce484222325
        pass

    def _iterate_value(self, value : int):
        self._h = (self._h * 0x100000001b3) ^ value
        self._h = self._h & 0xffffffffffffffff

    def u32(self, value : int):
        self._iterate_value(value & 0xffffffff)

    def boolean(self, value : bool):
        self._iterate_value(int(value))

    def string(self, value : str):
        self._iterate_value(0xff)
        for c in value:
            self._iterate_value(ord(c) & 0xff)

    def get(self):
        return self._h

def version_major(version : int) -> int:
    return (version >> 22) & 0x3ff
def version_minor(version : int) -> int:
    return (version >> 12) & 0x3ff
def version_patch(version : int) -> int:
    return (version >> 0) & 0xfff

def make_version(major : int, minor : int) -> int:
    return (major << 22) | (minor << 12)

# VendorIDs we expect to see.
VendorIDs = [
    0x1002, # AMD
    0x8086, # Intel
    0x10de, # NV
    0x5143, # QCOM
]

def dep_to_stype(dep : str) -> int:
    match dep:
        case 'MutableDescriptorType': return 1000351000
        case 'BufferDeviceAddress': return 1000257000
        case 'DynamicRendering': return 1000044003
        case 'BindlessUBO': return 1000161001
        case 'DescriptorHeap': return 1000135009
        case 'DescriptorBuffer': return 1000316002
        case _: return 0

def reverse_engine_version(filter : dict, hash : int, engine_name : str, engine_version) -> Optional[dict]:
    num_variant_deps = len(filter['bucketVariantDependencies']) if 'bucketVariantDependencies' in filter else 0
    num_variant_feature_deps = len(filter['bucketVariantFeatureDependencies']) if 'bucketVariantFeatureDependencies' in filter else 0
    num_variants = 1 << (num_variant_deps + num_variant_feature_deps)

    if num_variant_deps != 0:
        # VendorID consumes 2 bits instead of 1.
        if 'VendorID' in filter['bucketVariantDependencies']:
            num_variants <<= 1

        # Implied by outer arguments.
        if 'EngineVersionMajor' in filter['bucketVariantDependencies']:
            num_variants >>= 1
        if 'EngineVersionMinor' in filter['bucketVariantDependencies']:
            num_variants >>= 1
        if 'EngineName' in filter['bucketVariantDependencies']:
            num_variants >>= 1

    for variant in range(num_variants):
        h = Hasher()

        # Per app hashing
        h.u32(0)
        # Per engine hashing
        h.u32(0)

        candidate = {}
        bit_index = 0

        # Hash in the same way that fossilize application filter would do.
        if num_variant_deps != 0:
            for dep in filter['bucketVariantDependencies']:
                if dep == 'EngineVersionMajor':
                    candidate[dep] = engine_version[0]
                    h.u32(engine_version[0])
                elif dep == 'EngineVersionMinor':
                    candidate[dep] = engine_version[1]
                    h.u32(engine_version[1])
                elif dep == 'EngineName':
                    candidate[dep] = engine_name
                    h.string(engine_name)
                elif 'Engine' in dep:
                    return None
                elif 'Application' in dep:
                    return None
                elif dep == 'VendorID':
                    vendor_id = VendorIDs[(variant >> bit_index) & 3]
                    candidate[dep] = vendor_id
                    h.u32(vendor_id)
                    bit_index += 2
                else:
                    state_bit = (variant >> bit_index) & 1
                    candidate[dep] = state_bit
                    h.u32(state_bit)

                    # Subfeature hashing
                    if dep == 'FragmentShadingRate':
                        h.u32(1) # attachmentFragmentShadingRate
                        h.u32(1) # pipelineFragmentShadingRate
                        h.u32(1) # primitiveFragmentShadingRate
                    elif dep == 'DescriptorBuffer':
                        h.u32(1) # descriptorBuffer
                        h.u32(1 if engine_name == 'vkd3d' else 0) # descriptorBufferPushDescriptor

                    bit_index += 1

        if num_variant_feature_deps != 0:
            for dep in filter['bucketVariantFeatureDependencies']:
                state_bit = (variant >> bit_index) & 1
                candidate[dep] = state_bit
                if state_bit:
                    h.u32(dep_to_stype(dep))
                    if dep == 'BindlessUBO':
                        h.u32(10)

                    # Subfeature hashing
                    if dep == 'FragmentShadingRate':
                        h.u32(1) # attachmentFragmentShadingRate
                        h.u32(1) # pipelineFragmentShadingRate
                        h.u32(1) # primitiveFragmentShadingRate
                    elif dep == 'DescriptorBuffer':
                        h.u32(1) # descriptorBuffer
                        h.u32(1 if engine_name == 'vkd3d' else 0) # descriptorBufferPushDescriptor
                    else:
                        h.u32(1) # generic

                bit_index += 1

        if 'engineVersionDeltas' in filter:
            for dep in filter['engineVersionDeltas']:
                if dep >= make_version(engine_version[0], engine_version[1]):
                    h.u32(dep ^ 0xcafe)

        #print(f'Engine {engine_name}, version {engine_version[0]}.{engine_version[1]}, candidate {candidate} yields hash {hex(h.get())}')

        if h.get() == hash:
            return { 'bucket' : candidate }
    return None

def reverse_engine_version_range(filter : dict, hash : int, engine_name : str,
                                 minimum_version, maximum_version, maximum_minor_version) -> Optional[dict]:
    # Iterate over all possible versions of the engine in the hope we'll strike gold.
    maximum_major_version = maximum_version[0]
    while minimum_version[0] <= maximum_major_version:
        while minimum_version[1] <= maximum_minor_version:
            if minimum_version[0] == maximum_version[0] and minimum_version[1] > maximum_version[1]:
                return None
            res = reverse_engine_version(filter, hash, engine_name, minimum_version)
            if res:
                return res
            minimum_version = (minimum_version[0], minimum_version[1] + 1)
        minimum_version = (minimum_version[0] + 1, 0)

    return None

def reverse_bucket_hash_vkd3d(filter : dict, hash : int) -> Optional[dict]:
    # The Steam filter uses these. That's what we care about.
    if 'minimumEngineVersion' not in filter:
        return None

    minimum_version = (version_major(filter['minimumEngineVersion']), version_minor(filter['minimumEngineVersion']))
    return reverse_engine_version_range(filter, hash, 'vkd3d', minimum_version, (3, 1), 14)

def reverse_bucket_hash_dxvk(filter : dict, hash : int) -> Optional[dict]:
    pass

# filter is a loaded dict of the fossilize_engine_filters.json.
def reverse_bucket_hash(filter : dict, hash : int) -> Optional[dict]:
    if 'asset' not in filter:
        return None
    if filter['asset'] != 'FossilizeApplicationInfoFilter':
        return None
    if filter['version'] != 1:
        return None
    if 'engineFilters' not in filter:
        return None

    engine_filters = filter['engineFilters']
    for engine_filter in engine_filters:
        if engine_filter == 'DXVK':
            result = reverse_bucket_hash_dxvk(engine_filters[engine_filter], hash)
            if result:
                return result
        elif engine_filter == 'vkd3d':
            result = reverse_bucket_hash_vkd3d(engine_filters[engine_filter], hash)
            if result:
                return result
        else:
            # We only know how to reverse these two. We don't use buckets for anything else right now.
            pass

def main():
    parser = argparse.ArgumentParser(description = 'Tool for reversing a bucket hash back to its bucket.json')
    parser.add_argument('--filter', type = str, help = 'Path to fossilize_engine_filters.json')
    parser.add_argument('--hash', type = str, help = 'Bucket hash')
    args = parser.parse_args()

    if not args.filter:
        raise ArgumentError('Must define --filter')
    if not args.hash:
        raise ArgumentError('Must define --hash')

    hash = int(args.hash, 16)
    with open(args.filter, 'r') as f:
        filter_dict = json.loads(f.read())

    bucket = reverse_bucket_hash(filter_dict, hash)
    if bucket:
        print(json.dumps(bucket, indent = 4))
    else:
        print('Could not reverse the bucket')
        sys.exit(1)

if __name__ == '__main__':
    main()