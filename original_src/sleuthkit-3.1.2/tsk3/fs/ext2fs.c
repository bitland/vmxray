/*
** The Sleuth Kit 
**
** Brian Carrier [carrier <at> sleuthkit [dot] org]
** Copyright (c) 2006-2008 Brian Carrier, Basis Technology.  All Rights reserved
** Copyright (c) 2003-2005 Brian Carrier.  All rights reserved 
**
** TASK
** Copyright (c) 2002-2003 Brian Carrier, @stake Inc.  All rights reserved
** 
** Copyright (c) 1997,1998,1999, International Business Machines          
** Corporation and others. All Rights Reserved.
*/

/**
 *\file ext2fs.c
 * Contains the internal TSK ext2/ext3 file system functions.
 */

/* TCT 
 * LICENSE
 *	This software is distributed under the IBM Public License.
 * AUTHOR(S)
 *	Wietse Venema
 *	IBM T.J. Watson Research
 *	P.O. Box 704
 *	Yorktown Heights, NY 10598, USA
 --*/

#include "tsk_fs_i.h"
#include "tsk_ext2fs.h"


/* ext2fs_group_load - load block group descriptor into cache 
 *
 * return 1 on error and 0 on success
 *
 * */
static uint8_t
ext2fs_group_load(EXT2FS_INFO * ext2fs, EXT2_GRPNUM_T grp_num)
{
    ext2fs_gd *gd;
    TSK_OFF_T offs;
    ssize_t cnt;
    TSK_FS_INFO *fs = (TSK_FS_INFO *) ext2fs;

    /*
     * Sanity check
     */
    if (grp_num < 0 || grp_num >= ext2fs->groups_count) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ext2fs_group_load: invalid cylinder group number: %"
            PRI_EXT2GRP "", grp_num);
        return 1;
    }


    if (ext2fs->grp_buf == NULL) {
        if ((ext2fs->grp_buf =
                (ext2fs_gd *) tsk_malloc(sizeof(ext2fs_gd))) == NULL) {
            return 1;
        }
    }
    else if (ext2fs->grp_num == grp_num) {
        return 0;
    }

    gd = ext2fs->grp_buf;

    /*
     * We're not reading group descriptors often, so it is OK to do small
     * reads instead of cacheing group descriptors in a large buffer.
     */
    offs = ext2fs->groups_offset + grp_num * sizeof(ext2fs_gd);
    cnt = tsk_fs_read(&ext2fs->fs_info, offs, (char *) gd,
        sizeof(ext2fs_gd));
    if (cnt != sizeof(ext2fs_gd)) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_READ;
        }
        snprintf(tsk_errstr2, TSK_ERRSTR_L,
            "ext2fs_group_load: Group descriptor %" PRI_EXT2GRP " at %"
            PRIuOFF, grp_num, offs);
        return 1;
    }


    /* Perform a sanity check on the data to make sure offsets are in range */
    if ((tsk_getu32(fs->endian,
                gd->bg_block_bitmap) > fs->last_block) ||
        (tsk_getu32(fs->endian,
                gd->bg_inode_bitmap) > fs->last_block) ||
        (tsk_getu32(fs->endian, gd->bg_inode_table) > fs->last_block)) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_CORRUPT;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "extXfs_group_load: Group %" PRI_EXT2GRP
            " descriptor block locations too large at byte offset %"
            PRIuDADDR, grp_num, offs);
        return 1;
    }


    ext2fs->grp_num = grp_num;

    if (tsk_verbose) {
        TSK_FS_INFO *fs = (TSK_FS_INFO *) & ext2fs->fs_info;
        tsk_fprintf(stderr,
            "\tgroup %" PRI_EXT2GRP ": %" PRIu16 "/%" PRIu16
            " free blocks/inodes\n", grp_num, tsk_getu16(fs->endian,
                gd->bg_free_blocks_count),
            tsk_getu16(fs->endian, gd->bg_free_inodes_count));
    }

    return 0;
}

/* ext2fs_print_map - print a bitmap */

static void
ext2fs_print_map(uint8_t * map, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        if (i > 0 && i % 10 == 0)
            putc('|', stderr);
        putc(isset(map, i) ? '1' : '.', stderr);
    }
    putc('\n', stderr);
}


/* ext2fs_bmap_load - look up block bitmap & load into cache 
 *
 * return 1 on error and 0 on success
 * */
static uint8_t
ext2fs_bmap_load(EXT2FS_INFO * ext2fs, EXT2_GRPNUM_T grp_num)
{
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & ext2fs->fs_info;
    ssize_t cnt;

    /*
     * Look up the group descriptor info.  The load will do the sanity check.
     */
    if ((ext2fs->grp_buf == NULL) || (ext2fs->grp_num != grp_num)) {
        if (ext2fs_group_load(ext2fs, grp_num)) {
            return 1;
        }
    }

    if (ext2fs->bmap_buf == NULL) {
        if ((ext2fs->bmap_buf =
                (uint8_t *) tsk_malloc(fs->block_size)) == NULL) {
            return 1;
        }
    }
    else if (ext2fs->bmap_grp_num == grp_num)
        return 0;

    /*
     * Look up the block allocation bitmap.
     */
    if (tsk_getu32(fs->endian,
            ext2fs->grp_buf->bg_block_bitmap) > fs->last_block) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_BLK_NUM;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ext2fs_bmap_load: Block too large for image: %" PRIu32
            "", tsk_getu32(fs->endian, ext2fs->grp_buf->bg_block_bitmap));
        return 1;
    }

    cnt = tsk_fs_read(fs, (TSK_DADDR_T) tsk_getu32(fs->endian,
            ext2fs->grp_buf->bg_block_bitmap) * fs->block_size,
        (char *) ext2fs->bmap_buf, ext2fs->fs_info.block_size);

    if (cnt != ext2fs->fs_info.block_size) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_READ;
        }
        snprintf(tsk_errstr2, TSK_ERRSTR_L,
            "ext2fs_bmap_load: Bitmap group %" PRI_EXT2GRP " at %"
            PRIu32, grp_num, tsk_getu32(fs->endian,
                ext2fs->grp_buf->bg_block_bitmap));
    }

    ext2fs->bmap_grp_num = grp_num;

    if (tsk_verbose > 1)
        ext2fs_print_map(ext2fs->bmap_buf,
            tsk_getu32(fs->endian, ext2fs->fs->s_blocks_per_group));

    return 0;
}


/* ext2fs_imap_load - look up inode bitmap & load into cache 
 *
 * return 0 on success and 1 on error
 * */
static uint8_t
ext2fs_imap_load(EXT2FS_INFO * ext2fs, EXT2_GRPNUM_T grp_num)
{
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & ext2fs->fs_info;
    ssize_t cnt;

    /*
     * Look up the group descriptor info.
     */
    if ((ext2fs->grp_buf == NULL) || (ext2fs->grp_num != grp_num)) {
        if (ext2fs_group_load(ext2fs, grp_num)) {
            return 1;
        }
    }

    /* Allocate the cache buffer and exit if map is already loaded */
    if (ext2fs->imap_buf == NULL) {
        if ((ext2fs->imap_buf =
                (uint8_t *) tsk_malloc(fs->block_size)) == NULL) {
            return 1;
        }
    }
    else if (ext2fs->imap_grp_num == grp_num) {
        return 0;
    }

    /*
     * Look up the inode allocation bitmap.
     */
    if (tsk_getu32(fs->endian,
            ext2fs->grp_buf->bg_inode_bitmap) > fs->last_block) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_BLK_NUM;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ext2fs_imap_load: Block too large for image: %" PRIu32
            "", tsk_getu32(fs->endian, ext2fs->grp_buf->bg_inode_bitmap));
    }

    cnt = tsk_fs_read(fs,
        (TSK_DADDR_T) tsk_getu32(fs->endian,
            ext2fs->grp_buf->bg_inode_bitmap) * fs->block_size,
        (char *) ext2fs->imap_buf, ext2fs->fs_info.block_size);

    if (cnt != ext2fs->fs_info.block_size) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_READ;
        }
        snprintf(tsk_errstr2, TSK_ERRSTR_L,
            "ext2fs_imap_load: Inode bitmap %" PRI_EXT2GRP " at %"
            PRIu32, grp_num, tsk_getu32(fs->endian,
                ext2fs->grp_buf->bg_inode_bitmap));
    }

    ext2fs->imap_grp_num = grp_num;
    if (tsk_verbose > 1)
        ext2fs_print_map(ext2fs->imap_buf,
            tsk_getu32(fs->endian, ext2fs->fs->s_inodes_per_group));

    return 0;
}

/* ext2fs_dinode_load - look up disk inode & load into cache 
 *
 * return 1 on error and 0 on success
 * */

static uint8_t
ext2fs_dinode_load(EXT2FS_INFO * ext2fs, TSK_INUM_T inum)
{
    ext2fs_inode *dino;
    EXT2_GRPNUM_T grp_num;
    TSK_OFF_T addr;
    ssize_t cnt;
    TSK_INUM_T rel_inum;
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & ext2fs->fs_info;

    /*
     * Sanity check.
     * Use last_num-1 to account for virtual Orphan directory in last_inum.
     */
    if ((inum < fs->first_inum) || (inum > fs->last_inum - 1)) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_INODE_NUM;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ext2fs_dinode_load: address: %" PRIuINUM, inum);
        return 1;
    }

    /* Allocate the buffer or return if already loaded */
    if (ext2fs->dino_buf == NULL) {
        if ((ext2fs->dino_buf =
                (ext2fs_inode *) tsk_malloc(ext2fs->inode_size)) == NULL) {
            return 1;
        }
    }
    else if (ext2fs->dino_inum == inum) {
        return 0;
    }

    dino = ext2fs->dino_buf;

    /*
     * Look up the group descriptor for this inode. 
     */
    grp_num = (EXT2_GRPNUM_T) ((inum - fs->first_inum) /
        tsk_getu32(fs->endian, ext2fs->fs->s_inodes_per_group));

    if ((ext2fs->grp_buf == NULL) || (ext2fs->grp_num != grp_num)) {
        if (ext2fs_group_load(ext2fs, grp_num)) {
            return 1;
        }
    }

    /*
     * Look up the inode table block for this inode.
     */
    rel_inum =
        (inum - 1) - tsk_getu32(fs->endian,
        ext2fs->fs->s_inodes_per_group) * grp_num;
    addr =
        (TSK_OFF_T) tsk_getu32(fs->endian,
        ext2fs->grp_buf->bg_inode_table) * (TSK_OFF_T) fs->block_size +
        rel_inum * (TSK_OFF_T) ext2fs->inode_size;

    cnt = tsk_fs_read(fs, addr, (char *) dino, ext2fs->inode_size);
    if (cnt != ext2fs->inode_size) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_READ;
        }
        snprintf(tsk_errstr2, TSK_ERRSTR_L,
            "ext2fs_dinode_load: Inode %" PRIuINUM
            " from %" PRIuOFF, inum, addr);
        return 1;
    }

    ext2fs->dino_inum = inum;
    if (tsk_verbose)
        tsk_fprintf(stderr,
            "%" PRIuINUM " m/l/s=%o/%d/%" PRIuOFF
            " u/g=%d/%d macd=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32
            "\n", inum, tsk_getu16(fs->endian, dino->i_mode),
            tsk_getu16(fs->endian, dino->i_nlink), (tsk_getu32(fs->endian,
                    dino->i_size) + (tsk_getu16(fs->endian,
                        dino->i_mode) & EXT2_IN_REG) ? (uint64_t)
                tsk_getu32(fs->endian, dino->i_size_high) << 32 : 0),
            tsk_getu16(fs->endian, dino->i_uid) + (tsk_getu16(fs->endian,
                    dino->i_uid_high) << 16), tsk_getu16(fs->endian,
                dino->i_gid) + (tsk_getu16(fs->endian,
                    dino->i_gid_high) << 16), tsk_getu32(fs->endian,
                dino->i_mtime), tsk_getu32(fs->endian, dino->i_atime),
            tsk_getu32(fs->endian, dino->i_ctime), tsk_getu32(fs->endian,
                dino->i_dtime));

    return 0;
}

