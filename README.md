# System Monitor 46.0 — optimized build for Ubuntu 24.04.5 LTS

This branch (`perf/graph-rendering`) is GNOME System Monitor **46.0**, the
version shipped in **Ubuntu 24.04.5 LTS** (package `46.0-1build1`), with
performance optimizations, leak fixes and a GPU monitor added on top. It builds
against the stock 24.04 stack (GTK 4.14.5, gtkmm 4.10, libadwaita 1.5,
libgtop 2.41) and is a drop-in replacement for the distribution package.

It is not meant for upstream: GNOME `main` (51.x) needs GTK ≥ 4.17.5 and
libadwaita ≥ 1.8, and has since rewritten many of the files touched here.

## Results

Measured on an 88-CPU machine (NVIDIA GTX 980 Ti, ~1100 processes), main-thread
CPU of the app with the same settings, stock 46.0 vs this branch:

| | Stock 46.0 | This branch |
|---|---|---|
| Resources tab: CPU | 20–25 % | **11.2–12 %** |
| Resources tab: frame rate | ~10 fps, steps visibly | **~20 fps, continuous scroll** (cap 30) |
| Resources tab: render cost per frame | 18–23 ms | **4.6–4.9 ms** |
| Processes tab: CPU | 4.1–9.2 % | **3.2–3.3 %** |
| Heap growth (100 ms update interval) | ~20 kB/s (~1.7 GB/day) | **0** |
| Child processes spawned (Resources tab) | 6 per second (`lsblk \| sed …`) | **0** |

## Graph rendering (Resources tab)

- **Cached layers.** Each graph renders its background (grid, labels) and its
  curves into cached surfaces; animation frames only move the curves layer.
  Before, all curves (88 antialiased Bézier curves for the CPU graph) were
  re-rasterized on every animation tick just to shift them by a fraction of a
  pixel (~41 % of the profile). Software GL (llvmpipe): 79–87 % → 15 % CPU.
- **Smooth scrolling.** Scrolling follows elapsed time instead of a 10-step
  counter, moves in whole device pixels, redraws only when the offset changes,
  and is capped at 30 fps.
- **Reused render nodes.** The same GSK nodes are appended every frame, so GTK
  only repaints the plot strips instead of the whole window
  (4.04 → 3.55 ms paint per frame).
- **One shared animation timer** instead of a frame-clock tick per graph:
  frame clock cycles 100 → 31 per second.
- **One shared sampling clock.** All graphs sample together and scroll in
  lockstep: 31 → 20 window repaints/s, 13.2 → 11.3 % CPU.
- **Cairo GSK renderer by default** (an explicit `GSK_RENDERER` still wins):
  same cost as GL on a GPU, far cheaper under software GL.
- **Hidden graphs don't touch their labels.** ~95 labels were rewritten every
  second while another tab was shown, each forcing a layout cycle.
- **Disk I/O read directly from `/proc/diskstats`.** `glibtop_get_disk()` ran a
  six-process `lsblk | sed | sed | sed | sed | tr` pipeline on every sample and
  over-counted md RAID I/O ~1.75×; totals are now exact.

## Processes tab

- **Translations cached** for per-cell strings (`gettext` was ~14 % of the
  profile): full list redraw 42 → 32 ms.
- **`/proc/<pid>/cgroup` read once per update.** It was read four times per
  process (once directly, three times by `sd_pid_get_*`), about half of each
  update. The systemd unit/session/seat/owner are recomputed only when it
  changes: 163k → 95k syscalls per 30 s.
- **Fixed-height rows** in the tree view, so changed rows aren't re-measured:
  5–15 % → 2.7–5.3 % CPU in interleaved runs.
- **Waiting Channel read only while its column is shown** (hidden by default;
  was ~19 % of each update).

## GPU monitoring (new)

- **Resources tab: GPU section** with a load graph and current load, video
  memory, power draw and temperature per GPU. Shown only when a supported GPU
  is found; colors configurable (`gpu-colors` key).
- **Processes tab: "% GPU" and "GPU Memory" columns**, placed next to % CPU and
  Memory. Load is the busiest engine (3D/compute, encode, decode) averaged
  since the previous update.
- **NVIDIA** via NVML (`libnvidia-ml.so.1`, the library behind `nvidia-smi`),
  loaded with `dlopen()` on first use, so there's no build or package
  dependency. A full sample is ~0.65 ms of direct library calls; no process
  is spawned. **AMD** via amdgpu sysfs (no per-process columns yet).
- **Zero overhead without a GPU:** no section, no columns, NVML never loaded.
  The per-process query runs only while a GPU column is visible. A
  runtime-suspended GPU (idle laptop dGPU) shows "Suspended" and isn't woken up.

## Memory leak fixes

Found with AddressSanitizer and valgrind, all in code that runs repeatedly
(several are also fixed upstream since 48–50):

- **Color buttons** leaked the cairo context and its recording surface on every
  redraw: the whole ~20 kB/s heap growth. Their arrow icon is now rasterized
  once per process instead of replaying the SVG on each redraw.
- **Memory label** text leaked on every sample.
- **SELinux** security context leaked for every new process.
- **File Systems tab** leaked each filesystem's icon, pixbuf and mount names on
  every refresh.

Leaked at exit under valgrind: 495 KB → 34 KB (the rest is one-time startup
allocations).

## Testing

- ASan + UBSan: no memory errors or undefined behaviour across all tabs and
  settings.
- valgrind memcheck: no invalid accesses in System Monitor code.
- TSan: no races in System Monitor data (it creates no threads; all reports
  pair the main thread with GLib/GTK library threads).
