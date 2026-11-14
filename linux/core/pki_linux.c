/*
 * Rufus: The Reliable USB Formatting Utility
 * PKI functions (signature validation) - Linux version
 * Copyright © 2015-2026 Pete Batard <pete@akeo.ie>
 * Copyright © 2026 Rufus Linux port contributors
 *
 * This is the Linux counterpart of src/pki.c. The Windows version relies on
 * CryptoAPI/WinTrust, which this file replaces with OpenSSL (libcrypto 3):
 * - ValidateOpensslSignature() validates the RSA-2048/SHA-256 signatures of
 *   the files we download from our server (same public key as on Windows).
 * - GetIssuerCertificateInfo() extracts the issuer (or signer) name and SHA-1
 *   thumbprint from the Authenticode signature of a UEFI bootloader, for the
 *   Secure Boot revocation checks performed in hash.c.
 * Authenticode validation of Windows executables (application self-update,
 * signed PowerShell scripts) has no use on Linux, so the related calls are
 * stubs that report the signature as unavailable.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <stdio.h>
#include <wintrust.h>
#include <assert.h>

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pkcs7.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "rufus.h"
#include "resource.h"
#include "msapi_utf8.h"
#include "localization.h"

/* PKI error codes that the Windows headers would provide */
#ifndef NTE_BAD_SIGNATURE
#define NTE_BAD_SIGNATURE               ((HRESULT)0x80090006L)
#endif
#ifndef TRUST_E_SYSTEM_ERROR
#define TRUST_E_SYSTEM_ERROR            ((HRESULT)0x80096001L)
#endif
#ifndef TRUST_E_BAD_DIGEST
#define TRUST_E_BAD_DIGEST              ((HRESULT)0x80096010L)
#endif
#ifndef TRUST_E_EXPLICIT_DISTRUST
#define TRUST_E_EXPLICIT_DISTRUST       ((HRESULT)0x800B0111L)
#endif
#ifndef CERT_E_UNTRUSTEDROOT
#define CERT_E_UNTRUSTEDROOT            ((HRESULT)0x800B0109L)
#endif

/*
 * The RSA public key modulus for the private key we use to sign the files on the server.
 * This is the exact same data as in src/pki.c. NB: OpenSSL can use it as is (big endian,
 * with the extra leading 0x00 that openssl adds to force a positive integer).
 */
