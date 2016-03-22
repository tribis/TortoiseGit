// HwSMTP.cpp: implementation of the CHwSMTP class.
//
// Schannel/SSPI implementation based on http://www.coastrd.com/c-schannel-smtp
//
//////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "afxstr.h"
#include "HwSMTP.h"
#include "SpeedPostEmail.h"
#include "Windns.h"
#include <Afxmt.h>
#include "FormatMessageWrapper.h"
#include <atlenc.h>
#include "AppUtils.h"

#define IO_BUFFER_SIZE 0x10000

#pragma comment(lib, "Secur32.lib")

DWORD dwProtocol = SP_PROT_TLS1; // SP_PROT_TLS1; // SP_PROT_PCT1; SP_PROT_SSL2; SP_PROT_SSL3; 0=default
ALG_ID aiKeyExch = 0; // = default; CALG_DH_EPHEM; CALG_RSA_KEYX;

SCHANNEL_CRED SchannelCred;
PSecurityFunctionTable g_pSSPI;

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

CHwSMTP::CHwSMTP () :
	m_bConnected ( FALSE ),
	m_nSmtpSrvPort ( 25 ),
	m_bMustAuth ( TRUE )
{
	m_csPartBoundary = _T( "WC_MAIL_PaRt_BoUnDaRy_05151998" );
	m_csMIMEContentType.Format(_T("multipart/mixed; boundary=%s"), (LPCTSTR)m_csPartBoundary);
	m_csNoMIMEText = _T( "This is a multi-part message in MIME format." );
	//m_csCharSet = _T("\r\n\tcharset=\"iso-8859-1\"\r\n");

	hContext = nullptr;
	hCreds = nullptr;
	pbIoBuffer = nullptr;
	cbIoBufferLength = 0;

	m_iSecurityLevel = none;

	SecureZeroMemory(&Sizes, sizeof(SecPkgContext_StreamSizes));

	AfxSocketInit();
}

CHwSMTP::~CHwSMTP()
{
}

void CHwSMTP::GetNameAddress(CString &in, CString &name,CString &address)
{
	int start,end;
	start=in.Find(_T('<'));
	end=in.Find(_T('>'));

	if(start >=0 && end >=0)
	{
		name=in.Left(start);
		address=in.Mid(start+1,end-start-1);
	}
	else
		address=in;
}

CString CHwSMTP::GetServerAddress(CString &email)
{
	CString str;
	int start,end;

	start = email.Find(_T("<"));
	end = email.Find(_T(">"));

	if(start>=0 && end >=0)
	{
		str=email.Mid(start+1,end-start-1);
	}
	else
	{
		str=email;
	}

	start = str.Find(_T('@'));
	return str.Mid(start+1);

}

BOOL CHwSMTP::SendSpeedEmail
		(
			LPCTSTR	lpszAddrFrom,
			LPCTSTR	lpszAddrTo,
			LPCTSTR	lpszSubject,
			LPCTSTR	lpszBody,
			LPCTSTR	lpszCharSet,
			CStringArray *pStrAryAttach,
			LPCTSTR	pStrAryCC,
			LPCTSTR	pSend
		)
{

	BOOL ret=true;
	CString To;
	To += GET_SAFE_STRING(lpszAddrTo);
	To += _T(";");
	To += GET_SAFE_STRING(pStrAryCC);

	std::map<CString,std::vector<CString>> Address;

	int start = 0;
	while( start >= 0 )
	{
		CString one= To.Tokenize(_T(";"),start);
		one=one.Trim();
		if(one.IsEmpty())
			continue;

		CString addr;
		addr = GetServerAddress(one);
		if(addr.IsEmpty())
			continue;

		Address[addr].push_back(one);

	}

	std::map<CString,std::vector<CString>>::iterator itr1  =  Address.begin();
	for(  ;  itr1  !=  Address.end();  ++itr1 )
	{
		PDNS_RECORD pDnsRecord;
		PDNS_RECORD pNext;

		DNS_STATUS status = 
		DnsQuery(itr1->first ,
						DNS_TYPE_MX,DNS_QUERY_STANDARD,
						NULL,			//Contains DNS server IP address.
						&pDnsRecord,	//Resource record that contains the response.
						NULL
						);
		if (status)
		{
			m_csLastError.Format(_T("DNS query failed %d"), status);
			ret = false;
			continue;
		}

		CString to;
		to.Empty();
		for (size_t i = 0; i < itr1->second.size(); ++i)
		{
			to+=itr1->second[i];
			to+=_T(";");
		}
		if(to.IsEmpty())
			continue;

		pNext=pDnsRecord;
		while(pNext)
		{
			if(pNext->wType == DNS_TYPE_MX)
				if(SendEmail(pNext->Data.MX.pNameExchange,NULL,NULL,false,
					lpszAddrFrom,to,lpszSubject,lpszBody,lpszCharSet,pStrAryAttach,pStrAryCC,
					25,pSend,lpszAddrTo))
					break;
			pNext=pNext->pNext;
		}
		if(pNext == NULL)
			ret = false;

		if (pDnsRecord)
			DnsRecordListFree(pDnsRecord,DnsFreeRecordList);
	}

	return ret;
}

