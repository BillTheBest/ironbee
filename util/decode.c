/*****************************************************************************
 * Licensed to Qualys, Inc. (QUALYS) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * QUALYS licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

/**
 * @file
 * @brief IronBee &mdash; String related functions
 * @author Nick LeRoy <nleroy@qualys.com>
 */

#include "ironbee_config_auto.h"

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>

#include <ironbee/types.h>
#include <ironbee/debug.h>
#include <ironbee/util.h>
#include <ironbee/string.h>
#include <ironbee/mpool.h>

ib_status_t ib_util_decode_url(char *data,
                               ib_flags_t *result)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    size_t len;
    rc = ib_util_decode_url_ex((uint8_t *)data, strlen(data), &len, result);
    if (rc == IB_OK) {
        *(data+len) = '\0';
    }
    IB_FTRACE_RET_STATUS(rc);
}

ib_status_t ib_util_decode_url_cow(ib_mpool_t *mp,
                                   const char *data_in,
                                   char **data_out,
                                   ib_flags_t *result)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    size_t len;
    uint8_t *out;
    rc = ib_util_decode_url_cow_ex(mp, (uint8_t *)data_in, strlen(data_in),
                                   &out, &len, result);
    if (rc == IB_OK) {
        *(out+len) = '\0';
        *data_out = (char *)out;
    }
    IB_FTRACE_RET_STATUS(rc);
}

ib_status_t ib_util_decode_html_entity(char *data,
                                       ib_flags_t *result)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    size_t len;
    rc = ib_util_decode_html_entity_ex((uint8_t *)data,
                                       strlen(data),
                                       &len,
                                       result);
    if (rc == IB_OK) {
        *(data+len) = '\0';
    }
    IB_FTRACE_RET_STATUS(rc);
}

ib_status_t ib_util_decode_html_entity_cow(ib_mpool_t *mp,
                                           const char *data_in,
                                           char **data_out,
                                           ib_flags_t *result)
{
    IB_FTRACE_INIT();
    ib_status_t rc;
    size_t len;
    uint8_t *out;
    rc = ib_util_decode_html_entity_cow_ex(mp,
                                           (uint8_t *)data_in,
                                           strlen(data_in),
                                           &out, &len,
                                           result);
    if (rc == IB_OK) {
        *(out+len) = '\0';
        *data_out = (char *)out;
    }
    IB_FTRACE_RET_STATUS(rc);
}
