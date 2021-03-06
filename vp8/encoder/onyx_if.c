/*
 *  Copyright (c) 2010 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */


#include "onyxc_int.h"
#include "onyx_int.h"
#include "systemdependent.h"
#include "quantize.h"
#include "alloccommon.h"
#include "mcomp.h"
#include "firstpass.h"
#include "psnr.h"
#include "vpx_scale/vpxscale.h"
#include "extend.h"
#include "ratectrl.h"
#include "quant_common.h"
#include "segmentation.h"
#include "g_common.h"
#include "vpx_scale/yv12extend.h"
#include "postproc.h"
#include "vpx_mem/vpx_mem.h"
#include "swapyv12buffer.h"
#include "threading.h"
#include "vpx_ports/vpx_timer.h"
#include "vpxerrors.h"
#include "temporal_filter.h"
#if ARCH_ARM
#include "vpx_ports/arm.h"
#endif

#include <math.h>
#include <stdio.h>
#include <limits.h>

#if CONFIG_RUNTIME_CPU_DETECT
#define IF_RTCD(x) (x)
#define RTCD(x) &cpi->common.rtcd.x
#else
#define IF_RTCD(x) NULL
#define RTCD(x) NULL
#endif

extern void vp8cx_init_mv_bits_sadcost();
extern void vp8cx_pick_filter_level_fast(YV12_BUFFER_CONFIG *sd, VP8_COMP *cpi);
extern void vp8cx_set_alt_lf_level(VP8_COMP *cpi, int filt_val);
extern void vp8cx_pick_filter_level(YV12_BUFFER_CONFIG *sd, VP8_COMP *cpi);

extern void vp8_init_loop_filter(VP8_COMMON *cm);
extern void vp8_loop_filter_frame(VP8_COMMON *cm,    MACROBLOCKD *mbd,  int filt_val);
extern void vp8_loop_filter_frame_yonly(VP8_COMMON *cm,    MACROBLOCKD *mbd,  int filt_val, int sharpness_lvl);
extern void vp8_dmachine_specific_config(VP8_COMP *cpi);
extern void vp8_cmachine_specific_config(VP8_COMP *cpi);
extern void vp8_calc_auto_iframe_target_size(VP8_COMP *cpi);
extern void vp8_deblock_frame(YV12_BUFFER_CONFIG *source, YV12_BUFFER_CONFIG *post, int filt_lvl, int low_var_thresh, int flag);
extern void print_parms(VP8_CONFIG *ocf, char *filenam);
extern unsigned int vp8_get_processor_freq();
extern void print_tree_update_probs();
extern void vp8cx_create_encoder_threads(VP8_COMP *cpi);
extern void vp8cx_remove_encoder_threads(VP8_COMP *cpi);
#if HAVE_ARMV7
extern void vp8_yv12_copy_frame_func_neon(YV12_BUFFER_CONFIG *src_ybc, YV12_BUFFER_CONFIG *dst_ybc);
extern void vp8_yv12_copy_src_frame_func_neon(YV12_BUFFER_CONFIG *src_ybc, YV12_BUFFER_CONFIG *dst_ybc);
#endif

int vp8_estimate_entropy_savings(VP8_COMP *cpi);
int vp8_calc_ss_err(YV12_BUFFER_CONFIG *source, YV12_BUFFER_CONFIG *dest, const vp8_variance_rtcd_vtable_t *rtcd);
int vp8_calc_low_ss_err(YV12_BUFFER_CONFIG *source, YV12_BUFFER_CONFIG *dest, const vp8_variance_rtcd_vtable_t *rtcd);


static void set_default_lf_deltas(VP8_COMP *cpi);

extern const int vp8_gf_interval_table[101];

#if CONFIG_PSNR
#include "math.h"

extern double vp8_calc_ssim
(
    YV12_BUFFER_CONFIG *source,
    YV12_BUFFER_CONFIG *dest,
    int lumamask,
    double *weight
);

extern double vp8_calc_ssimg
(
    YV12_BUFFER_CONFIG *source,
    YV12_BUFFER_CONFIG *dest,
    double *ssim_y,
    double *ssim_u,
    double *ssim_v
);


#endif


#ifdef OUTPUT_YUV_SRC
FILE *yuv_file;
#endif

#if 0
FILE *framepsnr;
FILE *kf_list;
FILE *keyfile;
#endif

#if 0
extern int skip_true_count;
extern int skip_false_count;
#endif


#ifdef ENTROPY_STATS
extern int intra_mode_stats[10][10][10];
#endif

#ifdef SPEEDSTATS
unsigned int frames_at_speed[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
unsigned int tot_pm = 0;
unsigned int cnt_pm = 0;
unsigned int tot_ef = 0;
unsigned int cnt_ef = 0;
#endif

#ifdef MODE_STATS
extern unsigned __int64 Sectionbits[50];
extern int y_modes[5]  ;
extern int uv_modes[4] ;
extern int b_modes[10]  ;

extern int inter_y_modes[10] ;
extern int inter_uv_modes[4] ;
extern unsigned int inter_b_modes[15];
#endif

extern void (*vp8_short_fdct4x4)(short *input, short *output, int pitch);
extern void (*vp8_short_fdct8x4)(short *input, short *output, int pitch);

extern const int vp8_bits_per_mb[2][QINDEX_RANGE];

extern const int qrounding_factors[129];
extern const int qzbin_factors[129];
extern void vp8cx_init_quantizer(VP8_COMP *cpi);
extern const int vp8cx_base_skip_false_prob[128];

// Tables relating active max Q to active min Q
static const int kf_low_motion_minq[QINDEX_RANGE] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
    5, 5, 5, 6, 6, 6, 7, 7, 7, 8, 8, 8, 9, 9, 10,10,
    11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18,
    19,19,20,20,21,21,22,22,23,23,24,24,25,25,26,26,
    27,27,28,28,29,29,30,30,31,32,33,34,35,36,37,38,
};
static const int kf_high_motion_minq[QINDEX_RANGE] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5,
    6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 9, 9, 9, 10,10,
    11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18,
    19,19,20,20,21,21,22,22,23,23,24,24,25,25,26,26,
    27,27,28,28,29,29,30,30,31,31,32,32,33,33,34,34,
    35,35,36,36,37,38,39,40,41,42,43,44,45,46,47,48,
};
/*static const int kf_minq[QINDEX_RANGE] =
{
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10,10,11,11,12,12,13,13,14,14,
    15,15,16,16,17,17,18,18,19,19,20,20,21,21,22,22,
    23,23,24,24,25,25,26,26,27,27,28,28,29,29,30,30,
    31,31,32,32,33,33,34,34,35,35,36,36,37,37,38,38
};*/
static const int gf_low_motion_minq[QINDEX_RANGE] =
{
    0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,
    3,3,3,3,4,4,4,4,5,5,5,5,6,6,6,6,
    7,7,7,7,8,8,8,8,9,9,9,9,10,10,10,10,
    11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18,
    19,19,20,20,21,21,22,22,23,23,24,24,25,25,26,26,
    27,27,28,28,29,29,30,30,31,31,32,32,33,33,34,34,
    35,35,36,36,37,37,38,38,39,39,40,40,41,41,42,42,
    43,44,45,46,47,48,49,50,51,52,53,54,55,56,57,58
};
static const int gf_mid_motion_minq[QINDEX_RANGE] =
{
    0,0,0,0,1,1,1,1,1,1,2,2,3,3,3,4,
    4,4,5,5,5,6,6,6,7,7,7,8,8,8,9,9,
    9,10,10,10,10,11,11,11,12,12,12,12,13,13,13,14,
    14,14,15,15,16,16,17,17,18,18,19,19,20,20,21,21,
    22,22,23,23,24,24,25,25,26,26,27,27,28,28,29,29,
    30,30,31,31,32,32,33,33,34,34,35,35,36,36,37,37,
    38,39,39,40,40,41,41,42,42,43,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
};
static const int gf_high_motion_minq[QINDEX_RANGE] =
{
    0,0,0,0,1,1,1,1,1,2,2,2,3,3,3,4,
    4,4,5,5,5,6,6,6,7,7,7,8,8,8,9,9,
    9,10,10,10,11,11,12,12,13,13,14,14,15,15,16,16,
    17,17,18,18,19,19,20,20,21,21,22,22,23,23,24,24,
    25,25,26,26,27,27,28,28,29,29,30,30,31,31,32,32,
    33,33,34,34,35,35,36,36,37,37,38,38,39,39,40,40,
    41,41,42,42,43,44,45,46,47,48,49,50,51,52,53,54,
    55,56,57,58,59,60,62,64,66,68,70,72,74,76,78,80,
};
/*static const int gf_arf_minq[QINDEX_RANGE] =
{
    0,0,0,0,1,1,1,1,1,1,2,2,3,3,3,4,
    4,4,5,5,5,6,6,6,7,7,7,8,8,8,9,9,
    9,10,10,10,11,11,11,12,12,12,13,13,13,14,14,14,
    15,15,16,16,17,17,18,18,19,19,20,20,21,21,22,22,
    23,23,24,24,25,25,26,26,27,27,28,28,29,29,30,30,
    31,31,32,32,33,33,34,34,35,35,36,36,37,37,38,39,
    39,40,40,41,41,42,42,43,43,44,45,46,47,48,49,50,
    51,52,53,54,55,56,57,58,59,60,61,62,63,64,65,66
};*/
static const int inter_minq[QINDEX_RANGE] =
{
    0,0,0,0,1,1,2,3,3,4,4,5,6,6,7,7,
    8,8,9,9,10,11,11,12,12,13,13,14,14,15,15,16,
    16,17,17,17,18,18,19,19,20,20,21,21,22,22,22,23,
    23,24,24,24,25,25,26,27,28,28,29,30,31,32,33,34,
    35,35,36,37,38,39,39,40,41,42,43,43,44,45,46,47,
    47,48,49,49,51,52,53,54,54,55,56,56,57,57,58,58,
    59,59,60,61,61,62,62,63,64,64,65,66,67,67,68,69,
    69,70,71,71,72,73,74,75,76,76,77,78,79,80,81,81,
};

void vp8_initialize()
{
    static int init_done = 0;

    if (!init_done)
    {
        vp8_scale_machine_specific_config();
        vp8_initialize_common();
        //vp8_dmachine_specific_config();
        vp8_tokenize_initialize();

        vp8cx_init_mv_bits_sadcost();
        init_done = 1;
    }
}
#ifdef PACKET_TESTING
extern FILE *vpxlogc;
#endif

static void setup_features(VP8_COMP *cpi)
{
    // Set up default state for MB feature flags
    cpi->mb.e_mbd.segmentation_enabled = 0;
    cpi->mb.e_mbd.update_mb_segmentation_map = 0;
    cpi->mb.e_mbd.update_mb_segmentation_data = 0;
    vpx_memset(cpi->mb.e_mbd.mb_segment_tree_probs, 255, sizeof(cpi->mb.e_mbd.mb_segment_tree_probs));
    vpx_memset(cpi->mb.e_mbd.segment_feature_data, 0, sizeof(cpi->mb.e_mbd.segment_feature_data));

    cpi->mb.e_mbd.mode_ref_lf_delta_enabled = 0;
    cpi->mb.e_mbd.mode_ref_lf_delta_update = 0;
    vpx_memset(cpi->mb.e_mbd.ref_lf_deltas, 0, sizeof(cpi->mb.e_mbd.ref_lf_deltas));
    vpx_memset(cpi->mb.e_mbd.mode_lf_deltas, 0, sizeof(cpi->mb.e_mbd.mode_lf_deltas));
    vpx_memset(cpi->mb.e_mbd.last_ref_lf_deltas, 0, sizeof(cpi->mb.e_mbd.ref_lf_deltas));
    vpx_memset(cpi->mb.e_mbd.last_mode_lf_deltas, 0, sizeof(cpi->mb.e_mbd.mode_lf_deltas));

    set_default_lf_deltas(cpi);

}


void vp8_dealloc_compressor_data(VP8_COMP *cpi)
{

    // Delete sementation map
    if (cpi->segmentation_map != 0)
        vpx_free(cpi->segmentation_map);

    cpi->segmentation_map = 0;

    if (cpi->active_map != 0)
        vpx_free(cpi->active_map);

    cpi->active_map = 0;

    // Delete first pass motion map
    if (cpi->fp_motion_map != 0)
        vpx_free(cpi->fp_motion_map);

    cpi->fp_motion_map = 0;

    vp8_de_alloc_frame_buffers(&cpi->common);

    vp8_yv12_de_alloc_frame_buffer(&cpi->last_frame_uf);
    vp8_yv12_de_alloc_frame_buffer(&cpi->scaled_source);
#if VP8_TEMPORAL_ALT_REF
    vp8_yv12_de_alloc_frame_buffer(&cpi->alt_ref_buffer.source_buffer);
#endif
    {
        int i;

        for (i = 0; i < MAX_LAG_BUFFERS; i++)
            vp8_yv12_de_alloc_frame_buffer(&cpi->src_buffer[i].source_buffer);

        cpi->source_buffer_count = 0;
    }

    vpx_free(cpi->tok);
    cpi->tok = 0;

    // Structure used to minitor GF useage
    if (cpi->gf_active_flags != 0)
        vpx_free(cpi->gf_active_flags);

    cpi->gf_active_flags = 0;

    if(cpi->mb.pip)
        vpx_free(cpi->mb.pip);

    cpi->mb.pip = 0;

    vpx_free(cpi->total_stats);
    vpx_free(cpi->this_frame_stats);
}

static void enable_segmentation(VP8_PTR ptr)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);

    // Set the appropriate feature bit
    cpi->mb.e_mbd.segmentation_enabled = 1;
    cpi->mb.e_mbd.update_mb_segmentation_map = 1;
    cpi->mb.e_mbd.update_mb_segmentation_data = 1;
}
static void disable_segmentation(VP8_PTR ptr)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);

    // Clear the appropriate feature bit
    cpi->mb.e_mbd.segmentation_enabled = 0;
}

// Valid values for a segment are 0 to 3
// Segmentation map is arrange as [Rows][Columns]
static void set_segmentation_map(VP8_PTR ptr, unsigned char *segmentation_map)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);

    // Copy in the new segmentation map
    vpx_memcpy(cpi->segmentation_map, segmentation_map, (cpi->common.mb_rows * cpi->common.mb_cols));

    // Signal that the map should be updated.
    cpi->mb.e_mbd.update_mb_segmentation_map = 1;
    cpi->mb.e_mbd.update_mb_segmentation_data = 1;
}

// The values given for each segment can be either deltas (from the default value chosen for the frame) or absolute values.
//
// Valid range for abs values is (0-127 for MB_LVL_ALT_Q) , (0-63 for SEGMENT_ALT_LF)
// Valid range for delta values are (+/-127 for MB_LVL_ALT_Q) , (+/-63 for SEGMENT_ALT_LF)
//
// abs_delta = SEGMENT_DELTADATA (deltas) abs_delta = SEGMENT_ABSDATA (use the absolute values given).
//
//
static void set_segment_data(VP8_PTR ptr, signed char *feature_data, unsigned char abs_delta)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);

    cpi->mb.e_mbd.mb_segement_abs_delta = abs_delta;
    vpx_memcpy(cpi->segment_feature_data, feature_data, sizeof(cpi->segment_feature_data));
}


static void segmentation_test_function(VP8_PTR ptr)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);

    unsigned char *seg_map;
    signed char feature_data[MB_LVL_MAX][MAX_MB_SEGMENTS];

    // Create a temporary map for segmentation data.
    CHECK_MEM_ERROR(seg_map, vpx_calloc(cpi->common.mb_rows * cpi->common.mb_cols, 1));

    // MB loop to set local segmentation map
    /*for ( i = 0; i < cpi->common.mb_rows; i++ )
    {
        for ( j = 0; j < cpi->common.mb_cols; j++ )
        {
            //seg_map[(i*cpi->common.mb_cols) + j] = (j % 2) + ((i%2)* 2);
            //if ( j < cpi->common.mb_cols/2 )

            // Segment 1 around the edge else 0
            if ( (i == 0) || (j == 0) || (i == (cpi->common.mb_rows-1)) || (j == (cpi->common.mb_cols-1)) )
                seg_map[(i*cpi->common.mb_cols) + j] = 1;
            //else if ( (i < 2) || (j < 2) || (i > (cpi->common.mb_rows-3)) || (j > (cpi->common.mb_cols-3)) )
            //  seg_map[(i*cpi->common.mb_cols) + j] = 2;
            //else if ( (i < 5) || (j < 5) || (i > (cpi->common.mb_rows-6)) || (j > (cpi->common.mb_cols-6)) )
            //  seg_map[(i*cpi->common.mb_cols) + j] = 3;
            else
                seg_map[(i*cpi->common.mb_cols) + j] = 0;
        }
    }*/

    // Set the segmentation Map
    set_segmentation_map(ptr, seg_map);

    // Activate segmentation.
    enable_segmentation(ptr);

    // Set up the quant segment data
    feature_data[MB_LVL_ALT_Q][0] = 0;
    feature_data[MB_LVL_ALT_Q][1] = 4;
    feature_data[MB_LVL_ALT_Q][2] = 0;
    feature_data[MB_LVL_ALT_Q][3] = 0;
    // Set up the loop segment data
    feature_data[MB_LVL_ALT_LF][0] = 0;
    feature_data[MB_LVL_ALT_LF][1] = 0;
    feature_data[MB_LVL_ALT_LF][2] = 0;
    feature_data[MB_LVL_ALT_LF][3] = 0;

    // Initialise the feature data structure
    // SEGMENT_DELTADATA    0, SEGMENT_ABSDATA      1
    set_segment_data(ptr, &feature_data[0][0], SEGMENT_DELTADATA);

    // Delete sementation map
    if (seg_map != 0)
        vpx_free(seg_map);

    seg_map = 0;

}

// A simple function to cyclically refresh the background at a lower Q
static void cyclic_background_refresh(VP8_COMP *cpi, int Q, int lf_adjustment)
{
    unsigned char *seg_map;
    signed char feature_data[MB_LVL_MAX][MAX_MB_SEGMENTS];
    int i;
    int block_count = cpi->cyclic_refresh_mode_max_mbs_perframe;
    int mbs_in_frame = cpi->common.mb_rows * cpi->common.mb_cols;

    // Create a temporary map for segmentation data.
    CHECK_MEM_ERROR(seg_map, vpx_calloc(cpi->common.mb_rows * cpi->common.mb_cols, 1));

    cpi->cyclic_refresh_q = Q;

    for (i = Q; i > 0; i--)
    {
        if (vp8_bits_per_mb[cpi->common.frame_type][i] >= ((vp8_bits_per_mb[cpi->common.frame_type][Q]*(Q + 128)) / 64))
            //if ( vp8_bits_per_mb[cpi->common.frame_type][i] >= ((vp8_bits_per_mb[cpi->common.frame_type][Q]*((2*Q)+96))/64) )
        {
            break;
        }
    }

    cpi->cyclic_refresh_q = i;

    // Only update for inter frames
    if (cpi->common.frame_type != KEY_FRAME)
    {
        // Cycle through the macro_block rows
        // MB loop to set local segmentation map
        for (i = cpi->cyclic_refresh_mode_index; i < mbs_in_frame; i++)
        {
            // If the MB is as a candidate for clean up then mark it for possible boost/refresh (segment 1)
            // The segment id may get reset to 0 later if the MB gets coded anything other than last frame 0,0
            // as only (last frame 0,0) MBs are eligable for refresh : that is to say Mbs likely to be background blocks.
            if (cpi->cyclic_refresh_map[i] == 0)
            {
                seg_map[i] = 1;
            }
            else
            {
                seg_map[i] = 0;

                // Skip blocks that have been refreshed recently anyway.
                if (cpi->cyclic_refresh_map[i] < 0)
                    //cpi->cyclic_refresh_map[i] = cpi->cyclic_refresh_map[i] / 16;
                    cpi->cyclic_refresh_map[i]++;
            }


            if (block_count > 0)
                block_count--;
            else
                break;

        }

        // If we have gone through the frame reset to the start
        cpi->cyclic_refresh_mode_index = i;

        if (cpi->cyclic_refresh_mode_index >= mbs_in_frame)
            cpi->cyclic_refresh_mode_index = 0;
    }

    // Set the segmentation Map
    set_segmentation_map((VP8_PTR)cpi, seg_map);

    // Activate segmentation.
    enable_segmentation((VP8_PTR)cpi);

    // Set up the quant segment data
    feature_data[MB_LVL_ALT_Q][0] = 0;
    feature_data[MB_LVL_ALT_Q][1] = (cpi->cyclic_refresh_q - Q);
    feature_data[MB_LVL_ALT_Q][2] = 0;
    feature_data[MB_LVL_ALT_Q][3] = 0;

    // Set up the loop segment data
    feature_data[MB_LVL_ALT_LF][0] = 0;
    feature_data[MB_LVL_ALT_LF][1] = lf_adjustment;
    feature_data[MB_LVL_ALT_LF][2] = 0;
    feature_data[MB_LVL_ALT_LF][3] = 0;

    // Initialise the feature data structure
    // SEGMENT_DELTADATA    0, SEGMENT_ABSDATA      1
    set_segment_data((VP8_PTR)cpi, &feature_data[0][0], SEGMENT_DELTADATA);

    // Delete sementation map
    if (seg_map != 0)
        vpx_free(seg_map);

    seg_map = 0;

}

static void set_default_lf_deltas(VP8_COMP *cpi)
{
    cpi->mb.e_mbd.mode_ref_lf_delta_enabled = 1;
    cpi->mb.e_mbd.mode_ref_lf_delta_update = 1;

    vpx_memset(cpi->mb.e_mbd.ref_lf_deltas, 0, sizeof(cpi->mb.e_mbd.ref_lf_deltas));
    vpx_memset(cpi->mb.e_mbd.mode_lf_deltas, 0, sizeof(cpi->mb.e_mbd.mode_lf_deltas));

    // Test of ref frame deltas
    cpi->mb.e_mbd.ref_lf_deltas[INTRA_FRAME] = 2;
    cpi->mb.e_mbd.ref_lf_deltas[LAST_FRAME] = 0;
    cpi->mb.e_mbd.ref_lf_deltas[GOLDEN_FRAME] = -2;
    cpi->mb.e_mbd.ref_lf_deltas[ALTREF_FRAME] = -2;

    cpi->mb.e_mbd.mode_lf_deltas[0] = 4;               // BPRED
    cpi->mb.e_mbd.mode_lf_deltas[1] = -2;              // Zero
    cpi->mb.e_mbd.mode_lf_deltas[2] = 2;               // New mv
    cpi->mb.e_mbd.mode_lf_deltas[3] = 4;               // Split mv
}

