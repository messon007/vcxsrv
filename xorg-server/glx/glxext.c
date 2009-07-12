/*
** The contents of this file are subject to the GLX Public License Version 1.0
** (the "License"). You may not use this file except in compliance with the
** License. You may obtain a copy of the License at Silicon Graphics, Inc.,
** attn: Legal Services, 2011 N. Shoreline Blvd., Mountain View, CA 94043
** or at http://www.sgi.com/software/opensource/glx/license.html.
**
** Software distributed under the License is distributed on an "AS IS"
** basis. ALL WARRANTIES ARE DISCLAIMED, INCLUDING, WITHOUT LIMITATION, ANY
** IMPLIED WARRANTIES OF MERCHANTABILITY, OF FITNESS FOR A PARTICULAR
** PURPOSE OR OF NON- INFRINGEMENT. See the License for the specific
** language governing rights and limitations under the License.
**
** The Original Software is GLX version 1.2 source code, released February,
** 1999. The developer of the Original Software is Silicon Graphics, Inc.
** Those portions of the Subject Software created by Silicon Graphics, Inc.
** are Copyright (c) 1991-9 Silicon Graphics, Inc. All Rights Reserved.
**
*/

#define NEED_REPLIES
#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#else

#include "glheader.h"

#endif

#include <string.h>
#include "glxserver.h"
#include <windowstr.h>
#include <propertyst.h>
#include "privates.h"
#include <os.h>
#include "g_disptab.h"
#include "unpack.h"
#include "glxutil.h"
#include "glxext.h"
#include "indirect_table.h"
#include "indirect_util.h"

/*
** The last context used by the server.  It is the context that is current
** from the server's perspective.
*/
__GLXcontext *__glXLastContext;

/*
** X resources.
*/
RESTYPE __glXContextRes;
RESTYPE __glXDrawableRes;
RESTYPE __glXSwapBarrierRes;

/*
** Reply for most singles.
*/
xGLXSingleReply __glXReply;

static DevPrivateKey glxClientPrivateKey = &glxClientPrivateKey;

/*
** Client that called into GLX dispatch.
*/
ClientPtr __pGlxClient;

/*
** Forward declarations.
*/
static int __glXDispatch(ClientPtr);

/*
** Called when the extension is reset.
*/
static void ResetExtension(ExtensionEntry* extEntry)
{
    __glXFlushContextCache();
}

/*
** Reset state used to keep track of large (multi-request) commands.
*/
void __glXResetLargeCommandStatus(__GLXclientState *cl)
{
    cl->largeCmdBytesSoFar = 0;
    cl->largeCmdBytesTotal = 0;
    cl->largeCmdRequestsSoFar = 0;
    cl->largeCmdRequestsTotal = 0;
}

/*
** This procedure is called when the client who created the context goes
** away OR when glXDestroyContext is called.  In either case, all we do is
** flag that the ID is no longer valid, and (maybe) free the context.
** use.
*/
static int ContextGone(__GLXcontext* cx, XID id)
{
    cx->idExists = GL_FALSE;
    if (!cx->isCurrent) {
	__glXFreeContext(cx);
    }

    return True;
}

/*
** Destroy routine that gets called when a drawable is freed.  A drawable
** contains the ancillary buffers needed for rendering.
*/
static Bool DrawableGone(__GLXdrawable *glxPriv, XID xid)
{
    ScreenPtr pScreen = glxPriv->pDraw->pScreen;

    switch (glxPriv->type) {
	case GLX_DRAWABLE_PIXMAP:
	case GLX_DRAWABLE_PBUFFER:
	    (*pScreen->DestroyPixmap)((PixmapPtr) glxPriv->pDraw);
	    break;
    }

    glxPriv->pDraw = NULL;
    glxPriv->drawId = 0;
    __glXUnrefDrawable(glxPriv);

    return True;
}

static __GLXcontext *glxPendingDestroyContexts;
static int glxServerLeaveCount;
static int glxBlockClients;

/*
** Free a context.
*/
GLboolean __glXFreeContext(__GLXcontext *cx)
{
    if (cx->idExists || cx->isCurrent) return GL_FALSE;
    
    if (cx->feedbackBuf) xfree(cx->feedbackBuf);
    if (cx->selectBuf) xfree(cx->selectBuf);
    if (cx == __glXLastContext) {
	__glXFlushContextCache();
    }

    /* We can get here through both regular dispatching from
     * __glXDispatch() or as a callback from the resource manager.  In
     * the latter case we need to lift the DRI lock manually. */

    if (!glxBlockClients) {
	__glXleaveServer(GL_FALSE);
	cx->destroy(cx);
	__glXenterServer(GL_FALSE);
    } else {
	cx->next = glxPendingDestroyContexts;
	glxPendingDestroyContexts = cx;
    }

    return GL_TRUE;
}

extern RESTYPE __glXSwapBarrierRes;

