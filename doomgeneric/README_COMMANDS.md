# Doom Stage5 Benchmark Commands

This file documents only the paths used for Stage5 performance comparison.

## What matters

- `SW baseline`: `-bench-sw`
- `HW baseline`: `-bench-hw -pl-scale`

When `-bench-hw -pl-scale` is used, runtime uses BRAM overlay mode:

- PL composite source: OFF
- Raster handoff source: shared BRAM ON
- Present path: PL upscale/present from BRAM source
- PS overlay policy:
  - gameplay: HUD/status band is overlaid by PS on final frame
  - menu/messages: not overlaid in perf mode (HUD-only path)
- In `-screen` mode with PL direct present:
  - composite ON: no PS copy path
  - composite OFF: only HUD/status band is copied by PS to scanout

## Throughput compare (no client)

Software path:

```sh
./doom_stream -iwad DOOM1.WAD -bench-sw -no-client -fullres -scaling 5 -async-present -timedemo demo1
```

Hardware + PL scale path:

```sh
./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -no-client -fullres -scaling 5 -async-present -timedemo demo1
```

Force HLS composite path:

```sh
./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -pl-composite -no-client -fullres -scaling 5 -async-present -timedemo demo1
```

Force BRAM handoff + PS HUD overlay path:

```sh
./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -pl-bram -no-client -fullres -scaling 5 -async-present -timedemo demo1
```

## Screen run (mini-DP)

Run from board shell:

```sh
./doom_stream -iwad DOOM1.WAD -screen -bench-hw -pl-scale -fullres -scaling 5 -async-present
```

Screen + timedemo (BRAM handoff + HUD overlay, full-performance path):

```sh
./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -pl-bram -screen -fullres -scaling 5 -async-present -timedemo demo1
```

Screen + timedemo (PL composite path, currently no HUD in this runtime flow):

```sh
./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -pl-composite -screen -fullres -scaling 5 -async-present -timedemo demo1
```

If startup prints `SCREEN: PL direct present disabled`, force scanout physical address:

```sh
DOOM_FB_SCANOUT_PHYS=0x<fb0_scanout_phys> ./doom_stream -iwad DOOM1.WAD -screen -bench-hw -pl-scale -fullres -scaling 5 -async-present
```

Disable HUD overlay completely (max perf sanity check):

```sh
DOOM_HUD_OVERLAY=0 ./doom_stream -iwad DOOM1.WAD -screen -bench-hw -pl-scale -fullres -scaling 5 -async-present
```

From SSH, launch on tty1:

```sh
sudo systemctl stop getty@tty1.service
sudo systemd-run --unit doom-screen --collect \
  -p WorkingDirectory=/home/xilinx/jupyter_notebooks \
  -p TTYPath=/dev/tty1 -p StandardInput=tty -p StandardOutput=journal -p StandardError=journal \
  /bin/bash -lc 'chvt 1; exec ./doom_stream -iwad DOOM1.WAD -screen -bench-hw -pl-scale -fullres -scaling 5 -async-present'
```

Logs:

```sh
journalctl -fu doom-screen
```

Stop:

```sh
sudo systemctl stop doom-screen
```

## Output mode switches

- `-tcp-screen`: TCP viewer mode
- `-screen`: local mini-DP (`/dev/fb0`)
- `-headless`: no output/present

sed -i '/^screenblocks/d;/^show_messages/d' .default.cfg
printf "screenblocks 10\nshow_messages 1\n" >> .default.cfg
grep -nE '^(screenblocks|show_messages)' .default.cfg

then /////////////////

sudo systemctl stop getty@tty1.service
sudo systemd-run --unit doom-screen --collect \
 -p WorkingDirectory=/home/xilinx/jupyter_notebooks \
 -p TTYPath=/dev/tty1 -p StandardInput=tty -p StandardOutput=journal -p StandardError=journal \
 /bin/bash -lc 'chvt 1; exec ./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -screen -fullres -scaling 5 -async-present -timedemo demo1'