/* ext2fs_dinode_copy - copy cached disk inode into generic inode 
 *
 * returns 1 on error and 0 on success
 * */
static uint8_t
ext2fs_dinode_copy(EXT2FS_INFO * ext2fs, TSK_FS_META * fs_meta)
{
    int i;
    ext2fs_inode *in = ext2fs->dino_buf;
    TSK_FS_INFO *fs = (TSK_FS_INFO *) & ext2fs->fs_info;
    ext2fs_sb *sb = ext2fs->fs;
    EXT2_GRPNUM_T grp_num;
    TSK_INUM_T ibase = 0;
    TSK_DADDR_T *addr_ptr;

    if (ext2fs->dino_buf == NULL) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ext2fs_dinode_copy: dino_buf is NULL");
        return 1;
    }

    fs_meta->attr_state = TSK_FS_META_ATTR_EMPTY;
    if (fs_meta->attr) {
        tsk_fs_attrlist_markunused(fs_meta->attr);
    }

    // set the type
    switch (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_FMT) {
    case EXT2_IN_REG:
        fs_meta->type = TSK_FS_META_TYPE_REG;
        break;
    case EXT2_IN_DIR:
        fs_meta->type = TSK_FS_META_TYPE_DIR;
        break;
    case EXT2_IN_SOCK:
        fs_meta->type = TSK_FS_META_TYPE_SOCK;
        break;
    case EXT2_IN_LNK:
        fs_meta->type = TSK_FS_META_TYPE_LNK;
        break;
    case EXT2_IN_BLK:
        fs_meta->type = TSK_FS_META_TYPE_BLK;
        break;
    case EXT2_IN_CHR:
        fs_meta->type = TSK_FS_META_TYPE_CHR;
        break;
    case EXT2_IN_FIFO:
        fs_meta->type = TSK_FS_META_TYPE_FIFO;
        break;
    default:
        fs_meta->type = TSK_FS_META_TYPE_UNDEF;
        break;
    }

    // set the mode
    fs_meta->mode = 0;
    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_ISUID)
        fs_meta->mode |= TSK_FS_META_MODE_ISUID;
    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_ISGID)
        fs_meta->mode |= TSK_FS_META_MODE_ISGID;
    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_ISVTX)
        fs_meta->mode |= TSK_FS_META_MODE_ISVTX;

    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_IRUSR)
        fs_meta->mode |= TSK_FS_META_MODE_IRUSR;
    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_IWUSR)
        fs_meta->mode |= TSK_FS_META_MODE_IWUSR;
    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_IXUSR)
        fs_meta->mode |= TSK_FS_META_MODE_IXUSR;

    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_IRGRP)
        fs_meta->mode |= TSK_FS_META_MODE_IRGRP;
    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_IWGRP)
        fs_meta->mode |= TSK_FS_META_MODE_IWGRP;
    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_IXGRP)
        fs_meta->mode |= TSK_FS_META_MODE_IXGRP;

    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_IROTH)
        fs_meta->mode |= TSK_FS_META_MODE_IROTH;
    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_IWOTH)
        fs_meta->mode |= TSK_FS_META_MODE_IWOTH;
    if (tsk_getu16(fs->endian, in->i_mode) & EXT2_IN_IXOTH)
        fs_meta->mode |= TSK_FS_META_MODE_IXOTH;

    fs_meta->nlink = tsk_getu16(fs->endian, in->i_nlink);

    fs_meta->size = tsk_getu32(fs->endian, in->i_size);

    fs_meta->addr = ext2fs->dino_inum;

    /* the general size value in the inode is only 32-bits,
     * but the i_dir_acl value is used for regular files to 
     * hold the upper 32-bits 
     *
     * The RO_COMPAT_LARGE_FILE flag in the super block will identify
     * if there are any large files in the file system
     */
    if ((fs_meta->type == TSK_FS_META_TYPE_REG) &&
        (tsk_getu32(fs->endian, sb->s_feature_ro_compat) &
            EXT2FS_FEATURE_RO_COMPAT_LARGE_FILE)) {
        fs_meta->size +=
            ((uint64_t) tsk_getu32(fs->endian, in->i_size_high) << 32);
    }

    fs_meta->uid =
        tsk_getu16(fs->endian, in->i_uid) + (tsk_getu16(fs->endian,
            in->i_uid_high) << 16);
    fs_meta->gid =
        tsk_getu16(fs->endian, in->i_gid) + (tsk_getu16(fs->endian,
            in->i_gid_high) << 16);
    fs_meta->mtime = tsk_getu32(fs->endian, in->i_mtime);
    fs_meta->atime = tsk_getu32(fs->endian, in->i_atime);
    fs_meta->ctime = tsk_getu32(fs->endian, in->i_ctime);
    fs_meta->crtime = 0;
    fs_meta->time2.ext2.dtime = tsk_getu32(fs->endian, in->i_dtime);
    fs_meta->mtime_nano = fs_meta->atime_nano = fs_meta->ctime_nano =
        fs_meta->crtime_nano = 0;
    fs_meta->time2.ext2.dtime_nano = 0;
    fs_meta->seq = 0;

    if (fs_meta->link) {
        free(fs_meta->link);
        fs_meta->link = NULL;
    }

    if (fs_meta->content_len != EXT2FS_FILE_CONTENT_LEN) {
        if ((fs_meta =
                tsk_fs_meta_realloc(fs_meta,
                    EXT2FS_FILE_CONTENT_LEN)) == NULL) {
            return 1;
        }
    }

    addr_ptr = (TSK_DADDR_T *) fs_meta->content_ptr;
    for (i = 0; i < EXT2FS_NDADDR + EXT2FS_NIADDR; i++)
        addr_ptr[i] = tsk_gets32(fs->endian, in->i_block[i]);

    /* set the link string 
     * the size check prevents us from trying to allocate a huge amount of
     * memory for a bad inode value
     */
    if ((fs_meta->type == TSK_FS_META_TYPE_LNK)
        && (fs_meta->size < EXT2FS_MAXPATHLEN) && (fs_meta->size >= 0)) {
        unsigned int count = 0;
        int i;

        if ((fs_meta->link =
                tsk_malloc((size_t) (fs_meta->size + 1))) == NULL)
            return 1;

        /* it is located directly in the pointers */
        if (fs_meta->size < 4 * (EXT2FS_NDADDR + EXT2FS_NIADDR)) {
            unsigned int j;

            for (i = 0; i < (EXT2FS_NDADDR + EXT2FS_NIADDR) &&
                count < fs_meta->size; i++) {
                char *a_ptr = (char *) &in->i_block[i];
                for (j = 0; j < 4 && count < fs_meta->size; j++) {
                    fs_meta->link[count++] = a_ptr[j];
                }
            }
            fs_meta->link[count] = '\0';

            /* clear the content pointer data to avoid the prog from reading them */
            memset(fs_meta->content_ptr, 0, fs_meta->content_len);
        }

        /* it is in blocks */
        else {
            TSK_FS_INFO *fs = (TSK_FS_INFO *) & ext2fs->fs_info;
            char *data_buf;
            char *a_ptr = fs_meta->link;
            TSK_DADDR_T *addr_ptr = fs_meta->content_ptr;;

            if ((data_buf = tsk_malloc(fs->block_size)) == NULL)
                return 1;

            /* we only need to do the direct blocks due to the limit 
             * on path length */
            for (i = 0; i < EXT2FS_NDADDR && count < fs_meta->size; i++) {
                ssize_t cnt;

                int read_count =
                    (fs_meta->size - count <
                    fs->block_size) ? (int) (fs_meta->size -
                    count) : (int) (fs->block_size);

                cnt = tsk_fs_read_block(fs,
                    addr_ptr[i], data_buf, fs->block_size);

                if (cnt != fs->block_size) {
                    if (cnt >= 0) {
                        tsk_error_reset();
                        tsk_errno = TSK_ERR_FS_READ;
                    }
                    snprintf(tsk_errstr2, TSK_ERRSTR_L,
                        "ext2fs_dinode_copy: symlink destination from %"
                        PRIuDADDR, addr_ptr[i]);
                    free(data_buf);
                    return 1;
                }

                memcpy(a_ptr, data_buf, read_count);
                count += read_count;
                a_ptr = (char *) ((uintptr_t) a_ptr + count);
            }

            /* terminate the string */
            *a_ptr = '\0';
            free(data_buf);
        }

        /* Clean up name */
        i = 0;
        while (fs_meta->link[i] != '\0') {
            if (TSK_IS_CNTRL(fs_meta->link[i]))
                fs_meta->link[i] = '^';
            i++;
        }
    }

    /* Fill in the flags value */
    grp_num = (EXT2_GRPNUM_T) ((ext2fs->dino_inum - fs->first_inum) /
        tsk_getu32(fs->endian, ext2fs->fs->s_inodes_per_group));

    if (ext2fs->imap_grp_num != grp_num) {
        if (ext2fs_imap_load(ext2fs, grp_num)) {
            return 1;
        }
    }

    ibase =
        grp_num * tsk_getu32(fs->endian,
        ext2fs->fs->s_inodes_per_group) + fs->first_inum;

    /*
     * Apply the allocated/unallocated restriction.
     */
    fs_meta->flags = (isset(ext2fs->imap_buf, ext2fs->dino_inum - ibase) ?
        TSK_FS_META_FLAG_ALLOC : TSK_FS_META_FLAG_UNALLOC);

    /*
     * Apply the used/unused restriction.
     */
    fs_meta->flags |= (fs_meta->ctime ?
        TSK_FS_META_FLAG_USED : TSK_FS_META_FLAG_UNUSED);

    return 0;
}



