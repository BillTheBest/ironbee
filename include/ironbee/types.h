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

#ifndef _IB_TYPES_H_
#define _IB_TYPES_H_

/**
 * @file
 * @brief IronBee &mdash; General Type Definitions
 *
 * These are types that are used throughout IronBee.
 *
 * @author Brian Rectanus <brectanus@qualys.com>
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup IronBeeUtilTypes Type Definitions
 * @ingroup IronBeeUtil
 *
 * Common type definitions.
 *
 * @{
 */

/* Type definitions. */
typedef uint32_t ib_flags_t;
typedef uint64_t ib_flags64_t;

/** Generic function pointer type. */
typedef void (*ib_void_fn_t)(void);

/** Status code. */
typedef enum ib_status_t {
    IB_OK,                      /**<  0: No error */
    IB_DECLINED,                /**<  1: Declined execution */
    IB_EUNKNOWN,                /**<  2: Unknown error */
    IB_ENOTIMPL,                /**<  3: Not implemented (yet?) */
    IB_EINCOMPAT,               /**<  4: Incompatible with ABI version */
    IB_EALLOC,                  /**<  5: Could not allocate resources */
    IB_EINVAL,                  /**<  6: Invalid argument */
    IB_ENOENT,                  /**<  7: Entity does not exist */
    IB_ETRUNC,                  /**<  8: Buffer truncated, size limit reached */
    IB_ETIMEDOUT,               /**<  9: Operation timed out */
    IB_EAGAIN,                  /**< 10: Not ready, try again later */
    IB_EOTHER,                  /**< 11: Other error */
    IB_EBADVAL,                 /**< 12: A value outside the allowed range */
    IB_EEXIST,                  /**< 13: Entry already exists, not overwriting */
} ib_status_t;

/**
 * Convert status code to a string for human consumption.
 *
 * @param[in] status Status code.
 * @return String describing status code, e.g., EINVAL.
 **/
const char *ib_status_to_string(ib_status_t status);

/**
 * @} IronBeeUtil
 */

#ifdef __cplusplus
}
#endif

#endif /* _IB_TYPES_H_ */
