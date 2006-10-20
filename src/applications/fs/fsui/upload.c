/*
     This file is part of GNUnet.
     (C) 2001, 2002, 2003, 2004, 2006 Christian Grothoff (and other contributing authors)

     GNUnet is free software; you can redistribute it and/or modify
     it under the terms of the GNU General Public License as published
     by the Free Software Foundation; either version 2, or (at your
     option) any later version.

     GNUnet is distributed in the hope that it will be useful, but
     WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     General Public License for more details.

     You should have received a copy of the GNU General Public License
     along with GNUnet; see the file COPYING.  If not, write to the
     Free Software Foundation, Inc., 59 Temple Place - Suite 330,
     Boston, MA 02111-1307, USA.
*/

/**
 * @file applications/fs/fsui/upload.c
 * @brief upload functions
 * @author Krista Bennett
 * @author Christian Grothoff
 *
 * TODO:
 * - use extractors to obtain metadata and keywords!
 */

#include "platform.h"
#include "gnunet_ecrs_lib.h"
#include "gnunet_uritrack_lib.h"
#include "gnunet_fsui_lib.h"
#include "fsui.h"
#include <extractor.h>

#define DEBUG_UPLOAD NO

/**
 * Transform an ECRS progress callback into an FSUI event.
 */
static void progressCallback(unsigned long long totalBytes,
			     unsigned long long completedBytes,
			     cron_t eta,
			     void * ptr) {
  FSUI_UploadList * utc = ptr;
  FSUI_Event event;
  cron_t now;

  now = get_time();
  event.type = FSUI_upload_progress;
  event.data.UploadProgress.uc.pos = utc;
  event.data.UploadProgress.uc.cctx = utc->cctx;
  event.data.UploadProgress.uc.ppos = utc->parent;
  event.data.UploadProgress.uc.pcctx = utc->parent->cctx;
  event.data.UploadProgress.completed = completedBytes;
  event.data.UploadProgress.total = totalBytes;
  event.data.UploadProgress.filename = utc->filename;
  event.data.UploadProgress.eta = eta;
  utc->completed = completedBytes;
  utc->shared->ctx->ecb(utc->shared->ctx->ecbClosure,
			&event);
}

static int testTerminate(void * cls) {
  FSUI_UploadList * utc = cls;
  if ( (utc->shared->force_termination != NO) ||
       (utc->state != FSUI_ACTIVE) )
    return SYSERR;
  return OK;
}

/**
 * Take the current directory entries from utc, create
 * a directory, upload it and store the uri in  *uri.
 */
static char *
createDirectoryHelper(struct GE_Context * ectx,
		      struct FSUI_UploadList * children,
		      struct ECRS_MetaData * meta) {
  ECRS_FileInfo * fis;
  unsigned int count;
  unsigned int size;
  char * data;
  unsigned long long len;
  int ret;
  char * tempName;
  struct FSUI_UploadList * pos;
  int handle;

  fis = NULL;
  size = 0;
  count = 0;
  pos = children;
  while (pos != NULL) {
    if (pos->uri != NULL)
      count++;
    pos = pos->next;
  }
  if (count == 0) {
    GE_BREAK(ectx, 0);
    return NULL;
  }
  GROW(fis, 
       size,
       count);
  count = 0;
  pos = children;
  while (pos != NULL) {
    if (pos->uri != NULL) {
      fis[count].uri = pos->uri;
      fis[count].meta = pos->meta;
      count++;
    }
    pos = pos->next;
  }
  GE_BREAK(ectx, count == size);
  ret = ECRS_createDirectory(ectx,
			     &data,
			     &len,
			     size,
			     fis,
			     meta);
  GROW(fis,
       size,
       0);
  if (ret != OK) 
    return NULL;   
  tempName = STRDUP("/tmp/gnunet-upload-dir.XXXXXX");
  handle = mkstemp(tempName);
  if (handle == -1) {
    GE_LOG_STRERROR_FILE(ectx,
			 GE_ERROR | GE_USER | GE_BULK,
			 tempName, 
			 "mkstemp");
    FREE(tempName);
    FREE(data);
    return NULL;
  }
  if (len != WRITE(handle,
		   data,
		   len)) {
    GE_LOG_STRERROR_FILE(ectx,
			 GE_ERROR | GE_USER | GE_BULK,
			 tempName,
			 "write");
    FREE(tempName);
    FREE(data);
    return NULL;    
  }
  CLOSE(handle);
  FREE(data);
  return tempName;
}

