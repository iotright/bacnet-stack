/**
 * @file
 * @brief BACNet secure connect node API.
 * @author Kirill Neznamov
 * @date October 2022
 * @section LICENSE
 *
 * Copyright (C) 2022 Legrand North America, LLC
 * as an unpublished work.
 *
 * SPDX-License-Identifier: MIT
 */
#include "bacnet/basic/sys/debug.h"
#include "bacnet/datalink/bsc/bsc-node.h"
#include "bacnet/datalink/bsc/bsc-mutex.h"
#include "bacnet/datalink/bsc/bsc-event.h"
#include "bacnet/datalink/bsc/bsc-util.h"
#include "bacnet/datalink/bsc/bsc-runloop.h"
#include "bacnet/datalink/bsc/bsc-hub-function.h"
#include "bacnet/datalink/bsc/bsc-hub-connector.h"
#include "bacnet/datalink/bsc/bsc-node-switch.h"
#include "bacnet/datalink/bsc/bsc-socket.h"
#include "bacnet/bacdef.h"
#include "bacnet/npdu.h"
#include "bacnet/bacenum.h"

#define DEBUG_BSC_NODE 0

#if DEBUG_BSC_NODE == 1
#define DEBUG_PRINTF debug_printf
#else
#undef DEBUG_ENABLED
#define DEBUG_PRINTF(...)
#endif

#define ERROR_STR_OPTION_NOT_UNDERSTOOD \
    "'must understand' option not understood "

#define ERROR_STR_DIRECT_CONNECTIONS_NOT_SUPPORTED \
    "direct connections are not supported"

typedef enum {
    BSC_NODE_STATE_IDLE = 0,
    BSC_NODE_STATE_STARTING = 1,
    BSC_NODE_STATE_STARTED = 2,
    BSC_NODE_STATE_RESTARTING = 3,
    BSC_NODE_STATE_STOPPING = 4
} BSC_NODE_STATE;

struct BSC_Node {
    bool used;
    BSC_NODE_STATE state;
    BSC_NODE_CONF *conf;
    BSC_ADDRESS_RESOLUTION *resolution;
    BSC_HUB_CONNECTOR_HANDLE hub_connector;
    BSC_HUB_FUNCTION_HANDLE hub_function;
    BSC_NODE_SWITCH_HANDLE node_switch;
};

static struct BSC_Node bsc_node[BSC_CONF_NODES_NUM] = { 0 };

static BSC_ADDRESS_RESOLUTION
    bsc_address_resolution[BSC_CONF_NODES_NUM]
                          [BSC_CONF_SERVER_DIRECT_CONNECTIONS_MAX_NUM];
static BSC_NODE_CONF bsc_conf[BSC_CONF_NODES_NUM];

static BSC_SC_RET bsc_node_start_state(BSC_NODE *node, BSC_NODE_STATE state);

static BSC_NODE *bsc_alloc_node(void)
{
    int i;

    for (i = 0; i < BSC_CONF_NODES_NUM; i++) {
        if (bsc_node[i].used == false) {
            bsc_node[i].used = true;
            bsc_node[i].conf = &bsc_conf[i];
            bsc_node[i].resolution = &bsc_address_resolution[i][0];
            return &bsc_node[i];
        }
    }
    return NULL;
}

static BSC_ADDRESS_RESOLUTION *node_get_address_resolution(
    BSC_NODE *node, BACNET_SC_VMAC_ADDRESS *vmac)
{
    int i;

    for (i = 0; i < BSC_CONF_SERVER_DIRECT_CONNECTIONS_MAX_NUM; i++) {
        if (node->resolution[i].used &&
            !memcmp(&vmac->address[0], &node->resolution[i].vmac.address[0],
                BVLC_SC_VMAC_SIZE)) {
            return &node->resolution[i];
        }
    }
    return NULL;
}

static BSC_ADDRESS_RESOLUTION *node_alloc_address_resolution(
    BSC_NODE *node, BACNET_SC_VMAC_ADDRESS *vmac)
{
    int i;
    for (i = 0; i < BSC_CONF_SERVER_DIRECT_CONNECTIONS_MAX_NUM; i++) {
        if (!node->resolution[i].used) {
            node->resolution[i].used = true;
            memcpy(&node->resolution[i].vmac.address[0], &vmac->address[0],
                BVLC_SC_VMAC_SIZE);
            return &node->resolution[i];
        }
    }
    return NULL;
}

