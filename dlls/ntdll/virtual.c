/*
 * Win32 virtual memory functions
 *
 * Copyright 1997, 2002 Alexandre Julliard
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

#include "config.h"
#include "wine/port.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#ifdef HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif
#ifdef HAVE_SYS_STAT_H
# include <sys/stat.h>
#endif
#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif
#ifdef HAVE_SYS_SYSINFO_H
# include <sys/sysinfo.h>
#endif
#ifdef HAVE_VALGRIND_VALGRIND_H
# include <valgrind/valgrind.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#define NONAMELESSUNION
#include "windef.h"
#include "winternl.h"
#include "wine/library.h"
#include "wine/server.h"
#include "wine/exception.h"
#include "wine/rbtree.h"
#include "wine/debug.h"
#include "ntdll_misc.h"

WINE_DEFAULT_DEBUG_CHANNEL(virtual);

static const UINT page_shift = 12;
static const UINT_PTR page_mask = 0xfff;

SIZE_T signal_stack_size = 0;
SIZE_T signal_stack_mask = 0;
static SIZE_T signal_stack_align;

#define ROUND_SIZE(addr,size) \
   (((SIZE_T)(size) + ((UINT_PTR)(addr) & page_mask) + page_mask) & ~page_mask)

/**********************************************************************
 *           RtlCreateUserStack (NTDLL.@)
 */
NTSTATUS WINAPI RtlCreateUserStack( SIZE_T commit, SIZE_T reserve, ULONG zero_bits,
                                    SIZE_T commit_align, SIZE_T reserve_align, INITIAL_TEB *stack )
{
    TRACE("commit %#lx, reserve %#lx, zero_bits %u, commit_align %#lx, reserve_align %#lx, stack %p\n",
            commit, reserve, zero_bits, commit_align, reserve_align, stack);

    if (!commit_align || !reserve_align)
        return STATUS_INVALID_PARAMETER;

    if (!commit || !reserve)
    {
        IMAGE_NT_HEADERS *nt = RtlImageNtHeader( NtCurrentTeb()->Peb->ImageBaseAddress );
        if (!reserve) reserve = nt->OptionalHeader.SizeOfStackReserve;
        if (!commit) commit = nt->OptionalHeader.SizeOfStackCommit;
    }

    reserve = (reserve + reserve_align - 1) & ~(reserve_align - 1);
    commit = (commit + commit_align - 1) & ~(commit_align - 1);

    return unix_funcs->virtual_alloc_thread_stack( stack, reserve, commit, NULL );
}


/**********************************************************************
 *           RtlFreeUserStack (NTDLL.@)
 */
void WINAPI RtlFreeUserStack( void *stack )
{
    SIZE_T size = 0;

    TRACE("stack %p\n", stack);

    NtFreeVirtualMemory( NtCurrentProcess(), &stack, &size, MEM_RELEASE );
}

/***********************************************************************
 *           virtual_init
 */
void virtual_init(void)
{
    size_t size = ROUND_SIZE( 0, sizeof(TEB) ) + max( MINSIGSTKSZ, 8192 );
    /* find the first power of two not smaller than size */
    signal_stack_align = page_shift;
    while ((1u << signal_stack_align) < size) signal_stack_align++;
    signal_stack_mask = (1 << signal_stack_align) - 1;
    signal_stack_size = (1 << signal_stack_align) - ROUND_SIZE( 0, sizeof(TEB) );
}


/***********************************************************************
 *           __wine_locked_recvmsg
 */
ssize_t CDECL __wine_locked_recvmsg( int fd, struct msghdr *hdr, int flags )
{
    return unix_funcs->virtual_locked_recvmsg( fd, hdr, flags );
}


