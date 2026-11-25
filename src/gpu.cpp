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
    return {};

  ssize_t n = read (fd, buf, sizeof buf - 1);
  close (fd);
  if (n <= 0)
    return {};

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
struct nvmlUtilization_t { unsigned gpu, memory; };
struct nvmlMemory_t { unsigned long long total, free, used; };
enum { NVML_SUCCESS = 0 };

struct Nvml
{
  bool loaded = false;
  int (*DeviceGetHandleByPciBusId) (const char *, nvmlDevice_t *);
  int (*DeviceGetName) (nvmlDevice_t, char *, unsigned);
  int (*DeviceGetUtilizationRates) (nvmlDevice_t, nvmlUtilization_t *);
  int (*DeviceGetMemoryInfo) (nvmlDevice_t, nvmlMemory_t *);
  int (*DeviceGetPowerUsage) (nvmlDevice_t, unsigned *);

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

    if (!init || !DeviceGetHandleByPciBusId || !DeviceGetName || !DeviceGetUtilizationRates
        || !DeviceGetMemoryInfo || !DeviceGetPowerUsage || init () != NVML_SUCCESS)
      {
        dlclose (lib);
        return false;
      }

    loaded = true;
    return true;
  }
};

Nvml nvml;

class NvidiaGpu : public GpuDevice
{
  std::string pci_path;
  std::string bus_id;
  std::string device_name;
  nvmlDevice_t device = nullptr;

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

    // NVML calls wake a runtime-suspended GPU; leave it asleep
    if (runtime_suspended (pci_path))
      {
        s.suspended = true;
        return s;
      }

    if (!device)
      {
        if (!nvml.load () || nvml.DeviceGetHandleByPciBusId (bus_id.c_str (), &device) != NVML_SUCCESS)
          {
            device = nullptr;
            return s;
          }

        char buf[96];
        if (nvml.DeviceGetName (device, buf, sizeof buf) == NVML_SUCCESS)
          device_name = buf;
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

    return s;
  }
};


/* AMD, through amdgpu's sysfs attributes */

class AmdGpu : public GpuDevice
{
  std::string pci_path;
  std::string power_path;
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
      s.power = value / 1e6;    // microwatts

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
    if (e->d_name[0] != '.')
      addresses.push_back (e->d_name);
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
