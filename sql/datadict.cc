/* Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2017, 2022, MariaDB corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mariadb.h"
#include "datadict.h"
#include "sql_priv.h"
#include "sql_class.h"
#include "sql_table.h"
#include "ha_sequence.h"

static int read_string(File file, uchar**to, size_t length)
{
  DBUG_ENTER("read_string");

  /* This can't use MY_THREAD_SPECIFIC as it's used on server start */
  if (!(*to= (uchar*) my_malloc(PSI_INSTRUMENT_ME, length+1,MYF(MY_WME))) ||
      mysql_file_read(file, *to, length, MYF(MY_NABP)))
  {
     my_free(*to);
    *to= 0;
    DBUG_RETURN(1);
  }
  *((char*) *to+length)= '\0'; // C-style safety
  DBUG_RETURN (0);
}


/**
  Check type of .frm if we are not going to parse it.

  @param[in]  thd               The current session.
  @param[in]  path              path to FRM file.
  @param[in/out] engine_name    table engine name (length < NAME_CHAR_LEN)

  engine_name is a LEX_CSTRING, where engine_name->str must point to
  a buffer of at least NAME_CHAR_LEN+1 bytes.
  If engine_name is 0, then the function will only test if the file is a
  view or not

  @retval  TABLE_TYPE_UNKNOWN   error  - file can't be opened
  @retval  TABLE_TYPE_NORMAL    table
  @retval  TABLE_TYPE_SEQUENCE  sequence table
  @retval  TABLE_TYPE_VIEW      view
*/

