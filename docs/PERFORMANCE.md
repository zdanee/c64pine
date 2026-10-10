# The hidden C64: performance

> **Since v1.4** the desktop's C64 is one `x64sc` on the real display (it is also the C64 SCREEN and runs the C64
> software): no Xvfb any more (~50 MB less), and no second VICE for games. Measured on the Arch VM, the C64 running
> at real speed: `x64sc` 38 % of a core vs. `x64` 33 % (cycle-exact costs ~15 % more, only while BASIC runs); idle it
> is frozen at 0 % as before. The numbers below are from the old hidden `x64` + Xvfb.

c64desk runs C64 BASIC in a hidden VICE (`x64` on its own Xvfb `:64`), started with the first line that goes to
BASIC. Measured in a VM with 2 cores and 2 GB RAM (host: Core Ultra 7 258V; CPU = % of one core of it; a Core 2 Duo
core is roughly 3-4x slower, so multiply for an estimate).

| State | x64 CPU | Memory |
|---|---|---|
| desktop idle, BASIC never used | VICE not running (whole desktop ~1.7 % of 2 cores) | 0 |
| BASIC at READY. (between lines, after a program) | 0 % (SIGSTOP) | x64 ~200 MB + Xvfb ~60 MB |
| ... idle for 10 minutes | unloaded (snapshot in `~/.cache/c64`) | 0 |
| program running, printing or not | ~12 % | as above |
| program using the SID (sound on) | ~32 % (+ PipeWire ~1.5 %) | as above |

## What was changed (October 2026)

- **Warp**: VICE 3.10 has no `WarpMode` resource any more, so the "warp off after boot" never worked: the hidden C64
  ran at full host speed (a whole core) whenever a program ran, and BASIC timing was wrong. It now boots at normal
  speed (~2.5 s) and runs at the C64's speed.
- **Sound**: playing into ALSA/PipeWire cost VICE 15-25 % of a core even for silent programs. It plays into the
  `dummy` device until a program may use the SID (a POKE to 54272-54300, a POKE to a computed address, SYS).
- **Polling**: every look at the C64 stops and restarts the emulation through the binary monitor. c64desk polls
  every 25 ms while output flows and backs off to 100 ms when a program runs quietly (keys are still sent at once).
- **Unloading**: after `~/.config/c64/vice-unload` seconds stopped (default 600, 0 = never) c64desk saves a VICE
  snapshot and quits VICE and Xvfb; the next line restores it (program, variables, screen).
- Fixed on the way: a line longer than 1024 characters (`PRINT "X";` in a loop) froze the desktop text and grew
  without limit; RUN/STOP did not stop a program that printed a lot; a line typed during SYS 64738 could be fed
  before the C64 was ready.

Measure it yourself: `docs/perf-sample.sh` prints the CPU use per program every few seconds.
