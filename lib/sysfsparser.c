/*
 * License: GPLv3+
 * Copyright (c) 2014 Davide Madrisan <davide.madrisan@gmail.com>
 *
 * A library for parsing the sysfs filesystem
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#ifndef _GNU_SOURCE
# define _GNU_SOURCE	/* activate extra prototypes for glibc */
#endif

#include <sys/types.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "logging.h"
#include "messages.h"
#include "sysfsparser.h"
#include "xasprintf.h"

#define PATH_SYS_SYSTEM		"/sys/devices/system"
#define PATH_SYS_CPU		PATH_SYS_SYSTEM "/cpu"

enum sysfsparser_cpufreq_sysfile_numeric_id
{
  CPUINFO_CUR_FREQ,
  CPUINFO_MIN_FREQ,
  CPUINFO_MAX_FREQ,
  CPUINFO_LATENCY,
  SCALING_CUR_FREQ,
  SCALING_MIN_FREQ,
  SCALING_MAX_FREQ,
  MAX_VALUE_FILES
};

static const char *
  sysfsparser_devices_system_cpu_file_numeric[MAX_VALUE_FILES] = {
  [CPUINFO_CUR_FREQ] = "cpuinfo_cur_freq",
  [CPUINFO_MIN_FREQ] = "cpuinfo_min_freq",
  [CPUINFO_MAX_FREQ] = "cpuinfo_max_freq",
  [CPUINFO_LATENCY]  = "cpuinfo_transition_latency",
  [SCALING_CUR_FREQ] = "scaling_cur_freq",
  [SCALING_MIN_FREQ] = "scaling_min_freq",
  [SCALING_MAX_FREQ] = "scaling_max_freq"
};

enum sysfsparser_cpufreq_sysfile_string_id
{
  SCALING_DRIVER,
  SCALING_GOVERNOR,
  SCALING_AVAILABLE_GOVERNORS,
  SCALING_AVAILABLE_FREQS,
  MAX_STRING_FILES
};

static const char *
  sysfsparser_devices_system_cpu_file_string[MAX_STRING_FILES] = {
  [SCALING_DRIVER]   = "scaling_driver",
  [SCALING_GOVERNOR] = "scaling_governor",
  [SCALING_AVAILABLE_GOVERNORS] = "scaling_available_governors",
  [SCALING_AVAILABLE_FREQS] = "scaling_available_frequencies"
};

bool
sysfsparser_path_exist (const char *path, ...)
{
  char *filename;
  va_list args;

  va_start (args, path);
  if (vasprintf (&filename, path, args) < 0)
    plugin_error (STATE_UNKNOWN, errno, "vasprintf has failed");
  va_end (args);

  return access (filename, F_OK) == 0;
}

void
sysfsparser_opendir(DIR **dirp, const char *path, ...)
{
  char *dirname;
  va_list args;

  va_start (args, path);
  if (vasprintf (&dirname, path, args) < 0)
    plugin_error (STATE_UNKNOWN, errno, "vasprintf has failed");
  va_end (args);

  if ((*dirp = opendir (dirname)) == NULL)
    plugin_error (STATE_UNKNOWN, errno, "Cannot open %s", dirname);
}

void sysfsparser_closedir(DIR *dirp)
{
  closedir(dirp);
}

struct dirent *
sysfsparser_readfilename(DIR *dirp, unsigned int flags)
{
  struct dirent *dp;

  for (;;)
    {
      errno = 0;
      if ((dp = readdir (dirp)) == NULL)
	{
	  if (errno != 0)
	    plugin_error (STATE_UNKNOWN, errno, "readdir() failure");
	  else
	    return NULL;		/* end-of-directory */
	}

      /* ignore directory entries */
      if (!strcmp (dp->d_name, ".") || !strcmp (dp->d_name, ".."))
	continue;

     if (dp->d_type & flags)
	return dp;
    }
}

char *
sysfsparser_getline (const char *format, ...)
{
  FILE *fp;
  char *filename, *line = NULL;
  size_t len = 0;
  ssize_t chread;
  va_list args;

  va_start (args, format);
  if (vasprintf (&filename, format, args) < 0)
    plugin_error (STATE_UNKNOWN, errno, "vasprintf has failed");
  va_end (args);

  if ((fp = fopen (filename, "r")) == NULL)
    return NULL;
  
  chread = getline (&line, &len, fp);
  fclose (fp);

  if (chread < 1)
    return NULL;

  len = strlen (line);
  if (line[len-1] == '\n')
    line[len-1] = '\0';

  return line;
}