static uint8_t rsa_pubkey_modulus[] = {
	/*
		$ openssl genrsa -aes256 -out private.pem 2048
		$ openssl rsa -in private.pem -pubout -out public.pem
		$ openssl rsa -pubin -inform PEM -text -noout < public.pem
		Public-Key: (2048 bit)
		Modulus:
			00:b6:40:7d:d1:98:7b:81:9e:be:23:0f:32:5d:55:
			60:c6:bf:b4:41:bb:43:1b:f1:e1:e6:f9:2b:d6:dd:
			11:50:e8:b9:3f:19:97:5e:a7:8b:4a:30:c6:76:58:
			72:1c:ac:ff:a1:f8:96:6c:51:5d:13:11:e3:5b:11:
			82:f5:9a:69:e4:28:97:0f:ca:1f:02:ea:1f:7d:dc:
			f9:fc:79:2f:61:ff:8e:45:60:65:ba:37:9b:de:49:
			05:6a:a8:fd:70:d0:0c:79:b6:d7:81:aa:54:c3:c6:
			4a:87:a0:45:ee:ca:d5:d5:c5:c2:ac:86:42:b3:58:
			27:d2:43:b9:37:f2:e6:75:66:17:53:d0:38:d0:c6:
			57:c2:55:36:a2:43:87:ea:24:f0:96:ec:34:dd:79:
			4d:80:54:9d:84:81:a7:cf:0c:a5:7c:d6:63:fa:7a:
			66:30:a9:50:ee:f0:e5:f8:a2:2d:ac:fc:24:21:fe:
			ef:e8:d3:6f:0e:27:b0:64:22:95:3e:6d:a6:66:97:
			c6:98:c2:47:b3:98:69:4d:b1:b5:d3:6f:43:f5:d7:
			a5:13:5e:8c:28:4f:62:4e:01:48:0a:63:89:e7:ca:
			34:aa:7d:2f:bb:70:e0:31:bb:39:49:a3:d2:c9:2e:
			a6:30:54:9a:5c:4d:58:17:d9:fc:3a:43:e6:8e:2a:
			18:e9
		Exponent: 65537 (0x10001)
	*/
	0x00, 0xb6, 0x40, 0x7d, 0xd1, 0x98, 0x7b, 0x81, 0x9e, 0xbe, 0x23, 0x0f, 0x32, 0x5d, 0x55,
	0x60, 0xc6, 0xbf, 0xb4, 0x41, 0xbb, 0x43, 0x1b, 0xf1, 0xe1, 0xe6, 0xf9, 0x2b, 0xd6, 0xdd,
	0x11, 0x50, 0xe8, 0xb9, 0x3f, 0x19, 0x97, 0x5e, 0xa7, 0x8b, 0x4a, 0x30, 0xc6, 0x76, 0x58,
	0x72, 0x1c, 0xac, 0xff, 0xa1, 0xf8, 0x96, 0x6c, 0x51, 0x5d, 0x13, 0x11, 0xe3, 0x5b, 0x11,
	0x82, 0xf5, 0x9a, 0x69, 0xe4, 0x28, 0x97, 0x0f, 0xca, 0x1f, 0x02, 0xea, 0x1f, 0x7d, 0xdc,
	0xf9, 0xfc, 0x79, 0x2f, 0x61, 0xff, 0x8e, 0x45, 0x60, 0x65, 0xba, 0x37, 0x9b, 0xde, 0x49,
	0x05, 0x6a, 0xa8, 0xfd, 0x70, 0xd0, 0x0c, 0x79, 0xb6, 0xd7, 0x81, 0xaa, 0x54, 0xc3, 0xc6,
	0x4a, 0x87, 0xa0, 0x45, 0xee, 0xca, 0xd5, 0xd5, 0xc5, 0xc2, 0xac, 0x86, 0x42, 0xb3, 0x58,
	0x27, 0xd2, 0x43, 0xb9, 0x37, 0xf2, 0xe6, 0x75, 0x66, 0x17, 0x53, 0xd0, 0x38, 0xd0, 0xc6,
	0x57, 0xc2, 0x55, 0x36, 0xa2, 0x43, 0x87, 0xea, 0x24, 0xf0, 0x96, 0xec, 0x34, 0xdd, 0x79,
	0x4d, 0x80, 0x54, 0x9d, 0x84, 0x81, 0xa7, 0xcf, 0x0c, 0xa5, 0x7c, 0xd6, 0x63, 0xfa, 0x7a,
	0x66, 0x30, 0xa9, 0x50, 0xee, 0xf0, 0xe5, 0xf8, 0xa2, 0x2d, 0xac, 0xfc, 0x24, 0x21, 0xfe,
	0xef, 0xe8, 0xd3, 0x6f, 0x0e, 0x27, 0xb0, 0x64, 0x22, 0x95, 0x3e, 0x6d, 0xa6, 0x66, 0x97,
	0xc6, 0x98, 0xc2, 0x47, 0xb3, 0x98, 0x69, 0x4d, 0xb1, 0xb5, 0xd3, 0x6f, 0x43, 0xf5, 0xd7,
	0xa5, 0x13, 0x5e, 0x8c, 0x28, 0x4f, 0x62, 0x4e, 0x01, 0x48, 0x0a, 0x63, 0x89, 0xe7, 0xca,
	0x34, 0xaa, 0x7d, 0x2f, 0xbb, 0x70, 0xe0, 0x31, 0xbb, 0x39, 0x49, 0xa3, 0xd2, 0xc9, 0x2e,
	0xa6, 0x30, 0x54, 0x9a, 0x5c, 0x4d, 0x58, 0x17, 0xd9, 0xfc, 0x3a, 0x43, 0xe6, 0x8e, 0x2a,
	0x18, 0xe9
};

/* Return the last OpenSSL error as a string (thread local buffer) */
static const char* OpensslErrorString(void)
{
	static __thread char error_string[256];
	unsigned long err = ERR_get_error();

	if (err == 0)
		return "Unknown error";
	ERR_error_string_n(err, error_string, sizeof(error_string));
	// Don't leave stale errors in the queue
	ERR_clear_error();
	return error_string;
}

/*
 * FormatMessage does not handle PKI errors (kept for API compatibility with
 * the Windows version - the PKI errors we can produce are listed below)
 */
const char* WinPKIErrorString(void)
{
	static char error_string[64];
	DWORD error_code = GetLastError();

	if (((error_code >> 16) != 0x8009) && ((error_code >> 16) != 0x800B))
		return WindowsErrorString();

	switch (error_code) {
	case NTE_BAD_SIGNATURE:
		return "Invalid Signature.";
	case CRYPT_E_BAD_MSG:
		return "Not a cryptographic message.";
	case CERT_E_UNTRUSTEDROOT:
		return "The root certificate is not trusted.";
	case TRUST_E_SYSTEM_ERROR:
		return "A system-level error occurred while verifying trust.";
	case TRUST_E_TIME_STAMP:
		return "The timestamp could not be verified.";
	case TRUST_E_BAD_DIGEST:
		return "The file content has been altered.";
	case TRUST_E_NOSIGNATURE:
		return "Not digitally signed.";
	case TRUST_E_EXPLICIT_DISTRUST:
		return "One of the certificates used was marked as untrusted by the user.";
	default:
		static_sprintf(error_string, "Unknown PKI error 0x%08X", error_code);
		return error_string;
	}
}

