/*
 * Copyright (c) 2014 Universidad de La Laguna <cap@pcg.ull.es>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "data.h"
#include "debug.h"
#include "device.h"
#include "driver.h"
#include "eml.h"
#include "error.h"
#include "monitor.h"

static const struct emlDriver* drivers[EML_DEVICE_TYPE_COUNT] = {0};
static struct emlDevice* devices = NULL;
static size_t ndevices = 0;

enum emlError emlInit() {
  if (devices)
    return EML_ALREADY_INITIALIZED;

#ifdef ENABLE_NVML
  extern struct emlDriver nvml_driver;
  drivers[EML_DEV_NVML] = &nvml_driver;
#endif

#ifdef ENABLE_RAPL
  extern struct emlDriver rapl_driver;
  drivers[EML_DEV_RAPL] = &rapl_driver;
#endif

#ifdef ENABLE_MIC
  extern struct emlDriver mic_driver;
  drivers[EML_DEV_MIC] = &mic_driver;
#endif

  devices = malloc(0);
  if (!devices)
    return EML_NO_MEMORY;

  for (size_t i = 0; i < EML_DEVICE_TYPE_COUNT; i++) {
    const struct emlDriver* drv = drivers[i];

    if (!drv)
      continue;

    if (drv->initialized) {
      dbglog_warn("Driver '%s' init failed: already initialized", drv->name);
      continue;
    }

    enum emlError ret = drv->init();

    assert(ret != EML_ALREADY_INITIALIZED);
    if (ret != EML_SUCCESS) {
      dbglog_warn("Driver '%s' init failed: %s", drv->name, drv->failed_reason);
      continue;
    }

    if (drv->ndevices > 0) {
      size_t newsize = (ndevices + drv->ndevices) * sizeof(*devices);
      struct emlDevice* tmpdevices;
      tmpdevices = realloc(devices, newsize);
      if (!tmpdevices) {
        free(devices);
        devices = NULL;
        return EML_NO_MEMORY;
      }
      devices = tmpdevices;

      for (size_t j = 0; j < drv->ndevices; j++) {
        struct emlDevice devinit = {
          .type = i,
          .index = j,
          .driver = drv,
        };

        struct emlDevice* newdev = &devices[ndevices + j];
        memcpy(newdev, &devinit, sizeof(*newdev));
        snprintf(newdev->name, MAX_NAME_LEN, "%s%zu", drv->name, j);
        emlDeviceMonitorInit(newdev);
      }

      ndevices += drv->ndevices;
    }
  }

  return EML_SUCCESS;
}

enum emlError emlShutdown() {
  if (!devices)
    return EML_NOT_INITIALIZED;

  for (size_t i = 0; i < ndevices; i++)
    emlDeviceMonitorShutdown(&devices[i]);

  for (size_t i = 0; i < EML_DEVICE_TYPE_COUNT; i++) {
    const struct emlDriver* drv = drivers[i];

    if (!drv) continue;
    if (!drv->initialized) continue;

    enum emlError ret = drv->shutdown();

    assert(ret != EML_NOT_INITIALIZED);
    if (ret != EML_SUCCESS) {
      dbglog_warn("Driver '%s' shutdown failed: %s", drv->name, drv->failed_reason);
      continue;
    }
  }

  free(devices);
  devices = NULL;

  return EML_SUCCESS;
}

enum emlError emlDeviceGetCount(size_t* const count) {
  if (!devices)
    return EML_NOT_INITIALIZED;

  *count = ndevices;
  return EML_SUCCESS;
}

enum emlError emlDeviceByIndex(size_t index, struct emlDevice** const device) {
  if (!devices)
    return EML_NOT_INITIALIZED;
  if (index >= ndevices)
    return EML_INVALID_PARAMETER;

  *device = &devices[index];
  return EML_SUCCESS;
}

enum emlError emlDeviceGetName(const struct emlDevice* const device, const char** const name) {
  if (!devices)
    return EML_NOT_INITIALIZED;
  if (!device)
    return EML_INVALID_PARAMETER;

  *name = device->name;
  return EML_SUCCESS;
}

enum emlError emlDeviceGetType(const struct emlDevice* const device, enum emlDeviceType* const type) {
  if (!devices)
    return EML_NOT_INITIALIZED;
  if (!device)
    return EML_INVALID_PARAMETER;

  *type = device->type;
  return EML_SUCCESS;
}

enum emlError emlDeviceGetTypeStatus(const enum emlDeviceType type, enum emlDeviceTypeStatus* const status) {
  if (!devices)
    return EML_NOT_INITIALIZED;
  if (type >= EML_DEVICE_TYPE_COUNT)
    return EML_INVALID_PARAMETER;

  if (drivers[type] == NULL)
    *status = EML_SUPPORT_NOT_COMPILED;
  else if (!drivers[type]->initialized)
    *status = EML_SUPPORT_NOT_RUNTIME;
  else
    *status = EML_SUPPORT_AVAILABLE;

  return EML_SUCCESS;
}

enum emlError emlDeviceStart(const struct emlDevice* const device) {
  if (!devices)
    return EML_NOT_INITIALIZED;
  if (!device)
    return EML_INVALID_PARAMETER;

  return emlDeviceMonitorStart(device);
}

enum emlError emlDeviceStop(
    const struct emlDevice* const device,
    struct emlData** const result)
{
  if (!devices)
    return EML_NOT_INITIALIZED;
  if (!device || !result)
    return EML_INVALID_PARAMETER;

  struct emlData* data;
  data = malloc(sizeof(*data));
  if (!data)
    return EML_NO_MEMORY;

  enum emlError ret = emlDeviceMonitorStop(device, &data);

  if (ret == EML_SUCCESS) {
    ret = emlDataUpdateTotals(data);
    *result = data;
  }

  return ret;
}

enum emlError emlStart() {
  if (!devices)
    return EML_NOT_INITIALIZED;

  for (size_t i = 0; i < ndevices; i++) {
    enum emlError ret = emlDeviceStart(&devices[i]);

    if (ret != EML_SUCCESS) {
      dbglog_error("emlStart: %s", emlErrorMessage(ret));
      while (i--) {
        struct emlData* discarded;
        emlDeviceStop(&devices[i], &discarded);
        emlDataFree(discarded);
      }

      return ret;
    }
  }

  return EML_SUCCESS;
}

enum emlError emlStop(struct emlData** results) {
  if (!devices)
    return EML_NOT_INITIALIZED;

  enum emlError ret = EML_SUCCESS;
  for (size_t i = 0; i < ndevices; i++) {
    ret = emlDeviceStop(&devices[i], &results[i]);

    if (ret != EML_SUCCESS) {
      dbglog_error("emlStop: %s", emlErrorMessage(ret));
    }
  }

  return ret;
}