/****************************************************************************
*
*                            Open Watcom Project
*
*    Portions Copyright (c) 1983-2002 Sybase, Inc. All Rights Reserved.
*
*  ========================================================================
*
*    This file contains Original Code and/or Modifications of Original
*    Code as defined in and that are subject to the Sybase Open Watcom
*    Public License version 1.0 (the 'License'). You may not use this file
*    except in compliance with the License. BY USING THIS FILE YOU AGREE TO
*    ALL TERMS AND CONDITIONS OF THE LICENSE. A copy of the License is
*    provided with the Original Code and Modifications, and is also
*    available at www.sybase.com/developer/opensource.
*
*    The Original Code and all software distributed under the License are
*    distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
*    EXPRESS OR IMPLIED, AND SYBASE AND ALL CONTRIBUTORS HEREBY DISCLAIM
*    ALL SUCH WARRANTIES, INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF
*    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR
*    NON-INFRINGEMENT. Please see the License for the specific language
*    governing rights and limitations under the License.
*
*  ========================================================================
*
* Description:  WHEN YOU FIGURE OUT WHAT THIS FILE DOES, PLEASE
*               DESCRIBE IT HERE!
*
****************************************************************************/


/*
  MIXCACHE - object file caching routines which do both full file caching
                and paged caching.
*/

#include <string.h>
#include <limits.h>
#include <stdio.h>
#include "linkstd.h"
#include "msg.h"
#include "alloc.h"
#include "wlnkmsg.h"
#include "fileio.h"
#include "objio.h"
#include "objcache.h"

static bool             DumpFileCache( infilelist *, bool );

static bool             Multipage;

#define CACHE_PAGE_SIZE         (8*1024)

static unsigned NumCacheBlocks( unsigned long len )
/*************************************************/
// figure out the number of cache blocks necessary
{
    unsigned    numblocks;

    numblocks = len / CACHE_PAGE_SIZE;
    if( len % CACHE_PAGE_SIZE != 0 ) {
        numblocks++;
    }
    return( numblocks );
}

bool CacheOpen( file_list *list )
/**************************************/
{
    infilelist  *file;
    unsigned    numblocks;
    char        **cache;

    if( list == NULL )
        return( true );
    file = list->file;
    if( file->flags & INSTAT_IOERR )
        return( false );
    if( DoObjOpen( file ) ) {
        file->flags |= INSTAT_IN_USE;
    } else {
        file->flags |= INSTAT_IOERR;
        return( false );
    }
    if( file->len == 0 ) {
        file->len = QFileSize( file->handle );
        if( file->len == 0 ) {
            LnkMsg( ERR+MSG_BAD_OBJECT, "s", file->name );
            file->flags |= INSTAT_IOERR;
            return( false );
        }
    }
    if( (file->flags & INSTAT_SET_CACHE) == 0 ) {
        if( LinkFlags & LF_CACHE_FLAG ) {
            file->flags |= INSTAT_FULL_CACHE;
        } else if( LinkFlags & LF_NOCACHE_FLAG ) {
            file->flags |= INSTAT_PAGE_CACHE;
        } else {
            if( file->flags & INSTAT_LIBRARY ) {
                file->flags |= INSTAT_PAGE_CACHE;
            } else {
                file->flags |= INSTAT_FULL_CACHE;
            }
        }
    }
    if( file->cache == NULL ) {
        if( file->flags & INSTAT_FULL_CACHE ) {
            _ChkAlloc( file->cache, file->len );
            if( file->currpos != 0 ) {
                QLSeek( file->handle, 0, SEEK_SET, file->name.u.ptr );
            }
            QRead( file->handle, file->cache, file->len, file->name.u.ptr );
            file->currpos = file->len;
        } else {
            numblocks = NumCacheBlocks( file->len );
            _Pass1Alloc( file->cache, numblocks * sizeof( char * ) );
            cache = file->cache;
            while( numblocks-- > 0 ) {
                *cache++ = NULL;
            }
        }
    }
    return( true );
}

void CacheClose( file_list *list, unsigned pass )
/******************************************************/
{
    infilelist *file;
    bool        nukecache;

    if( list == NULL )
        return;
    file = list->file;
//    if( file->handle == NIL_FHANDLE ) return;
    file->flags &= ~INSTAT_IN_USE;
    switch( pass ) {
    case 1: /* first pass */
        nukecache = ( (file->flags & INSTAT_LIBRARY) == 0 );
        if( file->flags & INSTAT_FULL_CACHE ) {
            if( nukecache ) {
                FreeObjCache( list );
            }
        } else {
            DumpFileCache( file, nukecache );   // don't cache .obj's
        }
        break;
    case 3: /* freeing structure */
        FreeObjCache( list );
        if( file->handle != NIL_FHANDLE ) {
            QClose( file->handle, file->name.u.ptr );
            file->handle = NIL_FHANDLE;
        }
        break;
    }
}

