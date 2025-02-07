/*
 * Common Functionality for pipe
 *
 * $Copyright (C) 2008 Broadcom Corporation$
 *
 * $Id: wlu_pipe.c 385622 2013-02-16 03:40:23Z jwang $
 */
#ifdef WIN32
#define NEED_IR_TYPES

#include <windows.h>
#ifdef UNDER_CE
/*  Expected order for succesfull compliation */
#include <winsock2.h>
#endif
#include <epictrl.h>
#include <irelay.h>
#endif /* WIN32 */

#ifdef LINUX
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <wlioctl.h>
#include <net/if.h>
#endif

#ifdef vxworks
#include "wlu_vx.h"
#include "inetLib.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#ifdef TARGETOS_symbian
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#else
#if !defined(TARGETOS_nucleus) && !defined (MACOSX) && !defined (vxworks)
#include <malloc.h>
#endif
#endif /* TARGETOS_symbian */
#include <typedefs.h>
#include <wlioctl.h>

#include <proto/ethernet.h>
#include <bcmendian.h>
#include <bcmutils.h>
#include <bcmcdc.h>
#include <proto/802.11.h>
#if defined(RWL_WIFI) || defined(WIFI_REFLECTOR)
#include <rwl_wifi.h>
#endif /* defined(RWL_WIFI) || defined(WIFI_REFLECTOR) */
#include "wlu.h"
#include "wlu_remote.h"

static rem_ioctl_t rem_cdc;
char  *g_rwl_device_name_serial = "";
bool g_rwl_swap = FALSE;
char g_rem_ifname[IFNAMSIZ] = "\0";
int need_speedy_response; /* Indicate findserver is checking channels */

#ifdef LINUX
	/* Linux 100 usec */
#define SYNC_TIME 100
#else
	/* WinXP 1 sec  */
#define SYNC_TIME 1
#endif

#define UART_FIFO_LEN 			64
#define END_OF_PACK_SEP_LEN 	2
#define INIT_CMD_SLEEP 			500
#define dtoh32(i) i

/* dword align allocation */
union {
	uchar bufdata[ETHER_ADDR_LEN];
	uint32 alignme;
} bufmac_wlu;
char *g_rwl_buf_mac = (char*) &bufmac_wlu.bufdata;

#ifdef RWL_WIFI
/* dword align allocation */
union {
	uchar shelldata[WL_MAX_BUF_LEN];
	uint32 alignme;
} shell_wlu;
char *g_rwl_buf_shell = (char*) &shell_wlu.shelldata;
int rwl_find_remote_wifi_server(void *wl, char *id);
int wl_ioctl(void *wl, int cmd, void *buf, int len, bool set);

/*
 * This function runs a set of commands before running the wi-fi server
 * This is avoids packet drops and improves performance.
 * We run the following wl commands
 * up, mpc 0, wsec 0, slow_timer 999999, fast_timer 999999, glacial_timer 999999
 * legacylink 1, monitor 1.
 */
void remote_wifi_ser_init_cmds(void *wl)
{
	int err;
	char bigbuf[RWL_WIFI_BUF_LEN];
	uint len = 0, count;
	/* The array stores command, length and then data format */
	remote_wifi_cmds_t wifi_cmds[] = {
						{WLC_UP,  NULL, 0x0},
						{WLC_SET_VAR,  "mpc", 0},
						{WLC_SET_WSEC,  NULL, 0x0},
						{WLC_SET_VAR, "slow_timer", 999999},
						{WLC_SET_VAR, "fast_timer", 999999},
						{WLC_SET_VAR, "glacial_timer", 999999},
						{WLC_SET_MONITOR, NULL, 0x1},
						{WLC_SET_PM, NULL, 0x0}
	};

	for (count = 0; count < ARRAYSIZE(wifi_cmds); count++) {

		if (wifi_cmds[count].data == NULL)
			len = sizeof(int);
		else
			len = strlen(wifi_cmds[count].data) + 1 + sizeof(int);

		/* If the command length exceeds the buffer length continue
		 * executing the next command
		 */
		if (len > sizeof(bigbuf)) {
			DPRINT_ERR(ERR, "Err: command len exceeds buf len. Check"
				"initialization cmds\n");
			continue;
		}

		if (wifi_cmds[count].data != NULL) {
			strcpy(bigbuf, wifi_cmds[count].data);
			memcpy(&bigbuf[strlen(wifi_cmds[count].data)+1],
			(char*)&wifi_cmds[count].value, sizeof(int));
		} else {
			memcpy(&bigbuf[0], (char*)&wifi_cmds[count].value, sizeof(int));
		}
#ifdef WIN32
		/* Add OID base for NDIS commands */

		err = (int)ir_setinformation(wl, wifi_cmds[count].cmd + WL_OID_BASE,
		bigbuf, &len);
#endif

#if defined(LINUX) || defined(TARGETOS_symbian) || defined(MACOSX)
		if (wifi_cmds[count].cmd == WLC_UP)
			/* NULL needs to be passed to the driver if WL UP command needs to
			 * be executed Otherwise driver hangs
			 */
			err = wl_ioctl(wl, wifi_cmds[count].cmd,
				NULL, 0, TRUE);
		else
			err = wl_ioctl(wl, wifi_cmds[count].cmd,
			(void*)&bigbuf, len, TRUE);

#elif vxworks
		if (wifi_cmds[count].cmd == WLC_UP) {
			/* NULL needs to be passed to the driver if WL UP command needs to
			 * be executed Otherwise driver hangs
			 */
			if ((err = wl_ioctl_vx(wl, wifi_cmds[count].cmd,
				NULL, 0)) == FAIL)
				DPRINT_ERR(ERR, "wifi_cmds: Error in wl_ioctl_vx\n");
		}
		else {
			if ((err = wl_ioctl_vx(wl, wifi_cmds[count].cmd,
				(void*)&bigbuf, len)) == FAIL)
				DPRINT_ERR(ERR, "remote_wifi_ser_init_cmds:Error in wl_ioctl_vx\n");
		}
#endif /* LINUX || TARGETOS_symbian || MACOSX */
		rwl_sleep(INIT_CMD_SLEEP);
	}
	BCM_REFERENCE(err);
}

/* When user wants to execute local CMD being in remote wifi mode, this fucntion is used
 * to change the remote types.
 * This fucntion is called to swap the remote type to execute the cmd using the local
 *  driver interface.
 * This is required for to call proper front end fucntions to achive the local set/get ioctl.
 */
void
rwl_wifi_swap_remote_type(int flag)
{
	static int remote_flag;
	if (flag == REMOTE_WIFI) {
		remote_type = NO_REMOTE;
		remote_flag = flag;
	} else if (remote_flag == REMOTE_WIFI) {
		remote_type = remote_flag;
		remote_flag = flag;
	}
	return;
}

void
rwl_wifi_free_list(dot11_action_wifi_vendor_specific_t *list[])
{
	int i;
	for (i = 0; i < RWL_DEFAULT_WIFI_FRAG_COUNT; i++) {
		if (list[i])
			free(list[i]);
	}
}

/*
 *  Configures local channel  for finding server.
 *  Server call this fucntion for getting its current channel,
 *  client uses this fucntion for setting its channel to new channel.
 */
