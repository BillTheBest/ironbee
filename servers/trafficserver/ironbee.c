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
 * @brief IronBee &mdash; Apache Traffic Server Plugin
 *
 * @author Nick Kew <nkew@qualys.com>
 */

#include "ironbee_config_auto.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <ts/ts.h>

#include <sys/socket.h>
#include <netdb.h>

// This gets the PRI*64 types
# define __STDC_FORMAT_MACROS 1
# include <inttypes.h>

#include <ironbee/engine.h>
#include <ironbee/config.h>
#include <ironbee/module.h> /* Only needed while config is in here. */
#include <ironbee/provider.h>
#include <ironbee/server.h>
#include <ironbee/state_notify.h>
#include <ironbee/util.h>
#include <ironbee/debug.h>


static void addr2str(const struct sockaddr *addr, char *str, int *port);

#define ADDRSIZE 48 /* what's the longest IPV6 addr ? */

ib_engine_t DLL_LOCAL *ironbee = NULL;
TSTextLogObject ironbee_log;
#define DEFAULT_LOG "ts-ironbee"

typedef enum {
    HDR_OK,
    HDR_ERROR,
    HDR_HTTP_STATUS
} ib_hdr_outcome;
#define IB_HDR_OUTCOME_IS_HTTP(outcome,data) \
    (outcome == HDR_HTTP_STATUS && (data)->status >= 200 && (data)->status < 600)

typedef struct {
    ib_conn_t *iconn;
    /* store the IPs here so we can clean them up and not leak memory */
    char remote_ip[ADDRSIZE];
    char local_ip[ADDRSIZE];
    TSHttpTxn txnp; /* hack: conn data requires txnp to access */
    /* Keep track of whether this is open and has active transactions */
    int txn_count;
    int closing;
    TSMutex mutex;
} ib_ssn_ctx;

typedef struct {
    /* data filtering stuff */
    TSVIO output_vio;
    TSIOBuffer output_buffer;
    TSIOBufferReader output_reader;
    char *buf;
    unsigned int buflen;
} ib_filter_ctx;

#define IBD_REQ IB_SERVER_REQUEST
#define IBD_RESP IB_SERVER_RESPONSE
#define HDRS_IN IB_SERVER_REQUEST
#define HDRS_OUT IB_SERVER_RESPONSE
#define START_RESPONSE 0x04
#define DATA 0

typedef struct hdr_do {
    ib_server_header_action_t action;
    ib_server_direction_t dir;
    char *hdr;
    char *value;
    struct hdr_do *next;
} hdr_do;

typedef struct hdr_list {
    char *hdr;
    char *value;
    struct hdr_list *next;
} hdr_list;

typedef struct {
    const char *ctype;
    const char *redirect;
    const char *authn;
    const char *body;
} error_resp_t;

typedef struct {
    ib_ssn_ctx *ssn;
    ib_tx_t *tx;
    TSHttpTxn txnp;
    ib_filter_ctx in;
    ib_filter_ctx out;
    int state;
    int status;
    hdr_do *hdr_actions;
    hdr_list *err_hdrs;
    char *err_body;    /* this one can't be const */
} ib_txn_ctx;

/* mod_ironbee uses ib_state_notify_conn_data_[in|out]
 * for both headers and data
 */
typedef struct {
    ib_server_direction_t dir;
    const char *word;
    TSReturnCode (*hdr_get)(TSHttpTxn, TSMBuffer *, TSMLoc *);
    ib_status_t (*ib_notify_header)(ib_engine_t*, ib_tx_t*,
                 ib_parsed_header_wrapper_t*);
    ib_status_t (*ib_notify_header_finished)(ib_engine_t*, ib_tx_t*);
    ib_status_t (*ib_notify_body)(ib_engine_t*, ib_tx_t*, ib_txdata_t*);
    ib_status_t (*ib_notify_end)(ib_engine_t*, ib_tx_t*);
} ironbee_direction_t;

static ironbee_direction_t ironbee_direction_req = {
    IBD_REQ,
    "request",
    TSHttpTxnClientReqGet,
    //ib_state_notify_conn_data_in
    ib_state_notify_request_header_data,
    ib_state_notify_request_header_finished,
    ib_state_notify_request_body_data,
    ib_state_notify_request_finished
};
static ironbee_direction_t ironbee_direction_resp = {
    IBD_RESP,
    "response",
    //TSHttpTxnClientRespGet,
    TSHttpTxnServerRespGet,
    //ib_state_notify_conn_data_out
    ib_state_notify_response_header_data,
    ib_state_notify_response_header_finished,
    ib_state_notify_response_body_data,
    ib_state_notify_response_finished
};

typedef struct {
    ironbee_direction_t *ibd;
    ib_filter_ctx *data;
} ibd_ctx;

/**
 * Callback functions for Ironbee to signal to us
 */
static ib_status_t ib_header_callback(ib_tx_t *tx, ib_server_direction_t dir,
                                      ib_server_header_action_t action,
                                      const char *hdr, const char *value,
                                      void *cbdata)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    hdr_do *header;
    /* Logic for whether we're in time for the requested action */
    /* Output headers can change any time before they're sent */
    /* Input headers can only be touched during their read */

    if (ctx->state & HDRS_OUT ||
        (ctx->state & HDRS_IN && dir == IB_SERVER_REQUEST))
        return IB_ENOTIMPL;  /* too late for requested op */

    header = TSmalloc(sizeof(*header));
    header->next = ctx->hdr_actions;
    ctx->hdr_actions = header;
    header->dir = dir;
    /* FIXME: deferring merge support - implementing append instead */
    header->action = action = action == IB_HDR_MERGE ? IB_HDR_APPEND : action;
    header->hdr = TSstrdup(hdr);
    header->value = TSstrdup(value);

    return IB_OK;
}

/**
 * Handler function to generate an error response
 */
