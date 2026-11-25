#include <config.h>

#include "gpu.h"

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <glib/gi18n.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

/* Reads a small sysfs/proc file; empty if it can't be read. */
static std::string
read_file (const std::string&path)
{
  char buf[256];
  int fd = open (path.c_str (), O_RDONLY | O_CLOEXEC);

  if (fd < 0)
    return {}
  ;

  ssize_t n = read (fd, buf, sizeof buf - 1);
  close (fd);
  if (n <= 0)
    return {}
  ;

  buf[n] = '\0';
  g_strchomp (buf);
  return buf;
}

static bool
read_u64 (const std::string&path,
          guint64          *value)
{
  std::string s = read_file (path);

  if (s.empty ())
    return false;

  *value = g_ascii_strtoull (s.c_str (), NULL, 0);
  return true;
}

/* sysfs path of the PCI device, e.g. /sys/bus/pci/devices/0000:03:00.0 */
static bool
runtime_suspended (const std::string&pci_path)
{
  return read_file (pci_path + "/power/runtime_status") == "suspended";
}


/* NVIDIA, through NVML */

namespace {
typedef void *nvmlDevice_t;
struct nvmlUtilization_t
{
  unsigned gpu, memory;
};
struct nvmlMemory_t
{
  unsigned long long total, free, used;
};
struct nvmlProcessInfo_t
{
  unsigned pid;
  unsigned long long usedGpuMemory;
  unsigned gpuInstanceId, computeInstanceId;
};
struct nvmlProcessUtilizationSample_t
{
  unsigned pid;
  unsigned long long timeStamp;
  unsigned smUtil, memUtil, encUtil, decUtil;
};
enum
{
  NVML_SUCCESS = 0, NVML_ERROR_NOT_FOUND = 6, NVML_ERROR_INSUFFICIENT_SIZE = 7
};
enum
{
  NVML_TEMPERATURE_GPU = 0
};
const unsigned long long NVML_VALUE_NOT_AVAILABLE = ~0ULL;
typedef int (*ProcessListFunc) (nvmlDevice_t,
                                unsigned *,
                                nvmlProcessInfo_t *);

struct Nvml
{
  bool loaded = false;
  int (*DeviceGetHandleByPciBusId) (const char *,
                                    nvmlDevice_t *);
  int (*DeviceGetName) (nvmlDevice_t,
                        char *,
                        unsigned);
  int (*DeviceGetUtilizationRates) (nvmlDevice_t,
                                    nvmlUtilization_t *);
  int (*DeviceGetMemoryInfo) (nvmlDevice_t,
                              nvmlMemory_t *);
  int (*DeviceGetPowerUsage) (nvmlDevice_t,
                              unsigned *);
  int (*DeviceGetTemperature) (nvmlDevice_t,
                               int,
                               unsigned *);
  // optional
  ProcessListFunc DeviceGetGraphicsRunningProcesses;
  ProcessListFunc DeviceGetComputeRunningProcesses;
  int (*DeviceGetProcessUtilization) (nvmlDevice_t,
                                      nvmlProcessUtilizationSample_t *,
                                      unsigned *,
                                      unsigned long long);