static int
rwl_wifi_config_channel(void *wl, int cmd, int *channel)
{
	int error;
	channel_info_t ci;
	error = -1;
	/* Get functionality is used only by server */
	if (cmd == WLC_GET_CHANNEL) {
		memset((char*)&ci, 0, sizeof(ci));
		if ((error = wl_get(wl, cmd, &ci, sizeof(channel_info_t))) < 0)
			return error;
		ci.hw_channel = dtoh32(ci.hw_channel);
		ci.scan_channel = dtoh32(ci.scan_channel);
		ci.target_channel = dtoh32(ci.target_channel);
		if (ci.scan_channel) {
			printf("Scan in progress.\n");
		}
		*channel = ci.hw_channel;
	}

	if (cmd == WLC_SET_CHANNEL) {
		/* Set functionality is used by the client */
		ci.target_channel = *channel;
		/*   When user wants to execute local CMD being in remote wifi mode,
		*	rwl_wifi_swap_remote_type fucntion is used to change the remote types.
		*/
		rwl_wifi_swap_remote_type(remote_type);
		error = wl_set(wl, cmd, &ci.target_channel, sizeof(int));
		/* rever it back to same old remote type */
		rwl_wifi_swap_remote_type(remote_type);
	}
	return error;
}
/*
allocate the memory for action frame and update with wifi tranport header.
*/
dot11_action_wifi_vendor_specific_t *
rwl_wifi_allocate_actionframe()
{
	dot11_action_wifi_vendor_specific_t *action_frame;

	if ((action_frame = (dot11_action_wifi_vendor_specific_t *)
	malloc(RWL_WIFI_ACTION_FRAME_SIZE)) == NULL) {
		DPRINT_ERR(ERR, "rwl_wifi_allocate_actionframe: unable to allocate frame \n");
		return action_frame;
	}
	action_frame->category 	= RWL_ACTION_WIFI_CATEGORY;
	action_frame->OUI[0] 	= RWL_WIFI_OUI_BYTE0;
	action_frame->OUI[1] 	= RWL_WIFI_OUI_BYTE1;
	action_frame->OUI[2] 	= RWL_WIFI_OUI_BYTE2;
	action_frame->type   	= RWL_WIFI_DEFAULT_TYPE;
	action_frame->subtype   = RWL_WIFI_DEFAULT_SUBTYPE;

	return action_frame;
}
/*
 * Send the valid action frame (CDC+DATA) through the REF driver interface.
 * if the CMD is "findserver" then "findmypeer" frames are sent on the diffrent
 * channels to reconnect
 * to with server. Other wl cmd takes the normal path.
 * parameter 3 , i.e. buf contains the cmd line arguments  and buf_len is the actual
 * length of the buf. data_len is the length of the actual data to be sent to remote server.
 */
int
remote_CDC_wifi_tx(void *wl, uint cmd, uchar *buf, uint buf_len, uint data_len, uint flags)
{
	rem_ioctl_t *rem_ptr = &rem_cdc;
	int error, read_try;
	dot11_action_wifi_vendor_specific_t *rem_wifi_send;

	/* prepare CDC header */
	rem_ptr->msg.cmd = cmd;
	rem_ptr->msg.len = buf_len;
	rem_ptr->msg.flags = flags;
	rem_ptr->data_len = data_len;
#ifndef OLYMPIC_RWL
	if (strlen(g_rem_ifname) != 0)
		strncpy(rem_ptr->intf_name, g_rem_ifname, (int)IFNAMSIZ);
	rwl_swap_header(rem_ptr, HOST_TO_NETWORK);
#endif

	if ((data_len > buf_len)) {
		DPRINT_ERR(ERR, "remote_CDC_wifi_tx: data_len (%d) > buf_len (%d)\n",
		data_len, buf_len);
		return (FAIL);
	}
	/* client will not send data greater than RWL_WIFI_FRAG_DATA_SIZE to server,
	 * this condition should not be hit on client side, when sending the cmd
	 * to remote server
	 */
	if (data_len > RWL_WIFI_FRAG_DATA_SIZE)
		DPRINT_DBG(OUTPUT, "data size exceeds data_len %d\n", rem_ptr->msg.len);

	if ((buf != NULL) && (strlen((char*)buf) >= (sizeof(RWL_WIFI_FIND_SER_CMD)-1)) &&
		(!strcmp((char*)buf, RWL_WIFI_FIND_SER_CMD))) {
		/* This is special case for wifi, when user wants to findserver,
		* client has to execute it locally.Find the channel on the on
		* which DUT is operating and sync up with specified MAC address,
		* retry if fails to find the server
		*/
		for (read_try = 0; read_try < RWL_WIFI_RETRY; read_try++) {
			if (((error = rwl_find_remote_wifi_server(wl,
			&g_rwl_buf_mac[0])) == 0)) {
				break;
			}
		}
		return error;
	}

	if ((rem_wifi_send = rwl_wifi_allocate_actionframe()) == NULL) {
		DPRINT_ERR(ERR, "remote_CDC_wifi_tx: Failed to allocate memory\n");
		return (FAIL);
	}
	/* only data length needs to be sent to remote server using this function
	* This function is only meant for client to send data to server
	* Copy the CDC header and data to action frame data feild
	* Now we have encapsulated the CDC header and data completely to in the
	* action frame.
	*/
	memcpy((void*)&rem_wifi_send->data[RWL_WIFI_CDC_HEADER_OFFSET],
		(char*)rem_ptr, REMOTE_SIZE);
	if (buf != NULL) {
	  memcpy((void*)&rem_wifi_send->data[REMOTE_SIZE], buf, data_len);
	}

	/* Send the action frame to remote server using the  rwl_var_setbuf fucntion,
	 * which will use the local driver interface to send this frame on the air
	 */
	if ((error = rwl_var_send_vs_actionframe(wl, RWL_WIFI_ACTION_CMD, rem_wifi_send,
	RWL_WIFI_ACTION_FRAME_SIZE)) < 0) {
		DPRINT_ERR(ERR, "Unable to read the action frame %d error\n", error);
	}
	free(rem_wifi_send);
	return error;
}

/*
 * Read the valid action frame through the REF/DUT driver interface.
 * Retry for no of times, wait for action frame for the specified time.
 */
int
remote_CDC_DATA_wifi_rx(void *wl, dot11_action_wifi_vendor_specific_t * rec_frame)
{
	int error, read_try;
	void *ptr = NULL;


	/* retry is to ensure to read late arrival action frame */
	for (read_try = 0; read_try < RWL_WIFI_RX_RETRY; read_try++) {
		/* read the action frame queued in the local driver wifi queue */
		if ((error = rwl_var_getbuf(wl, RWL_WIFI_GET_ACTION_CMD, rec_frame,
		 RWL_WIFI_ACTION_FRAME_SIZE, &ptr)) < 0) {
			DPRINT_ERR(ERR, "remote_CDC_DATA_wifi_rx: Error in reading the frame %d\n",
			error);
			return error;
		}
		/* copy the read action frame to the user frame and cjheck for the action category.
		 *  If the action category matches with RWL_ACTION_WIFI_CATEGORY ,
		 *  then its the valid frame, otherwise ignore it.
		 */
		memcpy((char*)rec_frame, ptr, RWL_WIFI_ACTION_FRAME_SIZE);

		if (rec_frame->category == RWL_ACTION_WIFI_CATEGORY) {
			break;
		} else {
			/* If we are executing findserver then sleep less */
			if (!need_speedy_response)
				rwl_sleep(RWL_WIFI_RX_DELAY);
			else
				rwl_sleep(RWL_CHANNEL_RX_SCAN_DELAY);
		}
	}
	/* If failed to get the valid frame , indicate the error  */
	if (!(rec_frame->category == RWL_ACTION_WIFI_CATEGORY)) {
		return (FAIL);
	}
	return error;
}
/*
 * read data that has reached client in fragments. If the functtion is
 * called from rwl_shell_information_fe then the flag will be set to 1.
 * For shell response this function will output the response on the standard interface.
 * Response will be coming in out of order , this fucntion will make it inorder.
 * Duplicate action frames are ignored.
 */