static int SwapBarrierGone(int screen, XID drawable)
{
    __GLXscreen *pGlxScreen = glxGetScreen(screenInfo.screens[screen]);

    if (pGlxScreen->swapBarrierFuncs) {
        pGlxScreen->swapBarrierFuncs->bindSwapBarrierFunc(screen, drawable, 0);
    }
    FreeResourceByType(drawable, __glXSwapBarrierRes, FALSE);
    return True;
}

/************************************************************************/

/*
** These routines can be used to check whether a particular GL command
** has caused an error.  Specifically, we use them to check whether a
** given query has caused an error, in which case a zero-length data
** reply is sent to the client.
*/

static GLboolean errorOccured = GL_FALSE;

/*
** The GL was will call this routine if an error occurs.
*/
void __glXErrorCallBack(GLenum code)
{
    errorOccured = GL_TRUE;
}

/*
** Clear the error flag before calling the GL command.
*/
void __glXClearErrorOccured(void)
{
    errorOccured = GL_FALSE;
}

/*
** Check if the GL command caused an error.
*/
GLboolean __glXErrorOccured(void)
{
    return errorOccured;
}

static int __glXErrorBase;

int __glXError(int error)
{
    return __glXErrorBase + error;
}

__GLXclientState *
glxGetClient(ClientPtr pClient)
{
    return dixLookupPrivate(&pClient->devPrivates, glxClientPrivateKey);
}

static void
glxClientCallback (CallbackListPtr	*list,
		   pointer		closure,
		   pointer		data)
{
    NewClientInfoRec	*clientinfo = (NewClientInfoRec *) data;
    ClientPtr		pClient = clientinfo->client;
    __GLXclientState	*cl = glxGetClient(pClient);
    __GLXcontext	*cx;
    int i;

    switch (pClient->clientState) {
    case ClientStateRunning:
	/*
	** By default, assume that the client supports
	** GLX major version 1 minor version 0 protocol.
	*/
	cl->GLClientmajorVersion = 1;
	cl->GLClientminorVersion = 0;
	cl->client = pClient;
	break;

    case ClientStateGone:
	for (i = 0; i < cl->numCurrentContexts; i++) {
	    cx = cl->currentContexts[i];
	    if (cx) {
		cx->isCurrent = GL_FALSE;
		if (!cx->idExists)
		    __glXFreeContext(cx);
	    }
	}

	if (cl->returnBuf) xfree(cl->returnBuf);
	if (cl->largeCmdBuf) xfree(cl->largeCmdBuf);
	if (cl->currentContexts) xfree(cl->currentContexts);
	if (cl->GLClientextensions) xfree(cl->GLClientextensions);
	break;

    default:
	break;
    }
}

/************************************************************************/

static __GLXprovider *__glXProviderStack;

void GlxPushProvider(__GLXprovider *provider)
{
    provider->next = __glXProviderStack;
    __glXProviderStack = provider;
}

/*
** Initialize the GLX extension.
*/
void GlxExtensionInit(void)
{
    ExtensionEntry *extEntry;
    ScreenPtr pScreen;
    int i;
    __GLXprovider *p;
    Bool glx_provided = False;

    __glXContextRes = CreateNewResourceType((DeleteType)ContextGone);
    __glXDrawableRes = CreateNewResourceType((DeleteType)DrawableGone);
    __glXSwapBarrierRes = CreateNewResourceType((DeleteType)SwapBarrierGone);

    if (!dixRequestPrivate(glxClientPrivateKey, sizeof (__GLXclientState)))
	return;
    if (!AddCallback (&ClientStateCallback, glxClientCallback, 0))
	return;

    for (i = 0; i < screenInfo.numScreens; i++) {
	pScreen = screenInfo.screens[i];

	for (p = __glXProviderStack; p != NULL; p = p->next) {
	    if (p->screenProbe(pScreen) != NULL) {
		LogMessage(X_INFO,
			   "GLX: Initialized %s GL provider for screen %d\n",
			   p->name, i);
		break;
	    }
	}

	if (!p)
	    LogMessage(X_INFO,
		       "GLX: no usable GL providers found for screen %d\n", i);
	else
	    glx_provided = True;
    }

    /* don't register extension if GL is not provided on any screen */
    if (!glx_provided)
	return;

    /*
    ** Add extension to server extensions.
    */
    extEntry = AddExtension(GLX_EXTENSION_NAME, __GLX_NUMBER_EVENTS,
			    __GLX_NUMBER_ERRORS, __glXDispatch,
			    __glXDispatch, ResetExtension,
			    StandardMinorOpcode);
    if (!extEntry) {
	FatalError("__glXExtensionInit: AddExtensions failed\n");
	return;
    }
    if (!AddExtensionAlias(GLX_EXTENSION_ALIAS, extEntry)) {
	ErrorF("__glXExtensionInit: AddExtensionAlias failed\n");
	return;
    }

    __glXErrorBase = extEntry->errorBase;
}

/************************************************************************/

void __glXFlushContextCache(void)
{
    __glXLastContext = 0;
}