static SECURITY_STATUS ClientHandshakeLoop(CSocket * Socket, PCredHandle phCreds, CtxtHandle * phContext, BOOL fDoInitialRead, SecBuffer * pExtraData)
{
	SecBufferDesc OutBuffer, InBuffer;
	SecBuffer InBuffers[2], OutBuffers[1];
	DWORD dwSSPIFlags, dwSSPIOutFlags, cbData, cbIoBuffer;
	TimeStamp tsExpiry;
	SECURITY_STATUS scRet;
	BOOL fDoRead;

	dwSSPIFlags = ISC_REQ_SEQUENCE_DETECT	| ISC_REQ_REPLAY_DETECT		| ISC_REQ_CONFIDENTIALITY |
				  ISC_RET_EXTENDED_ERROR	| ISC_REQ_ALLOCATE_MEMORY	| ISC_REQ_STREAM;

	// Allocate data buffer.
	auto IoBuffer = std::make_unique<UCHAR[]>(IO_BUFFER_SIZE);
	if (!IoBuffer)
	{
		// printf("**** Out of memory (1)\n");
		return SEC_E_INTERNAL_ERROR;
	}
	cbIoBuffer = 0;
	fDoRead = fDoInitialRead;

	// Loop until the handshake is finished or an error occurs.
	scRet = SEC_I_CONTINUE_NEEDED;

	while (scRet == SEC_I_CONTINUE_NEEDED || scRet == SEC_E_INCOMPLETE_MESSAGE || scRet == SEC_I_INCOMPLETE_CREDENTIALS)
	{
		if (0 == cbIoBuffer || scRet == SEC_E_INCOMPLETE_MESSAGE) // Read data from server.
		{
			if (fDoRead)
			{
				cbData = Socket->Receive(IoBuffer.get() + cbIoBuffer, IO_BUFFER_SIZE - cbIoBuffer, 0);
				if (cbData == SOCKET_ERROR)
				{
					// printf("**** Error %d reading data from server\n", WSAGetLastError());
					scRet = SEC_E_INTERNAL_ERROR;
					break;
				}
				else if (cbData == 0)
				{
					// printf("**** Server unexpectedly disconnected\n");
					scRet = SEC_E_INTERNAL_ERROR;
					break;
				}
				// printf("%d bytes of handshake data received\n", cbData);
				cbIoBuffer += cbData;
			}
			else
				fDoRead = TRUE;
		}

		// Set up the input buffers. Buffer 0 is used to pass in data
		// received from the server. Schannel will consume some or all
		// of this. Leftover data (if any) will be placed in buffer 1 and
		// given a buffer type of SECBUFFER_EXTRA.
		InBuffers[0].pvBuffer	= IoBuffer.get();
		InBuffers[0].cbBuffer	= cbIoBuffer;
		InBuffers[0].BufferType	= SECBUFFER_TOKEN;

		InBuffers[1].pvBuffer	= nullptr;
		InBuffers[1].cbBuffer	= 0;
		InBuffers[1].BufferType	= SECBUFFER_EMPTY;

		InBuffer.cBuffers		= 2;
		InBuffer.pBuffers		= InBuffers;
		InBuffer.ulVersion		= SECBUFFER_VERSION;

		// Set up the output buffers. These are initialized to NULL
		// so as to make it less likely we'll attempt to free random
		// garbage later.
		OutBuffers[0].pvBuffer	= nullptr;
		OutBuffers[0].BufferType= SECBUFFER_TOKEN;
		OutBuffers[0].cbBuffer	= 0;

		OutBuffer.cBuffers		= 1;
		OutBuffer.pBuffers		= OutBuffers;
		OutBuffer.ulVersion		= SECBUFFER_VERSION;

		// Call InitializeSecurityContext.
		scRet = g_pSSPI->InitializeSecurityContext(phCreds, phContext, nullptr, dwSSPIFlags, 0, SECURITY_NATIVE_DREP, &InBuffer, 0, nullptr, &OutBuffer, &dwSSPIOutFlags, &tsExpiry);

		// If InitializeSecurityContext was successful (or if the error was
		// one of the special extended ones), send the contends of the output
		// buffer to the server.
		if (scRet == SEC_E_OK || scRet == SEC_I_CONTINUE_NEEDED || FAILED(scRet) && (dwSSPIOutFlags & ISC_RET_EXTENDED_ERROR))
		{
			if (OutBuffers[0].cbBuffer != 0 && OutBuffers[0].pvBuffer != nullptr)
			{
				cbData = Socket->Send(OutBuffers[0].pvBuffer, OutBuffers[0].cbBuffer, 0 );
				if(cbData == SOCKET_ERROR || cbData == 0)
				{
					// printf( "**** Error %d sending data to server (2)\n",  WSAGetLastError() );
					g_pSSPI->FreeContextBuffer(OutBuffers[0].pvBuffer);
					g_pSSPI->DeleteSecurityContext(phContext);
					return SEC_E_INTERNAL_ERROR;
				}
				// printf("%d bytes of handshake data sent\n", cbData);

				// Free output buffer.
				g_pSSPI->FreeContextBuffer(OutBuffers[0].pvBuffer);
				OutBuffers[0].pvBuffer = nullptr;
			}
		}

		// If InitializeSecurityContext returned SEC_E_INCOMPLETE_MESSAGE,
		// then we need to read more data from the server and try again.
		if (scRet == SEC_E_INCOMPLETE_MESSAGE) continue;

		// If InitializeSecurityContext returned SEC_E_OK, then the
		// handshake completed successfully.
		if (scRet == SEC_E_OK)
		{
			// If the "extra" buffer contains data, this is encrypted application
			// protocol layer stuff. It needs to be saved. The application layer
			// will later decrypt it with DecryptMessage.
			// printf("Handshake was successful\n");

			if (InBuffers[1].BufferType == SECBUFFER_EXTRA)
			{
				pExtraData->pvBuffer = LocalAlloc( LMEM_FIXED, InBuffers[1].cbBuffer );
				if (pExtraData->pvBuffer == nullptr)
				{
					// printf("**** Out of memory (2)\n");
					return SEC_E_INTERNAL_ERROR;
				}

				MoveMemory(pExtraData->pvBuffer, IoBuffer.get() + (cbIoBuffer - InBuffers[1].cbBuffer), InBuffers[1].cbBuffer);

				pExtraData->cbBuffer	= InBuffers[1].cbBuffer;
				pExtraData->BufferType	= SECBUFFER_TOKEN;

				// printf( "%d bytes of app data was bundled with handshake data\n", pExtraData->cbBuffer );
			}
			else
			{
				pExtraData->pvBuffer	= nullptr;
				pExtraData->cbBuffer	= 0;
				pExtraData->BufferType	= SECBUFFER_EMPTY;
			}
			break; // Bail out to quit
		}

		// Check for fatal error.
		if (FAILED(scRet))
		{
			// printf("**** Error 0x%x returned by InitializeSecurityContext (2)\n", scRet);
			break;
		}

		// If InitializeSecurityContext returned SEC_I_INCOMPLETE_CREDENTIALS,
		// then the server just requested client authentication.
		if (scRet == SEC_I_INCOMPLETE_CREDENTIALS)
		{
			// Busted. The server has requested client authentication and
			// the credential we supplied didn't contain a client certificate.
			// This function will read the list of trusted certificate
			// authorities ("issuers") that was received from the server
			// and attempt to find a suitable client certificate that
			// was issued by one of these. If this function is successful,
			// then we will connect using the new certificate. Otherwise,
			// we will attempt to connect anonymously (using our current credentials).
			//GetNewClientCredentials(phCreds, phContext);

			// Go around again.
			fDoRead = FALSE;
			scRet = SEC_I_CONTINUE_NEEDED;
			continue;
		}

		// Copy any leftover data from the "extra" buffer, and go around again.
		if ( InBuffers[1].BufferType == SECBUFFER_EXTRA )
		{
			MoveMemory(IoBuffer.get(), IoBuffer.get() + (cbIoBuffer - InBuffers[1].cbBuffer), InBuffers[1].cbBuffer);
			cbIoBuffer = InBuffers[1].cbBuffer;
		}
		else
			cbIoBuffer = 0;
	}

	// Delete the security context in the case of a fatal error.
	if (FAILED(scRet))
		g_pSSPI->DeleteSecurityContext(phContext);

	return scRet;
}