  /* Loads and initializes NVML on first use; false if unavailable. */
  bool
  load ()
  {
    static bool tried = false;

    if (tried)
      return loaded;
    tried = true;

    void *lib = dlopen ("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
      return false;

    auto init = (int (*)(void))dlsym (lib, "nvmlInit_v2");
    DeviceGetHandleByPciBusId = (decltype (DeviceGetHandleByPciBusId))dlsym (lib, "nvmlDeviceGetHandleByPciBusId_v2");
    DeviceGetName = (decltype (DeviceGetName))dlsym (lib, "nvmlDeviceGetName");
    DeviceGetUtilizationRates = (decltype (DeviceGetUtilizationRates))dlsym (lib, "nvmlDeviceGetUtilizationRates");
    DeviceGetMemoryInfo = (decltype (DeviceGetMemoryInfo))dlsym (lib, "nvmlDeviceGetMemoryInfo");
    DeviceGetPowerUsage = (decltype (DeviceGetPowerUsage))dlsym (lib, "nvmlDeviceGetPowerUsage");
    DeviceGetTemperature = (decltype (DeviceGetTemperature))dlsym (lib, "nvmlDeviceGetTemperature");
    // the v2 and v3 entry points take the same struct
    for (const char *sym : { "nvmlDeviceGetGraphicsRunningProcesses_v3", "nvmlDeviceGetGraphicsRunningProcesses_v2" })
      if (!DeviceGetGraphicsRunningProcesses)
        DeviceGetGraphicsRunningProcesses = (ProcessListFunc)dlsym (lib, sym);
    for (const char *sym : { "nvmlDeviceGetComputeRunningProcesses_v3", "nvmlDeviceGetComputeRunningProcesses_v2" })
      if (!DeviceGetComputeRunningProcesses)
        DeviceGetComputeRunningProcesses = (ProcessListFunc)dlsym (lib, sym);
    DeviceGetProcessUtilization = (decltype (DeviceGetProcessUtilization))dlsym (lib, "nvmlDeviceGetProcessUtilization");

    if (!init || !DeviceGetHandleByPciBusId || !DeviceGetName || !DeviceGetUtilizationRates
        || !DeviceGetMemoryInfo || !DeviceGetPowerUsage || !DeviceGetTemperature || init () != NVML_SUCCESS)
      {
        dlclose (lib);
        return false;
      }

    loaded = true;
    return true;
  }
};

Nvml nvml;

class NvidiaGpu: public GpuDevice
{
std::string pci_path;
std::string bus_id;
std::string device_name;
nvmlDevice_t device = nullptr;
// newest per-process utilization sample seen, so each call only
// returns the samples taken since the previous one
unsigned long long last_utilization_sample = 0;
std::vector<nvmlProcessInfo_t> processes;
std::vector<nvmlProcessUtilizationSample_t> samples;

/* Gets the NVML handle on first use; false if the GPU can't be queried
   * now (NVML calls would wake a runtime-suspended GPU). */
bool
ensure_device ()
{
  if (runtime_suspended (pci_path))
    return false;

  if (device)
    return true;

  if (!nvml.load () || nvml.DeviceGetHandleByPciBusId (bus_id.c_str (), &device) != NVML_SUCCESS)
    {
      device = nullptr;
      return false;
    }

  char buf[96];
  if (nvml.DeviceGetName (device, buf, sizeof buf) == NVML_SUCCESS)
    device_name = buf;
  return true;
}

void
add_process_memory (ProcessListFunc func,
                    GpuProcessMap&  stats)
{
  if (!func)
    return;

  unsigned count = processes.size ();
  int rc = func (device, &count, processes.data ());
  if (rc == NVML_ERROR_INSUFFICIENT_SIZE)
    {
      processes.resize (count + 8);
      count = processes.size ();
      rc = func (device, &count, processes.data ());
    }
  if (rc != NVML_SUCCESS)
    return;

  for (unsigned i = 0; i < count; i++)
    {
      unsigned long long used = processes[i].usedGpuMemory;
      if (used == NVML_VALUE_NOT_AVAILABLE)
        continue;
      // a process with both a graphics and a compute context is listed
      // twice with the same usage
      auto&memory = stats[processes[i].pid].memory;
      memory = MAX (memory, used);
    }
}

public:
NvidiaGpu (const std::string&pci_path,
           const std::string&bus_id)
  : pci_path (pci_path), bus_id (bus_id), device_name (_("NVIDIA GPU"))
{
}

std::string
name () override
{
  return device_name;
}

GpuSample
sample () override
{
  GpuSample s;

  if (!ensure_device ())
    {
      s.suspended = runtime_suspended (pci_path);
      return s;
    }

  nvmlUtilization_t util;
  if (nvml.DeviceGetUtilizationRates (device, &util) == NVML_SUCCESS)
    s.load = util.gpu / 100.0;

  nvmlMemory_t mem;
  if (nvml.DeviceGetMemoryInfo (device, &mem) == NVML_SUCCESS)
    {
      s.vram_used = mem.used;
      s.vram_total = mem.total;
    }

  unsigned milliwatts;
  if (nvml.DeviceGetPowerUsage (device, &milliwatts) == NVML_SUCCESS)
    s.power = milliwatts / 1000.0;

  unsigned celsius;
  if (nvml.DeviceGetTemperature (device, NVML_TEMPERATURE_GPU, &celsius) == NVML_SUCCESS)
    s.temperature = celsius;

  return s;
}

bool
has_process_stats () const override
{
  return true;
}

void
add_process_stats (GpuProcessMap&stats) override
{
  if (!ensure_device ())
    return;

  add_process_memory (nvml.DeviceGetGraphicsRunningProcesses, stats);
  add_process_memory (nvml.DeviceGetComputeRunningProcesses, stats);

  if (!nvml.DeviceGetProcessUtilization)
    return;

  if (samples.empty ())
    samples.resize (64);

  unsigned count = samples.size ();
  int rc = nvml.DeviceGetProcessUtilization (device, samples.data (), &count, last_utilization_sample);
  if (rc == NVML_ERROR_INSUFFICIENT_SIZE)
    {
      samples.resize (count + 16);
      count = samples.size ();
      rc = nvml.DeviceGetProcessUtilization (device, samples.data (), &count, last_utilization_sample);
    }
  if (rc != NVML_SUCCESS)     // NVML_ERROR_NOT_FOUND: no samples since the last call
    return;

  // The driver samples each process' utilization a few times per second;
  // average them per process. Like other system monitors, a process' load
  // is that of its busiest engine.
  std::unordered_map<pid_t, std::pair<double, unsigned> > sums;
  for (unsigned i = 0; i < count; i++)
    {
      const auto&u = samples[i];
      auto&sum = sums[u.pid];
      sum.first += MAX (u.smUtil, MAX (u.encUtil, u.decUtil));
      sum.second++;
      last_utilization_sample = MAX (last_utilization_sample, u.timeStamp);
    }
  for (const auto&i : sums)
    stats[i.first].load += MIN (100.0, i.second.first / i.second.second);
}
};


/* AMD, through amdgpu's sysfs attributes */

class AmdGpu: public GpuDevice
{
std::string pci_path;
std::string power_path;
std::string temperature_path;
std::string device_name;

public:
AmdGpu (const std::string&pci_path)
  : pci_path (pci_path)
{
  device_name = read_file (pci_path + "/product_name");
  if (device_name.empty ())
    device_name = _("AMD GPU");

  // average power (newer kernels report the current draw as power1_input)
  std::string hwmon = pci_path + "/hwmon";
  if (DIR *dir = opendir (hwmon.c_str ()))
    {
      while (struct dirent *e = readdir (dir))
        {
          if (strncmp (e->d_name, "hwmon", 5) != 0)
            continue;
          for (const char *file : { "/power1_average", "/power1_input" })
            {
              std::string path = hwmon + "/" + e->d_name + file;
              if (power_path.empty () && access (path.c_str (), R_OK) == 0)
                power_path = path;
            }
          // temp1 is the "edge" sensor
          std::string path = hwmon + "/" + e->d_name + "/temp1_input";
          if (temperature_path.empty () && access (path.c_str (), R_OK) == 0)
            temperature_path = path;
        }
      closedir (dir);
    }
}

std::string
name () override
{
  return device_name;
}

GpuSample
sample () override
{
  GpuSample s;

  // reading the attributes resumes a runtime-suspended GPU
  if (runtime_suspended (pci_path))
    {
      s.suspended = true;
      return s;
    }

  guint64 value;
  if (read_u64 (pci_path + "/gpu_busy_percent", &value))
    s.load = value / 100.0;

  read_u64 (pci_path + "/mem_info_vram_used", &s.vram_used);
  read_u64 (pci_path + "/mem_info_vram_total", &s.vram_total);

  if (!power_path.empty () && read_u64 (power_path, &value))
    s.power = value / 1e6;      // microwatts

  if (!temperature_path.empty () && read_u64 (temperature_path, &value))
    s.temperature = value / 1000.0;     // millidegrees

  return s;
}
};
}


static std::vector<std::unique_ptr<GpuDevice> >
probe_gpus ()
{
  std::vector<std::unique_ptr<GpuDevice> > gpus;
  const std::string base = "/sys/bus/pci/devices";
  DIR *dir = opendir (base.c_str ());

  if (!dir)
    return gpus;

  std::vector<std::string> addresses;
  while (struct dirent *e = readdir (dir))
    {
      if (e->d_name[0] != '.')
        addresses.push_back (e->d_name);
    }
  closedir (dir);
  std::sort (addresses.begin (), addresses.end ());

  for (const auto&address : addresses)
    {
      std::string path = base + "/" + address;

      // PCI class 0x03xxxx: display controller
      if (read_file (path + "/class").rfind ("0x03", 0) != 0)
        continue;

      char *driver_link = g_file_read_link ((path + "/driver").c_str (), NULL);
      if (!driver_link)
        continue;
      gchar *driver_name = g_path_get_basename (driver_link);
      std::string driver = driver_name;
      g_free (driver_name);
      g_free (driver_link);

      if (driver == "nvidia")
        gpus.emplace_back (new NvidiaGpu (path, address));
      else if (driver == "amdgpu" && access ((path + "/gpu_busy_percent").c_str (), R_OK) == 0)
        gpus.emplace_back (new AmdGpu (path));
    }

  return gpus;
}

const std::vector<std::unique_ptr<GpuDevice> >&
gpu_devices ()
{
  static auto gpus = probe_gpus ();

  return gpus;
}

bool
gpu_has_process_stats ()
{
  for (const auto&gpu : gpu_devices ())
    if (gpu->has_process_stats ())
      return true;
  return false;
}

GpuProcessMap
gpu_process_stats ()
{
  GpuProcessMap stats;

  for (const auto&gpu : gpu_devices ())
    gpu->add_process_stats (stats);
  return stats;
}