int
remote_CDC_DATA_wifi_rx_frag(void *wl, rem_ioctl_t *rem_ptr, uint input_len,
void *input, bool shell)
{
	int error, totalfrag, seq_num, num_frags, remainingbytes;
	dot11_action_wifi_vendor_specific_t *rec_frame;
	uchar *input_buf = (uchar*)input;
	/* An array of pointers to each recieved frag */
	dot11_action_wifi_vendor_specific_t *master_list[RWL_DEFAULT_WIFI_FRAG_COUNT];

	UNUSED_PARAMETER(input_len);
	remainingbytes = 0;

	memset(master_list, 0, sizeof(master_list));
	/* in case of shell cmd's receive size is unknown */
	if (shell) {
		input_buf = (uchar*)g_rwl_buf_shell;
		memset(input_buf, 0, WL_MAX_BUF_LEN);
	}

	/* We don't yet know how many fragments we will need to read since the
	   length is contained in the first frgment of the message itself. Set
	   totalfrag to an arbitry large number and we will readjust it after we
	   successfully recieve the first frag.
	*/
	totalfrag = RWL_DEFAULT_WIFI_FRAG_COUNT;

	for (num_frags = 0; num_frags <= totalfrag; num_frags++) {
		if ((rec_frame = rwl_wifi_allocate_actionframe()) == NULL) {
			DPRINT_DBG(OUTPUT, "malloc failure\n");
			rwl_wifi_free_list(master_list);
			return (FAIL);
		}
		if ((error = remote_CDC_DATA_wifi_rx((void*)wl, rec_frame)) < 0) {
			free(rec_frame);
			rwl_wifi_free_list(master_list);
			return FAIL;
		}

		if (rec_frame->subtype >= RWL_DEFAULT_WIFI_FRAG_COUNT) {
			DPRINT_DBG(OUTPUT, " Read bogus subtype %d\n", rec_frame->subtype);
			free(rec_frame);
			continue;
		}
		/* Keep only originals and discard any dup frags */
		if (!master_list[rec_frame->subtype]) {
			master_list[rec_frame->subtype] = rec_frame;
		} else {
			num_frags--;
			free(rec_frame);
		}

		/* Look for first frag so we can accurately calculate totalfrag */
		if (rec_frame->subtype == RWL_WIFI_DEFAULT_SUBTYPE) {
			memcpy((char*)rem_ptr,
			(char*)&master_list[rec_frame->subtype]->
			data[RWL_WIFI_CDC_HEADER_OFFSET], REMOTE_SIZE);
			rwl_swap_header(rem_ptr, NETWORK_TO_HOST);
			totalfrag = rem_ptr->msg.len / RWL_WIFI_FRAG_DATA_SIZE;
			remainingbytes = rem_ptr->msg.len % RWL_WIFI_FRAG_DATA_SIZE;
		}
	}

	/* All frags are now read and there are no dups. Check for missing frags */
	for (seq_num = 0; seq_num < totalfrag; seq_num++) {
		if (!master_list[seq_num]) {
			DPRINT_DBG(OUTPUT, "Missing frag number %d\n", seq_num);
			rwl_wifi_free_list(master_list);
			return (FAIL);
		}
	}
	/*
	case 1: response in one frame i.e if (totalfrag==0)
	case 2: response in multiple frame ( multiple of RWL_WIFI_FRAG_DATA_SIZE)
	case 3: response in multiple frame and not in multiple of RWL_WIFI_FRAG_DATA_SIZE
	*/

	/* case 1: Check for the response in single frame */
	if (totalfrag == 0)
		memcpy((char*)&input_buf[0],
		(char*)&master_list[0]->data[REMOTE_SIZE], rem_ptr->msg.len);
	else /* case 2: Copy fragments into contiguous frame */
		memcpy((char*)&input_buf[0],
		(char*)&master_list[0]->data[REMOTE_SIZE], RWL_WIFI_FRAG_DATA_SIZE);

	/*
	* If all the frames are recieved , copy them to a contigues buffer
	*/
	for (seq_num = 1; seq_num < totalfrag; seq_num++) {
		memcpy((char*)&input_buf[seq_num*RWL_WIFI_FRAG_DATA_SIZE],
		(char*)&master_list[seq_num]->data, RWL_WIFI_FRAG_DATA_SIZE);
	}

	/* case 3 : if response is in fragments and valid data in the last frame is less
	 * than RWL_WIFI_FRAG_DATA_SIZE
	 */
	if (remainingbytes && (totalfrag > 0))
		memcpy((char*)&input_buf[seq_num*RWL_WIFI_FRAG_DATA_SIZE],
		(char*)&master_list[seq_num]->data, remainingbytes);

	if (shell) {
#ifdef LINUX
		write(1, (char*)input_buf, strlen((char*)input_buf));
#else
		fputs((char*)input_buf, stdout);
#endif /* LINUX */
	}

	rwl_wifi_free_list(master_list);
	return error;
}
/*
 * read out all the action frame which are queued in the driver even
 * before issuing any wl cmd. This is essential because due to late arrival of frame it can
 * get queued after the read expires.
 */
void
rwl_wifi_purge_actionframes(void *wl)
{
	dot11_action_wifi_vendor_specific_t *rec_frame;
	void *ptr = NULL;

	if ((rec_frame = (dot11_action_wifi_vendor_specific_t *)
	malloc(RWL_WIFI_ACTION_FRAME_SIZE)) == NULL) {
		DPRINT_DBG(OUTPUT, "Purge Error in reading the frame \n");
		return;
	}

	for (;;) {
		if (rwl_var_getbuf(wl, RWL_WIFI_GET_ACTION_CMD, rec_frame,
		RWL_WIFI_ACTION_FRAME_SIZE, &ptr) < 0) {
			DPRINT_DBG(OUTPUT, "rwl_wifi_purge_actionframes:"
			"Purge Error in reading the frame \n");
			break;
		}
		memcpy((char*)rec_frame, ptr, RWL_WIFI_ACTION_FRAME_SIZE);

		if ((rec_frame->category != RWL_ACTION_WIFI_CATEGORY))
			break;
	}

	free(rec_frame);

	return;
}
/*
 * check for the channel of remote and respond if it matches with its current
 * channel. Once the server gets the handshake cmd, it will check the channel
 * number of the remote with its channel and if it matches , then it send out the
 * ack to the remote client. This fucntion is used only by the server.
 */
void
rwl_wifi_find_server_response(void *wl, dot11_action_wifi_vendor_specific_t *rec_frame)
{
	int error, send, server_channel;

	if (rec_frame->type == RWL_WIFI_FIND_MY_PEER) {

		rec_frame->type = RWL_WIFI_FOUND_PEER;
		/* read channel on of the SERVER */
		rwl_wifi_config_channel(wl, WLC_GET_CHANNEL, &server_channel);
		/* overlapping channel not supported,
		so server will only respond to client on the channel of the client
		*/
		if (rec_frame->data[RWL_WIFI_CLIENT_CHANNEL_OFFSET] == server_channel) {
			/* send the response by updating server channel in the frame */
			rec_frame->data[RWL_WIFI_SERVER_CHANNEL_OFFSET] = server_channel;
			/* change the TYPE feild for giving the ACK */
			for (send = 0; send < RWL_WIFI_SEND; send++) {
				if ((error = rwl_var_send_vs_actionframe(wl,
					RWL_WIFI_ACTION_CMD,
					rec_frame,
					RWL_WIFI_ACTION_FRAME_SIZE)) < 0) {
					DPRINT_ERR(ERR, "rwl_wifi_find_server_response: Failed"
					"to Send the Frame %d\n", error);
					break;
				}
				rwl_sleep(RWL_WIFI_SEND_DELAY);
			}
		}
	}
	return;
}
/*
 * This function is used by client only. Sends the finmypeer sync frame to remote
 * server on diffrent channels and  waits for the response.
 */
