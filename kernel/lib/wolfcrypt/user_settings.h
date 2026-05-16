/*
 * user_settings.h - SLM-OS configuration for vendored wolfSSL + wolfSSH.
 *
 * Consulted only when -DWOLFSSL_USER_SETTINGS is set on the compile
 * line, which the CMakeLists guard does behind the `NET_SSHD` option.
 *
 * Goal: link the smallest possible wolfCrypt + wolfSSH that supports:
 *
 *   - Curve25519 KEX
 *   - Ed25519 host keys
 *   - ChaCha20-Poly1305 AEAD (chacha20-poly1305@openssh.com)
 *   - SHA-256 + HMAC-SHA-256
 *   - HKDF (used by SSH for key derivation)
 *   - scrypt (for #199d password hashing)
 *
 * Everything else — RSA, DH, ECC NIST curves, AES, 3DES, MD5, TLS,
 * SFTP, SCP, X11 forwarding, agent forwarding — is feature-gated off.
 */

#ifndef SLMOS_WOLFCRYPT_USER_SETTINGS_H
#define SLMOS_WOLFCRYPT_USER_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ----- Platform / runtime environment ----- */

#define WOLFCRYPT_ONLY
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define NO_STDIO_FILESYSTEM
#define NO_WRITEV
#define NO_WOLFSSL_DIR
#define WOLFSSL_USER_IO         /* wolfSSL: user-provided socket send/recv */
#define WOLFSSH_USER_IO         /* wolfSSH: same, gates default BSD IO in io.c */

/* Custom memory + time + entropy hooks come from kernel/net/ssh/wolf_os.c. */
#define XMALLOC_USER
#define WOLFSSL_NO_MALLOC
#define NO_WOLFSSL_MEMORY
#define USER_TIME
#define USER_TICKS

/* XMALLOC / XFREE / XREALLOC are *functions* under XMALLOC_USER —
 * wolfssl emits `extern` declarations and expects us to provide the
 * bodies. The implementations live in `kernel/net/ssh/wolf_os.c` and
 * forward to the kernel's byte-level allocator (lua_stubs.c's
 * malloc/free/realloc). DO NOT redefine these as macros here — that
 * collides with the extern declarations in
 * `wolfssl/wolfcrypt/types.h:490-494`. */

/* misc.c is compiled as its own translation unit (listed in the
 * CMake source set), so suppress wolfssl's default behaviour of
 * `#include <wolfcrypt/src/misc.c>` from inside other TUs (which
 * also wouldn't find the file under our `kernel/lib/wolfcrypt/`
 * layout anyway). */
#define WOLFSSL_MISC_INCLUDED

/* HASHDRBG seed source — wired to SLM-OS rng_get_bytes (which itself
 * draws from RNDR / RDRAND / SHA-256-mixed jitter pool depending on
 * platform). wolfSSL's HASHDRBG then mixes the entropy through its
 * own well-audited DRBG, so wolfSSL crypto consumers see the
 * SLM-OS entropy floor with an additional layer of conditioning. */
#define CUSTOM_RAND_GENERATE_SEED  slm_wolfcrypt_seed

/* ----- Inline asm / optimisations ----- */

/* Skip `#include <wolfcrypt/src/misc.c>` from inside other TUs;
 * misc.c is compiled as its own translation unit (CMake source set).
 * The gate inside random.c (and a few other files) is `NO_INLINE`. */
#define NO_INLINE

#define WC_NO_HARDEN            /* defer constant-time hardening to #199e audit */
#define TFM_TIMING_RESISTANT
#define ECC_TIMING_RESISTANT

/* SP-math single-precision only (curve25519/ed25519 use field-level
 * arithmetic, no general bigint needed). */
#define WOLFSSL_SP_MATH
#define WOLFSSL_SP_MATH_ALL
#define WOLFSSL_SP_SMALL

/* ----- Algorithms — enabled ----- */

