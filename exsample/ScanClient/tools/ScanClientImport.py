#@category Bedrock.ScanClient
# Import readable mapped section extents from a ScanClient output bundle.

import json
import os
from java.io import FileInputStream


def parse_hex(value):
    return int(value, 16) if isinstance(value, basestring) else int(value)


def run():
    bundle = askDirectory("Select ScanClient output folder", "Import")
    layout_path = os.path.join(bundle.getAbsolutePath(), "module_layout.json")
    with open(layout_path, "r") as source:
        layout = json.load(source)

    memory = currentProgram.getMemory()
    if memory.getBlocks():
        raise Exception("Use a new, empty x64 program. This importer does not overwrite existing memory blocks.")

    module = layout["module"]
    loaded_base = parse_hex(module["loaded_base"])
    transaction = currentProgram.startTransaction("Import ScanClient mapped image")
    created = 0
    skipped = 0
    try:
        currentProgram.setImageBase(toAddr(loaded_base), True)
        for section in layout.get("sections", []):
            characteristics = parse_hex(section.get("characteristics", "0x0"))
            readable = bool(characteristics & 0x40000000)
            writable = bool(characteristics & 0x80000000)
            executable = bool(characteristics & 0x20000000)
            for extent in section.get("readable_extents", []):
                if not extent.get("dumped"):
                    skipped += 1
                    continue
                relative_path = extent.get("dump_file", "")
                dump_path = os.path.join(bundle.getAbsolutePath(), *relative_path.replace("/", os.sep).split(os.sep))
                if not os.path.isfile(dump_path):
                    skipped += 1
                    println("Missing extent dump: " + dump_path)
                    continue
                address = toAddr(parse_hex(extent["loaded_va"]))
                size = os.path.getsize(dump_path)
                if size == 0 or size != int(extent["size"]):
                    skipped += 1
                    println("Invalid extent size: " + dump_path)
                    continue
                block_name = "sc_%s_%s" % (section["name"], extent["rva"].replace("0x", ""))
                stream = FileInputStream(dump_path)
                try:
                    block = createMemoryBlock(block_name, address, stream, size, False)
                finally:
                    stream.close()
                block.setRead(readable)
                block.setWrite(writable)
                block.setExecute(executable)
                block.setComment("ScanClient readable mapped extent; RVA " + extent["rva"])
                createLabel(address, block_name, True)
                created += 1
        println("Imported %d readable extents; skipped %d." % (created, skipped))
        println("Loaded base: 0x%X. Review zero-filled gaps and module_layout.json before interpreting pointers." % loaded_base)
    except:
        currentProgram.endTransaction(transaction, False)
        raise
    currentProgram.endTransaction(transaction, True)


run()