Table_type dd_frm_type(THD *thd, char *path, LEX_CSTRING *engine_name,
                       LEX_CSTRING *partition_engine_name,
                       LEX_CUSTRING *table_version)
{
  File file;
  uchar header[64+ MY_UUID_SIZE + 2];     // Header and uuid
  size_t error;
  Table_type type= TABLE_TYPE_UNKNOWN;
  uchar dbt;
  DBUG_ENTER("dd_frm_type");

  file= mysql_file_open(key_file_frm, path, O_RDONLY | O_SHARE, MYF(0));
  if (file < 0)
    DBUG_RETURN(TABLE_TYPE_UNKNOWN);

  /*
    We return TABLE_TYPE_NORMAL if we can open the .frm file. This allows us
    to drop a bad .frm file with DROP TABLE
  */
  type= TABLE_TYPE_NORMAL;

  /*
    Initialize engine name in case we are not able to find it out
    The cast is safe, as engine_name->str points to a usable buffer.
   */
  if (engine_name)
  {
    engine_name->length= 0;
    ((char*) (engine_name->str))[0]= 0;
  }
  if (partition_engine_name)
  {
    partition_engine_name->length= 0;
    partition_engine_name->str= 0;
  }
  if (table_version)
  {
    table_version->length= 0;
    table_version->str= 0;                      // Allocated if needed
  }
  if (unlikely((error= mysql_file_read(file, (uchar*) header, sizeof(header),
                                       MYF(MY_NABP)))))
    goto err;

  if (unlikely((!strncmp((char*) header, "TYPE=VIEW\n", 10))))
  {
    type= TABLE_TYPE_VIEW;
    goto err;
  }

  if (!is_binary_frm_header(header))
    goto err;

  dbt= header[3];

  if ((header[39] & 0x30) == (HA_CHOICE_YES << 4))
  {
    DBUG_PRINT("info", ("Sequence found"));
    type= TABLE_TYPE_SEQUENCE;
  }

  if (table_version)
  {
    /* Read the table version (if it is a 'new' frm file) */
    if (header[64] == EXTRA2_TABLEDEF_VERSION && header[65] == MY_UUID_SIZE)
      if ((table_version->str= (uchar*) thd->memdup(header + 66, MY_UUID_SIZE)))
        table_version->length= MY_UUID_SIZE;
  }

  /* cannot use ha_resolve_by_legacy_type without a THD */
  if (thd && dbt < DB_TYPE_FIRST_DYNAMIC)
  {
    handlerton *ht= ha_resolve_by_legacy_type(thd, (legacy_db_type) dbt);
    if (ht)
    {
      if (engine_name)
        *engine_name= hton2plugin[ht->slot]->name;
#ifdef WITH_PARTITION_STORAGE_ENGINE
      if (partition_engine_name && dbt == DB_TYPE_PARTITION_DB)
      {
        handlerton *p_ht;
        legacy_db_type new_dbt= (legacy_db_type) header[61];
        if (new_dbt >= DB_TYPE_FIRST_DYNAMIC)
          goto cont;
        if (!(p_ht= ha_resolve_by_legacy_type(thd, new_dbt)))
          goto err;
        *partition_engine_name= *hton_name(p_ht);
      }
#endif // WITH_PARTITION_STORAGE_ENGINE
      goto err;
    }
  }

#ifdef WITH_PARTITION_STORAGE_ENGINE
cont:
#endif
  /* read the true engine name */
  if (engine_name)
  {
    MY_STAT state;
    uchar *frm_image= 0;
    uint n_length;

    if (mysql_file_fstat(file, &state, MYF(MY_WME)))
      goto err;

    MSAN_STAT_WORKAROUND(&state);

    if (mysql_file_seek(file, 0, SEEK_SET, MYF(MY_WME)))
      goto err;

    if (read_string(file, &frm_image, (size_t)state.st_size))
      goto err;

    /* The test for !engine_name->length is only true for partition engine */
    if (!engine_name->length && (n_length= uint4korr(frm_image+55)))
    {
      uint record_offset= uint2korr(frm_image+6)+
                      ((uint2korr(frm_image+14) == 0xffff ?
                        uint4korr(frm_image+47) : uint2korr(frm_image+14)));
      uint reclength= uint2korr(frm_image+16);

      uchar *next_chunk= frm_image + record_offset + reclength;
      uchar *buff_end= next_chunk + n_length;
      uint connect_string_length= uint2korr(next_chunk);
      next_chunk+= connect_string_length + 2;
      if (next_chunk + 2 < buff_end)
      {
        uint len= uint2korr(next_chunk);
        if (len <= NAME_CHAR_LEN)
        {
          /*
            The following cast is safe as the caller has allocated buffer
            and it's up to this function to generate the name.
          */
          strmake((char*) engine_name->str, (char*)next_chunk + 2,
                  engine_name->length= len);
        }
      }
    }

#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (partition_engine_name && dbt == DB_TYPE_PARTITION_DB)
    {
      uint len;
      const uchar *extra2;
      /* Length of the MariaDB extra2 segment in the form file. */
      len = uint2korr(frm_image+4);
      extra2= frm_image + 64;
      if (*extra2 != '/')   // old frm had '/' there
      {
        const uchar *e2end= extra2 + len;
        while (extra2 + 3 <= e2end)
        {
          uchar type= *extra2++;
          size_t length= *extra2++;
          if (!length)
          {
            if (extra2 + 2 >= e2end)
              break;
            length= uint2korr(extra2);
            extra2+= 2;
            if (length < 256)
              break;
          }
          if (extra2 + length > e2end)
            break;
          if (type == EXTRA2_DEFAULT_PART_ENGINE)
          {
            partition_engine_name->str= thd->strmake((char*)extra2, length);
            partition_engine_name->length= length;
            break;
          }
          extra2+= length;
        }
      }
    }
#endif // WITH_PARTITION_STORAGE_ENGINE
    my_free(frm_image);
  }

  /* Probably a table. */
err:
  mysql_file_close(file, MYF(MY_WME));
  DBUG_RETURN(type);
}


/*
  Regenerate a metadata locked table.

  @param  thd   Thread context.
  @param  db    Name of the database to which the table belongs to.
  @param  name  Table name.

  @retval  FALSE  Success.
  @retval  TRUE   Error.
*/

bool dd_recreate_table(THD *thd, const char *db, const char *table_name)
{
  HA_CREATE_INFO create_info;
  char path_buf[FN_REFLEN + 1];
  DBUG_ENTER("dd_recreate_table");

  /* There should be a exclusive metadata lock on the table. */
  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, db, table_name,
                                             MDL_EXCLUSIVE));
  create_info.init();
  build_table_filename(path_buf, sizeof(path_buf) - 1,
                       db, table_name, "", 0);
  /* Attempt to reconstruct the table. */
  DBUG_RETURN(ha_create_table(thd, path_buf, db, table_name, &create_info, 0, 0));
}


