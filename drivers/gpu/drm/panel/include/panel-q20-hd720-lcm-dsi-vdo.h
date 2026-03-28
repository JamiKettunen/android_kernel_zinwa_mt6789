/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#ifndef PANEL_Q20_HD720_LCM_DSI_VDO
#define PANEL_Q20_HD720_LCM_DSI_VDO

//#define LCM_DSI_CMD_MODE

#ifdef LCM_DSI_CMD_MODE
//#define CMD_HFP_SUPPORT
//#define ENALE_DSC
//#define SUPPORT_FPS_90
#endif

#define REGFLAG_CMD             0xFFFA
#define REGFLAG_DELAY           0xFFFC
#define REGFLAG_UDELAY          0xFFFB
#define REGFLAG_END_OF_TABLE    0xFFFD
#define REGFLAG_RESET_LOW       0xFFFE
#define REGFLAG_RESET_HIGH      0xFFFF

#define FRAME_WIDTH                 720
#define FRAME_HEIGHT                720

#define PHYSICAL_WIDTH              62000
#define PHYSICAL_HEIGHT             62000

#ifdef ENALE_DSC
#define DATA_RATE                   525
#else
#define DATA_RATE                   652
#endif

#define HSA                         50
#define HBP                         40
#define VSA                         5
#define VBP                         35

/*Parameter setting for mode 0 Start*/
#define MODE_0_FPS                  60
#define MODE_0_VFP                  68
#define MODE_0_HFP                  60
#define MODE_0_DATA_RATE            DATA_RATE*2
/*Parameter setting for mode 0 End*/

/*Parameter setting for mode 1 Start*/
#define MODE_1_FPS                  90
#define MODE_1_VFP                  8
#define MODE_1_HFP                  40
#define MODE_1_DATA_RATE            (DATA_RATE*2)
/*Parameter setting for mode 1 End*/

/*Parameter setting for mode 2 Start*/
#define MODE_2_FPS                  120
#define MODE_2_VFP                  8
#define MODE_2_HFP                  40
#define MODE_2_DATA_RATE            (DATA_RATE*2)
/*Parameter setting for mode 2 End*/

/* DSC RELATED */

#define DSC_DISABLE                 0
#define DSC_ENABLE                  1
#define DSC_VER                     17
#define DSC_SLICE_MODE              1
#define DSC_RGB_SWAP                0
#define DSC_DSC_CFG                 34
#define DSC_RCT_ON                  1
#define DSC_BIT_PER_CHANNEL         8
#define DSC_DSC_LINE_BUF_DEPTH      9
#define DSC_BP_ENABLE               1
#define DSC_BIT_PER_PIXEL           128
#define DSC_PIC_HEIGHT            2400
#define DSC_PIC_WIDTH             1080
#define DSC_SLICE_HEIGHT            12
#define DSC_SLICE_WIDTH             540
#define DSC_CHUNK_SIZE              540
#define DSC_XMIT_DELAY              512
#define DSC_DEC_DELAY               526
#define DSC_SCALE_VALUE             32
#define DSC_INCREMENT_INTERVAL     287
#define DSC_DECREMENT_INTERVAL      7
#define DSC_LINE_BPG_OFFSET         12
#define DSC_NFL_BPG_OFFSET          2235
#define DSC_SLICE_BPG_OFFSET       2170
#define DSC_INITIAL_OFFSET          6144
#define DSC_FINAL_OFFSET            4336
#define DSC_FLATNESS_MINQP          3
#define DSC_FLATNESS_MAXQP          12
#define DSC_RC_MODEL_SIZE           8192
#define DSC_RC_EDGE_FACTOR          6
#define DSC_RC_QUANT_INCR_LIMIT0    11
#define DSC_RC_QUANT_INCR_LIMIT1    11
#define DSC_RC_TGT_OFFSET_HI        3
#define DSC_RC_TGT_OFFSET_LO        3

#endif //end of PANEL_Q20_HD720_LCM_DSI_VDO
