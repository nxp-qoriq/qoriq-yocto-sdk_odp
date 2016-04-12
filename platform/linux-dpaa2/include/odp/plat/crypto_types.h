/* Copyright (c) 2015, Linaro Limited
 * All rights reserved.
 *
 * SPDX-License-Identifier:     BSD-3-Clause
 */


/**
 * @file
 *
 * ODP crypto
 */

#ifndef ODP_CRYPTO_TYPES_H_
#define ODP_CRYPTO_TYPES_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @addtogroup odp_crypto
 *  @{
 */

#define ODP_CRYPTO_SESSION_INVALID (0xffffffffffffffffULL)

typedef odp_packet_t odp_crypto_compl_t;

/**
 * Crypto API opaque session handle
 */
typedef uint64_t odp_crypto_session_t;

/**
 * Crypto API operation mode
 */
enum odp_crypto_op_mode {
	ODP_CRYPTO_SYNC,    /**< Synchronous, return results immediately */
	ODP_CRYPTO_ASYNC,   /**< Aynchronous, return results via posted event */
};

/**
 * Crypto API operation type
 */
enum odp_crypto_op {
	ODP_CRYPTO_OP_ENCODE, /**< Encrypt and/or compute authentication ICV */
	ODP_CRYPTO_OP_DECODE  /**< Decrypt and/or verify authentication ICV */
};

/**
 * Crypto API cipher algorithm
 */
enum  odp_cipher_alg {
	ODP_CIPHER_ALG_NULL,	 /**< No cipher algorithm specified */
	ODP_CIPHER_ALG_DES,	 /**< DES */
	ODP_CIPHER_ALG_3DES_CBC, /**< Triple DES with cipher block chaining */
	ODP_CIPHER_ALG_AES128_CBC,  /**< AES128 with cipher block chaining */
	ODP_CIPHER_ALG_AES_CTR,  /**< AES128 with Counter Mode */
	ODP_CIPHER_ALG_KASUMI_F8,  /**< Kasumi F8 Encryption */
	ODP_CIPHER_ALG_SNOW_F8,   /**< Snow F8 Encryption */
	ODP_CIPHER_ALG_ZUC, /**< ZUC Ciphering */
};

/**
 * Crypto API authentication algorithm
 */
enum odp_auth_alg {
	ODP_AUTH_ALG_NULL,   /**< No authentication algorithm specified */
	ODP_AUTH_ALG_MD5_96, /**< HMAC-MD5 with 96 bit key */
	ODP_AUTH_ALG_SHA1_96,/**<SHA1 with 96 bit key */
	ODP_AUTH_ALG_SHA1_160,/**<SHA1 with 160 bit key */
	ODP_AUTH_ALG_SHA256_128,/**<SHA256 with 128 bit key */
	ODP_AUTH_ALG_SHA384_192,/**<SHA384 with 192 bit key */
	ODP_AUTH_ALG_SHA512_256, /**<SHA512 with 256 bit key */
	ODP_AUTH_ALG_KASUMI_F9, /**< Kasumi F9 Authentication */
	ODP_AUTH_ALG_AES_CMAC,/**<AESCMAC with 128 bit key */
	ODP_AUTH_ALG_SNOW_3G,/**< Snow 3G Authentication */
	ODP_AUTH_ALG_ZUC,       /**< ZUC Authentication */
};

/**
 * Crypto API session creation return code
 */
enum odp_crypto_ses_create_err {
	ODP_CRYPTO_SES_CREATE_ERR_NONE,       /**< Session created */
	ODP_CRYPTO_SES_CREATE_ERR_ENOMEM,     /**< Failed, no resources */
	ODP_CRYPTO_SES_CREATE_ERR_ENOTSUP,
	ODP_CRYPTO_SES_CREATE_ERR_INV_CIPHER, /**< Failed, bad cipher params */
	ODP_CRYPTO_SES_CREATE_ERR_INV_AUTH,   /**< Failed, bad auth params */
	ODP_CRYPTO_SES_CREATE_ERR_EUNSPEC,
};

/**
 * Crypto API algorithm return code
 */
enum crypto_alg_err {
	ODP_CRYPTO_ALG_ERR_NONE,      /**< Algorithm successful */
	ODP_CRYPTO_ALG_ERR_DATA_SIZE, /**< Invalid data block size */
	ODP_CRYPTO_ALG_ERR_KEY_SIZE,  /**< Key size invalid for algorithm */
	ODP_CRYPTO_ALG_ERR_ICV_CHECK, /**< Computed ICV value mismatch */
	ODP_CRYPTO_ALG_ERR_IV_INVALID,
};

/**
 * Crypto API hardware centric return code
 */
enum crypto_hw_err {
	ODP_CRYPTO_HW_ERR_NONE,		/**< Operation completed successfully */
	ODP_CRYPTO_HW_ERR_DMA,		/**< Error detected during DMA of data */
	ODP_CRYPTO_HW_ERR_BP_DEPLETED,	/**< Operation failed due to buffer pool depletion */
	ODP_CRYPTO_HW_ERR_UNSPEC,
};

/** Get printable format of odp_crypto_session_t */
static inline uint64_t odp_crypto_session_to_u64(odp_crypto_session_t hdl)
{
	return (uint64_t)hdl;
}

/** Get printable format of odp_crypto_compl_t_t */
static inline uint64_t odp_crypto_compl_to_u64(odp_crypto_compl_t hdl)
{
	return _odp_pri(hdl);
}

/**
 * @}
 */

#ifdef __cplusplus
}
#endif

#endif
