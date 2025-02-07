/*
 * Copyright 2014, Broadcom Corporation
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Broadcom Corporation;
 * the contents of this file may not be disclosed to third parties, copied
 * or duplicated in any form, in whole or in part, without the prior
 * written permission of Broadcom Corporation.
 */

/** @file
 *  Provides prototypes / declarations for chip-specific APSTA functionality
 */

#ifndef INCLUDED_WWD_AP_H_
#define INCLUDED_WWD_AP_H_

#include <stdint.h>
#include "wwd_constants.h"

#ifdef __cplusplus
extern "C" {
#endif

extern wiced_bool_t wiced_wifi_ap_is_up;

extern wiced_bool_t wwd_wifi_is_packet_from_ap(uint8_t flags2);

#ifdef __cplusplus
} /*extern "C" */
#endif

#endif /* ifndef INCLUDED_WWD_AP_H_ */