static SECURITY_STATUS PerformClientHandshake( CSocket * Socket, PCredHandle phCreds, LPTSTR pszServerName, CtxtHandle * phContext, SecBuffer * pExtraData)
{
	SecBufferDesc OutBuffer;
	SecBuffer OutBuffers[1];
	DWORD dwSSPIFlags, dwSSPIOutFlags, cbData;
	TimeStamp tsExpiry;
	SECURITY_STATUS scRet;

	dwSSPIFlags = ISC_REQ_SEQUENCE_DETECT	| ISC_REQ_REPLAY_DETECT		| ISC_REQ_CONFIDENTIALITY |
				  ISC_RET_EXTENDED_ERROR	| ISC_REQ_ALLOCATE_MEMORY	| ISC_REQ_STREAM;

	//  Initiate a ClientHello message and generate a token.
	OutBuffers[0].pvBuffer = nullptr;
	OutBuffers[0].BufferType = SECBUFFER_TOKEN;
	OutBuffers[0].cbBuffer = 0;

	OutBuffer.cBuffers = 1;
	OutBuffer.pBuffers = OutBuffers;
	OutBuffer.ulVersion = SECBUFFER_VERSION;

	scRet = g_pSSPI->InitializeSecurityContext(phCreds, nullptr, pszServerName, dwSSPIFlags, 0, SECURITY_NATIVE_DREP, nullptr, 0, phContext, &OutBuffer, &dwSSPIOutFlags, &tsExpiry);

	if (scRet != SEC_I_CONTINUE_NEEDED)
	{
		// printf("**** Error %d returned by InitializeSecurityContext (1)\n", scRet);
		return scRet;
	}

	// Send response to server if there is one.
	if (OutBuffers[0].cbBuffer != 0 && OutBuffers[0].pvBuffer != nullptr)
	{
		cbData = Socket->Send(OutBuffers[0].pvBuffer, OutBuffers[0].cbBuffer, 0);
		if (cbData == SOCKET_ERROR || cbData == 0)
		{
			// printf("**** Error %d sending data to server (1)\n", WSAGetLastError());
			g_pSSPI->FreeContextBuffer(OutBuffers[0].pvBuffer);
			g_pSSPI->DeleteSecurityContext(phContext);
			return SEC_E_INTERNAL_ERROR;
		}
		// printf("%d bytes of handshake data sent\n", cbData);

		g_pSSPI->FreeContextBuffer(OutBuffers[0].pvBuffer); // Free output buffer.
		OutBuffers[0].pvBuffer = nullptr;
	}

	return ClientHandshakeLoop(Socket, phCreds, phContext, TRUE, pExtraData);
}

static SECURITY_STATUS CreateCredentials(PCredHandle phCreds)
{
	TimeStamp tsExpiry;
	SECURITY_STATUS Status;
	DWORD cSupportedAlgs = 0;
	ALG_ID rgbSupportedAlgs[16];

	// Build Schannel credential structure. Currently, this sample only
	// specifies the protocol to be used (and optionally the certificate,
	// of course). Real applications may wish to specify other parameters as well.
	SecureZeroMemory(&SchannelCred, sizeof(SchannelCred));

	SchannelCred.dwVersion = SCHANNEL_CRED_VERSION;
	SchannelCred.grbitEnabledProtocols = dwProtocol;

	if (aiKeyExch)
		rgbSupportedAlgs[cSupportedAlgs++] = aiKeyExch;

	if (cSupportedAlgs)
	{
		SchannelCred.cSupportedAlgs = cSupportedAlgs;
		SchannelCred.palgSupportedAlgs = rgbSupportedAlgs;
	}

	SchannelCred.dwFlags |= SCH_CRED_NO_DEFAULT_CREDS;

	// The SCH_CRED_MANUAL_CRED_VALIDATION flag is specified because
	// this sample verifies the server certificate manually.
	// Applications that expect to run on WinNT, Win9x, or WinME
	// should specify this flag and also manually verify the server
	// certificate. Applications running on newer versions of Windows can
	// leave off this flag, in which case the InitializeSecurityContext
	// function will validate the server certificate automatically.
	SchannelCred.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION;

	// Create an SSPI credential.
	Status = g_pSSPI->AcquireCredentialsHandle(nullptr,                // Name of principal
												 UNISP_NAME,           // Name of package
												 SECPKG_CRED_OUTBOUND, // Flags indicating use
												 nullptr,              // Pointer to logon ID
												 &SchannelCred,        // Package specific data
												 nullptr,              // Pointer to GetKey() func
												 nullptr,              // Value to pass to GetKey()
												 phCreds,              // (out) Cred Handle
												 &tsExpiry );          // (out) Lifetime (optional)

	return Status;
}

static DWORD EncryptSend(CSocket * Socket, CtxtHandle * phContext, PBYTE pbIoBuffer, SecPkgContext_StreamSizes Sizes)
// http://msdn.microsoft.com/en-us/library/aa375378(VS.85).aspx
// The encrypted message is encrypted in place, overwriting the original contents of its buffer.
{
	SECURITY_STATUS scRet;
	SecBufferDesc Message;
	SecBuffer Buffers[4];
	DWORD cbMessage;
	PBYTE pbMessage;

	pbMessage = pbIoBuffer + Sizes.cbHeader; // Offset by "header size"
	cbMessage = (DWORD)strlen((char *)pbMessage);

	// Encrypt the HTTP request.
	Buffers[0].pvBuffer     = pbIoBuffer;                 // Pointer to buffer 1
	Buffers[0].cbBuffer     = Sizes.cbHeader;             // length of header
	Buffers[0].BufferType   = SECBUFFER_STREAM_HEADER;    // Type of the buffer

	Buffers[1].pvBuffer     = pbMessage;                  // Pointer to buffer 2
	Buffers[1].cbBuffer     = cbMessage;                  // length of the message
	Buffers[1].BufferType   = SECBUFFER_DATA;             // Type of the buffer

	Buffers[2].pvBuffer     = pbMessage + cbMessage;      // Pointer to buffer 3
	Buffers[2].cbBuffer     = Sizes.cbTrailer;            // length of the trailor
	Buffers[2].BufferType   = SECBUFFER_STREAM_TRAILER;   // Type of the buffer

	Buffers[3].pvBuffer     = SECBUFFER_EMPTY;            // Pointer to buffer 4
	Buffers[3].cbBuffer     = SECBUFFER_EMPTY;            // length of buffer 4
	Buffers[3].BufferType   = SECBUFFER_EMPTY;            // Type of the buffer 4

	Message.ulVersion       = SECBUFFER_VERSION;          // Version number
	Message.cBuffers        = 4;                          // Number of buffers - must contain four SecBuffer structures.
	Message.pBuffers        = Buffers;                    // Pointer to array of buffers

	scRet = g_pSSPI->EncryptMessage(phContext, 0, &Message, 0); // must contain four SecBuffer structures.
	if (FAILED(scRet))
	{
		// printf("**** Error 0x%x returned by EncryptMessage\n", scRet);
		return scRet;
	}


	// Send the encrypted data to the server.
	return Socket->Send(pbIoBuffer, Buffers[0].cbBuffer + Buffers[1].cbBuffer + Buffers[2].cbBuffer, 0);
}

