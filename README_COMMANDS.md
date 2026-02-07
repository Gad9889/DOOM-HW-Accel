# DOOM Stream Commands

This file keeps the practical run commands in one place for quick testing.

## 1) Screen output (mini-DP) - HW path

Use this for real on-screen HW benchmarking:

```bash
sudo systemctl stop getty@tty1.service
sudo systemd-run --unit doom-screen --collect \
  -p WorkingDirectory=/home/xilinx/jupyter_notebooks \
  -p TTYPath=/dev/tty1 -p StandardInput=tty -p StandardOutput=journal -p StandardError=journal \
  /bin/bash -lc 'chvt 1; exec ./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -screen -fullres -scaling 5 -async-present -timedemo demo1'
```

Read logs:

```bash
journalctl -fu doom-screen
```

Stop:

```bash
sudo systemctl stop doom-screen
sudo systemctl start getty@tty1.service
```

## 2) Screen output (mini-DP) - SW baseline

```bash
sudo systemctl stop getty@tty1.service
sudo systemd-run --unit doom-screen --collect \
  -p WorkingDirectory=/home/xilinx/jupyter_notebooks \
  -p TTYPath=/dev/tty1 -p StandardInput=tty -p StandardOutput=journal -p StandardError=journal \
  /bin/bash -lc 'chvt 1; exec ./doom_stream -iwad DOOM1.WAD -bench-sw -screen -fullres -scaling 5 -async-present -timedemo demo1'
```

## 3) Headless benchmark (no screen/no tcp)

HW:

```bash
./doom_stream -iwad DOOM1.WAD -no-client -bench-hw -pl-scale -fullres -scaling 5 -async-present -timedemo demo1
```

SW:

```bash
./doom_stream -iwad DOOM1.WAD -no-client -bench-sw -fullres -scaling 5 -async-present -timedemo demo1
```

## 4) TCP viewer mode

Board:

```bash
./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -fullres -scaling 5 -async-present
```

Host viewer:

```bash
python3 doomgeneric/doom_udp_viewer.py --ip <board-ip> --port 5000
```

## 5) RCAS-lite tuning

Default:

```bash
./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -screen -fullres -scaling 5 -async-present -timedemo demo1 -rcas-lite
```

Max strength check:

```bash
./doom_stream -iwad DOOM1.WAD -bench-hw -pl-scale -screen -fullres -scaling 5 -async-present -timedemo demo1 -rcas-lite 255
```

## Notes

- If `doom-screen` exits immediately, check:
  - `journalctl -u doom-screen -n 200 --no-pager`
- If tty handoff gets stuck, restart getty:
  - `sudo systemctl restart getty@tty1.service`
