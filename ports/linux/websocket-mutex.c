/**
 * @file
 * @brief Implementation of websocket client interface.
 * @author Kirill Neznamov
 * @date May 2022
 * @section LICENSE
 *
 * Copyright (C) 2022 Legrand North America, LLC
 * as an unpublished work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later WITH GCC-exception-2.0
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

static pthread_mutex_t websocket_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

void bsc_websocket_global_lock(void)
{
  pthread_mutex_lock(&websocket_mutex);
}

void bsc_websocket_global_unlock(void)
{
  pthread_mutex_unlock(&websocket_mutex);
}