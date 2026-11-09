#!/usr/bin/env python3
"""Build the original LotusGB GBA audio check ROM (MIT licensed)."""

from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parent
ROM_ENTRY = 0xC0
LITERAL_POOL = 0x100

WRITES = (
    (0x04000084, 0x0080),  # Enable PSG and Direct Sound master output.
    (0x04000080, 0x7777),  # Full PSG volume, route all PSG channels L/R.
    (0x04000088, 0x0200),  # Default GBA output bias and resolution.
    (0x04000062, 0xF080),  # Square 1: 50% duty, constant maximum envelope.
    (0x04000064, 0x86D6),  # Trigger square 1 at approximately 440 Hz.
)

DIRECT_SOUND_WRITES = (
    (0x04000084, 0x0080, 16),  # Enable PSG and Direct Sound master output.
    (0x04000082, 0x2100, 16),  # FIFO A right, FIFO B left.
    (0x04000088, 0x0200, 16),  # Default GBA output bias and resolution.
    (0x040000BC, 0x08001000, 32),  # DMA1 source buffer in the ROM.
    (0x040000C0, 0x040000A0, 32),  # DMA1 FIFO A destination.
    (0x040000C4, 1, 16),
    (0x040000C6, 0xB640, 16),  # Enable, FIFO timing, repeat, 32-bit.
    (0x040000C8, 0x08009000, 32),  # DMA2 source buffer in the ROM.
    (0x040000CC, 0x040000A4, 32),  # DMA2 FIFO B destination.
    (0x040000D0, 1, 16),
    (0x040000D2, 0xB640, 16),
    (0x04000100, 0xF000, 16),  # Timer 0 clocks Direct Sound.
    (0x04000102, 0x0080, 16),
)


def arm_ldr_literal(register: int, offset: int) -> int:
    if not 0 <= offset <= 0xFFF:
        raise ValueError("ARM literal load offset is out of range")
    return 0xE59F0000 | (register << 12) | offset


def build_rom() -> bytes:
    rom = bytearray(LITERAL_POOL + len(WRITES) * 8)
    struct.pack_into("<I", rom, 0, 0xEA00002E)  # Branch to 0x080000C0.

    instructions = []
    for index, _ in enumerate(WRITES):
        address_literal = LITERAL_POOL + index * 8
        value_literal = address_literal + 4
        address_pc = ROM_ENTRY + len(instructions) * 4 + 8
        instructions.append(arm_ldr_literal(0, address_literal - address_pc))
        value_pc = ROM_ENTRY + len(instructions) * 4 + 8
        instructions.append(arm_ldr_literal(1, value_literal - value_pc))
        instructions.append(0xE1C010B0)  # STRH r1, [r0]
    instructions.append(0xEAFFFFFE)  # Idle forever after starting the tone.

    for index, instruction in enumerate(instructions):
        struct.pack_into("<I", rom, ROM_ENTRY + index * 4, instruction)
    for index, (address, value) in enumerate(WRITES):
        struct.pack_into("<II", rom, LITERAL_POOL + index * 8, address, value)
    return bytes(rom)


def build_direct_sound_rom() -> bytes:
    instructions = []
    literals = []

    def load_literal(register: int, value: int) -> None:
        instructions.append(0)
        literals.append((len(instructions) - 1, register, value))

    for address, value, width in DIRECT_SOUND_WRITES:
        load_literal(0, address)
        load_literal(1, value)
        instructions.append(0xE5801000 if width == 32 else 0xE1C010B0)

    instructions.append(0xEAFFFFFE)  # Idle forever after starting the tones.

    literal_pool = 0x200
    buffer_size = 32 * 1024
    rom = bytearray(0x9000 + buffer_size)
    struct.pack_into("<I", rom, 0, 0xEA00002E)  # Branch to 0x080000C0.
    for index, instruction in enumerate(instructions):
        address = ROM_ENTRY + index * 4
        struct.pack_into("<I", rom, address, instruction)

    # Resolve each LDR after the final instruction positions are known.
    for index, (instruction_index, register, value) in enumerate(literals):
        instruction_address = ROM_ENTRY + instruction_index * 4
        literal_address = literal_pool + index * 4
        offset = literal_address - (instruction_address + 8)
        struct.pack_into(
            "<I", rom, instruction_address,
            arm_ldr_literal(register, offset),
        )
        struct.pack_into("<I", rom, literal_address, value)

    # Alternate positive/negative samples at 4096 Hz, producing a 2048 Hz
    # square-wave component on each Direct Sound channel.
    for address, sample in ((0x1000, 0xE020E020), (0x9000, 0xC040C040)):
        rom[address:address + buffer_size] = struct.pack("<I", sample) * (
            buffer_size // 4
        )

    return bytes(rom)


if __name__ == "__main__":
    outputs = (
        (ROOT / "audio-check.gba", build_rom()),
        (ROOT / "direct-sound-check.gba", build_direct_sound_rom()),
    )
    for output, rom in outputs:
        output.write_bytes(rom)
        print(f"Wrote {output} ({output.stat().st_size} bytes)")