void vp8_set_speed_features(VP8_COMP *cpi)
{
    SPEED_FEATURES *sf = &cpi->sf;
    int Mode = cpi->compressor_speed;
    int Speed = cpi->Speed;
    int i;
    VP8_COMMON *cm = &cpi->common;

    // Initialise default mode frequency sampling variables
    for (i = 0; i < MAX_MODES; i ++)
    {
        cpi->mode_check_freq[i] = 0;
        cpi->mode_test_hit_counts[i] = 0;
        cpi->mode_chosen_counts[i] = 0;
    }

    cpi->mbs_tested_so_far = 0;

    // best quality
    sf->RD = 1;
    sf->search_method = NSTEP;
    sf->improved_quant = 1;
    sf->improved_dct = 1;
    sf->auto_filter = 1;
    sf->recode_loop = 1;
    sf->quarter_pixel_search = 1;
    sf->half_pixel_search = 1;
    sf->full_freq[0] = 7;
    sf->full_freq[1] = 7;
    sf->min_fs_radius = 8;
    sf->max_fs_radius = 32;
    sf->iterative_sub_pixel = 1;
    sf->optimize_coefficients = 1;

    sf->first_step = 0;
    sf->max_step_search_steps = MAX_MVSEARCH_STEPS;

    cpi->do_full[0] = 0;
    cpi->do_full[1] = 0;

    // default thresholds to 0
    for (i = 0; i < MAX_MODES; i++)
        sf->thresh_mult[i] = 0;

    switch (Mode)
    {
#if !(CONFIG_REALTIME_ONLY)
    case 0: // best quality mode
        sf->thresh_mult[THR_ZEROMV   ] = 0;
        sf->thresh_mult[THR_ZEROG    ] = 0;
        sf->thresh_mult[THR_ZEROA    ] = 0;
        sf->thresh_mult[THR_NEARESTMV] = 0;
        sf->thresh_mult[THR_NEARESTG ] = 0;
        sf->thresh_mult[THR_NEARESTA ] = 0;
        sf->thresh_mult[THR_NEARMV   ] = 0;
        sf->thresh_mult[THR_NEARG    ] = 0;
        sf->thresh_mult[THR_NEARA    ] = 0;

        sf->thresh_mult[THR_DC       ] = 0;

        sf->thresh_mult[THR_V_PRED   ] = 1000;
        sf->thresh_mult[THR_H_PRED   ] = 1000;
        sf->thresh_mult[THR_B_PRED   ] = 2000;
        sf->thresh_mult[THR_TM       ] = 1000;

        sf->thresh_mult[THR_NEWMV    ] = 1000;
        sf->thresh_mult[THR_NEWG     ] = 1000;
        sf->thresh_mult[THR_NEWA     ] = 1000;

        sf->thresh_mult[THR_SPLITMV  ] = 2500;
        sf->thresh_mult[THR_SPLITG   ] = 5000;
        sf->thresh_mult[THR_SPLITA   ] = 5000;

        sf->full_freq[0] = 7;
        sf->full_freq[1] = 15;

        sf->first_step = 0;
        sf->max_step_search_steps = MAX_MVSEARCH_STEPS;

        if (!(cpi->ref_frame_flags & VP8_LAST_FLAG))
        {
            sf->thresh_mult[THR_NEWMV    ] = INT_MAX;
            sf->thresh_mult[THR_NEARESTMV] = INT_MAX;
            sf->thresh_mult[THR_ZEROMV   ] = INT_MAX;
            sf->thresh_mult[THR_NEARMV   ] = INT_MAX;
            sf->thresh_mult[THR_SPLITMV  ] = INT_MAX;
        }

        if (!(cpi->ref_frame_flags & VP8_GOLD_FLAG))
        {
            sf->thresh_mult[THR_NEARESTG ] = INT_MAX;
            sf->thresh_mult[THR_ZEROG    ] = INT_MAX;
            sf->thresh_mult[THR_NEARG    ] = INT_MAX;
            sf->thresh_mult[THR_NEWG     ] = INT_MAX;
            sf->thresh_mult[THR_SPLITG   ] = INT_MAX;
        }

        if (!(cpi->ref_frame_flags & VP8_ALT_FLAG))
        {
            sf->thresh_mult[THR_NEARESTA ] = INT_MAX;
            sf->thresh_mult[THR_ZEROA    ] = INT_MAX;
            sf->thresh_mult[THR_NEARA    ] = INT_MAX;
            sf->thresh_mult[THR_NEWA     ] = INT_MAX;
            sf->thresh_mult[THR_SPLITA   ] = INT_MAX;
        }

        break;
    case 1:
    case 3:
        sf->thresh_mult[THR_NEARESTMV] = 0;
        sf->thresh_mult[THR_ZEROMV   ] = 0;
        sf->thresh_mult[THR_DC       ] = 0;
        sf->thresh_mult[THR_NEARMV   ] = 0;
        sf->thresh_mult[THR_V_PRED   ] = 1000;
        sf->thresh_mult[THR_H_PRED   ] = 1000;
        sf->thresh_mult[THR_B_PRED   ] = 2500;
        sf->thresh_mult[THR_TM       ] = 1000;

        sf->thresh_mult[THR_NEARESTG ] = 1000;
        sf->thresh_mult[THR_NEARESTA ] = 1000;

        sf->thresh_mult[THR_ZEROG    ] = 1000;
        sf->thresh_mult[THR_ZEROA    ] = 1000;
        sf->thresh_mult[THR_NEARG    ] = 1000;
        sf->thresh_mult[THR_NEARA    ] = 1000;

        sf->thresh_mult[THR_NEWMV    ] = 1500;
        sf->thresh_mult[THR_NEWG     ] = 1500;
        sf->thresh_mult[THR_NEWA     ] = 1500;

        sf->thresh_mult[THR_SPLITMV  ] = 5000;
        sf->thresh_mult[THR_SPLITG   ] = 10000;
        sf->thresh_mult[THR_SPLITA   ] = 10000;

        sf->full_freq[0] = 15;
        sf->full_freq[1] = 31;

        sf->first_step = 0;
        sf->max_step_search_steps = MAX_MVSEARCH_STEPS;

        if (!(cpi->ref_frame_flags & VP8_LAST_FLAG))
        {
            sf->thresh_mult[THR_NEWMV    ] = INT_MAX;
            sf->thresh_mult[THR_NEARESTMV] = INT_MAX;
            sf->thresh_mult[THR_ZEROMV   ] = INT_MAX;
            sf->thresh_mult[THR_NEARMV   ] = INT_MAX;
            sf->thresh_mult[THR_SPLITMV  ] = INT_MAX;
        }

        if (!(cpi->ref_frame_flags & VP8_GOLD_FLAG))
        {
            sf->thresh_mult[THR_NEARESTG ] = INT_MAX;
            sf->thresh_mult[THR_ZEROG    ] = INT_MAX;
            sf->thresh_mult[THR_NEARG    ] = INT_MAX;
            sf->thresh_mult[THR_NEWG     ] = INT_MAX;
            sf->thresh_mult[THR_SPLITG   ] = INT_MAX;
        }

        if (!(cpi->ref_frame_flags & VP8_ALT_FLAG))
        {
            sf->thresh_mult[THR_NEARESTA ] = INT_MAX;
            sf->thresh_mult[THR_ZEROA    ] = INT_MAX;
            sf->thresh_mult[THR_NEARA    ] = INT_MAX;
            sf->thresh_mult[THR_NEWA     ] = INT_MAX;
            sf->thresh_mult[THR_SPLITA   ] = INT_MAX;
        }

        if (Speed > 0)
        {
            // Disable coefficient optimization above speed 0
            sf->optimize_coefficients = 0;

            cpi->mode_check_freq[THR_SPLITG] = 4;
            cpi->mode_check_freq[THR_SPLITA] = 4;
            cpi->mode_check_freq[THR_SPLITMV] = 2;

            sf->thresh_mult[THR_TM       ] = 1500;
            sf->thresh_mult[THR_V_PRED   ] = 1500;
            sf->thresh_mult[THR_H_PRED   ] = 1500;
            sf->thresh_mult[THR_B_PRED   ] = 5000;

            if (cpi->ref_frame_flags & VP8_LAST_FLAG)
            {
                sf->thresh_mult[THR_NEWMV    ] = 2000;
                sf->thresh_mult[THR_SPLITMV  ] = 10000;
            }

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                sf->thresh_mult[THR_NEARESTG ] = 1500;
                sf->thresh_mult[THR_ZEROG    ] = 1500;
                sf->thresh_mult[THR_NEARG    ] = 1500;
                sf->thresh_mult[THR_NEWG     ] = 2000;
                sf->thresh_mult[THR_SPLITG   ] = 20000;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                sf->thresh_mult[THR_NEARESTA ] = 1500;
                sf->thresh_mult[THR_ZEROA    ] = 1500;
                sf->thresh_mult[THR_NEARA    ] = 1500;
                sf->thresh_mult[THR_NEWA     ] = 2000;
                sf->thresh_mult[THR_SPLITA   ] = 20000;
            }

            sf->improved_quant = 0;
            sf->improved_dct = 0;

            sf->first_step = 1;
            sf->max_step_search_steps = MAX_MVSEARCH_STEPS;
        }

        if (Speed > 1)
        {
            cpi->mode_check_freq[THR_SPLITG] = 15;
            cpi->mode_check_freq[THR_SPLITA] = 15;
            cpi->mode_check_freq[THR_SPLITMV] = 7;

            sf->thresh_mult[THR_TM       ] = 2000;
            sf->thresh_mult[THR_V_PRED   ] = 2000;
            sf->thresh_mult[THR_H_PRED   ] = 2000;
            sf->thresh_mult[THR_B_PRED   ] = 7500;

            if (cpi->ref_frame_flags & VP8_LAST_FLAG)
            {
                sf->thresh_mult[THR_NEWMV    ] = 2000;
                sf->thresh_mult[THR_SPLITMV  ] = 25000;
            }

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                sf->thresh_mult[THR_NEARESTG ] = 2000;
                sf->thresh_mult[THR_ZEROG    ] = 2000;
                sf->thresh_mult[THR_NEARG    ] = 2000;
                sf->thresh_mult[THR_NEWG     ] = 2500;
                sf->thresh_mult[THR_SPLITG   ] = 50000;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                sf->thresh_mult[THR_NEARESTA ] = 2000;
                sf->thresh_mult[THR_ZEROA    ] = 2000;
                sf->thresh_mult[THR_NEARA    ] = 2000;
                sf->thresh_mult[THR_NEWA     ] = 2500;
                sf->thresh_mult[THR_SPLITA   ] = 50000;
            }

            // Only do recode loop on key frames and golden frames
            sf->recode_loop = 2;

            sf->full_freq[0] = 31;
            sf->full_freq[1] = 63;

        }

        if (Speed > 2)
        {
            sf->auto_filter = 0;                     // Faster selection of loop filter
            cpi->mode_check_freq[THR_V_PRED] = 2;
            cpi->mode_check_freq[THR_H_PRED] = 2;
            cpi->mode_check_freq[THR_B_PRED] = 2;

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                cpi->mode_check_freq[THR_NEARG] = 2;
                cpi->mode_check_freq[THR_NEWG] = 4;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                cpi->mode_check_freq[THR_NEARA] = 2;
                cpi->mode_check_freq[THR_NEWA] = 4;
            }

            sf->thresh_mult[THR_SPLITA  ] = INT_MAX;
            sf->thresh_mult[THR_SPLITG  ] = INT_MAX;
            sf->thresh_mult[THR_SPLITMV  ] = INT_MAX;

            sf->full_freq[0] = 63;
            sf->full_freq[1] = 127;
        }

        if (Speed > 3)
        {
            cpi->mode_check_freq[THR_V_PRED] = 0;
            cpi->mode_check_freq[THR_H_PRED] = 0;
            cpi->mode_check_freq[THR_B_PRED] = 0;
            cpi->mode_check_freq[THR_NEARG] = 0;
            cpi->mode_check_freq[THR_NEWG] = 0;
            cpi->mode_check_freq[THR_NEARA] = 0;
            cpi->mode_check_freq[THR_NEWA] = 0;

            sf->auto_filter = 1;
            sf->recode_loop = 0; // recode loop off
            sf->RD = 0;         // Turn rd off
            sf->full_freq[0] = INT_MAX;
            sf->full_freq[1] = INT_MAX;
        }

        if (Speed > 4)
        {
            sf->auto_filter = 0;                     // Faster selection of loop filter

            cpi->mode_check_freq[THR_V_PRED] = 2;
            cpi->mode_check_freq[THR_H_PRED] = 2;
            cpi->mode_check_freq[THR_B_PRED] = 2;

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                cpi->mode_check_freq[THR_NEARG] = 2;
                cpi->mode_check_freq[THR_NEWG] = 4;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                cpi->mode_check_freq[THR_NEARA] = 2;
                cpi->mode_check_freq[THR_NEWA] = 4;
            }

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                sf->thresh_mult[THR_NEARESTG ] = 2000;
                sf->thresh_mult[THR_ZEROG    ] = 2000;
                sf->thresh_mult[THR_NEARG    ] = 2000;
                sf->thresh_mult[THR_NEWG     ] = 4000;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                sf->thresh_mult[THR_NEARESTA ] = 2000;
                sf->thresh_mult[THR_ZEROA    ] = 2000;
                sf->thresh_mult[THR_NEARA    ] = 2000;
                sf->thresh_mult[THR_NEWA     ] = 4000;
            }
        }

        break;
#endif
    case 2:
        sf->optimize_coefficients = 0;
        sf->recode_loop = 0;
        sf->auto_filter = 1;
        sf->iterative_sub_pixel = 1;
        sf->thresh_mult[THR_NEARESTMV] = 0;
        sf->thresh_mult[THR_ZEROMV   ] = 0;
        sf->thresh_mult[THR_DC       ] = 0;
        sf->thresh_mult[THR_TM       ] = 0;
        sf->thresh_mult[THR_NEARMV   ] = 0;
        sf->thresh_mult[THR_V_PRED   ] = 1000;
        sf->thresh_mult[THR_H_PRED   ] = 1000;
        sf->thresh_mult[THR_B_PRED   ] = 2500;
        sf->thresh_mult[THR_NEARESTG ] = 1000;
        sf->thresh_mult[THR_ZEROG    ] = 1000;
        sf->thresh_mult[THR_NEARG    ] = 1000;
        sf->thresh_mult[THR_NEARESTA ] = 1000;
        sf->thresh_mult[THR_ZEROA    ] = 1000;
        sf->thresh_mult[THR_NEARA    ] = 1000;
        sf->thresh_mult[THR_NEWMV    ] = 2000;
        sf->thresh_mult[THR_NEWG     ] = 2000;
        sf->thresh_mult[THR_NEWA     ] = 2000;
        sf->thresh_mult[THR_SPLITMV  ] = 5000;
        sf->thresh_mult[THR_SPLITG   ] = 10000;
        sf->thresh_mult[THR_SPLITA   ] = 10000;
        sf->full_freq[0] = 15;
        sf->full_freq[1] = 31;
        sf->search_method = NSTEP;

        if (!(cpi->ref_frame_flags & VP8_LAST_FLAG))
        {
            sf->thresh_mult[THR_NEWMV    ] = INT_MAX;
            sf->thresh_mult[THR_NEARESTMV] = INT_MAX;
            sf->thresh_mult[THR_ZEROMV   ] = INT_MAX;
            sf->thresh_mult[THR_NEARMV   ] = INT_MAX;
            sf->thresh_mult[THR_SPLITMV  ] = INT_MAX;
        }

        if (!(cpi->ref_frame_flags & VP8_GOLD_FLAG))
        {
            sf->thresh_mult[THR_NEARESTG ] = INT_MAX;
            sf->thresh_mult[THR_ZEROG    ] = INT_MAX;
            sf->thresh_mult[THR_NEARG    ] = INT_MAX;
            sf->thresh_mult[THR_NEWG     ] = INT_MAX;
            sf->thresh_mult[THR_SPLITG   ] = INT_MAX;
        }

        if (!(cpi->ref_frame_flags & VP8_ALT_FLAG))
        {
            sf->thresh_mult[THR_NEARESTA ] = INT_MAX;
            sf->thresh_mult[THR_ZEROA    ] = INT_MAX;
            sf->thresh_mult[THR_NEARA    ] = INT_MAX;
            sf->thresh_mult[THR_NEWA     ] = INT_MAX;
            sf->thresh_mult[THR_SPLITA   ] = INT_MAX;
        }

        if (Speed > 0)
        {
            cpi->mode_check_freq[THR_SPLITG] = 4;
            cpi->mode_check_freq[THR_SPLITA] = 4;
            cpi->mode_check_freq[THR_SPLITMV] = 2;

            sf->thresh_mult[THR_DC       ] = 0;
            sf->thresh_mult[THR_TM       ] = 1000;
            sf->thresh_mult[THR_V_PRED   ] = 2000;
            sf->thresh_mult[THR_H_PRED   ] = 2000;
            sf->thresh_mult[THR_B_PRED   ] = 5000;

            if (cpi->ref_frame_flags & VP8_LAST_FLAG)
            {
                sf->thresh_mult[THR_NEARESTMV] = 0;
                sf->thresh_mult[THR_ZEROMV   ] = 0;
                sf->thresh_mult[THR_NEARMV   ] = 0;
                sf->thresh_mult[THR_NEWMV    ] = 2000;
                sf->thresh_mult[THR_SPLITMV  ] = 10000;
            }

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                sf->thresh_mult[THR_NEARESTG ] = 1000;
                sf->thresh_mult[THR_ZEROG    ] = 1000;
                sf->thresh_mult[THR_NEARG    ] = 1000;
                sf->thresh_mult[THR_NEWG     ] = 2000;
                sf->thresh_mult[THR_SPLITG   ] = 20000;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                sf->thresh_mult[THR_NEARESTA ] = 1000;
                sf->thresh_mult[THR_ZEROA    ] = 1000;
                sf->thresh_mult[THR_NEARA    ] = 1000;
                sf->thresh_mult[THR_NEWA     ] = 2000;
                sf->thresh_mult[THR_SPLITA   ] = 20000;
            }

            sf->improved_quant = 0;
            sf->improved_dct = 0;
        }

        if (Speed > 1)
        {
            cpi->mode_check_freq[THR_SPLITMV] = 7;
            cpi->mode_check_freq[THR_SPLITG] = 15;
            cpi->mode_check_freq[THR_SPLITA] = 15;

            sf->thresh_mult[THR_TM       ] = 2000;
            sf->thresh_mult[THR_V_PRED   ] = 2000;
            sf->thresh_mult[THR_H_PRED   ] = 2000;
            sf->thresh_mult[THR_B_PRED   ] = 5000;

            if (cpi->ref_frame_flags & VP8_LAST_FLAG)
            {
                sf->thresh_mult[THR_NEWMV    ] = 2000;
                sf->thresh_mult[THR_SPLITMV  ] = 25000;
            }

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                sf->thresh_mult[THR_NEARESTG ] = 2000;
                sf->thresh_mult[THR_ZEROG    ] = 2000;
                sf->thresh_mult[THR_NEARG    ] = 2000;
                sf->thresh_mult[THR_NEWG     ] = 2500;
                sf->thresh_mult[THR_SPLITG   ] = 50000;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                sf->thresh_mult[THR_NEARESTA ] = 2000;
                sf->thresh_mult[THR_ZEROA    ] = 2000;
                sf->thresh_mult[THR_NEARA    ] = 2000;
                sf->thresh_mult[THR_NEWA     ] = 2500;
                sf->thresh_mult[THR_SPLITA   ] = 50000;
            }

            sf->full_freq[0] = 31;
            sf->full_freq[1] = 63;
        }

        if (Speed > 2)
        {
            sf->auto_filter = 0;                     // Faster selection of loop filter

            cpi->mode_check_freq[THR_V_PRED] = 2;
            cpi->mode_check_freq[THR_H_PRED] = 2;
            cpi->mode_check_freq[THR_B_PRED] = 2;

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                cpi->mode_check_freq[THR_NEARG] = 2;
                cpi->mode_check_freq[THR_NEWG] = 4;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                cpi->mode_check_freq[THR_NEARA] = 2;
                cpi->mode_check_freq[THR_NEWA] = 4;
            }

            sf->thresh_mult[THR_SPLITMV  ] = INT_MAX;
            sf->thresh_mult[THR_SPLITG  ] = INT_MAX;
            sf->thresh_mult[THR_SPLITA  ] = INT_MAX;

            sf->full_freq[0] = 63;
            sf->full_freq[1] = 127;
        }

        if (Speed > 3)
        {
            sf->RD = 0;
            sf->full_freq[0] = INT_MAX;
            sf->full_freq[1] = INT_MAX;

            sf->auto_filter = 1;
        }

        if (Speed > 4)
        {
            sf->auto_filter = 0;                     // Faster selection of loop filter

#if CONFIG_REALTIME_ONLY
            sf->search_method = HEX;
#else
            sf->search_method = DIAMOND;
#endif

            cpi->mode_check_freq[THR_V_PRED] = 4;
            cpi->mode_check_freq[THR_H_PRED] = 4;
            cpi->mode_check_freq[THR_B_PRED] = 4;

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                cpi->mode_check_freq[THR_NEARG] = 2;
                cpi->mode_check_freq[THR_NEWG] = 4;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                cpi->mode_check_freq[THR_NEARA] = 2;
                cpi->mode_check_freq[THR_NEWA] = 4;
            }

            sf->thresh_mult[THR_TM       ] = 2000;
            sf->thresh_mult[THR_B_PRED   ] = 5000;

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                sf->thresh_mult[THR_NEARESTG ] = 2000;
                sf->thresh_mult[THR_ZEROG    ] = 2000;
                sf->thresh_mult[THR_NEARG    ] = 2000;
                sf->thresh_mult[THR_NEWG     ] = 4000;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                sf->thresh_mult[THR_NEARESTA ] = 2000;
                sf->thresh_mult[THR_ZEROA    ] = 2000;
                sf->thresh_mult[THR_NEARA    ] = 2000;
                sf->thresh_mult[THR_NEWA     ] = 4000;
            }
        }

        if (Speed > 5)
        {
            // Disable split MB intra prediction mode
            sf->thresh_mult[THR_B_PRED] = INT_MAX;
        }

        if (Speed > 6)
        {
            unsigned int i, sum = 0;
            unsigned int total_mbs = cm->MBs;
            int thresh;
            int total_skip;

            int min = 2000;
            sf->iterative_sub_pixel = 0;

            if (cpi->oxcf.encode_breakout > 2000)
                min = cpi->oxcf.encode_breakout;

            min >>= 7;

            for (i = 0; i < min; i++)
            {
                sum += cpi->error_bins[i];
            }

            total_skip = sum;
            sum = 0;

            // i starts from 2 to make sure thresh started from 2048
            for (; i < 1024; i++)
            {
                sum += cpi->error_bins[i];

                if (10 * sum >= (unsigned int)(cpi->Speed - 6)*(total_mbs - total_skip))
                    break;
            }

            i--;
            thresh = (i << 7);

            if (thresh < 2000)
                thresh = 2000;

            if (cpi->ref_frame_flags & VP8_LAST_FLAG)
            {
                sf->thresh_mult[THR_NEWMV] = thresh;
                sf->thresh_mult[THR_NEARESTMV ] = thresh >> 1;
                sf->thresh_mult[THR_NEARMV    ] = thresh >> 1;
            }

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                sf->thresh_mult[THR_NEWG] = thresh << 1;
                sf->thresh_mult[THR_NEARESTG ] = thresh;
                sf->thresh_mult[THR_NEARG    ] = thresh;
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                sf->thresh_mult[THR_NEWA] = thresh << 1;
                sf->thresh_mult[THR_NEARESTA ] = thresh;
                sf->thresh_mult[THR_NEARA    ] = thresh;
            }

            // Disable other intra prediction modes
            sf->thresh_mult[THR_TM] = INT_MAX;
            sf->thresh_mult[THR_V_PRED] = INT_MAX;
            sf->thresh_mult[THR_H_PRED] = INT_MAX;

        }

        if (Speed > 8)
        {
            sf->quarter_pixel_search = 0;
        }

        if (Speed > 9)
        {
            int Tmp = cpi->Speed - 8;

            if (Tmp > 4)
                Tmp = 4;

            if (cpi->ref_frame_flags & VP8_GOLD_FLAG)
            {
                cpi->mode_check_freq[THR_ZEROG] = 1 << (Tmp - 1);
                cpi->mode_check_freq[THR_NEARESTG] = 1 << (Tmp - 1);
                cpi->mode_check_freq[THR_NEARG] = 1 << Tmp;
                cpi->mode_check_freq[THR_NEWG] = 1 << (Tmp + 1);
            }

            if (cpi->ref_frame_flags & VP8_ALT_FLAG)
            {
                cpi->mode_check_freq[THR_ZEROA] = 1 << (Tmp - 1);
                cpi->mode_check_freq[THR_NEARESTA] = 1 << (Tmp - 1);
                cpi->mode_check_freq[THR_NEARA] = 1 << Tmp;
                cpi->mode_check_freq[THR_NEWA] = 1 << (Tmp + 1);
            }

            cpi->mode_check_freq[THR_NEWMV] = 1 << (Tmp - 1);
        }

        cm->filter_type = NORMAL_LOOPFILTER;

        if (Speed >= 14)
            cm->filter_type = SIMPLE_LOOPFILTER;

        if (Speed >= 15)
        {
            sf->half_pixel_search = 0;        // This has a big hit on quality. Last resort
        }

        vpx_memset(cpi->error_bins, 0, sizeof(cpi->error_bins));

    };

    if (cpi->sf.search_method == NSTEP)
    {
        vp8_init3smotion_compensation(&cpi->mb, cm->yv12_fb[cm->lst_fb_idx].y_stride);
    }
    else if (cpi->sf.search_method == DIAMOND)
    {
        vp8_init_dsmotion_compensation(&cpi->mb, cm->yv12_fb[cm->lst_fb_idx].y_stride);
    }

    if (cpi->sf.improved_dct)
    {
        cpi->mb.vp8_short_fdct8x4 = FDCT_INVOKE(&cpi->rtcd.fdct, short8x4);
        cpi->mb.vp8_short_fdct4x4 = FDCT_INVOKE(&cpi->rtcd.fdct, short4x4);
    }
    else
    {
        cpi->mb.vp8_short_fdct8x4   = FDCT_INVOKE(&cpi->rtcd.fdct, fast8x4);
        cpi->mb.vp8_short_fdct4x4   = FDCT_INVOKE(&cpi->rtcd.fdct, fast4x4);
    }

    cpi->mb.short_walsh4x4 = FDCT_INVOKE(&cpi->rtcd.fdct, walsh_short4x4);

    if (cpi->sf.improved_quant)
    {
        cpi->mb.quantize_b    = QUANTIZE_INVOKE(&cpi->rtcd.quantize, quantb);
    }
    else
    {
        cpi->mb.quantize_b      = QUANTIZE_INVOKE(&cpi->rtcd.quantize, fastquantb);
    }

#if CONFIG_RUNTIME_CPU_DETECT
    cpi->mb.e_mbd.rtcd = &cpi->common.rtcd;
#endif

    if (cpi->sf.iterative_sub_pixel == 1)
    {
        cpi->find_fractional_mv_step = vp8_find_best_sub_pixel_step_iteratively;
    }
    else if (cpi->sf.quarter_pixel_search)
    {
        cpi->find_fractional_mv_step = vp8_find_best_sub_pixel_step;
    }
    else if (cpi->sf.half_pixel_search)
    {
        cpi->find_fractional_mv_step = vp8_find_best_half_pixel_step;
    }
    else
    {
        cpi->find_fractional_mv_step = vp8_skip_fractional_mv_step;
    }

    if (cpi->sf.optimize_coefficients == 1)
        cpi->mb.optimize = 1 + cpi->is_next_src_alt_ref;
    else
        cpi->mb.optimize = 0;

    if (cpi->common.full_pixel)
        cpi->find_fractional_mv_step = vp8_skip_fractional_mv_step;