static void error_response(TSHttpTxn txnp, ib_txn_ctx *txndata)
{
    const char *reason = TSHttpHdrReasonLookup(txndata->status);
    TSMBuffer bufp;
    TSMLoc hdr_loc, field_loc;
    hdr_list *hdrs;
    TSReturnCode rv;

    if (TSHttpTxnClientRespGet(txnp, &bufp, &hdr_loc) != TS_SUCCESS) {
        TSError("Errordoc: couldn't retrieve client response header");
        TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
        return;
    }
    rv = TSHttpHdrStatusSet(bufp, hdr_loc, txndata->status);
    if (rv != TS_SUCCESS) {
        TSError("ErrorDoc - TSHttpHdrStatusSet");
    }
    rv = TSHttpHdrReasonSet(bufp, hdr_loc, reason, strlen(reason));
    if (rv != TS_SUCCESS) {
        TSError("ErrorDoc - TSHttpHdrReasonSet");
    }

    while (hdrs = txndata->err_hdrs, hdrs != 0) {
        txndata->err_hdrs = hdrs->next;
        rv = TSMimeHdrFieldCreate(bufp, hdr_loc, &field_loc);
        if (rv != TS_SUCCESS) {
            TSError("ErrorDoc - TSMimeHdrFieldCreate");
            goto errordoc_free;
        }
        rv = TSMimeHdrFieldNameSet(bufp, hdr_loc, field_loc,
                                   hdrs->hdr, strlen(hdrs->hdr));
        if (rv != TS_SUCCESS) {
            TSError("ErrorDoc - TSMimeHdrFieldNameSet");
            goto errordoc_free1;
        }
        rv = TSMimeHdrFieldValueStringInsert(bufp, hdr_loc, field_loc, -1,
                                        hdrs->value, strlen(hdrs->value));
        if (rv != TS_SUCCESS) {
            TSError("ErrorDoc - TSMimeHdrFieldValueStringInsert");
            goto errordoc_free1;
        }
        rv = TSMimeHdrFieldAppend(bufp, hdr_loc, field_loc);
        if (rv != TS_SUCCESS) {
            TSError("ErrorDoc - TSMimeHdrFieldAppend");
            goto errordoc_free1;
        }
errordoc_free1:
        rv = TSHandleMLocRelease(bufp, hdr_loc, field_loc);
        if (rv != TS_SUCCESS) {
            TSError("ErrorDoc - TSHandleMLocRelease 1");
            goto errordoc_free;
        }
errordoc_free:
        TSfree(hdrs->hdr);
        TSfree(hdrs->value);
        TSfree(hdrs);
        hdrs = txndata->err_hdrs;
    }
    if (txndata->err_body) {
        /* this will free the body, so copy it first! */
        TSHttpTxnErrorBodySet(txnp, txndata->err_body,
                              strlen(txndata->err_body), NULL);
    }
    rv = TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);
    if (rv != TS_SUCCESS) {
        TSError("ErrorDoc - TSHandleMLocRelease 2");
    }
}

static ib_status_t ib_error_callback(ib_tx_t *tx, int status, void *cbdata)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    TSDebug("ironbee", "ib_error_callback with status=%d", status);
    if (status >= 200 && status < 600) {
        /* We can't return an error after the response has started */
        if (ctx->state & START_RESPONSE) {
            TSDebug("ironbee", "Too late to change status=%d", status);
            return IB_DECLINED;
        }
        /* ironbee wants to return an HTTP status.  We'll oblige */
        /* FIXME: would the semantics work for 1xx?  Do we care? */
        /* No, we don't care unless a use case arises for the proxy
         * to initiate a 1xx response independently of the backend.
         */
        ctx->status = status;
        return IB_OK;
    }
    return IB_ENOTIMPL;
}

static ib_status_t ib_errhdr_callback(ib_tx_t *tx, const char *hdr, const char *val, void *cbdata)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    hdr_list *hdrs;
    /* We can't return an error after the response has started */
    if (ctx->state & START_RESPONSE)
        return IB_DECLINED;
    if (!hdr || !val)
        return IB_EINVAL;
    hdrs = TSmalloc(sizeof(*hdrs));
    hdrs->hdr = TSstrdup(hdr);
    hdrs->value = TSstrdup(val);
    hdrs->next = ctx->err_hdrs;
    ctx->err_hdrs = hdrs;
    return IB_OK;
}

static ib_status_t ib_errdata_callback(ib_tx_t *tx, const char *data, void *cbdata)
{
    ib_txn_ctx *ctx = (ib_txn_ctx *)tx->sctx;
    /* We can't return an error after the response has started */
    if (ctx->state & START_RESPONSE)
        return IB_DECLINED;
    if (!data)
        return IB_EINVAL;
    ctx->err_body = TSstrdup(data);
    return IB_OK;
}

/* Plugin Structure */
ib_server_t DLL_LOCAL ibplugin = {
    IB_SERVER_HEADER_DEFAULTS,
    "ts-ironbee",
    ib_header_callback,
    NULL,
    ib_error_callback,
    NULL,
    ib_errhdr_callback,
    NULL,
    ib_errdata_callback,
    NULL,
};

/**
 * Handle transaction context destroy.
 *
 * Handles TS_EVENT_HTTP_TXN_CLOSE (transaction close) close event from the
 * ATS.
 *
 * @param[in,out] data Transaction context data
 */
static void ib_txn_ctx_destroy(ib_txn_ctx * data)
{
    if (data) {
        hdr_do *x;
        TSDebug("ironbee", "TX DESTROY: conn=>%p tx=%p id=%s txn_count=%d", data->tx->conn, data->tx, data->tx->id, data->ssn->txn_count);
        ib_tx_destroy(data->tx);
        if (data->out.output_buffer) {
            TSIOBufferDestroy(data->out.output_buffer);
            data->out.output_buffer = NULL;
        }
        if (data->in.output_buffer) {
            TSIOBufferDestroy(data->in.output_buffer);
            data->in.output_buffer = NULL;
        }
        while (x=data->hdr_actions, x != NULL) {
            data->hdr_actions = x->next;
            TSfree(x->hdr);
            TSfree(x->value);
            TSfree(x);
        }
        /* Decrement the txn count on the ssn, and destroy ssn if it's closing */
        if (data->ssn) {
            /* If it's closing, the contp and with it the mutex are already gone.
             * Trust TS not to create more TXNs after signalling SSN close!
             */
            if (data->ssn->closing) {
                if (data->ssn->iconn) {
                    TSDebug("ironbee", "ib_txn_ctx_destroy: calling ib_state_notify_conn_closed()");
                    ib_state_notify_conn_closed(ironbee, data->ssn->iconn);
                    TSDebug("ironbee", "CONN DESTROY: conn=%p", data->ssn->iconn);
                    ib_conn_destroy(data->ssn->iconn);
                }
                TSfree(data->ssn);
            }
            else {
                TSMutexLock(data->ssn->mutex);
                --data->ssn->txn_count;
                TSMutexUnlock(data->ssn->mutex);
            }
        }
        TSfree(data);
    }
}

/**
 * Handle session context destroy.
 *
 * Handles TS_EVENT_HTTP_SSN_CLOSE (session close) close event from the
 * ATS.
 *
 * @param[in,out] data session context data
 */
static void ib_ssn_ctx_destroy(ib_ssn_ctx * data)
{
    /* To avoid the risk of sequencing issues with this coming before TXN_CLOSE,
     * we just mark the session as closing, but leave actually closing it
     * for the TXN_CLOSE if there's a TXN
     */
    if (data) {
        TSMutexLock(data->mutex);
        if (data->txn_count == 0) { /* TXN_CLOSE happened already */
            if (data->iconn) {
                TSDebug("ironbee", "ib_ssn_ctx_destroy: calling ib_state_notify_conn_closed()");
                ib_state_notify_conn_closed(ironbee, data->iconn);
                TSDebug("ironbee", "CONN DESTROY: conn=%p", data->iconn);
                ib_conn_destroy(data->iconn);
            }
            TSMutexUnlock(data->mutex);
            TSfree(data);
        }
        else {
            data->closing = 1;
            TSMutexUnlock(data->mutex);
        }
    }
}