static LONG DisconnectFromServer(CSocket * Socket, PCredHandle phCreds, CtxtHandle * phContext)
{
	PBYTE pbMessage;
	DWORD dwType, dwSSPIFlags, dwSSPIOutFlags, cbMessage, cbData, Status;
	SecBufferDesc OutBuffer;
	SecBuffer OutBuffers[1];
	TimeStamp tsExpiry;

	dwType = SCHANNEL_SHUTDOWN; // Notify schannel that we are about to close the connection.

	OutBuffers[0].pvBuffer = &dwType;
	OutBuffers[0].BufferType = SECBUFFER_TOKEN;
	OutBuffers[0].cbBuffer = sizeof(dwType);

	OutBuffer.cBuffers = 1;
	OutBuffer.pBuffers = OutBuffers;
	OutBuffer.ulVersion = SECBUFFER_VERSION;

	Status = g_pSSPI->ApplyControlToken(phContext, &OutBuffer);
	if (FAILED(Status))
	{
		// printf("**** Error 0x%x returned by ApplyControlToken\n", Status);
		goto cleanup; 
	}

	// Build an SSL close notify message.
	dwSSPIFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;

	OutBuffers[0].pvBuffer = nullptr;
	OutBuffers[0].BufferType = SECBUFFER_TOKEN;
	OutBuffers[0].cbBuffer = 0;

	OutBuffer.cBuffers = 1;
	OutBuffer.pBuffers = OutBuffers;
	OutBuffer.ulVersion = SECBUFFER_VERSION;

	Status = g_pSSPI->InitializeSecurityContext(phCreds, phContext, nullptr, dwSSPIFlags, 0, SECURITY_NATIVE_DREP, nullptr, 0, phContext, &OutBuffer, &dwSSPIOutFlags, &tsExpiry);

	if (FAILED(Status))
	{
		// printf("**** Error 0x%x returned by InitializeSecurityContext\n", Status);
		goto cleanup;
	}

	pbMessage = (PBYTE)OutBuffers[0].pvBuffer;
	cbMessage = OutBuffers[0].cbBuffer;

	// Send the close notify message to the server.
	if (pbMessage != nullptr && cbMessage != 0)
	{
		cbData = Socket->Send(pbMessage, cbMessage, 0);
		if (cbData == SOCKET_ERROR || cbData == 0)
		{
			Status = WSAGetLastError();
			goto cleanup;
		}
		// printf("Sending Close Notify\n");
		// printf("%d bytes of handshake data sent\n", cbData);
		g_pSSPI->FreeContextBuffer(pbMessage); // Free output buffer.
	}

cleanup:
	g_pSSPI->DeleteSecurityContext(phContext); // Free the security context.
	Socket->Close();

	return Status;
}

static SECURITY_STATUS ReadDecrypt(CSocket * Socket, PCredHandle phCreds, CtxtHandle * phContext, PBYTE pbIoBuffer, DWORD cbIoBufferLength)

// calls recv() - blocking socket read
// http://msdn.microsoft.com/en-us/library/ms740121(VS.85).aspx

// The encrypted message is decrypted in place, overwriting the original contents of its buffer.
// http://msdn.microsoft.com/en-us/library/aa375211(VS.85).aspx

