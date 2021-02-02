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

#define OS2EMX_PLAIN_CHAR
#define INCL_BASE
#include <os2.h>

#include <errno.h>

#include "../shared.h"

#include "libcx/handles.h"
#include "libcx/shmem.h"

#define TR_HANDLE_SHMEM 1
#define TR_HANDLE_SOCKET 2

typedef struct TRANSIT_HANDLE
{
  int8_t type;
  int16_t flags;
  union
  {
    SHMEM h;
    int fd;
    int value;
  };
} TRANSIT_HANDLE;

typedef struct HANDLES_DATA
{
  size_t num_handles;
  int flags;
  TRANSIT_HANDLE handles[0]; /* Must be last! */
} HANDLES_DATA;

static int send_handles_worker(pid_t pid, void *data)
{
  HANDLES_DATA *h_data = (HANDLES_DATA*)data;

  TRACE("pid 0x%x data %p num_handles %u flags 0x%x\n",
        pid, h_data, h_data->num_handles, h_data->flags);

  global_lock();

  /* Process handles after transit */
  for (size_t i = 0; i < h_data->num_handles; ++i)
  {
    TRACE("transit handle %u: type %d flags 0x%x value %d\n", i,
          h_data->handles[i].type, h_data->handles[i].flags, h_data->handles[i].value);

    switch (h_data->handles[i].type)
    {
      case TR_HANDLE_SHMEM:
        if (h_data->handles[i].flags & SHMEM_PUBLIC)
        {
          /* Public memory needs to be opened */
          int rc2 = shmem_open(h_data->handles[i].h, 0);
          if (rc2 == -1 && errno != EPERM)
            ASSERT_NO_PERR(rc2);
        }
        break;
      default:
        ASSERT_FAILED();
        break;
    }
  }

  /* TODO: call GLOBAL_MEM_SET_OWNER(h_data, pid) when it's ready */

  global_unlock();

  return 0;
}

int libcx_send_handles(LIBCX_HANDLE *handles, size_t num_handles, pid_t pid, int flags)
{
  TRACE("handles %p num_handles %u pid 0x%x flags 0x%X\n", handles, num_handles, pid, flags);

  int rc = -1;

  /* Input validation */
  if (!handles || !num_handles)
  {
    errno = EINVAL;
    TRACE_PERR(rc);
    return rc;
  }

  global_lock();

  HANDLES_DATA *h_data = NULL;

  do
  {
    GLOBAL_NEW_PLUS_ARRAY(h_data, h_data->handles, num_handles);
    if (!h_data)
    {
      errno = ENOMEM;
      break;
    }

    rc = 0;

    /* Check & convert all handles */
    for (size_t i = 0; i < num_handles && rc == 0; ++i)
    {
      TRACE("handle %u: type %d flags 0x%x value %d\n", i,
            handles[i].type, handles[i].flags, handles[i].value);

      /* Check for duplcates */
      for (size_t j = 0; j < i; ++j)
      {
        if (handles[j].type == handles[i].type && handles[j].value == handles[i].value)
        {
          errno = EINVAL;
          rc = -1;
          break;
        }
      }

      if (rc != 0)
        break;

      switch (handles[i].type)
      {
        case LIBCX_HANDLE_SHMEM:
        {
          SHMEM h = (SHMEM)handles[i].value;
          int flags;
          if (shmem_get_info(h, &flags, NULL, NULL) != 0)
          {
            errno = EINVAL;
            rc = -1;
            break;
          }
          h_data->handles[i].type = TR_HANDLE_SHMEM;
          if (flags & SHMEM_PUBLIC)
            h_data->handles[i].flags = SHMEM_PUBLIC;
          h_data->handles[i].h = h;
          break;
        }

        default:
          errno = EINVAL;
          rc = -1;
          break;
      }
    }

    if (rc != 0)
      break;

    h_data->num_handles = num_handles;
    h_data->flags = flags;

    /* Prepare handles for transit */
    for (size_t i = 0; i < h_data->num_handles; ++i)
    {
      switch (h_data->handles[i].type)
      {
        case TR_HANDLE_SHMEM:
          if (!(h_data->handles[i].flags & SHMEM_PUBLIC))
          {
            /* Private memory needs to be given to the other process */
            int rc2 = shmem_give(h_data->handles[i].h, pid, 0);
            if (rc2 == -1 && errno != EPERM)
              ASSERT_NO_PERR(rc2);
          }
          break;
        default:
          ASSERT_FAILED();
          break;
      }
    }

    /* TODO: call GLOBAL_MEM_SET_OWNER(h_data, pid) when it's ready */
  }
  while (0);

  global_unlock();

  if (rc == 0)
  {
    /* Perform the waiting interrupt request */
    INTERRUPT_RESULT result;
    rc = interrupt_request(pid, send_handles_worker, h_data, &result);

    if (rc == 0)
    {
      global_lock();

      /* Process handles after transit */
      for (size_t i = 0; i < num_handles; ++i)
      {
        switch (handles[i].type)
        {
          case LIBCX_HANDLE_SHMEM:
            if (flags & LIBCX_HANDLE_CLOSE)
            {
              int rc2 = shmem_close((SHMEM)handles[i].value);
              if (rc2 == -1 && errno != EINVAL)
                ASSERT_NO_PERR(rc2);
            }
            break;
          default:
            ASSERT_FAILED();
            break;
        }
      }

      global_unlock();

      /* Note: Important to release the result after processing handles! */
      interrupt_request_release(result);
    }
  }

  /* TODO: use GLOBAL_DEL(h_data) when it's ready */
  if (h_data)
    free(h_data);

  TRACE_PERR(rc);
  return rc;

}

