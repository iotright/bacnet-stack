/**
 * @file
 * @brief Samble BACNet/SC hub.
 * @author Mikhail Antropov
 * @date December 2022
 * @section LICENSE
 *
 * Copyright (C) 2022 Legrand North America, LLC
 * as an unpublished work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later WITH GCC-exception-2.0
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include "bacnet/config.h"
#include "bacnet/bacdef.h"
#include "bacnet/bacdcode.h"
#include "bacnet/apdu.h"
#include "bacnet/dcc.h"
#include "bacnet/iam.h"
#include "bacnet/npdu.h"
#include "bacnet/getevent.h"
#include "bacnet/version.h"
#include "bacnet/basic/services.h"
#include "bacnet/datalink/dlenv.h"
#include "bacnet/basic/sys/filename.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/basic/tsm/tsm.h"
#include "bacnet/datalink/datalink.h"
#include "bacnet/basic/binding/address.h"
/* include the device object */
#include "bacnet/basic/object/device.h"
#include "bacnet/basic/object/lc.h"
#include "bacnet/basic/object/trendlog.h"
#if defined(INTRINSIC_REPORTING)
#include "bacnet/basic/object/nc.h"
#endif /* defined(INTRINSIC_REPORTING) */
#include "bacnet/basic/object/bacfile.h"
#if defined(BAC_UCI)
#include "bacnet/basic/ucix/ucix.h"
#endif /* defined(BAC_UCI) */
#include "bacnet/basic/object/netport.h"
#include "bacnet/basic/object/sc_netport.h"
#include "bacnet/datalink/bsc/bsc-datalink.h"
#include "bacnet/datalink/bsc/bsc-event.h"

static uint8_t *Ca_Certificate = NULL;
static uint8_t *Certificate = NULL;
static uint8_t *Key = NULL;
static char *PrimaryUrl = "wss://127.0.0.1:9999";
static char *FailoverUrl = "wss://127.0.0.1:9999";

#define SC_NETPORT_BACFILE_START_INDEX    0

/* (Doxygen note: The next two lines pull all the following Javadoc
 *  into the ServerDemo module.) */
/** @addtogroup SCServerDemo */
/*@{*/

#ifndef BACDL_BSC
#error "BACDL_BSC must de defined"
#endif
#ifndef BACFILE
#error "BACFILE must de defined"
#endif

/* current version of the BACnet stack */
static const char *BACnet_Version = BACNET_VERSION_TEXT;

/** Buffer used for receiving */
static uint8_t Rx_Buf[MAX_MPDU] = { 0 };

/** Initialize the handlers we will utilize.
 * @see Device_Init, apdu_set_unconfirmed_handler, apdu_set_confirmed_handler
 */
static void Init_Service_Handlers(void)
{
    Device_Init(NULL);
    /* we need to handle who-is to support dynamic device binding */
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_IS, handler_who_is);
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_WHO_HAS, handler_who_has);