{
	SecBuffer ExtraBuffer;
	SecBuffer * pDataBuffer, * pExtraBuffer;

	SECURITY_STATUS scRet;
	SecBufferDesc Message;
	SecBuffer Buffers[4];

	DWORD cbIoBuffer, cbData, length;
	PBYTE buff;

	// Read data from server until done.
	cbIoBuffer = 0;
	scRet = 0;
	while (TRUE) // Read some data.
	{
		if (cbIoBuffer == 0 || scRet == SEC_E_INCOMPLETE_MESSAGE) // get the data
		{
			cbData = Socket->Receive(pbIoBuffer + cbIoBuffer, cbIoBufferLength - cbIoBuffer, 0);
			if (cbData == SOCKET_ERROR)
			{
				// printf("**** Error %d reading data from server\n", WSAGetLastError());
				scRet = SEC_E_INTERNAL_ERROR;
				break;
			}
			else if (cbData == 0) // Server disconnected.
			{
				if (cbIoBuffer)
				{
					// printf("**** Server unexpectedly disconnected\n");
					scRet = SEC_E_INTERNAL_ERROR;
					return scRet;
				}
				else
					break; // All Done
			}
			else // success
			{
				// printf("%d bytes of (encrypted) application data received\n", cbData);
				cbIoBuffer += cbData;
			}
		}
		
		// Decrypt the received data.
		Buffers[0].pvBuffer     = pbIoBuffer;
		Buffers[0].cbBuffer     = cbIoBuffer;
		Buffers[0].BufferType   = SECBUFFER_DATA;  // Initial Type of the buffer 1
		Buffers[1].BufferType   = SECBUFFER_EMPTY; // Initial Type of the buffer 2
		Buffers[2].BufferType   = SECBUFFER_EMPTY; // Initial Type of the buffer 3
		Buffers[3].BufferType   = SECBUFFER_EMPTY; // Initial Type of the buffer 4

		Message.ulVersion       = SECBUFFER_VERSION;    // Version number
		Message.cBuffers        = 4;                    // Number of buffers - must contain four SecBuffer structures.
		Message.pBuffers        = Buffers;              // Pointer to array of buffers

		scRet = g_pSSPI->DecryptMessage(phContext, &Message, 0, nullptr);
		if (scRet == SEC_I_CONTEXT_EXPIRED)
			break; // Server signalled end of session
//		if (scRet == SEC_E_INCOMPLETE_MESSAGE - Input buffer has partial encrypted record, read more
		if (scRet != SEC_E_OK && scRet != SEC_I_RENEGOTIATE && scRet != SEC_I_CONTEXT_EXPIRED)
			return scRet;

		// Locate data and (optional) extra buffers.
		pDataBuffer  = nullptr;
		pExtraBuffer = nullptr;
		for (int i = 1; i < 4; ++i)
		{
			if (pDataBuffer  == nullptr && Buffers[i].BufferType == SECBUFFER_DATA)
				pDataBuffer  = &Buffers[i];
			if (pExtraBuffer == nullptr && Buffers[i].BufferType == SECBUFFER_EXTRA)
				pExtraBuffer = &Buffers[i];
		}

		// Display the decrypted data.
		if (pDataBuffer)
		{
			length = pDataBuffer->cbBuffer;
			if (length) // check if last two chars are CR LF
			{
				buff = (PBYTE)pDataBuffer->pvBuffer;
				if (buff[length-2] == 13 && buff[length-1] == 10) // Found CRLF
				{
					buff[length] = 0;
					break;
				}
			}
		}

		// Move any "extra" data to the input buffer.
		if (pExtraBuffer)
		{
			MoveMemory(pbIoBuffer, pExtraBuffer->pvBuffer, pExtraBuffer->cbBuffer);
			cbIoBuffer = pExtraBuffer->cbBuffer;
		}
		else
			cbIoBuffer = 0;

		// The server wants to perform another handshake sequence.
		if (scRet == SEC_I_RENEGOTIATE)
		{
			// printf("Server requested renegotiate!\n");
			scRet = ClientHandshakeLoop( Socket, phCreds, phContext, FALSE, &ExtraBuffer);
			if (scRet != SEC_E_OK)
				return scRet;

			if (ExtraBuffer.pvBuffer) // Move any "extra" data to the input buffer.
			{
				MoveMemory(pbIoBuffer, ExtraBuffer.pvBuffer, ExtraBuffer.cbBuffer);
				cbIoBuffer = ExtraBuffer.cbBuffer;
			}
		}
	} // Loop till CRLF is found at the end of the data

	return SEC_E_OK;
}

BOOL CHwSMTP::SendEmail (
		LPCTSTR lpszSmtpSrvHost,
		LPCTSTR lpszUserName,
		LPCTSTR lpszPasswd,
		BOOL bMustAuth,
		LPCTSTR lpszAddrFrom,
		LPCTSTR lpszAddrTo,
		LPCTSTR lpszSubject,
		LPCTSTR lpszBody,
		LPCTSTR lpszCharSet,						// �ַ������ͣ����磺������������Ӧ����"big5"����������ʱ����"gb2312"
		CStringArray *pStrAryAttach/*=NULL*/,
		LPCTSTR pStrAryCC/*=NULL*/,
		UINT nSmtpSrvPort,/*=25*/
		LPCTSTR pSender,
		LPCTSTR pToList,
		DWORD secLevel
		)
{
	m_StrAryAttach.RemoveAll();

	m_StrCC += GET_SAFE_STRING(pStrAryCC);

	m_csSmtpSrvHost = GET_SAFE_STRING ( lpszSmtpSrvHost );
	if ( m_csSmtpSrvHost.GetLength() <= 0 )
	{
		m_csLastError.Format ( _T("Parameter Error!") );
		return FALSE;
	}
	m_csUserName = GET_SAFE_STRING ( lpszUserName );
	m_csPasswd = GET_SAFE_STRING ( lpszPasswd );
	m_bMustAuth = bMustAuth;
	if ( m_bMustAuth && m_csUserName.GetLength() <= 0 )
	{
		m_csLastError.Format ( _T("Parameter Error!") );
		return FALSE;
	}

	m_csAddrFrom = GET_SAFE_STRING ( lpszAddrFrom );
	m_csAddrTo = GET_SAFE_STRING ( lpszAddrTo );
//	m_csFromName = GET_SAFE_STRING ( lpszFromName );
//	m_csReceiverName = GET_SAFE_STRING ( lpszReceiverName );
	m_csSubject = GET_SAFE_STRING ( lpszSubject );
	m_csBody = GET_SAFE_STRING ( lpszBody );

	this->m_csSender = GET_SAFE_STRING(pSender);
	this->m_csToList = GET_SAFE_STRING(pToList);

	m_nSmtpSrvPort = nSmtpSrvPort;

	if ( lpszCharSet && lstrlen(lpszCharSet) > 0 )
		m_csCharSet.Format ( _T("\r\n\tcharset=\"%s\"\r\n"), lpszCharSet );

	if	(
			m_csAddrFrom.GetLength() <= 0 || m_csAddrTo.GetLength() <= 0
		)
	{
		m_csLastError.Format ( _T("Parameter Error!") );
		return FALSE;
	}

	if ( pStrAryAttach )
	{
		m_StrAryAttach.Append ( *pStrAryAttach );
	}
	if ( m_StrAryAttach.GetSize() < 1 )
		m_csMIMEContentType.Format(_T("text/plain; %s"), (LPCTSTR)m_csCharSet);

	// ����Socket
	m_SendSock.Close();
	if ( !m_SendSock.Create () )
	{
		//int nResult = GetLastError();
		m_csLastError.Format ( _T("Create socket failed!") );
		return FALSE;
	}

	switch (secLevel)
	{
	case 1:
		m_iSecurityLevel = want_tls;
		break;
	case 2:
		m_iSecurityLevel = ssl;
		break;
	default:
		m_iSecurityLevel = none;
	}

	if ( !m_SendSock.Connect ( m_csSmtpSrvHost, m_nSmtpSrvPort ) )
	{
		m_csLastError.Format(_T("Connect to [%s] failed"), (LPCTSTR)m_csSmtpSrvHost);
		return FALSE;
	}

	if (m_iSecurityLevel == want_tls) {
		if (!GetResponse("220"))
			return FALSE;
		m_bConnected = TRUE;
		Send(L"STARTTLS\n");
		if (!GetResponse("220"))
			return FALSE;
		m_iSecurityLevel = tls_established;
	}

	BOOL ret = FALSE;

	SecBuffer ExtraData;
	SECURITY_STATUS Status;

	CtxtHandle contextStruct;
	CredHandle credentialsStruct;

	if (m_iSecurityLevel >= ssl)
	{
		g_pSSPI = InitSecurityInterface();

		contextStruct.dwLower = 0;
		contextStruct.dwUpper = 0;

		hCreds = &credentialsStruct;
		credentialsStruct.dwLower = 0;
		credentialsStruct.dwUpper = 0;
		Status = CreateCredentials(hCreds);
		if (Status != SEC_E_OK)
		{
			m_csLastError = CFormatMessageWrapper(Status);
			return FALSE;
		}

		hContext = &contextStruct;
		Status = PerformClientHandshake(&m_SendSock, hCreds, m_csSmtpSrvHost.GetBuffer(), hContext, &ExtraData);
		if (Status != SEC_E_OK)
		{
			m_csLastError = CFormatMessageWrapper(Status);
			return FALSE;
		}

		PCCERT_CONTEXT pRemoteCertContext = nullptr;
		// Authenticate server's credentials. Get server's certificate.
		Status = g_pSSPI->QueryContextAttributes(hContext, SECPKG_ATTR_REMOTE_CERT_CONTEXT, (PVOID)&pRemoteCertContext);
		if (Status)
		{
			m_csLastError = CFormatMessageWrapper(Status);
			goto cleanup;
		}

		git_cert_x509 cert;
		cert.parent.cert_type = GIT_CERT_X509;
		cert.data = pRemoteCertContext->pbCertEncoded;
		cert.len = pRemoteCertContext->cbCertEncoded;
		if (CAppUtils::Git2CertificateCheck((git_cert*)&cert, 0, CUnicodeUtils::GetUTF8(m_csSmtpSrvHost), nullptr))
		{
			CertFreeCertificateContext(pRemoteCertContext);
			m_csLastError = _T("Invalid certificate.");
			goto cleanup;
		}

		CertFreeCertificateContext(pRemoteCertContext);

		Status = g_pSSPI->QueryContextAttributes(hContext, SECPKG_ATTR_STREAM_SIZES, &Sizes);
		if (Status)
		{
			m_csLastError = CFormatMessageWrapper(Status);
			goto cleanup;
		}

		// Create a buffer.
		cbIoBufferLength = Sizes.cbHeader + Sizes.cbMaximumMessage + Sizes.cbTrailer;
		pbIoBuffer = (PBYTE)LocalAlloc(LMEM_FIXED, cbIoBufferLength);
		SecureZeroMemory(pbIoBuffer, cbIoBufferLength);
		if (pbIoBuffer == nullptr)
		{
			m_csLastError = _T("Could not allocate memory");
			goto cleanup;
		}
	}

	if (m_iSecurityLevel <= ssl)
	{
		if (!GetResponse("220"))
			goto cleanup;
		m_bConnected = TRUE;
	}
	
	ret = SendEmail();

cleanup:
	if (m_iSecurityLevel >= ssl)
	{
		if (hContext && hCreds)
			DisconnectFromServer(&m_SendSock, hCreds, hContext);
		if (pbIoBuffer)
		{
			LocalFree(pbIoBuffer);
			pbIoBuffer = nullptr;
			cbIoBufferLength = 0;
		}
		if (hContext)
		{
			g_pSSPI->DeleteSecurityContext(hContext);
			hContext = nullptr;
		}
		if (hCreds)
		{
			g_pSSPI->FreeCredentialsHandle(hCreds);
			hCreds = nullptr;
		}
		g_pSSPI = nullptr;
	}
	else
		m_SendSock.Close();

	return ret;
}

