/*
 *	Process synchronisation
 *
 * Copyright 1996, 1997, 1998 Marcus Meissner
 * Copyright 1997, 1998, 1999 Alexandre Julliard
 * Copyright 1999, 2000 Juergen Schmied
 * Copyright 2003 Eric Pouech
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

#include <limits.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "windef.h"
#include "winternl.h"
#include "wine/debug.h"
#include "wine/list.h"
#include "ntdll_misc.h"

WINE_DEFAULT_DEBUG_CHANNEL(sync);
WINE_DECLARE_DEBUG_CHANNEL(relay);

static const char *debugstr_timeout( const LARGE_INTEGER *timeout )
{
    if (!timeout) return "(infinite)";
    return wine_dbgstr_longlong( timeout->QuadPart );
}

/******************************************************************
 *              RtlRunOnceInitialize (NTDLL.@)
 */
void WINAPI RtlRunOnceInitialize( RTL_RUN_ONCE *once )
{
    once->Ptr = NULL;
}

/******************************************************************
 *              RtlRunOnceBeginInitialize (NTDLL.@)
 */
DWORD WINAPI RtlRunOnceBeginInitialize( RTL_RUN_ONCE *once, ULONG flags, void **context )
{
    if (flags & RTL_RUN_ONCE_CHECK_ONLY)
    {
        ULONG_PTR val = (ULONG_PTR)once->Ptr;

        if (flags & RTL_RUN_ONCE_ASYNC) return STATUS_INVALID_PARAMETER;
        if ((val & 3) != 2) return STATUS_UNSUCCESSFUL;
        if (context) *context = (void *)(val & ~3);
        return STATUS_SUCCESS;
    }

    for (;;)
    {
        ULONG_PTR next, val = (ULONG_PTR)once->Ptr;

        switch (val & 3)
        {
        case 0:  /* first time */
            if (!InterlockedCompareExchangePointer( &once->Ptr,
                                                    (flags & RTL_RUN_ONCE_ASYNC) ? (void *)3 : (void *)1, 0 ))
                return STATUS_PENDING;
            break;

        case 1:  /* in progress, wait */
            if (flags & RTL_RUN_ONCE_ASYNC) return STATUS_INVALID_PARAMETER;
            next = val & ~3;
            if (InterlockedCompareExchangePointer( &once->Ptr, (void *)((ULONG_PTR)&next | 1),
                                                   (void *)val ) == (void *)val)
                NtWaitForKeyedEvent( 0, &next, FALSE, NULL );
            break;

        case 2:  /* done */
            if (context) *context = (void *)(val & ~3);
            return STATUS_SUCCESS;

        case 3:  /* in progress, async */
            if (!(flags & RTL_RUN_ONCE_ASYNC)) return STATUS_INVALID_PARAMETER;
            return STATUS_PENDING;
        }
    }
}

/******************************************************************
 *              RtlRunOnceComplete (NTDLL.@)
 */
DWORD WINAPI RtlRunOnceComplete( RTL_RUN_ONCE *once, ULONG flags, void *context )
{
    if ((ULONG_PTR)context & 3) return STATUS_INVALID_PARAMETER;

    if (flags & RTL_RUN_ONCE_INIT_FAILED)
    {
        if (context) return STATUS_INVALID_PARAMETER;
        if (flags & RTL_RUN_ONCE_ASYNC) return STATUS_INVALID_PARAMETER;
    }
    else context = (void *)((ULONG_PTR)context | 2);

    for (;;)
    {
        ULONG_PTR val = (ULONG_PTR)once->Ptr;

        switch (val & 3)
        {
        case 1:  /* in progress */
            if (InterlockedCompareExchangePointer( &once->Ptr, context, (void *)val ) != (void *)val) break;
            val &= ~3;
            while (val)
            {
                ULONG_PTR next = *(ULONG_PTR *)val;
                NtReleaseKeyedEvent( 0, (void *)val, FALSE, NULL );
                val = next;
            }
            return STATUS_SUCCESS;

        case 3:  /* in progress, async */
            if (!(flags & RTL_RUN_ONCE_ASYNC)) return STATUS_INVALID_PARAMETER;
            if (InterlockedCompareExchangePointer( &once->Ptr, context, (void *)val ) != (void *)val) break;
            return STATUS_SUCCESS;

        default:
            return STATUS_UNSUCCESSFUL;
        }
    }
}


