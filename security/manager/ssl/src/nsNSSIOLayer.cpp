/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Mozilla Public
 * License Version 1.1 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy of
 * the License at http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS
 * IS" basis, WITHOUT WARRANTY OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation. All
 * Rights Reserved.
 *
 * Contributor(s):
 *  Brian Ryner <bryner@netscape.com>
 */

#include "nsNSSIOLayer.h"
#include "nsNSSCallbacks.h"

#include "nsString.h"
#include "prlog.h"
#include "nsISecurityManagerComponent.h"
#include "nsIServiceManager.h"
#include "nsIWebProgressListener.h"

#include "ssl.h"

//#define DEBUG_SSL_VERBOSE

static nsISecurityManagerComponent* gNSSService = nsnull;
static PRBool firstTime = PR_TRUE;
static PRDescIdentity nsSSLIOLayerIdentity;
static PRIOMethods nsSSLIOLayerMethods;

#ifdef PR_LOGGING
extern PRLogModuleInfo* gPIPNSSLog;
#endif

nsNSSSocketInfo::nsNSSSocketInfo()
  : mSecurityState(nsIWebProgressListener::STATE_IS_INSECURE),
    mForceHandshake(PR_FALSE),
    mUseTLS(PR_FALSE)
{ 
  NS_INIT_ISUPPORTS();
}

nsNSSSocketInfo::~nsNSSSocketInfo()
{
}

NS_IMPL_THREADSAFE_ISUPPORTS2(nsNSSSocketInfo,
                              nsIChannelSecurityInfo,
                              nsISSLSocketControl)

NS_IMETHODIMP
nsNSSSocketInfo::GetHostName(char * *aHostName)
{
  if (mHostName.IsEmpty())
    *aHostName = nsnull;
  else
    *aHostName = mHostName.ToNewCString();
  return NS_OK;
}