BOOL CHwSMTP::GetResponse(LPCSTR lpszVerifyCode)
{
	if (!lpszVerifyCode || strlen(lpszVerifyCode) < 1)
		return FALSE;

	SECURITY_STATUS scRet = SEC_E_OK;

	char szRecvBuf[1024] = {0};
	int nRet = 0;
	char szStatusCode[4] = {0};

	if (m_iSecurityLevel >= ssl)
	{
		scRet = ReadDecrypt(&m_SendSock, hCreds, hContext, pbIoBuffer, cbIoBufferLength);
		SecureZeroMemory(szRecvBuf, 1024);
		memcpy(szRecvBuf, pbIoBuffer+Sizes.cbHeader, 1024);
	}
	else
		nRet = m_SendSock.Receive(szRecvBuf, sizeof(szRecvBuf));
	//TRACE(_T("Received : %s\r\n"), szRecvBuf);
	if (nRet == 0 && m_iSecurityLevel == none || m_iSecurityLevel >= ssl && scRet != SEC_E_OK)
	{
		m_csLastError.Format ( _T("Receive TCP data failed") );
		return FALSE;
	}
	memcpy ( szStatusCode, szRecvBuf, 3 );
	if (strcmp(szStatusCode, lpszVerifyCode) != 0)
	{
		m_csLastError.Format(_T("Received invalid response: %s"), (LPCTSTR)CUnicodeUtils::GetUnicode(szRecvBuf));
		return FALSE;
	}

	return TRUE;
}
BOOL CHwSMTP::SendBuffer(const char* buff, int size)
{
	if(size<0)
		size=(int)strlen(buff);
	if ( !m_bConnected )
	{
		m_csLastError.Format ( _T("Didn't connect") );
		return FALSE;
	}

	if (m_iSecurityLevel >= ssl)
	{
		int sent = 0;
		while (size - sent > 0)
		{
			int toSend = min(size - sent, (int)Sizes.cbMaximumMessage);
			SecureZeroMemory(pbIoBuffer + Sizes.cbHeader, Sizes.cbMaximumMessage);
			memcpy(pbIoBuffer + Sizes.cbHeader, buff + sent, toSend);
			DWORD cbData = EncryptSend(&m_SendSock, hContext, pbIoBuffer, Sizes);
			if (cbData == SOCKET_ERROR || cbData == 0)
				return FALSE;
			sent += toSend;
		}
	}
	else if (m_SendSock.Send ( buff, size ) != size)
	{
		m_csLastError.Format ( _T("Socket send data failed") );
		return FALSE;
	}

	return TRUE;
}

BOOL CHwSMTP::Send(const CString &str )
{
	return Send(CUnicodeUtils::GetUTF8(str));
}

BOOL CHwSMTP::Send(const CStringA &str)
{
	//TRACE(_T("Send: %s\r\n"), (LPCTSTR)CUnicodeUtils::GetUnicode(str));
	return SendBuffer(str, str.GetLength());
}