/**
 * Process data from ATS.
 *
 * Process data from one of the ATS events.
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] ibd unknown
 */
static void process_data(TSCont contp, ibd_ctx* ibd)
{
    TSVConn output_conn;
    TSIOBuffer buf_test;
    TSVIO input_vio;
    ib_txn_ctx *data;
    int64_t towrite;
    int64_t avail;
    int first_time = 0;
    char *bufp = NULL;

    TSDebug("ironbee", "Entering process_data()");
    /* Get the output (downstream) vconnection where we'll write data to. */

    output_conn = TSTransformOutputVConnGet(contp);

    /* Get the write VIO for the write operation that was performed on
     * ourself. This VIO contains the buffer that we are to read from
     * as well as the continuation we are to call when the buffer is
     * empty. This is the input VIO (the write VIO for the upstream
     * vconnection).
     */
    input_vio = TSVConnWriteVIOGet(contp);

    data = TSContDataGet(contp);
    if (!ibd->data->output_buffer) {
        first_time = 1;

        ibd->data->output_buffer = TSIOBufferCreate();
        ibd->data->output_reader = TSIOBufferReaderAlloc(ibd->data->output_buffer);
        TSDebug("ironbee", "\tWriting %"PRId64" bytes on VConn", TSVIONBytesGet(input_vio));
        ibd->data->output_vio = TSVConnWrite(output_conn, contp, ibd->data->output_reader, INT64_MAX);
    }
    if (ibd->data->buf) {
        /* this is the second call to us, and we have data buffered.
         * Feed buffered data to ironbee
         */
        ib_txdata_t itxdata;
        itxdata.data = (uint8_t *)ibd->data->buf;
        itxdata.dlen = ibd->data->buflen;
        TSDebug("ironbee", "process_data: calling ib_state_notify_%s_body() %s:%d", ((ibd->ibd->dir == IBD_REQ)?"request":"response"), __FILE__, __LINE__);
        (*ibd->ibd->ib_notify_body)(ironbee, data->tx, &itxdata);
        TSfree(ibd->data->buf);
        ibd->data->buf = NULL;
        ibd->data->buflen = 0;
    }

    /* test for input data */
    buf_test = TSVIOBufferGet(input_vio);

    if (!buf_test) {
        TSDebug("ironbee", "No more data, finishing");
        TSVIONBytesSet(ibd->data->output_vio, TSVIONDoneGet(input_vio));
        TSVIOReenable(ibd->data->output_vio);
        ibd->data->output_buffer = NULL;
        ibd->data->output_reader = NULL;
        ibd->data->output_vio = NULL;
        return;
    }

    /* Determine how much data we have left to read. For this null
     * transform plugin this is also the amount of data we have left
     * to write to the output connection.
     */
    towrite = TSVIONTodoGet(input_vio);
    TSDebug("ironbee", "\ttoWrite is %" PRId64 "", towrite);

    if (towrite > 0) {
        /* The amount of data left to read needs to be truncated by
         * the amount of data actually in the read buffer.
         */

        avail = TSIOBufferReaderAvail(TSVIOReaderGet(input_vio));
        TSDebug("ironbee", "\tavail is %" PRId64 "", avail);
        if (towrite > avail) {
            towrite = avail;
        }

        if (towrite > 0) {
            int btowrite = towrite;
            /* Copy the data from the read buffer to the output buffer. */
            TSIOBufferCopy(TSVIOBufferGet(ibd->data->output_vio), TSVIOReaderGet(input_vio), towrite, 0);

            /* first time through, we have to buffer the data until
             * after the headers have been sent.  Ugh!
             * At this point, we know the size to alloc.
             */
            if (first_time) {
                bufp = ibd->data->buf = TSmalloc(towrite);
                ibd->data->buflen = towrite;
            }

            /* feed the data to ironbee, and consume them */
            while (btowrite > 0) {
                //ib_conndata_t icdata;
                int64_t ilength;
                TSIOBufferReader input_reader = TSVIOReaderGet(input_vio);
                TSIOBufferBlock blkp = TSIOBufferReaderStart(input_reader);
                const char *ibuf = TSIOBufferBlockReadStart(blkp, input_reader, &ilength);

                /* feed it to ironbee or to buffer */
                if (first_time) {
                    memcpy(bufp, ibuf, ilength);
                    bufp += ilength;
                }
                else {
                    ib_txdata_t itxdata;
                    itxdata.data = (uint8_t *)ibd->data->buf;
                    itxdata.dlen = ibd->data->buflen;
                    TSDebug("ironbee", "process_data: calling ib_state_notify_%s_body() %s:%d", ((ibd->ibd->dir == IBD_REQ)?"request":"response"), __FILE__, __LINE__);
                    (*ibd->ibd->ib_notify_body)(ironbee, data->tx,
                                                (ilength!=0) ? &itxdata : NULL);
                }

                /* and mark it as all consumed */
                btowrite -= ilength;
                TSIOBufferReaderConsume(input_reader, ilength);
                TSVIONDoneSet(input_vio, TSVIONDoneGet(input_vio) + ilength);
            }
        }
    }

    /* Now we check the input VIO to see if there is data left to
     * read.
     */
    if (TSVIONTodoGet(input_vio) > 0) {
        if (towrite > 0) {
            /* If there is data left to read, then we re-enable the output
             * connection by re-enabling the output VIO. This will wake up
             * the output connection and allow it to consume data from the
             * output buffer.
             */
            TSVIOReenable(ibd->data->output_vio);

            /* Call back the input VIO continuation to let it know that we
             * are ready for more data.
             */
            TSContCall(TSVIOContGet(input_vio), TS_EVENT_VCONN_WRITE_READY, input_vio);
        }
    }
    else {
        /* If there is no data left to read, then we modify the output
         * VIO to reflect how much data the output connection should
         * expect. This allows the output connection to know when it
         * is done reading. We then re-enable the output connection so
         * that it can consume the data we just gave it.
         */
        TSVIONBytesSet(ibd->data->output_vio, TSVIONDoneGet(input_vio));
        TSVIOReenable(ibd->data->output_vio);

        /* Call back the input VIO continuation to let it know that we
         * have completed the write operation.
         */
        TSContCall(TSVIOContGet(input_vio), TS_EVENT_VCONN_WRITE_COMPLETE, input_vio);
    }
}

/**
 * Handle a data event from ATS.
 *
 * Handles all data events from ATS, uses process_data to handle the data
 * itself.
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] event Event from ATS
 * @param[in,out] ibd unknown
 *
 * @returns status
 */