static void bsc_free_node(BSC_NODE *node)
{
    node->used = false;
}

static void bsc_node_process_stop_event(BSC_NODE *node)
{
    bool stopped = true;

    DEBUG_PRINTF("bsc_node_process_stop_event() >>> node = %p, state = %d\n",
        node, node->state);

    if (node->conf->hub_function_enabled) {
        if (node->hub_function &&
            !bsc_hub_function_stopped(node->hub_function)) {
            DEBUG_PRINTF("bsc_node_process_stop_event() hub_function %p is not "
                         "stopped\n",
                node->hub_function);
            stopped = false;
        }
    }
    if (node->node_switch && node->conf->node_switch_enabled) {
        if (!bsc_node_switch_stopped(node->node_switch)) {
            DEBUG_PRINTF(
                "bsc_node_process_stop_event() node_switch %p is not stopped\n",
                node->node_switch);
            stopped = false;
        }
    }
    if (node->hub_connector &&
        !bsc_hub_connector_stopped(node->hub_connector)) {
        DEBUG_PRINTF(
            "bsc_node_process_stop_event() hub_connector %p is not stopped\n",
            node->hub_connector);
        stopped = false;
    }

    DEBUG_PRINTF("bsc_node_process_stop_event() stopped = %d\n", stopped);

    if (node->state == BSC_NODE_STATE_STOPPING) {
        if (stopped) {
            node->state = BSC_NODE_STATE_IDLE;
            node->conf->event_func(node, BSC_NODE_EVENT_STOPPED, NULL, 0);
        }
    } else if (node->state == BSC_NODE_STATE_RESTARTING) {
        if (stopped) {
            bsc_node_start_state(node, BSC_NODE_STATE_RESTARTING);
        }
    }
    DEBUG_PRINTF("bsc_node_process_stop_event() <<<\n");
}

static void bsc_node_process_start_event(BSC_NODE *node)
{
    bool started = true;

    DEBUG_PRINTF("bsc_node_process_start_event() >>> node = %p, state = %d\n",
        node, node->state);
    if (node->hub_function && node->conf->hub_function_enabled) {
        if (!bsc_hub_function_started(node->hub_function)) {
            started = false;
        }
    }
    if (node->node_switch && node->conf->node_switch_enabled) {
        if (!bsc_node_switch_started(node->node_switch)) {
            started = false;
        }
    }
    DEBUG_PRINTF("bsc_node_process_start_event() started = %d\n", started);
    if (started) {
        if (node->state == BSC_NODE_STATE_STARTING) {
            node->state = BSC_NODE_STATE_STARTED;
            node->conf->event_func(node, BSC_NODE_EVENT_STARTED, NULL, 0);
        } else if (node->state == BSC_NODE_STATE_RESTARTING) {
            node->state = BSC_NODE_STATE_STARTED;
            node->conf->event_func(node, BSC_NODE_EVENT_RESTARTED, NULL, 0);
        }
    }
    DEBUG_PRINTF("bsc_node_process_start_event() <<<\n");
}

static void bsc_node_restart(BSC_NODE *node)
{
    DEBUG_PRINTF("bsc_node_restart() >>> node = %p hub_function %p "
                 "hub_connector %p node_switch %p\n",
        node, node->hub_function, node->hub_connector, node->node_switch);
    node->state = BSC_NODE_STATE_RESTARTING;
    bsc_hub_connector_stop(node->hub_connector);
    if (node->hub_function && node->conf->hub_function_enabled) {
        bsc_hub_function_stop(node->hub_function);
    }
    if (node->conf->node_switch_enabled) {
        bsc_node_switch_stop(node->node_switch);
    }
    DEBUG_PRINTF("bsc_node_restart() <<<\n");
}