static int take_handles_worker(pid_t pid, void *data)
{
  HANDLES_DATA *h_data = (HANDLES_DATA*)data;

  TRACE("pid 0x%x data %p num_handles %u flags 0x%x\n",
        pid, h_data, h_data->num_handles, h_data->flags);

  int rc = 0;

  global_lock();

  /* Prepare handles for transit (or close them afterwards) */
  for (size_t i = 0; i < h_data->num_handles; ++i)
  {
    TRACE("transit handle %u: type %d flags 0x%x value %d\n", i,
          h_data->handles[i].type, h_data->handles[i].flags, h_data->handles[i].value);

    switch (h_data->handles[i].type)
    {
      case TR_HANDLE_SHMEM:
      {
        if (h_data->flags & LIBCX_HANDLE_CLOSE)
        {
          int rc2 = shmem_close(h_data->handles[i].h);
          if (rc2 == -1 && errno != EINVAL)
            ASSERT_NO_PERR(rc2);
        }
        else
        {
          SHMEM h = h_data->handles[i].h;
          int flags;
          if (shmem_get_info(h_data->handles[i].h, &flags, NULL, NULL) != 0)
          {
            errno = EINVAL;
            rc = -1;
            break;
          }
          if (flags & SHMEM_PUBLIC)
            h_data->handles[i].flags = SHMEM_PUBLIC;
          else
          {
            /* Private memory needs to be given to the other process */
            int rc2 = shmem_give(h_data->handles[i].h, pid, 0);
            if (rc2 == -1 && errno != EPERM)
              ASSERT_NO_PERR(rc2);
          }
        }
        break;
      }
      default:
        ASSERT_FAILED();
        break;
    }
  }

  if (h_data->flags & LIBCX_HANDLE_CLOSE)
  {
    /* TODO: use GLOBAL_DEL(h_data) when it's ready */
    free(h_data);
  }
  else
  {
    /* TODO: call GLOBAL_MEM_SET_OWNER(h_data, pid) when it's ready */
  }

  global_unlock();

  TRACE_PERR(rc);
  return rc == 0 ? 0 : errno;
}

