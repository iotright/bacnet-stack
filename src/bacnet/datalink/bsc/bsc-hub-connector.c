/**
 * @file
 * @brief BACNet hub connector API.
 * @author Kirill Neznamov
 * @date July 2022
 * @section LICENSE
 *
 * Copyright (C) 2022 Legrand North America, LLC
 * as an unpublished work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later WITH GCC-exception-2.0
 */

#include "bacnet/basic/sys/debug.h"
#include "bacnet/datalink/bsc/bvlc-sc.h"
#include "bacnet/datalink/bsc/bsc-socket.h"
#include "bacnet/datalink/bsc/bsc-util.h"
#include "bacnet/datalink/bsc/bsc-mutex.h"
#include "bacnet/datalink/bsc/bsc-runloop.h"
#include "bacnet/datalink/bsc/bsc-hub-connector.h"
#include "bacnet/bacdef.h"
#include "bacnet/npdu.h"
#include "bacnet/bacenum.h"

typedef enum {
    BSC_HUB_CONN_PRIMARY = 0,
    BSC_HUB_CONN_FAILOVER = 1
} BSC_HUB_CONN_TYPE;

static BSC_SOCKET *hub_connector_find_connection_for_vmac(
    BACNET_SC_VMAC_ADDRESS *vmac);
static BSC_SOCKET *hub_connector_find_connection_for_uuid(BACNET_SC_UUID *uuid);
static void hub_connector_socket_event(BSC_SOCKET *c,
    BSC_SOCKET_EVENT ev,
    BSC_SC_RET err,
    uint8_t *pdu,
    uint16_t pdu_len,
    BVLC_SC_DECODED_MESSAGE *decoded_pdu);
static void hub_connector_context_event(BSC_SOCKET_CTX *ctx, BSC_CTX_EVENT ev);

typedef enum {
    BSC_HUB_CONNECTOR_STATE_IDLE = 0,
    BSC_HUB_CONNECTOR_STATE_CONNECTING_PRIMARY = 1,
    BSC_HUB_CONNECTOR_STATE_CONNECTING_FAILOVER = 2,
    BSC_HUB_CONNECTOR_STATE_CONNECTED_PRIMARY = 3,
    BSC_HUB_CONNECTOR_STATE_CONNECTED_FAILOVER = 4,
    BSC_HUB_CONNECTOR_STATE_WAIT_FOR_RECONNECT = 5,
    BSC_HUB_CONNECTOR_STATE_WAIT_FOR_CTX_DEINIT = 6,
    BSC_HUB_CONNECTOR_STATE_ERROR = 7
} BSC_HUB_CONNECTOR_STATE;

typedef struct BSC_Hub_Connector {
    BSC_SOCKET_CTX ctx;
    BSC_CONTEXT_CFG cfg;
    BSC_SOCKET sock[2];
    BSC_HUB_CONNECTOR_STATE state;
    unsigned int reconnect_timeout_s;
    uint8_t primary_url[BSC_WSURL_MAX_LEN + 1];
    uint8_t failover_url[BSC_WSURL_MAX_LEN + 1];
    struct mstimer t;
    HUB_CONNECTOR_EVENT event_func;
    BSC_SC_RET error;
} BSC_HUB_CONNECTOR;

static BSC_HUB_CONNECTOR bsc_hub_connector;
static bool bsc_hub_connector_started = false;

static BSC_SOCKET_CTX_FUNCS bsc_hub_connector_ctx_funcs = {
    hub_connector_find_connection_for_vmac,
    hub_connector_find_connection_for_uuid, hub_connector_socket_event,
    hub_connector_context_event
};

static BSC_SOCKET *hub_connector_find_connection_for_vmac(
    BACNET_SC_VMAC_ADDRESS *vmac)
{
    return NULL;
}

static BSC_SOCKET *hub_connector_find_connection_for_uuid(BACNET_SC_UUID *uuid)
{
    return NULL;
}