#ifdef SPEEDSTATS
    frames_at_speed[cpi->Speed]++;
#endif
}
static void alloc_raw_frame_buffers(VP8_COMP *cpi)
{
    int i, buffers;

    buffers = cpi->oxcf.lag_in_frames;

    if (buffers > MAX_LAG_BUFFERS)
        buffers = MAX_LAG_BUFFERS;

    if (buffers < 1)
        buffers = 1;

    for (i = 0; i < buffers; i++)
        if (vp8_yv12_alloc_frame_buffer(&cpi->src_buffer[i].source_buffer,
                                        cpi->oxcf.Width, cpi->oxcf.Height,
                                        16))
            vpx_internal_error(&cpi->common.error, VPX_CODEC_MEM_ERROR,
                               "Failed to allocate lag buffer");

#if VP8_TEMPORAL_ALT_REF

    if (vp8_yv12_alloc_frame_buffer(&cpi->alt_ref_buffer.source_buffer,
                                    cpi->oxcf.Width, cpi->oxcf.Height, 16))
        vpx_internal_error(&cpi->common.error, VPX_CODEC_MEM_ERROR,
                           "Failed to allocate altref buffer");

#endif

    cpi->source_buffer_count = 0;
}

static int vp8_alloc_partition_data(VP8_COMP *cpi)
{
    cpi->mb.pip = vpx_calloc((cpi->common.mb_cols + 1) *
                                (cpi->common.mb_rows + 1),
                                sizeof(PARTITION_INFO));
    if(!cpi->mb.pip)
        return ALLOC_FAILURE;

    cpi->mb.pi = cpi->mb.pip + cpi->common.mode_info_stride + 1;

    return 0;
}

void vp8_alloc_compressor_data(VP8_COMP *cpi)
{
    VP8_COMMON *cm = & cpi->common;

    int width = cm->Width;
    int height = cm->Height;

    if (vp8_alloc_frame_buffers(cm, width, height))
        vpx_internal_error(&cpi->common.error, VPX_CODEC_MEM_ERROR,
                           "Failed to allocate frame buffers");

    if (vp8_alloc_partition_data(cpi))
        vpx_internal_error(&cpi->common.error, VPX_CODEC_MEM_ERROR,
                           "Failed to allocate partition data");


    if ((width & 0xf) != 0)
        width += 16 - (width & 0xf);

    if ((height & 0xf) != 0)
        height += 16 - (height & 0xf);


    if (vp8_yv12_alloc_frame_buffer(&cpi->last_frame_uf,
                                    width, height, VP8BORDERINPIXELS))
        vpx_internal_error(&cpi->common.error, VPX_CODEC_MEM_ERROR,
                           "Failed to allocate last frame buffer");

    if (vp8_yv12_alloc_frame_buffer(&cpi->scaled_source, width, height, 16))
        vpx_internal_error(&cpi->common.error, VPX_CODEC_MEM_ERROR,
                           "Failed to allocate scaled source buffer");


    if (cpi->tok != 0)
        vpx_free(cpi->tok);

    {
        unsigned int tokens = cm->mb_rows * cm->mb_cols * 24 * 16;

        CHECK_MEM_ERROR(cpi->tok, vpx_calloc(tokens, sizeof(*cpi->tok)));
    }

    // Data used for real time vc mode to see if gf needs refreshing
    cpi->inter_zz_count = 0;
    cpi->gf_bad_count = 0;
    cpi->gf_update_recommended = 0;


    // Structures used to minitor GF usage
    if (cpi->gf_active_flags != 0)
        vpx_free(cpi->gf_active_flags);

    CHECK_MEM_ERROR(cpi->gf_active_flags, vpx_calloc(1, cm->mb_rows * cm->mb_cols));

    cpi->gf_active_count = cm->mb_rows * cm->mb_cols;

    cpi->total_stats = vpx_calloc(1, vp8_firstpass_stats_sz(cpi->common.MBs));
    cpi->this_frame_stats = vpx_calloc(1, vp8_firstpass_stats_sz(cpi->common.MBs));
    if(!cpi->total_stats || !cpi->this_frame_stats)
        vpx_internal_error(&cpi->common.error, VPX_CODEC_MEM_ERROR,
                           "Failed to allocate firstpass stats");
}


// Quant MOD
static const int q_trans[] =
{
    0,   1,  2,  3,  4,  5,  7,  8,
    9,  10, 12, 13, 15, 17, 18, 19,
    20,  21, 23, 24, 25, 26, 27, 28,
    29,  30, 31, 33, 35, 37, 39, 41,
    43,  45, 47, 49, 51, 53, 55, 57,
    59,  61, 64, 67, 70, 73, 76, 79,
    82,  85, 88, 91, 94, 97, 100, 103,
    106, 109, 112, 115, 118, 121, 124, 127,
};

int vp8_reverse_trans(int x)
{
    int i;

    for (i = 0; i < 64; i++)
        if (q_trans[i] >= x)
            return i;

    return 63;
};
void vp8_new_frame_rate(VP8_COMP *cpi, double framerate)
{
    if(framerate < .1)
        framerate = 30;

    cpi->oxcf.frame_rate             = framerate;
    cpi->output_frame_rate            = cpi->oxcf.frame_rate;
    cpi->per_frame_bandwidth          = (int)(cpi->oxcf.target_bandwidth / cpi->output_frame_rate);
    cpi->av_per_frame_bandwidth        = (int)(cpi->oxcf.target_bandwidth / cpi->output_frame_rate);
    cpi->min_frame_bandwidth          = (int)(cpi->av_per_frame_bandwidth * cpi->oxcf.two_pass_vbrmin_section / 100);
    cpi->max_gf_interval = (int)(cpi->output_frame_rate / 2) + 2;

    //cpi->max_gf_interval = (int)(cpi->output_frame_rate * 2 / 3) + 1;
    //cpi->max_gf_interval = 24;

    if (cpi->max_gf_interval < 12)
        cpi->max_gf_interval = 12;


    // Special conditions when altr ref frame enabled in lagged compress mode
    if (cpi->oxcf.play_alternate && cpi->oxcf.lag_in_frames)
    {
        if (cpi->max_gf_interval > cpi->oxcf.lag_in_frames - 1)
            cpi->max_gf_interval = cpi->oxcf.lag_in_frames - 1;
    }
}


static int
rescale(int val, int num, int denom)
{
    int64_t llnum = num;
    int64_t llden = denom;
    int64_t llval = val;

    return llval * llnum / llden;
}


void vp8_init_config(VP8_PTR ptr, VP8_CONFIG *oxcf)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);
    VP8_COMMON *cm = &cpi->common;

    if (!cpi)
        return;

    cpi->auto_gold = 1;
    cpi->auto_adjust_gold_quantizer = 1;
    cpi->goldquantizer = 1;
    cpi->goldfreq = 7;
    cpi->auto_adjust_key_quantizer = 1;
    cpi->keyquantizer = 1;

    cm->version = oxcf->Version;
    vp8_setup_version(cm);

    if (oxcf == 0)
    {
        cpi->pass                     = 0;

        cpi->auto_worst_q              = 0;
        cpi->oxcf.best_allowed_q            = MINQ;
        cpi->oxcf.worst_allowed_q           = MAXQ;

        cpi->oxcf.end_usage                = USAGE_STREAM_FROM_SERVER;
        cpi->oxcf.starting_buffer_level     =   4000;
        cpi->oxcf.optimal_buffer_level      =   5000;
        cpi->oxcf.maximum_buffer_size       =   6000;
        cpi->oxcf.under_shoot_pct           =  90;
        cpi->oxcf.allow_df                 =   0;
        cpi->oxcf.drop_frames_water_mark     =  20;

        cpi->oxcf.allow_spatial_resampling  = 0;
        cpi->oxcf.resample_down_water_mark   = 40;
        cpi->oxcf.resample_up_water_mark     = 60;

        cpi->oxcf.fixed_q = cpi->interquantizer;

        cpi->filter_type = NORMAL_LOOPFILTER;

        if (cm->simpler_lpf)
            cpi->filter_type = SIMPLE_LOOPFILTER;

        cpi->compressor_speed = 1;
        cpi->horiz_scale = 0;
        cpi->vert_scale = 0;
        cpi->oxcf.two_pass_vbrbias = 50;
        cpi->oxcf.two_pass_vbrmax_section = 400;
        cpi->oxcf.two_pass_vbrmin_section = 0;

        cpi->oxcf.Sharpness = 0;
        cpi->oxcf.noise_sensitivity = 0;
    }
    else
        cpi->oxcf = *oxcf;


    switch (cpi->oxcf.Mode)
    {

    case MODE_REALTIME:
        cpi->pass = 0;
        cpi->compressor_speed = 2;

        if (cpi->oxcf.cpu_used < -16)
        {
            cpi->oxcf.cpu_used = -16;
        }

        if (cpi->oxcf.cpu_used > 16)
            cpi->oxcf.cpu_used = 16;

        break;

#if !(CONFIG_REALTIME_ONLY)
    case MODE_GOODQUALITY:
        cpi->pass = 0;
        cpi->compressor_speed = 1;

        if (cpi->oxcf.cpu_used < -5)
        {
            cpi->oxcf.cpu_used = -5;
        }

        if (cpi->oxcf.cpu_used > 5)
            cpi->oxcf.cpu_used = 5;

        break;

    case MODE_BESTQUALITY:
        cpi->pass = 0;
        cpi->compressor_speed = 0;
        break;

    case MODE_FIRSTPASS:
        cpi->pass = 1;
        cpi->compressor_speed = 1;
        break;
    case MODE_SECONDPASS:
        cpi->pass = 2;
        cpi->compressor_speed = 1;

        if (cpi->oxcf.cpu_used < -5)
        {
            cpi->oxcf.cpu_used = -5;
        }

        if (cpi->oxcf.cpu_used > 5)
            cpi->oxcf.cpu_used = 5;

        break;
    case MODE_SECONDPASS_BEST:
        cpi->pass = 2;
        cpi->compressor_speed = 0;
        break;
#endif
    }

    if (cpi->pass == 0)
        cpi->auto_worst_q = 1;

    cpi->oxcf.worst_allowed_q = q_trans[oxcf->worst_allowed_q];
    cpi->oxcf.best_allowed_q  = q_trans[oxcf->best_allowed_q];

    if (oxcf->fixed_q >= 0)
    {
        if (oxcf->worst_allowed_q < 0)
            cpi->oxcf.fixed_q = q_trans[0];
        else
            cpi->oxcf.fixed_q = q_trans[oxcf->worst_allowed_q];

        if (oxcf->alt_q < 0)
            cpi->oxcf.alt_q = q_trans[0];
        else
            cpi->oxcf.alt_q = q_trans[oxcf->alt_q];

        if (oxcf->key_q < 0)
            cpi->oxcf.key_q = q_trans[0];
        else
            cpi->oxcf.key_q = q_trans[oxcf->key_q];

        if (oxcf->gold_q < 0)
            cpi->oxcf.gold_q = q_trans[0];
        else
            cpi->oxcf.gold_q = q_trans[oxcf->gold_q];

    }

    cpi->baseline_gf_interval = cpi->oxcf.alt_freq ? cpi->oxcf.alt_freq : DEFAULT_GF_INTERVAL;
    cpi->ref_frame_flags = VP8_ALT_FLAG | VP8_GOLD_FLAG | VP8_LAST_FLAG;

    //cpi->use_golden_frame_only = 0;
    //cpi->use_last_frame_only = 0;
    cm->refresh_golden_frame = 0;
    cm->refresh_last_frame = 1;
    cm->refresh_entropy_probs = 1;

    if (cpi->oxcf.token_partitions >= 0 && cpi->oxcf.token_partitions <= 3)
        cm->multi_token_partition = (TOKEN_PARTITION) cpi->oxcf.token_partitions;

    setup_features(cpi);

    {
        int i;

        for (i = 0; i < MAX_MB_SEGMENTS; i++)
            cpi->segment_encode_breakout[i] = cpi->oxcf.encode_breakout;
    }

    // At the moment the first order values may not be > MAXQ
    if (cpi->oxcf.fixed_q > MAXQ)
        cpi->oxcf.fixed_q = MAXQ;

    // local file playback mode == really big buffer
    if (cpi->oxcf.end_usage == USAGE_LOCAL_FILE_PLAYBACK)
    {
        cpi->oxcf.starting_buffer_level   = 60000;
        cpi->oxcf.optimal_buffer_level    = 60000;
        cpi->oxcf.maximum_buffer_size     = 240000;

    }


    // Convert target bandwidth from Kbit/s to Bit/s
    cpi->oxcf.target_bandwidth       *= 1000;
    cpi->oxcf.starting_buffer_level =
        rescale(cpi->oxcf.starting_buffer_level,
                cpi->oxcf.target_bandwidth, 1000);

    if (cpi->oxcf.optimal_buffer_level == 0)
        cpi->oxcf.optimal_buffer_level = cpi->oxcf.target_bandwidth / 8;
    else
        cpi->oxcf.optimal_buffer_level =
            rescale(cpi->oxcf.optimal_buffer_level,
                    cpi->oxcf.target_bandwidth, 1000);

    if (cpi->oxcf.maximum_buffer_size == 0)
        cpi->oxcf.maximum_buffer_size = cpi->oxcf.target_bandwidth / 8;
    else
        cpi->oxcf.maximum_buffer_size =
            rescale(cpi->oxcf.maximum_buffer_size,
                    cpi->oxcf.target_bandwidth, 1000);

    cpi->buffer_level                = cpi->oxcf.starting_buffer_level;
    cpi->bits_off_target              = cpi->oxcf.starting_buffer_level;

    vp8_new_frame_rate(cpi, cpi->oxcf.frame_rate);
    cpi->worst_quality               = cpi->oxcf.worst_allowed_q;
    cpi->active_worst_quality         = cpi->oxcf.worst_allowed_q;
    cpi->avg_frame_qindex             = cpi->oxcf.worst_allowed_q;
    cpi->best_quality                = cpi->oxcf.best_allowed_q;
    cpi->active_best_quality          = cpi->oxcf.best_allowed_q;
    cpi->buffered_mode = (cpi->oxcf.optimal_buffer_level > 0) ? TRUE : FALSE;

    cpi->rolling_target_bits          = cpi->av_per_frame_bandwidth;
    cpi->rolling_actual_bits          = cpi->av_per_frame_bandwidth;
    cpi->long_rolling_target_bits      = cpi->av_per_frame_bandwidth;
    cpi->long_rolling_actual_bits      = cpi->av_per_frame_bandwidth;

    cpi->total_actual_bits            = 0;
    cpi->total_target_vs_actual        = 0;

    // Only allow dropped frames in buffered mode
    cpi->drop_frames_allowed          = cpi->oxcf.allow_df && cpi->buffered_mode;

    cm->filter_type      = (LOOPFILTERTYPE) cpi->filter_type;

    if (!cm->use_bilinear_mc_filter)
        cm->mcomp_filter_type = SIXTAP;
    else
        cm->mcomp_filter_type = BILINEAR;

    cpi->target_bandwidth = cpi->oxcf.target_bandwidth;

    cm->Width       = cpi->oxcf.Width     ;
    cm->Height      = cpi->oxcf.Height    ;

    cpi->intra_frame_target = (4 * (cm->Width + cm->Height) / 15) * 1000; // As per VP8

    cm->horiz_scale  = cpi->horiz_scale;
    cm->vert_scale   = cpi->vert_scale ;

    // VP8 sharpness level mapping 0-7 (vs 0-10 in general VPx dialogs)
    if (cpi->oxcf.Sharpness > 7)
        cpi->oxcf.Sharpness = 7;

    cm->sharpness_level = cpi->oxcf.Sharpness;

    if (cm->horiz_scale != NORMAL || cm->vert_scale != NORMAL)
    {
        int UNINITIALIZED_IS_SAFE(hr), UNINITIALIZED_IS_SAFE(hs);
        int UNINITIALIZED_IS_SAFE(vr), UNINITIALIZED_IS_SAFE(vs);

        Scale2Ratio(cm->horiz_scale, &hr, &hs);
        Scale2Ratio(cm->vert_scale, &vr, &vs);

        // always go to the next whole number
        cm->Width = (hs - 1 + cpi->oxcf.Width * hr) / hs;
        cm->Height = (vs - 1 + cpi->oxcf.Height * vr) / vs;
    }

    if (((cm->Width + 15) & 0xfffffff0) != cm->yv12_fb[cm->lst_fb_idx].y_width ||
        ((cm->Height + 15) & 0xfffffff0) != cm->yv12_fb[cm->lst_fb_idx].y_height ||
        cm->yv12_fb[cm->lst_fb_idx].y_width == 0)
    {
        alloc_raw_frame_buffers(cpi);
        vp8_alloc_compressor_data(cpi);
    }

    // Clamp KF frame size to quarter of data rate
    if (cpi->intra_frame_target > cpi->target_bandwidth >> 2)
        cpi->intra_frame_target = cpi->target_bandwidth >> 2;

    if (cpi->oxcf.fixed_q >= 0)
    {
        cpi->last_q[0] = cpi->oxcf.fixed_q;
        cpi->last_q[1] = cpi->oxcf.fixed_q;
    }

    cpi->Speed = cpi->oxcf.cpu_used;

    // force to allowlag to 0 if lag_in_frames is 0;
    if (cpi->oxcf.lag_in_frames == 0)
    {
        cpi->oxcf.allow_lag = 0;
    }
    // Limit on lag buffers as these are not currently dynamically allocated
    else if (cpi->oxcf.lag_in_frames > MAX_LAG_BUFFERS)
        cpi->oxcf.lag_in_frames = MAX_LAG_BUFFERS;

    // YX Temp
    cpi->last_alt_ref_sei    = -1;
    cpi->is_src_frame_alt_ref = 0;
    cpi->is_next_src_alt_ref = 0;

#if 0
    // Experimental RD Code
    cpi->frame_distortion = 0;
    cpi->last_frame_distortion = 0;
#endif

#if VP8_TEMPORAL_ALT_REF

    cpi->use_weighted_temporal_filter = 0;

    {
        int i;

        cpi->fixed_divide[0] = 0;

        for (i = 1; i < 512; i++)
            cpi->fixed_divide[i] = 0x80000 / i;
    }
#endif
}

/*
 * This function needs more clean up, i.e. be more tuned torwards
 * change_config rather than init_config  !!!!!!!!!!!!!!!!
 * YX - 5/28/2009
 *
 */

void vp8_change_config(VP8_PTR ptr, VP8_CONFIG *oxcf)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);
    VP8_COMMON *cm = &cpi->common;

    if (!cpi)
        return;

    if (!oxcf)
        return;

    if (cm->version != oxcf->Version)
    {
        cm->version = oxcf->Version;
        vp8_setup_version(cm);
    }

    cpi->oxcf = *oxcf;

    switch (cpi->oxcf.Mode)
    {

    case MODE_REALTIME:
        cpi->pass = 0;
        cpi->compressor_speed = 2;

        if (cpi->oxcf.cpu_used < -16)
        {
            cpi->oxcf.cpu_used = -16;
        }

        if (cpi->oxcf.cpu_used > 16)
            cpi->oxcf.cpu_used = 16;

        break;

#if !(CONFIG_REALTIME_ONLY)
    case MODE_GOODQUALITY:
        cpi->pass = 0;
        cpi->compressor_speed = 1;

        if (cpi->oxcf.cpu_used < -5)
        {
            cpi->oxcf.cpu_used = -5;
        }

        if (cpi->oxcf.cpu_used > 5)
            cpi->oxcf.cpu_used = 5;

        break;

    case MODE_BESTQUALITY:
        cpi->pass = 0;
        cpi->compressor_speed = 0;
        break;

    case MODE_FIRSTPASS:
        cpi->pass = 1;
        cpi->compressor_speed = 1;
        break;
    case MODE_SECONDPASS:
        cpi->pass = 2;
        cpi->compressor_speed = 1;

        if (cpi->oxcf.cpu_used < -5)
        {
            cpi->oxcf.cpu_used = -5;
        }

        if (cpi->oxcf.cpu_used > 5)
            cpi->oxcf.cpu_used = 5;

        break;
    case MODE_SECONDPASS_BEST:
        cpi->pass = 2;
        cpi->compressor_speed = 0;
        break;
#endif
    }

    if (cpi->pass == 0)
        cpi->auto_worst_q = 1;

    cpi->oxcf.worst_allowed_q = q_trans[oxcf->worst_allowed_q];
    cpi->oxcf.best_allowed_q = q_trans[oxcf->best_allowed_q];

    if (oxcf->fixed_q >= 0)
    {
        if (oxcf->worst_allowed_q < 0)
            cpi->oxcf.fixed_q = q_trans[0];
        else
            cpi->oxcf.fixed_q = q_trans[oxcf->worst_allowed_q];

        if (oxcf->alt_q < 0)
            cpi->oxcf.alt_q = q_trans[0];
        else
            cpi->oxcf.alt_q = q_trans[oxcf->alt_q];

        if (oxcf->key_q < 0)
            cpi->oxcf.key_q = q_trans[0];
        else
            cpi->oxcf.key_q = q_trans[oxcf->key_q];

        if (oxcf->gold_q < 0)
            cpi->oxcf.gold_q = q_trans[0];
        else
            cpi->oxcf.gold_q = q_trans[oxcf->gold_q];

    }

    cpi->baseline_gf_interval = cpi->oxcf.alt_freq ? cpi->oxcf.alt_freq : DEFAULT_GF_INTERVAL;

    cpi->ref_frame_flags = VP8_ALT_FLAG | VP8_GOLD_FLAG | VP8_LAST_FLAG;

    //cpi->use_golden_frame_only = 0;
    //cpi->use_last_frame_only = 0;
    cm->refresh_golden_frame = 0;
    cm->refresh_last_frame = 1;
    cm->refresh_entropy_probs = 1;

    if (cpi->oxcf.token_partitions >= 0 && cpi->oxcf.token_partitions <= 3)
        cm->multi_token_partition = (TOKEN_PARTITION) cpi->oxcf.token_partitions;

    setup_features(cpi);

    {
        int i;

        for (i = 0; i < MAX_MB_SEGMENTS; i++)
            cpi->segment_encode_breakout[i] = cpi->oxcf.encode_breakout;
    }

    // At the moment the first order values may not be > MAXQ
    if (cpi->oxcf.fixed_q > MAXQ)
        cpi->oxcf.fixed_q = MAXQ;

    // local file playback mode == really big buffer
    if (cpi->oxcf.end_usage == USAGE_LOCAL_FILE_PLAYBACK)
    {
        cpi->oxcf.starting_buffer_level   = 60000;
        cpi->oxcf.optimal_buffer_level    = 60000;
        cpi->oxcf.maximum_buffer_size     = 240000;

    }

    // Convert target bandwidth from Kbit/s to Bit/s
    cpi->oxcf.target_bandwidth       *= 1000;

    cpi->oxcf.starting_buffer_level =
        rescale(cpi->oxcf.starting_buffer_level,
                cpi->oxcf.target_bandwidth, 1000);

    if (cpi->oxcf.optimal_buffer_level == 0)
        cpi->oxcf.optimal_buffer_level = cpi->oxcf.target_bandwidth / 8;
    else
        cpi->oxcf.optimal_buffer_level =
            rescale(cpi->oxcf.optimal_buffer_level,
                    cpi->oxcf.target_bandwidth, 1000);

    if (cpi->oxcf.maximum_buffer_size == 0)
        cpi->oxcf.maximum_buffer_size = cpi->oxcf.target_bandwidth / 8;
    else
        cpi->oxcf.maximum_buffer_size =
            rescale(cpi->oxcf.maximum_buffer_size,
                    cpi->oxcf.target_bandwidth, 1000);

    cpi->buffer_level                = cpi->oxcf.starting_buffer_level;
    cpi->bits_off_target              = cpi->oxcf.starting_buffer_level;

    vp8_new_frame_rate(cpi, cpi->oxcf.frame_rate);
    cpi->worst_quality               = cpi->oxcf.worst_allowed_q;
    cpi->active_worst_quality         = cpi->oxcf.worst_allowed_q;
    cpi->avg_frame_qindex             = cpi->oxcf.worst_allowed_q;
    cpi->best_quality                = cpi->oxcf.best_allowed_q;
    cpi->active_best_quality          = cpi->oxcf.best_allowed_q;
    cpi->buffered_mode = (cpi->oxcf.optimal_buffer_level > 0) ? TRUE : FALSE;

    cpi->rolling_target_bits          = cpi->av_per_frame_bandwidth;
    cpi->rolling_actual_bits          = cpi->av_per_frame_bandwidth;
    cpi->long_rolling_target_bits      = cpi->av_per_frame_bandwidth;
    cpi->long_rolling_actual_bits      = cpi->av_per_frame_bandwidth;

    cpi->total_actual_bits            = 0;
    cpi->total_target_vs_actual        = 0;

    // Only allow dropped frames in buffered mode
    cpi->drop_frames_allowed          = cpi->oxcf.allow_df && cpi->buffered_mode;

    cm->filter_type                  = (LOOPFILTERTYPE) cpi->filter_type;

    if (!cm->use_bilinear_mc_filter)
        cm->mcomp_filter_type = SIXTAP;
    else
        cm->mcomp_filter_type = BILINEAR;

    cpi->target_bandwidth = cpi->oxcf.target_bandwidth;

    cm->Width       = cpi->oxcf.Width     ;
    cm->Height      = cpi->oxcf.Height    ;

    cm->horiz_scale  = cpi->horiz_scale;
    cm->vert_scale   = cpi->vert_scale ;

    cpi->intra_frame_target           = (4 * (cm->Width + cm->Height) / 15) * 1000; // As per VP8

    // VP8 sharpness level mapping 0-7 (vs 0-10 in general VPx dialogs)
    if (cpi->oxcf.Sharpness > 7)
        cpi->oxcf.Sharpness = 7;

    cm->sharpness_level = cpi->oxcf.Sharpness;

    if (cm->horiz_scale != NORMAL || cm->vert_scale != NORMAL)
    {
        int UNINITIALIZED_IS_SAFE(hr), UNINITIALIZED_IS_SAFE(hs);
        int UNINITIALIZED_IS_SAFE(vr), UNINITIALIZED_IS_SAFE(vs);

        Scale2Ratio(cm->horiz_scale, &hr, &hs);
        Scale2Ratio(cm->vert_scale, &vr, &vs);

        // always go to the next whole number
        cm->Width = (hs - 1 + cpi->oxcf.Width * hr) / hs;
        cm->Height = (vs - 1 + cpi->oxcf.Height * vr) / vs;
    }

    if (((cm->Width + 15) & 0xfffffff0) != cm->yv12_fb[cm->lst_fb_idx].y_width ||
        ((cm->Height + 15) & 0xfffffff0) != cm->yv12_fb[cm->lst_fb_idx].y_height ||
        cm->yv12_fb[cm->lst_fb_idx].y_width == 0)
    {
        alloc_raw_frame_buffers(cpi);
        vp8_alloc_compressor_data(cpi);
    }

    // Clamp KF frame size to quarter of data rate
    if (cpi->intra_frame_target > cpi->target_bandwidth >> 2)
        cpi->intra_frame_target = cpi->target_bandwidth >> 2;

    if (cpi->oxcf.fixed_q >= 0)
    {
        cpi->last_q[0] = cpi->oxcf.fixed_q;
        cpi->last_q[1] = cpi->oxcf.fixed_q;
    }

    cpi->Speed = cpi->oxcf.cpu_used;

    // force to allowlag to 0 if lag_in_frames is 0;
    if (cpi->oxcf.lag_in_frames == 0)
    {
        cpi->oxcf.allow_lag = 0;
    }
    // Limit on lag buffers as these are not currently dynamically allocated
    else if (cpi->oxcf.lag_in_frames > MAX_LAG_BUFFERS)
        cpi->oxcf.lag_in_frames = MAX_LAG_BUFFERS;

    // YX Temp
    cpi->last_alt_ref_sei    = -1;
    cpi->is_src_frame_alt_ref = 0;
    cpi->is_next_src_alt_ref = 0;

