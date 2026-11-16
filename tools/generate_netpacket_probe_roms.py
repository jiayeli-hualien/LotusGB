#!/usr/bin/env python3
"""Generate original synthetic ROMs for the RetroArch Netpacket probe."""

from pathlib import Path
import argparse


ROM_SIZE = 0x8000
PROGRAM_ADDRESS = 0x0150


def write_sram_marker(program: bytearray, marker: int, address: int) -> None:
    program.extend((0x3E, marker, 0xEA, address & 0xFF, address >> 8))


def make_rom(role: str, client_start: str = "auto",
             transfer_count: int = 1) -> bytes:
    if not 1 <= transfer_count <= 16:
        raise ValueError("transfer count must be between 1 and 16")
    if role == "host":
        outgoing = 0xA5
        # The host arms an external-clock transfer and waits for its peer.
        control = 0x80
        title = b"LGB NET HOST"
    elif role == "client":
        outgoing = 0x3C
        # The client starts the clock after a delay or an explicit A press.
        control = 0x81
        title = b"LGB NET CLIENT" if client_start == "auto" else b"LGB NET READY"
    else:
        raise ValueError(f"unsupported probe role: {role}")
    if client_start not in ("auto", "button"):
        raise ValueError(f"unsupported client start mode: {client_start}")

    rom = bytearray(ROM_SIZE)
    rom[0x100:0x103] = bytes((0xC3, PROGRAM_ADDRESS & 0xFF,
                              PROGRAM_ADDRESS >> 8))
    rom[0x134:0x134 + len(title)] = title
    rom[0x147] = 0x03  # MBC1 + RAM + battery.
    rom[0x148] = 0x00  # 32 KiB ROM.
    rom[0x149] = 0x02  # 8 KiB SRAM.

    program = bytearray((
        0xF3,                    # DI.
        0x31, 0xFE, 0xFF,        # LD SP, $FFFE.
        0x3E, 0x0A, 0xEA, 0x00, 0x00,  # Enable cartridge SRAM.
    ))
    write_sram_marker(program, 0x11, 0xA010)  # Entered program.
    if role == "client" and client_start == "button":
        # Wait for Game Boy A after the client has joined the Netplay room.
        # JOYP bit 5 low selects action buttons; A is active-low bit 0.
        program.extend((
            0x3E, 0x10, 0xE0, 0x00,  # Select action buttons at JOYP ($FF00).
            0xF0, 0x00,              # Wait for A (JOYP bit 0 becomes zero).
            0xCB, 0x47,              # BIT 0, A.
            0x20, 0xFA,              # JR NZ, back to LDH A, ($FF00).
            0x3E, 0x30, 0xE0, 0x00,  # Deselect both button groups.
        ))
    elif client_start == "auto" or role == "host":
        # Keep automatic probes for direct-IP launches and unattended tests.
        # This 250 ms delay is only a settling aid, not a join-state check.
        program.extend((
            0x3E, 0x01,              # LD A, 1 outer pass.
            0x06, 0xFF,              # LD B, 255.
            0x0E, 0xFF,              # LD C, 255.
            0x0D, 0x20, 0xFD,        # DEC C; JR NZ, DEC C.
            0x05, 0x20, 0xF8,        # DEC B; JR NZ, LD C, 255.
            0x3D, 0x20, 0xF3,        # DEC A; JR NZ, LD B, 255.
        ))
    write_sram_marker(program, 0x22, 0xA011)  # Transfer started.
    first_outgoing = outgoing
    for index in range(transfer_count):
        outgoing_byte = (first_outgoing + index *
                         (11 if role == "host" else 7)) & 0xFF
        program.extend((0x3E, outgoing_byte, 0xE0, 0x01))  # SB = outgoing byte.
        program.extend((0x3E, control, 0xE0, 0x02))        # Start transfer.
        program.extend((
            0xF0, 0x02,              # LDH A, ($FF02).
            0xCB, 0x7F,              # BIT 7, A.
            0x20, 0xFA,              # JR NZ, back to LDH A, ($FF02).
            0xF0, 0x01,              # Read received SB.
            0xEA, (0x20 + index), 0xA0,  # Save every received byte at SRAM[0x20+index].
        ))
        if index == 0:
            program.extend((0xEA, 0x00, 0xA0))  # Preserve the first-byte verifier prefix.
    write_sram_marker(program, 0x33, 0xA012)  # Transfer completed.
    program.extend((0x3E, first_outgoing, 0xEA, 0x01, 0xA0))
    program.extend((0x3E, 0xA5, 0xEA, 0x02, 0xA0))
    program.extend((0x3E, transfer_count, 0xEA, 0x14, 0xA0))
    write_sram_marker(program, 0x44, 0xA013)  # Final checkpoint reached.
    program.extend((0x18, 0xFE))  # Remain in the completed probe.
    rom[PROGRAM_ADDRESS:PROGRAM_ADDRESS + len(program)] = program

    checksum = 0
    for value in rom[0x134:0x14D]:
        checksum = (checksum - value - 1) & 0xFF
    rom[0x14D] = checksum
    global_checksum = sum(rom[:0x14E]) + sum(rom[0x150:])
    rom[0x14E:0x150] = global_checksum.to_bytes(2, "big")
    return bytes(rom)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument(
        "--client-start", choices=("auto", "button"), default="auto",
        help="start the client automatically after a short delay, or wait for Game Boy A",
    )
    parser.add_argument(
        "--transfers", type=int, choices=range(1, 17), default=1,
        metavar="1..16", help="number of ordered serial-byte pairs to probe",
    )
    args = parser.parse_args()
    output_dir = args.output_dir
    output_dir.mkdir(parents=True, exist_ok=True)
    for role in ("host", "client"):
        path = output_dir / f"{role}.gb"
        path.write_bytes(make_rom(role, args.client_start, args.transfers))
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
