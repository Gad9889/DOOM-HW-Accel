# Doom Stream Commands (Canonical)

This file is the single source of truth for `doom_stream` runtime commands.

## Build

From `doomgeneric/` on host:

```bat
build.bat
```

On board (if building there):

```sh
make -f Makefile.sdl
```

## Runtime Flags (doomgeneric_udp.c)

Use only these flags:

- `-tcp-screen` enable TCP viewer mode (default)
- `-screen` display on local `/dev/fb0` (mini-DP path)
- `-headless` disable TCP and skip present (pure headless benchmark mode)
- `-bench-sw` force software path
- `-bench-hw` force hardware path
- `-no-client` do not wait for TCP viewer
- `-bench-headless` skip present when no client is connected
- `-pl-scale` enable PL fullres upscale/present path
- `-native320` force 320x200 stream mode
- `-fullres` force 1600x1000 stream mode
- `-help` or `--help` print runtime help

DOOM engine flags you will commonly combine with these:

- `-iwad <file>`
- `-timedemo <demo>`
- `-scaling <n>`
- `-async-present`

## Recommended Bench Commands

### 0) Display mode quick switch

TCP screen mode:

```sh
./doom_stream -iwad DOOM1.WAD -tcp-screen
```

Mini-DP screen mode (local framebuffer):

```sh
./doom_stream -iwad DOOM1.WAD -screen
```

Headless mode:

```sh
./doom_stream -iwad DOOM1.WAD -headless
```

### 1) Software baseline, timedemo, no client, no present cost

```sh
./doom_stream -iwad DOOM1.WAD -bench-sw -headless -timedemo demo1 -native320
```

### 2) Hardware raster, timedemo, no client, no present cost

```sh
./doom_stream -iwad DOOM1.WAD -bench-hw -headless -timedemo demo1 -native320
```

### 3) Software fullres path (PS scaling), timedemo

```sh
./doom_stream -iwad DOOM1.WAD -bench-sw -no-client -timedemo demo1 -fullres -scaling 5 -async-present
```

### 4) Hardware + PL fullres present, timedemo

```sh
./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -no-client -timedemo demo1 -fullres -scaling 5 -async-present
```

### 5) Gameplay (not timedemo), hardware path

```sh
./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -fullres -scaling 5 -async-present
```

## Viewer Command (PC)

```sh
python doom_udp_viewer.py --ip <board_ip> --port 5000 --scale 0
```

`--scale 0` means auto-scale display window on the PC side.

## systemd Presets (Clean Screen Launch)

Use a templated service so you can switch presets (`screen-hw`, `screen-sw`, `headless-hw`) without changing unit files.

Create preset directory:

```sh
sudo mkdir -p /etc/doom/presets
```

Create template unit:

```sh
sudo tee /etc/systemd/system/doom@.service >/dev/null <<'EOF'
[Unit]
Description=Doom preset %i on tty1
After=multi-user.target
Conflicts=getty@tty1.service

[Service]
Type=simple
WorkingDirectory=/home/xilinx/jupyter_notebooks
EnvironmentFile=/etc/doom/presets/%i.env
ExecStart=/bin/bash -lc 'exec /home/xilinx/jupyter_notebooks/doom_stream $DOOM_ARGS'
TTYPath=/dev/tty1
StandardInput=tty
StandardOutput=journal
StandardError=journal
Restart=on-failure
User=root

[Install]
WantedBy=multi-user.target
EOF
```

Create preset env files:

```sh
sudo tee /etc/doom/presets/screen-hw.env >/dev/null <<'EOF'
DOOM_ARGS='-iwad /home/xilinx/jupyter_notebooks/DOOM1.WAD -screen -bench-hw -pl-scale -fullres -scaling 5 -async-present'
EOF

sudo tee /etc/doom/presets/screen-sw.env >/dev/null <<'EOF'
DOOM_ARGS='-iwad /home/xilinx/jupyter_notebooks/DOOM1.WAD -screen -bench-sw -fullres -scaling 5 -sync-present'
EOF

sudo tee /etc/doom/presets/headless-hw.env >/dev/null <<'EOF'
DOOM_ARGS='-iwad /home/xilinx/jupyter_notebooks/DOOM1.WAD -headless -bench-hw -pl-scale -fullres -scaling 5 -async-present -timedemo demo1'
EOF
```

Reload and start a preset:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now doom@screen-hw
```

Switch preset:

```sh
sudo systemctl stop doom@screen-hw
sudo systemctl start doom@headless-hw
```

Logs:

```sh
journalctl -fu doom@screen-hw
```

Stop:

```sh
sudo systemctl stop doom@screen-hw
```

## Useful Environment Variables

- `DOOM_RASTER_BASE` raster IP AXI-Lite base (default `0xA0000000`)
- `DOOM_PRESENT_BASE` present IP AXI-Lite base (default `0xA0010000`)
- `DOOM_SWAP_IPS=1` swap raster/present base defaults
- `DOOM_STAGE5_BRAM_HANDOFF=0` disable shared-BRAM handoff (forces DDR handoff)
- `DOOM_PL_COMPOSITE=1` force PL present source to composed `I_VideoBuffer` (HUD/menu included, default)

Example:

```sh
DOOM_RASTER_BASE=0xA0010000 DOOM_PRESENT_BASE=0xA0000000 ./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -no-client -timedemo demo1 -fullres -scaling 5 -async-present
```

## Notes

- If you run without `-timedemo`, game logic/tics can become the limiter.
- For throughput comparisons, use `-timedemo demo1`.
- `-bench-headless` is for pure render benchmarking without present/send cost.
- Legacy underscore/alias forms were removed; use the canonical flags listed above.
- `-screen` requires writable access to `/dev/fb0` (run as root or with proper group permissions).
- If `-screen` initialization fails, the program exits with an error (no automatic fallback to headless).
- `-screen` supports `/dev/fb0` in `16 bpp (RGB565)` and `32 bpp`.
- In `-screen` mode with PL upscale:
  - `32 bpp fb0`: PL can present directly to active scanout as `XRGB8888` (no CPU copy).
  - `16 bpp fb0`: PL can present directly to active scanout as `RGB565` (no CPU copy).
  - Direct present uses runtime fb0 scanout offset and stride; no fixed physical address lock is required.
