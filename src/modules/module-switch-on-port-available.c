/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering
  Copyright 2011 Canonical Ltd

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core.h>
#include <pulsecore/device-port.h>
#include <pulsecore/hashmap.h>

#include "module-switch-on-port-available-symdef.h"

static bool profile_good_for_output(pa_card_profile *profile) {
    pa_sink *sink;
    uint32_t idx;

    pa_assert(profile);

    if (profile->card->active_profile->n_sources != profile->n_sources)
        return false;

    if (profile->card->active_profile->max_source_channels != profile->max_source_channels)
        return false;

    /* Try not to switch to HDMI sinks from analog when HDMI is becoming available */
    PA_IDXSET_FOREACH(sink, profile->card->sinks, idx) {
        if (!sink->active_port)
            continue;

        if (sink->active_port->available != PA_AVAILABLE_NO)
            return false;
    }

    return true;
}

static bool profile_good_for_input(pa_card_profile *profile) {
    pa_assert(profile);

    if (profile->card->active_profile->n_sinks != profile->n_sinks)
        return false;

    if (profile->card->active_profile->max_sink_channels != profile->max_sink_channels)
        return false;

    return true;
}

static int try_to_switch_profile(pa_device_port *port) {
    pa_card_profile *best_profile = NULL, *profile;
    void *state;

    pa_log_debug("Finding best profile");

    PA_HASHMAP_FOREACH(profile, port->profiles, state) {
        bool good = false;

        if (best_profile && best_profile->priority >= profile->priority)
            continue;

        /* We make a best effort to keep other direction unchanged */
        switch (port->direction) {
            case PA_DIRECTION_OUTPUT:
                good = profile_good_for_output(profile);
                break;

            case PA_DIRECTION_INPUT:
                good = profile_good_for_input(profile);
                break;
        }

        if (!good)
            continue;

        best_profile = profile;
    }

    if (!best_profile) {
        pa_log_debug("No suitable profile found");
        return -1;
    }

    if (pa_card_set_profile(port->card, best_profile, false) != 0) {
        pa_log_debug("Could not set profile %s", best_profile->name);
        return -1;
    }

    return 0;
}

static void find_sink_and_source(pa_card *card, pa_device_port *port, pa_sink **si, pa_source **so) {
    pa_sink *sink = NULL;
    pa_source *source = NULL;
    uint32_t state;

    switch (port->direction) {
        case PA_DIRECTION_OUTPUT:
            PA_IDXSET_FOREACH(sink, card->sinks, state)
                if (port == pa_hashmap_get(sink->ports, port->name))
                    break;
            break;

        case PA_DIRECTION_INPUT:
            PA_IDXSET_FOREACH(source, card->sources, state)
                if (port == pa_hashmap_get(source->ports, port->name))
                    break;
            break;
    }

    *si = sink;
    *so = source;
}

static pa_hook_result_t port_available_hook_callback(pa_core *c, pa_device_port *port, void* userdata) {
    pa_card* card;
    pa_sink *sink;
    pa_source *source;
    bool is_active_profile, is_active_port;

    if (port->available == PA_AVAILABLE_UNKNOWN)
        return PA_HOOK_OK;

    card = port->card;

    if (!card) {
        pa_log_warn("Port %s does not have a card", port->name);
        return PA_HOOK_OK;
    }

    if (pa_idxset_size(card->sinks) == 0 && pa_idxset_size(card->sources) == 0)
        /* This card is not initialized yet. We'll handle it in
           sink_new / source_new callbacks later. */
        return PA_HOOK_OK;

    find_sink_and_source(card, port, &sink, &source);

    is_active_profile = card->active_profile == pa_hashmap_get(port->profiles, card->active_profile->name);
    is_active_port = (sink && sink->active_port == port) || (source && source->active_port == port);

    if (port->available == PA_AVAILABLE_NO && !is_active_port)
        return PA_HOOK_OK;

    if (port->available == PA_AVAILABLE_YES) {
        if (is_active_port)
            return PA_HOOK_OK;

        if (!is_active_profile) {
            if (try_to_switch_profile(port) < 0)
                return PA_HOOK_OK;

            pa_assert(card->active_profile == pa_hashmap_get(port->profiles, card->active_profile->name));

            /* Now that profile has changed, our sink and source pointers must be updated */
            find_sink_and_source(card, port, &sink, &source);
        }

        if (source)
            pa_source_set_port(source, port->name, false);
        if (sink)
            pa_sink_set_port(sink, port->name, false);
    }

    if (port->available == PA_AVAILABLE_NO) {
        if (sink) {
            pa_device_port *p2 = pa_device_port_find_best(sink->ports);

            if (p2 && p2->available != PA_AVAILABLE_NO)
                pa_sink_set_port(sink, p2->name, false);
            else {
                /* Maybe try to switch to another profile? */
            }
        }

        if (source) {
            pa_device_port *p2 = pa_device_port_find_best(source->ports);

            if (p2 && p2->available != PA_AVAILABLE_NO)
                pa_source_set_port(source, p2->name, false);
            else {
                /* Maybe try to switch to another profile? */
            }
        }
    }

    return PA_HOOK_OK;
}

