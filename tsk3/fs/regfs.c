/*
** The Sleuth Kit 
**
** Willi Ballenthin [william.ballenthin <at> mandiant [dot] com]
** Copyright (c) 2012 Willi Ballenthin.  All rights reserved
**
** This software is distributed under the Common Public License 1.0 
*/

/**
 *\file regfs.c
 * Contains the internal TSK Registry file system functions.
 */

#include "tsk_fs_i.h"
#include "tsk_regfs.h"

static TSK_RETVAL_ENUM
regfs_utf16to8(TSK_ENDIAN_ENUM endian, char *error_class,
	       uint8_t *utf16, ssize_t utf16_length,
	       char *utf8, ssize_t utf8_length) {
  UTF16 *name16;
  UTF8 *name8;
  int retVal;
  
  name16 = (UTF16 *) utf16;
  name8 = (UTF8 *) utf8;
  retVal = tsk_UTF16toUTF8(endian, 
			   (const UTF16 **) &name16,
			   (UTF16 *) ((uintptr_t) name16 + utf16_length),
			   &name8,
			   (UTF8 *) ((uintptr_t) name8 + utf8_length),
			   TSKlenientConversion);
  if (retVal != TSKconversionOK) {
    if (tsk_verbose)
      tsk_fprintf(stderr, "fsstat: Error converting %s to UTF8: %d",
		  error_class, retVal);
    *name8 = '\0';
  }
  else if ((uintptr_t) name8 >= (uintptr_t) utf8 + utf8_length) {
    /* Make sure it is NULL Terminated */
    utf8[utf8_length - 1] = '\0';
  }
  else {
    *name8 = '\0';
  }
  return TSK_OK;
}

/**
 * Given the address as `inum`, load metadata about the Cell into 
 * the cell pointed to by `cell`.
 * @return TSK_OK on success, TSK_ERR on error.
 */
static TSK_RETVAL_ENUM
reg_load_cell(TSK_FS_INFO *fs, REGFS_CELL *cell, TSK_INUM_T inum) {
  ssize_t count;
  uint32_t len;
  uint16_t type;
  uint8_t  buf[4];

  if (inum < fs->first_block || inum > fs->last_block) {
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_BLK_NUM);
    tsk_error_set_errstr("Invalid block number to load: %" PRIuDADDR "", inum);
    return TSK_ERR;
  }

  cell->inum = inum;

  count = tsk_fs_read(fs, inum, (char *)buf, 4);
  if (count != 4) {
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_READ);
    tsk_error_set_errstr("Failed to read cell structure");
    return TSK_ERR;
  }

  len = (tsk_getu32(fs->endian, buf));
  if (len & 1 << 31) {
    cell->is_allocated = 1;
    cell->length = (-1 * tsk_gets32(fs->endian, buf));
  } else {
    cell->is_allocated = 0;
    cell->length = (tsk_getu32(fs->endian, buf));
  }
  if (cell->length >= HBIN_SIZE) {
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
    tsk_error_set_errstr("Registry cell corrupt: size too large (%" PRIuINUM ")",
			 cell->length);
    return TSK_ERR;
  }

  count = tsk_fs_read(fs, inum + 4, (char *)buf, 2);
  if (count != 2) {
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_READ);
    tsk_error_set_errstr("Failed to read cell structure");
    return TSK_ERR;
  }
  type = (tsk_getu16(fs->endian, buf));

  switch (type) {
  case 0x6b76:
    cell->type = TSK_REGFS_RECORD_TYPE_VK;
    break;
  case 0x6b6e:
    cell->type = TSK_REGFS_RECORD_TYPE_NK;
    break;
  case 0x666c:
    cell->type = TSK_REGFS_RECORD_TYPE_LF;
    break;
  case 0x686c:
    cell->type = TSK_REGFS_RECORD_TYPE_LH;
    break;
  case 0x696c:
    cell->type = TSK_REGFS_RECORD_TYPE_LI;
    break;
  case 0x6972:
    cell->type = TSK_REGFS_RECORD_TYPE_RI;
    break;
  case 0x6b73:
    cell->type = TSK_REGFS_RECORD_TYPE_SK;
    break;
  case 0x6264:
    cell->type = TSK_REGFS_RECORD_TYPE_DB;
    break;
  default:
    cell->type = TSK_REGFS_RECORD_TYPE_UNKNOWN;
    break;
  }
  
  return TSK_OK;
}