unsigned long long
sysfsparser_getvalue (const char *format, ...)
{
  char *line, *endptr, *filename;
  unsigned long long value;
  va_list args;

  va_start (args, format);
  if (vasprintf (&filename, format, args) < 0)
    plugin_error (STATE_UNKNOWN, errno, "vasprintf has failed");
  va_end (args);

  if (NULL == (line = sysfsparser_getline ("%s", filename)))
    return 0;

  errno = 0;
  value = strtoull (line, &endptr, 0);
  if ((endptr == line) || (errno == ERANGE))
    value = 0;

  free (line);
  return value;
}

static unsigned long
sysfsparser_cpufreq_get_value (unsigned int cpunum,
			       enum sysfsparser_cpufreq_sysfile_numeric_id which)
{
  char *line, *endptr;
  long value;

  if (which >= MAX_VALUE_FILES)
    return 0;

  line =
    sysfsparser_getline (PATH_SYS_CPU "/cpu%u/cpufreq/%s", cpunum,
			 sysfsparser_devices_system_cpu_file_numeric[which]);
  if (NULL == line)
    return 0;

  errno = 0;
  value = strtoul (line, &endptr, 10);
  if ((endptr == line) || (errno == ERANGE))
    value = 0;

  free (line);
  return value;
}

static char *
sysfsparser_cpufreq_get_string (unsigned int cpunum,
				enum sysfsparser_cpufreq_sysfile_string_id which)
{
  char *line;
  size_t len = 0;

  if (which >= MAX_STRING_FILES)
    return NULL;

  line = 
    sysfsparser_getline (PATH_SYS_CPU "/cpu%u/cpufreq/%s", cpunum,
			 sysfsparser_devices_system_cpu_file_string[which]);
  if (NULL == line)
    return 0;

  len = strlen (line);
  if (line[len-1] == '\n')
    line[len-1] = '\0';
  
  return line;
}

/* CPU Freq functions  */

int
sysfsparser_cpufreq_get_hardware_limits (unsigned int cpu,
					 unsigned long *min,
					 unsigned long *max)
{
  if ((!min) || (!max))
    return -EINVAL;

  *min = sysfsparser_cpufreq_get_value (cpu, CPUINFO_MIN_FREQ);
  if (!*min)
    return -ENODEV;

  *max = sysfsparser_cpufreq_get_value (cpu, CPUINFO_MAX_FREQ);
  if (!*max)
    return -ENODEV;

  return 0;
}

unsigned long
sysfsparser_cpufreq_get_freq_kernel (unsigned int cpu)
{
  return sysfsparser_cpufreq_get_value (cpu, SCALING_CUR_FREQ);
}

char *
sysfsparser_cpufreq_get_available_freqs (unsigned int cpu)
{
  return sysfsparser_cpufreq_get_string (cpu, SCALING_AVAILABLE_FREQS);
}

unsigned long
sysfsparser_cpufreq_get_transition_latency (unsigned int cpu)
{
  return sysfsparser_cpufreq_get_value (cpu, CPUINFO_LATENCY);
}

char *
sysfsparser_cpufreq_get_driver (unsigned int cpu)
{
  return sysfsparser_cpufreq_get_string (cpu, SCALING_DRIVER);
}

char *
sysfsparser_cpufreq_get_governor (unsigned int cpu)
{
  return sysfsparser_cpufreq_get_string (cpu, SCALING_GOVERNOR);
}

char *
sysfsparser_cpufreq_get_available_governors (unsigned int cpu)
{
  return sysfsparser_cpufreq_get_string (cpu, SCALING_AVAILABLE_GOVERNORS);
}

/* Thermal Sensors function  */

#define PATH_SYS_ACPI   "/sys/class"
#define PATH_SYS_ACPI_THERMAL   PATH_SYS_ACPI "/thermal"