static int data_event(TSCont contp, TSEvent event, ibd_ctx *ibd)
{
    /* Check to see if the transformation has been closed by a call to
     * TSVConnClose.
     */
    ib_txn_ctx *data;
    TSDebug("ironbee", "Entering out_data for %s\n", ibd->ibd->word);

    if (TSVConnClosedGet(contp)) {
        TSDebug("ironbee", "\tVConn is closed");
        TSContDestroy(contp);    /* from null-transform, ???? */


        return 0;
    }
    switch (event) {
        case TS_EVENT_ERROR:
        {
            TSVIO input_vio;

            TSDebug("ironbee", "\tEvent is TS_EVENT_ERROR");
            /* Get the write VIO for the write operation that was
             * performed on ourself. This VIO contains the continuation of
             * our parent transformation. This is the input VIO.
             */
            input_vio = TSVConnWriteVIOGet(contp);

            /* Call back the write VIO continuation to let it know that we
             * have completed the write operation.
             */
            TSContCall(TSVIOContGet(input_vio), TS_EVENT_ERROR, input_vio);
        }
        break;
        case TS_EVENT_VCONN_WRITE_COMPLETE:
            TSDebug("ironbee", "\tEvent is TS_EVENT_VCONN_WRITE_COMPLETE");
            /* When our output connection says that it has finished
             * reading all the data we've written to it then we should
             * shutdown the write portion of its connection to
             * indicate that we don't want to hear about it anymore.
             */
            TSVConnShutdown(TSTransformOutputVConnGet(contp), 0, 1);

            data = TSContDataGet(contp);
            TSDebug("ironbee", "data_event: calling ib_state_notify_%s_finished()", ((ibd->ibd->dir == IBD_REQ)?"request":"response"));
            (*ibd->ibd->ib_notify_end)(ironbee, data->tx);
            break;
        case TS_EVENT_VCONN_WRITE_READY:
            TSDebug("ironbee", "\tEvent is TS_EVENT_VCONN_WRITE_READY");
            /* fall through */
        default:
            TSDebug("ironbee", "\t(event is %d)", event);
            /* If we get a WRITE_READY event or any other type of
             * event (sent, perhaps, because we were re-enabled) then
             * we'll attempt to transform more data.
             */
            process_data(contp, ibd);
            break;
    }

    return 0;
}

/**
 * Handle a outgoing data event from ATS.
 *
 * Handles all outgoing data events from ATS, uses process_data to handle the
 * data itself.
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] event Event from ATS
 * @param[in,out] edata Event data
 *
 * @returns status
 */
static int out_data_event(TSCont contp, TSEvent event, void *edata)
{
    ib_txn_ctx *data = TSContDataGet(contp);
    if (data->out.buflen == (unsigned int)-1) {
    TSDebug("ironbee", "\tout_data_event: buflen = -1");
    //ib_log_debug(ironbee, 9,
                     //"ironbee/out_data_event(): buflen = -1");
    return 0;
    }
    ibd_ctx direction;
    direction.ibd = &ironbee_direction_resp;
    direction.data = &data->out;
    return data_event(contp, event, &direction);
}

/**
 * Handle a incoming data event from ATS.
 *
 * Handles all incoming data events from ATS, uses process_data to handle the
 * data itself.
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] event Event from ATS
 * @param[in,out] edata Event data
 *
 * @returns status
 */
static int in_data_event(TSCont contp, TSEvent event, void *edata)
{
    ib_txn_ctx *data = TSContDataGet(contp);
    if (data->out.buflen == (unsigned int)-1) {
    TSDebug("ironbee", "\tin_data_event: buflen = -1");
    //ib_log_debug(ironbee, 9,
                 //"ironbee/in_data_event(): buflen = -1");
    return 0;
    }
    ibd_ctx direction;
    direction.ibd = &ironbee_direction_req;
    direction.data = &data->in;
    return data_event(contp, event, &direction);
}
/**
 * Parse lines in an HTTP header buffer
 *
 * Given a buffer including "\r\n" linends, this finds the next line and its
 * length.  Where a line is wrapped, continuation lines are included in
 * in the (multi-)line parsed.
 *
 * Can now also error-correct for "\r" or "\n" line ends.
 *
 * @param[in] line Buffer to parse
 * @param[out] lenp Line length (excluding line end)
 * @return 1 if a line was parsed, 2 if parsed but with error correction,
 *         0 for a blank line (no more headers), -1 for irrecoverable error
 */
static int next_line(const char **linep, size_t *lenp)
{
    int rv = 1;
    size_t lelen = 2;
    size_t len = 0;
    const char *end;
    const char *line = *linep;
    if (line[0] == '\r' && line[1] == '\n') {
        return 0; /* blank line = no more hdr lines */
    }
    else if (line[0] == '\r' || line[0] == '\n') {
        return 0; /* blank line which is also malformed HTTP */
    }
    else {
        /* skip to next start-of-line from where we are */
        line += strcspn(line, "\r\n");
        if (line[0] == '\r' && line[1] == '\n') {
            /* valid line end.  Set pointer to start of next line */
            line += 2;
        }
        else {   /* bogus lineend!
                  * Treat a single '\r' or '\n' as a lineend
                  */
            line += 1;
            rv = 2; /* bogus linend */
        }
        if (line[0] == '\r' && line[1] == '\n') {
            return 0; /* blank line = no more hdr lines */
        }
        else if (line[0] == '\r' || line[0] == '\n') {
            return 0; /* blank line which is also malformed HTTP */
        }
        /* Use a loop here to catch theoretically-unlimited numbers
         * of continuation lines in a folded header.  The isspace
         * tests for a continuation line
         */
        do {
            if (len > 0) {
                /* we have a continuation line.  Add the lineend. */
                len += lelen;
            }
            end = strstr(line + len, "\r\n");
            if (end) {  /* All's well, this is a good line */
                lelen = 2;
            }
            else {
                /* Malformed header.  Check for a bogus single-char lineend */
                end = line + strcspn(line + len, "\r\n");
                if (end > line) {
                    lelen = 1;
                    rv = 2;
                }
                else { /* nothing at all we can interpret as lineend */
                    return -1;
                }
            }
            len = end - line;
        } while (isspace(end[lelen]) && end[lelen] != '\r' && end[lelen] != '\n');

        *lenp = len;
        *linep = line;
        return rv;
    }
}

/**
 * Process an HTTP header from ATS.
 *
 * Handles an HTTP header, called from ironbee_plugin.
 *
 * @param[in,out] data Transaction context
 * @param[in,out] txnp ATS transaction pointer
 * @param[in,out] ibd unknown
 * @return OK (nothing to tell), Error (something bad happened),
 *         HTTP_STATUS (check data->status).
 */