/**
 * @return 1 on error, 0 otherwise.
 */
uint8_t
reg_block_walk(TSK_FS_INFO * fs,
    TSK_DADDR_T a_start_blk, TSK_DADDR_T a_end_blk,
    TSK_FS_BLOCK_WALK_FLAG_ENUM a_flags, TSK_FS_BLOCK_WALK_CB a_action,
    void *a_ptr)
{
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    REGFS_CELL cell;

    tsk_error_reset();

    if (a_start_blk < fs->first_block || a_start_blk > fs->last_block) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_WALK_RNG);
        tsk_error_set_errstr("%s: Start block: %" PRIuDADDR "", myname,
            a_start_blk);
        return 1;
    }
    if (a_end_blk < fs->first_block || a_end_blk > fs->last_block) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_WALK_RNG);
        tsk_error_set_errstr("%s: End block: %" PRIuDADDR "", myname,
            a_end_blk);
        return 1;
    }

    if (tsk_verbose) {
      tsk_fprintf(stderr,
		  "regfs_block_walk: Block Walking %" PRIuDADDR " to %"
		  PRIuDADDR "\n", a_start_blk, a_end_blk);
    }

    /* Sanity check on a_flags -- make sure at least one ALLOC is set */
    if (((a_flags & TSK_FS_BLOCK_WALK_FLAG_ALLOC) == 0) &&
        ((a_flags & TSK_FS_BLOCK_WALK_FLAG_UNALLOC) == 0)) {
        a_flags |=
            (TSK_FS_BLOCK_WALK_FLAG_ALLOC |
            TSK_FS_BLOCK_WALK_FLAG_UNALLOC);
    }

    if (((a_flags & TSK_FS_BLOCK_WALK_FLAG_META) == 0) &&
        ((a_flags & TSK_FS_BLOCK_WALK_FLAG_CONT) == 0)) {
        a_flags |=
            (TSK_FS_BLOCK_WALK_FLAG_CONT | TSK_FS_BLOCK_WALK_FLAG_META);
    }

    if ((fs_block = tsk_fs_block_alloc(fs)) == NULL) {
        return 1;
    }

    addr = a_start_blk;

    uint32_t current_hbin_start;

    current_hbin_start = addr - (addr % HBIN_SIZE);

    while (addr < a_end_blk) {
      int myflags;      

      // TODO(wb) be sure not to overrun image


      if (reg_load_cell(fs, &cell, addr) == TSK_ERR) {
	tsk_fs_block_free(fs_block);
	return 1;
      }
      myflags = 0;

      if (cell.is_allocated) {
	myflags = TSK_FS_BLOCK_FLAG_ALLOC;
      } else {
	myflags = TSK_FS_BLOCK_FLAG_UNALLOC;
      }

      switch (cell.type) {
      case TSK_REGFS_RECORD_TYPE_NK: // fall through intended
      case TSK_REGFS_RECORD_TYPE_LF: // fall through intended
      case TSK_REGFS_RECORD_TYPE_LH: // fall through intended
      case TSK_REGFS_RECORD_TYPE_LI: // fall through intended
      case TSK_REGFS_RECORD_TYPE_RI: // fall through intended
      case TSK_REGFS_RECORD_TYPE_DB: // fall through intended
      case TSK_REGFS_RECORD_TYPE_SK: // fall through intended
      case TSK_REGFS_RECORD_TYPE_VK: // fall through intended
	myflags |= TSK_FS_BLOCK_FLAG_META;
	break;
      case TSK_REGFS_RECORD_TYPE_UNKNOWN:
      default:
	myflags |= TSK_FS_BLOCK_FLAG_CONT;
	break;
      }

      if (addr + cell.length > current_hbin_start + HBIN_SIZE - 1) {
	// The Cell overran into the next HBIN header
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_BLK_NUM);
        tsk_error_set_errstr("Cell overran into subsequent HBIN header");
	tsk_fs_block_free(fs_block);
        return 1;
      }


      addr += cell.length;

      // skip HBIN headers
      if (addr > current_hbin_start + HBIN_SIZE) {
	current_hbin_start += HBIN_SIZE;
	addr = current_hbin_start + 0x20;
      }
    }

    tsk_fs_block_free(fs_block);
    return 0;
}

reg_block_getflags(TSK_FS_INFO * fs, TSK_DADDR_T a_addr)
{
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    return 0;
}