/* ext2fs_inode_lookup - lookup inode, external interface 
 *
 * Returns 1 on error and 0 on success
 * 
 */

static uint8_t
ext2fs_inode_lookup(TSK_FS_INFO * fs, TSK_FS_FILE * a_fs_file,
    TSK_INUM_T inum)
{
    EXT2FS_INFO *ext2fs = (EXT2FS_INFO *) fs;

    if (a_fs_file == NULL) {
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "ext2fs_inode_lookup: fs_file is NULL");
        return 1;
    }

    if (a_fs_file->meta == NULL) {
        if ((a_fs_file->meta =
                tsk_fs_meta_alloc(EXT2FS_FILE_CONTENT_LEN)) == NULL)
            return 1;
    }
    else {
        tsk_fs_meta_reset(a_fs_file->meta);
    }

    // see if they are looking for the special "orphans" directory
    if (inum == TSK_FS_ORPHANDIR_INUM(fs)) {
        if (tsk_fs_dir_make_orphan_dir_meta(fs, a_fs_file->meta))
            return 1;
        else
            return 0;
    }

    if (ext2fs_dinode_load(ext2fs, inum)) {
        return 1;
    }

    if (ext2fs_dinode_copy(ext2fs, a_fs_file->meta)) {
        return 1;
    }

    return 0;
}



/* ext2fs_inode_walk - inode iterator 
 *
 * flags used: TSK_FS_META_FLAG_USED, TSK_FS_META_FLAG_UNUSED,
 *  TSK_FS_META_FLAG_ALLOC, TSK_FS_META_FLAG_UNALLOC, TSK_FS_META_FLAG_ORPHAN
 *
 *  Return 1 on error and 0 on success
*/

uint8_t
ext2fs_inode_walk(TSK_FS_INFO * fs, TSK_INUM_T start_inum,
    TSK_INUM_T end_inum, TSK_FS_META_FLAG_ENUM flags,
    TSK_FS_META_WALK_CB a_action, void *a_ptr)
{
    char *myname = "extXfs_inode_walk";
    EXT2FS_INFO *ext2fs = (EXT2FS_INFO *) fs;
    EXT2_GRPNUM_T grp_num;
    TSK_INUM_T inum;
    TSK_INUM_T end_inum_tmp;
    TSK_INUM_T ibase = 0;
    TSK_FS_FILE *fs_file;
    int myflags;

    // clean up any error messages that are lying around
    tsk_error_reset();

    /*
     * Sanity checks.
     */
    if (start_inum < fs->first_inum || start_inum > fs->last_inum) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "%s: start inode: %" PRIuINUM "", myname, start_inum);
        return 1;
    }

    if (end_inum < fs->first_inum || end_inum > fs->last_inum
        || end_inum < start_inum) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "%s: end inode: %" PRIuINUM "", myname, end_inum);
        return 1;
    }

    /* If ORPHAN is wanted, then make sure that the flags are correct */
    if (flags & TSK_FS_META_FLAG_ORPHAN) {
        flags |= TSK_FS_META_FLAG_UNALLOC;
        flags &= ~TSK_FS_META_FLAG_ALLOC;
        flags |= TSK_FS_META_FLAG_USED;
        flags &= ~TSK_FS_META_FLAG_UNUSED;
    }
    else {
        if (((flags & TSK_FS_META_FLAG_ALLOC) == 0) &&
            ((flags & TSK_FS_META_FLAG_UNALLOC) == 0)) {
            flags |= (TSK_FS_META_FLAG_ALLOC | TSK_FS_META_FLAG_UNALLOC);
        }

        /* If neither of the USED or UNUSED flags are set, then set them
         * both
         */
        if (((flags & TSK_FS_META_FLAG_USED) == 0) &&
            ((flags & TSK_FS_META_FLAG_UNUSED) == 0)) {
            flags |= (TSK_FS_META_FLAG_USED | TSK_FS_META_FLAG_UNUSED);
        }
    }



    /* If we are looking for orphan files and have not yet filled
     * in the list of unalloc inodes that are pointed to, then fill
     * in the list
     */
    if ((flags & TSK_FS_META_FLAG_ORPHAN)
        && (fs->list_inum_named == NULL)) {

        if (tsk_fs_dir_load_inum_named(fs) != TSK_OK) {
            strncat(tsk_errstr2,
                " - ext2fs_inode_walk: identifying inodes allocated by file names",
                TSK_ERRSTR_L);
            return 1;
        }

    }

    if ((fs_file = tsk_fs_file_alloc(fs)) == NULL)
        return 1;
    if ((fs_file->meta =
            tsk_fs_meta_alloc(EXT2FS_FILE_CONTENT_LEN)) == NULL)
        return 1;

    // we need to handle fs->last_inum specially because it is for the 
    // virtual ORPHANS directory.  Handle it outside of the loop.
    if (end_inum == TSK_FS_ORPHANDIR_INUM(fs))
        end_inum_tmp = end_inum - 1;
    else
        end_inum_tmp = end_inum;

    /*
     * Iterate.
     */
    for (inum = start_inum; inum <= end_inum_tmp; inum++) {
        int retval;

        /*
         * Be sure to use the proper group descriptor data. XXX Linux inodes
         * start at 1, as in Fortran.
         */
        grp_num =
            (EXT2_GRPNUM_T) ((inum - 1) / tsk_getu32(fs->endian,
                ext2fs->fs->s_inodes_per_group));
        if ((ext2fs->imap_buf == NULL)
            || (ext2fs->imap_grp_num != grp_num)) {
            if (ext2fs_imap_load(ext2fs, grp_num)) {
                return 1;
            }
            ibase =
                grp_num * tsk_getu32(fs->endian,
                ext2fs->fs->s_inodes_per_group) + 1;
        }

        /* In case we didn't need to load it the bitmap */
        else if (inum == start_inum) {
            ibase =
                grp_num * tsk_getu32(fs->endian,
                ext2fs->fs->s_inodes_per_group) + 1;
        }

        /*
         * Apply the allocated/unallocated restriction.
         */
        myflags = (isset(ext2fs->imap_buf, inum - ibase) ?
            TSK_FS_META_FLAG_ALLOC : TSK_FS_META_FLAG_UNALLOC);
        if ((flags & myflags) != myflags)
            continue;

        if (ext2fs_dinode_load(ext2fs, inum)) {
            tsk_fs_file_close(fs_file);
            return 1;
        }


        /*
         * Apply the used/unused restriction.
         */
        myflags |= (tsk_getu32(fs->endian, ext2fs->dino_buf->i_ctime) ?
            TSK_FS_META_FLAG_USED : TSK_FS_META_FLAG_UNUSED);

        if ((flags & myflags) != myflags)
            continue;

        /* If we want only orphans, then check if this
         * inode is in the seen list
         */
        if ((myflags & TSK_FS_META_FLAG_UNALLOC) &&
            (flags & TSK_FS_META_FLAG_ORPHAN) &&
            (tsk_list_find(fs->list_inum_named, inum))) {
            continue;
        }


        /*
         * Fill in a file system-independent inode structure and pass control
         * to the application.
         */
        if (ext2fs_dinode_copy(ext2fs, fs_file->meta)) {
            tsk_fs_meta_close(fs_file->meta);
            return 1;
        }

        retval = a_action(fs_file, a_ptr);
        if (retval == TSK_WALK_STOP) {
            tsk_fs_file_close(fs_file);
            return 0;
        }
        else if (retval == TSK_WALK_ERROR) {
            tsk_fs_file_close(fs_file);
            return 1;
        }
    }

    // handle the virtual orphans folder if they asked for it
    if ((end_inum == TSK_FS_ORPHANDIR_INUM(fs))
        && (flags & TSK_FS_META_FLAG_ALLOC)
        && (flags & TSK_FS_META_FLAG_USED)) {
        int retval;

        if (tsk_fs_dir_make_orphan_dir_meta(fs, fs_file->meta)) {
            tsk_fs_file_close(fs_file);
            return 1;
        }
        /* call action */
        retval = a_action(fs_file, a_ptr);
        if (retval == TSK_WALK_STOP) {
            tsk_fs_file_close(fs_file);
            return 0;
        }
        else if (retval == TSK_WALK_ERROR) {
            tsk_fs_file_close(fs_file);
            return 1;
        }
    }

    /*
     * Cleanup.
     */
    tsk_fs_file_close(fs_file);
    return 0;
}



