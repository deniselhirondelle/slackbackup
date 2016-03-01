/*++

Module Name:

    ufsdvfs.c

Abstract:

    This module implements VFS entry points for
    UFSD-based Linux filesystem driver.

Author:

    Ahdrey Shedel

Revision History:

    27/12/2002 - Andrey Shedel - Created

    Since 29/07/2005 - Alexander Mamaev

--*/

//
// This field is updated by SVN
//
static const char s_FileVer[] = "$Id: ufsdvfs.c 233352 2014-08-05 13:53:21Z shura $";

//
// Tune ufsdvfs.c
//

//#define UFSD_COUNT_CONTAINED        "Use unix semantics for dir->i_nlink"
//#define UFSD_USE_ASM_DIV64          "Use built-in macros do_div in <asm/div64.h> instead of __udivdi3"
#define UFSD_READAHEAD_PAGES        8
// 2.6.25 certainly bug, 2.6.32 certainly not
// NOTE: Kernel's utf8 does not support U+10000 (see utf8_mbtowc for details and note that 'typedef _u16 wchar_t;' )
//#define UFSD_BUILTINT_UTF8          "Use builtin utf8 code page"
#ifdef UFSD_BUILTINT_UTF8
#pragma message "Use builtin utf8 code page"
#endif
#ifdef UFSD_DEBUG
#define UFSD_DEBUG_ALLOC            "Track memory allocation/deallocation"
#endif
// Activate this define to test readdir
//#define UFSD_EMULATE_SMALL_READDIR_BUFFER 10

#ifndef UFSD_SMART_DIRTY_SEC
  #define UFSD_SMART_DIRTY_SEC  0
#endif

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/nls.h>
#include <asm/uaccess.h>
#include <linux/backing-dev.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/mount.h>
#include <linux/xattr.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/uio.h>
#include <linux/statfs.h>
#include <linux/vermagic.h>
#include <linux/mpage.h>
#include <linux/blkdev.h>
#include <linux/delay.h> // jiffies_to_msecs
#include <linux/fs_struct.h>
#include <linux/aio.h>
#include <linux/pagevec.h>
#include <linux/namei.h>
#include <linux/swap.h>
#include <linux/bit_spinlock.h>
#include <linux/prefetch.h>
#include <linux/exportfs.h>
#include <linux/mutex.h>

#include "config.h"
#include "ufsdapi.h"
#include "vfsdebug.h"

#ifdef CONFIG_DEBUG_MUTEXES
//#warning "CONFIG_DEBUG_MUTEXES is ON"
#pragma message "CONFIG_DEBUG_MUTEXES is ON"
#endif

#if defined HAVE_LINUX_UIDGID_H && HAVE_LINUX_UIDGID_H
  #include <linux/uidgid.h>
#else
  #define __kuid_val( x )  (x)
  #define __kgid_val( x )  (x)
  #define uid_eq( v1, v2 ) ( v1 == v2 )
  #define gid_eq( v1, v2 ) ( v1 == v2 )
  #define KUIDT_INIT(value) ( value )
  #define KGIDT_INIT(value) ( value )
#endif

#if defined HAVE_STRUCT_BIO_BI_ITER && HAVE_STRUCT_BIO_BI_ITER
#define BIO_BISECTOR( bio ) bio->bi_iter.bi_sector
#define BIO_BISIZE( bio )   bio->bi_iter.bi_size
#else
#define BIO_BISECTOR( bio ) bio->bi_sector
#define BIO_BISIZE( bio )   bio->bi_size
#endif

#if defined UFSD_USE_ASM_DIV64
  #include <asm/div64.h> // this file defines macros 'do_div'
#endif

#if defined CONFIG_FS_POSIX_ACL \
  && ( (defined HAVE_DECL_POSIX_ACL_FROM_XATTR && HAVE_DECL_POSIX_ACL_FROM_XATTR)\
    || (defined HAVE_DECL_POSIX_ACL_TO_XATTR_V2 && HAVE_DECL_POSIX_ACL_TO_XATTR_V2) )\
  && (defined UFSD_NTFS2 || defined UFSD_HFS)
  #include <linux/posix_acl_xattr.h>
  #define UFSD_USE_XATTR              "Include code to support xattr and acl"

  #if defined HAVE_DECL_MODE_TYPE_MODE_T && HAVE_DECL_MODE_TYPE_MODE_T
    #define posix_acl_mode mode_t
  #elif defined HAVE_DECL_MODE_TYPE_UMODE_T && HAVE_DECL_MODE_TYPE_UMODE_T
    #define posix_acl_mode umode_t
  #endif
#endif

#ifndef UFSD_USE_FLUSH_THREAD
  #if !(defined HAVE_STRUCT_SUPER_OPERATIONS_WRITE_SUPER && HAVE_STRUCT_SUPER_OPERATIONS_WRITE_SUPER)
    #define UFSD_USE_FLUSH_THREAD "Use thread to flush periodically"
  #endif
#endif

//
// Default trace level for many functions in this module
//
#define Dbg  UFSD_LEVEL_VFS

#define UFSD_PACKAGE_STAMP " " "lke_9.0.0_33"

//
// Used to trace driver version
//
static const char s_DriverVer[] = PACKAGE_VERSION
#ifdef PACKAGE_TAG
   " " PACKAGE_TAG
#else
   UFSD_PACKAGE_STAMP
#endif
#if defined CONFIG_LBD | defined CONFIG_LBDAF
  ", LBD=ON"
#else
  ", LBD=OFF"
#endif
#if defined UFSD_DELAY_ALLOC
  ", da"
#endif
#if defined UFSD_USE_XATTR
  ", acl"
#endif
#if !defined UFSD_NO_USE_IOCTL
  ", ioctl"
#endif
#ifndef UFSD_DISABLE_UGM
  ", ugm"
#endif
#ifdef UFSD_CHECK_BDI
  ", bdi"
#endif
#ifdef UFSD_USE_FLUSH_THREAD
  ", sd2(" __stringify(UFSD_SMART_DIRTY_SEC) ")"
#else
  ", sd(" __stringify(UFSD_SMART_DIRTY_SEC) ")"
#endif
#ifdef WRITE_FLUSH_FUA
  ", fua"
#elif defined WRITE_BARRIER
  ", wb"
#else
  ", nb"
#endif
#ifdef UFSD_USE_BUILTIN_ZEROING
  ", bz"
#endif
#ifdef UFSD_DEBUG
  ", debug"
#elif defined UFSD_TRACE
  ", tr"
#endif
#ifdef CONFIG_DEBUG_MUTEXES
  ", dm"
#endif
  ;

#if (defined CONFIG_NLS | defined CONFIG_NLS_MODULE) & !defined UFSD_BUILTINT_UTF8
  #define UFSD_USE_NLS  "Use nls functions instead of builtin utf8 to convert strings"
#endif

#ifndef SLAB_MEM_SPREAD
  #define SLAB_MEM_SPREAD 0
#endif

#ifndef ACL_NOT_CACHED
 #define ACL_NOT_CACHED ((void *)(-1))
#endif

#if  !(defined HAVE_DECL_KMEM_CACHE_CREATE_V1 && HAVE_DECL_KMEM_CACHE_CREATE_V1)\
  && !(defined HAVE_DECL_KMEM_CACHE_CREATE_V2 && HAVE_DECL_KMEM_CACHE_CREATE_V2)\
  && !(defined HAVE_DECL_KMEM_CACHE_CREATE_V3 && HAVE_DECL_KMEM_CACHE_CREATE_V3)\
  && !(defined HAVE_DECL_KMEM_CACHE_CREATE_V4 && HAVE_DECL_KMEM_CACHE_CREATE_V4)
#error "Unknown version of kmem_cache_create"
#endif

//
// Implement missing functions and helper macroses to reduce chaos
//
#if !(defined HAVE_DECL_BLK_START_PLUG  && HAVE_DECL_BLK_START_PLUG)
  struct blk_plug{};
  static inline void blk_start_plug( struct blk_plug *plug ){}
  static inline void blk_finish_plug( struct blk_plug *plug ){}
#endif

#if defined HAVE_DECL_KMAP_ATOMIC_V1 && HAVE_DECL_KMAP_ATOMIC_V1
  #define atomic_kmap(p)    kmap_atomic( (p), KM_USER0 )
  #define atomic_kunmap(p)  kunmap_atomic( (p), KM_USER0 )
#else
  #define atomic_kmap(p)    kmap_atomic( (p) )
  #define atomic_kunmap(p)  kunmap_atomic( (p) )
#endif

#if defined HAVE_DECL_KMEM_CACHE_CREATE_V1 && HAVE_DECL_KMEM_CACHE_CREATE_V1
  #define Kmem_cache_create( n, s, f, i ) kmem_cache_create( n, s, 0, SLAB_RECLAIM_ACCOUNT|SLAB_PANIC | (f), i, NULL )
#else
  #define Kmem_cache_create( n, s, f, i ) kmem_cache_create( n, s, 0, SLAB_RECLAIM_ACCOUNT|SLAB_PANIC | (f), i )
#endif

#if !( defined HAVE_DECL_FILE_INODE && HAVE_DECL_FILE_INODE )
  #define file_inode(x) (x)->f_dentry->d_inode
#endif

#ifndef PAGE_CACHE_IDX
  #define PAGE_CACHE_IDX(addr)  (((addr)+PAGE_CACHE_SIZE-1)>>PAGE_CACHE_SHIFT)
#endif

#ifdef UFSD_TRACE
  #define lock_ufsd(s)     _lock_ufsd( s, __func__ )
  #define try_lock_ufsd(s)  _try_lock_ufsd( s, __func__ )
  #define unlock_ufsd(s)   _unlock_ufsd( s, __func__ )
  DEBUG_ONLY( static unsigned long WaitMutex; )
  static unsigned long StartJiffies;
#else
  #define lock_ufsd(s)     _lock_ufsd( s )
  #define try_lock_ufsd(s)  _try_lock_ufsd( s )
  #define unlock_ufsd(s)   _unlock_ufsd( s )
#endif

#ifdef UFSD_DEBUG
  #define ProfileEnter(s,name)    \
    s->name##_cnt += 1;           \
    s->name##_ticks -= jiffies

  #define ProfileLeave(s,name)    \
    s->name##_ticks += jiffies
#else
  #define ProfileEnter(s,name)
  #define ProfileLeave(s,name)
#endif

#if !(defined HAVE_DECL_CURRENT_UMASK && HAVE_DECL_CURRENT_UMASK)
  #define current_umask() (current->fs->umask)
#endif

#ifndef current_fsuid
  #define current_fsuid() (current->fsuid)
#endif

#ifndef current_fsgid
  #define current_fsgid() (current->fsgid)
#endif

#ifndef current_uid
  #define current_uid() (current->uid)
#endif

#ifndef current_gid
  #define current_gid() (current->gid)
#endif

#ifndef is_owner_or_cap
  #define is_owner_or_cap(i) ( ( uid_eq(current_fsuid(), (i)->i_uid) ) || capable(CAP_FOWNER) )
#endif

#if !(defined HAVE_DECL_SET_NLINK && HAVE_DECL_SET_NLINK)
static inline void set_nlink(struct inode *i, unsigned int nlink){ i->i_nlink = nlink; }
#endif
#if !(defined HAVE_DECL_DROP_NLINK && HAVE_DECL_DROP_NLINK)
static inline void drop_nlink(struct inode *i){ i->i_nlink--; }
#endif
#if !(defined HAVE_DECL_INC_NLINK && HAVE_DECL_INC_NLINK)
static inline void inc_nlink(struct inode *i){ i->i_nlink++; }
#endif

#if defined HAVE_STRUCT_MODULE_MODULE_CORE && HAVE_STRUCT_MODULE_MODULE_CORE
  #define UFSD_MODULE_CORE() __this_module.module_core
#elif defined HAVE_STRUCT_MODULE_MODULE_CORE_RX && HAVE_STRUCT_MODULE_MODULE_CORE_RX
  #define UFSD_MODULE_CORE() __this_module.module_core_rx
#else
  #define UFSD_MODULE_CORE() (void*)0
#endif

#if defined HAVE_DECL_TRY_TO_WRITEBACK_INODES_SB && HAVE_DECL_TRY_TO_WRITEBACK_INODES_SB
  #define Try_to_writeback_inodes_sb(s) try_to_writeback_inodes_sb( (s), WB_REASON_FREE_MORE_MEM )
#elif defined HAVE_DECL_WRITEBACK_INODES_SB_IF_IDLE_V1 && HAVE_DECL_WRITEBACK_INODES_SB_IF_IDLE_V1
  #define Try_to_writeback_inodes_sb(s) writeback_inodes_sb_if_idle( (s) )
#elif defined HAVE_DECL_WRITEBACK_INODES_SB_IF_IDLE_V2 && HAVE_DECL_WRITEBACK_INODES_SB_IF_IDLE_V2
  #define Try_to_writeback_inodes_sb(s) writeback_inodes_sb_if_idle( (s), WB_REASON_FREE_MORE_MEM )
#endif

// TODO: Check for field 'queue_limits::discard_granularity' instead of kernel version
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,33)
  #ifdef DISCARD_FL_WAIT
    #define Blkdev_issue_discard( d, s, n )  blkdev_issue_discard( d, s, n, GFP_NOFS, DISCARD_FL_WAIT|DISCARD_FL_BARRIER )
  #else
    #define Blkdev_issue_discard( d, s, n )  blkdev_issue_discard( d, s, n, GFP_NOFS, 0 )
  #endif
#endif

//
// This function returns UFSD's handle for 'inode'
//
// ufsd_file* UFSD_FH( IN struct inode *inode );
//
#define UFSD_FH(i)      (UFSD_U(i)->ufile)

#define UFSD_SB(sb)     ((usuper*)(sb)->s_fs_info)
#define UFSD_VOLUME(sb) UFSD_SB(sb)->ufsd

#define UFSD_SBI_FLAGS_ABORTED    0x00000001
#define UFSD_SBI_FLAGS_DISRCARD   0x00000002

//
// This function returns 'unode' for 'inode'
//
// struct unode* UFSD_U( IN struct inode* inode );
//
#define UFSD_U(inode)   (container_of((inode), struct unode, i))

//
// Private superblock structure.
// Stored in super_block.s_fs_info
//
typedef struct usuper {
    UINT64            max_block;
    UINT64            end_of_dir;                // End of directory
    ufsd_volume       *ufsd;
#if !(defined HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT && HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT)
    struct vfsmount   *vfs_mnt;
    char              mnt_buffer[32];
#endif
    unsigned long     flags;              // UFSD_SBI_FLAGS_XXX ...
    struct mutex      api_mutex;
    struct mutex      nocase_mutex;
    mount_options     options;
#ifdef UFSD_CHECK_BDI
    struct backing_dev_info *bdi;
#endif
#ifdef UFSD_USE_XATTR
    void              *x_buffer;
    size_t            bytes_per_xbuffer;
#endif

#ifdef UFSD_USE_FLUSH_THREAD
    rwlock_t            state_lock;        // Protect the various scalars
    wait_queue_head_t   wait_done_flush;
    wait_queue_head_t   wait_exit_flush;
    struct task_struct  *flush_task;       // Pointer to the current flush thread for this volume
    struct timer_list   flush_timer;       // The timer used to wakeup the flush thread
    unsigned char       exit_flush_timer;  // Used to exit from flush thread
    unsigned char       bdirty;
#endif
    unsigned long       last_dirty;

#if defined CONFIG_PROC_FS
    struct proc_dir_entry *procdir;
#endif
    TRACE_ONLY( struct sysinfo    sys_info; )
    spinlock_t        ddt_lock;     // do_delayed_tasks lock
    struct list_head  clear_list;   // List of inodes to clear

    unsigned int      ra_blocks;
#ifdef UFSD_NTFS2
    #define RW_BUFFER_SIZE  (4*PAGE_SIZE)
    void              *rw_buffer;    // RW_BUFFER_SIZE
#endif

    UINT64            cluster_mask_inv;   // ~(bytes_per_cluster-1)
    UINT64            cluster_mask;       // bytes_per_cluster-1
    unsigned int      bytes_per_cluster;
    unsigned int      discard_granularity;
    UINT64            discard_granularity_mask_inv; // ~(discard_granularity_mask_inv-1)

#ifdef UFSD_TRACE
    size_t            nDelClear;      // Delayed clear
    size_t            nWrittenBlocks; // Count of written blocks
    size_t            nReadBlocks;    // Count of read blocks
    size_t            nWrittenBlocksNa; // Count of written not aligned blocks
    size_t            nReadBlocksNa;    // Count of read not aligned blocks
    size_t            nPinBlocks;     // Count of pinned blocks
    size_t            nUnpinBlocks;   // Count of unpinned blocks
    size_t            nMappedBh;      // Count of mapped buffers
    size_t            nMappedMem;     // Count of mapped buffers
    size_t            nUnMapped;      // Count of mapped buffers
    size_t            nHashCalls;     // Count of ufsd_name_hash calls
    size_t            nCompareCalls;  // Count of ufsd_compare calls

#ifdef UFSD_DEBUG
    // Internal profiler
    size_t            bdread_cnt;
    size_t            bdread_ticks;
    size_t            bdwrite_cnt;
    size_t            bdwrite_ticks;
    size_t            bdmap_cnt;
    size_t            bdmap_ticks;
    size_t            bdsetdirty_cnt;
    size_t            bdsetdirty_ticks;
    size_t            write_begin_cnt;
    size_t            write_begin_ticks;
    size_t            write_end_cnt;
    size_t            write_end_ticks;
    size_t            write_inode_cnt;
    size_t            write_inode_ticks;
    size_t            readpage_cnt;
    size_t            readpage_ticks;
    size_t            readpages_cnt;
    size_t            readpages_ticks;
    size_t            do_readpage_cnt;
    size_t            do_readpage_ticks;
    size_t            buf_readpage_cnt;
    size_t            buf_readpage_ticks;
    size_t            writepage_cnt;
    size_t            writepage_ticks;
    size_t            writepages_cnt;
    size_t            writepages_ticks;
    size_t            do_writepage_cnt;
    size_t            do_writepage_ticks;
    size_t            buf_writepage_cnt;
    size_t            buf_writepage_ticks;
#endif
#endif

    atomic_t          VFlush;       // Need volume flush

} usuper;

typedef struct {
  struct list_head  list;
  ufsd_file         *file;
} delay_task;


#define UFSD_UNODE_FLAG_LAZY_OPEN_BIT   1
#define UFSD_UNODE_FLAG_LAZY_INIT_BIT   2
#define UFSD_UNODE_FLAG_SET_MODE_BIT    3   // mode of file is modified
#define UFSD_UNODE_FLAG_MODIFIED_BIT    4   // content of file is modified
#define UFSD_UNODE_FLAG_SPARSE_BIT      9   // file is sparsed
#define UFSD_UNODE_FLAG_COMPRESS_BIT    11  // file is compressed
#define UFSD_UNODE_FLAG_ENCRYPT_BIT     14  // file is encrypted
#define UFSD_UNODE_FLAG_EA_BIT          29  // file contains extended attributes

#define UFSD_UNODE_FLAG_SPARSE    (1<<UFSD_UNODE_FLAG_SPARSE_BIT)
#define UFSD_UNODE_FLAG_COMPRESS  (1<<UFSD_UNODE_FLAG_COMPRESS_BIT)
#define UFSD_UNODE_FLAG_ENCRYPT   (1<<UFSD_UNODE_FLAG_ENCRYPT_BIT)
#define UFSD_UNODE_FLAG_EA        (1<<UFSD_UNODE_FLAG_EA_BIT)

#define UFSD_UNODE_FLAG_API_FLAGS (UFSD_UNODE_FLAG_SPARSE | UFSD_UNODE_FLAG_COMPRESS | UFSD_UNODE_FLAG_ENCRYPT | UFSD_UNODE_FLAG_EA)

//
// In memory ufsd inode
//
typedef struct unode {
  rwlock_t            valid_lock;     // protect field 'valid'
  struct rw_semaphore size_sema;      // per file lock

#if defined UFSD_USE_XATTR && !(defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL)
  // inode does not contain cached values of acl/default_acl.  use own acl cache
  struct posix_acl *acl;
  struct posix_acl *default_acl;
#endif

  //
  // 'init_once' initialize members [0 - 'ufile')
  // 'ufsd_alloc_inode' resets members ['ufile' - 'i')
  //
  ufsd_file     *ufile;

  unsigned long flags;
  unsigned      ver;                  // changes

  unsigned      atime, ctime, mtime;  // saved on-disk times in seconds
  loff_t        valid;

  NTFS_ONLY( loff_t total_alloc; )    // total allocated for sparse files

  //
  // Vfs inode
  //
  struct inode  i;

} unode;


#ifdef UFSD_NTFS2
  static int is_sparsed( IN const unode *u ) { return FlagOn( u->flags, UFSD_UNODE_FLAG_SPARSE ); }
  static int is_compressd( IN const unode *u ) { return FlagOn( u->flags, UFSD_UNODE_FLAG_COMPRESS ); }
  static int is_sparsed_or_compressed( IN const unode *u ) { return FlagOn( u->flags, UFSD_UNODE_FLAG_SPARSE | UFSD_UNODE_FLAG_COMPRESS ); }
  static int is_encrypted( IN const unode *u ) { return FlagOn( u->flags, UFSD_UNODE_FLAG_ENCRYPT ); }
#else
  #define is_sparsed(u)  0
  #define is_compressd(u) 0
  #define is_sparsed_or_compressed(u) 0
  #define is_encrypted(u) 0
#endif

#if (defined UFSD_NTFS2 || defined UFSD_HFS) && defined UFSD_USE_XATTR
static int is_xattr( IN const unode *u ) {
  return FlagOn( u->flags, UFSD_UNODE_FLAG_EA );
}
#endif


//
// assert tv_sec is the first member of type time_t
//
typedef char AssertTvSecOff [0 == offsetof(struct timespec, tv_sec)? 1 : -1];
typedef char AssertTvSecSz [sizeof(time_t) == sizeof(((struct timespec*)NULL)->tv_sec)? 1 : -1];

#define TIMESPEC_SECONDS(t) (*(time_t*)(t))

#define _100ns2seconds        10000000UL
#define SecondsToStartOf1970  0x00000002B6109100ULL
// How many seconds since 1970 till 1980
#define Seconds1970To1980     0x12CEA600

#ifdef UFSD_USE_POSIX_TIME
///////////////////////////////////////////////////////////
// ufsd_time (GMT time)
//
// This function returns the number of seconds since 1970
///////////////////////////////////////////////////////////
UINT64 UFSDAPI_CALL
ufsd_time_posix( void )
{
  return get_seconds();
}
#endif


#ifdef UFSD_USE_NT_TIME
///////////////////////////////////////////////////////////
// ufsd_time (GMT time)
//
// This function returns the number of 100 nanoseconds since 1601
///////////////////////////////////////////////////////////
UINT64 UFSDAPI_CALL
ufsd_time_nt( void )
{
  time_t sec = get_seconds();
  // 10^7 units of 100 nanoseconds in one second
  UINT64 NtTime = _100ns2seconds * (sec + SecondsToStartOf1970);
#ifdef UFSD_PROFILE
  // Internal profiler uses this function to measure
  // time differences. The resolution of 1 second seems is too poor
  NtTime += CURRENT_TIME.tv_nsec/100;
#endif
  return NtTime;
}

///////////////////////////////////////////////////////////
// nttime2posix
//
// Convert Nt time (100 nseconds from 1601) to posix time (seconds from 1970)
///////////////////////////////////////////////////////////
static inline time_t
nttime2posix(
    IN UINT64 NtTime
    )
{
#if defined UFSD_USE_ASM_DIV64
  UINT64 seconds = NtTime;
  // WARNING: do_div changes its first argument(!)
  (void)do_div( seconds, _100ns2seconds );
  seconds -= SecondsToStartOf1970;
#else
  UINT64 seconds = NtTime / _100ns2seconds - SecondsToStartOf1970;
#endif

  assert( seconds > 0 );

  return (time_t)seconds;
}
#endif


#ifdef UFSD_EXFAT
//
// This variable is used to get the bias
//
extern struct timezone sys_tz;

///////////////////////////////////////////////////////////
// ufsd_bias
//
// Returns minutes west of Greenwich
///////////////////////////////////////////////////////////
int UFSDAPI_CALL
ufsd_bias( void )
{
  return sys_tz.tz_minuteswest;
}
#endif


///////////////////////////////////////////////////////////
// ufsd_times_to_inode
//
//
///////////////////////////////////////////////////////////
static inline void
ufsd_times_to_inode(
    IN usuper       *sbi,
    IN unode        *u,
    IN struct inode *i,
    IN const finfo  *fi
    )
{
#if defined UFSD_USE_POSIX_TIME && defined UFSD_USE_NT_TIME
  if ( sbi->options.posixtime ){
    u->atime = TIMESPEC_SECONDS( &i->i_atime )  = fi->ReffTime;
    u->ctime = TIMESPEC_SECONDS( &i->i_ctime )  = fi->ChangeTime;
    u->mtime = TIMESPEC_SECONDS( &i->i_mtime )  = fi->ModiffTime;
  } else {
    u->atime = TIMESPEC_SECONDS( &i->i_atime )  = nttime2posix( fi->ReffTime );
    u->ctime = TIMESPEC_SECONDS( &i->i_ctime )  = nttime2posix( fi->ChangeTime );
    u->mtime = TIMESPEC_SECONDS( &i->i_mtime )  = nttime2posix( fi->ModiffTime );
  }
#elif defined UFSD_USE_POSIX_TIME
  u->atime = TIMESPEC_SECONDS( &i->i_atime )  = fi->ReffTime;
  u->ctime = TIMESPEC_SECONDS( &i->i_ctime )  = fi->ChangeTime;
  u->mtime = TIMESPEC_SECONDS( &i->i_mtime )  = fi->ModiffTime;
#else
  u->atime = TIMESPEC_SECONDS( &i->i_atime )  = nttime2posix( fi->ReffTime );
  u->ctime = TIMESPEC_SECONDS( &i->i_ctime )  = nttime2posix( fi->ChangeTime );
  u->mtime = TIMESPEC_SECONDS( &i->i_mtime )  = nttime2posix( fi->ModiffTime );
#endif
}


///////////////////////////////////////////////////////////
// ufsd_printk
//
// Used to show different messages (errors and warnings)
///////////////////////////////////////////////////////////
void UFSDAPI_CALLv
ufsd_printk(
    IN struct super_block  *sb,
    IN const char *fmt, ...
    )
{
  va_list va;
  va_start( va, fmt );
//  assert( '\n' == fmt[strlen(fmt)-1] );

#ifndef UFSD_DEBUG
  if ( printk_ratelimit() )
#endif
  {
#if defined HAVE_STRUCT_VA_FORMAT && HAVE_STRUCT_VA_FORMAT
    struct va_format vaf;
    vaf.fmt = fmt;
    vaf.va  = &va;
    printk( KERN_CRIT QUOTED_UFSD_DEVICE ": \"%s\" (%s): %pV", current->comm, NULL == sb? "" : sb->s_id, &vaf );
#else
    static char buf[100];
    static DEFINE_SPINLOCK( s_printk_spin );
    spin_lock( &s_printk_spin );
    vsnprintf( buf, sizeof(buf), fmt, va );
    printk( KERN_ERR QUOTED_UFSD_DEVICE ": \"%s\" (%s): %s", current->comm, NULL == sb? "" : sb->s_id, buf );
    spin_unlock( &s_printk_spin );
#endif
  }

  va_end( va );
}


//
// Memory allocation routines.
// Debug version of memory allocation/deallocation routine performs
// detection of memory leak/overwrite
//
#if defined UFSD_DEBUG_ALLOC

typedef struct memblock_head {
    struct list_head Link;
    unsigned int  asize;
    unsigned int  seq;
    unsigned int  size;
    unsigned char barrier[64 - 3*sizeof(int) - sizeof(struct list_head)];

  /*
     offset  0x40
     |---------------------|
     | Requested memory of |
     |   size 'DataSize'   |
     |---------------------|
  */
  //unsigned char barrier2[64 - 3*sizeof(int) - sizeof(struct list_head)];

} memblock_head;

static size_t TotalKmallocs;
static size_t TotalVmallocs;
static size_t UsedMemMax;
static size_t TotalAllocs;
static size_t TotalAllocBlocks;
static size_t TotalAllocSequence;
static size_t MemMaxRequest;
static LIST_HEAD(TotalAllocHead);
static struct mutex MemMutex;


///////////////////////////////////////////////////////////
// trace_mem_report
//
// Helper function to trace memory usage information
///////////////////////////////////////////////////////////
static void
trace_mem_report(
    IN int OnExit
    )
{
  size_t Mb = UsedMemMax/(1024*1024);
  size_t Kb = (UsedMemMax%(1024*1024)) / 1024;
  size_t b  = UsedMemMax%1024;
  unsigned long level = OnExit? UFSD_LEVEL_ERROR : Dbg;

  if ( 0 != Mb ) {
    DebugTrace( 0, level, ("Memory report: Peak usage %Zu.%03Zu Mb (%Zu bytes), kmalloc %Zu, vmalloc %Zu\n",
                  Mb, Kb, UsedMemMax, TotalKmallocs, TotalVmallocs ) );
  } else {
    DebugTrace( 0, level, ("Memory report: Peak usage %Zu.%03Zu Kb (%Zu bytes),  kmalloc %Zu, vmalloc %Zu\n",
                  Kb, b, UsedMemMax, TotalKmallocs, TotalVmallocs ) );
  }
  DebugTrace( 0, level, ("%s:  %Zu bytes in %Zu blocks, Max request %Zu bytes\n",
                OnExit? "Leak":"Total allocated", TotalAllocs, TotalAllocBlocks, MemMaxRequest ) );
}


///////////////////////////////////////////////////////////
// ufsd_make_tag_string
//
// local debug support routine.
///////////////////////////////////////////////////////////
static const unsigned char*
ufsd_make_tag_string(
    IN const unsigned char  *p,
    OUT unsigned char       *tag
    )
{
  int i;
  for (i = 0; i < 8; i++, p++)
    tag[i] = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == ' '
      ? *p : '.';
  tag[8] = 0;
  return tag;
}


///////////////////////////////////////////////////////////
// ufsd_heap_alloc
//
// Debug version of memory allocation routine
// NOTE: __GFP_ZERO passed in kmalloc/vmalloc does not zero memory at least in kernels up to 2.6.23 (?)
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_heap_alloc(
    IN unsigned long size,
    IN int    zero
    )
{
  memblock_head *head;
  int use_kmalloc;
  // Overhead includes private information and two barriers to check overwriting
  size_t asize = size + sizeof(memblock_head) + sizeof(head->barrier);

  if ( asize <= PAGE_SIZE ) {
    use_kmalloc = 1;
    // size_t align
    asize = (asize + sizeof(size_t)-1) & ~(sizeof(size_t)-1);
    head  = kmalloc( asize, GFP_NOFS );
  } else {
    use_kmalloc = 0;
    asize = PAGE_ALIGN(asize);
    head  = vmalloc( asize );
    assert( (size_t)head >= VMALLOC_START && (size_t)head < VMALLOC_END );
#ifdef UFSD_DEBUG
    if ( (size_t)head < VMALLOC_START || (size_t)head >= VMALLOC_END )
      _UFSDTrace( "vmalloc(%Zu) returns %p. Must be in range [%lx, %lx)\n", asize, head, (long)VMALLOC_START, (long)VMALLOC_END );
#endif
  }

  assert(NULL != head);
  if ( NULL == head ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("HeapAlloc(%lu) failed\n", size));
    return NULL;
  }
  assert(0 == (asize & 1U));

  mutex_lock( &MemMutex );

  // Fill head private fields
  head->asize = use_kmalloc? asize : (asize | 1);
  head->size  = size;
  list_add( &head->Link, &TotalAllocHead );
  head->seq   = ++TotalAllocSequence;

  //
  // fills two barriers to check memory overwriting
  //
  memset( &head->barrier[0], 0xde, sizeof(head->barrier) );
  if ( zero )
    memset( head + 1, 0, size );
  memset( Add2Ptr( head + 1, size), 0xed, sizeof(head->barrier) );

  //
  // Update statistics
  //
  use_kmalloc? ++TotalKmallocs : ++TotalVmallocs;
  TotalAllocs    += size;
  if( TotalAllocs > UsedMemMax )
    UsedMemMax = TotalAllocs;
  TotalAllocBlocks += 1;
  if ( size > MemMaxRequest )
    MemMaxRequest = size;

  DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("alloc(%lu) -> %p%s, seq=%u\n",
                size, head+1, use_kmalloc? "" : "(v)", head->seq));

  mutex_unlock( &MemMutex );
  return head + 1;
}


///////////////////////////////////////////////////////////
// ufsd_heap_free
//
// Debug version of memory deallocation routine
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_heap_free(
    IN void *p
    )
{
  memblock_head *block;

  if ( NULL == p )
    return;

  mutex_lock( &MemMutex );

#if 1
  // Fast but unsafe find
  block = (memblock_head*)p - 1;
#else
  // Safe but slow find
  {
    struct list_head  *pos;
    list_for_each( pos, &TotalAllocHead )
    {
      block = list_entry( pos, memblock_head, Link );
      if ( p == (void*)(block + 1) )
        goto Found;
    }
  }
  assert( !"failed to find block" );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("HeapFree(%p) failed to find block\n", p ));
  mutex_unlock( &MemMutex );
  return;
Found:
#endif

  // Verify barrier
  {
    unsigned char *p;
    size_t i  = sizeof(block->barrier);
    char *err = NULL;
    for ( p = &block->barrier[0]; 0 != i; p++, i-- ) {
      if ( *p != 0xde ) {
        err = "head";
BadNews:
        DebugTrace( 0, UFSD_LEVEL_ERROR, ("**** Allocated %u seq %u DataSize %u\n",
                   block->asize, block->seq, block->size ));
        DebugTrace( 0, UFSD_LEVEL_ERROR, ("**** HeapFree(%p) %s barrier failed at 0x%Zx\n", p, err, PtrOffset( block, p ) ));
        TRACE_ONLY( ufsd_trace_level = -1; )
        TRACE_ONLY( ufsdapi_dump_memory( block, 512 ); )
        TRACE_ONLY( ufsd_trace_level = 0; )
        BUG_ON(1);
      }
    }

    i = sizeof(block->barrier);
    for ( p = Add2Ptr( block + 1, block->size ); 0 != i; p++, i-- ) {
      if ( *p != 0xed ) {
        err = "tail";
        goto BadNews;
      }
    }
  }

  list_del( &block->Link );

  //
  // Update statistics
  //
  TotalAllocs -= block->size;
  TotalAllocBlocks -= 1;
  mutex_unlock( &MemMutex );
  DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("free(%p, %u) seq=%u\n", block + 1, block->size, block->seq));

  memset( block + 1, 0xcc, block->size );

  // declaration of vfree and kfree differs!
  if ( block->asize & 1U )
    vfree( block );
  else
    kfree( block );
}

#else

///////////////////////////////////////////////////////////
// ufsd_heap_alloc
//
// Release version of memory allocation routine
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_heap_alloc(
    IN unsigned long  size,
    IN int  zero
    )
{
  void *ptr;

  // NOTE: __GFP_ZERO passed in kmalloc/vmalloc does not zero memory at least in kernels up to 2.6.23 (?)
  // PAGE_KERNEL - G.P.L. for mips
#ifndef __mips__
  if ( size <= PAGE_SIZE ) {
    ptr = kmalloc( size, zero?(GFP_NOFS|__GFP_ZERO) : GFP_NOFS );
  } else {
    ptr = __vmalloc( size, zero?(GFP_KERNEL|__GFP_ZERO) : GFP_KERNEL, PAGE_KERNEL );
    assert( (size_t)ptr >= VMALLOC_START && (size_t)ptr < VMALLOC_END );
  }
  if ( NULL != ptr ) {
    DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("alloc(%lu) -> %p%s\n", size, ptr, size <= PAGE_SIZE?"" : "(v)" ));
    return ptr;
  }
#else
  if ( size <= PAGE_SIZE ) {
    ptr = kmalloc( size, GFP_NOFS );
  } else {
    ptr = vmalloc( size );
    assert( (size_t)ptr >= VMALLOC_START && (size_t)ptr < VMALLOC_END );
  }
  if ( NULL != ptr ) {
    if ( zero )
      memset( ptr, 0, size );
    DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("alloc(%lu) -> %p%s\n", size, ptr, size <= PAGE_SIZE?"" : "(v)" ));
    return ptr;
  }
#endif

  assert( !"no memory" );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("alloc(%lu) failed\n", size));
  return NULL;
}


///////////////////////////////////////////////////////////
// ufsd_heap_free
//
// Release version of memory deallocation routine
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_heap_free(
    IN void *ptr
    )
{
  if ( NULL != ptr ) {
    DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("HeapFree(%p)\n", ptr));
    if ( (size_t)ptr >= VMALLOC_START && (size_t)ptr < VMALLOC_END ) {
      // This memory was allocated via vmalloc
      vfree( ptr );
    } else {
      // This memory was allocated via kmalloc
      kfree( ptr );
    }
  }
}

#endif // #ifndef UFSD_DEBUG_ALLOC


#if defined UFSD_HFS || defined UFSD_EXFAT
//
// Use 'kmem_cache_create' for 2.6.30+
//
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,30)
  #define UFSD_USE_KMEM_CACHE
#endif

///////////////////////////////////////////////////////////
// ufsd_cache_create
//
//
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_cache_create(
    IN const char *Name,
    IN unsigned   size
    )
{
#ifdef UFSD_USE_KMEM_CACHE
  struct kmem_cache *cache = Kmem_cache_create( Name, size, SLAB_MEM_SPREAD, NULL );
#else
  void *cache = (void*)(size_t)size;
#endif

  DebugTrace( 0, Dbg, ("Cache create: \"%s\" (%x) -> %p\n", Name, size, cache ) );
  return cache;
}


///////////////////////////////////////////////////////////
// ufsd_cache_destroy
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_cache_destroy(
    IN void *Cache
    )
{
  DebugTrace( 0, Dbg, ("Cache destroy: %p \n", Cache ) );
#ifdef UFSD_USE_KMEM_CACHE
  kmem_cache_destroy( Cache );
#endif
}


///////////////////////////////////////////////////////////
// ufsd_cache_alloc
//
//
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_cache_alloc(
    IN void *Cache,
    IN int  bZero
    )
{
#ifdef UFSD_USE_KMEM_CACHE
  void  *p = kmem_cache_alloc( Cache, bZero? (__GFP_ZERO | GFP_KERNEL) : GFP_KERNEL );
#else
  void  *p = kmalloc( (size_t)Cache, GFP_KERNEL );
  if ( NULL != p && bZero )
    memset( p, 0, (size_t)Cache );
#endif
  DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("CacheAlloc(%p)->%p\n", Cache, p ) );
  return p;
}


///////////////////////////////////////////////////////////
// ufsd_cache_free
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_cache_free(
    IN void *Cache,
    IN void *p
    )
{
  DebugTrace( 0, UFSD_LEVEL_MEMMNGR, ("CacheFree(%p,%p)\n", Cache, p ) );
#ifdef UFSD_USE_KMEM_CACHE
  kmem_cache_free( Cache, p );
#else
  kfree( p );
#endif
}
#endif // #if defined UFSD_HFS || defined UFSD_EXFAT


#if defined UFSD_NTFS2 || defined UFSD_EXFAT || defined UFSD_REFS
//
// Shared memory struct.
// Used to share memory between volumes
//
static DEFINE_SPINLOCK( s_shared_lock );

struct {
  void      *ptr;
  unsigned  len;
  int       cnt;
} s_shared[8];


///////////////////////////////////////////////////////////
// ufsd_set_shared
//
// Returns 'ptr' if pointer was saved in shared memory
// Returns not NULL if pointer was shared
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_set_shared(
    IN void     *ptr,
    IN unsigned bytes
    )
{
  void  *ret = NULL;
  int i, j = -1;

  spin_lock( &s_shared_lock );
  for ( i = 0; i < ARRAY_SIZE(s_shared); i++ ) {
    if ( 0 == s_shared[i].cnt )
      j = i;
    else if ( bytes == s_shared[i].len && 0 == memcmp ( s_shared[i].ptr, ptr, bytes ) ) {
      s_shared[i].cnt += 1;
      ret = s_shared[i].ptr;
      break;
    }
  }

  if ( NULL == ret && -1 != j ) {
    s_shared[j].ptr = ptr;
    s_shared[j].len = bytes;
    s_shared[j].cnt = 1;
    ret = ptr;
  }
  spin_unlock( &s_shared_lock );

  DebugTrace( 0, Dbg, ("set_shared(%p,%x) => %p\n",  ptr, bytes, ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_put_shared
//
// Returns 'ptr' if pointer is not shared anymore
// Returns NULL if pointer is still shared
///////////////////////////////////////////////////////////
void*
UFSDAPI_CALL
ufsd_put_shared(
    IN void *ptr
    )
{
  void  *ret = ptr;
  int i;

  spin_lock( &s_shared_lock );
  for ( i = 0; i < ARRAY_SIZE(s_shared); i++ ) {
    if ( s_shared[i].ptr == ptr ) {
      if ( 0 != --s_shared[i].cnt )
        ret = NULL;
      break;
    }
  }
  spin_unlock( &s_shared_lock );

  DebugTrace( 0, Dbg, ("put_shared (%p) => %p\n",  ptr, ret ));
  return ret;
}
#endif // #if defined UFSD_NTFS2 || defined UFSD_EXFAT


//
// NLS support routines requiring
// access to kernel-dependent nls_table structure.
//

///////////////////////////////////////////////////////////
// ufsd_char2uni
//
// Converts multibyte string to UNICODE string
// Returns the length of destination string in wide symbols
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_char2uni(
    OUT unsigned short      *ws,        // Destination UNICODE string
    IN  int                 max_out,    // Maximum UNICODE characters in ws
    IN  const unsigned char *s,         // Source BCS string
    IN  int                 len,        // The length of BCS strings in bytes
    IN  struct nls_table    *nls        // Code pages
    )
{
#ifdef UFSD_USE_NLS
  int ret   = 0;
  int len0  = len;

  for ( ;; ) {

    int charlen;
    wchar_t wc;

    if ( len <= 0 || 0 == *s )
      return ret; // The only correct way to exit

    if ( max_out <= 0 ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("A2U: too little output buffer\n" ) );
      return ret;
    }

    wc      = *ws;
    charlen = nls->char2uni( s, len, &wc );

    if ( charlen <= 0 ){
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("char2uni (%s) failed:\n", nls->charset ) );
      printk( KERN_NOTICE  QUOTED_UFSD_DEVICE": %s failed to convert '%.*s' to unicode. Pos %d, chars %x %x %x\n",
              nls->charset, len0, s - (len0-len), len0-len, (int)s[0], len > 1? (int)s[1] : 0, len > 2? (int)s[2] : 0 );
      return 0;
    }

    *ws++    = (unsigned short)wc;
    ret    += 1;
    max_out -= 1;
    len     -= charlen;
    s       += charlen;
  }

#else

  *ws = 0;
  return 0;

#endif
}


///////////////////////////////////////////////////////////
// ufsd_uni2char
//
// Converts UNICODE string to multibyte
// Returns the length of destination string in chars
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_uni2char(
    OUT unsigned char         *s,         // Destination BCS string
    IN  int                   max_out,    // Maximum bytes in BCS string
    IN  const unsigned short  *ws,        // Source UNICODE string
    IN  int                   len,        // The length of UNICODE string
    IN  struct nls_table      *nls        // Code pages
   )
{
#ifdef UFSD_USE_NLS
  unsigned char *s0 = s;

  for ( ;; ) {

    int charlen;

    if ( len <= 0 || 0 == *ws )
      return (int)(s - s0); // The only correct way to exit

    if ( max_out <= 0 ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("U2A: too little output buffer\n" ) );
      return (int)(s - s0);
    }

    charlen = nls->uni2char( *ws, s, max_out );
    if ( charlen <= 0 ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("uni2char (%s) failed:\n", nls->charset ));
      assert( !"U2A: failed to convert" );
      printk( KERN_NOTICE  QUOTED_UFSD_DEVICE": %s failed to convert from unicode. Pos %d, chars %x %x %x\n",
              nls->charset, (int)(s-s0), (unsigned)ws[0], len > 1? (unsigned)ws[1] : 0, len > 2? (unsigned)ws[2] : 0 );
      return 0;
    }

    ws      += 1;
    len     -= 1;
    max_out -= charlen;
    s       += charlen;
  }

#else

  *s = 0;
  return 0;

#endif
}


#ifdef UFSD_USE_NLS
///////////////////////////////////////////////////////////
// ufsd_uload_nls
//
//
///////////////////////////////////////////////////////////
static void
ufsd_uload_nls(
    IN mount_options  *opts
    )
{
  int cp;
  for ( cp = 0; cp < opts->nls_count; cp++ ){
    if ( NULL != opts->nls[cp] )
      unload_nls( opts->nls[cp] );
    opts->nls[cp] = NULL;
  }
  opts->nls_count = 0;
}
#else
  #define ufsd_uload_nls( o )
#endif // #ifdef UFSD_USE_NLS


//
// Device IO functions.
//
#if !(defined HAVE_DECL_SET_BUFFER_ORDERED & HAVE_DECL_SET_BUFFER_ORDERED)
  #define set_buffer_ordered( bh )
  #define clear_buffer_ordered( bh )
#endif

#ifdef UFSD_HFS
///////////////////////////////////////////////////////////
// bh_tail
//
// Get buffer_head for tail
///////////////////////////////////////////////////////////
struct buffer_head*
bh_tail(
    IN struct super_block *sb,
    IN size_t              bytes2skip
    )
{
  struct buffer_head *bh;
  usuper *sbi = UFSD_SB( sb );
  sector_t TailBlock = ((sbi->max_block << sb->s_blocksize_bits) + bytes2skip) >> 9;
  struct page *page = alloc_page( GFP_KERNEL | __GFP_ZERO );
  if ( NULL == page )
    return NULL;
  bh = alloc_buffer_head( GFP_NOFS );
  if ( NULL == bh ) {
out:
    __free_page( page );
    return NULL;
  }

  bh->b_state = 0;
  init_buffer( bh, end_buffer_read_sync, NULL );
  atomic_set( &bh->b_count, 2 );
  set_bh_page( bh, page, bytes2skip );
  bh->b_size    = 512;
  bh->b_bdev    = sb->s_bdev;
  bh->b_blocknr = TailBlock;
  set_buffer_mapped( bh );
  lock_buffer( bh );
  submit_bh( READ, bh );
  wait_on_buffer( bh );
  if ( !buffer_uptodate( bh ) ) {
    brelse( bh );
    goto out;
  }

  assert( 1 == atomic_read( &bh->b_count ) );
//    DebugTrace( 0, 0, ("bh_tail\n"));
  get_bh( bh );
  return bh;
}
#endif


///////////////////////////////////////////////////////////
// ufsd_bd_read_ahead
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_read_ahead(
    IN struct super_block   *sb,
    IN  unsigned long long  offset,
    IN  unsigned  bytes
   )
{
  //
  // NOTE: sb->s_blocksize == block_size(sb->s_bdev)
  //
  usuper    *sbi    = UFSD_SB( sb );
  sector_t  RaBlock = offset >> sb->s_blocksize_bits;
  // do not align on block size 'cause it is already aligned
  size_t    nBlocks = bytes >> sb->s_blocksize_bits;

  DebugTrace( 0, UFSD_LEVEL_IO, ("BdRead_ahead: %s, [%"PSCT"x, %"PSCT"x)\n", ufsd_bd_name( sb ), RaBlock, RaBlock + nBlocks ));

  if ( RaBlock >= sbi->max_block )
    return;

  if ( RaBlock + nBlocks > sbi->max_block )
    nBlocks = sbi->max_block - RaBlock;

  while( 0 != nBlocks-- )
    __breadahead( sb->s_bdev, RaBlock++, sb->s_blocksize );
}


#if 1
///////////////////////////////////////////////////////////
// ufsd_bd_unmap_meta
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_unmap_meta(
    IN struct super_block *sb,
    IN unsigned long long offset,
    IN unsigned long long bytes
    )
{
  sector_t  devblock  = offset >> sb->s_blocksize_bits;
  size_t  nBlocks     = bytes >> sb->s_blocksize_bits;

  DebugTrace( 0, UFSD_LEVEL_IO, ("unmap_meta: %s, [%"PSCT"x + %Zx)\n", ufsd_bd_name( sb ), devblock, nBlocks ));

  while( 0 != nBlocks-- ) {
    unmap_underlying_metadata( sb->s_bdev, devblock++ );
  }
}
#endif


///////////////////////////////////////////////////////////
// ufsd_bd_read
//
// Read data from block device
///////////////////////////////////////////////////////////
unsigned
UFSDAPI_CALL
ufsd_bd_read(
    IN  struct super_block *sb,
    IN  UINT64  offset,
    IN  size_t  bytes,
    OUT void    *buffer
   )
{
  //
  // NOTE: sb->s_blocksize == block_size(sb->s_bdev)
  //
  usuper    *sbi        = UFSD_SB( sb );
  sector_t  devblock    = offset >> sb->s_blocksize_bits;
  size_t    bytes2skip  = ((size_t)offset) & (sb->s_blocksize - 1); // offset % sb->s_blocksize

  int ReadAhead     = 0;
  int RaBlocks      = sbi->ra_blocks;
  sector_t RaBlock  = devblock;
  unsigned err      = 0;

  DebugTrace( +1, UFSD_LEVEL_IO, ("BdRead: %s, %"PSCT"x, %Zx, %p\n", ufsd_bd_name( sb ), devblock, bytes, buffer));

  ProfileEnter( sbi, bdread );

  while ( 0 != bytes ) {

    size_t ToRead;
    struct buffer_head *bh;

    while( ReadAhead++ < RaBlocks && RaBlock < sbi->max_block )
      __breadahead( sb->s_bdev, RaBlock++, sb->s_blocksize );
    ReadAhead -= 1;

#ifdef UFSD_HFS
    if ( devblock == sbi->max_block ) {
      assert( 512 == bytes );
      bh = bh_tail( sb, bytes2skip );
      bytes2skip = 0;
    } else
#endif
    {
      TRACE_ONLY( if ( 0 != bytes2skip || bytes < sb->s_blocksize ) sbi->nReadBlocksNa += 1; )

      bh = __bread( sb->s_bdev, devblock, sb->s_blocksize );
    }

    if ( NULL == bh ) {
      assert( !"BdRead: failed to map block" );
      printk( KERN_CRIT QUOTED_UFSD_DEVICE ":failed to read block 0x%"PSCT"x (max=%llx)\n", devblock, sbi->max_block);
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("BdRead: failed to map block starting from %"PSCT"x, %llx\n", devblock, sbi->max_block));
      err = -EIO;
      goto out;
    }

    TRACE_ONLY( sbi->nReadBlocks += 1; )

    ToRead = sb->s_blocksize - bytes2skip;
    if ( ToRead > bytes )
      ToRead = bytes;

#if !defined UFSD_AVOID_COPY_PAGE && defined HAVE_DECL_COPY_PAGE && HAVE_DECL_COPY_PAGE
    if ( likely( PAGE_SIZE == ToRead ) ) {
      assert( 0 == bytes2skip );
      assert( 0 == ((size_t)buffer & 0x3f) );
      copy_page( buffer, bh->b_data );
    }
    else
#endif
      memcpy( buffer, bh->b_data + bytes2skip, ToRead );

    __brelse( bh );

    buffer      = Add2Ptr( buffer, ToRead );
    devblock   += 1;
    bytes      -= ToRead;
    bytes2skip  = 0;
  }

out:
  ProfileLeave( sbi, bdread );

#ifdef UFSD_TRACE
  if ( ufsd_trace_level & UFSD_LEVEL_IO )
    ufsd_trace_inc( -1 );
#endif
//  DebugTrace( -1, UFSD_LEVEL_IO, ("BdRead -> ok\n"));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_bd_sync
//
//
///////////////////////////////////////////////////////////
static inline int
ufsd_bd_sync(
    IN struct super_block *sb,
    IN struct buffer_head *bh,
    IN size_t wait
    )
{
  int err = 0;
  if ( wait ) {
#ifdef WRITE_FLUSH_FUA
    // 2.6.37+
    if ( wait & UFSD_RW_WAIT_BARRIER ) {
      err = __sync_dirty_buffer( bh, WRITE_SYNC | WRITE_FLUSH_FUA );
      if ( 0 != err ) {
        DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdsync: \"__sync_dirty_buffer( bh, WRITE_SYNC | WRITE_FLUSH_FUA )\" failed -> %d\n", err ));
      }
    } else {
      err = sync_dirty_buffer( bh );
      if ( 0 != err ) {
        DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdsync: \"sync_dirty_buffer( bh )\" failed -> %d\n", err ));
      }
    }
#elif defined WRITE_BARRIER
    if ( wait & UFSD_RW_WAIT_BARRIER )
      set_buffer_ordered( bh );

    err = sync_dirty_buffer( bh );

    if ( wait & UFSD_RW_WAIT_BARRIER )
      clear_buffer_ordered( bh );

    if ( 0 != err ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdsync: %s \"sync_dirty_buffer( bh )\" failed -> %d\n", wait & UFSD_RW_WAIT_BARRIER? "ordered":"noordered", err ));
    }
#else
    ll_rw_block( WRITE, 1, &bh ); // TODO: do SG IO.
    wait_on_buffer( bh );
#endif
  }

#ifdef WRITE_BARRIER
  if ( -EOPNOTSUPP == err && (wait & UFSD_RW_WAIT_BARRIER) ) {
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": disabling barriers on %s - not supported\n", ufsd_bd_name( sb ) );
    UFSD_SB( sb )->options.nobarrier = 1;

    // And try again, without the barrier
    set_buffer_uptodate( bh );
    set_buffer_dirty( bh );
    err = sync_dirty_buffer( bh );

    if ( 0 != err ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdsync: nobarrier \"sync_dirty_buffer( bh )\" failed -> %d\n", err ));
    }
  }
#endif

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_bd_write
//
// Write data to block device
///////////////////////////////////////////////////////////
unsigned
UFSDAPI_CALL
ufsd_bd_write(
    IN struct super_block *sb,
    IN UINT64       offset,
    IN size_t       bytes,
    IN const void   *buffer,
    IN size_t       wait
   )
{
  //
  // NOTE: sb->s_blocksize == block_size(sb->s_bdev)
  //
  usuper    *sbi        = UFSD_SB( sb );
  sector_t  devblock    = offset >> sb->s_blocksize_bits;
  size_t    bytes2skip  = ((size_t)offset) & (sb->s_blocksize - 1); // offset % sb->s_blocksize
  unsigned  err         = 0;
  if ( !wait && FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    wait = UFSD_RW_WAIT_SYNC;

  DebugTrace( +1, UFSD_LEVEL_IO, ("bdwrite: %s, %"PSCT"x, %Zx, %p%s\n", ufsd_bd_name( sb ), devblock, bytes, buffer, wait?", wait":""));

  ProfileEnter( sbi, bdwrite );

  while ( 0 != bytes ) {

    size_t towrite;
    struct buffer_head *bh;

#ifdef UFSD_HFS
    if ( devblock == sbi->max_block ) {
      assert( bytes == 512 );
      bh = bh_tail( sb, bytes2skip );
      bytes2skip = 0;
    } else
#endif
    {
      TRACE_ONLY( if ( 0 != bytes2skip || bytes < sb->s_blocksize ) sbi->nWrittenBlocksNa += 1; )

      bh = ( 0 != bytes2skip || bytes < sb->s_blocksize ? __bread : __getblk )( sb->s_bdev, devblock, sb->s_blocksize );
    }

    if ( NULL == bh ) {
      assert( !"bdwrite: failed to map block" );
      printk( KERN_CRIT QUOTED_UFSD_DEVICE ":failed to write block 0x%"PSCT"x (max=%llx)\n", devblock, sbi->max_block );
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdwrite: failed to map block starting from %"PSCT"x, %llx\n", devblock, sbi->max_block));
      err = -EIO;
      goto out;
    }

    if ( buffer_locked( bh ) )
      __wait_on_buffer( bh );

    towrite = sb->s_blocksize - bytes2skip;
    if ( towrite > bytes )
      towrite = bytes;

    //
    // Update buffer with user data
    //
    lock_buffer( bh );
#if !defined UFSD_AVOID_COPY_PAGE && defined HAVE_DECL_COPY_PAGE && HAVE_DECL_COPY_PAGE
    if ( likely( PAGE_SIZE == towrite ) ) {
      assert( 0 == bytes2skip );
      assert( 0 == ((size_t)buffer & 0x3f) );
      copy_page( bh->b_data, (void*)buffer ); // copy_page requires source page as non const!
    }
    else
#endif
      memcpy( bh->b_data + bytes2skip, buffer, towrite );
    buffer  = Add2Ptr( buffer, towrite );

    set_buffer_uptodate( bh );
    mark_buffer_dirty( bh );
    unlock_buffer( bh );

    TRACE_ONLY( sbi->nWrittenBlocks += 1; )

    if ( wait ) {
#ifdef UFSD_DEBUG
      if ( !(ufsd_trace_level & UFSD_LEVEL_IO) )
        DebugTrace( 0, UFSD_LEVEL_VFS, ("bdwrite(wait), bh=%"PSCT"x\n", devblock));
#endif

      if ( sbi->options.nobarrier )
        wait &= ~UFSD_RW_WAIT_BARRIER;

      err = ufsd_bd_sync( sb, bh, wait );

      if ( 0 != err ) {
        assert( !"bdwrite: failed to write block" );
        DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdwrite: failed to write block starting from %"PSCT"x, %llx, error=%d\n", devblock, sbi->max_block, err));
        __brelse( bh );
        goto out;
      }
    }

    __brelse( bh );

    devblock    += 1;
    bytes       -= towrite;
    bytes2skip   = 0;
  }

out:
  ProfileLeave( sbi, bdwrite );

#ifdef UFSD_TRACE
  if ( ufsd_trace_level & UFSD_LEVEL_IO )
    ufsd_trace_inc( -1 );
#endif
//  DebugTrace( -1, UFSD_LEVEL_IO, ("BdWrite -> ok\n"));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_bd_map
//
//
///////////////////////////////////////////////////////////
unsigned
UFSDAPI_CALL
ufsd_bd_map(
    IN  struct super_block *sb,
    IN  UINT64  offset,
    IN  size_t  bytes,
    IN  size_t  flags,
    OUT struct buffer_head **bcb,
    OUT void    **mem
    )
{
  struct buffer_head *bh;
#if defined UFSD_TRACE || defined UFSD_HFS || !defined UFSD_USE_BUILTIN_ZEROING
  usuper *sbi = UFSD_SB( sb );
#endif
  unsigned int BlockSize  = sb->s_blocksize;
  sector_t  devblock      = (sector_t)(offset >> sb->s_blocksize_bits);
  size_t bytes2skip       = (size_t)(offset & (BlockSize - 1)); // offset % sb->s_blocksize
  TRACE_ONLY( const char *hint; )
  TRACE_ONLY( const char *hint2 = ""; )

  if ( bytes2skip + bytes > BlockSize ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdmap: [%llx %Zx] overlaps block boundary %x\n", offset, bytes, BlockSize));
    return -EINVAL;
  }

  ProfileEnter( sbi, bdmap );

#ifdef UFSD_HFS
  if ( devblock == sbi->max_block ) {
    assert( bytes == 512 );
    bh = bh_tail( sb, bytes2skip );
    bytes2skip = 0;
    TRACE_ONLY( hint = "tail "; )
  } else
#endif
  {
    if ( 0 == bytes2skip && bytes == BlockSize && FlagOn( flags, UFSD_RW_MAP_NO_READ ) ) {
      TRACE_ONLY( hint = "g "; )
      bh = __getblk( sb->s_bdev, devblock, BlockSize );
      if ( NULL != bh )
        set_buffer_uptodate( bh );
    } else {
      //
      // Do not readahead implicitly. cause BdZero
      //
#ifndef UFSD_USE_BUILTIN_ZEROING
      int RaBlocks      = sbi->ra_blocks;
      sector_t RaBlock  = devblock;
      while( 0 != RaBlocks-- ) {
        if ( RaBlock >= sbi->max_block )
          break;
        __breadahead( sb->s_bdev, RaBlock++, BlockSize );
      }
#endif

      TRACE_ONLY( hint = "b "; )
      bh = __bread( sb->s_bdev, devblock, BlockSize );
    }
  }

  ProfileLeave( sbi, bdmap );

  if ( NULL == bh ) {
    assert( !"bdmap: failed to map block" );
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdmap: failed to map block %"PSCT"x, max = %llx\n", devblock, sbi->max_block));
    return -EIO;
  }

  if ( buffer_locked( bh ) ) {
    TRACE_ONLY( hint2 = " w"; )
    __wait_on_buffer( bh );
  }

  DebugTrace( 0, UFSD_LEVEL_IO, ("bdmap: %s, %"PSCT"x, %Zx, %s%s%s -> %p (%d)\n", ufsd_bd_name( sb ), devblock, bytes,
              hint, buffer_dirty(bh)?"d":"c", hint2, bh, atomic_read( &bh->b_count ) ));

  //
  // Return pointer into page
  //
  *mem = Add2Ptr( bh->b_data, bytes2skip );
  *bcb = bh;
  DEBUG_ONLY( sbi->nMappedBh += 1; )
  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_bd_unmap
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_unmap(
#ifdef UFSD_DEBUG
    IN struct super_block *sb,
#endif
    IN struct buffer_head *bh,
    IN int Forget
    )
{
  assert( NULL != bh );

  DebugTrace( 0, UFSD_LEVEL_IO, ("bdunmap: %"PSCT"x,%s %d\n", bh->b_blocknr, buffer_dirty(bh)?"d":"c", atomic_read( &bh->b_count ) - 1 ));
  (Forget?__bforget : __brelse)( bh );

  DEBUG_ONLY( UFSD_SB( sb )->nUnMapped += 1; )
}


///////////////////////////////////////////////////////////
// ufsd_bd_set_dirty
//
//
///////////////////////////////////////////////////////////
unsigned
UFSDAPI_CALL
ufsd_bd_set_dirty(
    IN struct super_block *sb,
    IN struct buffer_head *bh,
    IN size_t   wait
    )
{
  int err = 0;
  usuper *sbi = UFSD_SB( sb );

  if ( !wait && FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    wait = UFSD_RW_WAIT_SYNC;

  if ( wait && sbi->options.nobarrier )
    wait &= ~UFSD_RW_WAIT_BARRIER;

  assert( NULL != bh );

  DebugTrace( 0, UFSD_LEVEL_IO, ("bddirty: %"PSCT"x,%s %d\n", bh->b_blocknr, buffer_dirty(bh)?"d":"c", atomic_read( &bh->b_count ) ));
  set_buffer_uptodate( bh );
  mark_buffer_dirty( bh );

  if ( wait ) {
    ProfileEnter( sbi, bdsetdirty );
    err = ufsd_bd_sync( sb, bh, wait );
    ProfileLeave( sbi, bdsetdirty );
  }
#ifdef UFSD_HFS
  else if ( bh->b_blocknr >= sbi->max_block ) {
    DebugTrace( 0, UFSD_LEVEL_IO, ("write tail: %"PSCT"x,%llx\n", bh->b_blocknr, sbi->max_block ));
    lock_buffer( bh );
    submit_bh( WRITE, bh );
  }
#endif

  return err;
}


#ifdef UFSD_HFS
///////////////////////////////////////////////////////////
// ufsd_bd_lock_buffer
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_lock_buffer(
    IN struct buffer_head *bh
    )
{
  assert( NULL != bh );
  assert( !buffer_locked( bh ) );
  lock_buffer( bh );
}


///////////////////////////////////////////////////////////
// ufsd_bd_unlock_buffer
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_unlock_buffer(
    IN struct buffer_head *bh
    )
{
  assert( NULL != bh );
  assert( buffer_locked( bh ) );
  set_buffer_uptodate( bh );
  unlock_buffer( bh );
}
#endif


///////////////////////////////////////////////////////////
// ufsd_bd_discard
//
// Issue a discard request (trim for SSD)
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_discard(
    IN struct super_block *sb,
    IN UINT64 offset,
    IN UINT64 bytes
    )
{
  usuper *sbi = UFSD_SB( sb );
  if ( FlagOn( sbi->flags, UFSD_SBI_FLAGS_DISRCARD ) && sbi->options.discard ) {
#ifdef Blkdev_issue_discard
    int err;
    // Align up 'start' on discard_granularity
    UINT64 start  = (offset + sbi->discard_granularity - 1) & sbi->discard_granularity_mask_inv;
    // Align down 'end' on discard_granularity
    UINT64 end    = (offset + bytes) & sbi->discard_granularity_mask_inv;
    UINT64 len;

    if ( start >= end ) {
      DebugTrace(0, UFSD_LEVEL_IO, ("discard: %s, %llx, %llx => nothing due to granularity\n", ufsd_bd_name( sb ), offset, bytes ));
      return 0;
    }

    len = end - start;

    DebugTrace(+1, UFSD_LEVEL_IO, ("discard: %s, %llx, %llx (%llx, %llx)\n", ufsd_bd_name( sb ), offset, bytes, start, len ));

    err = Blkdev_issue_discard( sb->s_bdev, start >> 9, len >> 9 );
    if ( -EOPNOTSUPP == err ) {
      DebugTrace(-1, UFSD_LEVEL_IO, ("discard -> not supported\n"));
      ClearFlag( sbi->flags, UFSD_SBI_FLAGS_DISRCARD );
      return ERR_NOTIMPLEMENTED;
    }

#ifdef UFSD_TRACE
    if ( 0 != err ) {
      DebugTrace( -1, UFSD_LEVEL_IO, ("discard -> failed %d\n", err));
    } else if ( ufsd_trace_level & UFSD_LEVEL_IO )
      ufsd_trace_inc( -1 );
#endif

    return err;
#else
    DebugTrace( 0, UFSD_LEVEL_IO, ("discard -> not implemented\n"));
    ClearFlag( sbi->flags, UFSD_SBI_FLAGS_DISRCARD );
#endif
  }
  return ERR_NOTIMPLEMENTED;
}

#ifdef UFSD_USE_BUILTIN_ZEROING

#if !(defined HAVE_DECL_BLKDEV_ISSUE_ZEROOUT && HAVE_DECL_BLKDEV_ISSUE_ZEROOUT)
//
// Stolen from block/blk-lib.c
//

struct bio_batch {
  atomic_t          done;
  unsigned long     flags;
  struct completion *wait;
};

static void bio_batch_end_io( struct bio *bio, int err )
{
  struct bio_batch *bb = bio->bi_private;
  if ( err && EOPNOTSUPP != err )
    clear_bit( BIO_UPTODATE, &bb->flags );
  if ( atomic_dec_and_test( &bb->done ) )
    complete( bb->wait );
  bio_put( bio );
}
#endif // #if !(defined HAVE_DECL_BLKDEV_ISSUE_ZEROOUT && HAVE_DECL_BLKDEV_ISSUE_ZEROOUT)


///////////////////////////////////////////////////////////
// ufsd_bd_zero
//
// Helper function to zero blocks in block device
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_zero(
    IN struct super_block *sb,
    IN UINT64 offset,
    IN size_t bytes
    )
{
  int err;

#if defined HAVE_DECL_BLKDEV_ISSUE_ZEROOUT && HAVE_DECL_BLKDEV_ISSUE_ZEROOUT
  //
  // 2.6.35+
  //
  DebugTrace( +1, UFSD_LEVEL_IO, ("bdzero: %p, %llx, %Zx\n", sb, offset, bytes));
  err = blkdev_issue_zeroout( sb->s_bdev, offset >> 9, bytes >> 9, GFP_NOFS
#ifdef BLKDEV_IFL_WAIT
                              ,  BLKDEV_IFL_WAIT | BLKDEV_IFL_BARRIER
#endif
                             );
#else
  sector_t  sector    = offset >> 9;
  sector_t  nr_sects  = bytes >> 9;
  struct bio_batch bb;
  struct page *zero;
#ifdef DECLARE_COMPLETION_ONSTACK
  DECLARE_COMPLETION_ONSTACK( wait );
#else
  DECLARE_COMPLETION( wait );
#endif

#if 1
  zero = alloc_page( GFP_KERNEL | __GFP_ZERO );
  if ( NULL == zero )
    return -ENOMEM;
#else
  // Don't use ZERO_PAGE(0). Sometimes G.P.L., sometimes not exported ...
  zero = ZERO_PAGE(0);
#endif

  DebugTrace( +1, UFSD_LEVEL_IO, ("bdzero: %p, %llx, %Zx\n", sb, offset, bytes));

  atomic_set( &bb.done, 1 );
  err       = 0;
  bb.flags  = 1 << BIO_UPTODATE;
  bb.wait   = &wait;

  while ( 0 != nr_sects ) {

    struct bio *bio = bio_alloc( GFP_NOFS, min(nr_sects, (sector_t)BIO_MAX_PAGES) );
    if ( !bio ) {
      err = -ENOMEM;
      break;
    }

    BIO_BISECTOR( bio )  = sector;
    bio->bi_bdev    = sb->s_bdev;
    bio->bi_end_io  = bio_batch_end_io;
    bio->bi_private = &bb;

    while ( 0 != nr_sects ) {
      unsigned int sz = min((sector_t) PAGE_SIZE , nr_sects << 9 );
      int ok = bio_add_page( bio, zero, sz, 0 );
      DebugTrace( 0, UFSD_LEVEL_IO, ("added %d\n", ok));
      nr_sects -= ok >> 9;
      sector   += ok >> 9;
      if ( ok < sz )
        break;
    }
    atomic_inc( &bb.done );
    DebugTrace( 0, UFSD_LEVEL_IO, ("bio: %"PSCT"x, sz=%x\n", BIO_BISECTOR( bio ), BIO_BISIZE( bio )));
    submit_bio( WRITE, bio );
  }

  // wait for bios in-flight
  if ( !atomic_dec_and_test( &bb.done ) )
    wait_for_completion( &wait );

  if ( !test_bit( BIO_UPTODATE, &bb.flags ) )
    err = -EIO; // One of bios in the batch was completed with error

  __free_page( zero );
#endif

#ifdef UFSD_TRACE
  if ( 0 != err ) {
    DebugTrace( -1, UFSD_LEVEL_IO, ("zero failed: err=%d\n", err));
  } else if ( ufsd_trace_level & UFSD_LEVEL_IO )
    ufsd_trace_inc( -1 );
#endif
  return err;
}
#endif // #ifdef UFSD_USE_BUILTIN_ZEROING


///////////////////////////////////////////////////////////
// ufsd_bd_set_blocksize
//
//
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_set_blocksize(
    IN struct super_block *sb,
    IN unsigned int BytesPerBlock
    )
{
  usuper *sbi = UFSD_SB( sb );

  if ( BytesPerBlock <= PAGE_CACHE_SIZE ) {
    UINT64 BytesPerDev    = sb->s_bdev->bd_inode->i_size;
    unsigned int RaBytes  = sbi->ra_blocks << sb->s_blocksize_bits;
    sb_set_blocksize( sb, BytesPerBlock );
    sbi->ra_blocks        = RaBytes >> sb->s_blocksize_bits;
    sbi->max_block        = BytesPerDev >> sb->s_blocksize_bits;
    DebugTrace( 0, Dbg, ("BdSetBlockSize %x\n", BytesPerBlock ));
  } else {
    DebugTrace( 0, Dbg, ("BdSetBlockSize %x -> %lx\n", BytesPerBlock, sb->s_blocksize ));
  }
}


///////////////////////////////////////////////////////////
// ufsd_bd_isreadonly
//
// Returns !0 for readonly media
///////////////////////////////////////////////////////////
int
UFSDAPI_CALL
ufsd_bd_isreadonly(
    IN struct super_block *sb
    )
{
  return FlagOn( sb->s_flags, MS_RDONLY );
}


///////////////////////////////////////////////////////////
// ufsd_bd_name
//
// Returns the name of block device
///////////////////////////////////////////////////////////
const char*
UFSDAPI_CALL
ufsd_bd_name(
    IN struct super_block *sb
    )
{
  return sb->s_id;
}


///////////////////////////////////////////////////////////
// ufsd_bd_flush
//
//
///////////////////////////////////////////////////////////
unsigned
UFSDAPI_CALL
ufsd_bd_flush(
    IN struct super_block *sb,
    IN size_t wait
    )
{
#if defined WRITE_FLUSH_FUA | defined WRITE_BARRIER
  if ( 0 == wait && 0 == UFSD_SB( sb )->options.nobarrier )
    return 0;
#endif

  DebugTrace( 0, Dbg, ("bdflush (%s)\n", ufsd_bd_name( sb ) ));

  return sync_blockdev( sb->s_bdev );
}


#ifdef UFSD_EXFAT
///////////////////////////////////////////////////////////
// ufsd_bd_invalidate
//
// Invalidate clean unused buffers and pagecache
// Used while texfat initialization
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_bd_invalidate(
    IN struct super_block *sb
    )
{
//  DebugTrace( 0, UFSD_LEVEL_IO, ("bd_invalidate: %s\n", ufsd_bd_name( sb ) ));
  sync_blockdev( sb->s_bdev );
#if defined HAVE_DECL_INVALIDATE_BDEV_V1 && HAVE_DECL_INVALIDATE_BDEV_V1
  invalidate_bdev( sb->s_bdev );
#elif defined HAVE_DECL_INVALIDATE_BDEV_V2 && HAVE_DECL_INVALIDATE_BDEV_V2
  invalidate_bdev( sb->s_bdev, 0 );
#else
  #error "Unknown version of invalidate_bdev"
#endif
}
#endif


///////////////////////////////////////////////////////////
// do_delayed_tasks
//
// This function is called under locked api_mutex
///////////////////////////////////////////////////////////
static void
do_delayed_tasks(
    IN usuper *sbi
    )
{
  unsigned int cnt;
  delay_task *task;
  int VFlush = atomic_read( &sbi->VFlush );

  if ( 0 != VFlush || ( sbi->options.sync && ufsdapi_is_volume_dirty( sbi->ufsd ) ) ){
    ufsdapi_volume_flush( sbi->ufsd, 2 == VFlush );
    atomic_set( &sbi->VFlush, 0 );
  }

  //
  // Do delayed clear
  //
  for ( cnt = 0; ; cnt++ ) {
    ufsd_file *file;
    spin_lock( &sbi->ddt_lock );
    if ( list_empty( &sbi->clear_list ) ) {
      task = NULL;
    } else {
      task = list_entry( sbi->clear_list.next, delay_task, list );
      list_del( &task->list );
    }
    spin_unlock( &sbi->ddt_lock );

    if ( NULL == task )
      break;

    file = task->file;
    assert( NULL != file );

    ufsdapi_file_close( sbi->ufsd, file );
    kfree( task );
  }

  if ( 0 != cnt ){
    DebugTrace( 0, Dbg, ("do_delayed_tasks: clear=%u\n", cnt ) );
  }
}


///////////////////////////////////////////////////////////
// _lock_ufsd
//
//
///////////////////////////////////////////////////////////
static void
_lock_ufsd(
    IN usuper *sb
#ifdef UFSD_TRACE
    , IN const char *Hint
#endif
    )
{
#ifdef UFSD_TRACE
  DEBUG_ONLY( unsigned long dT; )
  DEBUG_ONLY( unsigned long T0; )
  if ( ufsd_trace_level & UFSD_LEVEL_SEMA ) {
    si_meminfo( &sb->sys_info );
    _UFSDTrace(
#ifdef UFSD_DEBUG
                "%u: %lx %lx \"%s\" %s (+), ... ",
#else
                "%u: %lx %lx \"%s\" %s (+)\n",
#endif
                jiffies_to_msecs(jiffies-StartJiffies),
                sb->sys_info.freeram, sb->sys_info.bufferram,
                current->comm, Hint );

    ufsd_trace_inc( 1 );
  }
  DEBUG_ONLY( T0 = jiffies; )
#endif

  mutex_lock( &sb->api_mutex );

#if defined UFSD_TRACE && defined UFSD_DEBUG
  dT         = jiffies - T0;
  WaitMutex += dT;
  if ( ufsd_trace_level & UFSD_LEVEL_SEMA ) {
    if ( 0 == dT )
      _UFSDTrace("OK\n");
    else
      _UFSDTrace( "OKw %u\n", jiffies_to_msecs( dT ) );
  }
#endif

  //
  // Perform any delayed tasks
  //
  do_delayed_tasks( sb );
}


///////////////////////////////////////////////////////////
// try_lock_ufsd
//
// Returns 0 if mutex is locked
///////////////////////////////////////////////////////////
static int
_try_lock_ufsd(
    IN usuper *sb
#ifdef UFSD_TRACE
    , IN const char *Hint
#endif
    )
{
  int ok = mutex_trylock( &sb->api_mutex );
  assert( 0 == ok || 1 == ok );

#ifdef UFSD_TRACE
  if ( ufsd_trace_level & UFSD_LEVEL_SEMA ) {
    si_meminfo( &sb->sys_info );
    _UFSDTrace( "%u: %lx %lx \"%s\" %s %s\n",
                jiffies_to_msecs(jiffies-StartJiffies),
                sb->sys_info.freeram, sb->sys_info.bufferram,
                current->comm, Hint, ok? "(+)" : "-> wait" );
    if ( ok )
      ufsd_trace_inc( 1 );
  }
#endif

  if ( !ok )
    return 1;

  //
  // Perform any delayed tasks
  //
  do_delayed_tasks( sb );

  return 0;
}


///////////////////////////////////////////////////////////
// unlock_ufsd
//
//
///////////////////////////////////////////////////////////
static void
_unlock_ufsd(
    IN usuper *sb
#ifdef UFSD_TRACE
    , IN const char *Hint
#endif
    )
{
  //
  // Perform any delayed tasks
  //
  do_delayed_tasks( sb );

#ifdef UFSD_TRACE
  if ( ufsd_trace_level & UFSD_LEVEL_SEMA ) {
    si_meminfo( &sb->sys_info );
    ufsd_trace_inc( -1 );
    _UFSDTrace( "%u: %lx %lx \"%s\" %s (-)\n",
                jiffies_to_msecs(jiffies-StartJiffies),
                sb->sys_info.freeram, sb->sys_info.bufferram,
                current->comm, Hint );
  }
#endif

  mutex_unlock( &sb->api_mutex );
}


//
// Parameter structure for
// iget4 call to be passed as 'opaque'.
//

typedef struct ufsd_iget4_param {
  ucreate             *Create;
  finfo               *fi;
  ufsd_file           *fh;
  int                 subdir_count;
  const unsigned char *name;
  size_t              name_len;
} ufsd_iget4_param;


#if defined HAVE_DECL_READDIR_V1 && HAVE_DECL_READDIR_V1

  #define READDIR_DECLARE_ARG struct file *file, void *dirent, filldir_t filldir
  #define READDIR_POS         file->f_pos
  #define READDIR_FILL(Name, NameLen, pos, ino, dt) filldir( dirent, Name, NameLen, pos, ino, dt )
  #define iterate             readdir

#elif defined HAVE_DECL_READDIR_V2 && HAVE_DECL_READDIR_V2

  #define READDIR_DECLARE_ARG struct file *file, struct dir_context *ctx
  #define READDIR_POS         ctx->pos
  #define READDIR_FILL(Name, NameLen, dpos, ino, dt) (ctx->pos=dpos, !dir_emit( ctx, Name, NameLen, ino, dt ))

#else
  #error "Unknown readdir"
#endif


///////////////////////////////////////////////////////////
// ufsd_readdir
//
// file_operations::readdir
//
// This routine is a callback used to fill readdir() buffer.
//  file - Directory pointer.
//    'f_pos' member contains position to start scan from.
//
//  dirent, filldir - data to be passed to
//    'filldir()' helper
///////////////////////////////////////////////////////////
static int
ufsd_readdir(
    READDIR_DECLARE_ARG
    )
{
  struct inode *i = file_inode( file );
  unode *u        = UFSD_U( i );
  usuper *sbi     = UFSD_SB( i->i_sb );
  UINT64 pos      = READDIR_POS;
  ufsd_search *DirScan = NULL;
#ifdef UFSD_EMULATE_SMALL_READDIR_BUFFER
  size_t cnt = 0;
#endif

  if ( pos >= sbi->end_of_dir ) {
    DebugTrace( 0, Dbg, ("readdir: r=%lx,%llx -> no more\n", i->i_ino, pos ));
    return 0;
  }

  DebugTrace( +1, Dbg, ("readdir: %p, r=%lx, %llx\n", file, i->i_ino, pos ));

#ifdef UFSD_EMULATE_DOTS
  if ( 0 == pos ) {
    if ( READDIR_FILL( ".", 1, 0, i->i_ino, DT_DIR ) )
      goto out;
    pos = 1;
  }

  if ( 1 == pos ) {
    if ( READDIR_FILL( "..", 2, 1, parent_ino( file->f_path.dentry ), DT_DIR ) )
      goto out;
    pos = 2;
  }
#endif

  lock_ufsd( sbi );

  if ( 0 == ufsdapi_find_open( sbi->ufsd, u->ufile, pos, &DirScan ) ) {

    size_t  ino;
    int     is_dir;
    char    *Name;
    size_t  NameLen;
    int nfsd  = 0 == strcmp( "nfsd", current->comm );

    //
    // Enumerate UFSD's direntries
    //
    while ( 0 == ufsdapi_find_get( DirScan, &pos, &Name, &NameLen, &is_dir, &ino ) ) {

      int fd;

      if ( nfsd )
        unlock_ufsd( sbi );

#ifdef UFSD_EMULATE_SMALL_READDIR_BUFFER
      if ( ++cnt > UFSD_EMULATE_SMALL_READDIR_BUFFER )
        break;
#endif

      //
      // Unfortunately nfsd callback function opens file which in turn calls 'lock_ufsd'
      // Linux's mutex does not allow recursive locks
      //
      fd = READDIR_FILL( Name, NameLen, pos, (ino_t)ino, is_dir? DT_DIR : DT_REG );

      if ( nfsd )
        lock_ufsd( sbi );

      if ( fd )
        break;
    }

    ufsdapi_find_close( DirScan );
  }

  unlock_ufsd( sbi );

#ifdef UFSD_EMULATE_DOTS
out:
#endif
  //
  // Save position and return
  //
  READDIR_POS = pos;
  file->f_version = i->i_version;
#if defined HAVE_DECL_UPDATE_ATIME && HAVE_DECL_UPDATE_ATIME
  update_atime( i );
#endif

  DebugTrace( -1, Dbg, ("readdir -> 0 (next=%x)\n", (unsigned)file->f_pos));
  return 0;
}


#ifdef UFSD_NTFS2
///////////////////////////////////////////////////////////
// lazy_open
//
// Assumed lock_ufsd()
// Returns 0 if OK
///////////////////////////////////////////////////////////
static int
lazy_open(
    IN usuper       *sbi,
    IN struct inode *i
    )
{
  finfo *fi;
  unode *u = UFSD_U(i);

  if ( NULL != u->ufile )
    return 0;

  assert( sbi->options.ntfs );

  if ( 0 == ufsdapi_file_open_by_id( sbi->ufsd, i->i_ino, &u->ufile, &fi ) ) {
    assert( NULL != u->ufile );
    assert( i->i_ino == fi->Id );

    i_size_write( i, fi->FileSize );
    inode_set_bytes( i, fi->AllocSize );

    if ( S_ISDIR( i->i_mode ) != FlagOn( fi->Attrib, UFSDAPI_SUBDIR ) ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("Incorrect dir/file of inode r=%lx\n", i->i_ino ));
    } else {
      u->flags = (u->flags & ~UFSD_UNODE_FLAG_API_FLAGS) | (fi->Attrib & UFSD_UNODE_FLAG_API_FLAGS);

      if ( !FlagOn( fi->Attrib, UFSDAPI_SUBDIR ) ) {
        set_nlink( i, fi->HardLinks );
//        u->sparse   = FlagOn( fi->FSAttrib, UFSDAPI_SPARSE );
//        u->compr    = FlagOn( fi->FSAttrib, UFSDAPI_COMPRESSED );
//        u->encrypt  = FlagOn( fi->FSAttrib, UFSDAPI_ENCRYPTED );
        u->valid    = fi->ValidSize;
      }
//      u->xattr  = FlagOn( fi->Attrib, UFSDAPI_EA );
      return 0;
    }
  }

  make_bad_inode( i );
  return -ENOENT;
}
#else
  #define lazy_open( s, i ) 0
#endif


///////////////////////////////////////////////////////////
// ufsd_preopen_file
//
// helper function
///////////////////////////////////////////////////////////
static int
ufsd_preopen_file(
    IN usuper *sbi,
    IN unode  *u
    )
{
  int err     = 0;

  if ( test_and_clear_bit( UFSD_UNODE_FLAG_LAZY_OPEN_BIT, &u->flags ) ) {
    lock_ufsd( sbi );
    err = lazy_open( sbi, &u->i );
    unlock_ufsd( sbi );
    if ( err )
      goto out;
  }

  if ( test_and_clear_bit( UFSD_UNODE_FLAG_LAZY_INIT_BIT, &u->flags ) ) {
    mapinfo  Map;
    down_write( &u->size_sema );
    lock_ufsd( sbi );
    ufsdapi_file_map( u->ufile, 0, 0, 0, &Map );
    unlock_ufsd( sbi );
    up_write( &u->size_sema );
  }
out:
  return err;
}


#ifdef UFSD_NTFS2
///////////////////////////////////////////////////////////
// is_stream
//
// Helper function returns non zero if filesystem supports streams and
// 'file' is stream handler
///////////////////////////////////////////////////////////
static inline const unsigned char*
is_stream(
    IN struct file *file
    )
{
  const unsigned char *r = file->private_data;
#if 0
  // Nobody should use 'file->private_data'
  return r;
#else
  // Safe check
  if ( NULL != r ) {
    int d = (int)(r - file->f_dentry->d_name.name);
    if ( 0 < d && d <= file->f_dentry->d_name.len )
      return r;
  }
  return NULL;
#endif
}
#endif


///////////////////////////////////////////////////////////
// ufsd_file_open
//
// file_operations::open
///////////////////////////////////////////////////////////
static int
ufsd_file_open(
    IN struct inode *i,
    IN struct file  *file
    )
{
  usuper *sbi = UFSD_SB( i->i_sb );
  unode *u    = UFSD_U( i );
  TRACE_ONLY( struct qstr *s  = &file->f_dentry->d_name; )
  TRACE_ONLY( const char *hint=""; )
  int err;

  assert( file->f_mapping == i->i_mapping && "Check kernel config!" );
  DebugTrace( +1, Dbg, ("file_open: r=%lx, c=%x, l=%x, f=%p, fl=o%o%s%s, %.*s\n",
                i->i_ino, atomic_read( &i->i_count ), i->i_nlink,
                file, file->f_flags,
                FlagOn( file->f_flags, O_DIRECT )?",d":"", FlagOn( file->f_flags, O_APPEND )?",a":"",
                (int)s->len, s->name ));

  // Check file size
  err = generic_file_open( i, file );
  if ( 0 == err )
    err = ufsd_preopen_file( sbi, u );

  if ( 0 != err ) {
    DebugTrace( -1, Dbg, ("file_open -> failed\n"));
    return err;
  }

  if ( (is_compressd(u) || is_encrypted(u) ) && FlagOn( file->f_flags, O_DIRECT ) ) {
    DebugTrace( -1, Dbg, ("file_open -> failed to open compressed file with O_DIRECT\n"));
    return -ENOTBLK;
  }

#ifdef UFSD_NTFS2
  assert( NULL == file->private_data );
  if ( 0 != sbi->options.delim ) {
#ifndef UFSD_TRACE
    struct qstr *s  = &file->f_dentry->d_name;
#endif
    char *p = strchr( s->name, sbi->options.delim );
    if ( NULL != p ){
      igrab( i );
      dget( file->f_dentry );
      file->private_data = p + 1;
      assert( is_stream( file ) );
      TRACE_ONLY( hint="(stream)"; )
    }
  }
#endif

  if ( i->i_nlink <= 1 ) {
    DebugTrace( -1, Dbg, ("file_open%s -> ok%s, sz=%llx,%llx\n", hint, is_compressd(u)?", c" : "", u->valid, i->i_size ));
  } else {
    DebugTrace( -1, Dbg, ("file_open%s -> l=%x%s\n", hint, i->i_nlink, is_compressd(u)?", c" : "" ));
  }

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_file_release
//
// file_operations::release
///////////////////////////////////////////////////////////
static int
ufsd_file_release(
    IN struct inode *i,
    IN struct file  *file
    )
{
//  unode *u = UFSD_U( i );
  TRACE_ONLY( const char *hint=""; )
#ifdef UFSD_NTFS2
  if ( is_stream( file ) ) {
    dput( file->f_dentry );
    iput( i );
    TRACE_ONLY( hint="(stream)"; )
  }
#endif

  DebugTrace( 0, Dbg, ("file_release%s: r=%lx, %p\n", hint, i->i_ino, file ));

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_fsync
//
// file_operations::fsync
///////////////////////////////////////////////////////////
static int
ufsd_fsync(
    IN struct file *file,
#if defined HAVE_DECL_FO_FSYNC_V1 && HAVE_DECL_FO_FSYNC_V1
    IN struct dentry *de,
#elif defined HAVE_DECL_FO_FSYNC_V2 && HAVE_DECL_FO_FSYNC_V2
#elif defined HAVE_DECL_FO_FSYNC_V3 && HAVE_DECL_FO_FSYNC_V3
    IN loff_t start,
    IN loff_t end,
#else
#error "unknown version of fsync"
#endif
    IN int datasync
    )
{
  struct inode *i = file_inode( file );
  int err;

  DebugTrace( +1, Dbg, ("fsync: r=%lx, %d\n", i->i_ino, datasync ));
  err = write_inode_now( i, 1 );
  DebugTrace( -1, Dbg, ("fsync => %d\n", err ));
  return err;
}


#ifndef UFSD_NO_USE_IOCTL

#ifndef VFAT_IOCTL_GET_VOLUME_ID
  #define VFAT_IOCTL_GET_VOLUME_ID  _IOR('r', 0x12, __u32)
#endif

///////////////////////////////////////////////////////////
// ufsd_ioctl
//
// file_operations::ioctl
///////////////////////////////////////////////////////////
#if defined HAVE_STRUCT_FILE_OPERATIONS_IOCTL && HAVE_STRUCT_FILE_OPERATIONS_IOCTL
static int
ufsd_ioctl(
    IN struct inode   *i,
    IN struct file    *file,
    IN unsigned int   cmd,
    IN unsigned long  arg
    )
{
#else
#define ioctl unlocked_ioctl
static long
ufsd_ioctl(
    IN struct file    *file,
    IN unsigned int   cmd,
    IN unsigned long  arg
    )
{
  struct inode *i = file_inode( file );
#endif
  int err;
  size_t ioctl;
  unsigned insize = 0, osize = 0;
  size_t BytesReturned;
  usuper *sbi  = UFSD_SB( i->i_sb );
  finfo *fi;
  unsigned long flags;
  unode *u;

  DebugTrace( +1, Dbg,("ioctl: ('%.*s'), r=%lx, m=%o, f=%p, %08x, %lx\n",
                       (int)file->f_dentry->d_name.len, file->f_dentry->d_name.name,
                       i->i_ino, i->i_mode, file, cmd, arg));

  if ( VFAT_IOCTL_GET_VOLUME_ID == cmd ) {
    //
    // Special code
    //
    err = ufsdapi_query_volume_id( sbi->ufsd );
    DebugTrace( -1, Dbg, ("ioctl (VFAT_IOCTL_GET_VOLUME_ID ) -> %x\n", (unsigned)err));
    return err;
  }

  switch( cmd ) {
  case UFSD_IOC_SETVALID:
  case UFSD_IOC_SETCLUMP:
  case UFSD_IOC_SETTIMES:
  case UFSD_IOC_GETTIMES:
  case UFSD_IOC_SETATTR:
  case UFSD_IOC_GETATTR:
  case UFSD_IOC_GETMEMUSE:
  case UFSD_IOC_GETVOLINFO:
    ioctl = _IOC_NR(cmd);
    break;

#if defined FITRIM && defined Blkdev_issue_discard
  case FITRIM:
    if ( !capable( CAP_SYS_ADMIN ) ) {
      err = -EPERM;
      goto out;
    }

    if ( 0 == sbi->discard_granularity ) {
      err = -EOPNOTSUPP;
      goto out;
    }

    // code 121 is already used by ufsd
    ioctl = 44;
    break;
#endif

  default:
    DebugTrace( -1, Dbg, ("ioctl -> '-ENOTTY'\n"));
    return -ENOTTY;
  }

  if ( _IOC_DIR( cmd ) & _IOC_WRITE ) {
    insize  = _IOC_SIZE( cmd );
    if ( !access_ok( VERIFY_READ, (__user void*)arg, insize ) ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("ioctl: invalid input buffer.\n"));
      err = -EFAULT;
      goto out;
    }
  }

  if (_IOC_DIR( cmd ) & _IOC_READ ) {
    osize   = _IOC_SIZE( cmd );
    if ( !access_ok( VERIFY_WRITE, (__user void*)arg, osize ) ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("ioctl: invalid output buffer.\n"));
      err = -EFAULT;
      goto out;
    }
  }

  assert( NULL != i );
  assert( NULL != UFSD_VOLUME(i->i_sb) );

  //
  // And call the library.
  //
  lock_ufsd( sbi );

  u   = UFSD_U( i );
  err = ufsdapi_ioctl( sbi->ufsd, u->ufile, ioctl, (void*)arg, insize, (void*)arg, osize, &BytesReturned, &fi );

  if ( 0 == err ) {
    switch( cmd ) {
    case UFSD_IOC_SETTIMES:
      ufsd_times_to_inode( sbi, u, i, fi );
      mark_inode_dirty_sync( i );
      break;
    case UFSD_IOC_SETVALID:
      write_lock_irqsave( &u->valid_lock, flags );
      u->valid  = fi->ValidSize;
      write_unlock_irqrestore( &u->valid_lock, flags );
      // no break here!
    case UFSD_IOC_SETATTR:
      mark_inode_dirty_sync( i );
      break;
    }
  }

  unlock_ufsd( sbi );

  //
  // Translate possible UFSD IoControl errors (see u_errors.h):
  //
  switch( (unsigned)err ) {
  case 0:                       err = 0; break;           // OK
  case ERR_NOTIMPLEMENTED:      err = -ENOSYS; break;     // Function not implemented
  case ERR_INSUFFICIENT_BUFFER: err = -ENODATA; break;    // No data available
  case ERR_MORE_DATA:           err = -EOVERFLOW; break;  // Value too large for defined data type
  default:                      err = -EINVAL;
  }

out:

  DebugTrace( -1, Dbg, ("ioctl -> %d\n", err));
  return err;
}


#ifdef CONFIG_COMPAT
#include <linux/compat.h>
///////////////////////////////////////////////////////////
// ufsd_compat_ioctl
//
// 32 application -> 64 bit driver
///////////////////////////////////////////////////////////
static long
ufsd_compat_ioctl(
    IN struct file    *file,
    IN unsigned int   cmd,
    IN unsigned long  arg
    )
{
  switch( cmd ) {
  case UFSD_IOC32_GETMEMUSE:
    cmd = UFSD_IOC_GETMEMUSE;
    break;
  }

  return ufsd_ioctl(
#if defined HAVE_STRUCT_FILE_OPERATIONS_IOCTL && HAVE_STRUCT_FILE_OPERATIONS_IOCTL
    file_inode( file ),
#endif
    file, cmd, (unsigned long)compat_ptr(arg)
    );
}
#endif // #ifdef CONFIG_COMPAT
#endif // #ifndef UFSD_NO_USE_IOCTL

static const struct file_operations ufsd_dir_operations = {
  .llseek   = generic_file_llseek,
  .read     = generic_read_dir,
  .iterate  = ufsd_readdir,
  .fsync    = ufsd_fsync,
  .open     = ufsd_file_open,
  .release  = ufsd_file_release,
#ifndef UFSD_NO_USE_IOCTL
  .ioctl    = ufsd_ioctl,
#ifdef CONFIG_COMPAT
  .compat_ioctl = ufsd_compat_ioctl,
#endif
#endif
};


///////////////////////////////////////////////////////////
// ufsd_compare_hlp
//
// Helper function for both version of 'ufsd_compare'
// Returns 0 if names equal, +1 if not equal, -1 if UFSD required
///////////////////////////////////////////////////////////
static inline int
ufsd_compare_hlp(
    IN const char   *n1,
    IN unsigned int l1,
    IN const char   *n2,
    IN unsigned int l2
    )
{
  unsigned int len = min( l1, l2 );
  while( 0 != len-- ){
    unsigned char c1 = *n1++, c2 = *n2++;
    if ( (c1 >= 0x80 || c2 >= 0x80) && c1 != c2 )
      return -1; // Requires UFSD
    if ( 'a' <= c1 && c1 <= 'z' )
      c1 -= 'a'-'A';  // c1 &= ~0x20
    if ( 'a' <= c2 && c2 <= 'z' )
      c2 -= 'a'-'A';  // c2 &= ~0x20

    if ( c1 != c2 )
      return 1; // not equal
  }
  return l1 == l2? 0 : 1;
}


///////////////////////////////////////////////////////////
// ufsd_name_hash_hlp
//
// Helper function for both version of 'ufsd_name_hash'
///////////////////////////////////////////////////////////
static inline unsigned int
ufsd_name_hash_hlp(
    IN const char   *n,
    IN unsigned int len,
    OUT int         *err
    )
{
  unsigned int hash = 0;

  while( 0 != len-- ) {
    unsigned int c = *n++;
    if ( c >= 0x80 ) {
      *err = -1;  // Requires UFSD
      return 0;
    }

    if ( 'a' <= c && c <= 'z' )
      c -= 'a'-'A'; // simple upcase

    hash = (hash + (c << 4) + (c >> 4)) * 11;
  }

  *err = 0;
  return hash;
}


#if defined HAVE_DECL_DHASH_V1 && HAVE_DECL_DHASH_V1
///////////////////////////////////////////////////////////
// ufsd_compare
//
// dentry_operations::d_compare
///////////////////////////////////////////////////////////
static int
ufsd_compare(
    IN struct dentry *de,
    IN struct qstr   *name1,
    IN struct qstr   *name2
    )
{
  int ret;

  // Custom compare used to support case-insensitive scan.
  // Should return zero on name match.
  assert(NULL != de->d_inode);

  DEBUG_ONLY( UFSD_SB(de->d_inode->i_sb)->nCompareCalls += 1; )

  ret = ufsd_compare_hlp( name1->name, name1->len, name2->name, name2->len );
  if ( ret < 0 ) {
    usuper *sbi  = UFSD_SB(de->d_inode->i_sb);
    mutex_lock( &sbi->nocase_mutex );
    ret = !ufsdapi_names_equal( sbi->ufsd, name1->name, name1->len, name2->name, name2->len );
    mutex_unlock( &sbi->nocase_mutex );
  }
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_name_hash
//
// dentry_operations::d_hash
///////////////////////////////////////////////////////////
static int
ufsd_name_hash(
    IN struct dentry *de,
    IN struct qstr   *name
    )
{
  int err;
  DEBUG_ONLY( UFSD_SB(de->d_inode->i_sb)->nHashCalls += 1; )

  name->hash = ufsd_name_hash_hlp( name->name, name->len, &err );
  if ( err ) {
    usuper *sbi  = UFSD_SB(de->d_inode->i_sb);
    mutex_lock( &sbi->nocase_mutex );
    name->hash = ufsdapi_names_hash( sbi->ufsd, name->name, name->len );
    mutex_unlock( &sbi->nocase_mutex );
  }

  return 0;
}

#elif ( defined HAVE_DECL_DHASH_V2 && HAVE_DECL_DHASH_V2 ) || ( defined HAVE_DECL_DHASH_V3 && HAVE_DECL_DHASH_V3 )

///////////////////////////////////////////////////////////
// ufsd_compare
//
// dentry_operations::d_compare
///////////////////////////////////////////////////////////
static int
ufsd_compare(
    IN const struct dentry *parent,
#if defined HAVE_DECL_DCOMPARE_V2 && HAVE_DECL_DCOMPARE_V2
    IN const struct inode  *iparent,
#endif
    IN const struct dentry *de,
#if defined HAVE_DECL_DCOMPARE_V2 && HAVE_DECL_DCOMPARE_V2
    IN const struct inode  *i,
#endif
    IN unsigned int         len,
    IN const char          *str,
    IN const struct qstr   *name
    )
{
  int ret;

//  DebugTrace( 0, Dbg, ("ufsd_compare: %p %p %p %p %*.s %.*s\n", parent, iparent, de, i, len, str, name->len, name->name ));

  // Custom compare used to support case-insensitive scan.
  // Should return zero on name match.
  //
  // NOTE: do not use 'i' cause it can be NULL (3.6.6+)
  //
#if defined HAVE_DECL_DCOMPARE_V2 && HAVE_DECL_DCOMPARE_V2
  assert( NULL != parent && NULL != iparent && parent->d_inode == iparent );
#elif defined HAVE_DECL_DCOMPARE_V3 && HAVE_DECL_DCOMPARE_V3
  assert( NULL != parent );
#endif

  DEBUG_ONLY( UFSD_SB( parent->d_inode->i_sb )->nCompareCalls += 1; )

  ret = ufsd_compare_hlp( name->name, name->len, str, len );
  if ( ret < 0 ) {
    usuper *sbi  = UFSD_SB( parent->d_inode->i_sb );
    mutex_lock( &sbi->nocase_mutex );
    ret = !ufsdapi_names_equal( sbi->ufsd, name->name, name->len, str, len );
    mutex_unlock( &sbi->nocase_mutex );
  }

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_name_hash
//
// dentry_operations::d_hash
///////////////////////////////////////////////////////////
static int
ufsd_name_hash(
    IN const struct dentry *de,
#if defined HAVE_DECL_DHASH_V2 && HAVE_DECL_DHASH_V2
    IN const struct inode  *i,
#endif
    IN struct qstr         *name
    )
{
  int err;
  DEBUG_ONLY( UFSD_SB(de->d_inode->i_sb)->nHashCalls += 1; )

  name->hash = ufsd_name_hash_hlp( name->name, name->len, &err );
  if ( err ) {
    usuper *sbi  = UFSD_SB(de->d_inode->i_sb);
    mutex_lock( &sbi->nocase_mutex );
    name->hash = ufsdapi_names_hash( sbi->ufsd, name->name, name->len );
    mutex_unlock( &sbi->nocase_mutex );
  }

  return 0;
}

#else

#error "unknown dentry_operations.d_hash"

#endif


static struct dentry_operations ufsd_dop = {
  // case insensitive (nocase=1)
  .d_hash       = ufsd_name_hash,
  .d_compare    = ufsd_compare,
};


static void ufsd_read_inode2 (struct inode *i, void *p);
///////////////////////////////////////////////////////////
// iget4
//
//
///////////////////////////////////////////////////////////
static inline struct inode*
iget4(
    IN struct super_block *sb,
    IN unsigned long ino,
    IN void *unused,
    IN void *ctxt
    )
{
  struct inode *i = iget_locked( sb, ino );
  if ( NULL != i && FlagOn( i->i_state, I_NEW ) ) {
    ufsd_read_inode2( i, ctxt );
    unlock_new_inode( i );
  }
  return i;
}


// Forward declaration
static int
ufsd_create_or_open (
    IN struct inode       *dir,
    IN OUT struct dentry  *de,
    IN ucreate            *cr,
    OUT struct inode      **inode
    );

#ifdef UFSD_USE_XATTR
static int
ufsd_acl_chmod(
    IN struct inode *i
    );
#endif

#if ( defined HAVE_DECL_INOP_CREATE_V3 && HAVE_DECL_INOP_CREATE_V3 || defined HAVE_DECL_INOP_CREATE_V4 && HAVE_DECL_INOP_CREATE_V4 )
  typedef umode_t  Umode_t;
#else
  typedef int      Umode_t;
#endif

///////////////////////////////////////////////////////////
// ufsd_create
//
// create/open use the same helper.
// inode_operations::create
///////////////////////////////////////////////////////////
static int
ufsd_create(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN Umode_t         mode
#if (defined HAVE_DECL_INOP_CREATE_V2 && HAVE_DECL_INOP_CREATE_V2) || (defined HAVE_DECL_INOP_CREATE_V3 && HAVE_DECL_INOP_CREATE_V3)
    , struct nameidata *nd
#elif defined HAVE_DECL_INOP_CREATE_V4 && HAVE_DECL_INOP_CREATE_V4
    , bool namei
#endif
    )
{
  int err;
  struct inode *i = NULL;

  ucreate  cr;

  cr.lnk  = NULL;
  cr.data = NULL;
  cr.len  = 0;
  cr.mode = mode;

  err = ufsd_create_or_open( dir, de, &cr, &i );

  if ( !err )
    d_instantiate( de, i );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_mkdir
//
// inode_operations::mkdir
///////////////////////////////////////////////////////////
static int
ufsd_mkdir(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN Umode_t        mode
    )
{
  int err;
  struct inode *i = NULL;
  ucreate  cr;

  cr.lnk  = NULL;
  cr.data = NULL;
  cr.len  = 0;
  cr.mode = mode | S_IFDIR;

  err = ufsd_create_or_open( dir, de, &cr, &i );

  if ( !err )
    d_instantiate( de, i );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_unlink
//
// inode_operations::unlink
// inode_operations::rmdir
///////////////////////////////////////////////////////////
static int
ufsd_unlink(
    IN struct inode   *dir,
    IN struct dentry  *de
    )
{
  int err;
  struct inode *i   = de->d_inode;
  usuper *sbi       = UFSD_SB( i->i_sb );
  struct qstr *s    = &de->d_name;
  unsigned char *p  = 0 == sbi->options.delim? NULL : strchr( s->name, sbi->options.delim );
  char *sname;
  int flen, slen;

  if ( NULL == p ) {
    flen  = s->len;
    sname = NULL;
    slen  = 0;
  } else {
    flen  = p - s->name;
    sname = p + 1;
    slen  = s->name + s->len - p - 1;
  }

  DebugTrace( +1, Dbg, ("unlink: r=%lx, ('%.*s'), r=%lx, c=%x, l=%x\n",
              dir->i_ino, (int)s->len, s->name,
              i->i_ino, atomic_read( &i->i_count ), i->i_nlink));

  lock_ufsd( sbi );

  err = ufsdapi_unlink( sbi->ufsd, UFSD_FH(dir), s->name, flen, sname, slen, UFSD_FH(i) );

  unlock_ufsd( sbi );

  switch( err ) {
  case ERR_DIRNOTEMPTY:
    DebugTrace( -1, Dbg, ("unlink -> ENOTEMPTY\n"));
    return -ENOTEMPTY;
  case ERR_NOSPC:
    DebugTrace( -1, Dbg, ("unlink -> ENOSPC\n"));
    return -ENOSPC;
  }

  if ( 0 != err ) {
    make_bad_inode( i );
    DebugTrace( -1, Dbg, ("unlink -> EACCES\n"));
    return -EACCES;
  }

  if ( NULL == sname ) {
    UINT64 dir_size = ufsdapi_get_dir_size( UFSD_FH( dir ) );
    drop_nlink( i );

    // Mark dir as requiring resync.
    dir->i_version += 1;
    TIMESPEC_SECONDS( &dir->i_mtime )  = TIMESPEC_SECONDS( &dir->i_ctime ) = get_seconds();
    i_size_write( dir, dir_size );
    inode_set_bytes( dir, dir_size );
    mark_inode_dirty_sync( dir );
    i->i_ctime    = dir->i_ctime;
    mark_inode_dirty_sync( i );
  }

#ifdef UFSD_COUNT_CONTAINED
  if ( S_ISDIR( i->i_mode ) ) {
    assert(dir->i_nlink > 0);
    dir->i_nlink -= 1;
    mark_inode_dirty_sync( dir );
  }
#endif

  DebugTrace( -1, Dbg, ("unlink -> %d\n", 0));
  return 0;
}


#if !(defined HAVE_DECL_TRUNCATE_SETSIZE && HAVE_DECL_TRUNCATE_SETSIZE)
///////////////////////////////////////////////////////////
// ufsd_truncate
//
// inode_operations::truncate
///////////////////////////////////////////////////////////
static void
ufsd_truncate(
    IN struct inode *i
    )
{
  UINT64 asize;
  UINT64 isize  = i_size_read( i );
  usuper *sbi   = UFSD_SB( i->i_sb );
  unode *u      = UFSD_U( i );

  DebugTrace( +1, Dbg, ("truncate(r=%lx) -> %llx\n", i->i_ino, isize ) );

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  if ( 0 == lazy_open( sbi, i )
    && 0 == ufsdapi_file_set_size( u->ufile, isize, &asize ) )
  {
    unsigned long flags;
    inode_set_bytes( i, asize );
    write_lock_irqsave( &u->valid_lock, flags );
    if ( u->valid > isize )
      u->valid  = isize;
    write_unlock_irqrestore( &u->valid_lock, flags );
  }

  //
  // 'lazy_open' may change 'i->i_size'
  // Always restore correct value
  //
  i->i_size = isize;

  unlock_ufsd( sbi );

  DebugTrace( -1, Dbg, ("truncate -> ok\n"));
}
#endif


///////////////////////////////////////////////////////////
// ufsd_set_size
//
// Helper function
///////////////////////////////////////////////////////////
static int
ufsd_set_size(
    IN struct inode *i,
    IN UINT64 isize,
    IN UINT64 new_size
    )
{
  int err;
  UINT64 asize;
  unode *u  = UFSD_U( i );
#if defined HAVE_DECL_TRUNCATE_SETSIZE && HAVE_DECL_TRUNCATE_SETSIZE
  //
  // 2.6.36++ way
  //
  usuper *sbi   = UFSD_SB( i->i_sb );
  TRACE_ONLY( const char *hint = new_size >= isize? "expand":"truncate"; )

  DebugTrace( +1, Dbg, ("%s: %llx -> %llx%s\n", hint, isize, new_size, is_sparsed(u)?" ,sp" : "" ) );

  truncate_setsize( i, new_size );

  down_write( &u->size_sema );
  lock_ufsd( sbi );

  err = lazy_open( sbi, i );

  if ( 0 == err ) {
//    u->ver += 1;
    err = ufsdapi_file_set_size( u->ufile, new_size, &asize );

    if ( 0 != err ) {
      // If no free space we should return -EFBIG or -EINVAL
      //err = ERR_NOSPC == err? -ENOSPC : -EINVAL;
      err = -EINVAL;
    } else {
      unsigned long flags;
      assert( new_size <= asize );
      inode_set_bytes( i, asize );
      write_lock_irqsave( &u->valid_lock, flags );
      if ( u->valid > new_size )
        u->valid  = new_size;
      write_unlock_irqrestore( &u->valid_lock, flags );
    }
  }

  unlock_ufsd( sbi );
  up_write( &u->size_sema );

  if ( 0 != err ) {
    // restore size
    i_size_write( i, isize );
    DebugTrace( -1, Dbg, ("%s failed -> %d\n", hint, err ) );
    return err;
  }

  DebugTrace( -1, Dbg, ("%s -> ok, valid=%llx, asize=%llx\n", hint, u->valid, asize ) );

#else

  //
  // 2.6.36-- way
  //
  if ( new_size >= isize ) {
    usuper *sbi   = UFSD_SB( i->i_sb );
    DebugTrace( +1, Dbg, ("expand: %llx -> %llx%s\n", isize, new_size, is_sparsed(u)?" ,sp" : "" ) );

    down_write( &u->size_sema );
    lock_ufsd( sbi );

    err = lazy_open( sbi, i );

    if ( 0 == err ) {

      err = ufsdapi_file_set_size( u->ufile, new_size, &asize );

      if ( 0 == err ) {
        i_size_write( i, new_size );
        inode_set_bytes( i, asize );
      } else
        // If no free space we should return -EFBIG or -EINVAL
        //err = ERR_NOSPC == err? -ENOSPC : -EINVAL;
        err = -EINVAL;
    }

    unlock_ufsd( sbi );
    up_write( &u->size_sema );

    if ( 0 == err ) {
      DebugTrace( -1, Dbg, ("expand -> ok, valid=%llx\n", u->valid ) );
    } else {
      DebugTrace( -1, Dbg, ("expand failed -> %d\n", err ) );
      return err;
    }
  } else {
    DebugTrace( 0, Dbg, ("vmtruncate: %llx -> %llx\n", isize, new_size ));
    err = vmtruncate( i, new_size );
    if ( err ) {
      DebugTrace( 0, Dbg, ("vmtruncate failed %d\n", err ));
      return err;
    }
    assert( new_size == i->i_size );
  }
#endif

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_setattr
//
// inode_operations::setattr
///////////////////////////////////////////////////////////
static int
ufsd_setattr(
    IN struct dentry *de,
    IN struct iattr  *attr
    )
{
  UINT64 isize, ia_size;
  struct inode *i = de->d_inode;
  unode *u        = UFSD_U( i );
  int err;
  int dirty = 0;
  unsigned int ia_valid = attr->ia_valid;

  DebugTrace( +1, Dbg, ("setattr(%x): r=%lx, uid=%d,gid=%d,m=%o,sz=%llx,%llx\n",
                        ia_valid, i->i_ino, __kuid_val(i->i_uid), __kgid_val(i->i_gid), i->i_mode,
                        u->valid, i->i_size ));

  err = inode_change_ok( i, attr );
  if ( err ) {
#ifdef UFSD_DEBUG
    unsigned int fs_uid   = __kuid_val( current_fsuid() );
    DebugTrace( 0, Dbg, ("inode_change_ok failed: \"%s\" current_fsuid=%d, ia_valid=%x\n", current->comm, fs_uid, ia_valid ));
    if ( ia_valid & ATTR_UID )
      DebugTrace( 0, Dbg, ("new uid=%d, capable(CAP_CHOWN)=%d\n", __kuid_val( attr->ia_uid ), capable(CAP_CHOWN) ));

    if ( ia_valid & ATTR_GID )
      DebugTrace( 0, Dbg, ("new gid=%d, in_group_p=%d, capable(CAP_CHOWN)=%d\n", __kgid_val( attr->ia_gid ), in_group_p(attr->ia_gid), capable(CAP_CHOWN) ));

    if ( ia_valid & ATTR_MODE )
      DebugTrace( 0, Dbg, ("new mode=%o, is_owner_or_cap=%d\n", (unsigned)attr->ia_mode, (int)is_owner_or_cap(i) ));

#ifndef ATTR_TIMES_SET
  #define ATTR_TIMES_SET  (1 << 16)
#endif
    if ( ia_valid & (ATTR_MTIME_SET | ATTR_ATIME_SET | ATTR_TIMES_SET) )
      DebugTrace( 0, Dbg, ("new times, is_owner_or_cap=%d\n", (int)is_owner_or_cap(i) ));
#endif
    goto out;
  }

  if ( (ia_valid & ATTR_SIZE) && ( isize = i_size_read( i ) ) != ( ia_size = attr->ia_size ) ) {
    if ( is_encrypted(u) ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("setattr: attempt to resize encrypted file\n" ) );
      err = -ENOSYS;
      goto out;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,3,0)
    inode_dio_wait( i );
#endif

    err = ufsd_set_size( i, isize, ia_size );
    if ( 0 != err )
      goto out;
    dirty = 1;
    TIMESPEC_SECONDS( &i->i_mtime ) = TIMESPEC_SECONDS( &i->i_ctime ) = get_seconds();
  }

  //
  // Do smart setattr_copy
  //
  if ( (ia_valid & ATTR_UID) && !uid_eq( i->i_uid, attr->ia_uid ) ) {
    dirty = 1;
    i->i_uid = attr->ia_uid;
    set_bit( UFSD_UNODE_FLAG_SET_MODE_BIT, &u->flags );
  }
  if ( (ia_valid & ATTR_GID) && !gid_eq( i->i_gid, attr->ia_gid ) ) {
    dirty = 1;
    i->i_gid = attr->ia_gid;
    set_bit( UFSD_UNODE_FLAG_SET_MODE_BIT, &u->flags );
  }
  if ( (ia_valid & ATTR_ATIME) && 0 != timespec_compare( &i->i_atime, &attr->ia_atime ) ) {
    dirty = 1;
    i->i_atime = attr->ia_atime;
  }
  if ( (ia_valid & ATTR_MTIME) && 0 != timespec_compare( &i->i_mtime, &attr->ia_mtime ) ) {
    dirty = 1;
    i->i_mtime = attr->ia_mtime;
  }
  if ( (ia_valid & ATTR_CTIME) && 0 != timespec_compare( &i->i_ctime, &attr->ia_ctime ) ) {
    dirty = 1;
    i->i_ctime = attr->ia_ctime;
  }
  if ( ia_valid & ATTR_MODE ) {
    umode_t mode = attr->ia_mode;
    if (!in_group_p(i->i_gid) && !capable(CAP_FSETID))
      mode &= ~S_ISGID;

    if ( i->i_mode != mode ) {
      dirty = 1;
      i->i_mode = mode;
      set_bit( UFSD_UNODE_FLAG_SET_MODE_BIT, &u->flags );
    }

#ifdef UFSD_USE_XATTR
    err = ufsd_acl_chmod( i );
    if ( err )
      goto out;
#endif
  }

out:
  if ( dirty )
    mark_inode_dirty_sync( i );

  DebugTrace( -1, Dbg, ("setattr -> %d, uid=%d,gid=%d,m=%o,sz=%llx,%llx%s\n", err,
                        __kuid_val(i->i_uid), __kgid_val(i->i_gid), i->i_mode,
                        u->valid, i->i_size, FlagOn(i->i_state, I_DIRTY)?",d":"" ));
  return err;
}


#ifdef UFSD_NTFS2
///////////////////////////////////////////////////////////
// ufsd_setattr
//
// inode_operations::getattr
///////////////////////////////////////////////////////////
static int
ufsd_getattr(
    IN struct vfsmount  *mnt,
    IN struct dentry    *de,
    OUT struct kstat    *kstat
    )
{
  struct inode *i = de->d_inode;
  unode *u = UFSD_U( i );
#if 0
  usuper *sbi = UFSD_SB( i->i_sb );
  ufsd_preopen_file( sbi, u );
#endif

  generic_fillattr( i, kstat );

  if ( is_sparsed_or_compressed( u ) )
    kstat->blocks = u->total_alloc >> 9;

#if 0
  DebugTrace( 0, Dbg, ("getattr (r=%llx): m=%o, t=%lx,%lx,%lx, s=%llx, b=%llx\n",
                      kstat->ino, (unsigned)kstat->mode,
                      kstat->atime.tv_sec, kstat->mtime.tv_sec, kstat->ctime.tv_sec,
                      kstat->size, kstat->blocks ));
#endif

  return 0;
}
#else
#define ufsd_getattr NULL
#endif


///////////////////////////////////////////////////////////
// ufsd_rename
//
// inode_operations::rename
///////////////////////////////////////////////////////////
static int
ufsd_rename(
    IN struct inode   *odir,
    IN struct dentry  *ode,
    IN struct inode   *ndir,
    IN struct dentry  *nde
    )
{
  int err;
  usuper *sbi = UFSD_SB( odir->i_sb );
  UINT64 dir_size;

  assert( NULL != UFSD_FH( odir ) );

  DebugTrace( +1, Dbg, ("rename: r=%lx, %p('%.*s') => r=%lx, %p('%.*s')\n",
                      odir->i_ino, ode,
                      (int)ode->d_name.len, ode->d_name.name,
                      ndir->i_ino, nde,
                      (int)nde->d_name.len, nde->d_name.name ));

  //
  // If the target already exists, delete it first.
  // I will not unwind it on move failure. Although it's a weak point
  // it's better to not have it implemented then trying to create
  // a complex workaround.
  //
  if ( NULL != nde->d_inode ) {

    DebugTrace( 0, Dbg, ("rename: deleting existing target %p (r=%lx)\n", nde->d_inode, nde->d_inode->i_ino));

    dget( nde );
    err = ufsd_unlink( ndir, nde );
    dput( nde );
    if ( err ) {
      DebugTrace( -1, Dbg, ("rename -> failed to unlink target, %d\n", err));
      return err;
    }
  }

  lock_ufsd( sbi );

  // Call UFSD library
  err = ufsdapi_file_move( sbi->ufsd, UFSD_FH( odir ), UFSD_FH( ndir ), UFSD_FH( ode->d_inode ),
                          ode->d_name.name, ode->d_name.len,
                          nde->d_name.name, nde->d_name.len );
  // Translate UFSD errors
  switch( err ) {
  case 0: break;
  case ERR_NOFILEEXISTS: err = -ENOENT; break;
  case ERR_FILEEXISTS: err = -EEXIST; break;
  case ERR_NOSPC: err = -ENOSPC; break;
  default: err = -EPERM;
  }

  unlock_ufsd( sbi );

  if ( 0 != err ) {
    DebugTrace( -1, Dbg, ("rename -> failed, %d\n", err));
    return err;
  }


  // Mark dir as requiring resync.
  odir->i_version += 1;
  TIMESPEC_SECONDS( &odir->i_ctime ) = TIMESPEC_SECONDS( &odir->i_mtime ) = get_seconds();
  mark_inode_dirty_sync( odir );
  mark_inode_dirty_sync( ndir );
  dir_size = ufsdapi_get_dir_size( UFSD_FH( odir ) );
  i_size_write( odir, dir_size );
  inode_set_bytes( odir, dir_size );

  if ( ndir != odir ) {
    dir_size = ufsdapi_get_dir_size( UFSD_FH( ndir ) );

    ndir->i_version += 1;
    ndir->i_mtime  = ndir->i_ctime = odir->i_ctime;
    i_size_write( ndir, dir_size );
    inode_set_bytes( ndir, dir_size );

#ifdef UFSD_COUNT_CONTAINED
    if ( S_ISDIR( ode->d_inode->i_mode ) ) {
      assert(odir->i_nlink > 0);
      odir->i_nlink -= 1;
      ndir->i_nlink += 1;
    }
#endif
  }

  if ( NULL != ode->d_inode ) {
    ode->d_inode->i_ctime = odir->i_ctime;
    mark_inode_dirty_sync( ode->d_inode );
  }

  DebugTrace( -1, Dbg, ("rename -> %d\n", err));
  return 0;
}


#ifdef UFSD_USE_XATTR

#if defined HAVE_DECL_POSIX_ACL_TO_XATTR_V2 && HAVE_DECL_POSIX_ACL_TO_XATTR_V2
#if defined HAVE_LINUX_PROC_NS_H && HAVE_LINUX_PROC_NS_H
  #include <linux/proc_ns.h>
#endif
#include <linux/user_namespace.h>
  // wait for 'init_user_ns' to be non G.P.L.
  struct user_namespace user_ns = {
    .uid_map    = { .nr_extents = 1, .extent[0] = { .count = ~0u, }, },
    .gid_map    = { .nr_extents = 1, .extent[0] = { .count = ~0u, }, },
    .projid_map = { .nr_extents = 1, .extent[0] = { .count = ~0u, }, },
#if defined HAVE_STRUCT_USER_NAMESPACE_COUNT && HAVE_STRUCT_USER_NAMESPACE_COUNT
    .count = ATOMIC_INIT(3),
#else
    .kref = { .refcount = ATOMIC_INIT(3), },
#endif
    .owner = GLOBAL_ROOT_UID,
    .group = GLOBAL_ROOT_GID,
#if defined HAVE_STRUCT_USER_NAMESPACE_PROC_INUM && HAVE_STRUCT_USER_NAMESPACE_PROC_INUM
    .proc_inum = PROC_USER_INIT_INO,
#endif
  };
  #define Posix_acl_to_xattr( acl, buffer, size )   posix_acl_to_xattr( &user_ns, acl, buffer, size )
  #define Posix_acl_from_xattr( value, size )       posix_acl_from_xattr( &user_ns, value, size )
#else
  #define Posix_acl_to_xattr( acl, buffer, size )   posix_acl_to_xattr( acl, buffer, size )
  #define Posix_acl_from_xattr( value, size )       posix_acl_from_xattr( value, size )
#endif


///////////////////////////////////////////////////////////
// ufsd_listxattr
//
// inode_operations::listxattr
//
// Copy a list of attribute names into the buffer
// provided, or compute the buffer size required.
// buffer is NULL to compute the size of the buffer required.
//
// Returns a negative error number on failure, or the number of bytes
// used / required on success.
///////////////////////////////////////////////////////////
ssize_t
ufsd_listxattr(
    IN  struct dentry *de,
    OUT char          *buffer,
    IN  size_t        size
    )
{
  struct inode *i = de->d_inode;
  unode *u        = UFSD_U( i );
  usuper *sbi     = UFSD_SB( i->i_sb );
  ssize_t ret;

  DebugTrace( +1, Dbg, ("listxattr: r=%lx, %p, %Zu, flags = %lx\n", i->i_ino, buffer, size, u->flags ));

  lock_ufsd( sbi );

  ret = lazy_open( sbi, i );

  if ( 0 == ret ) {
    switch( ufsdapi_list_xattr( sbi->ufsd, u->ufile, buffer, size, (size_t*)&ret ) ){
    case 0                  : break; // Ok
    case ERR_NOTIMPLEMENTED : ret = -EOPNOTSUPP; break;
    case ERR_MORE_DATA      : ret = -ERANGE; break;
    default                 : ret = -EINVAL;
    }
  }

  unlock_ufsd( sbi );

  DebugTrace( -1, Dbg, ("listxattr -> %Zd\n", ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_getxattr
//
// Helper function
///////////////////////////////////////////////////////////
static int
ufsd_getxattr(
    IN  struct inode  *i,
    IN  const char    *name,
    OUT void          *value,
    IN  size_t        size,
    OUT size_t        *required
    )
{
  unode *u    = UFSD_U( i );
  usuper *sbi = UFSD_SB( i->i_sb );
  int ret;
  size_t len;

  if ( NULL != u->ufile && !is_xattr( u ) )
    return -ENODATA;

  DebugTrace( +1, Dbg, ("getxattr: r=%lx, \"%s\", %p, %Zu\n", i->i_ino, name, value, size ));

  if ( NULL == required )
    lock_ufsd( sbi );

  ret = lazy_open( sbi, i );

  if ( 0 == ret ) {
    switch( ufsdapi_get_xattr( sbi->ufsd, u->ufile,
                              name, strlen(name), value, size, &len ) ) {
    case 0                  : ret = (int)len; break;  // Ok
    case ERR_NOTIMPLEMENTED : ret = -EOPNOTSUPP; break;
    case ERR_MORE_DATA      : ret = -ERANGE; if ( NULL != required ) *required = len; break;
    case ERR_NOFILEEXISTS   : ret = -ENODATA; break;
    default                 : ret = -EINVAL;
    }
  }

  if ( NULL == required )
    unlock_ufsd( sbi );

  DebugTrace( -1, Dbg, ("getxattr -> %d\n", ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_setxattr
//
// Helper function
///////////////////////////////////////////////////////////
static int
ufsd_setxattr(
    IN struct inode *i,
    IN const char   *name,
    IN const void   *value,
    IN size_t       size,
    IN int          flags,
    IN int          locked
    )
{
  unode *u    = UFSD_U( i );
  usuper *sbi = UFSD_SB( i->i_sb );
  int ret;

  DebugTrace( +1, Dbg, ("setxattr: r=%lx \"%s\", %p, %Zu, %d\n",
                        i->i_ino, name, value, size, flags ));

  if ( !locked )
    lock_ufsd( sbi );

  ret = lazy_open( sbi, i );

  if ( 0 == ret ) {
    switch( ufsdapi_set_xattr( sbi->ufsd, u->ufile,
                              name, strlen(name), value, size,
                              0 != (flags & XATTR_CREATE),
                              0 != (flags & XATTR_REPLACE) ) ) {
    case 0:
      // Check if we delete the last xattr ( 0 == size && XATTR_REPLACE == flags && no xattrs )
      if ( 0 != size
        || XATTR_REPLACE != flags
        || 0 != ufsdapi_list_xattr( sbi->ufsd, u->ufile, NULL, 0, &size )
        || 0 != size ) {
        set_bit( UFSD_UNODE_FLAG_EA_BIT, &u->flags );
      } else {
        clear_bit( UFSD_UNODE_FLAG_EA_BIT, &u->flags );
        DebugTrace( 0, Dbg, ("setxattr: (removed last extended attribute)\n" ));
      }
      ret = 0;
      break;  // Ok
    case ERR_NOTIMPLEMENTED : ret = -EOPNOTSUPP; break;
    case ERR_NOFILEEXISTS   : ret = -ENODATA; break;
    default                 : ret = -EINVAL;
    }
  }

  if ( !locked )
    unlock_ufsd( sbi );

  DebugTrace( -1, Dbg, ("setxattr -> %d\n", ret ));
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_posix_acl_release
//
//
///////////////////////////////////////////////////////////
static inline void
ufsd_posix_acl_release(
    IN struct posix_acl *acl
    )
{
  assert( NULL != acl );
  if ( NULL != acl && atomic_dec_and_test( &acl->a_refcount ) )
    kfree( acl );
}


///////////////////////////////////////////////////////////
// ufsd_get_acl_ex
//
// Helper function for ufsd_get_acl
///////////////////////////////////////////////////////////
static struct posix_acl*
ufsd_get_acl_ex(
    IN struct inode *i,
    IN int          type,
    IN int          locked
    )
{
  const char *name;
  struct posix_acl *acl, **p;
  size_t req;
  int ret;
  usuper *sbi = UFSD_SB( i->i_sb );
  unode *u    = UFSD_U( i );

  assert( sbi->options.acl );

  switch ( type ) {
#if defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL
  case ACL_TYPE_ACCESS:   p = &i->i_acl; break;
  case ACL_TYPE_DEFAULT:  p = &i->i_default_acl; break;
#else
  case ACL_TYPE_ACCESS:   p = &u->acl; break;
  case ACL_TYPE_DEFAULT:  p = &u->default_acl; break;
#endif
  default:  return ERR_PTR(-EINVAL);
  }

  //
  // Check cached value of 'acl' and 'default_acl'
  //
  spin_lock( &i->i_lock );
  acl = *p;
  if ( ACL_NOT_CACHED != acl )
    acl = posix_acl_dup( acl );
  else if ( NULL != u->ufile && !is_xattr(u) )
    acl = NULL;
  spin_unlock( &i->i_lock );

  if ( ACL_NOT_CACHED != acl )
    return acl;

  //
  // Possible values of 'type' was already checked above
  //
  name = ACL_TYPE_ACCESS == type? POSIX_ACL_XATTR_ACCESS : POSIX_ACL_XATTR_DEFAULT;

  if ( !locked )
    lock_ufsd( sbi );

  //
  // Get the size of extended attribute
  //
  ret = ufsd_getxattr( i, name, sbi->x_buffer, sbi->bytes_per_xbuffer, &req );

  if ( (ret > 0 && NULL == sbi->x_buffer) || -ERANGE == ret ) {

    //
    // Allocate/Reallocate buffer and read again
    //
    if ( NULL != sbi->x_buffer ) {
      assert( -ERANGE == ret );
      kfree( sbi->x_buffer );
    }

    if ( ret > 0 )
      req = ret;

    sbi->x_buffer = kmalloc( req, GFP_KERNEL );
    if ( NULL != sbi->x_buffer ) {
      sbi->bytes_per_xbuffer = req;

      //
      // Read the extended attribute.
      //
      ret = ufsd_getxattr( i, name, sbi->x_buffer, sbi->bytes_per_xbuffer, &req );
      assert( ret > 0 );

    } else {
      ret = -ENOMEM;
      sbi->bytes_per_xbuffer = 0;
    }
  }

  if ( !locked )
    unlock_ufsd( sbi );

  //
  // Translate extended attribute to acl
  //
  if ( ret > 0 ) {
    acl = Posix_acl_from_xattr( sbi->x_buffer, ret );
    if ( !IS_ERR( acl ) ) {
      struct posix_acl *old;
      spin_lock( &i->i_lock );
      old = *p;
      *p  = posix_acl_dup( acl );
      spin_unlock( &i->i_lock );
      if ( ACL_NOT_CACHED != old )
        ufsd_posix_acl_release( old );
    }
  } else {
    acl = -ENODATA == ret || -ENOSYS == ret ? NULL : ERR_PTR( ret );
  }

  return acl;
}


///////////////////////////////////////////////////////////
// ufsd_get_acl
//
// inode_operations::get_acl
// inode->i_mutex: don't care
///////////////////////////////////////////////////////////
static struct posix_acl*
ufsd_get_acl(
    IN struct inode *i,
    IN int          type
    )
{
  return ufsd_get_acl_ex( i, type, 0 );
}


///////////////////////////////////////////////////////////
// ufsd_set_acl_ex
//
// Helper function
///////////////////////////////////////////////////////////
static int
ufsd_set_acl_ex(
    IN struct inode     *i,
    IN int              type,
    IN struct posix_acl *acl,
    IN int              locked
    )
{
  const char *name;
  void *value = NULL;
  size_t size = 0;
  int err     = 0;

  if ( S_ISLNK( i->i_mode ) )
    return -EOPNOTSUPP;

  assert( UFSD_SB( i->i_sb )->options.acl );

  switch( type ) {
    case ACL_TYPE_ACCESS:
      if ( NULL != acl ) {
        posix_acl_mode mode = i->i_mode;
        err = posix_acl_equiv_mode( acl, &mode );
        if ( err < 0 )
          return err;

        if ( i->i_mode != mode ) {
          i->i_mode = mode;
          mark_inode_dirty_sync( i );
          set_bit( UFSD_UNODE_FLAG_SET_MODE_BIT, &UFSD_U(i)->flags );
        }
        if ( 0 == err )
          acl = NULL; // acl can be exactly represented in the traditional file mode permission bits
      }
      name = POSIX_ACL_XATTR_ACCESS;
      break;

    case ACL_TYPE_DEFAULT:
      if ( !S_ISDIR( i->i_mode ) )
        return acl ? -EACCES : 0;
      name = POSIX_ACL_XATTR_DEFAULT;
      break;

    default:
      return -EINVAL;
  }

  if ( NULL != acl ) {
    size  = posix_acl_xattr_size( acl->a_count );
    value = kmalloc( size, GFP_KERNEL );
    if ( NULL == value )
      return -ENOMEM;

    err = Posix_acl_to_xattr( acl, value, size );
    if ( err < 0 )
      return err;
  }

  err = ufsd_setxattr( i, name, value, size, 0, locked );
  if ( 0 == err ) {
#if defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL
    struct posix_acl **p  = ACL_TYPE_ACCESS == type? &i->i_acl : &i->i_default_acl;
#else
    unode *u = UFSD_U(i);
    struct posix_acl **p  = ACL_TYPE_ACCESS == type? &u->acl : &u->default_acl;
#endif
    struct posix_acl *old;

    spin_lock( &i->i_lock );
    old = *p;
    *p  = posix_acl_dup( acl );
    spin_unlock( &i->i_lock );
    if ( ACL_NOT_CACHED != old )
      ufsd_posix_acl_release( old );
  }

  kfree( value );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_set_acl
//
///////////////////////////////////////////////////////////
static int
ufsd_set_acl(
    IN struct inode     *i,
    IN int              type,
    IN struct posix_acl *acl
    )
{
  return ufsd_set_acl_ex( i, type, acl, 0 );
}


#if defined HAVE_DECL_GENERIC_PERMISSION_V3 && HAVE_DECL_GENERIC_PERMISSION_V3

///////////////////////////////////////////////////////////
// inode_operations::permission = generic_permission
//
// generic_permission does not require 'check_acl'
///////////////////////////////////////////////////////////
#define ufsd_permission generic_permission

#else

///////////////////////////////////////////////////////////
// ufsd_check_acl
//
// Helper function for ufsd_permission
///////////////////////////////////////////////////////////
static int
ufsd_check_acl(
    IN struct inode   *i,
    IN int            mask
#ifdef IPERM_FLAG_RCU
    , IN unsigned int flags
#endif
    )
{
  int err;
  struct posix_acl *acl;

  assert( UFSD_SB( i->i_sb )->options.acl );

#ifdef IPERM_FLAG_RCU
  if ( flags & IPERM_FLAG_RCU ) {
    if ( !negative_cached_acl( i, ACL_TYPE_ACCESS ) )
      return -ECHILD;
    return -EAGAIN;
  }
#endif

  acl = ufsd_get_acl( i, ACL_TYPE_ACCESS );
  if ( IS_ERR( acl ) )
    return PTR_ERR( acl );

  if ( NULL == acl )
    return -EAGAIN;

  //
  // Trace acl
  //
#if 0//def UFSD_DEBUG
  {
    int n;
    for ( n = 0; n < acl->a_count; n++ ) {
      DebugTrace( 0, Dbg, ("e_tag=%x, e_perm=%x e_id=%x\n", (unsigned)acl->a_entries[n].e_tag, (unsigned)acl->a_entries[n].e_perm, (unsigned)acl->a_entries[n].e_id ));
    }
  }
#endif

  err = posix_acl_permission( i, acl, mask );
  ufsd_posix_acl_release( acl );

  DebugTrace( 0, Dbg, ("check_acl (r=%lx, m=%o) -> %d\n", i->i_ino, mask, err) );

  return err;
}


#ifdef IPERM_FLAG_RCU
///////////////////////////////////////////////////////////
// ufsd_permission
//
// inode_operations::permission
///////////////////////////////////////////////////////////
static int
ufsd_permission(
    IN struct inode *i,
    IN int          mask,
    IN unsigned int flag
    )
{
  return generic_permission( i, mask, flag, ufsd_check_acl );
}
#else
///////////////////////////////////////////////////////////
// ufsd_permission
//
// inode_operations::permission
///////////////////////////////////////////////////////////
static int
ufsd_permission(
    IN struct inode *i,
    IN int          mask
#if defined HAVE_DECL_INOP_PERMISSION_V1 & HAVE_DECL_INOP_PERMISSION_V1
    , IN struct nameidata *nd
#endif
#if defined HAVE_DECL_INOP_PERMISSION_V2 & HAVE_DECL_INOP_PERMISSION_V2
    , IN unsigned int ui
#endif
    )
{
  return generic_permission( i, mask, ufsd_check_acl );
}
#endif // #ifdef IPERM_FLAG_RCU
#endif // #if defined HAVE_DECL_GENERIC_PERMISSION_V3 && HAVE_DECL_GENERIC_PERMISSION_V3


///////////////////////////////////////////////////////////
// ufsd_acl_chmod
//
//
///////////////////////////////////////////////////////////
static int
ufsd_acl_chmod(
    IN struct inode *i
    )
{
#if !( defined HAVE_DECL_POSIX_ACL_CHMOD_V2 && HAVE_DECL_POSIX_ACL_CHMOD_V2 )
  struct posix_acl *acl;
#endif
  int err;

  if ( !UFSD_SB( i->i_sb )->options.acl )
    return 0;

  if ( S_ISLNK( i->i_mode ) )
    return -EOPNOTSUPP;

  DebugTrace( +1, Dbg, ("acl_chmod r=%lx\n", i->i_ino));
#if defined HAVE_DECL_POSIX_ACL_CHMOD_V2 && HAVE_DECL_POSIX_ACL_CHMOD_V2
  err = posix_acl_chmod( i, i->i_mode );
#else
  acl = ufsd_get_acl( i, ACL_TYPE_ACCESS );
  if ( IS_ERR( acl ) || !acl )
    err = PTR_ERR( acl );
  else {
#if defined HAVE_DECL_POSIX_ACL_CHMOD_V1 && HAVE_DECL_POSIX_ACL_CHMOD_V1
    err = posix_acl_chmod( &acl, GFP_KERNEL, i->i_mode );
    if ( err ) {
      DebugTrace( -1, Dbg, ("acl_chmod -> %d\n", err));
      return err;
    }
    err = ufsd_set_acl( i, ACL_TYPE_ACCESS, acl );
    ufsd_posix_acl_release( acl );
#else
    struct posix_acl *clone = posix_acl_clone( acl, GFP_KERNEL );
    ufsd_posix_acl_release( acl );
    if ( NULL == clone )
      err = -ENOMEM;
    else {
      err = posix_acl_chmod_masq( clone, i->i_mode );
      if ( 0 == err )
        err = ufsd_set_acl( i, ACL_TYPE_ACCESS, clone );
      ufsd_posix_acl_release( clone );
    }
#endif
  }
#endif

  DebugTrace( -1, Dbg, ("acl_chmod -> %d\n", err));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_get_acl
//
// Helper function for ufsd_xattr_acl_access_get/ufsd_xattr_acl_default_get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_get_acl(
    IN struct inode *i,
    IN int          type,
    OUT void        *buffer,
    IN size_t       size
    )
{
  struct posix_acl *acl;
  int err;

  if ( !UFSD_SB( i->i_sb )->options.acl )
    return -EOPNOTSUPP;

  acl = ufsd_get_acl( i, type );
  if ( IS_ERR( acl ) )
    return PTR_ERR( acl );

  if ( NULL == acl )
    return -ENODATA;

  err = Posix_acl_to_xattr( acl, buffer, size );
  ufsd_posix_acl_release( acl );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_set_acl
//
// Helper function for ufsd_xattr_acl_access_set/ufsd_xattr_acl_default_set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_set_acl(
    IN struct inode *i,
    IN int          type,
    IN const void   *value,
    IN size_t       size
    )
{
  struct posix_acl *acl;
  int err;

  if ( !UFSD_SB( i->i_sb )->options.acl )
    return -EOPNOTSUPP;

  if ( !is_owner_or_cap( i ) )
    return -EPERM;

  if ( NULL == value )
    acl = NULL;
  else {
    acl = Posix_acl_from_xattr( value, size );
    if ( IS_ERR( acl ) )
      return PTR_ERR(acl);

    if ( NULL != acl ) {
      err = posix_acl_valid( acl );
      if ( err )
        goto release_and_out;
    }
  }

  err = ufsd_set_acl( i, type, acl );

release_and_out:
  ufsd_posix_acl_release( acl );
  return err;
}


#if defined HAVE_DECL_XATTR_HANDLER_V2 & HAVE_DECL_XATTR_HANDLER_V2

  #define DECLARE_XATTR_LIST_ARG  struct dentry *de, char *list, size_t list_size, const char *name, size_t name_len, int handler_flags
  #define DECLARE_XATTR_GET_ARG   struct dentry *de, const char *name, void *buffer, size_t size, int handler_flags
  #define DECLARE_XATTR_SET_ARG   struct dentry *de, const char *name, const void *value, size_t size, int flags, int handler_flags
  #define DECLARE_XATTR_INODE     struct inode *i = de->d_inode;

#else

  #define DECLARE_XATTR_LIST_ARG  struct inode *i, char *list, size_t list_size, const char *name, size_t name_len
  #define DECLARE_XATTR_GET_ARG   struct inode *i, const char *name, void *buffer, size_t size
  #define DECLARE_XATTR_SET_ARG   struct inode *i, const char *name, const void *value, size_t size, int flags
  #define DECLARE_XATTR_INODE

#endif

///////////////////////////////////////////////////////////
// ufsd_xattr_acl_access_list
//
// ufsd_xattr_acl_access_handler::list
///////////////////////////////////////////////////////////
static size_t
ufsd_xattr_acl_access_list(
    DECLARE_XATTR_LIST_ARG
    )
{
  DECLARE_XATTR_INODE
  if ( !UFSD_SB( i->i_sb )->options.acl )
    return 0;
  if ( NULL != list && list_size >= sizeof(POSIX_ACL_XATTR_ACCESS) )
    memcpy( list, POSIX_ACL_XATTR_ACCESS, sizeof(POSIX_ACL_XATTR_ACCESS) );
  return sizeof(POSIX_ACL_XATTR_ACCESS);
}


///////////////////////////////////////////////////////////
// ufsd_xattr_acl_access_get
//
// ufsd_xattr_acl_access_handler::get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_acl_access_get(
    DECLARE_XATTR_GET_ARG
    )
{
  int ret;
  DECLARE_XATTR_INODE

  DebugTrace( +1, Dbg, ("acl_access_get: r=%lx, sz=%Zu\n", i->i_ino, size ));

  ret = 0 != name[0]
    ? -EINVAL
    : ufsd_xattr_get_acl( i, ACL_TYPE_ACCESS, buffer, size );

  DebugTrace( -1, Dbg, ("acl_access_get -> %d\n", ret ));

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_acl_access_set
//
// ufsd_xattr_acl_access_handler::set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_acl_access_set(
    DECLARE_XATTR_SET_ARG
    )
{
  int ret;
  DECLARE_XATTR_INODE

  DebugTrace( +1, Dbg, ("acl_access_set: r=%lx, sz=%Zu, fl=%d\n", i->i_ino, size, flags ));

  ret = 0 != name[0]
    ? -EINVAL
    : ufsd_xattr_set_acl( i, ACL_TYPE_ACCESS, value, size );

  DebugTrace( -1, Dbg, ("acl_access_set -> %d\n", ret ));

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_acl_default_list
//
// ufsd_xattr_acl_default_handler::list
///////////////////////////////////////////////////////////
static size_t
ufsd_xattr_acl_default_list(
    DECLARE_XATTR_LIST_ARG
    )
{
  DECLARE_XATTR_INODE
  if ( !UFSD_SB( i->i_sb )->options.acl )
    return 0;
  if ( NULL != list && list_size >= sizeof(POSIX_ACL_XATTR_DEFAULT) )
    memcpy( list, POSIX_ACL_XATTR_DEFAULT, sizeof(POSIX_ACL_XATTR_DEFAULT) );
  return sizeof(POSIX_ACL_XATTR_DEFAULT);
}


///////////////////////////////////////////////////////////
// ufsd_xattr_acl_default_get
//
// ufsd_xattr_acl_default_handler::get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_acl_default_get(
    DECLARE_XATTR_GET_ARG
    )
{
  int ret;
  DECLARE_XATTR_INODE

  DebugTrace( +1, Dbg, ("acl_default_get: r=%lx, sz=%Zu\n", i->i_ino, size ));

  ret = 0 != name[0]
    ? -EINVAL
    : ufsd_xattr_get_acl( i, ACL_TYPE_DEFAULT, buffer, size );

  DebugTrace( -1, Dbg, ("acl_default_get -> %d\n", ret ));

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_acl_default_set
//
// ufsd_xattr_acl_default_handler::set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_acl_default_set(
    DECLARE_XATTR_SET_ARG
    )
{
  int ret;
  DECLARE_XATTR_INODE

  DebugTrace( +1, Dbg, ("acl_default_set: r=%lx, sz=%Zu, fl=%d\n", i->i_ino, size, flags ));

  ret = 0 != name[0]
    ? -EINVAL
    : ufsd_xattr_set_acl( i, ACL_TYPE_DEFAULT, value, size );

  DebugTrace( -1, Dbg, ("acl_default_set -> %d\n", ret ));

  return ret;
}

#ifndef XATTR_USER_PREFIX_LEN
  #define XATTR_USER_PREFIX "user."
  #define XATTR_USER_PREFIX_LEN (sizeof (XATTR_USER_PREFIX) - 1)
#endif

///////////////////////////////////////////////////////////
// ufsd_xattr_user_list
//
// ufsd_xattr_user_handler::list
///////////////////////////////////////////////////////////
static size_t
ufsd_xattr_user_list(
    DECLARE_XATTR_LIST_ARG
    )
{
  const size_t prefix_len = XATTR_USER_PREFIX_LEN;
  const size_t total_len = prefix_len + name_len + 1;
  DECLARE_XATTR_INODE

  if ( !UFSD_SB( i->i_sb )->options.user_xattr )
    return 0;

  if ( NULL != list && total_len <= list_size ) {
    memcpy( list, XATTR_USER_PREFIX, prefix_len );
    memcpy( list+prefix_len, name, name_len );
    list[prefix_len + name_len] = 0;
  }
  return total_len;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_user_get
//
// ufsd_xattr_user_handler::get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_user_get(
    DECLARE_XATTR_GET_ARG
    )
{
  int ret;
  DECLARE_XATTR_INODE

  ret = 0 == name[0]
    ? -EINVAL
    : !UFSD_SB( i->i_sb )->options.user_xattr
    ? -EOPNOTSUPP
    : ufsd_getxattr( i, name - XATTR_USER_PREFIX_LEN, buffer, size, NULL );

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_user_set
//
// ufsd_xattr_user_handler::set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_user_set(
    DECLARE_XATTR_SET_ARG
    )
{
  DECLARE_XATTR_INODE

  return 0 == name[0]
    ? -EINVAL
    : !UFSD_SB( i->i_sb )->options.user_xattr
    ? -EOPNOTSUPP
    : ufsd_setxattr( i, name - XATTR_USER_PREFIX_LEN, value, size, flags, 0 );
}

#ifndef XATTR_TRUSTED_PREFIX_LEN
  #define XATTR_TRUSTED_PREFIX "trusted."
  #define XATTR_TRUSTED_PREFIX_LEN (sizeof (XATTR_TRUSTED_PREFIX) - 1)
#endif

///////////////////////////////////////////////////////////
// ufsd_xattr_trusted_list
//
// ufsd_xattr_trusted_handler::list
///////////////////////////////////////////////////////////
static size_t
ufsd_xattr_trusted_list(
    DECLARE_XATTR_LIST_ARG
    )
{
  const int prefix_len    = XATTR_TRUSTED_PREFIX_LEN;
  const size_t total_len  = prefix_len + name_len + 1;

  if ( !capable( CAP_SYS_ADMIN ) )
    return 0;

  if ( NULL != list && total_len <= list_size ) {
    memcpy( list, XATTR_TRUSTED_PREFIX, prefix_len );
    memcpy( list+prefix_len, name, name_len );
    list[prefix_len + name_len] = 0;
  }
  return total_len;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_trusted_get
//
// ufsd_xattr_trusted_handler::get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_trusted_get(
    DECLARE_XATTR_GET_ARG
    )
{
  DECLARE_XATTR_INODE

  return 0 == name[0]? -EINVAL : ufsd_getxattr( i, name - XATTR_TRUSTED_PREFIX_LEN, buffer, size, NULL );
}


///////////////////////////////////////////////////////////
// ufsd_xattr_trusted_set
//
// ufsd_xattr_trusted_handler::set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_trusted_set(
    DECLARE_XATTR_SET_ARG
    )
{
  DECLARE_XATTR_INODE

  return 0 == name[0]? -EINVAL : ufsd_setxattr( i, name - XATTR_TRUSTED_PREFIX_LEN, value, size, flags, 0 );
}

#ifndef XATTR_SECURITY_PREFIX_LEN
  #define XATTR_SECURITY_PREFIX "security."
  #define XATTR_SECURITY_PREFIX_LEN (sizeof (XATTR_SECURITY_PREFIX) - 1)
#endif

///////////////////////////////////////////////////////////
// ufsd_xattr_security_list
//
// ufsd_xattr_security_handler::list
///////////////////////////////////////////////////////////
static size_t
ufsd_xattr_security_list(
    DECLARE_XATTR_LIST_ARG
    )
{
  const int prefix_len    = XATTR_SECURITY_PREFIX_LEN;
  const size_t total_len  = prefix_len + name_len + 1;

  if ( NULL != list && total_len <= list_size ) {
    memcpy( list, XATTR_SECURITY_PREFIX, prefix_len );
    memcpy( list+prefix_len, name, name_len );
    list[prefix_len + name_len] = 0;
  }
  return total_len;
}


///////////////////////////////////////////////////////////
// ufsd_xattr_security_get
//
// ufsd_xattr_security_handler::get
///////////////////////////////////////////////////////////
static int
ufsd_xattr_security_get(
    DECLARE_XATTR_GET_ARG
    )
{
  DECLARE_XATTR_INODE

  return 0 == name[0]? -EINVAL : ufsd_getxattr( i, name - XATTR_SECURITY_PREFIX_LEN, buffer, size, NULL );
}


///////////////////////////////////////////////////////////
// ufsd_xattr_security_set
//
// ufsd_xattr_security_handler::set
///////////////////////////////////////////////////////////
static int
ufsd_xattr_security_set(
    DECLARE_XATTR_SET_ARG
    )
{
  DECLARE_XATTR_INODE

  return 0 == name[0]? -EINVAL : ufsd_setxattr( i, name - XATTR_SECURITY_PREFIX_LEN, value, size, flags, 0 );
}


static const struct xattr_handler ufsd_xattr_acl_access_handler = {
  .prefix = POSIX_ACL_XATTR_ACCESS,
  .list   = ufsd_xattr_acl_access_list,
  .get    = ufsd_xattr_acl_access_get,
  .set    = ufsd_xattr_acl_access_set,
};


static const struct xattr_handler ufsd_xattr_acl_default_handler = {
  .prefix = POSIX_ACL_XATTR_DEFAULT,
  .list   = ufsd_xattr_acl_default_list,
  .get    = ufsd_xattr_acl_default_get,
  .set    = ufsd_xattr_acl_default_set,
};


static const struct xattr_handler ufsd_xattr_user_handler = {
  .prefix = XATTR_USER_PREFIX,
  .list   = ufsd_xattr_user_list,
  .get    = ufsd_xattr_user_get,
  .set    = ufsd_xattr_user_set,
};


static const struct xattr_handler ufsd_xattr_trusted_handler = {
  .prefix = XATTR_TRUSTED_PREFIX,
  .list   = ufsd_xattr_trusted_list,
  .get    = ufsd_xattr_trusted_get,
  .set    = ufsd_xattr_trusted_set,
};


static const struct xattr_handler ufsd_xattr_security_handler = {
  .prefix = XATTR_SECURITY_PREFIX,
  .list   = ufsd_xattr_security_list,
  .get    = ufsd_xattr_security_get,
  .set    = ufsd_xattr_security_set,
};


//
// xattr operations
// super_block::s_xattr
//
static const struct xattr_handler *ufsd_xattr_handlers[] = {
  &ufsd_xattr_user_handler,
  &ufsd_xattr_trusted_handler,
  &ufsd_xattr_acl_access_handler,
  &ufsd_xattr_acl_default_handler,
  &ufsd_xattr_security_handler,
  NULL
};

#endif // #ifdef UFSD_USE_XATTR

///////////////////////////////////////////////////////////
// ufsd_lookup
//
// inode_operations::lookup
//
//  This routine is a callback used to load inode for a
//  direntry when this direntry was not found in dcache.
//
// dir - container inode for this operation.
//
// dentry - On entry contains name of the entry to find.
//          On exit should contain inode loaded.
//
// Return:
// struct dentry* - direntry in case of one differs from one
//     passed to me. I return NULL to indicate original direntry has been used.
//     ERRP() can also be returned to indicate error condition.
//
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_lookup(
    IN struct inode  *dir,
    IN struct dentry *de
#if defined HAVE_DECL_INOP_LOOKUP_V2 && HAVE_DECL_INOP_LOOKUP_V2
  , IN struct nameidata *nd
#elif defined HAVE_DECL_INOP_LOOKUP_V3 && HAVE_DECL_INOP_LOOKUP_V3
  , IN unsigned int nd
#endif
    )
{
  struct inode *i = NULL;
  int err = ufsd_create_or_open( dir, de, NULL, &i );

  if ( NULL != i ) {
    if ( UFSD_SB( i->i_sb )->options.nocase ) {
      struct dentry *a = d_find_alias( i );
      if ( NULL != a ) {
        if ( IS_ROOT( a ) && (a->d_flags & DCACHE_DISCONNECTED) )
          dput( a );
        else {
          BUG_ON(d_unhashed( a ));
          if ( !S_ISDIR( i->i_mode ))
            d_move( a, de );
          iput( i );
          return a;
        }
      }
    }
    return d_splice_alias( i, de );
  }

  // ENOENT is expected and will be handled by the caller.
  // (a least on some old kernels).
  if ( err && -ENOENT != err ) {
    assert(NULL == i);
    return ERR_PTR(err);
  }

  d_add( de, i );
  return NULL;
}

#if (defined UFSD_EXFAT || defined UFSD_REFS) && !defined UFSD_NTFS2 && !defined UFSD_HFS
  #define ufsd_link NULL
#else
///////////////////////////////////////////////////////////
// ufsd_link
//
// This function creates a hard link
// inode_operations::link
///////////////////////////////////////////////////////////
static int
ufsd_link(
    IN struct dentry  *ode,
    IN struct inode   *dir,
    OUT struct dentry *de
    )
{
  int err;
  struct inode *i;
  struct inode *oi = ode->d_inode;

  ucreate  cr;

  cr.lnk  = (ufsd_file*)oi; // NOTE: use inode not ufsd handle cause it may be not loaded yet
  cr.data = NULL;
  cr.len  = 0;
  cr.mode = 0;

  assert( NULL != oi );
  assert( NULL != dir && NULL != UFSD_FH(dir) );
  assert( S_ISDIR( dir->i_mode ) );
  assert( dir->i_sb == oi->i_sb );

  DebugTrace( +1, Dbg, ("link: r=%lx \"%.*s\" => r=%lx /\"%.*s\"\n",
                        oi->i_ino, (int)ode->d_name.len, ode->d_name.name,
                        dir->i_ino, (int)de->d_name.len, de->d_name.name ));

  err = ufsd_create_or_open( dir, de, &cr, &i );

  if ( 0 == err ) {
    //
    // Hard link is created
    //
    assert( i == oi );

    d_instantiate( de, i );
    inc_nlink( i );
  }

  DebugTrace( -1, Dbg, ("link -> %d\n", err ));

  return err;
}
#endif

static int
ufsd_symlink(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN const char     *symname
    );

static int
ufsd_mknod(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN Umode_t        mode,
    IN dev_t          rdev
    );


static const struct inode_operations ufsd_dir_inode_operations = {
  .lookup       = ufsd_lookup,
  .create       = ufsd_create,
  .link         = ufsd_link,
  .unlink       = ufsd_unlink,
  .symlink      = ufsd_symlink,
  .mkdir        = ufsd_mkdir,
  .rmdir        = ufsd_unlink,
  .mknod        = ufsd_mknod,
  .rename       = ufsd_rename,
  .setattr      = ufsd_setattr,
#ifdef UFSD_USE_XATTR
  .permission   = ufsd_permission,
  .setxattr     = generic_setxattr,
  .getxattr     = generic_getxattr,
  .listxattr    = ufsd_listxattr,
  .removexattr  = generic_removexattr,
#if defined HAVE_STRUCT_INODE_OPERATIONS_GET_ACL && HAVE_STRUCT_INODE_OPERATIONS_GET_ACL
  .get_acl      = ufsd_get_acl,
#endif
#endif
};


static const struct inode_operations ufsd_special_inode_operations = {
#ifdef UFSD_USE_XATTR
  .permission   = ufsd_permission,
  .setxattr     = generic_setxattr,
  .getxattr     = generic_getxattr,
  .listxattr    = ufsd_listxattr,
  .removexattr  = generic_removexattr,
#if defined HAVE_STRUCT_INODE_OPERATIONS_GET_ACL && HAVE_STRUCT_INODE_OPERATIONS_GET_ACL
  .get_acl      = ufsd_get_acl,
#endif
#endif
  .setattr      = ufsd_setattr,
};


///////////////////////////////////////////////////////////
// ufsd_mknod
//
// ufsd_dir_inode_operations::mknod
///////////////////////////////////////////////////////////
static int
ufsd_mknod(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN Umode_t        mode,
    IN dev_t          rdev
    )
{
  struct inode *i = NULL;
  int     err;
  unsigned int udev32;

  ucreate  cr;

  cr.lnk  = NULL;
  cr.data = &udev32;
  cr.len  = sizeof(udev32);
  cr.mode = mode;

  DebugTrace( +1, Dbg, ("mknod m=%o\n", mode));

  udev32 = new_encode_dev( rdev );

  err = ufsd_create_or_open( dir, de, &cr, &i );

  if ( 0 == err ) {
    init_special_inode( i, i->i_mode, rdev );
    i->i_op = &ufsd_special_inode_operations;
    mark_inode_dirty_sync( i );
    d_instantiate( de, i );
  }

  DebugTrace( -1, Dbg, ("mknod -> %d\n", err));

  return err;
}


#ifdef UFSD_CHECK_BDI
///////////////////////////////////////////////////////////
// is_bdi_ok
//
// Returns 0 if bdi is removed
///////////////////////////////////////////////////////////
static int
is_bdi_ok(
    IN struct super_block *sb
    )
{
#if defined HAVE_STRUCT_SUPER_BLOCK_S_BDI && HAVE_STRUCT_SUPER_BLOCK_S_BDI
  if ( UFSD_SB( sb )->bdi != sb->s_bdi )
#else
  if ( NULL == UFSD_SB( sb )->bdi->dev )
#endif
  {
    printk( KERN_CRIT QUOTED_UFSD_DEVICE": media removed\n" );
    return 0;
  }
  return 1;
}
#endif


///////////////////////////////////////////////////////////
// ufsd_file_aio_read
//
// file_operations::aio_read
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_aio_read(
    IN struct kiocb       *iocb,
    IN const struct iovec *iov,
    IN unsigned long       nr_segs,
    IN loff_t              pos
    )
{
  ssize_t err;
#if defined UFSD_NTFS2 || defined UFSD_TRACE
  struct file *file = iocb->ki_filp;
  struct inode *i   = file->f_mapping->host;
  unode *u          = UFSD_U( i );
#endif
#ifdef UFSD_NTFS2
  const unsigned char* p;
#endif

#ifdef UFSD_NTFS2
  if ( is_encrypted( u ) ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("file_aio_read: r=%lx. Attempt to read from encrypted file\n", i->i_ino ));
    return -ENOSYS;
  }
#endif

  DebugTrace( +1, Dbg, ("file_aio_read: r=%lx, %llx, %Zx, sz=%llx,%llx\n", i->i_ino, pos, iov_length( iov, nr_segs ), u->valid, i->i_size ));

#ifdef UFSD_NTFS2
  if ( NULL != ( p = is_stream( file ) ) ) {
    // Read stream
    usuper* sbi   = UFSD_SB( i->i_sb );
    unsigned long seg;
    struct qstr* s  = &file->f_dentry->d_name;
    int len = s->name + s->len - p;
    size_t count;         // after file limit checks
    size_t read;

    DebugTrace( +1, Dbg, ("file_aio_read(s): r=%lx (:%.*s)\n", i->i_ino, len, p ));

    count = 0;
    err   = generic_segment_checks( iov, &nr_segs, &count, VERIFY_WRITE );
    if ( err )
      goto out_stream;

    if ( unlikely( file->f_flags & O_DIRECT ) ) {
      DebugTrace( 0, Dbg, ("does not support direct I/O for streams\n" ));
      err = -EOPNOTSUPP;
      goto out_stream;
    }

    //
    // Write 'count' bytes via sbi->rw_buffer
    //
    lock_ufsd( sbi );
    read = 0;

    //
    // Allocate helper buffer if not
    //
    if ( NULL == sbi->rw_buffer ) {
      sbi->rw_buffer = vmalloc( RW_BUFFER_SIZE );
      if ( NULL == sbi->rw_buffer ){
        err = -ENOMEM;
        goto end_cycle;
      }
    }

    for ( seg = 0; seg < nr_segs && read < count; seg++ ) {
      char __user *buf = iov[seg].iov_base;
      size_t iov_len  = min_t( size_t, iov[seg].iov_len, count - read );

      while( 0 != iov_len ) {

        size_t to_read = min_t( size_t, RW_BUFFER_SIZE - (((size_t)pos) & ( RW_BUFFER_SIZE - 1 )), iov_len );
        size_t read2;

        err = ufsdapi_file_read( sbi->ufsd, u->ufile, p, len,
                                 pos, to_read, sbi->rw_buffer, &read2 );

        if ( 0 == err && 0 != copy_to_user( buf, sbi->rw_buffer, read2 ) ) {
          err = -EFAULT;
          goto end_cycle; // double break
        } else if ( ERR_NOFILEEXISTS == err ) {
          err = -ENOENT;
          goto end_cycle; // double break
        } else if ( 0 != err ) {
          err = -EIO;
          goto end_cycle; // double break
        }

        read    += read2;
        pos     += read2;
        buf     += read2;
        iov_len -= read2;

        if ( read2 < to_read )
          break;
      } // while( 0 != iov_len )
    } // for ( seg = 0; seg < nr_segs && read < count; seg++ )

end_cycle:
    unlock_ufsd( sbi );

    // Update file position
    iocb->ki_pos = pos;

    if ( unlikely( 0 != read ) )
      err = read;

out_stream:
    DebugTrace( -1, Dbg, ("file_aio_read(s) => %Zx\n", err ));
  } else
#endif // #ifdef UFSD_NTFS2

  {
    err = generic_file_aio_read( iocb, iov, nr_segs, pos );
  }

  if ( err >= 0 ){
    DebugTrace( -1, Dbg, ("file_aio_read -> %Zx\n", (size_t)err));
  } else {
    DebugTrace( -1, Dbg, ("file_aio_read -> error %d\n", (int)err));
  }
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_extend_initialized_size
//
//
///////////////////////////////////////////////////////////
static int
ufsd_extend_initialized_size(
    IN struct inode*  i,
    IN loff_t         size
    )
{
  int err;
  unsigned long flags;
  pgoff_t index, end_index;
  loff_t valid, i_bytes, i_size = i->i_size;
  struct address_space* mapping = i->i_mapping;
  unode *u  = UFSD_U( i );

  assert( mutex_is_locked( &i->i_mutex ) );

  read_lock_irqsave( &u->valid_lock, flags );
//  i_size  = i->i_size;
  valid   = u->valid;
  read_unlock_irqrestore( &u->valid_lock, flags );

  DebugTrace( +1, Dbg, ("extend_initialized_size r=%lx, sz=[%llx,%llx,%llx] => %llx\n", i->i_ino, valid, i_size, inode_get_bytes( i ), size ));

  if ( unlikely( valid >= size ) ) {
    DebugTrace( -1, Dbg, ("extend_initialized_size => 0, nothing\n" ));
    return 0;
  }

  i_bytes = inode_get_bytes( i );
  assert( 0 == ( i_bytes & UFSD_SB( i->i_sb )->cluster_mask ) );
  assert( i_bytes >= i->i_size );

  BUG_ON( size >= i_bytes );
  if ( size > i_size )
    i_size_write( i, size );

  end_index = PAGE_CACHE_IDX( size );
  index     = valid >> PAGE_CACHE_SHIFT;
  do {
    struct page* page = read_cache_page( mapping, index, (filler_t*)mapping->a_ops->readpage, NULL );

    if ( IS_ERR( page ) ) {
      err = PTR_ERR( page );
//      if ( -ENOMEM != err && NULL != mapping->host )
//        SetFlag( UFSD_SB( mapping->host->i_sb )->_c4, 2 );
error:
      write_lock_irqsave( &u->valid_lock, flags );
      u->valid = valid;
      write_unlock_irqrestore( &u->valid_lock, flags );
      // Undo 'i_size_write'
      ufsd_set_size( i, size, i_size );
      DebugTrace( -1, Dbg, ("extend_initialized_size => %d, v=%llx, s=%llx, sz=%llx\n", err, valid, size, i_size ));
      ufsd_printk( i->i_sb, "failed to extend initialized size of inode 0x%lx (error %d).", i->i_ino, err );
      return err;
    }

    if ( PageError( page ) ) {
//      if ( NULL != mapping->host )
//        SetFlag( UFSD_SB( mapping->host->i_sb )->_c4, 2 );
      page_cache_release( page  );
      err = -EIO;
      goto error;
    }

    index += 1;
    write_lock_irqsave( &u->valid_lock, flags );
    u->valid = (loff_t)index << PAGE_CACHE_SHIFT;
    if ( u->valid > size )
      u->valid = size;
    write_unlock_irqrestore( &u->valid_lock, flags );

    mark_page_accessed( page );
    set_page_dirty( page );
    page_cache_release( page );

    balance_dirty_pages_ratelimited( mapping );
    might_sleep();

  } while( index < end_index );

//  SetFlag( unode->_d8, 8 );
  mark_inode_dirty_sync( i );
  DebugTrace( -1, Dbg, ("extend_initialized_size (update valid size ) %llx, %llx\n", u->valid, size ));
  return 0;
}


///////////////////////////////////////////////////////////
// __ufsd_file_aio_write
//
// Helper function for ufsd_file_aio_write
// based on
// 'mm\filemap.c: __generic_file_aio_write
///////////////////////////////////////////////////////////
static ssize_t
__ufsd_file_aio_write(
    IN struct kiocb       *iocb,
    IN const struct iovec *iov,
    IN unsigned long      nr_segs,
    IN OUT loff_t         *ppos
    )
{
  ssize_t err;
  struct file *file = iocb->ki_filp;
  struct address_space * mapping = file->f_mapping;
  struct inode *i = mapping->host;
  unode *u      = UFSD_U( i );
  usuper* sbi   = UFSD_SB( i->i_sb );
  size_t ocount = 0;    // original count
  size_t count;         // after file limit checks
  loff_t    end_aligned, valid, i_bytes, pos;
  ssize_t   written;
  int dirty = 0;
  unsigned long flags;
#ifdef UFSD_NTFS2
  const unsigned char* p;
#endif

#ifdef UFSD_NTFS2
  if ( is_encrypted( u ) ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("file_write: r=%lx. Attempt to write to encrypted file\n", i->i_ino ));
    return -ENOSYS;
  }
#endif

#ifdef UFSD_CHECK_BDI
  if ( !is_bdi_ok( i->i_sb ) )
    return -ENODEV;
#endif

  err = generic_segment_checks( iov, &nr_segs, &ocount, VERIFY_READ );
  if ( unlikely( err ) )
    return err;

  count = ocount;
  pos   = *ppos;

#ifdef vfs_check_frozen
  // 3.5--
  vfs_check_frozen( i->i_sb, SB_FREEZE_WRITE );
#endif

  // We can write back this queue in page reclaim
  current->backing_dev_info = mapping->backing_dev_info;

  written = 0;
  err = generic_write_checks( file, &pos, &count, S_ISBLK( i->i_mode ) );
  if ( unlikely( err ) )
    goto out;

  if ( unlikely( 0 == count ) )
    goto out;

  err = file_remove_suid( file );
  if ( unlikely( err ) )
    goto out;

#ifdef UFSD_NTFS2
  if ( NULL != ( p = is_stream( file ) ) ) {
    // Write stream
    unsigned long seg;
    struct qstr* s  = &file->f_dentry->d_name;
    int len = s->name + s->len - p;

    DebugTrace( +1, Dbg, ("file_aio_write(s): r=%lx (:%.*s), %llx, %Zx\n", i->i_ino, len, p, pos, count ));

    if ( unlikely( file->f_flags & O_DIRECT ) ) {
      DebugTrace( 0, Dbg, ("does not support direct I/O for streams\n" ));
      err = -EOPNOTSUPP;
      goto out_stream;
    }

    if ( FlagOn( file->f_flags, O_APPEND ) )
      pos = i->i_size;

    //
    // Write 'count' bytes via sbi->rw_buffer
    //
    lock_ufsd( sbi );
    written = 0;

    //
    // Allocate helper buffer if not
    //
    if ( NULL == sbi->rw_buffer ) {
      sbi->rw_buffer = vmalloc( RW_BUFFER_SIZE );
      if ( NULL == sbi->rw_buffer ){
        err = -ENOMEM;
        goto end_cycle;
      }
    }

    for ( seg = 0; seg < nr_segs && written < count; seg++ ) {
      const char __user *buf = iov[seg].iov_base;
      size_t iov_len  = min_t( size_t, iov[seg].iov_len, count - written );

      while( 0 != iov_len ) {

        size_t to_write  = min_t( size_t, RW_BUFFER_SIZE - (((size_t)pos) & ( RW_BUFFER_SIZE - 1 )), iov_len );
        size_t written2;

        if ( copy_from_user( sbi->rw_buffer, buf, to_write ) ) {
          err = -EFAULT;
          goto end_cycle; // double break
        }

        err = ufsdapi_file_write( sbi->ufsd, u->ufile, p, len,
                                  pos, to_write, sbi->rw_buffer, &written2 );
        if ( 0 != err ) {
          err = ERR_NOTIMPLEMENTED == err? -EOPNOTSUPP : ERR_NOSPC == err? -ENOSPC : -EINVAL;
          goto end_cycle; // double break
        }

        if ( written2 != to_write ) {
          err = -EIO;     // ??
          goto end_cycle; // double break
        }

        written += written2;
        pos     += written2;
        buf     += written2;
        iov_len -= written2;
      } // while( 0 != iov_len )
    } // for ( seg = 0; seg < nr_segs && written < count; seg++ )

end_cycle:
    unlock_ufsd( sbi );

    // Update file position
    *ppos = pos;

    if ( unlikely( 0 != written ) )
      err = written;

out_stream:
    current->backing_dev_info = NULL;
    DebugTrace( -1, Dbg, ("file_aio_write(s) => %Zx\n", err ));
    return err;
  }
#endif // #ifdef UFSD_NTFS2

//  TIMESPEC_SECONDS( &i->i_mtime ) = TIMESPEC_SECONDS( &i->i_ctime ) = get_seconds(); //  file_update_time( file );

  //
  // Set flag that the content of file is modified
  //
//  if ( !test_and_set_bit( &u->flags, UFSD_UNODE_FLAG_MODIFIED_BIT ) )
//    dirty = 1;

  end_aligned = (pos + sbi->cluster_mask + count) & sbi->cluster_mask_inv;

  i_bytes = inode_get_bytes( i );
  assert( i_bytes >= i->i_size );

  if ( likely( end_aligned > i_bytes ) || is_sparsed_or_compressed( u ) ) {
    mapinfo Map;
    loff_t new_size = pos + count;

    down_write( &u->size_sema );
    lock_ufsd( sbi );

    //
    // TODO: allow to allocate less
    //
    err = ufsdapi_file_map( u->ufile, pos, count, UFSD_MAP_VBO_CREATE, &Map );

    if ( 0 == err ) {
      inode_set_bytes( i, Map.alloc );
      NTFS_ONLY( u->total_alloc = Map.total_alloc; )
      if ( new_size > i->i_size )
        i->i_size = new_size;
//      DebugTrace( 0, Dbg, ("after allocate: sz=%llx,%llx,%llx\n", u->valid, i->i_size, inode_get_bytes( i ) ));
      dirty = 1;
    }
    unlock_ufsd( sbi );
    up_write( &u->size_sema );

    if ( 0 != err )
      goto out2;
  }

  read_lock_irqsave( &u->valid_lock, flags );
  valid = u->valid;
  read_unlock_irqrestore( &u->valid_lock, flags );

  if ( pos > valid ) {
    err = ufsd_extend_initialized_size( i, pos );
    if ( err < 0 )
      goto out;
  }

  //
  // coalesce the iovecs and go direct-to-BIO for O_DIRECT
  //
  if ( unlikely( file->f_flags & O_DIRECT ) ) {
    ssize_t written_buffered, endbyte;

    written = generic_file_direct_write( iocb, iov, &nr_segs, pos, ppos, count, ocount );
    if ( written < 0 || written == count )
      goto out;

    pos   += written;
    count -= written;
    written_buffered = generic_file_buffered_write( iocb, iov, nr_segs, pos, ppos, count, written );
    if ( written_buffered < 0 ) {
      err = written_buffered;
      goto out;
    }
    //
    // We need to ensure that the page cache pages are written to
    // disk and invalidated to preserve the expected O_DIRECT
    // semantics.
    //
    endbyte = pos + written_buffered - written - 1;
    err = filemap_write_and_wait_range( mapping, pos, endbyte );
    if ( 0 == err ) {
      written = written_buffered;
      invalidate_mapping_pages( mapping, pos >> PAGE_CACHE_SHIFT, endbyte >> PAGE_CACHE_SHIFT );
    } else {
      //
      // We don't know how much we wrote, so just return
      // the number of bytes which were direct-written
      //
    }
  } else {
    written = generic_file_buffered_write( iocb, iov, nr_segs, pos, ppos, count, 0 );
  }
out:
  TIMESPEC_SECONDS( &i->i_mtime ) = TIMESPEC_SECONDS( &i->i_ctime ) = get_seconds();  //  file_update_time( file );
  if ( dirty  )
    mark_inode_dirty_sync( i );

out2:
  current->backing_dev_info = NULL;
  if ( unlikely( 0 != written ) ) {
    err = written;
#ifdef Try_to_writeback_inodes_sb
    if ( unlikely( sbi->options.wb && written >= PAGE_CACHE_SIZE ) )
      Try_to_writeback_inodes_sb( i->i_sb );
#endif
  }

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_file_aio_write
//
// based on
// 'mm\filemap.c' generic_file_aio_write
//
// file_operations::aio_write
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_aio_write(
    IN struct kiocb       *iocb,
    IN const struct iovec *iov,
    IN unsigned long      nr_segs,
    IN loff_t             pos
    )
{
  ssize_t ret;
  struct file *file = iocb->ki_filp;
  struct inode *i   = file->f_mapping->host;
#if defined UFSD_NTFS2 || defined UFSD_TRACE
  unode *u  = UFSD_U( i );
#endif
  struct blk_plug plug;

  BUG_ON( iocb->ki_pos != pos );

#ifdef UFSD_CHECK_BDI
  if ( !is_bdi_ok( i->i_sb ) )
    return -ENODEV;
#endif

#ifdef UFSD_NTFS2
  if ( is_encrypted(u) ) {
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("file_write: r=%lx. Attempt to write to encrypted file\n", i->i_ino ));
    return -ENOSYS;
  }
#endif

  mutex_lock( &i->i_mutex );
  blk_start_plug( &plug );

  DebugTrace( +1, Dbg, ("file_aio_write: r=%lx, [%llx + %Zx), %lx, sz=%llx,%llx\n", i->i_ino, pos, iov_length( iov, nr_segs ), nr_segs, u->valid, i->i_size ));

  ret = __ufsd_file_aio_write( iocb, iov, nr_segs, &iocb->ki_pos );

  mutex_unlock( &i->i_mutex );

#if defined HAVE_DECL_GENERIC_WRITE_SYNC && HAVE_DECL_GENERIC_WRITE_SYNC
  if ( likely( ret > 0 || EIOCBQUEUED == ret ) ) {
    ssize_t err = generic_write_sync( file, pos, ret );
    if ( err < 0 && ret > 0 )
      ret = err;
  }
#else
  if ( likely( ret > 0 && ((file->f_flags & O_SYNC) || IS_SYNC( i ) ) ) ) {
    ssize_t err = sync_page_range( i, i->i_mapping, pos, ret );
    if ( err < 0 )
      ret = err;
  }
#endif

  assert( inode_get_bytes( i ) >= i_size_read( i ) );

  blk_finish_plug( &plug );

  DebugTrace( -1, Dbg, ("file_aio_write => %Zx\n", ret ));

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_file_mmap
//
// file_operations::mmap
///////////////////////////////////////////////////////////
static int
ufsd_file_mmap(
    IN struct file            *file,
    IN struct vm_area_struct  *vma
    )
{
  int err;
  struct inode *i   = file_inode( file );
  unode *u          = UFSD_U( i );
  UINT64 from       = ((UINT64)vma->vm_pgoff << PAGE_SHIFT);
  unsigned long len = vma->vm_end - vma->vm_start;
  UINT64 isize      = i_size_read( i );
  UINT64 vsize      = from + len;

  assert( from < isize );
  if ( vsize > isize ) {
    len   = isize - from;
    vsize = isize;
  }

  DebugTrace( +1, Dbg, ("file_mmap: r=%lx %lx(%s%s), %llx, %lx s=%llx,%llx\n",
              i->i_ino, vma->vm_flags,
              (vma->vm_flags & VM_READ)?"r":"",
              (vma->vm_flags & VM_WRITE)?"w":"",
              from, len, u->valid, isize ));
#ifdef UFSD_NTFS2
  if ( is_stream( file ) || is_encrypted(u) ) {
    err = -ENODEV; // no mmap for streams
    goto out;
  }
#endif

  if ( (vma->vm_flags & VM_WRITE) && u->valid < vsize ) {
    loff_t pos              = vsize - 1;
    unsigned int flags      = file->f_flags;
    mm_segment_t old_limit  = get_fs();
    char zero = 0;

    DebugTrace( 0, Dbg, ("file_mmap: zero range [%llx,%llx)\n", u->valid, vsize ));

    file->f_flags &= ~O_DIRECT;
    set_fs( KERNEL_DS );

    err = do_sync_write( file, &zero, 1, &pos );

    set_fs( old_limit );
    file->f_flags = flags;

    if ( 1 != err )
      goto out;
  }

  err = generic_file_mmap( file, vma );

out:
  DebugTrace( -1, Dbg, ("file_mmap -> %d\n", err) );

  return err;
}


#if defined HAVE_DECL_GENERIC_FILE_SENDFILE && HAVE_DECL_GENERIC_FILE_SENDFILE
#if defined UFSD_TRACE | defined UFSD_NTFS2
///////////////////////////////////////////////////////////
// ufsd_file_sendfile
//
// file_operations::sendfile
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_sendfile(
    IN struct file  *file,
    IN OUT loff_t   *ppos,
    IN size_t       count,
    IN read_actor_t actor,
    IN void         *target
    )
{
  ssize_t ret;
  DebugTrace( +1, Dbg, ("file_sendfile: r=%lx, %llx %Zx\n", file_inode( file )->i_ino, *ppos, count ));

#ifdef UFSD_NTFS2
  if ( is_stream( file ) )
    ret = -ENOSYS;
  else
#endif
    ret = generic_file_sendfile( file, ppos, count, actor, target );

  DebugTrace( -1, Dbg, ("file_sendfile -> %Zx\n", ret ));
  return ret;
}
#else
  #define ufsd_file_sendfile generic_file_sendfile
#endif
#endif


#if defined HAVE_DECL_GENERIC_FILE_SPLICE_READ && HAVE_DECL_GENERIC_FILE_SPLICE_READ
#if defined UFSD_TRACE | defined UFSD_NTFS2
///////////////////////////////////////////////////////////
// ufsd_file_splice_read
//
// file_operations::splice_read
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_splice_read(
    IN struct file  *file,
    IN OUT loff_t   *ppos,
    IN struct pipe_inode_info *pipe,
    IN size_t       len,
    IN unsigned int flags
    )
{
  ssize_t ret;
  DebugTrace( +1, Dbg, ("file_splice_read: r=%lx, %llx %Zx\n", file_inode( file )->i_ino, *ppos, len ));

#ifdef UFSD_NTFS2
  if ( is_stream( file ) ){
    DebugTrace( -1, Dbg, ("file_splice_read failed to read stream -> -ENOSYS\n"));
    return -ENOSYS;
  }
#endif

  ret = generic_file_splice_read( file, ppos, pipe, len, flags );

  if ( ret >= 0 ){
    DebugTrace( -1, Dbg, ("file_splice_read -> %Zx\n", (size_t)ret));
  } else {
    DebugTrace( -1, Dbg, ("file_splice_read -> error %d\n", (int)ret));
  }
  return ret;
}
#else
  #define ufsd_file_splice_read generic_file_splice_read
#endif
#endif


#if defined HAVE_DECL_GENERIC_FILE_SPLICE_WRITE && HAVE_DECL_GENERIC_FILE_SPLICE_WRITE
#if defined UFSD_TRACE | defined UFSD_NTFS2
///////////////////////////////////////////////////////////
// ufsd_file_splice_write
//
// file_operations::splice_write
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_splice_write(
    IN struct pipe_inode_info *pipe,
    IN struct file  *file,
    IN OUT loff_t   *ppos,
    IN size_t       len,
    IN unsigned int flags
    )
{
  ssize_t ret;
#if defined UFSD_CHECK_BDI || defined UFSD_TRACE
  struct inode *i = file_inode( file );
#endif

#ifdef UFSD_CHECK_BDI
  if ( !is_bdi_ok( i->i_sb ) )
    return -ENODEV;
#endif

  DebugTrace( +1, Dbg, ("file_splice_write: r=%lx, %llx %Zx\n", i->i_ino, *ppos, len ));

#ifdef UFSD_NTFS2
  if ( is_stream( file ) ) {
    DebugTrace( -1, Dbg, ("file_splice_write failed to write stream -> -ENOSYS\n"));
    return -ENOSYS;
  }
#endif

  ret = generic_file_splice_write( pipe, file, ppos, len, flags );

  if ( ret >= 0 ){
    DebugTrace( -1, Dbg, ("file_splice_write -> %Zx\n", (size_t)ret));
  } else {
    DebugTrace( -1, Dbg, ("file_splice_write -> error %d\n", (int)ret));
  }
  return ret;
}
#else
  #define ufsd_file_splice_write  generic_file_splice_write
#endif
#endif


#if defined HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE && HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE
  #define DECLARE_FALLOCATE_ARG     struct inode* i
  #define DECLARE_FALLOCATE_INODE
#elif defined HAVE_STRUCT_FILE_OPERATIONS_FALLOCATE && HAVE_STRUCT_FILE_OPERATIONS_FALLOCATE
  #define DECLARE_FALLOCATE_ARG     struct file*  file
  #define DECLARE_FALLOCATE_INODE   struct inode* i = file_inode( file );
#endif

#ifdef DECLARE_FALLOCATE_ARG
#include <linux/falloc.h>
///////////////////////////////////////////////////////////
// ufsd_fallocate
//
// inode_operations::fallocate
///////////////////////////////////////////////////////////
static long
ufsd_fallocate(
    IN DECLARE_FALLOCATE_ARG,
    IN int    mode,
    IN loff_t offset,
    IN loff_t len
    )
{
  long ret;
  DECLARE_FALLOCATE_INODE
  usuper *sbi = UFSD_SB( i->i_sb );
  unode *u    = UFSD_U( i );

  DebugTrace( +1, Dbg, ("fallocate: r=%lx, %llx, %llx, %d\n", i->i_ino, offset, len, mode ));

  //
  // Call UFSD library
  //
  down_write( &u->size_sema );
  lock_ufsd( sbi );

  ret = lazy_open( sbi, i );

  if ( 0 == ret ) {
    int KeepSize = FlagOn( mode, FALLOC_FL_KEEP_SIZE );
    mapinfo Map;

    ret = ufsdapi_file_map( u->ufile, offset, len, UFSD_MAP_VBO_CREATE, &Map );
    if ( 0 == ret ) {
      inode_set_bytes( i, Map.alloc );
      NTFS_ONLY( u->total_alloc = Map.total_alloc; )
    }
    switch( ret ) {
    case 0:
      offset += len;
      if ( !KeepSize && offset > i_size_read( i ) )
        i_size_write( i, offset );
      break;
    case ERR_NOSPC          : ret = -ENOSPC; break;
    case ERR_NOTIMPLEMENTED : ret = -EOPNOTSUPP; break;
    default                 : ret = -EINVAL;
    }
  }

  unlock_ufsd( sbi );
  up_write( &u->size_sema );

  if ( 0 == ret ){
    DebugTrace( -1, Dbg, ("fallocate -> ok\n"));
  } else {
    DebugTrace( -1, Dbg, ("fallocate -> error %d\n", (int)ret));
  }
  return ret;
}
#endif // #if defined HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE && HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE


///////////////////////////////////////////////////////////
// ufsd_file_sendpage
//
// file_operations::sendpage
///////////////////////////////////////////////////////////
static ssize_t
ufsd_file_sendpage(
    IN struct file  *file,
    IN struct page  *page,
    IN int          offset,
    IN size_t       len,
    IN OUT loff_t   *pos,
    IN int          more
    )
{
  return -EOPNOTSUPP;
}


static const struct file_operations ufsd_file_ops = {
  .llseek   = generic_file_llseek,
  .read     = do_sync_read,
  .write    = do_sync_write,
  .aio_read = ufsd_file_aio_read,
  .aio_write = ufsd_file_aio_write,
#ifndef UFSD_NO_USE_IOCTL
  .ioctl    = ufsd_ioctl,
#ifdef CONFIG_COMPAT
  .compat_ioctl = ufsd_compat_ioctl,
#endif
#endif
  .mmap     = ufsd_file_mmap,
  .open     = ufsd_file_open,
  .release  = ufsd_file_release,
  .fsync    = ufsd_fsync,
//  int (*aio_fsync) (struct kiocb *, int datasync);
//  int (*fasync) (int, struct file *, int);
  .sendpage = ufsd_file_sendpage,
#if defined HAVE_DECL_GENERIC_FILE_SENDFILE && HAVE_DECL_GENERIC_FILE_SENDFILE
//  .sendfile     = ufsd_file_sendfile,
#endif
#if defined HAVE_DECL_GENERIC_FILE_SPLICE_WRITE && HAVE_DECL_GENERIC_FILE_SPLICE_WRITE
  .splice_write = ufsd_file_splice_write,
#endif
#if defined HAVE_DECL_GENERIC_FILE_SPLICE_READ && HAVE_DECL_GENERIC_FILE_SPLICE_READ
  .splice_read  = ufsd_file_splice_read,
#endif
#if defined HAVE_STRUCT_FILE_OPERATIONS_FALLOCATE && HAVE_STRUCT_FILE_OPERATIONS_FALLOCATE
  .fallocate    = ufsd_fallocate,
#endif
};

static const struct inode_operations ufsd_file_inode_ops = {
#if !(defined HAVE_DECL_TRUNCATE_SETSIZE && HAVE_DECL_TRUNCATE_SETSIZE)
  .truncate     = ufsd_truncate,
#endif
  .setattr      = ufsd_setattr,
  .getattr      = ufsd_getattr,
#ifdef UFSD_USE_XATTR
  .permission   = ufsd_permission,
  .setxattr     = generic_setxattr,
  .getxattr     = generic_getxattr,
  .listxattr    = ufsd_listxattr,
  .removexattr  = generic_removexattr,
#if defined HAVE_STRUCT_INODE_OPERATIONS_GET_ACL && HAVE_STRUCT_INODE_OPERATIONS_GET_ACL
  .get_acl      = ufsd_get_acl,
#endif
#endif
#if defined HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE && HAVE_STRUCT_INODE_OPERATIONS_FALLOCATE
  .fallocate    = ufsd_fallocate,
#endif
};


#if defined UFSD_NTFS2 || defined UFSD_EXFAT || defined UFSD_REFS
///////////////////////////////////////////////////////////
// ufsd_readlink_hlp
//
// helper for  ufsd_readlink and ufsd_follow_link
///////////////////////////////////////////////////////////
static int
ufsd_readlink_hlp(
    IN usuper       *sbi,
    IN struct inode *i,
    IN char         *kaddr,
    IN int          buflen
    )
{
  int len;
  char *p   = kaddr;
  char *l   = kaddr + buflen;
  unode *u  = UFSD_U(i);

  //
  // Call library code to read link
  //
  lock_ufsd( sbi );

  len = lazy_open( sbi, i );
  if ( 0 == len )
    len = ufsdapi_read_link( u->ufile, kaddr, buflen );

  unlock_ufsd( sbi );

  if ( 0 != len )
    return -EFAULT;

  // safe strlen
  while( 0 != *p && p <= l )
    p += 1;
  len = (int)(p-kaddr);

#if defined UFSD_NTFS2 && !(defined HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT && HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT)
  //
  // Assume that link points to the same volume
  // and convert strings
  // C:\\Users => /mnt/ntfs/Users
  //
  if ( len > 3
    && 'a' <= (kaddr[0] | 0x20)
    && (kaddr[0] | 0x20) <= 'z'
    && ':' == kaddr[1]
    && '\\' == kaddr[2]
    && NULL != sbi->vfs_mnt ) {

    char *MntPath;
#if defined HAVE_DECL_D_PATH_V1 && HAVE_DECL_D_PATH_V1
    MntPath       = d_path( sbi->vfs_mnt->mnt_root, sbi->vfs_mnt, sbi->mnt_buffer, sizeof(sbi->mnt_buffer) - 1 );
#elif defined HAVE_DECL_D_PATH_V2 && HAVE_DECL_D_PATH_V2
    struct path path;
    path.dentry   = sbi->vfs_mnt->mnt_root;
    path.mnt      = sbi->vfs_mnt;
    MntPath       = d_path( &path, sbi->mnt_buffer, sizeof(sbi->mnt_buffer) - 1 );
#else
#error d_path unknown
#endif

    if ( !IS_ERR( MntPath ) ) {
//      DebugTrace( 0, Dbg,("mount path %s\n", MntPath ));
      // Add last slash
      int MntPathLen = strlen( MntPath );
      MntPath[MntPathLen++] = '/';

      if ( MntPathLen + len - 3 < buflen ) {
        p = kaddr + MntPathLen;
        memmove( p, kaddr + 3, len - 3 );
        memcpy( kaddr, MntPath, MntPathLen );
        len += MntPathLen - 3;
        // Convert slashes
        l = kaddr + len;
        while( ++p < l ){
          if ( '\\' == *p )
            *p = '/';
        }
        *p = 0;
      }
    }
  }
#endif

  return len;
}


///////////////////////////////////////////////////////////
// ufsd_readlink
//
// inode_operations::readlink
///////////////////////////////////////////////////////////
static int
ufsd_readlink(
    IN struct dentry  *de,
    OUT char __user   *buffer,
    IN int            buflen
    )
{
  int err;
  char *kaddr;
  struct inode *i = de->d_inode;
  usuper *sbi     = UFSD_SB( i->i_sb );

  DebugTrace( +1, Dbg, ("readlink: r=%lx, '%.*s', %d\n", i->i_ino, (int)de->d_name.len, de->d_name.name, buflen ));

  kaddr = ufsd_heap_alloc( buflen, 0 );
  if ( NULL == kaddr ) {
    DebugTrace( -1, Dbg, ("readlink: HeapAlloc failed to allocate %d bytes\n", buflen ));
    return -ENOMEM;
  }

  //
  // Call helper function that reads symlink into buffer
  //
  err = ufsd_readlink_hlp( sbi, i, kaddr, buflen );

  if ( err > 0 ) {
    DebugTrace( -1, Dbg, ("readlink: '%.*s' -> '%.*s'\n",
                (int)de->d_name.len, de->d_name.name, err, kaddr ));
  } else {
    DebugTrace( -1, Dbg, ("readlink: ufsdapi_read_link failed %d\n", err ));
    err = -EFAULT;
  }

  if ( err > 0 && 0 != copy_to_user( buffer, kaddr, err ) )
    err = -EFAULT;
  ufsd_heap_free( kaddr );

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_follow_link
//
// inode_operations::follow_link
///////////////////////////////////////////////////////////
static void*
ufsd_follow_link(
    IN struct dentry    *de,
    IN struct nameidata *nd
    )
{
  void  *ret;
  struct inode *i = de->d_inode;
  usuper *sbi     = UFSD_SB( i->i_sb );

  DebugTrace( +1, Dbg, ("follow_link: '%.*s'\n", (int)de->d_name.len, de->d_name.name ));

  ret = kmalloc( PAGE_SIZE, GFP_NOFS );
  //
  // Call helper function that reads symlink into buffer
  //
  if ( NULL != ret && ufsd_readlink_hlp( sbi, i, ret, PAGE_SIZE ) > 0 )
    nd_set_link( nd, (char*)ret );

  if ( !IS_ERR( ret ) ) {
    DebugTrace( -1, Dbg, ("follow_link -> %p '%.*s'\n", ret, (int)PAGE_SIZE, (char*)ret ));
  } else {
    DebugTrace( -1, Dbg, ("follow_link failed (%p)\n", ret ));
  }

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_put_link
//
// inode_operations::put_link
///////////////////////////////////////////////////////////
static void
ufsd_put_link(
    IN struct dentry    *de,
    IN struct nameidata *nd,
    IN void             *cookie
    )
{
  kfree( cookie );
}

static const struct inode_operations ufsd_link_inode_operations_ufsd = {
  .readlink    = ufsd_readlink,
  .follow_link = ufsd_follow_link,
  .put_link    = ufsd_put_link,
};
#endif // #if defined UFSD_NTFS2 || defined UFSD_EXFAT

#ifdef UFSD_HFS
static const struct inode_operations ufsd_link_inode_operations_u8 = {
  .readlink    = generic_readlink,
  .follow_link = page_follow_link_light,
  .put_link    = page_put_link,
};
#endif


typedef struct upage_data {
  loff_t      vbo, lbo, len, valid;
  sector_t    next_block;
  struct bio* bio;
  int         version;
} upage_data;


///////////////////////////////////////////////////////////
// ufsd_end_buffer_async_read
//
// this function comes from fs/buffer.c 'end_buffer_async_read'
///////////////////////////////////////////////////////////
static void
ufsd_end_buffer_async_read(
    IN struct buffer_head* bh,
    IN int uptodate
    )
{
  unsigned long flags;
  struct buffer_head  *tmp;
  int page_uptodate = 1;
  struct page* page = bh->b_page;
  struct buffer_head  *first = page_buffers( page );
  struct inode* i   = page->mapping->host;
  unode *u          = UFSD_U( i );

  if ( uptodate ) {
    size_t bh_off           = bh_offset( bh );
    loff_t end, buffer_off  = bh_off + ((loff_t)page->index << PAGE_CACHE_SHIFT);

    set_buffer_uptodate( bh );

    read_lock_irqsave( &u->valid_lock, flags );
    end = min_t( loff_t, u->valid, i_size_read( i ) );
    read_unlock_irqrestore( &u->valid_lock, flags );

    if ( buffer_off + bh->b_size > end ) {
      unsigned off = end > buffer_off? end - buffer_off : 0;

//      printk( "end_buffer_async_read (zero) bh=%p,%Zx, bh_off=%Zx, buffer_off=%llx, off=%x\n", bh->b_data, bh->b_size, bh_off, buffer_off, off );

      local_irq_save( flags );
      zero_user( page, bh_off + off, bh->b_size - off );
      local_irq_restore( flags );
    }
  } else {
    clear_buffer_uptodate( first );
    SetPageError( page );
    ufsd_printk( NULL == i? NULL : i->i_sb, "buffer i/o error, logical block 0x%" PSCT "x.", first->b_blocknr );
  }

  local_irq_save( flags );
  bit_spin_lock( BH_Uptodate_Lock, &first->b_state );
  clear_buffer_async_read( bh );
  unlock_buffer( bh );
  tmp = bh;
  do {
    if ( !buffer_uptodate( tmp ) ) {
      page_uptodate = 0;
    }
    if ( buffer_async_read( tmp ) ) {
      BUG_ON(!buffer_locked( tmp ));
      bit_spin_unlock( BH_Uptodate_Lock, &first->b_state );
      local_irq_restore( flags );
      return;
    }
  } while( bh != ( tmp = tmp->b_this_page ) );

  bit_spin_unlock( BH_Uptodate_Lock, &first->b_state );
  local_irq_restore( flags );

  if ( page_uptodate && !PageError( page ) )
    SetPageUptodate( page );
  unlock_page( page );
}


///////////////////////////////////////////////////////////
// ufsd_end_io_read
//
// I/O completion handler for multipage BIOs
///////////////////////////////////////////////////////////
static void
ufsd_end_io_read(
    IN struct bio *bio,
    IN int        error
    )
{
  struct bio_vec *bvec = &bio->bi_io_vec[bio->bi_vcnt-1];
  struct inode *i = bvec->bv_page->mapping->host;
  unode *u        = UFSD_U( i );

  error = !test_bit( BIO_UPTODATE, &bio->bi_flags );
  if ( error ){
    ufsd_printk( NULL == i? NULL : i->i_sb, "bio read I/O error." );
  }

  do {
    struct page *page = bvec->bv_page;
    if ( !error ) {
      unsigned long flags;
      loff_t end, page_off = (loff_t)page->index << PAGE_CACHE_SHIFT;

      read_lock_irqsave( &u->valid_lock, flags );
      end = min_t( loff_t, u->valid, i_size_read( i ) );
      read_unlock_irqrestore( &u->valid_lock, flags );

      if ( page_off + PAGE_CACHE_SIZE > end ){
        local_irq_save( flags );
        zero_user_segment( page, end > page_off? end - page_off : 0, PAGE_CACHE_SIZE );
        local_irq_restore( flags );
      }
      SetPageUptodate( page );
    } else {
      ClearPageDirty( page );
      SetPageError( page );
    }
    unlock_page( page );
  } while ( --bvec >= bio->bi_io_vec );
  bio_put( bio );
}


///////////////////////////////////////////////////////////
// ufsd_end_io_write
//
// I/O completion handler for multipage BIOs
///////////////////////////////////////////////////////////
static void
ufsd_end_io_write(
    IN struct bio *bio,
    IN int        err
    )
{
  struct bio_vec *bvec = &bio->bi_io_vec[bio->bi_vcnt-1];
  err = !test_bit( BIO_UPTODATE, &bio->bi_flags );

  if ( err ) {
    struct inode* i = bio->bi_io_vec[0].bv_page->mapping->host;
    ufsd_printk( NULL == i? NULL : i->i_sb, "bio write I/O error." );
  }

//  printk( "end_io_write %p at %llx sz=%Zx, cnt=%x\n", bio, (UINT64)BIO_BISECTOR( bio ) << 9, BIO_BISIZE( bio ), (unsigned)bio->bi_vcnt );

  do {
    struct page* page = bvec->bv_page;
    if ( err ) {
      SetPageError( page );
      set_bit( AS_EIO, &page->mapping->flags );
    }

    end_page_writeback( page );
  } while( --bvec >= bio->bi_io_vec );

  bio_put( bio );
}


///////////////////////////////////////////////////////////
// mpage_alloc
//
// stolen from fs/mpage.c
///////////////////////////////////////////////////////////
static struct bio*
mpage_alloc(
    IN struct block_device *bdev,
    IN sector_t first_sector,
    IN unsigned nr_vecs,
    IN gfp_t    gfp_flags
    )
{
  while( 1 ) {
    struct bio *bio = bio_alloc( gfp_flags, nr_vecs );
    if ( likely( NULL != bio ) ) {
      BIO_BISECTOR( bio ) = first_sector;
      bio->bi_bdev    = bdev;

      DebugTrace( 0, UFSD_LEVEL_BIO, ("bio+: o=%" PSCT "x\n", first_sector << 9 ));
      return bio;
    }

    if ( !(current->flags & PF_MEMALLOC) )
      return NULL;

    nr_vecs >>= 1;
    if ( 0 == nr_vecs )
      return NULL;
  }
}


///////////////////////////////////////////////////////////
// ufsd_bio_submit
//
// stolen from fs/mpage.c
///////////////////////////////////////////////////////////
static void
ufsd_bio_submit(
    IN int rw,
    IN struct bio *bio
    )
{
  DebugTrace( 0, UFSD_LEVEL_BIO, ("submit_bio %s at o=%" PSCT "x, sz=%x, cnt=%x\n", WRITE == rw? "write" : "read", BIO_BISECTOR( bio ) << 9, BIO_BISIZE( bio ), (unsigned)bio->bi_vcnt ));
  bio->bi_end_io = WRITE == rw? ufsd_end_io_write : ufsd_end_io_read;
  submit_bio( rw, bio );
}


typedef struct vbo2lbo{
  loff_t  vbo;
  mapinfo map;
  int     locked; // 1 down_read, 2 - down_write
} vbo2lbo;


///////////////////////////////////////////////////////////
// unlock_sema
//
//
///////////////////////////////////////////////////////////
static void
unlock_sema(
    IN unode *u,
    IN vbo2lbo* v2l
    )
{
  if ( 1 == v2l->locked )
    up_read( &u->size_sema );
#if defined UFSD_NTFS2 || defined UFSD_HFS
  else if ( 2 == v2l->locked )
    up_write( &u->size_sema );
#endif
  v2l->locked = 0;
}


///////////////////////////////////////////////////////////
// vbo_to_lbo
//
//
///////////////////////////////////////////////////////////
static int
vbo_to_lbo(
    IN usuper *sbi,
    IN unode  *u,
    IN OUT vbo2lbo *v2l
    )
{
  int err;

  if ( !v2l->locked ) {
    down_read( &u->size_sema );
    v2l->locked = 1;
  }

  assert( v2l->vbo <= inode_get_bytes( &u->i ) );

  err = ufsdapi_file_map( u->ufile, v2l->vbo, 0, UFSD_MAP_VBO_NOLOCK, &v2l->map );

  if ( likely( 0 == err ) )
    return err;

  unlock_sema( u, v2l );

#if defined UFSD_NTFS2 || defined UFSD_HFS
  if ( !sbi->options.exfat ) {
    down_write( &u->size_sema );
    lock_ufsd( sbi );
    err = ufsdapi_file_map( u->ufile, v2l->vbo, 0, 0, &v2l->map );
    unlock_ufsd( sbi );
    up_write( &u->size_sema );
  }
#endif
  if ( 0 != err )
    v2l->map.len = 0;
  return err;
}


#ifdef UFSD_NTFS2
///////////////////////////////////////////////////////////
// ufsd_read_ntfs_file
//
// Helper function to read resident/compressed files
///////////////////////////////////////////////////////////
static int
ufsd_read_ntfs_file(
    IN usuper *sbi,
    IN unode  *u,
    IN struct page *page,
    IN loff_t vbo
    )
{
  size_t ret;
  int err;
  char* kaddr;
  unsigned from = vbo & ~PAGE_CACHE_MASK;

  //
  // Read file via UFSD -> UFSD_BdRead
  //
  DebugTrace( 0, Dbg, ("r=%lx: use ufsd to at off %llx\n", u->i.i_ino, vbo ));

  lock_ufsd( sbi );

  kaddr = kmap( page );
  err   = ufsdapi_file_read( sbi->ufsd, u->ufile, NULL, 0, vbo, PAGE_CACHE_SIZE - from, kaddr + from, &ret );
  if ( 0 == err ) {
    if ( ret + from < PAGE_CACHE_SIZE )
      memset( kaddr + ret + from, 0, PAGE_CACHE_SIZE - ret - from );
    SetPageUptodate( page );
  } else {
    ret = -EIO;
    SetPageError( page );
  }
  kunmap( page );
  flush_dcache_page( page );

  unlock_ufsd( sbi );

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_write_ntfs_file
//
// Helper function to write resident/compressed files
///////////////////////////////////////////////////////////
static int
ufsd_write_ntfs_file(
    IN usuper *sbi,
    IN unode  *u,
    IN struct page *page,
    IN loff_t vbo,
    IN size_t len
    )
{
  int err;
  size_t written;
  char* kaddr;

  assert( vbo + len <= i_size_read( &u->i ) );
  //
  // Write file via UFSD -> UFSD_BdWrite
  //
  DebugTrace( 0, Dbg, ("r=%lx: use ufsd to write page at off %llx\n", u->i.i_ino, vbo ));

  down_write( &u->size_sema );
  lock_ufsd( sbi );

  kaddr = kmap( page ) + (vbo & ~PAGE_CACHE_MASK); // atomic_kmap( page ); for resident?
  err   = ufsdapi_file_write( sbi->ufsd, u->ufile, NULL, 0, vbo, len, kaddr, &written );
  kunmap( page );

  unlock_ufsd( sbi );
  up_write( &u->size_sema );

  if ( 0 != err || written != len ) {
    ufsd_printk( u->i.i_sb, "failed to write file %x\n", err );
    return -EIO;
  }

  return 0;
}
#endif // #ifdef UFSD_NTFS2


///////////////////////////////////////////////////////////
// ufsd_buf_readpage
//
// fs/buffer.c 'block_read_full_page'
///////////////////////////////////////////////////////////
static int
ufsd_buf_readpage(
    IN struct page *page
    )
{
  struct inode *i = page->mapping->host;
  pgoff_t index   = page->index;
  loff_t i_size   = i_size_read( i );

  DebugTrace( +1, UFSD_LEVEL_PAGE_RW, ("buf_readpage: r=%lx, %llx, sz=%llx,%llx\n", i->i_ino, (UINT64)index << PAGE_CACHE_SHIFT, UFSD_U(i)->valid, i_size ));

  ProfileEnter( UFSD_SB( i->i_sb ), buf_readpage );

  BUG_ON( !PageLocked( page ) );

  if ( index >= PAGE_CACHE_IDX( i_size ) ) {
Zero:
    zero_user( page, 0, PAGE_CACHE_SIZE );
    SetPageUptodate( page );
    unlock_page( page );
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_readpage (zero) -> ok\n" ));
  } else {
    unode *u    = UFSD_U( i );
    usuper *sbi = UFSD_SB( i->i_sb );
    const unsigned blkbits   = i->i_blkbits;
    const unsigned blocksize = 1 << blkbits;
    struct buffer_head *bh, *head, *arr[MAX_BUF_PER_PAGE];
    int nr;
    loff_t i_bytes;
    unsigned long flags;
    vbo2lbo v2l;

    BUG_ON( PageUptodate( page ) );

    if ( !page_has_buffers( page ) ) {
      create_empty_buffers( page, blocksize, 0 );
      if ( !page_has_buffers( page ) ) {
        unlock_page( page );
        ProfileLeave( sbi, buf_readpage );
        DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_readpage -> nomemory\n" ));
        return -ENOMEM;
      }
    }

    head  = page_buffers( page );
    down_read( &u->size_sema );
    v2l.locked  = 1; // sema was locked for read
    read_lock_irqsave( &u->valid_lock, flags );
    i_size  = min_t( loff_t, i_size_read( i ), u->valid );
    i_bytes = inode_get_bytes( i );
    read_unlock_irqrestore( &u->valid_lock, flags );

    v2l.vbo = (loff_t)index << PAGE_CACHE_SHIFT;
    if ( v2l.vbo >= i_bytes || vbo_to_lbo( sbi, u, &v2l ) ) {
      unlock_sema( u, &v2l );
      goto Zero;
    }

    BUG_ON( 0 == v2l.map.len );
    bh  = head;
    nr  = 0;
    while ( 1 ) {

      if ( buffer_uptodate( bh ) )
        goto next_bh; // continue

      if ( buffer_mapped( bh ) ){
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_readpage: buffer_mapped %llx, bh=%" PSCT "x\n", v2l.vbo, bh->b_blocknr ));
        arr[nr++] = bh;
        goto next_bh; // continue
      }

      //
      // Buffer not mapped
      //
      bh->b_bdev = i->i_sb->s_bdev;
      if ( unlikely( 0 == v2l.map.len ) ) {
        bh->b_blocknr = -1;
        goto zero_buf;
      }

#ifdef UFSD_NTFS2
      if ( sbi->options.ntfs ) {
        if ( UFSD_VBO_LBO_HOLE == v2l.map.lbo ) {
          //
          // Check if all buffers in page are sparsed
          //
          bh->b_blocknr = -1;
          goto zero_buf;
        } else if ( UFSD_VBO_LBO_RESIDENT == v2l.map.lbo || UFSD_VBO_LBO_COMPRESSED == v2l.map.lbo ) {
          unsigned long flags;
          loff_t end, page_off = (loff_t)page->index << PAGE_CACHE_SHIFT;

          assert( 0 == nr );

          unlock_sema( u, &v2l );

          if ( ufsd_read_ntfs_file( sbi, u, page, v2l.vbo ) < 0 )
            break;

          read_lock_irqsave( &u->valid_lock, flags );
          end = min_t( loff_t, u->valid, i_size_read( i ) );
          read_unlock_irqrestore( &u->valid_lock, flags );

          if ( page_off + PAGE_CACHE_SIZE > end ){
            local_irq_save( flags );
            zero_user_segment( page, end > page_off? end - page_off : 0, PAGE_CACHE_SIZE );
            local_irq_restore( flags );
          }
          set_buffer_uptodate( bh );
          goto next_bh;
        }
      }
#endif // #ifdef UFSD_NTFS2

      bh->b_blocknr = v2l.map.lbo >> blkbits;
      DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_readpage: set_buffer_mapped %llx, bh=%" PSCT "x\n", v2l.vbo, bh->b_blocknr ));
      set_buffer_mapped( bh );
      if ( v2l.vbo < i_size ) {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_readpage: store_bh\n" ));
        arr[nr++] = bh;
        goto next_bh; // continue
      }

zero_buf:
      DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_readpage zero bh page: [%lx,%x)\n", bh_offset( bh ), blocksize ));
      zero_user( page, bh_offset( bh ), blocksize );
      set_buffer_uptodate( bh );

next_bh:
      bh = bh->b_this_page;
      if ( head == bh )
        break;

      //
      // Get next block position
      //
      v2l.vbo += blocksize;
      if ( likely( 0 != v2l.map.len ) ) {
        if ( v2l.map.len > blocksize ) {
          v2l.map.len -= blocksize;
#ifdef UFSD_NTFS2
          if ( UFSD_VBO_LBO_HOLE != v2l.map.lbo && UFSD_VBO_LBO_RESIDENT != v2l.map.lbo && UFSD_VBO_LBO_COMPRESSED != v2l.map.lbo )
#endif
          {
            v2l.map.lbo += blocksize;
          }
        } else if ( v2l.vbo >= i_bytes || vbo_to_lbo( sbi, u, &v2l ) ) {
          v2l.map.len = 0;
        } else {
          assert( 0 != v2l.map.len );
          assert( 0 != v2l.map.lbo );
        }
      }
    } // while (1)

    unlock_sema( u, &v2l );

    if ( !nr ) {
      if ( !PageError( page ) )
        SetPageUptodate( page );
      unlock_page( page );
    } else {

      int k;

      for ( k = 0; k < nr; ++k ) {
        bh = arr[k];
        lock_buffer( bh );
        bh->b_end_io = ufsd_end_buffer_async_read;
        set_buffer_async_read( bh );
      }

      for ( k = 0; k < nr; ++k ) {
        bh = arr[k];
        if ( buffer_uptodate( bh ) ) {
          ufsd_end_buffer_async_read( bh, 1 );
        } else {
          DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("submit bh %" PSCT "x\n", bh->b_blocknr));
          submit_bh( READ, bh );
        }
      }
    }
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_readpage -> ok, nr=%d\n", nr ));
  }

  ProfileLeave( UFSD_SB( i->i_sb ), buf_readpage );

  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_do_readpage
//
// fs/mpage.c 'do_mpage_readpage'
///////////////////////////////////////////////////////////
static int
ufsd_do_readpage(
    IN struct page    *page,
    IN unsigned       nr_pages,
    IN OUT upage_data *mpage
    )
{
  int err;
#ifdef UFSD_TRACE
  struct inode *i = page->mapping->host;
  unode *u  = UFSD_U( i );
#endif

  DebugTrace( +1, UFSD_LEVEL_PAGE_RW, ("do_readpage: r=%lx, o=%llx, sz=%llx,%llx\n", i->i_ino, (UINT64)page->index << PAGE_CACHE_SHIFT, u->valid, i->i_size ));

  ProfileEnter( UFSD_SB( i->i_sb ), do_readpage );

  if ( !page_has_buffers( page ) ) {
    //
    // Check if we can read page without buffers
    //
#ifndef UFSD_TRACE
    struct inode *i = page->mapping->host;
    unode *u        = UFSD_U( i );
#endif
    usuper *sbi     = UFSD_SB( i->i_sb );
    const unsigned blkbits    = i->i_blkbits;
    const unsigned block_size = 1u << blkbits;
    const unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits;
    unsigned long flags;
    unsigned page_block, length;
    sector_t start_block = 0; // not necessary, just to suppress warnings
    vbo2lbo v2l;

    v2l.locked = 0;

repeat:
    page_block  = 0;
    v2l.vbo     = (loff_t)page->index << PAGE_CACHE_SHIFT;

    //
    // Check cached fragment
    //
    if ( mpage->version != u->ver ) {
      read_lock_irqsave( &u->valid_lock, flags );
      mpage->valid = u->valid;
      read_unlock_irqrestore( &u->valid_lock, flags );

      if ( v2l.vbo >= mpage->valid ) {
        zero_user( page, 0, PAGE_CACHE_SIZE );
        SetPageUptodate( page );
        unlock_page( page );
        DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> zero (%llx,%llx)\n", v2l.vbo, mpage->valid ));
        return 0;
      }

      goto get_blocknr;
    }

    //
    // Use cached fragment
    //
    for ( ; v2l.vbo < mpage->valid; v2l.vbo += block_size ) {
      loff_t gap  = v2l.vbo - mpage->vbo;
      sector_t block_nr;

      if ( likely( gap >= 0 && gap < mpage->len ) ) {
        block_nr = (mpage->lbo + gap) >> blkbits;
      } else {
        if ( !v2l.locked ) {
          if ( 0 != page_block && mpage->version != u->ver )
            goto repeat;

          read_lock_irqsave( &u->valid_lock, flags );
          mpage->valid = u->valid;
          read_unlock_irqrestore( &u->valid_lock, flags );
        }

get_blocknr:
        //
        // Get fragment
        //
        if ( vbo_to_lbo( sbi, u, &v2l ) )
          goto page_done;

        //
        // Update cache info
        //
        mpage->vbo      = v2l.vbo;
        mpage->lbo      = v2l.map.lbo;
        mpage->len      = v2l.map.len;
//        mpage->flags    = v2l.map.flags;
        mpage->version  = u->ver;
        block_nr        = v2l.map.lbo >> blkbits;
        gap             = 0;
      }

#ifdef UFSD_NTFS2
      if ( sbi->options.ntfs ) {
        if ( UFSD_VBO_LBO_HOLE == mpage->lbo ) {
          //
          // Check if all buffers in page are sparsed
          //
          if ( mpage->len - gap >= PAGE_CACHE_SIZE ) {
            assert( 0 == page_block );
            goto page_done;
          }
          DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("confused: page is not full sparsed\n"));
          goto confused;
        } else if ( UFSD_VBO_LBO_RESIDENT == mpage->lbo || UFSD_VBO_LBO_COMPRESSED == mpage->lbo ) {

          assert( 0 == page_block );

          unlock_sema( u, &v2l );

          err = ufsd_read_ntfs_file( sbi, u, page, v2l.vbo );

          unlock_page( page );
          if ( err > 0 )
            err = 0;

          ProfileLeave( sbi, do_readpage );
          DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> %d\n", err ));
          return err;
        }
      }
#endif // #ifdef UFSD_NTFS2

      //
      // Check if all blocks in page are continues
      //
      if ( 0 == page_block ) {
        start_block = block_nr;
        page_block  = 1;
#ifdef UFSD_NTFS2
      } else if ( -1 == block_nr && -1 == start_block ) {
        page_block += 1;
#endif
      } else if ( start_block + page_block == block_nr ) {
        page_block += 1;
      } else {
        unlock_sema( u, &v2l );
        goto confused;  // page buffers are not continues
      }

      if ( page_block >= blocks_per_page ) {
        assert( page_block == blocks_per_page );
        break;
      }
    }

page_done:
    unlock_sema( u, &v2l );

    if ( page_block != blocks_per_page ){
      // reset the rest of page
      zero_user_segment( page, page_block << blkbits, PAGE_CACHE_SIZE );
      if ( !page_block ) {
        SetPageUptodate( page );
        unlock_page( page );
        ProfileLeave( sbi, do_readpage );
        DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> full zero\n" ));
        return 0;
      }
    }

    //
    // here we have valid 'start_block'
    // Try to merge with previous request
    //
    if ( mpage->bio && mpage->next_block != start_block ) {
      ufsd_bio_submit( READ, mpage->bio );
      goto alloc_new;
    }

    if ( NULL == mpage->bio ) {
      struct block_device *bdev;
alloc_new:
      bdev = i->i_sb->s_bdev;
      mpage->bio = mpage_alloc( bdev, start_block << (blkbits - 9),
                                min_t( unsigned, bio_get_nr_vecs( bdev ), min_t( unsigned, 256u, nr_pages ) ),
                                GFP_NOFS|__GFP_HIGH );
      if ( NULL == mpage->bio )
        goto buf_read;
    }

    length = page_block << blkbits;
    if ( bio_add_page( mpage->bio, page, length, 0 ) < length ) {
      ufsd_bio_submit( READ, mpage->bio );
      goto alloc_new;
    }

    if ( blocks_per_page == page_block ) {
      mpage->next_block = start_block + blocks_per_page;
      DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> ok, next=%" PSCT "x\n", mpage->next_block ));
    } else {
      ufsd_bio_submit( READ, mpage->bio );
      mpage->bio = NULL;
      DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> ok, submitted\n" ));
    }

    ProfileLeave( sbi, do_readpage );
    return 0;
  }

confused:
  if ( NULL != mpage->bio ) {
    ufsd_bio_submit( READ, mpage->bio );
    mpage->bio = NULL;
    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_readpage, submitted\n" ));
  }

buf_read:
  err = ufsd_buf_readpage( page );

  ProfileLeave( UFSD_SB( i->i_sb ), do_readpage );
  DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_readpage -> %d (buf)\n", err ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_readpage
//
// address_space_operations::readpage
///////////////////////////////////////////////////////////
static int
ufsd_readpage(
    IN struct file *file,
    IN struct page *page
    )
{
  int err;
  struct inode *i = page->mapping->host;
  upage_data mpage;

  mpage.bio     = NULL;
  mpage.version = UFSD_U( i )->ver-1;

  ProfileEnter( UFSD_SB( i->i_sb ), readpage );

  DebugTrace( +1, Dbg, ("readpage: r=%lx, o=%llx\n", i->i_ino, (UINT64)page->index << PAGE_CACHE_SHIFT ));

  err = ufsd_do_readpage( page, 1, &mpage );
  if ( NULL != mpage.bio )
    ufsd_bio_submit( READ, mpage.bio );

  ProfileLeave( UFSD_SB( i->i_sb ), readpage );

  if ( 0 == err ) {
    DebugTrace( -1, Dbg, ("readpage -> ok%s\n", mpage.bio? ", submitted":"" ));
  } else {
    DebugTrace( -1, Dbg, ("readpage -> err %d%s\n", err, mpage.bio? ", submitted":"" ));
    ufsd_printk( i->i_sb, "failed to read page 0x%llx for inode 0x%lx, (error %d)", (UINT64)page->index, i->i_ino, err );
  }
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_buf_writepage
//
// fs/buffer.c 'block_write_full_page'
///////////////////////////////////////////////////////////
static int
ufsd_buf_writepage(
    IN struct page *page,
    IN struct writeback_control *wbc
    )
{
  struct inode *i = page->mapping->host;
  loff_t i_size   = i_size_read( i );
  pgoff_t index   = page->index;
  const pgoff_t end_index = i_size >> PAGE_CACHE_SHIFT;
  pgoff_t npages  = PAGE_CACHE_IDX( i_size );
  unode *u        = UFSD_U( i );
  struct super_block *sb = i->i_sb;
  usuper* sbi    = UFSD_SB( sb );
  const unsigned blkbits    = i->i_blkbits;
  const unsigned blocksize  = 1 << blkbits;
  struct buffer_head *bh, *head;
  loff_t valid, end_block;
  unsigned long flags;
  int valid_in_page;
  vbo2lbo v2l;
  int err = 0, all_done;

  DebugTrace( +1, UFSD_LEVEL_PAGE_RW, ("buf_writepage: o=%llx, sz=%llx,%llx, npages=%lx\n", (loff_t)index << PAGE_CACHE_SHIFT, u->valid, i_size, npages ));

  ProfileEnter( sbi, buf_writepage );

  BUG_ON( !PageLocked( page ) );

  if ( index >= npages ) {
#if defined HAVE_DECL_BLOCK_INVALIDATEPAGE_V1 && HAVE_DECL_BLOCK_INVALIDATEPAGE_V1
    block_invalidatepage( page, 0 );
#elif defined HAVE_DECL_BLOCK_INVALIDATEPAGE_V2 && HAVE_DECL_BLOCK_INVALIDATEPAGE_V2
    block_invalidatepage( page, 0, PAGE_CACHE_SIZE );
#else
#error "Unknown block_invalidatepage"
#endif
    unlock_page( page );
    ProfileLeave( sbi, buf_writepage );
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_writepage (out of size) => 0\n" ));
    return 0;
  }

  if ( index >= end_index ) {
    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_writepage zero_user_segment: %llx\n", i_size & ~PAGE_CACHE_MASK ));
    zero_user_segment( page, i_size & ~PAGE_CACHE_MASK, PAGE_CACHE_SIZE);
  }

  //
  // Below is modified variant of '__block_write_full_page'
  //
  if ( !page_has_buffers( page ) ) {
//    DebugTrace( 0, Dbg, ("buf_writepage: create page buffers\n" ));
    create_empty_buffers( page, blocksize, (1u<<BH_Uptodate) | (1u<<BH_Dirty) );

    if ( !page_has_buffers( page ) ) {
      printk( "Error allocating page buffers.  Redirtying page so we try again later." );
      redirty_page_for_writepage( wbc, page );
      unlock_page( page );
      ProfileLeave( sbi, buf_writepage );
      DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_writepage no memory\n" ));
      return 0;
    }
  }

  head    = page_buffers( page );
  v2l.vbo = (loff_t)index << PAGE_CACHE_SHIFT;
  read_lock_irqsave( &u->valid_lock, flags );
  valid   = u->valid;
  read_unlock_irqrestore( &u->valid_lock, flags );

  v2l.locked = 0;
  valid_in_page = 0;
  if ( valid > i_size )
    valid = i_size;
  else if ( valid < i_size && v2l.vbo <= valid && valid < v2l.vbo + PAGE_CACHE_SIZE )
    valid_in_page = 1;

  bh  = head;
  do {
    end_block = v2l.vbo + blocksize;
    assert( page == bh->b_page );

    if ( v2l.vbo >= i_size ) {
      clear_buffer_dirty( bh );
      set_buffer_uptodate( bh );
      continue;
    }

    if ( !buffer_dirty( bh ) && (!valid_in_page || valid >= end_block) )
      continue;

    if ( PageUptodate( page ) ) {
      if ( !buffer_uptodate( bh ) )
        set_buffer_uptodate( bh );
    } else if ( !buffer_uptodate( bh ) ) {
      if ( buffer_dirty( bh ) ) {
        if ( !valid_in_page ) {
          clear_buffer_dirty( bh );
          continue;
        }
      } else {
        BUG_ON( !valid_in_page );
      }
      BUG_ON( valid >= end_block );

      zero_user( page, bh_offset( bh ) + valid - v2l.vbo, end_block - valid );
      set_buffer_uptodate( bh );
    }

    if ( valid < end_block ) {
      if ( valid_in_page ) {
        BUG_ON( valid < v2l.vbo );

        zero_user( page, bh_offset( bh ) + valid - v2l.vbo, end_block - valid );

        if ( !buffer_dirty( bh ) )
          set_buffer_dirty( bh );

        if ( end_block > i_size ) {
          valid_in_page = 0;
          valid = i_size;
        } else {
          valid = end_block;
        }

        write_lock_irqsave( &u->valid_lock, flags );
        u->ver += 1;
        u->valid = valid;
        write_unlock_irqrestore( &u->valid_lock, flags );

  //      if ( !test_and_set_bit( 3, &unode->_d8 ) )
        mark_inode_dirty_sync( i );
      } else {
        if ( valid < i_size ) {
          ufsd_printk( sb, "writing beyond initialized size is not supported yet, sorry. %llx, %llx, [%llx %llx)", valid, i_size, v2l.vbo, end_block );
          err = -EOPNOTSUPP;//-95;
          BUG_ON( valid >= v2l.vbo );
          break;
        }
      }
    }

    if ( buffer_mapped( bh ) )
      continue;

    bh->b_bdev = sb->s_bdev;

    if ( !v2l.locked ) {
      if ( vbo_to_lbo( sbi, u, &v2l ) ) {
        printk( "failed to map v2l.vbo %llx\n", v2l.vbo );

not_mapped:
        bh->b_blocknr = -1;
        clear_buffer_dirty( bh );
        zero_user( page, bh_offset( bh ), blocksize );
        set_buffer_uptodate( bh );
        continue;
      }
    } else if ( v2l.map.len > blocksize ) {
      v2l.map.len -= blocksize;
#ifdef UFSD_NTFS2
      if ( UFSD_VBO_LBO_HOLE != v2l.map.lbo && UFSD_VBO_LBO_RESIDENT != v2l.map.lbo && UFSD_VBO_LBO_COMPRESSED != v2l.map.lbo )
#endif
      {
        v2l.map.lbo += blocksize;
      }
    } else if ( vbo_to_lbo( sbi, u, &v2l ) ) {
      assert( 0 == v2l.map.len );
      goto not_mapped;
    }

#ifdef UFSD_NTFS2
    if ( UFSD_VBO_LBO_HOLE == v2l.map.lbo ) {
      unlock_sema( u, &v2l );

      down_write( &u->size_sema );
      lock_ufsd( sbi );

      err = ufsdapi_file_map( u->ufile, v2l.vbo, blocksize, UFSD_MAP_VBO_CREATE, &v2l.map );

      unlock_ufsd( sbi );
      up_write( &u->size_sema );

      if ( 0 != err )
        break;

      assert( FlagOn( v2l.map.flags, UFSD_MAP_LBO_NEW ) );
//      unmap_underlying_metadata( bh->b_bdev, v2l.map.lbo >> blkbits );
    } else if ( UFSD_VBO_LBO_RESIDENT == v2l.map.lbo ) {

      //
      // File is resident
      //
      size_t towrite = v2l.vbo + blocksize > i_size ? (size_t)(i_size - v2l.vbo) : blocksize;
      assert( (int)towrite > 0 );

//      assert( buffer_delay( bh ) );
//      clear_buffer_delay( bh );

      unlock_sema( u, &v2l );
      err = ufsd_write_ntfs_file( sbi, u, page, v2l.vbo + bh_offset( bh ), towrite );
      if ( 0 != err )
        break;

      continue;

    } else if ( UFSD_VBO_LBO_COMPRESSED == v2l.map.lbo ) {
      //
      // Page if full compressed
      // Write it now
      //
      loff_t vbo = (loff_t)index << PAGE_CACHE_SHIFT;
      size_t towrite = vbo + PAGE_CACHE_SIZE > i_size ? (size_t)(i_size - vbo) : PAGE_CACHE_SIZE;

//      assert( buffer_delay( bh ) );
//      clear_buffer_delay( bh );

      unlock_sema( u, &v2l );
      err = ufsd_write_ntfs_file( sbi, u, page, vbo, towrite );
      if ( 0 == err ) {
        SetPageUptodate( page );
        bh = head;
        do {
          assert( buffer_uptodate( bh ) );
          set_buffer_uptodate( bh );
          clear_buffer_dirty( bh );
        } while ( head != (bh = bh->b_this_page) );
      }

      goto ok;
    }
#endif

    bh->b_blocknr = v2l.map.lbo >> blkbits;
    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("buf_writepage: set_buffer_mapped %llx, bh=%" PSCT "x\n", v2l.vbo, bh->b_blocknr ));
    set_buffer_mapped( bh );

  } while( v2l.vbo = end_block, (bh = bh->b_this_page) != head );

  unlock_sema( u, &v2l );

  if ( !PageUptodate( page ) ) {
    bh = head;
    while ( buffer_uptodate( bh ) ) {
      bh = bh->b_this_page;
      if ( head == bh ) {
        // All buffers uptodate -> page uptodate
        SetPageUptodate( page );
        break;
      }
    }
  }

  bh = head;
  do {
    if ( !buffer_mapped( bh ) || !buffer_dirty( bh ) ) {
      if ( err && -ENOMEM != err ) {
        clear_buffer_dirty( bh );
      }
      continue;
    }
    lock_buffer( bh );
    if ( test_clear_buffer_dirty( bh ) ){
      BUG_ON( !buffer_uptodate( bh ) );
      mark_buffer_async_write( bh );
    } else{
      unlock_buffer( bh );
    }

  } while ( (bh = bh->b_this_page) != head );

#ifdef UFSD_NTFS2
ok:
#endif
  if ( err )  {
    if ( -EOPNOTSUPP == err )
      err = 0;
    else if ( -ENOMEM == err ) {
      ufsd_printk( sb, "error allocating memory. redirtying page to try again later." );
      redirty_page_for_writepage( wbc, page );
      err = 0;
    } else{
      SetPageError( page );
    }
  }

  BUG_ON( PageWriteback( page ) );
  all_done = 1;
  set_page_writeback( page );

  assert( bh == head );
  do {
    struct buffer_head *next = bh->b_this_page;
    if ( buffer_async_write( bh ) ) {
//      DebugTrace( 0, Dbg, ("submit_bh( %llx )\n", bh->b_blocknr ));
      submit_bh( WRITE, bh );
      all_done = 0;
    }
    bh = next;
  } while( bh != head );

  unlock_page( page );
  if ( all_done ) {
    end_page_writeback( page );
  }

  ProfileLeave( sbi, buf_writepage );
  // TODO: update 'arch' bit for exfat/ntfs
  DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("buf_writepage => %d, sz=%llx,%llx, %s\n", err, u->valid, i->i_size, all_done? "done":"wb" ));

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_do_writepage
//
// fs/mpage.c '__mpage_writepage'
///////////////////////////////////////////////////////////
static int
ufsd_do_writepage(
    IN struct page *page,
    IN struct writeback_control *wbc,
    IN OUT upage_data *mpage
    )
{
  sector_t start_block = 0; // not necessary, just to suppress warnings
  struct address_space *mapping = page->mapping;
  struct inode *i = mapping->host;
  usuper *sbi     = UFSD_SB( i->i_sb );
  unode *u        = UFSD_U( i );
  const unsigned blkbits    = i->i_blkbits;
  const unsigned blocksize  = 1 << blkbits;
  const unsigned blocks_per_page = PAGE_CACHE_SIZE >> blkbits;
  struct buffer_head *head, *bh;
  unsigned first_unmapped = blocks_per_page;
  pgoff_t end_index;
  unsigned long flags;
  unsigned page_block;
  int uptodate;
  unsigned length;
  loff_t i_size, valid;
  int err;
  vbo2lbo v2l;

  v2l.locked  = 0;
  v2l.vbo     = (loff_t)page->index << PAGE_CACHE_SHIFT;

  DebugTrace( +1, UFSD_LEVEL_PAGE_RW, ("do_writepage(%s) r=%lx, o=%llx, sz=%llx,%llx\n", current->comm, i->i_ino, v2l.vbo, u->valid, i->i_size ));

  ProfileEnter( sbi, do_writepage );

retry:
  read_lock_irqsave( &u->valid_lock, flags );
  i_size  = i_size_read( i );
  valid   = u->valid;
  read_unlock_irqrestore( &u->valid_lock, flags );

  if ( unlikely( page_has_buffers( page ) ) ) {
    loff_t vbo = v2l.vbo;
    bh = head  = page_buffers( page );
    page_block = 0;
    uptodate   = 1;
    first_unmapped = blocks_per_page; // assume that no mapped buffers

    //
    // Check for all buffers in page
    //
    do {
      BUG_ON( buffer_locked( bh ) );
      if ( !buffer_mapped( bh ) ) {
        if ( buffer_dirty( bh ) ) {
          DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(1) o=%llx v=%llx\n", vbo, valid ));
          goto confused;
        }
        if ( !buffer_uptodate( bh ) )
          uptodate = 0;
        // Save the position of hole
        if ( first_unmapped == blocks_per_page )
          first_unmapped = page_block;
        continue;
      }

      if ( first_unmapped != blocks_per_page ) {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(2) o=%llx v=%llx\n", vbo, valid ));
        goto confused;  // hole -> non-hole
      }

      if ( !buffer_dirty( bh ) || !buffer_uptodate( bh ) ) {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(3) o=%llx v=%llx\n", vbo, valid ));
        goto confused;
      }

      if ( i_size > valid && vbo >= valid ) {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(4) o=%llx v=%llx\n", vbo, valid ));
        goto confused;
      }

      if ( 0 == page_block ) {
        start_block = bh->b_blocknr;
        page_block  = 1;
      } else if ( bh->b_blocknr == start_block + page_block ) {
        page_block += 1;
      } else {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(5) o=%llx v=%llx\n", vbo, valid ));
        goto confused;
      }
      // Reset the position of hole
      first_unmapped = blocks_per_page;

    } while ( vbo += blocksize, head != ( bh = bh->b_this_page ) );

    page_block = first_unmapped;

    if ( !PageUptodate( page ) && uptodate )
      SetPageUptodate( page );
  } else {
    int reuse = 0;

    page_block = 0;

    //
    // Check cached fragment
    //
    if ( mpage->version != u->ver  ) {
      if ( v2l.vbo >= valid ) {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(6) o=%llx v=%llx\n", v2l.vbo, valid ));
        goto confused;
      }

      read_lock_irqsave( &u->valid_lock, flags );
      i_size = i_size_read( i );
      mpage->valid = u->valid;
      read_unlock_irqrestore( &u->valid_lock, flags );

      goto get_block_nr;
    }

    reuse = 1;

    //
    // 'mpage' has actual fragment information
    //
    for ( ; v2l.vbo < mpage->valid; v2l.vbo += blocksize ) {
      loff_t gap = v2l.vbo - mpage->vbo;
      sector_t blocknr;

      if ( likely( gap >= 0 && gap < mpage->len ) ) {
        blocknr = (mpage->lbo + gap ) >> blkbits;

        if ( reuse ) {
          DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("reuse fragment [%llx,%llx,%llx), %llx, bh=%" PSCT "x\n", mpage->vbo, mpage->lbo, mpage->len, v2l.vbo, blocknr ));
          reuse = 0;
        }
      } else {
        if ( !v2l.locked ) {
          if ( page_block && mpage->version != u->ver ) {
            // file layout may be changed. Try again
            goto retry;
          }

          read_lock_irqsave( &u->valid_lock, flags );
          i_size = i_size_read( i );
          mpage->valid = u->valid;
          read_unlock_irqrestore( &u->valid_lock, flags );
        }

get_block_nr:
        //
        // Find fragment
        //
        if ( vbo_to_lbo( sbi, u, &v2l ) ) {
          DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage no map(1) o=%llx v=%llx\n", v2l.vbo, valid ));
          goto page_done;
        }

        //
        // Save actual fragment information
        //
        mpage->vbo      = v2l.vbo;
        mpage->lbo      = v2l.map.lbo;
        mpage->len      = v2l.map.len;
//        mpage->flags    = v2l.map.flags;
        mpage->version  = u->ver;
        blocknr         = v2l.map.lbo >> blkbits;
      }

#ifdef UFSD_NTFS2
      if ( sbi->options.ntfs ) {

        size_t towrite = 0;
        int last = 0;

        if ( UFSD_VBO_LBO_HOLE == mpage->lbo || UFSD_VBO_LBO_RESIDENT == mpage->lbo || UFSD_VBO_LBO_COMPRESSED == mpage->lbo ) {

          unlock_sema( u, &v2l );

          if ( v2l.vbo >= i->i_size ) {
            DebugTrace( 0, 0, ( "looks like truncate in progress\n" ));
            break;
          }

          if ( UFSD_VBO_LBO_COMPRESSED == mpage->lbo || UFSD_VBO_LBO_RESIDENT == mpage->lbo ) {
            //
            // Check if we can use the rest of page
            //
            loff_t end_of_page = ((loff_t)(1 + page->index)) << PAGE_CACHE_SHIFT;
            if ( v2l.vbo + v2l.map.len >= end_of_page ) {
              //
              // We can write the rest of page in one request
              //
              towrite = min_t( loff_t, end_of_page, i_size )- v2l.vbo;
              last = 1;
            }
          }

          if ( !last )
            towrite = v2l.vbo + blocksize > i->i_size ? (size_t)(i->i_size - v2l.vbo) : blocksize;
          assert( (int)towrite > 0 );
          if ( (int)towrite <= 0 ) {
            DebugTrace( 0, 0, ( "**** vbo %llx, i_size %llx, %d, %d\n", v2l.vbo, i->i_size, mpage->version, u->ver ));
            break;
          }
        }

        if ( UFSD_VBO_LBO_HOLE == mpage->lbo ) {
          //
          // Allocate clusters for hole
          //
          down_write( &u->size_sema );
          lock_ufsd( sbi );
          err = ufsdapi_file_map( u->ufile, v2l.vbo, towrite, UFSD_MAP_VBO_CREATE, &v2l.map );
          unlock_ufsd( sbi );
          up_write( &u->size_sema );
          if ( 0 != err )
            goto confused;

          mpage->vbo      = v2l.vbo;
          mpage->lbo      = v2l.map.lbo;
          mpage->len      = v2l.map.len;
//          mpage->flags    = v2l.map.flags;
          blocknr         = v2l.map.lbo >> blkbits;
          assert( FlagOn( v2l.map.flags, UFSD_MAP_LBO_NEW ) );
//          unmap_underlying_metadata( i->i_sb->s_bdev, blocknr );

        } else if ( UFSD_VBO_LBO_RESIDENT == mpage->lbo || UFSD_VBO_LBO_COMPRESSED == mpage->lbo ) {
          //
          // File is resident/compressed
          //
          err = ufsd_write_ntfs_file( sbi, u, page, v2l.vbo + (page_block<<blkbits), towrite );
          if ( 0 != err )
            goto confused;

          if ( v2l.vbo + towrite == i_size || last ) {
            unlock_page( page );
            DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_writepage -> ok, last, sz=%llx,%llx\n", u->valid, i->i_size ));
            return 0;
          }

          continue;
        }
      }
#endif

      //
      // Check for continues range
      //
      if ( 0 == page_block ) {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage start_block=%" PSCT "x, o=%llx v=%llx\n", start_block, v2l.vbo, valid ));
        start_block = blocknr;
        page_block  = 1;
      } else if ( blocknr == start_block + page_block ) {
        page_block  += 1;
      } else {
        DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(7) o=%llx v=%llx, b=%" PSCT "x\n", v2l.vbo, valid, blocknr ));
        goto confused;
      }

      if ( page_block >= blocks_per_page )
        goto page_done;
    }

    if ( i_size > mpage->valid || !page_block ) {
      DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(8) o=%llx v=%llx\n", v2l.vbo, mpage->valid ));
      goto confused;
    }
    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage page_done o=%llx v=%llx\n", v2l.vbo, mpage->valid ));
  }

page_done:
  unlock_sema( u, &v2l );

  if ( unlikely( !page_block ) ) {
    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(8) o=%llx v=%llx\n", v2l.vbo, valid ));
    goto confused;
  }

  end_index = i_size >> PAGE_CACHE_SHIFT;
  if ( unlikely( page->index >= end_index ) ) {
    unsigned offset = i_size & (PAGE_CACHE_SIZE - 1);
    if ( page->index > end_index || !offset ) {
      DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage confused(9) o=%llx v=%llx\n", v2l.vbo, valid ));
      goto confused;
    }
    DebugTrace( 0, UFSD_LEVEL_PAGE_RW, ("do_writepage zero page\n" ));
    zero_user_segment( page, offset, PAGE_CACHE_SIZE );
  }

  //
  // 'blocks' is not empty. Check if we can merge with previous bio
  //
  if ( mpage->bio && mpage->next_block != start_block ) {
    //
    // Write previous fragment
    //
    ufsd_bio_submit( WRITE, mpage->bio );
    goto alloc_new;
  }

  if ( NULL == mpage->bio ){
    struct block_device *bdev;
alloc_new:
    bdev = i->i_sb->s_bdev;
    mpage->bio = mpage_alloc( bdev, start_block << (blkbits - 9), min_t( unsigned, bio_get_nr_vecs( bdev ), 256 ), GFP_NOFS|__GFP_HIGH );
    if ( NULL == mpage->bio )
      goto out; // confused
  }

  length = page_block << blkbits;
  if ( bio_add_page( mpage->bio, page, length, 0 ) < length ) {
    //
    // Looks like bio request is too big
    // Submit current bio and allocate new
    //
    ufsd_bio_submit( WRITE, mpage->bio );
    goto alloc_new;
  }

  if ( unlikely( page_has_buffers( page ) ) ) {
    unsigned buffer_counter = 0;
    head  = page_buffers( page );
    bh    = head;
    do  {
      if ( buffer_counter++ == page_block )
        break;
      clear_buffer_dirty( bh );
    } while ( head != ( bh = bh->b_this_page ) );
  }

  BUG_ON( PageWriteback( page ) );
  test_set_page_writeback( page );
  unlock_page( page );

  if ( page_block == blocks_per_page ) {
    mpage->next_block = start_block + page_block;
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_writepage -> ok, next=%" PSCT "x, sz=%llx,%llx\n", mpage->next_block, u->valid, i->i_size ));
  } else {
    ufsd_bio_submit( WRITE, mpage->bio );
    mpage->bio = NULL;
    DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_writepage -> ok (sumitted) sz=%llx,%llx\n", u->valid, i->i_size ));
  }

  ProfileLeave( sbi, do_writepage );
  return 0;

confused:
  unlock_sema( u, &v2l );

  if ( mpage->bio ) {
    ufsd_bio_submit( WRITE, mpage->bio );
    mpage->bio = NULL;
  }

out:
  err = ufsd_buf_writepage( page, wbc );
  if ( err )
    mapping_set_error( mapping, err );

  ProfileLeave( sbi, do_writepage );

  DebugTrace( -1, UFSD_LEVEL_PAGE_RW, ("do_writepage -> %d (buf), sz=%llx,%llx\n", err, u->valid, i->i_size ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_writepage
//
// address_space_operations::writepage
///////////////////////////////////////////////////////////
static int
ufsd_writepage(
    IN struct page *page,
    IN struct writeback_control *wbc
    )
{
  struct address_space *mapping = page->mapping;
  struct inode  *i = mapping->host;
  unode *u  = UFSD_U( i );
  upage_data mpage;
  int err;

  DebugTrace( 0, Dbg, ("writepage: r=%lx, o=%llx\n", i->i_ino, (UINT64)page->index << PAGE_CACHE_SHIFT) );

  ProfileEnter( UFSD_SB( i->i_sb ), writepage );

  // TODO: update 'arch' bit for exfat/ntfs
  mpage.bio  = NULL;
  mpage.version = u->ver-1;

  err = ufsd_do_writepage( page, wbc, &mpage );
  if ( NULL != mpage.bio )
    ufsd_bio_submit( WRITE, mpage.bio );

  ProfileLeave( UFSD_SB( i->i_sb ), writepage );
  if ( 0 == err ) {
    DebugTrace( 0, Dbg, ("writepage -> ok%s\n", mpage.bio? ", submitted":"" ));
  } else {
    DebugTrace( 0, Dbg, ("writepage -> err %d%s\n", err, mpage.bio? ", submitted":"" ));
    ufsd_printk( i->i_sb, "failed to write page for inode 0x%lx, page index 0x%llx (error %d).", i->i_ino, (UINT64)page->index, err );
  }
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_write_begin
//
// fs/buffer.c block_write_begin + __block_write_begin
// address_space_operations::write_begin
///////////////////////////////////////////////////////////
static int
ufsd_write_begin(
    IN struct file    *file,
    IN struct address_space *mapping,
    IN loff_t         pos,
    IN unsigned       len,
    IN unsigned       flags,
    OUT struct page   **pagep,
    OUT void          **fsdata
    )
{
  int err = 0;
  struct buffer_head *bh, *head, *wait[2], **wait_bh=wait;
  struct inode *i = mapping->host;
  unode *u = UFSD_U( i );
  struct super_block *sb = i->i_sb;
  usuper *sbi   = UFSD_SB( sb );
  loff_t to     = pos + len;
  loff_t to_aligned = (to + sbi->cluster_mask) & sbi->cluster_mask_inv;
  // Don't loff_t from = pos & PAGE_CACHE_MASK; PAGE_CACHE_MASK = 0xfffff000
  loff_t from = pos & ~(loff_t)( PAGE_CACHE_SIZE - 1 );
  const unsigned blkbits    = i->i_blkbits;
  const unsigned blocksize  = 1 << blkbits;
  loff_t i_bytes, valid, block_end;
  struct page *page;
  vbo2lbo v2l;
  unsigned long lockf;

  DebugTrace( +1, UFSD_LEVEL_VFS_WBWE, ("write_begin: r=%lx, o=%llx,%x fl=%x sz=%llx,%llx%s\n",
                        i->i_ino, pos, len, flags, u->valid, i->i_size, is_sparsed(u)?",sp":"" ));

  assert( mutex_is_locked( &i->i_mutex ) );

  ProfileEnter( sbi, write_begin );

  i_bytes = inode_get_bytes( i );
  assert( i_bytes >= i->i_size );

  if ( unlikely( to_aligned > i_bytes ) ){
    down_write( &u->size_sema );
    lock_ufsd( sbi );
    // TODO: partially allocation?? UFSD_MAP_VBO_CREATE_PARTITIAL
    err = ufsdapi_file_map( u->ufile, from, to - from, UFSD_MAP_VBO_CREATE, &v2l.map );
    unlock_ufsd( sbi );
    up_write( &u->size_sema );

    if ( 0 != err ) {
      ufsd_printk( sb, "failed to extend allocation of inode 0x%lx (error %x,%d).", i->i_ino, err, err );
      goto fin;
    }

    inode_set_bytes( i, v2l.map.alloc );
    NTFS_ONLY( u->total_alloc = v2l.map.total_alloc; )
    if ( to > i->i_size )
      i->i_size = to;

    assert( v2l.map.alloc >= i->i_size );

//    DebugTrace( 0, Dbg, ("after allocate: sz=%llx,%llx,%llx\n", u->valid, i->i_size, inode_get_bytes( i ) ));

    mark_inode_dirty_sync( i );
  }

  read_lock_irqsave( &u->valid_lock, lockf );
  valid = u->valid;
  read_unlock_irqrestore( &u->valid_lock, lockf );

  if ( unlikely( pos > valid ) ) {
    err = ufsd_extend_initialized_size( i, pos );
    if ( err < 0 )  {
      to = to_aligned;
      goto restore;
    }
  }

  page = grab_cache_page_write_begin( mapping, pos >> PAGE_CACHE_SHIFT, flags | AOP_FLAG_NOFS );

  if ( unlikely( NULL == page ) ) {
    ufsd_printk( sb, "failed to allocate page cache page at start 0x%llx.", pos );
    err = -ENOMEM;
    to = to_aligned;
    goto restore;
  }

  if ( likely( !page_has_buffers( page ) ) ) {
    if ( likely( pos <= from && to >= from + PAGE_CACHE_SIZE ) ) {
      DebugTrace( 0, UFSD_LEVEL_VFS_WBWE, ("full page\n" ));
      goto ok;
    }

    if ( unlikely( PageUptodate( page ) ) ) {
      DebugTrace( 0, UFSD_LEVEL_VFS_WBWE, ("!full page + page_uptodate\n" ));
      goto ok;
    }

    create_empty_buffers( page, blocksize, 0 );
    if ( !page_has_buffers( page ) ) {
      ufsd_printk( sb, "failed to allocate page buffers." );
      err = -ENOMEM;
      goto unlock_page;
    }
  }

  head    = page_buffers( page );
  i_bytes = inode_get_bytes( i );

  assert( i_bytes >= i->i_size );

  v2l.vbo     = from;
  v2l.locked  = 0;

  bh = head;
  do {
    block_end = v2l.vbo + blocksize;

    if ( v2l.vbo >= i->i_size ) {
      DebugTrace( 0, UFSD_LEVEL_VFS_WBWE, ("write_begin: zero_tail %llx + %x\n", v2l.vbo, blocksize ));
      zero_user( page, bh_offset( bh ), blocksize );
      set_buffer_uptodate( bh );
      continue;
    }

    if ( buffer_mapped( bh ) )
      goto check_bh;

    //
    // Buffer is not mapped
    //
    bh->b_bdev = sb->s_bdev;

    if ( !v2l.locked ) {
      if ( unlikely( vbo_to_lbo( sbi, u, &v2l ) ) ) {
        DebugTrace( 0, UFSD_LEVEL_ERROR, ("block_start %llx\n", v2l.vbo ));
not_mapped:
//          SetFlag( sbi->_c4, 1 );
        ufsd_printk( sb, "Inside allocated size but outside cluster ext!");
        err = -EIO;
        break;
      }
    } else if ( v2l.map.len > blocksize ) {
      v2l.map.len -= blocksize;
#ifdef UFSD_NTFS2
      if ( UFSD_VBO_LBO_HOLE != v2l.map.lbo && UFSD_VBO_LBO_RESIDENT != v2l.map.lbo && UFSD_VBO_LBO_COMPRESSED != v2l.map.lbo )
#endif
      {
        v2l.map.lbo += blocksize;
      }
    } else if ( unlikely( vbo_to_lbo( sbi, u, &v2l ) ) ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("block_start %llx, i_bytes %llx, i_size %llx\n", v2l.vbo, i_bytes, i->i_size ));
      assert( 0 == v2l.map.len );
      goto not_mapped;
    }

#ifdef UFSD_NTFS2
    if ( UFSD_VBO_LBO_HOLE == v2l.map.lbo ) {
      //
      // Allocate clusters
      //
      size_t towrite = v2l.vbo + blocksize > i->i_size ? (size_t)(i->i_size - v2l.vbo) : blocksize;
      assert( (int)towrite > 0 );
      if ( (int)towrite <= 0 ) {
        DebugTrace( 0, 0, ("**** vbo %llx, i_bytes %llx, to %llx, i_size %llx\n", v2l.vbo, i_bytes, to, i->i_size ));
      }

      unlock_sema( u, &v2l );
      down_write( &u->size_sema );
      lock_ufsd( sbi );
      err = ufsdapi_file_map( u->ufile, v2l.vbo, towrite, UFSD_MAP_VBO_CREATE, &v2l.map );
      unlock_ufsd( sbi );
      up_write( &u->size_sema );
      if ( 0 != err )
        goto unlock_page;

      assert( FlagOn( v2l.map.flags, UFSD_MAP_LBO_NEW ) );
//      unmap_underlying_metadata( sb->s_bdev, v2l.map.lbo >> blkbits );

    } else if ( UFSD_VBO_LBO_RESIDENT == v2l.map.lbo
             || UFSD_VBO_LBO_COMPRESSED == v2l.map.lbo ) {
//      clear_buffer_dirty( bh );
//      set_buffer_delay( bh );
      DebugTrace( 0, UFSD_LEVEL_VFS_WBWE, ("resident bh: %llx, %lx\n", v2l.vbo, bh->b_state ));

      //
      // Page if full resident/compressed
      // Write it now
      //
      assert( bh == head );
      unlock_sema( u, &v2l );
      err = ufsd_write_ntfs_file( sbi, u, page, pos, len );//v2l.vbo, to - v2l.vbo );
      if ( 0 != err )
        goto unlock_page;

      SetPageUptodate( page );

      do  {
        loff_t v27 = bh_offset( bh ) + from;

        if ( valid < v27 + blocksize ) {
          unsigned start = valid > v27? (valid - v27) : 0;
          unsigned start_page = start + bh_offset( bh );
          DebugTrace( 0, UFSD_LEVEL_VFS_WBWE, ( "write_begin: from=%llx, v27=%llx, valid=%llx, zero_user_segment_compr( %x, %lx )\n", from, v27, valid, start_page, PAGE_CACHE_SIZE ));
          zero_user_segment( page, start_page, PAGE_CACHE_SIZE );
        }

        set_buffer_uptodate( bh );
        clear_buffer_dirty( bh );

      } while ( head != (bh = bh->b_this_page) );

      goto ok;
    }
#endif

    bh->b_blocknr = v2l.map.lbo >> blkbits;

//    if ( FlagOn( v2l.map.flags, UFSD_MAP_LBO_NEW ) )
//      unmap_underlying_metadata( sb->s_bdev, bh->b_blocknr );

    DebugTrace( 0, UFSD_LEVEL_VFS_WBWE, ("write_begin: set_buffer_mapped %llx, bh=%" PSCT "x\n", v2l.vbo, bh->b_blocknr ));
    set_buffer_mapped( bh );

check_bh:
    if ( buffer_uptodate( bh ) )
      continue;

    if ( PageUptodate( page ) ) {
      set_buffer_uptodate( bh );
      continue;
    }

    if ( ( v2l.vbo < pos && pos < block_end ) || ( v2l.vbo < to && to < block_end ) ) {
      read_lock_irqsave( &u->valid_lock, lockf );
      valid = u->valid;
      read_unlock_irqrestore( &u->valid_lock, lockf );
      if ( v2l.vbo < valid ) {
        DebugTrace( 0, UFSD_LEVEL_VFS_WBWE, ("write_begin: read %llx, bh=%" PSCT "x\n", v2l.vbo, bh->b_blocknr ));
        ll_rw_block( READ, 1, &bh );
        *wait_bh++=bh;
      } else {
        DebugTrace( 0, UFSD_LEVEL_VFS_WBWE, ("write_begin: zero_user %llx, bh=%" PSCT "x + %lx\n", v2l.vbo, bh->b_blocknr, bh_offset( bh ) ));
        zero_user( page, bh_offset( bh ), blocksize );
        set_buffer_uptodate( bh );
      }
    }

  } while( v2l.vbo = block_end, head != (bh = bh->b_this_page) );

  unlock_sema( u, &v2l );

  //
  // If we issued read requests - let them complete.
  //
  while( unlikely( wait_bh > wait ) )  {
    bh = *--wait_bh;
    wait_on_buffer( bh );
    DebugTrace( 0, UFSD_LEVEL_VFS_WBWE, ("write_begin wait: %" PSCT "x, from=%llx, v=%llx\n", bh->b_blocknr, from, valid ));
    if ( !buffer_uptodate( bh ) ) {
      if ( !err )
        err = -EIO;
      ClearPageUptodate( page );
    } else {
      loff_t v27 = bh_offset( bh ) + from;

      if ( valid < v27 + blocksize ) {
        unsigned start = valid > v27? (valid - v27) : 0;
        unsigned start_page = start + bh_offset( bh );
        DebugTrace( 0, UFSD_LEVEL_VFS_WBWE, ( "write_begin: from=%llx, v27=%llx, valid=%llx, zero_user_segment( %x, %lx )\n", from, v27, valid, start_page, PAGE_CACHE_SIZE ));
        zero_user_segment( page, start_page, PAGE_CACHE_SIZE );
      }
    }
  }

  if ( likely( !err ) ){
ok:
    *pagep = page;
  } else {
unlock_page:
    unlock_page( page );
    page_cache_release( page );

restore:
    ufsd_printk(sb, "failed to begin write (error %d).", -err);

    if ( to > i->i_size )
      ufsd_set_size( i, to, i->i_size );

fin:
    *pagep = NULL;
  }

  ProfileLeave( sbi, write_begin );

  DebugTrace( -1, UFSD_LEVEL_VFS_WBWE, ("write_begin: -> %d, sz=%llx,%llx\n", err, u->valid, i->i_size ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_write_end
//
// address_space_operations::write_end
///////////////////////////////////////////////////////////
static int
ufsd_write_end(
    IN struct file  *file,
    IN struct address_space *mapping,
    IN loff_t       pos,
    IN unsigned     len,
    IN unsigned     copied,
    IN struct page  *page,
    IN void         *fsdata
    )
{
  int err;
  unsigned ret;
  struct inode *i = page->mapping->host;
  const unsigned blkbits = i->i_blkbits;
  unode *u        = UFSD_U( i );
  loff_t end, valid;
  unsigned long flags;
//  int dirty = 0;

  assert( copied <= len );
  assert( page->index == (pos >> PAGE_CACHE_SHIFT) );

  DebugTrace( +1, UFSD_LEVEL_VFS_WBWE, ("write_end: r=%lx pos=%llx,%x,%x s=%llx,%llx, pf=%lx\n",
                        i->i_ino, pos, len, copied, u->valid, i->i_size, page->flags ));

  ProfileEnter( UFSD_SB(i->i_sb), write_end );

#ifdef UFSD_NTFS2
  assert( NULL == file || !is_stream( file ) );
#endif

  err = ret = copied >= len || PageUptodate( page )? copied : 0;
  end = ret + pos;

  if ( unlikely( page_has_buffers( page ) ) ) {
    struct buffer_head *bh, *head;
    loff_t block_start = (loff_t)page->index << PAGE_CACHE_SHIFT;
    const unsigned blocksize = 1 << blkbits;
    loff_t block_end = block_start + blocksize;
    int partial = 0;

    bh = head = page_buffers( page );

    do {
      if ( block_end <= pos || block_start >= end ) {
        if ( !buffer_uptodate( bh ) )
          partial = 1;
      } else {
        set_buffer_uptodate( bh );
        mark_buffer_dirty( bh );
      }
      block_start = block_end;
      block_end  += blocksize;
    } while ( head != ( bh = bh->b_this_page ) );

    if ( !partial && !PageUptodate( page ) )
      SetPageUptodate( page );
  } else {
    if ( likely( copied == len ) ) {
      if ( !PageUptodate( page ) )
        SetPageUptodate( page );
      set_page_dirty(page);
    }
  }

  read_lock_irqsave( &u->valid_lock, flags );
  valid = u->valid;
  read_unlock_irqrestore( &u->valid_lock, flags );

  if ( end > valid ) {
    loff_t i_bytes;
    write_lock_irqsave( &u->valid_lock, flags );
    i_bytes = inode_get_bytes( i );
//    assert( i_bytes >= i->i_size );
    BUG_ON( 0 != i_bytes && end > i_bytes );

    u->ver  += 1;
    u->valid = end;
    if ( end > i->i_size ) {
      i_size_write( i, end );
//      assert( i_bytes >= end );
    }
    write_unlock_irqrestore( &u->valid_lock, flags );

    mark_inode_dirty_sync( i );
//    dirty = test_and_set_bit( 3, &u->_d8 );
  }

  unlock_page( page );
  page_cache_release( page );

//  if ( !dirty )
//    mark_inode_dirty_sync( i );

  if ( copied != len ){
    loff_t to = pos + len;
    BUG_ON( copied > len );
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("write_end: copied %x < len %x\n", copied, len ) );
    if ( err )
      ufsd_printk( i->i_sb, "partial write (orig copied %u, len %u, actual copied %u).", copied, len, err );
    else
      ufsd_printk( i->i_sb, "write failed (orig copied %u, len %u, actual copied 0).", copied, len );

    if ( to > i->i_size )
      ufsd_set_size( i, to, i->i_size );
  }

  ProfileLeave( UFSD_SB(i->i_sb), write_end );

  DebugTrace( -1, UFSD_LEVEL_VFS_WBWE, (err > 0? "write_end: -> %x s=%llx,%llx\n" : "write_end: -> %d s=%llx,%llx\n", err, u->valid, i->i_size) );
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_readpages
//
// based on fs/mpage.c 'mpage_readpages'
//        mm/filemap.c 'add_to_page_cache_lru'
// address_space_operations::readpages
///////////////////////////////////////////////////////////
static int
ufsd_readpages(
    IN struct file          *file,
    IN struct address_space *mapping,
    IN struct list_head     *pages,
    IN unsigned             nr_pages
    )
{
  int err = 0;
  upage_data  mpage;
  struct inode *i = mapping->host;
  unode *u = UFSD_U( i );

  DebugTrace( +1, Dbg, ("readpages r=%lx %llx + %x\n", i->i_ino, (UINT64)(list_entry(pages->prev, struct page, lru)->index), nr_pages ));

  ProfileEnter( UFSD_SB( i->i_sb ), readpages );

  mpage.bio = NULL;
  mpage.version = u->ver - 1;

  for ( ; 0 != nr_pages; nr_pages-- ) {
    struct page *page = list_entry(pages->prev, struct page, lru);
    prefetchw( &page->flags );
    list_del( &page->lru );

    if ( mapping_cap_swap_backed( mapping ) )
      SetPageSwapBacked( page );

    if ( 0 == add_to_page_cache( page, mapping, page->index, GFP_NOFS ) ) {
      int err2 = ufsd_do_readpage( page, nr_pages, &mpage );
      if ( err2 ) {
        ufsd_printk( i->i_sb, "Failed to read page for inode r=0x%lx, page index 0x%llx (error %d).", i->i_ino, (UINT64)page->index, err2 );
        if ( !err )
          err = err2;
      }

      PageSwapBacked( page )? lru_cache_add_anon( page ) : lru_cache_add_file( page );
    }

    page_cache_release( page );
  }

  BUG_ON(!list_empty(pages));

  if ( mpage.bio )
    ufsd_bio_submit( READ, mpage.bio );

  ProfileLeave( UFSD_SB( i->i_sb ), readpages );
  DebugTrace( -1, Dbg, ("readpages -> %d%s\n", err, mpage.bio? ", submitted":"" ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_writepages
//
// address_space_operations::writepages
///////////////////////////////////////////////////////////
static int
ufsd_writepages(
    IN struct address_space     *mapping,
    IN struct writeback_control *wbc
    )
{
  int err;
  // Save current 'nr_to_write' to show the number of written pages on exit
  TRACE_ONLY( long nr = wbc->nr_to_write; )
  struct inode* i = mapping->host;
  unode *u = UFSD_U( i );
  upage_data mpage;

  ProfileEnter( UFSD_SB(i->i_sb), writepages );

  DebugTrace( +1, Dbg, ("%u: writepages r=%lx, %ld \"%s\"\n", jiffies_to_msecs(jiffies-StartJiffies), i->i_ino, wbc->nr_to_write, current->comm ));

  // TODO: update 'arch' bit for exfat/ntfs
  mpage.bio  = NULL;
  mpage.version = u->ver - 1;

  err = write_cache_pages( mapping , wbc, (writepage_t)ufsd_do_writepage, &mpage );

  if ( mpage.bio )
    ufsd_bio_submit( WRITE, mpage.bio );

  ProfileLeave( UFSD_SB(i->i_sb), writepages );

  if ( 0 == err ) {
    DebugTrace( -1, Dbg, ("%u: writepages -> ok, %ld%s\n", jiffies_to_msecs(jiffies-StartJiffies), nr - wbc->nr_to_write, mpage.bio? ", submitted":"" ));
  } else {
    DebugTrace( -1, Dbg, ("writepages -> fail %d%s\n", err, mpage.bio? ", submitted":"" ));
    ufsd_printk( i->i_sb, "Failed due to error(s) for inode 0x%lx (error %d).", i->i_ino, err );
  }

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_bmap
//
// address_space_operations::bmap
///////////////////////////////////////////////////////////
static sector_t
ufsd_bmap(
    IN struct address_space *mapping,
    IN sector_t block
    )
{
  sector_t ret = 0;
  struct inode *i = mapping->host;
  struct super_block *sb = i->i_sb;

  DebugTrace( +1, Dbg, ("bmap (%" PSCT "x)\n", block ));

  if ( S_ISDIR( i->i_mode ) ) {
    ufsd_printk( sb, "BMAP only makes sense for files, returning 0." );
  } else {
    unsigned long flags;
    vbo2lbo v2l;
    loff_t valid, i_size;
    unode *u  = UFSD_U( i );
    v2l.vbo   = (loff_t)block << sb->s_blocksize_bits;

    read_lock_irqsave( &u->valid_lock, flags );
    valid   = u->valid;
    i_size  = i_size_read( i );
    read_unlock_irqrestore( &u->valid_lock, flags );
    if ( v2l.vbo < valid && ( v2l.vbo + sb->s_blocksize <= valid || i_size <= valid ) )
    {
      v2l.locked  = 0;
      if ( vbo_to_lbo( UFSD_SB( i->i_sb ), u, &v2l ) )
        ret = 0;
      else
        ret = v2l.map.lbo >> sb->s_blocksize_bits;

      unlock_sema( u, &v2l );
    }
  }

  DebugTrace( -1, Dbg, ("bmap -> %" PSCT "x\n", ret ));
  return ret;
}


#if 0 // def UFSD_TRACE
///////////////////////////////////////////////////////////
// ufsd_releasepage
//
// address_space_operations::releasepage
///////////////////////////////////////////////////////////
static int
ufsd_releasepage(
    IN struct page * page,
    IN gfp_t gfp_mask
    )
{
  struct address_space * const mapping = page->mapping;
  loff_t o = (loff_t)page->index << PAGE_CACHE_SHIFT;
  int ret = try_to_free_buffers( page );
  if ( NULL == mapping || NULL ==  mapping->host ) {
    DebugTrace( 0, Dbg, ("releasepage: o=%llx -> %d\n", o, ret ));
  } else {
    DebugTrace( 0, Dbg, ("releasepage: r=%lx, o=%llx -> %d\n", mapping->host->i_ino, o, ret ));
  }
  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_freepage
//
// address_space_operations::ufsd_aops
///////////////////////////////////////////////////////////
static void
ufsd_freepage(
    IN struct page * page
    )
{
  struct address_space * const mapping = page->mapping;
  loff_t o = (loff_t)page->index << PAGE_CACHE_SHIFT;
  if ( NULL == mapping || NULL == mapping->host ) {
    DebugTrace( 0, Dbg, ("freepage: o=%llx\n", o ));
  } else {
    DebugTrace( 0, Dbg, ("freepage: r=%lx, %llx\n", mapping->host->i_ino, o ));
  }
}
#define ufsd_releasepage  ufsd_releasepage
#define ufsd_freepage     ufsd_freepage
#endif


///////////////////////////////////////////////////////////
// ufsd_get_block_for_direct_IO
//
//
///////////////////////////////////////////////////////////
static int
ufsd_get_block_for_direct_IO(
    IN int                  rw,
    IN struct inode*        i,
    IN sector_t             iblock,
    IN struct buffer_head*  bh
    )
{
  unode *u                  = UFSD_U( i );
  const unsigned blkbits    = i->i_blkbits;
  const unsigned blocksize  = blkbits;
  unsigned long flags;
  vbo2lbo v2l;
  loff_t MaxOff;
//  loff_t block_off, MaxOff, Lbo, Len;
  int err;

  BUG_ON( !S_ISREG( i->i_mode ) );

  if ( rw ) {
    BUG_ON( WRITE != rw );
    MaxOff = inode_get_bytes( i );
    assert( 0 == ( MaxOff & UFSD_SB( i->i_sb )->cluster_mask ) );
    assert( MaxOff >= i->i_size );
  } else {
    read_lock_irqsave( &u->valid_lock, flags );
    MaxOff = u->valid;
    read_unlock_irqrestore( &u->valid_lock, flags );
  }

  v2l.vbo = (loff_t)iblock << blkbits;
  if ( v2l.vbo >= MaxOff )
    return 0;
  if ( v2l.vbo + bh->b_size > MaxOff )
    bh->b_size = ((MaxOff + blocksize - 1) & ~(loff_t)(blocksize - 1)) - v2l.vbo;

  v2l.locked = 0;
  err = vbo_to_lbo( UFSD_SB( i->i_sb ), u, &v2l );
  unlock_sema( u, &v2l );

  if ( err )
    return 0;

  if ( bh->b_size > v2l.map.len )
    bh->b_size = v2l.map.len;
  bh->b_bdev    = i->i_sb->s_bdev;
  bh->b_blocknr = v2l.map.lbo >> blkbits;
  set_buffer_mapped( bh );
  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_get_block_for_direct_IO_write
//
//
///////////////////////////////////////////////////////////
static int
ufsd_get_block_for_direct_IO_write(
    IN struct inode*        i,
    IN sector_t             iblock,
    IN struct buffer_head*  bh,
    IN int                  create
    )
{
  return ufsd_get_block_for_direct_IO( WRITE, i, iblock, bh );
}


///////////////////////////////////////////////////////////
// ufsd_get_block_for_direct_IO_read
//
//
///////////////////////////////////////////////////////////
static int
ufsd_get_block_for_direct_IO_read(
    IN struct inode*        i,
    IN sector_t             iblock,
    IN struct buffer_head*  bh,
    IN int                  create
    )
{
  return ufsd_get_block_for_direct_IO( READ, i, iblock, bh );
}


#if defined HAVE_DECL_BLOCKDEV_DIRECT_IO_V1 && HAVE_DECL_BLOCKDEV_DIRECT_IO_V1
  #define Blockdev_direct_IO( rw, iocb, i, bdev, iov, offset, nr_segs, get_block )  \
      blockdev_direct_IO( rw, iocb, i, bdev, iov, offset, nr_segs, get_block, NULL )
#elif defined HAVE_DECL_BLOCKDEV_DIRECT_IO_V2 && HAVE_DECL_BLOCKDEV_DIRECT_IO_V2
  #define Blockdev_direct_IO( rw, iocb, i, bdev, iov, offset, nr_segs, get_block )  \
      blockdev_direct_IO( rw, iocb, i, iov, offset, nr_segs, get_block )
#else
  #error "Unknown type blockdev_direct_IO"
#endif


///////////////////////////////////////////////////////////
// ufsd_direct_IO
//
// address_space_operations::direct_IO
///////////////////////////////////////////////////////////
static ssize_t
ufsd_direct_IO(
    IN int                rw,
    IN struct kiocb       *iocb,
    IN const struct iovec *iov,
    IN loff_t             offset,
    IN unsigned long      nr_segs
    )
{
  struct inode *i = iocb->ki_filp->f_mapping->host;
  unode *u        = UFSD_U( i );
  unsigned long flags;
  loff_t valid, NewSize, i_size;
  struct page **pages;
  const struct iovec *iov_last = iov + nr_segs;
  size_t uaddr, len;
  ssize_t ret;

  DebugTrace( +1, Dbg, ("direct_IO: %s, %llx, %lu s=%llx,%llx\n",
              (rw&WRITE)? "w":"r", offset, nr_segs, u->valid, i->i_size ));

  if ( WRITE == rw ) {
    ret = Blockdev_direct_IO( WRITE, iocb, i, i->i_sb->s_bdev, iov, offset, nr_segs, ufsd_get_block_for_direct_IO_write );

    if ( ret > 0 ) {
      read_lock_irqsave( &u->valid_lock, flags );
      valid = u->valid;
      read_unlock_irqrestore( &u->valid_lock, flags );
      BUG_ON( offset > valid );

      NewSize = offset + ret;
      if ( NewSize > valid && !S_ISBLK( i->i_mode ) ) {
        loff_t i_bytes = inode_get_bytes( i );

        assert( 0 == ( i_bytes & UFSD_SB( i->i_sb )->cluster_mask ) );
        assert( i_bytes >= i->i_size );

        BUG_ON( NewSize > i_bytes );

        i_size_write( i, NewSize );
        write_lock_irqsave( &u->valid_lock, flags );
        u->ver += 1;
        u->valid = NewSize;
        write_unlock_irqrestore( &u->valid_lock, flags );
//        if ( !test_and_set_bit( 3, &unode->_d8 ) )
          mark_inode_dirty_sync( i );
      }
    }
  } else {
    BUG_ON( READ != rw );

    read_lock_irqsave( &u->valid_lock, flags  );
    valid = u->valid;
    i_size = i_size_read( i );
    read_unlock_irqrestore( &u->valid_lock, flags );
    if ( valid >= i_size ) {
      ret = Blockdev_direct_IO( READ, iocb, i, i->i_sb->s_bdev, iov, offset, nr_segs, ufsd_get_block_for_direct_IO_read );
      goto out;
    }

    if ( offset < valid ) {
      loff_t tail_valid = valid - offset;
      loff_t done_read  = iov[0].iov_len;
      unsigned long seg;
      size_t iov_off;

      //
      // How many segments until valid
      //
      for ( seg = 1; seg < nr_segs && done_read < tail_valid; seg += 1 )
        done_read += iov[seg].iov_len;

      ret = Blockdev_direct_IO( READ, iocb, i, i->i_sb->s_bdev, iov, offset, seg, ufsd_get_block_for_direct_IO_read );

      if ( seg >= nr_segs || ret < tail_valid )
        goto out;

      if ( ret > tail_valid )
        ret = tail_valid;
      offset += ret;
      iov    += seg;
      iov_off = iov->iov_len + tail_valid - done_read;
      uaddr   = iov_off + (size_t)iov->iov_base;
      len     = iov->iov_len - iov_off;
    } else {
      ret     = 0;
      uaddr   = (size_t)iov->iov_base;
      len     = iov->iov_len;
    }

    if ( iov >= iov_last )
      goto out;

    //
    // Zero the rest of memory
    //
    pages = kmalloc( 64 * sizeof(struct page*), GFP_KERNEL );
    if ( NULL == pages ) {
      ret = -ENOMEM;
      goto out;
    }

    for ( ;; ) {
      size_t nr_pages = ((uaddr + len + PAGE_SIZE - 1) >> PAGE_SHIFT) - (uaddr >> PAGE_SHIFT);

      while( nr_pages ) {
        long page_idx, mapped_pages;
        size_t to_zero;
        unsigned off_in_page;

        down_read( &current->mm->mmap_sem );
        mapped_pages = get_user_pages( current, current->mm, uaddr, min_t( unsigned long, nr_pages, 64 ), 1, 0, pages, 0 );
        up_read( &current->mm->mmap_sem );

        if ( mapped_pages <= 0 ) {
          if ( !ret )
            ret = mapped_pages;
          goto end_zero;
        }

        nr_pages   -= mapped_pages;
        off_in_page = uaddr & ~PAGE_CACHE_MASK;
        to_zero     = (mapped_pages << PAGE_CACHE_SHIFT) - off_in_page;

        for ( page_idx = 0; page_idx < mapped_pages; page_idx++ ) {
          struct page *page = pages[page_idx];
          unsigned tail     = PAGE_CACHE_SIZE - off_in_page;
          assert( 0 != len );
          if ( tail > len )
            tail = len;

          ret     += tail;
          offset  += tail;

          //
          // Zero full page after 'i_size'
          //
          zero_user_segment( page, off_in_page, offset >= i_size? PAGE_CACHE_SIZE : off_in_page + tail );

          if ( offset >= i_size ) {
            ret -= offset - i_size;
            while( page_idx < mapped_pages )
              page_cache_release( pages[page_idx++] );
            goto end_zero;
          }
          page_cache_release( page );
          off_in_page  = 0;
          len -= tail;
        }

        assert( (0 == nr_pages) == ( 0 == len ) );
        uaddr += to_zero;
      }

      if ( ++iov >= iov_last )
        break;

      uaddr = (size_t)iov->iov_base;
      len   = iov->iov_len;
    }

end_zero:
    kfree( pages );
  }

out:
  if ( ret > 0 ) {
    DebugTrace( -1, Dbg, ("direct_IO -> %Zx\n", ret ));
  } else {
    DebugTrace( -1, Dbg, ("direct_IO -> %d\n", (int)ret ));
  }
  return ret;
}


//
// Address space operations
//
static const struct address_space_operations ufsd_aops = {
  .writepage      = ufsd_writepage,
  .readpage       = ufsd_readpage,
  .writepages     = ufsd_writepages,
  .readpages      = ufsd_readpages,
  .write_begin    = ufsd_write_begin,
  .write_end      = ufsd_write_end,
  .bmap           = ufsd_bmap,
#ifdef ufsd_releasepage
  .releasepage    = ufsd_releasepage,
  .freepage       = ufsd_freepage,
#endif
  .direct_IO      = ufsd_direct_IO,
#if defined HAVE_STRUCT_ADDRESS_SPACE_IS_PARTIALLY_UPTODATE && HAVE_STRUCT_ADDRESS_SPACE_IS_PARTIALLY_UPTODATE
  .is_partially_uptodate  = block_is_partially_uptodate,
#endif
#if defined HAVE_STRUCT_ADDRESS_SPACE_ERROR_REMOVE_PAGE && HAVE_STRUCT_ADDRESS_SPACE_ERROR_REMOVE_PAGE
  .error_remove_page  = generic_error_remove_page,
#endif
};


static struct kmem_cache *unode_cachep;

///////////////////////////////////////////////////////////
// ufsd_alloc_inode
//
// super_operations::alloc_inode
///////////////////////////////////////////////////////////
static struct inode*
ufsd_alloc_inode(
    IN struct super_block *sb
    )
{
  unode *u = kmem_cache_alloc( unode_cachep, GFP_KERNEL );
  if ( NULL == u )
    return NULL;

  //
  // NOTE: explicitly zero all unode members from 'ufile' until 'i'
  //
  memset( &u->ufile, 0, offsetof(unode,i) - offsetof(unode,ufile) );

#if defined UFSD_USE_XATTR && !(defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL)
  assert( ACL_NOT_CACHED == u->acl );
  assert( ACL_NOT_CACHED == u->default_acl );
#endif

  return &u->i;
}


///////////////////////////////////////////////////////////
// ufsd_destroy_inode
//
// super_operations::destroy_inode
///////////////////////////////////////////////////////////
static void
ufsd_destroy_inode(
    IN struct inode *i
    )
{
  kmem_cache_free( unode_cachep, UFSD_U( i ) );
}


///////////////////////////////////////////////////////////
// init_once
//
// callback function for 'Kmem_cache_create'
///////////////////////////////////////////////////////////
static void
init_once(
#if ( defined HAVE_DECL_KMEM_CACHE_CREATE_V1 && HAVE_DECL_KMEM_CACHE_CREATE_V1 ) \
 || ( defined HAVE_DECL_KMEM_CACHE_CREATE_V2 && HAVE_DECL_KMEM_CACHE_CREATE_V2 )
    IN void           *foo,
    IN struct kmem_cache *cachep,
    IN unsigned long  flags
#elif defined HAVE_DECL_KMEM_CACHE_CREATE_V3 && HAVE_DECL_KMEM_CACHE_CREATE_V3
    IN struct kmem_cache *cachep,
    IN void           *foo
#elif defined HAVE_DECL_KMEM_CACHE_CREATE_V4 && HAVE_DECL_KMEM_CACHE_CREATE_V4
    IN void           *foo
#endif
    )
{
  unode *u = (unode *)foo;

  //
  // NOTE: once init unode members from start to 'ufile'
  //
#if defined SLAB_CTOR_CONSTRUCTOR && defined SLAB_CTOR_VERIFY
  if ( (flags & (SLAB_CTOR_VERIFY|SLAB_CTOR_CONSTRUCTOR)) != SLAB_CTOR_CONSTRUCTOR )
    return;
#endif

  rwlock_init( &u->valid_lock );
  init_rwsem( &u->size_sema );

#if defined UFSD_USE_XATTR && !(defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL)
  u->acl = u->default_acl = ACL_NOT_CACHED;
#endif

  inode_init_once( &u->i );
}


///////////////////////////////////////////////////////////
// ufsd_symlink
//
// inode_operations::symlink
///////////////////////////////////////////////////////////
static int
ufsd_symlink(
    IN struct inode   *dir,
    IN struct dentry  *de,
    IN const char     *symname
    )
{
  struct inode *i = NULL;
  int err;

  ucreate  cr;

  cr.lnk  = NULL;
  cr.data = symname;
  cr.len  = strlen( symname ) + 1;
  cr.mode = S_IFLNK;

  DebugTrace( +1, Dbg, ("symlink: r=%lx /\"%.*s\" => \"%s\"\n",
              dir->i_ino, (int)de->d_name.len, de->d_name.name, symname ));

  if ( cr.len > dir->i_sb->s_blocksize ) {
    DebugTrace( 0, Dbg, ("symlink name is too long\n" ));
    err = -ENAMETOOLONG;
    goto out;
  }

  err = ufsd_create_or_open( dir, de, &cr, &i );

  if ( 0 == err ) {

    assert( NULL != i && NULL != UFSD_FH(i) );

    i->i_mode &= ~(S_IFDIR | S_IFREG);
    i->i_mode |= S_IFLNK;

#ifdef UFSD_HFS_ONLY
    // hfs+
    i->i_op = &ufsd_link_inode_operations_u8;
    mutex_lock( &i->i_mutex );
    err = page_symlink( i, symname, cr.len );
    mutex_unlock( &i->i_mutex );
#elif defined UFSD_HFS
    // hfs+/ntfs/exfat
    if ( UFSD_SB( i->i_sb )->options.hfs ) {
      i->i_op = &ufsd_link_inode_operations_u8;
      mutex_lock( &i->i_mutex );
      err = page_symlink( i, symname, cr.len );
      mutex_unlock( &i->i_mutex );
    } else {
      i->i_op = &ufsd_link_inode_operations_ufsd;
    }
#else
    i->i_op = &ufsd_link_inode_operations_ufsd;
#endif

    if ( 0 == err ) {
      d_instantiate( de, i );
    } else {
      drop_nlink( i );
    }

    mark_inode_dirty_sync( i );

    if ( 0 != err )
      iput( i );
  }

out:
  DebugTrace( -1, Dbg, ("symlink -> %d\n", err ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_read_inode2
//
// read_inode2() callback
// 'opaque' is passed from iget4()
///////////////////////////////////////////////////////////
static void
ufsd_read_inode2(
    IN struct inode *i,
    IN OUT void     *param
    )
{
  unode *u                = UFSD_U( i );
  ufsd_iget4_param *p     = (ufsd_iget4_param*)param;
  const ucreate *cr       = p->Create;
  const finfo *fi         = p->fi;
  struct super_block *sb  = i->i_sb;
  usuper *sbi             = UFSD_SB( sb );
  int check_special       = 0;
  mode_t mode;

  C_ASSERT( UFSD_UNODE_FLAG_SPARSE == UFSDAPI_SPARSE && UFSD_UNODE_FLAG_COMPRESS == UFSDAPI_COMPRESSED
         && UFSD_UNODE_FLAG_ENCRYPT == UFSDAPI_ENCRYPTED && UFSD_UNODE_FLAG_EA == UFSDAPI_EA );

  //
  // Next members are set at this point:
  //
  // i->i_sb    = sb;
  // i->i_dev   = sb->s_dev;
  // i->i_blkbits = sb->s_blocksize_bits;
  // i->i_ino   = fi->Id;
  // i->i_flags = 0;
  //
  assert( i->i_ino == fi->Id );
//  assert( NULL == p->lnk );
  assert( 1 == atomic_read( &i->i_count ) );

  i->i_op = NULL;

  //
  // Setup 'uid' and 'gid'
  //
  i->i_uid = KUIDT_INIT( unlikely(sbi->options.uid)? sbi->options.fs_uid : cr? cr->uid : FlagOn( fi->Attrib, UFSDAPI_UGM )? fi->Uid : sbi->options.fs_uid );
  i->i_gid = KGIDT_INIT( unlikely(sbi->options.gid)? sbi->options.fs_gid : cr? cr->gid : FlagOn( fi->Attrib, UFSDAPI_UGM )? fi->Gid : sbi->options.fs_gid );

  //
  // Setup 'mode'
  //
  if ( FlagOn( fi->Attrib, UFSDAPI_SUBDIR ) ) {
    if ( sbi->options.dmask ) {
      // use mount options "dmask" or "umask"
      mode = S_IRWXUGO & sbi->options.fs_dmask;
    } else if ( NULL != cr ) {
      mode = cr->mode;
      check_special = 1;
    } else if ( FlagOn( fi->Attrib, UFSDAPI_UGM ) ) {
      // no mount options "dmask"/"umask" and fs supports "ugm"
      mode     = fi->Mode;
      check_special = 1;
    } else if ( NULL == sb->s_root ) {
      // Read root inode while mounting
      mode = S_IRWXUGO;
    } else {
      // by default ~(current->fs->umask)
      mode = S_IRWXUGO & sbi->options.fs_dmask;
    }
  } else {
    if ( sbi->options.fmask ) {
      // use mount options "fmask" or "umask"
      mode = S_IRWXUGO & sbi->options.fs_fmask;
    } else if ( NULL != cr ) {
      mode = cr->mode;
      check_special = 1;
    } else if ( FlagOn( fi->Attrib, UFSDAPI_UGM ) ) {
      // no mount options "fmask"/"umask" and fs supports "ugm"
      mode     = fi->Mode;
      check_special = 1;
    } else {
      // by default ~(current->fs->umask)
      mode = S_IRWXUGO & sbi->options.fs_fmask;
    }
  }

  i->i_mode = mode;
  if ( check_special && ( S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) || S_ISSOCK(mode) ) ) {
    init_special_inode( i, mode, new_decode_dev( fi->Dev ) );
    i->i_op = &ufsd_special_inode_operations;
  } else {
    assert( NULL == cr || !FlagOn( fi->Attrib, UFSDAPI_UGM ) || cr->mode == fi->Mode );
  }

  i->i_version    = 0;
  i->i_generation = p->fi->Gen; // Used by NFS
  ufsd_times_to_inode( sbi, u, i, fi );
  i->i_size     = fi->FileSize;

  //
  // Setup unode
  //
  u->flags = (u->flags & ~UFSD_UNODE_FLAG_API_FLAGS) | (fi->Attrib & UFSD_UNODE_FLAG_API_FLAGS);

//  u->sparse   = FlagOn( fi->FSAttrib, UFSDAPI_SPARSE );
//  u->compr    = FlagOn( fi->FSAttrib, UFSDAPI_COMPRESSED );
//  u->encrypt  = FlagOn( fi->FSAttrib, UFSDAPI_ENCRYPTED );
//  u->xattr    = FlagOn( fi->Attrib, UFSDAPI_EA );
  u->valid    = fi->ValidSize;
  assert( NULL == p->fh || FlagOn( fi->Attrib, UFSDAPI_VSIZE ) );
//  BUG_ON( 0 != u->len );
//  assert( fi->ValidSize <= fi->FileSize );
//  assert( fi->FileSize <= fi->AllocSize );

#ifdef UFSD_NTFS2
  if ( FlagOn( fi->Attrib, UFSDAPI_TASIZE ) ) {
    assert( is_sparsed_or_compressed( u ) );
    u->total_alloc = fi->TotalAllocSize;
  }
#endif

  inode_set_bytes( i, fi->AllocSize );

  if ( NULL != i->i_op ) {
    ;
  } else if ( FlagOn( fi->Attrib, UFSDAPI_SUBDIR ) ) {
    // dot and dot-dot should be included in count but was not included
    // in enumeration.
    assert( 1 == fi->HardLinks ); // Usually a hard link to directories are disabled
#ifdef UFSD_COUNT_CONTAINED
    set_nlink( i, fi->HardLinks + p->subdir_count + 1 );
#else
    set_nlink( i, 1 );
#endif
    i->i_op   = &ufsd_dir_inode_operations;
    i->i_fop  = &ufsd_dir_operations;
    i->i_mode |= S_IFDIR;
  } else {
    set_nlink( i, fi->HardLinks );
    i->i_op     = &ufsd_file_inode_ops;
    i->i_fop    = &ufsd_file_ops;
    i->i_mapping->a_ops = &ufsd_aops;
    i->i_mode |= S_IFREG;
  }

  if ( FlagOn( fi->Attrib, UFSDAPI_RDONLY ) )
    i->i_mode &= ~S_IWUGO;

  if ( FlagOn( fi->Attrib, UFSDAPI_LINK ) ) {
    // ntfs supports dir-symlinks but vfs preffers links to be files
    i->i_mode &= ~(S_IFDIR | S_IFREG);
    i->i_mode |= S_IFLNK;

#ifdef UFSD_HFS_ONLY
    // hfs+
    i->i_op = &ufsd_link_inode_operations_u8;
#elif defined UFSD_HFS
    // hfs+/ntfs/exfat
    i->i_op = sbi->options.hfs? &ufsd_link_inode_operations_u8 : &ufsd_link_inode_operations_ufsd;
#else
    // ntfs/exfat
    i->i_op = &ufsd_link_inode_operations_ufsd;
#endif
    i->i_fop   = NULL;
  }

  if ( sbi->options.sys_immutable && FlagOn( fi->Attrib, UFSDAPI_SYSTEM )
    && !( S_ISFIFO(i->i_mode) || S_ISSOCK(i->i_mode) || S_ISLNK(i->i_mode) ) )
  {
    DebugTrace( 0, 0, ("Set inode r=%lx immutable\n", i->i_ino) );
    i->i_flags |= S_IMMUTABLE;
  }
  else
    i->i_flags &= ~S_IMMUTABLE;
#ifdef S_PRIVATE
  i->i_flags |= S_PRIVATE;  // ???
#endif

  u->ufile  = p->fh;

  if ( NULL == u->ufile )
    SetFlag( u->flags, 1u << UFSD_UNODE_FLAG_LAZY_OPEN_BIT );
  if ( S_ISREG( i->i_mode ) )
    SetFlag( u->flags, 1u << UFSD_UNODE_FLAG_LAZY_INIT_BIT );
  p->fh     = NULL;
}


///////////////////////////////////////////////////////////
// ufsd_create_or_open
//
//  This routine is a callback used to load or create inode for a
//  direntry when this direntry was not found in dcache or direct
//  request for create or mkdir is being served.
///////////////////////////////////////////////////////////
static int
ufsd_create_or_open(
    IN struct inode       *dir,
    IN OUT struct dentry  *de,
    IN ucreate            *cr,
    OUT struct inode      **inode
    )
{
  ufsd_iget4_param param;
  struct inode *i = NULL;
  usuper *sbi     = UFSD_SB( dir->i_sb );
  int err = -ENOENT;
#ifdef UFSD_USE_XATTR
  struct posix_acl *acl = NULL;
#endif
  unsigned char *p = 0 == sbi->options.delim? NULL : strchr( de->d_name.name, sbi->options.delim );
  TRACE_ONLY( const char *hint = NULL==cr?"open":S_ISDIR(cr->mode)?"mkdir":cr->lnk?"link":S_ISLNK(cr->mode)?"symlink":cr->data?"mknode":"create"; )

  param.Create        = cr;
  param.subdir_count  = 0;
  param.name          = de->d_name.name;
  param.name_len      = NULL == p? de->d_name.len : p - de->d_name.name;
#if !(defined HAVE_STRUCT_SUPER_BLOCK_S_D_OP && HAVE_STRUCT_SUPER_BLOCK_S_D_OP)
  de->d_op = sbi->options.nocase? &ufsd_dop : NULL;
#endif

  DebugTrace( +1, Dbg, ("%s: r=%lx '%s' m=%o\n", hint, dir->i_ino, de->d_name.name, NULL == cr? 0u : (unsigned)cr->mode ));
//  DebugTrace( +1, Dbg, ("%s: %p '%.*s'\n", hint, dir, (int)param.name_len, param.name));

  //
  // The rest to be set in this routine
  // follows the attempt to open the file.
  //
  lock_ufsd( sbi );

  if ( NULL != dir && 0 != lazy_open( sbi, dir ) ) {
    // Failed to open parent directory
    goto Exit;
  }

  if ( NULL != cr ) {
    struct inode *lnk = (struct inode*)cr->lnk;
    if ( NULL != lnk ) {
      if ( 0 != lazy_open( sbi, lnk ) ) {
        // Failed to open link node
        goto Exit;
      }
      cr->lnk = UFSD_FH( lnk );
    }
    cr->uid = __kuid_val( current_fsuid() );
    if ( !(dir->i_mode & S_ISGID) )
      cr->gid = __kgid_val( current_fsgid() );
    else {
      cr->gid = __kgid_val( dir->i_gid );
      if ( S_ISDIR(cr->mode) )
        cr->mode |= S_ISGID;
    }

    if ( NULL == cr->lnk ) {
#ifdef UFSD_USE_XATTR
      if ( sbi->options.acl ) {
        acl = ufsd_get_acl_ex( dir, ACL_TYPE_DEFAULT, 1 );
        if ( IS_ERR( acl ) ) {
          err = PTR_ERR( acl );
          acl = NULL;
          goto Exit;
        }
      }
      if ( NULL == acl )
#endif
        cr->mode &= ~current_umask();
    }
  }

  err = ufsdapi_file_open( sbi->ufsd, UFSD_FH( dir ), param.name, param.name_len,
                          cr,
#ifdef UFSD_COUNT_CONTAINED
                          &param.subdir_count,
#else
                          NULL,
#endif
                          &param.fh, &param.fi );

  switch( err ) {
  case 0: break;
  case ERR_BADNAME_LEN: err = -ENAMETOOLONG; goto Exit;
  case ERR_NOTIMPLEMENTED: err = -ENOSYS; goto Exit;
  case ERR_WPROTECT:  err = -EROFS; goto Exit;
  case ERR_NOSPC:  err = -ENOSPC; goto Exit;
  default:  err = -ENOENT; goto Exit;
  }

  assert( NULL == cr || NULL != param.fh );
  assert( NULL != dir || FlagOn( param.fi->Attrib, UFSDAPI_SUBDIR ) ); // root must be directory

  //
  // Load and init inode
  // iget4 calls ufsd_read_inode2 for new nodes
  // if node was not loaded then param.fh will be copied into UFSD_FH(inode)
  // and original param.fh will be zeroed
  // if node is already loaded then param.fh will not be changed
  // and we must to close it
  //
  i = iget4( dir->i_sb, param.fi->Id, NULL, &param );

  if ( NULL != param.fh ){
    // inode was already opened
    if ( NULL == i ) {
      DebugTrace( 0, Dbg, ("assert: i=NULL, new=%p\n", param.fh ));
    } else {
      DebugTrace( 0, Dbg, ("assert: i=%p, l=%x, old=%p, new=%p\n", i, i->i_nlink, UFSD_FH(i), param.fh ));
    }
    // UFSD handle was not used. Close it
    ufsdapi_file_close( sbi->ufsd, param.fh );
  }

  if ( NULL != i ) {
    assert( NULL == cr || NULL != UFSD_FH(i) );
    // OK
    err = 0;

    if ( NULL != cr ) {
      UINT64 dir_size = ufsdapi_get_dir_size( UFSD_FH( dir ) );

#ifdef UFSD_COUNT_CONTAINED
      if ( S_ISDIR ( i->i_mode ) )
        inc_nlink( dir );
#endif
      TIMESPEC_SECONDS( &dir->i_mtime ) = TIMESPEC_SECONDS( &dir->i_ctime ) = get_seconds();
      // Mark dir as requiring resync.
      dir->i_version += 1;
      i_size_write( dir, dir_size );
      inode_set_bytes( dir, dir_size );

      mark_inode_dirty_sync( dir );

      if ( NULL != cr->lnk ){
        i->i_ctime = dir->i_ctime;
      }
#ifdef UFSD_USE_XATTR
      else if ( NULL != acl ) {
        posix_acl_mode mode = i->i_mode;

        if ( !S_ISDIR( mode ) || 0 == ( err = ufsd_set_acl_ex( i, ACL_TYPE_DEFAULT, acl, 1 ) ) ) {
#if ( defined HAVE_DECL_POSIX_ACL_CREATE_V1 && HAVE_DECL_POSIX_ACL_CREATE_V1 ) || ( defined HAVE_DECL_POSIX_ACL_CREATE_V2 && HAVE_DECL_POSIX_ACL_CREATE_V2 )
#if defined HAVE_DECL_POSIX_ACL_CREATE_V2 && HAVE_DECL_POSIX_ACL_CREATE_V2
          err = __posix_acl_create( &acl, GFP_KERNEL, &mode );
#else
          err = posix_acl_create( &acl, GFP_KERNEL, &mode );
#endif
          if ( err >= 0 ) {
            if ( mode != i->i_mode ){
              i->i_mode = mode;
              mark_inode_dirty_sync( i );
              set_bit( UFSD_UNODE_FLAG_SET_MODE_BIT, &UFSD_U(i)->flags );
            }
            if ( err > 0 )
              err = ufsd_set_acl_ex( i, ACL_TYPE_ACCESS, acl, 1 );
          }
#else
          struct posix_acl *clone = posix_acl_clone( acl, GFP_KERNEL );
          if ( NULL == clone )
            err = -ENOMEM;
          else {
            err   = posix_acl_create_masq( clone, &mode );
            if ( err >= 0 ) {
              if ( mode != i->i_mode ){
                i->i_mode = mode;
                mark_inode_dirty_sync( i );
                set_bit( UFSD_UNODE_FLAG_SET_MODE_BIT, &UFSD_U(i)->flags );
              }
              if ( err > 0 )
                err = ufsd_set_acl_ex( i, ACL_TYPE_ACCESS, clone, 1 );
            }
            ufsd_posix_acl_release( clone );
          }
#endif
        }
        err = 0; // ignore any acl errors?
      }
#endif
    }
  }

Exit:
  unlock_ufsd( sbi );
#ifdef UFSD_USE_XATTR
  if ( NULL != acl )
    ufsd_posix_acl_release( acl );
#endif

  if ( 0 == err ) {
    DebugTrace( -1, Dbg, ("%s -> i=%p de=%p h=%p r=%llx l=%x m=%o%s\n",
                         hint, i, de, UFSD_FH(i),
                         param.fi->Id, i->i_nlink, i->i_mode, FlagOn( param.fi->Attrib, UFSDAPI_SPARSE )?",sp" : FlagOn( param.fi->Attrib, UFSDAPI_COMPRESSED )?",c":""));
  } else {
    DebugTrace( -1, Dbg, ("%s failed %d\n", hint, err ));
  }

  *inode = i;
  return err;
}


#ifdef UFSD_TRACE
///////////////////////////////////////////////////////////
// parse_trace_level
//
// parses string for trace level
///////////////////////////////////////////////////////////
static void
parse_trace_level(
    IN const char *v
    )
{
  if ( NULL == v || 0 == v[0] )
    ufsd_trace_level = UFSD_LEVEL_DEFAULT;
  else if ( 0 == strcmp( v, "all" ) )
    ufsd_trace_level = UFSD_LEVEL_STR_ALL;
  else if ( 0 == strcmp( v, "vfs" ) )
    ufsd_trace_level = UFSD_LEVEL_STR_VFS;
  else if ( 0 == strcmp( v, "lib" ) )
    ufsd_trace_level = UFSD_LEVEL_STR_LIB;
  else if ( 0 == strcmp( v, "mid" ) )
    ufsd_trace_level = UFSD_LEVEL_STR_MID;
  else if ( 0 == strcmp( v, "io" ) )
    ufsd_trace_level = UFSD_LEVEL_IO;
  else
    ufsd_trace_level = simple_strtoul( v, NULL, 16 );
  DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("%s: trace mask set to %08lx\n", v, ufsd_trace_level));
}


#ifndef CONFIG_VERSION_SIGNATURE
  #if defined HAVE_GENERATED_COMPILE_H && HAVE_GENERATED_COMPILE_H
    #include <generated/compile.h>
  #endif

  #if defined HAVE_GENERATED_UTSRELEASE_H && HAVE_GENERATED_UTSRELEASE_H
    #include <generated/utsrelease.h>
  #endif

  #ifndef UTS_RELEASE
    #define UTS_RELEASE ""
  #endif

  #ifndef UTS_VERSION
    #define UTS_VERSION ""
  #endif

  #define CONFIG_VERSION_SIGNATURE  UTS_RELEASE ", " UTS_VERSION
#endif


///////////////////////////////////////////////////////////
// trace_hdr
//
// Trace standard header into log file
///////////////////////////////////////////////////////////
void
trace_hdr( void )
{
  DebugTrace( 0, UFSD_LEVEL_ALWAYS, (CONFIG_VERSION_SIGNATURE"\n" ) );
  DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("Kernel version %d.%d.%d, cpus="_QUOTE2(NR_CPUS)"\n", LINUX_VERSION_CODE>>16,
                                      (LINUX_VERSION_CODE>>8)&0xFF, LINUX_VERSION_CODE&0xFF ));
  DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("%s", ufsdapi_library_version( NULL ) ) );
  DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("%s%s\n", s_FileVer, s_DriverVer ) );
  DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("Module address %p\n", UFSD_MODULE_CORE() ));
#ifdef UFSD_DEBUG
  DebugTrace( 0, UFSD_LEVEL_ALWAYS, ("sizeof(inode)=%Zu\n", sizeof(struct inode) ));
#endif
}
#endif // #ifdef UFSD_TRACE

#if defined CONFIG_PROC_FS

static struct proc_dir_entry *proc_info_root = NULL;
#define PROC_FS_UFSD_NAME "fs/ufsd"

#if !( defined HAVE_DECL_PDE_DATA && HAVE_DECL_PDE_DATA )
  #define PDE_DATA(X) PDE(X)->data
#endif

///////////////////////////////////////////////////////////
// ufsd_proc_dev_version_show
//
// /proc/fs/ufsd/version
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_version_show(
    IN struct seq_file  *m,
    IN void             *o
    )
{
  seq_printf( m, "%s%s\ndriver (%s) loaded at %p, sizeof(inode)=%u\n",
              ufsdapi_library_version( NULL ), s_FileVer, s_DriverVer, UFSD_MODULE_CORE(), (unsigned)sizeof(struct inode) );

#ifdef UFSD_DEBUG_ALLOC
  {
    size_t Mb = UsedMemMax/(1024*1024);
    size_t Kb = (UsedMemMax%(1024*1024)) / 1024;
    size_t b  = UsedMemMax%1024;
    if ( 0 != Mb ) {
      seq_printf( m, "Memory report: Peak usage %Zu.%03Zu Mb (%Zu bytes), kmalloc %Zu, vmalloc %Zu\n",
                  Mb, Kb, UsedMemMax, TotalKmallocs, TotalVmallocs );
    } else {
      seq_printf( m, "Memory report: Peak usage %Zu.%03Zu Kb (%Zu bytes), kmalloc %Zu, vmalloc %Zu\n",
                  Kb, b, UsedMemMax, TotalKmallocs, TotalVmallocs );
    }
    seq_printf( m, "Total allocated:  %Zu bytes in %Zu blocks, Max request %Zu bytes\n",
                  TotalAllocs, TotalAllocBlocks, MemMaxRequest );
  }
#endif

  return 0;
}

static int ufsd_proc_dev_version_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_version_show, NULL );
}

static const struct file_operations ufsd_proc_dev_version_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_version_open,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_dirty_show
//
// /proc/fs/ufsd/<dev>/dirty
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_dirty_show(
    IN struct seq_file  *m,
    IN void             *o
    )
{
  struct super_block *sb = m->private;
#ifdef UFSD_USE_FLUSH_THREAD
  seq_printf( m, "%u\n", (unsigned)UFSD_SB( sb )->bdirty );
#else
  seq_printf( m, "%u\n", (unsigned)sb->s_dirt );
#endif
  return 0;
}

static int ufsd_proc_dev_dirty_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_dirty_show, PDE_DATA(inode) );
}

static const struct file_operations ufsd_proc_dev_dirty_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_dirty_open,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_volinfo
//
// /proc/fs/ufsd/<dev>/volinfo
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_volinfo(
    IN struct seq_file  *m,
    IN void             *o
    )
{
  usuper *sbi = UFSD_SB( (struct super_block*)(m->private) );

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  ufsdapi_trace_volume_info( sbi->ufsd, m, &seq_printf );

  unlock_ufsd( sbi );
  return 0;
}

static int ufsd_proc_dev_volinfo_open(struct inode *inode, struct file *file)
{
  return single_open( file, ufsd_proc_dev_volinfo, PDE_DATA(inode) );
}

static const struct file_operations ufsd_proc_dev_volinfo_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_volinfo_open,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_label_show
//
// /proc/fs/ufsd/<dev>/label
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_label_show(
    OUT struct seq_file *m,
    IN void             *o
    )
{
  usuper *sbi = UFSD_SB( (struct super_block*)(m->private) );
  char *Label = kmalloc( PAGE_SIZE, GFP_NOFS );
  if ( NULL == Label )
    return -ENOMEM;

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  ufsdapi_query_volume_info( sbi->ufsd, NULL, Label, PAGE_SIZE, NULL );
  Label[PAGE_SIZE-1] = 0;

  unlock_ufsd( sbi );

  DebugTrace( 0, Dbg, ("read_label: %s\n", Label ) );

  seq_printf( m, "%s\n", Label );

  kfree( Label );
  return 0;
}

static int ufsd_proc_dev_label_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_label_show, PDE_DATA(inode) );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_label_write
//
// /proc/fs/ufsd/<dev>/label
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_label_write(
    IN struct file  *file,
    IN const char __user *buffer,
    IN size_t       count,
    IN OUT loff_t   *ppos
    )
{
  struct super_block *sb = PDE_DATA( file_inode( file ) );
  usuper *sbi = UFSD_SB( sb );
  ssize_t ret = count < PAGE_SIZE? count : PAGE_SIZE;
  char *Label = kmalloc( ret, GFP_NOFS );
  if ( NULL == Label )
    return -ENOMEM;

  if ( copy_from_user( Label, buffer, ret ) ) {
    ret = -EFAULT;
  } else {
    // Remove last '\n'
    while( ret > 0 && '\n' == Label[ret-1] )
      ret -= 1;
    // Set last zero
    Label[ret] = 0;

    DebugTrace( 0, Dbg, ("write_label: %s\n", Label ) );

    //
    // Call UFSD library
    //
    lock_ufsd( sbi );

    ret = ufsdapi_set_volume_info( sbi->ufsd, Label, ret );

    unlock_ufsd( sbi );

    if ( 0 == ret ){
      ret   = count; // Ok
      *ppos += count;
    } else {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("write_label failed: %x\n", (unsigned)ret ) );
      ret = -EINVAL;
    }
  }
  kfree( Label );
  return ret;
}

static const struct file_operations ufsd_proc_dev_label_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_label_open,
  .write    = ufsd_proc_dev_label_write,
};


#if defined UFSD_NTFS2 || defined UFSD_HFS
///////////////////////////////////////////////////////////
// ufsd_proc_dev_tune_show
//
// /proc/fs/ufsd/<dev>/tune
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_tune_show(
    IN struct seq_file  *m,
    IN void             *o
    )
{
  usuper *sbi = UFSD_SB( (struct super_block*)(m->private) );
  ufsd_volume_tune vt;

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  if ( 0 != ufsdapi_query_volume_tune( sbi->ufsd, &vt ) )
    vt.DirAge = vt.JnlRam = 0;

  unlock_ufsd( sbi );

  seq_printf( m, "Ra=%u DirAge=%u JnlRam=%u\n", sbi->ra_blocks, vt.DirAge, vt.JnlRam );
  return 0;
}

static int ufsd_proc_dev_tune_open(struct inode *inode, struct file *file)
{
  return single_open( file, ufsd_proc_dev_tune_show, PDE_DATA( inode ) );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_tune_write
//
// /proc/fs/ufsd/<dev>/tune
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_tune_write(
    IN struct file  *file,
    IN const char __user *buffer,
    IN size_t       count,
    IN OUT loff_t   *ppos
    )
{
  struct super_block *sb = PDE_DATA(file_inode(file));
  usuper *sbi = UFSD_SB( sb );
  ssize_t ret = count < PAGE_SIZE? count : PAGE_SIZE;
  char *Tune = kmalloc( ret, GFP_NOFS );
  if ( NULL == Tune )
    return -ENOMEM;

  //
  // Copy buffer into kernel memory
  //
  if ( 0 != copy_from_user( Tune, buffer, ret ) ) {
    ret = -EINVAL;
  } else {
    unsigned int NewReadAhead;
    ufsd_volume_tune vt;
    int Parsed = sscanf( Tune, "Ra=%u DirAge=%u JnlRam=%u", &NewReadAhead, &vt.DirAge, &vt.JnlRam );
    if ( Parsed < 1 ) {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("failed to parse tune buffer \"%s\"\n", Tune) );
      ret = -EINVAL;
    } else {
      sbi->ra_blocks = NewReadAhead;

      if ( Parsed >= 3 ) {
        //
        // Call UFSD library
        //
        lock_ufsd( sbi );

        ret = ufsdapi_set_volume_tune( sbi->ufsd, &vt );

        unlock_ufsd( sbi );
      } else {
        ret = 0;
      }
    }

    if ( 0 == ret ){
      ret = count; // Ok
      *ppos += count;
    } else {
      DebugTrace( 0, UFSD_LEVEL_ERROR, ("write_tune failed: %x\n", (unsigned)ret ) );
      ret = -EINVAL;
    }
  }

  kfree( Tune );
  return ret;
}

static const struct file_operations ufsd_proc_dev_tune_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_tune_open,
  .write    = ufsd_proc_dev_tune_write,
};
#endif // #if defined UFSD_NTFS2 || defined UFSD_HFS


#ifdef UFSD_TRACE

///////////////////////////////////////////////////////////
// ufsd_proc_dev_trace_show
//
// /proc/fs/ufsd/trace
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_trace_show(
    IN struct seq_file  *m,
    IN void             *o
    )
{
  const char *hint;
  switch( ufsd_trace_level ) {
  case UFSD_LEVEL_STR_ALL:  hint = "all"; break;
  case UFSD_LEVEL_STR_VFS:  hint = "vfs"; break;
  case UFSD_LEVEL_STR_LIB:  hint = "lib"; break;
  case UFSD_LEVEL_STR_MID:  hint = "mid"; break;
  default:
    seq_printf( m, "%lx\n", ufsd_trace_level );
    return 0;
  }
  seq_printf( m, "%s\n", hint );
  return 0;
}

static int ufsd_proc_dev_trace_open(struct inode *inode, struct file *file)
{
  return single_open( file, ufsd_proc_dev_trace_show, NULL );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_trace_write
//
// /proc/fs/ufsd/trace
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_trace_write(
    IN struct file  *file,
    IN const char __user *buffer,
    IN size_t       count,
    IN OUT loff_t   *ppos
    )
{
  //
  // Copy buffer into kernel memory
  //
  char kbuffer[16];
  size_t len = count;
  if ( len > sizeof(kbuffer)-1 )
    len = sizeof(kbuffer)-1;

  if ( 0 != copy_from_user( kbuffer, buffer, len ) )
    return -EINVAL;

  // Remove last '\n'
  while( len > 0 && '\n' == kbuffer[len-1] )
    len -= 1;

  // Set last zero
  kbuffer[len] = 0;

  parse_trace_level( kbuffer );
  *ppos += count;
  return count;
}


static const struct file_operations ufsd_proc_dev_trace_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_trace_open,
  .write    = ufsd_proc_dev_trace_write,
};


///////////////////////////////////////////////////////////
// ufsd_proc_dev_log_show
//
// /proc/fs/ufsd/trace
///////////////////////////////////////////////////////////
static int
ufsd_proc_dev_log_show(
    IN struct seq_file  *m,
    IN void             *o
    )
{
  seq_printf( m, "%s\n", ufsd_trace_file );
  return 0;
}

static int ufsd_proc_dev_log_open( struct inode *inode, struct file *file )
{
  return single_open( file, ufsd_proc_dev_log_show, NULL );
}


///////////////////////////////////////////////////////////
// ufsd_proc_dev_log_write
//
// /proc/fs/ufsd/trace
///////////////////////////////////////////////////////////
static ssize_t
ufsd_proc_dev_log_write(
    IN struct file  *file,
    IN const char __user *buffer,
    IN size_t       count,
    IN OUT loff_t   *ppos
    )
{
  //
  // Copy buffer into kernel memory
  //
  char kbuffer[sizeof(ufsd_trace_file)];
  size_t len = count;
  if ( len > sizeof(kbuffer)-1 )
    len = sizeof(kbuffer)-1;

  if ( 0 != copy_from_user( kbuffer, buffer, len ) )
    return -EINVAL;

  // Remove last '\n'
  while( len > 0 && '\n' == kbuffer[len-1] )
    len -= 1;

  // Set last zero
  kbuffer[len] = 0;

  if ( 0 != strcmp( ufsd_trace_file, kbuffer ) ) {
    memcpy( ufsd_trace_file, kbuffer, len + 1 );
    ufsd_close_trace();
  }

  *ppos += count;
  return count;
}

static const struct file_operations ufsd_proc_dev_log_fops = {
  .owner    = THIS_MODULE,
  .read     = seq_read,
  .llseek   = seq_lseek,
  .release  = single_release,
  .open     = ufsd_proc_dev_log_open,
  .write    = ufsd_proc_dev_log_write,
};

#endif // #ifdef UFSD_TRACE

typedef struct {
  const char   name[8];
  const struct file_operations *fops;
  unsigned int mode;
} ufsd_proc_entries;

static const ufsd_proc_entries ProcInfoEntries[] = {
  { "dirty",    &ufsd_proc_dev_dirty_fops   , S_IFREG | S_IRUGO },
  { "label",    &ufsd_proc_dev_label_fops   , S_IFREG | S_IRUGO | S_IWUGO },
#if defined UFSD_NTFS2 || defined UFSD_HFS
  { "tune",     &ufsd_proc_dev_tune_fops    , S_IFREG | S_IRUGO | S_IWUGO },
#endif
  { "volinfo",  &ufsd_proc_dev_volinfo_fops , S_IFREG | S_IRUGO },
};

static const ufsd_proc_entries ProcRootEntries[] = {
  { "version",  &ufsd_proc_dev_version_fops , S_IFREG | S_IRUGO },
#ifdef UFSD_TRACE
  { "trace",    &ufsd_proc_dev_trace_fops   , S_IFREG | S_IRUGO | S_IWUGO },
  { "log",      &ufsd_proc_dev_log_fops     , S_IFREG | S_IRUGO | S_IWUGO },
#endif
};


///////////////////////////////////////////////////////////
// _proc_mkdir
//
// Helper function to reduce chaos
///////////////////////////////////////////////////////////
static struct proc_dir_entry*
_proc_mkdir( const char *name, struct proc_dir_entry *parent )
{
  struct proc_dir_entry *e = proc_mkdir( name, parent );
#if defined HAVE_STRUCT_PROC_DIR_ENTRY_OWNER && HAVE_STRUCT_PROC_DIR_ENTRY_OWNER
  if ( NULL != e )
    e->owner = THIS_MODULE;
#endif
  return e;
}


///////////////////////////////////////////////////////////
// create_proc_entries
//
//
///////////////////////////////////////////////////////////
static const char*
create_proc_entries(
    IN const ufsd_proc_entries  *e,
    IN unsigned int             count,
    IN struct proc_dir_entry    *parent,
    IN void                     *data
    )
{
  for ( ; 0 != count--; e++ ) {
#if defined HAVE_DECL_PROC_CREATE_DATA && HAVE_DECL_PROC_CREATE_DATA
    if ( NULL == proc_create_data( e->name, e->mode, parent, e->fops, data ) )
      return e->name;
#else
    // 2.6.22 -
    struct proc_dir_entry *de = create_proc_entry( e->name, e->mode, parent );
    if ( NULL == de )
      return e->name;
    de->data = data;
    // to suppress warnings on old platforms (2.6.15?)
    de->proc_fops = (struct file_operations*)e->fops;
#endif
  }
  return NULL;
}


///////////////////////////////////////////////////////////
// remove_proc_entries
//
//
///////////////////////////////////////////////////////////
static void
remove_proc_entries(
    IN const ufsd_proc_entries  *e,
    IN unsigned int             count,
    IN struct proc_dir_entry    *parent
    )
{
  for ( ; 0 != count--; e++ )
    remove_proc_entry( e->name, parent );
}


///////////////////////////////////////////////////////////
// ufsd_proc_info_create
//
// creates /proc/fs/ufsd/<dev>
// Called from 'ufsd_read_super'
///////////////////////////////////////////////////////////
static void
ufsd_proc_info_create(
    IN struct super_block *sb
    )
{
  if ( NULL != proc_info_root ) {
    const char *dev   = ufsd_bd_name( sb );
    struct proc_dir_entry *e = _proc_mkdir( dev, proc_info_root );
    const char *hint  = NULL == e? "" : create_proc_entries( ProcInfoEntries, ARRAY_SIZE( ProcInfoEntries ), e, sb );
    if ( NULL != hint )
      printk( KERN_NOTICE QUOTED_UFSD_DEVICE": cannot create /proc/"PROC_FS_UFSD_NAME"/%s/%s", dev, hint );
    UFSD_SB( sb )->procdir = e;
  }
}


///////////////////////////////////////////////////////////
// ufsd_proc_info_delete
//
// deletes /proc/fs/ufsd/<dev>
// Called from 'ufsd_put_super'
///////////////////////////////////////////////////////////
static void
ufsd_proc_info_delete(
    IN struct super_block *sb
    )
{
  usuper *sbi = UFSD_SB( sb );

  if ( NULL != sbi->procdir )
    remove_proc_entries( ProcInfoEntries, ARRAY_SIZE( ProcInfoEntries ), sbi->procdir );

  if ( NULL != proc_info_root )
    remove_proc_entry( ufsd_bd_name( sb ), proc_info_root );
  sbi->procdir = NULL;
}


///////////////////////////////////////////////////////////
// ufsd_proc_create
//
// creates "/proc/fs/ufsd"
// Called from 'ufsd_init'
///////////////////////////////////////////////////////////
static void
ufsd_proc_create( void )
{
  struct proc_dir_entry *e = _proc_mkdir( PROC_FS_UFSD_NAME, NULL );
  const char *hint = NULL == e? "" : create_proc_entries( ProcRootEntries, ARRAY_SIZE( ProcRootEntries), e, NULL );
  if ( NULL != hint )
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": cannot create /proc/"PROC_FS_UFSD_NAME"/%s\n", hint );
  proc_info_root = e;
}


///////////////////////////////////////////////////////////
// ufsd_proc_delete
//
// deletes "/proc/fs/ufsd"
// Called from 'ufsd_exit'
///////////////////////////////////////////////////////////
static void
ufsd_proc_delete( void )
{
  if ( NULL != proc_info_root ) {
    remove_proc_entries( ProcRootEntries, ARRAY_SIZE( ProcRootEntries), proc_info_root );
    proc_info_root = NULL;
    remove_proc_entry( PROC_FS_UFSD_NAME, NULL );
  }
}

#else

  #define ufsd_proc_info_create( s )
  #define ufsd_proc_info_delete( s )
  #define ufsd_proc_create()
  #define ufsd_proc_delete()

#endif // #if defined CONFIG_PROC_FS


///////////////////////////////////////////////////////////
// ufsd_put_super
//
// super_operations::put_super
// Drop the volume handle.
///////////////////////////////////////////////////////////
static void
ufsd_put_super(
    IN struct super_block *sb
    )
{
  usuper *sbi = UFSD_SB( sb );
  DebugTrace( +1, Dbg, ("put_super: %p (%s)\n", sb, ufsd_bd_name(sb)));

  //
  // Perform any delayed tasks
  //
  do_delayed_tasks( sbi );

#ifdef UFSD_USE_FLUSH_THREAD
  //
  // Stop flush thread
  //
  write_lock( &sbi->state_lock );
  sbi->exit_flush_timer = 1;

  while ( NULL != sbi->flush_task ) {
    wake_up( &sbi->wait_exit_flush );
    write_unlock( &sbi->state_lock );
    wait_event( sbi->wait_done_flush, NULL == sbi->flush_task );
    write_lock( &sbi->state_lock );
  }
  write_unlock( &sbi->state_lock );
#endif

  // Remove /proc/fs/ufsd/..
  ufsd_proc_info_delete( sb );

  ufsdapi_volume_umount( sbi->ufsd );

  ufsd_uload_nls( &sbi->options );

#ifdef UFSD_NTFS2
  if ( NULL != sbi->rw_buffer )
    vfree( sbi->rw_buffer );
#endif

#ifndef CONFIG_DEBUG_MUTEXES // G.P.L.
  mutex_destroy( &sbi->api_mutex );
  mutex_destroy( &sbi->nocase_mutex );
#endif

#if !defined UFSD_TRACE_SILENT && defined UFSD_DEBUG
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("Delayed clear %Zu\n", sbi->nDelClear ));
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("Read %Zu, Written %Zu\n", sbi->nReadBlocks, sbi->nWrittenBlocks ));
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("ReadNa %Zu, WrittenNa %Zu\n", sbi->nReadBlocksNa, sbi->nWrittenBlocksNa ));
  assert( sbi->nPinBlocks == sbi->nUnpinBlocks );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("Pinned %Zu, Unpinned %Zu\n", sbi->nPinBlocks, sbi->nUnpinBlocks ));
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("Mapped: %Zu + %Zu - %Zu\n", sbi->nMappedBh, sbi->nMappedMem, sbi->nUnMapped ));
  assert( sbi->nMappedBh + sbi->nMappedMem == sbi->nUnMapped );
  if ( 0 != sbi->nCompareCalls )
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("ufsd_compare %Zu\n", (ssize_t)sbi->nCompareCalls ));
  if ( 0 != sbi->nHashCalls )
    DebugTrace( 0, UFSD_LEVEL_ERROR, ("ufsd_name_hash %Zu\n", (ssize_t)sbi->nHashCalls ));

  DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdread        : %Zu, %u msec\n", sbi->bdread_cnt, jiffies_to_msecs( sbi->bdread_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdwrite       : %Zu, %u msec\n", sbi->bdwrite_cnt, jiffies_to_msecs( sbi->bdwrite_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdmap         : %Zu, %u msec\n", sbi->bdmap_cnt, jiffies_to_msecs( sbi->bdmap_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("bdsetdirty    : %Zu, %u msec\n", sbi->bdsetdirty_cnt, jiffies_to_msecs( sbi->bdsetdirty_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("readpage     : %Zu, %u msec\n", sbi->readpage_cnt, jiffies_to_msecs( sbi->readpage_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("readpages    : %Zu, %u msec\n", sbi->readpages_cnt, jiffies_to_msecs( sbi->readpages_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("do_readpage  : %Zu, %u msec\n", sbi->do_readpage_cnt, jiffies_to_msecs( sbi->do_readpage_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("buf_readpage : %Zu, %u msec\n", sbi->buf_readpage_cnt, jiffies_to_msecs( sbi->buf_readpage_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("write_begin   : %Zu, %u msec\n", sbi->write_begin_cnt, jiffies_to_msecs( sbi->write_begin_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("write_end     : %Zu, %u msec\n", sbi->write_end_cnt, jiffies_to_msecs( sbi->write_end_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("writepage     : %Zu, %u msec\n", sbi->writepage_cnt, jiffies_to_msecs( sbi->writepage_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("writepages    : %Zu, %u msec\n", sbi->writepages_cnt, jiffies_to_msecs( sbi->writepages_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("do_writepage  : %Zu, %u msec\n", sbi->do_writepage_cnt, jiffies_to_msecs( sbi->do_writepage_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("buf_writepage : %Zu, %u msec\n", sbi->buf_writepage_cnt, jiffies_to_msecs( sbi->buf_writepage_ticks ) ) );
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("write_inode   : %Zu, %u msec\n", sbi->write_inode_cnt, jiffies_to_msecs( sbi->write_inode_ticks ) ) );
#endif //#if !defined UFSD_TRACE_SILENT && defined UFSD_DEBUG

#ifdef UFSD_USE_XATTR
  if ( NULL != sbi->x_buffer )
    kfree( sbi->x_buffer );
#endif

  ufsd_heap_free( sbi );
  sb->s_fs_info = NULL;
  assert( NULL == UFSD_SB( sb ) );

  sync_blockdev( sb->s_bdev );

  DebugTrace( -1, Dbg, ("put_super ->\n"));
}


///////////////////////////////////////////////////////////
// ufsd_write_inode
//
// super_operations::write_inode
///////////////////////////////////////////////////////////
static int
ufsd_write_inode(
    IN struct inode *i,
#if defined HAVE_DECL_SO_WRITE_INODE_V2 && HAVE_DECL_SO_WRITE_INODE_V2
    IN int sync
#elif defined HAVE_DECL_SO_WRITE_INODE_V3 && HAVE_DECL_SO_WRITE_INODE_V3
    IN struct writeback_control *wbc
#else
#error "Unknown type ufsd_write_inode"
#endif
    )
{
  unode *u    = UFSD_U( i );
  usuper *sbi = UFSD_SB( i->i_sb );
  int flushed = 0;

#if defined HAVE_DECL_SO_WRITE_INODE_V2 && HAVE_DECL_SO_WRITE_INODE_V2
  DebugTrace( +1, Dbg, ("write_inode: r=%lx, h=%p, s=%d\n", i->i_ino, u->ufile, sync));
#elif defined HAVE_DECL_SO_WRITE_INODE_V3 && HAVE_DECL_SO_WRITE_INODE_V3
  DebugTrace( +1, Dbg, ("write_inode: r=%lx, h=%p, s=%d\n", i->i_ino, u->ufile, (int)wbc->sync_mode));
#endif

  ProfileEnter( sbi, write_inode );

  if ( NULL == u->ufile ){
    DebugTrace( 0, Dbg, ("write_inode: no ufsd handle for this inode\n"));
  } else {

    if ( !try_lock_ufsd( sbi ) ){

      //
      // Do not flush while write_begin/write_end in progress
      //
      if ( mutex_trylock( &i->i_mutex ) )
      {
        UINT64 isize = i_size_read(i);
        ufsd_file *file;
        unsigned long flags;
        spin_lock( &sbi->ddt_lock );
        file = u->ufile;
        spin_unlock( &sbi->ddt_lock );

        if ( is_sparsed( u ) ) {
          write_lock_irqsave( &u->valid_lock, flags );
          u->valid  = isize;
          write_unlock_irqrestore( &u->valid_lock, flags );
        }

        if ( NULL != file ) {
          UINT64 valid;
          read_lock_irqsave( &u->valid_lock, flags );
          valid = u->valid;
          read_unlock_irqrestore( &u->valid_lock, flags );

          ufsdapi_file_flush( sbi->ufsd, file, isize, valid,
                             TIMESPEC_SECONDS( &i->i_atime ) == u->atime? NULL : &i->i_atime,
                             TIMESPEC_SECONDS( &i->i_mtime ) == u->mtime? NULL : &i->i_mtime,
                             TIMESPEC_SECONDS( &i->i_ctime ) == u->ctime? NULL : &i->i_ctime,
                             __kgid_val( i->i_gid ), __kuid_val( i->i_uid ),
                             test_and_clear_bit( UFSD_UNODE_FLAG_SET_MODE_BIT, &u->flags )? &i->i_mode : NULL );

          u->atime = TIMESPEC_SECONDS( &i->i_atime );
          u->mtime = TIMESPEC_SECONDS( &i->i_mtime );
          u->ctime = TIMESPEC_SECONDS( &i->i_ctime );
        }
        flushed = 1;
        mutex_unlock( &i->i_mutex );
      }
      unlock_ufsd( sbi );
    }

    if ( !flushed )
      mark_inode_dirty_sync( i );
  }

  ProfileLeave( sbi, write_inode );

  DebugTrace( -1, Dbg, ("write_inode ->%s\n", flushed? "":" (d)"));
  return 0;
}


///////////////////////////////////////////////////////////
// ufsd_sync_volume
//
// super_operations::sync_fs
///////////////////////////////////////////////////////////
static int
ufsd_sync_volume(
    IN struct super_block *sb,
    IN int wait
    )
{
  usuper *sbi = UFSD_SB( sb );
  DebugTrace( +1, Dbg, ("sync_volume: %p (%s)%s\n", sb, ufsd_bd_name(sb), wait? ",w":""));

#ifndef UFSD_USE_FLUSH_THREAD
  sb->s_dirt = 0;
#else
  sbi->bdirty = 0;
#endif

#ifdef UFSD_SMART_TRACE
  printk( "<4>ufsd: sync_volume:+\n" );
#endif

  if ( !try_lock_ufsd( sbi ) ){

    ufsdapi_volume_flush( sbi->ufsd, wait );
    unlock_ufsd( sbi );

  } else {

    //
    // Do volume flush later
    //
    atomic_set( &sbi->VFlush, wait? 2 : 1 );
  }

#ifdef UFSD_SMART_TRACE
  printk( "<4>ufsd: sync_volume:-\n" );
#endif

  DebugTrace( -1, Dbg, ("sync_volume ->\n"));
  return 0;
}

#ifdef UFSD_USE_FLUSH_THREAD

#include <linux/freezer.h>
#include <linux/kthread.h>

///////////////////////////////////////////////////////////
// ufsd_add_timer
//
// Helper function to add timer ufsd_SMART_DIRTY_SEC after last dirty
///////////////////////////////////////////////////////////
static inline void
ufsd_add_timer(
    IN usuper *sbi
    )
{
#if UFSD_SMART_DIRTY_SEC
  mod_timer( &sbi->flush_timer, HZ + sbi->last_dirty + msecs_to_jiffies( UFSD_SMART_DIRTY_SEC * 1000 ) );
#else
  mod_timer( &sbi->flush_timer, HZ + sbi->last_dirty + msecs_to_jiffies( 5000 ) );
#endif
}


///////////////////////////////////////////////////////////
// flush_timer_fn
//
// Timer function
///////////////////////////////////////////////////////////
static void
flush_timer_fn(
    IN unsigned long data
    )
{
  usuper *sbi = (usuper*)data;

  if ( !sbi->bdirty ) {
    // Do not wake up flush thread
  } else {
#if UFSD_SMART_DIRTY_SEC
    long dj = jiffies - sbi->last_dirty;
    if ( dj <= 0 || jiffies_to_msecs( dj ) < UFSD_SMART_DIRTY_SEC * 1000 ) {
      // Do not wake up flush thread
      // Sleep for another period
      ufsd_add_timer( sbi );
    } else
#endif
    if ( NULL != sbi->flush_task ) {
      //
      // Volume is dirty and there are no writes last UFSD_SMART_DIRTY_SEC
      // Wake up flush thread
      //
      wake_up_process( sbi->flush_task );
    }
  }
}


///////////////////////////////////////////////////////////
// ufsd_flush_thread
//
// 'dirty_writeback_interval'
///////////////////////////////////////////////////////////
static int
ufsd_flush_thread(
    IN void *arg
    )
{
  struct super_block *sb = arg;
  usuper *sbi = UFSD_SB( sb );
#ifdef UFSD_TRACE
  unsigned long j0, j1, j_a = 0, j_s = 0, cnt = 0;
#endif

  // Record that the flush thread is running
  sbi->flush_task = current;

  //
  // Set up an interval timer which can be used to trigger a flush wakeup after the flush interval expires
  //
  setup_timer( &sbi->flush_timer, flush_timer_fn, (unsigned long)sbi );

  wake_up( &sbi->wait_done_flush );

#if defined HAVE_DECL_SET_FREEZABLE && HAVE_DECL_SET_FREEZABLE
  set_freezable();
#endif

  //
  // And now, wait forever for flush wakeup events
  //
  write_lock( &sbi->state_lock );

  TRACE_ONLY( j0 = jiffies; )

  for ( ;; ) {
    if ( sbi->exit_flush_timer ) {
      write_unlock( &sbi->state_lock );
      del_timer_sync( &sbi->flush_timer );
      sbi->flush_task = NULL;
      wake_up( &sbi->wait_done_flush );
      DebugTrace( 0, Dbg, ("flush_thread exiting: active %u, sleep %u, cycles %lu\n", jiffies_to_msecs( j_a ), jiffies_to_msecs( j_s ), cnt ));
      return 0;
    }

    if ( sbi->bdirty ) {
#if UFSD_SMART_DIRTY_SEC
      long dj = jiffies - sbi->last_dirty;
#endif
      TRACE_ONLY( const char *hint;  )

      DebugTrace( +1, Dbg, ("flush_thread: %p (%s)\n", sb, ufsd_bd_name(sb)));

#if UFSD_SMART_DIRTY_SEC
      if ( dj <= 0 || (jiffies_to_msecs( dj )) < UFSD_SMART_DIRTY_SEC * 1000 ) {
        TRACE_ONLY( hint = "skip"; )
        ufsd_add_timer( sbi );
      } else
#endif
      {
        sbi->bdirty = 0;
#ifdef UFSD_SMART_TRACE
        printk( KERN_WARNING QUOTED_UFSD_DEVICE": flush_thread:+\n" );
#endif
        write_unlock( &sbi->state_lock );
        if ( !try_lock_ufsd( sbi ) ){
          ufsdapi_volume_flush( sbi->ufsd, 1 );
          unlock_ufsd( sbi );
          TRACE_ONLY( hint = "flushed"; )
        } else {
          //
          // Do volume flush later
          //
          atomic_set( &sbi->VFlush, 1 );
          TRACE_ONLY( hint = "delay"; )
        }
        write_lock( &sbi->state_lock );
#ifdef UFSD_SMART_TRACE
        printk( KERN_WARNING QUOTED_UFSD_DEVICE": flush_thread:-\n" );
#endif
      }

      DebugTrace( -1, Dbg, ("flush_thread -> %s\n", hint));
    }

    wake_up( &sbi->wait_done_flush );

    TRACE_ONLY( cnt += 1; )
    TRACE_ONLY( j1 = jiffies; )
    TRACE_ONLY( j_a += j1 - j0; )
    TRACE_ONLY( j0 = j1; )

    if ( freezing( current ) ) {
      DebugTrace( 0, Dbg, ("now suspending flush_thread\n" ));
      write_unlock( &sbi->state_lock );
#if defined HAVE_DECL_REFRIGERATOR && HAVE_DECL_REFRIGERATOR
      refrigerator();
#else
      try_to_freeze();
#endif
      write_lock( &sbi->state_lock );

    } else if ( !sbi->exit_flush_timer ) {

      DEFINE_WAIT( wait );
      prepare_to_wait( &sbi->wait_exit_flush, &wait, TASK_INTERRUPTIBLE );
      write_unlock( &sbi->state_lock );

      schedule();

      TRACE_ONLY( j1 = jiffies; )
      TRACE_ONLY( j_s += j1 - j0; )
      TRACE_ONLY( j0 = j1; )

      write_lock( &sbi->state_lock );
      finish_wait( &sbi->wait_exit_flush, &wait );
    }
  }
}

#else

///////////////////////////////////////////////////////////
// ufsd_write_super
//
// super_operations::write_super
///////////////////////////////////////////////////////////
static void
ufsd_write_super(
    IN struct super_block *sb
    )
{
  usuper *sbi = UFSD_SB( sb );
#if UFSD_SMART_DIRTY_SEC
  long dj = jiffies - sbi->last_dirty;
#endif
  TRACE_ONLY( const char *hint;  )
  DebugTrace( +1, Dbg, ("write_super: %p (%s)\n", sb, ufsd_bd_name(sb)));

#if UFSD_SMART_DIRTY_SEC
  if ( dj <= 0  || (jiffies_to_msecs( dj ) < UFSD_SMART_DIRTY_SEC * 1000 ) ) {
    TRACE_ONLY( hint = "skip"; )
  } else
#endif
  {
    // Clear 's_dirt' to avoid next calls
    sb->s_dirt  = 0;
#ifdef UFSD_SMART_TRACE
    printk( "<4>ufsd: write_super:+\n" );
#endif
    if ( !try_lock_ufsd( sbi ) ){

      ufsdapi_volume_flush( sbi->ufsd, 0 );
      unlock_ufsd( sbi );
      TRACE_ONLY( hint = "flushed"; )

    } else {

      //
      // Do volume flush later
      //
      atomic_set( &sbi->VFlush, 1 );
      TRACE_ONLY( hint = "delay"; )
    }
#ifdef UFSD_SMART_TRACE
    printk( "<4>ufsd: write_super:-\n" );
#endif
  }

  DebugTrace( -1, Dbg, ("write_super -> %s\n", hint));
}

#endif // #ifdef UFSD_USE_FLUSH_THREAD


///////////////////////////////////////////////////////////
// ufsd_on_set_dirty
//
// Callback function. Called when volume becomes dirty
///////////////////////////////////////////////////////////
void
UFSDAPI_CALL
ufsd_on_set_dirty(
    IN struct super_block *sb
    )
{
  usuper *sbi = UFSD_SB( sb );

  assert( !(sb->s_flags & MS_RDONLY) );

#ifdef UFSD_USE_FLUSH_THREAD
  write_lock( &sbi->state_lock );
  sbi->last_dirty = jiffies;
  if ( !sbi->bdirty ) {
#ifdef UFSD_SMART_TRACE
    printk( KERN_NOTICE"ufsd: ufsd_on_set_dirty()\n" );
#endif
    DebugTrace( 0, Dbg, ("ufsd_on_set_dirty()\n" ));
    sbi->bdirty = 1;
  }
  if ( NULL != sbi->flush_timer.function ) // check case when this function is called while mounting
    ufsd_add_timer( sbi );
  write_unlock( &sbi->state_lock );
#else
#ifdef UFSD_SMART_TRACE
  if ( !sb->s_dirt )
    printk( KERN_NOTICE "ufsd: ufsd_on_set_dirty()\n" );
#endif
  sb->s_dirt      = 1;
  sbi->last_dirty = jiffies;
#endif
}


///////////////////////////////////////////////////////////
// ufsd_statfs
//
// super_operations::statfs
///////////////////////////////////////////////////////////
static int
ufsd_statfs(
#if defined HAVE_DECL_SO_STATFS_V1 && HAVE_DECL_SO_STATFS_V1
    IN  struct super_block  *sb,
#elif defined HAVE_DECL_SO_STATFS_V2 && HAVE_DECL_SO_STATFS_V2
    IN struct dentry        *de,
#else
  #error "Unknown statfs"
#endif
    OUT struct kstatfs      *buf
    )
{
#if defined HAVE_DECL_SO_STATFS_V2 && HAVE_DECL_SO_STATFS_V2
  struct super_block *sb = de->d_sb;
#endif
  usuper *sbi = UFSD_SB( sb );
  ufsd_volume_info info;
  UINT64 free_clusters;
  DebugTrace( +1, Dbg, ("statfs: %p (%s), %p\n", sb, ufsd_bd_name(sb), buf));
  lock_ufsd( sbi );

  ufsdapi_query_volume_info( sbi->ufsd, &info, NULL, 0, &free_clusters );

  unlock_ufsd( sbi );

  buf->f_type   = info.fs_signature;
  buf->f_bsize  = info.bytes_per_cluster;
  buf->f_blocks = info.total_clusters;
  buf->f_bfree  = free_clusters;
  buf->f_bavail = buf->f_bfree;
  buf->f_files  = 0;
  buf->f_ffree  = 0;
  buf->f_namelen= info.namelen;

  DebugTrace( -1, Dbg, ("statfs -> free=%llx\n", free_clusters));
  //TRACE_ONLY(show_buffers();)
#if defined UFSD_DEBUG_ALLOC & !defined UFSD_TRACE_SILENT
  trace_mem_report( 0 );
#endif
  return 0;
}

// Forward declaration
static int
ufsd_parse_options(
    char **options,
    mount_options *opts
    );


///////////////////////////////////////////////////////////
// ufsd_remount
//
// super_operations::remount_fs
///////////////////////////////////////////////////////////
static int
ufsd_remount(
    IN struct super_block *sb,
    IN int                *flags,
    IN char               *data
    )
{
  mount_options opts_saved;
  char *options = data;
  int err = -EINVAL;
  int NeedParse = NULL != data && 0 != data[0];
  int Ro = *flags & MS_RDONLY;
  ufsd_volume_info info;
  usuper *sbi = UFSD_SB( sb );
  C_ASSERT( sizeof(sbi->options) == sizeof(opts_saved) );
  const char *dev_name = ufsd_bd_name( sb );

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  DebugTrace( +1, Dbg, ("remount \"%s\", %lx, options \"%s\"\n", dev_name, sb->s_flags, NULL == options? "(null)" : options));

  if ( (sb->s_flags & MS_RDONLY) && !Ro && sbi->options.journal >= JOURNAL_STATUS_NEED_REPLAY ) {
    DebugTrace( 0, Dbg, ("remount \"%s\": ro -> rw + jnl\n", dev_name ));
    printk( KERN_WARNING QUOTED_UFSD_DEVICE ": Couldn't remount \"%s\" rw because journal is not replayed."
            " Please umount/remount instead\n", dev_name );
    NeedParse = 0;
    goto Exit;
  }

  if ( NeedParse ) {

    // Save current options
    memcpy( &opts_saved, &sbi->options, sizeof(opts_saved) );

    // Parse options passed in command 'mount'
    memset( &sbi->options, 0, sizeof(opts_saved) );

    if ( !ufsd_parse_options( &options, &sbi->options ) ) {
      DebugTrace( 0, Dbg, ("remount: failed to remount \"%s\", bad options \"%s\"\n", dev_name, options));
      goto Exit;
    }
  }

  *flags |= MS_NODIRATIME | (sbi->options.noatime? MS_NOATIME : 0);

  if ( !Ro
    && ( 0 != ufsdapi_query_volume_info( sbi->ufsd, &info, NULL, 0, NULL )
      || 0 != info.dirty )
    && !sbi->options.force ) {
    //
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": volume is dirty and \"force\" flag is not set\n" );
    goto Exit;
  }

  err = ufsdapi_volume_remount( sbi->ufsd, &Ro, &sbi->options );
  if ( 0 != err ) {
    DebugTrace( 0, Dbg, ("remount: failed to remount \"%s\", ufsdapi_volume_remount failed %x\n", dev_name, (unsigned)err ));
    err = -EINVAL;
    goto Exit;
  }

  if ( NeedParse ) {
    // unload original nls
    ufsd_uload_nls( &opts_saved );
  }

#if defined HAVE_STRUCT_SUPER_BLOCK_S_D_OP && HAVE_STRUCT_SUPER_BLOCK_S_D_OP
  sb->s_d_op = sbi->options.nocase? &ufsd_dop : NULL;
#endif

#if defined HAVE_STRUCT_SUPER_BLOCK_S_BDI && HAVE_STRUCT_SUPER_BLOCK_S_BDI
  if ( sbi->options.raKb )
    sb->s_bdi->ra_pages = sbi->options.raKb >> ( PAGE_CACHE_SHIFT-10 );
#else
  if ( sbi->options.raKb )
    sb->s_bdev->bd_inode_backing_dev_info->ra_pages = sbi->options.raKb >> ( PAGE_CACHE_SHIFT-10 );
#endif

  if ( Ro )
    sb->s_flags |= MS_RDONLY;
  else
    sb->s_flags &= ~MS_RDONLY;

  //
  // Save 'sync' flag
  //
  if ( FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    sbi->options.sync = 1;

Exit:

  if ( 0 != err && NeedParse ) {
    // unload new nls
    ufsd_uload_nls( &sbi->options );
    // Restore original options
    memcpy( &sbi->options, &opts_saved, sizeof(opts_saved) );
  }

  unlock_ufsd( sbi );

  if ( 0 == err ) {
    DebugTrace( -1, Dbg, ("remount -> ok\n"));
  } else {
    DebugTrace( -1, Dbg, ("remount failed: %d\n", err));
  }

  return err;
}


///////////////////////////////////////////////////////////
// ufsd_evict_inode
//
// super_operations::evict_inode/clear_inode
///////////////////////////////////////////////////////////
static void
ufsd_evict_inode(
    IN struct inode *i
    )
{
  usuper *sbi = UFSD_SB( i->i_sb );
  unode *u    = UFSD_U( i );
  ufsd_file *file;
  TRACE_ONLY( int d = 0; )

  DebugTrace( +1, Dbg, ("evict_inode: r=%lx, h=%p, c=%u\n", i->i_ino, u->ufile, atomic_read( &i->i_count ) ));

  //
  // wait pending io operations to be finished ( 0 == u->ioend_count )
  //

#if defined HAVE_STRUCT_SUPER_OPERATIONS_EVICT_INODE && HAVE_STRUCT_SUPER_OPERATIONS_EVICT_INODE
  if ( i->i_data.nrpages )
    truncate_inode_pages( &i->i_data, 0 );
#if defined HAVE_DECL_END_WRITEBACK && HAVE_DECL_END_WRITEBACK
  end_writeback( i );
#elif defined HAVE_DECL_CLEAR_INODE && HAVE_DECL_CLEAR_INODE
  //In kernel 3.5 end_writeback renamed to clear_inode
  clear_inode( i );
#else
#error "end_writeback or clear_inode not defined"
#endif
#else
  #define evict_inode clear_inode
#endif

  if ( NULL == sbi ){
    DebugTrace( 0, Dbg, ("evict_inode: forgotten inode\n") );
  } else if ( NULL == u->ufile ){
    ;
  } else if ( !try_lock_ufsd( sbi ) ) {

    spin_lock( &sbi->ddt_lock );
    file = u->ufile;
    u->ufile = NULL;
    spin_unlock( &sbi->ddt_lock );

    ufsdapi_file_close( sbi->ufsd, file );

    unlock_ufsd( sbi );

  } else {

    int is_dir = S_ISDIR( i->i_mode );

    spin_lock( &sbi->ddt_lock );
    file = u->ufile;
    u->ufile = NULL;
    spin_unlock( &sbi->ddt_lock );

    if ( NULL != file ) {
      delay_task *task = (delay_task*)kmalloc( sizeof(delay_task), GFP_NOFS );
      if ( NULL == task ){
        //
        // UFSD must correct close unclosed files in the end of the session
        //
      } else {

        //
        // Add this inode to internal list to clear later
        //
        task->file = file;
        spin_lock( &sbi->ddt_lock );
        if ( is_dir )
          list_add_tail( &task->list, &sbi->clear_list );
        else
          list_add( &task->list, &sbi->clear_list );
        spin_unlock( &sbi->ddt_lock );
        TRACE_ONLY( sbi->nDelClear += 1; )
        TRACE_ONLY( d = 1; )
      }
    }
  }

#if defined UFSD_USE_XATTR && !(defined HAVE_STRUCT_INODE_I_ACL && HAVE_STRUCT_INODE_I_ACL)
  if ( ACL_NOT_CACHED != u->acl ) {
    ufsd_posix_acl_release( u->acl );
    u->acl = ACL_NOT_CACHED;
  }
  if ( ACL_NOT_CACHED != u->default_acl ) {
    ufsd_posix_acl_release( u->default_acl );
    u->default_acl  = ACL_NOT_CACHED;
  }
#endif

  DebugTrace( -1, Dbg, ("evict_inode ->%s\n", d? " (d)" : "") );
}


///////////////////////////////////////////////////////////
// ufsd_show_options
//
// super_operations::show_options
///////////////////////////////////////////////////////////
static int
ufsd_show_options(
    IN struct seq_file  *seq,
#if defined HAVE_DECL_SO_SHOW_OPTIONS_V2 && HAVE_DECL_SO_SHOW_OPTIONS_V2
    IN struct dentry    *dnt
#else
    IN struct vfsmount  *vfs
#endif
    )
{
#if defined HAVE_DECL_SO_SHOW_OPTIONS_V2 && HAVE_DECL_SO_SHOW_OPTIONS_V2
  usuper *sbi = UFSD_SB( dnt->d_sb );
#else
  usuper *sbi = UFSD_SB( vfs->mnt_sb );
#endif

  mount_options *opts = &sbi->options;
//  TRACE_ONLY( char *buf = seq->buf + seq->count; )

//  DebugTrace( +1, Dbg, ("show_options: %p\n", sbi));

#ifdef UFSD_USE_NLS
  {
    int cp;
    for ( cp = 0; cp < opts->nls_count; cp++ ) {
      struct nls_table *nls = opts->nls[cp];
      if ( NULL != nls )
        seq_printf( seq, ",nls=%s", nls->charset );
      else
        seq_printf( seq, ",nls=utf8" );
    }
  }
#endif

  if ( opts->uid )
    seq_printf( seq, ",uid=%d", opts->fs_uid );
  if ( opts->gid )
    seq_printf( seq, ",gid=%d", opts->fs_gid );
  if ( opts->fmask )
    seq_printf( seq, ",fmask=%o", (int)(unsigned short)~opts->fs_fmask );
  if ( opts->dmask )
    seq_printf( seq, ",dmask=%o", (int)(unsigned short)~opts->fs_dmask );
  if ( opts->clumpKb )
    seq_printf( seq, ",clump=%u", opts->clumpKb );
  if ( opts->showmeta )
    seq_printf( seq, ",showmeta" );
  if ( opts->sys_immutable )
    seq_printf( seq, ",sys_immutable" );
  if ( opts->nocase )
    seq_printf( seq, ",nocase" );
  if ( opts->noatime )
    seq_printf( seq, ",noatime" );
  if ( opts->bestcompr )
    seq_printf( seq, ",bestcompr" );
  if ( opts->sparse )
    seq_printf( seq, ",sparse" );
  if ( opts->force )
    seq_printf( seq, ",force" );
  if ( opts->nohidden )
    seq_printf( seq, ",nohidden" );
  if ( opts->user_xattr )
    seq_printf( seq, ",user_xattr" );
  if ( opts->acl )
    seq_printf( seq, ",acl" );
  if ( opts->nolazy )
    seq_printf( seq, ",nolazy" );
#ifdef UFSD_DELAY_ALLOC
  if ( opts->delalloc )
    seq_printf( seq, ",delalloc" );
#endif
  if ( opts->nojnl )
    seq_printf( seq, ",nojnl" );
  if ( opts->wb )
    seq_printf( seq, ",wb=1" );
  if ( opts->raKb ) {
    if ( 0 == (opts->raKb&0x3ff) )
      seq_printf( seq, ",ra=%uM", opts->raKb>>10 );
    else
      seq_printf( seq, ",ra=%u", opts->raKb );
  }
  if ( opts->discard )
    seq_printf( seq, ",discard" );

//  DebugTrace( -1, Dbg, ("show_options -> \"%s\"\n", buf));
  return 0;
}


//
// Volume operations
// super_block::s_op
//
static const struct super_operations ufsd_sops = {
  .alloc_inode    = ufsd_alloc_inode,
  .destroy_inode  = ufsd_destroy_inode,
  .put_super      = ufsd_put_super,
  .statfs         = ufsd_statfs,
  .remount_fs     = ufsd_remount,
#ifndef UFSD_USE_FLUSH_THREAD
  .write_super    = ufsd_write_super,
#endif
  .sync_fs        = ufsd_sync_volume,
  .write_inode    = ufsd_write_inode,
  .evict_inode    = ufsd_evict_inode,
  .show_options   = ufsd_show_options,
};


///////////////////////////////////////////////////////////
// ufsd_get_name
//
// dentry - the directory in which to find a name
// name   - a pointer to a %NAME_MAX+1 char buffer to store the name
// child  - the dentry for the child directory.
//
//
// Get the name of child entry by its ino
// export_operations::get_name
///////////////////////////////////////////////////////////
static int
ufsd_get_name(
    IN struct dentry  *de,
    OUT char          *name,
    IN struct dentry  *ch
    )
{
  int err;
  struct inode *i_p   = de->d_inode;
  struct inode *i_ch  = ch->d_inode;
  usuper *sbi = UFSD_SB( i_ch->i_sb );

  DebugTrace( +1, Dbg, ("get_name: r=%lx=%p('%.*s'), r=%lx=%p('%.*s')\n",
              i_p->i_ino, de, (int)de->d_name.len, de->d_name.name,
              i_ch->i_ino, ch, (int)ch->d_name.len, ch->d_name.name ));

  //
  // Reset returned value
  //
  name[0] = 0;

  //
  // Lock UFSD
  //
  lock_ufsd( sbi );

  err = 0 == lazy_open( sbi, i_ch )
     && 0 == ufsdapi_file_get_name( sbi->ufsd, UFSD_FH(i_ch), i_p->i_ino, name, NAME_MAX )
     ? 0
     : -ENOENT;

  unlock_ufsd( sbi );

  DebugTrace( -1, Dbg, ("get_name -> %d (%s)\n", err, name ));
  return err;
}


///////////////////////////////////////////////////////////
// ufsd_get_parent
//
// export_operations::get_parent
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_get_parent(
    IN struct dentry *ch
    )
{
  ufsd_iget4_param param;
  struct inode *i_ch  = ch->d_inode;
  usuper *sbi         = UFSD_SB( i_ch->i_sb );
  struct inode *i     = NULL;
  struct dentry *de;
  int err;

  DebugTrace( +1, Dbg, ("get_parent: r=%lx,%p('%.*s')\n", i_ch->i_ino, ch, (int)ch->d_name.len, ch->d_name.name));

  param.subdir_count = 0;

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  if ( 0 == lazy_open( sbi, i_ch )
    && 0 == ufsdapi_file_get_parent( sbi->ufsd, UFSD_FH(i_ch), &param.fh, &param.fi ) ) {

    assert( NULL != param.fh );

    i = iget4( i_ch->i_sb, param.fi->Id, NULL, &param );

    if ( NULL == i ) {
      err = -ENOMEM;
      DebugTrace( 0, Dbg, ("get_parent: -> No memory for new inode\n" ));
    } else {
      assert( NULL != UFSD_FH(i) );
      // OK
      err = 0;
    }

    if ( NULL != param.fh ){
      // UFSD handle was not used. Close it
      ufsdapi_file_close( sbi->ufsd, param.fh );
      param.fh = NULL;
      if ( 0 == err ){
        DebugTrace( 0, Dbg, ("get_parent: r=%lx already loaded\n", i->i_ino ));
      }
    }
  } else {
    //
    // No parent for given inode
    //
    err = -ENOENT;
  }

  unlock_ufsd( sbi );

  if ( 0 != err ){
    DebugTrace( -1, Dbg, ("get_parent -> error %d\n", err ));
    return ERR_PTR(err);
  }

  DebugTrace( 0, Dbg, ("get_parent -> OK, r=%lx h=%p id=%llx l=%x\n",
                      i->i_ino, UFSD_FH(i), param.fi->Id, i->i_nlink));

  // Finally get a dentry for the parent directory and return it.
  de = d_obtain_alias( i );

  if ( unlikely( IS_ERR( de ) ) ) {
    iput( i );
    DebugTrace( -1, Dbg, ("get_parent: -> No memory for dentry\n" ));
    return ERR_PTR(-ENOMEM);
  }

  DebugTrace( -1, Dbg, ("get_parent -> %p('%.*s'))\n", de, (int)de->d_name.len, de->d_name.name));
  return de;
}



#ifdef UFSD_EXFAT
///////////////////////////////////////////////////////////
// ufsd_encode_fh
//
// stores in the file handle fragment 'fh' (using at most 'max_len' bytes)
// information that can be used by 'decode_fh' to recover the file refered
// to by the 'struct dentry* de'
//
// export_operations::encode_fh
///////////////////////////////////////////////////////////
static int
ufsd_encode_fh(
#if defined HAVE_DECL_ENCODE_FH_V1 && HAVE_DECL_ENCODE_FH_V1
    IN struct dentry  *de,
#elif defined HAVE_DECL_ENCODE_FH_V2 && HAVE_DECL_ENCODE_FH_V2
    IN struct inode   *i,
#else
#error Unknown ufsd_encode_fh
#endif
    IN __u32          *fh,
    IN OUT int        *max_len,
#if defined HAVE_DECL_ENCODE_FH_V1 && HAVE_DECL_ENCODE_FH_V1
    IN int            connectable
#elif defined HAVE_DECL_ENCODE_FH_V2 && HAVE_DECL_ENCODE_FH_V2
    IN struct inode   *connectable
#endif
    )
{
  int type;
#if defined HAVE_DECL_ENCODE_FH_V1 && HAVE_DECL_ENCODE_FH_V1
  struct inode *i = de->d_inode;
#endif
  usuper *sbi     = UFSD_SB( i->i_sb );

#if defined HAVE_DECL_ENCODE_FH_V1 && HAVE_DECL_ENCODE_FH_V1
  DebugTrace( +1, Dbg, ("encode_fh: r=%lx, %p('%.*s'), %x\n",
              i->i_ino, de, (int)de->d_name.len, de->d_name.name, *max_len ));
#else
  DebugTrace( +1, Dbg, ("encode_fh: r=%lx, %x\n", i->i_ino, *max_len ));
#endif

  lock_ufsd( sbi );

  if ( 0 != lazy_open( sbi, i ) )
    type = -ENOENT;
  else {
    type = ufsdapi_encode_fh( sbi->ufsd, UFSD_FH(i), fh, max_len );
    if ( type < 0 )
      type = 255; // no room
  }

  unlock_ufsd( sbi );

  DebugTrace( -1, Dbg, ("encode_fh -> %d,%d\n", type, *max_len) );

  return type;
}


///////////////////////////////////////////////////////////
// ufsd_decode_fh
//
// Helper function for export (inverse function to ufsd_encode_fh)
///////////////////////////////////////////////////////////
static struct inode*
ufsd_decode_fh(
    IN struct super_block *sb,
    IN const void   *fh,
    IN unsigned     fh_len,
    IN const int    *fh_type,
    IN int          parent
    )
{
  int err;
  ufsd_iget4_param param;
  struct inode *i = NULL;
  usuper *sbi     = UFSD_SB( sb );

  DebugTrace( +1, Dbg, ("decode_fh: sb=%p %d,%d,%d\n", sb, NULL == fh_type? 0 : *fh_type, fh_len, parent));

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  if ( 0 != ufsdapi_decode_fh( sbi->ufsd, fh, fh_len, fh_type, parent, &param.fh, &param.fi ) )
    err = -ENOENT;
  else {
    err = 0;

    i = iget4( sb, param.fi->Id, NULL, &param );

    if ( NULL == i ) {
      err = -ENOMEM;
      DebugTrace( 0, Dbg, ("decode_fh: -> No memory for new inode\n" ));
    }

    if ( NULL != param.fh ){
      // UFSD handle was not used. Close it
      ufsdapi_file_close( sbi->ufsd, param.fh );
      if ( 0 == err ){
        DebugTrace( 0, Dbg, ("decode_fh: i=%p,r=%lx already loaded\n", i, i->i_ino ));
      }
    }
  }

  unlock_ufsd( sbi );

  if ( 0 != err ) {
    DebugTrace( -1, Dbg, ("decode_fh -> %d\n", err ));
    return ERR_PTR(err);
  }

  DebugTrace( -1, Dbg, ("decode_fh -> r=%lx h=%p r=%lx l=%x m=%o\n", i->i_ino, UFSD_FH(i), i->i_ino, i->i_nlink, i->i_mode ));

  return i;
}


///////////////////////////////////////////////////////////
// ufsd_encode_fh_to_dentry
//
// encode_export_operations::fh_to_dentry
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_encode_fh_to_dentry(
    IN struct super_block *sb,
    IN struct fid *fid,
    IN int fh_len,
    IN int fh_type
    )
{
  struct dentry *de;
  struct inode *i;

  DebugTrace( +1, Dbg, ("fh_to_dentry: sb=%p,r=%x,gen=%x\n", sb, fid->i32.ino, fid->i32.gen ));

  i   = ufsd_decode_fh( sb, fid, fh_len, &fh_type, 0 );
  de  = IS_ERR(i)? (struct dentry*)i : d_obtain_alias( i );

  DebugTrace( -1, Dbg, ("fh_to_dentry -> %p\n", de ));

  return de;
}


///////////////////////////////////////////////////////////
// ufsd_encode_fh_to_parent
//
// encode_export_operations::fh_to_parent
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_encode_fh_to_parent(
    IN struct super_block *sb,
    IN struct fid *fid,
    IN int fh_len,
    IN int fh_type
    )
{
  struct dentry *de;
  struct inode *i;

  DebugTrace( +1, Dbg, ("fh_to_parent: sb=%p,r=%x,gen=%x\n", sb, fid->i32.parent_ino, fh_len > 3 ? fid->i32.parent_gen : 0 ));

  i  = ufsd_decode_fh( sb, fid, fh_len, &fh_type, 1 );
  de = IS_ERR(i)? (struct dentry*)i : d_obtain_alias( i );

  DebugTrace( -1, Dbg, ("fh_to_parent -> %p\n", de ));

  return de;
}


//
// NFS operations.
// super_block::s_export_op
//
static const struct export_operations ufsd_encode_export_op = {
  .encode_fh    = ufsd_encode_fh,
  .get_name     = ufsd_get_name,
  .get_parent   = ufsd_get_parent,
  .fh_to_dentry = ufsd_encode_fh_to_dentry,
  .fh_to_parent = ufsd_encode_fh_to_parent,
};

#endif // #ifdef UFSD_EXFAT


///////////////////////////////////////////////////////////
// ufsd_nfs_get_inode
//
// Helper function for export
///////////////////////////////////////////////////////////
static struct inode*
ufsd_nfs_get_inode(
    IN struct super_block *sb,
    IN u64 ino,
    IN u32 gen
    )
{
  int err;
  ufsd_iget4_param param;
  struct inode *i = NULL;
  usuper *sbi     = UFSD_SB( sb );

  DebugTrace( +1, Dbg, ("nfs_get_inode: sb=%p,r=%x,gen=%x\n", sb, (unsigned)ino, gen ));

  //
  // Call UFSD library
  //
  lock_ufsd( sbi );

  err = ufsdapi_file_open_by_id( sbi->ufsd, ino, &param.fh, &param.fi );

  if ( 0 == err ) {
    i = iget4( sb, param.fi->Id, NULL, &param );

    if ( NULL == i ) {
      err = -ENOMEM;
      DebugTrace( 0, Dbg, ("nfs_get_inode: -> No memory for new inode\n" ));
    } else if ( gen && i->i_generation != gen ) {
      // we didn't find the right inode...
      DebugTrace( 0, Dbg, ("nfs_get_inode: -> invalid generation\n" ));
      iput( i );
      err = -ESTALE;
    }

    if ( NULL != param.fh ){
      // UFSD handle was not used. Close it
      ufsdapi_file_close( sbi->ufsd, param.fh );
      if ( 0 == err ){
        DebugTrace( 0, Dbg, ("nfs_get_inode: r=%lx already loaded\n", i->i_ino ));
      }
    }
  }

  unlock_ufsd( sbi );

  if ( 0 != err ) {
    DebugTrace( -1, Dbg, ("nfs_get_inode: -> error %d\n", err ));
    return ERR_PTR(err);
  }

  DebugTrace( -1, Dbg, ("nfs_get_inode -> r=%lx\n", i->i_ino ));

  return i;
}


///////////////////////////////////////////////////////////
// ufsd_fh_to_dentry
//
// export_operations::fh_to_dentry
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_fh_to_dentry(
    IN struct super_block *sb,
    IN struct fid *fid,
    IN int fh_len,
    IN int fh_type
    )
{
  struct dentry *de = NULL;

  DebugTrace( +1, Dbg, ("fh_to_dentry: sb=%p,r=%x,gen=%x\n", sb, fid->i32.ino, fid->i32.gen ));

  if ( fh_len >= 2 && ( FILEID_INO32_GEN == fh_type || FILEID_INO32_GEN_PARENT == fh_type ) )
  {
    struct inode *i = ufsd_nfs_get_inode( sb, fid->i32.ino, fid->i32.gen );
    de = IS_ERR(i)? (struct dentry*)i : d_obtain_alias( i );
  }

  DebugTrace( -1, Dbg, ("fh_to_dentry -> %p\n", de ));

  return de;
}


///////////////////////////////////////////////////////////
// ufsd_fh_to_parent
//
// export_operations::fh_to_parent
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_fh_to_parent(
    IN struct super_block *sb,
    IN struct fid *fid,
    IN int fh_len,
    IN int fh_type
    )
{
  struct dentry *de = NULL;

  DebugTrace( +1, Dbg, ("fh_to_parent: sb=%p,r=%x,gen=%x\n", sb, fid->i32.parent_ino, fh_len > 3 ? fid->i32.parent_gen : 0 ));

  if ( fh_len > 2 && FILEID_INO32_GEN_PARENT == fh_type )
  {
    struct inode *i = ufsd_nfs_get_inode( sb, fid->i32.parent_ino, fh_len > 3 ? fid->i32.parent_gen : 0 );
    de = IS_ERR( i )? (struct dentry*)i : d_obtain_alias( i );
  }

  DebugTrace( -1, Dbg, ("fh_to_parent -> %p\n", de ));

  return de;
}


//
// NFS operations.
// super_block::s_export_op
//
static const struct export_operations ufsd_export_op = {
  .get_name     = ufsd_get_name,
  .get_parent   = ufsd_get_parent,
  .fh_to_dentry = ufsd_fh_to_dentry,
  .fh_to_parent = ufsd_fh_to_parent,
};


#ifdef UFSD_USE_NLS
///////////////////////////////////////////////////////////
// ufsd_add_nls
//
//
///////////////////////////////////////////////////////////
static int
ufsd_add_nls(
    IN OUT mount_options *opts,
    IN struct nls_table  *nls
    )
{
  int cp;
  if ( NULL == nls )
    return -1; // error

  for ( cp = 0; cp < opts->nls_count; cp++ ) {
    if ( 0 == strcmp( opts->nls[cp]->charset, nls->charset ) )
      return 0;
  }

  if ( opts->nls_count >= sizeof(opts->nls)/sizeof(opts->nls[0]) )
    return -1; // error

  opts->nls[opts->nls_count] = nls;
  opts->nls_count += 1;
  return 0; // ok
}
#endif


static const char s_Options[][16] = {
  "nocase",           // 0
  "uid",              // 1
  "gid",              // 2
  "umask",            // 3
  "fmask",            // 4
  "dmask",            // 5
  "trace",            // 6
  "log",              // 7
  "sys_immutable",    // 8
  "quiet",            // 9
  "noatime",          // 10
  "bestcompr",        // 11
  "showmeta",         // 12
  "nobuf",            // 13
  "sparse",           // 14
  "codepage",         // 15
  "nls",              // 16
  "iocharset",        // 17
  "force",            // 18
  "nohidden",         // 19
  "clump",            // 20
  "bias",             // 21
  "user_xattr",       // 22
  "acl",              // 23
  "chkcnv",           // 24
  "cycle",            // 25
  "delim",            // 26
  "nolazy",           // 27
  "delalloc",         // 28
  "nojnl",            // 29
  "wb",               // 30
  "ra",               // 31
  "discard"           // 32
};


///////////////////////////////////////////////////////////
// ufsd_parse_options
//
// Parse options.
// Helper function for read_super
// Returns 0 if error
///////////////////////////////////////////////////////////
static int
ufsd_parse_options(
    IN OUT char **options,
    OUT mount_options *opts
    )
{
  char *t,*v,*delim,**ret_opt=options;
  int i;
  char c;
  unsigned long tmp;

#ifdef UFSD_USE_NLS
  char nls_name[50];
  struct nls_table *nls;
  int cp;
  assert( 0 == opts->nls_count );
#endif

  assert( NULL != current->fs );

  //
  // Setup default options
  //
  opts->fs_uid   = __kuid_val( current_uid() );
  opts->fs_gid   = __kgid_val( current_gid() );
  opts->fs_fmask = opts->fs_dmask = ~current_umask();
  opts->bias     = -1;

  if ( NULL == options || NULL == options[0] || 0 == options[0][0] )
    goto Ok;

  while ( NULL != ( t = strsep( options, "," ) ) ) {

    // Save current pointer to "=" delimiter
    // It will be used to restore current option
    // to print in correct form in log message
    v = delim = strchr( t, '=' );
    if ( NULL != v )
      *v++ = 0;

    for ( i = 0; i < sizeof(s_Options)/sizeof(s_Options[0]); i++ ) {
      if ( 0 == strcmp( t, s_Options[i] ) )
        break;
    }

    switch( i ) {
      case 0:   // "nocase"
      case 22:  // "user_xattr"
      case 23:  // "acl"
      case 27:  // "nolazy"
      case 28:  // "delalloc"
      case 29:  // "nojnl"
      case 30:  // "wb="
      case 32:  // "discard"
        // Support both forms: 'nocase' and 'nocase=0/1'
        if ( NULL == v || 0 == v[0] )
          c = 1;  // parse short form "nocase"
        else if ( 0 == v[1] && '0' <= v[0] && v[0] <= '9' )
          c = (char)(v[0] - '0'); // parse wide form "nocase=X", where X=0,1,..,9
        else
          goto Err;
        switch( i ) {
          case 0:   opts->nocase = c; break;
          case 22:  opts->user_xattr = c; break;
          case 23:  opts->acl = c; break;
          case 27:  opts->nolazy = c; break;
          case 28:  opts->delalloc = c; break;
          case 29:  opts->nojnl = c; break;
          case 30:  opts->wb = c; break;
          case 32:  opts->discard = c; break;
        }
        break;
      case 1:   // "uid"
      case 2:   // "gid"
      case 21:  // "bias"
        if ( NULL == v || 0 == v[0] ) goto Err;
        tmp = simple_strtoul( v, &v, 0 );
        if ( 0 != v[0] ) goto Err;
        switch( i ) {
        case 1: opts->fs_uid = tmp; opts->uid = 1; break;
        case 2: opts->fs_gid = tmp; opts->gid = 1; break;
        case 21: opts->bias = tmp; break;
        }
        break;
      case 3: // "umask"
      case 4: // "fmask"
      case 5: // "dmask"
        if ( NULL == v || 0 == v[0] ) goto Err;
        tmp = ~simple_strtoul( v, &v, 8 );
        if ( 0 != v[0] ) goto Err;
        switch( i ) {
        case 3: opts->fs_fmask = opts->fs_dmask = tmp; opts->fmask = opts->dmask = 1; break;
        case 4: opts->fs_fmask = tmp; opts->fmask = 1; break;
        case 5: opts->fs_dmask = tmp; opts->dmask = 1; break;
        }
        break;
      case 20:  // "clump"
      case 31:  // "ra"
        if ( NULL == v || 0 == v[0] ) goto Err;
        tmp = simple_strtoul( v, &v, 0 );
        if ( 0 == v[0] || 'K' == v[0] )
          ;
        else if ( 'M' == *v )
          tmp *= 1024;
        else
          goto Err;
        switch( i ) {
        case 20: opts->clumpKb = tmp; break;
        case 31: opts->raKb = tmp; break;
        }
        break;
#ifdef UFSD_TRACE
      case 6: // "trace"
        parse_trace_level( v );
        break;
      case 7: // "log"
        if ( NULL == v ) goto Err;
        strncpy( ufsd_trace_file, v, sizeof(ufsd_trace_file) );
        ufsd_trace_file[sizeof(ufsd_trace_file)-1] = 0;
        break;
      case 25:  // "cycle"
        // Support both forms: 'cycle' and 'cycle=256'
        if ( NULL == v || 0 == v[0] )
          tmp = 1;
        else {
          tmp = simple_strtoul( v, &v, 0 );
          if ( 'K' == *v )
            tmp *= 1024;
          else if ( 'M' == *v )
            tmp *= 1024*1024;
        }
        ufsd_cycle_mb = (tmp + 1024*1024 - 1) >> 20;
        break;
#else
      case 6:   // trace
      case 7:   // log
      case 25:  // cycle
        break;
#endif
      case 8: // "sys_immutable"
        if ( NULL != v ) goto Err;
        opts->sys_immutable = 1;
        break;
      case 9: // "quiet"
        break;
      case 10: // "noatime"
        if ( NULL != v ) goto Err;
        opts->noatime = 1;
        break;
      case 11: // "bestcompr"
        if ( NULL != v ) goto Err;
        opts->bestcompr = 1;
        break;
      case 12: // "showmeta"
        if ( NULL != v ) goto Err;
        opts->showmeta = 1;
        break;
      case 13: // "nobuf"
        break;
      case 14: // "sparse"
        if ( NULL != v ) goto Err;
        opts->sparse = 1;
        break;
#ifdef UFSD_USE_NLS
      case 15: // "codepage"
        if ( NULL == v || 0 == v[0] ) goto Err;
        sprintf( nls_name, "cp%d", (int)simple_strtoul( v, &v, 0 ) );
        if ( 0 != v[0] ) goto Err;
        v = nls_name;
        // no break here!!
      case 16: // "nls"
      case 17: // "iocharset"
        // Add this nls into array of codepages
        if ( NULL == v || 0 == v[0] || ufsd_add_nls( opts, load_nls(v) ) )
          goto Err;
#else
      case 15: // "codepage"
      case 16: // "nls"
      case 17: // "iocharset"
        // Ignore any nls related options
#endif
        break;
      case 18: // "force"
        if ( NULL != v ) goto Err;
        opts->force = 1;
        break;
      case 19: // "nohidden"
        if ( NULL != v ) goto Err;
        opts->nohidden = 1;
        break;
      case 24: // "chkcnv"
        if ( NULL != v ) goto Err;
        opts->chkcnv = 1;
        break;
      case 26:  // "delim=':'
        if ( NULL == v || 0 == v[0] )
          opts->delim = 0;
        else if ( 0 == v[1] )
          opts->delim = v[0];
        else
          goto Err;
        break;
      default:
Err:
        // Return error options
        *ret_opt = t;
    }

    // Restore options string
    if ( NULL != delim )
      delim[0] = '=';

    // Restore full string
    if ( NULL != *options )
      (*options)[-1] = ',';

    if ( *ret_opt == t )
      return 0; // error
  }

Ok:
#ifdef UFSD_USE_NLS
  //
  // Load default nls if no nls related options
  //
  if ( 0 == opts->nls_count ){
    struct nls_table *nls_def = load_nls_default();
    if ( NULL != nls_def && 0 != memcmp( nls_def->charset, "utf8", sizeof("utf8") ) ) {
#ifndef UFSD_TRACE_SILENT
      DebugTrace( 0, Dbg, ("default nls %s\n", nls_def->charset ));
//      printk( KERN_NOTICE QUOTED_UFSD_DEVICE": default nls %s\n", nls_def->charset );
#endif
      ufsd_add_nls( opts, nls_def );
    }
  } else {
    //
    // Remove kernel utf8 and use builtin utf8
    //
    for ( cp = 0; cp < opts->nls_count; cp++ ) {
      nls = opts->nls[cp];
      if ( 0 == memcmp( nls->charset, "utf8", sizeof("utf8") ) ) {
#ifndef UFSD_TRACE_SILENT
        DebugTrace( 0, Dbg, ("unload kernel utf8\n"));
//        printk( KERN_NOTICE QUOTED_UFSD_DEVICE": use builtin utf8 instead of kernel utf8\n" );
#endif
        unload_nls( nls );
        opts->nls[cp] = NULL;
      } else {
#ifndef UFSD_TRACE_SILENT
        DebugTrace( 0, Dbg, ("loaded nls %s\n", nls->charset ));
        printk( KERN_NOTICE QUOTED_UFSD_DEVICE": loaded nls %s\n", nls->charset );
#endif
      }
    }
  }
#endif

  //
  // If no nls then use builtin utf8
  //
  if ( 0 == opts->nls_count ) {
#ifndef UFSD_TRACE_SILENT
    DebugTrace( 0, Dbg, ("use builtin utf8\n" ));
//    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": use builtin utf8\n" );
#endif
    opts->nls_count = 1;
    opts->nls[0]    = NULL;
  }

#ifndef UFSD_USE_XATTR
  opts->acl = opts->user_xattr = 0;
#endif

#ifndef UFSD_TRACE_SILENT
  if ( opts->delalloc ) {
#ifndef UFSD_DELAY_ALLOC
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ignore delalloc 'cause not supported\n" );
    opts->delalloc = 0;
#elif !defined Try_to_writeback_inodes_sb
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": delalloc may work incorrect due to old kernel\n" );
    DebugTrace( 0, UFSD_LEVEL_ALWAYS, (" delalloc may work incorrect due to old kernel\n"));
#endif
  }
#endif

  if ( opts->wb ) {
#ifndef Try_to_writeback_inodes_sb
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ignore \"wb=1\" 'cause not supported\n" );
    opts->wb = 0;
#elif defined UFSD_DELAY_ALLOC
    if ( opts->delalloc ) {
      printk( KERN_ERR QUOTED_UFSD_DEVICE": \"delalloc\" and \"wb=1\" are not compatible\n" );
      return 0; // error
    }
#endif
  }

#if !(defined HAVE_STRUCT_SUPER_BLOCK_S_BDI && HAVE_STRUCT_SUPER_BLOCK_S_BDI)
  if ( opts->raKb ){
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ignore \"ra\" option 'cause not supported\n" );
    opts->raKb = 0;
  }
#endif

  return 1; // ok
}


//
// This mutex is used to protect 'ufsd_trace_level'
//
TRACE_ONLY( struct mutex  s_MountMutex );


///////////////////////////////////////////////////////////
// ufsd_read_super
//
// This routine is a callback used to recognize and
// initialize superblock using this filesystem driver.
//
// sb - Superblock structure. On entry sb->s_dev is set to device,
//     sb->s_flags contains requested mount mode.
//     On exit this structure should have initialized root directory
//     inode and superblock callbacks.
//
// data - mount options in a string form.
//
// silent - non-zero if no messages should be displayed.
//
// Return: struct super_block* - 'sb' on success, NULL on failure.
//
///////////////////////////////////////////////////////////
static int
ufsd_read_super(
    IN OUT struct super_block *sb,
    IN void *data,
    IN int  silent
    )
{
  ufsd_volume* Volume = NULL;
  int err = -EINVAL; // default error
  ufsd_volume_info  info;
  usuper *sbi = NULL;
  struct inode *i = NULL;
  char *options = (char*)data;
  ufsd_iget4_param param;
  const char *dev_name;
  struct block_device *bdev   = sb->s_bdev;
  UINT64 sb_size = bdev->bd_inode->i_size;
  UINT64 free_clusters = 0;
#if defined HAVE_DECL_BDEV_PHYSICAL_BLOCK_SIZE && HAVE_DECL_BDEV_PHYSICAL_BLOCK_SIZE
  unsigned int BytesPerSector = bdev_physical_block_size( bdev );
#elif (defined HAVE_DECL_BDEV_HARDSECT_SIZE && HAVE_DECL_BDEV_HARDSECT_SIZE)
  unsigned int BytesPerSector = bdev_hardsect_size( bdev );
#else
  unsigned int BytesPerSector = 512;
#endif
#ifndef UFSD_TRACE
  struct sysinfo    sys_info;
#endif
  struct sysinfo    *psys_info;

  TRACE_ONLY( const char *hint = ""; )
  TRACE_ONLY( unsigned long TraceLevelToRestore; )  // Trace level to restore late

  sbi = ufsd_heap_alloc( sizeof(usuper), 1 );
  assert(NULL != sbi);
  if ( NULL == sbi )
    return -ENOMEM;

  mutex_init( &sbi->api_mutex );

  spin_lock_init( &sbi->ddt_lock );
  mutex_init( &sbi->nocase_mutex );
  INIT_LIST_HEAD( &sbi->clear_list );
#ifdef UFSD_USE_FLUSH_THREAD
  rwlock_init( &sbi->state_lock );
  init_waitqueue_head( &sbi->wait_done_flush );
  init_waitqueue_head( &sbi->wait_exit_flush );
#endif

  dev_name = sb->s_id;

  //
  // Most of the function is under s_MountMutex
  //
  TRACE_ONLY( mutex_lock( &s_MountMutex ); )
  TRACE_ONLY( TraceLevelToRestore = ufsd_trace_level; )  // Save trace level to restore late

  //
  // Check for size
  //
  if ( sb_size <= 10*PAGE_CACHE_SIZE ) {
    printk(KERN_WARNING QUOTED_UFSD_DEVICE": \"%s\": the volume size (0x%llx bytes) is too small to keep any fs\n", dev_name, sb_size );
    TRACE_ONLY( hint = "too small"; )
    goto ExitInc;
  }

  //
  // Parse options.
  //
  if ( !ufsd_parse_options( &options, &sbi->options ) ) {
    printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to mount \"%s\". bad option \"%s\"\n", dev_name, options );
    TRACE_ONLY( hint = "bad options"; )
    goto ExitInc;
  }

#ifdef UFSD_TRACE
  if ( TraceLevelToRestore != ufsd_trace_level ) {
    TraceLevelToRestore = ufsd_trace_level; // "trace=" was parsed while mounting
  } else if ( !FlagOn( TraceLevelToRestore, UFSD_LEVEL_UFSD ) ) {
    TraceLevelToRestore = ufsd_trace_level;
    // Temporary turn on UFSD logs
    ufsd_trace_level |= UFSD_LEVEL_UFSD | UFSD_LEVEL_ERROR;
  }
#endif

  //
  // Now trace is activated
  //
  DebugTrace( +1, Dbg, ("read_super(\"%s\"), %u: %p %lx, %s, %s\n", dev_name, jiffies_to_msecs(jiffies-StartJiffies),
                        sb, sb->s_flags, (char*)data,  silent ? "silent" : "verbose"));

#ifdef UFSD_TRACE
  psys_info = &sbi->sys_info;
#else
  psys_info = &sys_info;
#endif
  si_meminfo( psys_info );

  DebugTrace( 0, Dbg, ("Pages: total=%lx, free=%lx, buff=%lx, unit="_QUOTE2(PAGE_SIZE)"\n",
                        psys_info->totalram, psys_info->freeram, psys_info->bufferram ));

  DebugTrace( 0, Dbg, ("Page flags: lck=%x, err=%x, ref=%x, upt=%x, drt=%x, wrb=%x, priv=%x\n",
                        1u<<PG_locked, 1u<<PG_error, 1u<<PG_referenced, 1u<<PG_uptodate, 1u<<PG_dirty, 1u<<PG_writeback, 1u<<PG_private ));
  DebugTrace( 0, Dbg, ("Buff flags: upt=%x, drt=%x, lck=%x, map=%x, new=%x, del=%x, aread=%x, awrite=%x\n",
                        1u<<BH_Uptodate, 1u<<BH_Dirty, 1u<<BH_Lock, 1u<<BH_Mapped, 1u<<BH_New, 1u<<BH_Delay, 1u<<BH_Async_Read, 1u<<BH_Async_Write ));

#if defined HAVE_STRUCT_SUPER_BLOCK_S_D_OP && HAVE_STRUCT_SUPER_BLOCK_S_D_OP
  sb->s_d_op = sbi->options.nocase? &ufsd_dop : NULL;
#endif

  //
  // Save 'sync' flag
  //
  if ( FlagOn( sb->s_flags, MS_SYNCHRONOUS ) )
    sbi->options.sync = 1;

  sb_set_blocksize( sb, PAGE_CACHE_SIZE );
  sbi->max_block = sb_size >> PAGE_CACHE_SHIFT;

  //
  // set s_fs_info to access options in BdRead/BdWrite
  //
  sb->s_fs_info = sbi;

  //
  // Set default readahead pages
  //
#ifdef UFSD_READAHEAD_PAGES
  sbi->ra_blocks = UFSD_READAHEAD_PAGES;
#endif

  //
  // 'dev' member of superblock set to device in question.
  // At exit in case of filesystem been
  // successfully recognized next members of superblock should be set:
  // 's_magic'    - filesystem magic nr
  // 's_maxbytes' - maximal file size for this filesystem.
  //
  DebugTrace( 0, Dbg, ("\"%s\": size = 0x%llx*0x%x >= 0x%llx*0x%lx\n",
                        dev_name, sb_size>>blksize_bits( BytesPerSector ), BytesPerSector, sbi->max_block, PAGE_CACHE_SIZE ));

//  {
//    unsigned int logical_block_size = NULL == bdev? 0 : queue_logical_block_size( bdev->bd_disk->queue );
//    printk( KERN_ERR QUOTED_UFSD_DEVICE": logical_block_size of \"%s\" = %x\n", dev_name, logical_block_size );
//    DebugTrace( 0, Dbg, ("logical_block_size of \"%s\" = %x\n", dev_name, logical_block_size ));
//  }

#ifdef Blkdev_issue_discard
  {
    struct request_queue * q = bdev_get_queue( bdev );
    if ( NULL == q || !blk_queue_discard( q ) || 0 == q->limits.discard_granularity ) {
      DebugTrace( 0, Dbg, ( "\"%s\": no discard\n", dev_name ));
    } else {
      sbi->discard_granularity          = q->limits.discard_granularity;
      sbi->discard_granularity_mask_inv = ~(UINT64)(sbi->discard_granularity-1);
      SetFlag( sbi->flags, UFSD_SBI_FLAGS_DISRCARD );
      DebugTrace( 0, Dbg, ( "\"%s\": discard_granularity = %x, max_discard_sectors = %x\n", dev_name, sbi->discard_granularity, q->limits.max_discard_sectors ));
    }
  }
#endif

  err = ufsdapi_volume_mount( sb, BytesPerSector, &sb_size, &sbi->options, &Volume, psys_info->totalram, PAGE_CACHE_SIZE );

  if ( 0 != err ) {
    if ( ERR_NEED_REPLAY == err ) {
      printk( KERN_ERR QUOTED_UFSD_DEVICE": unable to replay native journal on \"%s\"\n", dev_name);
    } else {
      if (!silent)
        printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to mount \"%s\"\n", dev_name);
      TRACE_ONLY( hint = "unknown fs"; )
    }
    err = -EINVAL;
    goto Exit;
  }

//  sb->s_flags |= MS_NODIRATIME|MS_NOATIME;
  sb->s_flags = (sb->s_flags & ~MS_POSIXACL) | MS_NODIRATIME
            | (sbi->options.noatime? MS_NOATIME : 0)
            | (sbi->options.acl? MS_POSIXACL : 0);

  //
  // At this point filesystem has been recognized.
  // Let's query for it's capabilities.
  //
  ufsdapi_query_volume_info( Volume, &info, NULL, 0, sbi->options.delalloc? &free_clusters : NULL );

  if ( info.ReadOnly && !FlagOn( sb->s_flags, MS_RDONLY ) ) {
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": No write support. Marking filesystem read-only\n");
    sb->s_flags |= MS_RDONLY;
  }

  //
  // Check for dirty flag
  //
  if ( !FlagOn( sb->s_flags, MS_RDONLY ) && info.dirty && !sbi->options.force ) {
    printk( KERN_WARNING QUOTED_UFSD_DEVICE": volume is dirty and \"force\" flag is not set\n" );
    TRACE_ONLY( hint = "no \"force\" and dirty"; )
    err = -1000; // Return special value to detect no 'force'
    goto Exit;
  }

  //
  // Set maximum file size and 'end of directory'
  //
  sb->s_maxbytes  = info.maxbytes;
  sbi->end_of_dir = info.end_of_dir;
  sb->s_time_gran = 1000000000; // 1 sec

  sbi->bytes_per_cluster  = info.bytes_per_cluster;
  sbi->cluster_mask       = info.bytes_per_cluster-1;
  sbi->cluster_mask_inv   = ~sbi->cluster_mask;

  //
  // At this point I know enough to allocate my root.
  //
  sb->s_magic       = info.fs_signature;
  sb->s_op          = &ufsd_sops;
  // NFS support
  sb->s_export_op   = &ufsd_export_op;
#ifdef UFSD_EXFAT
  if ( info.NeedEncode )
    sb->s_export_op = &ufsd_encode_export_op;
#endif

#ifdef UFSD_USE_XATTR
  sb->s_xattr       = (__typeof__( sb->s_xattr )) ufsd_xattr_handlers;
#endif
  sbi->ufsd         = Volume;
  assert(UFSD_SB( sb ) == sbi);
  assert(UFSD_VOLUME(sb) == Volume);

  param.subdir_count = 0;
  if ( 0 == ufsdapi_file_open( Volume, NULL, "/", 1, NULL,
#ifdef UFSD_COUNT_CONTAINED
                              &param.subdir_count,
#else
                              NULL,
#endif
                              &param.fh, &param.fi ) ) {
    param.Create = NULL;
    i = iget4( sb, param.fi->Id, NULL, &param );
  }

  if ( NULL == i ) {
    printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to open root on \"%s\"\n", dev_name );
    TRACE_ONLY( hint = "open root"; )
    err = -EINVAL;
    goto Exit;
  }

  // Always clear S_IMMUTABLE
  i->i_flags &= ~S_IMMUTABLE;
#if defined HAVE_DECL_D_MAKE_ROOT && HAVE_DECL_D_MAKE_ROOT
  sb->s_root = d_make_root( i );
#else
  sb->s_root = d_alloc_root( i );
#endif

  if ( NULL == sb->s_root ) {
    iput( i );
    printk( KERN_ERR QUOTED_UFSD_DEVICE": No memory for root entry\n" );
    TRACE_ONLY( hint = "no memory"; )
    // Not necessary to close root_ufsd
    goto Exit;
  }

  // Create /proc/fs/ufsd/..
  ufsd_proc_info_create( sb );

#if defined HAVE_STRUCT_SUPER_BLOCK_S_BDI && HAVE_STRUCT_SUPER_BLOCK_S_BDI
  if ( sbi->options.raKb )
    sb->s_bdi->ra_pages = sbi->options.raKb >> ( PAGE_CACHE_SHIFT-10 );
#else
  if ( sbi->options.raKb )
    bdev->bd_inode_backing_dev_info->ra_pages = sbi->options.raKb >> ( PAGE_CACHE_SHIFT-10 );
#endif

#ifdef UFSD_CHECK_BDI
  // Save current bdi to check for media suprise remove
  #if defined HAVE_STRUCT_SUPER_BLOCK_S_BDI && HAVE_STRUCT_SUPER_BLOCK_S_BDI
    sbi->bdi = sb->s_bdi;
  #else
    sbi->bdi = blk_get_backing_dev_info( bdev );
  #endif
#endif

#ifdef UFSD_USE_FLUSH_THREAD
  //
  // Start flush thread.
  // To simplify remount logic do it for read-only volumes too
  //
  {
    void *p = kthread_run( ufsd_flush_thread, sb, "ufsd_%s", dev_name );
    if ( IS_ERR( p ) ) {
//      printk( KERN_ERR QUOTED_UFSD_DEVICE": failed to create flush thread\n" );
      err = PTR_ERR( p );
      goto Exit;
    }

    wait_event( sbi->wait_done_flush, NULL != sbi->flush_task );
  }
#endif

  //
  // Done.
  //
  DebugTrace( -1, Dbg, ("read_super(\"%s\"), %u -> sb=%p,i=%p,r=%lx,uid=%d,gid=%d,m=%o\n", dev_name, jiffies_to_msecs(jiffies-StartJiffies), sb, i,
                        i->i_ino, __kuid_val( i->i_uid ), __kgid_val( i->i_gid ), i->i_mode ));

  err = 0;

  if ( 0 ) {
ExitInc:
#ifdef UFSD_TRACE
    if ( ufsd_trace_level & Dbg )
      ufsd_trace_inc( +1 ); // compensate the last 'DebugTrace( -1, ... )'
#endif

Exit:
    //
    // Free resources allocated in this function
    //
    if ( NULL != Volume )
      ufsdapi_volume_umount( Volume );

    assert( NULL != sbi );
#ifndef CONFIG_DEBUG_MUTEXES // G.P.L.
    mutex_destroy( &sbi->api_mutex );
    mutex_destroy( &sbi->nocase_mutex );
#endif
    ufsd_uload_nls( &sbi->options );

    // NOTE: 'dev_name' may point into 'sbi->s_id'
    DebugTrace( -1, Dbg, ("read_super failed to mount %s: \"%s\" ->%d\n", dev_name, hint, err));

    ufsd_heap_free( sbi );
    sb->s_fs_info = NULL;
  }

  // Restore trace level
  TRACE_ONLY( ufsd_trace_level = TraceLevelToRestore; )
  TRACE_ONLY( mutex_unlock( &s_MountMutex ); )

  return err;
}


#if defined HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT && HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT
///////////////////////////////////////////////////////////
// ufsd_mount (2.6.38+)
//
// fstype::mount
///////////////////////////////////////////////////////////
static struct dentry*
ufsd_mount(
    IN struct file_system_type  *fs_type,
    IN int        flags,
    IN const char *dev_name,
    IN void       *data
    )
{
  return mount_bdev( fs_type, flags, dev_name, data, ufsd_read_super );
}

#else

///////////////////////////////////////////////////////////
// ufsd_get_sb  [2.6.18 - 2.6.38]
//
// fstype::get_sb
///////////////////////////////////////////////////////////
static int
ufsd_get_sb(
    IN struct file_system_type  *fs_type,
    IN int              flags,
    IN const char       *dev_name,
    IN void             *data,
    IN struct vfsmount  *mnt
    )
{
  int err = get_sb_bdev( fs_type, flags, dev_name, data, ufsd_read_super, mnt );
  if ( 0 == err ) {
    // Save mount path to correct ntfs symlinks (see ufsd_readlink_hlp)
    usuper *sbi = UFSD_SB( mnt->mnt_sb );
    sbi->vfs_mnt = mnt;
  }
  return err;
}
#endif

static struct file_system_type ufsd_fs_type = {
  .owner      = THIS_MODULE,
  .name       = QUOTED_UFSD_DEVICE,
#if defined HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT && HAVE_STRUCT_FILE_SYSTEM_TYPE_MOUNT
  .mount      = ufsd_mount,
#else
  .get_sb     = ufsd_get_sb,
#endif
  .kill_sb    = kill_block_super,
  .fs_flags   = FS_REQUIRES_DEV,
};


///////////////////////////////////////////////////////////
// ufsd_init
//
// module init function
///////////////////////////////////////////////////////////
static int
__init ufsd_init(void)
{
  int ret;
  int EndianError;
#ifdef UFSD_DEBUG_ALLOC
  TotalKmallocs=0;
  TotalVmallocs=0;
  UsedMemMax=0;
  TotalAllocs=0;
  TotalAllocBlocks=0;
  TotalAllocSequence=0;
  MemMaxRequest=0;
  mutex_init( &MemMutex );
#endif

  TRACE_ONLY( mutex_init( &s_MountMutex ); )

  DEBUG_ONLY( WaitMutex=0; )
  TRACE_ONLY( StartJiffies=jiffies; )

  TRACE_ONLY( parse_trace_level( ufsd_trace_level_ ); )

#ifndef UFSD_TRACE_SILENT
  printk( KERN_NOTICE QUOTED_UFSD_DEVICE": driver (%s) loaded at %p\n%s", s_DriverVer, UFSD_MODULE_CORE(), ufsdapi_library_version( &EndianError ) );
#else
  ufsdapi_library_version( &EndianError );
#endif

  if ( EndianError )
    return -EINVAL;

#ifdef UFSD_EXFAT
  //
  // exfat stores dates relative 1980
  // 'get_seconds' returns seconds since 1970
  // Check current date
  if ( get_seconds() < Seconds1970To1980 )
    printk( KERN_NOTICE QUOTED_UFSD_DEVICE": exfat can't store dates before Jan 1, 1980. Please update current date\n" );
#endif

  ufsd_proc_create();

  unode_cachep  = Kmem_cache_create( QUOTED_UFSD_DEVICE "_unode_cache", sizeof(unode), SLAB_MEM_SPREAD, init_once );

  //
  // Allow UFSD to init globals
  //
  ret = ufsdapi_main( 0 );
  if ( 0 == ret ) {
    //
    // Finally register filesystem
    //
    ret = register_filesystem( &ufsd_fs_type );
    if ( 0 == ret )
      return 0; // Ok
  }

  //
  // Deinit UFSD globals
  //
  ufsdapi_main( 1 );

  kmem_cache_destroy( unode_cachep );

  // remove /proc/fs/ufsd
  ufsd_proc_delete();

//  printk( KERN_NOTICE QUOTED_UFSD_DEVICE": ufsd_init failed %d\n", ret );

  return ret;
}


///////////////////////////////////////////////////////////
// ufsd_exit
//
// module exit function
///////////////////////////////////////////////////////////
static void
__exit ufsd_exit(void)
{
#ifdef UFSD_DEBUG_ALLOC
  struct list_head *pos, *pos2;
#endif
  // remove /proc/fs/ufsd
  ufsd_proc_delete();

  unregister_filesystem( &ufsd_fs_type );

  //
  // Deinit UFSD globals
  //
  ufsdapi_main( 1 );

  kmem_cache_destroy( unode_cachep );

#ifndef UFSD_TRACE_SILENT
  printk( KERN_NOTICE QUOTED_UFSD_DEVICE": driver unloaded\n" );
#endif

#if defined UFSD_TRACE && defined UFSD_DEBUG && (defined UFSD_NTFS2 || defined UFSD_EXFAT || defined UFSD_REFS)
  {
    int i;
    for ( i = 0; i < ARRAY_SIZE(s_shared); i++ ) {
      if ( 0 != s_shared[i].cnt )
        DebugTrace( 0, UFSD_LEVEL_ERROR, ("forgotten shared ptr %p,%x,%d\n", s_shared[i].ptr, s_shared[i].len, s_shared[i].cnt ));
    }
  }
#endif

#ifdef UFSD_DEBUG_ALLOC
  assert(0 == TotalAllocs);
  trace_mem_report( 1 );
  list_for_each_safe( pos, pos2, &TotalAllocHead )
  {
    memblock_head *block = list_entry( pos, memblock_head, Link );
    unsigned char *p = (unsigned char*)(block+1);
    unsigned char tag[9];
    DebugTrace( 0, UFSD_LEVEL_ERROR,
           ("block %p, seq=%u, %u bytes, tag '%s': '%02x %02x %02x %02x %02x %02x %02x %02x'\n",
          p, block->seq, block->size,
          ufsd_make_tag_string(p, tag),
          p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]));

    // Don't: (( block->asize & 1U)? vfree : kfree)( block );
    // 'cause declaration of vfree and kfree differs!
    if ( block->asize & 1U )
      vfree( block );
    else
      kfree( block );
  }
#ifdef UFSD_DEBUG
  DebugTrace( 0, UFSD_LEVEL_ERROR, ("inuse = %u msec, wait = %u msec, HZ=%u\n", jiffies_to_msecs( jiffies - StartJiffies ), jiffies_to_msecs( WaitMutex ), (unsigned)HZ ));
#endif
#endif
  ufsd_close_trace();
#ifndef CONFIG_DEBUG_MUTEXES // G.P.L.
  TRACE_ONLY( mutex_destroy( &s_MountMutex ); )
#endif
}

//
// And now the modules code and kernel interface.
//
MODULE_DESCRIPTION("Paragon " QUOTED_UFSD_DEVICE " driver");
MODULE_AUTHOR("Andrey Shedel & Alexander Mamaev");
MODULE_LICENSE("Commercial product");

#ifdef UFSD_TRACE
module_param_string(trace, ufsd_trace_level_, sizeof(ufsd_trace_level_), S_IRUGO);
MODULE_PARM_DESC(trace, " trace level for ufsd module");
module_param_string(log, ufsd_trace_file, sizeof(ufsd_trace_file), S_IRUGO);
MODULE_PARM_DESC(log, " ufsd log file, default is system log");
module_param_named(cycle, ufsd_cycle_mb, ulong, S_IRUGO);
MODULE_PARM_DESC(cycle, " the size of cycle log in MB, default is 0");
#endif

module_init(ufsd_init)
module_exit(ufsd_exit)