static uint8_t
reg_inode_walk(TSK_FS_INFO * fs, TSK_INUM_T start_inum,
    TSK_INUM_T end_inum, TSK_FS_META_FLAG_ENUM flags,
    TSK_FS_META_WALK_CB a_action, void *ptr)
{
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    return 0;
}

static TSK_FS_ATTR_TYPE_ENUM
reg_get_default_attr_type(const TSK_FS_FILE * a_file)
{
    if ((a_file == NULL) || (a_file->meta == NULL))
        return TSK_FS_ATTR_TYPE_DEFAULT;

    /* Use DATA for files and IDXROOT for dirs */
    if (a_file->meta->type == TSK_FS_META_TYPE_DIR)
        return TSK_FS_ATTR_TYPE_NTFS_IDXROOT;
    else
        return TSK_FS_ATTR_TYPE_NTFS_DATA;
}

/** \internal
 * Load the attributes.
 * @param a_fs_file File to load attributes for.
 * @returns 1 on error
 */
static uint8_t
reg_load_attrs(TSK_FS_FILE * a_fs_file)
{
    return 0;
}

/**
 * Read an MFT entry and save it in the generic TSK_FS_META format.
 *
 * @param fs File system to read from.
 * @param mftnum Address of mft entry to read
 * @returns 1 on error
 */
static uint8_t
reg_inode_lookup(TSK_FS_INFO * fs, TSK_FS_FILE * a_fs_file,
    TSK_INUM_T mftnum)
{
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    return 0;
}

TSK_RETVAL_ENUM
reg_dir_open_meta(TSK_FS_INFO * fs, TSK_FS_DIR ** a_fs_dir,
    TSK_INUM_T a_addr)
{
    REGFS_INFO *ntfs = (REGFS_INFO *) fs;
    return 0;
}

/**
 * Print details about the file system to a file handle.
 *
 * @param fs File system to print details on
 * @param hFile File handle to print text to
 *
 * @returns 1 on error and 0 on success
 */
static uint8_t
reg_fsstat(TSK_FS_INFO * fs, FILE * hFile)
{
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    char asc[512];

    tsk_fprintf(hFile, "\nFILE SYSTEM INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "File System Type: Windows Registry\n");

    // TODO(wb): print readable versions
    tsk_fprintf(hFile, "Major Version: %d\n", 
		(tsk_getu32(fs->endian, reg->regf.major_version)));
    tsk_fprintf(hFile, "Minor Version: %d\n", 
		(tsk_getu32(fs->endian, reg->regf.minor_version)));

    if ((tsk_getu32(fs->endian, reg->regf.seq1) == 
	 (tsk_getu32(fs->endian, reg->regf.seq2)))) {
      tsk_fprintf(hFile, "Synchronized: %s\n", "Yes");
    } else {
      tsk_fprintf(hFile, "Synchronized: %s\n", "No");
    }

    if (regfs_utf16to8(fs->endian, "REGF hive name label",
		       reg->regf.hive_name, 30,
		       asc, 512) != TSK_OK) {
	tsk_error_reset();
	tsk_error_set_errno(TSK_ERR_FS_UNICODE);
	tsk_error_set_errstr("Failed to convert REGF hive name string to UTF-8");
	return 1;
    }
    tsk_fprintf(hFile, "Hive name: %s\n", asc);    


    tsk_fprintf(hFile, "\nMETADATA INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    tsk_fprintf(hFile, "Offset to first key: %" PRIu32 "\n",
		(tsk_getu32(fs->endian, reg->regf.first_key_offset)));

    tsk_fprintf(hFile, "Offset to last HBIN: %" PRIu32 "\n",
		(tsk_getu32(fs->endian, reg->regf.last_hbin_offset)));

    tsk_fprintf(hFile, "\nCONTENT INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Number of active cells: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of inactive cells: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of active bytes: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of inactive bytes: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of VK records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of NK records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of LF records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of LH records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of LI records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of RI records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of SK records: <unknown>\n"); //  TODO(wb)
    tsk_fprintf(hFile, "Number of DB records: <unknown>\n"); //  TODO(wb)

    return 0;
}

static uint8_t
reg_fscheck(TSK_FS_INFO * fs, FILE * hFile)
{
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_UNSUPFUNC);
    tsk_error_set_errstr("fscheck not implemented for Windows Registries yet");
    return 1;
}

