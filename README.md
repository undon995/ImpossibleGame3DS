# The Impossible Game 3DS — Beta 0.7

> **Unofficial homebrew adaptation** for Nintendo 3DS (including Old 3DS). This is an early **beta**, not a finished or official release. Not affiliated with the original game's developer or publisher.

## Status
- **Beta 0.7 source preview**
- Five selectable level layouts
- Single-button jumping and rotating cube
- Touchscreen and face-button controls
- Practice Mode checkpoint support (**experimental**)
- Attempts increase on death and reset when leaving a level
- Original-style transitions and level events (**still being tested**)

## Known issues
- The **last level needs fixes** and may not be completable.
- Jump timing, block collisions, and audio synchronization do not always match the original.
- FPS, stability after many attempts, and Practice Mode need more hardware testing.
- This is a work in progress; please report issues with the level name and what happened.

## Build
This public repository is **source-only**. You will need devkitPro MSYS2, devkitARM, libctru, Python 3, Pillow, FFmpeg, and your **own legally obtained copy of the original game's assets**. The source includes an importer, not commercial graphics or audio.

```bash
python prepare_assets.py "/path/to/your/original-game.zip"
python prepare_levels.py --download
bash BUILD_MSYS2.sh
```

The build should create `ImpossibleGame3DS.3dsx`. Copy it to `SD:/3ds/ImpossibleGame3DS/` and launch via Homebrew Launcher. The installer commands only work after the full source files are committed.

## Controls
- **A / B / X / touchscreen:** Jump
- **START:** Pause
- **Practice Mode:** R to add a checkpoint, L to remove the latest checkpoint

## Assets and licensing
The original title, music, graphics, sprites, and related content remain the property of their respective owners. **No commercial game assets are included in this public source repository.** An asset-containing `.cia` / `.3dsx` should not be publicly distributed without permission from the rights holder.

No source-code license has been chosen for Beta 0.7; this repository does not grant permission to redistribute third-party assets.

## Report bugs
Use [Issues](https://github.com/undon995/ImpossibleGame3DS/issues) and include your 3DS model, level, and exact problem.

See [Beta release notes](RELEASE_NOTES.md) for details.