/**
 * Signal upload error to client.
 */
static void signalError(FSUI_UploadList * utc,
			const char * message) {
  FSUI_Event event;

  utc->state = FSUI_ERROR;
  event.type = FSUI_upload_error;
  event.data.UploadError.uc.pos = utc;
  event.data.UploadError.uc.cctx = utc->cctx;
  event.data.UploadError.uc.ppos = utc->parent;
  event.data.UploadError.uc.pcctx = utc->parent->cctx;
  event.data.UploadError.message = message;
  utc->shared->ctx->ecb(utc->shared->ctx->ecbClosure,
			&event);		
}

/**
 * Thread that does the upload.
 */
void * FSUI_uploadThread(void * cls) {
  FSUI_UploadList * utc = cls;
  FSUI_UploadList * cpos;
  FSUI_Event event;
  ECRS_FileInfo fi;
  int ret;
  struct GE_Context * ectx;
  char * filename;

  ectx = utc->shared->ctx->ectx;
  GE_ASSERT(ectx, utc->filename != NULL);
  cpos = utc->child;
  while ( (cpos != NULL) &&
	  (utc->shared->force_termination == NO) ) {
    if (cpos->state == FSUI_PENDING)
      FSUI_uploadThread(cpos);
    cpos = cpos->next;
  }
  if (utc->shared->force_termination == YES)
    return NULL; /* aborted */
  if (utc->child != NULL) {
    filename = createDirectoryHelper(ectx,
				     utc->child,
				     utc->meta);
    if (filename == NULL) {
      signalError(utc,
		  _("Failed to create temporary directory."));
      return NULL;
    }      
  } else {
    filename = STRDUP(utc->filename);
  }  
  utc->start_time = get_time();
  utc->state = FSUI_ACTIVE;
    
  ret = ECRS_uploadFile(utc->shared->ctx->ectx,
			utc->shared->ctx->cfg,
			filename,
			utc->shared->doIndex,
			utc->shared->anonymityLevel,
			utc->shared->priority,
			utc->shared->expiration,
			&progressCallback,
			utc,
			&testTerminate,
			utc,
			&utc->uri);
  if (ret != OK) {
    signalError(utc,
		_("Upload failed (consult logs)."));
    if (utc->child != NULL) 
      UNLINK(filename);
    FREE(filename);
    return NULL;
  }

  /* FIXME: metadata extraction! */

  utc->state = FSUI_COMPLETED;
  ECRS_delFromMetaData(utc->meta,
		       EXTRACTOR_FILENAME,
		       NULL);
  ECRS_addToMetaData(utc->meta,
		     EXTRACTOR_FILENAME,
		     utc->filename);  
  ECRS_delFromMetaData(utc->meta,
		       EXTRACTOR_SPLIT,
		       NULL);
  fi.meta = utc->meta;
  fi.uri = utc->uri;
  URITRACK_trackURI(ectx,
		    utc->shared->ctx->cfg,
		    &fi);
  if (utc->shared->global_keywords != NULL)
    ECRS_addToKeyspace(ectx,
		       utc->shared->ctx->cfg,
		       utc->shared->global_keywords,
		       utc->shared->anonymityLevel,
		       utc->shared->priority,
		       utc->shared->expiration,
		       utc->uri,
		       utc->meta);	
  if (utc->keywords != NULL)
    ECRS_addToKeyspace(ectx,
		       utc->shared->ctx->cfg,
		       utc->keywords,
		       utc->shared->anonymityLevel,
		       utc->shared->priority,
		       utc->shared->expiration,
		       utc->uri,
		       utc->meta);	    
  event.type = FSUI_upload_complete;
  event.data.UploadCompleted.uc.pos = utc;
  event.data.UploadCompleted.uc.cctx = utc->cctx;
  event.data.UploadCompleted.uc.ppos = utc->parent;
  event.data.UploadCompleted.uc.pcctx = utc->parent->cctx;
  event.data.UploadCompleted.total = utc->total;
  event.data.UploadCompleted.filename = utc->filename;
  event.data.UploadCompleted.uri = utc->uri;
  utc->shared->ctx->ecb(utc->shared->ctx->ecbClosure,
		&event);		
  if (utc->child != NULL) 
    UNLINK(filename);
  FREE(filename);
  return NULL;
}

