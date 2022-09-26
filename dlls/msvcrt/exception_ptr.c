/*
 * std::exception_ptr helper functions
 *
 * Copyright 2022 Torge Matthies for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <stdbool.h>

#include "windef.h"
#include "winternl.h"
#include "wine/exception.h"
#include "wine/debug.h"
#include "msvcrt.h"
#include "cxx.h"

#if _MSVCR_VER >= 100

WINE_DEFAULT_DEBUG_CHANNEL(msvcrt);

/*********************************************************************
 * ?__ExceptionPtrCreate@@YAXPAX@Z
 * ?__ExceptionPtrCreate@@YAXPEAX@Z
 */
void __cdecl __ExceptionPtrCreate(exception_ptr *ep)
{
    TRACE("(%p)\n", ep);

    ep->rec = NULL;
    ep->ref = NULL;
}

#ifdef __ASM_USE_THISCALL_WRAPPER
extern void call_dtor(const cxx_exception_type *type, void *func, void *object);

__ASM_GLOBAL_FUNC( call_dtor,
                   "movl 12(%esp),%ecx\n\t"
                   "call *8(%esp)\n\t"
                   "ret" );
#elif __x86_64__
static inline void call_dtor(const cxx_exception_type *type, unsigned int dtor, void *object)
{
    char *base = RtlPcToFileHeader((void*)type, (void**)&base);
    void (__cdecl *func)(void*) = (void*)(base + dtor);
    func(object);
}
#else
#define call_dtor(type, func, object) ((void (__thiscall*)(void*))(func))(object)
#endif

/*********************************************************************
 * ?__ExceptionPtrDestroy@@YAXPAX@Z
 * ?__ExceptionPtrDestroy@@YAXPEAX@Z
 */
void __cdecl __ExceptionPtrDestroy(exception_ptr *ep)
{
    TRACE("(%p)\n", ep);

    if (!ep->rec)
        return;

    if (!InterlockedDecrement(ep->ref))
    {
        if (ep->rec->ExceptionCode == CXX_EXCEPTION)
        {
            const cxx_exception_type *type = (void*)ep->rec->ExceptionInformation[2];
            void *obj = (void*)ep->rec->ExceptionInformation[1];

            if (type && type->destructor) call_dtor(type, type->destructor, obj);
            HeapFree(GetProcessHeap(), 0, obj);
        }

        HeapFree(GetProcessHeap(), 0, ep->rec);
        HeapFree(GetProcessHeap(), 0, ep->ref);
    }
}

/*********************************************************************
 * ?__ExceptionPtrCopy@@YAXPAXPBX@Z
 * ?__ExceptionPtrCopy@@YAXPEAXPEBX@Z
 */
void __cdecl __ExceptionPtrCopy(exception_ptr *ep, const exception_ptr *copy)
{
    TRACE("(%p %p)\n", ep, copy);

    /* don't destroy object stored in ep */
    *ep = *copy;
    if (ep->ref)
        InterlockedIncrement(copy->ref);
}

/*********************************************************************
 * ?__ExceptionPtrAssign@@YAXPAXPBX@Z
 * ?__ExceptionPtrAssign@@YAXPEAXPEBX@Z
 */
void __cdecl __ExceptionPtrAssign(exception_ptr *ep, const exception_ptr *assign)
{
    TRACE("(%p %p)\n", ep, assign);

    /* don't destroy object stored in ep */
    if (ep->ref)
        InterlockedDecrement(ep->ref);

    *ep = *assign;
    if (ep->ref)
        InterlockedIncrement(ep->ref);
}

/*********************************************************************
 * ?__ExceptionPtrRethrow@@YAXPBX@Z
 * ?__ExceptionPtrRethrow@@YAXPEBX@Z
 */
void __cdecl __ExceptionPtrRethrow(const exception_ptr *ep)
{
    TRACE("(%p)\n", ep);

    if (!ep->rec)
    {
        throw_exception("bad exception");
        return;
    }

    RaiseException(ep->rec->ExceptionCode, ep->rec->ExceptionFlags & (~EH_UNWINDING),
            ep->rec->NumberParameters, ep->rec->ExceptionInformation);
}