bool Extra2_info::read(const uchar *frm_image, size_t frm_size)
{
  read_size= uint2korr(frm_image + 4);

  if (frm_size < FRM_HEADER_SIZE + read_size)
    return true;

  const uchar *pos= frm_image + 64;

  DBUG_ENTER("Extra2_info::read");

  if (*pos == '/')   // old frm had '/' there
    DBUG_RETURN(false);

  const uchar *e2end= pos + read_size;
  while (pos + 3 <= e2end)
  {
    extra2_frm_value_type type= (extra2_frm_value_type)*pos++;
    size_t length= extra2_read_len(&pos, e2end);
    if (!length)
      DBUG_RETURN(true);

    bool fail= false;
    switch (type) {
      case EXTRA2_TABLEDEF_VERSION:
        if (version.str) // see init_from_sql_statement_string()
        {
          if (length != version.length)
            DBUG_RETURN(true);
        }
        else
        {
          version.str= pos;
          version.length= length;
        }
        break;
      case EXTRA2_ENGINE_TABLEOPTS:
        fail= read_once(&options, pos, length);
        break;
      case EXTRA2_DEFAULT_PART_ENGINE:
        engine.set((const char*)pos, length);
        break;
      case EXTRA2_GIS:
        fail= read_once(&gis, pos, length);
        break;
      case EXTRA2_PERIOD_FOR_SYSTEM_TIME:
        fail= read_once(&system_period, pos, length)
          || length != 2 * frm_fieldno_size;
        break;
      case EXTRA2_FIELD_FLAGS:
        fail= read_once(&field_flags, pos, length);
        break;
      case EXTRA2_APPLICATION_TIME_PERIOD:
        fail= read_once(&application_period, pos, length);
        break;
      case EXTRA2_FIELD_DATA_TYPE_INFO:
        fail= read_once(&field_data_type_info, pos, length);
        break;
      case EXTRA2_PERIOD_WITHOUT_OVERLAPS:
        fail= read_once(&without_overlaps, pos, length);
        break;
      case EXTRA2_INDEX_FLAGS:
        fail= read_once(&index_flags, pos, length);
        break;
      default:
        /* abort frm parsing if it's an unknown but important extra2 value */
        if (type >= EXTRA2_ENGINE_IMPORTANT)
          DBUG_RETURN(true);
    }
    if (fail)
      DBUG_RETURN(true);

    pos+= length;
  }
  if (pos != e2end)
    DBUG_RETURN(true);

  DBUG_ASSERT(store_size() == read_size);
  DBUG_RETURN(false);
}


/*
  TODO: now this is used in by MDEV-20865 but this can be also used by
  build_frm_image() (see TODO comment there).
*/
uchar *
Extra2_info::write(uchar *frm_image, size_t frm_size)
{
  // FIXME: what to do with frm_size here (and in read())?
  uchar *pos;
  /* write the extra2 segment */
  pos = frm_image + FRM_HEADER_SIZE;
  compile_time_assert(EXTRA2_TABLEDEF_VERSION != '/');

  if (version.str)
    pos= extra2_write(pos, EXTRA2_TABLEDEF_VERSION, version);

  if (engine.str)
    pos= extra2_write(pos, EXTRA2_DEFAULT_PART_ENGINE, engine);

  if (options.str)
    pos= extra2_write(pos, EXTRA2_ENGINE_TABLEOPTS, options);

  if (gis.str)
    pos= extra2_write(pos, EXTRA2_GIS, gis);

  if (field_data_type_info.str)
    pos= extra2_write(pos, EXTRA2_FIELD_DATA_TYPE_INFO, field_data_type_info);

  if (index_flags.str)
    pos= extra2_write(pos, EXTRA2_INDEX_FLAGS, index_flags);

  if (system_period.str)
    pos= extra2_write(pos, EXTRA2_PERIOD_FOR_SYSTEM_TIME, system_period);

  if (application_period.str)
    pos= extra2_write(pos, EXTRA2_APPLICATION_TIME_PERIOD, application_period);

  if (without_overlaps.str)
    pos= extra2_write(pos, EXTRA2_PERIOD_WITHOUT_OVERLAPS, without_overlaps);

  if (field_flags.str)
    pos= extra2_write(pos, EXTRA2_FIELD_FLAGS, field_flags);

  write_size= pos - frm_image - FRM_HEADER_SIZE;
  DBUG_ASSERT(write_size == store_size());
  DBUG_ASSERT(write_size <= 0xffff - FRM_HEADER_SIZE - 4);

#if 0
  int4store(pos, filepos); // end of the extra2 segment
#endif
  return pos;
}