int
rwl_find_remote_wifi_server(void *wl, char *id)
{
	dot11_action_wifi_vendor_specific_t *rem_wifi_send, *rem_wifi_recv;
	rem_ioctl_t *rem_ptr = &rem_cdc;
	/* This list is generated considering valid channel and if this
	 * may requires updation or deletion. This needs to be identified.
	 * we have assumed that server band is not known and considered all band channels.
	 */
	int wifichannel[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
	11, 36, 40, 44, 48, 149, 153, 157, 161, 165};
	int i, error, schannel, channel_count;
	struct ether_addr * curr_macaddr;
	int ret;
	need_speedy_response = TRUE;

	if ((rem_wifi_send = rwl_wifi_allocate_actionframe()) == NULL) {
		DPRINT_ERR(ERR, " rwl_find_remote_wifi_server : Failed to allocate \n");
		return FAIL;
	}
	if ((rem_wifi_recv = rwl_wifi_allocate_actionframe()) == NULL) {
		DPRINT_ERR(ERR, " rwl_find_remote_wifi_server : Failed to allocate\n");
		free(rem_wifi_send);
		return FAIL;
	}

	channel_count = sizeof(wifichannel) / sizeof(int);

	/* make dummy read to make sure we don't read the already queued
	 * actionframes against the cmd we issue
	 */
	rwl_wifi_purge_actionframes(wl);

	/* update the client sync specifier */
	rem_wifi_send->type = RWL_WIFI_FIND_MY_PEER;
	/* update the CDC flag to indicate it is handshake frame */
	rem_ptr->msg.cmd = 0;
	/* cmd =0 ,this will be ignored when server receive frame
	 * with REMOTE_FINDSERVER_IOCTL flag
	 */
	rem_ptr->msg.len = RWL_WIFI_FRAG_DATA_SIZE;
	rem_ptr->msg.flags = REMOTE_FINDSERVER_IOCTL;
	rem_ptr->data_len = RWL_WIFI_FRAG_DATA_SIZE;
	rwl_swap_header(rem_ptr, HOST_TO_NETWORK);

	memcpy((char*)&rem_wifi_send->data, (char*)rem_ptr, REMOTE_SIZE);
	/* copy server mac to which ref driver needs to send unicast action frame */
	memcpy((char*)&rem_wifi_send->data[RWL_REF_MAC_ADDRESS_OFFSET], &id[0], ETHER_ADDR_LEN);

	if ((ret = rwl_var_getbuf(wl, "cur_etheraddr", NULL, 0, (void**) &curr_macaddr)) < 0) {
		DPRINT_ERR(ERR, "Error getting current Mac addr \n");
		return FAIL;
	}

	memcpy((char*)&rem_wifi_send->data[RWL_DUT_MAC_ADDRESS_OFFSET], (char*)curr_macaddr->octet,
		ETHER_ADDR_LEN);
	/* Start with the channel in the list and keep changing till the server
	 * responds or channels list ends
	 */
	for (i = 0; i < channel_count; i++) {
		DPRINT_INFO(OUTPUT, "Scanning Channel: %d ...\n", wifichannel[i]);
		if ((error = rwl_wifi_config_channel(wl, WLC_SET_CHANNEL,
		&wifichannel[i])) < 0) {
			DPRINT_ERR(ERR, " Failed to set the specified channel %d\n",
			wifichannel[i]);
			break;
		}
		/* send channel detail of client to server */
		rem_wifi_send->data[RWL_WIFI_CLIENT_CHANNEL_OFFSET] = wifichannel[i];

		if ((error = rwl_var_send_vs_actionframe(wl, RWL_WIFI_ACTION_CMD, rem_wifi_send,
		RWL_WIFI_ACTION_FRAME_SIZE)) < 0) {
			DPRINT_DBG(OUTPUT, "Failed to Send the Frame %d\n", error);
			break;
		}
		/* read the server response on the same channel */
		if ((error = remote_CDC_DATA_wifi_rx(wl, rem_wifi_recv)) < 0) {
			rwl_sleep(RWL_CHANNEL_SCAN_WAIT);
			continue;
		}
		/* Verify for the Type RWL_WIFI_FOUND_PEER */
		if (rem_wifi_recv->type == RWL_WIFI_FOUND_PEER) {
			if (rem_wifi_recv->data[RWL_WIFI_SERVER_CHANNEL_OFFSET] ==
				rem_wifi_recv->data[RWL_WIFI_CLIENT_CHANNEL_OFFSET]) {

				DPRINT_INFO(OUTPUT, "Server is on channel # %d\n",
				rem_wifi_recv->data[RWL_WIFI_SERVER_CHANNEL_OFFSET]);

				schannel = rem_wifi_recv->data[RWL_WIFI_SERVER_CHANNEL_OFFSET];
				/* Set the back to the channel on which REF was originally */
				if ((error = rwl_wifi_config_channel(wl,
				WLC_SET_CHANNEL, &schannel) < 0)) {
					DPRINT_ERR(ERR, "Failed to set the specified"
					"channel %d\n", schannel);
				} else {
					DPRINT_ERR(ERR, "REF now moved to the"
					"channel of server # %d\n", schannel);
				}
				need_speedy_response = FALSE;
				/* we are done here, end the loop */
				break;
			} else {
				DPRINT_INFO(OUTPUT, "Server is operating on diffrent channel."
				"continue scanning\n");
			}
		}
		/* before chaning the channel of client and sending sync frame
		 * wait for while and send
		 */
		rwl_sleep(RWL_CHANNEL_SCAN_WAIT);
	}
	need_speedy_response = FALSE;

	free(rem_wifi_send);
	free(rem_wifi_recv);
	return error;
}
#endif /* RWL_WIFI */
#ifdef RWL_DONGLE
static int
remote_CDC_tx_dongle(void *wl, rem_ioctl_t *rem_ptr, uchar *buf)
{
	unsigned long numwritten;
	char end_of_packet[END_OF_PACK_SEP_LEN] = "\n\n";
	uchar loc_buf[UART_FIFO_LEN];
	uint len = END_OF_PACK_SEP_LEN;
	uint noframes, frame_count, rem_bytes;
	uint n_bytes;
	uint data_len;

	/* Converting the CDC header with keyword 'rwl ' in ascii format
	 * as dongle UART understands only ascii format.
	 * In dongle UART driver CDC structure is made from the ascii data
	 * it received.
	 */
	sprintf((char*)loc_buf, "rwl %d %d %d %d ", rem_ptr->msg.cmd, rem_ptr->msg.len,
		rem_ptr->msg.flags, rem_ptr->data_len);
	n_bytes = strlen((char*)loc_buf);

	data_len = ltoh32(rem_ptr->data_len);
	DPRINT_DBG(OUTPUT, "rwl %x %d %d %d ", ltoh32(rem_ptr->msg.cmd), ltoh32(rem_ptr->msg.len),
		ltoh32(rem_ptr->msg.flags), data_len);
	DPRINT_DBG(OUTPUT, "CDC Header:No of bytes to be sent=%d\n", n_bytes);
	DPRINT_DBG(OUTPUT, "Data:No of bytes to be sent=%d\n", data_len);

	/* Send the CDC Header */
	if (rwl_write_serial_port(wl, (char*)loc_buf, n_bytes, &numwritten) < 0) {
		DPRINT_ERR(ERR, "CDC_Tx: Header: Write failed\n");
		DPRINT_ERR(ERR, "CDC_Tx: Header: numwritten %ld != n_bytes %d\n",
			numwritten, n_bytes);
		return (FAIL);
	}

	/* Dongle UART FIFO len is 64 bytes and flow control is absent.
	 * While transmitting large chunk of data the data was getting lost
	 * at UART driver so for large chunk of data 64 bytes are sent at a time
	 * folowed by delay and then next set of 64 bytes and so on.
	 * For data which is less than 64 bytes it is sent in one shot
	 */

	noframes = rem_ptr->data_len/UART_FIFO_LEN;
	if (noframes == 0) {
		/* Send the data now */
		if (rwl_write_serial_port(wl, (char*)buf, rem_ptr->data_len, &numwritten) < 0) {
			DPRINT_ERR(ERR, "Data_Tx: Header: Write failed\n");
			DPRINT_ERR(ERR, "Data_Tx: Header: numwritten %ld != len %d\n",
			numwritten, rem_ptr->data_len);
			return (FAIL);
		}

	} else {
		if (rem_ptr->data_len % UART_FIFO_LEN == 0) {
			rem_bytes = UART_FIFO_LEN;
		} else {
			rem_bytes = rem_ptr->data_len % UART_FIFO_LEN;
			noframes += 1;
		}

		for (frame_count = 0; frame_count < noframes; frame_count++) {
			if (frame_count != noframes-1) {
				memcpy(loc_buf, (char*)(&buf[frame_count*UART_FIFO_LEN]),
				UART_FIFO_LEN);
				/* Send the data now */
				if (rwl_write_serial_port(wl, (char*)loc_buf, UART_FIFO_LEN,
				&numwritten) == -1) {
					DPRINT_ERR(ERR, "Data_Tx: Header: Write failed\n");
					return (-1);
				}

			} else {
				memcpy(loc_buf, (char*)(&buf[frame_count*UART_FIFO_LEN]),
				rem_bytes);

				if (rwl_write_serial_port(wl, (char*)loc_buf, rem_bytes,
				&numwritten) == -1) {
					DPRINT_ERR(ERR, "Data_Tx: Header: Write failed\n");
					return (-1);
				}

			}
			rwl_sleep(SYNC_TIME);
		}
	}

	/* Send end of packet now */
	if (rwl_write_serial_port(wl, end_of_packet, len, &numwritten) == -1) {
		DPRINT_ERR(ERR, "CDC_Tx: Header: Write failed\n");
		DPRINT_ERR(ERR, "CDC_Tx: Header: numwritten %ld != len %d\n",
			numwritten, len);
		return (FAIL);
	}

	DPRINT_DBG(OUTPUT, "Packet sent!\n");

	/* Return size of actual buffer to satisfy accounting going on above this level */
	return (ltoh32(rem_ptr->msg.len));
}
#endif /* RWL_DONGLE */

