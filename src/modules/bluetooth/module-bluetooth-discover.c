/***
  This file is part of PulseAudio.

  Copyright 2013 João Paulo Rechi Vita

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>

#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/module.h>
#include <pulsecore/usergroup.h>

PA_MODULE_AUTHOR("João Paulo Rechi Vita");
PA_MODULE_DESCRIPTION("Detect available Bluetooth daemon and load the corresponding discovery module");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);
PA_MODULE_USAGE(
    "headset=ofono|native|auto (bluez 5 only)"
    "autodetect_mtu=<boolean> (bluez 5 only)"
);

#define GDM_USERNAME "Debian-gdm"

struct userdata {
    uint32_t bluez5_module_idx;
    uint32_t bluez4_module_idx;
};

int pa__init(pa_module* m) {
    struct userdata *u;
    pa_module *mm;
    struct passwd *gdm_passwd;

    pa_assert(m);

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->bluez5_module_idx = PA_INVALID_INDEX;
    u->bluez4_module_idx = PA_INVALID_INDEX;

    /* Disable Bluetooth support on GDM */
    gdm_passwd = pa_getpwnam_malloc(GDM_USERNAME);
    if (getuid() == gdm_passwd->pw_uid) {
        pa_log_warn("Bluetooth support is disabled under %s, refusing to load", GDM_USERNAME);
        pa_xfree(u);
        return -1;
    }

    if (pa_module_exists("module-bluez5-discover")) {
        pa_module_load(&mm, m->core, "module-bluez5-discover", m->argument);
        if (mm)
            u->bluez5_module_idx = mm->index;
    }

    if (pa_module_exists("module-bluez4-discover")) {
        pa_module_load(&mm, m->core, "module-bluez4-discover",  NULL);
        if (mm)
            u->bluez4_module_idx = mm->index;
    }

    if (u->bluez5_module_idx == PA_INVALID_INDEX && u->bluez4_module_idx == PA_INVALID_INDEX) {
        pa_xfree(u);
        return -1;
    }

    return 0;
}

void pa__done(pa_module* m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->bluez5_module_idx != PA_INVALID_INDEX)
        pa_module_unload_by_index(m->core, u->bluez5_module_idx, true);

    if (u->bluez4_module_idx != PA_INVALID_INDEX)
        pa_module_unload_by_index(m->core, u->bluez4_module_idx, true);

    pa_xfree(u);
}