TSK_FS_BLOCK_FLAG_ENUM
ext2fs_block_getflags(TSK_FS_INFO * a_fs, TSK_DADDR_T a_addr)
{
    EXT2FS_INFO *ext2fs = (EXT2FS_INFO *) a_fs;
    int flags;
    EXT2_GRPNUM_T grp_num;
    TSK_DADDR_T dbase = 0;      /* first block number in group */
    TSK_DADDR_T dmin = 0;       /* first block after inodes */

    // these blocks are not described in the group descriptors
    // sparse
    if (a_addr == 0)
        return TSK_FS_BLOCK_FLAG_CONT | TSK_FS_BLOCK_FLAG_ALLOC;
    if (a_addr < ext2fs->first_data_block)
        return TSK_FS_BLOCK_FLAG_META | TSK_FS_BLOCK_FLAG_ALLOC;

    grp_num = ext2_dtog_lcl(a_fs, ext2fs->fs, a_addr);

    /* Lookup bitmap if not loaded */
    if ((ext2fs->bmap_buf == NULL)
        || (ext2fs->bmap_grp_num != grp_num)) {

        if (ext2fs_bmap_load(ext2fs, grp_num)) {
            return 0;
        }

        if (tsk_verbose)
            tsk_fprintf(stderr,
                "ext2_block_walk: loading group %" PRI_EXT2GRP
                " dbase %" PRIuDADDR " bmap +%" PRIuDADDR
                " imap +%" PRIuDADDR " inos +%" PRIuDADDR "..%"
                PRIuDADDR "\n", grp_num, dbase,
                (TSK_DADDR_T) tsk_getu32(a_fs->endian,
                    ext2fs->grp_buf->bg_block_bitmap)
                - dbase, (TSK_DADDR_T) tsk_getu32(a_fs->endian,
                    ext2fs->grp_buf->bg_inode_bitmap) - dbase,
                (TSK_DADDR_T) tsk_getu32(a_fs->endian,
                    ext2fs->grp_buf->bg_inode_table) - dbase,
                dmin - 1 - dbase);
    }

    /*
     * Be sure to use the right group descriptor information. XXX There
     * appears to be an off-by-one discrepancy between bitmap offsets and
     * disk block numbers.
     *
     * Addendum: this offset is controlled by the super block's
     * s_first_data_block field.
     */
#define INODE_TABLE_SIZE(ext2fs) \
    ((tsk_getu32(a_fs->endian, ext2fs->fs->s_inodes_per_group) * ext2fs->inode_size - 1) \
           / a_fs->block_size + 1)

    dbase = ext2_cgbase_lcl(a_fs, ext2fs->fs, grp_num);
    dmin =
        tsk_getu32(a_fs->endian,
        ext2fs->grp_buf->bg_inode_table) + INODE_TABLE_SIZE(ext2fs);

    /*
     *  Identify meta blocks
     * (any blocks that can't be allocated for file/directory data).
     *
     * XXX With sparse superblock placement, most block groups have the
     * block and inode bitmaps where one would otherwise find the backup
     * superblock and the backup group descriptor blocks. The inode
     * blocks are in the normal place, though. This leaves little gaps
     * between the bitmaps and the inode table - and ext2fs will use
     * those blocks for file/directory data blocks. So we must properly
     * account for those gaps between meta blocks.
     *
     * Thus, superblocks and group descriptor blocks are sometimes overlaid
     * by bitmap blocks. This means that one can still assume that the
     * locations of superblocks and group descriptor blocks are reserved.
     * They just happen to be reserved for something else :-)
     */
    flags = (isset(ext2fs->bmap_buf, a_addr - dbase) ?
        TSK_FS_BLOCK_FLAG_ALLOC : TSK_FS_BLOCK_FLAG_UNALLOC);

    if ((a_addr >= dbase
            && a_addr < tsk_getu32(a_fs->endian,
                ext2fs->grp_buf->bg_block_bitmap))
        || (a_addr == tsk_getu32(a_fs->endian,
                ext2fs->grp_buf->bg_block_bitmap))
        || (a_addr == tsk_getu32(a_fs->endian,
                ext2fs->grp_buf->bg_inode_bitmap))
        || (a_addr >= tsk_getu32(a_fs->endian,
                ext2fs->grp_buf->bg_inode_table)
            && a_addr < dmin))
        flags |= TSK_FS_BLOCK_FLAG_META;
    else
        flags |= TSK_FS_BLOCK_FLAG_CONT;

    return flags;
}


/* ext2fs_block_walk - block iterator 
 *
 * flags: TSK_FS_BLOCK_FLAG_ALLOC, TSK_FS_BLOCK_FLAG_UNALLOC, TSK_FS_BLOCK_FLAG_CONT,
 *  TSK_FS_BLOCK_FLAG_META
 *
 *  Return 1 on error and 0 on success
*/

uint8_t
ext2fs_block_walk(TSK_FS_INFO * a_fs, TSK_DADDR_T a_start_blk,
    TSK_DADDR_T a_end_blk, TSK_FS_BLOCK_WALK_FLAG_ENUM a_flags,
    TSK_FS_BLOCK_WALK_CB a_action, void *a_ptr)
{
    char *myname = "extXfs_block_walk";
    TSK_FS_BLOCK *fs_block;
    TSK_DADDR_T addr;

    // clean up any error messages that are lying around
    tsk_error_reset();

    /*
     * Sanity checks.
     */
    if (a_start_blk < a_fs->first_block || a_start_blk > a_fs->last_block) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "%s: start block: %" PRIuDADDR, myname, a_start_blk);
        return 1;
    }
    if (a_end_blk < a_fs->first_block || a_end_blk > a_fs->last_block
        || a_end_blk < a_start_blk) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_WALK_RNG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "%s: end block: %" PRIuDADDR, myname, a_end_blk);
        return 1;
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


    if ((fs_block = tsk_fs_block_alloc(a_fs)) == NULL) {
        return 1;
    }

    /*
     * Iterate. This is not as tricky as it could be, because the free list
     * map covers the entire disk partition, including blocks occupied by
     * group descriptor blocks, bit maps, and other non-data blocks.
     */
    for (addr = a_start_blk; addr <= a_end_blk; addr++) {
        int retval;
        int myflags;

        myflags = ext2fs_block_getflags(a_fs, addr);

        // test if we should call the callback with this one
        if ((myflags & TSK_FS_BLOCK_FLAG_META)
            && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_META)))
            continue;
        else if ((myflags & TSK_FS_BLOCK_FLAG_CONT)
            && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_CONT)))
            continue;
        else if ((myflags & TSK_FS_BLOCK_FLAG_ALLOC)
            && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_ALLOC)))
            continue;
        else if ((myflags & TSK_FS_BLOCK_FLAG_UNALLOC)
            && (!(a_flags & TSK_FS_BLOCK_WALK_FLAG_UNALLOC)))
            continue;

        if (tsk_fs_block_get(a_fs, fs_block, addr) == NULL) {
            snprintf(tsk_errstr2, TSK_ERRSTR_L,
                "ext2fs_block_walk: block %" PRIuDADDR, addr);
            tsk_fs_block_free(fs_block);
            return 1;
        }

        retval = a_action(fs_block, a_ptr);
        if (retval == TSK_WALK_STOP) {
            break;
        }
        else if (retval == TSK_WALK_ERROR) {
            tsk_fs_block_free(fs_block);
            return 1;
        }
    }

    /*
     * Cleanup.
     */
    tsk_fs_block_free(fs_block);
    return 0;
}



