#!/usr/bin/env python3
"""Build 3DS-friendly graphics, original level layout, and optional PCM audio
from the ZIP supplied by the user. No game assets are redistributed with source.
"""
import argparse
import io
import shutil
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is needed: py -m pip install pillow")

HERE = Path(__file__).resolve().parent
SPECS = {
    # image, original atlas entry, output image size
    'title': ('game3.bin', 'menutitle', (368, 45)),
    'menu': ('game2.bin', 'mainmenu', (137, 170)),
    'footer': ('game2.bin', 'footer', (248, 28)),
    'leftbar': ('game5.bin', 'leftmenubar', (17, 216)),
    'rightbar': ('game5.bin', 'rightmenubar', (17, 216)),
    'background': ('game1.bin', 'background', (400, 240)),
    'background2': ('game1.bin', 'background2', (400, 240)),
    'background3': ('game1.bin', 'background3', (400, 240)),
    'background4': ('game1.bin', 'background4', (400, 240)),
    'background5': ('game1.bin', 'background5', (400, 240)),
    'levels': ('game2.bin', 'levels', (198, 128)),
    'player': ('game1.bin', 'player', (24, 24)),
    'spike': ('game1.bin', 'spike', (24, 24)),
    'block': ('game1.bin', 'block', (24, 24)),
    'attempt': ('game1.bin', 'attempt', (124, 33)),
    'pressstart': ('game1.bin', 'pressstart', (160, 25)),
    'leftarrow': ('game2.bin', 'left', (21, 20)),
    'rightarrow': ('game2.bin', 'right', (21, 20)),
    'practiceheader': ('game1.bin', 'practicemodeheader', (232, 32)),
    'flag': ('game1.bin', 'flag', (18, 38)),
    'resumemenu': ('game1.bin', 'resumemenu', (232, 88)),
    'endnormal': ('game3.bin', 'endnoflags', (352, 169)),
    'endpractice': ('game5.bin', 'endpracticemode', (352, 169)),
}


def read_string(data, offset):
    length = struct.unpack_from('>H', data, offset)[0]
    offset += 2
    return data[offset:offset + length].decode('utf-8'), offset + length


def parse_atlas(data):
    offset = 0
    count = struct.unpack_from('>H', data, offset)[0]
    offset += 2
    if count != 1:
        raise ValueError('Unexpected atlas texture count')
    png, offset = read_string(data, offset)
    entry_count = struct.unpack_from('>H', data, offset)[0]
    offset += 2
    channel, offset = read_string(data, offset)
    if channel != 'CHANNEL':
        raise ValueError('Unknown atlas channel')
    entries = {}
    for _ in range(entry_count):
        name, offset = read_string(data, offset)
        uv = struct.unpack_from('>ffff', data, offset)
        offset += 16
        entries[name] = uv
    return png, entries


def encode_image(image, output):
    # 2x 16-bit little endian width, height + 4 bytes RGBA per pixel
    if image.width > 65535 or image.height > 65535:
        raise ValueError('Image too large for our format')
    output.write_bytes(struct.pack('<HH', *image.size) + image.convert('RGBA').tobytes())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path, help='Your original The.Impossible.Game.zip')
    parser.add_argument('--no-audio', action='store_true', help='Skip PCM conversion')
    args = parser.parse_args()
    if not args.archive.exists():
        parser.error('Game ZIP not found')
    graphics_dir = HERE / 'romfs' / 'sprites'
    audio_dir = HERE / 'romfs' / 'audio'
    graphics_dir.mkdir(parents=True, exist_ok=True)
    audio_dir.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(args.archive) as archive:
        names = {name.split('/')[-1]: name for name in archive.namelist() if '/res/' in name and not name.endswith('/')}
        def data(name):
            if name not in names:
                raise ValueError(f'{name} was not found inside res folder of your ZIP')
            return archive.read(names[name])

        atlases = {}
        for alias, (metadata, sprite_name, size) in SPECS.items():
            if metadata not in atlases:
                filename, entries = parse_atlas(data(metadata))
                atlas = Image.open(io.BytesIO(data(filename))).convert('RGBA')
                atlases[metadata] = (atlas, entries)
            atlas, entries = atlases[metadata]
            u, v, w, h = entries[sprite_name]
            rect = (round(u * atlas.width), round(v * atlas.height),
                    round((u + w) * atlas.width), round((v + h) * atlas.height))
            part = atlas.crop(rect)
            # Original art remains in the original user's ZIP. Only generated locally.
            part = part.resize(size, Image.Resampling.LANCZOS)
            encode_image(part, graphics_dir / (alias + '.rgba'))
            print(f'Extracted {alias:12} {size[0]}x{size[1]}')
            if alias == 'player':
                ico = Image.new('RGBA', (48, 48), (0, 0, 0, 255))
                ico.alpha_composite(part.resize((32, 32)), (8, 8))
                ico.save(HERE / 'icon.png')

        (HERE / 'romfs' / 'level5.bin').write_bytes(data('level5.bin'))
        print('Extracted level5.bin (original level object positions)')
        if not args.no_audio:
            ffmpeg = shutil.which('ffmpeg')
            if ffmpeg is None:
                print('FFmpeg not detected: no music; install ffmpeg then rerun for soundtrack.')
            else:
                tracks = {'menutrack.ogg': 'menu.pcm', 'soundtrack.ogg': 'level.pcm',
                          'soundtrack2.ogg': 'level2.pcm', 'soundtrack3.ogg': 'level3.pcm',
                          'soundtrack4.ogg': 'level4.pcm', 'explosion.wav': 'explosion.pcm',
                          'boop.wav': 'select.pcm',
                          'practicetrack.ogg': 'practice.pcm', 'fireworks.wav':'fireworks.pcm'}
                for src, dest in tracks.items():
                    proc = subprocess.run([
                        ffmpeg, '-hide_banner', '-loglevel', 'error', '-y',
                        '-i', 'pipe:0', '-ac', '1', '-ar', '22050',
                        '-f', 's16le', '-acodec', 'pcm_s16le', str(audio_dir / dest)
                    ], input=data(src), capture_output=True)
                    if proc.returncode:
                        print('Audio conversion failed for', src, proc.stderr.decode(errors='replace'))
                    else:
                        print('Converted', src, 'to', dest)
    print('\nReady! Run make in MSYS2 with devkitPro to build a 3DSX.')


if __name__ == '__main__':
    main()