static void bsc_node_process_received(BSC_NODE *node,
    uint8_t *pdu,
    uint16_t pdu_len,
    BVLC_SC_DECODED_MESSAGE *decoded_pdu)
{
    int i, j;
    int start;
    static uint8_t buf[BVLC_SC_NPDU_SIZE_CONF];
    uint16_t bufsize;
    BSC_SC_RET ret;
    uint16_t error_class;
    uint16_t error_code;
    BSC_ADDRESS_RESOLUTION *r;

    DEBUG_PRINTF("bsc_node_process_received() >>> node = %p, pdu = %p, pdu_len "
                 "= %d, decoded_pdu = %p\n",
        node, pdu, pdu_len, decoded_pdu);

    if (decoded_pdu->hdr.dest_options_len) {
        for (i = 0; i < decoded_pdu->hdr.dest_options_len; i++) {
            if (decoded_pdu->dest_options[i].must_understand) {
                DEBUG_PRINTF("bsc_node_process_received() pdu with "
                             "'must-understand' is dropped\n");
                if (bvlc_sc_need_send_bvlc_result(decoded_pdu)) {
                    error_code = ERROR_CODE_HEADER_NOT_UNDERSTOOD;
                    error_class = ERROR_CLASS_COMMUNICATION;
                    bufsize = bvlc_sc_encode_result(buf, sizeof(buf),
                        decoded_pdu->hdr.message_id, decoded_pdu->hdr.origin,
                        decoded_pdu->hdr.dest, decoded_pdu->hdr.bvlc_function,
                        1, &decoded_pdu->dest_options[i].packed_header_marker,
                        &error_class, &error_code,
                        (uint8_t *)ERROR_STR_OPTION_NOT_UNDERSTOOD);
                    if (bufsize) {
                        ret = bsc_node_send(node, buf, bufsize);
                        if (ret != BSC_SC_SUCCESS) {
                            DEBUG_PRINTF(
                                "bsc_node_process_received() warning "
                                "bvlc-result pdu is not sent, error %d\n",
                                ret);
                        }
                    }
                    DEBUG_PRINTF("bsc_node_process_received() <<<\n");
                    return;
                }
            }
        }
    }

    switch (decoded_pdu->hdr.bvlc_function) {
        case BVLC_SC_RESULT: {
            if (decoded_pdu->payload.result.bvlc_function ==
                BVLC_SC_ADDRESS_RESOLUTION) {
                DEBUG_PRINTF("received a NAK for address resolution from %s\n",
                    bsc_vmac_to_string(decoded_pdu->hdr.origin));
                r = node_get_address_resolution(node, decoded_pdu->hdr.origin);
                if (r) {
                    r->urls_num = 0;
                    mstimer_restart(&r->fresh_timer);
                } else {
                    r = node_alloc_address_resolution(
                        node, decoded_pdu->hdr.origin);
                    if (r) {
                        r->urls_num = 0;
                        mstimer_restart(&r->fresh_timer);
                    } else {
                        DEBUG_PRINTF("can't allocate address resolutio for "
                                     "node with address %s\n",
                            bsc_vmac_to_string(decoded_pdu->hdr.origin));
                    }
                }
            } else {
                DEBUG_PRINTF("node %p get unexpected result pdu with bvlc "
                             "function %d from node %s\n",
                    node, decoded_pdu->payload.result.bvlc_function,
                    bsc_vmac_to_string(decoded_pdu->hdr.origin));
            }
            break;
        }
        case BVLC_SC_ADVERTISIMENT: {
            // Ignore that messages for now
            break;
        }
        case BVLC_SC_ADVERTISIMENT_SOLICITATION: {
            BACNET_STACK_EXPORT
            bufsize = bvlc_sc_encode_advertisiment(buf, sizeof(buf),
                bsc_get_next_message_id(), NULL, decoded_pdu->hdr.origin,
                bsc_hub_connector_status(node->hub_connector),
                node->conf->node_switch_enabled
                    ? BVLC_SC_DIRECT_CONNECTIONS_ACCEPT_SUPPORTED
                    : BVLC_SC_DIRECT_CONNECTIONS_ACCEPT_UNSUPPORTED,
                node->conf->max_local_bvlc_len, node->conf->max_local_npdu_len);
            if (bufsize) {
                ret = bsc_node_send(node, buf, bufsize);
                if (ret != BSC_SC_SUCCESS) {
                    DEBUG_PRINTF(
                        "bsc_node_process_received() warning "
                        "advertisement pdu is not sent to node %s, error %d\n",
                        ret, bsc_vmac_to_string(decoded_pdu->hdr.origin));
                }
            }
            break;
        }
        case BVLC_SC_ADDRESS_RESOLUTION: {
            if (node->conf->node_switch_enabled) {
                bufsize = bvlc_sc_encode_address_resolution_ack(buf,
                    sizeof(buf), decoded_pdu->hdr.message_id,
                    decoded_pdu->hdr.origin, decoded_pdu->hdr.dest,
                    (uint8_t *)node->conf->direct_connection_accept_uris,
                    node->conf->direct_connection_accept_uris_len);
                if (bufsize) {
                    ret = bsc_node_send(node, buf, bufsize);
                    if (ret != BSC_SC_SUCCESS) {
                        DEBUG_PRINTF(
                            "bsc_node_process_received() warning "
                            "address resolution ack is not sent, error %d\n",
                            ret);
                    }
                }
            } else {
                error_code = ERROR_CODE_OPTIONAL_FUNCTIONALITY_NOT_SUPPORTED;
                error_class = ERROR_CLASS_COMMUNICATION;
                bufsize = bvlc_sc_encode_result(buf, sizeof(buf),
                    decoded_pdu->hdr.message_id, decoded_pdu->hdr.origin,
                    decoded_pdu->hdr.dest, decoded_pdu->hdr.bvlc_function, 1,
                    NULL, &error_class, &error_code,
                    (uint8_t *)ERROR_STR_DIRECT_CONNECTIONS_NOT_SUPPORTED);
                if (bufsize) {
                    ret = bsc_node_send(node, buf, bufsize);
                    if (ret != BSC_SC_SUCCESS) {
                        DEBUG_PRINTF("bsc_node_process_received() warning "
                                     "bvlc-result pdu is not sent, error %d\n",
                            ret);
                    }
                }
            }
            break;
        }
        case BVLC_SC_ADDRESS_RESOLUTION_ACK: {
            r = node_get_address_resolution(node, decoded_pdu->hdr.origin);
            if (!r) {
                r = node_alloc_address_resolution(
                    node, decoded_pdu->hdr.origin);
                if (!r) {
                    DEBUG_PRINTF("can't allocate address resolutio for node "
                                 "with address %s\n",
                        bsc_vmac_to_string(decoded_pdu->hdr.origin));
                }
            }
            if (r) {
                r->urls_num = 0;
                for (i = 0, j = 0, start = 0;
                     i < decoded_pdu->payload.address_resolution_ack
                             .utf8_websocket_uri_string_len;
                     i++) {
                    if (i == 0x20) {
                        if (i > BSC_CONF_NODE_MAX_URI_SIZE_IN_ADDRESS_RESOLUTION_ACK ||
                            (i - start) == 0) {
                            start = i + 1;
                            continue;
                        } else {
                            memcpy(&r->utf8_urls[j][0],
                                &decoded_pdu->payload.address_resolution_ack
                                     .utf8_websocket_uri_string[start],
                                i - start);
                            r->utf8_urls[j][i] = 0;
                            j++;
                            start = i + 1;
                        }
                    }
                }
                if (i - start > 0 &&
                    i <= BSC_CONF_NODE_MAX_URI_SIZE_IN_ADDRESS_RESOLUTION_ACK) {
                    memcpy(&r->utf8_urls[j][0],
                        &decoded_pdu->payload.address_resolution_ack
                             .utf8_websocket_uri_string[start],
                        i - start);
                    r->utf8_urls[j][i] = 0;
                    j++;
                }
                r->urls_num = j;
                mstimer_restart(&r->fresh_timer);
                bsc_node_switch_process_address_resolution(
                    node->node_switch, r);
            }
            break;
        }
        case BVLC_SC_ENCAPSULATED_NPDU: {
            node->conf->event_func(node, BSC_NODE_EVENT_RECEIVED, pdu, pdu_len);
            break;
        }
    }
    DEBUG_PRINTF("bsc_node_process_received() <<<\n");
}