static uint8_t
ext2fs_fscheck(TSK_FS_INFO * fs, FILE * hFile)
{
    tsk_error_reset();
    tsk_errno = TSK_ERR_FS_UNSUPFUNC;
    snprintf(tsk_errstr, TSK_ERRSTR_L,
        "fscheck not implemented yet for Ext3");
    return 1;
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
ext2fs_fsstat(TSK_FS_INFO * fs, FILE * hFile)
{
    unsigned int i;
    EXT2FS_INFO *ext2fs = (EXT2FS_INFO *) fs;
    ext2fs_sb *sb = ext2fs->fs;
    int ibpg;
    time_t tmptime;


    // clean up any error messages that are lying around
    tsk_error_reset();

    tsk_fprintf(hFile, "FILE SYSTEM INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    tsk_fprintf(hFile, "File System Type: %s\n",
        (fs->ftype == TSK_FS_TYPE_EXT3) ? "Ext3" : "Ext2");
    tsk_fprintf(hFile, "Volume Name: %s\n", sb->s_volume_name);
    tsk_fprintf(hFile, "Volume ID: %" PRIx64 "%" PRIx64 "\n",
        tsk_getu64(fs->endian, &sb->s_uuid[8]), tsk_getu64(fs->endian,
            &sb->s_uuid[0]));

    tmptime = tsk_getu32(fs->endian, sb->s_wtime);
    tsk_fprintf(hFile, "\nLast Written at: %s",
        (tmptime > 0) ? asctime(localtime(&tmptime)) : "empty");
    tmptime = tsk_getu32(fs->endian, sb->s_lastcheck);
    tsk_fprintf(hFile, "Last Checked at: %s",
        (tmptime > 0) ? asctime(localtime(&tmptime)) : "empty");

    tmptime = tsk_getu32(fs->endian, sb->s_mtime);
    tsk_fprintf(hFile, "\nLast Mounted at: %s",
        (tmptime > 0) ? asctime(localtime(&tmptime)) : "empty");

    /* State of the file system */
    if (tsk_getu16(fs->endian, sb->s_state) & EXT2FS_STATE_VALID)
        tsk_fprintf(hFile, "Unmounted properly\n");
    else
        tsk_fprintf(hFile, "Unmounted Improperly\n");

    if (sb->s_last_mounted != '\0')
        tsk_fprintf(hFile, "Last mounted on: %s\n", sb->s_last_mounted);

    tsk_fprintf(hFile, "\nSource OS: ");
    switch (tsk_getu32(fs->endian, sb->s_creator_os)) {
    case EXT2FS_OS_LINUX:
        tsk_fprintf(hFile, "Linux\n");
        break;
    case EXT2FS_OS_HURD:
        tsk_fprintf(hFile, "HURD\n");
        break;
    case EXT2FS_OS_MASIX:
        tsk_fprintf(hFile, "MASIX\n");
        break;
    case EXT2FS_OS_FREEBSD:
        tsk_fprintf(hFile, "FreeBSD\n");
        break;
    case EXT2FS_OS_LITES:
        tsk_fprintf(hFile, "LITES\n");
        break;
    default:
        tsk_fprintf(hFile, "%" PRIx32 "\n", tsk_getu32(fs->endian,
                sb->s_creator_os));
        break;
    }

    if (tsk_getu32(fs->endian, sb->s_rev_level) == EXT2FS_REV_ORIG)
        tsk_fprintf(hFile, "Static Structure\n");
    else
        tsk_fprintf(hFile, "Dynamic Structure\n");


    /* add features */
    if (tsk_getu32(fs->endian, sb->s_feature_compat)) {
        tsk_fprintf(hFile, "Compat Features: ");

        if (tsk_getu32(fs->endian, sb->s_feature_compat) &
            EXT2FS_FEATURE_COMPAT_DIR_PREALLOC)
            tsk_fprintf(hFile, "Dir Prealloc, ");
        if (tsk_getu32(fs->endian, sb->s_feature_compat) &
            EXT2FS_FEATURE_COMPAT_IMAGIC_INODES)
            tsk_fprintf(hFile, "iMagic inodes, ");
        if (tsk_getu32(fs->endian, sb->s_feature_compat) &
            EXT2FS_FEATURE_COMPAT_HAS_JOURNAL)
            tsk_fprintf(hFile, "Journal, ");
        if (tsk_getu32(fs->endian, sb->s_feature_compat) &
            EXT2FS_FEATURE_COMPAT_EXT_ATTR)
            tsk_fprintf(hFile, "Ext Attributes, ");
        if (tsk_getu32(fs->endian, sb->s_feature_compat) &
            EXT2FS_FEATURE_COMPAT_RESIZE_INO)
            tsk_fprintf(hFile, "Resize Inode, ");
        if (tsk_getu32(fs->endian, sb->s_feature_compat) &
            EXT2FS_FEATURE_COMPAT_DIR_INDEX)
            tsk_fprintf(hFile, "Dir Index");

        tsk_fprintf(hFile, "\n");
    }

    if (tsk_getu32(fs->endian, sb->s_feature_incompat)) {
        tsk_fprintf(hFile, "InCompat Features: ");

        if (tsk_getu32(fs->endian, sb->s_feature_incompat) &
            EXT2FS_FEATURE_INCOMPAT_COMPRESSION)
            tsk_fprintf(hFile, "Compression, ");
        if (tsk_getu32(fs->endian, sb->s_feature_incompat) &
            EXT2FS_FEATURE_INCOMPAT_FILETYPE)
            tsk_fprintf(hFile, "Filetype, ");
        if (tsk_getu32(fs->endian, sb->s_feature_incompat) &
            EXT2FS_FEATURE_INCOMPAT_RECOVER)
            tsk_fprintf(hFile, "Needs Recovery, ");
        if (tsk_getu32(fs->endian, sb->s_feature_incompat) &
            EXT2FS_FEATURE_INCOMPAT_JOURNAL_DEV)
            tsk_fprintf(hFile, "Journal Dev");

        tsk_fprintf(hFile, "\n");
    }

    if (tsk_getu32(fs->endian, sb->s_feature_ro_compat)) {
        tsk_fprintf(hFile, "Read Only Compat Features: ");

        if (tsk_getu32(fs->endian, sb->s_feature_ro_compat) &
            EXT2FS_FEATURE_RO_COMPAT_SPARSE_SUPER)
            tsk_fprintf(hFile, "Sparse Super, ");
        if (tsk_getu32(fs->endian, sb->s_feature_ro_compat) &
            EXT2FS_FEATURE_RO_COMPAT_LARGE_FILE)
            tsk_fprintf(hFile, "Has Large Files, ");
        if (tsk_getu32(fs->endian, sb->s_feature_ro_compat) &
            EXT2FS_FEATURE_RO_COMPAT_BTREE_DIR)
            tsk_fprintf(hFile, "Btree Dir");

        tsk_fprintf(hFile, "\n");
    }

    /* Print journal information */
    if (tsk_getu32(fs->endian, sb->s_feature_compat) &
        EXT2FS_FEATURE_COMPAT_HAS_JOURNAL) {

        tsk_fprintf(hFile, "\nJournal ID: %" PRIx64 "%" PRIx64 "\n",
            tsk_getu64(fs->endian, &sb->s_journal_uuid[8]),
            tsk_getu64(fs->endian, &sb->s_journal_uuid[0]));

        if (tsk_getu32(fs->endian, sb->s_journal_inum) != 0)
            tsk_fprintf(hFile, "Journal Inode: %" PRIu32 "\n",
                tsk_getu32(fs->endian, sb->s_journal_inum));

        if (tsk_getu32(fs->endian, sb->s_journal_dev) != 0)
            tsk_fprintf(hFile, "Journal Device: %" PRIu32 "\n",
                tsk_getu32(fs->endian, sb->s_journal_dev));


    }

    tsk_fprintf(hFile, "\nMETADATA INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    tsk_fprintf(hFile, "Inode Range: %" PRIuINUM " - %" PRIuINUM "\n",
        fs->first_inum, fs->last_inum);
    tsk_fprintf(hFile, "Root Directory: %" PRIuINUM "\n", fs->root_inum);

    tsk_fprintf(hFile, "Free Inodes: %" PRIu32 "\n",
        tsk_getu32(fs->endian, sb->s_free_inode_count));


    if (tsk_getu32(fs->endian, sb->s_last_orphan)) {
        uint32_t or_in;
        tsk_fprintf(hFile, "Orphan Inodes: ");
        or_in = tsk_getu32(fs->endian, sb->s_last_orphan);

        while (or_in) {
            TSK_FS_FILE *fs_file;

            if ((or_in > fs->last_inum) || (or_in < fs->first_inum))
                break;

            tsk_fprintf(hFile, "%" PRIu32 ", ", or_in);

            if ((fs_file = tsk_fs_file_alloc(fs)) == NULL) {
                /* Ignore this error */
                tsk_error_reset();
                break;
            }

            /* Get the next one */
            if (ext2fs_inode_lookup(fs, fs_file, or_in)) {
                /* Ignore this error */
                tsk_error_reset();
                break;
            }

            or_in = (uint32_t) fs_file->meta->time2.ext2.dtime;
            tsk_fs_file_close(fs_file);
        }
        tsk_fprintf(hFile, "\n");
    }

    tsk_fprintf(hFile, "\nCONTENT INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    tsk_fprintf(hFile, "Block Range: %" PRIuDADDR " - %" PRIuDADDR "\n",
        fs->first_block, fs->last_block);

    if (fs->last_block != fs->last_block_act)
        tsk_fprintf(hFile,
            "Total Range in Image: %" PRIuDADDR " - %" PRIuDADDR "\n",
            fs->first_block, fs->last_block_act);

    tsk_fprintf(hFile, "Block Size: %u\n", fs->block_size);

    if (tsk_getu32(fs->endian, sb->s_first_data_block))
        tsk_fprintf(hFile,
            "Reserved Blocks Before Block Groups: %" PRIu32 "\n",
            tsk_getu32(fs->endian, sb->s_first_data_block));

    tsk_fprintf(hFile, "Free Blocks: %" PRIu32 "\n",
        tsk_getu32(fs->endian, sb->s_free_blocks_count));

    tsk_fprintf(hFile, "\nBLOCK GROUP INFORMATION\n");
    tsk_fprintf(hFile, "--------------------------------------------\n");

    tsk_fprintf(hFile, "Number of Block Groups: %" PRI_EXT2GRP "\n",
        ext2fs->groups_count);

    tsk_fprintf(hFile, "Inodes per group: %" PRIu32 "\n",
        tsk_getu32(fs->endian, sb->s_inodes_per_group));
    tsk_fprintf(hFile, "Blocks per group: %" PRIu32 "\n",
        tsk_getu32(fs->endian, sb->s_blocks_per_group));


    /* number of blocks the inodes consume */
    ibpg =
        (tsk_getu32(fs->endian,
            sb->s_inodes_per_group) * ext2fs->inode_size + fs->block_size -
        1) / fs->block_size;

    for (i = 0; i < ext2fs->groups_count; i++) {
        TSK_DADDR_T cg_base;
        TSK_INUM_T inum;

        if (ext2fs_group_load(ext2fs, i)) {
            return 1;
        }
        tsk_fprintf(hFile, "\nGroup: %d:\n", i);

        inum =
            fs->first_inum + tsk_gets32(fs->endian,
            sb->s_inodes_per_group) * i;
        tsk_fprintf(hFile, "  Inode Range: %" PRIuINUM " - ", inum);

        if ((inum + tsk_gets32(fs->endian, sb->s_inodes_per_group) - 1) <
            fs->last_inum)
            tsk_fprintf(hFile, "%" PRIuINUM "\n",
                inum + tsk_gets32(fs->endian, sb->s_inodes_per_group) - 1);
        else
            tsk_fprintf(hFile, "%" PRIuINUM "\n", fs->last_inum);


        cg_base = ext2_cgbase_lcl(fs, sb, i);

        tsk_fprintf(hFile,
            "  Block Range: %" PRIuDADDR " - %" PRIuDADDR "\n", cg_base,
            ((ext2_cgbase_lcl(fs, sb,
                        i + 1) - 1) <
                fs->last_block) ? (ext2_cgbase_lcl(fs, sb,
                    i + 1) - 1) : fs->last_block);


        tsk_fprintf(hFile, "  Layout:\n");

        /* only print the super block data if we are not in a sparse
         * group 
         */
        if (((tsk_getu32(fs->endian, ext2fs->fs->s_feature_ro_compat) &
                    EXT2FS_FEATURE_RO_COMPAT_SPARSE_SUPER) &&
                (cg_base != tsk_getu32(fs->endian,
                        ext2fs->grp_buf->bg_block_bitmap)))
            || ((tsk_getu32(fs->endian,
                        ext2fs->fs->s_feature_ro_compat) &
                    EXT2FS_FEATURE_RO_COMPAT_SPARSE_SUPER) == 0)) {

            TSK_OFF_T boff;

            /* the super block is the first 1024 bytes */
            tsk_fprintf(hFile,
                "    Super Block: %" PRIuDADDR " - %" PRIuDADDR "\n",
                cg_base,
                cg_base +
                ((sizeof(ext2fs_sb) + fs->block_size -
                        1) / fs->block_size) - 1);

            boff = roundup(sizeof(ext2fs_sb), fs->block_size);

            /* Group Descriptors */
            tsk_fprintf(hFile,
                "    Group Descriptor Table: %" PRIuDADDR " - ",
                (cg_base + (boff + fs->block_size - 1) / fs->block_size));

            boff += (ext2fs->groups_count * sizeof(ext2fs_gd));
            tsk_fprintf(hFile, "%" PRIuDADDR "\n",
                ((cg_base +
                        (boff + fs->block_size - 1) / fs->block_size) -
                    1));
        }


        /* The block bitmap is a full block */
        tsk_fprintf(hFile, "    Data bitmap: %" PRIu32 " - %" PRIu32 "\n",
            tsk_getu32(fs->endian, ext2fs->grp_buf->bg_block_bitmap),
            tsk_getu32(fs->endian, ext2fs->grp_buf->bg_block_bitmap));


        /* The inode bitmap is a full block */
        tsk_fprintf(hFile, "    Inode bitmap: %" PRIu32 " - %" PRIu32 "\n",
            tsk_getu32(fs->endian, ext2fs->grp_buf->bg_inode_bitmap),
            tsk_getu32(fs->endian, ext2fs->grp_buf->bg_inode_bitmap));


        tsk_fprintf(hFile, "    Inode Table: %" PRIu32 " - %" PRIu32 "\n",
            tsk_getu32(fs->endian, ext2fs->grp_buf->bg_inode_table),
            tsk_getu32(fs->endian,
                ext2fs->grp_buf->bg_inode_table) + ibpg - 1);


        tsk_fprintf(hFile, "    Data Blocks: ");

        /* If we are in a sparse group, display the other addresses */
        if ((tsk_getu32(fs->endian, ext2fs->fs->s_feature_ro_compat) &
                EXT2FS_FEATURE_RO_COMPAT_SPARSE_SUPER) &&
            (cg_base == tsk_getu32(fs->endian,
                    ext2fs->grp_buf->bg_block_bitmap))) {

            /* it goes from the end of the inode bitmap to before the
             * table
             *
             * This hard coded aspect does not scale ...
             */
            tsk_fprintf(hFile, "%" PRIu32 " - %" PRIu32 ", ",
                tsk_getu32(fs->endian,
                    ext2fs->grp_buf->bg_inode_bitmap) + 1,
                tsk_getu32(fs->endian,
                    ext2fs->grp_buf->bg_inode_table) - 1);
        }

        tsk_fprintf(hFile, "%" PRIuDADDR " - %" PRIuDADDR "\n",
            (uint64_t) tsk_getu32(fs->endian,
                ext2fs->grp_buf->bg_inode_table) + ibpg,
            ((ext2_cgbase_lcl(fs, sb, i + 1) - 1) <
                fs->last_block) ? (ext2_cgbase_lcl(fs, sb,
                    i + 1) - 1) : fs->last_block);


        /* Print the free info */


        /* The last group may not have a full number of blocks */
        if (i != (ext2fs->groups_count - 1)) {
            tsk_fprintf(hFile,
                "  Free Inodes: %" PRIu16 " (%" PRIu32 "%%)\n",
                tsk_getu16(fs->endian,
                    ext2fs->grp_buf->bg_free_inodes_count),
                (100 * tsk_getu16(fs->endian,
                        ext2fs->grp_buf->bg_free_inodes_count)) /
                tsk_getu32(fs->endian, sb->s_inodes_per_group));

            tsk_fprintf(hFile,
                "  Free Blocks: %" PRIu16 " (%" PRIu32 "%%)\n",
                tsk_getu16(fs->endian,
                    ext2fs->grp_buf->bg_free_blocks_count),
                (100 * tsk_getu16(fs->endian,
                        ext2fs->grp_buf->bg_free_blocks_count)) /
                tsk_getu32(fs->endian, sb->s_blocks_per_group));
        }
        else {
            TSK_INUM_T inum_left;
            TSK_DADDR_T blk_left;

            inum_left =
                fs->last_inum % tsk_gets32(fs->endian,
                sb->s_inodes_per_group);
            if (inum_left == 0)
                inum_left = tsk_getu32(fs->endian, sb->s_inodes_per_group);

            tsk_fprintf(hFile, "  Free Inodes: %" PRIu16 " (%d%%)\n",
                tsk_getu16(fs->endian,
                    ext2fs->grp_buf->bg_free_inodes_count),
                (100 * tsk_getu16(fs->endian,
                        ext2fs->grp_buf->bg_free_inodes_count)) /
                inum_left);

            /* Now blocks */
            blk_left =
                fs->block_count % tsk_getu32(fs->endian,
                sb->s_blocks_per_group);
            if (blk_left == 0)
                blk_left = tsk_getu32(fs->endian, sb->s_blocks_per_group);

            tsk_fprintf(hFile, "  Free Blocks: %" PRIu16 " (%d%%)\n",
                tsk_getu16(fs->endian,
                    ext2fs->grp_buf->bg_free_blocks_count),
                (100 * tsk_getu16(fs->endian,
                        ext2fs->grp_buf->bg_free_blocks_count)) /
                blk_left);
        }

        tsk_fprintf(hFile, "  Total Directories: %" PRIu16 "\n",
            tsk_getu16(fs->endian, ext2fs->grp_buf->bg_used_dirs_count));
    }

    return 0;
}


/************************* istat *******************************/

static void
ext2fs_make_acl_str(char *str, int len, uint16_t perm)
{
    int i = 0;

    if (perm & EXT2_PACL_PERM_READ) {
        snprintf(&str[i], len - 1, "Read");
        i += 4;
    }
    if (perm & EXT2_PACL_PERM_WRITE) {
        if (i) {
            snprintf(&str[i], len - 1, ", ");
            i += 2;
        }
        snprintf(&str[i], len - 1, "Write");
        i += 5;
    }
    if (perm & EXT2_PACL_PERM_EXEC) {
        if (i) {
            snprintf(&str[i], len - 1, ", ");
            i += 2;
        }
        snprintf(&str[i], len - 1, "Execute");
        i += 7;
    }
}


typedef struct {
    FILE *hFile;
    int idx;
} EXT2FS_PRINT_ADDR;


/* Callback for istat to print the block addresses */
static TSK_WALK_RET_ENUM
print_addr_act(TSK_FS_FILE * fs_file, TSK_OFF_T a_off, TSK_DADDR_T addr,
    char *buf, size_t size, TSK_FS_BLOCK_FLAG_ENUM flags, void *a_ptr)
{
    TSK_FS_INFO *fs = fs_file->fs_info;
    EXT2FS_PRINT_ADDR *print = (EXT2FS_PRINT_ADDR *) a_ptr;

    if (flags & TSK_FS_BLOCK_FLAG_CONT) {
        int i, s;
        /* cycle through the blocks if they exist */
        for (i = 0, s = (int) size; s > 0; s -= fs->block_size, i++) {

            /* sparse file */
            if (addr)
                tsk_fprintf(print->hFile, "%" PRIuDADDR " ", addr + i);
            else
                tsk_fprintf(print->hFile, "0 ");

            if (++(print->idx) == 8) {
                tsk_fprintf(print->hFile, "\n");
                print->idx = 0;
            }
        }
    }

    return TSK_WALK_CONT;
}


/**
 * Print details on a specific file to a file handle. 
 *
 * @param fs File system file is located in
 * @param hFile File handle to print text to
 * @param inum Address of file in file system
 * @param numblock The number of blocks in file to force print (can go beyond file size)
 * @param sec_skew Clock skew in seconds to also print times in
 * 
 * @returns 1 on error and 0 on success
 */
static uint8_t
ext2fs_istat(TSK_FS_INFO * fs, FILE * hFile, TSK_INUM_T inum,
    TSK_DADDR_T numblock, int32_t sec_skew)
{
    EXT2FS_INFO *ext2fs = (EXT2FS_INFO *) fs;
    TSK_FS_META *fs_meta;
    TSK_FS_FILE *fs_file;
    char ls[12];
    EXT2FS_PRINT_ADDR print;
    const TSK_FS_ATTR *fs_attr_indir;

    // clean up any error messages that are lying around
    tsk_error_reset();

    if ((fs_file = tsk_fs_file_open_meta(fs, NULL, inum)) == NULL) {
        return 1;
    }
    fs_meta = fs_file->meta;

    tsk_fprintf(hFile, "inode: %" PRIuINUM "\n", inum);
    tsk_fprintf(hFile, "%sAllocated\n",
        (fs_meta->flags & TSK_FS_META_FLAG_ALLOC) ? "" : "Not ");

    tsk_fprintf(hFile, "Group: %" PRIuGID "\n", ext2fs->grp_num);

    // Note that if this is a "virtual file", then ext2fs->dino_buf may not be set.
    if (ext2fs->dino_buf)
        tsk_fprintf(hFile, "Generation Id: %" PRIu32 "\n",
            tsk_getu32(fs->endian, ext2fs->dino_buf->i_generation));

    if (fs_meta->link)
        tsk_fprintf(hFile, "symbolic link to: %s\n", fs_meta->link);

    tsk_fprintf(hFile, "uid / gid: %" PRIuUID " / %" PRIuGID "\n",
        fs_meta->uid, fs_meta->gid);

    tsk_fs_meta_make_ls(fs_meta, ls, sizeof(ls));
    tsk_fprintf(hFile, "mode: %s\n", ls);

    if (ext2fs->dino_buf) {
        /* Print the device ids */
        if ((fs_meta->type == TSK_FS_META_TYPE_BLK)
            || (fs_meta->type == TSK_FS_META_TYPE_CHR)) {
            tsk_fprintf(hFile,
                "Device Major: %" PRIu8 "   Minor: %" PRIu8 "\n",
                ext2fs->dino_buf->i_block[0][1],
                ext2fs->dino_buf->i_block[0][0]);
        }

        if (tsk_getu32(fs->endian, ext2fs->dino_buf->i_flags)) {
            tsk_fprintf(hFile, "Flags: ");
            if (tsk_getu32(fs->endian,
                    ext2fs->dino_buf->i_flags) & EXT2_IN_SECDEL)
                tsk_fprintf(hFile, "Secure Delete, ");

            if (tsk_getu32(fs->endian,
                    ext2fs->dino_buf->i_flags) & EXT2_IN_UNRM)
                tsk_fprintf(hFile, "Undelete, ");

            if (tsk_getu32(fs->endian,
                    ext2fs->dino_buf->i_flags) & EXT2_IN_COMP)
                tsk_fprintf(hFile, "Compressed, ");

            if (tsk_getu32(fs->endian,
                    ext2fs->dino_buf->i_flags) & EXT2_IN_SYNC)
                tsk_fprintf(hFile, "Sync Updates, ");

            if (tsk_getu32(fs->endian,
                    ext2fs->dino_buf->i_flags) & EXT2_IN_IMM)
                tsk_fprintf(hFile, "Immutable, ");

            if (tsk_getu32(fs->endian,
                    ext2fs->dino_buf->i_flags) & EXT2_IN_APPEND)
                tsk_fprintf(hFile, "Append Only, ");

            if (tsk_getu32(fs->endian,
                    ext2fs->dino_buf->i_flags) & EXT2_IN_NODUMP)
                tsk_fprintf(hFile, "Do Not Dump, ");

            if (tsk_getu32(fs->endian,
                    ext2fs->dino_buf->i_flags) & EXT2_IN_NOA)
                tsk_fprintf(hFile, "No A-Time, ");

            tsk_fprintf(hFile, "\n");
        }
    }

    tsk_fprintf(hFile, "size: %" PRIuOFF "\n", fs_meta->size);
    tsk_fprintf(hFile, "num of links: %d\n", fs_meta->nlink);

    if (ext2fs->dino_buf) {
        /* Ext attribute are stored in a block with a header and a list
         * of entries that are aligned to 4-byte boundaries.  The attr
         * value is stored at the end of the block.  There are 4 null bytes
         * in between the headers and values 
         */
        if (tsk_getu32(fs->endian, ext2fs->dino_buf->i_file_acl) != 0) {
            char *buf;
            ext2fs_ea_header *ea_head;
            ext2fs_ea_entry *ea_entry;
            ssize_t cnt;

            if ((buf = tsk_malloc(fs->block_size)) == NULL) {
                tsk_fs_file_close(fs_file);
                return 1;
            }

            tsk_fprintf(hFile,
                "\nExtended Attributes  (Block: %" PRIu32 ")\n",
                tsk_getu32(fs->endian, ext2fs->dino_buf->i_file_acl));

            /* Is the value too big? */
            if (tsk_getu32(fs->endian,
                    ext2fs->dino_buf->i_file_acl) > fs->last_block) {
                tsk_fprintf(hFile,
                    "Extended Attributes block is larger than file system\n");
                goto egress_ea;
            }

            cnt = tsk_fs_read(fs,
                (TSK_DADDR_T) tsk_getu32(fs->endian,
                    ext2fs->dino_buf->i_file_acl) * fs->block_size,
                buf, fs->block_size);

            if (cnt != fs->block_size) {
                if (cnt >= 0) {
                    tsk_error_reset();
                    tsk_errno = TSK_ERR_FS_READ;
                }
                snprintf(tsk_errstr2, TSK_ERRSTR_L,
                    "ext2fs_istat: ACL block %" PRIu32,
                    tsk_getu32(fs->endian, ext2fs->dino_buf->i_file_acl));
                tsk_fs_file_close(fs_file);
                free(buf);
                return 1;
            }


            /* Check the header */
            ea_head = (ext2fs_ea_header *) buf;
            if (tsk_getu32(fs->endian, ea_head->magic) != EXT2_EA_MAGIC) {
                tsk_fprintf(hFile,
                    "Incorrect extended attribute header: %" PRIx32 "\n",
                    tsk_getu32(fs->endian, ea_head->magic));
            }


            /* Cycle through each entry - at the top of the block */
            for (ea_entry = (ext2fs_ea_entry *) & ea_head->entry;
                ((uintptr_t) ea_entry <
                    ((uintptr_t) buf + fs->block_size -
                        sizeof(ext2fs_ea_entry)));
                ea_entry =
                (ext2fs_ea_entry *) ((uintptr_t) ea_entry +
                    EXT2_EA_LEN(ea_entry->nlen))) {

                char name[256];

                /* Stop if the first four bytes are NULL */
                if ((ea_entry->nlen == 0) && (ea_entry->nidx == 0) &&
                    (tsk_getu16(fs->endian, ea_entry->val_off) == 0))
                    break;

                /* The Linux src does not allow this */
                if (tsk_getu32(fs->endian, ea_entry->val_blk) != 0) {
                    tsk_fprintf(hFile,
                        "Attribute has non-zero value block - skipping\n");
                    continue;
                }


                /* Is the value location and size valid? */
                if ((tsk_getu32(fs->endian,
                            ea_entry->val_off) > fs->block_size)
                    || ((tsk_getu16(fs->endian,
                                ea_entry->val_off) + tsk_getu32(fs->endian,
                                ea_entry->val_size)) > fs->block_size)) {
                    continue;
                }


                /* Copy the name into a buffer - not NULL term */
                strncpy(name, (char *) &ea_entry->name, ea_entry->nlen);
                name[ea_entry->nlen] = '\0';


                /* User assigned attributes - setfattr / getfattr */
                if ((ea_entry->nidx == EXT2_EA_IDX_USER) ||
                    (ea_entry->nidx == EXT2_EA_IDX_TRUSTED) ||
                    (ea_entry->nidx == EXT2_EA_IDX_SECURITY)) {
                    char val[256];

                    strncpy(val,
                        &buf[tsk_getu16(fs->endian, ea_entry->val_off)],
                        tsk_getu32(fs->endian,
                            ea_entry->val_size) >
                        256 ? 256 : tsk_getu32(fs->endian,
                            ea_entry->val_size));

                    val[tsk_getu32(fs->endian, ea_entry->val_size) > 256 ?
                        256 : tsk_getu32(fs->endian, ea_entry->val_size)] =
                        '\0';

                    if (ea_entry->nidx == EXT2_EA_IDX_USER)
                        tsk_fprintf(hFile, "user.%s=%s\n", name, val);
                    else if (ea_entry->nidx == EXT2_EA_IDX_TRUSTED)
                        tsk_fprintf(hFile, "trust.%s=%s\n", name, val);
                    else if (ea_entry->nidx == EXT2_EA_IDX_SECURITY)
                        tsk_fprintf(hFile, "security.%s=%s\n", name, val);

                }


                /* POSIX ACL - setfacl / getfacl stuff */
                else if ((ea_entry->nidx == EXT2_EA_IDX_POSIX_ACL_ACCESS)
                    || (ea_entry->nidx == EXT2_EA_IDX_POSIX_ACL_DEFAULT)) {

                    ext2fs_pos_acl_entry_lo *acl_lo;
                    ext2fs_pos_acl_head *acl_head;

                    if (ea_entry->nidx == EXT2_EA_IDX_POSIX_ACL_ACCESS)
                        tsk_fprintf(hFile,
                            "POSIX Access Control List Entries:\n");
                    else if (ea_entry->nidx ==
                        EXT2_EA_IDX_POSIX_ACL_DEFAULT)
                        tsk_fprintf(hFile,
                            "POSIX Default Access Control List Entries:\n");

                    /* examine the header */
                    acl_head =
                        (ext2fs_pos_acl_head *) &
                        buf[tsk_getu16(fs->endian, ea_entry->val_off)];

                    if (tsk_getu32(fs->endian, acl_head->ver) != 1) {
                        tsk_fprintf(hFile,
                            "Invalid ACL Header Version: %" PRIu32 "\n",
                            tsk_getu32(fs->endian, acl_head->ver));
                        continue;
                    }

                    /* The first entry starts after the header */
                    acl_lo =
                        (ext2fs_pos_acl_entry_lo *) ((uintptr_t) acl_head +
                        sizeof(ext2fs_pos_acl_head));


                    /* Cycle through the values */
                    while ((uintptr_t) acl_lo <
                        ((uintptr_t) buf +
                            tsk_getu16(fs->endian,
                                ea_entry->val_off) + tsk_getu32(fs->endian,
                                ea_entry->val_size))) {

                        char perm[64];
                        int len;

                        /* Make a string from the permissions */
                        ext2fs_make_acl_str(perm, 64,
                            tsk_getu16(fs->endian, acl_lo->perm));

                        switch (tsk_getu16(fs->endian, acl_lo->tag)) {
                        case EXT2_PACL_TAG_USERO:
                            tsk_fprintf(hFile, "  uid: %" PRIuUID ": %s\n",
                                fs_meta->uid, perm);
                            len = sizeof(ext2fs_pos_acl_entry_sh);
                            break;

                        case EXT2_PACL_TAG_GRPO:
                            tsk_fprintf(hFile, "  gid: %" PRIuGID ": %s\n",
                                fs_meta->gid, perm);
                            len = sizeof(ext2fs_pos_acl_entry_sh);
                            break;
                        case EXT2_PACL_TAG_OTHER:
                            tsk_fprintf(hFile, "  other: %s\n", perm);
                            len = sizeof(ext2fs_pos_acl_entry_sh);
                            break;
                        case EXT2_PACL_TAG_MASK:
                            tsk_fprintf(hFile, "  mask: %s\n", perm);
                            len = sizeof(ext2fs_pos_acl_entry_sh);
                            break;


                        case EXT2_PACL_TAG_GRP:
                            tsk_fprintf(hFile, "  gid: %" PRIu32 ": %s\n",
                                tsk_getu32(fs->endian, acl_lo->id), perm);
                            len = sizeof(ext2fs_pos_acl_entry_lo);
                            break;

                        case EXT2_PACL_TAG_USER:
                            tsk_fprintf(hFile, "  uid: %" PRIu32 ": %s\n",
                                tsk_getu32(fs->endian, acl_lo->id), perm);

                            len = sizeof(ext2fs_pos_acl_entry_lo);
                            break;

                        default:
                            tsk_fprintf(hFile, "Unknown ACL tag: %d\n",
                                tsk_getu16(fs->endian, acl_lo->tag));
                            len = sizeof(ext2fs_pos_acl_entry_sh);
                            break;
                        }
                        acl_lo =
                            (ext2fs_pos_acl_entry_lo *) ((uintptr_t) acl_lo
                            + len);
                    }
                }
                else {
                    tsk_fprintf(hFile,
                        "Unsupported Extended Attr Type: %d\n",
                        ea_entry->nidx);
                }
            }
          egress_ea:

            free(buf);
        }
    }

    if (sec_skew != 0) {
        tsk_fprintf(hFile, "\nAdjusted Inode Times:\n");
        fs_meta->mtime -= sec_skew;
        fs_meta->atime -= sec_skew;
        fs_meta->ctime -= sec_skew;

        tsk_fprintf(hFile, "Accessed:\t%s", ctime(&fs_meta->atime));
        tsk_fprintf(hFile, "File Modified:\t%s", ctime(&fs_meta->mtime));
        tsk_fprintf(hFile, "Inode Modified:\t%s", ctime(&fs_meta->ctime));

        if (fs_meta->time2.ext2.dtime) {
            fs_meta->time2.ext2.dtime -= sec_skew;
            tsk_fprintf(hFile, "Deleted:\t%s",
                ctime(&fs_meta->time2.ext2.dtime));
            fs_meta->time2.ext2.dtime += sec_skew;
        }

        fs_meta->mtime += sec_skew;
        fs_meta->atime += sec_skew;
        fs_meta->ctime += sec_skew;

        tsk_fprintf(hFile, "\nOriginal Inode Times:\n");
    }
    else {
        tsk_fprintf(hFile, "\nInode Times:\n");
    }

    tsk_fprintf(hFile, "Accessed:\t%s", ctime(&fs_meta->atime));
    tsk_fprintf(hFile, "File Modified:\t%s", ctime(&fs_meta->mtime));
    tsk_fprintf(hFile, "Inode Modified:\t%s", ctime(&fs_meta->ctime));

    if (fs_meta->time2.ext2.dtime)
        tsk_fprintf(hFile, "Deleted:\t%s",
            ctime(&fs_meta->time2.ext2.dtime));

    if (numblock > 0)
        fs_meta->size = numblock * fs->block_size;

    tsk_fprintf(hFile, "\nDirect Blocks:\n");

    print.idx = 0;
    print.hFile = hFile;

    if (tsk_fs_file_walk(fs_file, TSK_FS_FILE_WALK_FLAG_AONLY,
            print_addr_act, (void *) &print)) {
        tsk_fprintf(hFile, "\nError reading file:  ");
        tsk_error_print(hFile);
        tsk_error_reset();
    }
    else if (print.idx != 0) {
        tsk_fprintf(hFile, "\n");
    }

    fs_attr_indir = tsk_fs_file_attr_get_type(fs_file,
        TSK_FS_ATTR_TYPE_UNIX_INDIR, 0, 0);
    if (fs_attr_indir) {
        tsk_fprintf(hFile, "\nIndirect Blocks:\n");

        print.idx = 0;

        if (tsk_fs_attr_walk(fs_attr_indir, TSK_FS_FILE_WALK_FLAG_AONLY,
                print_addr_act, (void *) &print)) {
            tsk_fprintf(hFile, "\nError reading indirect attribute:  ");
            tsk_error_print(hFile);
            tsk_error_reset();
        }
        else if (print.idx != 0) {
            tsk_fprintf(hFile, "\n");
        }
    }

    tsk_fs_file_close(fs_file);
    return 0;
}


/* ext2fs_close - close an ext2fs file system */

static void
ext2fs_close(TSK_FS_INFO * fs)
{
    EXT2FS_INFO *ext2fs = (EXT2FS_INFO *) fs;

    fs->tag = 0;
    free((char *) ext2fs->fs);
    if (ext2fs->dino_buf != NULL)
        free((char *) ext2fs->dino_buf);

    if (ext2fs->grp_buf != NULL)
        free((char *) ext2fs->grp_buf);

    if (ext2fs->bmap_buf != NULL)
        free((char *) ext2fs->bmap_buf);

    if (ext2fs->imap_buf != NULL)
        free((char *) ext2fs->imap_buf);

    if (fs->list_inum_named) {
        tsk_list_free(fs->list_inum_named);
        fs->list_inum_named = NULL;
    }

    free(ext2fs);
}

/**
 * \internal
 * Open part of a disk image as a Ext2/3 file system. 
 *
 * @param img_info Disk image to analyze
 * @param offset Byte offset where file system starts
 * @param ftype Specific type of file system
 * @param test NOT USED
 * @returns NULL on error or if data is not an Ext2/3 file system
 */
TSK_FS_INFO *
ext2fs_open(TSK_IMG_INFO * img_info, TSK_OFF_T offset,
    TSK_FS_TYPE_ENUM ftype, uint8_t test)
{
    EXT2FS_INFO *ext2fs;
    unsigned int len;
    TSK_FS_INFO *fs;
    ssize_t cnt;

    // clean up any error messages that are lying around
    tsk_error_reset();

    if (TSK_FS_TYPE_ISEXT(ftype) == 0) {
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_ARG;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Invalid FS Type in ext2fs_open");
        return NULL;
    }

    if ((ext2fs = (EXT2FS_INFO *) tsk_malloc(sizeof(*ext2fs))) == NULL)
        return NULL;

    fs = &(ext2fs->fs_info);

    fs->ftype = ftype;
    fs->flags = 0;
    fs->img_info = img_info;
    fs->offset = offset;
    fs->tag = TSK_FS_INFO_TAG;

    /*
     * Read the superblock.
     */
    len = sizeof(ext2fs_sb);
    if ((ext2fs->fs = (ext2fs_sb *) tsk_malloc(len)) == NULL) {
        fs->tag = 0;
        free(ext2fs);
        return NULL;
    }

    cnt = tsk_fs_read(fs, EXT2FS_SBOFF, (char *) ext2fs->fs, len);
    if (cnt != len) {
        if (cnt >= 0) {
            tsk_error_reset();
            tsk_errno = TSK_ERR_FS_READ;
        }
        snprintf(tsk_errstr2, TSK_ERRSTR_L, "ext2fs_open: superblock");
        fs->tag = 0;
        free(ext2fs->fs);
        free(ext2fs);
        return NULL;
    }

    /* 
     * Verify we are looking at an EXTxFS image
     */
    if (tsk_fs_guessu16(fs, ext2fs->fs->s_magic, EXT2FS_FS_MAGIC)) {
        fs->tag = 0;
        free(ext2fs->fs);
        free(ext2fs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "not an EXTxFS file system (magic)");
        return NULL;
    }

    if (tsk_verbose) {
        if (tsk_getu32(fs->endian, ext2fs->fs->s_feature_ro_compat) &
            EXT2FS_FEATURE_RO_COMPAT_SPARSE_SUPER)
            tsk_fprintf(stderr, "File system has sparse super blocks\n");

        tsk_fprintf(stderr, "First data block is %" PRIu32 "\n",
            tsk_getu32(fs->endian, ext2fs->fs->s_first_data_block));
    }

    /* If autodetect was given, look for the journal */
    if (ftype == TSK_FS_TYPE_EXT_DETECT) {
        if (tsk_getu32(fs->endian, ext2fs->fs->s_feature_compat) &
            EXT2FS_FEATURE_COMPAT_HAS_JOURNAL)
            fs->ftype = TSK_FS_TYPE_EXT3;
        else
            fs->ftype = TSK_FS_TYPE_EXT2;
    }
    fs->duname = "Fragment";


    /* we need to figure out if dentries are v1 or v2 */
    if (tsk_getu32(fs->endian, ext2fs->fs->s_feature_incompat) &
        EXT2FS_FEATURE_INCOMPAT_FILETYPE)
        ext2fs->deentry_type = EXT2_DE_V2;
    else
        ext2fs->deentry_type = EXT2_DE_V1;


    /* 
     * Calculate the meta data info 
     */
    fs->inum_count = tsk_getu32(fs->endian, ext2fs->fs->s_inodes_count) + 1;    // we are adding 1 in this calc to account for Orphans directory
    fs->last_inum = fs->inum_count;
    fs->first_inum = EXT2FS_FIRSTINO;
    fs->root_inum = EXT2FS_ROOTINO;

    if (fs->inum_count < 10) {
        fs->tag = 0;
        free(ext2fs->fs);
        free(ext2fs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_MAGIC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "Not an EXTxFS file system (inum count)");
        return NULL;
    }


    /* Set the size of the inode, but default to our data structure
     * size if it is larger */
    ext2fs->inode_size = tsk_getu16(fs->endian, ext2fs->fs->s_inode_size);
    if (ext2fs->inode_size < sizeof(ext2fs_inode)) {
        ext2fs->inode_size = sizeof(ext2fs_inode);
        if (tsk_verbose)
            tsk_fprintf(stderr,
                "SB inode size is too small, using default");
    }


    /* 
     * Calculate the block info 
     */
    fs->dev_bsize = img_info->sector_size;
    fs->block_count = tsk_getu32(fs->endian, ext2fs->fs->s_blocks_count);
    fs->first_block = 0;
    fs->last_block_act = fs->last_block = fs->block_count - 1;
    ext2fs->first_data_block =
        tsk_getu32(fs->endian, ext2fs->fs->s_first_data_block);

    if (tsk_getu32(fs->endian, ext2fs->fs->s_log_block_size) !=
        tsk_getu32(fs->endian, ext2fs->fs->s_log_frag_size)) {
        fs->tag = 0;
        free(ext2fs->fs);
        free(ext2fs);
        tsk_error_reset();
        tsk_errno = TSK_ERR_FS_UNSUPFUNC;
        snprintf(tsk_errstr, TSK_ERRSTR_L,
            "This file system has fragments that are a different size than blocks, which is not currently supported\nContact brian with details of the system that created this image");
        return NULL;
    }

    fs->block_size =
        EXT2FS_MIN_BLOCK_SIZE << tsk_getu32(fs->endian,
        ext2fs->fs->s_log_block_size);

    // determine the last block we have in this image
    if ((TSK_DADDR_T) ((img_info->size - offset) / fs->block_size) <
        fs->block_count)
        fs->last_block_act =
            (img_info->size - offset) / fs->block_size - 1;


    /* The group descriptors are located in the block following the 
     * super block */
    ext2fs->groups_offset =
        roundup((EXT2FS_SBOFF + sizeof(ext2fs_sb)), fs->block_size);

    ext2fs->groups_count =
        (EXT2_GRPNUM_T) ((tsk_getu32(fs->endian,
                ext2fs->fs->s_blocks_count) - ext2fs->first_data_block +
            tsk_getu32(fs->endian,
                ext2fs->fs->s_blocks_per_group) -
            1) / tsk_getu32(fs->endian, ext2fs->fs->s_blocks_per_group));


    /* Volume ID */
    for (fs->fs_id_used = 0; fs->fs_id_used < 16; fs->fs_id_used++) {
        fs->fs_id[fs->fs_id_used] = ext2fs->fs->s_uuid[fs->fs_id_used];
    }

    /* Set the generic function pointers */
    fs->inode_walk = ext2fs_inode_walk;
    fs->block_walk = ext2fs_block_walk;
    fs->block_getflags = ext2fs_block_getflags;

    fs->get_default_attr_type = tsk_fs_unix_get_default_attr_type;
    fs->load_attrs = tsk_fs_unix_make_data_run;

    fs->file_add_meta = ext2fs_inode_lookup;
    fs->dir_open_meta = ext2fs_dir_open_meta;
    fs->fsstat = ext2fs_fsstat;
    fs->fscheck = ext2fs_fscheck;
    fs->istat = ext2fs_istat;
    fs->name_cmp = tsk_fs_unix_name_cmp;
    fs->close = ext2fs_close;


    /* Journal */
    fs->journ_inum = tsk_getu32(fs->endian, ext2fs->fs->s_journal_inum);
    fs->jblk_walk = ext2fs_jblk_walk;
    fs->jentry_walk = ext2fs_jentry_walk;
    fs->jopen = ext2fs_jopen;

    /* initialize the caches */
    /* inode map */
    ext2fs->imap_buf = NULL;
    ext2fs->imap_grp_num = 0xffffffff;

    /* block map */
    ext2fs->bmap_buf = NULL;
    ext2fs->bmap_grp_num = 0xffffffff;

    /* dinode */
    ext2fs->dino_buf = NULL;
    ext2fs->dino_inum = 0xffffffff;

    /* group descriptor */
    ext2fs->grp_buf = NULL;
    ext2fs->grp_num = 0xffffffff;

    fs->list_inum_named = NULL;


    /*
     * Print some stats.
     */
    if (tsk_verbose)
        tsk_fprintf(stderr,
            "inodes %" PRIu32 " root ino %" PRIuINUM " blocks %" PRIu32
            " blocks/group %" PRIu32 "\n", tsk_getu32(fs->endian,
                ext2fs->fs->s_inodes_count),
            fs->root_inum, tsk_getu32(fs->endian,
                ext2fs->fs->s_blocks_count), tsk_getu32(fs->endian,
                ext2fs->fs->s_blocks_per_group));

    return (fs);
}