#if 0
	/* 	BACnet Testing Observed Incident oi00107
		Server only devices should not indicate that they EXECUTE I-Am
		Revealed by BACnet Test Client v1.8.16 ( www.bac-test.com/bacnet-test-client-download )
			BITS: BIT00040
		Any discussions can be directed to edward@bac-test.com
		Please feel free to remove this comment when my changes accepted after suitable time for
		review by all interested parties. Say 6 months -> September 2016 */
	/* In this demo, we are the server only ( BACnet "B" device ) so we do not indicate
	   that we can execute the I-Am message */
    /* handle i-am to support binding to other devices */
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_I_AM, handler_i_am_bind);
#endif

    /* set the handler for all the services we don't implement */
    /* It is required to send the proper reject message... */
    apdu_set_unrecognized_service_handler_handler(handler_unrecognized_service);
    /* Set the handlers for any confirmed services that we support. */
    /* We must implement read property - it's required! */
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_READ_PROPERTY, handler_read_property);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_READ_PROP_MULTIPLE, handler_read_property_multiple);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_WRITE_PROPERTY, handler_write_property);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_WRITE_PROP_MULTIPLE, handler_write_property_multiple);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_READ_RANGE, handler_read_range);
#if defined(BACFILE)
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_ATOMIC_READ_FILE, handler_atomic_read_file);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_ATOMIC_WRITE_FILE, handler_atomic_write_file);
#endif
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_REINITIALIZE_DEVICE, handler_reinitialize_device);
    apdu_set_unconfirmed_handler(
        SERVICE_UNCONFIRMED_UTC_TIME_SYNCHRONIZATION, handler_timesync_utc);
    apdu_set_unconfirmed_handler(
        SERVICE_UNCONFIRMED_TIME_SYNCHRONIZATION, handler_timesync);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_SUBSCRIBE_COV, handler_cov_subscribe);
    apdu_set_unconfirmed_handler(
        SERVICE_UNCONFIRMED_COV_NOTIFICATION, handler_ucov_notification);
    /* handle communication so we can shutup when asked */
    apdu_set_confirmed_handler(SERVICE_CONFIRMED_DEVICE_COMMUNICATION_CONTROL,
        handler_device_communication_control);
    /* handle the data coming back from private requests */
    apdu_set_unconfirmed_handler(SERVICE_UNCONFIRMED_PRIVATE_TRANSFER,
        handler_unconfirmed_private_transfer);
#if defined(INTRINSIC_REPORTING)
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_ACKNOWLEDGE_ALARM, handler_alarm_ack);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_GET_EVENT_INFORMATION, handler_get_event_information);
    apdu_set_confirmed_handler(
        SERVICE_CONFIRMED_GET_ALARM_SUMMARY, handler_get_alarm_summary);
#endif /* defined(INTRINSIC_REPORTING) */
#if defined(BACNET_TIME_MASTER)
    handler_timesync_init();
#endif
}

static void print_usage(const char *filename)
{
    printf("Usage: %s port ca-cert cert key [device-instance [device-name]]\n", filename);
    printf("       [--version][--help]\n");
}

static void print_help(const char *filename)
{
    printf("Simulate a BACnet/SC HUB device\n"
           "port: Local port\n"
           "ca-cert: Filename of CA certificate\n"
           "cert: Filename of device certificate\n"
           "key: Filename of device certificate key\n"
           "device-instance: BACnet Device Object Instance number that you are\n"
           "trying simulate.\n"
           "device-name: The Device object-name is the text name for the device.\n"
           "\nExample:\n");
    printf("To simulate Device 123 on port #50000, use following command:\n"
           "%s 50000 ca_cert.pem cert.pem key.pem 123\n",
        filename);
    printf("To simulate Device 123 named Fred on port #50000, use following command:\n"
           "%s 50000 ca_cert.pem cert.pem key.pem 123 Fred\n",
        filename);
}

static uint32_t read_file(char *filename, uint8_t **buff)
{
    uint32_t size = 0;
    FILE *pFile;

    pFile = fopen(filename, "rb");
    if (pFile) {
        fseek(pFile, 0L, SEEK_END);
        size = ftell(pFile);
        fseek(pFile, 0, SEEK_SET);

        *buff = (uint8_t *)malloc(size);
        if (*buff != NULL) {
            if (fread(*buff, size, 1, pFile) == 0) {
                size = 0;
            }
        }
        fclose(pFile);
    }
    return *buff ? size : 0;
}