static void hub_connector_connect_or_stop(BSC_HUB_CONN_TYPE type)
{
    BSC_SC_RET ret;
    bsc_hub_connector.state = (type == BSC_HUB_CONN_PRIMARY)
        ? BSC_HUB_CONNECTOR_STATE_CONNECTING_PRIMARY
        : BSC_HUB_CONNECTOR_STATE_CONNECTING_FAILOVER;

    ret = bsc_connect(&bsc_hub_connector.ctx, &bsc_hub_connector.sock[type],
        (type == BSC_HUB_CONN_PRIMARY)
            ? (char *)bsc_hub_connector.primary_url
            : (char *)bsc_hub_connector.failover_url);

    if (ret != BSC_SC_SUCCESS) {
        debug_printf("hub_connector_connect_or_stop() got fatal error while "
                     "connecting to hub type %d, err = %d\n",
            type, ret);
        bsc_hub_connector.state = BSC_HUB_CONNECTOR_STATE_ERROR;
        bsc_hub_connector.error = ret;
        bsc_hub_connector_stop();
    }
}

static void hub_connector_process_state(void *ctx)
{
    (void)ctx;
    if (bsc_hub_connector.state == BSC_HUB_CONNECTOR_STATE_WAIT_FOR_RECONNECT) {
        if (mstimer_expired(&bsc_hub_connector.t)) {
            hub_connector_connect_or_stop(BSC_HUB_CONN_PRIMARY);
        }
    }
}

static void hub_connector_socket_event(BSC_SOCKET *c,
    BSC_SOCKET_EVENT ev,
    BSC_SC_RET err,
    uint8_t *pdu,
    uint16_t pdu_len,
    BVLC_SC_DECODED_MESSAGE *decoded_pdu)
{
    BSC_SC_RET ret;
    debug_printf("hub_connector_socket_event() >>> c = %p, ev = %d, err = %d, "
                 "pdu = %p, pdu_len = %d\n",
        c, ev, err, pdu, pdu_len);
    bsc_global_mutex_lock();
    if (ev == BSC_SOCKET_EVENT_CONNECTED) {
        if (bsc_hub_connector.state ==
            BSC_HUB_CONNECTOR_STATE_CONNECTING_PRIMARY) {
            bsc_hub_connector.state = BSC_HUB_CONNECTOR_STATE_CONNECTED_PRIMARY;
            bsc_hub_connector.event_func(
                BSC_HUBC_EVENT_CONNECTED_PRIMARY, BSC_SC_SUCCESS, NULL, 0);
        } else if (bsc_hub_connector.state ==
            BSC_HUB_CONNECTOR_STATE_CONNECTING_FAILOVER) {
            bsc_hub_connector.state =
                BSC_HUB_CONNECTOR_STATE_CONNECTED_FAILOVER;
            bsc_hub_connector.event_func(
                BSC_HUBC_EVENT_CONNECTED_FAILOVER, BSC_SC_SUCCESS, NULL, 0);
        }
    } else if (ev == BSC_SOCKET_EVENT_DISCONNECTED) {
        if (err == BSC_SC_DUPLICATED_VMAC) {
            debug_printf("hub_connector_socket_event() got fatal error "
                         "BSC_SC_DUPLICATED_VMAC\n");
            bsc_hub_connector.state = BSC_HUB_CONNECTOR_STATE_ERROR;
            bsc_hub_connector.error = BSC_SC_DUPLICATED_VMAC;
            bsc_hub_connector.event_func(
                BSC_HUBC_EVENT_DISCONNECTED, BSC_SC_DUPLICATED_VMAC, NULL, 0);
            bsc_hub_connector_stop();
        } else if (bsc_hub_connector.state ==
            BSC_HUB_CONNECTOR_STATE_CONNECTING_PRIMARY) {
            hub_connector_connect_or_stop(BSC_HUB_CONN_FAILOVER);
        } else if (bsc_hub_connector.state ==
            BSC_HUB_CONNECTOR_STATE_CONNECTING_FAILOVER) {
            debug_printf("hub_connector_socket_event() wait for %d seconds\n",
                bsc_hub_connector.reconnect_timeout_s);
            bsc_hub_connector.state =
                BSC_HUB_CONNECTOR_STATE_WAIT_FOR_RECONNECT;
            mstimer_set(&bsc_hub_connector.t,
                bsc_hub_connector.reconnect_timeout_s * 1000);
        } else if (bsc_hub_connector.state ==
                BSC_HUB_CONNECTOR_STATE_CONNECTED_PRIMARY ||
            bsc_hub_connector.state ==
                BSC_HUB_CONNECTOR_STATE_CONNECTED_FAILOVER) {
            bsc_hub_connector.event_func(
                BSC_HUBC_EVENT_DISCONNECTED, err, NULL, 0);
            hub_connector_connect_or_stop(BSC_HUB_CONN_PRIMARY);
        }
    } else if (ev == BSC_SOCKET_EVENT_RECEIVED) {
        bsc_hub_connector.event_func(
            BSC_HUBC_EVENT_RECEIVED, BSC_SC_SUCCESS, pdu, pdu_len);
    }
    bsc_global_mutex_unlock();
    debug_printf("hub_connector_context_event() <<<\n");
}

