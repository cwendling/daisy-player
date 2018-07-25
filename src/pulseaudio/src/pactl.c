/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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
#include "../config.h"
#endif

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <locale.h>
#include <ctype.h>

#include "sndfile.h"

#include <pulse/pulseaudio.h>
#include <pulse/ext-device-restore.h>

#include "i18n.h" 
#include "macro.h"
#include "core-util.h"

static pa_context *context = NULL;
static pa_mainloop_api *mainloop_api = NULL;

static char
    *list_type = NULL,
    *sample_name = NULL,
    *sink_name = NULL,
    *source_name = NULL,
    *module_args = NULL,
    *card_name = NULL,
    *profile_name = NULL,
    *port_name = NULL,
    *formats = NULL;

static pa_cvolume volume;
static enum volume_flags
{
    VOL_UINT     = 0,
    VOL_PERCENT  = 1,   
    VOL_LINEAR   = 2,
    VOL_DECIBEL  = 3,
    VOL_ABSOLUTE = 0 << 4,
    VOL_RELATIVE = 1 << 4,
} volume_flags;

static pa_proplist *proplist = NULL;

static SNDFILE *sndfile = NULL;
static pa_stream *sample_stream = NULL;

/* This variable tracks the number of ongoing asynchronous operations. When a
 * new operation begins, this is incremented simply with actions++, and when
 * an operation finishes, this is decremented with the complete_action()
 * function, which shuts down the program if actions reaches zero. */
static int actions = 0;


static enum
{
    SET_SINK_VOLUME
} action = SET_SINK_VOLUME;

static void quit (int ret)
{
    pa_assert(mainloop_api);
    mainloop_api->quit(mainloop_api, ret);
}

static void context_drain_complete (pa_context *c, void *userdata)
{
userdata = userdata; // avoid notification
    pa_context_disconnect(c);
}

static void drain (void)
{
    pa_operation *o;

    if (! (o = pa_context_drain (context, context_drain_complete, NULL)))
        pa_context_disconnect(context);
    else
        pa_operation_unref(o);
}

static void complete_action(void) 
{
    pa_assert(actions > 0);

    if (!(--actions))
        drain();
}

static void simple_callback (pa_context *c, int success, void *userdata)
{
c = c; // avoid notification
userdata = userdata; // avoid notification
    if (!success)
    {
        quit(1);
        return;
    }

    complete_action();
}

static void volume_relative_adjust (pa_cvolume *cv) 
{
    pa_assert(volume_flags & VOL_RELATIVE);

    /* Relative volume change is additive in case of UINT or PERCENT
     * and multiplicative for LINEAR or DECIBEL */
    if ((volume_flags & 0x0F) == VOL_UINT || (volume_flags & 0x0F) == VOL_PERCENT)
    {
        unsigned i;
        for (i = 0; i < cv->channels; i++) 
        {
            if (cv->values[i] + volume.values[i] < PA_VOLUME_NORM)
                cv->values[i] = PA_VOLUME_MUTED;
            else
                cv->values[i] = cv->values[i] + volume.values[i] - PA_VOLUME_NORM;
        }
    }
    if ((volume_flags & 0x0F) == VOL_LINEAR || (volume_flags & 0x0F) == VOL_DECIBEL)
        pa_sw_cvolume_multiply(cv, cv, &volume);
}

static void fill_volume(pa_cvolume *cv, unsigned supported)
{
    if (volume.channels == 1)
    {
        pa_cvolume_set(&volume, supported, volume.values[0]);
    }
    else
    if (volume.channels != supported)
    {
        quit(1);
        return;
    }

    if (volume_flags & VOL_RELATIVE)
        volume_relative_adjust(cv);
    else
        *cv = volume;
}

static void get_sink_volume_callback(pa_context *c, const pa_sink_info *i, int is_last, void *userdata)
{
userdata = userdata; // avoid notification
    pa_cvolume cv;

    if (is_last < 0)
    {
        quit(1);
        return;
    }

    if (is_last)
        return;

    pa_assert(i);

    cv = i->volume;
    fill_volume(&cv, i->channel_map.channels);
    pa_operation_unref (pa_context_set_sink_volume_by_name(c, sink_name, &cv, simple_callback, NULL));
}

/* PA_MAX_FORMATS is defined in internal.h so we just define a sane value here */
#define MAX_FORMATS 256

static void context_state_callback (pa_context *c, void *userdata)
{
userdata = userdata; // avoid notification
    pa_operation *o = NULL;

    pa_assert (c);
    switch (pa_context_get_state (c))
    {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY:
            switch (action)
            {
                case SET_SINK_VOLUME:
                    o = pa_context_get_sink_info_by_name(c, sink_name, get_sink_volume_callback, NULL);
                    break;

            }

            if (o)
            {
                pa_operation_unref(o);
                actions++;
            }

            if (actions == 0) 
            {
                quit(1);
            }

            break;

        case PA_CONTEXT_TERMINATED:
            quit(0);
            break;

        case PA_CONTEXT_FAILED:
        default:
            quit(1);
    }
}