BOOL CHwSMTP::SendEmail()
{
	CStringA hostname;
	gethostname(CStrBufA(hostname, 64), 64);

	// make sure helo hostname can be interpreted as a FQDN
	if (hostname.Find(".") == -1)
		hostname += ".local";

	CStringA str;
	str.Format("HELO %s\r\n", (LPCSTR)hostname);
	if (!Send(str))
		return FALSE;
	if (!GetResponse("250"))
		return FALSE;

	if ( m_bMustAuth && !auth() )
		return FALSE;

	if ( !SendHead() )
		return FALSE;

	if (!SendSubject(CUnicodeUtils::GetUnicode(hostname)))
		return FALSE;

	if ( !SendBody() )
		return FALSE;

	if ( !SendAttach() )
	{
		return FALSE;
	}

	if (!Send(".\r\n"))
		return FALSE;
	if (!GetResponse("250"))
		return FALSE;

	if ( HANDLE_IS_VALID(m_SendSock.m_hSocket) )
		Send("QUIT\r\n");
	m_bConnected = FALSE;

	return TRUE;
}

static CStringA EncodeBase64(const char* source, int len)
{
	int neededLength = Base64EncodeGetRequiredLength(len);
	CStringA output;
	Base64Encode((BYTE*)source, len, CStrBufA(output, neededLength), &neededLength);
	return output;
}

static CStringA EncodeBase64(const CString& source)
{
	CStringA buf = CUnicodeUtils::GetUTF8(source);
	return EncodeBase64(buf, buf.GetLength());
}

BOOL CHwSMTP::auth()
{
	if (!Send("auth login\r\n"))
		return FALSE;
	if (!GetResponse("334"))
		return FALSE;

	if (!Send(EncodeBase64(m_csUserName)))
		return FALSE;

	if (!GetResponse("334"))
	{
		m_csLastError.Format ( _T("Authentication UserName failed") );
		return FALSE;
	}

	if (!Send(EncodeBase64(m_csPasswd)))
		return FALSE;

	if (!GetResponse("235"))
	{
		m_csLastError.Format ( _T("Authentication Password failed") );
		return FALSE;
	}

	return TRUE;
}

BOOL CHwSMTP::SendHead()
{
	CString str;
	CString name,addr;
	GetNameAddress(m_csAddrFrom,name,addr);

	str.Format(_T("MAIL From: <%s>\r\n"), (LPCTSTR)addr);
	if (!Send(str))
		return FALSE;

	if (!GetResponse("250"))
		return FALSE;

	int start=0;
	while(start>=0)
	{
		CString one=m_csAddrTo.Tokenize(_T(";"),start);
		one=one.Trim();
		if(one.IsEmpty())
			continue;


		GetNameAddress(one,name,addr);

		str.Format(_T("RCPT TO: <%s>\r\n"), (LPCTSTR)addr);
		if (!Send(str))
			return FALSE;
		if (!GetResponse("250"))
			return FALSE;
	}

	if (!Send("DATA\r\n"))
		return FALSE;
	if (!GetResponse("354"))
		return FALSE;

	return TRUE;
}

BOOL CHwSMTP::SendSubject(const CString &hostname)
{
	CString csSubject;
	csSubject += _T("Date: ");
	COleDateTime tNow = COleDateTime::GetCurrentTime();
	if ( tNow > 1 )
	{
		csSubject += FormatDateTime (tNow, _T("%a, %d %b %y %H:%M:%S %Z"));
	}
	csSubject += _T("\r\n");
	csSubject.AppendFormat(_T("From: %s\r\n"), (LPCTSTR)m_csAddrFrom);

	if (!m_StrCC.IsEmpty())
		csSubject.AppendFormat(_T("CC: %s\r\n"), (LPCTSTR)m_StrCC);

	if(m_csSender.IsEmpty())
		m_csSender =  this->m_csAddrFrom;

	csSubject.AppendFormat(_T("Sender: %s\r\n"), (LPCTSTR)m_csSender);

	if(this->m_csToList.IsEmpty())
		m_csToList = m_csReceiverName;

	csSubject.AppendFormat(_T("To: %s\r\n"), (LPCTSTR)m_csToList);

	csSubject.AppendFormat(_T("Subject: %s\r\n"), (LPCTSTR)m_csSubject);

	CString m_ListID;
	GUID guid;
	HRESULT hr = CoCreateGuid(&guid);
	if (hr == S_OK)
	{
		RPC_WSTR guidStr;
		if (UuidToString(&guid, &guidStr) == RPC_S_OK)
		{
			m_ListID = (LPTSTR)guidStr;
			RpcStringFree(&guidStr);
		}
	}
	if (m_ListID.IsEmpty())
	{
		m_csLastError = _T("Could not generate Message-ID");
		return FALSE;
	}
	csSubject.AppendFormat(_T("Message-ID: <%s@%s>\r\n"), (LPCTSTR)m_ListID, (LPCTSTR)hostname);
	csSubject.AppendFormat(_T("X-Mailer: TortoiseGit\r\nMIME-Version: 1.0\r\nContent-Type: %s\r\n\r\n"), (LPCTSTR)m_csMIMEContentType);

	return Send(csSubject);
}

BOOL CHwSMTP::SendBody()
{
	CString csBody, csTemp;

	if ( m_StrAryAttach.GetSize() > 0 )
	{
		csBody.AppendFormat(_T("%s\r\n\r\n"), (LPCTSTR)m_csNoMIMEText);
		csBody.AppendFormat(_T("--%s\r\n"), (LPCTSTR)m_csPartBoundary);
		csBody.AppendFormat(_T("Content-Type: text/plain\r\n%sContent-Transfer-Encoding: UTF-8\r\n\r\n"), m_csCharSet);
	}

	//csTemp.Format ( _T("%s\r\n"), m_csBody );
	csBody += m_csBody;
	csBody += _T("\r\n");

	return Send(csBody);
}

BOOL CHwSMTP::SendAttach()
{
	int nCountAttach = (int)m_StrAryAttach.GetSize();
	if ( nCountAttach < 1 ) return TRUE;

	for ( int i=0; i<nCountAttach; i++ )
	{
		if ( !SendOnAttach ( m_StrAryAttach.GetAt(i) ) )
			return FALSE;
	}

	Send(L"--" + m_csPartBoundary + L"--\r\n");

	return TRUE;
}