static void bsc_hub_connector_event(BSC_HUB_CONNECTOR_EVENT ev,
    BSC_HUB_CONNECTOR_HANDLE h,
    void *user_arg,
    uint8_t *pdu,
    uint16_t pdu_len,
    BVLC_SC_DECODED_MESSAGE *decoded_pdu)
{
    BSC_NODE *node = (BSC_NODE *)user_arg;
    bsc_global_mutex_lock();
    DEBUG_PRINTF("bsc_hub_connector_event() >>> ev = %d, h = %p, node = %p\n",
        ev, h, user_arg);
    if (ev == BSC_HUBC_EVENT_STOPPED) {
        node->hub_connector = NULL;
        bsc_node_process_stop_event(node);
    } else if (ev == BSC_HUBC_EVENT_ERROR_DUPLICATED_VMAC) {
        if (node->state != BSC_NODE_STATE_STOPPING &&
            node->state != BSC_NODE_STATE_RESTARTING) {
            bsc_node_restart(node);
        }
    } else if (ev == BSC_HUBC_EVENT_RECEIVED) {
        bsc_node_process_received(node, pdu, pdu_len, decoded_pdu);
    }
    DEBUG_PRINTF("bsc_hub_connector_event() <<<\n");
    bsc_global_mutex_unlock();
}