#if 0
    // Experimental RD Code
    cpi->frame_distortion = 0;
    cpi->last_frame_distortion = 0;
#endif

}

#define M_LOG2_E 0.693147180559945309417
#define log2f(x) (log (x) / (float) M_LOG2_E)
static void cal_mvsadcosts(int *mvsadcost[2])
{
    int i = 1;

    mvsadcost [0] [0] = 300;
    mvsadcost [1] [0] = 300;

    do
    {
        double z = 256 * (2 * (log2f(2 * i) + .6));
        mvsadcost [0][i] = (int) z;
        mvsadcost [1][i] = (int) z;
        mvsadcost [0][-i] = (int) z;
        mvsadcost [1][-i] = (int) z;
    }
    while (++i <= mv_max);
}

VP8_PTR vp8_create_compressor(VP8_CONFIG *oxcf)
{
    int i;
    volatile union
    {
        VP8_COMP *cpi;
        VP8_PTR   ptr;
    } ctx;

    VP8_COMP *cpi;
    VP8_COMMON *cm;

    cpi = ctx.cpi = vpx_memalign(32, sizeof(VP8_COMP));
    // Check that the CPI instance is valid
    if (!cpi)
        return 0;

    cm = &cpi->common;

    vpx_memset(cpi, 0, sizeof(VP8_COMP));

    if (setjmp(cm->error.jmp))
    {
        VP8_PTR ptr = ctx.ptr;

        ctx.cpi->common.error.setjmp = 0;
        vp8_remove_compressor(&ptr);
        return 0;
    }

    cpi->common.error.setjmp = 1;

    CHECK_MEM_ERROR(cpi->rdtok, vpx_calloc(256 * 3 / 2, sizeof(TOKENEXTRA)));
    CHECK_MEM_ERROR(cpi->mb.ss, vpx_calloc(sizeof(search_site), (MAX_MVSEARCH_STEPS * 8) + 1));

    vp8_create_common(&cpi->common);
    vp8_cmachine_specific_config(cpi);

    vp8_init_config((VP8_PTR)cpi, oxcf);

    memcpy(cpi->base_skip_false_prob, vp8cx_base_skip_false_prob, sizeof(vp8cx_base_skip_false_prob));
    cpi->common.current_video_frame   = 0;
    cpi->kf_overspend_bits            = 0;
    cpi->kf_bitrate_adjustment        = 0;
    cpi->frames_till_gf_update_due      = 0;
    cpi->gf_overspend_bits            = 0;
    cpi->non_gf_bitrate_adjustment     = 0;
    cpi->prob_last_coded              = 128;
    cpi->prob_gf_coded                = 128;
    cpi->prob_intra_coded             = 63;

    // Prime the recent reference frame useage counters.
    // Hereafter they will be maintained as a sort of moving average
    cpi->recent_ref_frame_usage[INTRA_FRAME]  = 1;
    cpi->recent_ref_frame_usage[LAST_FRAME]   = 1;
    cpi->recent_ref_frame_usage[GOLDEN_FRAME] = 1;
    cpi->recent_ref_frame_usage[ALTREF_FRAME] = 1;

    // Set reference frame sign bias for ALTREF frame to 1 (for now)
    cpi->common.ref_frame_sign_bias[ALTREF_FRAME] = 1;

    cpi->gf_decay_rate = 0;
    cpi->baseline_gf_interval = DEFAULT_GF_INTERVAL;

    cpi->gold_is_last = 0 ;
    cpi->alt_is_last  = 0 ;
    cpi->gold_is_alt  = 0 ;



    // Create the encoder segmentation map and set all entries to 0
    CHECK_MEM_ERROR(cpi->segmentation_map, vpx_calloc(cpi->common.mb_rows * cpi->common.mb_cols, 1));
    CHECK_MEM_ERROR(cpi->active_map, vpx_calloc(cpi->common.mb_rows * cpi->common.mb_cols, 1));
    vpx_memset(cpi->active_map , 1, (cpi->common.mb_rows * cpi->common.mb_cols));
    cpi->active_map_enabled = 0;

    // Create the first pass motion map structure and set to 0
    // Allocate space for maximum of 15 buffers
    CHECK_MEM_ERROR(cpi->fp_motion_map, vpx_calloc(15*cpi->common.MBs, 1));

#if 0
    // Experimental code for lagged and one pass
    // Initialise one_pass GF frames stats
    // Update stats used for GF selection
    if (cpi->pass == 0)
    {
        cpi->one_pass_frame_index = 0;

        for (i = 0; i < MAX_LAG_BUFFERS; i++)
        {
            cpi->one_pass_frame_stats[i].frames_so_far = 0;
            cpi->one_pass_frame_stats[i].frame_intra_error = 0.0;
            cpi->one_pass_frame_stats[i].frame_coded_error = 0.0;
            cpi->one_pass_frame_stats[i].frame_pcnt_inter = 0.0;
            cpi->one_pass_frame_stats[i].frame_pcnt_motion = 0.0;
            cpi->one_pass_frame_stats[i].frame_mvr = 0.0;
            cpi->one_pass_frame_stats[i].frame_mvr_abs = 0.0;
            cpi->one_pass_frame_stats[i].frame_mvc = 0.0;
            cpi->one_pass_frame_stats[i].frame_mvc_abs = 0.0;
        }
    }
#endif

    // Should we use the cyclic refresh method.
    // Currently this is tied to error resilliant mode
    cpi->cyclic_refresh_mode_enabled = cpi->oxcf.error_resilient_mode;
    cpi->cyclic_refresh_mode_max_mbs_perframe = (cpi->common.mb_rows * cpi->common.mb_cols) / 40;
    cpi->cyclic_refresh_mode_index = 0;
    cpi->cyclic_refresh_q = 32;

    if (cpi->cyclic_refresh_mode_enabled)
    {
        CHECK_MEM_ERROR(cpi->cyclic_refresh_map, vpx_calloc((cpi->common.mb_rows * cpi->common.mb_cols), 1));
    }
    else
        cpi->cyclic_refresh_map = (signed char *) NULL;

    // Test function for segmentation
    //segmentation_test_function((VP8_PTR) cpi);

#ifdef ENTROPY_STATS
    init_context_counters();
#endif


    cpi->frames_since_key = 8;        // Give a sensible default for the first frame.
    cpi->key_frame_frequency = cpi->oxcf.key_freq;

    cpi->source_alt_ref_pending = FALSE;
    cpi->source_alt_ref_active = FALSE;
    cpi->common.refresh_alt_ref_frame = 0;

    cpi->b_calculate_psnr = CONFIG_PSNR;
#if CONFIG_PSNR
    cpi->b_calculate_ssimg = 0;

    cpi->count = 0;
    cpi->bytes = 0;

    if (cpi->b_calculate_psnr)
    {
        cpi->total_sq_error = 0.0;
        cpi->total_sq_error2 = 0.0;
        cpi->total_y = 0.0;
        cpi->total_u = 0.0;
        cpi->total_v = 0.0;
        cpi->total = 0.0;
        cpi->totalp_y = 0.0;
        cpi->totalp_u = 0.0;
        cpi->totalp_v = 0.0;
        cpi->totalp = 0.0;
        cpi->tot_recode_hits = 0;
        cpi->summed_quality = 0;
        cpi->summed_weights = 0;
    }

    if (cpi->b_calculate_ssimg)
    {
        cpi->total_ssimg_y = 0;
        cpi->total_ssimg_u = 0;
        cpi->total_ssimg_v = 0;
        cpi->total_ssimg_all = 0;
    }

#ifndef LLONG_MAX
#define LLONG_MAX  9223372036854775807LL
#endif
    cpi->first_time_stamp_ever = LLONG_MAX;

#endif

    cpi->frames_till_gf_update_due      = 0;
    cpi->key_frame_count              = 1;
    cpi->tot_key_frame_bits            = 0;

    cpi->ni_av_qi                     = cpi->oxcf.worst_allowed_q;
    cpi->ni_tot_qi                    = 0;
    cpi->ni_frames                   = 0;
    cpi->total_byte_count             = 0;

    cpi->drop_frame                  = 0;
    cpi->drop_count                  = 0;
    cpi->max_drop_count               = 0;
    cpi->max_consec_dropped_frames     = 4;

    cpi->rate_correction_factor         = 1.0;
    cpi->key_frame_rate_correction_factor = 1.0;
    cpi->gf_rate_correction_factor  = 1.0;
    cpi->est_max_qcorrection_factor  = 1.0;

    cpi->mb.mvcost[0] = &cpi->mb.mvcosts[0][mv_max+1];
    cpi->mb.mvcost[1] = &cpi->mb.mvcosts[1][mv_max+1];
    cpi->mb.mvsadcost[0] = &cpi->mb.mvsadcosts[0][mv_max+1];
    cpi->mb.mvsadcost[1] = &cpi->mb.mvsadcosts[1][mv_max+1];

    cal_mvsadcosts(cpi->mb.mvsadcost);

    for (i = 0; i < KEY_FRAME_CONTEXT; i++)
    {
        cpi->prior_key_frame_size[i]     = cpi->intra_frame_target;
        cpi->prior_key_frame_distance[i] = (int)cpi->output_frame_rate;
    }

    cpi->check_freq[0] = 15;
    cpi->check_freq[1] = 15;

#ifdef OUTPUT_YUV_SRC
    yuv_file = fopen("bd.yuv", "ab");
#endif

#if 0
    framepsnr = fopen("framepsnr.stt", "a");
    kf_list = fopen("kf_list.stt", "w");
#endif

    cpi->output_pkt_list = oxcf->output_pkt_list;

#if !(CONFIG_REALTIME_ONLY)

    if (cpi->pass == 1)
    {
        vp8_init_first_pass(cpi);
    }
    else if (cpi->pass == 2)
    {
        size_t packet_sz = vp8_firstpass_stats_sz(cpi->common.MBs);
        int packets = oxcf->two_pass_stats_in.sz / packet_sz;

        cpi->stats_in = oxcf->two_pass_stats_in.buf;
        cpi->stats_in_end = (void*)((char *)cpi->stats_in
                            + (packets - 1) * packet_sz);
        vp8_init_second_pass(cpi);
    }

#endif

    if (cpi->compressor_speed == 2)
    {
        cpi->cpu_freq            = 0; //vp8_get_processor_freq();
        cpi->avg_encode_time      = 0;
        cpi->avg_pick_mode_time    = 0;
    }

    vp8_set_speed_features(cpi);

    // Set starting values of RD threshold multipliers (128 = *1)
    for (i = 0; i < MAX_MODES; i++)
    {
        cpi->rd_thresh_mult[i] = 128;
    }

#ifdef ENTROPY_STATS
    init_mv_ref_counts();
#endif

    vp8cx_create_encoder_threads(cpi);

    cpi->fn_ptr[BLOCK_16X16].sdf            = VARIANCE_INVOKE(&cpi->rtcd.variance, sad16x16);
    cpi->fn_ptr[BLOCK_16X16].vf             = VARIANCE_INVOKE(&cpi->rtcd.variance, var16x16);
    cpi->fn_ptr[BLOCK_16X16].svf            = VARIANCE_INVOKE(&cpi->rtcd.variance, subpixvar16x16);
    cpi->fn_ptr[BLOCK_16X16].svf_halfpix_h  = VARIANCE_INVOKE(&cpi->rtcd.variance, halfpixvar16x16_h);
    cpi->fn_ptr[BLOCK_16X16].svf_halfpix_v  = VARIANCE_INVOKE(&cpi->rtcd.variance, halfpixvar16x16_v);
    cpi->fn_ptr[BLOCK_16X16].svf_halfpix_hv = VARIANCE_INVOKE(&cpi->rtcd.variance, halfpixvar16x16_hv);
    cpi->fn_ptr[BLOCK_16X16].sdx3f          = VARIANCE_INVOKE(&cpi->rtcd.variance, sad16x16x3);
    cpi->fn_ptr[BLOCK_16X16].sdx4df         = VARIANCE_INVOKE(&cpi->rtcd.variance, sad16x16x4d);

    cpi->fn_ptr[BLOCK_16X8].sdf            = VARIANCE_INVOKE(&cpi->rtcd.variance, sad16x8);
    cpi->fn_ptr[BLOCK_16X8].vf             = VARIANCE_INVOKE(&cpi->rtcd.variance, var16x8);
    cpi->fn_ptr[BLOCK_16X8].svf            = VARIANCE_INVOKE(&cpi->rtcd.variance, subpixvar16x8);
    cpi->fn_ptr[BLOCK_16X8].svf_halfpix_h  = NULL;
    cpi->fn_ptr[BLOCK_16X8].svf_halfpix_v  = NULL;
    cpi->fn_ptr[BLOCK_16X8].svf_halfpix_hv = NULL;
    cpi->fn_ptr[BLOCK_16X8].sdx3f          = VARIANCE_INVOKE(&cpi->rtcd.variance, sad16x8x3);
    cpi->fn_ptr[BLOCK_16X8].sdx4df         = VARIANCE_INVOKE(&cpi->rtcd.variance, sad16x8x4d);

    cpi->fn_ptr[BLOCK_8X16].sdf            = VARIANCE_INVOKE(&cpi->rtcd.variance, sad8x16);
    cpi->fn_ptr[BLOCK_8X16].vf             = VARIANCE_INVOKE(&cpi->rtcd.variance, var8x16);
    cpi->fn_ptr[BLOCK_8X16].svf            = VARIANCE_INVOKE(&cpi->rtcd.variance, subpixvar8x16);
    cpi->fn_ptr[BLOCK_8X16].svf_halfpix_h  = NULL;
    cpi->fn_ptr[BLOCK_8X16].svf_halfpix_v  = NULL;
    cpi->fn_ptr[BLOCK_8X16].svf_halfpix_hv = NULL;
    cpi->fn_ptr[BLOCK_8X16].sdx3f          = VARIANCE_INVOKE(&cpi->rtcd.variance, sad8x16x3);
    cpi->fn_ptr[BLOCK_8X16].sdx4df         = VARIANCE_INVOKE(&cpi->rtcd.variance, sad8x16x4d);

    cpi->fn_ptr[BLOCK_8X8].sdf            = VARIANCE_INVOKE(&cpi->rtcd.variance, sad8x8);
    cpi->fn_ptr[BLOCK_8X8].vf             = VARIANCE_INVOKE(&cpi->rtcd.variance, var8x8);
    cpi->fn_ptr[BLOCK_8X8].svf            = VARIANCE_INVOKE(&cpi->rtcd.variance, subpixvar8x8);
    cpi->fn_ptr[BLOCK_8X8].svf_halfpix_h  = NULL;
    cpi->fn_ptr[BLOCK_8X8].svf_halfpix_v  = NULL;
    cpi->fn_ptr[BLOCK_8X8].svf_halfpix_hv = NULL;
    cpi->fn_ptr[BLOCK_8X8].sdx3f          = VARIANCE_INVOKE(&cpi->rtcd.variance, sad8x8x3);
    cpi->fn_ptr[BLOCK_8X8].sdx4df         = VARIANCE_INVOKE(&cpi->rtcd.variance, sad8x8x4d);

    cpi->fn_ptr[BLOCK_4X4].sdf            = VARIANCE_INVOKE(&cpi->rtcd.variance, sad4x4);
    cpi->fn_ptr[BLOCK_4X4].vf             = VARIANCE_INVOKE(&cpi->rtcd.variance, var4x4);
    cpi->fn_ptr[BLOCK_4X4].svf            = VARIANCE_INVOKE(&cpi->rtcd.variance, subpixvar4x4);
    cpi->fn_ptr[BLOCK_4X4].svf_halfpix_h  = NULL;
    cpi->fn_ptr[BLOCK_4X4].svf_halfpix_v  = NULL;
    cpi->fn_ptr[BLOCK_4X4].svf_halfpix_hv = NULL;
    cpi->fn_ptr[BLOCK_4X4].sdx3f          = VARIANCE_INVOKE(&cpi->rtcd.variance, sad4x4x3);
    cpi->fn_ptr[BLOCK_4X4].sdx4df         = VARIANCE_INVOKE(&cpi->rtcd.variance, sad4x4x4d);

#if !(CONFIG_REALTIME_ONLY)
    cpi->full_search_sad = SEARCH_INVOKE(&cpi->rtcd.search, full_search);
#endif
    cpi->diamond_search_sad = SEARCH_INVOKE(&cpi->rtcd.search, diamond_search);

    cpi->ready_for_new_frame = 1;

    cpi->source_encode_index = 0;

    // make sure frame 1 is okay
    cpi->error_bins[0] = cpi->common.MBs;

    //vp8cx_init_quantizer() is first called here. Add check in vp8cx_frame_init_quantizer() so that vp8cx_init_quantizer is only called later
    //when needed. This will avoid unnecessary calls of vp8cx_init_quantizer() for every frame.
    vp8cx_init_quantizer(cpi);
    {
        vp8_init_loop_filter(cm);
        cm->last_frame_type = KEY_FRAME;
        cm->last_filter_type = cm->filter_type;
        cm->last_sharpness_level = cm->sharpness_level;
    }
    cpi->common.error.setjmp = 0;
    return (VP8_PTR) cpi;

}


void vp8_remove_compressor(VP8_PTR *ptr)
{
    VP8_COMP *cpi = (VP8_COMP *)(*ptr);

    if (!cpi)
        return;

    if (cpi && (cpi->common.current_video_frame > 0))
    {
#if !(CONFIG_REALTIME_ONLY)

        if (cpi->pass == 2)
        {
            vp8_end_second_pass(cpi);
        }

#endif

#ifdef ENTROPY_STATS
        print_context_counters();
        print_tree_update_probs();
        print_mode_context();
#endif

#if CONFIG_PSNR

        if (cpi->pass != 1)
        {
            FILE *f = fopen("opsnr.stt", "a");
            double time_encoded = (cpi->source_end_time_stamp - cpi->first_time_stamp_ever) / 10000000.000;
            double total_encode_time = (cpi->time_receive_data + cpi->time_compress_data)   / 1000.000;
            double dr = (double)cpi->bytes * (double) 8 / (double)1000  / time_encoded;

            if (cpi->b_calculate_psnr)
            {
                YV12_BUFFER_CONFIG *lst_yv12 = &cpi->common.yv12_fb[cpi->common.lst_fb_idx];
                double samples = 3.0 / 2 * cpi->count * lst_yv12->y_width * lst_yv12->y_height;
                double total_psnr = vp8_mse2psnr(samples, 255.0, cpi->total_sq_error);
                double total_psnr2 = vp8_mse2psnr(samples, 255.0, cpi->total_sq_error2);
                double total_ssim = 100 * pow(cpi->summed_quality / cpi->summed_weights, 8.0);

                fprintf(f, "Bitrate\tAVGPsnr\tGLBPsnr\tAVPsnrP\tGLPsnrP\tVPXSSIM\t  Time(us)\n");
                fprintf(f, "%7.3f\t%7.3f\t%7.3f\t%7.3f\t%7.3f\t%7.3f %8.0f\n",
                        dr, cpi->total / cpi->count, total_psnr, cpi->totalp / cpi->count, total_psnr2, total_ssim,
                        total_encode_time);
            }

            if (cpi->b_calculate_ssimg)
            {
                fprintf(f, "BitRate\tSSIM_Y\tSSIM_U\tSSIM_V\tSSIM_A\t  Time(us)\n");
                fprintf(f, "%7.3f\t%6.4f\t%6.4f\t%6.4f\t%6.4f\t%8.0f\n", dr,
                        cpi->total_ssimg_y / cpi->count, cpi->total_ssimg_u / cpi->count,
                        cpi->total_ssimg_v / cpi->count, cpi->total_ssimg_all / cpi->count, total_encode_time);
            }

            fclose(f);
#if 0
            f = fopen("qskip.stt", "a");
            fprintf(f, "minq:%d -maxq:%d skipture:skipfalse = %d:%d\n", cpi->oxcf.best_allowed_q, cpi->oxcf.worst_allowed_q, skiptruecount, skipfalsecount);
            fclose(f);
#endif

        }

#endif


#ifdef SPEEDSTATS

        if (cpi->compressor_speed == 2)
        {
            int i;
            FILE *f = fopen("cxspeed.stt", "a");
            cnt_pm /= cpi->common.MBs;

            for (i = 0; i < 16; i++)
                fprintf(f, "%5d", frames_at_speed[i]);

            fprintf(f, "\n");
            //fprintf(f, "%10d PM %10d %10d %10d EF %10d %10d %10d\n", cpi->Speed, cpi->avg_pick_mode_time, (tot_pm/cnt_pm), cnt_pm,  cpi->avg_encode_time, 0, 0);
            fclose(f);
        }

#endif


#ifdef MODE_STATS
        {
            extern int count_mb_seg[4];
            FILE *f = fopen("modes.stt", "a");
            double dr = (double)cpi->oxcf.frame_rate * (double)bytes * (double)8 / (double)count / (double)1000 ;
            fprintf(f, "intra_mode in Intra Frames:\n");
            fprintf(f, "Y: %8d, %8d, %8d, %8d, %8d\n", y_modes[0], y_modes[1], y_modes[2], y_modes[3], y_modes[4]);
            fprintf(f, "UV:%8d, %8d, %8d, %8d\n", uv_modes[0], uv_modes[1], uv_modes[2], uv_modes[3]);
            fprintf(f, "B: ");
            {
                int i;

                for (i = 0; i < 10; i++)
                    fprintf(f, "%8d, ", b_modes[i]);

                fprintf(f, "\n");

            }

            fprintf(f, "Modes in Inter Frames:\n");
            fprintf(f, "Y: %8d, %8d, %8d, %8d, %8d, %8d, %8d, %8d, %8d, %8d\n",
                    inter_y_modes[0], inter_y_modes[1], inter_y_modes[2], inter_y_modes[3], inter_y_modes[4],
                    inter_y_modes[5], inter_y_modes[6], inter_y_modes[7], inter_y_modes[8], inter_y_modes[9]);
            fprintf(f, "UV:%8d, %8d, %8d, %8d\n", inter_uv_modes[0], inter_uv_modes[1], inter_uv_modes[2], inter_uv_modes[3]);
            fprintf(f, "B: ");
            {
                int i;

                for (i = 0; i < 15; i++)
                    fprintf(f, "%8d, ", inter_b_modes[i]);

                fprintf(f, "\n");

            }
            fprintf(f, "P:%8d, %8d, %8d, %8d\n", count_mb_seg[0], count_mb_seg[1], count_mb_seg[2], count_mb_seg[3]);
            fprintf(f, "PB:%8d, %8d, %8d, %8d\n", inter_b_modes[LEFT4X4], inter_b_modes[ABOVE4X4], inter_b_modes[ZERO4X4], inter_b_modes[NEW4X4]);



            fclose(f);
        }
#endif

#ifdef ENTROPY_STATS
        {
            int i, j, k;
            FILE *fmode = fopen("modecontext.c", "w");

            fprintf(fmode, "\n#include \"entropymode.h\"\n\n");
            fprintf(fmode, "const unsigned int vp8_kf_default_bmode_counts ");
            fprintf(fmode, "[VP8_BINTRAMODES] [VP8_BINTRAMODES] [VP8_BINTRAMODES] =\n{\n");

            for (i = 0; i < 10; i++)
            {

                fprintf(fmode, "    { //Above Mode :  %d\n", i);

                for (j = 0; j < 10; j++)
                {

                    fprintf(fmode, "        {");

                    for (k = 0; k < 10; k++)
                    {
                        if (!intra_mode_stats[i][j][k])
                            fprintf(fmode, " %5d, ", 1);
                        else
                            fprintf(fmode, " %5d, ", intra_mode_stats[i][j][k]);
                    }

                    fprintf(fmode, "}, // left_mode %d\n", j);

                }

                fprintf(fmode, "    },\n");

            }

            fprintf(fmode, "};\n");
            fclose(fmode);
        }
#endif


#if defined(SECTIONBITS_OUTPUT)

        if (0)
        {
            int i;
            FILE *f = fopen("tokenbits.stt", "a");

            for (i = 0; i < 28; i++)
                fprintf(f, "%8d", (int)(Sectionbits[i] / 256));

            fprintf(f, "\n");
            fclose(f);
        }

#endif

#if 0
        {
            printf("\n_pick_loop_filter_level:%d\n", cpi->time_pick_lpf / 1000);
            printf("\n_frames recive_data encod_mb_row compress_frame  Total\n");
            printf("%6d %10ld %10ld %10ld %10ld\n", cpi->common.current_video_frame, cpi->time_receive_data / 1000, cpi->time_encode_mb_row / 1000, cpi->time_compress_data / 1000, (cpi->time_receive_data + cpi->time_compress_data) / 1000);
        }
#endif

    }

    vp8cx_remove_encoder_threads(cpi);

    vp8_dealloc_compressor_data(cpi);
    vpx_free(cpi->mb.ss);
    vpx_free(cpi->tok);
    vpx_free(cpi->rdtok);
    vpx_free(cpi->cyclic_refresh_map);

    vp8_remove_common(&cpi->common);
    vpx_free(cpi);
    *ptr = 0;

#ifdef OUTPUT_YUV_SRC
    fclose(yuv_file);
#endif

#if 0

    if (keyfile)
        fclose(keyfile);

    if (framepsnr)
        fclose(framepsnr);

    if (kf_list)
        fclose(kf_list);

#endif

}


