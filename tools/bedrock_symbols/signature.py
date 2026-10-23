from __future__ import annotations

import capstone
from capstone.x86 import X86_REG_RIP, X86_REG_RSP

def parse_pattern(pattern: str) -> tuple[bytes, bytes]:
    toks = pattern.strip().split()
    if not toks:
        raise ValueError("empty signature")
    if len(toks) > 512:
        raise ValueError("signature too long")
    data, mask = bytearray(), bytearray()
    for token in toks:
        if token in ("?", "??"):
            data.append(0)
            mask.append(0)
        elif len(token) == 2:
            try:
                data.append(int(token, 16))
            except ValueError as exc:
                raise ValueError(f"bad signature token: {token}") from exc
            mask.append(1)
        else:
            raise ValueError(f"bad signature token: {token}")
    return bytes(data), bytes(mask)


def pattern_string(code: bytes, mask: bytes) -> str:
    return " ".join(f"{c:02X}" if m else "??" for c, m in zip(code, mask))


def find_matches(haystack: bytes, pattern: str,
                 max_matches: int = 256) -> list[int] | None:
    """Return full masked-pattern hits, or None if the result cap is exceeded."""
    if max_matches < 1:
        raise ValueError("max_matches must be positive")
    code, mask = parse_pattern(pattern)
    # Longest adjacent non-wildcard run serves as the search anchor.
    best_start, best_length, i = 0, 0, 0
    while i < len(mask):
        if not mask[i]:
            i += 1
            continue
        j = i
        while j < len(mask) and mask[j]:
            j += 1
        if j - i > best_length:
            best_start, best_length = i, j - i
        i = j
    if best_length < 4:
        return []
    anchor = code[best_start:best_start + best_length]
    hits: list[int] = []
    pos = 0
    while True:
        anchor_pos = haystack.find(anchor, pos)
        if anchor_pos < 0:
            break
        pos = anchor_pos + 1
        start = anchor_pos - best_start
        if start < 0 or start + len(code) > len(haystack):
            continue
        if all(not fixed or haystack[start + ix] == byte
               for ix, (byte, fixed) in enumerate(zip(code, mask))):
            hits.append(start)
            if len(hits) > max_matches:
                return None
    return hits


def unique_match(haystack: bytes, pattern: str) -> int | None:
    """Find only if there is exactly one full masked hit."""
    hits = find_matches(haystack, pattern, max_matches=1)
    return hits[0] if hits else None


def generate_unique_signature(code: bytes, binary_text: bytes, expected_offset: int,
                              max_length: int = 128) -> tuple[str, int] | None:
    if not code or expected_offset < 0 or binary_text[expected_offset:expected_offset + len(code)] != code:
        raise ValueError("input code doesn't match the expected offset")
    disasm = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    disasm.detail = True
    taken = 0
    mask = bytearray()
    for inst in disasm.disasm(code, 0):
        if taken + inst.size > max_length:
            break
        insn_mask = bytearray([1] * inst.size)
        # PC-relative call/jump offsets change across builds.
        if inst.imm_size and (inst.group(capstone.CS_GRP_CALL) or inst.group(capstone.CS_GRP_JUMP)):
            for n in range(inst.imm_offset, inst.imm_offset + inst.imm_size):
                insn_mask[n] = 0
        # Stack-frame allocation sizes often vary between optimizer/compiler builds.
        if inst.mnemonic in ("sub", "add") and len(inst.operands) >= 2:
            dst, src = inst.operands[0], inst.operands[1]
            if (dst.type == capstone.x86.X86_OP_REG and
                    inst.reg_name(dst.reg) == "rsp" and
                    src.type == capstone.x86.X86_OP_IMM and inst.imm_size):
                for n in range(inst.imm_offset, inst.imm_offset + inst.imm_size):
                    insn_mask[n] = 0
        if inst.mnemonic == "lea" and len(inst.operands) >= 2 and inst.disp_size:
            dst, src = inst.operands[0], inst.operands[1]
            if (dst.type == capstone.x86.X86_OP_REG and
                    inst.reg_name(dst.reg) == "rbp" and
                    src.type == capstone.x86.X86_OP_MEM and
                    inst.reg_name(src.mem.base) == "rsp"):
                for n in range(inst.disp_offset, inst.disp_offset + inst.disp_size):
                    insn_mask[n] = 0
        # RSP-relative offsets identify arguments and locals whose positions
        # move when the compiler changes the frame size or saved registers.
        stack_mem = any(op.type == capstone.x86.X86_OP_MEM and
                        op.mem.base == X86_REG_RSP
                        for op in inst.operands)
        if stack_mem and inst.disp_size:
            for n in range(inst.disp_offset, inst.disp_offset + inst.disp_size):
                insn_mask[n] = 0
        # RIP-relative memory references and relocatable displacements.
        rip_mem = any(op.type == capstone.x86.X86_OP_MEM and op.mem.base == X86_REG_RIP
                      for op in inst.operands)
        if rip_mem and inst.disp_size:
            for n in range(inst.disp_offset, inst.disp_offset + inst.disp_size):
                insn_mask[n] = 0
        mask.extend(insn_mask)
        taken += inst.size
        # Ensure useful fixed-byte entropy and at least 4 instructions where possible.
        if taken < 16:
            continue
        candidate = pattern_string(code[:taken], mask)
        match = unique_match(binary_text, candidate)
        if match == expected_offset:
            return candidate, taken
    return None