static void freeUploadList(struct FSUI_UploadList * ul) {
  struct FSUI_UploadList * next;

  while (ul->child != NULL)
    freeUploadList(ul->child);
  FREE(ul->filename);
  if (ul->keywords != NULL)
    ECRS_freeUri(ul->keywords);
  if (ul->uri != NULL)
    ECRS_freeUri(ul->uri);
  ECRS_freeMetaData(ul->meta);
  /* unlink from parent */
  next = ul->parent->child;
  if (next == ul) {
    ul->parent->child = ul->next;
  } else {
    while (next->next != ul)
      next = next->next;
    next->next = ul->next;
  }
  FREE(ul);
}

static struct FSUI_UploadList *
addUploads(struct FSUI_UploadShared * shared,
	   const char * filename,	 
	   const struct ECRS_URI * keywords,
	   const struct ECRS_MetaData * md,
	   struct FSUI_UploadList * parent);

static int addChildUpload(const char * name,
			  const char * dirName,
			  void * data) {
  struct FSUI_UploadList * parent = data;
  char * filename;
  struct FSUI_UploadList * child;
  struct ECRS_MetaData * md;

  filename = MALLOC(strlen(dirName) + strlen(name) + 2);
  strcpy(filename, dirName);
  strcat(filename, DIR_SEPARATOR_STR);
  strcat(filename, name);
  md = ECRS_createMetaData();
  child = addUploads(parent->shared,
		     filename,
		     NULL,
		     md,
		     parent);
  FREE(filename);
  ECRS_freeMetaData(md);
  if (child == NULL) 
    return SYSERR;
  GE_ASSERT(NULL, child->next == NULL);
  child->next = parent->child;
  parent->child = child;
  return OK;
}

static struct FSUI_UploadList *
addUploads(struct FSUI_UploadShared * shared,
	   const char * filename,	 
	   const struct ECRS_URI * keywords,
	   const struct ECRS_MetaData * md,
	   struct FSUI_UploadList * parent) {
  FSUI_UploadList * utc;

  utc = MALLOC(sizeof(FSUI_UploadList));
  utc->completed = 0;
  utc->total = 0; /* to be set later */
  utc->start_time = 0;
  utc->shared = shared;
  utc->next = NULL;
  utc->child = NULL;
  utc->parent = parent;
  utc->uri = NULL;
  utc->cctx = NULL; /* to be set later */
  utc->state = FSUI_PENDING;
  if (YES == disk_file_test(shared->ctx->ectx,
			    filename)) {
    /* add this file */
    if (OK != disk_file_size(shared->ctx->ectx,
			     filename,
			     &utc->total,
			     YES)) {
      FREE(utc);
      return NULL;
    }    
    utc->meta = ECRS_dupMetaData(md);
  } else {
    if (SYSERR == shared->dsc(shared->dscClosure,
			      filename,
			      &addChildUpload,
			      utc)) {
      /* error scanning upload directory */
      while (utc->child != NULL)
	freeUploadList(utc->child);
      FREE(utc);
      return NULL;
    }
    utc->meta = ECRS_dupMetaData(md);
    ECRS_addToMetaData(utc->meta,
		       EXTRACTOR_MIMETYPE,
		       GNUNET_DIRECTORY_MIME);
  }
  if (keywords != NULL)
    utc->keywords = ECRS_dupUri(keywords);
  else
    utc->keywords = NULL;
  utc->filename = STRDUP(filename);

  /* finally, link with parent */
  MUTEX_LOCK(shared->ctx->lock);
  utc->next = parent->child;
  parent->child = utc;
  MUTEX_UNLOCK(shared->ctx->lock);
  return utc;
}