nsresult 
nsNSSSocketInfo::SetHostName(const char *aHostName)
{
  mHostName.AssignWithConversion(aHostName);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSSocketInfo::GetHostPort(PRInt32 *aPort)
{
  *aPort = mHostPort;
  return NS_OK;
}

nsresult 
nsNSSSocketInfo::SetHostPort(PRInt32 aPort)
{
  mHostPort = aPort;
  return NS_OK;
}


NS_IMETHODIMP
nsNSSSocketInfo::GetProxyName(char** aName)
{
  if (mProxyName.IsEmpty())
    *aName = nsnull;
  else
    *aName = mProxyName.ToNewCString();
  return NS_OK;
}


nsresult 
nsNSSSocketInfo::SetProxyName(const char* aName)
{
  mProxyName.AssignWithConversion(aName);
  return NS_OK;
}


NS_IMETHODIMP 
nsNSSSocketInfo::GetProxyPort(PRInt32* aPort)
{
  *aPort = mProxyPort;
  return NS_OK;
}

nsresult
nsNSSSocketInfo::SetProxyPort(PRInt32 aPort)
{
  mProxyPort = aPort;
  return NS_OK;
}

NS_IMETHODIMP
nsNSSSocketInfo::GetSecurityState(PRInt32* state)
{
  *state = mSecurityState;
  return NS_OK;
}

nsresult
nsNSSSocketInfo::SetSecurityState(PRInt32 aState)
{
  mSecurityState = aState;
  return NS_OK;
}

NS_IMETHODIMP
nsNSSSocketInfo::GetShortSecurityDescription(PRUnichar** aText) {
  if (mShortDesc.IsEmpty())
    *aText = nsnull;
  else
    *aText = mShortDesc.ToNewUnicode();
  return NS_OK;
}

nsresult
nsNSSSocketInfo::SetShortSecurityDescription(const PRUnichar* aText) {
  mShortDesc.Assign(aText);
  return NS_OK;
}

NS_IMETHODIMP
nsNSSSocketInfo::GetForceHandshake(PRBool* forceHandshake)
{
  *forceHandshake = mForceHandshake;
  return NS_OK;
}

NS_IMETHODIMP
nsNSSSocketInfo::SetForceHandshake(PRBool forceHandshake)
{
  mForceHandshake = forceHandshake;
  return NS_OK;
}

nsresult
nsNSSSocketInfo::GetUseTLS(PRBool* aResult)
{
  *aResult = mUseTLS;
  return NS_OK;
}

nsresult
nsNSSSocketInfo::SetUseTLS(PRBool useTLS)
{
  mUseTLS = useTLS;
  return NS_OK;
}

NS_IMETHODIMP
nsNSSSocketInfo::ProxyStepUp()
{
  return NS_OK;
}

NS_IMETHODIMP
nsNSSSocketInfo::TLSStepUp()
{
  return NS_OK;
}

static PRStatus PR_CALLBACK
nsSSLIOLayerConnect(PRFileDesc* fd, const PRNetAddr* addr,
                    PRIntervalTime timeout)
{
  if (!fd || !fd->lower)
    return PR_FAILURE;
  
  PRStatus status = PR_SUCCESS;
  
  // Due to limitations in NSPR 4.0, we must execute this entire connect
  // as a blocking operation.
  
  PRSocketOptionData sockopt;
  sockopt.option = PR_SockOpt_Nonblocking;
  PR_GetSocketOption(fd, &sockopt);
  
  PRBool nonblock = sockopt.value.non_blocking;
  sockopt.option = PR_SockOpt_Nonblocking;
  sockopt.value.non_blocking = PR_FALSE;
  PR_SetSocketOption(fd, &sockopt);
  
  nsNSSSocketInfo* infoObject = (nsNSSSocketInfo*) fd->secret;
  
  status = fd->lower->methods->connect(fd->lower, addr, 
                                       PR_INTERVAL_NO_TIMEOUT);
  if (status != PR_SUCCESS) {
    PR_LOG(gPIPNSSLog, PR_LOG_ERROR, ("[%p] Lower layer connect error: %d\n",
                                      (void*)fd, PR_GetError()));
    goto loser;
  }
  
  PRBool forceHandshake, useTLS;
  infoObject->GetForceHandshake(&forceHandshake);
  infoObject->GetUseTLS(&useTLS);
  
  PR_LOG(gPIPNSSLog, PR_LOG_DEBUG, ("[%p] Connect: forceHandshake = %d, useTLS = %d\n",
                                    (void*)fd, forceHandshake, useTLS));

  if (!useTLS && forceHandshake) {
    PRInt32 res = SSL_ForceHandshake(fd);
    
    if (res == -1) {
      PR_LOG(gPIPNSSLog, PR_LOG_ERROR, ("[%p] ForceHandshake failure -- error %d\n",
                                        (void*)fd, PR_GetError()));
      status = PR_FAILURE;
    }
  }

 loser:
  sockopt.option = PR_SockOpt_Nonblocking;
  sockopt.value.non_blocking = nonblock;
  PR_SetSocketOption(fd, &sockopt);
  
  return status;
}

static PRStatus PR_CALLBACK
nsSSLIOLayerClose(PRFileDesc *fd)
{
  if (!fd)
    return PR_FAILURE;

  PR_LOG(gPIPNSSLog, PR_LOG_DEBUG, ("[%p] Shutting down socket\n", (void*)fd));
  
  PRFileDesc* popped = PR_PopIOLayer(fd, PR_TOP_IO_LAYER);
  PRStatus status = fd->methods->close(fd);
  if (status != PR_SUCCESS) return status;
  
  popped->identity = PR_INVALID_IO_LAYER;
  nsNSSSocketInfo *infoObject = (nsNSSSocketInfo*) popped->secret;
  NS_RELEASE(infoObject);
  
  return status;
}

#ifdef DEBUG_SSL_VERBOSE

static PRInt32 PR_CALLBACK
nsSSLIOLayerRead(PRFileDesc* fd, void* buf, PRInt32 amount)
{
  if (!fd || !fd->lower)
    return PR_FAILURE;

  PRInt32 bytesRead = fd->lower->methods->read(fd->lower, buf, amount);
  PR_LOG(gPIPNSSLog, PR_LOG_DEBUG, ("[%p] read %d bytes\n", (void*)fd, bytesRead));
  return bytesRead;
}

static PRInt32 PR_CALLBACK
nsSSLIOLayerWrite(PRFileDesc* fd, const void* buf, PRInt32 amount)
{
  if (!fd || !fd->lower)
    return PR_FAILURE;

  PRInt32 bytesWritten = fd->lower->methods->write(fd->lower, buf, amount);
  PR_LOG(gPIPNSSLog, PR_LOG_DEBUG, ("[%p] wrote %d bytes\n", (void*)fd, bytesWritten));
  return bytesWritten;
}

#endif // DEBUG_SSL_VERBOSE

nsresult InitNSSMethods()
{
  nsSSLIOLayerIdentity = PR_GetUniqueIdentity("NSS layer");
  nsSSLIOLayerMethods  = *PR_GetDefaultIOMethods();
  
  nsSSLIOLayerMethods.connect = nsSSLIOLayerConnect;
  nsSSLIOLayerMethods.close = nsSSLIOLayerClose;

#ifdef DEBUG_SSL_VERBOSE
  nsSSLIOLayerMethods.read = nsSSLIOLayerRead;
  nsSSLIOLayerMethods.write = nsSSLIOLayerWrite;
#endif
  
  nsresult rv;
  /* This performs NSS initialization for us */
  rv = nsServiceManager::GetService(PSM_COMPONENT_CONTRACTID,
                                    NS_GET_IID(nsISecurityManagerComponent),
                                    (nsISupports**)&gNSSService);
  return rv;
}

nsresult
nsSSLIOLayerNewSocket(const char *host,
                      PRInt32 port,
                      const char *proxyHost,
                      PRInt32 proxyPort,
                      PRFileDesc **fd,
                      nsISupports** info,
                      PRBool useTLS)
{
  if (firstTime) {
    nsresult rv = InitNSSMethods();
    if (NS_FAILED(rv)) return rv;
    firstTime = PR_FALSE;
  }

  PRFileDesc* sock = PR_OpenTCPSocket(PR_AF_INET6);
  if (!sock) return NS_ERROR_OUT_OF_MEMORY;

  nsresult rv = nsSSLIOLayerAddToSocket(host, port, proxyHost, proxyPort,
                                        sock, info, useTLS);
  if (NS_FAILED(rv)) {
    PR_Close(sock);
    return rv;
  }

  *fd = sock;
  return NS_OK;
}


nsresult
nsSSLIOLayerAddToSocket(const char* host,
                        PRInt32 port,
                        const char* proxyHost,
                        PRInt32 proxyPort,
                        PRFileDesc* fd,
                        nsISupports** info,
                        PRBool useTLS)
{
  if (firstTime) {
    nsresult rv = InitNSSMethods();
    if (NS_FAILED(rv)) return rv;
    firstTime = PR_FALSE;
  }

  PRFileDesc* sslSock = SSL_ImportFD(nsnull, fd);
  if (!sslSock) {
    NS_ASSERTION(PR_FALSE, "NSS: Error importing socket");
    return NS_ERROR_FAILURE;
  }

  SSL_SetPKCS11PinArg(sslSock, nsnull);
  SSL_HandshakeCallback(sslSock, HandshakeCallback, nsnull);
  SSL_GetClientAuthDataHook(sslSock, (SSLGetClientAuthData)NSS_GetClientAuthData,
                            nsnull);

  PRInt32 ret = SSL_SetURL(sslSock, host);
  if (ret == -1) {
    NS_ASSERTION(PR_FALSE, "NSS: Error setting server name");
    return NS_ERROR_FAILURE;
  }
  
  nsNSSSocketInfo* infoObject = new nsNSSSocketInfo();
  if (!infoObject) return NS_ERROR_FAILURE;
  
  NS_ADDREF(infoObject);
  
  infoObject->SetHostName(host);
  infoObject->SetHostPort(port);
  infoObject->SetProxyName(proxyHost);
  infoObject->SetProxyPort(proxyPort);
  infoObject->SetUseTLS(useTLS);

  /* Now, layer ourselves on top of the SSL socket... */
  PRFileDesc* layer = PR_CreateIOLayerStub(nsSSLIOLayerIdentity,
                                           &nsSSLIOLayerMethods);
  if (!layer)
    return NS_ERROR_FAILURE;
  
  layer->secret = (PRFilePrivate*) infoObject;
  nsresult rv = PR_PushIOLayer(sslSock, PR_GetLayersIdentity(sslSock), layer);
  
  if (NS_FAILED(rv)) {
    NS_RELEASE(infoObject);
    PR_DELETE(layer);
    return NS_ERROR_FAILURE;
  }

  PR_LOG(gPIPNSSLog, PR_LOG_DEBUG, ("[%p] Socket set up\n", (void*)sslSock));
  infoObject->QueryInterface(NS_GET_IID(nsISupports), (void**) (info));
  return NS_OK;
}
  
