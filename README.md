# Nami

Nami (波, "wave") is a native anime browsing and streaming app for the PlayStation 3, for consoles running custom
firmware. It runs entirely on the console, with no server or helper PC.

## Features

- Browse rows of shows with posters, and search with an on-screen keyboard and live results
- Hardware-accelerated H.264 playback, with subtitles, seeking and sub or dub audio
- Continue watching, My List, per-episode watched marks and resume
- Season selector, skip intro and automatic next episode
- 480p by default, or 720p in Settings
- Controller support, with Blu-ray remote support written but not yet tested
- Update check against this repository's releases

## Limits

- The PS3's H.264 decoder can't correctly play the 5-reference 1080p streams most sources use, so
  shows with only 1080p aren't available by default. **Settings > Experimental 1080p** plays them
  with visible corruption.
- It depends on a third-party website, and will stop working when that site changes until it's updated.
- Tested on one console, on Evilnat 4.91 with Cobra 8.5.

This project hosts no video. It isn't affiliated with Sony or with any site it reads from.

## Installing

Download the `.pkg` from [Releases](https://github.com/razziedazzel/nami-ps3/releases), put it on a
FAT32 USB stick or in `/dev_hdd0/packages/`, and install it from
**Package Manager > Install Package Files > Standard**.

**Restart the console after installing.** Nami then appears under **TV/Video Services**, not Game.

## Controls

| Where | Button | Action |
|---|---|---|
| Home | X / Triangle / Square | Open / Search / Settings |
| Home, personal rows | Select | Remove |
| Details | X | Choose a button or play an episode |
| Details, on an episode | Triangle | Watched options |
| Player | X | Pause |
| Player | D-pad left, right / up, down | Seek 10 / 60 seconds |
| Player | L1 / R1 | Previous / next episode |
| Player | Triangle / Square | Subtitles / skip intro |
| Anywhere | Circle | Back |

## Building

Build [ps3toolchain](https://github.com/ps3dev/ps3toolchain) at commit `de85d7d`, with these changes
for a modern host (tested on Fedora 44 with GCC 16):

1. Apply `toolchain/ps3toolchain.patch` to ps3toolchain before building. It fixes GCC building on GCC 16.
2. After PSL1GHT is unpacked, apply `toolchain/psl1ght-rsx-callback.patch` to it and rebuild `librsx`.
   **Without this, apps freeze when the RSX command buffer fills.** The callback's inline assembly
   lacks a `"memory"` clobber, so GCC 13 keeps using the old write position after it.
3. Skip the GDB steps, which don't compile on GCC 16.

The shader compiler also needs NVIDIA's Cg Toolkit, with `libCg.so` on `LD_LIBRARY_PATH`.

```bash
export PS3DEV=/opt/ps3dev PSL1GHT=/opt/ps3dev
export PATH=$PATH:$PS3DEV/bin:$PS3DEV/ppu/bin:$PS3DEV/spu/bin
cd app && make pkg
```

To test on a console running webMAN MOD, after installing the package once:

```bash
PS3_HOST=<address> tools/deploy_app.sh app ANIMEPS30
```

## License

GPL-3.0. See `LICENSE` and `THIRD_PARTY.md`.