static void signalUploadStarted(struct FSUI_UploadList * utc,
				int first_only) {
  FSUI_Event event;
  
  while (utc != NULL) {
    event.type = FSUI_upload_started;
    event.data.UploadStarted.uc.pos = utc;
    event.data.UploadStarted.uc.cctx = utc->cctx;
    event.data.UploadStarted.uc.ppos = utc->parent;
    event.data.UploadStarted.uc.pcctx = utc->parent->cctx;
    event.data.UploadStarted.total = utc->total;
    event.data.UploadStarted.anonymityLevel = utc->shared->anonymityLevel;
    event.data.UploadStarted.filename = utc->filename;
    utc->cctx = utc->shared->ctx->ecb(utc->shared->ctx->ecbClosure,
				      &event);
    signalUploadStarted(utc->child, 0);
    if (first_only)
      break;
    utc = utc->next;
  }  
}

static void signalUploadStopped(struct FSUI_UploadList * ul,
				int first_only) {
  FSUI_Event event;
  
  while (ul != NULL) {
    signalUploadStopped(ul->child, 0);
    event.type = FSUI_upload_stopped;
    event.data.UploadStopped.uc.pos = ul;
    event.data.UploadStopped.uc.cctx = ul->cctx;
    event.data.UploadStopped.uc.ppos = ul->parent;
    event.data.UploadStopped.uc.pcctx = ul->parent->cctx;
    ul->shared->ctx->ecb(ul->shared->ctx->ecbClosure,
			 &event);
    if (first_only)
      break;
    ul = ul->next;
  }  
}

static void freeShared(struct FSUI_UploadShared * shared) {
  ECRS_freeUri(shared->global_keywords);
  EXTRACTOR_removeAll(shared->extractors);
  FREE(shared->extractor_config);
  FREE(shared);
}

/**
 * Start uploading a file.  Note that an upload cannot be stopped once
 * started (not necessary anyway), but it can fail.  The function also
 * automatically the uploaded file in the global keyword space under
 * the given keywords.
 *
 * @return OK on success (at least we started with it),
 *  SYSERR if the file does not exist or gnunetd is not
 *  running
 */