/***********************************************************************
 * Critical sections
 ***********************************************************************/


static void *no_debug_info_marker = (void *)(ULONG_PTR)-1;

static BOOL crit_section_has_debuginfo( const RTL_CRITICAL_SECTION *crit )
{
    return crit->DebugInfo != NULL && crit->DebugInfo != no_debug_info_marker;
}

static const char *crit_section_get_name( const RTL_CRITICAL_SECTION *crit )
{
    if (crit_section_has_debuginfo( crit ))
        return (char *)crit->DebugInfo->Spare[0];
    return "?";
}

static inline HANDLE get_semaphore( RTL_CRITICAL_SECTION *crit )
{
    HANDLE ret = crit->LockSemaphore;
    if (!ret)
    {
        HANDLE sem;
        if (NtCreateSemaphore( &sem, SEMAPHORE_ALL_ACCESS, NULL, 0, 1 )) return 0;
        if (!(ret = InterlockedCompareExchangePointer( &crit->LockSemaphore, sem, 0 )))
            ret = sem;
        else
            NtClose(sem);  /* somebody beat us to it */
    }
    return ret;
}

static inline NTSTATUS wait_semaphore( RTL_CRITICAL_SECTION *crit, int timeout )
{
    LARGE_INTEGER time = {.QuadPart = timeout * (LONGLONG)-10000000};

    /* debug info is cleared by MakeCriticalSectionGlobal */
    if (!crit_section_has_debuginfo( crit ))
    {
        HANDLE sem = get_semaphore( crit );
        return NtWaitForSingleObject( sem, FALSE, &time );
    }
    else
    {
        LONG *lock = (LONG *)&crit->LockSemaphore;
        while (!InterlockedCompareExchange( lock, 0, 1 ))
        {
            static const LONG zero;
            /* this may wait longer than specified in case of multiple wake-ups */
            if (RtlWaitOnAddress( lock, &zero, sizeof(LONG), &time ) == STATUS_TIMEOUT)
                return STATUS_TIMEOUT;
        }
        return STATUS_WAIT_0;
    }
}

/******************************************************************************
 *      RtlInitializeCriticalSection   (NTDLL.@)
 */
