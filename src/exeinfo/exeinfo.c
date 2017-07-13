/*
 * Implementation of exeinfo API.
 * Copyright (C) 2017 bww bitwise works GmbH.
 * This file is part of the kLIBC Extension Library.
 * Authored by Dmitry Kuminov <coding@dmik.org>, 2017.
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

#include <os2tk45/newexe.h>
#include <os2tk45/exe386.h>

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <io.h>
#include <fcntl.h>
#include <errno.h>

#define TRACE_GROUP TRACE_GROUP_EXEINFO

#include "../shared.h"

#include "libcx/exeinfo.h"

typedef struct
{
  struct exe_hdr hdr;

  union
  {
    struct
    {
      struct e32_exe hdr;
      char *ldr_data;
      struct o32_obj *obj_tab;
      struct o32_map *map_tab;
      struct rsrc32 *res_tab;
      char **obj_data;
    }
    lx;
  };
}
exeinfo_header;

typedef struct _exeinfo
{
  int fd;
  EXEINFO_FORMAT fmt;
  exeinfo_header *exe;
}
exeinfo;

void lx_unexepack1(const char *from, char *to, unsigned short sz)
{
  const char *fp = from, *fe = from + sz;
  char *tp = to;
  unsigned short cnt, len;

  while (fp < fe)
  {
    cnt = *(*(unsigned short **) &fp)++;
    len = *(*(unsigned short **) &fp)++;
    while (cnt--)
    {
      memcpy(tp, fp, len);
      tp += len;
    }
    fp += len;
  }
}

static void lx_unexepack2(const char *from, char *to, unsigned short sz, size_t page_sz)
{
  const char *fp = from, *fe = from + sz;
  char *tp = to;
  union
  {
    struct
    {
      unsigned char b1;
      unsigned char b2;
      unsigned char b3;
    };
    unsigned short word;
    unsigned long dword;
  }
  ctl;
  unsigned char len;

  while (fp < fe)
  {
    ctl.b1 = *fp++;
    switch (ctl.b1 & 0x3)
    {
      case 0:
      {
        if (!ctl.b1)
        {
          /* fill byte */
          len = *fp++;
          unsigned char fill = *fp++;
          memset (tp, fill, len);
          tp += len;
        }
        else
        {
          /* copy block */
          len = ctl.b1 >> 2;
          memcpy (tp, fp, len);
          tp += len;
          fp += len;
        }
        break;
      }
      case 1:
      {
        /* copy block and its sub-block */
        ctl.b2 = *fp++;
        len = (ctl.b1 >> 2) & 0x3;
        memcpy (tp, fp, len);
        tp += len;
        fp += len;
        len = ((ctl.b1 >> 4) & 0x7) + 3;
        memcpy (tp, tp - (ctl.word >> 7), len);
        tp += len;
        break;
      }
      case 2:
      {
        /* copy previous block's sub-block */
        ctl.b2 = *fp++;
        len = ((ctl.b1 >> 2) & 0x3) + 3;
        memcpy (tp, tp - (ctl.word >> 4), len);
        tp += len;
        break;
      }
      case 3:
      {
        /* copy block and its sub-block (big) */
        ctl.b2 = *fp++;
        ctl.b3 = *fp++;
        len = (ctl.b1 >> 2) & 0xF;
        memcpy (tp, fp, len);
        tp += len;
        fp += len;
        len = (ctl.word >> 6) & 0x3F;
        memcpy (tp, tp - ((ctl.dword >> 12) & (page_sz - 1)), len);
        tp += len;
        break;
      }
    }
  }
}

static int lx_load_page(EXEINFO info, unsigned long pn, char *buf)
{
  TRACE("info %p, pn %d\n", info, pn);

  int rc;

  /* Page number is 1-based, convert to 0-based index */
  --pn;

  unsigned long page_sz = info->exe->lx.hdr.e32_pagesize;

  /* Note that page index is 1-based */
  struct o32_map *pg = info->exe->lx.map_tab + pn;

  char *tmp_buf = malloc(page_sz);
  if (!tmp_buf)
    SET_ERRNO_AND(return -1, ENOMEM);

  rc = lseek(info->fd, info->exe->lx.hdr.e32_datapage +
             (pg->o32_pagedataoffset << info->exe->lx.hdr.e32_pageshift),
             SEEK_SET);
  if (rc != -1)
  {
    TRACE("o32_pageflags %d\n", pg->o32_pageflags);
    switch (pg->o32_pageflags)
    {
      case VALID:
        rc = read(info->fd, buf, pg->o32_pagesize);
        break;
      case ZEROED:
        memset(buf, 0, page_sz);
        break;
      case ITERDATA:
        rc = read(info->fd, tmp_buf, pg->o32_pagesize);
        if (rc < pg->o32_pagesize)
          break;
        lx_unexepack1(tmp_buf, buf, pg->o32_pagesize);
        break;
      case ITERDATA2:
        rc = read(info->fd, tmp_buf, pg->o32_pagesize);
        if (rc < pg->o32_pagesize)
          break;
        lx_unexepack2(tmp_buf, buf, pg->o32_pagesize, page_sz);
        break;
      default:
        SET_ERRNO_AND(rc = -1, EILSEQ);
    }
  }

  free(tmp_buf);

  return rc;
}