/***********************************************************************
 *             NtAllocateVirtualMemory   (NTDLL.@)
 *             ZwAllocateVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtAllocateVirtualMemory( HANDLE process, PVOID *ret, ULONG_PTR zero_bits,
                                         SIZE_T *size_ptr, ULONG type, ULONG protect )
{
    return unix_funcs->NtAllocateVirtualMemory( process, ret, zero_bits, size_ptr, type, protect );
}


/***********************************************************************
 *             NtFreeVirtualMemory   (NTDLL.@)
 *             ZwFreeVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtFreeVirtualMemory( HANDLE process, PVOID *addr_ptr, SIZE_T *size_ptr, ULONG type )
{
    return unix_funcs->NtFreeVirtualMemory( process, addr_ptr, size_ptr, type );
}


/***********************************************************************
 *             NtProtectVirtualMemory   (NTDLL.@)
 *             ZwProtectVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI DECLSPEC_HOTPATCH NtProtectVirtualMemory( HANDLE process, PVOID *addr_ptr, SIZE_T *size_ptr,
                                                          ULONG new_prot, ULONG *old_prot )
{
    return unix_funcs-> NtProtectVirtualMemory( process, addr_ptr, size_ptr, new_prot, old_prot );
}


/***********************************************************************
 *             NtQueryVirtualMemory   (NTDLL.@)
 *             ZwQueryVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtQueryVirtualMemory( HANDLE process, LPCVOID addr,
                                      MEMORY_INFORMATION_CLASS info_class,
                                      PVOID buffer, SIZE_T len, SIZE_T *res_len )
{
    return unix_funcs->NtQueryVirtualMemory( process, addr, info_class, buffer, len, res_len );
}


/***********************************************************************
 *             NtLockVirtualMemory   (NTDLL.@)
 *             ZwLockVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtLockVirtualMemory( HANDLE process, PVOID *addr, SIZE_T *size, ULONG unknown )
{
    return unix_funcs->NtLockVirtualMemory( process, addr, size, unknown );
}


/***********************************************************************
 *             NtUnlockVirtualMemory   (NTDLL.@)
 *             ZwUnlockVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtUnlockVirtualMemory( HANDLE process, PVOID *addr, SIZE_T *size, ULONG unknown )
{
    return unix_funcs->NtUnlockVirtualMemory( process, addr, size, unknown );
}


/***********************************************************************
 *             NtCreateSection   (NTDLL.@)
 *             ZwCreateSection   (NTDLL.@)
 */
NTSTATUS WINAPI NtCreateSection( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr,
                                 const LARGE_INTEGER *size, ULONG protect,
                                 ULONG sec_flags, HANDLE file )
{
    return unix_funcs->NtCreateSection( handle, access, attr, size, protect, sec_flags, file );
}


/***********************************************************************
 *             NtOpenSection   (NTDLL.@)
 *             ZwOpenSection   (NTDLL.@)
 */
NTSTATUS WINAPI NtOpenSection( HANDLE *handle, ACCESS_MASK access, const OBJECT_ATTRIBUTES *attr )
{
    return unix_funcs->NtOpenSection( handle, access, attr );
}


/***********************************************************************
 *             NtMapViewOfSection   (NTDLL.@)
 *             ZwMapViewOfSection   (NTDLL.@)
 */
NTSTATUS WINAPI NtMapViewOfSection( HANDLE handle, HANDLE process, PVOID *addr_ptr, ULONG_PTR zero_bits,
                                    SIZE_T commit_size, const LARGE_INTEGER *offset_ptr, SIZE_T *size_ptr,
                                    SECTION_INHERIT inherit, ULONG alloc_type, ULONG protect )
{
    return unix_funcs->NtMapViewOfSection( handle, process, addr_ptr, zero_bits, commit_size, offset_ptr,
                                           size_ptr, inherit, alloc_type, protect );
}


/***********************************************************************
 *             NtUnmapViewOfSection   (NTDLL.@)
 *             ZwUnmapViewOfSection   (NTDLL.@)
 */
NTSTATUS WINAPI NtUnmapViewOfSection( HANDLE process, PVOID addr )
{
    return unix_funcs->NtUnmapViewOfSection( process, addr );
}


/******************************************************************************
 *             virtual_fill_image_information
 *
 * Helper for NtQuerySection.
 */