#if defined (RWL_SERIAL) || defined (RWL_DONGLE)|| defined (RWL_SOCKET)
void *
rwl_open_pipe(int remote_type, char *port, int ReadTotalTimeout, int debug)
{
	return rwl_open_transport(remote_type, port, ReadTotalTimeout, debug);
}

int
rwl_close_pipe(int remote_type, void* handle)
{
	return rwl_close_transport(remote_type, handle);
}
#endif

#ifdef DEBUG_SERIAL

const char* status_list[] = {
    "Success",
    "Error generic",
    "Bad Argument",
    "Bad option",
    "Not up",
    "Not down",
    "Not AP",
    "Not STA ",
    "BAD Key Index",
    "Radio Off",
    "Not  band locked",
    "No Clock",
    "BAD Rate valueset",
    "BAD Band",
    "Buffer too short",
    "Buffer too long",
    "Busy",
    "Not Associated",
    "Bad SSID len",
    "Out of Range Channel",
    "Bad Channel",
    "Bad Address",
    "Not Enough Resources",
    "Unsupported",
    "Bad length",
    "Not Ready",
    "Not Permitted",
    "No Memory",
    "Associated",
    "Not In Range",
    "Not Found",
    "WME Not Enabled",
    "TSPEC Not Found",
    "ACM Not Supported",
    "Not WME Association",
    "SDIO Bus Error",
    "Dongle Not Accessible",
    "Incorrect version",
    "TX failure",
    "RX failure",
    "Device not present",
    "NMODE disabled",
    "access to nonresident overlay",
};