static TSK_RETVAL_ENUM
reg_istat_vk(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "\nRECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "VK");
    return TSK_OK;
}


static TSK_RETVAL_ENUM
reg_istat_nk(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    ssize_t count;
    uint8_t buf[HBIN_SIZE];
    REGFS_CELL_NK *nk;
    char s[512]; // to be used throughout, temporarily
    uint16_t name_length;

    // TODO(wb): need a call to the following function
    // tsk_fs_file_open_meta(TSK_FS_INFO * a_fs, TSK_FS_FILE * a_fs_file, TSK_INUM_T a_addr)
    // and use the TSK_FS_FILE structure instead of
    // directly accessing the REGFS_CELL_NK memory structure


    if (cell->length > HBIN_SIZE) {
      tsk_error_reset();
      tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
      tsk_error_set_errstr("Registry cell corrupt: size too large 4");
      return TSK_ERR;
    }

    count = tsk_fs_read(fs, (cell->inum), (char *)buf, cell->length);
    if (count != cell->length) {
      tsk_error_reset();
      tsk_error_set_errno(TSK_ERR_FS_READ);
      tsk_error_set_errstr("Failed to read cell structure");
      return TSK_ERR;
    }

    tsk_fprintf(hFile, "\nRECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "NK");

    nk = (REGFS_CELL_NK *)(buf + 4);

    if ((tsk_gets32(fs->endian, nk->classname_offset)) == 0xFFFFFFFF) {
      tsk_fprintf(hFile, "Class Name: %s\n", "None");
    } else {
      char asc[512];
      uint32_t classname_offset;
      uint32_t classname_length;

      classname_offset = (tsk_gets32(fs->endian, nk->classname_offset));
      classname_length = (tsk_gets16(fs->endian, nk->classname_length));

      if (classname_length > 512) {
	tsk_error_reset();
	tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
	tsk_error_set_errstr("NK classname string too long");
	return TSK_ERR;
      }

      count = tsk_fs_read(fs, FIRST_HBIN_OFFSET + classname_offset + 4, 
			  s, classname_length);
      if (count != classname_length) {
	tsk_error_reset();
	tsk_error_set_errno(TSK_ERR_FS_READ);
	tsk_error_set_errstr("Failed to read NK classname string");
	return TSK_ERR;
      }

      if (regfs_utf16to8(fs->endian, "NK class name", (uint8_t *)s, 
			 512, asc, 512) != TSK_OK) {
	tsk_error_reset();
	tsk_error_set_errno(TSK_ERR_FS_UNICODE);
	tsk_error_set_errstr("Failed to convert NK classname string to UTF-8");
	return TSK_ERR;
      }

      tsk_fprintf(hFile, "Class Name: %s\n", asc);    
    }

    name_length = (tsk_getu16(fs->endian, nk->name_length));
    if (name_length > 512) {
	tsk_error_reset();
	tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
	tsk_error_set_errstr("NK key name string too long");
	return TSK_ERR;
    }

    strncpy(s, nk->name, name_length);
    tsk_fprintf(hFile, "Key Name: %s\n", s);

    if ((tsk_getu16(fs->endian, nk->is_root)) == 0x2C) {
      tsk_fprintf(hFile, "Root Record: %s\n", "Yes");
    } else {
      tsk_fprintf(hFile, "Root Record: %s\n", "No");
    }

    if (sec_skew != 0) {
        tsk_fprintf(hFile, "\nAdjusted Entry Times:\n");

        if (fs_meta->mtime)
            fs_meta->mtime -= sec_skew;

        tsk_fprintf(hFile, "Modified:\t%s\n",
            tsk_fs_time_to_str(fs_meta->mtime, timeBuf));

        if (fs_meta->mtime == 0)
            fs_meta->mtime += sec_skew;

        tsk_fprintf(hFile, "\nOriginal Entry Times:\n");
    }
    else {
        tsk_fprintf(hFile, "\Entry Times:\n");
    }
    tsk_fprintf(hFile, "Modified:\t%s\n", tsk_fs_time_to_str(fs_meta->mtime,
            timeBuf));


    tsk_fprintf(hFile, "Parent Record: %" PRIuINUM "\n", 
		FIRST_HBIN_OFFSET + (tsk_getu32(fs->endian, nk->parent_nk_offset)));

    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_lf(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "LF");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_lh(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "LH");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_li(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "LI");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_ri(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "RI");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_sk(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "SK");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_db(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "DB");
    return TSK_OK;
}

static TSK_RETVAL_ENUM
reg_istat_unknown(TSK_FS_INFO * fs, FILE * hFile,
		  REGFS_CELL *cell, TSK_DADDR_T numblock, int32_t sec_skew) {
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    ssize_t count;
    uint8_t buf[HBIN_SIZE];

    if (cell->length > HBIN_SIZE) {
      tsk_error_reset();
      tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
      tsk_error_set_errstr("Registry cell corrupt: size too large 2");
      return TSK_ERR;
    }

    count = tsk_fs_read(fs, (cell->inum), (char *)buf, cell->length);
    if (count != cell->length) {
      tsk_error_reset();
      tsk_error_set_errno(TSK_ERR_FS_READ);
      tsk_error_set_errstr("Failed to read cell structure");
      return TSK_ERR;
    }

    tsk_fprintf(hFile, "RECORD INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");
    tsk_fprintf(hFile, "Record Type: %s\n", "Unknown (Data Record?)");
    tsk_fprintf(hFile, "Type identifier: 0x%x%x\n", *(buf + 4), *(buf + 5));
    return TSK_OK;
}





/**
 * Print details on a specific file to a file handle.
 *
 * @param fs File system file is located in
 * @param hFile File name to print text to
 * @param inum Address of file in file system
 * @param numblock The number of blocks in file to force print (can go beyond file size)
 * @param sec_skew Clock skew in seconds to also print times in
 *
 * @returns 1 on error and 0 on success
 */
static uint8_t
reg_istat(TSK_FS_INFO * fs, FILE * hFile,
    TSK_INUM_T inum, TSK_DADDR_T numblock, int32_t sec_skew)
{
    REGFS_INFO *reg = (REGFS_INFO *) fs;
    REGFS_CELL cell;

    tsk_fprintf(hFile, "\nCELL INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    if (reg_load_cell(fs, &cell, inum) != TSK_OK) {
      return 1;
    }

    tsk_fprintf(hFile, "Cell: %" PRIuINUM "\n", inum);    
    if (cell.is_allocated) {
      tsk_fprintf(hFile, "Allocated: %s\n", "Yes");    
    } else {
      tsk_fprintf(hFile, "Allocated: %s\n", "No");    
    }
    tsk_fprintf(hFile, "Cell Size: %" PRIuINUM "\n", cell.length);

    switch (cell.type) {
    case TSK_REGFS_RECORD_TYPE_VK:
      reg_istat_vk(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_NK:
      reg_istat_nk(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_LF:
      reg_istat_lf(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_LH:
      reg_istat_lh(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_LI:
      reg_istat_li(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_RI:
      reg_istat_ri(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_SK:
      reg_istat_sk(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_DB:
      reg_istat_db(fs, hFile, &cell, numblock, sec_skew);
      break;
    case TSK_REGFS_RECORD_TYPE_UNKNOWN:
      // fall through intended
    default:
      reg_istat_unknown(fs, hFile, &cell, numblock, sec_skew);
      break;
    }

    return 0;
}

static void
reg_close(TSK_FS_INFO * fs)
{
    REGFS_INFO *reg = (REGFS_INFO *) fs;

    if (fs == NULL)
        return;
    tsk_fs_free(fs);
}

int
reg_name_cmp(TSK_FS_INFO * a_fs_info, const char *s1, const char *s2)
{
    return strcasecmp(s1, s2);
}

/**
 * @brief reg_journal_unsupported
 */
static void
reg_journal_unsupported() {
    tsk_error_reset();
    tsk_error_set_errno(TSK_ERR_FS_UNSUPFUNC);
    tsk_error_set_errstr("The Windows Registry does not have a journal.\n");
    return;
}

/**
 * @brief reg_jblk_walk
 * @param fs
 * @param start
 * @param end
 * @param flags
 * @param a_action
 * @param ptr
 * @return 1, as this is unsupported.
 */
static uint8_t
reg_jblk_walk(TSK_FS_INFO * fs, TSK_DADDR_T start,
    TSK_DADDR_T end, int flags, TSK_FS_JBLK_WALK_CB a_action, void *ptr)
{
    reg_journal_unsupported();
    return 1;
}

/**
 * @brief reg_jentry_walk
 * @param fs
 * @param flags
 * @param a_action
 * @param ptr
 * @return 1, as this is unsupported.
 */
static uint8_t
reg_jentry_walk(TSK_FS_INFO * fs, int flags,
    TSK_FS_JENTRY_WALK_CB a_action, void *ptr)
{
    reg_journal_unsupported();
    return 1;
}

/**
 * @brief ntfs_jopen
 * @param fs
 * @param inum
 * @return 1, as this is unsupported.
 */
static uint8_t
reg_jopen(TSK_FS_INFO * fs, TSK_INUM_T inum)
{
    reg_journal_unsupported();
    return 1;
}




















/**
 * reg_load_regf
 *   Read data into the supplied REGF, and do some sanity checking.
 */
TSK_RETVAL_ENUM
reg_load_regf(TSK_FS_INFO *fs_info, REGF *regf) {
    ssize_t count;

    count = tsk_fs_read(fs_info, 0, (char *)regf, sizeof(REGF));
    if (count != sizeof(REGF)) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_READ);
        tsk_error_set_errstr("Failed to read REGF header structure");
        return TSK_ERR;
    }

    if ((tsk_getu32(fs_info->endian, regf->magic)) != REG_REGF_MAGIC) {
        tsk_error_set_errno(TSK_ERR_FS_INODE_COR);
        tsk_error_set_errstr("REGF header has an invalid magic header");
        return TSK_ERR;
    }

    return TSK_OK;
}



/**
 * \internal
 * Open part of a disk image as a Windows Registry.
 *
 * @param img_info Disk image to analyze
 * @param offset Byte offset where file system starts
 * @param ftype Specific type of file system
 * @param test NOT USED * @returns NULL on error or if data is not a Registry
 */
TSK_FS_INFO *
regfs_open(TSK_IMG_INFO * img_info, TSK_OFF_T offset,
    TSK_FS_TYPE_ENUM ftype, uint8_t test)
{
    TSK_FS_INFO *fs;
    REGFS_INFO *reg;

    tsk_error_reset();

    if (TSK_FS_TYPE_ISREG(ftype) == 0) {
        tsk_error_reset();
        tsk_error_set_errno(TSK_ERR_FS_ARG);
        tsk_error_set_errstr("Invalid FS type in reg_open");
        return NULL;
    }

    if ((reg = (REGFS_INFO *) tsk_fs_malloc(sizeof(REGFS_INFO))) == NULL) {
        return NULL;
    }
    fs = &(reg->fs_info);

    fs->ftype = TSK_FS_TYPE_REG;
    fs->duname = "Cell";
    fs->flags = TSK_FS_INFO_FLAG_NONE;
    fs->tag = TSK_FS_INFO_TAG;
    fs->endian = TSK_LIT_ENDIAN;

    fs->img_info = img_info;
    fs->offset = offset;

    if (reg_load_regf(fs, &(reg->regf)) != TSK_OK) {
        free(reg);
        return NULL;
    }


    fs->first_inum = FIRST_HBIN_OFFSET;
    fs->last_inum  = (tsk_getu32(fs->endian, reg->regf.last_hbin_offset)) + HBIN_SIZE;
    // TODO(wb): set root inode
    // TODO(wb): set num inodes
    fs->first_block = FIRST_HBIN_OFFSET + 0x20;
    fs->last_block = (tsk_getu32(fs->endian, reg->regf.last_hbin_offset)) + HBIN_SIZE;
    fs->last_block_act = img_info->size - HBIN_SIZE;

    fs->inode_walk = reg_inode_walk;
    fs->block_walk = reg_block_walk;
    fs->block_getflags = reg_block_getflags;

    fs->get_default_attr_type = reg_get_default_attr_type;
    fs->load_attrs = reg_load_attrs;

    fs->file_add_meta = reg_inode_lookup;
    fs->dir_open_meta = reg_dir_open_meta;
    fs->fsstat = reg_fsstat;
    fs->fscheck = reg_fscheck;
    fs->istat = reg_istat;
    fs->close = reg_close;
    fs->name_cmp = reg_name_cmp;

    fs->fread_owner_sid = reg_file_get_sidstr;
    fs->jblk_walk = reg_jblk_walk;
    fs->jentry_walk = reg_jentry_walk;
    fs->jopen = reg_jopen;
    fs->journ_inum = 0;


    return (fs);
}