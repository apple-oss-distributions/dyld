/*
 * Copyright (c) 2024 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */


//
// included by:
//   ld to convert function variant name(s) to required flag bits
//   dyld to initialize actual flags bits
//
// FUNCTION_VARIANT_ARM64(_reqFlagBitNum, _name, _flagBitsInitialization)
//


//
//  arm64 processor specific function variants
//
FUNCTION_VARIANT_ARM64( 0, "default",    true )

// These were added in Spring 2025
FUNCTION_VARIANT_ARM64( 1, "flagm",     (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_FlagM))   )
FUNCTION_VARIANT_ARM64( 2, "flagm2",    (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_FlagM2))  )
FUNCTION_VARIANT_ARM64( 3, "fhm",       (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_FHM))     )
FUNCTION_VARIANT_ARM64( 4, "dotprod",   (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_DotProd)) )
FUNCTION_VARIANT_ARM64( 5, "sha3",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SHA3))    )
FUNCTION_VARIANT_ARM64( 6, "rdm",       (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_RDM))     )
FUNCTION_VARIANT_ARM64( 7, "lse",       (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_LSE))     )
FUNCTION_VARIANT_ARM64( 8, "sha256",    (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SHA256))  )
FUNCTION_VARIANT_ARM64( 9, "sha512",    (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SHA512))  )
FUNCTION_VARIANT_ARM64(10, "sha1",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SHA1))    )
FUNCTION_VARIANT_ARM64(11, "aes",       (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_AES))     )
FUNCTION_VARIANT_ARM64(12, "pmull",     (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_PMULL))   )
FUNCTION_VARIANT_ARM64(13, "specres",   (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SPECRES)) )
FUNCTION_VARIANT_ARM64(14, "sb",        (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SB))      )
FUNCTION_VARIANT_ARM64(15, "frintts",   (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_FRINTTS)) )
FUNCTION_VARIANT_ARM64(16, "lrcpc",     (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_LRCPC))   )
FUNCTION_VARIANT_ARM64(17, "lrcpc2",    (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_LRCPC2))  )
FUNCTION_VARIANT_ARM64(18, "fcma",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_FCMA))    )
FUNCTION_VARIANT_ARM64(19, "jscvt",     (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_JSCVT))   )
FUNCTION_VARIANT_ARM64(20, "PAuth",     (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_PAuth))   )
FUNCTION_VARIANT_ARM64(21, "PAuth2",    (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_PAuth2))  )
FUNCTION_VARIANT_ARM64(22, "fpac",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_FPAC))    )
FUNCTION_VARIANT_ARM64(23, "dpb",       (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_DPB))     )
FUNCTION_VARIANT_ARM64(24, "dpb2",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_DPB2))    )
FUNCTION_VARIANT_ARM64(25, "bf16",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_BF16))    )
FUNCTION_VARIANT_ARM64(26, "i8mm",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_I8MM))    )
FUNCTION_VARIANT_ARM64(27, "WFxT",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_WFxT))    )
FUNCTION_VARIANT_ARM64(28, "rpres",     (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_RPRES))   )
FUNCTION_VARIANT_ARM64(29, "ecv",       (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_ECV))     )
FUNCTION_VARIANT_ARM64(30, "afp",       (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_AFP))     )
FUNCTION_VARIANT_ARM64(31, "lse2",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_LSE2))    )
FUNCTION_VARIANT_ARM64(32, "csv2",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_CSV2))    )
FUNCTION_VARIANT_ARM64(33, "cvs3",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_CSV3))    )
FUNCTION_VARIANT_ARM64(34, "dit",       (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_DIT))     )
FUNCTION_VARIANT_ARM64(35, "fp16",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_FP16))    )
FUNCTION_VARIANT_ARM64(36, "ssbs",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SSBS))    )
FUNCTION_VARIANT_ARM64(37, "bti",       (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_BTI))     )
FUNCTION_VARIANT_ARM64(44, "sme",        (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SME))         )
FUNCTION_VARIANT_ARM64(45, "sme2",       (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SME2))        )
FUNCTION_VARIANT_ARM64(46, "smef64f64",  (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SME_F64F64))  )
FUNCTION_VARIANT_ARM64(47, "smei16i64",  (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_FEAT_SME_I16I64))  )
FUNCTION_VARIANT_ARM64(48, "smef32f32",  (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_SME_F32F32))       )
FUNCTION_VARIANT_ARM64(49, "smebi32i32", (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_SME_BI32I32))      )
FUNCTION_VARIANT_ARM64(50, "smeb16f32",  (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_SME_B16F32))       )
FUNCTION_VARIANT_ARM64(51, "smef16f32",  (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_SME_F16F32))       )
FUNCTION_VARIANT_ARM64(52, "smei8i32",   (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_SME_I8I32))        )
FUNCTION_VARIANT_ARM64(53, "smei16i32",  (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_SME_I16I32))       )
FUNCTION_VARIANT_ARM64(54, "AdvSIMD",    (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_AdvSIMD))          )
FUNCTION_VARIANT_ARM64(55, "AdvSIMDHpfp",(hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_AdvSIMD_HPFPCvt))  )  // name?
FUNCTION_VARIANT_ARM64(56, "crc32",      (hw_opt_arm_caps & ((__uint128_t)1 << CAP_BIT_CRC32))            )


// These were added in Fall 2025