static void bsc_hub_function_event(
    BSC_HUB_FUNCTION_EVENT ev, BSC_HUB_FUNCTION_HANDLE h, void *user_arg)
{
    BSC_NODE *node = (BSC_NODE *)user_arg;
    bsc_global_mutex_lock();
    DEBUG_PRINTF("bsc_hub_function_event() >>> ev = %d, h = %p, node = %p\n",
        ev, h, user_arg);
    if (ev == BSC_HUBF_EVENT_STARTED) {
        bsc_node_process_start_event(node);
    } else if (ev == BSC_HUBF_EVENT_STOPPED) {
        node->hub_function = NULL;
        bsc_node_process_stop_event(node);
    } else if (ev == BSC_HUBF_EVENT_ERROR_DUPLICATED_VMAC) {
        if (node->state != BSC_NODE_STATE_STOPPING &&
            node->state != BSC_NODE_STATE_RESTARTING) {
            bsc_node_restart(node);
        }
    }
    DEBUG_PRINTF("bsc_hub_function_event() <<<\n");
    bsc_global_mutex_unlock();
}

static void bsc_node_switch_event(BSC_NODE_SWITCH_EVENT ev,
    BSC_NODE_SWITCH_HANDLE h,
    void *user_arg,
    uint8_t *pdu,
    uint16_t pdu_len,
    BVLC_SC_DECODED_MESSAGE *decoded_pdu)
{
    BSC_NODE *node = (BSC_NODE *)user_arg;

    bsc_global_mutex_lock();
    DEBUG_PRINTF("bsc_node_switch_event() >>> ev = %d, h = %p, node = %p\n", ev,
        h, user_arg);
    if (ev == BSC_NODE_SWITCH_EVENT_STARTED) {
        bsc_node_process_start_event(node);
    } else if (ev == BSC_NODE_SWITCH_EVENT_STOPPED) {
        node->node_switch = NULL;
        bsc_node_process_stop_event(node);
    } else if (ev == BSC_NODE_SWITCH_EVENT_DUPLICATED_VMAC) {
        if (node->state != BSC_NODE_STATE_STOPPING &&
            node->state != BSC_NODE_STATE_RESTARTING) {
            bsc_node_restart(node);
        }
    } else if (ev == BSC_NODE_SWITCH_EVENT_RECEIVED) {
        bsc_node_process_received(node, pdu, pdu_len, decoded_pdu);
    }
    DEBUG_PRINTF(" bsc_node_switch_event() <<<\n");
    bsc_global_mutex_unlock();
}