BOOL CHwSMTP::SendOnAttach(LPCTSTR lpszFileName)
{
	ASSERT ( lpszFileName );
	CString csAttach, csTemp;

	csTemp = lpszFileName;
	CString csShortFileName = csTemp.GetBuffer(0) + csTemp.ReverseFind ( '\\' );
	csShortFileName.TrimLeft ( _T("\\") );

	csAttach.AppendFormat(_T("--%s\r\n"), (LPCTSTR)m_csPartBoundary);
	csAttach.AppendFormat(_T("Content-Type: application/octet-stream; file=%s\r\n"), (LPCTSTR)csShortFileName);
	csAttach.AppendFormat(_T("Content-Transfer-Encoding: base64\r\n"));
	csAttach.AppendFormat(_T("Content-Disposition: attachment; filename=%s\r\n\r\n"), (LPCTSTR)csShortFileName);

	DWORD dwFileSize =  hwGetFileAttr(lpszFileName);
	if ( dwFileSize > 5*1024*1024 )
	{
		m_csLastError.Format ( _T("File [%s] too big. File size is : %s"), lpszFileName, FormatBytes(dwFileSize) );
		return FALSE;
	}
	auto pBuf = std::make_unique<char[]>(dwFileSize + 1);
	if (!pBuf)
		::AfxThrowMemoryException();

	if (!Send(csAttach))
		return FALSE;

	CFile file;
	CStringA filedata;
	try
	{
		if ( !file.Open ( lpszFileName, CFile::modeRead ) )
		{
			m_csLastError.Format ( _T("Open file [%s] failed"), lpszFileName );			
			return FALSE;
		}
		UINT nFileLen = file.Read(pBuf.get(), dwFileSize);
		filedata = EncodeBase64(pBuf.get(), nFileLen);
		filedata += _T("\r\n\r\n");
	}
	catch (CFileException *e)
	{
		e->Delete();
		m_csLastError.Format ( _T("Read file [%s] failed"), lpszFileName );
		return FALSE;
	}

	if (!SendBuffer(filedata))
		return FALSE;

	return TRUE;
}

CString CHwSMTP::GetLastErrorText()
{
	return m_csLastError;
}


CString FormatDateTime (COleDateTime &DateTime, LPCTSTR /*pFormat*/)
{
	// If null, return empty string
	if ( DateTime.GetStatus() == COleDateTime::null || DateTime.GetStatus() == COleDateTime::invalid )
		return _T("");

	UDATE ud;
	if (S_OK != VarUdateFromDate(DateTime.m_dt, 0, &ud))
	{
		return _T("");
	}

	TCHAR *weeks[]={_T("Sun"),_T("Mon"),_T("Tue"),_T("Wen"),_T("Thu"),_T("Fri"),_T("Sat")};
	TCHAR *month[]={_T("Jan"),_T("Feb"),_T("Mar"),_T("Apr"),
					_T("May"),_T("Jun"),_T("Jul"),_T("Aug"),
					_T("Sep"),_T("Oct"),_T("Nov"),_T("Dec")};

	TIME_ZONE_INFORMATION stTimeZone;
	GetTimeZoneInformation(&stTimeZone);

	CString strDate;
	strDate.Format(_T("%s, %d %s %d %02d:%02d:%02d %c%04d")
		,weeks[ud.st.wDayOfWeek],
		ud.st.wDay,month[ud.st.wMonth-1],ud.st.wYear,ud.st.wHour,
		ud.st.wMinute,ud.st.wSecond,
		stTimeZone.Bias>0?_T('-'):_T('+'),
		abs(stTimeZone.Bias*10/6)
		);
	return strDate;
}

int hwGetFileAttr ( LPCTSTR lpFileName, OUT CFileStatus *pFileStatus/*=NULL*/ )
{
	if ( !lpFileName || lstrlen(lpFileName) < 1 ) return -1;

	CFileStatus fileStatus;
	fileStatus.m_attribute = 0;
	fileStatus.m_size = 0;
	memset ( fileStatus.m_szFullName, 0, sizeof(fileStatus.m_szFullName) );
	BOOL bRet = FALSE;
	TRY
	{
		if ( CFile::GetStatus(lpFileName,fileStatus) )
		{
			bRet = TRUE;
		}
	}
	CATCH (CFileException, e)
	{
		ASSERT ( FALSE );
		bRet = FALSE;
	}
	CATCH_ALL(e)
	{
		ASSERT ( FALSE );
		bRet = FALSE;
	}
	END_CATCH_ALL;

	if ( pFileStatus )
	{
		pFileStatus->m_ctime = fileStatus.m_ctime;
		pFileStatus->m_mtime = fileStatus.m_mtime;
		pFileStatus->m_atime = fileStatus.m_atime;
		pFileStatus->m_size = fileStatus.m_size;
		pFileStatus->m_attribute = fileStatus.m_attribute;
		lstrcpy ( pFileStatus->m_szFullName, fileStatus.m_szFullName );

	}

	return (int)fileStatus.m_size;
}

CString FormatBytes ( double fBytesNum, BOOL bShowUnit/*=TRUE*/, int nFlag/*=0*/ )
{
	CString csRes;
	if ( nFlag == 0 )
	{
		if ( fBytesNum >= 1024.0 && fBytesNum < 1024.0*1024.0 )
			csRes.Format ( _T("%.2f%s"), fBytesNum / 1024.0, bShowUnit?_T(" K"):_T("") );
		else if ( fBytesNum >= 1024.0*1024.0 && fBytesNum < 1024.0*1024.0*1024.0 )
			csRes.Format ( _T("%.2f%s"), fBytesNum / (1024.0*1024.0), bShowUnit?_T(" M"):_T("") );
		else if ( fBytesNum >= 1024.0*1024.0*1024.0 )
			csRes.Format ( _T("%.2f%s"), fBytesNum / (1024.0*1024.0*1024.0), bShowUnit?_T(" G"):_T("") );
		else
			csRes.Format ( _T("%.2f%s"), fBytesNum, bShowUnit?_T(" B"):_T("") );
	}
	else if ( nFlag == 1 )
	{
		csRes.Format ( _T("%.2f%s"), fBytesNum / 1024.0, bShowUnit?_T(" K"):_T("") );
	}
	else if ( nFlag == 2 )
	{
		csRes.Format ( _T("%.2f%s"), fBytesNum / (1024.0*1024.0), bShowUnit?_T(" M"):_T("") );
	}
	else if ( nFlag == 3 )
	{
		csRes.Format ( _T("%.2f%s"), fBytesNum / (1024.0*1024.0*1024.0), bShowUnit?_T(" G"):_T("") );
	}

	return csRes;
}