#ifdef __i386__
extern void call_copy_ctor( void *func, void *this, void *src, int has_vbase );
#else
static inline void call_copy_ctor( void *func, void *this, void *src, int has_vbase )
{
    TRACE( "calling copy ctor %p object %p src %p\n", func, this, src );
    if (has_vbase)
        ((void (__cdecl*)(void*, void*, BOOL))func)(this, src, 1);
    else
        ((void (__cdecl*)(void*, void*))func)(this, src);
}
#endif

#ifndef __x86_64__
static void exception_ptr_from_record(exception_ptr *ep, EXCEPTION_RECORD *rec)
{
    TRACE("(%p)\n", ep);

    if (!rec)
    {
        ep->rec = NULL;
        ep->ref = NULL;
        return;
    }

    ep->rec = HeapAlloc(GetProcessHeap(), 0, sizeof(EXCEPTION_RECORD));
    ep->ref = HeapAlloc(GetProcessHeap(), 0, sizeof(int));

    *ep->rec = *rec;
    *ep->ref = 1;

    if (ep->rec->ExceptionCode == CXX_EXCEPTION)
    {
        const cxx_exception_type *et = (void*)ep->rec->ExceptionInformation[2];
        const cxx_type_info *ti;
        void **data, *obj;

        ti = et->type_info_table->info[0];
        data = HeapAlloc(GetProcessHeap(), 0, ti->size);

        obj = (void*)ep->rec->ExceptionInformation[1];
        if (ti->flags & CLASS_IS_SIMPLE_TYPE)
        {
            memcpy(data, obj, ti->size);
            if (ti->size == sizeof(void *)) *data = get_this_pointer(&ti->offsets, *data);
        }
        else if (ti->copy_ctor)
        {
            call_copy_ctor(ti->copy_ctor, data, get_this_pointer(&ti->offsets, obj),
                    ti->flags & CLASS_HAS_VIRTUAL_BASE_CLASS);
        }
        else
            memcpy(data, get_this_pointer(&ti->offsets, obj), ti->size);
        ep->rec->ExceptionInformation[1] = (ULONG_PTR)data;
    }
    return;
}
#else
static void exception_ptr_from_record(exception_ptr *ep, EXCEPTION_RECORD *rec)
{
    TRACE("(%p)\n", ep);

    if (!rec)
    {
        ep->rec = NULL;
        ep->ref = NULL;
        return;
    }

    ep->rec = HeapAlloc(GetProcessHeap(), 0, sizeof(EXCEPTION_RECORD));
    ep->ref = HeapAlloc(GetProcessHeap(), 0, sizeof(int));

    *ep->rec = *rec;
    *ep->ref = 1;

    if (ep->rec->ExceptionCode == CXX_EXCEPTION)
    {
        const cxx_exception_type *et = (void*)ep->rec->ExceptionInformation[2];
        const cxx_type_info *ti;
        void **data, *obj;
        char *base = RtlPcToFileHeader((void*)et, (void**)&base);

        ti = (const cxx_type_info*)(base + ((const cxx_type_info_table*)(base + et->type_info_table))->info[0]);
        data = HeapAlloc(GetProcessHeap(), 0, ti->size);

        obj = (void*)ep->rec->ExceptionInformation[1];
        if (ti->flags & CLASS_IS_SIMPLE_TYPE)
        {
            memcpy(data, obj, ti->size);
            if (ti->size == sizeof(void *)) *data = get_this_pointer(&ti->offsets, *data);
        }
        else if (ti->copy_ctor)
        {
            call_copy_ctor(base + ti->copy_ctor, data, get_this_pointer(&ti->offsets, obj),
                    ti->flags & CLASS_HAS_VIRTUAL_BASE_CLASS);
        }
        else
            memcpy(data, get_this_pointer(&ti->offsets, obj), ti->size);
        ep->rec->ExceptionInformation[1] = (ULONG_PTR)data;
    }
    return;
}
#endif

/*********************************************************************
 * ?__ExceptionPtrCurrentException@@YAXPAX@Z
 * ?__ExceptionPtrCurrentException@@YAXPEAX@Z
 */
void __cdecl __ExceptionPtrCurrentException(exception_ptr *ep)
{
    TRACE("(%p)\n", ep);
    exception_ptr_from_record(ep, msvcrt_get_thread_data()->exc_record);
}

#if _MSVCR_VER >= 110
/*********************************************************************
 * ?__ExceptionPtrToBool@@YA_NPBX@Z
 * ?__ExceptionPtrToBool@@YA_NPEBX@Z
 */