- The desktop and appdata validation tests pass. The `formatting` suite fails
  only on the six files that already fail it in stock 46.0.

## Installing on Ubuntu 24.04 as a .deb (replacing the stock package)

### Prebuilt package

A prebuilt `amd64` package for Ubuntu 24.04 is attached to the
[v46.0-perf5 release](https://github.com/alfishe/gnome-system-monitor/releases/tag/v46.0-perf5):

```sh
wget https://github.com/alfishe/gnome-system-monitor/releases/download/v46.0-perf5/gnome-system-monitor_46.0-1build1+perf5_amd64.deb
sudo apt install ./gnome-system-monitor_46.0-1build1+perf5_amd64.deb
sudo apt-mark hold gnome-system-monitor
```

### Building the package yourself

The `packaging/debian/` directory is Ubuntu's packaging for
`46.0-1build1` with the build switched to LTO + profile-guided optimization.
It produces `gnome-system-monitor_46.0-1build1+perf5_amd64.deb`, which has a
higher version than the stock package, so it upgrades it in place.

1. Install the build tools and dependencies:

   ```sh
   sudo apt install git build-essential devscripts equivs
   git clone -b perf/graph-rendering https://github.com/alfishe/gnome-system-monitor.git
   cd gnome-system-monitor
   cp -r packaging/debian debian
   sudo mk-build-deps -i -r debian/control
   ```

2. Build the package. Run this from a terminal inside your desktop session:
   the profile-guided build starts the app for about 100 s (Resources,
   Processes and File Systems tabs) to collect a profile. Windows will open and
   close on their own; leave them alone.

   ```sh
   dpkg-buildpackage -us -uc -b
   ```

   Without a graphical session (SSH, container, CI), build with LTO only.
   It ran at the same speed in testing:

   ```sh
   DEB_BUILD_OPTIONS=nopgo dpkg-buildpackage -us -uc -b
   ```

3. Install it and hold it, so `apt upgrade` and unattended upgrades don't
   replace it with Ubuntu's build:

   ```sh
   sudo apt install ../gnome-system-monitor_46.0-1build1+perf5_amd64.deb
   sudo apt-mark hold gnome-system-monitor
   ```

   Check: `apt-mark showhold` lists `gnome-system-monitor`, and
   `dpkg -l gnome-system-monitor` shows `hi` and version `46.0-1build1+perf5`.
   Restart System Monitor if it was running.

**Updating** to a newer build of this branch: unhold, install, hold again
(apt won't change a held package):

```sh
git pull && rm -rf debian && cp -r packaging/debian debian
dpkg-buildpackage -us -uc -b
sudo apt-mark unhold gnome-system-monitor
sudo apt install ../gnome-system-monitor_46.0-1build1+perf5_amd64.deb
sudo apt-mark hold gnome-system-monitor
```

If the version in `packaging/debian/changelog` didn't change, use
`sudo apt install --reinstall ../gnome-system-monitor_*.deb` instead.

**Going back** to Ubuntu's package:

```sh
sudo apt-mark unhold gnome-system-monitor
sudo apt install --allow-downgrades gnome-system-monitor=46.0-1build1
```

While the package is held, Ubuntu security or bug-fix updates to
`gnome-system-monitor` aren't installed: `apt list --upgradable` will still
show them.

## Building without packaging

```sh
meson setup build --buildtype=release
ninja -C build
sudo ninja -C build install   # installs to /usr/local, alongside the stock package
```

---

# System Monitor
GNOME System Monitor is a GNOME process viewer and system monitor with an attractive, 
easy-to-use interface, It has features, such as a tree view for process dependencies,
icons for processes, the ability to hide processes that you don't want to see,
graphical time histories of CPU/memory/swap usage,
the ability to kill/renice processes needing root access,
as well as the standard features that you might expect from a process viewer.

## License
This project is licensed under the **GNU General Public License v2.0**. [Learn more](https://choosealicense.com/licenses/gpl-2.0/)

## Building
The steps described below show how to compile and install Gnome-System-Monitor from its source.

### Install required dependencies
To build the application, the following dependencies are required:

#### apt-get (Debian/Ubuntu/derivatives - deb-based package management)
Use the following command to install dependencies:
`sudo apt-get install meson gettext appstream-util itstool libglibmm-2.4-dev libgtkmm-3.0-dev libgtop2-dev librsvg2-dev libxml2-dev libhandy-1-dev libsystemd-dev`

#### dnf (Fedora/Centos/etc - rpm-based package management)
Use the following command to install dependencies:
`sudo dnf install meson gettext appstream itstool glibmm24-devel gtkmm30-devel libgtop2-devel librsvg2-devel libxml2-devel libhandy1-devel systemd-devel`

#### Optional dependencies:
- polkit - recommended
- gksu2
- libgnomesu
- libselinux
- lsb_release in PATH - recommended on linux
- libwnck


### Building and installing
Before following the steps below, clone the repository and change to its working directory.

##### Configure and create the build directory with Meson.
`meson build`

Where `build` is just a directory name, and is up to your chosing.
##### Build the application - this compiles the source.
`ninja -C build`
 
##### Install the application on your system - required to run Gnome-System-Monitor.
`ninja -C build install`

### Cleanup

##### Use the following command to clean up the build directory and remove old build files.
`ninja -C build -t clean`

##### Remove the build directory to rebuild from scratch.
`rm -rf build`

## Bugs

Please file System-Monitor bugs at:
https://gitlab.gnome.org/GNOME/gnome-system-monitor/issues