void virtual_fill_image_information( const pe_image_info_t *pe_info, SECTION_IMAGE_INFORMATION *info )
{
    info->TransferAddress      = wine_server_get_ptr( pe_info->entry_point );
    info->ZeroBits             = pe_info->zerobits;
    info->MaximumStackSize     = pe_info->stack_size;
    info->CommittedStackSize   = pe_info->stack_commit;
    info->SubSystemType        = pe_info->subsystem;
    info->SubsystemVersionLow  = pe_info->subsystem_low;
    info->SubsystemVersionHigh = pe_info->subsystem_high;
    info->GpValue              = pe_info->gp;
    info->ImageCharacteristics = pe_info->image_charact;
    info->DllCharacteristics   = pe_info->dll_charact;
    info->Machine              = pe_info->machine;
    info->ImageContainsCode    = pe_info->contains_code;
    info->u.ImageFlags         = pe_info->image_flags & ~(IMAGE_FLAGS_WineBuiltin|IMAGE_FLAGS_WineFakeDll);
    info->LoaderFlags          = pe_info->loader_flags;
    info->ImageFileSize        = pe_info->file_size;
    info->CheckSum             = pe_info->checksum;
#ifndef _WIN64 /* don't return 64-bit values to 32-bit processes */
    if (pe_info->machine == IMAGE_FILE_MACHINE_AMD64 || pe_info->machine == IMAGE_FILE_MACHINE_ARM64)
    {
        info->TransferAddress = (void *)0x81231234;  /* sic */
        info->MaximumStackSize = 0x100000;
        info->CommittedStackSize = 0x10000;
    }
#endif
}

/******************************************************************************
 *             NtQuerySection   (NTDLL.@)
 *             ZwQuerySection   (NTDLL.@)
 */
NTSTATUS WINAPI NtQuerySection( HANDLE handle, SECTION_INFORMATION_CLASS class, void *ptr,
                                SIZE_T size, SIZE_T *ret_size )
{
    return unix_funcs->NtQuerySection( handle, class, ptr, size, ret_size );
}


/***********************************************************************
 *             NtFlushVirtualMemory   (NTDLL.@)
 *             ZwFlushVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtFlushVirtualMemory( HANDLE process, LPCVOID *addr_ptr,
                                      SIZE_T *size_ptr, ULONG unknown )
{
    return unix_funcs->NtFlushVirtualMemory( process, addr_ptr, size_ptr, unknown );
}


/***********************************************************************
 *             NtGetWriteWatch   (NTDLL.@)
 *             ZwGetWriteWatch   (NTDLL.@)
 */
NTSTATUS WINAPI NtGetWriteWatch( HANDLE process, ULONG flags, PVOID base, SIZE_T size, PVOID *addresses,
                                 ULONG_PTR *count, ULONG *granularity )
{
    return unix_funcs->NtGetWriteWatch( process, flags, base, size, addresses, count, granularity );
}


/***********************************************************************
 *             NtResetWriteWatch   (NTDLL.@)
 *             ZwResetWriteWatch   (NTDLL.@)
 */
NTSTATUS WINAPI NtResetWriteWatch( HANDLE process, PVOID base, SIZE_T size )
{
    return unix_funcs->NtResetWriteWatch( process, base, size );
}


/***********************************************************************
 *             NtReadVirtualMemory   (NTDLL.@)
 *             ZwReadVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtReadVirtualMemory( HANDLE process, const void *addr, void *buffer,
                                     SIZE_T size, SIZE_T *bytes_read )
{
    return unix_funcs->NtReadVirtualMemory( process, addr, buffer, size, bytes_read );
}


/***********************************************************************
 *             NtWriteVirtualMemory   (NTDLL.@)
 *             ZwWriteVirtualMemory   (NTDLL.@)
 */
NTSTATUS WINAPI NtWriteVirtualMemory( HANDLE process, void *addr, const void *buffer,
                                      SIZE_T size, SIZE_T *bytes_written )
{
    return unix_funcs->NtWriteVirtualMemory( process, addr, buffer, size, bytes_written );
}


/***********************************************************************
 *             NtAreMappedFilesTheSame   (NTDLL.@)
 *             ZwAreMappedFilesTheSame   (NTDLL.@)
 */
NTSTATUS WINAPI NtAreMappedFilesTheSame(PVOID addr1, PVOID addr2)
{
    return unix_funcs->NtAreMappedFilesTheSame( addr1, addr2 );
}