static ib_hdr_outcome process_hdr(ib_txn_ctx *data, TSHttpTxn txnp,
                                  ironbee_direction_t *ibd)
{
    int rv;
    TSMBuffer bufp;
    TSMLoc hdr_loc;
    TSIOBuffer iobufp;
    TSIOBufferReader readerp;
    TSIOBufferBlock blockp;
    int64_t len;
    hdr_do *hdr;
    const char *line, *lptr;
    size_t line_len = 0;

    char *head_buf;
    unsigned char *dptr, *icdatabuf;

    ib_parsed_header_wrapper_t *ibhdrs;
    size_t n_len, v_len;

    TSDebug("ironbee", "process %s headers\n", ibd->word);

    /* Use alternative simpler path to get the un-doctored request
     * if we have the fix for TS-998
     *
     * This check will want expanding/fine-tuning according to what released
     * versions incorporate the fix
     */
    /* We'll get a bogus URL from TS-998 */

    rv = (*ibd->hdr_get)(txnp, &bufp, &hdr_loc);
    if (rv) {
        TSError ("couldn't retrieve %s header: %d\n", ibd->word, rv);
        return HDR_ERROR;
    }
    if (ibd->dir == IBD_REQ) {
        ib_parsed_req_line_t *rline;
        int m_len;
        int64_t u_len;
        const char *ubuf;
        TSMLoc url_loc;
        char cversion[9];
        const char *method = TSHttpHdrMethodGet(bufp, hdr_loc, &m_len);
        int version = TSHttpHdrVersionGet(bufp, hdr_loc);
        /* sanity-check against buffer overflow */
        int major = TS_HTTP_MAJOR(version);
        int minor = TS_HTTP_MINOR(version);
        if (major < 0 || major > 9 || minor < 0 || minor > 9) {
           TSError("Bogus HTTP version: %d.%d", major, minor);
           major = minor = 0;
        }
        sprintf(cversion, "HTTP/%d.%d", major, minor);
        rv = TSHttpHdrUrlGet(bufp, hdr_loc, &url_loc);
        iobufp = TSIOBufferCreate();
        TSUrlPrint(bufp, url_loc, iobufp);

        readerp = TSIOBufferReaderAlloc(iobufp);
        blockp = TSIOBufferReaderStart(readerp);

        TSIOBufferBlockReadAvail(blockp, readerp);
        ubuf = TSIOBufferBlockReadStart(blockp, readerp, &u_len);


        /* Drop crap from the front of the buf.
         * We can't consume it, because we don't have the length of the
         * rest of the request line, so leave that for when we consume
         * the headers below.
         */
        while (isspace(*ubuf)) {
            ++ubuf;
            --u_len;
        }
        if (u_len >= 8 && ubuf[0] == 'h' && ubuf[1] == 't'
            && ubuf[2] == 't' && ubuf[3] == 'p') {
            if (ubuf[4] == ':' && ubuf[5] == '/' && ubuf[6] == '/') {
                ubuf += 7;
                u_len -= 7;
            }
            else if (ubuf[4] &&
                     ubuf[5] == ':' && ubuf[6] == '/' && ubuf[7] == '/') {
                ubuf += 8;
                u_len -= 8;
            }
        }

        rv = ib_parsed_req_line_create(data->tx, &rline,
                                       NULL, 0,
                                       method, m_len,
                                       ubuf, u_len,
                                       cversion, strlen(cversion));
        TSDebug("ironbee", "process_hdr: calling ib_state_notify_request_started()");
        ib_state_notify_request_started(ironbee, data->tx, rline);

        TSIOBufferReaderFree(readerp);
        TSIOBufferDestroy(iobufp);
    }
    else {
        ib_parsed_resp_line_t *rline;
        char cversion[9], cstatus[4];
        const char *reason;
        TSHttpStatus status;
        int r_len;
        int version = TSHttpHdrVersionGet(bufp, hdr_loc);
        /* sanity-check against buffer overflow */
        int major = TS_HTTP_MAJOR(version);
        int minor = TS_HTTP_MINOR(version);
        if (major < 0 || major > 9 || minor < 0 || minor > 9) {
           TSError("Bogus HTTP version: %d.%d", major, minor);
           major = minor = 0;
        }
        sprintf(cversion, "HTTP/%d.%d", major, minor);

        status = TSHttpHdrStatusGet(bufp, hdr_loc);
        /* status is an enum.  Do a very minimal sanity check */
        if ((int)status < 0 || status >= 600) {
           TSError("Bogus HTTP status: %d", status);
           status = 0;
        }
        sprintf(cstatus, "%d", status);

        reason = TSHttpHdrReasonGet(bufp, hdr_loc, &r_len);

        ib_log_debug_tx(data->tx, "RESP_LINE: %s %d %s",
                        cversion, status, reason);

        rv = ib_parsed_resp_line_create(data->tx, &rline,
                                        NULL, 0,
                                        cversion, strlen(cversion),
                                        cstatus, strlen(cstatus),
                                        reason, r_len);
        TSDebug("ironbee", "process_hdr: calling ib_state_notify_response_started()");
        ib_log_debug_tx(data->tx, "ib_state_notify_response_started rline=%p", rline);
        rv = ib_state_notify_response_started(ironbee, data->tx, rline);

    }

    /* Get the data into an IOBuffer so we can access them! */
    //iobufp = TSIOBufferSizedCreate(...);
    iobufp = TSIOBufferCreate();
    TSHttpHdrPrint(bufp, hdr_loc, iobufp);

    readerp = TSIOBufferReaderAlloc(iobufp);
    blockp = TSIOBufferReaderStart(readerp);

    len = TSIOBufferBlockReadAvail(blockp, readerp);
    //ib_log_debug(ironbee, 9,
                 //"ts/ironbee/process_header: len=%ld", len );

    /* if we're going to enable manipulation of headers, we need a copy */
    icdatabuf = dptr = TSmalloc(len);

    for (head_buf = (void *)TSIOBufferBlockReadStart(blockp, readerp, &len);
         len > 0;
         head_buf = (void *)TSIOBufferBlockReadStart(
                        TSIOBufferReaderStart(readerp), readerp, &len)) {

        memcpy(dptr, head_buf, len);
        dptr += len;

        /* if there's more to come, go round again ... */
        TSIOBufferReaderConsume(readerp, len);
    }

    /* parse into lines and feed to ironbee as parsed data */

    /* now loop over header lines */
    /* The buffer contains the Request line / Status line, together with
     * the actual headers.  So we'll skip the first line, which we already
     * dealt with.
     *
     */
    rv = ib_parsed_name_value_pair_list_wrapper_create(&ibhdrs, data->tx);
    // get_line ensures CRLF (line_len + 2)?
    line = (const char*) icdatabuf;
    while (next_line(&line, &line_len) > 0) {
        n_len = strcspn(line, ":");
        lptr = line + n_len + 1;
        while (isspace(*lptr) && lptr < line + line_len)
            ++lptr;
        v_len = line_len - (lptr - line);
        /* Ironbee presumably wants to know of anything zero-length
         * so don't reject on those grounds!
         */
        rv = ib_parsed_name_value_pair_list_add(ibhdrs,
                                                line, n_len,
                                                lptr, v_len);
    }

    TSDebug("ironbee", "process_hdr: notifying header data");
    rv = (*ibd->ib_notify_header)(ironbee, data->tx, ibhdrs);
    TSDebug("ironbee", "process_hdr: notifying header finished");
    rv = (*ibd->ib_notify_header_finished)(ironbee, data->tx);


    /* Now manipulate header as requested by ironbee */
    for (hdr = data->hdr_actions; hdr != NULL; hdr = hdr->next) {
        TSMLoc field_loc;
        if (hdr->dir != ibd->dir)
            continue;    /* it's not for us */

        TSDebug("ironbee", "Manipulating HTTP headers");

        switch (hdr->action) {
            case IB_HDR_SET:  /* replace any previous instance == unset + add */
            case IB_HDR_UNSET:  /* unset it */
                TSDebug("ironbee", "Remove HTTP Header %s", hdr->hdr);
                /* Use a while loop in case there are multiple instances */
                while (field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, hdr->hdr,
                                                      strlen(hdr->hdr)),
                           field_loc != TS_NULL_MLOC) {
                    TSMimeHdrFieldDestroy(bufp, hdr_loc, field_loc);
                    TSHandleMLocRelease(bufp, hdr_loc, field_loc);
                }
                if (hdr->action == IB_HDR_UNSET)
                    break;
                /* else fallthrough to ADD */
            case IB_HDR_ADD:  /* add it in, regardless of whether it exists */
add_hdr:
                TSDebug("ironbee", "Add HTTP Header %s=%s", hdr->hdr, hdr->value);
                rv = TSMimeHdrFieldCreate(bufp, hdr_loc, &field_loc);
                if (rv != TS_SUCCESS)
                     TSError("Failed to add MIME header field");
                rv = TSMimeHdrFieldNameSet(bufp, hdr_loc, field_loc,
                                           hdr->hdr, strlen(hdr->hdr));
                rv = TSMimeHdrFieldValueStringSet(bufp, hdr_loc, field_loc, -1,
                                                  hdr->value, strlen(hdr->value));
                rv = TSMimeHdrFieldAppend(bufp, hdr_loc, field_loc);
                TSHandleMLocRelease(bufp, hdr_loc, field_loc);
                break;
            case IB_HDR_MERGE:  /* append UNLESS value already appears */
                /* FIXME: implement this in full */
                /* treat this as APPEND */
            case IB_HDR_APPEND: /* append it to any existing instance */
                TSDebug("ironbee", "Merge/Append HTTP Header %s=%s", hdr->hdr, hdr->value);
                field_loc = TSMimeHdrFieldFind(bufp, hdr_loc, hdr->hdr,
                                               strlen(hdr->hdr));
                if (field_loc == TS_NULL_MLOC) {
                    /* this is identical to IB_HDR_ADD */
                    goto add_hdr;
                }
                /* This header exists, so append to it
                 * (the function is called Insert but actually appends
                 */
                rv = TSMimeHdrFieldValueStringInsert(bufp, hdr_loc, field_loc, -1,
                                               hdr->value, strlen(hdr->value));
                TSHandleMLocRelease(bufp, hdr_loc, field_loc);
                break;
            default:  /* bug !! */
                TSDebug("ironbee", "Bogus header action %d", hdr->action);
                break;
        }
    }

    TSHandleMLocRelease(bufp, TS_NULL_MLOC, hdr_loc);
    TSIOBufferReaderFree(readerp);
    TSIOBufferDestroy(iobufp);
    TSfree(icdatabuf);

    return data->status == 0 ? HDR_OK : HDR_HTTP_STATUS;
}