static void handle_all_unavailable(pa_core *core) {
    pa_card *card;
    uint32_t state;

    PA_IDXSET_FOREACH(card, core->cards, state) {
        pa_device_port *port;
        void *state2;

        PA_HASHMAP_FOREACH(port, card->ports, state2) {
            if (port->available == PA_AVAILABLE_NO)
                port_available_hook_callback(core, port, NULL);
        }
    }
}

static pa_device_port *new_sink_source(pa_hashmap *ports, const char *name) {

    void *state;
    pa_device_port *i, *p = NULL;

    if (!ports)
        return NULL;
    if (name)
        p = pa_hashmap_get(ports, name);
    if (!p)
        PA_HASHMAP_FOREACH(i, ports, state)
            if (!p || i->priority > p->priority)
                p = i;
    if (!p)
        return NULL;
    if (p->available != PA_AVAILABLE_NO)
        return NULL;

    pa_assert_se(p = pa_device_port_find_best(ports));
    return p;
}

static bool profile_contains_available_ports(char *profile_name, pa_hashmap *ports) {
    pa_device_port *port;
    void *state;

    pa_assert(profile_name);

    PA_HASHMAP_FOREACH(port, ports, state) {
        if (pa_hashmap_get(port->profiles, profile_name)
            && port->available != PA_AVAILABLE_NO)
            return true;
    }

    return false;
}

static pa_card_profile *find_best_profile_with_available_ports(pa_card_new_data *data) {
    pa_card_profile *profile = NULL;
    pa_card_profile *best_profile = NULL;
    void *state;

    pa_assert(data);

    PA_HASHMAP_FOREACH(profile, data->profiles, state) {
        if (profile->available == PA_AVAILABLE_NO)
            continue;

        if (!profile_contains_available_ports(profile->name, data->ports))
            continue;

        if (!best_profile || profile->priority > best_profile->priority)
            best_profile = profile;
    }

    return best_profile;
}

static pa_hook_result_t card_new_hook_callback(pa_core *c, pa_card_new_data *data, void *u) {
    pa_card_profile *alt_profile;

    if (data->active_profile && profile_contains_available_ports(data->active_profile, data->ports))
        return PA_HOOK_OK;

    /* Try to avoid situations where we could settle on a profile when
       there are not available ports that could be actually used. */
    if (alt_profile = find_best_profile_with_available_ports(data)) {
        pa_card_new_data_set_profile(data, alt_profile->name);
        data->save_profile = false;
    }

    return PA_HOOK_OK;
}

static pa_hook_result_t sink_new_hook_callback(pa_core *c, pa_sink_new_data *new_data, void *u) {

    pa_device_port *p = new_sink_source(new_data->ports, new_data->active_port);

    if (p) {
        pa_log_debug("Switching initial port for sink '%s' to '%s'", new_data->name, p->name);
        pa_sink_new_data_set_port(new_data, p->name);
    }
    return PA_HOOK_OK;
}

static pa_hook_result_t source_new_hook_callback(pa_core *c, pa_source_new_data *new_data, void *u) {

    pa_device_port *p = new_sink_source(new_data->ports, new_data->active_port);

    if (p) {
        pa_log_debug("Switching initial port for source '%s' to '%s'", new_data->name, p->name);
        pa_source_new_data_set_port(new_data, p->name);
    }
    return PA_HOOK_OK;
}

int pa__init(pa_module*m) {
    pa_assert(m);

    /* Make sure we are after module-device-restore, so we can overwrite that suggestion if necessary */
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_CARD_NEW],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) card_new_hook_callback, NULL);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SINK_NEW],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) sink_new_hook_callback, NULL);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_SOURCE_NEW],
                           PA_HOOK_NORMAL, (pa_hook_cb_t) source_new_hook_callback, NULL);
    pa_module_hook_connect(m, &m->core->hooks[PA_CORE_HOOK_PORT_AVAILABLE_CHANGED],
                           PA_HOOK_LATE, (pa_hook_cb_t) port_available_hook_callback, NULL);

    handle_all_unavailable(m->core);

    return 0;
}