int libcx_take_handles(LIBCX_HANDLE *handles, size_t num_handles, pid_t pid, int flags)
{
  TRACE("handles %p num_handles %u pid 0x%x flags 0x%X\n", handles, num_handles, pid, flags);

  int rc = -1;

  /* Input validation */
  if (!handles || !num_handles)
  {
    errno = EINVAL;
    TRACE_PERR(rc);
    return rc;
  }

  global_lock();

  HANDLES_DATA *h_data = NULL;

  rc = 0;

  do
  {
    GLOBAL_NEW_PLUS_ARRAY(h_data, h_data->handles, num_handles);

    if (!h_data)
    {
      errno = ENOMEM;
      break;
    }

    /* Check & convert all handles */
    for (size_t i = 0; i < num_handles && rc == 0; ++i)
    {
      TRACE("handle %u: type %d flags 0x%x value %d\n", i,
            handles[i].type, handles[i].flags, handles[i].value);

      /* Check for duplcates */
      for (size_t j = 0; j < i; ++j)
      {
        if (handles[j].type == handles[i].type && handles[j].value == handles[i].value)
        {
          errno = EINVAL;
          rc = -1;
          break;
        }
      }

      if (rc != 0)
        break;

      switch (handles[i].type)
      {
        case LIBCX_HANDLE_SHMEM:
        {
          h_data->handles[i].type = TR_HANDLE_SHMEM;
          h_data->handles[i].h = (SHMEM)handles[i].value;
          break;
        }

        default:
          errno = EINVAL;
          rc = -1;
          break;
      }
    }

    if (rc != 0)
      break;

    h_data->num_handles = num_handles;
    h_data->flags = (flags & ~LIBCX_HANDLE_CLOSE);

    /* TODO: call GLOBAL_MEM_SET_OWNER(h_data, pid) when it's ready */
  }
  while (0);

  global_unlock();

  if (rc == 0)
  {
    /* Perform the waiting interrupt request */
    INTERRUPT_RESULT result;
    rc = interrupt_request(pid, take_handles_worker, h_data, &result);

    if (rc == 0)
    {
      int request_rc = interrupt_request_rc(result);

      if (request_rc == 0)
      {
        global_lock();

        /* Process handles after transit */
        for (size_t i = 0; i < h_data->num_handles; ++i)
        {
          switch (h_data->handles[i].type)
          {
            case TR_HANDLE_SHMEM:
              if (h_data->handles[i].flags & SHMEM_PUBLIC)
              {
                /* Public memory needs to be opened */
                int rc2 = shmem_open(h_data->handles[i].h, 0);
                if (rc2 == -1 && errno != EPERM)
                  ASSERT_NO_PERR(rc2);
              }
              break;
            default:
              ASSERT_FAILED();
              break;
          }
        }

        global_unlock();
      }
      else
      {
        errno = request_rc;
        rc = -1;
      }

      /* Note: Important to release the result after processing handles! */
      interrupt_request_release(result);

      /* Now instruct the other party to close handles */
      if (rc == 0 && (flags & LIBCX_HANDLE_CLOSE))
      {
        h_data->flags = LIBCX_HANDLE_CLOSE;

        /* Refresh source handles (as the worker could change them) */
        for (size_t i = 0; i < num_handles && rc == 0; ++i)
        {
          switch (handles[i].type)
          {
            case LIBCX_HANDLE_SHMEM:
            {
              h_data->handles[i].type = TR_HANDLE_SHMEM;
              h_data->handles[i].h = (SHMEM)handles[i].value;
              break;
            }
          }
        }

        /* TODO: call GLOBAL_MEM_SET_OWNER(h_data, pid) when it's ready */

        rc = interrupt_request(pid, take_handles_worker, h_data, &result);

        if (rc == 0)
          interrupt_request_release(result);

        /* The worker is responsible to free h_data in after closing handles */
        h_data = NULL;
      }
    }
  }

  /* TODO: use GLOBAL_DEL(h_data) when it's ready */
  if (h_data)
    free(h_data);

  TRACE_PERR(rc);
  return rc;
}