static bool init_bsc(uint16_t port, char *filename_ca_cert, char *filename_cert,
    char *filename_key)
{
    uint32_t instance = 1;
    uint32_t size;

    Network_Port_Object_Instance_Number_Set(0, instance);

    size = read_file(filename_ca_cert, &Ca_Certificate);
    Network_Port_Issuer_Certificate_File_Set_From_Memory(instance, 0,
        Ca_Certificate, size, SC_NETPORT_BACFILE_START_INDEX);

    size = read_file(filename_cert, &Certificate);
    Network_Port_Operational_Certificate_File_Set_From_Memory(instance,
        Certificate, size, SC_NETPORT_BACFILE_START_INDEX + 1);

    size = read_file(filename_key, &Key);
    Network_Port_Certificate_Key_File_Set_From_Memory(instance,
        Key, size, SC_NETPORT_BACFILE_START_INDEX + 2);

    Network_Port_SC_Primary_Hub_URI_Set(instance, PrimaryUrl);
    Network_Port_SC_Failover_Hub_URI_Set(instance, FailoverUrl);

    Network_Port_SC_Direct_Connect_Initiate_Enable_Set(instance, false);
    Network_Port_SC_Direct_Connect_Accept_Enable_Set(instance,  true);
    // TODO: get this param from command line
    Network_Port_SC_Direct_Server_Port_Set(instance, 9999);
    Network_Port_SC_Hub_Function_Enable_Set(instance, true);
    Network_Port_SC_Hub_Server_Port_Set(instance, port);

    return true;
}

/** Main function of server demo.
 *
 * @see Device_Set_Object_Instance_Number, dlenv_init, Send_I_Am,
 *      datalink_receive, npdu_handler,
 *      dcc_timer_seconds, datalink_maintenance_timer,
 *      Load_Control_State_Machine_Handler, handler_cov_task,
 *      tsm_timer_milliseconds
 *
 * @param argc [in] Arg count.
 * @param argv [in] Takes one argument: the Device Instance #.
 * @return 0 on success.
 */