bool __cdecl __ExceptionPtrToBool(exception_ptr *ep)
{
    return !!ep->rec;
}
#endif

/*********************************************************************
 * ?__ExceptionPtrCopyException@@YAXPAXPBX1@Z
 * ?__ExceptionPtrCopyException@@YAXPEAXPEBX1@Z
 */
#ifndef __x86_64__
void __cdecl __ExceptionPtrCopyException(exception_ptr *ep,
        exception *object, const cxx_exception_type *type)
{
    const cxx_type_info *ti;
    void **data;

    __ExceptionPtrDestroy(ep);

    ep->rec = HeapAlloc(GetProcessHeap(), 0, sizeof(EXCEPTION_RECORD));
    ep->ref = HeapAlloc(GetProcessHeap(), 0, sizeof(int));
    *ep->ref = 1;

    memset(ep->rec, 0, sizeof(EXCEPTION_RECORD));
    ep->rec->ExceptionCode = CXX_EXCEPTION;
    ep->rec->ExceptionFlags = EH_NONCONTINUABLE;
    ep->rec->NumberParameters = 3;
    ep->rec->ExceptionInformation[0] = CXX_FRAME_MAGIC_VC6;
    ep->rec->ExceptionInformation[2] = (ULONG_PTR)type;

    ti = type->type_info_table->info[0];
    data = HeapAlloc(GetProcessHeap(), 0, ti->size);
    if (ti->flags & CLASS_IS_SIMPLE_TYPE)
    {
        memcpy(data, object, ti->size);
        if (ti->size == sizeof(void *)) *data = get_this_pointer(&ti->offsets, *data);
    }
    else if (ti->copy_ctor)
    {
        call_copy_ctor(ti->copy_ctor, data, get_this_pointer(&ti->offsets, object),
                ti->flags & CLASS_HAS_VIRTUAL_BASE_CLASS);
    }
    else
        memcpy(data, get_this_pointer(&ti->offsets, object), ti->size);
    ep->rec->ExceptionInformation[1] = (ULONG_PTR)data;
}
#else
void __cdecl __ExceptionPtrCopyException(exception_ptr *ep,
        exception *object, const cxx_exception_type *type)
{
    const cxx_type_info *ti;
    void **data;
    char *base;

    RtlPcToFileHeader((void*)type, (void**)&base);
    __ExceptionPtrDestroy(ep);

    ep->rec = HeapAlloc(GetProcessHeap(), 0, sizeof(EXCEPTION_RECORD));
    ep->ref = HeapAlloc(GetProcessHeap(), 0, sizeof(int));
    *ep->ref = 1;

    memset(ep->rec, 0, sizeof(EXCEPTION_RECORD));
    ep->rec->ExceptionCode = CXX_EXCEPTION;
    ep->rec->ExceptionFlags = EH_NONCONTINUABLE;
    ep->rec->NumberParameters = 4;
    ep->rec->ExceptionInformation[0] = CXX_FRAME_MAGIC_VC6;
    ep->rec->ExceptionInformation[2] = (ULONG_PTR)type;
    ep->rec->ExceptionInformation[3] = (ULONG_PTR)base;

    ti = (const cxx_type_info*)(base + ((const cxx_type_info_table*)(base + type->type_info_table))->info[0]);
    data = HeapAlloc(GetProcessHeap(), 0, ti->size);
    if (ti->flags & CLASS_IS_SIMPLE_TYPE)
    {
        memcpy(data, object, ti->size);
        if (ti->size == sizeof(void *)) *data = get_this_pointer(&ti->offsets, *data);
    }
    else if (ti->copy_ctor)
    {
        call_copy_ctor(base + ti->copy_ctor, data, get_this_pointer(&ti->offsets, object),
                ti->flags & CLASS_HAS_VIRTUAL_BASE_CLASS);
    }
    else
        memcpy(data, get_this_pointer(&ti->offsets, object), ti->size);
    ep->rec->ExceptionInformation[1] = (ULONG_PTR)data;
}
#endif

bool __cdecl __ExceptionPtrCompare(const exception_ptr *ep1, const exception_ptr *ep2)
{
    return ep1->rec == ep2->rec;
}

#endif /* _MSVCR_VER >= 100 */
