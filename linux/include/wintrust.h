/* Rufus for Linux: the <wintrust.h> definitions Rufus uses */
#pragma once
#include <windows.h>
typedef struct _WIN_CERTIFICATE {
	DWORD dwLength;
	WORD wRevision;
	WORD wCertificateType;
	BYTE bCertificate[ANYSIZE_ARRAY];
} WIN_CERTIFICATE, *LPWIN_CERTIFICATE;
#define WIN_CERT_TYPE_X509              0x0001
#define WIN_CERT_TYPE_PKCS_SIGNED_DATA  0x0002
#define WIN_CERT_REVISION_1_0           0x0100
#define WIN_CERT_REVISION_2_0           0x0200