static uint64_t calc_plane_error(unsigned char *orig, int orig_stride,
                                 unsigned char *recon, int recon_stride,
                                 unsigned int cols, unsigned int rows,
                                 vp8_variance_rtcd_vtable_t *rtcd)
{
    unsigned int row, col;
    uint64_t total_sse = 0;
    int diff;

    for (row = 0; row + 16 <= rows; row += 16)
    {
        for (col = 0; col + 16 <= cols; col += 16)
        {
            unsigned int sse;

            VARIANCE_INVOKE(rtcd, mse16x16)(orig + col, orig_stride,
                                            recon + col, recon_stride,
                                            &sse);
            total_sse += sse;
        }

        /* Handle odd-sized width */
        if (col < cols)
        {
            unsigned int   border_row, border_col;
            unsigned char *border_orig = orig;
            unsigned char *border_recon = recon;

            for (border_row = 0; border_row < 16; border_row++)
            {
                for (border_col = col; border_col < cols; border_col++)
                {
                    diff = border_orig[border_col] - border_recon[border_col];
                    total_sse += diff * diff;
                }

                border_orig += orig_stride;
                border_recon += recon_stride;
            }
        }

        orig += orig_stride * 16;
        recon += recon_stride * 16;
    }

    /* Handle odd-sized height */
    for (; row < rows; row++)
    {
        for (col = 0; col < cols; col++)
        {
            diff = orig[col] - recon[col];
            total_sse += diff * diff;
        }

        orig += orig_stride;
        recon += recon_stride;
    }

    return total_sse;
}


static void generate_psnr_packet(VP8_COMP *cpi)
{
    YV12_BUFFER_CONFIG      *orig = cpi->Source;
    YV12_BUFFER_CONFIG      *recon = cpi->common.frame_to_show;
    struct vpx_codec_cx_pkt  pkt;
    uint64_t                 sse;
    int                      i;
    unsigned int             width = cpi->common.Width;
    unsigned int             height = cpi->common.Height;

    pkt.kind = VPX_CODEC_PSNR_PKT;
    sse = calc_plane_error(orig->y_buffer, orig->y_stride,
                           recon->y_buffer, recon->y_stride,
                           width, height,
                           IF_RTCD(&cpi->rtcd.variance));
    pkt.data.psnr.sse[0] = sse;
    pkt.data.psnr.sse[1] = sse;
    pkt.data.psnr.samples[0] = width * height;
    pkt.data.psnr.samples[1] = width * height;

    width = (width + 1) / 2;
    height = (height + 1) / 2;

    sse = calc_plane_error(orig->u_buffer, orig->uv_stride,
                           recon->u_buffer, recon->uv_stride,
                           width, height,
                           IF_RTCD(&cpi->rtcd.variance));
    pkt.data.psnr.sse[0] += sse;
    pkt.data.psnr.sse[2] = sse;
    pkt.data.psnr.samples[0] += width * height;
    pkt.data.psnr.samples[2] = width * height;

    sse = calc_plane_error(orig->v_buffer, orig->uv_stride,
                           recon->v_buffer, recon->uv_stride,
                           width, height,
                           IF_RTCD(&cpi->rtcd.variance));
    pkt.data.psnr.sse[0] += sse;
    pkt.data.psnr.sse[3] = sse;
    pkt.data.psnr.samples[0] += width * height;
    pkt.data.psnr.samples[3] = width * height;

    for (i = 0; i < 4; i++)
        pkt.data.psnr.psnr[i] = vp8_mse2psnr(pkt.data.psnr.samples[i], 255.0,
                                             pkt.data.psnr.sse[i]);

    vpx_codec_pkt_list_add(cpi->output_pkt_list, &pkt);
}


int vp8_use_as_reference(VP8_PTR ptr, int ref_frame_flags)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);

    if (ref_frame_flags > 7)
        return -1 ;

    cpi->ref_frame_flags = ref_frame_flags;
    return 0;
}
int vp8_update_reference(VP8_PTR ptr, int ref_frame_flags)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);

    if (ref_frame_flags > 7)
        return -1 ;

    cpi->common.refresh_golden_frame = 0;
    cpi->common.refresh_alt_ref_frame = 0;
    cpi->common.refresh_last_frame   = 0;

    if (ref_frame_flags & VP8_LAST_FLAG)
        cpi->common.refresh_last_frame = 1;

    if (ref_frame_flags & VP8_GOLD_FLAG)
        cpi->common.refresh_golden_frame = 1;

    if (ref_frame_flags & VP8_ALT_FLAG)
        cpi->common.refresh_alt_ref_frame = 1;

    return 0;
}

int vp8_get_reference(VP8_PTR ptr, VP8_REFFRAME ref_frame_flag, YV12_BUFFER_CONFIG *sd)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);
    VP8_COMMON *cm = &cpi->common;
    int ref_fb_idx;

    if (ref_frame_flag == VP8_LAST_FLAG)
        ref_fb_idx = cm->lst_fb_idx;
    else if (ref_frame_flag == VP8_GOLD_FLAG)
        ref_fb_idx = cm->gld_fb_idx;
    else if (ref_frame_flag == VP8_ALT_FLAG)
        ref_fb_idx = cm->alt_fb_idx;
    else
        return -1;

    vp8_yv12_copy_frame_ptr(&cm->yv12_fb[ref_fb_idx], sd);

    return 0;
}
int vp8_set_reference(VP8_PTR ptr, VP8_REFFRAME ref_frame_flag, YV12_BUFFER_CONFIG *sd)
{
    VP8_COMP *cpi = (VP8_COMP *)(ptr);
    VP8_COMMON *cm = &cpi->common;

    int ref_fb_idx;

    if (ref_frame_flag == VP8_LAST_FLAG)
        ref_fb_idx = cm->lst_fb_idx;
    else if (ref_frame_flag == VP8_GOLD_FLAG)
        ref_fb_idx = cm->gld_fb_idx;
    else if (ref_frame_flag == VP8_ALT_FLAG)
        ref_fb_idx = cm->alt_fb_idx;
    else
        return -1;

    vp8_yv12_copy_frame_ptr(sd, &cm->yv12_fb[ref_fb_idx]);

    return 0;
}
int vp8_update_entropy(VP8_PTR comp, int update)
{
    VP8_COMP *cpi = (VP8_COMP *) comp;
    VP8_COMMON *cm = &cpi->common;
    cm->refresh_entropy_probs = update;

    return 0;
}


#if OUTPUT_YUV_SRC
void vp8_write_yuv_frame(const char *name, YV12_BUFFER_CONFIG *s)
{
    FILE *yuv_file = fopen(name, "ab");
    unsigned char *src = s->y_buffer;
    int h = s->y_height;

    do
    {
        fwrite(src, s->y_width, 1,  yuv_file);
        src += s->y_stride;
    }
    while (--h);

    src = s->u_buffer;
    h = s->uv_height;

    do
    {
        fwrite(src, s->uv_width, 1,  yuv_file);
        src += s->uv_stride;
    }
    while (--h);

    src = s->v_buffer;
    h = s->uv_height;

    do
    {
        fwrite(src, s->uv_width, 1, yuv_file);
        src += s->uv_stride;
    }
    while (--h);

    fclose(yuv_file);
}
#endif


static void scale_and_extend_source(YV12_BUFFER_CONFIG *sd, VP8_COMP *cpi)
{
    VP8_COMMON *cm = &cpi->common;

    // are we resizing the image
    if (cm->horiz_scale != 0 || cm->vert_scale != 0)
    {
#if CONFIG_SPATIAL_RESAMPLING
        int UNINITIALIZED_IS_SAFE(hr), UNINITIALIZED_IS_SAFE(hs);
        int UNINITIALIZED_IS_SAFE(vr), UNINITIALIZED_IS_SAFE(vs);
        int tmp_height;

        if (cm->vert_scale == 3)
            tmp_height = 9;
        else
            tmp_height = 11;

        Scale2Ratio(cm->horiz_scale, &hr, &hs);
        Scale2Ratio(cm->vert_scale, &vr, &vs);

        vp8_scale_frame(sd, &cpi->scaled_source, cm->temp_scale_frame.y_buffer,
                        tmp_height, hs, hr, vs, vr, 0);

        cpi->Source = &cpi->scaled_source;
#endif
    }
    // we may need to copy to a buffer so we can extend the image...
    else if (cm->Width != cm->yv12_fb[cm->lst_fb_idx].y_width ||
             cm->Height != cm->yv12_fb[cm->lst_fb_idx].y_height)
    {
        //vp8_yv12_copy_frame_ptr(sd, &cpi->scaled_source);
#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
        if (cm->rtcd.flags & HAS_NEON)
#endif
        {
            vp8_yv12_copy_src_frame_func_neon(sd, &cpi->scaled_source);
        }
#if CONFIG_RUNTIME_CPU_DETECT
        else
#endif
#endif
#if !HAVE_ARMV7 || CONFIG_RUNTIME_CPU_DETECT
        {
            vp8_yv12_copy_frame_ptr(sd, &cpi->scaled_source);
        }
#endif

        cpi->Source = &cpi->scaled_source;
    }

    vp8_extend_to_multiple_of16(cpi->Source, cm->Width, cm->Height);

}
static void resize_key_frame(VP8_COMP *cpi)
{
#if CONFIG_SPATIAL_RESAMPLING
    VP8_COMMON *cm = &cpi->common;

    // Do we need to apply resampling for one pass cbr.
    // In one pass this is more limited than in two pass cbr
    // The test and any change is only made one per key frame sequence
    if (cpi->oxcf.allow_spatial_resampling && (cpi->oxcf.end_usage == USAGE_STREAM_FROM_SERVER))
    {
        int UNINITIALIZED_IS_SAFE(hr), UNINITIALIZED_IS_SAFE(hs);
        int UNINITIALIZED_IS_SAFE(vr), UNINITIALIZED_IS_SAFE(vs);
        int new_width, new_height;

        // If we are below the resample DOWN watermark then scale down a notch.
        if (cpi->buffer_level < (cpi->oxcf.resample_down_water_mark * cpi->oxcf.optimal_buffer_level / 100))
        {
            cm->horiz_scale = (cm->horiz_scale < ONETWO) ? cm->horiz_scale + 1 : ONETWO;
            cm->vert_scale = (cm->vert_scale < ONETWO) ? cm->vert_scale + 1 : ONETWO;
        }
        // Should we now start scaling back up
        else if (cpi->buffer_level > (cpi->oxcf.resample_up_water_mark * cpi->oxcf.optimal_buffer_level / 100))
        {
            cm->horiz_scale = (cm->horiz_scale > NORMAL) ? cm->horiz_scale - 1 : NORMAL;
            cm->vert_scale = (cm->vert_scale > NORMAL) ? cm->vert_scale - 1 : NORMAL;
        }

        // Get the new hieght and width
        Scale2Ratio(cm->horiz_scale, &hr, &hs);
        Scale2Ratio(cm->vert_scale, &vr, &vs);
        new_width = ((hs - 1) + (cpi->oxcf.Width * hr)) / hs;
        new_height = ((vs - 1) + (cpi->oxcf.Height * vr)) / vs;

        // If the image size has changed we need to reallocate the buffers
        // and resample the source image
        if ((cm->Width != new_width) || (cm->Height != new_height))
        {
            cm->Width = new_width;
            cm->Height = new_height;
            vp8_alloc_compressor_data(cpi);
            scale_and_extend_source(cpi->un_scaled_source, cpi);
        }
    }

#endif
}
// return of 0 means drop frame
static int pick_frame_size(VP8_COMP *cpi)
{
    VP8_COMMON *cm = &cpi->common;

    // First Frame is a special case
    if (cm->current_video_frame == 0)
    {
#if !(CONFIG_REALTIME_ONLY)

        if (cpi->pass == 2)
            vp8_calc_auto_iframe_target_size(cpi);

        // 1 Pass there is no information on which to base size so use bandwidth per second * fixed fraction
        else
#endif
            cpi->this_frame_target = cpi->oxcf.target_bandwidth / 2;

        // in error resilient mode the first frame is bigger since it likely contains
        // all the static background
        if (cpi->oxcf.error_resilient_mode == 1 || (cpi->compressor_speed == 2))
        {
            cpi->this_frame_target *= 3;      // 5;
        }

        // Key frame from VFW/auto-keyframe/first frame
        cm->frame_type = KEY_FRAME;

    }
    // Special case for forced key frames
    // The frame sizing here is still far from ideal for 2 pass.
    else if (cm->frame_flags & FRAMEFLAGS_KEY)
    {
        cm->frame_type = KEY_FRAME;
        resize_key_frame(cpi);
        vp8_calc_iframe_target_size(cpi);
    }
    else if (cm->frame_type == KEY_FRAME)
    {
        vp8_calc_auto_iframe_target_size(cpi);
    }
    else
    {
        // INTER frame: compute target frame size
        cm->frame_type = INTER_FRAME;
        vp8_calc_pframe_target_size(cpi);

        // Check if we're dropping the frame:
        if (cpi->drop_frame)
        {
            cpi->drop_frame = FALSE;
            cpi->drop_count++;
            return 0;
        }
    }

    // Note target_size in bits * 256 per MB
    cpi->target_bits_per_mb = (cpi->this_frame_target * 256) / cpi->common.MBs;

    return 1;
}
static void set_quantizer(VP8_COMP *cpi, int Q)
{
    VP8_COMMON *cm = &cpi->common;
    MACROBLOCKD *mbd = &cpi->mb.e_mbd;

    cm->base_qindex = Q;

    cm->y1dc_delta_q = 0;
    cm->y2dc_delta_q = 0;
    cm->y2ac_delta_q = 0;
    cm->uvdc_delta_q = 0;
    cm->uvac_delta_q = 0;

    // Set Segment specific quatizers
    mbd->segment_feature_data[MB_LVL_ALT_Q][0] = cpi->segment_feature_data[MB_LVL_ALT_Q][0];
    mbd->segment_feature_data[MB_LVL_ALT_Q][1] = cpi->segment_feature_data[MB_LVL_ALT_Q][1];
    mbd->segment_feature_data[MB_LVL_ALT_Q][2] = cpi->segment_feature_data[MB_LVL_ALT_Q][2];
    mbd->segment_feature_data[MB_LVL_ALT_Q][3] = cpi->segment_feature_data[MB_LVL_ALT_Q][3];
}

static void update_alt_ref_frame_and_stats(VP8_COMP *cpi)
{
    VP8_COMMON *cm = &cpi->common;

    // Update the golden frame buffer
    vp8_yv12_copy_frame_ptr(cm->frame_to_show, &cm->yv12_fb[cm->alt_fb_idx]);

    // Select an interval before next GF or altref
    if (!cpi->auto_gold)
        cpi->frames_till_gf_update_due = cpi->goldfreq;

    if ((cpi->pass != 2) && cpi->frames_till_gf_update_due)
    {
        cpi->current_gf_interval = cpi->frames_till_gf_update_due;

        // Set the bits per frame that we should try and recover in subsequent inter frames
        // to account for the extra GF spend... note that his does not apply for GF updates
        // that occur coincident with a key frame as the extra cost of key frames is dealt
        // with elsewhere.

        cpi->gf_overspend_bits += cpi->projected_frame_size;
        cpi->non_gf_bitrate_adjustment = cpi->gf_overspend_bits / cpi->frames_till_gf_update_due;
    }

    // Update data structure that monitors level of reference to last GF
    vpx_memset(cpi->gf_active_flags, 1, (cm->mb_rows * cm->mb_cols));
    cpi->gf_active_count = cm->mb_rows * cm->mb_cols;
    // this frame refreshes means next frames don't unless specified by user

    cpi->common.frames_since_golden = 0;

    // Clear the alternate reference update pending flag.
    cpi->source_alt_ref_pending = FALSE;

    // Set the alternate refernce frame active flag
    cpi->source_alt_ref_active = TRUE;


}
static void update_golden_frame_and_stats(VP8_COMP *cpi)
{
    VP8_COMMON *cm = &cpi->common;

    // Update the Golden frame reconstruction buffer if signalled and the GF usage counts.
    if (cm->refresh_golden_frame)
    {
        // Update the golden frame buffer
        vp8_yv12_copy_frame_ptr(cm->frame_to_show, &cm->yv12_fb[cm->gld_fb_idx]);

        // Select an interval before next GF
        if (!cpi->auto_gold)
            cpi->frames_till_gf_update_due = cpi->goldfreq;

        if ((cpi->pass != 2) && (cpi->frames_till_gf_update_due > 0))
        {
            cpi->current_gf_interval = cpi->frames_till_gf_update_due;

            // Set the bits per frame that we should try and recover in subsequent inter frames
            // to account for the extra GF spend... note that his does not apply for GF updates
            // that occur coincident with a key frame as the extra cost of key frames is dealt
            // with elsewhere.
            if ((cm->frame_type != KEY_FRAME) && !cpi->source_alt_ref_active)
            {
                // Calcluate GF bits to be recovered
                // Projected size - av frame bits available for inter frames for clip as a whole
                cpi->gf_overspend_bits += (cpi->projected_frame_size - cpi->inter_frame_target);
            }

            cpi->non_gf_bitrate_adjustment = cpi->gf_overspend_bits / cpi->frames_till_gf_update_due;

        }

        // Update data structure that monitors level of reference to last GF
        vpx_memset(cpi->gf_active_flags, 1, (cm->mb_rows * cm->mb_cols));
        cpi->gf_active_count = cm->mb_rows * cm->mb_cols;

        // this frame refreshes means next frames don't unless specified by user
        cm->refresh_golden_frame = 0;
        cpi->common.frames_since_golden = 0;

        //if ( cm->frame_type == KEY_FRAME )
        //{
        cpi->recent_ref_frame_usage[INTRA_FRAME] = 1;
        cpi->recent_ref_frame_usage[LAST_FRAME] = 1;
        cpi->recent_ref_frame_usage[GOLDEN_FRAME] = 1;
        cpi->recent_ref_frame_usage[ALTREF_FRAME] = 1;
        //}
        //else
        //{
        //  // Carry a potrtion of count over to begining of next gf sequence
        //  cpi->recent_ref_frame_usage[INTRA_FRAME] >>= 5;
        //  cpi->recent_ref_frame_usage[LAST_FRAME] >>= 5;
        //  cpi->recent_ref_frame_usage[GOLDEN_FRAME] >>= 5;
        //  cpi->recent_ref_frame_usage[ALTREF_FRAME] >>= 5;
        //}

        // ******** Fixed Q test code only ************
        // If we are going to use the ALT reference for the next group of frames set a flag to say so.
        if (cpi->oxcf.fixed_q >= 0 &&
            cpi->oxcf.play_alternate && !cpi->common.refresh_alt_ref_frame)
        {
            cpi->source_alt_ref_pending = TRUE;
            cpi->frames_till_gf_update_due = cpi->baseline_gf_interval;
        }

        if (!cpi->source_alt_ref_pending)
            cpi->source_alt_ref_active = FALSE;

        // Decrement count down till next gf
        if (cpi->frames_till_gf_update_due > 0)
            cpi->frames_till_gf_update_due--;

    }
    else if (!cpi->common.refresh_alt_ref_frame)
    {
        // Decrement count down till next gf
        if (cpi->frames_till_gf_update_due > 0)
            cpi->frames_till_gf_update_due--;

        if (cpi->common.frames_till_alt_ref_frame)
            cpi->common.frames_till_alt_ref_frame --;

        cpi->common.frames_since_golden ++;

        if (cpi->common.frames_since_golden > 1)
        {
            cpi->recent_ref_frame_usage[INTRA_FRAME] += cpi->count_mb_ref_frame_usage[INTRA_FRAME];
            cpi->recent_ref_frame_usage[LAST_FRAME] += cpi->count_mb_ref_frame_usage[LAST_FRAME];
            cpi->recent_ref_frame_usage[GOLDEN_FRAME] += cpi->count_mb_ref_frame_usage[GOLDEN_FRAME];
            cpi->recent_ref_frame_usage[ALTREF_FRAME] += cpi->count_mb_ref_frame_usage[ALTREF_FRAME];
        }
    }
}

// This function updates the reference frame probability estimates that
// will be used during mode selection
static void update_rd_ref_frame_probs(VP8_COMP *cpi)
{
    VP8_COMMON *cm = &cpi->common;

#if 0
    const int *const rfct = cpi->recent_ref_frame_usage;
    const int rf_intra = rfct[INTRA_FRAME];
    const int rf_inter = rfct[LAST_FRAME] + rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME];

    if (cm->frame_type == KEY_FRAME)
    {
        cpi->prob_intra_coded = 255;
        cpi->prob_last_coded  = 128;
        cpi->prob_gf_coded  = 128;
    }
    else if (!(rf_intra + rf_inter))
    {
        // This is a trap in case this function is called with cpi->recent_ref_frame_usage[] blank.
        cpi->prob_intra_coded = 63;
        cpi->prob_last_coded  = 128;
        cpi->prob_gf_coded    = 128;
    }
    else
    {
        cpi->prob_intra_coded = (rf_intra * 255) / (rf_intra + rf_inter);

        if (cpi->prob_intra_coded < 1)
            cpi->prob_intra_coded = 1;

        if ((cm->frames_since_golden > 0) || cpi->source_alt_ref_active)
        {
            cpi->prob_last_coded = rf_inter ? (rfct[LAST_FRAME] * 255) / rf_inter : 128;

            if (cpi->prob_last_coded < 1)
                cpi->prob_last_coded = 1;

            cpi->prob_gf_coded = (rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME])
                                 ? (rfct[GOLDEN_FRAME] * 255) / (rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME]) : 128;

            if (cpi->prob_gf_coded < 1)
                cpi->prob_gf_coded = 1;
        }
    }