/**
 * Plugin for the IronBee ATS.
 *
 * Handles some ATS events.
 *
 * @param[in,out] contp Pointer to the continuation
 * @param[in,out] event Event from ATS
 * @param[in,out] edata Event data
 *
 * @returns status
 */
static int ironbee_plugin(TSCont contp, TSEvent event, void *edata)
{
    TSVConn connp;
    TSCont mycont;
    TSMutex conn_mutex;
    TSHttpTxn txnp = (TSHttpTxn) edata;
    TSHttpSsn ssnp = (TSHttpSsn) edata;
    ib_txn_ctx *txndata;
    ib_ssn_ctx *ssndata;
    ib_hdr_outcome status;

    TSDebug("ironbee", "Entering ironbee_plugin with %d", event);
    switch (event) {

        /* CONNECTION */
        case TS_EVENT_HTTP_SSN_START:
            /* start of connection */
            /* But we can't initialize conn stuff here, because there's
             * no API to get the connection stuff required by ironbee
             * at this point.  So instead, intercept the first TXN
             *
             * what we can and must do: create a new contp whose
             * lifetime is our ssn
             */
            conn_mutex = TSMutexCreate();
            mycont = TSContCreate(ironbee_plugin, conn_mutex);
            TSHttpSsnHookAdd (ssnp, TS_HTTP_TXN_START_HOOK, mycont);
            ssndata = TSmalloc(sizeof(*ssndata));
            memset(ssndata, 0, sizeof(*ssndata));
            ssndata->mutex = conn_mutex;
            TSContDataSet(mycont, ssndata);

            TSHttpSsnHookAdd (ssnp, TS_HTTP_SSN_CLOSE_HOOK, mycont);

            TSHttpSsnReenable (ssnp, TS_EVENT_HTTP_CONTINUE);
            break;
        case TS_EVENT_HTTP_TXN_START:
            /* start of Request */
            /* First req on a connection, we set up conn stuff */

            ssndata = TSContDataGet(contp);
            TSMutexLock(ssndata->mutex);
            if (ssndata->iconn == NULL) {
                ib_status_t rc;
                rc = ib_conn_create(ironbee, &ssndata->iconn, contp);
                if (rc != IB_OK) {
                    TSError("ironbee: ib_conn_create: %d\n", rc);
                    return rc; // FIXME - figure out what to do
                }
                TSDebug("ironbee", "CONN CREATE: conn=%p", ssndata->iconn);
                ssndata->txnp = txnp;
                ssndata->txn_count = ssndata->closing = 0;
                TSContDataSet(contp, ssndata);
                TSDebug("ironbee", "ironbee_plugin: calling ib_state_notify_conn_opened()");
                ib_state_notify_conn_opened(ironbee, ssndata->iconn);
            }
            ++ssndata->txn_count;
            TSMutexUnlock(ssndata->mutex);

            /* create a txn cont (request ctx) */
            mycont = TSContCreate(ironbee_plugin, TSMutexCreate());
            txndata = TSmalloc(sizeof(*txndata));
            memset(txndata, 0, sizeof(*txndata));
            txndata->ssn = ssndata;
            txndata->txnp = txnp;
            TSContDataSet(mycont, txndata);

            /* With both of these, SSN_CLOSE gets called first.
             * I must be misunderstanding SSN
             * So hook it all to TXN
             */
            TSHttpTxnHookAdd(txnp, TS_HTTP_TXN_CLOSE_HOOK, mycont);

            /* Hook to process responses */
            TSHttpTxnHookAdd(txnp, TS_HTTP_READ_RESPONSE_HDR_HOOK, mycont);

            /* Hook to process requests */
            TSHttpTxnHookAdd(txnp, TS_HTTP_READ_REQUEST_HDR_HOOK, mycont);

            ib_tx_create(&txndata->tx, ssndata->iconn, txndata);
            TSDebug("ironbee", "TX CREATE: conn=%p tx=%p id=%s txn_count=%d", ssndata->iconn, txndata->tx, txndata->tx->id, txndata->ssn->txn_count);

            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;

        /* HTTP RESPONSE */
        case TS_EVENT_HTTP_READ_RESPONSE_HDR:
            txndata = TSContDataGet(contp);

            /* Feed ironbee the headers if not done alread. */
            if (!ib_tx_flags_isset(txndata->tx, IB_TX_FRES_STARTED)) {
                status = process_hdr(txndata, txnp, &ironbee_direction_resp);
                // FIXME: Need to know if this fails as it (I think) means
                //        that the response did not come from the server and
                //        that ironbee should ignore it.
                /* I've not seen a fail here.  AFAICT if either the origin
                 * isn't responding or we're responding from cache. we
                 * never reach here in the first place.
                 */
                if (ib_tx_flags_isset(txndata->tx, IB_TX_FRES_SEENHEADER)) {
                    txndata->state |= HDRS_OUT;
                }
            }

            /* hook to examine output headers */
            // FIXME: I think this is now only used for error_response
            /* Not sure why we can't do it right now, but it seems headers
             * are not yet available.
             * Can we use another case switch in this function?
             */
            //TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, contp);

            /* hook an output filter to watch data */
            connp = TSTransformCreate(out_data_event, txnp);
            TSContDataSet(connp, txndata);
            TSHttpTxnHookAdd(txnp, TS_HTTP_RESPONSE_TRANSFORM_HOOK, connp);

            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;

        /* hook for processing response headers */
        /* If ironbee has sent us into an error response then
         * we came here in our error path, with nonzero status
         * FIXME: tests
         */
        case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
            txndata = TSContDataGet(contp);

            if (txndata->status != 0) {
                error_response(txnp, txndata);
            }

            txndata->state |= START_RESPONSE;

            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;

            /* HTTP REQUEST */
        case TS_EVENT_HTTP_READ_REQUEST_HDR:
            txndata = TSContDataGet(contp);

            /* hook to examine output headers */
            /* Not sure why we can't do it right now, but it seems headers
             * are not yet available.
             * Can we use another case switch in this function?
             */
            //TSHttpTxnHookAdd(txnp, TS_HTTP_OS_DNS_HOOK, contp);
            TSHttpTxnHookAdd(txnp, TS_HTTP_PRE_REMAP_HOOK, contp);

            /* hook an input filter to watch data */
            connp = TSTransformCreate(in_data_event, txnp);
            TSContDataSet(connp, txndata);
            TSHttpTxnHookAdd(txnp, TS_HTTP_REQUEST_TRANSFORM_HOOK, connp);

            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;

            /* hook for processing incoming request/headers */
        case TS_EVENT_HTTP_PRE_REMAP:
        case TS_EVENT_HTTP_OS_DNS:
            txndata = TSContDataGet(contp);
            status = process_hdr(txndata, txnp, &ironbee_direction_req);
            txndata->state |= HDRS_IN;
            if (IB_HDR_OUTCOME_IS_HTTP(status, txndata)) {
                TSDebug("ironbee", "HTTP code %d contp=%p", txndata->status, contp);
                TSHttpTxnHookAdd(txnp, TS_HTTP_SEND_RESPONSE_HDR_HOOK, contp);
                TSHttpTxnReenable(txnp, TS_EVENT_HTTP_ERROR);
            }
            else {
                /* Other nonzero statuses not supported */
                if (status != HDR_OK) {
                    TSError("Unhandled state arose in handling request headers");
                }
                TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            }
            break;


            /* CLEANUP EVENTS */
        case TS_EVENT_HTTP_TXN_CLOSE:
            TSDebug("ironbee", "TXN Close: %lx\n", (long unsigned int)contp);
            ib_txn_ctx_destroy(TSContDataGet(contp));
            TSContDataSet(contp, NULL);
            TSContDestroy(contp);
            TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
            break;

        case TS_EVENT_HTTP_SSN_CLOSE:
            TSDebug("ironbee", "SSN Close: %lx\n", (long unsigned int)contp);
            ib_ssn_ctx_destroy(TSContDataGet(contp));
            TSContDestroy(contp);
            TSHttpSsnReenable(ssnp, TS_EVENT_HTTP_CONTINUE);
            break;

            /* if we get here we've got a bug */
        default:
            TSError("BUG: unhandled event %d in ironbee_plugin\n", event);
            break;
    }

    return 0;
}