BACNET_STACK_EXPORT
BSC_SC_RET bsc_node_init(BSC_NODE_CONF *conf, BSC_NODE **node)
{
    DEBUG_PRINTF("bsc_node_init() >>> conf = %p, node = %p\n", conf, node);

    if (!conf || !node) {
        DEBUG_PRINTF("bsc_node_init() <<< ret = BSC_SC_BAD_PARAM\n");
        return BSC_SC_BAD_PARAM;
    }

    if (!conf->ca_cert_chain || !conf->ca_cert_chain_size ||
        !conf->cert_chain || !conf->cert_chain_size || !conf->key ||
        !conf->key_size || !conf->local_uuid || !conf->local_vmac ||
        conf->connect_timeout_s <= 0 || conf->heartbeat_timeout_s <= 0 ||
        conf->disconnect_timeout_s <= 0 || conf->reconnnect_timeout_s <= 0 ||
        conf->address_resolution_timeout_s <= 0 ||
        conf->address_resolution_freshness_timeout_s <= 0 ||
        !conf->primaryURL || !conf->failoverURL || !conf->event_func) {
        DEBUG_PRINTF("bsc_node_init() <<< ret = BSC_SC_BAD_PARAM\n");
        return BSC_SC_BAD_PARAM;
    }
    bsc_global_mutex_lock();
    *node = bsc_alloc_node();

    if (!(*node)) {
        DEBUG_PRINTF("bsc_node_init() <<< ret =  BSC_SC_NO_RESOURCE\n");
        bsc_global_mutex_unlock();
        return BSC_SC_NO_RESOURCES;
    }

    memcpy((*node)->conf, conf, sizeof(*conf));
    bsc_global_mutex_unlock();
    DEBUG_PRINTF("bsc_node_init() <<< ret = BSC_SC_SUCCESS\n");
    return BSC_SC_SUCCESS;
}

BACNET_STACK_EXPORT
BSC_SC_RET bsc_node_deinit(BSC_NODE *node)
{
    DEBUG_PRINTF("bsc_node_deinit() >>> node = %p\n", node);
    bsc_global_mutex_lock();
    if (node->state != BSC_NODE_STATE_IDLE) {
        bsc_global_mutex_unlock();
        DEBUG_PRINTF("bsc_node_deinit() <<< ret = BSC_SC_INVALID_OPERATION\n");
        return BSC_SC_INVALID_OPERATION;
    }
    bsc_free_node(node);
    bsc_global_mutex_unlock();
    DEBUG_PRINTF("bsc_node_deinit() <<< ret = BSC_SC_SUCCESS\n");
    return BSC_SC_SUCCESS;
}