/* Thermal zone device sys I/F, created once it's registered:
 * /sys/class/thermal/thermal_zone[0-*]:
 *    |---type:                   Type of the thermal zone
 *    |---temp:                   Current temperature
 *    |---mode:                   Working mode of the thermal zone
 *    |---policy:                 Thermal governor used for this zone
 *    |---trip_point_[0-*]_temp:  Trip point temperature
 *    |---trip_point_[0-*]_type:  Trip point type
 *    |---trip_point_[0-*]_hyst:  Hysteresis value for this trip point
 *    |---emul_temp:              Emulated temperature set node
 */

bool
sysfsparser_thermal_kernel_support ()
{
  if (chdir (PATH_SYS_ACPI_THERMAL) < 0)
    return false;

  return true;
}

int
sysfsparser_thermal_get_critical_temperature (unsigned int thermal_zone)
{
  char *type;
  int i, crit_temp = -1;

  /* as far as I can see, the only possible trip points are:
   *  'critical', 'passive', 'active0', and 'active1'
   * Four (optional) entries max.   */
  for (i = 0; i < 4; i++)
    {
      type = sysfsparser_getline (PATH_SYS_ACPI_THERMAL
				  "/thermal_zone%u/trip_point_%d_type",
				  thermal_zone, i);
      if (NULL == type)
	continue;

      /* cat /sys/class/thermal/thermal_zone0/trip_point_0_temp
       *  98000
       * cat /sys/class/thermal/thermal_zone0/trip_point_0_type
       *  critical   */
      if (strncmp (type, "critical", strlen ("critical")))
	continue;

      crit_temp = sysfsparser_getvalue (PATH_SYS_ACPI_THERMAL
					"/thermal_zone%u/trip_point_%d_temp",
					thermal_zone, i);

      if (crit_temp > 0)
	dbg ("a critical trip point has been found: %.2f degrees C\n",
	     (float) (crit_temp / 1000.0));

      free (type);
      break;
    }

  return crit_temp;
}

int
sysfsparser_thermal_get_temperature (unsigned int selected_zone,
				     unsigned int *zone, char **type)
{
  DIR *d;
  struct dirent *de;
  bool found_data = false;
  unsigned long max_temp = 0, temp = 0;

  if (chdir (PATH_SYS_ACPI_THERMAL) < 0)
    plugin_error (STATE_UNKNOWN, 0, "no ACPI thermal support in kernel "
		  "or incorrect path (\"%s\")", PATH_SYS_ACPI_THERMAL);

  if ((d = opendir (".")) == NULL)
    plugin_error (STATE_UNKNOWN, errno,
		  "cannot open() " PATH_SYS_ACPI_THERMAL);

  while ((de = readdir (d)))
    {
      /* ignore directory entries */
      if (!strcmp (de->d_name, ".") || !strcmp (de->d_name, ".."))
	continue;
      /* ignore all files but 'thermal_zone[0-*]' ones */
      if (strncmp (de->d_name, "thermal_zone", 12))
	continue;

      if ((selected_zone != ALL_THERMAL_ZONES) &&
	  (selected_zone != strtoul (de->d_name + 12, NULL, 10)))
	continue;

      if (!strncmp (de->d_name, "thermal_zone", 12))
	{
	  /* temperatures are stored in the files
	   *  /sys/class/thermal/thermal_zone[0-9}/temp	  */
	  temp = sysfsparser_getvalue (PATH_SYS_ACPI_THERMAL "/%s/temp",
				       de->d_name);
	  *type = sysfsparser_getline (PATH_SYS_ACPI_THERMAL "/%s/type",
				       de->d_name);

	  /* FIXME: as a 1st step we get the highest temp
	   *        reported by sysfs */
	  if (temp > 0)
	    {
	      found_data = true;
	      if (max_temp < temp)
		{
		  max_temp = temp;
		  *zone = strtoul (de->d_name + 12, NULL, 10);
		}
	    }
	  dbg ("thermal information found: %.2f degrees C, zone: %d\n",
	       (float) (max_temp / 1000.0), *zone);
	}
    }
  closedir (d);

  if (false == found_data)
    {
      if (selected_zone == ALL_THERMAL_ZONES)
	plugin_error (STATE_UNKNOWN, 0,
		      "no thermal information has been found");
      else
	plugin_error (STATE_UNKNOWN, 0,
		      "no thermal information for zone '%u'", selected_zone);
    }

  return max_temp;
}