static void exit_signal_callback (pa_mainloop_api *m, pa_signal_event *e,
                                  int sig, void *userdata)
{
m = m; // avoid notification
e = e; // avoid notification
sig = sig; // avoid notification
userdata = userdata; // avoid notification
    quit(0);
}

static int parse_volume (const char *vol_spec, pa_volume_t *vol,
                         enum volume_flags *vol_flags)
{
    double v;
    char *vs;
    const char *atod_input;

    pa_assert (vol_spec);
    pa_assert (vol);
    pa_assert (vol_flags);

    vs = pa_xstrdup (vol_spec);

    *vol_flags = (pa_startswith (vs, "+") || pa_startswith (vs, "-")) ?
                  VOL_RELATIVE : VOL_ABSOLUTE;
    if (strchr(vs, '.'))
        *vol_flags |= VOL_LINEAR;
    if (pa_endswith (vs, "%"))
    {
        *vol_flags |= VOL_PERCENT;
        vs[strlen(vs)-1] = 0;
    }
    atod_input = vs;

    if (atod_input[0] == '+')
        atod_input++; /* pa_atod() doesn't accept leading '+', so skip it. */

    if (pa_atod(atod_input, &v) < 0)
    {
        pa_xfree(vs);
        return -1;
    }

    pa_xfree(vs);

    if (*vol_flags & VOL_RELATIVE)
    {
        if ((*vol_flags & 0x0F) == VOL_UINT)
            v += (double) PA_VOLUME_NORM;
        if ((*vol_flags & 0x0F) == VOL_PERCENT)
            v += 100.0;
        if ((*vol_flags & 0x0F) == VOL_LINEAR)
            v += 1.0;
    }
    if ((*vol_flags & 0x0F) == VOL_PERCENT)
        v = v * (double) PA_VOLUME_NORM / 100;
    if ((*vol_flags & 0x0F) == VOL_LINEAR)
        v = pa_sw_volume_from_linear(v);
    if ((*vol_flags & 0x0F) == VOL_DECIBEL)
        v = pa_sw_volume_from_dB(v);

    if (!PA_VOLUME_IS_VALID((pa_volume_t) v))
    {
        return -1;
    }

    *vol = (pa_volume_t) v;

    return 0;
}

static int parse_volumes (char *args, unsigned n)
{
    unsigned i;

    if (n >= PA_CHANNELS_MAX)
    {
        return -1;
    }

    volume.channels = n;
    for (i = 0; i < volume.channels; i++)
    {
        enum volume_flags flags;

        if (parse_volume (args, &volume.values[i], &flags) < 0)
            return -1;

        if (i > 0 && flags != volume_flags)
        {
            return -1;
        }
        else
            volume_flags = flags;
    } // for

    return 0;
}

enum
{
    ARG_VERSION = 256
};

int pactl (char *device, char *volume)
{
    pa_mainloop *m = NULL;
    int ret = 1;
    char *server = NULL;

    proplist = pa_proplist_new();

    action = SET_SINK_VOLUME;         
    sink_name = pa_xstrdup (device);
    if (parse_volumes (volume, 1) < 0)
        goto quit;
    if (! (m = pa_mainloop_new()))
    {
        goto quit;
    }

    mainloop_api = pa_mainloop_get_api (m);

    pa_assert_se(pa_signal_init(mainloop_api) == 0);
    pa_signal_new(SIGINT, exit_signal_callback, NULL);
    pa_signal_new(SIGTERM, exit_signal_callback, NULL);
    pa_disable_sigpipe();

    if (! (context = pa_context_new_with_proplist(mainloop_api, NULL, proplist)))
    {
        goto quit;
    }

    pa_context_set_state_callback(context, context_state_callback, NULL);
    if (pa_context_connect(context, server, 0, NULL) < 0) 
    {
        goto quit;
    }

    if (pa_mainloop_run(m, &ret) < 0) 
    {
        goto quit;
    }

quit:
    if (sample_stream)
        pa_stream_unref(sample_stream);

    if (context)
        pa_context_unref(context);

    if (m)
    {
        pa_signal_done();
        pa_mainloop_free(m);
    }

    pa_xfree(server);
    pa_xfree(list_type);
    pa_xfree(sample_name);
    pa_xfree(sink_name);
    pa_xfree(source_name);
    pa_xfree(module_args);
    pa_xfree(card_name);
    pa_xfree(profile_name);
    pa_xfree(port_name);
    pa_xfree(formats);

    if (sndfile)
        sf_close(sndfile);

    if (proplist)
        pa_proplist_free(proplist);

    return ret;
} // pactl
