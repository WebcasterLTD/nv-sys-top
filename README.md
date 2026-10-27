# nv-sys-top

An htop-style terminal monitor that shows your CPUs, memory **and NVIDIA GPUs**
in one screen, with a single process list that has CPU, RAM, GPU load and VRAM
side by side.

```
  0[||||                         7%]  14[||                            3%]
  1[                             0%]  15[                              0%]
  ...
Mem[|||||||||||||     19.8G/62.6G] CPU  10%  Load 3.07 4.29 6.62  Up 0d 13:09  Tasks 627
Swp[|||||||||||||||||| 856.8M/2.0G]
GPU0 RTX 5070 Ti  68°C  181/300W  fan 51%  2850MHz
Util[||||||||||||||||||||||||100%] VRAM[|||||||||||||||||||||||15.8/15.9G]
GPU1 RTX 5070 Ti  61°C  144/300W  fan 32%  2790MHz
Util[||||||||||||||||||||||||100%] VRAM[|||||||||||||||||||||||15.1/15.9G]
    PID USER      S  CPU%  MEM%     RES  GPU  GPU%    VRAM  COMMAND
3109934 alice     R 101.3   4.9    3.0G    0    99   14.6G  python serve.py --gpu 0
3109935 alice     R 100.3   4.9    3.0G    1    99   14.6G  python serve.py --gpu 1
   4429 root      S   2.0   0.2  114.2M  0,1     0  360.1M  /usr/lib/xorg/Xorg vt2
```

- One native binary written in C, about 40 KB. It needs ncurses and libc,
  nothing else.
- CPU, memory and processes are read straight from `/proc`.
- GPU data comes from **NVML** (`libnvidia-ml.so.1`, part of the NVIDIA
  driver). It is loaded at run time, so you don't need the CUDA toolkit to
  build. Without an NVIDIA driver the program still runs and shows the system
  part only.
- Every GPU is shown, plus each process's VRAM and its own SM load (`GPU%`).
- It doesn't call `nvidia-smi`, Python or any other helper.

## Build

Linux only. You need a C compiler, `make` and the ncurses development
package:

| Distro | Packages |
|---|---|
| Debian / Ubuntu | `sudo apt install build-essential libncurses-dev pkg-config` |
| Fedora / RHEL | `sudo dnf install gcc make ncurses-devel pkgconf` |
| Arch | `sudo pacman -S base-devel ncurses` |

```sh
git clone https://github.com/WebcasterLTD/nv-sys-top.git
cd nv-sys-top
make
./nv-sys-top
```

Install it system-wide (the default prefix is `/usr/local`):

```sh
sudo make install
```

Or for your user only:

```sh
make install PREFIX=$HOME/.local
```

## Usage

```
nv-sys-top [-d SECONDS] [-g] [-s cpu|mem|vram|gpu] [-b [-n ROWS]]

  -d SECONDS   refresh interval (default 1.0, 0.5 .. 10)
  -g           GPU processes only
  -s KEY       sort by cpu, mem, vram or gpu
  -b           batch: print one snapshot as text and exit
  -n ROWS      process rows in batch mode (default 20, 0 = all)
```

Batch mode is useful in scripts and logs:

```sh
nv-sys-top -b -g -s vram -n 0
```

### Keys

| Key | Action |
|---|---|
| `c` `m` `g` `u` | sort by CPU, RAM, VRAM, GPU load |
| `v` | show GPU processes only |
| `/` | filter by command line (empty input clears it) |
| arrows, PgUp, PgDn, Home, End | move the cursor |
| `k` / `K` | send SIGTERM / SIGKILL to the selected process (asks first) |
| `+` / `-` | slower / faster refresh |
| `q`, Esc, Ctrl-C | quit |

The cursor stays at the top of the list until you move it. After that it
follows the selected process as the order changes. Home pins it to the top
again.

## Notes

- Memory "used" is `MemTotal - MemAvailable`, the memory that can't be
  reclaimed without swapping.
- `CPU%` is per process across all of its threads, so 100% means one full
  core, as in htop.
- `VRAM` is the memory NVML reports for the process. A process on several
  GPUs shows every GPU index and its total VRAM across them.
- Per-process `GPU%` needs a driver that supports
  `nvmlDeviceGetProcessUtilization` (GeForce cards from Maxwell onward do).
  Where it isn't supported the column stays at 0.
- Inside containers, NVML reports host PIDs. Run nv-sys-top on the host to
  match GPU processes to the process list.

## License

MIT, see [LICENSE](LICENSE).

## Author

**Corneliu Ticu**, managing partner, [Webcaster Ltd](https://github.com/WebcasterLTD)
corneliu.ticu@gmail.com

Questions, ideas or interest in this project: write to me directly.