static const char *lx_load_object(EXEINFO info, int obj_n)
{
  TRACE("info %p, obj_n %d\n", info, obj_n);

  /* Object number is 1-based, convert to 0-based index */
  --obj_n;

  if (info->exe->lx.obj_data[obj_n])
    TRACE_AND(return info->exe->lx.obj_data[obj_n], "already loaded\n");

  unsigned long page_sz = info->exe->lx.hdr.e32_pagesize;

  /* Note that object index is 1-based */
  struct o32_obj *obj = info->exe->lx.obj_tab + obj_n;

  TRACE("o32_size %ld (0x%lx rounded up to 0x%lx)\n",
        obj->o32_size, obj->o32_size, ROUND_UP(obj->o32_size, page_sz));

  char *obj_data = malloc(ROUND_UP(obj->o32_size, page_sz));
  if (!obj_data)
    SET_ERRNO_AND(return NULL, ENOMEM);

  int i;
  for (i = 0; i < obj->o32_mapsize; ++i)
  {
    if (lx_load_page(info, obj->o32_pagemap + i, obj_data + page_sz * i) == -1)
    {
      free(obj_data);
      return NULL;
    }
  }

  info->exe->lx.obj_data[obj_n] = obj_data;

  return obj_data;
}

EXEINFO exeinfo_open(const char *fname)
{
  exeinfo *info;
  int rc;

  TRACE("fname [%s]\n", fname);

  NEW(info);
  if (!info)
    SET_ERRNO_AND(return NULL, ENOMEM);

  info->fd = open(fname, O_RDONLY);
  if (info->fd == -1)
  {
    TRACE_ERRNO("open");
    free(info);
    return NULL;
  }

  info->fmt = EXEINFO_FORMAT_UNKNOWN;

  do
  {
    struct exe_hdr hdr;

    rc = read(info->fd, &hdr, sizeof (hdr));
    if (rc < sizeof(hdr))
      TRACE_ERRNO_AND(break, "read");
    if (hdr.e_magic != EMAGIC)
      TRACE_AND(break, "invalid MZ magic %x\n", hdr.e_magic);

    NEW(info->exe);
    if (!info->exe)
      TRACE_ERRNO_AND(break, "NEW");

    // Keep a copy around for possible further reference
    info->exe->hdr = hdr;

    if (!hdr.e_lfanew)
      TRACE_AND(break, "missing new header\n");

    struct e32_exe *lx = &info->exe->lx.hdr;

    rc = lseek(info->fd, hdr.e_lfanew, SEEK_SET);
    if (rc != -1)
      rc = read(info->fd, lx, sizeof(*lx));
    if (rc < sizeof(*lx))
      TRACE_ERRNO_AND(break, "lseek/read");

    if (lx->e32_magic[0] != E32MAGIC1 || lx->e32_magic[1] != E32MAGIC2)
      TRACE_AND(break, "invalid LX magic %x/%x\n", lx->e32_magic[0], lx->e32_magic[1]);

    TRACE("LX page size %d\n", lx->e32_pagesize);
    TRACE("LX loader section size %d\n", lx->e32_ldrsize);
    TRACE("LX object count %d\n", lx->e32_objcnt);
    TRACE("LX resource count %d (address %x)\n", lx->e32_rsrccnt, lx->e32_rsrctab);

    NEW_ARRAY(info->exe->lx.ldr_data, lx->e32_ldrsize);
    if (!info->exe->lx.ldr_data)
      TRACE_ERRNO_AND(break, "NEW");

    NEW_ARRAY(info->exe->lx.obj_data, lx->e32_objcnt);
    if (!info->exe->lx.obj_data)
      TRACE_ERRNO_AND(break, "NEW");

    rc = lseek(info->fd, hdr.e_lfanew + lx->e32_objtab, SEEK_SET);
    if (rc != -1)
      rc = read(info->fd, info->exe->lx.ldr_data, lx->e32_ldrsize);
    if (rc < lx->e32_ldrsize)
      TRACE_ERRNO_AND(free(info->exe->lx.ldr_data); break, "lseek/read");

    info->exe->lx.obj_tab = (struct o32_obj *)info->exe->lx.ldr_data;
    info->exe->lx.map_tab = (struct o32_map *)(info->exe->lx.ldr_data + (lx->e32_objmap - lx->e32_objtab));
    info->exe->lx.res_tab = (struct rsrc32 *)(info->exe->lx.ldr_data + (lx->e32_rsrctab - lx->e32_objtab));

    info->fmt = EXEINFO_FORMAT_LX;
  }
  while (0);

  TRACE("info %p\n", info);

  return info;
}

EXEINFO_FORMAT exeinfo_get_format(EXEINFO info)
{
  if (!info)
    SET_ERRNO_AND(return -1, EINVAL);

  return info->fmt;
}

int exeinfo_get_resource_data(EXEINFO info, int type, int id, const char **data)
{
  TRACE("info %p, type %d, id %d, data %p\n", info, type, id, data);

  if (!info)
    SET_ERRNO_AND(return -1, EINVAL);

  int i;

  for (i = 0; i < info->exe->lx.hdr.e32_rsrccnt; ++i)
  {
    struct rsrc32 *res = info->exe->lx.res_tab + i;
    if (res->type == type && res->name == id)
    {
      if (res->obj < 1 || res->obj > info->exe->lx.hdr.e32_objcnt)
        SET_ERRNO_AND(return -1, EILSEQ);

      if (data)
      {
        const char *obj_data = lx_load_object(info, res->obj);
        if (!obj_data)
            return -1;

        *data = obj_data + res->offset;
      }

      return res->cb;
    }
  }

  errno = ENOENT;
  return -1;
}

int exeinfo_close(EXEINFO info)
{
  if (!info)
    SET_ERRNO_AND(return -1, EINVAL);

  if (info->fmt == EXEINFO_FORMAT_LX)
  {
    int i;
    for (i = 0; i < info->exe->lx.hdr.e32_objcnt; ++i)
      free(info->exe->lx.obj_data[i]);
    free(info->exe->lx.obj_data);
    free(info->exe->lx.ldr_data);
  }

  free(info->exe);
  close(info->fd);
  free(info);

  return 0;
}