/*
 * On Windows, this returns the name of the entity that signed a Windows executable
 * through Authenticode (or the current executable when path is NULL). The Linux
 * executable is not Authenticode signed (packages are signed by the distribution's
 * package manager instead) so there is no signature name to report.
 */
char* GetSignatureName(const char* path, const char* country_code, uint8_t* thumbprint, BOOL bSilent)
{
	(void)country_code;
	if (thumbprint != NULL)
		memset(thumbprint, 0, SHA1_HASHSIZE);
	if (path != NULL)
		suprintf("PKI: Authenticode signatures can not be validated on Linux ('%s')", path);
	SetLastError(TRUST_E_NOSIGNATURE);
	return NULL;
}

/* Copy the UTF-8 Common Name of an X.509 subject into 'name' */
static BOOL GetCommonName(X509* cert, char* name, size_t name_size)
{
	X509_NAME* subject;
	X509_NAME_ENTRY* entry;
	unsigned char* utf8 = NULL;
	int idx, len;

	subject = X509_get_subject_name(cert);
	if (subject == NULL)
		return FALSE;
	idx = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
	if (idx < 0)
		return FALSE;
	entry = X509_NAME_get_entry(subject, idx);
	if (entry == NULL)
		return FALSE;
	len = ASN1_STRING_to_UTF8(&utf8, X509_NAME_ENTRY_get_data(entry));
	if (len <= 0) {
		OPENSSL_free(utf8);
		return FALSE;
	}
	snprintf(name, name_size, "%.*s", len, (const char*)utf8);
	OPENSSL_free(utf8);
	return (name[0] != 0);
}

/*
 * Fills the certificate's name and thumbprint.
 * Tries the issuer first, and if none is available, falls back to current cert.
 * Returns 0 for unsigned, -1 on error, 1 for signer or 2 for issuer.
 *
 * NB: On Windows, the issuer is found by building a certificate chain from the
 * certificates embedded in the signature (plus the system store). On Linux we
 * look for the issuer of the signer among the embedded certificates, which is
 * where the intermediate CAs we care about (Microsoft Windows Production PCA,
 * Microsoft UEFI CA...) are always provided.
 */
int GetIssuerCertificateInfo(uint8_t* cert, cert_info_t* info)
{
	int i, ret = 0;
	unsigned int md_len = SHA1_HASHSIZE;
	WIN_CERTIFICATE* pWinCert = (WIN_CERTIFICATE*)cert;
	const unsigned char* p;
	long len;
	PKCS7* p7 = NULL;
	STACK_OF(X509)* signers = NULL;
	STACK_OF(X509)* certs;
	X509 *signer, *issuer = NULL, *candidate, *selected;

	if (info == NULL)
		return -1;
	if (pWinCert == NULL || pWinCert->dwLength <= offsetof(WIN_CERTIFICATE, bCertificate))
		return 0;

	// NB: dwLength includes the size of the WIN_CERTIFICATE header.
	p = pWinCert->bCertificate;
	len = (long)(pWinCert->dwLength - offsetof(WIN_CERTIFICATE, bCertificate));
	p7 = d2i_PKCS7(NULL, &p, len);
	if (p7 == NULL || !PKCS7_type_is_signed(p7)) {
		SetLastError(CRYPT_E_BAD_MSG);
		uprintf("PKI: Failed to get signature: %s", WinPKIErrorString());
		ERR_clear_error();
		goto out;
	}

	// Get the signer certificate (from the certificates embedded in the signature)
	signers = PKCS7_get0_signers(p7, NULL, 0);
	if (signers == NULL || sk_X509_num(signers) <= 0) {
		uprintf("PKI: Failed to locate signer certificate in store: %s", OpensslErrorString());
		goto out;
	}
	signer = sk_X509_value(signers, 0);

	// Look for the certificate that issued the signer's certificate
	certs = p7->d.sign->cert;
	for (i = 0; certs != NULL && i < sk_X509_num(certs); i++) {
		candidate = sk_X509_value(certs, i);
		if (candidate == signer || X509_cmp(candidate, signer) == 0)
			continue;
		if (X509_check_issued(candidate, signer) == X509_V_OK) {
			issuer = candidate;
			break;
		}
	}
	selected = (issuer != NULL) ? issuer : signer;

	// Isolate the certificate subject name
	if (!GetCommonName(selected, info->name, sizeof(info->name))) {
		uprintf("PKI: Failed to get Subject Name");
		goto out;
	}

	// The thumbprint is the SHA-1 of the DER encoded certificate
	if (!X509_digest(selected, EVP_sha1(), info->thumbprint, &md_len) || md_len != SHA1_HASHSIZE) {
		uprintf("PKI: Failed to compute the thumbprint: %s", OpensslErrorString());
		goto out;
	}
	ret = (issuer != NULL) ? 2 : 1;

out:
	sk_X509_free(signers);
	PKCS7_free(p7);
	return ret;
}

