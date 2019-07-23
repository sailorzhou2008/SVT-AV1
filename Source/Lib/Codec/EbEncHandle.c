/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

/*------------------------------------------------------------------
* strncpy_s.c / strcpy_s.c / strnlen_s.c
*
* October 2008, Bo Berry
*
* Copyright � 2008-2011 by Cisco Systems, Inc
* All rights reserved.

* safe_str_constraint.c
*
* October 2008, Bo Berry
* 2012, Jonathan Toppins <jtoppins@users.sourceforge.net>
*
* Copyright � 2008, 2009, 2012 Cisco Systems
* All rights reserved.

* ignore_handler_s.c
*
* 2012, Jonathan Toppins <jtoppins@users.sourceforge.net>
*
* Copyright � 2012 Cisco Systems
* All rights reserved.
*
* Permission is hereby granted, free of charge, to any person
* obtaining a copy of this software and associated documentation
* files (the "Software"), to deal in the Software without
* restriction, including without limitation the rights to use,
* copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the
* Software is furnished to do so, subject to the following
* conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
* OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
* HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
* OTHER DEALINGS IN THE SOFTWARE.
*------------------------------------------------------------------
*/

// SUMMARY
//   Contains the API component functions

/**************************************
 * Includes
 **************************************/
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <immintrin.h>

#include "EbDefinitions.h"
#include "EbSvtAv1Enc.h"
#include "EbThreads.h"
#include "EbUtility.h"
#include "EbEncHandle.h"
#include "EbSystemResourceManager.h"
#include "EbPictureControlSet.h"
#include "EbPictureOperators.h"
#include "EbSequenceControlSet.h"
#include "EbPictureBufferDesc.h"
#include "EbReferenceObject.h"
#include "EbResourceCoordinationProcess.h"
#include "EbPictureAnalysisProcess.h"
#include "EbPictureDecisionProcess.h"
#include "EbMotionEstimationProcess.h"
#include "EbInitialRateControlProcess.h"
#include "EbSourceBasedOperationsProcess.h"
#include "EbPictureManagerProcess.h"
#include "EbRateControlProcess.h"
#include "EbModeDecisionConfigurationProcess.h"
#include "EbEncDecProcess.h"
#include "EbEntropyCodingProcess.h"
#include "EbPacketizationProcess.h"
#include "EbResourceCoordinationResults.h"
#include "EbPictureAnalysisResults.h"
#include "EbPictureDecisionResults.h"
#include "EbMotionEstimationResults.h"
#include "EbInitialRateControlResults.h"
#include "EbPictureDemuxResults.h"
#include "EbRateControlTasks.h"
#include "EbRateControlResults.h"
#include "EbEncDecTasks.h"
#include "EbEncDecResults.h"
#include "EbEntropyCodingResults.h"
#include "EbPredictionStructure.h"
#if FILT_PROC
#include "EbDlfProcess.h"
#include "EbCdefProcess.h"
#include "EbRestProcess.h"
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif


#define RTCD_C
#include "aom_dsp_rtcd.h"

 /**************************************
  * Defines
  **************************************/
#define EB_EncodeInstancesTotalCount                    1
#define EB_ComputeSegmentInitCount                      1

  // Config Set Initial Count
#define EB_SequenceControlSetPoolInitCount              3

// Process Instantiation Initial Counts
#define EB_ResourceCoordinationProcessInitCount         1
#define EB_PictureDecisionProcessInitCount              1
#define EB_InitialRateControlProcessInitCount           1
#define EB_PictureManagerProcessInitCount               1
#define EB_RateControlProcessInitCount                  1
#define EB_PacketizationProcessInitCount                1

// Output Buffer Transfer Parameters
#define EB_OUTPUTSTREAMBUFFERSIZE                                       0x2DC6C0   //0x7D00        // match MTU Size
#define EB_OUTPUTRECONBUFFERSIZE                                        (MAX_PICTURE_WIDTH_SIZE*MAX_PICTURE_HEIGHT_SIZE*2)   // Recon Slice Size
#define EB_OUTPUTSTATISTICSBUFFERSIZE                                   0x30            // 6X8 (8 Bytes for Y, U, V, number of bits, picture number, QP)
#define EOS_NAL_BUFFER_SIZE                                             0x0010 // Bitstream used to code EOS NAL
#define EB_OUTPUTSTREAMBUFFERSIZE_MACRO(ResolutionSize)                ((ResolutionSize) < (INPUT_SIZE_1080i_TH) ? 0x1E8480 : (ResolutionSize) < (INPUT_SIZE_1080p_TH) ? 0x2DC6C0 : (ResolutionSize) < (INPUT_SIZE_4K_TH) ? 0x2DC6C0 : 0x2DC6C0  )   

#define ENCDEC_INPUT_PORT_MDC                                0
#define ENCDEC_INPUT_PORT_ENCDEC                             1
#define ENCDEC_INPUT_PORT_INVALID                           -1

#define SCD_LAD                                              6

/**************************************
 * Globals
 **************************************/

EbMemoryMapEntry                 *memory_map;
uint32_t                         *memory_map_index;
uint64_t                         *total_lib_memory;

uint32_t                         libMallocCount = 0;
uint32_t                         lib_thread_count = 0;
uint32_t                         libSemaphoreCount = 0;
uint32_t                         libMutexCount = 0;

uint8_t                          num_groups = 0;
#ifdef _WIN32
GROUP_AFFINITY                   group_affinity;
EbBool                           alternate_groups = 0;
#elif defined(__linux__)
cpu_set_t                        group_affinity;
typedef struct logicalProcessorGroup {
    uint32_t num;
    uint32_t group[1024];
}processorGroup;
#define MAX_PROCESSOR_GROUP 16
processorGroup                   lp_group[MAX_PROCESSOR_GROUP];
#endif

/**************************************
* Instruction Set Support
**************************************/