static void hub_connector_context_event(BSC_SOCKET_CTX *ctx, BSC_CTX_EVENT ev)
{
    BSC_HUB_CONNECTOR_STATE st;
    bool was_started;

    debug_printf(
        "hub_connector_context_event() >>> ctx = %p, ev = %d\n", ctx, ev);

    if (ev == BSC_CTX_DEINITIALIZED) {
        bsc_global_mutex_lock();
        was_started = bsc_hub_connector_started;
        bsc_hub_connector_started = false;
        st = bsc_hub_connector.state;
        bsc_hub_connector.state = BSC_HUB_CONNECTOR_STATE_IDLE;
        if (was_started) {
            if (st == BSC_HUB_CONNECTOR_STATE_ERROR) {
                bsc_hub_connector.event_func(
                    BSC_HUBC_EVENT_STOPPED, bsc_hub_connector.error, NULL, 0);
            } else {
                bsc_hub_connector.event_func(
                    BSC_HUBC_EVENT_STOPPED, BSC_SC_SUCCESS, NULL, 0);
            }
        }
        bsc_global_mutex_unlock();
    }

    debug_printf("hub_connector_context_event() <<<\n");
}

BACNET_STACK_EXPORT
BSC_SC_RET bsc_hub_connector_start(uint8_t *ca_cert_chain,
    size_t ca_cert_chain_size,
    uint8_t *cert_chain,
    size_t cert_chain_size,
    uint8_t *key,
    size_t key_size,
    BACNET_SC_UUID *local_uuid,
    BACNET_SC_VMAC_ADDRESS *local_vmac,
    uint16_t max_local_bvlc_len,
    uint16_t max_local_npdu_len,
    unsigned int connect_timeout_s,
    unsigned int heartbeat_timeout_s,
    unsigned int disconnect_timeout_s,
    char *primaryURL,
    char *failoverURL,
    unsigned int reconnect_timeout_s,
    HUB_CONNECTOR_EVENT event_func)
{
    BSC_SC_RET ret = BSC_SC_SUCCESS;

    debug_printf("bsc_hub_connector_start() >>>\n");

    if (!ca_cert_chain || !ca_cert_chain_size || !cert_chain ||
        !cert_chain_size || !key || !key_size || !local_uuid || !local_vmac ||
        !max_local_npdu_len || !max_local_bvlc_len || !connect_timeout_s ||
        !heartbeat_timeout_s || !disconnect_timeout_s || !primaryURL ||
        !failoverURL || !reconnect_timeout_s || !event_func) {
        debug_printf("bsc_hub_connector_start() <<< ret = BSC_SC_BAD_PARAM\n");
        return BSC_SC_BAD_PARAM;
    }

    if (strlen(primaryURL) > BSC_WSURL_MAX_LEN ||
        strlen(failoverURL) > BSC_WSURL_MAX_LEN) {
        debug_printf("bsc_hub_connector_start() <<< ret = BSC_SC_BAD_PARAM\n");
        return BSC_SC_BAD_PARAM;
    }

    bsc_global_mutex_lock();
    if (bsc_hub_connector_started) {
        bsc_global_mutex_unlock();
        debug_printf(
            "bsc_hub_connector_start() <<< ret = BSC_SC_INVALID_OPERATION\n");
        return BSC_SC_INVALID_OPERATION;
    }

    bsc_hub_connector.reconnect_timeout_s = reconnect_timeout_s;
    bsc_hub_connector.primary_url[0] = 0;
    bsc_hub_connector.failover_url[0] = 0;
    strcpy((char *)bsc_hub_connector.primary_url, primaryURL);
    strcpy((char *)bsc_hub_connector.failover_url, failoverURL);
    bsc_hub_connector.event_func = event_func;

    bsc_init_ctx_cfg(BSC_SOCKET_CTX_INITIATOR, &bsc_hub_connector.cfg,
        BSC_WEBSOCKET_HUB_PROTOCOL, 0, ca_cert_chain, ca_cert_chain_size,
        cert_chain, cert_chain_size, key, key_size, local_uuid, local_vmac,
        max_local_bvlc_len, max_local_npdu_len, connect_timeout_s,
        heartbeat_timeout_s, disconnect_timeout_s);

    ret = bsc_runloop_reg(&bsc_hub_connector, hub_connector_process_state);

    if (ret == BSC_SC_SUCCESS) {
        ret = bsc_init_сtx(&bsc_hub_connector.ctx, &bsc_hub_connector.cfg,
            &bsc_hub_connector_ctx_funcs, bsc_hub_connector.sock,
            sizeof(bsc_hub_connector.sock) / sizeof(BSC_SOCKET));

        bsc_hub_connector.state = BSC_HUB_CONNECTOR_STATE_CONNECTING_PRIMARY;

        if (ret == BSC_SC_SUCCESS) {
            ret = bsc_connect(&bsc_hub_connector.ctx,
                &bsc_hub_connector.sock[BSC_HUB_CONN_PRIMARY],
                (char *)bsc_hub_connector.primary_url);
            if (ret == BSC_SC_SUCCESS) {
                bsc_hub_connector_started = true;
            } else {
                bsc_hub_connector.state = BSC_HUB_CONNECTOR_STATE_IDLE;
                bsc_runloop_unreg(&bsc_hub_connector);
                bsc_deinit_ctx(&bsc_hub_connector.ctx);
            }
        }
    }

    bsc_global_mutex_unlock();
    debug_printf("bsc_hub_connector_start() <<< ret = %d\n", ret);
    return ret;
}

