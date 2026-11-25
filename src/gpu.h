#ifndef _GSM_GPU_H_
#define _GSM_GPU_H_

#include <glib.h>

#include <memory>
#include <string>
#include <vector>

/* GPU telemetry for the Resources tab.
 *
 * GPUs are found through sysfs (PCI display controllers bound to a
 * supported driver), so no GPU is touched just to find out whether
 * there is one:
 *  - nvidia: NVML (libnvidia-ml.so.1, the library nvidia-smi is built
 *    on), loaded with dlopen() on first use; a sample is a few direct
 *    library calls, no process is spawned.
 *  - amdgpu: plain sysfs attributes.
 * A GPU whose PCI device is runtime-suspended (e.g. an idle laptop dGPU)
 * is not queried, as that would wake it up. */

struct GpuSample
{
  bool suspended = false;
  double load = -1;           /* 0..1, < 0 if unknown */
  guint64 vram_used = 0;      /* bytes */
  guint64 vram_total = 0;     /* bytes, 0 if unknown */
  double power = -1;          /* watts, < 0 if unknown */
};

class GpuDevice
{
public:
  virtual ~GpuDevice() = default;
  /* may change after the first successful sample (NVML loads lazily) */
  virtual std::string name () = 0;
  virtual GpuSample   sample () = 0;
};

/* Supported GPUs of this machine, probed once. */
const std::vector<std::unique_ptr<GpuDevice> >&gpu_devices ();

#endif /* _GSM_GPU_H_ */
