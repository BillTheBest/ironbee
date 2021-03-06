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

/* This is always re-included to allow for prefixing the symbol names. */

#ifndef _IB_CORE_H_
#define _IB_CORE_H_

/**
 * @file
 * @brief IronBee &mdash; Module
 *
 * @author Brian Rectanus <brectanus@qualys.com>
 */

#include <ironbee/build.h>
#include <ironbee/types.h>
#include <ironbee/module.h>
#include <ironbee/rule_defs.h>
#include <ironbee/logformat.h>

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup IronBeeCore Core
 * @ingroup IronBee
 *
 * Core implements much of IronBee as a module.
 *
 * @{
 */

#define CORE_MODULE_NAME         core
#define CORE_MODULE_NAME_STR     IB_XSTRINGIFY(CORE_MODULE_NAME)

/* Static module declarations */
ib_module_t *ib_core_module(void);

/**
 * Core configuration.
 */
typedef struct ib_core_cfg_t ib_core_cfg_t;
struct ib_core_cfg_t {
    /** Provider instances */
    struct {
        ib_provider_inst_t *logger;     /**< Log provider instance */
        ib_provider_inst_t *parser;     /**< Parser provider instance */
    } pi;

    /** Providers (instance is per-transaction) */
    struct {
        ib_provider_t   *data;          /**< Core data provider */
        ib_provider_t   *audit;         /**< Audit log provider */
        ib_provider_t   *logevent;      /**< Logevent provider */
    } pr;

    ib_num_t         log_level;         /**< Log level */
    const char      *log_uri;           /**< Log URI */
    const char      *log_handler;       /**< Active logger provider key */
    const char      *logevent;          /**< Active logevent provider key */
    ib_num_t         buffer_req;        /**< Request buffering options */
    ib_num_t         buffer_res;        /**< Response buffering options */
    ib_num_t         audit_engine;      /**< Audit engine status */
    ib_num_t         auditlog_dmode;    /**< Audit log dir create mode */
    ib_num_t         auditlog_fmode;    /**< Audit log file create mode */
    ib_num_t         auditlog_parts;    /**< Audit log parts */
    const char      *auditlog_index_fmt;/**< Audit log index format string */
    const ib_logformat_t *auditlog_index_hp; /**< Audit log index fmt helper */
    const char      *auditlog_dir;      /**< Audit log base directory */
    const char      *auditlog_sdir_fmt; /**< Audit log sub-directory format */
    const char      *audit;             /**< Active audit provider key */
    const char      *parser;            /**< Active parser provider key */
    const char      *data;              /**< Active data provider key */
    const char      *module_base_path;  /**< Module base path. */
    const char      *rule_base_path;    /**< Rule base path. */
    ib_rule_log_mode_t rule_log_mode;   /**< Rule execution logging mode */
    ib_flags_t       rule_log_flags;    /**< Rule execution logging flags */
    ib_rule_log_level_t rule_log_level; /**< Rule debug logging level */
    ib_num_t         block_status;      /**< Status codes when blocking. */
};


/**
 * @} IronBeeCore
 */

#ifdef __cplusplus
}
#endif

#endif /* _IB_CORE_H_ */