const char* ioctl_list[] = {
    "WLC_GET_MAGIC                      ",
    "WLC_GET_VERSION                    ",
    "WLC_UP                             ",
    "WLC_DOWN                           ",
    "WLC_GET_LOOP                       ",
    "WLC_SET_LOOP                       ",
    "WLC_DUMP                           ",
    "WLC_GET_MSGLEVEL                   ",
    "WLC_SET_MSGLEVEL                   ",
    "WLC_GET_PROMISC                    ",
    "WLC_SET_PROMISC                    ",
    "INVALID COMMAND                    ",
    "WLC_GET_RATE                       ",
    "INVALID COMMAND                    ",
    "WLC_GET_INSTANCE                   ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "WLC_GET_INFRA                      ",
    "WLC_SET_INFRA                      ",
    "WLC_GET_AUTH                       ",
    "WLC_SET_AUTH                       ",
    "WLC_GET_BSSID                      ",
    "WLC_SET_BSSID                      ",
    "WLC_GET_SSID                       ",
    "WLC_SET_SSID                       ",
    "WLC_RESTART                        ",
    "INVALID COMMAND                    ",
    "WLC_GET_CHANNEL                    ",
    "WLC_SET_CHANNEL                    ",
    "WLC_GET_SRL                        ",
    "WLC_SET_SRL                        ",
    "WLC_GET_LRL                        ",
    "WLC_SET_LRL                        ",
    "WLC_GET_PLCPHDR                    ",
    "WLC_SET_PLCPHDR                    ",
    "WLC_GET_RADIO                      ",
    "WLC_SET_RADIO                      ",
    "WLC_GET_PHYTYPE                    ",
    "WLC_DUMP_RATE                      ",
    "WLC_SET_RATE_PARAMS                ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "WLC_GET_KEY                        ",
    "WLC_SET_KEY                        ",
    "WLC_GET_REGULATORY                 ",
    "WLC_SET_REGULATORY                 ",
    "WLC_GET_PASSIVE_SCAN               ",
    "WLC_SET_PASSIVE_SCAN               ",
    "WLC_SCAN                           ",
    "WLC_SCAN_RESULTS                   ",
    "WLC_DISASSOC                       ",
    "WLC_REASSOC                        ",
    "WLC_GET_ROAM_TRIGGER               ",
    "WLC_SET_ROAM_TRIGGER               ",
    "WLC_GET_ROAM_DELTA                 ",
    "WLC_SET_ROAM_DELTA                 ",
    "WLC_GET_ROAM_SCAN_PERIOD           ",
    "WLC_SET_ROAM_SCAN_PERIOD           ",
    "WLC_EVM                            ",
    "WLC_GET_TXANT                      ",
    "WLC_SET_TXANT                      ",
    "WLC_GET_ANTDIV                     ",
    "WLC_SET_ANTDIV                     ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "WLC_GET_CLOSED                     ",
    "WLC_SET_CLOSED                     ",
    "WLC_GET_MACLIST                    ",
    "WLC_SET_MACLIST                    ",
    "WLC_GET_RATESET                    ",
    "WLC_SET_RATESET                    ",
    "INVALID COMMAND                    ",
    "WLC_LONGTRAIN                      ",
    "WLC_GET_BCNPRD                     ",
    "WLC_SET_BCNPRD                     ",
    "WLC_GET_DTIMPRD                    ",
    "WLC_SET_DTIMPRD                    ",
    "WLC_GET_SROM                       ",
    "WLC_SET_SROM                       ",
    "WLC_GET_WEP_RESTRICT               ",
    "WLC_SET_WEP_RESTRICT               ",
    "WLC_GET_COUNTRY                    ",
    "WLC_SET_COUNTRY                    ",
    "WLC_GET_PM                         ",
    "WLC_SET_PM                         ",
    "WLC_GET_WAKE                       ",
    "WLC_SET_WAKE                       ",
    "INVALID COMMAND                    ",
    "WLC_GET_FORCELINK                  ",
    "WLC_SET_FORCELINK                  ",
    "WLC_FREQ_ACCURACY                  ",
    "WLC_CARRIER_SUPPRESS               ",
    "WLC_GET_PHYREG                     ",
    "WLC_SET_PHYREG                     ",
    "WLC_GET_RADIOREG                   ",
    "WLC_SET_RADIOREG                   ",
    "WLC_GET_REVINFO                    ",
    "WLC_GET_UCANTDIV                   ",
    "WLC_SET_UCANTDIV                   ",
    "WLC_R_REG                          ",
    "WLC_W_REG                          ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "WLC_GET_MACMODE                    ",
    "WLC_SET_MACMODE                    ",
    "WLC_GET_MONITOR                    ",
    "WLC_SET_MONITOR                    ",
    "WLC_GET_GMODE                      ",
    "WLC_SET_GMODE                      ",
    "WLC_GET_LEGACY_ERP                 ",
    "WLC_SET_LEGACY_ERP                 ",
    "WLC_GET_RX_ANT                     ",
    "WLC_GET_CURR_RATESET               ",
    "WLC_GET_SCANSUPPRESS               ",
    "WLC_SET_SCANSUPPRESS               ",
    "WLC_GET_AP                         ",
    "WLC_SET_AP                         ",
    "WLC_GET_EAP_RESTRICT               ",
    "WLC_SET_EAP_RESTRICT               ",
    "WLC_SCB_AUTHORIZE                  ",
    "WLC_SCB_DEAUTHORIZE                ",
    "WLC_GET_WDSLIST                    ",
    "WLC_SET_WDSLIST                    ",
    "WLC_GET_ATIM                       ",
    "WLC_SET_ATIM                       ",
    "WLC_GET_RSSI                       ",
    "WLC_GET_PHYANTDIV                  ",
    "WLC_SET_PHYANTDIV                  ",
    "WLC_AP_RX_ONLY                     ",
    "WLC_GET_TX_PATH_PWR                ",
    "WLC_SET_TX_PATH_PWR                ",
    "WLC_GET_WSEC                       ",
    "WLC_SET_WSEC                       ",
    "WLC_GET_PHY_NOISE                  ",
    "WLC_GET_BSS_INFO                   ",
    "WLC_GET_PKTCNTS                    ",
    "WLC_GET_LAZYWDS                    ",
    "WLC_SET_LAZYWDS                    ",
    "WLC_GET_BANDLIST                   ",
    "WLC_GET_BAND                       ",
    "WLC_SET_BAND                       ",
    "WLC_SCB_DEAUTHENTICATE             ",
    "WLC_GET_SHORTSLOT                  ",
    "WLC_GET_SHORTSLOT_OVERRIDE         ",
    "WLC_SET_SHORTSLOT_OVERRIDE         ",
    "WLC_GET_SHORTSLOT_RESTRICT         ",
    "WLC_SET_SHORTSLOT_RESTRICT         ",
    "WLC_GET_GMODE_PROTECTION           ",
    "WLC_GET_GMODE_PROTECTION_OVERRIDE  ",
    "WLC_SET_GMODE_PROTECTION_OVERRIDE  ",
    "WLC_UPGRADE                        ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "WLC_GET_IGNORE_BCNS                ",
    "WLC_SET_IGNORE_BCNS                ",
    "WLC_GET_SCB_TIMEOUT                ",
    "WLC_SET_SCB_TIMEOUT                ",
    "WLC_GET_ASSOCLIST                  ",
    "WLC_GET_CLK                        ",
    "WLC_SET_CLK                        ",
    "WLC_GET_UP                         ",
    "WLC_OUT                            ",
    "WLC_GET_WPA_AUTH                   ",
    "WLC_SET_WPA_AUTH                   ",
    "WLC_GET_UCFLAGS                    ",
    "WLC_SET_UCFLAGS                    ",
    "WLC_GET_PWRIDX                     ",
    "WLC_SET_PWRIDX                     ",
    "WLC_GET_TSSI                       ",
    "WLC_GET_SUP_RATESET_OVERRIDE       ",
    "WLC_SET_SUP_RATESET_OVERRIDE       ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "WLC_GET_PROTECTION_CONTROL         ",
    "WLC_SET_PROTECTION_CONTROL         ",
    "WLC_GET_PHYLIST                    ",
    "WLC_ENCRYPT_STRENGTH               ",
    "WLC_DECRYPT_STATUS                 ",
    "WLC_GET_KEY_SEQ                    ",
    "WLC_GET_SCAN_CHANNEL_TIME          ",
    "WLC_SET_SCAN_CHANNEL_TIME          ",
    "WLC_GET_SCAN_UNASSOC_TIME          ",
    "WLC_SET_SCAN_UNASSOC_TIME          ",
    "WLC_GET_SCAN_HOME_TIME             ",
    "WLC_SET_SCAN_HOME_TIME             ",
    "WLC_GET_SCAN_NPROBES               ",
    "WLC_SET_SCAN_NPROBES               ",
    "WLC_GET_PRB_RESP_TIMEOUT           ",
    "WLC_SET_PRB_RESP_TIMEOUT           ",
    "WLC_GET_ATTEN                      ",
    "WLC_SET_ATTEN                      ",
    "WLC_GET_SHMEM                      ",
    "WLC_SET_SHMEM                      ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "WLC_SET_WSEC_TEST                  ",
    "WLC_SCB_DEAUTHENTICATE_FOR_REASON  ",
    "WLC_TKIP_COUNTERMEASURES           ",
    "WLC_GET_PIOMODE                    ",
    "WLC_SET_PIOMODE                    ",
    "WLC_SET_ASSOC_PREFER               ",
    "WLC_GET_ASSOC_PREFER               ",
    "WLC_SET_ROAM_PREFER                ",
    "WLC_GET_ROAM_PREFER                ",
    "WLC_SET_LED                        ",
    "WLC_GET_LED                        ",
    "WLC_GET_INTERFERENCE_MODE          ",
    "WLC_SET_INTERFERENCE_MODE          ",
    "WLC_GET_CHANNEL_QA                 ",
    "WLC_START_CHANNEL_QA               ",
    "WLC_GET_CHANNEL_SEL                ",
    "WLC_START_CHANNEL_SEL              ",
    "WLC_GET_VALID_CHANNELS             ",
    "WLC_GET_FAKEFRAG                   ",
    "WLC_SET_FAKEFRAG                   ",
    "WLC_GET_PWROUT_PERCENTAGE          ",
    "WLC_SET_PWROUT_PERCENTAGE          ",
    "WLC_SET_BAD_FRAME_PREEMPT          ",
    "WLC_GET_BAD_FRAME_PREEMPT          ",
    "WLC_SET_LEAP_LIST                  ",
    "WLC_GET_LEAP_LIST                  ",
    "WLC_GET_CWMIN                      ",
    "WLC_SET_CWMIN                      ",
    "WLC_GET_CWMAX                      ",
    "WLC_SET_CWMAX                      ",
    "WLC_GET_WET                        ",
    "WLC_SET_WET                        ",
    "WLC_GET_PUB                        ",
    "INVALID COMMAND                    ",
    "INVALID COMMAND                    ",
    "WLC_GET_KEY_PRIMARY                ",
    "WLC_SET_KEY_PRIMARY                ",
    "INVALID COMMAND                    ",
    "WLC_GET_ACI_ARGS                   ",
    "WLC_SET_ACI_ARGS                   ",
    "WLC_UNSET_CALLBACK                 ",
    "WLC_SET_CALLBACK                   ",
    "WLC_GET_RADAR                      ",
    "WLC_SET_RADAR                      ",
    "WLC_SET_SPECT_MANAGMENT            ",
    "WLC_GET_SPECT_MANAGMENT            ",
    "WLC_WDS_GET_REMOTE_HWADDR          ",
    "WLC_WDS_GET_WPA_SUP                ",
    "WLC_SET_CS_SCAN_TIMER              ",
    "WLC_GET_CS_SCAN_TIMER              ",
    "WLC_MEASURE_REQUEST                ",
    "WLC_INIT                           ",
    "WLC_SEND_QUIET                     ",
    "WLC_KEEPALIVE                      ",
    "WLC_SEND_PWR_CONSTRAINT            ",
    "WLC_UPGRADE_STATUS                 ",
    "WLC_CURRENT_PWR                    ",
    "WLC_GET_SCAN_PASSIVE_TIME          ",
    "WLC_SET_SCAN_PASSIVE_TIME          ",
    "WLC_LEGACY_LINK_BEHAVIOR           ",
    "WLC_GET_CHANNELS_IN_COUNTRY        ",
    "WLC_GET_COUNTRY_LIST               ",
    "WLC_GET_VAR                        ",
    "WLC_SET_VAR                        ",
    "WLC_NVRAM_GET                      ",
    "WLC_NVRAM_SET                      ",
    "WLC_NVRAM_DUMP                     ",
    "WLC_REBOOT                         ",
    "WLC_SET_WSEC_PMK                   ",
    "WLC_GET_AUTH_MODE                  ",
    "WLC_SET_AUTH_MODE                  ",
    "WLC_GET_WAKEENTRY                  ",
    "WLC_SET_WAKEENTRY                  ",
    "WLC_NDCONFIG_ITEM                  ",
    "WLC_NVOTPW                         ",
    "WLC_OTPW                           ",
    "WLC_IOV_BLOCK_GET                  ",
    "WLC_IOV_MODULES_GET                ",
    "WLC_SOFT_RESET                     ",
    "WLC_GET_ALLOW_MODE                 ",
    "WLC_SET_ALLOW_MODE                 ",
    "WLC_GET_DESIRED_BSSID              ",
    "WLC_SET_DESIRED_BSSID              ",
    "WLC_DISASSOC_MYAP                  ",
    "WLC_GET_NBANDS                     ",
    "WLC_GET_BANDSTATES                 ",
    "WLC_GET_WLC_BSS_INFO               ",
    "WLC_GET_ASSOC_INFO                 ",
    "WLC_GET_OID_PHY                    ",
    "WLC_SET_OID_PHY                    ",
    "WLC_SET_ASSOC_TIME                 ",
    "WLC_GET_DESIRED_SSID               ",
    "WLC_GET_CHANSPEC                   ",
    "WLC_GET_ASSOC_STATE                ",
    "WLC_SET_PHY_STATE                  ",
    "WLC_GET_SCAN_PENDING               ",
    "WLC_GET_SCANREQ_PENDING            ",
    "WLC_GET_PREV_ROAM_REASON           ",
    "WLC_SET_PREV_ROAM_REASON           ",
    "WLC_GET_BANDSTATES_PI              ",
    "WLC_GET_PHY_STATE                  ",
    "WLC_GET_BSS_WPA_RSN                ",
    "WLC_GET_BSS_WPA2_RSN               ",
    "WLC_GET_BSS_BCN_TS                 ",
    "WLC_GET_INT_DISASSOC               ",
    "WLC_SET_NUM_PEERS                  ",
    "WLC_GET_NUM_BSS                    ",
};