#else
    const int *const rfct = cpi->count_mb_ref_frame_usage;
    const int rf_intra = rfct[INTRA_FRAME];
    const int rf_inter = rfct[LAST_FRAME] + rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME];

    if (cm->frame_type == KEY_FRAME)
    {
        cpi->prob_intra_coded = 255;
        cpi->prob_last_coded  = 128;
        cpi->prob_gf_coded  = 128;
    }
    else if (!(rf_intra + rf_inter))
    {
        // This is a trap in case this function is called with cpi->recent_ref_frame_usage[] blank.
        cpi->prob_intra_coded = 63;
        cpi->prob_last_coded  = 128;
        cpi->prob_gf_coded    = 128;
    }
    else
    {
        cpi->prob_intra_coded = (rf_intra * 255) / (rf_intra + rf_inter);

        if (cpi->prob_intra_coded < 1)
            cpi->prob_intra_coded = 1;

        cpi->prob_last_coded = rf_inter ? (rfct[LAST_FRAME] * 255) / rf_inter : 128;

        if (cpi->prob_last_coded < 1)
            cpi->prob_last_coded = 1;

        cpi->prob_gf_coded = (rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME])
                             ? (rfct[GOLDEN_FRAME] * 255) / (rfct[GOLDEN_FRAME] + rfct[ALTREF_FRAME]) : 128;

        if (cpi->prob_gf_coded < 1)
            cpi->prob_gf_coded = 1;
    }

    // update reference frame costs since we can do better than what we got last frame.

    if (cpi->common.refresh_alt_ref_frame)
    {
        cpi->prob_intra_coded += 40;
        cpi->prob_last_coded = 200;
        cpi->prob_gf_coded = 1;
    }
    else if (cpi->common.frames_since_golden == 0)
    {
        cpi->prob_last_coded = 214;
        cpi->prob_gf_coded = 1;
    }
    else if (cpi->common.frames_since_golden == 1)
    {
        cpi->prob_last_coded = 192;
        cpi->prob_gf_coded = 220;
    }
    else if (cpi->source_alt_ref_active)
    {
        //int dist = cpi->common.frames_till_alt_ref_frame + cpi->common.frames_since_golden;
        cpi->prob_gf_coded -= 20;

        if (cpi->prob_gf_coded < 10)
            cpi->prob_gf_coded = 10;
    }

#endif
}


// 1 = key, 0 = inter
static int decide_key_frame(VP8_COMP *cpi)
{
    VP8_COMMON *cm = &cpi->common;

    int code_key_frame = FALSE;

    cpi->kf_boost = 0;

    if (cpi->Speed > 11)
        return FALSE;

    // Clear down mmx registers
    vp8_clear_system_state();  //__asm emms;

    if ((cpi->compressor_speed == 2) && (cpi->Speed >= 5) && (cpi->sf.RD == 0))
    {
        double change = 1.0 * abs((int)(cpi->intra_error - cpi->last_intra_error)) / (1 + cpi->last_intra_error);
        double change2 = 1.0 * abs((int)(cpi->prediction_error - cpi->last_prediction_error)) / (1 + cpi->last_prediction_error);
        double minerror = cm->MBs * 256;

#if 0

        if (10 * cpi->intra_error / (1 + cpi->prediction_error) < 15
            && cpi->prediction_error > minerror
            && (change > .25 || change2 > .25))
        {
            FILE *f = fopen("intra_inter.stt", "a");

            if (cpi->prediction_error <= 0)
                cpi->prediction_error = 1;

            fprintf(f, "%d %d %d %d %14.4f\n",
                    cm->current_video_frame,
                    (int) cpi->prediction_error,
                    (int) cpi->intra_error,
                    (int)((10 * cpi->intra_error) / cpi->prediction_error),
                    change);

            fclose(f);
        }

#endif

        cpi->last_intra_error = cpi->intra_error;
        cpi->last_prediction_error = cpi->prediction_error;

        if (10 * cpi->intra_error / (1 + cpi->prediction_error) < 15
            && cpi->prediction_error > minerror
            && (change > .25 || change2 > .25))
        {
            /*(change > 1.4 || change < .75)&& cpi->this_frame_percent_intra > cpi->last_frame_percent_intra + 3*/
            return TRUE;
        }

        return FALSE;

    }

    // If the following are true we might as well code a key frame
    if (((cpi->this_frame_percent_intra == 100) &&
         (cpi->this_frame_percent_intra > (cpi->last_frame_percent_intra + 2))) ||
        ((cpi->this_frame_percent_intra > 95) &&
         (cpi->this_frame_percent_intra >= (cpi->last_frame_percent_intra + 5))))
    {
        code_key_frame = TRUE;
    }
    // in addition if the following are true and this is not a golden frame then code a key frame
    // Note that on golden frames there often seems to be a pop in intra useage anyway hence this
    // restriction is designed to prevent spurious key frames. The Intra pop needs to be investigated.
    else if (((cpi->this_frame_percent_intra > 60) &&
              (cpi->this_frame_percent_intra > (cpi->last_frame_percent_intra * 2))) ||
             ((cpi->this_frame_percent_intra > 75) &&
              (cpi->this_frame_percent_intra > (cpi->last_frame_percent_intra * 3 / 2))) ||
             ((cpi->this_frame_percent_intra > 90) &&
              (cpi->this_frame_percent_intra > (cpi->last_frame_percent_intra + 10))))
    {
        if (!cm->refresh_golden_frame)
            code_key_frame = TRUE;
    }

    return code_key_frame;

}

#if !(CONFIG_REALTIME_ONLY)
static void Pass1Encode(VP8_COMP *cpi, unsigned long *size, unsigned char *dest, unsigned int *frame_flags)
{
    (void) size;
    (void) dest;
    (void) frame_flags;
    set_quantizer(cpi, 26);

    scale_and_extend_source(cpi->un_scaled_source, cpi);
    vp8_first_pass(cpi);
}
#endif

#if 0
void write_cx_frame_to_file(YV12_BUFFER_CONFIG *frame, int this_frame)
{

    // write the frame
    FILE *yframe;
    int i;
    char filename[255];

    sprintf(filename, "cx\\y%04d.raw", this_frame);
    yframe = fopen(filename, "wb");

    for (i = 0; i < frame->y_height; i++)
        fwrite(frame->y_buffer + i * frame->y_stride, frame->y_width, 1, yframe);

    fclose(yframe);
    sprintf(filename, "cx\\u%04d.raw", this_frame);
    yframe = fopen(filename, "wb");

    for (i = 0; i < frame->uv_height; i++)
        fwrite(frame->u_buffer + i * frame->uv_stride, frame->uv_width, 1, yframe);

    fclose(yframe);
    sprintf(filename, "cx\\v%04d.raw", this_frame);
    yframe = fopen(filename, "wb");

    for (i = 0; i < frame->uv_height; i++)
        fwrite(frame->v_buffer + i * frame->uv_stride, frame->uv_width, 1, yframe);

    fclose(yframe);
}
#endif
// return of 0 means drop frame