/*
** Make a context the current one for the GL (in this implementation, there
** is only one instance of the GL, and we use it to serve all GL clients by
** switching it between different contexts).  While we are at it, look up
** a context by its tag and return its (__GLXcontext *).
*/
__GLXcontext *__glXForceCurrent(__GLXclientState *cl, GLXContextTag tag,
				int *error)
{
    __GLXcontext *cx;

    /*
    ** See if the context tag is legal; it is managed by the extension,
    ** so if it's invalid, we have an implementation error.
    */
    cx = (__GLXcontext *) __glXLookupContextByTag(cl, tag);
    if (!cx) {
	cl->client->errorValue = tag;
	*error = __glXError(GLXBadContextTag);
	return 0;
    }

    if (!cx->isDirect) {
	if (cx->drawPriv == NULL) {
	    /*
	    ** The drawable has vanished.  It must be a window, because only
	    ** windows can be destroyed from under us; GLX pixmaps are
	    ** refcounted and don't go away until no one is using them.
	    */
	    *error = __glXError(GLXBadCurrentWindow);
	    return 0;
    	}
    }
    
    if (cx == __glXLastContext) {
	/* No need to re-bind */
	return cx;
    }

    /* Make this context the current one for the GL. */
    if (!cx->isDirect) {
	if (!(*cx->forceCurrent)(cx)) {
	    /* Bind failed, and set the error code.  Bummer */
	    cl->client->errorValue = cx->id;
	    *error = __glXError(GLXBadContextState);
	    return 0;
    	}
    }
    __glXLastContext = cx;
    return cx;
}

/************************************************************************/

void glxSuspendClients(void)
{
    int i;

    for (i = 1; i < currentMaxClients; i++) {
	if (clients[i] && glxGetClient(clients[i])->inUse)
	    IgnoreClient(clients[i]);
    }

    glxBlockClients = TRUE;
}

void glxResumeClients(void)
{
    __GLXcontext *cx, *next;
    int i;

    glxBlockClients = FALSE;

    for (i = 1; i < currentMaxClients; i++) {
	if (clients[i] && glxGetClient(clients[i])->inUse)
	    AttendClient(clients[i]);
    }

    __glXleaveServer(GL_FALSE);
    for (cx = glxPendingDestroyContexts; cx != NULL; cx = next) {
	next = cx->next;

	cx->destroy(cx);
    }
    glxPendingDestroyContexts = NULL;
    __glXenterServer(GL_FALSE);
}

static void
__glXnopEnterServer(GLboolean rendering)
{
}
    
static void
__glXnopLeaveServer(GLboolean rendering)
{
}

static void (*__glXenterServerFunc)(GLboolean) = __glXnopEnterServer;
static void (*__glXleaveServerFunc)(GLboolean)  = __glXnopLeaveServer;

void __glXsetEnterLeaveServerFuncs(void (*enter)(GLboolean),
				   void (*leave)(GLboolean))
{
  __glXenterServerFunc = enter;
  __glXleaveServerFunc = leave;
}


void __glXenterServer(GLboolean rendering)
{
  glxServerLeaveCount--;

  if (glxServerLeaveCount == 0)
    (*__glXenterServerFunc)(rendering);
}

void __glXleaveServer(GLboolean rendering)
{
  if (glxServerLeaveCount == 0)
    (*__glXleaveServerFunc)(rendering);

  glxServerLeaveCount++;
}

/*
** Top level dispatcher; all commands are executed from here down.
*/
static int __glXDispatch(ClientPtr client)
{
    REQUEST(xGLXSingleReq);
    CARD8 opcode;
    __GLXdispatchSingleProcPtr proc;
    __GLXclientState *cl;
    int retval;

    opcode = stuff->glxCode;
    cl = glxGetClient(client);
    /* Mark it in use so we suspend it on VT switch. */
    cl->inUse = TRUE;

    /*
    ** If we're expecting a glXRenderLarge request, this better be one.
    */
    if ((cl->largeCmdRequestsSoFar != 0) && (opcode != X_GLXRenderLarge)) {
	client->errorValue = stuff->glxCode;
	return __glXError(GLXBadLargeRequest);
    }

    /* If we're currently blocking GLX clients, just put this guy to
     * sleep, reset the request and return. */
    if (glxBlockClients) {
	ResetCurrentRequest(client);
	client->sequence--;
	IgnoreClient(client);
	return(client->noClientException);
    }

    /*
    ** Use the opcode to index into the procedure table.
    */
    proc = (__GLXdispatchSingleProcPtr) __glXGetProtocolDecodeFunction(& Single_dispatch_info,
								       opcode,
								       client->swapped);
    if (proc != NULL) {
	GLboolean rendering = opcode <= X_GLXRenderLarge;
	__glXleaveServer(rendering);

	__pGlxClient = client;

	retval = (*proc)(cl, (GLbyte *) stuff);

	__glXenterServer(rendering);
    }
    else {
	retval = BadRequest;
    }

    return retval;
}