static int check_ts_version(void)
{

    const char *ts_version = TSTrafficServerVersionGet();
    int result = 0;

    if (ts_version) {
        int major_ts_version = 0;
        int minor_ts_version = 0;
        int patch_ts_version = 0;

        if (sscanf(ts_version, "%d.%d.%d", &major_ts_version, &minor_ts_version, &patch_ts_version) != 3) {
            return 0;
        }

        /* Need at least TS 2.0 */
        if (major_ts_version >= 2) {
            result = 1;
        }

    }

    return result;
}

/**
 * IronBee ATS logger.
 *
 * Performs IronBee logging for the ATS plugin.
 *
 * @param[in] dummy Dummy pointer
 * @param[in] level Debug level
 * @param[in] ib IronBee engine
 * @param[in] file File name
 * @param[in] line Line number
 * @param[in] fmt Format string
 * @param[in] ap Var args list to match the format
 */
static void ironbee_logger(void *dummy,
                           ib_log_level_t level,
                           const ib_engine_t *ib,
                           const char *file,
                           int line,
                           const char *fmt,
                           va_list ap)
{
    char buf[8192 + 1];
    int limit = 7000;
    int ec;
    TSReturnCode rc;
    const char *errmsg = NULL;

    /* Buffer the log line. */
    ec = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (ec >= limit) {
        /* Mark as truncated, with a " ...". */
        memcpy(buf + (limit - 5), " ...", 5);
        errmsg = "Data truncated in log";
    }

    /* Write it to the ironbee log. */
    /* FIXME: why is the format arg's prototype not const char* ? */
    if ((file != NULL) && (line > 0)) {
        rc = TSTextLogObjectWrite(ironbee_log,
                                  (char *)"(%s:%d) %s",
                                  file, line,
                                  buf);
    }
    else {
        rc = TSTextLogObjectWrite(ironbee_log,
                                  (char *)"%s",
                                  buf);
    }

    if (rc != TS_SUCCESS) {
        errmsg = "Data logging failed!";
    }

    if (errmsg != NULL) {
        TSError("[ts-ironbee] %s\n", errmsg);
    }
}

/**
 * Convert an IP address into a string.
 *
 * @param[in,out] addr IP address structure
 * @param[in] str Buffer in which to store the address string
 * @param[in] port Pointer to port number (also filled in)
 */
static void addr2str(const struct sockaddr *addr, char *str, int *port)
{
    char serv[8]; /* port num */
    int rv = getnameinfo(addr, sizeof(*addr), str, ADDRSIZE, serv, 8,
                         NI_NUMERICHOST|NI_NUMERICSERV);
    if (rv != 0) {
        TSError("[ts-ironbee] getnameinfo: %d\n", rv);
    }
    *port = atoi(serv);
}