/*
 * Return the signature timestamp (as a YYYYMMDDHHMMSS value) or 0 on error.
 * Only used for Authenticode signed Windows executables (see GetSignatureName()).
 */
uint64_t GetSignatureTimeStamp(const char* path)
{
	if (path != NULL)
		uprintf("PKI: Authenticode signatures can not be validated on Linux ('%s')", path);
	return 0ULL;
}

/*
 * On Windows, this validates the Authenticode signature of a downloaded executable
 * (application update) or PowerShell script (Fido). Neither applies on Linux, where
 * application updates are handled by the package manager and the download script is
 * validated through its RSA signature (ValidateOpensslSignature()) only.
 */
LONG ValidateSignature(HWND hDlg, const char* path)
{
	uprintf("PKI: Authenticode validation is not available on Linux - '%s' was not validated", path);
	if (hDlg != INVALID_HANDLE_VALUE && hDlg != NULL)
		Notification(MB_OK | MB_ICONERROR, lmprintf(MSG_283), lmprintf(MSG_284));
	SetLastError(TRUST_E_NOSIGNATURE);
	return TRUST_E_NOSIGNATURE;
}

/* Create an EVP_PKEY for our RSA public key */
static EVP_PKEY* GetRsaPublicKey(void)
{
	EVP_PKEY* pkey = NULL;
	EVP_PKEY_CTX* ctx = NULL;
	OSSL_PARAM_BLD* bld = NULL;
	OSSL_PARAM* params = NULL;
	BIGNUM *n = NULL, *e = NULL;

	n = BN_bin2bn(rsa_pubkey_modulus, sizeof(rsa_pubkey_modulus), NULL);
	e = BN_new();
	bld = OSSL_PARAM_BLD_new();
	if (n == NULL || e == NULL || bld == NULL || !BN_set_word(e, 0x010001))
		goto out;
	if (!OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n) ||
		!OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e))
		goto out;
	params = OSSL_PARAM_BLD_to_param(bld);
	if (params == NULL)
		goto out;
	ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
	if (ctx == NULL || EVP_PKEY_fromdata_init(ctx) <= 0 ||
		EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0)
		pkey = NULL;

out:
	EVP_PKEY_CTX_free(ctx);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(bld);
	BN_free(n);
	BN_free(e);
	return pkey;
}

/*
 * Validate an RSA-2048 PKCS#1 v1.5 SHA-256 signature, as produced by:
 *   openssl dgst -sha256 -sign private.pem -out file.sig file
 */
BOOL ValidateOpensslSignature(BYTE* pbBuffer, DWORD dwBufferLen, BYTE* pbSignature, DWORD dwSigLen)
{
	BOOL r = FALSE;
	int v;
	EVP_PKEY* pkey = NULL;
	EVP_MD_CTX* ctx = NULL;

	if (pbBuffer == NULL || pbSignature == NULL)
		return FALSE;

	// Import our RSA public key
	pkey = GetRsaPublicKey();
	if (pkey == NULL) {
		uprintf("PKI: Could not import public key: %s", OpensslErrorString());
		goto out;
	}

	// Create the hash object.
	ctx = EVP_MD_CTX_new();
	if (ctx == NULL || EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, pkey) <= 0) {
		uprintf("PKI: Could not create empty hash: %s", OpensslErrorString());
		goto out;
	}

	// Compute the hash of the buffer and verify the signature against it
	v = EVP_DigestVerify(ctx, pbSignature, dwSigLen, pbBuffer, dwBufferLen);
	r = (v == 1);
	if (!r) {
		// If the signature is invalid, clear the buffer so that
		// we don't keep potentially nasty stuff in memory.
		memset(pbBuffer, 0, dwBufferLen);
		if (v == 0) {
			SetLastError(NTE_BAD_SIGNATURE);
			ERR_clear_error();
			uprintf("Signature validation failed: %s", WinPKIErrorString());
		} else {
			uprintf("Signature validation failed: %s", OpensslErrorString());
		}
	}

out:
	EVP_MD_CTX_free(ctx);
	EVP_PKEY_free(pkey);
	return r;
}