static void encode_frame_to_data_rate
(
    VP8_COMP *cpi,
    unsigned long *size,
    unsigned char *dest,
    unsigned int *frame_flags
)
{
    int Q;
    int frame_over_shoot_limit;
    int frame_under_shoot_limit;

    int Loop = FALSE;
    int loop_count;
    int this_q;
    int last_zbin_oq;

    int q_low;
    int q_high;
    int zbin_oq_high;
    int zbin_oq_low = 0;
    int top_index;
    int bottom_index;
    VP8_COMMON *cm = &cpi->common;
    int active_worst_qchanged = FALSE;

    int overshoot_seen = FALSE;
    int undershoot_seen = FALSE;
    int drop_mark = cpi->oxcf.drop_frames_water_mark * cpi->oxcf.optimal_buffer_level / 100;
    int drop_mark75 = drop_mark * 2 / 3;
    int drop_mark50 = drop_mark / 4;
    int drop_mark25 = drop_mark / 8;

    // Clear down mmx registers to allow floating point in what follows
    vp8_clear_system_state();

    // Test code for segmentation of gf/arf (0,0)
    //segmentation_test_function((VP8_PTR) cpi);

    // For an alt ref frame in 2 pass we skip the call to the second pass function that sets the target bandwidth
#if !(CONFIG_REALTIME_ONLY)

    if (cpi->pass == 2)
    {
        if (cpi->common.refresh_alt_ref_frame)
        {
            cpi->per_frame_bandwidth = cpi->gf_bits;                           // Per frame bit target for the alt ref frame
            cpi->target_bandwidth = cpi->gf_bits * cpi->output_frame_rate;      // per second target bitrate
        }
    }
    else
#endif
        cpi->per_frame_bandwidth  = (int)(cpi->target_bandwidth / cpi->output_frame_rate);

    // Default turn off buffer to buffer copying
    cm->copy_buffer_to_gf = 0;
    cm->copy_buffer_to_arf = 0;

    // Clear zbin over-quant value and mode boost values.
    cpi->zbin_over_quant = 0;
    cpi->zbin_mode_boost = 0;

    // Enable mode based tweaking of the zbin
    cpi->zbin_mode_boost_enabled = TRUE;

    // Current default encoder behaviour for the altref sign bias
    if (cpi->source_alt_ref_active)
        cpi->common.ref_frame_sign_bias[ALTREF_FRAME] = 1;
    else
        cpi->common.ref_frame_sign_bias[ALTREF_FRAME] = 0;

    // Check to see if a key frame is signalled
    // For two pass with auto key frame enabled cm->frame_type may already be set, but not for one pass.
    if ((cm->current_video_frame == 0) ||
        (cm->frame_flags & FRAMEFLAGS_KEY) ||
        (cpi->oxcf.auto_key && (cpi->frames_since_key % cpi->key_frame_frequency == 0)))
    {
        // Key frame from VFW/auto-keyframe/first frame
        cm->frame_type = KEY_FRAME;
    }

    // Set default state for segment and mode based loop filter update flags
    cpi->mb.e_mbd.update_mb_segmentation_map = 0;
    cpi->mb.e_mbd.update_mb_segmentation_data = 0;
    cpi->mb.e_mbd.mode_ref_lf_delta_update = 0;

    // Set various flags etc to special state if it is a key frame
    if (cm->frame_type == KEY_FRAME)
    {
        int i;

        // Reset the loop filter deltas and segmentation map
        setup_features(cpi);

        // If segmentation is enabled force a map update for key frames
        if (cpi->mb.e_mbd.segmentation_enabled)
        {
            cpi->mb.e_mbd.update_mb_segmentation_map = 1;
            cpi->mb.e_mbd.update_mb_segmentation_data = 1;
        }

        // The alternate reference frame cannot be active for a key frame
        cpi->source_alt_ref_active = FALSE;

        // Reset the RD threshold multipliers to default of * 1 (128)
        for (i = 0; i < MAX_MODES; i++)
        {
            cpi->rd_thresh_mult[i] = 128;
        }
    }

    // Test code for segmentation
    //if ( (cm->frame_type == KEY_FRAME) || ((cm->current_video_frame % 2) == 0))
    //if ( (cm->current_video_frame % 2) == 0 )
    //  enable_segmentation((VP8_PTR)cpi);
    //else
    //  disable_segmentation((VP8_PTR)cpi);

#if 0
    // Experimental code for lagged compress and one pass
    // Initialise one_pass GF frames stats
    // Update stats used for GF selection
    //if ( cpi->pass == 0 )
    {
        cpi->one_pass_frame_index = cm->current_video_frame % MAX_LAG_BUFFERS;

        cpi->one_pass_frame_stats[cpi->one_pass_frame_index ].frames_so_far = 0;
        cpi->one_pass_frame_stats[cpi->one_pass_frame_index ].frame_intra_error = 0.0;
        cpi->one_pass_frame_stats[cpi->one_pass_frame_index ].frame_coded_error = 0.0;
        cpi->one_pass_frame_stats[cpi->one_pass_frame_index ].frame_pcnt_inter = 0.0;
        cpi->one_pass_frame_stats[cpi->one_pass_frame_index ].frame_pcnt_motion = 0.0;
        cpi->one_pass_frame_stats[cpi->one_pass_frame_index ].frame_mvr = 0.0;
        cpi->one_pass_frame_stats[cpi->one_pass_frame_index ].frame_mvr_abs = 0.0;
        cpi->one_pass_frame_stats[cpi->one_pass_frame_index ].frame_mvc = 0.0;
        cpi->one_pass_frame_stats[cpi->one_pass_frame_index ].frame_mvc_abs = 0.0;
    }
#endif

    update_rd_ref_frame_probs(cpi);

    if (cpi->drop_frames_allowed)
    {
        // The reset to decimation 0 is only done here for one pass.
        // Once it is set two pass leaves decimation on till the next kf.
        if ((cpi->buffer_level > drop_mark) && (cpi->decimation_factor > 0))
            cpi->decimation_factor --;

        if (cpi->buffer_level > drop_mark75 && cpi->decimation_factor > 0)
            cpi->decimation_factor = 1;

        else if (cpi->buffer_level < drop_mark25 && (cpi->decimation_factor == 2 || cpi->decimation_factor == 3))
        {
            cpi->decimation_factor = 3;
        }
        else if (cpi->buffer_level < drop_mark50 && (cpi->decimation_factor == 1 || cpi->decimation_factor == 2))
        {
            cpi->decimation_factor = 2;
        }
        else if (cpi->buffer_level < drop_mark75 && (cpi->decimation_factor == 0 || cpi->decimation_factor == 1))
        {
            cpi->decimation_factor = 1;
        }

        //vpx_log("Encoder: Decimation Factor: %d \n",cpi->decimation_factor);
    }

    // The following decimates the frame rate according to a regular pattern (i.e. to 1/2 or 2/3 frame rate)
    // This can be used to help prevent buffer under-run in CBR mode. Alternatively it might be desirable in
    // some situations to drop frame rate but throw more bits at each frame.
    //
    // Note that dropping a key frame can be problematic if spatial resampling is also active
    if (cpi->decimation_factor > 0)
    {
        switch (cpi->decimation_factor)
        {
        case 1:
            cpi->per_frame_bandwidth  = cpi->per_frame_bandwidth * 3 / 2;
            break;
        case 2:
            cpi->per_frame_bandwidth  = cpi->per_frame_bandwidth * 5 / 4;
            break;
        case 3:
            cpi->per_frame_bandwidth  = cpi->per_frame_bandwidth * 5 / 4;
            break;
        }

        // Note that we should not throw out a key frame (especially when spatial resampling is enabled).
        if ((cm->frame_type == KEY_FRAME)) // && cpi->oxcf.allow_spatial_resampling )
        {
            cpi->decimation_count = cpi->decimation_factor;
        }
        else if (cpi->decimation_count > 0)
        {
            cpi->decimation_count --;
            cpi->bits_off_target += cpi->av_per_frame_bandwidth;
            cm->current_video_frame++;
            cpi->frames_since_key++;

#if CONFIG_PSNR
            cpi->count ++;
#endif

            cpi->buffer_level = cpi->bits_off_target;

            return;
        }
        else
            cpi->decimation_count = cpi->decimation_factor;
    }

    // Decide how big to make the frame
    if (!pick_frame_size(cpi))
    {
        cm->current_video_frame++;
        cpi->frames_since_key++;
        return;
    }

    // Reduce active_worst_allowed_q for CBR if our buffer is getting too full.
    // This has a knock on effect on active best quality as well.
    // For CBR if the buffer reaches its maximum level then we can no longer
    // save up bits for later frames so we might as well use them up
    // on the current frame.
    if ((cpi->oxcf.end_usage == USAGE_STREAM_FROM_SERVER) &&
        (cpi->buffer_level >= cpi->oxcf.optimal_buffer_level) && cpi->buffered_mode)
    {
        int Adjustment = cpi->active_worst_quality / 4;       // Max adjustment is 1/4

        if (Adjustment)
        {
            int buff_lvl_step;
            int tmp_lvl = cpi->buffer_level;

            if (cpi->buffer_level < cpi->oxcf.maximum_buffer_size)
            {
                buff_lvl_step = (cpi->oxcf.maximum_buffer_size - cpi->oxcf.optimal_buffer_level) / Adjustment;

                if (buff_lvl_step)
                {
                    Adjustment = (cpi->buffer_level - cpi->oxcf.optimal_buffer_level) / buff_lvl_step;
                    cpi->active_worst_quality -= Adjustment;
                }
            }
            else
            {
                cpi->active_worst_quality -= Adjustment;
            }
        }
    }

    // Set an active best quality and if necessary active worst quality
    if (cpi->pass == 2 || (cm->current_video_frame > 150))
    {
        int Q;
        int i;
        int bpm_target;
        //int tmp;

        vp8_clear_system_state();

        Q = cpi->active_worst_quality;

        if ((cm->frame_type == KEY_FRAME) || cm->refresh_golden_frame || cpi->common.refresh_alt_ref_frame)
        {
            if (cm->frame_type != KEY_FRAME)
            {
                if (cpi->avg_frame_qindex < cpi->active_worst_quality)
                    Q = cpi->avg_frame_qindex;

               if ( cpi->gfu_boost > 1000 )
                    cpi->active_best_quality = gf_low_motion_minq[Q];
                else if ( cpi->gfu_boost < 400 )
                    cpi->active_best_quality = gf_high_motion_minq[Q];
                else
                    cpi->active_best_quality = gf_mid_motion_minq[Q];

                /*cpi->active_best_quality = gf_arf_minq[Q];
                tmp = (cpi->gfu_boost > 1000) ? 600 : cpi->gfu_boost - 400;
                //tmp = (cpi->gfu_boost > 1000) ? 600 :
                          //(cpi->gfu_boost < 400) ? 0 : cpi->gfu_boost - 400;
                tmp = 128 - (tmp >> 4);
                cpi->active_best_quality = (cpi->active_best_quality * tmp)>>7;*/

           }
           // KEY FRAMES
           else
           {
               if (cpi->gfu_boost > 600)
                   cpi->active_best_quality = kf_low_motion_minq[Q];
               else
                   cpi->active_best_quality = kf_high_motion_minq[Q];
           }
        }
        else
        {
            cpi->active_best_quality = inter_minq[Q];
        }

        // If CBR and the buffer is as full then it is reasonable to allow higher quality on the frames
        // to prevent bits just going to waste.
        if (cpi->oxcf.end_usage == USAGE_STREAM_FROM_SERVER)
        {
            // Note that the use of >= here elliminates the risk of a devide by 0 error in the else if clause
            if (cpi->buffer_level >= cpi->oxcf.maximum_buffer_size)
                cpi->active_best_quality = cpi->best_quality;

            else if (cpi->buffer_level > cpi->oxcf.optimal_buffer_level)
            {
                int Fraction = ((cpi->buffer_level - cpi->oxcf.optimal_buffer_level) * 128) / (cpi->oxcf.maximum_buffer_size - cpi->oxcf.optimal_buffer_level);
                int min_qadjustment = ((cpi->active_best_quality - cpi->best_quality) * Fraction) / 128;

                cpi->active_best_quality -= min_qadjustment;
            }

        }
    }

    // Clip the active best and worst quality values to limits
    if (cpi->active_worst_quality > cpi->worst_quality)
        cpi->active_worst_quality = cpi->worst_quality;

    if (cpi->active_best_quality < cpi->best_quality)
        cpi->active_best_quality = cpi->best_quality;
    else if (cpi->active_best_quality > cpi->active_worst_quality)
        cpi->active_best_quality = cpi->active_worst_quality;

    // Determine initial Q to try
    Q = vp8_regulate_q(cpi, cpi->this_frame_target);
    last_zbin_oq = cpi->zbin_over_quant;

    // Set highest allowed value for Zbin over quant
    if (cm->frame_type == KEY_FRAME)
        zbin_oq_high = 0; //ZBIN_OQ_MAX/16
    else if (cm->refresh_alt_ref_frame || (cm->refresh_golden_frame && !cpi->source_alt_ref_active))
        zbin_oq_high = 16;
    else
        zbin_oq_high = ZBIN_OQ_MAX;

    // Setup background Q adjustment for error resilliant mode
    if (cpi->cyclic_refresh_mode_enabled)
        cyclic_background_refresh(cpi, Q, 0);

    vp8_compute_frame_size_bounds(cpi, &frame_under_shoot_limit, &frame_over_shoot_limit);

    // Limit Q range for the adaptive loop (Values not clipped to range 20-60 as in VP8).
    bottom_index = cpi->active_best_quality;
    top_index    = cpi->active_worst_quality;

    vp8_save_coding_context(cpi);

    loop_count = 0;

    q_low  = cpi->best_quality;
    q_high = cpi->worst_quality;


    scale_and_extend_source(cpi->un_scaled_source, cpi);
#if !(CONFIG_REALTIME_ONLY) && CONFIG_POSTPROC

    if (cpi->oxcf.noise_sensitivity > 0)
    {
        unsigned char *src;
        int l = 0;

        switch (cpi->oxcf.noise_sensitivity)
        {
        case 1:
            l = 20;
            break;
        case 2:
            l = 40;
            break;
        case 3:
            l = 60;
            break;
        case 4:
            l = 80;
            break;
        case 5:
            l = 100;
            break;
        case 6:
            l = 150;
            break;
        }


        if (cm->frame_type == KEY_FRAME)
        {
            vp8_de_noise(cpi->Source, cpi->Source, l , 1,  0, RTCD(postproc));
            cpi->ppi.frame = 0;
        }
        else
        {
            vp8_de_noise(cpi->Source, cpi->Source, l , 1,  0, RTCD(postproc));

            src = cpi->Source->y_buffer;

            if (cpi->Source->y_stride < 0)
            {
                src += cpi->Source->y_stride * (cpi->Source->y_height - 1);
            }

            //temp_filter(&cpi->ppi,src,src,
            //  cm->last_frame.y_width * cm->last_frame.y_height,
            //  cpi->oxcf.noise_sensitivity);
        }
    }

#endif

#ifdef OUTPUT_YUV_SRC
    vp8_write_yuv_frame(cpi->Source);
#endif

    do
    {
        vp8_clear_system_state();  //__asm emms;

        /*
        if(cpi->is_src_frame_alt_ref)
            Q = 127;
            */

        set_quantizer(cpi, Q);
        this_q = Q;

        // setup skip prob for costing in mode/mv decision
        if (cpi->common.mb_no_coeff_skip)
        {
            cpi->prob_skip_false = cpi->base_skip_false_prob[Q];

            if (cm->frame_type != KEY_FRAME)
            {
                if (cpi->common.refresh_alt_ref_frame)
                {
                    if (cpi->last_skip_false_probs[2] != 0)
                        cpi->prob_skip_false = cpi->last_skip_false_probs[2];

                    /*
                                        if(cpi->last_skip_false_probs[2]!=0 && abs(Q- cpi->last_skip_probs_q[2])<=16 )
                       cpi->prob_skip_false = cpi->last_skip_false_probs[2];
                                        else if (cpi->last_skip_false_probs[2]!=0)
                       cpi->prob_skip_false = (cpi->last_skip_false_probs[2]  + cpi->prob_skip_false ) / 2;
                       */
                }
                else if (cpi->common.refresh_golden_frame)
                {
                    if (cpi->last_skip_false_probs[1] != 0)
                        cpi->prob_skip_false = cpi->last_skip_false_probs[1];

                    /*
                                        if(cpi->last_skip_false_probs[1]!=0 && abs(Q- cpi->last_skip_probs_q[1])<=16 )
                       cpi->prob_skip_false = cpi->last_skip_false_probs[1];
                                        else if (cpi->last_skip_false_probs[1]!=0)
                       cpi->prob_skip_false = (cpi->last_skip_false_probs[1]  + cpi->prob_skip_false ) / 2;
                       */
                }
                else
                {
                    if (cpi->last_skip_false_probs[0] != 0)
                        cpi->prob_skip_false = cpi->last_skip_false_probs[0];

                    /*
                    if(cpi->last_skip_false_probs[0]!=0 && abs(Q- cpi->last_skip_probs_q[0])<=16 )
                        cpi->prob_skip_false = cpi->last_skip_false_probs[0];
                    else if(cpi->last_skip_false_probs[0]!=0)
                        cpi->prob_skip_false = (cpi->last_skip_false_probs[0]  + cpi->prob_skip_false ) / 2;
                        */
                }

                //as this is for cost estimate, let's make sure it does not go extreme eitehr way
                if (cpi->prob_skip_false < 5)
                    cpi->prob_skip_false = 5;

                if (cpi->prob_skip_false > 250)
                    cpi->prob_skip_false = 250;

                if (cpi->is_src_frame_alt_ref)
                    cpi->prob_skip_false = 1;


            }

#if 0

            if (cpi->pass != 1)
            {
                FILE *f = fopen("skip.stt", "a");
                fprintf(f, "%d, %d, %4d ", cpi->common.refresh_golden_frame, cpi->common.refresh_alt_ref_frame, cpi->prob_skip_false);
                fclose(f);
            }

#endif

        }

        if (cm->frame_type == KEY_FRAME)
            vp8_setup_key_frame(cpi);

        // transform / motion compensation build reconstruction frame

        vp8_encode_frame(cpi);
        cpi->projected_frame_size -= vp8_estimate_entropy_savings(cpi);
        cpi->projected_frame_size = (cpi->projected_frame_size > 0) ? cpi->projected_frame_size : 0;

        vp8_clear_system_state();  //__asm emms;

        // Test to see if the stats generated for this frame indicate that we should have coded a key frame
        // (assuming that we didn't)!
        if (cpi->pass != 2 && cpi->oxcf.auto_key && cm->frame_type != KEY_FRAME)
        {
            if (decide_key_frame(cpi))
            {
                vp8_calc_auto_iframe_target_size(cpi);

                // Reset all our sizing numbers and recode
                cm->frame_type = KEY_FRAME;

                // Clear the Alt reference frame active flag when we have a key frame
                cpi->source_alt_ref_active = FALSE;

                // Reset the loop filter deltas and segmentation map
                setup_features(cpi);

                // If segmentation is enabled force a map update for key frames
                if (cpi->mb.e_mbd.segmentation_enabled)
                {
                    cpi->mb.e_mbd.update_mb_segmentation_map = 1;
                    cpi->mb.e_mbd.update_mb_segmentation_data = 1;
                }

                vp8_restore_coding_context(cpi);

                Q = vp8_regulate_q(cpi, cpi->this_frame_target);

                q_low  = cpi->best_quality;
                q_high = cpi->worst_quality;

                vp8_compute_frame_size_bounds(cpi, &frame_under_shoot_limit, &frame_over_shoot_limit);

                // Limit Q range for the adaptive loop (Values not clipped to range 20-60 as in VP8).
                bottom_index = cpi->active_best_quality;
                top_index    = cpi->active_worst_quality;


                loop_count++;
                Loop = TRUE;

                resize_key_frame(cpi);
                continue;
            }
        }

        vp8_clear_system_state();

        if (frame_over_shoot_limit == 0)
            frame_over_shoot_limit = 1;

        // Are we are overshooting and up against the limit of active max Q.
        if (((cpi->pass != 2) || (cpi->oxcf.end_usage == USAGE_STREAM_FROM_SERVER)) &&
            (Q == cpi->active_worst_quality)                     &&
            (cpi->active_worst_quality < cpi->worst_quality)      &&
            (cpi->projected_frame_size > frame_over_shoot_limit))
        {
            int over_size_percent = ((cpi->projected_frame_size - frame_over_shoot_limit) * 100) / frame_over_shoot_limit;

            // If so is there any scope for relaxing it
            while ((cpi->active_worst_quality < cpi->worst_quality) && (over_size_percent > 0))
            {
                cpi->active_worst_quality++;
                top_index = cpi->active_worst_quality;
                over_size_percent = (int)(over_size_percent * 0.96);        // Assume 1 qstep = about 4% on frame size.
            }

            // If we have updated the active max Q do not call vp8_update_rate_correction_factors() this loop.
            active_worst_qchanged = TRUE;
        }
        else
            active_worst_qchanged = FALSE;

#if !(CONFIG_REALTIME_ONLY)

        // Is the projected frame size out of range and are we allowed to attempt to recode.
        if (((cpi->sf.recode_loop == 1) ||
             ((cpi->sf.recode_loop == 2) && (cm->refresh_golden_frame || (cm->frame_type == KEY_FRAME)))) &&
            (((cpi->projected_frame_size > frame_over_shoot_limit) && (Q < top_index)) ||
             //((cpi->projected_frame_size > frame_over_shoot_limit ) && (Q == top_index) && (cpi->zbin_over_quant < ZBIN_OQ_MAX)) ||
             ((cpi->projected_frame_size < frame_under_shoot_limit) && (Q > bottom_index)))
           )
        {
            int last_q = Q;
            int Retries = 0;

            // Frame size out of permitted range:
            // Update correction factor & compute new Q to try...
            if (cpi->projected_frame_size > frame_over_shoot_limit)
            {
                //if ( cpi->zbin_over_quant == 0 )
                q_low = (Q < q_high) ? (Q + 1) : q_high; // Raise Qlow as to at least the current value

                if (cpi->zbin_over_quant > 0)           // If we are using over quant do the same for zbin_oq_low
                    zbin_oq_low = (cpi->zbin_over_quant < zbin_oq_high) ? (cpi->zbin_over_quant + 1) : zbin_oq_high;

                //if ( undershoot_seen || (Q == MAXQ) )
                if (undershoot_seen)
                {
                    // Update rate_correction_factor unless cpi->active_worst_quality has changed.
                    if (!active_worst_qchanged)
                        vp8_update_rate_correction_factors(cpi, 1);

                    Q = (q_high + q_low + 1) / 2;

                    // Adjust cpi->zbin_over_quant (only allowed when Q is max)
                    if (Q < MAXQ)
                        cpi->zbin_over_quant = 0;
                    else
                    {
                        zbin_oq_low = (cpi->zbin_over_quant < zbin_oq_high) ? (cpi->zbin_over_quant + 1) : zbin_oq_high;
                        cpi->zbin_over_quant = (zbin_oq_high + zbin_oq_low) / 2;
                    }
                }
                else
                {
                    // Update rate_correction_factor unless cpi->active_worst_quality has changed.
                    if (!active_worst_qchanged)
                        vp8_update_rate_correction_factors(cpi, 0);

                    Q = vp8_regulate_q(cpi, cpi->this_frame_target);

                    while (((Q < q_low) || (cpi->zbin_over_quant < zbin_oq_low)) && (Retries < 10))
                    {
                        vp8_update_rate_correction_factors(cpi, 0);
                        Q = vp8_regulate_q(cpi, cpi->this_frame_target);
                        Retries ++;
                    }
                }

                overshoot_seen = TRUE;
            }
            else
            {
                if (cpi->zbin_over_quant == 0)
                    q_high = (Q > q_low) ? (Q - 1) : q_low; // Lower q_high if not using over quant
                else                                    // else lower zbin_oq_high
                    zbin_oq_high = (cpi->zbin_over_quant > zbin_oq_low) ? (cpi->zbin_over_quant - 1) : zbin_oq_low;

                if (overshoot_seen)
                {
                    // Update rate_correction_factor unless cpi->active_worst_quality has changed.
                    if (!active_worst_qchanged)
                        vp8_update_rate_correction_factors(cpi, 1);

                    Q = (q_high + q_low) / 2;

                    // Adjust cpi->zbin_over_quant (only allowed when Q is max)
                    if (Q < MAXQ)
                        cpi->zbin_over_quant = 0;
                    else
                        cpi->zbin_over_quant = (zbin_oq_high + zbin_oq_low) / 2;
                }
                else
                {
                    // Update rate_correction_factor unless cpi->active_worst_quality has changed.
                    if (!active_worst_qchanged)
                        vp8_update_rate_correction_factors(cpi, 0);

                    Q = vp8_regulate_q(cpi, cpi->this_frame_target);

                    while (((Q > q_high) || (cpi->zbin_over_quant > zbin_oq_high)) && (Retries < 10))
                    {
                        vp8_update_rate_correction_factors(cpi, 0);
                        Q = vp8_regulate_q(cpi, cpi->this_frame_target);
                        Retries ++;
                    }
                }

                undershoot_seen = TRUE;
            }

            // Clamp Q to upper and lower limits:
            if (Q > q_high)
                Q = q_high;
            else if (Q < q_low)
                Q = q_low;

            // Clamp cpi->zbin_over_quant
            cpi->zbin_over_quant = (cpi->zbin_over_quant < zbin_oq_low) ? zbin_oq_low : (cpi->zbin_over_quant > zbin_oq_high) ? zbin_oq_high : cpi->zbin_over_quant;

            //Loop = ((Q != last_q) || (last_zbin_oq != cpi->zbin_over_quant)) ? TRUE : FALSE;
            Loop = ((Q != last_q)) ? TRUE : FALSE;
            last_zbin_oq = cpi->zbin_over_quant;
        }
        else
#endif
            Loop = FALSE;

        if (cpi->is_src_frame_alt_ref)
            Loop = FALSE;

        if (Loop == TRUE)
        {
            vp8_restore_coding_context(cpi);
            loop_count++;
#if CONFIG_PSNR
            cpi->tot_recode_hits++;
#endif
        }
    }
    while (Loop == TRUE);

#if 0
    // Experimental code for lagged and one pass
    // Update stats used for one pass GF selection
    {
        /*
            int frames_so_far;
            double frame_intra_error;
            double frame_coded_error;
            double frame_pcnt_inter;
            double frame_pcnt_motion;
            double frame_mvr;
            double frame_mvr_abs;
            double frame_mvc;
            double frame_mvc_abs;
        */

        cpi->one_pass_frame_stats[cpi->one_pass_frame_index].frame_coded_error = (double)cpi->prediction_error;
        cpi->one_pass_frame_stats[cpi->one_pass_frame_index].frame_intra_error = (double)cpi->intra_error;
        cpi->one_pass_frame_stats[cpi->one_pass_frame_index].frame_pcnt_inter = (double)(100 - cpi->this_frame_percent_intra) / 100.0;
    }
#endif

    // Update the GF useage maps.
    // This is done after completing the compression of a frame when all modes etc. are finalized but before loop filter
    vp8_update_gf_useage_maps(cpi, cm, &cpi->mb);

    if (cm->frame_type == KEY_FRAME)
        cm->refresh_last_frame = 1;

#if 0
    {
        FILE *f = fopen("gfactive.stt", "a");
        fprintf(f, "%8d %8d %8d %8d %8d\n", cm->current_video_frame, (100 * cpi->gf_active_count) / (cpi->common.mb_rows * cpi->common.mb_cols), cpi->this_iiratio, cpi->next_iiratio, cm->refresh_golden_frame);
        fclose(f);
    }
#endif

    // For inter frames the current default behaviour is that when cm->refresh_golden_frame is set we copy the old GF over to the ARF buffer
    // This is purely an encoder descision at present.
    if (!cpi->oxcf.error_resilient_mode && cm->refresh_golden_frame)
        cm->copy_buffer_to_arf  = 2;
    else
        cm->copy_buffer_to_arf  = 0;

    if (cm->refresh_last_frame)
    {
        vp8_swap_yv12_buffer(&cm->yv12_fb[cm->lst_fb_idx], &cm->yv12_fb[cm->new_fb_idx]);
        cm->frame_to_show = &cm->yv12_fb[cm->lst_fb_idx];
    }
    else
        cm->frame_to_show = &cm->yv12_fb[cm->new_fb_idx];



    //#pragma omp parallel sections
    {

        //#pragma omp section
        {

            struct vpx_usec_timer timer;

            vpx_usec_timer_start(&timer);

            if (cpi->sf.auto_filter == 0)
                vp8cx_pick_filter_level_fast(cpi->Source, cpi);
            else
                vp8cx_pick_filter_level(cpi->Source, cpi);

            vpx_usec_timer_mark(&timer);

            cpi->time_pick_lpf +=  vpx_usec_timer_elapsed(&timer);

            if (cm->no_lpf)
                cm->filter_level = 0;

            if (cm->filter_level > 0)
            {
                vp8cx_set_alt_lf_level(cpi, cm->filter_level);
                vp8_loop_filter_frame(cm, &cpi->mb.e_mbd, cm->filter_level);
                cm->last_frame_type = cm->frame_type;
                cm->last_filter_type = cm->filter_type;
                cm->last_sharpness_level = cm->sharpness_level;
            }

            vp8_yv12_extend_frame_borders_ptr(cm->frame_to_show);

            if (cpi->oxcf.error_resilient_mode == 1)
            {
                cm->refresh_entropy_probs = 0;
            }

        }
//#pragma omp section
        {
            // build the bitstream
            vp8_pack_bitstream(cpi, dest, size);
        }
    }

    {
        YV12_BUFFER_CONFIG *lst_yv12 = &cm->yv12_fb[cm->lst_fb_idx];
        YV12_BUFFER_CONFIG *new_yv12 = &cm->yv12_fb[cm->new_fb_idx];
        YV12_BUFFER_CONFIG *gld_yv12 = &cm->yv12_fb[cm->gld_fb_idx];
        YV12_BUFFER_CONFIG *alt_yv12 = &cm->yv12_fb[cm->alt_fb_idx];
        // At this point the new frame has been encoded coded.
        // If any buffer copy / swaping is signalled it should be done here.
        if (cm->frame_type == KEY_FRAME)
        {
            vp8_yv12_copy_frame_ptr(cm->frame_to_show, gld_yv12);
            vp8_yv12_copy_frame_ptr(cm->frame_to_show, alt_yv12);
        }
        else    // For non key frames
        {
            // Code to copy between reference buffers
            if (cm->copy_buffer_to_arf)
            {
                if (cm->copy_buffer_to_arf == 1)
                {
                    if (cm->refresh_last_frame)
                        // We copy new_frame here because last and new buffers will already have been swapped if cm->refresh_last_frame is set.
                        vp8_yv12_copy_frame_ptr(new_yv12, alt_yv12);
                    else
                        vp8_yv12_copy_frame_ptr(lst_yv12, alt_yv12);
                }
                else if (cm->copy_buffer_to_arf == 2)
                    vp8_yv12_copy_frame_ptr(gld_yv12, alt_yv12);
            }

            if (cm->copy_buffer_to_gf)
            {
                if (cm->copy_buffer_to_gf == 1)
                {
                    if (cm->refresh_last_frame)
                        // We copy new_frame here because last and new buffers will already have been swapped if cm->refresh_last_frame is set.
                        vp8_yv12_copy_frame_ptr(new_yv12, gld_yv12);
                    else
                        vp8_yv12_copy_frame_ptr(lst_yv12, gld_yv12);
                }
                else if (cm->copy_buffer_to_gf == 2)
                    vp8_yv12_copy_frame_ptr(alt_yv12, gld_yv12);
            }
        }
    }

    // Update rate control heuristics
    cpi->total_byte_count += (*size);
    cpi->projected_frame_size = (*size) << 3;

    if (!active_worst_qchanged)
        vp8_update_rate_correction_factors(cpi, 2);

    cpi->last_q[cm->frame_type] = cm->base_qindex;

    if (cm->frame_type == KEY_FRAME)
    {
        vp8_adjust_key_frame_context(cpi);
    }

    // Keep a record of ambient average Q.
    if (cm->frame_type == KEY_FRAME)
        cpi->avg_frame_qindex = cm->base_qindex;
    else
        cpi->avg_frame_qindex = (2 + 3 * cpi->avg_frame_qindex + cm->base_qindex) >> 2;

    // Keep a record from which we can calculate the average Q excluding GF updates and key frames
    if ((cm->frame_type != KEY_FRAME) && !cm->refresh_golden_frame && !cm->refresh_alt_ref_frame)
    {
        cpi->ni_frames++;

        // Calculate the average Q for normal inter frames (not key or GFU frames)
        // This is used as a basis for setting active worst quality.
        if (cpi->ni_frames > 150)
        {
            cpi->ni_tot_qi += Q;
            cpi->ni_av_qi = (cpi->ni_tot_qi / cpi->ni_frames);
        }
        // Early in the clip ... average the current frame Q value with the default
        // entered by the user as a dampening measure
        else
        {
            cpi->ni_tot_qi += Q;
            cpi->ni_av_qi = ((cpi->ni_tot_qi / cpi->ni_frames) + cpi->worst_quality + 1) / 2;
        }

        // If the average Q is higher than what was used in the last frame
        // (after going through the recode loop to keep the frame size within range)
        // then use the last frame value - 1.
        // The -1 is designed to stop Q and hence the data rate, from progressively
        // falling away during difficult sections, but at the same time reduce the number of
        // itterations around the recode loop.
        if (Q > cpi->ni_av_qi)
            cpi->ni_av_qi = Q - 1;

    }

#if 0

    // If the frame was massively oversize and we are below optimal buffer level drop next frame
    if ((cpi->drop_frames_allowed) &&
        (cpi->oxcf.end_usage == USAGE_STREAM_FROM_SERVER) &&
        (cpi->buffer_level < cpi->oxcf.drop_frames_water_mark * cpi->oxcf.optimal_buffer_level / 100) &&
        (cpi->projected_frame_size > (4 * cpi->this_frame_target)))
    {
        cpi->drop_frame = TRUE;
    }

#endif

    // Set the count for maximum consequative dropped frames based upon the ratio of
    // this frame size to the target average per frame bandwidth.
    // (cpi->av_per_frame_bandwidth > 0) is just a sanity check to prevent / 0.
    if (cpi->drop_frames_allowed && (cpi->av_per_frame_bandwidth > 0))
    {
        cpi->max_drop_count = cpi->projected_frame_size / cpi->av_per_frame_bandwidth;

        if (cpi->max_drop_count > cpi->max_consec_dropped_frames)
            cpi->max_drop_count = cpi->max_consec_dropped_frames;
    }

    // Update the buffer level variable.
    if (cpi->common.refresh_alt_ref_frame)
        cpi->bits_off_target -= cpi->projected_frame_size;
    else
        cpi->bits_off_target += cpi->av_per_frame_bandwidth - cpi->projected_frame_size;

    // Rolling monitors of whether we are over or underspending used to help regulate min and Max Q in two pass.
    cpi->rolling_target_bits = ((cpi->rolling_target_bits * 3) + cpi->this_frame_target + 2) / 4;
    cpi->rolling_actual_bits = ((cpi->rolling_actual_bits * 3) + cpi->projected_frame_size + 2) / 4;
    cpi->long_rolling_target_bits = ((cpi->long_rolling_target_bits * 31) + cpi->this_frame_target + 16) / 32;
    cpi->long_rolling_actual_bits = ((cpi->long_rolling_actual_bits * 31) + cpi->projected_frame_size + 16) / 32;

    // Actual bits spent
    cpi->total_actual_bits    += cpi->projected_frame_size;

    // Debug stats
    cpi->total_target_vs_actual += (cpi->this_frame_target - cpi->projected_frame_size);

    cpi->buffer_level = cpi->bits_off_target;

    // Update bits left to the kf and gf groups to account for overshoot or undershoot on these frames
    if (cm->frame_type == KEY_FRAME)
    {
        cpi->kf_group_bits += cpi->this_frame_target - cpi->projected_frame_size;

        if (cpi->kf_group_bits < 0)
            cpi->kf_group_bits = 0 ;
    }
    else if (cm->refresh_golden_frame || cm->refresh_alt_ref_frame)
    {
        cpi->gf_group_bits += cpi->this_frame_target - cpi->projected_frame_size;

        if (cpi->gf_group_bits < 0)
            cpi->gf_group_bits = 0 ;
    }

    if (cm->frame_type != KEY_FRAME)
    {
        if (cpi->common.refresh_alt_ref_frame)
        {
            cpi->last_skip_false_probs[2] = cpi->prob_skip_false;
            cpi->last_skip_probs_q[2] = cm->base_qindex;
        }
        else if (cpi->common.refresh_golden_frame)
        {
            cpi->last_skip_false_probs[1] = cpi->prob_skip_false;
            cpi->last_skip_probs_q[1] = cm->base_qindex;
        }
        else
        {
            cpi->last_skip_false_probs[0] = cpi->prob_skip_false;
            cpi->last_skip_probs_q[0] = cm->base_qindex;

            //update the baseline
            cpi->base_skip_false_prob[cm->base_qindex] = cpi->prob_skip_false;

        }
    }

#if 0 && CONFIG_PSNR
    {
        FILE *f = fopen("tmp.stt", "a");

        vp8_clear_system_state();  //__asm emms;

        if (cpi->total_coded_error_left != 0.0)
            fprintf(f, "%10d %10d %10d %10d %10d %10d %10d %10d %6ld %6ld"
                       "%6ld %6ld %5ld %5ld %5ld %8ld %8.2f %10d %10.3f"
                       "%10.3f %8ld\n",
                       cpi->common.current_video_frame, cpi->this_frame_target,
                       cpi->projected_frame_size,
                       (cpi->projected_frame_size - cpi->this_frame_target),
                       (int)cpi->total_target_vs_actual,
                       (cpi->oxcf.starting_buffer_level-cpi->bits_off_target),
                       (int)cpi->total_actual_bits, cm->base_qindex,
                       cpi->active_best_quality, cpi->active_worst_quality,
                       cpi->avg_frame_qindex, cpi->zbin_over_quant,
                       cm->refresh_golden_frame, cm->refresh_alt_ref_frame,
                       cm->frame_type, cpi->gfu_boost,
                       cpi->est_max_qcorrection_factor, (int)cpi->bits_left,
                       cpi->total_coded_error_left,
                       (double)cpi->bits_left / cpi->total_coded_error_left,
                       cpi->tot_recode_hits);
        else
            fprintf(f, "%10d %10d %10d %10d %10d %10d %10d %10d %6ld %6ld"
                       "%6ld %6ld %5ld %5ld %5ld %8ld %8.2f %10d %10.3f"
                       "%8ld\n",
                       cpi->common.current_video_frame,
                       cpi->this_frame_target, cpi->projected_frame_size,
                       (cpi->projected_frame_size - cpi->this_frame_target),
                       (int)cpi->total_target_vs_actual,
                       (cpi->oxcf.starting_buffer_level-cpi->bits_off_target),
                       (int)cpi->total_actual_bits, cm->base_qindex,
                       cpi->active_best_quality, cpi->active_worst_quality,
                       cpi->avg_frame_qindex, cpi->zbin_over_quant,
                       cm->refresh_golden_frame, cm->refresh_alt_ref_frame,
                       cm->frame_type, cpi->gfu_boost,
                       cpi->est_max_qcorrection_factor, (int)cpi->bits_left,
                       cpi->total_coded_error_left, cpi->tot_recode_hits);

        fclose(f);

        {
            FILE *fmodes = fopen("Modes.stt", "a");
            int i;

            fprintf(fmodes, "%6d:%1d:%1d:%1d ",
                        cpi->common.current_video_frame,
                        cm->frame_type, cm->refresh_golden_frame,
                        cm->refresh_alt_ref_frame);

            for (i = 0; i < MAX_MODES; i++)
                fprintf(fmodes, "%5d ", cpi->mode_chosen_counts[i]);

            fprintf(fmodes, "\n");

            fclose(fmodes);
        }
    }

#endif

    // If this was a kf or Gf note the Q
    if ((cm->frame_type == KEY_FRAME) || cm->refresh_golden_frame || cm->refresh_alt_ref_frame)
        cm->last_kf_gf_q = cm->base_qindex;

    if (cm->refresh_golden_frame == 1)
        cm->frame_flags = cm->frame_flags | FRAMEFLAGS_GOLDEN;
    else
        cm->frame_flags = cm->frame_flags&~FRAMEFLAGS_GOLDEN;

    if (cm->refresh_alt_ref_frame == 1)
        cm->frame_flags = cm->frame_flags | FRAMEFLAGS_ALTREF;
    else
        cm->frame_flags = cm->frame_flags&~FRAMEFLAGS_ALTREF;


    if (cm->refresh_last_frame & cm->refresh_golden_frame) // both refreshed
        cpi->gold_is_last = 1;
    else if (cm->refresh_last_frame ^ cm->refresh_golden_frame) // 1 refreshed but not the other
        cpi->gold_is_last = 0;

    if (cm->refresh_last_frame & cm->refresh_alt_ref_frame) // both refreshed
        cpi->alt_is_last = 1;
    else if (cm->refresh_last_frame ^ cm->refresh_alt_ref_frame) // 1 refreshed but not the other
        cpi->alt_is_last = 0;

    if (cm->refresh_alt_ref_frame & cm->refresh_golden_frame) // both refreshed
        cpi->gold_is_alt = 1;
    else if (cm->refresh_alt_ref_frame ^ cm->refresh_golden_frame) // 1 refreshed but not the other
        cpi->gold_is_alt = 0;

    cpi->ref_frame_flags = VP8_ALT_FLAG | VP8_GOLD_FLAG | VP8_LAST_FLAG;

    if (cpi->gold_is_last)
        cpi->ref_frame_flags &= ~VP8_GOLD_FLAG;

    if (cpi->alt_is_last)
        cpi->ref_frame_flags &= ~VP8_ALT_FLAG;

    if (cpi->gold_is_alt)
        cpi->ref_frame_flags &= ~VP8_ALT_FLAG;


    if (cpi->oxcf.error_resilient_mode)
    {
        // Is this an alternate reference update
        if (cpi->common.refresh_alt_ref_frame)
            vp8_yv12_copy_frame_ptr(cm->frame_to_show, &cm->yv12_fb[cm->alt_fb_idx]);

        if (cpi->common.refresh_golden_frame)
            vp8_yv12_copy_frame_ptr(cm->frame_to_show, &cm->yv12_fb[cm->gld_fb_idx]);
    }
    else
    {
        if (cpi->oxcf.play_alternate && cpi->common.refresh_alt_ref_frame)
            // Update the alternate reference frame and stats as appropriate.
            update_alt_ref_frame_and_stats(cpi);
        else
            // Update the Golden frame and golden frame and stats as appropriate.
            update_golden_frame_and_stats(cpi);
    }

    if (cm->frame_type == KEY_FRAME)
    {
        // Tell the caller that the frame was coded as a key frame
        *frame_flags = cm->frame_flags | FRAMEFLAGS_KEY;

        // As this frame is a key frame  the next defaults to an inter frame.
        cm->frame_type = INTER_FRAME;

        cpi->last_frame_percent_intra = 100;
    }
    else
    {
        *frame_flags = cm->frame_flags&~FRAMEFLAGS_KEY;

        cpi->last_frame_percent_intra = cpi->this_frame_percent_intra;
    }

    // Clear the one shot update flags for segmentation map and mode/ref loop filter deltas.
    cpi->mb.e_mbd.update_mb_segmentation_map = 0;
    cpi->mb.e_mbd.update_mb_segmentation_data = 0;
    cpi->mb.e_mbd.mode_ref_lf_delta_update = 0;


    // Dont increment frame counters if this was an altref buffer update not a real frame
    if (cm->show_frame)
    {
        cm->current_video_frame++;
        cpi->frames_since_key++;
    }

    // reset to normal state now that we are done.



#if 0
    {
        char filename[512];
        FILE *recon_file;
        sprintf(filename, "enc%04d.yuv", (int) cm->current_video_frame);
        recon_file = fopen(filename, "wb");
        fwrite(cm->yv12_fb[cm->lst_fb_idx].buffer_alloc,
               cm->yv12_fb[cm->lst_fb_idx].frame_size, 1, recon_file);
        fclose(recon_file);
    }
#endif

    // DEBUG
    //vp8_write_yuv_frame("encoder_recon.yuv", cm->frame_to_show);


}

