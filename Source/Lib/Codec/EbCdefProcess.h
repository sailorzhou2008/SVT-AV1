/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbCdefProcess_h
#define EbCdefProcess_h

#include "EbDefinitions.h"

#include "EbSystemResourceManager.h"
#include "EbPictureBufferDesc.h"
#include "EbSequenceControlSet.h"
#include "EbUtility.h"
#include "EbPsnr.h"
#include "EbPictureControlSet.h"

/**************************************
 * Cdef Context
 **************************************/
typedef struct CdefContext_s
{
    EbFifo_t                       *cdef_input_fifo_ptr;
    EbFifo_t                       *cdef_output_fifo_ptr;
} CdefContext_t;

/**************************************
 * Extern Function Declarations
 **************************************/
extern EbErrorType cdef_context_ctor(
    CdefContext_t **context_dbl_ptr,
    EbFifo_t                       *cdef_input_fifo_ptr,
    EbFifo_t                       *cdef_output_fifo_ptr,
    EbBool                  is16bit,
    uint32_t                max_input_luma_width,
    uint32_t                max_input_luma_height
   );

extern void* cdef_kernel(void *input_ptr);

#endif