int main(int argc, char *argv[])
{
    BACNET_ADDRESS src = { 0 }; /* address where message came from */
    uint16_t pdu_len = 0;
    unsigned timeout = 1; /* milliseconds */
    time_t last_seconds = 0;
    time_t current_seconds = 0;
    uint32_t elapsed_seconds = 0;
    uint32_t elapsed_milliseconds = 0;
    uint32_t address_binding_tmr = 0;
#if defined(INTRINSIC_REPORTING)
    uint32_t recipient_scan_tmr = 0;
#endif
#if defined(BACNET_TIME_MASTER)
    BACNET_DATE_TIME bdatetime;
#endif
#if defined(BAC_UCI)
    int uciId = 0;
    struct uci_context *ctx;
#endif
    int argi = 0;
    const char *filename = NULL;

    uint16_t port = 0;
    char *filename_ca_cert = NULL;
    char *filename_cert = NULL;
    char *filename_key = NULL;

    filename = filename_remove_path(argv[0]);
    argi = 1;
    if ((argc < 2) || (strcmp(argv[argi], "--help") == 0)) {
        print_usage(filename);
        print_help(filename);
        return 0;
    }
    if (strcmp(argv[argi], "--version") == 0) {
        printf("%s %s\n", filename, BACNET_VERSION_TEXT);
        printf("Copyright (C) 2022 by Steve Karg and others.\n"
               "This is free software; see the source for copying "
               "conditions.\n"
               "There is NO warranty; not even for MERCHANTABILITY or\n"
               "FITNESS FOR A PARTICULAR PURPOSE.\n");
        return 0;
    }
    port = strtol(argv[argi], NULL, 0);
    if (++argi < argc) {
        filename_ca_cert = argv[argi];
    }
    if (++argi < argc) {
        filename_cert = argv[argi];
    }
    if (++argi < argc) {
        filename_key = argv[argi];
    }

#if defined(BAC_UCI)
    ctx = ucix_init("bacnet_dev");
    if (!ctx)
        fprintf(stderr, "Failed to load config file bacnet_dev\n");
    uciId = ucix_get_option_int(ctx, "bacnet_dev", "0", "Id", 0);
    if (uciId != 0) {
        Device_Set_Object_Instance_Number(uciId);
    } else {
#endif /* defined(BAC_UCI) */
        /* allow the device ID to be set */
        if (++argi < argc) {
            Device_Set_Object_Instance_Number(strtol(argv[argi], NULL, 0));
        }

#if defined(BAC_UCI)
    }
    ucix_cleanup(ctx);
#endif /* defined(BAC_UCI) */

    printf("BACnet SC Hub Demo\n"
           "BACnet Stack Version %s\n"
           "BACnet Device ID: %u\n"
           "Max APDU: %d\n",
        BACnet_Version, Device_Object_Instance_Number(), MAX_APDU);
    /* load any static address bindings to show up
       in our device bindings list */
    address_init();
    Init_Service_Handlers();
#if defined(BAC_UCI)
    const char *uciname;
    ctx = ucix_init("bacnet_dev");
    if (!ctx)
        fprintf(stderr, "Failed to load config file bacnet_dev\n");
    uciname = ucix_get_option(ctx, "bacnet_dev", "0", "Name");
    if (uciname != 0) {
        Device_Object_Name_ANSI_Init(uciname);
    } else {
#endif /* defined(BAC_UCI) */
        if (++argi < argc) {
            Device_Object_Name_ANSI_Init(argv[argi]);
        }
#if defined(BAC_UCI)
    }
    ucix_cleanup(ctx);
#endif /* defined(BAC_UCI) */
    BACNET_CHARACTER_STRING DeviceName;
    if (Device_Object_Name(Device_Object_Instance_Number(),&DeviceName)) {
        printf("BACnet Device Name: %s\n", DeviceName.value);
    }

    if (!init_bsc(port, filename_ca_cert, filename_cert, filename_key)) {
        goto exit;
    }

    dlenv_init();
    atexit(datalink_cleanup);
    /* configure the timeout values */
    last_seconds = time(NULL);
    /* broadcast an I-Am on startup */
    Send_I_Am(&Handler_Transmit_Buffer[0]);
    /* loop forever */
    for (;;) {
        /* input */
        bsc_wait(1);
#if 0
        current_seconds = time(NULL);
        /* returns 0 bytes on timeout */
        pdu_len = datalink_receive(&src, &Rx_Buf[0], MAX_MPDU, timeout);

        /* process */
        if (pdu_len) {
            npdu_handler(&src, &Rx_Buf[0], pdu_len);
        }
        /* at least one second has passed */
        elapsed_seconds = (uint32_t)(current_seconds - last_seconds);
        if (elapsed_seconds) {
            last_seconds = current_seconds;
            dcc_timer_seconds(elapsed_seconds);
            datalink_maintenance_timer(elapsed_seconds);
            dlenv_maintenance_timer(elapsed_seconds);
            Load_Control_State_Machine_Handler();
            elapsed_milliseconds = elapsed_seconds * 1000;
            handler_cov_timer_seconds(elapsed_seconds);
            tsm_timer_milliseconds(elapsed_milliseconds);
            trend_log_timer(elapsed_seconds);
#if defined(INTRINSIC_REPORTING)
            Device_local_reporting();
#endif
#if defined(BACNET_TIME_MASTER)
            Device_getCurrentDateTime(&bdatetime);
            handler_timesync_task(&bdatetime);
#endif
        }
        handler_cov_task();
        /* scan cache address */
        address_binding_tmr += elapsed_seconds;
        if (address_binding_tmr >= 60) {
            address_cache_timer(address_binding_tmr);
            address_binding_tmr = 0;
        }
#if defined(INTRINSIC_REPORTING)
        /* try to find addresses of recipients */
        recipient_scan_tmr += elapsed_seconds;
        if (recipient_scan_tmr >= NC_RESCAN_RECIPIENTS_SECS) {
            Notification_Class_find_recipient();
            recipient_scan_tmr = 0;
        }
#endif
        /* output */

        /* blink LEDs, Turn on or off outputs, etc */
#endif
    }

exit:
    free(Ca_Certificate);
    free(Certificate);
    free(Key);

    return 0;
}

/* @} */

/* End group SCServerDemo */
