#ifndef _GSM_CGROUPS_H_
#define _GSM_CGROUPS_H_

#include "application.h"

/* Updates info.cgroup_name; returns false if the process' cgroups didn't
 * change since the last call, so data derived from them can be kept. */
bool get_process_cgroup_info (ProcInfo&info);
bool cgroups_enabled ();

#endif /* _GSM_CGROUPS_H_ */