static BSC_SC_RET bsc_node_start_state(BSC_NODE *node, BSC_NODE_STATE state)
{
    BSC_SC_RET ret;
    bsc_global_mutex_lock();
    DEBUG_PRINTF("bsc_node_start() >>> node = %p state = %d\n", node, state);

    node->state = state;
    node->hub_connector = NULL;
    node->hub_function = NULL;
    node->node_switch = NULL;

    if (node->state != BSC_NODE_STATE_RESTARTING) {
        memset(node->resolution, 0,
            sizeof(BSC_ADDRESS_RESOLUTION) *
                BSC_CONF_SERVER_DIRECT_CONNECTIONS_MAX_NUM);
    } else {
        // TODO: integration with BACNET/SC properties
        bsc_generate_random_vmac(node->conf->local_vmac);
    }
    ret = bsc_hub_connector_start(node->conf->ca_cert_chain,
        node->conf->ca_cert_chain_size, node->conf->cert_chain,
        node->conf->cert_chain_size, node->conf->key, node->conf->key_size,
        node->conf->local_uuid, node->conf->local_vmac,
        node->conf->max_local_bvlc_len, node->conf->max_local_npdu_len,
        node->conf->connect_timeout_s, node->conf->heartbeat_timeout_s,
        node->conf->disconnect_timeout_s, node->conf->primaryURL,
        node->conf->failoverURL, node->conf->reconnnect_timeout_s,
        bsc_hub_connector_event, node, &node->hub_connector);

    if (ret != BSC_SC_SUCCESS) {
        node->state = BSC_NODE_STATE_IDLE;
        bsc_global_mutex_unlock();
        DEBUG_PRINTF("bsc_node_start() <<< ret = %d\n", ret);
        return ret;
    }

    if (node->conf->hub_function_enabled) {
        ret = bsc_hub_function_start(node->conf->ca_cert_chain,
            node->conf->ca_cert_chain_size, node->conf->cert_chain,
            node->conf->cert_chain_size, node->conf->key, node->conf->key_size,
            node->conf->hub_server_port, node->conf->iface,
            node->conf->local_uuid, node->conf->local_vmac,
            node->conf->max_local_bvlc_len, node->conf->max_local_npdu_len,
            node->conf->connect_timeout_s, node->conf->heartbeat_timeout_s,
            node->conf->disconnect_timeout_s, bsc_hub_function_event, node,
            &node->hub_function);
        if (ret != BSC_SC_SUCCESS) {
            node->state = BSC_NODE_STATE_IDLE;
            bsc_hub_connector_stop(node->hub_connector);
            bsc_global_mutex_unlock();
            DEBUG_PRINTF("bsc_node_start() <<< ret = %d\n", ret);
            return ret;
        }
    }

    if (node->conf->node_switch_enabled) {
        ret = bsc_node_switch_start(node->conf->ca_cert_chain,
            node->conf->ca_cert_chain_size, node->conf->cert_chain,
            node->conf->cert_chain_size, node->conf->key, node->conf->key_size,
            node->conf->direct_server_port, node->conf->iface,
            node->conf->local_uuid, node->conf->local_vmac,
            node->conf->max_local_bvlc_len, node->conf->max_local_npdu_len,
            node->conf->connect_timeout_s, node->conf->heartbeat_timeout_s,
            node->conf->disconnect_timeout_s, node->conf->reconnnect_timeout_s,
            node->conf->address_resolution_timeout_s, bsc_node_switch_event,
            node, &node->node_switch);
        if (ret != BSC_SC_SUCCESS) {
            node->state = BSC_NODE_STATE_IDLE;
            bsc_hub_connector_stop(node->hub_connector);
            bsc_hub_function_stop(node->hub_function);
            bsc_global_mutex_unlock();
            DEBUG_PRINTF("bsc_node_start() <<< ret = %d\n", ret);
            return ret;
        }
    }
    DEBUG_PRINTF(
        "bsc_node_start() hub_function %p hub_connector %p node_switch %p\n",
        node->hub_function, node->hub_connector, node->node_switch);
    bsc_global_mutex_unlock();
    DEBUG_PRINTF("bsc_node_start() <<< ret = %d\n", ret);
    return ret;
}

BACNET_STACK_EXPORT
BSC_SC_RET bsc_node_start(BSC_NODE *node)
{
    BSC_SC_RET ret;

    DEBUG_PRINTF("bsc_node_start() >>> node = %p\n", node);

    if (!node) {
        DEBUG_PRINTF("bsc_node_start() <<< ret = BSC_SC_BAD_PARAM\n");
        return BSC_SC_BAD_PARAM;
    }

    bsc_global_mutex_lock();

    if (node->state != BSC_NODE_STATE_IDLE) {
        bsc_global_mutex_unlock();
        DEBUG_PRINTF("bsc_node_start() <<< ret = BSC_SC_INVALID_OPERATION\n");
        return BSC_SC_INVALID_OPERATION;
    }
    ret = bsc_node_start_state(node, BSC_NODE_STATE_STARTING);
    bsc_global_mutex_unlock();
    DEBUG_PRINTF("bsc_node_start() <<< ret = %d\n", ret);
    return ret;
}

BACNET_STACK_EXPORT
void bsc_node_stop(BSC_NODE *node)
{
    DEBUG_PRINTF("bsc_node_stop() >>> node = %p\n", node);

    if (node) {
        bsc_global_mutex_lock();

        if (node->state != BSC_NODE_STATE_IDLE) {
            node->state = BSC_NODE_STATE_STOPPING;
            bsc_hub_connector_stop(node->hub_connector);
            if (node->conf->hub_function_enabled) {
                bsc_hub_function_stop(node->hub_function);
            }
            if (node->conf->node_switch_enabled) {
                bsc_node_switch_stop(node->node_switch);
            }
        }

        bsc_global_mutex_unlock();
    }

    DEBUG_PRINTF("bsc_node_stop() <<<\n");
}