NTSTATUS WINAPI RtlInitializeCriticalSection( RTL_CRITICAL_SECTION *crit )
{
    return RtlInitializeCriticalSectionEx( crit, 0, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
}


/******************************************************************************
 *      RtlInitializeCriticalSectionAndSpinCount   (NTDLL.@)
 */
NTSTATUS WINAPI RtlInitializeCriticalSectionAndSpinCount( RTL_CRITICAL_SECTION *crit, ULONG spincount )
{
    return RtlInitializeCriticalSectionEx( crit, spincount, RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO );
}


/******************************************************************************
 *      RtlInitializeCriticalSectionEx   (NTDLL.@)
 */
NTSTATUS WINAPI RtlInitializeCriticalSectionEx( RTL_CRITICAL_SECTION *crit, ULONG spincount, ULONG flags )
{
    if (flags & (RTL_CRITICAL_SECTION_FLAG_DYNAMIC_SPIN|RTL_CRITICAL_SECTION_FLAG_STATIC_INIT))
        FIXME("(%p,%lu,0x%08lx) semi-stub\n", crit, spincount, flags);

    /* FIXME: if RTL_CRITICAL_SECTION_FLAG_STATIC_INIT is given, we should use
     * memory from a static pool to hold the debug info. Then heap.c could pass
     * this flag rather than initialising the process heap CS by hand. If this
     * is done, then debug info should be managed through Rtlp[Allocate|Free]DebugInfo
     * so (e.g.) MakeCriticalSectionGlobal() doesn't free it using HeapFree().
     */
    if (!(flags & RTL_CRITICAL_SECTION_FLAG_FORCE_DEBUG_INFO))
        crit->DebugInfo = no_debug_info_marker;
    else
    {
        crit->DebugInfo = RtlAllocateHeap( GetProcessHeap(), 0, sizeof(RTL_CRITICAL_SECTION_DEBUG ));
        if (crit->DebugInfo)
        {
            crit->DebugInfo->Type = 0;
            crit->DebugInfo->CreatorBackTraceIndex = 0;
            crit->DebugInfo->CriticalSection = crit;
            crit->DebugInfo->ProcessLocksList.Blink = &crit->DebugInfo->ProcessLocksList;
            crit->DebugInfo->ProcessLocksList.Flink = &crit->DebugInfo->ProcessLocksList;
            crit->DebugInfo->EntryCount = 0;
            crit->DebugInfo->ContentionCount = 0;
            memset( crit->DebugInfo->Spare, 0, sizeof(crit->DebugInfo->Spare) );
        }
    }
    crit->LockCount      = -1;
    crit->RecursionCount = 0;
    crit->OwningThread   = 0;
    crit->LockSemaphore  = 0;
    if (NtCurrentTeb()->Peb->NumberOfProcessors <= 1) spincount = 0;
    crit->SpinCount = spincount & ~0x80000000;
    return STATUS_SUCCESS;
}


/******************************************************************************
 *      RtlSetCriticalSectionSpinCount   (NTDLL.@)
 */
ULONG WINAPI RtlSetCriticalSectionSpinCount( RTL_CRITICAL_SECTION *crit, ULONG spincount )
{
    ULONG oldspincount = crit->SpinCount;
    if (NtCurrentTeb()->Peb->NumberOfProcessors <= 1) spincount = 0;
    crit->SpinCount = spincount;
    return oldspincount;
}


/******************************************************************************
 *      RtlDeleteCriticalSection   (NTDLL.@)
 */
NTSTATUS WINAPI RtlDeleteCriticalSection( RTL_CRITICAL_SECTION *crit )
{
    crit->LockCount      = -1;
    crit->RecursionCount = 0;
    crit->OwningThread   = 0;
    if (crit_section_has_debuginfo( crit ))
    {
        /* only free the ones we made in here */
        if (!crit->DebugInfo->Spare[0])
        {
            RtlFreeHeap( GetProcessHeap(), 0, crit->DebugInfo );
            crit->DebugInfo = NULL;
        }
    }
    else
    {
        NtClose( crit->LockSemaphore );
        crit->DebugInfo = NULL;
    }
    crit->LockSemaphore = 0;
    return STATUS_SUCCESS;
}


/******************************************************************************
 *      RtlpWaitForCriticalSection   (NTDLL.@)
 */
NTSTATUS WINAPI RtlpWaitForCriticalSection( RTL_CRITICAL_SECTION *crit )
{
    LONGLONG timeout = NtCurrentTeb()->Peb->CriticalSectionTimeout.QuadPart / -10000000;

    /* Don't allow blocking on a critical section during process termination */
    if (RtlDllShutdownInProgress())
    {
        WARN( "process %s is shutting down, returning STATUS_SUCCESS\n",
              debugstr_w(NtCurrentTeb()->Peb->ProcessParameters->ImagePathName.Buffer) );
        return STATUS_SUCCESS;
    }

    for (;;)
    {
        EXCEPTION_RECORD rec;
        NTSTATUS status = wait_semaphore( crit, 5 );
        timeout -= 5;

        if ( status == STATUS_TIMEOUT )
        {
            const char *name = crit_section_get_name( crit );

            ERR( "section %p %s wait timed out in thread %04lx, blocked by %04lx, retrying (60 sec)\n",
                 crit, debugstr_a(name), GetCurrentThreadId(), HandleToULong(crit->OwningThread) );
            status = wait_semaphore( crit, 60 );
            timeout -= 60;

            if ( status == STATUS_TIMEOUT && TRACE_ON(relay) )
            {
                ERR( "section %p %s wait timed out in thread %04lx, blocked by %04lx, retrying (5 min)\n",
                     crit, debugstr_a(name), GetCurrentThreadId(), HandleToULong(crit->OwningThread) );
                status = wait_semaphore( crit, 300 );
                timeout -= 300;
            }
        }
        if (status == STATUS_WAIT_0) break;

        /* Throw exception only for Wine internal locks */
        if (!crit_section_has_debuginfo( crit ) || !crit->DebugInfo->Spare[0]) continue;

        /* only throw deadlock exception if configured timeout is reached */
        if (timeout > 0) continue;

        rec.ExceptionCode    = STATUS_POSSIBLE_DEADLOCK;
        rec.ExceptionFlags   = 0;
        rec.ExceptionRecord  = NULL;
        rec.ExceptionAddress = RtlRaiseException;  /* sic */
        rec.NumberParameters = 1;
        rec.ExceptionInformation[0] = (ULONG_PTR)crit;
        RtlRaiseException( &rec );
    }
    if (crit_section_has_debuginfo( crit )) crit->DebugInfo->ContentionCount++;
    return STATUS_SUCCESS;
}


/******************************************************************************
 *      RtlpUnWaitCriticalSection   (NTDLL.@)
 */
NTSTATUS WINAPI RtlpUnWaitCriticalSection( RTL_CRITICAL_SECTION *crit )
{
    NTSTATUS ret;

    /* debug info is cleared by MakeCriticalSectionGlobal */
    if (!crit_section_has_debuginfo( crit ))
    {
        HANDLE sem = get_semaphore( crit );
        ret = NtReleaseSemaphore( sem, 1, NULL );
    }
    else
    {
        LONG *lock = (LONG *)&crit->LockSemaphore;
        InterlockedExchange( lock, 1 );
        RtlWakeAddressSingle( lock );
        ret = STATUS_SUCCESS;
    }
    if (ret) RtlRaiseStatus( ret );
    return ret;
}


/******************************************************************************
 *      RtlEnterCriticalSection   (NTDLL.@)
 */
NTSTATUS WINAPI RtlEnterCriticalSection( RTL_CRITICAL_SECTION *crit )
{
    if (crit->SpinCount)
    {
        ULONG count;

        if (RtlTryEnterCriticalSection( crit )) return STATUS_SUCCESS;
        for (count = crit->SpinCount; count > 0; count--)
        {
            if (crit->LockCount > 0) break;  /* more than one waiter, don't bother spinning */
            if (crit->LockCount == -1)       /* try again */
            {
                if (InterlockedCompareExchange( &crit->LockCount, 0, -1 ) == -1) goto done;
            }
            YieldProcessor();
        }
    }

    if (InterlockedIncrement( &crit->LockCount ))
    {
        if (crit->OwningThread == ULongToHandle(GetCurrentThreadId()))
        {
            crit->RecursionCount++;
            return STATUS_SUCCESS;
        }

        /* Now wait for it */
        RtlpWaitForCriticalSection( crit );
    }
done:
    crit->OwningThread   = ULongToHandle(GetCurrentThreadId());
    crit->RecursionCount = 1;
    return STATUS_SUCCESS;
}


/******************************************************************************
 *      RtlTryEnterCriticalSection   (NTDLL.@)
 */
BOOL WINAPI RtlTryEnterCriticalSection( RTL_CRITICAL_SECTION *crit )
{
    BOOL ret = FALSE;
    if (InterlockedCompareExchange( &crit->LockCount, 0, -1 ) == -1)
    {
        crit->OwningThread   = ULongToHandle(GetCurrentThreadId());
        crit->RecursionCount = 1;
        ret = TRUE;
    }
    else if (crit->OwningThread == ULongToHandle(GetCurrentThreadId()))
    {
        InterlockedIncrement( &crit->LockCount );
        crit->RecursionCount++;
        ret = TRUE;
    }
    return ret;
}


/******************************************************************************
 *      RtlIsCriticalSectionLocked   (NTDLL.@)
 */
BOOL WINAPI RtlIsCriticalSectionLocked( RTL_CRITICAL_SECTION *crit )
{
    return crit->RecursionCount != 0;
}


/******************************************************************************
 *      RtlIsCriticalSectionLockedByThread   (NTDLL.@)
 */
BOOL WINAPI RtlIsCriticalSectionLockedByThread( RTL_CRITICAL_SECTION *crit )
{
    return crit->OwningThread == ULongToHandle(GetCurrentThreadId()) &&
           crit->RecursionCount;
}


/******************************************************************************
 *      RtlLeaveCriticalSection   (NTDLL.@)
 */
NTSTATUS WINAPI RtlLeaveCriticalSection( RTL_CRITICAL_SECTION *crit )
{
    if (--crit->RecursionCount)
    {
        if (crit->RecursionCount > 0) InterlockedDecrement( &crit->LockCount );
        else ERR( "section %p %s is not acquired\n", crit, debugstr_a( crit_section_get_name( crit )));
    }
    else
    {
        crit->OwningThread = 0;
        if (InterlockedDecrement( &crit->LockCount ) >= 0)
        {
            /* someone is waiting */
            RtlpUnWaitCriticalSection( crit );
        }
    }
    return STATUS_SUCCESS;
}

/******************************************************************
 *              RtlRunOnceExecuteOnce (NTDLL.@)
 */
DWORD WINAPI RtlRunOnceExecuteOnce( RTL_RUN_ONCE *once, PRTL_RUN_ONCE_INIT_FN func,
                                    void *param, void **context )
{
    DWORD ret = RtlRunOnceBeginInitialize( once, 0, context );

    if (ret != STATUS_PENDING) return ret;

    if (!func( once, param, context ))
    {
        RtlRunOnceComplete( once, RTL_RUN_ONCE_INIT_FAILED, NULL );
        return STATUS_UNSUCCESSFUL;
    }

    return RtlRunOnceComplete( once, 0, context ? *context : NULL );
}

struct srw_lock
{
    short exclusive_waiters;

    /* Number of shared owners, or -1 if owned exclusive.
     *
     * Sadly Windows has no equivalent to FUTEX_WAIT_BITSET, so in order to wake
     * up *only* exclusive or *only* shared waiters (and thus avoid spurious
     * wakeups), we need to wait on two different addresses.
     * RtlAcquireSRWLockShared() needs to know the values of "exclusive_waiters"
     * and "owners", but RtlAcquireSRWLockExclusive() only needs to know the
     * value of "owners", so the former can wait on the entire structure, and
     * the latter waits only on the "owners" member. Note then that "owners"
     * must not be the first element in the structure.
     */
    short owners;
};
C_ASSERT( sizeof(struct srw_lock) == 4 );

/***********************************************************************
 *              RtlInitializeSRWLock (NTDLL.@)
 *
 * NOTES
 *  Please note that SRWLocks do not keep track of the owner of a lock.
 *  It doesn't make any difference which thread for example unlocks an
 *  SRWLock (see corresponding tests). This implementation uses two
 *  keyed events (one for the exclusive waiters and one for the shared
 *  waiters) and is limited to 2^15-1 waiting threads.
 */
void WINAPI RtlInitializeSRWLock( RTL_SRWLOCK *lock )
{
    lock->Ptr = NULL;
}

/***********************************************************************
 *              RtlAcquireSRWLockExclusive (NTDLL.@)
 *
 * NOTES
 *  Unlike RtlAcquireResourceExclusive this function doesn't allow
 *  nested calls from the same thread. "Upgrading" a shared access lock
 *  to an exclusive access lock also doesn't seem to be supported.
 */
void WINAPI RtlAcquireSRWLockExclusive( RTL_SRWLOCK *lock )
{
    union { RTL_SRWLOCK *rtl; struct srw_lock *s; LONG *l; } u = { lock };

    InterlockedIncrement16( &u.s->exclusive_waiters );

    for (;;)
    {
        union { struct srw_lock s; LONG l; } old, new;
        BOOL wait;

        do
        {
            old.s = *u.s;
            new.s = old.s;

            if (!old.s.owners)
            {
                /* Not locked exclusive or shared. We can try to grab it. */
                new.s.owners = -1;
                --new.s.exclusive_waiters;
                wait = FALSE;
            }
            else
            {
                wait = TRUE;
            }
        } while (InterlockedCompareExchange( u.l, new.l, old.l ) != old.l);

        if (!wait) return;
        RtlWaitOnAddress( &u.s->owners, &new.s.owners, sizeof(short), NULL );
    }
}

/***********************************************************************
 *              RtlAcquireSRWLockShared (NTDLL.@)
 *
 * NOTES
 *   Do not call this function recursively - it will only succeed when
 *   there are no threads waiting for an exclusive lock!
 */
void WINAPI RtlAcquireSRWLockShared( RTL_SRWLOCK *lock )
{
    union { RTL_SRWLOCK *rtl; struct srw_lock *s; LONG *l; } u = { lock };

    for (;;)
    {
        union { struct srw_lock s; LONG l; } old, new;
        BOOL wait;

        do
        {
            old.s = *u.s;
            new = old;

            if (old.s.owners != -1 && !old.s.exclusive_waiters)
            {
                /* Not locked exclusive, and no exclusive waiters.
                 * We can try to grab it. */
                ++new.s.owners;
                wait = FALSE;
            }
            else
            {
                wait = TRUE;
            }
        } while (InterlockedCompareExchange( u.l, new.l, old.l ) != old.l);

        if (!wait) return;
        RtlWaitOnAddress( u.s, &new.s, sizeof(struct srw_lock), NULL );
    }
}

/***********************************************************************
 *              RtlReleaseSRWLockExclusive (NTDLL.@)
 */
void WINAPI RtlReleaseSRWLockExclusive( RTL_SRWLOCK *lock )
{
    union { RTL_SRWLOCK *rtl; struct srw_lock *s; LONG *l; } u = { lock };
    union { struct srw_lock s; LONG l; } old, new;

    do
    {
        old.s = *u.s;
        new = old;

        if (old.s.owners != -1) ERR("Lock %p is not owned exclusive!\n", lock);

        new.s.owners = 0;
    } while (InterlockedCompareExchange( u.l, new.l, old.l ) != old.l);

    if (new.s.exclusive_waiters)
        RtlWakeAddressSingle( &u.s->owners );
    else
        RtlWakeAddressAll( u.s );
}

/***********************************************************************
 *              RtlReleaseSRWLockShared (NTDLL.@)
 */
void WINAPI RtlReleaseSRWLockShared( RTL_SRWLOCK *lock )
{
    union { RTL_SRWLOCK *rtl; struct srw_lock *s; LONG *l; } u = { lock };
    union { struct srw_lock s; LONG l; } old, new;

    do
    {
        old.s = *u.s;
        new = old;

        if (old.s.owners == -1) ERR("Lock %p is owned exclusive!\n", lock);
        else if (!old.s.owners) ERR("Lock %p is not owned shared!\n", lock);

        --new.s.owners;
    } while (InterlockedCompareExchange( u.l, new.l, old.l ) != old.l);

    if (!new.s.owners)
        RtlWakeAddressSingle( &u.s->owners );
}

/***********************************************************************
 *              RtlTryAcquireSRWLockExclusive (NTDLL.@)
 *
 * NOTES
 *  Similarly to AcquireSRWLockExclusive, recursive calls are not allowed
 *  and will fail with a FALSE return value.
 */
BOOLEAN WINAPI RtlTryAcquireSRWLockExclusive( RTL_SRWLOCK *lock )
{
    union { RTL_SRWLOCK *rtl; struct srw_lock *s; LONG *l; } u = { lock };
    union { struct srw_lock s; LONG l; } old, new;
    BOOLEAN ret;

    do
    {
        old.s = *u.s;
        new.s = old.s;

        if (!old.s.owners)
        {
            /* Not locked exclusive or shared. We can try to grab it. */
            new.s.owners = -1;
            ret = TRUE;
        }
        else
        {
            ret = FALSE;
        }
    } while (InterlockedCompareExchange( u.l, new.l, old.l ) != old.l);

    return ret;
}

/***********************************************************************
 *              RtlTryAcquireSRWLockShared (NTDLL.@)
 */
BOOLEAN WINAPI RtlTryAcquireSRWLockShared( RTL_SRWLOCK *lock )
{
    union { RTL_SRWLOCK *rtl; struct srw_lock *s; LONG *l; } u = { lock };
    union { struct srw_lock s; LONG l; } old, new;
    BOOLEAN ret;

    do
    {
        old.s = *u.s;
        new.s = old.s;

        if (old.s.owners != -1 && !old.s.exclusive_waiters)
        {
            /* Not locked exclusive, and no exclusive waiters.
             * We can try to grab it. */
            ++new.s.owners;
            ret = TRUE;
        }
        else
        {
            ret = FALSE;
        }
    } while (InterlockedCompareExchange( u.l, new.l, old.l ) != old.l);

    return ret;
}

/***********************************************************************
 *           RtlInitializeConditionVariable   (NTDLL.@)
 *
 * Initializes the condition variable with NULL.
 *
 * PARAMS
 *  variable [O] condition variable
 *
 * RETURNS
 *  Nothing.
 */
void WINAPI RtlInitializeConditionVariable( RTL_CONDITION_VARIABLE *variable )
{
    variable->Ptr = NULL;
}

/***********************************************************************
 *           RtlWakeConditionVariable   (NTDLL.@)
 *
 * Wakes up one thread waiting on the condition variable.
 *
 * PARAMS
 *  variable [I/O] condition variable to wake up.
 *
 * RETURNS
 *  Nothing.
 *
 * NOTES
 *  The calling thread does not have to own any lock in order to call
 *  this function.
 */
void WINAPI RtlWakeConditionVariable( RTL_CONDITION_VARIABLE *variable )
{
    InterlockedIncrement( (LONG *)&variable->Ptr );
    RtlWakeAddressSingle( variable );
}

/***********************************************************************
 *           RtlWakeAllConditionVariable   (NTDLL.@)
 *
 * See WakeConditionVariable, wakes up all waiting threads.
 */
void WINAPI RtlWakeAllConditionVariable( RTL_CONDITION_VARIABLE *variable )
{
    InterlockedIncrement( (LONG *)&variable->Ptr );
    RtlWakeAddressAll( variable );
}

/***********************************************************************
 *           RtlSleepConditionVariableCS   (NTDLL.@)
 *
 * Atomically releases the critical section and suspends the thread,
 * waiting for a Wake(All)ConditionVariable event. Afterwards it enters
 * the critical section again and returns.
 *
 * PARAMS
 *  variable  [I/O] condition variable
 *  crit      [I/O] critical section to leave temporarily
 *  timeout   [I]   timeout
 *
 * RETURNS
 *  see NtWaitForKeyedEvent for all possible return values.
 */
NTSTATUS WINAPI RtlSleepConditionVariableCS( RTL_CONDITION_VARIABLE *variable, RTL_CRITICAL_SECTION *crit,
                                             const LARGE_INTEGER *timeout )
{
    int value = *(int *)&variable->Ptr;
    NTSTATUS status;

    RtlLeaveCriticalSection( crit );
    status = RtlWaitOnAddress( &variable->Ptr, &value, sizeof(value), timeout );
    RtlEnterCriticalSection( crit );
    return status;
}

/***********************************************************************
 *           RtlSleepConditionVariableSRW   (NTDLL.@)
 *
 * Atomically releases the SRWLock and suspends the thread,
 * waiting for a Wake(All)ConditionVariable event. Afterwards it enters
 * the SRWLock again with the same access rights and returns.
 *
 * PARAMS
 *  variable  [I/O] condition variable
 *  lock      [I/O] SRWLock to leave temporarily
 *  timeout   [I]   timeout
 *  flags     [I]   type of the current lock (exclusive / shared)
 *
 * RETURNS
 *  see NtWaitForKeyedEvent for all possible return values.
 *
 * NOTES
 *  the behaviour is undefined if the thread doesn't own the lock.
 */
NTSTATUS WINAPI RtlSleepConditionVariableSRW( RTL_CONDITION_VARIABLE *variable, RTL_SRWLOCK *lock,
                                              const LARGE_INTEGER *timeout, ULONG flags )
{
    int value = *(int *)&variable->Ptr;
    NTSTATUS status;

    if (flags & RTL_CONDITION_VARIABLE_LOCKMODE_SHARED)
        RtlReleaseSRWLockShared( lock );
    else
        RtlReleaseSRWLockExclusive( lock );

    status = RtlWaitOnAddress( &variable->Ptr, &value, sizeof(value), timeout );

    if (flags & RTL_CONDITION_VARIABLE_LOCKMODE_SHARED)
        RtlAcquireSRWLockShared( lock );
    else
        RtlAcquireSRWLockExclusive( lock );
    return status;
}

/* RtlWaitOnAddress() and RtlWakeAddress*(), hereafter referred to as "Win32
 * futexes", offer futex-like semantics with a variable set of address sizes,
 * but are limited to a single process. They are also fair—the documentation
 * specifies this, and tests bear it out.
 *
 * On Windows they are implemented using NtAlertThreadByThreadId and
 * NtWaitForAlertByThreadId, which manipulate a single flag (similar to an
 * auto-reset event) per thread. This can be tested by attempting to wake a
 * thread waiting in RtlWaitOnAddress() via NtAlertThreadByThreadId.
 */

struct futex_entry
{
    struct list entry;
    const void *addr;
    DWORD tid;
};

struct futex_queue
{
    struct list queue;
    LONG lock;
};

static struct futex_queue futex_queues[256];

static struct futex_queue *get_futex_queue( const void *addr )
{
    ULONG_PTR val = (ULONG_PTR)addr;

    return &futex_queues[(val >> 4) % ARRAY_SIZE(futex_queues)];
}

static void spin_lock( LONG *lock )
{
    while (InterlockedCompareExchange( lock, -1, 0 ))
        YieldProcessor();
}

static void spin_unlock( LONG *lock )
{
    InterlockedExchange( lock, 0 );
}

static BOOL compare_addr( const void *addr, const void *cmp, SIZE_T size )
{
    switch (size)
    {
        case 1:
            return (*(const UCHAR *)addr == *(const UCHAR *)cmp);
        case 2:
            return (*(const USHORT *)addr == *(const USHORT *)cmp);
        case 4:
            return (*(const ULONG *)addr == *(const ULONG *)cmp);
        case 8:
            return (*(const ULONG64 *)addr == *(const ULONG64 *)cmp);
    }

    return FALSE;
}

/***********************************************************************
 *           RtlWaitOnAddress   (NTDLL.@)
 */
NTSTATUS WINAPI RtlWaitOnAddress( const void *addr, const void *cmp, SIZE_T size,
                                  const LARGE_INTEGER *timeout )
{
    struct futex_queue *queue = get_futex_queue( addr );
    struct futex_entry entry;
    NTSTATUS ret;

    TRACE("addr %p cmp %p size %#Ix timeout %s\n", addr, cmp, size, debugstr_timeout( timeout ));

    if (size != 1 && size != 2 && size != 4 && size != 8)
        return STATUS_INVALID_PARAMETER;

    entry.addr = addr;
    entry.tid = GetCurrentThreadId();

    spin_lock( &queue->lock );

    /* Do the comparison inside of the spinlock, to reduce spurious wakeups. */

    if (!compare_addr( addr, cmp, size ))
    {
        spin_unlock( &queue->lock );
        return STATUS_SUCCESS;
    }

    if (!queue->queue.next)
        list_init( &queue->queue );
    list_add_tail( &queue->queue, &entry.entry );

    spin_unlock( &queue->lock );

    ret = NtWaitForAlertByThreadId( NULL, timeout );

    /* We may have already been removed by a call to RtlWakeAddressSingle() or RtlWakeAddressAll(). */
    if (entry.addr)
    {
        spin_lock( &queue->lock );
        if (entry.addr)
            list_remove( &entry.entry );
        spin_unlock( &queue->lock );
    }

    TRACE("returning %#lx\n", ret);

    if (ret == STATUS_ALERTED) ret = STATUS_SUCCESS;
    return ret;
}

/***********************************************************************
 *           RtlWakeAddressAll    (NTDLL.@)
 */
void WINAPI RtlWakeAddressAll( const void *addr )
{
    struct futex_queue *queue = get_futex_queue( addr );
    struct futex_entry *entry, *next;
    unsigned int count = 0, i;
    DWORD tids[256];

    TRACE("%p\n", addr);

    if (!addr) return;

    spin_lock( &queue->lock );

    if (!queue->queue.next)
        list_init(&queue->queue);

    LIST_FOR_EACH_ENTRY_SAFE( entry, next, &queue->queue, struct futex_entry, entry )
    {
        if (entry->addr == addr)
        {
            entry->addr = NULL;
            list_remove( &entry->entry );
            /* Try to buffer wakes, so that we don't make a system call while
             * holding a spinlock. */
            if (count < ARRAY_SIZE(tids))
                tids[count++] = entry->tid;
            else
                NtAlertThreadByThreadId( (HANDLE)(DWORD_PTR)entry->tid );
        }
    }

    spin_unlock( &queue->lock );

    for (i = 0; i < count; ++i)
        NtAlertThreadByThreadId( (HANDLE)(DWORD_PTR)tids[i] );
}

/***********************************************************************
 *           RtlWakeAddressSingle (NTDLL.@)
 */
void WINAPI RtlWakeAddressSingle( const void *addr )
{
    struct futex_queue *queue = get_futex_queue( addr );
    struct futex_entry *entry;
    DWORD tid = 0;

    TRACE("%p\n", addr);

    if (!addr) return;

    spin_lock( &queue->lock );

    if (!queue->queue.next)
        list_init(&queue->queue);

    LIST_FOR_EACH_ENTRY( entry, &queue->queue, struct futex_entry, entry )
    {
        if (entry->addr == addr)
        {
            /* Try to buffer wakes, so that we don't make a system call while
             * holding a spinlock. */
            tid = entry->tid;

            /* Remove this entry from the queue, so that a simultaneous call to
             * RtlWakeAddressSingle() will not also wake it—two simultaneous
             * calls must wake at least two waiters if they exist. */
            entry->addr = NULL;
            list_remove( &entry->entry );
            break;
        }
    }

    spin_unlock( &queue->lock );

    if (tid) NtAlertThreadByThreadId( (HANDLE)(DWORD_PTR)tid );
}
