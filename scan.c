//
//  Based on:
//  Intel Edison Playground
//  Copyright (c) 2015 Damian Kołakowski. All rights reserved.
//

#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

#include "config.h"

#define STATE_OPEN		0
#define STATE_LOCKED	1

extern int do_post(const char* feed, const char* value);

struct hci_request ble_hci_request(uint16_t ocf, int clen, void * status, void * cparam)
{
	struct hci_request rq;
	memset(&rq, 0, sizeof(rq));
	rq.ogf = OGF_LE_CTL;
	rq.ocf = ocf;
	rq.cparam = cparam;
	rq.clen = clen;
	rq.rparam = status;
	rq.rlen = 1;
	return rq;
}

static int64_t timeval_subtract(const struct timeval *data)
{
	int sec = 0;
	long usec = 0;
	struct timeval current;
	gettimeofday(&current, NULL);

	sec = current.tv_sec - data->tv_sec;
	usec = current.tv_usec - data->tv_usec;

	return (sec * 1000000ll + usec);
}

static int getVal16(uint8_t* data) {
	int t = (data[1]&0xff)<<8;
	t += (data[0]&0xff);
	return t;
}

static void update_data(uint8_t* data, uint8_t length) {
	float temperature;
	float humidity;
	int battery;
	char buf[16];

	if (length!=0x16 && length!=0x17 && length!=0x19)
		return;
	if (data[4]!=0x16 || data[5]!=0x95 || data[6]!=0xFE)
		return;

	switch (data[18]) {
		case 0x0D:
			temperature = getVal16(&data[21]) / 10.0;
			humidity = getVal16(&data[23]) / 10.0;
			//printf("temperature %g humidity %g\n", temperature, humidity);
			break;
		case 0x0A:
			battery = data[21];
			sprintf(buf, "%d", battery);
			do_post("battery", buf);
			break;
		case 0x04:
			temperature = getVal16(&data[21]) / 10.0;
			sprintf(buf, "%g", temperature);
			do_post("temperature", buf);
			break;
		case 0x06:
			humidity = getVal16(&data[21]) / 10.0;
			sprintf(buf, "%g", humidity);
			do_post("humidity", buf);
			break;
		default:
			return;
	}
}

static void process_adv_info(le_advertising_info * info)
{
	static struct timeval open_at;
	static int status = STATE_LOCKED;
	int  new_status = 0;
	char addr[18];

	ba2str(&(info->bdaddr), addr);
	//printf("%s - RSSI %d\n", addr, (char)info->data[info->length]);
	if (strcmp(addr, BEACON_MAC_ADDR) == 0) {
		gettimeofday(&open_at, NULL);
		printf("\t%s - RSSI %d\n", addr, (char)info->data[info->length]);
	} else if (strcmp(addr, SENSOR_MAC_ADDR) == 0) {
		update_data(info->data, info->length);
		printf("\t%s - RSSI %d\n", addr, (char)info->data[info->length]);
	}
	int64_t duration = timeval_subtract(&open_at);
	//printf("duration %lld\n", duration);

	new_status = (duration > 5000000) ? STATE_LOCKED : STATE_OPEN;

	if (new_status != status) {
		printf("\n===Start===\n");
		printf("STATUS: %s\n", (new_status == STATE_OPEN) ? "OPEN" : "LOCKED");
		do_post("lock", (new_status == STATE_OPEN) ? "0" : "1");
		//sleep(1);
		printf("\n===End===\n");
		status = new_status;
		if (new_status == STATE_OPEN)
			gettimeofday(&open_at, NULL);
	}
}