struct FSUI_UploadList *
FSUI_startUpload(struct FSUI_Context * ctx,
		 const char * filename,
		 DirectoryScanCallback dsc,
		 void * dscClosure,
		 unsigned int anonymityLevel,
		 unsigned int priority,
		 int doIndex,
		 int doExtract,
		 int individualKeywords,
		 const struct ECRS_MetaData * md,
		 const struct ECRS_URI * globalURI,
		 const struct ECRS_URI * keyUri) {
  char * config;
  EXTRACTOR_ExtractorList * extractors;
  struct FSUI_UploadShared * shared;
  struct FSUI_UploadList * ul;

  if (doExtract) {
    extractors = EXTRACTOR_loadDefaultLibraries();
    if ( (0 == GC_get_configuration_value_string(ctx->cfg,
						 "FS",
						 "EXTRACTORS",
						 NULL,
						 &config)) &&
	 (config != NULL) ) {
      extractors = EXTRACTOR_loadConfigLibraries(extractors,
						 config);
    }
  } else {
    extractors = NULL;
    config = NULL;
  }
  shared = MALLOC(sizeof(FSUI_UploadShared));
  shared->dsc = dsc;
  shared->dscClosure = dscClosure;
  shared->extractors = extractors;
  shared->ctx = ctx;
  shared->handle = NULL;
  shared->global_keywords = ECRS_dupUri(globalURI);
  shared->extractor_config = config;
  shared->doIndex = doIndex;
  shared->anonymityLevel = anonymityLevel;
  shared->priority = priority;
  shared->individualKeywords = individualKeywords;
  shared->force_termination = NO;
  ul = addUploads(shared,
		  filename,
		  keyUri,
		  md,
		  &ctx->activeUploads);
  shared->handle = PTHREAD_CREATE(&FSUI_uploadThread,
				  ul,
				  128 * 1024);
  if (shared->handle == NULL) {
    GE_LOG_STRERROR(ctx->ectx,
		    GE_ERROR | GE_USER | GE_BULK, 
		    "PTHREAD_CREATE");
    freeUploadList(ul);
    freeShared(shared);
    return NULL;
  }
  signalUploadStarted(ul, 1);
  return ul;
}

/**
 * Abort an upload.  If the context is for a recursive
 * upload, all sub-uploads will also be aborted.
 * Note that if this is not the top-level upload,
 * the top-level upload will continue without the
 * subtree selected using this abort command.
 *
 * @return SYSERR on error
 */
int FSUI_abortUpload(struct FSUI_Context * ctx,
		     struct FSUI_UploadList * ul) {
  FSUI_UploadList * c;
  FSUI_Event event;
  
  GE_ASSERT(ctx->ectx, ul != NULL);
  if ( (ul->state != FSUI_ACTIVE) &&
       (ul->state != FSUI_PENDING) )
    return NO;
  ul->state = FSUI_ABORTED;
  ul->shared->force_termination = YES;
  c = ul->child;
  while (c != NULL) {
    FSUI_abortUpload(ctx, c);
    c = c->next;
  }    
  PTHREAD_STOP_SLEEP(ul->shared->handle);
  event.type = FSUI_upload_aborted;
  event.data.UploadAborted.uc.pos = ul;
  event.data.UploadAborted.uc.cctx = ul->cctx;
  event.data.UploadAborted.uc.ppos = ul->parent;
  event.data.UploadAborted.uc.pcctx = ul->parent->cctx;
  ctx->ecb(ctx->ecbClosure,
	   &event);
  return OK;
}

/**
 * Stop an upload.  Only to be called for the top-level
 * upload.
 *
 * @return SYSERR on error
 */
int FSUI_stopUpload(struct FSUI_Context * ctx,
		    struct FSUI_UploadList * ul) {
  void * unused;
  FSUI_UploadList * prev;
  struct FSUI_UploadShared * shared;

  GE_ASSERT(ctx->ectx, ul != NULL);
  GE_ASSERT(ctx->ectx, ul->parent == &ctx->activeUploads);
  MUTEX_LOCK(ctx->lock);
  prev = ctx->activeUploads.child;
  while ( (prev != ul) &&
	  (prev != NULL) &&
	  (prev->next != ul) ) 
    prev = prev->next;
  if (prev == NULL) {
    MUTEX_UNLOCK(ctx->lock);
    GE_LOG(ctx->ectx, 
	   GE_DEBUG | GE_REQUEST | GE_USER,
	   "FSUI_stopUpload failed to locate upload.\n");
    return SYSERR;
  }
  if (prev == ul) 
    ul->parent->child = ul->next; /* first child of parent */
  else 
    prev->next = ul->next; /* not first child */  
  MUTEX_UNLOCK(ctx->lock);
  PTHREAD_JOIN(ul->shared->handle,
	       &unused);
  signalUploadStopped(ul, 1);
  shared = ul->shared;
  freeUploadList(ul);
  freeShared(shared);
  return OK;
}

/* end of upload.c */