void *CachePermRead( file_list *list, unsigned long pos, size_t len )
/*******************************************************************/
{
    char        *buf;
    char        *result;

    buf = CacheRead( list, pos, len );
    if( list->file->flags & INSTAT_FULL_CACHE )
        return( buf );
    if( Multipage ) {
        _LnkRealloc( result, buf, len );
        _ChkAlloc( TokBuff, TokSize );
        Multipage = false;              // indicate that last read is permanent.
    } else {
        _ChkAlloc( result, len );
        memcpy( result, buf, len );
    }
    return( result );
}

void *CacheRead( file_list *list, unsigned long pos, size_t len )
/***************************************************************/
/* read len bytes out of the cache. */
{
    size_t          bufnum;
    size_t          startnum;
    size_t          offset;
    size_t          amtread;
    char            *result;
    char            **cache;
    unsigned long   newpos;
    infilelist      *file;

    if( list->file->flags & INSTAT_FULL_CACHE ) {
        if( pos + len > list->file->len )
            return( NULL );
        return( (char *)list->file->cache + pos );
    }
    Multipage = false;
    file = list->file;
    offset = pos % CACHE_PAGE_SIZE;
    amtread = CACHE_PAGE_SIZE - offset;
    startnum = pos / CACHE_PAGE_SIZE;
    bufnum = startnum;
    cache = file->cache;
    for( ;; ) {
        if( cache[bufnum] == NULL ) {   // make sure page is in.
            _ChkAlloc( cache[bufnum], CACHE_PAGE_SIZE );
            newpos = (unsigned long)bufnum * CACHE_PAGE_SIZE;
            if( file->currpos != newpos ) {
                QSeek( file->handle, newpos, file->name.u.ptr );
            }
            file->currpos = newpos + CACHE_PAGE_SIZE;
            QRead( file->handle, cache[bufnum], CACHE_PAGE_SIZE, file->name.u.ptr );
        }
        if( amtread >= len )
            break;
        amtread += CACHE_PAGE_SIZE;     // it spans pages.
        bufnum++;
        Multipage = true;
    }
    if( !Multipage ) {
        result = cache[startnum] + offset;
    } else {
        if( len > TokSize ) {
            TokSize = ROUND_UP( len, SECTOR_SIZE );
            _LnkRealloc( TokBuff, TokBuff, TokSize );
        }
        amtread = CACHE_PAGE_SIZE - offset;
        memcpy( TokBuff, cache[startnum] + offset, amtread );
        len -= amtread;
        result = TokBuff + amtread;
        for( ;; ) {
            startnum++;
            if( len <= CACHE_PAGE_SIZE ) {
                memcpy( result, cache[startnum], len );
                break;
            } else {
                memcpy( result, cache[startnum], CACHE_PAGE_SIZE );
                len -= CACHE_PAGE_SIZE;
                result += CACHE_PAGE_SIZE;
            }
        }
        result = TokBuff;
    }
    return( result );
}

bool CacheIsPerm( void )
/*****************************/
{
    return( !Multipage );
}

bool CacheEnd( file_list *list, unsigned long pos )
/*********************************************************/
{
    return( pos >= list->file->len );
}

void CacheFini( void )
/********************/
{
}

void CacheFree( file_list *list, void *mem )
/*************************************************/
// used for disposing things allocated by CachePermRead
{
    if( list->file->flags & INSTAT_PAGE_CACHE ) {
        _LnkFree( mem );
    }
}

static bool DumpFileCache( infilelist *file, bool nuke )
/******************************************************/
{
    unsigned    num;
    unsigned    savenum;
    unsigned    index;
    char **     blocklist;
    bool        blockfreed;

    blockfreed = false;
    if( nuke ) {
        savenum = UINT_MAX;
    } else {
        savenum = file->currpos / CACHE_PAGE_SIZE;
    }
    if( file->cache != NULL ) {
        num = NumCacheBlocks( file->len );
        blocklist = file->cache;
        for( index = 0; index < num; index++ ) {
            if( index != savenum && *blocklist != NULL ) {
                _LnkFree( *blocklist );
                *blocklist = NULL;
                blockfreed = true;
            }
            blocklist++;
        }
    }
    return( blockfreed );
}

void FreeObjCache( file_list *list )
/*****************************************/
{
    if( list == NULL )
        return;
    if( list->file->flags & INSTAT_FULL_CACHE ) {
        _LnkFree( list->file->cache );
    } else {
        DumpFileCache( list->file, true );
    }
    list->file->cache = NULL;
}

bool DumpObjCache( void )
/******************************/
// find and dump an object file cache.
{
    infilelist *file;

    for( file = CachedFiles; file != NULL; file = file->next ) {
        if( file->flags & INSTAT_PAGE_CACHE ) {
            if( CurrMod == NULL || CurrMod->f.source == NULL
                                || CurrMod->f.source->file != file ) {
                if( DumpFileCache( file, true ) ) {
                    return( true );
                }
            }
        }
    }
    return( false );
}
