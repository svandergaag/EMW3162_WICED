/*
 * Copyright 2014, Broadcom Corporation
 * All Rights Reserved.
 *
 * This is UNPUBLISHED PROPRIETARY SOURCE CODE of Broadcom Corporation;
 * the contents of this file may not be disclosed to third parties, copied
 * or duplicated in any form, in whole or in part, without the prior
 * written permission of Broadcom Corporation.
 */

/**
 * @file
 * This is the main file for the manufacturing test app
 *
 * To use the manufacturing test application, please
 * read the instructions in the WICED Manufacturing
 * Test User Guide provided in the <WICED-SDK>/Doc
 * directory: WICED-MFG2xx-R.pdf
 *
 */

#include <stdio.h>
#include "wiced.h"
#include "mfg_test.h"
#include "internal/wwd_internal.h"

/******************************************************
 *                      Macros
 ******************************************************/

/******************************************************
 *                    Constants
 ******************************************************/

/******************************************************
 *                   Enumerations
 ******************************************************/

/******************************************************
 *                 Type Definitions
 ******************************************************/

/******************************************************
 *                    Structures
 ******************************************************/

/******************************************************
 *               Static Function Declarations
 ******************************************************/

/******************************************************
 *               Variable Definitions
 ******************************************************/

/******************************************************
 *               Function Definitions
 ******************************************************/
void application_start( )
{
   int argc = 2;
   char *argv[] = { "", "" };

   wiced_init( );

   /* Run the main application function
    * turn off buffers, so IO occurs immediately
    */
   setvbuf(stdin, NULL, _IONBF, 0);
   setvbuf(stdout, NULL, _IONBF, 0);
   setvbuf(stderr, NULL, _IONBF, 0);

   wiced_platform_mcu_enable_powersave( );

   WWD_WLAN_KEEP_AWAKE( );

   /* Main server process for all transport types */
   remote_server_exec(argc, argv, NULL);

}