BACNET_STACK_EXPORT
BSC_SC_RET bsc_node_hub_connector_send(
    void *p_node, uint8_t *pdu, unsigned pdu_len)
{
    BSC_NODE *node = (BSC_NODE *)p_node;
    BSC_SC_RET ret;

    DEBUG_PRINTF("bsc_node_hub_connector_send() >>> p_node = %p, pdu = %p, "
                 "pdu_len = %d\n",
        p_node, pdu, pdu_len);

    if (!node) {
        DEBUG_PRINTF(
            "bsc_node_hub_connector_send() <<< ret =  BSC_SC_BAD_PARAM\n");
        return BSC_SC_BAD_PARAM;
    }

    bsc_global_mutex_lock();

    if (node->state != BSC_NODE_STATE_STARTED) {
        DEBUG_PRINTF("bsc_node_hub_connector_send() <<< ret = "
                     "BSC_SC_INVALID_OPERATION\n");
        bsc_global_mutex_unlock();
        return BSC_SC_INVALID_OPERATION;
    }

    ret = bsc_hub_connector_send(node->hub_connector, pdu, pdu_len);
    bsc_global_mutex_unlock();
    DEBUG_PRINTF("bsc_node_hub_connector_send() <<< ret = %d\n", ret);
    return ret;
}

BACNET_STACK_EXPORT
BSC_SC_RET bsc_node_send(BSC_NODE *p_node, uint8_t *pdu, unsigned pdu_len)
{
    BSC_NODE *node = (BSC_NODE *)p_node;
    BSC_SC_RET ret;

    DEBUG_PRINTF("bsc_node_send() >>> p_node = %p, pdu = %p, "
                 "pdu_len = %d\n",
        p_node, pdu, pdu_len);

    if (!node) {
        DEBUG_PRINTF("bsc_node_send() <<< ret =  BSC_SC_BAD_PARAM\n");
        return BSC_SC_BAD_PARAM;
    }

    bsc_global_mutex_lock();

    if (node->state != BSC_NODE_STATE_STARTED) {
        DEBUG_PRINTF("bsc_node_send() <<< ret = "
                     "BSC_SC_INVALID_OPERATION\n");
        bsc_global_mutex_unlock();
        return BSC_SC_INVALID_OPERATION;
    }

    if (node->conf->node_switch_enabled) {
        ret = bsc_node_switch_send(node->node_switch, pdu, pdu_len);
    } else {
        ret = bsc_hub_connector_send(node->hub_connector, pdu, pdu_len);
    }

    bsc_global_mutex_unlock();
    DEBUG_PRINTF("bsc_node_send() <<< ret = %d\n", ret);
    return ret;
}

BACNET_STACK_EXPORT
BSC_ADDRESS_RESOLUTION *bsc_node_get_address_resolution(
    void *p_node, BACNET_SC_VMAC_ADDRESS *vmac)
{
    int i;
    BSC_NODE *node = (BSC_NODE *)p_node;
    bsc_global_mutex_lock();
    if (node->state != BSC_NODE_STATE_STARTED || !vmac) {
        bsc_global_mutex_unlock();
        return NULL;
    }
    for (i = 0; i < BSC_CONF_SERVER_DIRECT_CONNECTIONS_MAX_NUM; i++) {
        if (node->resolution[i].used &&
            !memcmp(&vmac->address[0], &node->resolution[i].vmac.address[0],
                BVLC_SC_VMAC_SIZE)) {
            if (!mstimer_expired(&node->resolution[i].fresh_timer)) {
                bsc_global_mutex_unlock();
                return &node->resolution[i];
            } else {
                bsc_global_mutex_unlock();
                return NULL;
            }
        }
    }
    bsc_global_mutex_unlock();
    return NULL;
}

BSC_SC_RET bsc_node_send_address_resolution(
    void *p_node, BACNET_SC_VMAC_ADDRESS *dest)
{
    BSC_NODE *node = (BSC_NODE *)p_node;
    uint8_t pdu[32];
    unsigned pdu_len;
    BSC_SC_RET ret;
    DEBUG_PRINTF(
        "bsc_node_send_address_resolution() >>> node = %p, dest = %p\n", node,
        dest);
    pdu_len = bvlc_sc_encode_address_resolution(
        pdu, sizeof(pdu), bsc_get_next_message_id(), NULL, dest);
    ret = bsc_node_send(node, pdu, pdu_len);
    DEBUG_PRINTF("bsc_node_send_address_resolution() <<< ret = %d\n", ret);
    return ret;
}
