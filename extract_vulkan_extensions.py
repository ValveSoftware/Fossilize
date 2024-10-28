#!/usr/bin/env python3

import argparse
from collections import namedtuple
import xml.etree.ElementTree as ET
import json
import sys

StructType = namedtuple('StructType', ['extends', 'member_types'])
EnumType = namedtuple('EnumType', ['extensions'])

def traverse_active_struct_type(active_types, t, mapping):
    if t not in mapping:
        return
    if t in active_types:
        return
    active_types.add(t)
    for m in mapping[t].member_types:
        traverse_active_struct_type(active_types, m, mapping)

def traverse_active_struct_types(base_types, mapping):
    active_types = set()
    for t in base_types:
        traverse_active_struct_type(active_types, t, mapping)
    return active_types

def struct_extends_any(active_types, t, mappings):
    if t not in mappings:
        return False
    for e in mappings[t].extends:
        if e in active_types or struct_extends_any(active_types, e, mappings):
            return True
    return False

def find_extending_structs(active_types, mappings):
    extending_structs = set()
    for m in mappings.keys():
        if struct_extends_any(active_types, m, mappings):
            for memb in traverse_active_struct_types([m], mappings):
                extending_structs.add(memb)
    return extending_structs

def find_active_enum_types(active_types, extending_types, mappings, enum_types, bitmask_reqs):
    active_enum_types = set()

    for t in active_types:
        for m in mappings[t].member_types:
            if m in bitmask_reqs:
                active_enum_types.add(bitmask_reqs[m])
            elif m in enum_types:
                active_enum_types.add(m)

    for t in extending_types:
        for m in mappings[t].member_types:
            if m in bitmask_reqs:
                active_enum_types.add(bitmask_reqs[m])
            elif m in enum_types:
                active_enum_types.add(m)

    return active_enum_types

