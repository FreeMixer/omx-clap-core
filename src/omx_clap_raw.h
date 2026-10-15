/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * This file is part of omx-clap-host.
 *
 * omx-clap-host is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * omx-clap-host is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with omx-clap-host.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
 */

/** @file
 * @brief The raw CLAP open: dlopen, clap_entry, the plugin factory and a host that offers nothing. */

/*
 * omx_clap_raw.h: the bare sequence a probe, a qualifier or an oracle runs to reach a plugin, and
 * nothing more. dlopen the file, take `clap_entry`, check its CLAP version, init it, take the
 * plugin factory; create a plugin against a host whose every extension is absent and whose every
 * request is ignored, and init it.
 *
 * RAW BY CONSTRUCTION. No guard page, no warm-up, no clamp, no thread check, no timeout: a program
 * that judges a plugin must see what the plugin does on its own. A program that hosts plugins for
 * audio uses clap_host.h instead.
 *
 * Header only and inline: the library exports nothing for it and links nothing for it. The caller
 * links -ldl where its libc still needs it.
 */
#ifndef OMX_CLAP_RAW_H
#define OMX_CLAP_RAW_H

#include <dlfcn.h>
#include <stddef.h>

#include <clap/clap.h>

/** @brief An open .clap file: the library handle, its entry, inited, and its plugin factory. */
typedef struct omx_clap_raw {
  void *lib;                              /**< the dlopen handle */
  const clap_plugin_entry_t *entry;       /**< the file's clap_entry, init() has returned true */
  const clap_plugin_factory_t *factory;   /**< the entry's CLAP_PLUGIN_FACTORY_ID factory */
} omx_clap_raw_t;

/** @brief The null host's get_extension: no extension is offered. */
static inline const void *omx_clap_raw_no_extension(const clap_host_t *host, const char *id) {
  (void)host;
  (void)id;
  return NULL;
}

/** @brief The null host's request_restart, request_process and request_callback: the request is ignored. */
static inline void omx_clap_raw_no_request(const clap_host_t *host) { (void)host; }

/** @brief A clap_host_t initialiser for a host that offers no extension and ignores every request.
 *
 * @p name is the host name the plugin reads; vendor and url are empty, the version is "0".
 * `static const clap_host_t host = OMX_CLAP_RAW_NULL_HOST("my-probe");` */
#define OMX_CLAP_RAW_NULL_HOST(name)                                                                   \
  { CLAP_VERSION_INIT, NULL, (name), "", "", "0", omx_clap_raw_no_extension, omx_clap_raw_no_request, \
    omx_clap_raw_no_request, omx_clap_raw_no_request }

/** @brief Release what omx_clap_raw_open() took: deinit the entry, dlclose the file, zero @p raw.
 *
 * Every plugin created from the factory is destroyed first. A zeroed @p raw is left as it is. */
static inline void omx_clap_raw_close(omx_clap_raw_t *raw) {
  if (raw->entry) raw->entry->deinit();
  if (raw->lib) dlclose(raw->lib);
  raw->lib = NULL;
  raw->entry = NULL;
  raw->factory = NULL;
}

/** @brief Open a .clap file: dlopen it, take clap_entry, check its CLAP version, init it, take its plugin factory.
 *
 * @p path is passed to dlopen and to the entry's init as it is: a name with no slash goes through the
 * library search path.
 * @return NULL with @p raw filled, or why the file does not open (dlerror()'s text or a fixed string,
 * valid until the next dlerror call) with @p raw zeroed and nothing left loaded. */
static inline const char *omx_clap_raw_open(omx_clap_raw_t *raw, const char *path) {
  const char *why = NULL;

  raw->entry = NULL;
  raw->factory = NULL;
  raw->lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!raw->lib) return dlerror();
  raw->entry = (const clap_plugin_entry_t *)dlsym(raw->lib, "clap_entry");
  if (!raw->entry)
    why = "no clap_entry";
  else if (!clap_version_is_compatible(raw->entry->clap_version))
    why = "clap_entry has an incompatible CLAP version";
  else if (!raw->entry->init(path))
    why = "clap_entry init failed";
  if (why) {
    raw->entry = NULL;
    omx_clap_raw_close(raw);
    return why;
  }
  raw->factory = (const clap_plugin_factory_t *)raw->entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  if (!raw->factory) {
    omx_clap_raw_close(raw);
    return "no plugin factory";
  }
  return NULL;
}

/** @brief Create plugin @p id (the factory's first when NULL) against @p host and init it.
 *
 * @return the plugin, inited, for the caller to destroy; NULL when the factory has no such plugin, does
 * not create it or the plugin's init fails, in which case the created plugin is already destroyed. */
static inline const clap_plugin_t *omx_clap_raw_create(const omx_clap_raw_t *raw, const clap_host_t *host,
                                                       const char *id) {
  const clap_plugin_t *plugin;

  if (!id) {
    const clap_plugin_descriptor_t *desc =
        raw->factory->get_plugin_count(raw->factory) ? raw->factory->get_plugin_descriptor(raw->factory, 0) : NULL;
    if (!desc || !desc->id) return NULL;
    id = desc->id;
  }
  plugin = raw->factory->create_plugin(raw->factory, host, id);
  if (!plugin) return NULL;
  if (!plugin->init(plugin)) {
    plugin->destroy(plugin);
    return NULL;
  }
  return plugin;
}

#endif /* OMX_CLAP_RAW_H */