void bsc_hub_connector_stop(void)
{
    debug_printf("bsc_hub_connector_stop() >>>\n");
    bsc_global_mutex_lock();
    if (bsc_hub_connector_started &&
        (bsc_hub_connector.state !=
            BSC_HUB_CONNECTOR_STATE_WAIT_FOR_CTX_DEINIT)) {
        bsc_hub_connector.state = BSC_HUB_CONNECTOR_STATE_WAIT_FOR_CTX_DEINIT;
        bsc_runloop_unreg(&bsc_hub_connector);
        bsc_deinit_ctx(&bsc_hub_connector.ctx);
    }
    bsc_global_mutex_unlock();
    debug_printf("bsc_hub_connector_stop() <<<\n");
}

BACNET_STACK_EXPORT
BSC_SC_RET bsc_hub_connector_send(uint8_t *pdu, unsigned pdu_len)
{
    BSC_SC_RET ret;
    debug_printf(
        "bsc_hub_connector_send() >>> pdu = %p, pdu_len = %d\n", pdu, pdu_len);
    bsc_global_mutex_lock();

    if (!bsc_hub_connector_started ||
        (bsc_hub_connector.state != BSC_HUB_CONNECTOR_STATE_CONNECTED_PRIMARY &&
            bsc_hub_connector.state !=
                BSC_HUB_CONNECTOR_STATE_CONNECTED_FAILOVER)) {
        debug_printf("bsc_hub_connector_send() pdu is dropped\n");
        debug_printf(
            "bsc_hub_connector_send() <<< ret = BSC_SC_INVALID_OPERATION\n");
        return BSC_SC_INVALID_OPERATION;
    }
    if (bsc_hub_connector.state == BSC_HUB_CONNECTOR_STATE_CONNECTED_PRIMARY) {
        ret = bsc_send(
            &bsc_hub_connector.sock[BSC_HUB_CONN_PRIMARY], pdu, pdu_len);
    } else {
        ret = bsc_send(
            &bsc_hub_connector.sock[BSC_HUB_CONN_FAILOVER], pdu, pdu_len);
    }
    bsc_global_mutex_unlock();
    debug_printf("bsc_hub_connector_send() <<< ret = %d\n", ret);
    return ret;
}
