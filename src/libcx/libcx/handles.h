/*
 * Handle manipulation API.
 * Copyright (C) 2021 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2020.
 *
 * The kLIBC Extension Library is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The kLIBC Extension Library is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the GNU C Library; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef LIBCX_HANDLES_H
#define LIBCX_HANDLES_H

#include <sys/types.h>

__BEGIN_DECLS

#define LIBCX_HANDLE_SHMEM 1
#define LIBCX_HANDLE_FD 2

#define LIBCX_HANDLE_NEW 0x1

#define LIBCX_HANDLE_CLOSE 0x1

#pragma pack(1)
typedef struct
{
  union
  {
    int8_t type; /* LIBCX_HANDLE_SHMEM | LIBCX_HANDLE_FD */
    int16_t _reserved1;
  };
  int16_t flags;
  int32_t value;
} LIBCX_HANDLE;
#pragma pack()

/**
 * Sends selected kLIBC and LIBCx handles to another LIBCx process.
 *
 * Once this function returns a success, the specified handles will be available
 * in the target process as if they were opened there directly or inherited from
 * a parent. If LIBCX_HANDLE_CLOSE is passed in `flags`, the handles will also
 * be closed in the calling (source) process on success.
 *
 * All handles in the array must be valid and belong to the source process. The
 * send operation is synchronous and "atomic" which means that it will either
 * successfully transfer all handles and return a success or transfer none and
 * return a failure.
 *
 * The purpose of this function is to share access to resources between two
 * existing processes. This is done by first calling this function and then
 * communicating the handles to the target process for their actual usage via
 * some sort of IPC. This function is useful if a PID of the target process is
 * known when handles are to be transferred. For other use cases, check
 * `libcx_take_handles`.
 *
 * Note that if the target process already has an open handle of the respected
 * type, this function will try to assign a different handle to the same
 * resource the source handle is associated with, as if it were duplicated. In
 * this case, the new handle will be passed to the caller at the same index of
 * the handles array and the `flags` field at this index will have
 * LIBCX_HANDLE_NEW flag set. The return value in this case will indicate a
 * number of new handles in the array.
 *
 * @param      handles      Array of handles to send.
 * @param[in]  num_handles  Number of entries in handles array.
 * @param[in]  pid          Target process ID.
 * @param[in]  flags        0 or LIBCX_HANDLE_CLOSE.
 *
 * @return     0 or number of new handles on success, otherwise -1 and error
 *             code in `errno`.
 */
int libcx_send_handles(LIBCX_HANDLE *handles, size_t num_handles, pid_t pid, int flags);

/**
 * Takes selected kLIBC and LIBCx handles from another LIBCx process.
 *
 * Once this function returns a success, the specified handles will be made
 * accessible in the calling process as if they were opened there directly or
 * inherited from a parent. If LIBCX_HANDLE_CLOSE is passed in `flags`, the
 * handles will also be closed in another (source) process on success.
 *
 * All handles in the array must be valid and belong to the source process. The
 * take operation is synchronous and "atomic" which means that it will either
 * successfully transfers all handles and return a success or transfer none and
 * return a failure.
 *
 * The purpose of this function is to share access to resources between two
 * existing processes. This is done by first communicating the handles from the
 * source process to the target (calling) process and then calling this function
 * for their actual usage. This function is useful when a PID of the target
 * process is not known when the handles are to be transferred to it by the
 * source prcoess but a PID of the source process is known when they are
 * received in the target process via IPC.
 *
 * Note that if the target (calling) process already has an open handle of the
 * respected type, this function will try to assign a different handle to the
 * same resource the source handle is associated with, as if it were duplicated.
 * In this case, the new handle will be passed to the caller at the same index
 * of the handles array and the `flags` field at this index will have
 * LIBCX_HANDLE_NEW flag set. The return value in this case will indicate a
 * number of new handles in the array.
 *
 * @param      handles      Array of handles to take.
 * @param[in]  num_handles  Number of entries in handles array.
 * @param[in]  pid          Source process ID.
 * @param[in]  flags        0 or LIBCX_HANDLE_CLOSE.
 *
 * @return     0 or number of new handles on success, otherwise -1 and error
 *             code in `errno`.
 */
int libcx_take_handles(LIBCX_HANDLE *handles, size_t num_handles, pid_t pid, int flags);

__END_DECLS

#endif /* LIBCX_HANDLES_H */