void dump_cdc_hdr( rem_ioctl_t* data, char tx )
{
    if ( tx == 0 )
    {
        if (data->msg.flags & CDCF_IOC_ERROR )
        {
            printf( "IOCTL FAILED\n");
        }
        printf( "IOCTL Status: %s\n", status_list[data->msg.status]);
    }
    else
    {
        printf( "Command: %s\n", ioctl_list[data->msg.cmd]);
    }
}


#endif /* ifdef DEBUG_SERIAL */

int
remote_CDC_tx(void *wl, uint cmd, uchar *buf, uint buf_len, uint data_len, uint flags, int debug)
{
#ifdef RWL_SERIAL
	unsigned long numwritten = 0;
#endif
	rem_ioctl_t *rem_ptr = &rem_cdc;
#ifdef RWL_WIFI
	int error;
	uint totalframes, tx_count;
	dot11_action_wifi_vendor_specific_t *rem_wifi_send;
#endif
	UNUSED_PARAMETER(debug);
	UNUSED_PARAMETER(buf);
	UNUSED_PARAMETER(wl);

	memset(rem_ptr, 0, sizeof(rem_ioctl_t));
	rem_ptr->msg.cmd = cmd;
	rem_ptr->msg.len = buf_len;
	rem_ptr->msg.flags = flags;
	rem_ptr->data_len = data_len;
#ifndef OLYMPIC_RWL
	if (strlen(g_rem_ifname) != 0)
		strncpy(rem_ptr->intf_name, g_rem_ifname, (int)IFNAMSIZ);
	rwl_swap_header(rem_ptr, HOST_TO_NETWORK);
#endif
	if (data_len > buf_len) {
		DPRINT_ERR(ERR, "remote_CDC_tx: data_len (%d) > buf_len (%d)\n", data_len, buf_len);
		return (FAIL);
	}
#ifdef RWL_SERIAL
	if (remote_type == REMOTE_SERIAL) {
		int ret;
		char sync = '%';
#ifdef DEBUG_SERIAL
		printf("send sync %\n");
#endif /* ifdef DEBUG_SERIAL */
		/* Send the synchronization character first */
		if ((ret = rwl_write_serial_port(wl, (char *)&sync,
            1, &numwritten)) == -1) {
            DPRINT_ERR(ERR, "CDC_Tx: Sync char: Write failed \n");
            return (FAIL);
        }
        numwritten = ret;

		/* Send CDC header first */
#ifdef DEBUG_SERIAL
        printf("send CDC header\n");
        dump_cdc_hdr( rem_ptr, 1 );
#endif /* ifdef DEBUG_SERIAL */
		if ((ret = rwl_write_serial_port(wl, (char *)rem_ptr,
			REMOTE_SIZE, &numwritten)) == -1) {
			DPRINT_ERR(ERR, "CDC_Tx: Data: Write failed \n");
			return (FAIL);
		}
		numwritten = ret;

		/* Send data second */
#ifdef DEBUG_SERIAL
        printf("Send Data\n");
#endif /* ifdef DEBUG_SERIAL */
		if ((ret = rwl_write_serial_port(wl, (char*)buf,
			data_len, &numwritten)) == -1) {
			DPRINT_ERR(ERR, "CDC_Tx: Data: Write failed \n");
			return (FAIL);
		}
		numwritten = ret;

		return (buf_len);
	}
#endif /* RWL_SERIAL */
#ifdef RWL_DONGLE
	if (remote_type == REMOTE_DONGLE) {
		return (remote_CDC_tx_dongle(wl, rem_ptr, buf));
	}
#endif /* RWL_DONGLE */
#ifdef RWL_SOCKET
	if (remote_type == REMOTE_SOCKET) {
		int ret;

		/* Send CDC header first */
		if ((ret = rwl_send_to_streamsocket(*(int*)wl, (char *)rem_ptr,
			REMOTE_SIZE, 0)) == -1) {
			DPRINT_ERR(ERR, "CDC_Tx: Data: Write failed \n");
			return (FAIL);
		}
		numwritten = ret;

		/* Send data second */
		if ((ret = rwl_send_to_streamsocket(*(int*)wl, (const char*)buf,
			data_len, 0)) == -1) {
			DPRINT_ERR(ERR, "CDC_Tx: Data: Write failed \n");
			return (FAIL);
		}
		numwritten = ret;

		return (buf_len);
	}
#endif /* RWL_SOCKET */

#ifdef RWL_WIFI
	/*
	 * wifi action frame is formed based on the CDC header and data.
	 * If the data is bigger than RWL_WIFI_FRAG_DATA_SIZE size, number of fragments are
	 * calculated and sent
	 * similar number of action frames with subtype incremented with sequence.
	 * Frames are sent with delay to avoid the outof order at receving end
	 */
	if (remote_type == REMOTE_WIFI) {
		if ((rem_wifi_send = rwl_wifi_allocate_actionframe()) == NULL) {
			DPRINT_ERR(ERR, "remote_CDC_tx: Failed to get allocated buffer\n");
			return (FAIL);
		}

		if (buf_len > RWL_WIFI_FRAG_DATA_SIZE) {
			/* response needs to be sent in fragments */
			totalframes = buf_len / RWL_WIFI_FRAG_DATA_SIZE;
			memcpy((char*)&rem_wifi_send->data[RWL_WIFI_CDC_HEADER_OFFSET],
			(char*)rem_ptr,	REMOTE_SIZE);
			memcpy((char*)&rem_wifi_send->data[REMOTE_SIZE], &buf[0],
			RWL_WIFI_FRAG_DATA_SIZE);
			/* update type feild to inform receiver it's frammeted response frame
			*/
			rem_wifi_send->type = RWL_ACTION_WIFI_FRAG_TYPE;
			rem_wifi_send->subtype = RWL_WIFI_DEFAULT_SUBTYPE;

			if ((error = rwl_var_send_vs_actionframe(wl, RWL_WIFI_ACTION_CMD,
				rem_wifi_send, RWL_WIFI_ACTION_FRAME_SIZE)) < 0) {
				DPRINT_DBG(OUTPUT, "Failed to Send the Frame %d\n", error);
				free(rem_wifi_send);
				return error;
			}
			/* Send remaining bytes in fragments */
			for (tx_count = 1; tx_count < totalframes; tx_count++) {
				rem_wifi_send->type = RWL_ACTION_WIFI_FRAG_TYPE;
				rem_wifi_send->subtype = tx_count;
				/* First frame onwards , buf contains only data */
				memcpy((char*)&rem_wifi_send->data,
				&buf[tx_count*RWL_WIFI_FRAG_DATA_SIZE], RWL_WIFI_FRAG_DATA_SIZE);
				if ((error = rwl_var_send_vs_actionframe(wl, RWL_WIFI_ACTION_CMD,
					rem_wifi_send, RWL_WIFI_ACTION_FRAME_SIZE)) < 0) {
					free(rem_wifi_send);
					return error;
				}
				rwl_sleep(RWL_WIFI_SEND_DELAY);
			}

			/* Check for remaing bytes to send */
			if ((totalframes*RWL_WIFI_FRAG_DATA_SIZE) != buf_len) {
				rem_wifi_send->type = RWL_ACTION_WIFI_FRAG_TYPE;
				rem_wifi_send->subtype = tx_count;
				memcpy((char*)&rem_wifi_send->data,
				&buf[tx_count*RWL_WIFI_FRAG_DATA_SIZE],
				(buf_len - (tx_count*RWL_WIFI_FRAG_DATA_SIZE)));
				if ((error = rwl_var_send_vs_actionframe(wl, RWL_WIFI_ACTION_CMD,
					rem_wifi_send, RWL_WIFI_ACTION_FRAME_SIZE)) < 0) {
					free(rem_wifi_send);
					return error;
				}
			}
		} else {
			/* response fits to single frame */
			memcpy((char*)&rem_wifi_send->data[RWL_WIFI_CDC_HEADER_OFFSET],
			(char*)rem_ptr,	REMOTE_SIZE);
			/* when data_len is 0 , buf will be NULL */
			if (buf != NULL) {
				memcpy((char*)&rem_wifi_send->data[REMOTE_SIZE],
				&buf[0], data_len);
			}
			error = rwl_var_send_vs_actionframe(wl, RWL_WIFI_ACTION_CMD, rem_wifi_send,
			RWL_WIFI_ACTION_FRAME_SIZE);
			free(rem_wifi_send);
			return error;
		}
	}
#endif /* RWL_WIFI */
	return (0);
}