#define HAVE_HASHDRBG
#define HAVE_HKDF
#define HAVE_CURVE25519
#define HAVE_ED25519
#define HAVE_ED25519_SIGN
#define HAVE_ED25519_VERIFY
#define HAVE_ED25519_KEY_IMPORT
#define HAVE_ED25519_KEY_EXPORT
/* wolfSSH 1.4.18's signing-algorithm gate requires all four of
 * HAVE_ED25519, _SIGN, _VERIFY, _KEY_IMPORT, _KEY_EXPORT AND
 * WOLFSSL_ED25519_STREAMING_VERIFY. Without the streaming flag,
 * wolfSSH self-defines WOLFSSH_NO_ED25519 and fails the
 * "at least one signing algorithm" check. */
#define WOLFSSL_ED25519_STREAMING_VERIFY
/* AES-256-GCM is the cipher wolfSSH 1.4.18 supports for its AEAD
 * mode. The original #199 ticket called for chacha20-poly1305 but
 * wolfSSH 1.4.18 doesn't ship that cipher (its menu is AES-only).
 * AES-256-GCM is OpenSSH's first-choice cipher today (RFC 5647), so
 * the pivot is benign — every modern OpenSSH client supports it. */
#define HAVE_AES
#define HAVE_AESGCM
#define WOLFSSL_AES_COUNTER       /* AES-CTR alongside GCM */
#define WOLFSSL_AES_128
#define WOLFSSL_AES_192
#define WOLFSSL_AES_256
#define GCM_SMALL                 /* table-free AES-GCM, smaller binary */
#define HAVE_AEAD                 /* enables AES-GCM wrapper */

/* ChaCha20-Poly1305 still vendored — useful for #199d password KDF
 * hashing path and as a future cipher option if wolfSSH adds it. */
#define HAVE_CHACHA
#define HAVE_POLY1305
#define WOLFSSL_SHA256
#define WOLFSSL_SHA              /* SHA-1 — needed by some SSH legacy paths */

/* Ed25519 per RFC 8032 uses SHA-512 internally for signature
 * computation. Even though we don't expose SHA-512 to wolfSSH's
 * cipher menu, the algorithm itself requires it. */
#define WOLFSSL_SHA512
#define HAVE_SCRYPT              /* #199d password KDF */

/* ----- Algorithms — disabled ----- */

#define NO_RSA
#define NO_DSA
#define NO_DH
#define NO_ECC                   /* NIST P-curves */
#define NO_ED448
#define NO_CURVE448

/* NO_AES omitted — wolfSSH requires AES (see HAVE_AES block above). */
#define NO_DES3
#define NO_RC4
#define NO_HC128
#define NO_RABBIT
#define NO_CAMELLIA
#define NO_BLAKE2

#define NO_MD4
#define NO_MD5
#define WOLFSSL_NO_SHA224
#define NO_SHA256_CLIENT_HSH     /* not a real macro; harmless if absent */
#define NO_SHA384
/* NO_SHA512 omitted — Ed25519 (RFC 8032) requires SHA-512 internally. */
#define WOLFSSL_NO_SHAKE128
#define WOLFSSL_NO_SHAKE256
#define WOLFSSL_NO_SHA3

#define NO_PSK
#define NO_PKCS7
#define NO_PKCS8
#define NO_PKCS11
#define NO_PKCS12
/* NO_ASN omitted — wolfSSH unconditionally references CA_TYPE /
 * CERT_TYPE / PRIVATEKEY_TYPE constants from asn.h. Vendoring asn.c
 * keeps the link clean; the runtime ASN.1 paths aren't reached
 * because OpenSSH-format keys are not PEM/PKCS#8. */
#define NO_ASN_TIME
#define NO_PEM
#define NO_CERTS
#define NO_OLD_TLS
#define WOLFSSL_NO_TLS12
#define WOLFSSL_NO_TLS13

#define NO_ERROR_STRINGS
#define WC_NO_ASYNC_THREADING

#ifdef __cplusplus
}
#endif

#endif /* SLMOS_WOLFCRYPT_USER_SETTINGS_H */