int vp8_is_gf_update_needed(VP8_PTR ptr)
{
    VP8_COMP *cpi = (VP8_COMP *) ptr;
    int ret_val;

    ret_val = cpi->gf_update_recommended;
    cpi->gf_update_recommended = 0;

    return ret_val;
}

void vp8_check_gf_quality(VP8_COMP *cpi)
{
    VP8_COMMON *cm = &cpi->common;
    int gf_active_pct = (100 * cpi->gf_active_count) / (cm->mb_rows * cm->mb_cols);
    int gf_ref_usage_pct = (cpi->count_mb_ref_frame_usage[GOLDEN_FRAME] * 100) / (cm->mb_rows * cm->mb_cols);
    int last_ref_zz_useage = (cpi->inter_zz_count * 100) / (cm->mb_rows * cm->mb_cols);

    // Gf refresh is not currently being signalled
    if (cpi->gf_update_recommended == 0)
    {
        if (cpi->common.frames_since_golden > 7)
        {
            // Low use of gf
            if ((gf_active_pct < 10) || ((gf_active_pct + gf_ref_usage_pct) < 15))
            {
                // ...but last frame zero zero usage is reasonbable so a new gf might be appropriate
                if (last_ref_zz_useage >= 25)
                {
                    cpi->gf_bad_count ++;

                    if (cpi->gf_bad_count >= 8)   // Check that the condition is stable
                    {
                        cpi->gf_update_recommended = 1;
                        cpi->gf_bad_count = 0;
                    }
                }
                else
                    cpi->gf_bad_count = 0;        // Restart count as the background is not stable enough
            }
            else
                cpi->gf_bad_count = 0;            // Gf useage has picked up so reset count
        }
    }
    // If the signal is set but has not been read should we cancel it.
    else if (last_ref_zz_useage < 15)
    {
        cpi->gf_update_recommended = 0;
        cpi->gf_bad_count = 0;
    }

#if 0
    {
        FILE *f = fopen("gfneeded.stt", "a");
        fprintf(f, "%10d %10d %10d %10d %10ld \n",
                cm->current_video_frame,
                cpi->common.frames_since_golden,
                gf_active_pct, gf_ref_usage_pct,
                cpi->gf_update_recommended);
        fclose(f);
    }

#endif
}

#if !(CONFIG_REALTIME_ONLY)
static void Pass2Encode(VP8_COMP *cpi, unsigned long *size, unsigned char *dest, unsigned int *frame_flags)
{

    if (!cpi->common.refresh_alt_ref_frame)
        vp8_second_pass(cpi);

    encode_frame_to_data_rate(cpi, size, dest, frame_flags);
    cpi->bits_left -= 8 * *size;

    if (!cpi->common.refresh_alt_ref_frame)
    {
        double two_pass_min_rate = (double)(cpi->oxcf.target_bandwidth
            *cpi->oxcf.two_pass_vbrmin_section / 100);
        cpi->bits_left += (long long)(two_pass_min_rate / cpi->oxcf.frame_rate);
    }
}
#endif

//For ARM NEON, d8-d15 are callee-saved registers, and need to be saved by us.
#if HAVE_ARMV7
extern void vp8_push_neon(INT64 *store);
extern void vp8_pop_neon(INT64 *store);
#endif
int vp8_receive_raw_frame(VP8_PTR ptr, unsigned int frame_flags, YV12_BUFFER_CONFIG *sd, INT64 time_stamp, INT64 end_time)
{
    INT64 store_reg[8];
    VP8_COMP *cpi = (VP8_COMP *) ptr;
    VP8_COMMON *cm = &cpi->common;
    struct vpx_usec_timer  timer;

    if (!cpi)
        return -1;

#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
    if (cm->rtcd.flags & HAS_NEON)
#endif
    {
        vp8_push_neon(store_reg);
    }
#endif

    vpx_usec_timer_start(&timer);

    // no more room for frames;
    if (cpi->source_buffer_count != 0 && cpi->source_buffer_count >= cpi->oxcf.lag_in_frames)
    {
#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
        if (cm->rtcd.flags & HAS_NEON)
#endif
        {
            vp8_pop_neon(store_reg);
        }
#endif
        return -1;
    }

    //printf("in-cpi->source_buffer_count: %d\n", cpi->source_buffer_count);

    cm->clr_type = sd->clrtype;

    // make a copy of the frame for use later...
#if !(CONFIG_REALTIME_ONLY)

    if (cpi->oxcf.allow_lag)
    {
        int which_buffer =  cpi->source_encode_index - 1;
        SOURCE_SAMPLE *s;

        if (which_buffer == -1)
            which_buffer = cpi->oxcf.lag_in_frames - 1;

        if (cpi->source_buffer_count < cpi->oxcf.lag_in_frames - 1)
            which_buffer = cpi->source_buffer_count;

        s = &cpi->src_buffer[which_buffer];

        s->source_time_stamp = time_stamp;
        s->source_end_time_stamp = end_time;
        s->source_frame_flags = frame_flags;
        vp8_yv12_copy_frame_ptr(sd, &s->source_buffer);

        cpi->source_buffer_count ++;
    }
    else
#endif
    {
        SOURCE_SAMPLE *s;
        s = &cpi->src_buffer[0];
        s->source_end_time_stamp = end_time;
        s->source_time_stamp = time_stamp;
        s->source_frame_flags = frame_flags;
#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
        if (cm->rtcd.flags & HAS_NEON)
#endif
        {
            vp8_yv12_copy_src_frame_func_neon(sd, &s->source_buffer);
        }
#if CONFIG_RUNTIME_CPU_DETECT
        else
#endif
#endif
#if !HAVE_ARMV7 || CONFIG_RUNTIME_CPU_DETECT
        {
            vp8_yv12_copy_frame_ptr(sd, &s->source_buffer);
        }
#endif
        cpi->source_buffer_count = 1;
    }

    vpx_usec_timer_mark(&timer);
    cpi->time_receive_data += vpx_usec_timer_elapsed(&timer);

#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
    if (cm->rtcd.flags & HAS_NEON)
#endif
    {
        vp8_pop_neon(store_reg);
    }
#endif

    return 0;
}
int vp8_get_compressed_data(VP8_PTR ptr, unsigned int *frame_flags, unsigned long *size, unsigned char *dest, INT64 *time_stamp, INT64 *time_end, int flush)
{
    INT64 store_reg[8];
    VP8_COMP *cpi = (VP8_COMP *) ptr;
    VP8_COMMON *cm = &cpi->common;
    struct vpx_usec_timer  tsctimer;
    struct vpx_usec_timer  ticktimer;
    struct vpx_usec_timer  cmptimer;

    if (!cpi)
        return -1;

#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
    if (cm->rtcd.flags & HAS_NEON)
#endif
    {
        vp8_push_neon(store_reg);
    }
#endif

    vpx_usec_timer_start(&cmptimer);


    // flush variable tells us that even though we have less than 10 frames
    // in our buffer we need to start producing compressed frames.
    // Probably because we are at the end of a file....
    if ((cpi->source_buffer_count == cpi->oxcf.lag_in_frames && cpi->oxcf.lag_in_frames > 0)
        || (!cpi->oxcf.allow_lag && cpi->source_buffer_count > 0)
        || (flush && cpi->source_buffer_count > 0))
    {

        SOURCE_SAMPLE *s;

        s = &cpi->src_buffer[cpi->source_encode_index];
        cpi->source_time_stamp = s->source_time_stamp;
        cpi->source_end_time_stamp = s->source_end_time_stamp;

#if !(CONFIG_REALTIME_ONLY)

        // Should we code an alternate reference frame
        if (cpi->oxcf.error_resilient_mode == 0 &&
            cpi->oxcf.play_alternate &&
            cpi->source_alt_ref_pending  &&
            (cpi->frames_till_gf_update_due < cpi->source_buffer_count) &&
            cpi->oxcf.lag_in_frames != 0)
        {
            cpi->last_alt_ref_sei = (cpi->source_encode_index + cpi->frames_till_gf_update_due) % cpi->oxcf.lag_in_frames;

#if VP8_TEMPORAL_ALT_REF

            if (cpi->oxcf.arnr_max_frames > 0)
            {
#if 0
                // my attempt at a loop that tests the results of strength filter.
                int start_frame = cpi->last_alt_ref_sei - 3;

                int i, besti = -1, pastin = cpi->oxcf.arnr_strength;

                int besterr;

                if (start_frame < 0)
                    start_frame += cpi->oxcf.lag_in_frames;

                besterr = vp8_calc_low_ss_err(&cpi->src_buffer[cpi->last_alt_ref_sei].source_buffer,
                                              &cpi->src_buffer[start_frame].source_buffer, IF_RTCD(&cpi->rtcd.variance));

                for (i = 0; i < 7; i++)
                {
                    int thiserr;
                    cpi->oxcf.arnr_strength = i;
                    vp8cx_temp_filter_c(cpi);

                    thiserr = vp8_calc_low_ss_err(&cpi->alt_ref_buffer.source_buffer,
                                                  &cpi->src_buffer[start_frame].source_buffer, IF_RTCD(&cpi->rtcd.variance));

                    if (10 * thiserr < besterr * 8)
                    {
                        besterr = thiserr;
                        besti = i;
                    }
                }

                if (besti != -1)
                {
                    cpi->oxcf.arnr_strength = besti;
                    vp8cx_temp_filter_c(cpi);
                    s = &cpi->alt_ref_buffer;

                    // FWG not sure if I need to copy this data for the Alt Ref frame
                    s->source_time_stamp = cpi->src_buffer[cpi->last_alt_ref_sei].source_time_stamp;
                    s->source_end_time_stamp = cpi->src_buffer[cpi->last_alt_ref_sei].source_end_time_stamp;
                    s->source_frame_flags = cpi->src_buffer[cpi->last_alt_ref_sei].source_frame_flags;
                }
                else
                    s = &cpi->src_buffer[cpi->last_alt_ref_sei];

#else
                vp8cx_temp_filter_c(cpi);
                s = &cpi->alt_ref_buffer;

                // FWG not sure if I need to copy this data for the Alt Ref frame
                s->source_time_stamp = cpi->src_buffer[cpi->last_alt_ref_sei].source_time_stamp;
                s->source_end_time_stamp = cpi->src_buffer[cpi->last_alt_ref_sei].source_end_time_stamp;
                s->source_frame_flags = cpi->src_buffer[cpi->last_alt_ref_sei].source_frame_flags;

#endif
            }
            else
#endif
                s = &cpi->src_buffer[cpi->last_alt_ref_sei];

            cm->frames_till_alt_ref_frame = cpi->frames_till_gf_update_due;
            cm->refresh_alt_ref_frame = 1;
            cm->refresh_golden_frame = 0;
            cm->refresh_last_frame = 0;
            cm->show_frame = 0;
            cpi->source_alt_ref_pending = FALSE;   // Clear Pending altf Ref flag.
            cpi->is_src_frame_alt_ref = 0;
            cpi->is_next_src_alt_ref = 0;
        }
        else
#endif
        {
            cm->show_frame = 1;
#if !(CONFIG_REALTIME_ONLY)

            if (cpi->oxcf.allow_lag)
            {
                if (cpi->source_encode_index ==  cpi->last_alt_ref_sei)
                {
                    cpi->is_src_frame_alt_ref = 1;
                    cpi->last_alt_ref_sei    = -1;
                }
                else
                    cpi->is_src_frame_alt_ref = 0;

                cpi->source_encode_index = (cpi->source_encode_index + 1) % cpi->oxcf.lag_in_frames;

                if(cpi->source_encode_index == cpi->last_alt_ref_sei)
                    cpi->is_next_src_alt_ref = 1;
                else
                    cpi->is_next_src_alt_ref = 0;
            }

#endif
            cpi->source_buffer_count--;
        }

        cpi->un_scaled_source = &s->source_buffer;
        cpi->Source = &s->source_buffer;
        cpi->source_frame_flags = s->source_frame_flags;

        *time_stamp = cpi->source_time_stamp;
        *time_end = cpi->source_end_time_stamp;
    }
    else
    {
        *size = 0;
#if !(CONFIG_REALTIME_ONLY)

        if (flush && cpi->pass == 1 && !cpi->first_pass_done)
        {
            vp8_end_first_pass(cpi);    /* get last stats packet */
            cpi->first_pass_done = 1;
        }

#endif

#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
        if (cm->rtcd.flags & HAS_NEON)
#endif
        {
            vp8_pop_neon(store_reg);
        }
#endif
        return -1;
    }

    *frame_flags = cpi->source_frame_flags;

#if CONFIG_PSNR

    if (cpi->source_time_stamp < cpi->first_time_stamp_ever)
        cpi->first_time_stamp_ever = cpi->source_time_stamp;

#endif

    // adjust frame rates based on timestamps given
    if (!cm->refresh_alt_ref_frame)
    {
        if (cpi->last_time_stamp_seen == 0)
        {
            double this_fps = 10000000.000 / (cpi->source_end_time_stamp - cpi->source_time_stamp);

            vp8_new_frame_rate(cpi, this_fps);
        }
        else
        {
            long long nanosecs = cpi->source_time_stamp - cpi->last_time_stamp_seen;
            double this_fps = 10000000.000 / nanosecs;

            vp8_new_frame_rate(cpi, (7 * cpi->oxcf.frame_rate + this_fps) / 8);

        }

        cpi->last_time_stamp_seen = cpi->source_time_stamp;
    }

    if (cpi->compressor_speed == 2)
    {
        vp8_check_gf_quality(cpi);
    }

    if (!cpi)
    {
#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
        if (cm->rtcd.flags & HAS_NEON)
#endif
        {
            vp8_pop_neon(store_reg);
        }
#endif
        return 0;
    }

    if (cpi->compressor_speed == 2)
    {
        vpx_usec_timer_start(&tsctimer);
        vpx_usec_timer_start(&ticktimer);
    }

    // start with a 0 size frame
    *size = 0;

    // Clear down mmx registers
    vp8_clear_system_state();  //__asm emms;

    cm->frame_type = INTER_FRAME;
    cm->frame_flags = *frame_flags;

#if 0

    if (cm->refresh_alt_ref_frame)
    {
        //cm->refresh_golden_frame = 1;
        cm->refresh_golden_frame = 0;
        cm->refresh_last_frame = 0;
    }
    else
    {
        cm->refresh_golden_frame = 0;
        cm->refresh_last_frame = 1;
    }

#endif

#if !(CONFIG_REALTIME_ONLY)

    if (cpi->pass == 1)
    {
        Pass1Encode(cpi, size, dest, frame_flags);
    }
    else if (cpi->pass == 2)
    {
        Pass2Encode(cpi, size, dest, frame_flags);
    }
    else
#endif
        encode_frame_to_data_rate(cpi, size, dest, frame_flags);

    if (cpi->compressor_speed == 2)
    {
        unsigned int duration, duration2;
        vpx_usec_timer_mark(&tsctimer);
        vpx_usec_timer_mark(&ticktimer);

        duration = vpx_usec_timer_elapsed(&ticktimer);
        duration2 = (unsigned int)((double)duration / 2);

        if (cm->frame_type != KEY_FRAME)
        {
            if (cpi->avg_encode_time == 0)
                cpi->avg_encode_time = duration;
            else
                cpi->avg_encode_time = (7 * cpi->avg_encode_time + duration) >> 3;
        }

        if (duration2)
        {
            //if(*frame_flags!=1)
            {

                if (cpi->avg_pick_mode_time == 0)
                    cpi->avg_pick_mode_time = duration2;
                else
                    cpi->avg_pick_mode_time = (7 * cpi->avg_pick_mode_time + duration2) >> 3;
            }
        }

    }

    if (cm->refresh_entropy_probs == 0)
    {
        vpx_memcpy(&cm->fc, &cm->lfc, sizeof(cm->fc));
    }

    // if its a dropped frame honor the requests on subsequent frames
    if (*size > 0)
    {

        // return to normal state
        cm->refresh_entropy_probs = 1;
        cm->refresh_alt_ref_frame = 0;
        cm->refresh_golden_frame = 0;
        cm->refresh_last_frame = 1;
        cm->frame_type = INTER_FRAME;

    }

    cpi->ready_for_new_frame = 1;

    vpx_usec_timer_mark(&cmptimer);
    cpi->time_compress_data += vpx_usec_timer_elapsed(&cmptimer);

    if (cpi->b_calculate_psnr && cpi->pass != 1 && cm->show_frame)
        generate_psnr_packet(cpi);

#if CONFIG_PSNR

    if (cpi->pass != 1)
    {
        cpi->bytes += *size;

        if (cm->show_frame)
        {

            cpi->count ++;

            if (cpi->b_calculate_psnr)
            {
                double y, u, v;
                double sq_error;
                double frame_psnr = vp8_calc_psnr(cpi->Source, cm->frame_to_show, &y, &u, &v, &sq_error);

                cpi->total_y += y;
                cpi->total_u += u;
                cpi->total_v += v;
                cpi->total_sq_error += sq_error;
                cpi->total  += frame_psnr;
                {
                    double y2, u2, v2, frame_psnr2, frame_ssim2 = 0;
                    double weight = 0;

                    vp8_deblock(cm->frame_to_show, &cm->post_proc_buffer, cm->filter_level * 10 / 6, 1, 0, IF_RTCD(&cm->rtcd.postproc));
                    vp8_clear_system_state();
                    frame_psnr2 = vp8_calc_psnr(cpi->Source, &cm->post_proc_buffer, &y2, &u2, &v2, &sq_error);
                    frame_ssim2 = vp8_calc_ssim(cpi->Source, &cm->post_proc_buffer, 1, &weight);

                    cpi->summed_quality += frame_ssim2 * weight;
                    cpi->summed_weights += weight;

                    cpi->totalp_y += y2;
                    cpi->totalp_u += u2;
                    cpi->totalp_v += v2;
                    cpi->totalp  += frame_psnr2;
                    cpi->total_sq_error2 += sq_error;

                }
            }

            if (cpi->b_calculate_ssimg)
            {
                double y, u, v, frame_all;
                frame_all =  vp8_calc_ssimg(cpi->Source, cm->frame_to_show, &y, &u, &v);
                cpi->total_ssimg_y += y;
                cpi->total_ssimg_u += u;
                cpi->total_ssimg_v += v;
                cpi->total_ssimg_all += frame_all;
            }

        }
    }

#if 0

    if (cpi->common.frame_type != 0 && cpi->common.base_qindex == cpi->oxcf.worst_allowed_q)
    {
        skiptruecount += cpi->skip_true_count;
        skipfalsecount += cpi->skip_false_count;
    }

#endif
#if 0

    if (cpi->pass != 1)
    {
        FILE *f = fopen("skip.stt", "a");
        fprintf(f, "frame:%4d flags:%4x Q:%4d P:%4d Size:%5d\n", cpi->common.current_video_frame, *frame_flags, cpi->common.base_qindex, cpi->prob_skip_false, *size);

        if (cpi->is_src_frame_alt_ref == 1)
            fprintf(f, "skipcount: %4d framesize: %d\n", cpi->skip_true_count , *size);

        fclose(f);
    }

#endif
#endif

#if HAVE_ARMV7
#if CONFIG_RUNTIME_CPU_DETECT
    if (cm->rtcd.flags & HAS_NEON)
#endif
    {
        vp8_pop_neon(store_reg);
    }
#endif

    return 0;
}

int vp8_get_preview_raw_frame(VP8_PTR comp, YV12_BUFFER_CONFIG *dest, int deblock_level, int noise_level, int flags)
{
    VP8_COMP *cpi = (VP8_COMP *) comp;

    if (cpi->common.refresh_alt_ref_frame)
        return -1;
    else
    {
        int ret;
#if CONFIG_POSTPROC
        ret = vp8_post_proc_frame(&cpi->common, dest, deblock_level, noise_level, flags);
#else

        if (cpi->common.frame_to_show)
        {
            *dest = *cpi->common.frame_to_show;
            dest->y_width = cpi->common.Width;
            dest->y_height = cpi->common.Height;
            dest->uv_height = cpi->common.Height / 2;
            ret = 0;
        }
        else
        {
            ret = -1;
        }

#endif //!CONFIG_POSTPROC
        vp8_clear_system_state();
        return ret;
    }
}

int vp8_set_roimap(VP8_PTR comp, unsigned char *map, unsigned int rows, unsigned int cols, int delta_q[4], int delta_lf[4], unsigned int threshold[4])
{
    VP8_COMP *cpi = (VP8_COMP *) comp;
    signed char feature_data[MB_LVL_MAX][MAX_MB_SEGMENTS];

    if (cpi->common.mb_rows != rows || cpi->common.mb_cols != cols)
        return -1;

    if (!map)
    {
        disable_segmentation((VP8_PTR)cpi);
        return 0;
    }

    // Set the segmentation Map
    set_segmentation_map((VP8_PTR)cpi, map);

    // Activate segmentation.
    enable_segmentation((VP8_PTR)cpi);

    // Set up the quant segment data
    feature_data[MB_LVL_ALT_Q][0] = delta_q[0];
    feature_data[MB_LVL_ALT_Q][1] = delta_q[1];
    feature_data[MB_LVL_ALT_Q][2] = delta_q[2];
    feature_data[MB_LVL_ALT_Q][3] = delta_q[3];

    // Set up the loop segment data s
    feature_data[MB_LVL_ALT_LF][0] = delta_lf[0];
    feature_data[MB_LVL_ALT_LF][1] = delta_lf[1];
    feature_data[MB_LVL_ALT_LF][2] = delta_lf[2];
    feature_data[MB_LVL_ALT_LF][3] = delta_lf[3];

    cpi->segment_encode_breakout[0] = threshold[0];
    cpi->segment_encode_breakout[1] = threshold[1];
    cpi->segment_encode_breakout[2] = threshold[2];
    cpi->segment_encode_breakout[3] = threshold[3];

    // Initialise the feature data structure
    // SEGMENT_DELTADATA    0, SEGMENT_ABSDATA      1
    set_segment_data((VP8_PTR)cpi, &feature_data[0][0], SEGMENT_DELTADATA);

    return 0;
}

int vp8_set_active_map(VP8_PTR comp, unsigned char *map, unsigned int rows, unsigned int cols)
{
    VP8_COMP *cpi = (VP8_COMP *) comp;

    if (rows == cpi->common.mb_rows && cols == cpi->common.mb_cols)
    {
        if (map)
        {
            vpx_memcpy(cpi->active_map, map, rows * cols);
            cpi->active_map_enabled = 1;
        }
        else
            cpi->active_map_enabled = 0;

        return 0;
    }
    else
    {
        //cpi->active_map_enabled = 0;
        return -1 ;
    }
}

int vp8_set_internal_size(VP8_PTR comp, VPX_SCALING horiz_mode, VPX_SCALING vert_mode)
{
    VP8_COMP *cpi = (VP8_COMP *) comp;

    if (horiz_mode >= NORMAL && horiz_mode <= ONETWO)
        cpi->common.horiz_scale = horiz_mode;
    else
        return -1;

    if (vert_mode >= NORMAL && vert_mode <= ONETWO)
        cpi->common.vert_scale  = vert_mode;
    else
        return -1;

    return 0;
}



int vp8_calc_ss_err(YV12_BUFFER_CONFIG *source, YV12_BUFFER_CONFIG *dest, const vp8_variance_rtcd_vtable_t *rtcd)
{
    int i, j;
    int Total = 0;

    unsigned char *src = source->y_buffer;
    unsigned char *dst = dest->y_buffer;
    (void)rtcd;

    // Loop through the Y plane raw and reconstruction data summing (square differences)
    for (i = 0; i < source->y_height; i += 16)
    {
        for (j = 0; j < source->y_width; j += 16)
        {
            unsigned int sse;
            Total += VARIANCE_INVOKE(rtcd, mse16x16)(src + j, source->y_stride, dst + j, dest->y_stride, &sse);
        }

        src += 16 * source->y_stride;
        dst += 16 * dest->y_stride;
    }

    return Total;
}
int vp8_calc_low_ss_err(YV12_BUFFER_CONFIG *source, YV12_BUFFER_CONFIG *dest, const vp8_variance_rtcd_vtable_t *rtcd)
{
    int i, j;
    int Total = 0;

    unsigned char *src = source->y_buffer;
    unsigned char *dst = dest->y_buffer;
    (void)rtcd;

    // Loop through the Y plane raw and reconstruction data summing (square differences)
    for (i = 0; i < source->y_height; i += 16)
    {
        for (j = 0; j < source->y_width; j += 16)
        {
            unsigned int sse;
            VARIANCE_INVOKE(rtcd, mse16x16)(src + j, source->y_stride, dst + j, dest->y_stride, &sse);

            if (sse < 8096)
                Total += sse;
        }

        src += 16 * source->y_stride;
        dst += 16 * dest->y_stride;
    }

    return Total;
}

int vp8_get_speed(VP8_PTR c)
{
    VP8_COMP   *cpi = (VP8_COMP *) c;
    return cpi->Speed;
}
int vp8_get_quantizer(VP8_PTR c)
{
    VP8_COMP   *cpi = (VP8_COMP *) c;
    return cpi->common.base_qindex;
}