def generate_signature_anchors(code: bytes, binary_text: bytes, expected_offset: int,
                               max_anchors: int = 6) -> list[dict[str, object]]:
    """Create exact-build-unique entry/body anchors for offline version porting.

    Body anchors are searched inside the destination build's .pdata function
    ranges, so compiler-added prologues do not have to remain byte-identical.
    This is a candidate locator, never ABI or symbol verification.
    """
    if not code or expected_offset < 0 or binary_text[expected_offset:expected_offset+len(code)] != code:
        raise ValueError("input code doesn't match the expected offset")
    disasm = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    disasm.detail = True
    instructions = list(disasm.disasm(code, 0))
    if not instructions:
        return []

    # Probe the entry and evenly spaced body positions. Windows use complete
    # instructions and are kept short; function identity comes from .pdata.
    starts = {0}
    for fraction in (0.20, 0.40, 0.60, 0.80):
        target = int(len(code) * fraction)
        inst = next((item for item in instructions if item.address >= target), None)
        if inst is not None:
            starts.add(int(inst.address))
    candidates: list[dict[str, object]] = []
    occupied: list[tuple[int, int]] = []
    for start in sorted(starts):
        begin = next((i for i, item in enumerate(instructions) if item.address == start), None)
        if begin is None:
            continue
        window = bytearray()
        mask = bytearray()
        for inst in instructions[begin:]:
            if len(window) + inst.size > 48:
                break
            inst_mask = bytearray([1] * inst.size)
            if inst.imm_size and (inst.group(capstone.CS_GRP_CALL) or inst.group(capstone.CS_GRP_JUMP)):
                for n in range(inst.imm_offset, inst.imm_offset + inst.imm_size):
                    inst_mask[n] = 0
            # Stack slots, RIP-relative globals, and object member offsets are
            # layout-dependent; preserve opcodes/registers and mask displacements.
            if inst.disp_size:
                has_memory_operand = any(op.type == capstone.x86.X86_OP_MEM
                                         for op in inst.operands)
                if has_memory_operand:
                    for n in range(inst.disp_offset, inst.disp_offset + inst.disp_size):
                        inst_mask[n] = 0
            if inst.mnemonic in ("sub", "add") and len(inst.operands) >= 2:
                dst, src = inst.operands[0], inst.operands[1]
                if (dst.type == capstone.x86.X86_OP_REG and inst.reg_name(dst.reg) == "rsp"
                        and src.type == capstone.x86.X86_OP_IMM and inst.imm_size):
                    for n in range(inst.imm_offset, inst.imm_offset + inst.imm_size):
                        inst_mask[n] = 0
            window.extend(inst.bytes)
            mask.extend(inst_mask)
            if len(window) >= 32:
                break
        if len(window) < 20 or sum(mask) < 14 or sum(mask) * 2 < len(mask):
            continue
        end = start + len(window)
        if any(start < old_end and old_start < end for old_start, old_end in occupied):
            continue
        pattern = pattern_string(bytes(window), bytes(mask))
        hit = unique_match(binary_text, pattern)
        if hit != expected_offset + start:
            continue
        occupied.append((start, end))
        candidates.append({
            "offset": start,
            "pattern": pattern,
            "bytes": len(window),
            "fixed_bytes": sum(mask),
            "kind": "entry" if start == 0 else "body",
            "uniqueness": "unique-in-source-text",
        })
        if len(candidates) >= max_anchors:
            break
    return candidates