def main():
    parser = argparse.ArgumentParser(description = 'Script for extracting what needs to be parsed.')
    parser.add_argument('--xml', help = 'Path to Vulkan registry XML.')
    parser.add_argument('--completed-work', help = 'Path to JSON containing completed features.')
    args = parser.parse_args()
    if not args.xml:
        sys.stderr.write('Need XML.\n')
        sys.exit(1)

    if not args.completed_work:
        sys.stderr.write('Need completed work.\n')
        sys.exit(1)

    tree = ET.parse(args.xml)
    root = tree.getroot()

    top_level_types_pipeline = set([
        'VkGraphicsPipelineCreateInfo',
        'VkComputePipelineCreateInfo',
        'VkRayTracingPipelineCreateInfoKHR',
        'VkDeviceCreateInfo'])

    top_level_types = set([
        'VkGraphicsPipelineCreateInfo',
        'VkComputePipelineCreateInfo',
        'VkRayTracingPipelineCreateInfoKHR',
        'VkRenderPassCreateInfo',
        'VkRenderPassCreateInfo2',
        'VkDescriptorSetLayoutCreateInfo',
        'VkSamplerCreateInfo',
        'VkShaderModuleCreateInfo',
        'VkPipelineLayoutCreateInfo'])

    struct_types = {}
    enum_types = {}
    bitmask_requirements = {}
    type_aliases = {}

    for t in root.find('types').iter('type'):
        if 'alias' in t.attrib:
            type_aliases[t.attrib['name']] = t.attrib['alias']

    shallow_copy_pnext_size_lut = ''

    for t in root.find('types').iter('type'):
        if 'category' not in t.attrib:
            continue
        category = t.attrib['category']
        if category == 'struct':
            name = t.attrib['name']
            member_types = []
            extended_types = set()
            extends = set()
            if 'structextends' in t.attrib:
                extends = t.attrib['structextends'].split(',')
                extends = set([type_aliases.get(x, x) for x in extends])

                for ext in extends:
                    if ext in top_level_types_pipeline:
                        stype = t.find('member').attrib['values']
                        shallow_copy_pnext_size_lut += '{ ' + stype + ', sizeof(' + name + ') },\n'
                        break

            for elem in t.iter('member'):
                member_type = elem.find('type').text
                member_type = type_aliases.get(member_type, member_type)
                member_types.append(member_type)
            struct_types[name] = StructType(extends, member_types)
        elif category == 'enum':
            name = t.attrib['name']
            name = type_aliases.get(name, name)
            if name != 'VkStructureType' and name != 'VkFormat' and name != 'VkObjectType':
                enum_types[name] = EnumType(set()) 
        elif category == 'bitmask' and 'requires' in t.attrib:
            req = t.attrib['requires']
            bitmask_requirements[t.find('name').text] = req
        elif category == 'bitmask' and 'bitvalues' in t.attrib:
            req = t.attrib['bitvalues']
            bitmask_requirements[t.find('name').text] = req

    active_types = traverse_active_struct_types(top_level_types, struct_types)
    extending_types = find_extending_structs(active_types, struct_types)

    with open(args.completed_work, 'r') as file:
        completed = json.loads(file.read())

    print('\n\n=== pNext LUT ===')
    print(shallow_copy_pnext_size_lut)

    print('\n\n=== Base Types ===')
    for t in active_types:
        print(t)
    print('\n\n=== Extending Types ===')
    for t in extending_types:
        print(t)

    print('\n\n=== Active Enums ===')
    active_enums = find_active_enum_types(active_types, extending_types, struct_types, enum_types, bitmask_requirements)
    for e in active_enums:
        print(e)

    completed_extensions = set(completed['extensions']) if 'extensions' in completed else set()
    ignored_extensions = set(completed['fully-ignored']) if 'fully-ignored' in completed else set()
    cherry_picked = set(completed['cherry-pick']) if 'cherry-pick' in completed else set()
    completed_spirv_capabilities = set(completed['spirvcapabilities']) if 'spirvcapabilities' in completed else set()

    print('\n\n=== Completed extensions ===')
    for ext in root.find('extensions').iter('extension'):
        if 'vulkan' in ext.attrib['supported'].split(','):
            ext_name = ext.attrib['name']
            if ext_name in completed_extensions:
                print(ext_name, '|| Completed')
            elif ext_name in ignored_extensions:
                print(ext_name, '|| Ignored')

    print('\n\n=== Additions ===')
    for ext in root.find('extensions').iter('extension'):
        if 'vulkan' in ext.attrib['supported'].split(','):
            ext_name = ext.attrib['name']
            if ext_name in completed_extensions or ext_name in ignored_extensions:
                continue
            new_structs = []
            new_enums = []
            extended_enums = []
            feature_structs = []
            property_structs = []

            for reqs in ext.iter('require'):
                for t in reqs.iter('type'):
                    type_name = t.attrib['name']
                    type_name = type_aliases.get(type_name, type_name)
                    if type_name in active_types or type_name in extending_types:
                        if type_name not in cherry_picked:
                            new_structs.append(type_name)
                    elif type_name in active_enums:
                        if type_name not in cherry_picked:
                            new_enums.append(type_name)

                    if type_name in struct_types:
                        extends = struct_types[type_name].extends
                        if 'VkPhysicalDeviceFeatures2' in extends:
                            feature_structs.append(type_name)
                        if 'VkPhysicalDeviceProperties2' in extends:
                            property_structs.append(type_name)

                for e in reqs.iter('enum'):
                    enum_name = e.attrib['name']
                    if enum_name not in cherry_picked:
                        enum_name = type_aliases.get(enum_name, enum_name)
                        if 'extends' in e.attrib and e.attrib['extends'] in active_enums:
                            extended_enums.append((e.attrib['extends'], enum_name))

            if len(new_structs) + len(new_enums) + len(extended_enums) > 0:
                print('===', ext_name, '===')
                for new_struct in new_structs:
                    print('\t' + new_struct, 'extends', ', '.join(struct_types[new_struct].extends))
                for new_enum in new_enums:
                    print('\t' + new_enum)
                for extended_enum in extended_enums:
                    print('\t' + extended_enum[0], 'adds', extended_enum[1])
                print('   Feature:', ', '.join(feature_structs))
                print('  Property:', ', '.join(property_structs))
                print('')

    print('\n\n=== SPIR-V extensions table ===')
    for ext in root.find('spirvextensions').iter('spirvextension'):
        core_version = None
        vkext = None
        for en in ext.iter('enable'):
            if 'version' in en.attrib:
                core_version = en.attrib['version']
            if 'extension' in en.attrib:
                if vkext is not None:
                    raise('Cannot have more than one extension per SPIR-V extension.')
                vkext = en.attrib['extension']
        print('{', '"{0}", "{1}", {2}'.format(ext.attrib['name'], vkext, 'VK_API' + core_version[2:] if core_version else 0), '},')

    print('\n\n=== Completed SPIR-V capabilities ===')
    for ext in root.find('spirvcapabilities').iter('spirvcapability'):
        if ext.attrib['name'] in completed_spirv_capabilities:
            print(ext.attrib['name'], '|| Complete')

    print('\n\n=== Missing SPIR-V capabilities ===')
    for ext in root.find('spirvcapabilities').iter('spirvcapability'):
        if ext.attrib['name'] in completed_spirv_capabilities:
            continue

        print(ext.attrib['name'])
        for en in ext.iter('enable'):
            if 'version' in en.attrib:
                print('\tCore version:', en.attrib['version'])
            if 'feature' in en.attrib:
                print('\tVulkan feature:', en.attrib['struct'] + '::' + en.attrib['feature'])
                if 'requires' in en.attrib:
                    print('\t          from:', en.attrib['requires'].split(','))
            if 'property' in en.attrib:
                print('\tVulkan property:', en.attrib['property'] + '::' + en.attrib['member'], 'sets', en.attrib['value'])
                if 'requires' in en.attrib:
                    print('\t          from:', en.attrib['requires'].split(','))

            if 'extension' in en.attrib:
                print('\tVulkan extension:', en.attrib['extension'])


if __name__ == '__main__':
    main()