/**
 * Initialize the IB connection.
 *
 * Initializes an IronBee connection from a ATS continuation
 *
 * @param[in,out] ib IronBee engine
 * @param[in] iconn IB connection
 * @param[in] cbdata Callback data
 *
 * @returns status
 */
static ib_status_t ironbee_conn_init(ib_engine_t *ib,
                                     ib_state_event_type_t event,
                                     ib_conn_t *iconn,
                                     void *cbdata)
{
    /* when does this happen? */
    ib_status_t rc;
    const struct sockaddr *addr;
    int port;

    TSCont contp = iconn->server_ctx;
    ib_ssn_ctx* data = TSContDataGet(contp);
//  ib_clog_debug(....);

    /* remote ip */
    addr = TSHttpTxnClientAddrGet(data->txnp);

    addr2str(addr, data->remote_ip, &port);

    iconn->remote_ipstr = data->remote_ip;
    rc = ib_data_add_bytestr(iconn->dpi,
                             "remote_ip",
                             (uint8_t *)iconn->remote_ipstr,
                             strlen(data->remote_ip),
                             NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* remote port */
    iconn->remote_port = port;
    rc = ib_data_add_num(iconn->dpi, "remote_port", port, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* local end */
    addr = TSHttpTxnIncomingAddrGet(data->txnp);

    addr2str(addr, data->local_ip, &port);

    iconn->local_ipstr = data->local_ip;
    rc = ib_data_add_bytestr(iconn->dpi,
                             "local_ip",
                             (uint8_t *)iconn->local_ipstr,
                             strlen(data->local_ip),
                             NULL);
    if (rc != IB_OK) {
        return rc;
    }

    /* local_port */
    iconn->local_port = port;
    rc = ib_data_add_num(iconn->dpi, "local_port", port, NULL);
    if (rc != IB_OK) {
        return rc;
    }
    return IB_OK;
}

static IB_PROVIDER_IFACE_TYPE(logger) ironbee_logger_iface = {
    IB_PROVIDER_IFACE_HEADER_DEFAULTS,
    ironbee_logger
};


/* this can presumably be global since it's only setup on init */
//static ironbee_config_t ibconfig;
//#define TRACEFILE "/tmp/ironbee-trace"
#define TRACEFILE NULL

/**
 * Handle ATS shutdown for IronBee plugin.
 *
 * Registered via atexit() during initialization, destroys the IB engine,
 * etc.
 *
 */
static void ibexit(void)
{
    TSTextLogObjectDestroy(ironbee_log);
    ib_engine_destroy(ironbee);
}

/**
 * Initialize IronBee for ATS.
 *
 * Performs IB initializations for the ATS plugin.
 *
 * @param[in] configfile Configuration file
 * @param[in] logfile Log file
 *
 * @returns status
 */
static int ironbee_init(const char *configfile, const char *logfile)
{
    /* grab from httpd module's post-config */
    ib_status_t rc;
//  ib_provider_t *lpr;
    ib_cfgparser_t *cp;
    ib_context_t *ctx;
    int rv;

    rc = ib_initialize();
    if (rc != IB_OK) {
        return rc;
    }

    ib_util_log_level(4);

    ib_trace_init(TRACEFILE);

    rc = ib_engine_create(&ironbee, &ibplugin);
    if (rc != IB_OK) {
        return rc;
    }

    rc = ib_provider_register(ironbee, IB_PROVIDER_TYPE_LOGGER, "ironbee-ts",
                              NULL, &ironbee_logger_iface, NULL);
    if (rc != IB_OK) {
        return rc;
    }

    ib_context_set_string(ib_context_engine(ironbee),
                          IB_PROVIDER_TYPE_LOGGER, "ironbee-ts");
    ib_context_set_num(ib_context_engine(ironbee),
                       IB_PROVIDER_TYPE_LOGGER ".log_level", 4);

    rc = ib_engine_init(ironbee);
    if (rc != IB_OK) {
        return rc;
    }

    /* success is documented as TS_LOG_ERROR_NO_ERROR but that's undefined.
     * It's actually a TS_SUCCESS (proxy/InkAPI.cc line 6641).
     */
    rv = TSTextLogObjectCreate(logfile, TS_LOG_MODE_ADD_TIMESTAMP, &ironbee_log);
    if (rv != TS_SUCCESS) {
        return IB_OK + rv;
    }

    rc = atexit(ibexit);
    if (rc != 0) {
        return IB_OK + rv;
    }

    ib_hook_conn_register(ironbee, conn_opened_event,
                          ironbee_conn_init, NULL);


    TSDebug("ironbee", "ironbee_init: calling ib_state_notify_cfg_started()");
    ib_state_notify_cfg_started(ironbee);
    ctx = ib_context_main(ironbee);

    ib_context_set_string(ctx, IB_PROVIDER_TYPE_LOGGER, "ironbee-ts");
    ib_context_set_num(ctx, "logger.log_level", 4);

    rc = ib_cfgparser_create(&cp, ironbee);
    if (rc != IB_OK) {
        return rc;
    }
    if (cp != NULL) {  // huh?
        ib_cfgparser_parse(cp, configfile);
        ib_cfgparser_destroy(cp);
    }
    TSDebug("ironbee", "ironbee_init: calling ib_state_notify_cfg_finished()");
    ib_state_notify_cfg_finished(ironbee);


    return IB_OK;
}

/**
 * Initialize the IronBee ATS plugin.
 *
 * Performs initializations required by ATS.
 *
 * @param[in] argc Command-line argument count
 * @param[in] argv Command-line argument list
 */
void TSPluginInit(int argc, const char *argv[])
{
    int rv;
    TSPluginRegistrationInfo info;
    TSCont cont;

    /* FIXME - check why these are char*, not const char* */
    info.plugin_name = (char *)"ironbee";
    info.vendor_name = (char *)"Qualys, Inc";
    info.support_email = (char *)"ironbee-users@lists.sourceforge.com";

    if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
        TSError("[ironbee] Plugin registration failed.\n");
        goto Lerror;
    }

    if (!check_ts_version()) {
        TSError("[ironbee] Plugin requires Traffic Server 3.0 or later\n");
        goto Lerror;
    }

    cont = TSContCreate(ironbee_plugin, TSMutexCreate());

    /* connection initialization & cleanup */
    TSHttpHookAdd(TS_HTTP_SSN_START_HOOK, cont);


    if (argc < 2) {
        TSError("[ironbee] configuration file name required\n");
        goto Lerror;
    }
    rv = ironbee_init(argv[1], argc >= 3 ? argv[2] : DEFAULT_LOG);
    if (rv != IB_OK) {
        TSError("[ironbee] initialization failed with %d\n", rv);
    }
    return;

Lerror:
    TSError("[ironbee] Unable to initialize plugin (disabled).\n");
}
