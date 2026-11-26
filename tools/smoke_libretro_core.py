#!/usr/bin/env python3
"""Load a built LotusGB core through libretro and run a licensed homebrew ROM."""
import argparse
import ctypes
from pathlib import Path


RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY = 9
RETRO_ENVIRONMENT_SET_PIXEL_FORMAT = 10
RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS = 11
RETRO_PIXEL_FORMATS = {0, 1, 2}


class RetroGameInfo(ctypes.Structure):
    _fields_ = [
        ('path', ctypes.c_char_p),
        ('data', ctypes.c_void_p),
        ('size', ctypes.c_size_t),
        ('meta', ctypes.c_char_p),
    ]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', type=Path, required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--frames', type=int, default=120)
    args = parser.parse_args()
    core_path = args.core.resolve()
    rom_path = args.rom.resolve()
    if not core_path.is_file() or not rom_path.is_file():
        parser.error('core and ROM files must exist')
    if args.frames < 1 or args.frames > 10000:
        parser.error('frames must be between 1 and 10000')

    library = ctypes.CDLL(str(core_path))
    environment_callback_type = ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint, ctypes.c_void_p)
    video_callback_type = ctypes.CFUNCTYPE(
        None, ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint, ctypes.c_size_t)
    audio_callback_type = ctypes.CFUNCTYPE(None, ctypes.c_int16, ctypes.c_int16)
    audio_batch_callback_type = ctypes.CFUNCTYPE(
        ctypes.c_size_t, ctypes.POINTER(ctypes.c_int16), ctypes.c_size_t)
    input_poll_callback_type = ctypes.CFUNCTYPE(None)
    input_state_callback_type = ctypes.CFUNCTYPE(
        ctypes.c_int16, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_uint)

    video_frames = 0
    audio_frames = 0
    bad_frames = []

    @environment_callback_type
    def environment(command, data):
        if command == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            if data:
                ctypes.cast(data, ctypes.POINTER(ctypes.c_char_p))[0] = None
                return True
            return False
        if command == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            if not data:
                return False
            return ctypes.cast(data, ctypes.POINTER(ctypes.c_uint))[0] in RETRO_PIXEL_FORMATS
        if command == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
            return True
        return False

    @video_callback_type
    def video_refresh(data, width, height, pitch):
        nonlocal video_frames
        video_frames += 1
        if not data or width != 160 or height != 144 or pitch < width * 2:
            bad_frames.append((bool(data), width, height, pitch))

    @audio_batch_callback_type
    def audio_batch(_samples, frames):
        nonlocal audio_frames
        audio_frames += frames
        return frames

    @audio_callback_type
    def audio_sample(_left, _right):
        return None

    @input_poll_callback_type
    def input_poll():
        return None

    @input_state_callback_type
    def input_state(port, device, _index, _button):
        return 0

    api = {
        'retro_set_environment': (None, [environment_callback_type]),
        'retro_set_video_refresh': (None, [video_callback_type]),
        'retro_set_audio_sample': (None, [audio_callback_type]),
        'retro_set_audio_sample_batch': (None, [audio_batch_callback_type]),
        'retro_set_input_poll': (None, [input_poll_callback_type]),
        'retro_set_input_state': (None, [input_state_callback_type]),
        'retro_init': (None, []),
        'retro_deinit': (None, []),
        'retro_load_game': (ctypes.c_bool, [ctypes.POINTER(RetroGameInfo)]),
        'retro_run': (None, []),
        'retro_unload_game': (None, []),
    }
    for name, (return_type, argument_types) in api.items():
        function = getattr(library, name)
        function.restype = return_type
        function.argtypes = argument_types

    library.retro_set_environment(environment)
    library.retro_set_video_refresh(video_refresh)
    library.retro_set_audio_sample(audio_sample)
    library.retro_set_audio_sample_batch(audio_batch)
    library.retro_set_input_poll(input_poll)
    library.retro_set_input_state(input_state)

    rom_data = rom_path.read_bytes()
    rom_buffer = ctypes.create_string_buffer(rom_data)
    game = RetroGameInfo(str(rom_path).encode(), ctypes.cast(rom_buffer, ctypes.c_void_p),
                         len(rom_data), None)
    initialized = False
    loaded = False
    try:
        library.retro_init()
        initialized = True
        if not library.retro_load_game(ctypes.byref(game)):
            raise RuntimeError(f'core rejected ROM: {rom_path.name}')
        loaded = True
        for _ in range(args.frames):
            library.retro_run()
        if bad_frames:
            raise RuntimeError(f'core returned invalid video frame: {bad_frames[0]}')
        if video_frames != args.frames:
            raise RuntimeError(f'expected {args.frames} video callbacks, got {video_frames}')
    finally:
        if loaded:
            library.retro_unload_game()
        if initialized:
            library.retro_deinit()

    print(f'libretro smoke: PASS core={core_path.name} frames={video_frames} '
          f'audio_frames={audio_frames}')


if __name__ == '__main__':
    main()