#if defined(_MSC_VER)
# include <intrin.h>
#endif
// Helper Functions 
void RunCpuid(uint32_t eax, uint32_t ecx, int32_t* abcd)
{
#if defined(_MSC_VER)
    __cpuidex(abcd, eax, ecx);
#else
    uint32_t ebx = 0, edx = 0;
# if defined( __i386__ ) && defined ( __PIC__ )
    /* in case of PIC under 32-bit EBX cannot be clobbered */
    __asm__("movl %%ebx, %%edi \n\t cpuid \n\t xchgl %%ebx, %%edi" : "=D" (ebx),
# else
    __asm__("cpuid" : "+b" (ebx),
# endif
        "+a" (eax), "+c" (ecx), "=d" (edx));
    abcd[0] = eax; abcd[1] = ebx; abcd[2] = ecx; abcd[3] = edx;
#endif
}
int32_t CheckXcr0Ymm()
{
    uint32_t xcr0;
#if defined(_MSC_VER)
    xcr0 = (uint32_t)_xgetbv(0);  /* min VS2010 SP1 compiler is required */
#else
    __asm__("xgetbv" : "=a" (xcr0) : "c" (0) : "%edx");
#endif
    return ((xcr0 & 6) == 6); /* checking if xmm and ymm state are enabled in XCR0 */
}
int32_t Check4thGenIntelCoreFeatures()
{
    int32_t abcd[4];
    int32_t fma_movbe_osxsave_mask = ((1 << 12) | (1 << 22) | (1 << 27));
    int32_t avx2_bmi12_mask = (1 << 5) | (1 << 3) | (1 << 8);

    /* CPUID.(EAX=01H, ECX=0H):ECX.FMA[bit 12]==1   &&
       CPUID.(EAX=01H, ECX=0H):ECX.MOVBE[bit 22]==1 &&
       CPUID.(EAX=01H, ECX=0H):ECX.OSXSAVE[bit 27]==1 */
    RunCpuid(1, 0, abcd);
    if ((abcd[2] & fma_movbe_osxsave_mask) != fma_movbe_osxsave_mask)
        return 0;

    if (!CheckXcr0Ymm())
        return 0;

    /*  CPUID.(EAX=07H, ECX=0H):EBX.AVX2[bit 5]==1  &&
        CPUID.(EAX=07H, ECX=0H):EBX.BMI1[bit 3]==1  &&
        CPUID.(EAX=07H, ECX=0H):EBX.BMI2[bit 8]==1  */
    RunCpuid(7, 0, abcd);
    if ((abcd[1] & avx2_bmi12_mask) != avx2_bmi12_mask)
        return 0;
    /* CPUID.(EAX=80000001H):ECX.LZCNT[bit 5]==1 */
    RunCpuid(0x80000001, 0, abcd);
    if ((abcd[2] & (1 << 5)) == 0)
        return 0;
    return 1;
}
static int32_t CanUseIntelCore4thGenFeatures()
{
    static int32_t the_4th_gen_features_available = -1;
    /* test is performed once */
    if (the_4th_gen_features_available < 0)
        the_4th_gen_features_available = Check4thGenIntelCoreFeatures();
    return the_4th_gen_features_available;
}
EbAsm GetCpuAsmType()
{
    EbAsm asm_type = ASM_NON_AVX2;

    if (CanUseIntelCore4thGenFeatures() == 1)
        asm_type = ASM_AVX2;
    else
        // Need to change to support lower CPU Technologies
        asm_type = ASM_NON_AVX2;
    return asm_type;
}

//Get Number of logical processors
uint32_t GetNumProcessors() {
#ifdef _WIN32
    SYSTEM_INFO sysinfo;
    GetSystemInfo(&sysinfo);
    return num_groups == 1 ? sysinfo.dwNumberOfProcessors : sysinfo.dwNumberOfProcessors << 1;
#else
    return sysconf(_SC_NPROCESSORS_ONLN);
#endif
}

EbErrorType InitThreadManagmentParams() {
#ifdef _WIN32
    // Initialize group_affinity structure with Current thread info
    GetThreadGroupAffinity(GetCurrentThread(), &group_affinity);
    num_groups = (uint8_t)GetActiveProcessorGroupCount();
#elif defined(__linux__)
    const char* PROCESSORID = "processor";
    const char* PHYSICALID = "physical id";
    int processor_id_len = strnlen_ss(PROCESSORID, 128);
    int physical_id_len = strnlen_ss(PHYSICALID, 128);
    if (processor_id_len < 0 || processor_id_len >= 128)
        return EB_ErrorInsufficientResources;
    if (physical_id_len < 0 || physical_id_len >= 128)
        return EB_ErrorInsufficientResources;
    memset(lp_group, 0, sizeof(lp_group));

    int fd = open("/proc/cpuinfo", O_RDONLY | O_NOFOLLOW, "rt");
    struct stat file_stat;
    if (fd >= 0) {
        if (fstat(fd, &file_stat) != -1 && S_ISREG(file_stat.st_mode) != 0) {
            int processor_id = 0, socket_id = 0;
            char line[128];
            int bytes = 1;
            while (bytes > 0) {
                bytes = read(fd, line, 128);
                if (bytes > 0) {
                    if (strncmp(line, PROCESSORID, processor_id_len) == 0) {
                        char* p = line + processor_id_len;
                        while (*p < '0' || *p > '9') p++;
                        processor_id = strtol(p, NULL, 0);
                    }
                    if (strncmp(line, PHYSICALID, physical_id_len) == 0) {
                        char* p = line + physical_id_len;
                        while (*p < '0' || *p > '9') p++;
                        socket_id = strtol(p, NULL, 0);
                        if (socket_id < 0 || socket_id > 15) {
                            close(fd);
                            return EB_ErrorInsufficientResources;
                        }
                        if (socket_id + 1 > num_groups)
                            num_groups = socket_id + 1;
                        lp_group[socket_id].group[lp_group[socket_id].num++] = processor_id;
                    }
                    lseek(fd, -bytes + 1, SEEK_CUR);
                    while (line[0] != '\n' && bytes > 0) bytes = read(fd, line, 1);
                }
            }
        }
        close(fd);
    }
#endif
    return EB_ErrorNone;
}

#ifdef _WIN32
uint64_t GetAffinityMask(uint32_t lpnum) {
    uint64_t mask = 0x1;
    for (uint32_t i = lpnum - 1; i > 0; i--)
        mask += (uint64_t)1 << i;
    return mask;
}
#endif

void EbSetThreadManagementParameters(EbSvtAv1EncConfiguration   *config_ptr) {
    uint32_t num_logical_processors = GetNumProcessors();
#ifdef _WIN32
    // For system with a single processor group(no more than 64 logic processors all together)
    // Affinity of the thread can be set to one or more logical processors
    if (num_groups == 1) {
        uint32_t lps = config_ptr->logical_processors == 0 ? num_logical_processors :
            config_ptr->logical_processors < num_logical_processors ? config_ptr->logical_processors : num_logical_processors;
        group_affinity.Mask = GetAffinityMask(lps);
    }
    else if (num_groups > 1) { // For system with multiple processor group
        if (config_ptr->logical_processors == 0) {
            if (config_ptr->target_socket != -1) {
                group_affinity.Group = config_ptr->target_socket;
            }
        }
        else {
            uint32_t num_lp_per_group = num_logical_processors / num_groups;
            if (config_ptr->target_socket == -1) {
                if (config_ptr->logical_processors > num_lp_per_group) {
                    alternate_groups = TRUE;
                    SVT_LOG("SVT [WARNING]: -lp(logical processors) setting is ignored. Run on both sockets. \n");
                }
                else {
                    group_affinity.Mask = GetAffinityMask(config_ptr->logical_processors);
                }
            }
            else {
                uint32_t lps = config_ptr->logical_processors == 0 ? num_lp_per_group :
                    config_ptr->logical_processors < num_lp_per_group ? config_ptr->logical_processors : num_lp_per_group;
                group_affinity.Mask = GetAffinityMask(lps);
                group_affinity.Group = config_ptr->target_socket;
            }
        }
    }
#elif defined(__linux__)
    CPU_ZERO(&group_affinity);

    if (num_groups == 1) {
        uint32_t lps = config_ptr->logical_processors == 0 ? num_logical_processors :
            config_ptr->logical_processors < num_logical_processors ? config_ptr->logical_processors : num_logical_processors;
        for (uint32_t i = 0; i < lps; i++)
            CPU_SET(lp_group[0].group[i], &group_affinity);
    }
    else if (num_groups > 1) {
        uint32_t num_lp_per_group = num_logical_processors / num_groups;
        if (config_ptr->logical_processors == 0) {
            if (config_ptr->target_socket != -1) {
                for (uint32_t i = 0; i < lp_group[config_ptr->target_socket].num; i++)
                    CPU_SET(lp_group[config_ptr->target_socket].group[i], &group_affinity);
            }
        }
        else {
            if (config_ptr->target_socket == -1) {
                uint32_t lps = config_ptr->logical_processors == 0 ? num_logical_processors :
                    config_ptr->logical_processors < num_logical_processors ? config_ptr->logical_processors : num_logical_processors;
                if (lps > num_lp_per_group) {
                    for (uint32_t i = 0; i < lp_group[0].num; i++)
                        CPU_SET(lp_group[0].group[i], &group_affinity);
                    for (uint32_t i = 0; i < (lps - lp_group[0].num); i++)
                        CPU_SET(lp_group[1].group[i], &group_affinity);
                }
                else {
                    for (uint32_t i = 0; i < lps; i++)
                        CPU_SET(lp_group[0].group[i], &group_affinity);
                }
            }
            else {
                uint32_t lps = config_ptr->logical_processors == 0 ? num_lp_per_group :
                    config_ptr->logical_processors < num_lp_per_group ? config_ptr->logical_processors : num_lp_per_group;
                for (uint32_t i = 0; i < lps; i++)
                    CPU_SET(lp_group[config_ptr->target_socket].group[i], &group_affinity);
            }
        }
    }
#endif
}
void asmSetConvolveAsmTable(void);
void asmSetConvolveHbdAsmTable(void);
void init_intra_dc_predictors_c_internal(void);
void init_intra_predictors_internal(void);
void SwitchToRealTime(){
#if defined(__linux__) || defined(__APPLE__)

    struct sched_param schedParam = {
        .sched_priority = 99
    };

    int32_t retValue = pthread_setschedparam(pthread_self(), SCHED_FIFO, &schedParam);
    UNUSED(retValue);
#endif
}
int32_t set_parent_pcs(EbSvtAv1EncConfiguration*   config) {

    if (config){
        uint32_t fps            = (uint32_t)((config->frame_rate > 1000) ? 
                        config->frame_rate >> 16 : 
                        config->frame_rate);
        uint32_t ppcs_count     = fps;
        uint32_t min_ppcs_count = (2 << config->hierarchical_levels) + 1; // min picture count to start encoding

        fps        = fps > 120 ? 120   : fps;
        fps        = fps < 24  ? 24    : fps; 
        ppcs_count = MAX(min_ppcs_count, fps);
        ppcs_count = ((ppcs_count * 5) >> 2);  // 1.25 sec worth of internal buffering
    
        return (int32_t) ppcs_count;
    }
    else{
        SVT_LOG("SVT[error]: Configuration struct is corrupted\n");
        return -1;
    }
}
EbErrorType LoadDefaultBufferConfigurationSettings(
    SequenceControlSet_t       *sequence_control_set_ptr){

    EbErrorType           return_error = EB_ErrorNone;
    uint32_t encDecSegH = (sequence_control_set_ptr->static_config.super_block_size == 128) ?
        ((sequence_control_set_ptr->max_input_luma_height + 64) / 128) :
        ((sequence_control_set_ptr->max_input_luma_height + 32) / 64);
    uint32_t encDecSegW = (sequence_control_set_ptr->static_config.super_block_size == 128) ?
        ((sequence_control_set_ptr->max_input_luma_width + 64) / 128) :
        ((sequence_control_set_ptr->max_input_luma_width + 32) / 64);

    uint32_t meSegH     = (((sequence_control_set_ptr->max_input_luma_height + 32) / BLOCK_SIZE_64) < 6) ? 1 : 6;
    uint32_t meSegW     = (((sequence_control_set_ptr->max_input_luma_width + 32) / BLOCK_SIZE_64) < 10) ? 1 : 10;
    int32_t return_ppcs = set_parent_pcs(&sequence_control_set_ptr->static_config);
    if (return_ppcs == -1)
        return EB_ErrorInsufficientResources;
    uint32_t inputPic = (uint32_t)return_ppcs;

    unsigned int lpCount = GetNumProcessors();
    unsigned int coreCount = lpCount;
#if defined(_WIN32) || defined(__linux__)
    if (sequence_control_set_ptr->static_config.target_socket != -1)
        coreCount /= num_groups;
    if (sequence_control_set_ptr->static_config.logical_processors != 0)
        coreCount = sequence_control_set_ptr->static_config.logical_processors < coreCount ?
            sequence_control_set_ptr->static_config.logical_processors: coreCount;
#endif

#ifdef _WIN32
    //Handle special case on Windows
    //By default, on Windows an application is constrained to a single group
    if (sequence_control_set_ptr->static_config.target_socket == -1 &&
        sequence_control_set_ptr->static_config.logical_processors == 0)
        coreCount /= num_groups;

    //Affininty can only be set by group on Windows.
    //Run on both sockets if -lp is larger than logical processor per group.
    if (sequence_control_set_ptr->static_config.target_socket == -1 &&
        sequence_control_set_ptr->static_config.logical_processors > lpCount / num_groups)
        coreCount = lpCount;
#endif

    sequence_control_set_ptr->input_buffer_fifo_init_count         = 
        inputPic + SCD_LAD + sequence_control_set_ptr->static_config.look_ahead_distance ;
    sequence_control_set_ptr->output_stream_buffer_fifo_init_count = 
        sequence_control_set_ptr->input_buffer_fifo_init_count + 4;

    // ME segments
    sequence_control_set_ptr->me_segment_row_count_array[0] = meSegH;
    sequence_control_set_ptr->me_segment_row_count_array[1] = meSegH;
    sequence_control_set_ptr->me_segment_row_count_array[2] = meSegH;
    sequence_control_set_ptr->me_segment_row_count_array[3] = meSegH;
    sequence_control_set_ptr->me_segment_row_count_array[4] = meSegH;
    sequence_control_set_ptr->me_segment_row_count_array[5] = meSegH;

    sequence_control_set_ptr->me_segment_column_count_array[0] = meSegW;
    sequence_control_set_ptr->me_segment_column_count_array[1] = meSegW;
    sequence_control_set_ptr->me_segment_column_count_array[2] = meSegW;
    sequence_control_set_ptr->me_segment_column_count_array[3] = meSegW;
    sequence_control_set_ptr->me_segment_column_count_array[4] = meSegW;
    sequence_control_set_ptr->me_segment_column_count_array[5] = meSegW;

    // EncDec segments     
    sequence_control_set_ptr->enc_dec_segment_row_count_array[0] = encDecSegH;
    sequence_control_set_ptr->enc_dec_segment_row_count_array[1] = encDecSegH;
    sequence_control_set_ptr->enc_dec_segment_row_count_array[2] = encDecSegH;
    sequence_control_set_ptr->enc_dec_segment_row_count_array[3] = encDecSegH;
    sequence_control_set_ptr->enc_dec_segment_row_count_array[4] = encDecSegH;
    sequence_control_set_ptr->enc_dec_segment_row_count_array[5] = encDecSegH;

    sequence_control_set_ptr->enc_dec_segment_col_count_array[0] = encDecSegW;
    sequence_control_set_ptr->enc_dec_segment_col_count_array[1] = encDecSegW;
    sequence_control_set_ptr->enc_dec_segment_col_count_array[2] = encDecSegW;
    sequence_control_set_ptr->enc_dec_segment_col_count_array[3] = encDecSegW;
    sequence_control_set_ptr->enc_dec_segment_col_count_array[4] = encDecSegW;
    sequence_control_set_ptr->enc_dec_segment_col_count_array[5] = encDecSegW;

#if CDEF_M
    sequence_control_set_ptr->cdef_segment_column_count = meSegW;
    sequence_control_set_ptr->cdef_segment_row_count    = meSegH;
#endif
#if REST_M
    //since restoration unit size is same for Luma and Chroma, Luma segments and chroma segments do not correspond to the same area!
    //to keep proper processing, segments have to be configured based on chroma resolution.
    uint32_t unit_size                                  = 256;
    uint32_t rest_seg_w                                 = MAX((sequence_control_set_ptr->max_input_luma_width /2 + (unit_size >> 1)) / unit_size, 1);
    uint32_t rest_seg_h                                 = MAX((sequence_control_set_ptr->max_input_luma_height/2 + (unit_size >> 1)) / unit_size, 1);
    sequence_control_set_ptr->rest_segment_column_count = MIN(rest_seg_w,6);
    sequence_control_set_ptr->rest_segment_row_count    = MIN(rest_seg_h,4);
#endif
    //#====================== Data Structures and Picture Buffers ======================
    sequence_control_set_ptr->picture_control_set_pool_init_count       = inputPic;
    sequence_control_set_ptr->picture_control_set_pool_init_count_child = MAX(MAX(MIN(3, coreCount/2), coreCount / 6), 1);
    sequence_control_set_ptr->reference_picture_buffer_init_count       = MAX((uint32_t)(inputPic >> 1),
                                                                          (uint32_t)((1 << sequence_control_set_ptr->static_config.hierarchical_levels) + 2)) +
                                                                          sequence_control_set_ptr->static_config.look_ahead_distance + SCD_LAD;
    sequence_control_set_ptr->pa_reference_picture_buffer_init_count    = MAX((uint32_t)(inputPic >> 1),
                                                                          (uint32_t)((1 << sequence_control_set_ptr->static_config.hierarchical_levels) + 2)) +
                                                                          sequence_control_set_ptr->static_config.look_ahead_distance + SCD_LAD;
    sequence_control_set_ptr->output_recon_buffer_fifo_init_count       = sequence_control_set_ptr->reference_picture_buffer_init_count;

    //#====================== Inter process Fifos ======================
    sequence_control_set_ptr->resource_coordination_fifo_init_count       = 300;
    sequence_control_set_ptr->picture_analysis_fifo_init_count            = 300;
    sequence_control_set_ptr->picture_decision_fifo_init_count            = 300;
    sequence_control_set_ptr->initial_rate_control_fifo_init_count        = 300;
    sequence_control_set_ptr->picture_demux_fifo_init_count               = 300;
    sequence_control_set_ptr->rate_control_tasks_fifo_init_count          = 300;
    sequence_control_set_ptr->rate_control_fifo_init_count                = 301;
    sequence_control_set_ptr->mode_decision_configuration_fifo_init_count = 300;
    sequence_control_set_ptr->motion_estimation_fifo_init_count           = 300;
    sequence_control_set_ptr->entropy_coding_fifo_init_count              = 300;
    sequence_control_set_ptr->enc_dec_fifo_init_count                     = 300;
#if FILT_PROC    
    sequence_control_set_ptr->dlf_fifo_init_count                         = 300;
    sequence_control_set_ptr->cdef_fifo_init_count                        = 300;
    sequence_control_set_ptr->rest_fifo_init_count                        = 300;
#endif
    //#====================== Processes number ======================
    sequence_control_set_ptr->total_process_init_count                    = 0;

#if ONE_SEG
    sequence_control_set_ptr->total_process_init_count += sequence_control_set_ptr->picture_analysis_process_init_count = 1;//MAX(15, coreCount / 6);
    sequence_control_set_ptr->total_process_init_count += sequence_control_set_ptr->motion_estimation_process_init_count = 1;//MAX(20, coreCount / 3);
    sequence_control_set_ptr->total_process_init_count += sequence_control_set_ptr->source_based_operations_process_init_count = 1;//MAX(3, coreCount / 12);
    sequence_control_set_ptr->total_process_init_count += sequence_control_set_ptr->mode_decision_configuration_process_init_count = 1;//MAX(3, coreCount / 12);
    sequence_control_set_ptr->total_process_init_count += sequence_control_set_ptr->enc_dec_process_init_count = 1;//MAX(40, coreCount);
    sequence_control_set_ptr->total_process_init_count += sequence_control_set_ptr->entropy_coding_process_init_count = 1;//MAX(3, coreCount / 12);
#else
    sequence_control_set_ptr->total_process_init_count += (sequence_control_set_ptr->picture_analysis_process_init_count             = MAX(MIN(15, coreCount), coreCount / 6));
    sequence_control_set_ptr->total_process_init_count += (sequence_control_set_ptr->motion_estimation_process_init_count            = MAX(MIN(20, coreCount), coreCount / 3));
    sequence_control_set_ptr->total_process_init_count += (sequence_control_set_ptr->source_based_operations_process_init_count      = MAX(MIN(3, coreCount), coreCount / 12));
    sequence_control_set_ptr->total_process_init_count += (sequence_control_set_ptr->mode_decision_configuration_process_init_count  = MAX(MIN(3, coreCount), coreCount / 12));
    sequence_control_set_ptr->total_process_init_count += (sequence_control_set_ptr->enc_dec_process_init_count                      = MAX(MIN(40, coreCount), coreCount)    );
    sequence_control_set_ptr->total_process_init_count += (sequence_control_set_ptr->entropy_coding_process_init_count               = MAX(MIN(3, coreCount), coreCount / 12));
#endif

#if FILT_PROC
    sequence_control_set_ptr->total_process_init_count +=(sequence_control_set_ptr->dlf_process_init_count                           = MAX(MIN(40, coreCount), coreCount));
    sequence_control_set_ptr->total_process_init_count +=(sequence_control_set_ptr->cdef_process_init_count                          = MAX(MIN(40, coreCount), coreCount));
    sequence_control_set_ptr->total_process_init_count +=(sequence_control_set_ptr->rest_process_init_count                          = MAX(MIN(40, coreCount), coreCount));
#endif

    sequence_control_set_ptr->total_process_init_count += 6; // single processes count
    printf("Number of logical cores available: %u\nNumber of PPCS %u\n", coreCount, inputPic);

    return return_error;

}
 // Rate Control
static RateControlPorts_t rateControlPorts[] = {
    {RATE_CONTROL_INPUT_PORT_PICTURE_MANAGER,       0},
    {RATE_CONTROL_INPUT_PORT_PACKETIZATION,         0},
    {RATE_CONTROL_INPUT_PORT_ENTROPY_CODING,        0},
    {RATE_CONTROL_INPUT_PORT_INVALID,               0}
};
// Rate Control
static uint32_t RateControlPortLookup(
    RATE_CONTROL_INPUT_PORT_TYPES           type,
    uint32_t                                portTypeIndex){
    uint32_t portIndex = 0;
    uint32_t portCount = 0;

    while ((type != rateControlPorts[portIndex].type) && (type != RATE_CONTROL_INPUT_PORT_INVALID)) {
        portCount += rateControlPorts[portIndex++].count;
    }

    return (portCount + portTypeIndex);
}
// Rate Control
static uint32_t RateControlPortTotalCount(void){
    uint32_t portIndex = 0;
    uint32_t total_count = 0;

    while (rateControlPorts[portIndex].type != RATE_CONTROL_INPUT_PORT_INVALID) {
        total_count += rateControlPorts[portIndex++].count;
    }

    return total_count;
}

// EncDec
typedef struct {
    int32_t  type;
    uint32_t  count;
} EncDecPorts_t;
static EncDecPorts_t encDecPorts[] = {
    {ENCDEC_INPUT_PORT_MDC,        0},
    {ENCDEC_INPUT_PORT_ENCDEC,     0},
    {ENCDEC_INPUT_PORT_INVALID,    0}
};

/*****************************************
 * Input Port Lookup
 *****************************************/
// EncDec
static uint32_t EncDecPortLookup(
    int32_t  type,
    uint32_t  portTypeIndex)
{
    uint32_t portIndex = 0;
    uint32_t portCount = 0;

    while ((type != encDecPorts[portIndex].type) && (type != ENCDEC_INPUT_PORT_INVALID)) {
        portCount += encDecPorts[portIndex++].count;
    }

    return (portCount + portTypeIndex);
}
// EncDec
static uint32_t EncDecPortTotalCount(void){
    uint32_t portIndex = 0;
    uint32_t total_count = 0;

    while (encDecPorts[portIndex].type != ENCDEC_INPUT_PORT_INVALID) {
        total_count += encDecPorts[portIndex++].count;
    }

    return total_count;
}
/*****************************************
 * Input Port Total Count
 *****************************************/

void lib_svt_encoder_send_error_exit(
    EbPtr                    hComponent,
    uint32_t                 errorCode);

/**********************************
* Encoder Library Handle Constructor
**********************************/
static EbErrorType eb_enc_handle_ctor(
    EbEncHandle_t **encHandleDblPtr,
    EbComponentType * ebHandlePtr)
{
    uint32_t  instanceIndex;
    EbErrorType return_error = EB_ErrorNone;
    // Allocate Memory
    EbEncHandle_t *encHandlePtr = (EbEncHandle_t*)malloc(sizeof(EbEncHandle_t));
    *encHandleDblPtr = encHandlePtr;
    if (encHandlePtr == (EbEncHandle_t*)EB_NULL) {
        return EB_ErrorInsufficientResources;
    }
    encHandlePtr->memory_map = (EbMemoryMapEntry*)malloc(sizeof(EbMemoryMapEntry) * MAX_NUM_PTR);
    encHandlePtr->memory_map_index = 0;
    encHandlePtr->total_lib_memory = sizeof(EbEncHandle_t) + sizeof(EbMemoryMapEntry) * MAX_NUM_PTR;

    // Save Memory Map Pointers 
    total_lib_memory = &encHandlePtr->total_lib_memory;
    memory_map = encHandlePtr->memory_map;
    memory_map_index = &encHandlePtr->memory_map_index;
    libMallocCount = 0;
    lib_thread_count = 0;
    libMutexCount = 0;
    libSemaphoreCount = 0;

    if (memory_map == (EbMemoryMapEntry*)EB_NULL) {
        return EB_ErrorInsufficientResources;
    }

    return_error = InitThreadManagmentParams();
    if (return_error == EB_ErrorInsufficientResources) {
        return EB_ErrorInsufficientResources;
    }

    encHandlePtr->encodeInstanceTotalCount = EB_EncodeInstancesTotalCount;

    EB_MALLOC(uint32_t*, encHandlePtr->compute_segments_total_count_array, sizeof(uint32_t) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);

    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {
        encHandlePtr->compute_segments_total_count_array[instanceIndex] = EB_ComputeSegmentInitCount;
    }

    // Config Set Count
    encHandlePtr->sequenceControlSetPoolTotalCount = EB_SequenceControlSetPoolInitCount;

    // Sequence Control Set Buffers
    encHandlePtr->sequenceControlSetPoolPtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->sequenceControlSetPoolProducerFifoPtrArray = (EbFifo_t**)EB_NULL;

    // Picture Buffers
    encHandlePtr->referencePicturePoolPtrArray = (EbSystemResource_t**)EB_NULL;
    encHandlePtr->paReferencePicturePoolPtrArray = (EbSystemResource_t**)EB_NULL;

    // Picture Buffer Producer Fifos  
    encHandlePtr->referencePicturePoolProducerFifoPtrDblArray = (EbFifo_t***)EB_NULL;
    encHandlePtr->paReferencePicturePoolProducerFifoPtrDblArray = (EbFifo_t***)EB_NULL;

    // Threads
    encHandlePtr->resourceCoordinationThreadHandle = (EbHandle)EB_NULL;
    encHandlePtr->pictureAnalysisThreadHandleArray = (EbHandle*)EB_NULL;
    encHandlePtr->pictureDecisionThreadHandle = (EbHandle)EB_NULL;
    encHandlePtr->motionEstimationThreadHandleArray = (EbHandle*)EB_NULL;
    encHandlePtr->initialRateControlThreadHandle = (EbHandle)EB_NULL;
    encHandlePtr->sourceBasedOperationsThreadHandleArray = (EbHandle*)EB_NULL;
    encHandlePtr->pictureManagerThreadHandle = (EbHandle)EB_NULL;
    encHandlePtr->rateControlThreadHandle = (EbHandle)EB_NULL;
    encHandlePtr->modeDecisionConfigurationThreadHandleArray = (EbHandle*)EB_NULL;
    encHandlePtr->encDecThreadHandleArray = (EbHandle*)EB_NULL;
    encHandlePtr->entropyCodingThreadHandleArray = (EbHandle*)EB_NULL;
    encHandlePtr->packetizationThreadHandle = (EbHandle)EB_NULL;
#if FILT_PROC
    encHandlePtr->dlfThreadHandleArray = (EbHandle*)EB_NULL;
    encHandlePtr->cdefThreadHandleArray = (EbHandle*)EB_NULL;
    encHandlePtr->restThreadHandleArray = (EbHandle*)EB_NULL;
#endif
    // Contexts
    encHandlePtr->resourceCoordinationContextPtr = (EbPtr)EB_NULL;
    encHandlePtr->pictureAnalysisContextPtrArray = (EbPtr*)EB_NULL;
    encHandlePtr->pictureDecisionContextPtr = (EbPtr)EB_NULL;
    encHandlePtr->motionEstimationContextPtrArray = (EbPtr*)EB_NULL;
    encHandlePtr->initialRateControlContextPtr = (EbPtr)EB_NULL;
    encHandlePtr->sourceBasedOperationsContextPtrArray = (EbPtr*)EB_NULL;
    encHandlePtr->pictureManagerContextPtr = (EbPtr)EB_NULL;
    encHandlePtr->rateControlContextPtr = (EbPtr)EB_NULL;
    encHandlePtr->modeDecisionConfigurationContextPtrArray = (EbPtr*)EB_NULL;
    encHandlePtr->encDecContextPtrArray = (EbPtr*)EB_NULL;
    encHandlePtr->entropyCodingContextPtrArray = (EbPtr*)EB_NULL;
#if FILT_PROC
    encHandlePtr->dlfContextPtrArray = (EbPtr*)EB_NULL;
    encHandlePtr->cdefContextPtrArray = (EbPtr*)EB_NULL;
    encHandlePtr->restContextPtrArray = (EbPtr*)EB_NULL;
#endif
    encHandlePtr->packetizationContextPtr = (EbPtr)EB_NULL;

    // System Resource Managers
    encHandlePtr->input_buffer_resource_ptr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->output_stream_buffer_resource_ptr_array = (EbSystemResource_t**)EB_NULL;
    encHandlePtr->resourceCoordinationResultsResourcePtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->pictureAnalysisResultsResourcePtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->pictureDecisionResultsResourcePtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->motionEstimationResultsResourcePtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->initialRateControlResultsResourcePtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->pictureDemuxResultsResourcePtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->rateControlTasksResourcePtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->rateControlResultsResourcePtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->encDecTasksResourcePtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->encDecResultsResourcePtr = (EbSystemResource_t*)EB_NULL;
    encHandlePtr->entropyCodingResultsResourcePtr = (EbSystemResource_t*)EB_NULL;

    // Inter-Process Producer Fifos
    encHandlePtr->input_buffer_producer_fifo_ptr_array = (EbFifo_t**)EB_NULL;
    encHandlePtr->output_stream_buffer_producer_fifo_ptr_dbl_array = (EbFifo_t***)EB_NULL;
    encHandlePtr->resourceCoordinationResultsProducerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->pictureDemuxResultsProducerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->pictureManagerResultsProducerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->rateControlTasksProducerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->rateControlResultsProducerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->encDecTasksProducerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->encDecResultsProducerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->entropyCodingResultsProducerFifoPtrArray = (EbFifo_t**)EB_NULL;
#if FILT_PROC
    encHandlePtr->dlfResultsProducerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->cdefResultsProducerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->restResultsProducerFifoPtrArray = (EbFifo_t**)EB_NULL;

    encHandlePtr->dlfResultsConsumerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->cdefResultsConsumerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->restResultsConsumerFifoPtrArray = (EbFifo_t**)EB_NULL;
#endif
    // Inter-Process Consumer Fifos
    encHandlePtr->input_buffer_consumer_fifo_ptr_array = (EbFifo_t**)EB_NULL;
    encHandlePtr->output_stream_buffer_consumer_fifo_ptr_dbl_array = (EbFifo_t***)EB_NULL;
    encHandlePtr->resourceCoordinationResultsConsumerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->pictureDemuxResultsConsumerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->rateControlTasksConsumerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->rateControlResultsConsumerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->encDecTasksConsumerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->encDecResultsConsumerFifoPtrArray = (EbFifo_t**)EB_NULL;
    encHandlePtr->entropyCodingResultsConsumerFifoPtrArray = (EbFifo_t**)EB_NULL;

    // Initialize Callbacks
    EB_MALLOC(EbCallback_t**, encHandlePtr->app_callback_ptr_array, sizeof(EbCallback_t*) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);

    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {
        EB_MALLOC(EbCallback_t*, encHandlePtr->app_callback_ptr_array[instanceIndex], sizeof(EbCallback_t), EB_N_PTR);
        encHandlePtr->app_callback_ptr_array[instanceIndex]->ErrorHandler = lib_svt_encoder_send_error_exit;
        encHandlePtr->app_callback_ptr_array[instanceIndex]->handle = ebHandlePtr;
    }

    // Initialize Sequence Control Set Instance Array
    EB_MALLOC(EbSequenceControlSetInstance_t**, encHandlePtr->sequence_control_set_instance_array, sizeof(EbSequenceControlSetInstance_t*) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);

    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {
        return_error = eb_sequence_control_set_instance_ctor(&encHandlePtr->sequence_control_set_instance_array[instanceIndex]);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }
    return EB_ErrorNone;
}

EbErrorType EbInputBufferHeaderCtor(
    EbPtr *objectDblPtr,
    EbPtr  objectInitDataPtr);

EbErrorType EbOutputReconBufferHeaderCtor(
    EbPtr *objectDblPtr,
    EbPtr  objectInitDataPtr);

EbErrorType EbOutputBufferHeaderCtor(
    EbPtr *objectDblPtr,
    EbPtr objectInitDataPtr);


#if FILT_PROC

EbErrorType DlfResultsCtor(
    EbPtr *object_dbl_ptr,
    EbPtr object_init_data_ptr)
{
    DlfResults_t *context_ptr;
    EB_MALLOC(DlfResults_t*, context_ptr, sizeof(DlfResults_t), EB_N_PTR);

    *object_dbl_ptr = (EbPtr)context_ptr;

    (void)object_init_data_ptr;

    return EB_ErrorNone;
}
EbErrorType CdefResultsCtor(
    EbPtr *object_dbl_ptr,
    EbPtr object_init_data_ptr)
{
    CdefResults_t *context_ptr;
    EB_MALLOC(CdefResults_t*, context_ptr, sizeof(CdefResults_t), EB_N_PTR);

    *object_dbl_ptr = (EbPtr)context_ptr;

    (void)object_init_data_ptr;

    return EB_ErrorNone;
}

EbErrorType RestResultsCtor(
    EbPtr *object_dbl_ptr,
    EbPtr object_init_data_ptr)
{
    RestResults_t *context_ptr;
    EB_MALLOC(RestResults_t*, context_ptr, sizeof(RestResults_t), EB_N_PTR);

    *object_dbl_ptr = (EbPtr)context_ptr;

    (void)object_init_data_ptr;

    return EB_ErrorNone;
}
#endif
/**********************************
* Initialize Encoder Library
**********************************/
#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_init_encoder(EbComponentType *svt_enc_component)
{
    if(svt_enc_component == NULL)
        return EB_ErrorBadParameter;
    EbEncHandle_t *encHandlePtr = (EbEncHandle_t*)svt_enc_component->pComponentPrivate;
    EbErrorType return_error = EB_ErrorNone;
    uint32_t instanceIndex;
    uint32_t processIndex;
    uint32_t max_picture_width;
    uint32_t maxLookAheadDistance = 0;

    EbBool is16bit = (EbBool)(encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config.encoder_bit_depth > EB_8BIT);

    /************************************
    * Plateform detection
    ************************************/
    if (encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config.asm_type == 1) {
        encHandlePtr->sequence_control_set_instance_array[0]->encode_context_ptr->asm_type = GetCpuAsmType();
    }
    else {
        encHandlePtr->sequence_control_set_instance_array[0]->encode_context_ptr->asm_type = encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config.asm_type;
    }

#if !INTRA_ASM
    init_intra_predictors_internal();
#endif
    setup_rtcd_internal(encHandlePtr->sequence_control_set_instance_array[0]->encode_context_ptr->asm_type);
    asmSetConvolveAsmTable();

    init_intra_dc_predictors_c_internal();

    asmSetConvolveHbdAsmTable();

#if  INTRA_ASM
    init_intra_predictors_internal();
#endif
    EbSequenceControlSetInitData_t scs_init;
    scs_init.sb_size = encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config.super_block_size;

    build_blk_geom(scs_init.sb_size == 128);


    /************************************
    * Sequence Control Set
    ************************************/
    return_error = eb_system_resource_ctor(
        &encHandlePtr->sequenceControlSetPoolPtr,
        encHandlePtr->sequenceControlSetPoolTotalCount,
        1,
        0,
        &encHandlePtr->sequenceControlSetPoolProducerFifoPtrArray,
        (EbFifo_t ***)EB_NULL,
        EB_FALSE,
        eb_sequence_control_set_ctor,
        &scs_init);


    if (return_error == EB_ErrorInsufficientResources) {
        return EB_ErrorInsufficientResources;
    }
    /************************************
    * Picture Control Set: Parent
    ************************************/
    EB_MALLOC(EbSystemResource_t**, encHandlePtr->pictureParentControlSetPoolPtrArray, sizeof(EbSystemResource_t*)  * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);


    EB_MALLOC(EbFifo_t***, encHandlePtr->pictureParentControlSetPoolProducerFifoPtrDblArray, sizeof(EbSystemResource_t**) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);

    // Updating the pictureControlSetPoolTotalCount based on the maximum look ahead distance
    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {

#if !CONTENT_BASED_QPS
        if (encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.rate_control_mode == 0 && encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.improve_sharpness == 0) {
            encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.look_ahead_distance = 0;
        }
  #endif
        maxLookAheadDistance = MAX(maxLookAheadDistance, encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.look_ahead_distance);
    }


    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {

        // The segment Width & Height Arrays are in units of LCUs, not samples
        PictureControlSetInitData_t inputData;

        inputData.picture_width = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_width;
        inputData.picture_height = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_height;
        inputData.left_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->left_padding;
        inputData.right_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->right_padding;
        inputData.top_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->top_padding;
        inputData.bot_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->bot_padding;
        inputData.bit_depth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->output_bitdepth;
        inputData.sb_sz = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz;
        inputData.max_depth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_sb_depth;
        inputData.is16bit = is16bit;
        inputData.ten_bit_format = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.ten_bit_format;
        inputData.compressed_ten_bit_format = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.compressed_ten_bit_format;
        encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->picture_control_set_pool_init_count += maxLookAheadDistance;
        inputData.enc_mode = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.enc_mode;
        inputData.speed_control = (uint8_t)encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.speed_control_flag;
        inputData.film_grain_noise_level = encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config.film_grain_denoise_strength;
        inputData.encoder_bit_depth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.encoder_bit_depth;

        inputData.ext_block_flag = (uint8_t)encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.ext_block_flag;

        inputData.in_loop_me_flag = (uint8_t)encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.in_loop_me_flag;

        return_error = eb_system_resource_ctor(
            &(encHandlePtr->pictureParentControlSetPoolPtrArray[instanceIndex]),
            encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->picture_control_set_pool_init_count,//encHandlePtr->pictureControlSetPoolTotalCount,
            1,
            0,
            &encHandlePtr->pictureParentControlSetPoolProducerFifoPtrDblArray[instanceIndex],
            (EbFifo_t ***)EB_NULL,
            EB_FALSE,
            picture_parent_control_set_ctor,
            &inputData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    /************************************
    * Picture Control Set: Child
    ************************************/
    EB_MALLOC(EbSystemResource_t**, encHandlePtr->pictureControlSetPoolPtrArray, sizeof(EbSystemResource_t*)  * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);

    EB_MALLOC(EbFifo_t***, encHandlePtr->pictureControlSetPoolProducerFifoPtrDblArray, sizeof(EbSystemResource_t**) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);

    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {

        // The segment Width & Height Arrays are in units of LCUs, not samples
        PictureControlSetInitData_t inputData;
        unsigned i;

        inputData.enc_dec_segment_col = 0;
        inputData.enc_dec_segment_row = 0;
        for (i = 0; i <= encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.hierarchical_levels; ++i) {
            inputData.enc_dec_segment_col = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->enc_dec_segment_col_count_array[i] > inputData.enc_dec_segment_col ?
                (uint16_t)encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->enc_dec_segment_col_count_array[i] :
                inputData.enc_dec_segment_col;
            inputData.enc_dec_segment_row = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->enc_dec_segment_row_count_array[i] > inputData.enc_dec_segment_row ?
                (uint16_t)encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->enc_dec_segment_row_count_array[i] :
                inputData.enc_dec_segment_row;
        }

        inputData.picture_width = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_width;
        inputData.picture_height = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_height;
        inputData.left_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->left_padding;
        inputData.right_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->right_padding;
        inputData.top_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->top_padding;
        inputData.bot_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->bot_padding;
        inputData.bit_depth = EB_8BIT;
        inputData.sb_sz = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz;
        inputData.sb_size_pix = scs_init.sb_size;
        inputData.max_depth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_sb_depth;
        inputData.is16bit = is16bit;
        return_error = eb_system_resource_ctor(
            &(encHandlePtr->pictureControlSetPoolPtrArray[instanceIndex]),
            encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->picture_control_set_pool_init_count_child, //EB_PictureControlSetPoolInitCountChild,
            1,
            0,
            &encHandlePtr->pictureControlSetPoolProducerFifoPtrDblArray[instanceIndex],
            (EbFifo_t ***)EB_NULL,
            EB_FALSE,
            picture_control_set_ctor,
            &inputData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    /************************************
    * Picture Buffers
    ************************************/

    // Allocate Resource Arrays
    EB_MALLOC(EbSystemResource_t**, encHandlePtr->referencePicturePoolPtrArray, sizeof(EbSystemResource_t*) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);
    EB_MALLOC(EbSystemResource_t**, encHandlePtr->paReferencePicturePoolPtrArray, sizeof(EbSystemResource_t*) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);

    // Allocate Producer Fifo Arrays
    EB_MALLOC(EbFifo_t***, encHandlePtr->referencePicturePoolProducerFifoPtrDblArray, sizeof(EbFifo_t**) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);
    EB_MALLOC(EbFifo_t***, encHandlePtr->paReferencePicturePoolProducerFifoPtrDblArray, sizeof(EbFifo_t**) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);

    // Rate Control
    rateControlPorts[0].count = EB_PictureManagerProcessInitCount;
    rateControlPorts[1].count = EB_PacketizationProcessInitCount;
    rateControlPorts[2].count = encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->entropy_coding_process_init_count;
    rateControlPorts[3].count = 0;

    encDecPorts[ENCDEC_INPUT_PORT_MDC].count = encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->mode_decision_configuration_process_init_count;
    encDecPorts[ENCDEC_INPUT_PORT_ENCDEC].count = encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->enc_dec_process_init_count;

    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {

        EbReferenceObjectDescInitData_t     EbReferenceObjectDescInitDataStructure;
        EbPaReferenceObjectDescInitData_t   EbPaReferenceObjectDescInitDataStructure;
        EbPictureBufferDescInitData_t       referencePictureBufferDescInitData;
        EbPictureBufferDescInitData_t       quarterDecimPictureBufferDescInitData;
        EbPictureBufferDescInitData_t       sixteenthDecimPictureBufferDescInitData;

        // Initialize the various Picture types
        referencePictureBufferDescInitData.maxWidth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_width;
        referencePictureBufferDescInitData.maxHeight = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_height;
        referencePictureBufferDescInitData.bit_depth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->input_bitdepth;
        referencePictureBufferDescInitData.bufferEnableMask = PICTURE_BUFFER_DESC_FULL_MASK;

        referencePictureBufferDescInitData.left_padding = PAD_VALUE;
        referencePictureBufferDescInitData.right_padding = PAD_VALUE;
        referencePictureBufferDescInitData.top_padding = PAD_VALUE;
        referencePictureBufferDescInitData.bot_padding = PAD_VALUE;

        referencePictureBufferDescInitData.splitMode = EB_FALSE;

        if (is16bit) {
            referencePictureBufferDescInitData.bit_depth = EB_10BIT;
        }

        EbReferenceObjectDescInitDataStructure.referencePictureDescInitData = referencePictureBufferDescInitData;

        // Reference Picture Buffers
        return_error = eb_system_resource_ctor(
            &encHandlePtr->referencePicturePoolPtrArray[instanceIndex],
            encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->reference_picture_buffer_init_count,//encHandlePtr->referencePicturePoolTotalCount,
            EB_PictureManagerProcessInitCount,
            0,
            &encHandlePtr->referencePicturePoolProducerFifoPtrDblArray[instanceIndex],
            (EbFifo_t ***)EB_NULL,
            EB_FALSE,
            eb_reference_object_ctor,
            &(EbReferenceObjectDescInitDataStructure));

        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }

        // PA Reference Picture Buffers
        // Currently, only Luma samples are needed in the PA
        referencePictureBufferDescInitData.maxWidth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_width;
        referencePictureBufferDescInitData.maxHeight = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_height;
        referencePictureBufferDescInitData.bit_depth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->input_bitdepth;

        referencePictureBufferDescInitData.bufferEnableMask = 0;

        referencePictureBufferDescInitData.left_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz + ME_FILTER_TAP;
        referencePictureBufferDescInitData.right_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz + ME_FILTER_TAP;
        referencePictureBufferDescInitData.top_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz + ME_FILTER_TAP;
        referencePictureBufferDescInitData.bot_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz + ME_FILTER_TAP;
        referencePictureBufferDescInitData.splitMode = EB_FALSE;

        quarterDecimPictureBufferDescInitData.maxWidth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_width >> 1;
        quarterDecimPictureBufferDescInitData.maxHeight = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_height >> 1;
        quarterDecimPictureBufferDescInitData.bit_depth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->input_bitdepth;
        quarterDecimPictureBufferDescInitData.bufferEnableMask = PICTURE_BUFFER_DESC_LUMA_MASK;
        quarterDecimPictureBufferDescInitData.left_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz >> 1;
        quarterDecimPictureBufferDescInitData.right_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz >> 1;
        quarterDecimPictureBufferDescInitData.top_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz >> 1;
        quarterDecimPictureBufferDescInitData.bot_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz >> 1;
        quarterDecimPictureBufferDescInitData.splitMode = EB_FALSE;

        sixteenthDecimPictureBufferDescInitData.maxWidth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_width >> 2;
        sixteenthDecimPictureBufferDescInitData.maxHeight = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_height >> 2;
        sixteenthDecimPictureBufferDescInitData.bit_depth = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->input_bitdepth;
        sixteenthDecimPictureBufferDescInitData.bufferEnableMask = PICTURE_BUFFER_DESC_LUMA_MASK;
        sixteenthDecimPictureBufferDescInitData.left_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz >> 2;
        sixteenthDecimPictureBufferDescInitData.right_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz >> 2;
        sixteenthDecimPictureBufferDescInitData.top_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz >> 2;
        sixteenthDecimPictureBufferDescInitData.bot_padding = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->sb_sz >> 2;
        sixteenthDecimPictureBufferDescInitData.splitMode = EB_FALSE;

        EbPaReferenceObjectDescInitDataStructure.referencePictureDescInitData = referencePictureBufferDescInitData;
        EbPaReferenceObjectDescInitDataStructure.quarterPictureDescInitData = quarterDecimPictureBufferDescInitData;
        EbPaReferenceObjectDescInitDataStructure.sixteenthPictureDescInitData = sixteenthDecimPictureBufferDescInitData;

        // Reference Picture Buffers
        return_error = eb_system_resource_ctor(
            &encHandlePtr->paReferencePicturePoolPtrArray[instanceIndex],
            encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->pa_reference_picture_buffer_init_count,
            EB_PictureDecisionProcessInitCount,
            0,
            &encHandlePtr->paReferencePicturePoolProducerFifoPtrDblArray[instanceIndex],
            (EbFifo_t ***)EB_NULL,
            EB_FALSE,
            eb_pa_reference_object_ctor,
            &(EbPaReferenceObjectDescInitDataStructure));
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }

        // Set the SequenceControlSet Picture Pool Fifo Ptrs
        encHandlePtr->sequence_control_set_instance_array[instanceIndex]->encode_context_ptr->reference_picture_pool_fifo_ptr = (encHandlePtr->referencePicturePoolProducerFifoPtrDblArray[instanceIndex])[0];
        encHandlePtr->sequence_control_set_instance_array[instanceIndex]->encode_context_ptr->pa_reference_picture_pool_fifo_ptr = (encHandlePtr->paReferencePicturePoolProducerFifoPtrDblArray[instanceIndex])[0];
    }

    /************************************
    * System Resource Managers & Fifos
    ************************************/

    // EbBufferHeaderType Input
    return_error = eb_system_resource_ctor(
        &encHandlePtr->input_buffer_resource_ptr,
        encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->input_buffer_fifo_init_count,
        1,
        EB_ResourceCoordinationProcessInitCount,
        &encHandlePtr->input_buffer_producer_fifo_ptr_array,
        &encHandlePtr->input_buffer_consumer_fifo_ptr_array,
        EB_TRUE,
        EbInputBufferHeaderCtor,
        encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr);

    if (return_error == EB_ErrorInsufficientResources) {
        return EB_ErrorInsufficientResources;
    }
    // EbBufferHeaderType Output Stream
    EB_MALLOC(EbSystemResource_t**, encHandlePtr->output_stream_buffer_resource_ptr_array, sizeof(EbSystemResource_t*) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);
    EB_MALLOC(EbFifo_t***, encHandlePtr->output_stream_buffer_producer_fifo_ptr_dbl_array, sizeof(EbFifo_t**)          * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);
    EB_MALLOC(EbFifo_t***, encHandlePtr->output_stream_buffer_consumer_fifo_ptr_dbl_array, sizeof(EbFifo_t**)          * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);

    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {
        return_error = eb_system_resource_ctor(
            &encHandlePtr->output_stream_buffer_resource_ptr_array[instanceIndex],
            encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->output_stream_buffer_fifo_init_count,
            encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->total_process_init_count,//EB_PacketizationProcessInitCount,
            1,
            &encHandlePtr->output_stream_buffer_producer_fifo_ptr_dbl_array[instanceIndex],
            &encHandlePtr->output_stream_buffer_consumer_fifo_ptr_dbl_array[instanceIndex],
            EB_TRUE,
            EbOutputBufferHeaderCtor,
            &encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }
    if (encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config.recon_enabled) {
        // EbBufferHeaderType Output Recon
        EB_MALLOC(EbSystemResource_t**, encHandlePtr->output_recon_buffer_resource_ptr_array, sizeof(EbSystemResource_t*) * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);
        EB_MALLOC(EbFifo_t***, encHandlePtr->output_recon_buffer_producer_fifo_ptr_dbl_array, sizeof(EbFifo_t**)          * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);
        EB_MALLOC(EbFifo_t***, encHandlePtr->output_recon_buffer_consumer_fifo_ptr_dbl_array, sizeof(EbFifo_t**)          * encHandlePtr->encodeInstanceTotalCount, EB_N_PTR);

        for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {
            return_error = eb_system_resource_ctor(
                &encHandlePtr->output_recon_buffer_resource_ptr_array[instanceIndex],
                encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->output_recon_buffer_fifo_init_count,
                encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->enc_dec_process_init_count,
                1,
                &encHandlePtr->output_recon_buffer_producer_fifo_ptr_dbl_array[instanceIndex],
                &encHandlePtr->output_recon_buffer_consumer_fifo_ptr_dbl_array[instanceIndex],
                EB_TRUE,
                EbOutputReconBufferHeaderCtor,
                encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr);
            if (return_error == EB_ErrorInsufficientResources) {
                return EB_ErrorInsufficientResources;
            }
        }
    }

    // Resource Coordination Results
    {
        ResourceCoordinationResultInitData_t resourceCoordinationResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->resourceCoordinationResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->resource_coordination_fifo_init_count,
            EB_ResourceCoordinationProcessInitCount,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->picture_analysis_process_init_count,
            &encHandlePtr->resourceCoordinationResultsProducerFifoPtrArray,
            &encHandlePtr->resourceCoordinationResultsConsumerFifoPtrArray,
            EB_TRUE,
            resource_coordination_result_ctor,
            &resourceCoordinationResultInitData);

        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }


    // Picture Analysis Results
    {
        PictureAnalysisResultInitData_t pictureAnalysisResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->pictureAnalysisResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->picture_analysis_fifo_init_count,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->picture_analysis_process_init_count,
            EB_PictureDecisionProcessInitCount,
            &encHandlePtr->pictureAnalysisResultsProducerFifoPtrArray,
            &encHandlePtr->pictureAnalysisResultsConsumerFifoPtrArray,
            EB_TRUE,
            picture_analysis_result_ctor,
            &pictureAnalysisResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Picture Decision Results
    {
        PictureDecisionResultInitData_t pictureDecisionResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->pictureDecisionResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->picture_decision_fifo_init_count,
            EB_PictureDecisionProcessInitCount,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->motion_estimation_process_init_count,
            &encHandlePtr->pictureDecisionResultsProducerFifoPtrArray,
            &encHandlePtr->pictureDecisionResultsConsumerFifoPtrArray,
            EB_TRUE,
            picture_decision_result_ctor,
            &pictureDecisionResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Motion Estimation Results
    {
        MotionEstimationResultsInitData_t motionEstimationResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->motionEstimationResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->motion_estimation_fifo_init_count,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->motion_estimation_process_init_count,
            EB_InitialRateControlProcessInitCount,
            &encHandlePtr->motionEstimationResultsProducerFifoPtrArray,
            &encHandlePtr->motionEstimationResultsConsumerFifoPtrArray,
            EB_TRUE,
            MotionEstimationResultsCtor,
            &motionEstimationResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Initial Rate Control Results
    {
        InitialRateControlResultInitData_t initialRateControlResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->initialRateControlResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->initial_rate_control_fifo_init_count,
            EB_InitialRateControlProcessInitCount,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->source_based_operations_process_init_count,
            &encHandlePtr->initialRateControlResultsProducerFifoPtrArray,
            &encHandlePtr->initialRateControlResultsConsumerFifoPtrArray,
            EB_TRUE,
            InitialRateControlResultsCtor,
            &initialRateControlResultInitData);

        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Picture Demux Results
    {
        PictureResultInitData_t pictureResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->pictureDemuxResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->picture_demux_fifo_init_count,
#if FILT_PROC
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->source_based_operations_process_init_count + encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->rest_process_init_count,
#else
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->source_based_operations_process_init_count + encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->enc_dec_process_init_count,
#endif
            EB_PictureManagerProcessInitCount,
            &encHandlePtr->pictureDemuxResultsProducerFifoPtrArray,
            &encHandlePtr->pictureDemuxResultsConsumerFifoPtrArray,
            EB_TRUE,
            picture_results_ctor,
            &pictureResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Rate Control Tasks
    {
        RateControlTasksInitData_t rateControlTasksInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->rateControlTasksResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->rate_control_tasks_fifo_init_count,
            RateControlPortTotalCount(),
            EB_RateControlProcessInitCount,
            &encHandlePtr->rateControlTasksProducerFifoPtrArray,
            &encHandlePtr->rateControlTasksConsumerFifoPtrArray,
            EB_TRUE,
            rate_control_tasks_ctor,
            &rateControlTasksInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Rate Control Results
    {
        RateControlResultsInitData_t rateControlResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->rateControlResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->rate_control_fifo_init_count,
            EB_RateControlProcessInitCount,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->mode_decision_configuration_process_init_count,
            &encHandlePtr->rateControlResultsProducerFifoPtrArray,
            &encHandlePtr->rateControlResultsConsumerFifoPtrArray,
            EB_TRUE,
            rate_control_results_ctor,
            &rateControlResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }
    // EncDec Tasks
    {
        EncDecTasksInitData_t ModeDecisionResultInitData;
        unsigned i;

        ModeDecisionResultInitData.encDecSegmentRowCount = 0;

        for (i = 0; i <= encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config.hierarchical_levels; ++i) {
            ModeDecisionResultInitData.encDecSegmentRowCount = MAX(
                ModeDecisionResultInitData.encDecSegmentRowCount,
                encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->enc_dec_segment_row_count_array[i]);
        }

        return_error = eb_system_resource_ctor(
            &encHandlePtr->encDecTasksResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->mode_decision_configuration_fifo_init_count,
            EncDecPortTotalCount(),
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->enc_dec_process_init_count,
            &encHandlePtr->encDecTasksProducerFifoPtrArray,
            &encHandlePtr->encDecTasksConsumerFifoPtrArray,
            EB_TRUE,
            EncDecTasksCtor,
            &ModeDecisionResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // EncDec Results
    {
        EncDecResultsInitData_t encDecResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->encDecResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->enc_dec_fifo_init_count,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->enc_dec_process_init_count,
#if FILT_PROC
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->dlf_process_init_count,
#else
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->entropy_coding_process_init_count,
#endif
            &encHandlePtr->encDecResultsProducerFifoPtrArray,
            &encHandlePtr->encDecResultsConsumerFifoPtrArray,
            EB_TRUE,
            EncDecResultsCtor,
            &encDecResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

#if FILT_PROC
    //DLF results
    {
        EntropyCodingResultsInitData_t dlfResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->dlfResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->dlf_fifo_init_count,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->dlf_process_init_count,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->cdef_process_init_count,
            &encHandlePtr->dlfResultsProducerFifoPtrArray,
            &encHandlePtr->dlfResultsConsumerFifoPtrArray,
            EB_TRUE,
            DlfResultsCtor,
            &dlfResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }
    //CDEF results
    {
        EntropyCodingResultsInitData_t cdefResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->cdefResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->cdef_fifo_init_count,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->cdef_process_init_count,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->rest_process_init_count,
            &encHandlePtr->cdefResultsProducerFifoPtrArray,
            &encHandlePtr->cdefResultsConsumerFifoPtrArray,
            EB_TRUE,
            CdefResultsCtor,
            &cdefResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }

    }
    //REST results
    {
        EntropyCodingResultsInitData_t restResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->restResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->rest_fifo_init_count,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->rest_process_init_count,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->entropy_coding_process_init_count,
            &encHandlePtr->restResultsProducerFifoPtrArray,
            &encHandlePtr->restResultsConsumerFifoPtrArray,
            EB_TRUE,
            RestResultsCtor,
            &restResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }
#endif

    // Entropy Coding Results
    {
        EntropyCodingResultsInitData_t entropyCodingResultInitData;

        return_error = eb_system_resource_ctor(
            &encHandlePtr->entropyCodingResultsResourcePtr,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->entropy_coding_fifo_init_count,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->entropy_coding_process_init_count,
            EB_PacketizationProcessInitCount,
            &encHandlePtr->entropyCodingResultsProducerFifoPtrArray,
            &encHandlePtr->entropyCodingResultsConsumerFifoPtrArray,
            EB_TRUE,
            EntropyCodingResultsCtor,
            &entropyCodingResultInitData);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    /************************************
    * App Callbacks
    ************************************/
    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {
        encHandlePtr->sequence_control_set_instance_array[instanceIndex]->encode_context_ptr->app_callback_ptr = encHandlePtr->app_callback_ptr_array[instanceIndex];
    }

    // svt Output Buffer Fifo Ptrs
    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {
        encHandlePtr->sequence_control_set_instance_array[instanceIndex]->encode_context_ptr->stream_output_fifo_ptr     = (encHandlePtr->output_stream_buffer_producer_fifo_ptr_dbl_array[instanceIndex])[0];
        if (encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config.recon_enabled)
            encHandlePtr->sequence_control_set_instance_array[instanceIndex]->encode_context_ptr->recon_output_fifo_ptr      = (encHandlePtr->output_recon_buffer_producer_fifo_ptr_dbl_array[instanceIndex])[0];
    }

    /************************************
    * Contexts
    ************************************/

    // Resource Coordination Context
    return_error = resource_coordination_context_ctor(
        (ResourceCoordinationContext_t**)&encHandlePtr->resourceCoordinationContextPtr,
        encHandlePtr->input_buffer_consumer_fifo_ptr_array[0],
        encHandlePtr->resourceCoordinationResultsProducerFifoPtrArray[0],
        encHandlePtr->pictureParentControlSetPoolProducerFifoPtrDblArray[0],//ResourceCoordination works with ParentPCS
        encHandlePtr->sequence_control_set_instance_array,
        encHandlePtr->sequenceControlSetPoolProducerFifoPtrArray[0],
        encHandlePtr->app_callback_ptr_array,
        encHandlePtr->compute_segments_total_count_array,
        encHandlePtr->encodeInstanceTotalCount);

    if (return_error == EB_ErrorInsufficientResources) {
        return EB_ErrorInsufficientResources;
    }
    // Picture Analysis Context
    EB_MALLOC(EbPtr*, encHandlePtr->pictureAnalysisContextPtrArray, sizeof(EbPtr) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->picture_analysis_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->picture_analysis_process_init_count; ++processIndex) {

        EbPictureBufferDescInitData_t  pictureBufferDescConf;
        pictureBufferDescConf.maxWidth = encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_width;
        pictureBufferDescConf.maxHeight = encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_height;
        pictureBufferDescConf.bit_depth = EB_8BIT;
        pictureBufferDescConf.bufferEnableMask = PICTURE_BUFFER_DESC_Y_FLAG;
        pictureBufferDescConf.left_padding = 0;
        pictureBufferDescConf.right_padding = 0;
        pictureBufferDescConf.top_padding = 0;
        pictureBufferDescConf.bot_padding = 0;
        pictureBufferDescConf.splitMode = EB_FALSE;

        return_error = picture_analysis_context_ctor(
            &pictureBufferDescConf,
            EB_TRUE,
            (PictureAnalysisContext_t**)&encHandlePtr->pictureAnalysisContextPtrArray[processIndex],
            encHandlePtr->resourceCoordinationResultsConsumerFifoPtrArray[processIndex],
            encHandlePtr->pictureAnalysisResultsProducerFifoPtrArray[processIndex]);

        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Picture Decision Context
    {
        // Initialize the various Picture types

        instanceIndex = 0;


        return_error = picture_decision_context_ctor(
            (PictureDecisionContext_t**)&encHandlePtr->pictureDecisionContextPtr,
            encHandlePtr->pictureAnalysisResultsConsumerFifoPtrArray[0],
            encHandlePtr->pictureDecisionResultsProducerFifoPtrArray[0]);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Motion Analysis Context
    EB_MALLOC(EbPtr*, encHandlePtr->motionEstimationContextPtrArray, sizeof(EbPtr) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->motion_estimation_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->motion_estimation_process_init_count; ++processIndex) {

        return_error = MotionEstimationContextCtor(
            (MotionEstimationContext_t**)&encHandlePtr->motionEstimationContextPtrArray[processIndex],
            encHandlePtr->pictureDecisionResultsConsumerFifoPtrArray[processIndex],
            encHandlePtr->motionEstimationResultsProducerFifoPtrArray[processIndex]);

        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Initial Rate Control Context
    return_error = InitialRateControlContextCtor(
        (InitialRateControlContext_t**)&encHandlePtr->initialRateControlContextPtr,
        encHandlePtr->motionEstimationResultsConsumerFifoPtrArray[0],
        encHandlePtr->initialRateControlResultsProducerFifoPtrArray[0]);
    if (return_error == EB_ErrorInsufficientResources) {
        return EB_ErrorInsufficientResources;
    }

    // Source Based Operations Context
    EB_MALLOC(EbPtr*, encHandlePtr->sourceBasedOperationsContextPtrArray, sizeof(EbPtr) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->source_based_operations_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->source_based_operations_process_init_count; ++processIndex) {

        return_error = source_based_operations_context_ctor(
            (SourceBasedOperationsContext_t**)&encHandlePtr->sourceBasedOperationsContextPtrArray[processIndex],
            encHandlePtr->initialRateControlResultsConsumerFifoPtrArray[processIndex],
            encHandlePtr->pictureDemuxResultsProducerFifoPtrArray[processIndex],
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr);

        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Picture Manager Context
    return_error = picture_manager_context_ctor(
        (PictureManagerContext_t**)&encHandlePtr->pictureManagerContextPtr,
        encHandlePtr->pictureDemuxResultsConsumerFifoPtrArray[0],
        encHandlePtr->rateControlTasksProducerFifoPtrArray[RateControlPortLookup(RATE_CONTROL_INPUT_PORT_PICTURE_MANAGER, 0)],
        encHandlePtr->pictureControlSetPoolProducerFifoPtrDblArray[0]);//The Child PCS Pool here
    if (return_error == EB_ErrorInsufficientResources) {
        return EB_ErrorInsufficientResources;
    }
    // Rate Control Context
    return_error = rate_control_context_ctor(
        (RateControlContext_t**)&encHandlePtr->rateControlContextPtr,
        encHandlePtr->rateControlTasksConsumerFifoPtrArray[0],
        encHandlePtr->rateControlResultsProducerFifoPtrArray[0],
        encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->intra_period_length);
    if (return_error == EB_ErrorInsufficientResources) {
        return EB_ErrorInsufficientResources;
    }


    // Mode Decision Configuration Contexts
    {
        // Mode Decision Configuration Contexts
        EB_MALLOC(EbPtr*, encHandlePtr->modeDecisionConfigurationContextPtrArray, sizeof(EbPtr) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->mode_decision_configuration_process_init_count, EB_N_PTR);

        for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->mode_decision_configuration_process_init_count; ++processIndex) {
            return_error = ModeDecisionConfigurationContextCtor(
                (ModeDecisionConfigurationContext_t**)&encHandlePtr->modeDecisionConfigurationContextPtrArray[processIndex],
                encHandlePtr->rateControlResultsConsumerFifoPtrArray[processIndex],

                encHandlePtr->encDecTasksProducerFifoPtrArray[EncDecPortLookup(ENCDEC_INPUT_PORT_MDC, processIndex)],
                ((encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_width + BLOCK_SIZE_64 - 1) / BLOCK_SIZE_64) *
                ((encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_height + BLOCK_SIZE_64 - 1) / BLOCK_SIZE_64));


            if (return_error == EB_ErrorInsufficientResources) {
                return EB_ErrorInsufficientResources;
            }
        }
    }

    max_picture_width = 0;
    for (instanceIndex = 0; instanceIndex < encHandlePtr->encodeInstanceTotalCount; ++instanceIndex) {
        if (max_picture_width < encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_width) {
            max_picture_width = encHandlePtr->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_input_luma_width;
        }
    }

    // EncDec Contexts
    EB_MALLOC(EbPtr*, encHandlePtr->encDecContextPtrArray, sizeof(EbPtr) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->enc_dec_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->enc_dec_process_init_count; ++processIndex) {
        return_error = enc_dec_context_ctor(
            (EncDecContext_t**)&encHandlePtr->encDecContextPtrArray[processIndex],
            encHandlePtr->encDecTasksConsumerFifoPtrArray[processIndex],
            encHandlePtr->encDecResultsProducerFifoPtrArray[processIndex],
            encHandlePtr->encDecTasksProducerFifoPtrArray[EncDecPortLookup(ENCDEC_INPUT_PORT_ENCDEC, processIndex)],
            encHandlePtr->pictureDemuxResultsProducerFifoPtrArray[
                encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->source_based_operations_process_init_count+
                //1 +
                    processIndex], // Add port lookup logic here JMJ
            is16bit,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_width,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_height
        );

        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

#if FILT_PROC
    // Dlf Contexts
    EB_MALLOC(EbPtr*, encHandlePtr->dlfContextPtrArray, sizeof(EbPtr) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->dlf_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->dlf_process_init_count; ++processIndex) {
        return_error = dlf_context_ctor(
            (DlfContext_t**)&encHandlePtr->dlfContextPtrArray[processIndex],
            encHandlePtr->encDecResultsConsumerFifoPtrArray[processIndex],
            encHandlePtr->dlfResultsProducerFifoPtrArray[processIndex],             //output to EC
            is16bit,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_width,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_height
        );

        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    //CDEF Contexts
    EB_MALLOC(EbPtr*, encHandlePtr->cdefContextPtrArray, sizeof(EbPtr) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->cdef_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->cdef_process_init_count; ++processIndex) {
        return_error = cdef_context_ctor(
            (CdefContext_t**)&encHandlePtr->cdefContextPtrArray[processIndex],
            encHandlePtr->dlfResultsConsumerFifoPtrArray[processIndex],
            encHandlePtr->cdefResultsProducerFifoPtrArray[processIndex],  
            is16bit,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_width,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_height
        );

        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }
    //Rest Contexts
    EB_MALLOC(EbPtr*, encHandlePtr->restContextPtrArray, sizeof(EbPtr) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->rest_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->rest_process_init_count; ++processIndex) {
        return_error = rest_context_ctor(
            (RestContext_t**)&encHandlePtr->restContextPtrArray[processIndex],
            encHandlePtr->cdefResultsConsumerFifoPtrArray[processIndex],
            encHandlePtr->restResultsProducerFifoPtrArray[processIndex],             
            encHandlePtr->pictureDemuxResultsProducerFifoPtrArray[ 
                /*encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->source_based_operations_process_init_count*/ 1+ processIndex],
            is16bit,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_width,
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->max_input_luma_height
        );

        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }
#endif

    // Entropy Coding Contexts
    EB_MALLOC(EbPtr*, encHandlePtr->entropyCodingContextPtrArray, sizeof(EbPtr) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->entropy_coding_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->entropy_coding_process_init_count; ++processIndex) {
        return_error = entropy_coding_context_ctor(
            (EntropyCodingContext_t**)&encHandlePtr->entropyCodingContextPtrArray[processIndex],
#if FILT_PROC
            encHandlePtr->restResultsConsumerFifoPtrArray[processIndex],
#else
            encHandlePtr->encDecResultsConsumerFifoPtrArray[processIndex],
#endif
            encHandlePtr->entropyCodingResultsProducerFifoPtrArray[processIndex],
            encHandlePtr->rateControlTasksProducerFifoPtrArray[RateControlPortLookup(RATE_CONTROL_INPUT_PORT_ENTROPY_CODING, processIndex)],
            is16bit);
        if (return_error == EB_ErrorInsufficientResources) {
            return EB_ErrorInsufficientResources;
        }
    }

    // Packetization Context
    return_error = packetization_context_ctor(
        (PacketizationContext_t**)&encHandlePtr->packetizationContextPtr,
        encHandlePtr->entropyCodingResultsConsumerFifoPtrArray[0],
        encHandlePtr->rateControlTasksProducerFifoPtrArray[RateControlPortLookup(RATE_CONTROL_INPUT_PORT_PACKETIZATION, 0)]);

    if (return_error == EB_ErrorInsufficientResources) {
        return EB_ErrorInsufficientResources;
    }
    /************************************
    * Thread Handles
    ************************************/
    EbSvtAv1EncConfiguration   *config_ptr = &encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config;

    EbSetThreadManagementParameters(config_ptr);

    // Resource Coordination
    EB_CREATETHREAD(EbHandle, encHandlePtr->resourceCoordinationThreadHandle, sizeof(EbHandle), EB_THREAD, resource_coordination_kernel, encHandlePtr->resourceCoordinationContextPtr);

    // Picture Analysis
    EB_MALLOC(EbHandle*, encHandlePtr->pictureAnalysisThreadHandleArray, sizeof(EbHandle) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->picture_analysis_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->picture_analysis_process_init_count; ++processIndex) {
        EB_CREATETHREAD(EbHandle, encHandlePtr->pictureAnalysisThreadHandleArray[processIndex], sizeof(EbHandle), EB_THREAD, picture_analysis_kernel, encHandlePtr->pictureAnalysisContextPtrArray[processIndex]);
    }

    // Picture Decision
    EB_CREATETHREAD(EbHandle, encHandlePtr->pictureDecisionThreadHandle, sizeof(EbHandle), EB_THREAD, picture_decision_kernel, encHandlePtr->pictureDecisionContextPtr);

    // Motion Estimation
    EB_MALLOC(EbHandle*, encHandlePtr->motionEstimationThreadHandleArray, sizeof(EbHandle) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->motion_estimation_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->motion_estimation_process_init_count; ++processIndex) {
        EB_CREATETHREAD(EbHandle, encHandlePtr->motionEstimationThreadHandleArray[processIndex], sizeof(EbHandle), EB_THREAD, MotionEstimationKernel, encHandlePtr->motionEstimationContextPtrArray[processIndex]);
    }

    // Initial Rate Control
    EB_CREATETHREAD(EbHandle, encHandlePtr->initialRateControlThreadHandle, sizeof(EbHandle), EB_THREAD, InitialRateControlKernel, encHandlePtr->initialRateControlContextPtr);

    // Source Based Oprations
    EB_MALLOC(EbHandle*, encHandlePtr->sourceBasedOperationsThreadHandleArray, sizeof(EbHandle) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->source_based_operations_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->source_based_operations_process_init_count; ++processIndex) {
        EB_CREATETHREAD(EbHandle, encHandlePtr->sourceBasedOperationsThreadHandleArray[processIndex], sizeof(EbHandle), EB_THREAD, source_based_operations_kernel, encHandlePtr->sourceBasedOperationsContextPtrArray[processIndex]);
    }

    // Picture Manager
    EB_CREATETHREAD(EbHandle, encHandlePtr->pictureManagerThreadHandle, sizeof(EbHandle), EB_THREAD, picture_manager_kernel, encHandlePtr->pictureManagerContextPtr);

    // Rate Control
    EB_CREATETHREAD(EbHandle, encHandlePtr->rateControlThreadHandle, sizeof(EbHandle), EB_THREAD, rate_control_kernel, encHandlePtr->rateControlContextPtr);

    // Mode Decision Configuration Process
    EB_MALLOC(EbHandle*, encHandlePtr->modeDecisionConfigurationThreadHandleArray, sizeof(EbHandle) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->mode_decision_configuration_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->mode_decision_configuration_process_init_count; ++processIndex) {
        EB_CREATETHREAD(EbHandle, encHandlePtr->modeDecisionConfigurationThreadHandleArray[processIndex], sizeof(EbHandle), EB_THREAD, ModeDecisionConfigurationKernel, encHandlePtr->modeDecisionConfigurationContextPtrArray[processIndex]);
    }

    // EncDec Process
    EB_MALLOC(EbHandle*, encHandlePtr->encDecThreadHandleArray, sizeof(EbHandle) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->enc_dec_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->enc_dec_process_init_count; ++processIndex) {
        EB_CREATETHREAD(EbHandle, encHandlePtr->encDecThreadHandleArray[processIndex], sizeof(EbHandle), EB_THREAD, EncDecKernel, encHandlePtr->encDecContextPtrArray[processIndex]);
    }

#if FILT_PROC
    // Dlf Process
    EB_MALLOC(EbHandle*, encHandlePtr->dlfThreadHandleArray, sizeof(EbHandle) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->dlf_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->dlf_process_init_count; ++processIndex) {
        EB_CREATETHREAD(EbHandle, encHandlePtr->dlfThreadHandleArray[processIndex], sizeof(EbHandle), EB_THREAD, dlf_kernel, encHandlePtr->dlfContextPtrArray[processIndex]);
    }


    // Cdef Process
    EB_MALLOC(EbHandle*, encHandlePtr->cdefThreadHandleArray, sizeof(EbHandle) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->cdef_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->cdef_process_init_count; ++processIndex) {
        EB_CREATETHREAD(EbHandle, encHandlePtr->cdefThreadHandleArray[processIndex], sizeof(EbHandle), EB_THREAD, cdef_kernel, encHandlePtr->cdefContextPtrArray[processIndex]);
    }

    // Rest Process
    EB_MALLOC(EbHandle*, encHandlePtr->restThreadHandleArray, sizeof(EbHandle) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->rest_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->rest_process_init_count; ++processIndex) {
        EB_CREATETHREAD(EbHandle, encHandlePtr->restThreadHandleArray[processIndex], sizeof(EbHandle), EB_THREAD, rest_kernel, encHandlePtr->restContextPtrArray[processIndex]);
    }
#endif
    // Entropy Coding Process
    EB_MALLOC(EbHandle*, encHandlePtr->entropyCodingThreadHandleArray, sizeof(EbHandle) * encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->entropy_coding_process_init_count, EB_N_PTR);

    for (processIndex = 0; processIndex < encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr->entropy_coding_process_init_count; ++processIndex) {
        EB_CREATETHREAD(EbHandle, encHandlePtr->entropyCodingThreadHandleArray[processIndex], sizeof(EbHandle), EB_THREAD, EntropyCodingKernel, encHandlePtr->entropyCodingContextPtrArray[processIndex]);
    }

    // Packetization
    EB_CREATETHREAD(EbHandle, encHandlePtr->packetizationThreadHandle, sizeof(EbHandle), EB_THREAD, PacketizationKernel, encHandlePtr->packetizationContextPtr);

    
#if DISPLAY_MEMORY
    EB_MEMORY();
#endif
    return return_error;
}

/**********************************
* DeInitialize Encoder Library
**********************************/
#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_deinit_encoder(EbComponentType *svt_enc_component)
{
    if(svt_enc_component == NULL)
        return EB_ErrorBadParameter;
    EbEncHandle_t *encHandlePtr = (EbEncHandle_t*)svt_enc_component->pComponentPrivate;
    EbErrorType return_error = EB_ErrorNone;
    int32_t              ptrIndex = 0;
    EbMemoryMapEntry*   memoryEntry = (EbMemoryMapEntry*)EB_NULL;

    if (encHandlePtr) {
        if (encHandlePtr->memory_map_index) {
            // Loop through the ptr table and free all malloc'd pointers per channel
            for (ptrIndex = (encHandlePtr->memory_map_index) - 1; ptrIndex >= 0; --ptrIndex) {
                memoryEntry = &encHandlePtr->memory_map[ptrIndex];
                switch (memoryEntry->ptrType) {
                case EB_N_PTR:
                    free(memoryEntry->ptr);
                    break;
                case EB_A_PTR:
#ifdef _WIN32
                    _aligned_free(memoryEntry->ptr);
#else
                    free(memoryEntry->ptr);
#endif
                    break;
                case EB_SEMAPHORE:
                    eb_destroy_semaphore(memoryEntry->ptr);
                    break;
                case EB_THREAD:
                    eb_destroy_thread(memoryEntry->ptr);
                    break;
                case EB_MUTEX:
                    eb_destroy_mutex(memoryEntry->ptr);
                    break;
                default:
                    return_error = EB_ErrorMax;
                    break;
                }
            }
            if (encHandlePtr->memory_map != (EbMemoryMapEntry*)NULL) {
                free(encHandlePtr->memory_map);
            }

        }
    }
    return return_error;
}

EbErrorType eb_svt_enc_init_parameter(
    EbSvtAv1EncConfiguration * config_ptr);

EbErrorType init_svt_av1_encoder_handle(
    EbComponentType * hComponent);
/**********************************
* GetHandle
**********************************/
#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_init_handle(
    EbComponentType** p_handle,               // Function to be called in the future for manipulating the component
    void*              p_app_data,
    EbSvtAv1EncConfiguration  *config_ptr)              // pointer passed back to the client during callbacks

{
    EbErrorType           return_error = EB_ErrorNone;
    if(p_handle == NULL)
         return EB_ErrorBadParameter;

    *p_handle = (EbComponentType*)malloc(sizeof(EbComponentType));
    if (*p_handle != (EbComponentType*)NULL) {

        // Init Component OS objects (threads, semaphores, etc.)
        // also links the various Component control functions
        return_error = init_svt_av1_encoder_handle(*p_handle);

        if (return_error == EB_ErrorNone) {
            ((EbComponentType*)(*p_handle))->pApplicationPrivate = p_app_data;

        }
        else if (return_error == EB_ErrorInsufficientResources) {
            eb_deinit_encoder((EbComponentType*)NULL);
            *p_handle = (EbComponentType*)NULL;
        }
        else {
            return_error = EB_ErrorInvalidComponent;
        }
    }
    else {
        //SVT_LOG("Error: Component Struct Malloc Failed\n");
        return_error = EB_ErrorInsufficientResources;
    }
    return_error = eb_svt_enc_init_parameter(config_ptr);

    return return_error;
}

/**********************************
* Encoder Componenet DeInit
**********************************/
EbErrorType eb_h265_enc_component_de_init(EbComponentType  *svt_enc_component)
{
    EbErrorType       return_error = EB_ErrorNone;

    if (svt_enc_component->pComponentPrivate) {
        free((EbEncHandle_t *)svt_enc_component->pComponentPrivate);
    }
    else {
        return_error = EB_ErrorUndefined;
    }

    return return_error;
}

/**********************************
* eb_deinit_handle
**********************************/
#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_deinit_handle(
    EbComponentType  *svt_enc_component)
{
    EbErrorType return_error = EB_ErrorNone;

    if (svt_enc_component) {
        return_error = eb_h265_enc_component_de_init(svt_enc_component);

        free(svt_enc_component);
    }
    else {
        return_error = EB_ErrorInvalidComponent;
    }

    return return_error;
}

// Sets the default intra period the closest possible to 1 second without breaking the minigop
static int32_t compute_default_intra_period(
    SequenceControlSet_t       *sequence_control_set_ptr){

    int32_t intra_period               = 0;
    EbSvtAv1EncConfiguration   *config = &sequence_control_set_ptr->static_config;
    int32_t fps                        = config->frame_rate < 1000 ? 
                                            config->frame_rate : 
                                            config->frame_rate >> 16;
    int32_t mini_gop_size              = (1 << (config->hierarchical_levels));
    int32_t min_ip                     = ((int)((fps) / mini_gop_size)*(mini_gop_size));
    int32_t max_ip                     = ((int)((fps + mini_gop_size) / mini_gop_size)*(mini_gop_size));

    intra_period = (ABS((fps - max_ip)) > ABS((fps - min_ip))) ? min_ip : max_ip;

    if (config->intra_refresh_type == 1)
        intra_period -= 1;

    return intra_period;
}

// Set configurations for the hardcoded parameters
void SetDefaultConfigurationParameters(
    SequenceControlSet_t       *sequence_control_set_ptr)
{

    // LCU Definitions
    sequence_control_set_ptr->sb_sz = MAX_SB_SIZE;
    sequence_control_set_ptr->max_sb_depth = (uint8_t)EB_MAX_LCU_DEPTH;

    // No Cropping Window
    sequence_control_set_ptr->conformance_window_flag = 0;
    sequence_control_set_ptr->cropping_left_offset = 0;
    sequence_control_set_ptr->cropping_right_offset = 0;
    sequence_control_set_ptr->cropping_top_offset = 0;
    sequence_control_set_ptr->cropping_bottom_offset = 0;

    return;
}

static uint32_t compute_default_look_ahead(
    EbSvtAv1EncConfiguration*   config){

    int32_t lad = 0;
    if (config->rate_control_mode == 0)
        lad = (2 << config->hierarchical_levels)+1;
    else
        lad = config->intra_period_length;

    return lad;
}

// Only use the maximum look ahead needed if 
static uint32_t cap_look_ahead_distance(
    EbSvtAv1EncConfiguration*   config){

    uint32_t lad = 0;

    if(config){
        uint32_t fps = config->frame_rate < 1000 ?
                      config->frame_rate : 
                      config->frame_rate >> 16;
        uint32_t max_cqp_lad = (2 << config->hierarchical_levels) + 1;
        uint32_t max_rc_lad  = fps << 1;
        lad = config->look_ahead_distance;
        if (config->rate_control_mode == 0 && lad > max_cqp_lad)
            lad = max_cqp_lad;
        else if (config->rate_control_mode != 0 && lad > max_rc_lad)
            lad = max_rc_lad;
    }

    lad = lad > MAX_LAD ? MAX_LAD: lad; // clip to max allowed lad

    return lad;
}

void SetParamBasedOnInput(
    SequenceControlSet_t       *sequence_control_set_ptr){

    sequence_control_set_ptr->general_frame_only_constraint_flag = 0;
    sequence_control_set_ptr->general_progressive_source_flag = 1;
    sequence_control_set_ptr->general_interlaced_source_flag = 0;

    // Update picture width, and picture height
    if (sequence_control_set_ptr->max_input_luma_width % MIN_BLOCK_SIZE) {

        sequence_control_set_ptr->max_input_pad_right = MIN_BLOCK_SIZE - (sequence_control_set_ptr->max_input_luma_width % MIN_BLOCK_SIZE);
        sequence_control_set_ptr->max_input_luma_width = sequence_control_set_ptr->max_input_luma_width + sequence_control_set_ptr->max_input_pad_right;
    }
    else {

        sequence_control_set_ptr->max_input_pad_right = 0;
    }
    if (sequence_control_set_ptr->max_input_luma_height % MIN_BLOCK_SIZE) {

        sequence_control_set_ptr->max_input_pad_bottom = MIN_BLOCK_SIZE - (sequence_control_set_ptr->max_input_luma_height % MIN_BLOCK_SIZE);
        sequence_control_set_ptr->max_input_luma_height = sequence_control_set_ptr->max_input_luma_height + sequence_control_set_ptr->max_input_pad_bottom;
    }
    else {
        sequence_control_set_ptr->max_input_pad_bottom = 0;
    }

    sequence_control_set_ptr->max_input_chroma_width = sequence_control_set_ptr->max_input_luma_width >> 1;
    sequence_control_set_ptr->max_input_chroma_height = sequence_control_set_ptr->max_input_luma_height >> 1;


    // Configure the padding
    sequence_control_set_ptr->left_padding  = BLOCK_SIZE_64 + 4;
    sequence_control_set_ptr->top_padding = BLOCK_SIZE_64 + 4;
    sequence_control_set_ptr->right_padding = BLOCK_SIZE_64 + 4;
    sequence_control_set_ptr->bot_padding = BLOCK_SIZE_64 + 4;

    sequence_control_set_ptr->chroma_width = sequence_control_set_ptr->max_input_luma_width >> 1;
    sequence_control_set_ptr->chroma_height = sequence_control_set_ptr->max_input_luma_height >> 1;
    sequence_control_set_ptr->luma_width = sequence_control_set_ptr->max_input_luma_width;
    sequence_control_set_ptr->luma_height = sequence_control_set_ptr->max_input_luma_height;
    sequence_control_set_ptr->static_config.source_width = sequence_control_set_ptr->max_input_luma_width;
    sequence_control_set_ptr->static_config.source_height = sequence_control_set_ptr->max_input_luma_height;

    derive_input_resolution(
        sequence_control_set_ptr,
        sequence_control_set_ptr->luma_width*sequence_control_set_ptr->luma_height);
 #if DISABLE_128_SB_FOR_SUB_720
    sequence_control_set_ptr->static_config.super_block_size       = (sequence_control_set_ptr->static_config.enc_mode <= ENC_M2 && sequence_control_set_ptr->input_resolution >= INPUT_SIZE_1080i_RANGE) ? 128 : 64;
#endif
}

void CopyApiFromApp(
    SequenceControlSet_t       *sequence_control_set_ptr,
    EbSvtAv1EncConfiguration   *pComponentParameterStructure) {

    uint32_t                  hmeRegionIndex = 0;

    sequence_control_set_ptr->max_input_luma_width = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->source_width;
    sequence_control_set_ptr->max_input_luma_height = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->source_height;
    sequence_control_set_ptr->frame_rate = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->frame_rate;

    sequence_control_set_ptr->general_frame_only_constraint_flag = 0;
    sequence_control_set_ptr->general_progressive_source_flag = 1;
    sequence_control_set_ptr->general_interlaced_source_flag = 0;

    // SB Definitions
#if !DISABLE_128_SB_FOR_SUB_720
#if DISABLE_128X128_SB
    sequence_control_set_ptr->static_config.super_block_size = 64;
#else
    sequence_control_set_ptr->static_config.super_block_size       = (pComponentParameterStructure->enc_mode == ENC_M0) ? 128 : 64;
#endif
#endif
    sequence_control_set_ptr->static_config.pred_structure = 2; // Hardcoded(Cleanup)
    sequence_control_set_ptr->static_config.enable_qp_scaling_flag = 1;

    sequence_control_set_ptr->max_cu_size = (uint8_t)64;
    sequence_control_set_ptr->min_cu_size = (uint8_t)8;
    sequence_control_set_ptr->max_intra_size = (uint8_t)32;
    sequence_control_set_ptr->min_intra_size = (uint8_t)8;
    sequence_control_set_ptr->intra4x4_flag = 1;
    sequence_control_set_ptr->max_ref_count = 1;

    // Cropping Definitions - Hardcoded(CleanUp)
    sequence_control_set_ptr->cropping_left_offset = -1;
    sequence_control_set_ptr->cropping_right_offset = -1;
    sequence_control_set_ptr->cropping_top_offset = -1;
    sequence_control_set_ptr->cropping_bottom_offset = -1;

    // Padding Offsets
    sequence_control_set_ptr->sb_sz = (uint8_t)((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->sb_sz;
    sequence_control_set_ptr->max_sb_depth = (uint8_t)((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->partition_depth;

    if (sequence_control_set_ptr->cropping_left_offset == -1 &&
        sequence_control_set_ptr->cropping_right_offset == -1 &&
        sequence_control_set_ptr->cropping_top_offset == -1 &&
        sequence_control_set_ptr->cropping_bottom_offset == -1) {

        sequence_control_set_ptr->conformance_window_flag = 0;
    }
    else {
        sequence_control_set_ptr->conformance_window_flag = 1;
    }
    if (sequence_control_set_ptr->cropping_left_offset == -1)
        sequence_control_set_ptr->cropping_left_offset = 0;
    if (sequence_control_set_ptr->cropping_right_offset == -1)
        sequence_control_set_ptr->cropping_right_offset = 0;
    if (sequence_control_set_ptr->cropping_top_offset == -1)
        sequence_control_set_ptr->cropping_top_offset = 0;
    if (sequence_control_set_ptr->cropping_bottom_offset == -1)
        sequence_control_set_ptr->cropping_bottom_offset = 0;

    // Coding Structure
    sequence_control_set_ptr->static_config.intra_period_length = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->intra_period_length;
    sequence_control_set_ptr->static_config.intra_refresh_type = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->intra_refresh_type;
    sequence_control_set_ptr->static_config.base_layer_switch_mode = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->base_layer_switch_mode;
    sequence_control_set_ptr->static_config.hierarchical_levels = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->hierarchical_levels;
    sequence_control_set_ptr->static_config.enc_mode = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->enc_mode;
    sequence_control_set_ptr->intra_period_length = sequence_control_set_ptr->static_config.intra_period_length;
    sequence_control_set_ptr->intra_refresh_type = sequence_control_set_ptr->static_config.intra_refresh_type;
    sequence_control_set_ptr->max_temporal_layers = sequence_control_set_ptr->static_config.hierarchical_levels;
    sequence_control_set_ptr->static_config.use_qp_file = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->use_qp_file;

#if SHUT_FILTERING
    sequence_control_set_ptr->static_config.disable_dlf_flag = 1;//
#else
    // Deblock Filter
    sequence_control_set_ptr->static_config.disable_dlf_flag = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->disable_dlf_flag;
#endif

    // Local Warped Motion
    sequence_control_set_ptr->static_config.enable_warped_motion = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->enable_warped_motion;

    // ME Tools
    sequence_control_set_ptr->static_config.use_default_me_hme = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->use_default_me_hme;
    sequence_control_set_ptr->static_config.enable_hme_flag = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->enable_hme_flag;
    sequence_control_set_ptr->static_config.enable_hme_level0_flag = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->enable_hme_level0_flag;
    sequence_control_set_ptr->static_config.enable_hme_level1_flag = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->enable_hme_level1_flag;
    sequence_control_set_ptr->static_config.enable_hme_level2_flag = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->enable_hme_level2_flag;
    sequence_control_set_ptr->static_config.search_area_width = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->search_area_width;
    sequence_control_set_ptr->static_config.search_area_height = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->search_area_height;
    sequence_control_set_ptr->static_config.number_hme_search_region_in_width = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->number_hme_search_region_in_width;
    sequence_control_set_ptr->static_config.number_hme_search_region_in_height = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->number_hme_search_region_in_height;
    sequence_control_set_ptr->static_config.hme_level0_total_search_area_width = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->hme_level0_total_search_area_width;
    sequence_control_set_ptr->static_config.hme_level0_total_search_area_height = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->hme_level0_total_search_area_height;
    sequence_control_set_ptr->static_config.ext_block_flag = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->ext_block_flag;
    sequence_control_set_ptr->static_config.in_loop_me_flag = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->in_loop_me_flag;

    for (hmeRegionIndex = 0; hmeRegionIndex < sequence_control_set_ptr->static_config.number_hme_search_region_in_width; ++hmeRegionIndex) {
        sequence_control_set_ptr->static_config.hme_level0_search_area_in_width_array[hmeRegionIndex] = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->hme_level0_search_area_in_width_array[hmeRegionIndex];
        sequence_control_set_ptr->static_config.hme_level1_search_area_in_width_array[hmeRegionIndex] = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->hme_level1_search_area_in_width_array[hmeRegionIndex];
        sequence_control_set_ptr->static_config.hme_level2_search_area_in_width_array[hmeRegionIndex] = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->hme_level2_search_area_in_width_array[hmeRegionIndex];
    }

    for (hmeRegionIndex = 0; hmeRegionIndex < sequence_control_set_ptr->static_config.number_hme_search_region_in_height; ++hmeRegionIndex) {
        sequence_control_set_ptr->static_config.hme_level0_search_area_in_height_array[hmeRegionIndex] = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->hme_level0_search_area_in_height_array[hmeRegionIndex];
        sequence_control_set_ptr->static_config.hme_level1_search_area_in_height_array[hmeRegionIndex] = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->hme_level1_search_area_in_height_array[hmeRegionIndex];
        sequence_control_set_ptr->static_config.hme_level2_search_area_in_height_array[hmeRegionIndex] = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->hme_level2_search_area_in_height_array[hmeRegionIndex];
    }

    //Denoise - Hardcoded(CleanUp)
    sequence_control_set_ptr->static_config.enable_denoise_flag = 0;

    //Film Grain
    sequence_control_set_ptr->static_config.film_grain_denoise_strength = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->film_grain_denoise_strength;
    sequence_control_set_ptr->film_grain_denoise_strength = sequence_control_set_ptr->static_config.film_grain_denoise_strength;

    // MD Parameters
    sequence_control_set_ptr->static_config.constrained_intra = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->constrained_intra;

    // Adaptive Loop Filter
#if TILES
    sequence_control_set_ptr->static_config.tile_rows = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->tile_rows;
    sequence_control_set_ptr->static_config.tile_columns = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->tile_columns;
#endif

    // Rate Control
    sequence_control_set_ptr->static_config.scene_change_detection = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->scene_change_detection;
    sequence_control_set_ptr->static_config.rate_control_mode = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->rate_control_mode;
    sequence_control_set_ptr->static_config.look_ahead_distance = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->look_ahead_distance;
    sequence_control_set_ptr->static_config.frames_to_be_encoded = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->frames_to_be_encoded;
    sequence_control_set_ptr->static_config.frame_rate = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->frame_rate;
    sequence_control_set_ptr->static_config.frame_rate_denominator = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->frame_rate_denominator;
    sequence_control_set_ptr->static_config.frame_rate_numerator = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->frame_rate_numerator;

    sequence_control_set_ptr->static_config.target_bit_rate = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->target_bit_rate;

    sequence_control_set_ptr->static_config.max_qp_allowed = (sequence_control_set_ptr->static_config.rate_control_mode) ?
        ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->max_qp_allowed :
        63;

    sequence_control_set_ptr->static_config.min_qp_allowed = (sequence_control_set_ptr->static_config.rate_control_mode) ?
        ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->min_qp_allowed :
        0;

    // Misc
    sequence_control_set_ptr->static_config.encoder_bit_depth = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->encoder_bit_depth;
    sequence_control_set_ptr->static_config.ten_bit_format = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->ten_bit_format;
    sequence_control_set_ptr->static_config.compressed_ten_bit_format = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->compressed_ten_bit_format;

    // Thresholds
    sequence_control_set_ptr->static_config.improve_sharpness = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->improve_sharpness;
    sequence_control_set_ptr->static_config.high_dynamic_range_input = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->high_dynamic_range_input;

    // Annex A parameters
    sequence_control_set_ptr->static_config.profile = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->profile;
    sequence_control_set_ptr->static_config.tier = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->tier;
    sequence_control_set_ptr->static_config.level = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->level;
    sequence_control_set_ptr->static_config.stat_report = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->stat_report;

    sequence_control_set_ptr->static_config.injector_frame_rate = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->injector_frame_rate;
    sequence_control_set_ptr->static_config.speed_control_flag = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->speed_control_flag;

    // Buffers - Hardcoded(Cleanup)
    sequence_control_set_ptr->static_config.asm_type = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->asm_type;

    sequence_control_set_ptr->static_config.channel_id = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->channel_id;
    sequence_control_set_ptr->static_config.active_channel_count = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->active_channel_count;
    sequence_control_set_ptr->static_config.logical_processors = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->logical_processors;
    sequence_control_set_ptr->static_config.target_socket = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->target_socket;
    sequence_control_set_ptr->qp = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->qp;
    sequence_control_set_ptr->static_config.recon_enabled = ((EbSvtAv1EncConfiguration*)pComponentParameterStructure)->recon_enabled;

    // Extract frame rate from Numerator and Denominator if not 0
    if (sequence_control_set_ptr->static_config.frame_rate_numerator != 0 && sequence_control_set_ptr->static_config.frame_rate_denominator != 0) {
        sequence_control_set_ptr->frame_rate = sequence_control_set_ptr->static_config.frame_rate = (((sequence_control_set_ptr->static_config.frame_rate_numerator << 8) / (sequence_control_set_ptr->static_config.frame_rate_denominator)) << 8);
    }

    // Get Default Intra Period if not specified
    if (sequence_control_set_ptr->static_config.intra_period_length == -2) {
        sequence_control_set_ptr->intra_period_length = sequence_control_set_ptr->static_config.intra_period_length = compute_default_intra_period(sequence_control_set_ptr);
    }

    if (sequence_control_set_ptr->static_config.look_ahead_distance == (uint32_t)~0)
        sequence_control_set_ptr->static_config.look_ahead_distance = compute_default_look_ahead(&sequence_control_set_ptr->static_config);
    else
        sequence_control_set_ptr->static_config.look_ahead_distance = cap_look_ahead_distance(&sequence_control_set_ptr->static_config);

    return;
}

/******************************************
* Verify Settings
******************************************/
#define PowerOfTwoCheck(x) (((x) != 0) && (((x) & (~(x) + 1)) == (x)))

static int VerifyHmeDimention(unsigned int index, unsigned int HmeLevel0SearchAreaInWidth, uint32_t NumberHmeSearchRegionInWidth[EB_HME_SEARCH_AREA_ROW_MAX_COUNT], unsigned int numberHmeSearchRegionInWidth)
{
    int           return_error = 0;
    uint32_t        i;
    uint32_t        total_search_width = 0;

    for (i = 0; i < numberHmeSearchRegionInWidth; i++) {
        total_search_width += NumberHmeSearchRegionInWidth[i];
    }
    if ((total_search_width) != (HmeLevel0SearchAreaInWidth)) {
        SVT_LOG("Error Instance %u: Invalid  HME Total Search Area. \n", index);
        return_error = -1;
        return return_error;
    }

    return return_error;
}
static int VerifyHmeDimentionL1L2(unsigned int index, uint32_t NumberHmeSearchRegionInWidth[EB_HME_SEARCH_AREA_ROW_MAX_COUNT], unsigned int numberHmeSearchRegionInWidth)
{
    int             return_error = 0;
    uint32_t        i;
    uint32_t        total_search_width = 0;

    for (i = 0; i < numberHmeSearchRegionInWidth; i++) {
        total_search_width += NumberHmeSearchRegionInWidth[i];
    }
    if ((total_search_width > 256) || (total_search_width == 0)) {
        SVT_LOG("Error Instance %u: Invalid  HME Total Search Area. Must be [1 - 256].\n", index);
        return_error = -1;
        return return_error;
    }

    return return_error;
}

static EbErrorType VerifySettings(
    SequenceControlSet_t       *sequence_control_set_ptr)
{
    EbErrorType return_error = EB_ErrorNone;
    EbSvtAv1EncConfiguration *config = &sequence_control_set_ptr->static_config;
    unsigned int channelNumber = config->channel_id;
    if (config->enc_mode > MAX_ENC_PRESET) {
        SVT_LOG("Error instance %u: EncoderMode must be in the range of [0-%d]\n", channelNumber + 1, MAX_ENC_PRESET);
        return_error = EB_ErrorBadParameter;
    }

    if (config->ext_block_flag > 1) {
        SVT_LOG("Error instance %u: ExtBlockFlag must be [0-1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->in_loop_me_flag > 1) {
        SVT_LOG("Error instance %u: InLoopMeFlag must be [0-1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (sequence_control_set_ptr->max_input_luma_width < 64) {
        SVT_LOG("Error instance %u: Source Width must be at least 64\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
    if (sequence_control_set_ptr->max_input_luma_height < 64) {
        SVT_LOG("Error instance %u: Source Width must be at least 64\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->pred_structure != 2) {
        SVT_LOG("Error instance %u: Pred Structure must be [2]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->base_layer_switch_mode == 1 && config->pred_structure != 2) {
        SVT_LOG("Error Instance %u: Base Layer Switch Mode 1 only when Prediction Structure is Random Access\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
    if (sequence_control_set_ptr->max_input_luma_width % 8 && sequence_control_set_ptr->static_config.compressed_ten_bit_format == 1) {
        SVT_LOG("Error Instance %u: Only multiple of 8 width is supported for compressed 10-bit inputs \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (sequence_control_set_ptr->max_input_luma_width % 2) {
        SVT_LOG("Error Instance %u: Source Width must be even for YUV_420 colorspace\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (sequence_control_set_ptr->max_input_luma_height % 2) {
        SVT_LOG("Error Instance %u: Source Height must be even for YUV_420 colorspace\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if ((sequence_control_set_ptr->max_input_luma_height % 8) || (sequence_control_set_ptr->max_input_luma_width % 8)) {
        SVT_LOG("Error Instance %u: Only multiple of 8 resolutions are supported \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (sequence_control_set_ptr->max_input_luma_width > 4096) {
        SVT_LOG("Error instance %u: Source Width must be less than 4096\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (sequence_control_set_ptr->max_input_luma_height > 2160) {
        SVT_LOG("Error instance %u: Source Height must be less than 2160\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->qp > MAX_QP_VALUE) {
        SVT_LOG("Error instance %u: QP must be [0 - %d]\n", channelNumber + 1, MAX_QP_VALUE);
        return_error = EB_ErrorBadParameter;
    }
#if NEW_PRED_STRUCT
    if (config->hierarchical_levels != 3 && config->hierarchical_levels != 4) {
        SVT_LOG("Error instance %u: Hierarchical Levels supported [3-4]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
#else
    if (config->hierarchical_levels != 3 ) {
        SVT_LOG("Error instance %u: Hierarchical Levels supported [3]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
#endif
    if (config->intra_period_length < -2 || config->intra_period_length > 255) {
        SVT_LOG("Error Instance %u: The intra period must be [-2 - 255] \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->intra_refresh_type > 2 || config->intra_refresh_type < 1) {
        SVT_LOG("Error Instance %u: Invalid intra Refresh Type [1-2]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
    if (config->base_layer_switch_mode > 1) {
        SVT_LOG("Error Instance %u: Invalid Base Layer Switch Mode [0-1] \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->disable_dlf_flag > 1) {
        SVT_LOG("Error Instance %u: Invalid LoopFilterDisable. LoopFilterDisable must be [0 - 1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->use_default_me_hme > 1) {
        SVT_LOG("Error Instance %u: invalid use_default_me_hme. use_default_me_hme must be [0 - 1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
    if (config->enable_hme_flag > 1) {
        SVT_LOG("Error Instance %u: invalid HME. HME must be [0 - 1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->enable_hme_level0_flag > 1) {
        SVT_LOG("Error Instance %u: invalid enable HMELevel0. HMELevel0 must be [0 - 1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->enable_hme_level1_flag > 1) {
        SVT_LOG("Error Instance %u: invalid enable HMELevel1. HMELevel1 must be [0 - 1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->enable_hme_level2_flag > 1) {
        SVT_LOG("Error Instance %u: invalid enable HMELevel2. HMELevel2 must be [0 - 1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if ((config->search_area_width > 256) || (config->search_area_width == 0)) {
        SVT_LOG("Error Instance %u: Invalid SearchAreaWidth. SearchAreaWidth must be [1 - 256]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if ((config->search_area_height > 256) || (config->search_area_height == 0)) {
        SVT_LOG("Error Instance %u: Invalid SearchAreaHeight. SearchAreaHeight must be [1 - 256]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->enable_hme_flag) {

        if ((config->number_hme_search_region_in_width > (uint32_t)EB_HME_SEARCH_AREA_COLUMN_MAX_COUNT) || (config->number_hme_search_region_in_width == 0)) {
            SVT_LOG("Error Instance %u: Invalid number_hme_search_region_in_width. number_hme_search_region_in_width must be [1 - %d]\n", channelNumber + 1, EB_HME_SEARCH_AREA_COLUMN_MAX_COUNT);
            return_error = EB_ErrorBadParameter;
        }

        if ((config->number_hme_search_region_in_height > (uint32_t)EB_HME_SEARCH_AREA_ROW_MAX_COUNT) || (config->number_hme_search_region_in_height == 0)) {
            SVT_LOG("Error Instance %u: Invalid NumberHmeSearchRegionInHeight. NumberHmeSearchRegionInHeight must be [1 - %d]\n", channelNumber + 1, EB_HME_SEARCH_AREA_ROW_MAX_COUNT);
            return_error = EB_ErrorBadParameter;
        }

        if ((config->hme_level0_total_search_area_height > 256) || (config->hme_level0_total_search_area_height == 0)) {
            SVT_LOG("Error Instance %u: Invalid hmeLevel0TotalSearchAreaHeight. hmeLevel0TotalSearchAreaHeight must be [1 - 256]\n", channelNumber + 1);
            return_error = EB_ErrorBadParameter;
        }
        if ((config->hme_level0_total_search_area_width > 256) || (config->hme_level0_total_search_area_width == 0)) {
            SVT_LOG("Error Instance %u: Invalid hmeLevel0TotalSearchAreaWidth. hmeLevel0TotalSearchAreaWidth must be [1 - 256]\n", channelNumber + 1);
            return_error = EB_ErrorBadParameter;
        }
        if (VerifyHmeDimention(channelNumber + 1, config->hme_level0_total_search_area_height, config->hme_level0_search_area_in_height_array, config->number_hme_search_region_in_height)) {
            return_error = EB_ErrorBadParameter;
        }

        if (VerifyHmeDimention(channelNumber + 1, config->hme_level0_total_search_area_width, config->hme_level0_search_area_in_width_array, config->number_hme_search_region_in_width)) {
            return_error = EB_ErrorBadParameter;
        }
        if (VerifyHmeDimentionL1L2(channelNumber + 1, config->hme_level1_search_area_in_width_array, config->number_hme_search_region_in_width)) {
            return_error = EB_ErrorBadParameter;
        }
        if (VerifyHmeDimentionL1L2(channelNumber + 1, config->hme_level1_search_area_in_height_array, config->number_hme_search_region_in_width)) {
            return_error = EB_ErrorBadParameter;
        }
        if (VerifyHmeDimentionL1L2(channelNumber + 1, config->hme_level2_search_area_in_width_array, config->number_hme_search_region_in_width)) {
            return_error = EB_ErrorBadParameter;
        }
        if (VerifyHmeDimentionL1L2(channelNumber + 1, config->hme_level2_search_area_in_height_array, config->number_hme_search_region_in_width)) {
            return_error = EB_ErrorBadParameter;
        }
    }

    if (config->profile > 2) {
        SVT_LOG("Error Instance %u: The maximum allowed profile value is 2 \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    // Check if the current input video is conformant with the Level constraint
    if (config->frame_rate > (240 << 16)) {
        SVT_LOG("Error Instance %u: The maximum allowed frame rate is 240 fps\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
    // Check that the frameRate is non-zero
    if (config->frame_rate <= 0) {
        SVT_LOG("Error Instance %u: The frame rate should be greater than 0 fps \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
    if (config->intra_period_length < -2 || config->intra_period_length > 255) {
        SVT_LOG("Error Instance %u: The intra period must be [-2 - 255] \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
    if (config->constrained_intra > 1) {
        SVT_LOG("Error Instance %u: The constrained intra must be [0 - 1] \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
    if (config->rate_control_mode > 1) {
        SVT_LOG("Error Instance %u: The rate control mode must be [0 - 1] \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->look_ahead_distance > MAX_LAD && config->look_ahead_distance != (uint32_t)~0) {
        SVT_LOG("Error Instance %u: The lookahead distance must be [0 - %d] \n", channelNumber + 1, MAX_LAD);

        return_error = EB_ErrorBadParameter;
    }
#if TILES
    if (config->tile_rows < 0 || config->tile_columns < 0 || config->tile_rows > 6 || config->tile_columns > 6) {
        SVT_LOG("Error Instance %u: Log2Tile rows/cols must be [0 - 6] \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
#endif
    if (config->scene_change_detection > 1) {
        SVT_LOG("Error Instance %u: The scene change detection must be [0 - 1] \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
    if (config->max_qp_allowed > MAX_QP_VALUE) {
        SVT_LOG("Error instance %u: MaxQpAllowed must be [0 - %d]\n", channelNumber + 1, MAX_QP_VALUE);
        return_error = EB_ErrorBadParameter;
    }
    else if (config->min_qp_allowed >= MAX_QP_VALUE) {
        SVT_LOG("Error instance %u: MinQpAllowed must be [0 - %d]\n", channelNumber + 1, MAX_QP_VALUE-1);
        return_error = EB_ErrorBadParameter;
    }
    else if ((config->min_qp_allowed) > (config->max_qp_allowed)) {
        SVT_LOG("Error Instance %u:  MinQpAllowed must be smaller than MaxQpAllowed\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->improve_sharpness > 1) {
        SVT_LOG("Error instance %u : Invalid ImproveSharpness. ImproveSharpness must be [0 - 1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->high_dynamic_range_input > 1) {
        SVT_LOG("Error instance %u : Invalid HighDynamicRangeInput. HighDynamicRangeInput must be [0 - 1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if ((config->encoder_bit_depth != 8) &&
        (config->encoder_bit_depth != 10)
        ) {
        SVT_LOG("Error instance %u: Encoder Bit Depth shall be only 8 or 10 \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }
    // Check if the EncoderBitDepth is conformant with the Profile constraint
    if (config->profile == 1 && config->encoder_bit_depth == 10) {
        SVT_LOG("Error instance %u: The encoder bit depth shall be equal to 8 for Main Profile\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->compressed_ten_bit_format !=0)
    {
        SVT_LOG("Error instance %u: Compressed ten bit format is not supported in this version \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->speed_control_flag > 1) {
        SVT_LOG("Error Instance %u: Invalid Speed Control flag [0 - 1]\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (((int32_t)(config->asm_type) < -1) || ((int32_t)(config->asm_type) != 1)) {
       // SVT_LOG("Error Instance %u: Invalid asm type value [0: C Only, 1: Auto] .\n", channelNumber + 1);
        SVT_LOG("Error Instance %u: Asm 0 is not supported in this build .\n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    if (config->target_socket != -1 && config->target_socket != 0 && config->target_socket != 1) {
        SVT_LOG("Error instance %u: Invalid TargetSocket. TargetSocket must be [-1 - 1] \n", channelNumber + 1);
        return_error = EB_ErrorBadParameter;
    }

    return return_error;
}

/**********************************
Set Default Library Params
**********************************/
EbErrorType eb_svt_enc_init_parameter(
    EbSvtAv1EncConfiguration * config_ptr)
{
    EbErrorType                  return_error = EB_ErrorNone;

    if (!config_ptr) {
        SVT_LOG("The EbSvtAv1EncConfiguration structure is empty! \n");
        return EB_ErrorBadParameter;
    }

    config_ptr->frame_rate = 30 << 16;
    config_ptr->frame_rate_numerator = 0;
    config_ptr->frame_rate_denominator = 0;
    config_ptr->encoder_bit_depth = 8;
    config_ptr->ten_bit_format = 0;
    config_ptr->compressed_ten_bit_format = 0;
    config_ptr->source_width = 0;
    config_ptr->source_height = 0;
    config_ptr->frames_to_be_encoded = 0; 
    config_ptr->stat_report = 1;
#if TILES
    config_ptr->tile_rows = 0;
    config_ptr->tile_columns = 0;
#endif
    config_ptr->qp = 50;
    config_ptr->use_qp_file = EB_FALSE;
    config_ptr->scene_change_detection = 0;
    config_ptr->rate_control_mode = 0;
    config_ptr->look_ahead_distance = (uint32_t)~0;
    config_ptr->target_bit_rate = 7000000;
    config_ptr->max_qp_allowed = 63;
    config_ptr->min_qp_allowed = 0;
    config_ptr->base_layer_switch_mode = 0;
    config_ptr->enc_mode = MAX_ENC_PRESET;
    config_ptr->intra_period_length = 30;
    config_ptr->intra_refresh_type = 1;
#if NEW_PRED_STRUCT
    config_ptr->hierarchical_levels = 4;
#else
    config_ptr->hierarchical_levels = 3;
#endif    
    config_ptr->pred_structure = EB_PRED_RANDOM_ACCESS;
    config_ptr->disable_dlf_flag = EB_FALSE;
    config_ptr->enable_warped_motion = EB_FALSE;
    config_ptr->in_loop_me_flag = EB_TRUE;
    config_ptr->ext_block_flag = EB_FALSE;
    config_ptr->use_default_me_hme = EB_TRUE;
    config_ptr->enable_hme_flag = EB_TRUE;
    config_ptr->enable_hme_level0_flag = EB_TRUE;
    config_ptr->enable_hme_level1_flag = EB_FALSE;
    config_ptr->enable_hme_level2_flag = EB_FALSE;
    config_ptr->search_area_width = 16;
    config_ptr->search_area_height = 7;
    config_ptr->number_hme_search_region_in_width = 2;
    config_ptr->number_hme_search_region_in_height = 2;
    config_ptr->hme_level0_total_search_area_width = 64;
    config_ptr->hme_level0_total_search_area_height = 25;
    config_ptr->hme_level0_search_area_in_width_array[0] = 32;
    config_ptr->hme_level0_search_area_in_width_array[1] = 32;
    config_ptr->hme_level0_search_area_in_height_array[0] = 12;
    config_ptr->hme_level0_search_area_in_height_array[1] = 13;
    config_ptr->hme_level1_search_area_in_width_array[0] = 1;
    config_ptr->hme_level1_search_area_in_width_array[1] = 1;
    config_ptr->hme_level1_search_area_in_height_array[0] = 1;
    config_ptr->hme_level1_search_area_in_height_array[1] = 1;
    config_ptr->hme_level2_search_area_in_width_array[0] = 1;
    config_ptr->hme_level2_search_area_in_width_array[1] = 1;
    config_ptr->hme_level2_search_area_in_height_array[0] = 1;
    config_ptr->hme_level2_search_area_in_height_array[1] = 1;
    config_ptr->constrained_intra = EB_FALSE;
    config_ptr->improve_sharpness = EB_FALSE;

    // Bitstream options
    //config_ptr->codeVpsSpsPps = 0;
    //config_ptr->codeEosNal = 0;

    config_ptr->high_dynamic_range_input = 0;

    // Annex A parameters
    config_ptr->profile = 0;
    config_ptr->tier = 0;
    config_ptr->level = 0;

    // Latency
    config_ptr->injector_frame_rate = 60 << 16;
    config_ptr->speed_control_flag = 0;
#if    DISABLE_128X128_SB
    config_ptr->super_block_size = 64;
#else
    config_ptr->super_block_size = 128;
#endif
    config_ptr->sb_sz = 64;
    config_ptr->partition_depth = (uint8_t)EB_MAX_LCU_DEPTH;
    //config_ptr->latencyMode = 0;
    config_ptr->speed_control_flag = 0;
    config_ptr->film_grain_denoise_strength = 0;

    // ASM Type
    config_ptr->asm_type = 1;

    // Channel info
    config_ptr->logical_processors = 0;
    config_ptr->target_socket = -1;
    config_ptr->channel_id = 0;
    config_ptr->active_channel_count = 1;

    // Debug info
    config_ptr->recon_enabled = 0;

    return return_error;
}
//#define DEBUG_BUFFERS
static void PrintLibParams(
    SequenceControlSet_t* scs) {

    EbSvtAv1EncConfiguration*   config = &scs->static_config;

    SVT_LOG("------------------------------------------- ");
    if (config->profile == 0)
        SVT_LOG("\nSVT [config]: Main Profile\t");
    else
        SVT_LOG("\nSVT [config]: Main10 Profile\t");

    if (config->tier != 0 && config->level != 0)
        SVT_LOG("Tier %d\tLevel %.1f\t", config->tier, (float)(config->level / 10));
    else {
        if (config->tier == 0)
            SVT_LOG("Tier (auto)\t");
        else
            SVT_LOG("Tier %d\t", config->tier);

        if (config->level == 0)
            SVT_LOG("Level (auto)\t");
        else
            SVT_LOG("Level %.1f\t", (float)(config->level / 10));
    }
    SVT_LOG("\nSVT [config]: EncoderMode \t\t\t\t\t\t\t: %d ", config->enc_mode);
    SVT_LOG("\nSVT [config]: EncoderBitDepth / CompressedTenBitFormat\t\t\t\t: %d / %d ", config->encoder_bit_depth, config->compressed_ten_bit_format);
    SVT_LOG("\nSVT [config]: SourceWidth / SourceHeight\t\t\t\t\t: %d / %d ", config->source_width, config->source_height);
    if (config->frame_rate_denominator != 0 && config->frame_rate_numerator != 0)
        SVT_LOG("\nSVT [config]: Fps_Numerator / Fps_Denominator / Gop Size / IntraRefreshType \t: %d / %d / %d / %d", config->frame_rate_numerator > (1 << 16) ? config->frame_rate_numerator >> 16 : config->frame_rate_numerator,
            config->frame_rate_denominator > (1 << 16) ? config->frame_rate_denominator >> 16 : config->frame_rate_denominator,
            config->intra_period_length + 1,
            config->intra_refresh_type);
    else
        SVT_LOG("\nSVT [config]: FrameRate / Gop Size\t\t\t\t\t\t: %d / %d ", config->frame_rate > 1000 ? config->frame_rate >> 16 : config->frame_rate, config->intra_period_length + 1);
    SVT_LOG("\nSVT [config]: HierarchicalLevels / BaseLayerSwitchMode / PredStructure\t\t: %d / %d / %d ", config->hierarchical_levels, config->base_layer_switch_mode, config->pred_structure);
    if (config->rate_control_mode == 1)
        SVT_LOG("\nSVT [config]: RCMode / TargetBitrate / LookaheadDistance / SceneChange\t\t: VBR / %d / %d / %d ", config->target_bit_rate, config->look_ahead_distance, config->scene_change_detection);
    else
        SVT_LOG("\nSVT [config]: BRC Mode / QP  / LookaheadDistance / SceneChange\t\t\t: CQP / %d / %d / %d ", scs->qp, config->look_ahead_distance, config->scene_change_detection);
#ifdef DEBUG_BUFFERS
    SVT_LOG("\nSVT [config]: INPUT / OUTPUT \t\t\t\t\t\t\t: %d / %d", scs->input_buffer_fifo_init_count, scs->output_stream_buffer_fifo_init_count);
    SVT_LOG("\nSVT [config]: CPCS / PAREF / REF \t\t\t\t\t\t: %d / %d / %d", scs->picture_control_set_pool_init_count_child, scs->pa_reference_picture_buffer_init_count, scs->reference_picture_buffer_init_count);
    SVT_LOG("\nSVT [config]: ME_SEG_W0 / ME_SEG_W1 / ME_SEG_W2 / ME_SEG_W3 \t\t\t: %d / %d / %d / %d ",
        scs->me_segment_column_count_array[0],
        scs->me_segment_column_count_array[1],
        scs->me_segment_column_count_array[2],
        scs->me_segment_column_count_array[3]);
    SVT_LOG("\nSVT [config]: ME_SEG_H0 / ME_SEG_H1 / ME_SEG_H2 / ME_SEG_H3 \t\t\t: %d / %d / %d / %d ",
        scs->me_segment_row_count_array[0],
        scs->me_segment_row_count_array[1],
        scs->me_segment_row_count_array[2],
        scs->me_segment_row_count_array[3]);
    SVT_LOG("\nSVT [config]: ME_SEG_W0 / ME_SEG_W1 / ME_SEG_W2 / ME_SEG_W3 \t\t\t: %d / %d / %d / %d ",
        scs->enc_dec_segment_col_count_array[0],
        scs->enc_dec_segment_col_count_array[1],
        scs->enc_dec_segment_col_count_array[2],
        scs->enc_dec_segment_col_count_array[3]);
    SVT_LOG("\nSVT [config]: ME_SEG_H0 / ME_SEG_H1 / ME_SEG_H2 / ME_SEG_H3 \t\t\t: %d / %d / %d / %d ",
        scs->enc_dec_segment_row_count_array[0],
        scs->enc_dec_segment_row_count_array[1],
        scs->enc_dec_segment_row_count_array[2],
        scs->enc_dec_segment_row_count_array[3]);
    SVT_LOG("\nSVT [config]: PA_P / ME_P / SBO_P / MDC_P / ED_P / EC_P \t\t\t: %d / %d / %d / %d / %d / %d ",
        scs->picture_analysis_process_init_count,
        scs->motion_estimation_process_init_count,
        scs->source_based_operations_process_init_count,
        scs->mode_decision_configuration_process_init_count,
        scs->enc_dec_process_init_count,
        scs->entropy_coding_process_init_count);
    SVT_LOG("\nSVT [config]: DLF_P / CDEF_P / REST_P \t\t\t\t\t\t: %d / %d / %d",
        scs->dlf_process_init_count,
        scs->cdef_process_init_count,
        scs->rest_process_init_count);
#endif
    SVT_LOG("\n------------------------------------------- ");
    SVT_LOG("\n");

    fflush(stdout);
}
/**********************************

* Set Parameter
**********************************/
#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_svt_enc_set_parameter(
    EbComponentType              *svt_enc_component,
    EbSvtAv1EncConfiguration     *pComponentParameterStructure)
{
    if(svt_enc_component == NULL)
        return EB_ErrorBadParameter;

    EbErrorType           return_error  = EB_ErrorNone;
    EbEncHandle_t        *pEncCompData  = (EbEncHandle_t*)svt_enc_component->pComponentPrivate;
    uint32_t              instanceIndex = 0;

    // Acquire Config Mutex
    eb_block_on_mutex(pEncCompData->sequence_control_set_instance_array[instanceIndex]->config_mutex);

    SetDefaultConfigurationParameters(
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr);

    CopyApiFromApp(
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr,
        (EbSvtAv1EncConfiguration*)pComponentParameterStructure);

    return_error = (EbErrorType)VerifySettings(
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr);

    if (return_error == EB_ErrorBadParameter) {
        return EB_ErrorBadParameter;
    }

    SetParamBasedOnInput(
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr);

    // Initialize the Prediction Structure Group
    return_error = (EbErrorType)PredictionStructureGroupCtor(
        &pEncCompData->sequence_control_set_instance_array[instanceIndex]->encode_context_ptr->prediction_structure_group_ptr,
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.base_layer_switch_mode);

    if (return_error == EB_ErrorInsufficientResources) {
        return EB_ErrorInsufficientResources;
    }

    // Set the Prediction Structure
    pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->pred_struct_ptr = GetPredictionStructure(
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->encode_context_ptr->prediction_structure_group_ptr,
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->static_config.pred_structure,
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_ref_count,
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr->max_temporal_layers);

    return_error = LoadDefaultBufferConfigurationSettings(
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr);

    PrintLibParams(
        pEncCompData->sequence_control_set_instance_array[instanceIndex]->sequence_control_set_ptr);

    // Release Config Mutex
    eb_release_mutex(pEncCompData->sequence_control_set_instance_array[instanceIndex]->config_mutex);

    return return_error;
}
#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_svt_enc_stream_header(
    EbComponentType           *svt_enc_component,
    EbBufferHeaderType        **output_stream_ptr){

    EbErrorType             return_error = EB_ErrorNone;
    UNUSED(svt_enc_component);
    UNUSED(output_stream_ptr);
    return return_error;
}
//
#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_svt_enc_eos_nal(
    EbComponentType           *svt_enc_component,
    EbBufferHeaderType       **output_stream_ptr
)
{
    EbErrorType           return_error = EB_ErrorNone;
    UNUSED(svt_enc_component);
    UNUSED(output_stream_ptr);
    return return_error;
}

/***********************************************
**** Copy the input buffer from the
**** sample application to the library buffers
************************************************/
static EbErrorType CopyFrameBuffer(
    SequenceControlSet_t            *sequence_control_set_ptr,
    uint8_t                          *dst,
    uint8_t                          *src)
{
    EbSvtAv1EncConfiguration          *config = &sequence_control_set_ptr->static_config;
    EbErrorType                      return_error = EB_ErrorNone;

    EbPictureBufferDesc_t           *input_picture_ptr = (EbPictureBufferDesc_t*)dst;
    EbSvtIOFormat                   *inputPtr = (EbSvtIOFormat*)src;
    uint16_t                         inputRowIndex;
    EbBool                           is16BitInput = (EbBool)(config->encoder_bit_depth > EB_8BIT);

    // Need to include for Interlacing on the fly with pictureScanType = 1

    if (!is16BitInput) {

        uint32_t     lumaBufferOffset = (input_picture_ptr->stride_y*sequence_control_set_ptr->top_padding + sequence_control_set_ptr->left_padding) << is16BitInput;
        uint32_t     chromaBufferOffset = (input_picture_ptr->strideCr*(sequence_control_set_ptr->top_padding >> 1) + (sequence_control_set_ptr->left_padding >> 1)) << is16BitInput;
        uint16_t     lumaStride = input_picture_ptr->stride_y << is16BitInput;
        uint16_t     chromaStride = input_picture_ptr->strideCb << is16BitInput;
        uint16_t     lumaWidth = (uint16_t)(input_picture_ptr->width - sequence_control_set_ptr->max_input_pad_right) << is16BitInput;
        uint16_t     chromaWidth = (lumaWidth >> 1) << is16BitInput;
        uint16_t     lumaHeight = (uint16_t)(input_picture_ptr->height - sequence_control_set_ptr->max_input_pad_bottom);

        uint16_t     sourceLumaStride = (uint16_t)(inputPtr->yStride);
        uint16_t     sourceCrStride = (uint16_t)(inputPtr->crStride);
        uint16_t     sourceCbStride = (uint16_t)(inputPtr->cbStride);

        //uint16_t     lumaHeight  = input_picture_ptr->maxHeight;
        // Y
        for (inputRowIndex = 0; inputRowIndex < lumaHeight; inputRowIndex++) {

            EB_MEMCPY((input_picture_ptr->buffer_y + lumaBufferOffset + lumaStride * inputRowIndex),
                (inputPtr->luma + sourceLumaStride * inputRowIndex),
                lumaWidth);
        }

        // U
        for (inputRowIndex = 0; inputRowIndex < lumaHeight >> 1; inputRowIndex++) {
            EB_MEMCPY((input_picture_ptr->bufferCb + chromaBufferOffset + chromaStride * inputRowIndex),
                (inputPtr->cb + (sourceCbStride*inputRowIndex)),
                chromaWidth);
        }

        // V
        for (inputRowIndex = 0; inputRowIndex < lumaHeight >> 1; inputRowIndex++) {
            EB_MEMCPY((input_picture_ptr->bufferCr + chromaBufferOffset + chromaStride * inputRowIndex),
                (inputPtr->cr + (sourceCrStride*inputRowIndex)),
                chromaWidth);
        }

    }
    else if (is16BitInput && config->compressed_ten_bit_format == 1)
    {
        {
            uint32_t  lumaBufferOffset = (input_picture_ptr->stride_y*sequence_control_set_ptr->top_padding + sequence_control_set_ptr->left_padding);
            uint32_t  chromaBufferOffset = (input_picture_ptr->strideCr*(sequence_control_set_ptr->top_padding >> 1) + (sequence_control_set_ptr->left_padding >> 1));
            uint16_t  lumaStride = input_picture_ptr->stride_y;
            uint16_t  chromaStride = input_picture_ptr->strideCb;
            uint16_t  lumaWidth = (uint16_t)(input_picture_ptr->width - sequence_control_set_ptr->max_input_pad_right);
            uint16_t  chromaWidth = (lumaWidth >> 1);
            uint16_t  lumaHeight = (uint16_t)(input_picture_ptr->height - sequence_control_set_ptr->max_input_pad_bottom);

            uint16_t  sourceLumaStride = (uint16_t)(inputPtr->yStride);
            uint16_t  sourceCrStride = (uint16_t)(inputPtr->crStride);
            uint16_t  sourceCbStride = (uint16_t)(inputPtr->cbStride);

            // Y 8bit
            for (inputRowIndex = 0; inputRowIndex < lumaHeight; inputRowIndex++) {

                EB_MEMCPY((input_picture_ptr->buffer_y + lumaBufferOffset + lumaStride * inputRowIndex),
                    (inputPtr->luma + sourceLumaStride * inputRowIndex),
                    lumaWidth);
            }

            // U 8bit
            for (inputRowIndex = 0; inputRowIndex < lumaHeight >> 1; inputRowIndex++) {

                EB_MEMCPY((input_picture_ptr->bufferCb + chromaBufferOffset + chromaStride * inputRowIndex),
                    (inputPtr->cb + (sourceCbStride*inputRowIndex)),
                    chromaWidth);
            }

            // V 8bit
            for (inputRowIndex = 0; inputRowIndex < lumaHeight >> 1; inputRowIndex++) {

                EB_MEMCPY((input_picture_ptr->bufferCr + chromaBufferOffset + chromaStride * inputRowIndex),
                    (inputPtr->cr + (sourceCrStride*inputRowIndex)),
                    chromaWidth);
            }

            //efficient copy - final
            //compressed 2Bit in 1D format
            {
                uint16_t luma2BitWidth = sequence_control_set_ptr->max_input_luma_width / 4;
                uint16_t lumaHeight = sequence_control_set_ptr->max_input_luma_height;

                uint16_t sourceLuma2BitStride = sourceLumaStride / 4;
                uint16_t sourceChroma2BitStride = sourceLuma2BitStride >> 1;

                for (inputRowIndex = 0; inputRowIndex < lumaHeight; inputRowIndex++) {
                    EB_MEMCPY(input_picture_ptr->bufferBitIncY + luma2BitWidth * inputRowIndex, inputPtr->lumaExt + sourceLuma2BitStride * inputRowIndex, luma2BitWidth);
                }
                for (inputRowIndex = 0; inputRowIndex < lumaHeight >> 1; inputRowIndex++) {
                    EB_MEMCPY(input_picture_ptr->bufferBitIncCb + (luma2BitWidth >> 1)*inputRowIndex, inputPtr->cbExt + sourceChroma2BitStride * inputRowIndex, luma2BitWidth >> 1);
                }
                for (inputRowIndex = 0; inputRowIndex < lumaHeight >> 1; inputRowIndex++) {
                    EB_MEMCPY(input_picture_ptr->bufferBitIncCr + (luma2BitWidth >> 1)*inputRowIndex, inputPtr->crExt + sourceChroma2BitStride * inputRowIndex, luma2BitWidth >> 1);
                }
            }

        }

    }
    else { // 10bit packed 

        uint32_t lumaOffset = 0, chromaOffset = 0;
        uint32_t lumaBufferOffset = (input_picture_ptr->stride_y*sequence_control_set_ptr->top_padding + sequence_control_set_ptr->left_padding);
        uint32_t chromaBufferOffset = (input_picture_ptr->strideCr*(sequence_control_set_ptr->top_padding >> 1) + (sequence_control_set_ptr->left_padding >> 1));
        uint16_t lumaWidth = (uint16_t)(input_picture_ptr->width - sequence_control_set_ptr->max_input_pad_right);
        uint16_t chromaWidth = (lumaWidth >> 1);
        uint16_t lumaHeight = (uint16_t)(input_picture_ptr->height - sequence_control_set_ptr->max_input_pad_bottom);

        uint16_t sourceLumaStride = (uint16_t)(inputPtr->yStride);
        uint16_t sourceCrStride = (uint16_t)(inputPtr->crStride);
        uint16_t sourceCbStride = (uint16_t)(inputPtr->cbStride);

        un_pack2d(
            (uint16_t*)(inputPtr->luma + lumaOffset),
            sourceLumaStride,
            input_picture_ptr->buffer_y + lumaBufferOffset,
            input_picture_ptr->stride_y,
            input_picture_ptr->bufferBitIncY + lumaBufferOffset,
            input_picture_ptr->strideBitIncY,
            lumaWidth,
            lumaHeight,
            config->asm_type);

        un_pack2d(
            (uint16_t*)(inputPtr->cb + chromaOffset),
            sourceCbStride,
            input_picture_ptr->bufferCb + chromaBufferOffset,
            input_picture_ptr->strideCb,
            input_picture_ptr->bufferBitIncCb + chromaBufferOffset,
            input_picture_ptr->strideBitIncCb,
            chromaWidth,
            (lumaHeight >> 1),
            config->asm_type);

        un_pack2d(
            (uint16_t*)(inputPtr->cr + chromaOffset),
            sourceCrStride,
            input_picture_ptr->bufferCr + chromaBufferOffset,
            input_picture_ptr->strideCr,
            input_picture_ptr->bufferBitIncCr + chromaBufferOffset,
            input_picture_ptr->strideBitIncCr,
            chromaWidth,
            (lumaHeight >> 1),
            config->asm_type);
    }
    return return_error;
}
static void CopyInputBuffer(
    SequenceControlSet_t*    sequenceControlSet,
    EbBufferHeaderType*     dst,
    EbBufferHeaderType*     src
)
{
    // Copy the higher level structure
    dst->n_alloc_len = src->n_alloc_len;
    dst->n_filled_len = src->n_filled_len;
    dst->flags = src->flags;
    dst->pts = src->pts;
    dst->n_tick_count = src->n_tick_count;
    dst->size = src->size;
    dst->qp = src->qp;
    dst->pic_type = src->pic_type;

    // Copy the picture buffer
    if (src->p_buffer != NULL)
        CopyFrameBuffer(sequenceControlSet, dst->p_buffer, src->p_buffer);
}

/**********************************
* Empty This Buffer
**********************************/
#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_svt_enc_send_picture(
    EbComponentType      *svt_enc_component,
    EbBufferHeaderType   *p_buffer)
{
    EbEncHandle_t          *encHandlePtr = (EbEncHandle_t*)svt_enc_component->pComponentPrivate;
    EbObjectWrapper_t      *ebWrapperPtr;

    // Take the buffer and put it into our internal queue structure
    eb_get_empty_object(
        encHandlePtr->input_buffer_producer_fifo_ptr_array[0],
        &ebWrapperPtr);

    if (p_buffer != NULL) {
        CopyInputBuffer(
            encHandlePtr->sequence_control_set_instance_array[0]->sequence_control_set_ptr,
            (EbBufferHeaderType*)ebWrapperPtr->object_ptr,
            p_buffer);
    }

    eb_post_full_object(ebWrapperPtr);

    return EB_ErrorNone;
}
static void CopyOutputReconBuffer(
    EbBufferHeaderType   *dst,
    EbBufferHeaderType   *src
)
{
    // copy output bitstream fileds
    dst->size = src->size;
    dst->n_alloc_len = src->n_alloc_len;
    dst->n_filled_len = src->n_filled_len;
    dst->p_app_private = src->p_app_private;
    dst->n_tick_count = src->n_tick_count;
    dst->pts = src->pts;
    dst->dts = src->dts;
    dst->flags = src->flags;
    dst->pic_type = src->pic_type;
    if (src->p_buffer)
        EB_MEMCPY(dst->p_buffer, src->p_buffer, src->n_filled_len);

    return;
}

/**********************************
* eb_svt_get_packet sends out packet
**********************************/
#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_svt_get_packet(
    EbComponentType      *svt_enc_component,
    EbBufferHeaderType  **p_buffer,
    unsigned char          pic_send_done)
{
    EbErrorType             return_error = EB_ErrorNone;
    EbEncHandle_t          *pEncCompData = (EbEncHandle_t*)svt_enc_component->pComponentPrivate;
    EbObjectWrapper_t      *ebWrapperPtr = NULL;
    EbBufferHeaderType    *packet;
    if (pic_send_done)
        eb_get_full_object(
        (pEncCompData->output_stream_buffer_consumer_fifo_ptr_dbl_array[0])[0],
            &ebWrapperPtr);
    else
        eb_get_full_object_non_blocking(
        (pEncCompData->output_stream_buffer_consumer_fifo_ptr_dbl_array[0])[0],
            &ebWrapperPtr);

    if (ebWrapperPtr) {

        packet = (EbBufferHeaderType*)ebWrapperPtr->object_ptr;

        if (packet->flags != EB_BUFFERFLAG_EOS &&
            packet->flags != EB_BUFFERFLAG_SHOW_EXT &&
            packet->flags != EB_BUFFERFLAG_HAS_TD &&
            packet->flags != (EB_BUFFERFLAG_SHOW_EXT | EB_BUFFERFLAG_EOS) &&
            packet->flags != (EB_BUFFERFLAG_SHOW_EXT | EB_BUFFERFLAG_HAS_TD) &&
            packet->flags != (EB_BUFFERFLAG_SHOW_EXT | EB_BUFFERFLAG_HAS_TD | EB_BUFFERFLAG_EOS) &&
            packet->flags != (EB_BUFFERFLAG_HAS_TD | EB_BUFFERFLAG_EOS) &&
            packet->flags != 0) {
            return_error = EB_ErrorMax;
        }

        // return the output stream buffer
        *p_buffer = packet;

        // save the wrapper pointer for the release
        (*p_buffer)->wrapper_ptr = (void*)ebWrapperPtr;
    }
    else {
        return_error = EB_NoErrorEmptyQueue;
    }

    return return_error;
}

#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API void eb_svt_release_out_buffer(
    EbBufferHeaderType  **p_buffer)
{
    if (p_buffer&&(*p_buffer)->wrapper_ptr)
        // Release out put buffer back into the pool
        eb_release_object((EbObjectWrapper_t  *)(*p_buffer)->wrapper_ptr);
    return;
}

/**********************************
* Fill This Buffer
**********************************/
#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_svt_get_recon(
    EbComponentType      *svt_enc_component,
    EbBufferHeaderType   *p_buffer)
{
    EbErrorType           return_error = EB_ErrorNone;
    EbEncHandle_t          *pEncCompData = (EbEncHandle_t*)svt_enc_component->pComponentPrivate;
    EbObjectWrapper_t      *ebWrapperPtr = NULL;

    if (pEncCompData->sequence_control_set_instance_array[0]->sequence_control_set_ptr->static_config.recon_enabled) {

        eb_get_full_object_non_blocking(
            (pEncCompData->output_recon_buffer_consumer_fifo_ptr_dbl_array[0])[0],
            &ebWrapperPtr);

        if (ebWrapperPtr) {
            EbBufferHeaderType* objPtr = (EbBufferHeaderType*)ebWrapperPtr->object_ptr;
            CopyOutputReconBuffer(
                p_buffer,
                objPtr);

            if (p_buffer->flags != EB_BUFFERFLAG_EOS && p_buffer->flags != 0) {
                return_error = EB_ErrorMax;
            }
            eb_release_object((EbObjectWrapper_t  *)ebWrapperPtr);
        }
        else {
            return_error = EB_NoErrorEmptyQueue;
        }
    }
    else {
        // recon is not enabled
        return_error = EB_ErrorMax;
    }

    return return_error;
}

/**********************************
* Encoder Error Handling
**********************************/
void lib_svt_encoder_send_error_exit(
    EbPtr                    hComponent,
    uint32_t                 errorCode)
{
    EbComponentType      *svt_enc_component = (EbComponentType*)hComponent;
    EbEncHandle_t          *pEncCompData = (EbEncHandle_t*)svt_enc_component->pComponentPrivate;
    EbObjectWrapper_t      *ebWrapperPtr = NULL;
    EbBufferHeaderType    *outputPacket;

    eb_get_empty_object(
        (pEncCompData->output_stream_buffer_producer_fifo_ptr_dbl_array[0])[0],
        &ebWrapperPtr);

    outputPacket            = (EbBufferHeaderType*)ebWrapperPtr->object_ptr;

    outputPacket->size     = 0;
    outputPacket->flags    = errorCode;
    outputPacket->p_buffer   = NULL;

    eb_post_full_object(ebWrapperPtr);
}
/**********************************
* Encoder Handle Initialization
**********************************/
EbErrorType init_svt_av1_encoder_handle(
    EbComponentType * hComponent)
{
    EbErrorType       return_error = EB_ErrorNone;
    EbComponentType  *svt_enc_component = (EbComponentType*)hComponent;

    printf("SVT [version]:\tSVT-AV1 Encoder Lib v%d.%d.%d\n", SVT_VERSION_MAJOR, SVT_VERSION_MINOR, SVT_VERSION_PATCHLEVEL);
#if ( defined( _MSC_VER ) && (_MSC_VER < 1910) ) 
    printf("SVT [build]  : Visual Studio 2013");
#elif ( defined( _MSC_VER ) && (_MSC_VER >= 1910) ) 
    printf("SVT [build]  :\tVisual Studio 2017");
#elif defined(__GNUC__)
    printf("SVT [build]  :\tGCC %d.%d.%d\t", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    printf("SVT [build]  :\tunknown compiler");
#endif
    printf(" %u bit\n", (unsigned) sizeof(void*) * 8);
    printf("LIB Build date: %s %s\n", __DATE__, __TIME__);
    printf("-------------------------------------------\n");

    SwitchToRealTime();

    // Set Component Size & Version
    svt_enc_component->size = sizeof(EbComponentType);

    // Encoder Private Handle Ctor
    return_error = (EbErrorType)eb_enc_handle_ctor(
        (EbEncHandle_t**) &(svt_enc_component->pComponentPrivate),
        svt_enc_component);

    return return_error;
}
static EbErrorType allocate_frame_buffer(
    SequenceControlSet_t       *sequence_control_set_ptr,
    EbBufferHeaderType        *inputBuffer)
{
    EbErrorType   return_error = EB_ErrorNone;
    EbPictureBufferDescInitData_t input_picture_buffer_desc_init_data;
    EbSvtAv1EncConfiguration   * config = &sequence_control_set_ptr->static_config;
    uint8_t is16bit = config->encoder_bit_depth > 8 ? 1 : 0;
    // Init Picture Init data
    input_picture_buffer_desc_init_data.maxWidth = (uint16_t)sequence_control_set_ptr->max_input_luma_width;
    input_picture_buffer_desc_init_data.maxHeight = (uint16_t)sequence_control_set_ptr->max_input_luma_height;
    input_picture_buffer_desc_init_data.bit_depth = (EB_BITDEPTH)config->encoder_bit_depth;

    if (config->compressed_ten_bit_format == 1) {
        input_picture_buffer_desc_init_data.bufferEnableMask = 0;
    }
    else {
        input_picture_buffer_desc_init_data.bufferEnableMask = is16bit ? PICTURE_BUFFER_DESC_FULL_MASK : 0;
    }

    input_picture_buffer_desc_init_data.left_padding = sequence_control_set_ptr->left_padding;
    input_picture_buffer_desc_init_data.right_padding = sequence_control_set_ptr->right_padding;
    input_picture_buffer_desc_init_data.top_padding = sequence_control_set_ptr->top_padding;
    input_picture_buffer_desc_init_data.bot_padding = sequence_control_set_ptr->bot_padding;

    input_picture_buffer_desc_init_data.splitMode = is16bit ? EB_TRUE : EB_FALSE;

    input_picture_buffer_desc_init_data.bufferEnableMask = PICTURE_BUFFER_DESC_FULL_MASK;

    if (is16bit && config->compressed_ten_bit_format == 1) {
        input_picture_buffer_desc_init_data.splitMode = EB_FALSE;  //do special allocation for 2bit data down below.        
    }

    // Enhanced Picture Buffer
    return_error = eb_picture_buffer_desc_ctor(
        (EbPtr*) &(inputBuffer->p_buffer),
        (EbPtr)&input_picture_buffer_desc_init_data);

    if (return_error == EB_ErrorInsufficientResources) {
        return EB_ErrorInsufficientResources;
    }

    if (is16bit && config->compressed_ten_bit_format == 1) {
        //pack 4 2bit pixels into 1Byte
        EB_ALLIGN_MALLOC(uint8_t*, ((EbPictureBufferDesc_t*)(inputBuffer->p_buffer))->bufferBitIncY, sizeof(uint8_t) * (input_picture_buffer_desc_init_data.maxWidth / 4)*(input_picture_buffer_desc_init_data.maxHeight), EB_A_PTR);
        EB_ALLIGN_MALLOC(uint8_t*, ((EbPictureBufferDesc_t*)(inputBuffer->p_buffer))->bufferBitIncCb, sizeof(uint8_t) * (input_picture_buffer_desc_init_data.maxWidth / 8)*(input_picture_buffer_desc_init_data.maxHeight / 2), EB_A_PTR);
        EB_ALLIGN_MALLOC(uint8_t*, ((EbPictureBufferDesc_t*)(inputBuffer->p_buffer))->bufferBitIncCr, sizeof(uint8_t) * (input_picture_buffer_desc_init_data.maxWidth / 8)*(input_picture_buffer_desc_init_data.maxHeight / 2), EB_A_PTR);
    }

    return return_error;
}
/**************************************
* EbBufferHeaderType Constructor
**************************************/
EbErrorType EbInputBufferHeaderCtor(
    EbPtr *objectDblPtr,
    EbPtr  objectInitDataPtr)
{
    EbBufferHeaderType* inputBuffer;
    SequenceControlSet_t        *sequence_control_set_ptr = (SequenceControlSet_t*)objectInitDataPtr;
    EB_MALLOC(EbBufferHeaderType*, inputBuffer, sizeof(EbBufferHeaderType), EB_N_PTR);
    *objectDblPtr = (EbPtr)inputBuffer;
    // Initialize Header
    inputBuffer->size = sizeof(EbBufferHeaderType);

    allocate_frame_buffer(
        sequence_control_set_ptr,
        inputBuffer);

    inputBuffer->p_app_private = NULL;

    return EB_ErrorNone;
}

/**************************************
* EbBufferHeaderType Constructor
**************************************/
EbErrorType EbOutputBufferHeaderCtor(
    EbPtr *objectDblPtr,
    EbPtr objectInitDataPtr)
{
    EbSvtAv1EncConfiguration   * config = (EbSvtAv1EncConfiguration*)objectInitDataPtr;
    uint32_t nStride = (uint32_t)(EB_OUTPUTSTREAMBUFFERSIZE_MACRO(config->source_width * config->source_height));  //TBC
    EbBufferHeaderType* outBufPtr;

    EB_MALLOC(EbBufferHeaderType*, outBufPtr, sizeof(EbBufferHeaderType), EB_N_PTR);
    *objectDblPtr = (EbPtr)outBufPtr;

    // Initialize Header
    outBufPtr->size = sizeof(EbBufferHeaderType);

    EB_MALLOC(uint8_t*, outBufPtr->p_buffer, nStride, EB_N_PTR);

    outBufPtr->n_alloc_len = nStride;
    outBufPtr->p_app_private = NULL;

    (void)objectInitDataPtr;

    return EB_ErrorNone;
}

/**************************************
* EbBufferHeaderType Constructor
**************************************/
EbErrorType EbOutputReconBufferHeaderCtor(
    EbPtr *objectDblPtr,
    EbPtr  objectInitDataPtr)
{
    EbBufferHeaderType         *recon_buffer;
    SequenceControlSet_t        *sequence_control_set_ptr = (SequenceControlSet_t*)objectInitDataPtr;
    const uint32_t lumaSize =
        sequence_control_set_ptr->luma_width    *
        sequence_control_set_ptr->luma_height;
    // both u and v
    const uint32_t chromaSize = lumaSize >> 1;
    const uint32_t tenBit = (sequence_control_set_ptr->static_config.encoder_bit_depth > 8);
    const uint32_t frameSize = (lumaSize + chromaSize) << tenBit;

    EB_MALLOC(EbBufferHeaderType*, recon_buffer, sizeof(EbBufferHeaderType), EB_N_PTR);
    *objectDblPtr = (EbPtr)recon_buffer;

    // Initialize Header
    recon_buffer->size = sizeof(EbBufferHeaderType);

    // Assign the variables 
    EB_MALLOC(uint8_t*, recon_buffer->p_buffer, frameSize, EB_N_PTR);

    recon_buffer->n_alloc_len = frameSize;
    recon_buffer->p_app_private = NULL;

    return EB_ErrorNone;
}

/* SAFE STRING LIBRARY */

static constraint_handler_t str_handler = NULL;

void
invoke_safe_str_constraint_handler(const char *msg,
    void *ptr,
    errno_t error)
{
    if (NULL != str_handler) {
        str_handler(msg, ptr, error);
    }
    else {
        sl_default_handler(msg, ptr, error);
    }
}

void ignore_handler_s(const char *msg, void *ptr, errno_t error)
{
    (void)msg;
    (void)ptr;
    (void)error;
    sldebug_printf("IGNORE CONSTRAINT HANDLER: (%u) %s\n", error,
        (msg) ? msg : "Null message");
    return;
}
EXPORT_SYMBOL(ignore_handler_s)

errno_t
strncpy_ss(char *dest, rsize_t dmax, const char *src, rsize_t slen)
{
    rsize_t orig_dmax;
    char *orig_dest;
    const char *overlap_bumper;

    if (dest == NULL) {
        invoke_safe_str_constraint_handler((char*) "strncpy_ss: dest is null",
            NULL, ESNULLP);
        return RCNEGATE(ESNULLP);
    }

    if (dmax == 0) {
        invoke_safe_str_constraint_handler((char*) "strncpy_ss: dmax is 0",
            NULL, ESZEROL);
        return RCNEGATE(ESZEROL);
    }

    if (dmax > RSIZE_MAX_STR) {
        invoke_safe_str_constraint_handler((char*) "strncpy_ss: dmax exceeds max",
            NULL, ESLEMAX);
        return RCNEGATE(ESLEMAX);
    }

    /* hold base in case src was not copied */
    orig_dmax = dmax;
    orig_dest = dest;

    if (src == NULL) {
        handle_error(orig_dest, orig_dmax, (char*) "strncpy_ss: "
            "src is null",
            ESNULLP);
        return RCNEGATE(ESNULLP);
    }

    if (slen == 0) {
        handle_error(orig_dest, orig_dmax, (char*) "strncpy_ss: "
            "slen is zero",
            ESZEROL);
        return RCNEGATE(ESZEROL);
    }

    if (slen > RSIZE_MAX_STR) {
        handle_error(orig_dest, orig_dmax, (char*) "strncpy_ss: "
            "slen exceeds max",
            ESLEMAX);
        return RCNEGATE(ESLEMAX);
    }


    if (dest < src) {
        overlap_bumper = src;

        while (dmax > 0) {
            if (dest == overlap_bumper) {
                handle_error(orig_dest, orig_dmax, (char*) "strncpy_ss: "
                    "overlapping objects",
                    ESOVRLP);
                return RCNEGATE(ESOVRLP);
            }

            if (slen == 0) {
                /*
                * Copying truncated to slen chars.  Note that the TR says to
                * copy slen chars plus the null char.  We null the slack.
                */
                *dest = '\0';
                return RCNEGATE(EOK);
            }

            *dest = *src;
            if (*dest == '\0') {
                return RCNEGATE(EOK);
            }

            dmax--;
            slen--;
            dest++;
            src++;
        }

    }
    else {
        overlap_bumper = dest;

        while (dmax > 0) {
            if (src == overlap_bumper) {
                handle_error(orig_dest, orig_dmax, (char*) "strncpy_ss: "
                    "overlapping objects",
                    ESOVRLP);
                return RCNEGATE(ESOVRLP);
            }

            if (slen == 0) {
                /*
                * Copying truncated to slen chars.  Note that the TR says to
                * copy slen chars plus the null char.  We null the slack.
                */
                *dest = '\0';
                return RCNEGATE(EOK);
            }

            *dest = *src;
            if (*dest == '\0') {
                return RCNEGATE(EOK);
            }

            dmax--;
            slen--;
            dest++;
            src++;
        }
    }

    /*
    * the entire src was not copied, so zero the string
    */
    handle_error(orig_dest, orig_dmax, (char*) "strncpy_ss: not enough "
        "space for src",
        ESNOSPC);
    return RCNEGATE(ESNOSPC);
}
EXPORT_SYMBOL(strncpy_ss)

errno_t
strcpy_ss(char *dest, rsize_t dmax, const char *src)
{
    rsize_t orig_dmax;
    char *orig_dest;
    const char *overlap_bumper;

    if (dest == NULL) {
        invoke_safe_str_constraint_handler((char*) "strcpy_ss: dest is null",
            NULL, ESNULLP);
        return RCNEGATE(ESNULLP);
    }

    if (dmax == 0) {
        invoke_safe_str_constraint_handler((char*) "strcpy_ss: dmax is 0",
            NULL, ESZEROL);
        return RCNEGATE(ESZEROL);
    }

    if (dmax > RSIZE_MAX_STR) {
        invoke_safe_str_constraint_handler((char*) "strcpy_ss: dmax exceeds max",
            NULL, ESLEMAX);
        return RCNEGATE(ESLEMAX);
    }

    if (src == NULL) {
        *dest = '\0';
        invoke_safe_str_constraint_handler((char*) "strcpy_ss: src is null",
            NULL, ESNULLP);
        return RCNEGATE(ESNULLP);
    }

    if (dest == src) {
        return RCNEGATE(EOK);
    }

    /* hold base of dest in case src was not copied */
    orig_dmax = dmax;
    orig_dest = dest;

    if (dest < src) {
        overlap_bumper = src;

        while (dmax > 0) {
            if (dest == overlap_bumper) {
                handle_error(orig_dest, orig_dmax, (char*) "strcpy_ss: "
                    "overlapping objects",
                    ESOVRLP);
                return RCNEGATE(ESOVRLP);
            }

            *dest = *src;
            if (*dest == '\0') {
                return RCNEGATE(EOK);
            }

            dmax--;
            dest++;
            src++;
        }

    }
    else {
        overlap_bumper = dest;

        while (dmax > 0) {
            if (src == overlap_bumper) {
                handle_error(orig_dest, orig_dmax, (char*) "strcpy_ss: "
                    "overlapping objects",
                    ESOVRLP);
                return RCNEGATE(ESOVRLP);
            }

            *dest = *src;
            if (*dest == '\0') {
                return RCNEGATE(EOK);
            }

            dmax--;
            dest++;
            src++;
        }
    }

    /*
    * the entire src must have been copied, if not reset dest
    * to null the string.
    */
    handle_error(orig_dest, orig_dmax, (char*) "strcpy_ss: not "
        "enough space for src",
        ESNOSPC);
    return RCNEGATE(ESNOSPC);
}
EXPORT_SYMBOL(strcpy_ss)

rsize_t
strnlen_ss(const char *dest, rsize_t dmax)
{
    rsize_t count;

    if (dest == NULL) {
        return RCNEGATE(0);
    }

    if (dmax == 0) {
        invoke_safe_str_constraint_handler((char*) "strnlen_ss: dmax is 0",
            NULL, ESZEROL);
        return RCNEGATE(0);
    }

    if (dmax > RSIZE_MAX_STR) {
        invoke_safe_str_constraint_handler((char*) "strnlen_ss: dmax exceeds max",
            NULL, ESLEMAX);
        return RCNEGATE(0);
    }

    count = 0;
    while (*dest && dmax) {
        count++;
        dmax--;
        dest++;
    }

    return RCNEGATE(count);
}
EXPORT_SYMBOL(strnlen_ss)