rem_ioctl_t *
remote_CDC_rx_hdr(void *remote, int debug)
{
#ifdef RWL_SOCKET
	int ret;
#endif /* RWL_SOCKET */

#if defined(RWL_SERIAL) || defined (RWL_DONGLE) || defined (RWL_SOCKET)
	uint numread = 0;
#endif
	rem_ioctl_t *rem_ptr = &rem_cdc;
	memset(rem_ptr, 0, sizeof(rem_ioctl_t));
	char sync;

	UNUSED_PARAMETER(remote);
	UNUSED_PARAMETER(debug);

	switch (remote_type)	{
#if defined(RWL_SERIAL) || defined (RWL_DONGLE)
		case REMOTE_SERIAL:
		case REMOTE_DONGLE:
		    /* Wait for the synchronization character */
#ifdef DEBUG_SERIAL
		    printf("Waiting on receive sync %\n");
#endif /* ifdef DEBUG_SERIAL */
		    do
		    {
		        if (rwl_read_serial_port(remote, (char *)&sync, 1,
                    &numread) < 0) {
//                    DPRINT_ERR(ERR, "remote_CDC_rx_hdr: Header Read failed \n");
                    return (NULL);
                }
		    }
		    while (sync != '%');
#ifdef DEBUG_SERIAL
            printf("Receive CDC header\n");
#endif /* ifdef DEBUG_SERIAL */
			if (rwl_read_serial_port(remote, (char *)rem_ptr, sizeof(rem_ioctl_t),
				&numread) < 0) {
				DPRINT_ERR(ERR, "remote_CDC_rx_hdr: Header Read failed \n");
				return (NULL);
			}
#ifdef DEBUG_SERIAL
            dump_cdc_hdr( rem_ptr, 0 );
#endif /* ifdef DEBUG_SERIAL */
			break;
#endif /* RWL_SERIAL |  RWL_DONGLE */


#ifdef RWL_SOCKET
		case REMOTE_SOCKET:
			ret = rwl_receive_from_streamsocket(*(int*)remote, (char *)rem_ptr,
				sizeof(rem_ioctl_t), 0);
			numread = ret;
			if (ret == -1) {
				DPRINT_ERR(ERR, "remote_CDC_rx_hdr: numread:%d", numread);
				return (NULL);
			}
		if (numread == 0) {
		      DPRINT_DBG(OUTPUT, "\n remote_CDC_rx_hdr:No data to receive\n");
			return NULL;
			}
			break;
#endif
		default:
			DPRINT_ERR(ERR, "\n Unknown Transport Type\n");
			break;
	}

	return (rem_ptr);
}

/* Return a CDC type buffer */
int
remote_CDC_rx(void *wl, rem_ioctl_t *rem_ptr, uchar *readbuf, uint buflen, int debug)
{
	uint numread = 0;

#ifdef RWL_SOCKET
	int ret;
#endif

#ifdef RWL_WIFI
	UNUSED_PARAMETER(numread);
#endif /* RWL_WIFI */
	UNUSED_PARAMETER(wl);
	UNUSED_PARAMETER(readbuf);
	UNUSED_PARAMETER(buflen);
	UNUSED_PARAMETER(debug);
	UNUSED_PARAMETER(numread);

	if (rem_ptr->data_len > rem_ptr->msg.len) {
		DPRINT_ERR(ERR, "remote_CDC_rx: remote data len (%d) > msg len (%d)\n",
			rem_ptr->data_len, rem_ptr->msg.len);
		return (FAIL);
	}


#if defined (RWL_DONGLE) || defined (RWL_SERIAL)
	if ((remote_type == REMOTE_DONGLE) || (remote_type == REMOTE_SERIAL)) {
#ifdef DEBUG_SERIAL
            printf("Receive data\n");
#endif /* ifdef DEBUG_SERIAL */
		if (rwl_read_serial_port(wl, (char*)readbuf, rem_ptr->data_len,
			&numread) < 0) {
			DPRINT_ERR(ERR, "remote_CDC_rx_hdr: Data Receivefailed \n");
			return (FAIL);
		}
	}
#endif /* RWL_DONGLE || RWL_SERIAL */

#ifdef RWL_SOCKET
	if (remote_type == REMOTE_SOCKET) {
		if (((ret = rwl_receive_from_streamsocket(*(int*)wl, (char*)readbuf,
		rem_ptr->data_len, 0)) == -1)) {
				DPRINT_ERR(ERR, "remote_CDC_rx:Data Receive failed\n");
				return (FAIL);
		}
	}
#endif /* RWL_SOCKET */
	return (SUCCESS);
}
#ifdef RWL_SOCKET
int
rwl_sockconnect(int SockDes, struct sockaddr *servAddr, int size)
{
	DPRINT_DBG(OUTPUT, "sockconnet SockDes=%d\n", SockDes);
	if (rwl_connectsocket(SockDes, servAddr, size) < 0) {
			DPRINT_ERR(ERR, "\n rwl_socketconnect failed\n");
			return FAIL;
	}
	return SUCCESS;
}
#endif /* RWL_SOCKET */
void
rwl_swap_header(rem_ioctl_t *rem_ptr, bool host_to_network)
{
	rem_ptr->msg.cmd = host_to_network?(htol32(rem_ptr->msg.cmd)):(ltoh32(rem_ptr->msg.cmd));
	rem_ptr->msg.len = host_to_network?(htol32(rem_ptr->msg.len)):(ltoh32(rem_ptr->msg.len));
	rem_ptr->msg.flags = host_to_network?(htol32(rem_ptr->msg.flags)):
		(ltoh32(rem_ptr->msg.flags));
	rem_ptr->msg.status = host_to_network?(htol32(rem_ptr->msg.status)):
		(ltoh32(rem_ptr->msg.status));
	rem_ptr->data_len = host_to_network?(htol32(rem_ptr->data_len)):(ltoh32(rem_ptr->data_len));
}