int main()
{
	int ret, status;

	// Get HCI device.

	const int device = hci_open_dev(hci_get_route(NULL));
	if ( device < 0 ) { 
		perror("Failed to open HCI device.");
		return 0; 
	}

	// Set BLE scan parameters.
	
	le_set_scan_parameters_cp scan_params_cp;
	memset(&scan_params_cp, 0, sizeof(scan_params_cp));
	scan_params_cp.type 			= 0x00; 
	scan_params_cp.interval 		= htobs(0x0010);
	scan_params_cp.window 			= htobs(0x0010);
	scan_params_cp.own_bdaddr_type 	= 0x00; // Public Device Address (default).
	scan_params_cp.filter 			= 0x00; // Accept all.

	struct hci_request scan_params_rq = ble_hci_request(OCF_LE_SET_SCAN_PARAMETERS, LE_SET_SCAN_PARAMETERS_CP_SIZE, &status, &scan_params_cp);
	
	ret = hci_send_req(device, &scan_params_rq, 1000);
	if ( ret < 0 ) {
		hci_close_dev(device);
		perror("Failed to set scan parameters data.");
		return 0;
	}

	// Set BLE events report mask.

	le_set_event_mask_cp event_mask_cp;
	memset(&event_mask_cp, 0, sizeof(le_set_event_mask_cp));
	int i = 0;
	for ( i = 0 ; i < 8 ; i++ ) event_mask_cp.mask[i] = 0xFF;

	struct hci_request set_mask_rq = ble_hci_request(OCF_LE_SET_EVENT_MASK, LE_SET_EVENT_MASK_CP_SIZE, &status, &event_mask_cp);
	ret = hci_send_req(device, &set_mask_rq, 1000);
	if ( ret < 0 ) {
		hci_close_dev(device);
		perror("Failed to set event mask.");
		return 0;
	}

	// Enable scanning.

	le_set_scan_enable_cp scan_cp;
	memset(&scan_cp, 0, sizeof(scan_cp));
	scan_cp.enable 		= 0x01;	// Enable flag.
	scan_cp.filter_dup 	= 0x00; // Filtering disabled.

	struct hci_request enable_adv_rq = ble_hci_request(OCF_LE_SET_SCAN_ENABLE, LE_SET_SCAN_ENABLE_CP_SIZE, &status, &scan_cp);

	ret = hci_send_req(device, &enable_adv_rq, 1000);
	if ( ret < 0 ) {
		hci_close_dev(device);
		perror("Failed to enable scan.");
		return 0;
	}

	// Get Results.

	struct hci_filter nf;
	hci_filter_clear(&nf);
	hci_filter_set_ptype(HCI_EVENT_PKT, &nf);
	hci_filter_set_event(EVT_LE_META_EVENT, &nf);
	if ( setsockopt(device, SOL_HCI, HCI_FILTER, &nf, sizeof(nf)) < 0 ) {
		hci_close_dev(device);
		perror("Could not set socket options\n");
		return 0;
	}

	printf("Scanning....\n");

	uint8_t buf[HCI_MAX_EVENT_SIZE];
	evt_le_meta_event * meta_event;
	le_advertising_info * info;
	int len;

	while ( 1 ) {
		len = read(device, buf, sizeof(buf));
		if ( len >= HCI_EVENT_HDR_SIZE ) {
			meta_event = (evt_le_meta_event*)(buf+HCI_EVENT_HDR_SIZE+1);
			if ( meta_event->subevent == EVT_LE_ADVERTISING_REPORT ) {
				uint8_t reports_count = meta_event->data[0];
				void * offset = meta_event->data + 1;
				while ( reports_count-- ) {
					info = (le_advertising_info *)offset;
					process_adv_info(info);
					offset = info->data + info->length + 2;
				}
			}
		}
	}

	// Disable scanning.

	memset(&scan_cp, 0, sizeof(scan_cp));
	scan_cp.enable = 0x00;	// Disable flag.

	struct hci_request disable_adv_rq = ble_hci_request(OCF_LE_SET_SCAN_ENABLE, LE_SET_SCAN_ENABLE_CP_SIZE, &status, &scan_cp);
	ret = hci_send_req(device, &disable_adv_rq, 1000);
	if ( ret < 0 ) {
		hci_close_dev(device);
		perror("Failed to disable scan.");
		return 0;
	}

	hci_close_dev(device);
	
	return 0;
}
