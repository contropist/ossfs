/*
 * ossfs -  FUSE-based file system backed by Alibaba Cloud OSS
 *
 * Copyright(C) 2007 Takeshi Nakatani <ggtakec.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <unistd.h>
#include <limits.h>
#include <sys/time.h>
#include <algorithm>

#include "common.h"
#include "s3fs.h"
#include "fdcache_entity.h"
#include "fdcache.h"
#include "string_util.h"
#include "s3fs_util.h"
#include "autolock.h"
#include "curl.h"

//------------------------------------------------
// Symbols
//------------------------------------------------
static const int MAX_MULTIPART_CNT         = 10 * 1000; // OSS multipart max count

//------------------------------------------------
// FdEntity class variables
//------------------------------------------------
bool FdEntity::mixmultipart = true;

//------------------------------------------------
// FdEntity class methods
//------------------------------------------------
bool FdEntity::SetNoMixMultipart()
{
    bool old = mixmultipart;
    mixmultipart = false;
    return old;
}

int FdEntity::FillFile(int fd, unsigned char byte, off_t size, off_t start)
{
    unsigned char bytes[1024 * 32];         // 32kb
    memset(bytes, byte, std::min(static_cast<off_t>(sizeof(bytes)), size));

    for(off_t total = 0, onewrote = 0; total < size; total += onewrote){
        if(-1 == (onewrote = pwrite(fd, bytes, std::min(static_cast<off_t>(sizeof(bytes)), size - total), start + total))){
            S3FS_PRN_ERR("pwrite failed. errno(%d)", errno);
            return -errno;
        }
    }
    return 0;
}

// [NOTE]
// If fd is wrong or something error is occurred, return 0.
// The ino_t is allowed zero, but inode 0 is not realistic.
// So this method returns 0 on error assuming the correct
// inode is never 0.
// The caller must have exclusive control.
//
ino_t FdEntity::GetInode(int fd)
{
    if(-1 == fd){
        S3FS_PRN_ERR("file descriptor is wrong.");
        return 0;
    }

    struct stat st;
    if(0 != fstat(fd, &st)){
        S3FS_PRN_ERR("could not get stat for physical file descriptor(%d) by errno(%d).", fd, errno);
        return 0;
    }
    return st.st_ino;
}

//------------------------------------------------
// FdEntity methods
//------------------------------------------------
FdEntity::FdEntity(const char* tpath, const char* cpath) :
    is_lock_init(false), path(SAFESTRPTR(tpath)),
    physical_fd(-1), pfile(NULL), inode(0), size_orgmeta(0),
    cachepath(SAFESTRPTR(cpath)), is_meta_pending(false),
    is_direct_read(direct_read)
{
    holding_mtime.tv_sec = -1;
    holding_mtime.tv_nsec = 0;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
#if S3FS_PTHREAD_ERRORCHECK
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#endif
    int result;
    if(0 != (result = pthread_mutex_init(&fdent_lock, &attr))){
        S3FS_PRN_CRIT("failed to init fdent_lock: %d", result);
        abort();
    }
    if(0 != (result = pthread_mutex_init(&fdent_data_lock, &attr))){
        S3FS_PRN_CRIT("failed to init fdent_data_lock: %d", result);
        abort();
    }
    is_lock_init = true;
}

FdEntity::~FdEntity()
{
    Clear();

    if(is_lock_init){
      int result;
      if(0 != (result = pthread_mutex_destroy(&fdent_data_lock))){
          S3FS_PRN_CRIT("failed to destroy fdent_data_lock: %d", result);
          abort();
      }
      if(0 != (result = pthread_mutex_destroy(&fdent_lock))){
          S3FS_PRN_CRIT("failed to destroy fdent_lock: %d", result);
          abort();
      }
      is_lock_init = false;
    }
}

void FdEntity::Clear()
{
    AutoLock auto_lock(&fdent_lock);
    AutoLock auto_data_lock(&fdent_data_lock);

    for(fdinfo_map_t::iterator iter = pseudo_fd_map.begin(); iter != pseudo_fd_map.end(); ++iter){
        PseudoFdInfo* ppseudofdinfo = iter->second;
        delete ppseudofdinfo;
    }
    pseudo_fd_map.clear();

    if(-1 != physical_fd){
        if(!cachepath.empty()){
            // [NOTE]
            // Compare the inode of the existing cache file with the inode of
            // the cache file output by this object, and if they are the same,
            // serialize the pagelist.
            //
            ino_t cur_inode = GetInode();
            if(0 != cur_inode && cur_inode == inode){
                CacheFileStat cfstat(path.c_str());
                if(!pagelist.Serialize(cfstat, true, inode)){
                    S3FS_PRN_WARN("failed to save cache stat file(%s).", path.c_str());
                }
            }
        }
        if(pfile){
            fclose(pfile);
            pfile = NULL;
        }
        physical_fd = -1;
        inode       = 0;

        if(!mirrorpath.empty()){
            if(-1 == unlink(mirrorpath.c_str())){
                S3FS_PRN_WARN("failed to remove mirror cache file(%s) by errno(%d).", mirrorpath.c_str(), errno);
            }
            mirrorpath.erase();
        }
    }
    pagelist.Init(0, false, false);
    path      = "";
    cachepath = "";
}

// [NOTE]
// This method returns the inode of the file in cachepath.
// The return value is the same as the class method GetInode().
// The caller must have exclusive control.
//
ino_t FdEntity::GetInode()
{
    if(cachepath.empty()){
        S3FS_PRN_INFO("cache file path is empty, then return inode as 0.");
        return 0;
    }

    struct stat st;
    if(0 != stat(cachepath.c_str(), &st)){
        S3FS_PRN_INFO("could not get stat for file(%s) by errno(%d).", cachepath.c_str(), errno);
        return 0;
    }
    return st.st_ino;
}

void FdEntity::Close(int fd)
{
    AutoLock auto_lock(&fdent_lock);

    S3FS_PRN_DBG("[path=%s][pseudo_fd=%d][physical_fd=%d]", path.c_str(), fd, physical_fd);

    // search pseudo fd and close it.
    fdinfo_map_t::iterator iter = pseudo_fd_map.find(fd);
    if(pseudo_fd_map.end() != iter){
        PseudoFdInfo* ppseudoinfo = iter->second;
        pseudo_fd_map.erase(iter);
        delete ppseudoinfo;
    }else{
        S3FS_PRN_WARN("Not found pseudo_fd(%d) in entity object(%s)", fd, path.c_str());
    }

    // check pseudo fd count
    if(-1 != physical_fd && 0 == GetOpenCount(true)){
        AutoLock auto_data_lock(&fdent_data_lock);
        if(!cachepath.empty()){
            // [NOTE]
            // Compare the inode of the existing cache file with the inode of
            // the cache file output by this object, and if they are the same,
            // serialize the pagelist.
            //
            ino_t cur_inode = GetInode();
            if(0 != cur_inode && cur_inode == inode){
                CacheFileStat cfstat(path.c_str());
                if(!pagelist.Serialize(cfstat, true, inode)){
                    S3FS_PRN_WARN("failed to save cache stat file(%s).", path.c_str());
                }
            }
        }
        if(pfile){
            fclose(pfile);
            pfile = NULL;
        }
        physical_fd = -1;
        inode       = 0;

        if(!mirrorpath.empty()){
            if(-1 == unlink(mirrorpath.c_str())){
                S3FS_PRN_WARN("failed to remove mirror cache file(%s) by errno(%d).", mirrorpath.c_str(), errno);
            }
            mirrorpath.erase();
        }
    }
}

int FdEntity::Dup(int fd, bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    S3FS_PRN_DBG("[path=%s][pseudo_fd=%d][physical_fd=%d][pseudo fd count=%zu]", path.c_str(), fd, physical_fd, pseudo_fd_map.size());

    if(-1 == physical_fd){
        return -1;
    }
    fdinfo_map_t::iterator iter = pseudo_fd_map.find(fd);
    if(pseudo_fd_map.end() == iter){
        S3FS_PRN_ERR("Not found pseudo_fd(%d) in entity object(%s) for physical_fd(%d)", fd, path.c_str(), physical_fd);
        return -1;
    }
    PseudoFdInfo*   org_pseudoinfo = iter->second;
    PseudoFdInfo*   ppseudoinfo    = new PseudoFdInfo(physical_fd, (org_pseudoinfo ? org_pseudoinfo->GetFlags() : 0));
    int             pseudo_fd      = ppseudoinfo->GetPseudoFd();
    pseudo_fd_map[pseudo_fd]       = ppseudoinfo;

    return pseudo_fd;
}

int FdEntity::OpenPseudoFd(int flags, bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    S3FS_PRN_DBG("[path=%s][physical_fd=%d][pseudo fd count=%zu]", path.c_str(), physical_fd, pseudo_fd_map.size());

    if(-1 == physical_fd){
        return -1;
    }
    PseudoFdInfo*   ppseudoinfo = new PseudoFdInfo(physical_fd, flags);
    int             pseudo_fd   = ppseudoinfo->GetPseudoFd();
    pseudo_fd_map[pseudo_fd]    = ppseudoinfo;

    return pseudo_fd;
}

int FdEntity::GetOpenCount(bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    return static_cast<int>(pseudo_fd_map.size());
}

//
// Open mirror file which is linked cache file.
//
int FdEntity::OpenMirrorFile()
{
    if(cachepath.empty()){
        S3FS_PRN_ERR("cache path is empty, why come here");
        return -EIO;
    }

    // make temporary directory
    std::string bupdir;
    if(!FdManager::MakeCachePath(NULL, bupdir, true, true)){
        S3FS_PRN_ERR("could not make bup cache directory path or create it.");
        return -EIO;
    }

    // create seed generating mirror file name
    unsigned int seed = static_cast<unsigned int>(time(NULL));
    int urandom_fd;
    if(-1 != (urandom_fd = open("/dev/urandom", O_RDONLY))){
        unsigned int rand_data;
        if(sizeof(rand_data) == read(urandom_fd, &rand_data, sizeof(rand_data))){
            seed ^= rand_data;
        }
        close(urandom_fd);
    }

    // try to link mirror file
    while(true){
        // make random(temp) file path
        // (do not care for threading, because allowed any value returned.)
        //
        char         szfile[NAME_MAX + 1];
        sprintf(szfile, "%x.tmp", rand_r(&seed));
        mirrorpath = bupdir + "/" + szfile;

        // link mirror file to cache file
        if(0 == link(cachepath.c_str(), mirrorpath.c_str())){
            break;
        }
        if(EEXIST != errno){
            S3FS_PRN_ERR("could not link mirror file(%s) to cache file(%s) by errno(%d).", mirrorpath.c_str(), cachepath.c_str(), errno);
            return -errno;
        }
        ++seed;
    }

    // open mirror file
    int mirrorfd;
    if(-1 == (mirrorfd = open(mirrorpath.c_str(), O_RDWR))){
        S3FS_PRN_ERR("could not open mirror file(%s) by errno(%d).", mirrorpath.c_str(), errno);
        return -errno;
    }
    return mirrorfd;
}

bool FdEntity::FindPseudoFd(int fd, bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    if(-1 == fd){
        return false;
    }
    if(pseudo_fd_map.end() == pseudo_fd_map.find(fd)){
        return false;
    }
    return true;
}

PseudoFdInfo* FdEntity::CheckPseudoFdFlags(int fd, bool writable, bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    if(-1 == fd){
        return NULL;
    }
    fdinfo_map_t::iterator iter = pseudo_fd_map.find(fd);
    if(pseudo_fd_map.end() == iter || NULL == iter->second){
        return NULL;
    }
    if(writable){
        if(!iter->second->Writable()){
            return NULL;
        }
    }else{
        if(!iter->second->Readable()){
            return NULL;
        }
    }
    return iter->second;
}

bool FdEntity::IsUploading(bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    for(fdinfo_map_t::const_iterator iter = pseudo_fd_map.begin(); iter != pseudo_fd_map.end(); ++iter){
        PseudoFdInfo* ppseudoinfo = iter->second;
        if(ppseudoinfo && ppseudoinfo->IsUploading()){
            return true;
        }
    }
    return false;
}

// [NOTE]
// If the open is successful, returns pseudo fd.
// If it fails, it returns an error code with a negative value.
//
int FdEntity::Open(const headers_t* pmeta, off_t size, time_t time, int flags, AutoLock::Type type)
{
    AutoLock auto_lock(&fdent_lock, type);

    S3FS_PRN_DBG("[path=%s][physical_fd=%d][size=%lld][time=%lld][flags=0x%x]", path.c_str(), physical_fd, static_cast<long long>(size), static_cast<long long>(time), flags);

    if (!auto_lock.isLockAcquired()) {
        // had to wait for fd lock, return
        S3FS_PRN_ERR("Could not get lock.");
        return -EIO;
    }

    AutoLock auto_data_lock(&fdent_data_lock);

    if(-1 != physical_fd){
        //
        // already open file
        //

        // check only file size(do not need to save cfs and time.
        if(0 <= size && pagelist.Size() != size){
            // truncate temporary file size
            if(-1 == ftruncate(physical_fd, size) || -1 == fsync(physical_fd)){
                S3FS_PRN_ERR("failed to truncate temporary file(physical_fd=%d) by errno(%d).", physical_fd, errno);
                return -errno;
            }
            // resize page list
            if(!pagelist.Resize(size, false, true)){      // Areas with increased size are modified
                S3FS_PRN_ERR("failed to truncate temporary file information(physical_fd=%d).", physical_fd);
                return -EIO;
            }
        }
        // set original headers and set size.
        off_t new_size = (0 <= size ? size : size_orgmeta);
        if(pmeta){
            orgmeta  = *pmeta;
            size_orgmeta = get_size(orgmeta);
        }
        if(new_size < size_orgmeta){
            size_orgmeta = new_size;
        }

    }else{
        //
        // file is not opened yet
        //
        bool  need_save_csf = false;  // need to save(reset) cache stat file
        bool  is_truncate   = false;  // need to truncate

        if(!cachepath.empty()){
            // using cache
            struct stat st;
            if(stat(cachepath.c_str(), &st) == 0){
                if(st.st_mtime < time){
                    S3FS_PRN_DBG("cache file stale, removing: %s", cachepath.c_str());
                    if(unlink(cachepath.c_str()) != 0){
                        return (0 == errno ? -EIO : -errno);
                    }
                }
            }

            // open cache and cache stat file, load page info.
            CacheFileStat cfstat(path.c_str());

            // try to open cache file
            if( -1 != (physical_fd = open(cachepath.c_str(), O_RDWR)) &&
                0 != (inode = FdEntity::GetInode(physical_fd))        &&
                pagelist.Serialize(cfstat, false, inode)          )
            {
                // succeed to open cache file and to load stats data
                memset(&st, 0, sizeof(struct stat));
                if(-1 == fstat(physical_fd, &st)){
                    S3FS_PRN_ERR("fstat is failed. errno(%d)", errno);
                    physical_fd = -1;
                    inode       = 0;
                    return (0 == errno ? -EIO : -errno);
                }
                // check size, st_size, loading stat file
                if(-1 == size){
                    if(st.st_size != pagelist.Size()){
                        pagelist.Resize(st.st_size, false, true); // Areas with increased size are modified
                        need_save_csf = true;     // need to update page info
                    }
                    size = st.st_size;
                }else{
                    if(size != pagelist.Size()){
                        pagelist.Resize(size, false, true);       // Areas with increased size are modified
                        need_save_csf = true;     // need to update page info
                    }
                    if(size != st.st_size){
                        is_truncate = true;
                    }
                }

            }else{
                if(-1 != physical_fd){
                    close(physical_fd);
                }
                inode = 0;

                // could not open cache file or could not load stats data, so initialize it.
                if(-1 == (physical_fd = open(cachepath.c_str(), O_CREAT|O_RDWR|O_TRUNC, 0600))){
                    S3FS_PRN_ERR("failed to open file(%s). errno(%d)", cachepath.c_str(), errno);

                    // remove cache stat file if it is existed
                    if(!CacheFileStat::DeleteCacheFileStat(path.c_str())){
                        if(ENOENT != errno){
                            S3FS_PRN_WARN("failed to delete current cache stat file(%s) by errno(%d), but continue...", path.c_str(), errno);
                        }
                    }
                    return (0 == errno ? -EIO : -errno);
                }
                need_save_csf = true;       // need to update page info
                inode         = FdEntity::GetInode(physical_fd);
                if(-1 == size){
                    size = 0;
                    pagelist.Init(0, false, false);
                }else{
                    // [NOTE]
                    // The modify flag must not be set when opening a file,
                    // if the time parameter(mtime) is specified(not -1) and
                    // the cache file does not exist.
                    // If mtime is specified for the file and the cache file
                    // mtime is older than it, the cache file is removed and
                    // the processing comes here.
                    //
                    pagelist.Resize(size, false, (0 <= time ? false : true));

                    is_truncate = true;
                }
            }

            // open mirror file
            int mirrorfd;
            if(0 >= (mirrorfd = OpenMirrorFile())){
                S3FS_PRN_ERR("failed to open mirror file linked cache file(%s).", cachepath.c_str());
                return (0 == mirrorfd ? -EIO : mirrorfd);
            }
            // switch fd
            close(physical_fd);
            physical_fd = mirrorfd;

            // make file pointer(for being same tmpfile)
            if(NULL == (pfile = fdopen(physical_fd, "wb"))){
                S3FS_PRN_ERR("failed to get fileno(%s). errno(%d)", cachepath.c_str(), errno);
                close(physical_fd);
                physical_fd = -1;
                inode       = 0;
                return (0 == errno ? -EIO : -errno);
            }

        }else{
            // not using cache
            inode = 0;

            // open temporary file
            if(NULL == (pfile = FdManager::MakeTempFile()) || -1 ==(physical_fd = fileno(pfile))){
                S3FS_PRN_ERR("failed to open temporary file by errno(%d)", errno);
                if(pfile){
                    fclose(pfile);
                    pfile = NULL;
                }
                return (0 == errno ? -EIO : -errno);
            }
            if(-1 == size){
                size = 0;
                pagelist.Init(0, false, false);
            }else{
                // [NOTE]
                // The modify flag must not be set when opening a file,
                // if the time parameter(mtime) is specified(not -1) and
                // the cache file does not exist.
                // If mtime is specified for the file and the cache file
                // mtime is older than it, the cache file is removed and
                // the processing comes here.
                //
                pagelist.Resize(size, false, (0 <= time ? false : true));
                is_truncate = true;
            }
        }

        // truncate cache(tmp) file
        if(is_truncate){
            if(0 != ftruncate(physical_fd, size) || 0 != fsync(physical_fd)){
                S3FS_PRN_ERR("ftruncate(%s) or fsync returned err(%d)", cachepath.c_str(), errno);
                fclose(pfile);
                pfile       = NULL;
                physical_fd = -1;
                inode       = 0;
                return (0 == errno ? -EIO : -errno);
            }
        }

        // reset cache stat file
        if(need_save_csf){
            CacheFileStat cfstat(path.c_str());
            if(!pagelist.Serialize(cfstat, true, inode)){
                S3FS_PRN_WARN("failed to save cache stat file(%s), but continue...", path.c_str());
            }
        }

        // set original headers and size in it.
        if(pmeta){
            orgmeta      = *pmeta;
            size_orgmeta = get_size(orgmeta);
        }else{
            orgmeta.clear();
            size_orgmeta = 0;
        }

        // set mtime and ctime(set "x-oss-meta-mtime" and "x-oss-meta-ctime" in orgmeta)
        if(-1 != time){
            struct timespec ts = {time, 0};
            if(0 != SetMCtime(ts, ts, /*lock_already_held=*/ true)){
                S3FS_PRN_ERR("failed to set mtime. errno(%d)", errno);
                fclose(pfile);
                pfile       = NULL;
                physical_fd = -1;
                inode       = 0;
                return (0 == errno ? -EIO : -errno);
            }
        }
    }

    // create new pseudo fd, and set it to map
    PseudoFdInfo*   ppseudoinfo = new PseudoFdInfo(physical_fd, flags, is_direct_read, path, size_orgmeta);
    int             pseudo_fd   = ppseudoinfo->GetPseudoFd();
    pseudo_fd_map[pseudo_fd]    = ppseudoinfo;

    return pseudo_fd;
}

// [NOTE]
// This method is called for only nocopyapi functions.
// So we do not check disk space for this option mode, if there is no enough
// disk space this method will be failed.
//
bool FdEntity::LoadAll(int fd, headers_t* pmeta, off_t* size, bool force_load)
{
    AutoLock auto_lock(&fdent_lock);

    S3FS_PRN_INFO3("[path=%s][pseudo_fd=%d][physical_fd=%d]", path.c_str(), fd, physical_fd);

    if(-1 == physical_fd || !FindPseudoFd(fd, true)){
        S3FS_PRN_ERR("pseudo_fd(%d) and physical_fd(%d) for path(%s) is not opened yet", fd, physical_fd, path.c_str());
        return false;
    }

    AutoLock auto_data_lock(&fdent_data_lock);

    if(force_load){
        SetAllStatusUnloaded();
    }
    //
    // TODO: possibly do background for delay loading
    //
    int result;
    if(0 != (result = Load(/*start=*/ 0, /*size=*/ 0, AutoLock::ALREADY_LOCKED))){
        S3FS_PRN_ERR("could not download, result(%d)", result);
        return false;
    }
    if(size){
        *size = pagelist.Size();
    }
    return true;
}

//
// Rename file path.
//
// This method sets the FdManager::fent map registration key to fentmapkey.
//
// [NOTE]
// This method changes the file path of FdEntity.
// Old file is deleted after linking to the new file path, and this works
// without problem because the file descriptor is not affected even if the
// cache file is open.
// The mirror file descriptor is also the same. The mirror file path does
// not need to be changed and will remain as it is.
//
bool FdEntity::RenamePath(const std::string& newpath, std::string& fentmapkey)
{
    if(!cachepath.empty()){
        // has cache path

        // make new cache path
        std::string newcachepath;
        if(!FdManager::MakeCachePath(newpath.c_str(), newcachepath, true)){
          S3FS_PRN_ERR("failed to make cache path for object(%s).", newpath.c_str());
          return false;
        }

        // rename cache file
        if(-1 == rename(cachepath.c_str(), newcachepath.c_str())){
          S3FS_PRN_ERR("failed to rename old cache path(%s) to new cache path(%s) by errno(%d).", cachepath.c_str(), newcachepath.c_str(), errno);
          return false;
        }

        // link and unlink cache file stat
        if(!CacheFileStat::RenameCacheFileStat(path.c_str(), newpath.c_str())){
          S3FS_PRN_ERR("failed to rename cache file stat(%s to %s).", path.c_str(), newpath.c_str());
          return false;
        }
        fentmapkey = newpath;
        cachepath  = newcachepath;

    }else{
        // does not have cache path
        fentmapkey.erase();
        FdManager::MakeRandomTempPath(newpath.c_str(), fentmapkey);
    }
    // set new path
    path = newpath;

    return true;
}

bool FdEntity::IsModified() const
{
    AutoLock auto_data_lock(const_cast<pthread_mutex_t *>(&fdent_data_lock));
    return pagelist.IsModified();
}

bool FdEntity::GetStats(struct stat& st, bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);
    if(-1 == physical_fd){
        return false;
    }

    memset(&st, 0, sizeof(struct stat)); 
    if(-1 == fstat(physical_fd, &st)){
        S3FS_PRN_ERR("fstat failed. errno(%d)", errno);
        return false;
    }
    return true;
}

int FdEntity::SetCtime(struct timespec time, bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    S3FS_PRN_INFO3("[path=%s][physical_fd=%d][time=%lld]", path.c_str(), physical_fd, static_cast<long long>(time.tv_sec));

    if(-1 == time.tv_sec){
        return 0;
    }
    orgmeta["x-oss-meta-ctime"] = str(time);
    return 0;
}

int FdEntity::SetAtime(struct timespec time, bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    S3FS_PRN_INFO3("[path=%s][physical_fd=%d][time=%lld]", path.c_str(), physical_fd, static_cast<long long>(time.tv_sec));

    if(-1 == time.tv_sec){
        return 0;
    }
    orgmeta["x-oss-meta-atime"] = str(time);
    return 0;
}

// [NOTE]
// This method updates mtime as well as ctime.
//
int FdEntity::SetMCtime(struct timespec mtime, struct timespec ctime, bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    S3FS_PRN_INFO3("[path=%s][physical_fd=%d][mtime=%lld][ctime=%lld]", path.c_str(), physical_fd, static_cast<long long>(mtime.tv_sec), static_cast<long long>(ctime.tv_sec));

    if(mtime.tv_sec < 0 || ctime.tv_sec < 0){
        return 0;
    }

    if(-1 != physical_fd){
        struct timeval tv[2];
        tv[0].tv_sec = mtime.tv_sec;
        tv[0].tv_usec = mtime.tv_nsec / 1000;
        tv[1].tv_sec = ctime.tv_sec;
        tv[1].tv_usec = ctime.tv_nsec / 1000;
        if(-1 == futimes(physical_fd, tv)){
            S3FS_PRN_ERR("futimes failed. errno(%d)", errno);
            return -errno;
        }
    }else if(!cachepath.empty()){
        // not opened file yet.
        struct timeval n_time[2];
        n_time[0].tv_sec  = ctime.tv_sec;
        n_time[0].tv_usec = ctime.tv_nsec / 1000;
        n_time[1].tv_sec  = mtime.tv_sec;
        n_time[1].tv_usec = mtime.tv_nsec / 1000;
        if(-1 == utimes(cachepath.c_str(), n_time)){
            S3FS_PRN_ERR("utime failed. errno(%d)", errno);
            return -errno;
        }
    }
    orgmeta["x-oss-meta-mtime"] = str(mtime);
    orgmeta["x-oss-meta-ctime"] = str(ctime);

    return 0;
}

bool FdEntity::UpdateCtime()
{
    AutoLock auto_lock(&fdent_lock);
    struct stat st;
    if(!GetStats(st, /*lock_already_held=*/ true)){
        return false;
    }
    orgmeta["x-oss-meta-ctime"] = str(st.st_ctime);
    return true;
}

bool FdEntity::UpdateAtime()
{
    AutoLock auto_lock(&fdent_lock);
    struct stat st;
    if(!GetStats(st, /*lock_already_held=*/ true)){
        return false;
    }
    orgmeta["x-oss-meta-atime"] = str(st.st_atime);
    return true;
}

bool FdEntity::UpdateMtime(bool clear_holding_mtime)
{
    AutoLock auto_lock(&fdent_lock);

    if(0 <= holding_mtime.tv_sec){
        // [NOTE]
        // This conditional statement is very special.
        // If you copy a file with "cp -p" etc., utimens or chown will be
        // called after opening the file, after that call to write, flush.
        // If normally utimens are not called(cases like "cp" only), mtime
        // should be updated at the file flush.
        // Here, check the holding_mtime value to prevent mtime from being
        // overwritten.
        //
        if(clear_holding_mtime){
            if(!ClearHoldingMtime(true)){
                return false;
            }
        }
    }else{
        struct stat st;
        if(!GetStats(st, /*lock_already_held=*/ true)){
            return false;
        }
        orgmeta["x-oss-meta-mtime"] = str(st.st_mtime);
    }
    return true;
}

bool FdEntity::SetHoldingMtime(struct timespec mtime, bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    if(mtime.tv_sec < 0){
        return false;
    }
    holding_mtime = mtime;
    return true;
}

bool FdEntity::ClearHoldingMtime(bool lock_already_held)
{
    AutoLock auto_lock(&fdent_lock, lock_already_held ? AutoLock::ALREADY_LOCKED : AutoLock::NONE);

    if(holding_mtime.tv_sec < 0){
        return false;
    }
    struct stat st;
    if(!GetStats(st, true)){
        return false;
    }
    if(-1 != physical_fd){
        struct timeval tv[2];
        tv[0].tv_sec = holding_mtime.tv_sec;
        tv[0].tv_usec = holding_mtime.tv_nsec / 1000;
#if defined(__APPLE__)
        tv[1].tv_sec = st.st_ctime;
        tv[1].tv_usec = st.st_ctimespec.tv_nsec / 1000;
#else
        tv[1].tv_sec = st.st_ctim.tv_sec;
        tv[1].tv_usec = st.st_ctim.tv_nsec / 1000;
#endif
        if(-1 == futimes(physical_fd, tv)){
            S3FS_PRN_ERR("futimes failed. errno(%d)", errno);
            return false;
        }
    }else if(!cachepath.empty()){
        // not opened file yet.
        struct timeval n_time[2];
#if defined(__APPLE__)
        n_time[0].tv_sec = st.st_ctime;
        n_time[0].tv_usec = st.st_ctimespec.tv_nsec / 1000;
#else
        n_time[0].tv_sec = st.st_ctime;
        n_time[0].tv_usec = st.st_ctim.tv_nsec / 1000;
#endif
        n_time[1].tv_sec = holding_mtime.tv_sec;
        n_time[1].tv_usec = holding_mtime.tv_nsec / 1000;
        if(-1 == utimes(cachepath.c_str(), n_time)){
            S3FS_PRN_ERR("utime failed. errno(%d)", errno);
            return false;
        }
    }
    holding_mtime.tv_sec = -1;
    holding_mtime.tv_nsec = 0;

    return true;
}

bool FdEntity::GetSize(off_t& size)
{
    AutoLock auto_lock(&fdent_lock);
    if(-1 == physical_fd){
        return false;
    }

    AutoLock auto_data_lock(&fdent_data_lock);
    size = pagelist.Size();
    return true;
}

bool FdEntity::GetXattr(std::string& xattr)
{
    AutoLock auto_lock(&fdent_lock);

    headers_t::const_iterator iter = orgmeta.find("x-oss-meta-xattr");
    if(iter == orgmeta.end()){
        return false;
    }
    xattr = iter->second;
    return true;
}

bool FdEntity::SetXattr(const std::string& xattr)
{
    AutoLock auto_lock(&fdent_lock);
    orgmeta["x-oss-meta-xattr"] = xattr;
    return true;
}

bool FdEntity::SetMode(mode_t mode)
{
    AutoLock auto_lock(&fdent_lock);
    orgmeta["x-oss-meta-mode"] = str(mode);
    return true;
}

bool FdEntity::SetUId(uid_t uid)
{
    AutoLock auto_lock(&fdent_lock);
    orgmeta["x-oss-meta-uid"] = str(uid);
    return true;
}

bool FdEntity::SetGId(gid_t gid)
{
    AutoLock auto_lock(&fdent_lock);
    orgmeta["x-oss-meta-gid"] = str(gid);
    return true;
}

bool FdEntity::SetContentType(const char* path)
{
    if(!path){
        return false;
    }
    AutoLock auto_lock(&fdent_lock);
    orgmeta["Content-Type"] = S3fsCurl::LookupMimeType(std::string(path));
    return true;
}

bool FdEntity::GetSymlinkAttr(std::string& type, std::string& symlink)
{
    AutoLock auto_lock(&fdent_lock);
    headers_t::const_iterator iter = orgmeta.find("x-oss-meta-symlink-target");
    if(iter == orgmeta.end()){
        return false;
    }    
    std::string value = (*iter).second;
    std::string::size_type pos = value.find(':', 0);
    if(std::string::npos != pos){
        type = value.substr(0, pos);
    }

    pos = value.find(':', pos + 1);
    if(std::string::npos != pos){
        symlink = urlDecode(value.substr(pos + 1));
    }

    return true;
}

bool FdEntity::SetSymlinkAttr(const std::string& type, const std::string& symlink)
{
    AutoLock auto_lock(&fdent_lock);
    orgmeta.erase("x-oss-meta-symlink-target");

    //the target format is: type:size:urlencode(symlink)
    if (!type.empty()) {
        std::string value = type + ":" + str(symlink.length()) + ":" + urlEncode(symlink);
        orgmeta["x-oss-meta-symlink-target"] = value;
    }
    return true;
}

bool FdEntity::SetAllStatus(bool is_loaded)
{
    S3FS_PRN_INFO3("[path=%s][physical_fd=%d][%s]", path.c_str(), physical_fd, is_loaded ? "loaded" : "unloaded");

    if(-1 == physical_fd){
        return false;
    }
    // [NOTE]
    // this method is only internal use, and calling after locking.
    // so do not lock now.
    //
    //AutoLock auto_lock(&fdent_lock);

    // get file size
    struct stat st;
    memset(&st, 0, sizeof(struct stat));
    if(-1 == fstat(physical_fd, &st)){
        S3FS_PRN_ERR("fstat is failed. errno(%d)", errno);
        return false;
    }
    // Reinit
    pagelist.Init(st.st_size, is_loaded, false);

    return true;
}

int FdEntity::LoadWithSizeInfo(off_t start, off_t size, AutoLock::Type type, uint64_t &loaded_size, bool is_modified_flag)
{
    loaded_size = 0;
    AutoLock auto_lock(&fdent_lock, type);

    S3FS_PRN_DBG("[path=%s][physical_fd=%d][offset=%lld][size=%lld]", path.c_str(), physical_fd, static_cast<long long int>(start), static_cast<long long int>(size));

    if(-1 == physical_fd){
        return -EBADF;
    }
    AutoLock auto_data_lock(&fdent_data_lock, type);

    int result = 0;

    // check loaded area & load
    fdpage_list_t unloaded_list;
    if(0 < pagelist.GetUnloadedPages(unloaded_list, start, size)){
        for(fdpage_list_t::iterator iter = unloaded_list.begin(); iter != unloaded_list.end(); ++iter){
            if(0 != size && start + size <= iter->offset){
                // reached end
                break;
            }
            // check loading size
            off_t need_load_size = 0;
            if(iter->offset < size_orgmeta){
                // original file size(on OSS) is smaller than request.
                need_load_size = (iter->next() <= size_orgmeta ? iter->bytes : (size_orgmeta - iter->offset));
            }

            // download
            if(S3fsCurl::GetMultipartSize() <= need_load_size && !nomultipart){
                // parallel request
                result = S3fsCurl::ParallelGetObjectRequest(path.c_str(), physical_fd, iter->offset, need_load_size);
            }else{
                // single request
                if(0 < need_load_size){
                    S3fsCurl s3fscurl;
                    result = s3fscurl.GetObjectRequest(path.c_str(), physical_fd, iter->offset, need_load_size);
                }else{
                    result = 0;
                }
          }
          if(0 != result){
              break;
          }
          loaded_size += need_load_size;
          // Set loaded flag
          pagelist.SetPageLoadedStatus(iter->offset, iter->bytes, (is_modified_flag ? PageList::PAGE_LOAD_MODIFIED : PageList::PAGE_LOADED));
        }
        PageList::FreeList(unloaded_list);
    }
    return result;
}

// [NOTE]
// At no disk space for caching object.
// This method is downloading by dividing an object of the specified range
// and uploading by multipart after finishing downloading it.
//
// [NOTICE]
// Need to lock before calling this method.
//
int FdEntity::NoCacheLoadAndPost(PseudoFdInfo* pseudo_obj, off_t start, off_t size)
{
    int result = 0;

    S3FS_PRN_INFO3("[path=%s][physical_fd=%d][offset=%lld][size=%lld]", path.c_str(), physical_fd, static_cast<long long int>(start), static_cast<long long int>(size));

    if(!pseudo_obj){
        S3FS_PRN_ERR("Pseudo object is NULL.");
        return -EIO;
    }

    if(-1 == physical_fd){
        return -EBADF;
    }

    // [NOTE]
    // This method calling means that the cache file is never used no more.
    //
    if(!cachepath.empty()){
        // remove cache files(and cache stat file)
        FdManager::DeleteCacheFile(path.c_str());
        // cache file path does not use no more.
        cachepath.erase();
        mirrorpath.erase();
    }

    // Change entity key in manager mapping
    FdManager::get()->ChangeEntityToTempPath(this, path.c_str());

    // open temporary file
    FILE* ptmpfp;
    int   tmpfd;
    if(NULL == (ptmpfp = FdManager::MakeTempFile()) || -1 ==(tmpfd = fileno(ptmpfp))){
        S3FS_PRN_ERR("failed to open temporary file by errno(%d)", errno);
        if(ptmpfp){
            fclose(ptmpfp);
        }
        return (0 == errno ? -EIO : -errno);
    }

    // loop uploading by multipart
    for(fdpage_list_t::iterator iter = pagelist.pages.begin(); iter != pagelist.pages.end(); ++iter){
        if(iter->end() < start){
            continue;
        }
        if(0 != size && start + size <= iter->offset){
            break;
        }
        // download each multipart size(default 10MB) in unit
        for(off_t oneread = 0, totalread = (iter->offset < start ? start : 0); totalread < static_cast<off_t>(iter->bytes); totalread += oneread){
            int   upload_fd = physical_fd;
            off_t offset    = iter->offset + totalread;
            oneread         = std::min(static_cast<off_t>(iter->bytes) - totalread, S3fsCurl::GetMultipartSize());

            // check rest size is over minimum part size
            //
            // [NOTE]
            // If the final part size is smaller than 5MB, it is not allowed by OSS API.
            // For this case, if the previous part of the final part is not over 5GB,
            // we incorporate the final part to the previous part. If the previous part
            // is over 5GB, we want to even out the last part and the previous part.
            //
            if((iter->bytes - totalread - oneread) < MIN_MULTIPART_SIZE){
                if(FIVE_GB < iter->bytes - totalread){
                    oneread = (iter->bytes - totalread) / 2;
                }else{
                    oneread = iter->bytes - totalread;
                }
            }

            if(!iter->loaded){
                //
                // loading or initializing
                //
                upload_fd = tmpfd;

                // load offset & size
                size_t need_load_size = 0;
                if(size_orgmeta <= offset){
                    // all area is over of original size
                    need_load_size      = 0;
                }else{
                    if(size_orgmeta < (offset + oneread)){
                        // original file size(on OSS) is smaller than request.
                        need_load_size    = size_orgmeta - offset;
                    }else{
                        need_load_size    = oneread;
                    }
                }
                size_t over_size      = oneread - need_load_size;

                // [NOTE]
                // truncate file to zero and set length to part offset + size
                // after this, file length is (offset + size), but file does not use any disk space.
                //
                if(-1 == ftruncate(tmpfd, 0) || -1 == ftruncate(tmpfd, (offset + oneread))){
                    S3FS_PRN_ERR("failed to truncate temporary file(physical_fd=%d).", tmpfd);
                    result = -EIO;
                    break;
                }

                // single area get request
                if(0 < need_load_size){
                    S3fsCurl s3fscurl;
                    if(0 != (result = s3fscurl.GetObjectRequest(path.c_str(), tmpfd, offset, oneread))){
                        S3FS_PRN_ERR("failed to get object(start=%lld, size=%lld) for file(physical_fd=%d).", static_cast<long long int>(offset), static_cast<long long int>(oneread), tmpfd);
                        break;
                    }
                }
                // initialize fd without loading
                if(0 < over_size){
                    if(0 != (result = FdEntity::FillFile(tmpfd, 0, over_size, offset + need_load_size))){
                        S3FS_PRN_ERR("failed to fill rest bytes for physical_fd(%d). errno(%d)", tmpfd, result);
                        break;
                    }
                }
            }else{
                // already loaded area
            }
            // single area upload by multipart post
            if(0 != (result = NoCacheMultipartPost(pseudo_obj, upload_fd, offset, oneread))){
                S3FS_PRN_ERR("failed to multipart post(start=%lld, size=%lld) for file(physical_fd=%d).", static_cast<long long int>(offset), static_cast<long long int>(oneread), upload_fd);
                break;
            }
        }
        if(0 != result){
            break;
        }

        // set loaded flag
        if(!iter->loaded){
            if(iter->offset < start){
                fdpage page(iter->offset, start - iter->offset, iter->loaded, false);
                iter->bytes -= (start - iter->offset);
                iter->offset = start;
                pagelist.pages.insert(iter, page);
            }
            if(0 != size && start + size < iter->next()){
                fdpage page(iter->offset, start + size - iter->offset, true, false);
                iter->bytes -= (start + size - iter->offset);
                iter->offset = start + size;
                pagelist.pages.insert(iter, page);
            }else{
                iter->loaded   = true;
                iter->modified = false;
            }
        }
    }
    if(0 == result){
        // compress pagelist
        pagelist.Compress();

        // fd data do empty
        if(-1 == ftruncate(physical_fd, 0)){
            S3FS_PRN_ERR("failed to truncate file(physical_fd=%d), but continue...", physical_fd);
        }
    }

    // close temporary
    fclose(ptmpfp);

    return result;
}

// [NOTE]
// At no disk space for caching object.
// This method is starting multipart uploading.
//
int FdEntity::NoCachePreMultipartPost(PseudoFdInfo* pseudo_obj)
{
    if(!pseudo_obj){
        S3FS_PRN_ERR("Internal error, pseudo fd object pointer is null.");
        return -EIO;
    }

    // initialize multipart upload values
    pseudo_obj->ClearUploadInfo(true);

    S3fsCurl    s3fscurl(true);
    std::string upload_id;
    int         result;
    if(0 != (result = s3fscurl.PreMultipartPostRequest(path.c_str(), orgmeta, upload_id, false))){
        return result;
    }
    s3fscurl.DestroyCurlHandle();

    // Clear the dirty flag, because the meta data is updated.
    is_meta_pending = false;

    // reset upload_id
    if(!pseudo_obj->InitialUploadInfo(upload_id)){
        return -EIO;
    }
    return 0;
}

// [NOTE]
// At no disk space for caching object.
// This method is uploading one part of multipart.
//
int FdEntity::NoCacheMultipartPost(PseudoFdInfo* pseudo_obj, int tgfd, off_t start, off_t size)
{
    if(-1 == tgfd || !pseudo_obj || !pseudo_obj->IsUploading()){
        S3FS_PRN_ERR("Need to initialize for multipart post.");
        return -EIO;
    }

    // get upload id
    std::string upload_id;
    if(!pseudo_obj->GetUploadId(upload_id)){
        return -EIO;
    }

    // append new part and get it's etag string pointer
    etagpair* petagpair = NULL;
    if(!pseudo_obj->AppendUploadPart(start, size, false, &petagpair)){
        return -EIO;
    }

    S3fsCurl s3fscurl(true);
    return s3fscurl.MultipartUploadRequest(upload_id, path.c_str(), tgfd, start, size, petagpair);
}

// [NOTE]
// At no disk space for caching object.
// This method is finishing multipart uploading.
//
int FdEntity::NoCacheCompleteMultipartPost(PseudoFdInfo* pseudo_obj)
{
    etaglist_t etaglist;
    if(!pseudo_obj || !pseudo_obj->IsUploading() || !pseudo_obj->GetEtaglist(etaglist)){
        S3FS_PRN_ERR("There is no upload id or etag list.");
        return -EIO;
    }

    // get upload id
    std::string upload_id;
    if(!pseudo_obj->GetUploadId(upload_id)){
        return -EIO;
    }

    S3fsCurl s3fscurl(true);
    int      result;
    if(0 != (result = s3fscurl.CompleteMultipartPostRequest(path.c_str(), upload_id, etaglist))){
        return result;
    }
    s3fscurl.DestroyCurlHandle();

    // clear multipart upload info
    pseudo_obj->ClearUploadInfo(false);

    return 0;
}

off_t FdEntity::BytesModified()
{
    AutoLock auto_lock(&fdent_data_lock);
    return pagelist.BytesModified();
}

// [NOTE]
// There are conditions that allow you to perform multipart uploads.
// 
// According to the AWS spec:
//  - 1 to 10,000 parts are allowed
//  - minimum size of parts is 5MB (except for the last part)
// 
// For example, if you set the minimum part size to 5MB, you can upload
// a maximum (5 * 10,000)MB file.
// The part size can be changed in MB units, then the maximum file size
// that can be handled can be further increased.
// Files smaller than the minimum part size will not be multipart uploaded,
// but will be uploaded as single part(normally).
//
int FdEntity::RowFlush(int fd, const char* tpath, bool force_sync)
{
    S3FS_PRN_INFO3("[tpath=%s][path=%s][pseudo_fd=%d][physical_fd=%d]", SAFESTRPTR(tpath), path.c_str(), fd, physical_fd);

    if(-1 == physical_fd){
        return -EBADF;
    }

    AutoLock auto_lock(&fdent_lock);

    // check pseudo fd and its flag
    fdinfo_map_t::iterator miter = pseudo_fd_map.find(fd);
    if(pseudo_fd_map.end() == miter || NULL == miter->second){
        return -EBADF;
    }
    if(!miter->second->Writable() && !(miter->second->GetFlags() & O_CREAT)){
        // If the entity is opened read-only, it will end normally without updating.
        return 0;
    }
    PseudoFdInfo* pseudo_obj = miter->second;

    AutoLock auto_lock2(&fdent_data_lock);

    if(!force_sync && !pagelist.IsModified()){
        // nothing to update.
        return 0;
    }

    int result;
    if(nomultipart){
        // No multipart upload
        result = RowFlushNoMultipart(pseudo_obj, tpath);
    }else if(FdEntity::mixmultipart){
        // Mix multipart upload
        result = RowFlushMixMultipart(pseudo_obj, tpath);
    }else{
        // Normal multipart upload
        result = RowFlushMultipart(pseudo_obj, tpath);
    }

    // [NOTE]
    // if something went wrong, so if you are using a cache file,
    // the cache file may not be correct. So delete cache files.
    //
    if(0 != result && !cachepath.empty()){
        FdManager::DeleteCacheFile(tpath);
    }

    return result;
}

// [NOTE]
// Both fdent_lock and fdent_data_lock must be locked before calling.
//
int FdEntity::RowFlushNoMultipart(PseudoFdInfo* pseudo_obj, const char* tpath)
{
    S3FS_PRN_INFO3("[tpath=%s][path=%s][pseudo_fd=%d][physical_fd=%d]", SAFESTRPTR(tpath), path.c_str(), (pseudo_obj ? pseudo_obj->GetPseudoFd() : -1), physical_fd);

    if(-1 == physical_fd || !pseudo_obj){
        return -EBADF;
    }

    if(pseudo_obj->IsUploading()){
        S3FS_PRN_ERR("Why uploading now, even though ossfs is No Multipart uploading mode.");
        return -EBADF;
    }

    int         result;
    std::string tmppath    = path;
    headers_t   tmporgmeta = orgmeta;

    // If there is no loading all of the area, loading all area.
    off_t restsize = pagelist.GetTotalUnloadedPageSize();
    if(0 < restsize){
        // check disk space
        if(!ReserveDiskSpace(restsize)){
            // no enough disk space
            S3FS_PRN_WARN("Not enough local storage to flush: [path=%s][pseudo_fd=%d][physical_fd=%d]", path.c_str(), pseudo_obj->GetPseudoFd(), physical_fd);
            return -ENOSPC;   // No space left on device
        }
    }
    FdManager::FreeReservedDiskSpace(restsize);

    // Always load all uninitialized area
    if(0 != (result = Load(/*start=*/ 0, /*size=*/ 0, AutoLock::ALREADY_LOCKED))){
        S3FS_PRN_ERR("failed to upload all area(errno=%d)", result);
        return result;
    }

    // check size
    if(pagelist.Size() > MAX_MULTIPART_CNT * S3fsCurl::GetMultipartSize()){
        S3FS_PRN_ERR("Part count exceeds %d.  Increase multipart size and try again.", MAX_MULTIPART_CNT);
        return -EFBIG;
    }

    // backup upload file size
    struct stat st;
    memset(&st, 0, sizeof(struct stat));
    if(-1 == fstat(physical_fd, &st)){
        S3FS_PRN_ERR("fstat is failed by errno(%d), but continue...", errno);
    }

    S3fsCurl s3fscurl(true);
    result = s3fscurl.PutRequest(tpath ? tpath : tmppath.c_str(), tmporgmeta, physical_fd);

    // reset uploaded file size
    size_orgmeta = st.st_size;

    pseudo_obj->ClearUntreated();

    if(0 == result){
        pagelist.ClearAllModified();
    }
    return result;
}

// [NOTE]
// Both fdent_lock and fdent_data_lock must be locked before calling.
//
int FdEntity::RowFlushMultipart(PseudoFdInfo* pseudo_obj, const char* tpath)
{
    S3FS_PRN_INFO3("[tpath=%s][path=%s][pseudo_fd=%d][physical_fd=%d]", SAFESTRPTR(tpath), path.c_str(), (pseudo_obj ? pseudo_obj->GetPseudoFd() : -1), physical_fd);

    if(-1 == physical_fd || !pseudo_obj){
        return -EBADF;
    }

    int result = 0;

    if(!pseudo_obj->IsUploading()){
        // Start uploading

        // If there is no loading all of the area, loading all area.
        off_t restsize = pagelist.GetTotalUnloadedPageSize();

        // Check rest size and free disk space
        if(0 < restsize && !ReserveDiskSpace(restsize)){
           // no enough disk space
           if(0 != (result = NoCachePreMultipartPost(pseudo_obj))){
               S3FS_PRN_ERR("failed to switch multipart uploading with no cache(errno=%d)", result);
               return result;
           }
           // upload all by multipart uploading
           if(0 != (result = NoCacheLoadAndPost(pseudo_obj))){
               S3FS_PRN_ERR("failed to upload all area by multipart uploading(errno=%d)", result);
               return result;
           }

        }else{
            // enough disk space or no rest size
            std::string tmppath    = path;
            headers_t   tmporgmeta = orgmeta;

            FdManager::FreeReservedDiskSpace(restsize);

            // Load all uninitialized area(no mix multipart uploading)
            if(0 != (result = Load(/*start=*/ 0, /*size=*/ 0, AutoLock::ALREADY_LOCKED))){
                S3FS_PRN_ERR("failed to upload all area(errno=%d)", result);
                return result;
            }

            // backup upload file size
            struct stat st;
            memset(&st, 0, sizeof(struct stat));
            if(-1 == fstat(physical_fd, &st)){
                S3FS_PRN_ERR("fstat is failed by errno(%d), but continue...", errno);
            }

            if(pagelist.Size() > MAX_MULTIPART_CNT * S3fsCurl::GetMultipartSize()){
                S3FS_PRN_ERR("Part count exceeds %d.  Increase multipart size and try again.", MAX_MULTIPART_CNT);
                return -EFBIG;

            }else if(pagelist.Size() >= S3fsCurl::GetMultipartSize()){
                // multipart uploading
                result = S3fsCurl::ParallelMultipartUploadRequest(tpath ? tpath : tmppath.c_str(), tmporgmeta, physical_fd);

            }else{
                // normal uploading (too small part size)
                S3fsCurl s3fscurl(true);
                result = s3fscurl.PutRequest(tpath ? tpath : tmppath.c_str(), tmporgmeta, physical_fd);
            }

            // reset uploaded file size
            size_orgmeta = st.st_size;
       }
        pseudo_obj->ClearUntreated();

    }else{
        // Already start uploading

        // upload rest data
        off_t untreated_start = 0;
        off_t untreated_size  = 0;
        if(pseudo_obj->GetLastUntreated(untreated_start, untreated_size, S3fsCurl::GetMultipartSize(), 0) && 0 < untreated_size){
            if(0 != (result = NoCacheMultipartPost(pseudo_obj, physical_fd, untreated_start, untreated_size))){
                S3FS_PRN_ERR("failed to multipart post(start=%lld, size=%lld) for file(physical_fd=%d).", static_cast<long long int>(untreated_start), static_cast<long long int>(untreated_size), physical_fd);
                return result;
            }
            pseudo_obj->ClearUntreated(untreated_start, untreated_size);
        }
        // complete multipart uploading.
        if(0 != (result = NoCacheCompleteMultipartPost(pseudo_obj))){
            S3FS_PRN_ERR("failed to complete(finish) multipart post for file(physical_fd=%d).", physical_fd);
            return result;
        }
        // truncate file to zero
        if(-1 == ftruncate(physical_fd, 0)){
            // So the file has already been removed, skip error.
            S3FS_PRN_ERR("failed to truncate file(physical_fd=%d) to zero, but continue...", physical_fd);
        }
        // put pending headers
        if(0 != (result = UploadPendingMeta())){
            return result;
        }
    }

    if(0 == result){
        pagelist.ClearAllModified();
        is_meta_pending = false;
    }
    return result;
}

// [NOTE]
// Both fdent_lock and fdent_data_lock must be locked before calling.
//
int FdEntity::RowFlushMixMultipart(PseudoFdInfo* pseudo_obj, const char* tpath)
{
    S3FS_PRN_INFO3("[tpath=%s][path=%s][pseudo_fd=%d][physical_fd=%d]", SAFESTRPTR(tpath), path.c_str(), (pseudo_obj ? pseudo_obj->GetPseudoFd() : -1), physical_fd);

    if(-1 == physical_fd || !pseudo_obj){
        return -EBADF;
    }

    int result = 0;

    if(!pseudo_obj->IsUploading()){
        // Start uploading

        // If there is no loading all of the area, loading all area.
        off_t restsize = pagelist.GetTotalUnloadedPageSize(/* start */ 0, /* size = all */ 0, MIN_MULTIPART_SIZE);

        // Check rest size and free disk space
        if(0 < restsize && !ReserveDiskSpace(restsize)){
           // no enough disk space
           if(0 != (result = NoCachePreMultipartPost(pseudo_obj))){
               S3FS_PRN_ERR("failed to switch multipart uploading with no cache(errno=%d)", result);
               return result;
           }
           // upload all by multipart uploading
           if(0 != (result = NoCacheLoadAndPost(pseudo_obj))){
               S3FS_PRN_ERR("failed to upload all area by multipart uploading(errno=%d)", result);
               return result;
           }

        }else{
            // enough disk space or no rest size
            std::string tmppath    = path;
            headers_t   tmporgmeta = orgmeta;

            FdManager::FreeReservedDiskSpace(restsize);

            // backup upload file size
            struct stat st;
            memset(&st, 0, sizeof(struct stat));
            if(-1 == fstat(physical_fd, &st)){
                S3FS_PRN_ERR("fstat is failed by errno(%d), but continue...", errno);
            }

            if(pagelist.Size() > MAX_MULTIPART_CNT * S3fsCurl::GetMultipartSize()){
                S3FS_PRN_ERR("Part count exceeds %d.  Increase multipart size and try again.", MAX_MULTIPART_CNT);
                return -EFBIG;

            }else if(pagelist.Size() >= S3fsCurl::GetMultipartSize()){
                // mix multipart uploading

                // This is to ensure that each part is 5MB or more.
                // If the part is less than 5MB, download it.
                fdpage_list_t dlpages;
                fdpage_list_t mixuppages;
                if(!pagelist.GetPageListsForMultipartUpload(dlpages, mixuppages, S3fsCurl::GetMultipartSize())){
                    S3FS_PRN_ERR("something error occurred during getting download pagelist.");
                    return -1;
                }

                // [TODO] should use parallel downloading
                //
                for(fdpage_list_t::const_iterator iter = dlpages.begin(); iter != dlpages.end(); ++iter){
                    if(0 != (result = Load(iter->offset, iter->bytes, AutoLock::ALREADY_LOCKED, /*is_modified_flag=*/ true))){  // set loaded and modified flag
                        S3FS_PRN_ERR("failed to get parts(start=%lld, size=%lld) before uploading.", static_cast<long long int>(iter->offset), static_cast<long long int>(iter->bytes));
                        return result;
                    }
                }

                // multipart uploading with copy api
                result = S3fsCurl::ParallelMixMultipartUploadRequest(tpath ? tpath : tmppath.c_str(), tmporgmeta, physical_fd, mixuppages);

            }else{
                // normal uploading (too small part size)

                // If there are unloaded pages, they are loaded at here.
                if(0 != (result = Load(/*start=*/ 0, /*size=*/ 0, AutoLock::ALREADY_LOCKED))){
                    S3FS_PRN_ERR("failed to load parts before uploading object(%d)", result);
                    return result;
                }

                S3fsCurl s3fscurl(true);
                result = s3fscurl.PutRequest(tpath ? tpath : tmppath.c_str(), tmporgmeta, physical_fd);
            }

            // reset uploaded file size
            size_orgmeta = st.st_size;
        }
        pseudo_obj->ClearUntreated();

    }else{
        // Already start uploading

        // upload rest data
        off_t untreated_start = 0;
        off_t untreated_size  = 0;
        if(pseudo_obj->GetLastUntreated(untreated_start, untreated_size, S3fsCurl::GetMultipartSize(), 0) && 0 < untreated_size){
            if(0 != (result = NoCacheMultipartPost(pseudo_obj, physical_fd, untreated_start, untreated_size))){
                S3FS_PRN_ERR("failed to multipart post(start=%lld, size=%lld) for file(physical_fd=%d).", static_cast<long long int>(untreated_start), static_cast<long long int>(untreated_size), physical_fd);
                return result;
            }
            pseudo_obj->ClearUntreated(untreated_start, untreated_size);
	    }
        // complete multipart uploading.
        if(0 != (result = NoCacheCompleteMultipartPost(pseudo_obj))){
            S3FS_PRN_ERR("failed to complete(finish) multipart post for file(physical_fd=%d).", physical_fd);
            return result;
        }
        // truncate file to zero
        if(-1 == ftruncate(physical_fd, 0)){
            // So the file has already been removed, skip error.
            S3FS_PRN_ERR("failed to truncate file(physical_fd=%d) to zero, but continue...", physical_fd);
        }
        // put pending headers
        if(0 != (result = UploadPendingMeta())){
            return result;
        }
    }

    if(0 == result){
        pagelist.ClearAllModified();
        is_meta_pending = false;
    }
    return result;
}

// [NOTICE]
// Need to lock before calling this method.
bool FdEntity::ReserveDiskSpace(off_t size)
{
    if(FdManager::ReserveDiskSpace(size)){
        return true;
    }

    if(!pagelist.IsModified()){
        // try to clear all cache for this fd.
        pagelist.Init(pagelist.Size(), false, false);
        if(-1 == ftruncate(physical_fd, 0) || -1 == ftruncate(physical_fd, pagelist.Size())){
            S3FS_PRN_ERR("failed to truncate temporary file(physical_fd=%d).", physical_fd);
            return false;
        }

        if(FdManager::ReserveDiskSpace(size)){
            return true;
        }
    }

    FdManager::get()->CleanupCacheDir();

    return FdManager::ReserveDiskSpace(size);
}

ssize_t FdEntity::Read(int fd, char* bytes, off_t start, size_t size, bool force_load)
{
    S3FS_PRN_DBG("[path=%s][pseudo_fd=%d][physical_fd=%d][offset=%lld][size=%zu]", path.c_str(), fd, physical_fd, static_cast<long long int>(start), size);

    PseudoFdInfo* pseudo_obj = NULL;
    if(-1 == physical_fd || NULL == (pseudo_obj = CheckPseudoFdFlags(fd, false))){
        S3FS_PRN_DBG("pseudo_fd(%d) to physical_fd(%d) for path(%s) is not opened or not readable", fd, physical_fd, path.c_str());
        return -EBADF;
    }

    AutoLock auto_lock(&fdent_lock);
    AutoLock auto_lock2(&fdent_data_lock);

    ssize_t rsize = 0;

    if(is_direct_read){
        if (DirectReader::GetDirectReadLocalFileCacheSize() == 0) {
            return pseudo_obj->DirectReadAndPrefetch(bytes, start, size);
        }

        // mix-direct-read mode
        // enter direct-read-and-prefetch when the total_downloaded_size >= the threshold
        if (pseudo_obj->GetLoadedSize() >= DirectReader::GetDirectReadLocalFileCacheSize()) {
            if (pagelist.IsPageLoaded(start, size)) {
                // Reading
                if(-1 == (rsize = pread(physical_fd, bytes, size, start))){
                    S3FS_PRN_ERR("pread failed. errno(%d)", errno);
                    return -errno;
                }
                
                return rsize;
            }

            S3FS_PRN_DBG("start direct read. total_loaded_size = %lld, offset = %lld", pseudo_obj->GetLoadedSize(), start);
            return pseudo_obj->DirectReadAndPrefetch(bytes, start, size);
        }
    }

    CheckAndFreeDiskCacheIfNeeded();

    if(force_load){
        pagelist.SetPageLoadedStatus(start, size, PageList::PAGE_NOT_LOAD_MODIFIED);
    }

    int result = 0;
    uint64_t downloaded_size = 0;
    bool read_from_oss_directly = false;
    // check disk space
    if(0 < pagelist.GetTotalUnloadedPageSize(start, size)){
        // load size(for prefetch)
        size_t load_size = size;
        if(start + static_cast<ssize_t>(size) < pagelist.Size()){
            ssize_t prefetch_max_size = std::max(static_cast<off_t>(size), S3fsCurl::GetMultipartSize() * S3fsCurl::GetMaxParallelCount());

            if(start + prefetch_max_size < pagelist.Size()){
                load_size = prefetch_max_size;
            }else{
                load_size = pagelist.Size() - start;
            }
        }

        if(!ReserveDiskSpace(load_size)){
            S3FS_PRN_WARN("could not reserve disk space for pre-fetch download");
            load_size = size;
            if(!ReserveDiskSpace(load_size)){
                S3FS_PRN_WARN("could not reserve disk space for pre-fetch download");
                read_from_oss_directly = true;
            }
        }

        if(!read_from_oss_directly) {
            if(0 < size){
                // Loading
                result = LoadWithSizeInfo(start, load_size, AutoLock::ALREADY_LOCKED, downloaded_size);
            }

            FdManager::FreeReservedDiskSpace(load_size);
            if(0 != result){
                S3FS_PRN_WARN("could not download. start(%lld), size(%zu), errno(%d)", static_cast<long long int>(start), size, result);
                read_from_oss_directly = true;
            } else {
                pseudo_obj->AddLoadedSize(downloaded_size);
            }
        }
    }

    if(read_from_oss_directly){
        // direct read from oss, but no prefetch
        S3FS_PRN_WARN("could not reserve disk space for download, direct read from cloud.");
        S3fsCurl s3fscurl;
        result = s3fscurl.GetObjectStreamRequest(path.c_str(), bytes, start, size, rsize);
        if(0 != result){
            S3FS_PRN_ERR("could not download. start(%lld), size(%zu), errno(%d)", static_cast<long long int>(start), size, result);
            return result;
        }
        return rsize;
    }

    // Reading
    if(-1 == (rsize = pread(physical_fd, bytes, size, start))){
        S3FS_PRN_ERR("pread failed. errno(%d)", errno);
        return -errno;
    }
    return rsize;
}

ssize_t FdEntity::Write(int fd, const char* bytes, off_t start, size_t size)
{
    S3FS_PRN_DBG("[path=%s][pseudo_fd=%d][physical_fd=%d][offset=%lld][size=%zu]", path.c_str(), fd, physical_fd, static_cast<long long int>(start), size);

    PseudoFdInfo* pseudo_obj = NULL;
    if(-1 == physical_fd || NULL == (pseudo_obj = CheckPseudoFdFlags(fd, false))){
        S3FS_PRN_ERR("pseudo_fd(%d) to physical_fd(%d) for path(%s) is not opened or not writable", fd, physical_fd, path.c_str());
        return -EBADF;
    }

    // check if not enough disk space left BEFORE locking fd
    if(FdManager::IsCacheDir() && !FdManager::IsSafeDiskSpace(NULL, size)){
        FdManager::get()->CleanupCacheDir();
    }
    AutoLock auto_lock(&fdent_lock);
    AutoLock auto_lock2(&fdent_data_lock);

    // check file size
    if(pagelist.Size() < start){
        // grow file size
        if(-1 == ftruncate(physical_fd, start)){
            S3FS_PRN_ERR("failed to truncate temporary file(physical_fd=%d).", physical_fd);
            return -errno;
        }
        // add new area
        pagelist.SetPageLoadedStatus(pagelist.Size(), start - pagelist.Size(), PageList::PAGE_MODIFIED);
    }

    ssize_t wsize;
    if(nomultipart){
        // No multipart upload
        wsize = WriteNoMultipart(pseudo_obj, bytes, start, size);
    }else if(FdEntity::mixmultipart){
        // Mix multipart upload
        wsize = WriteMixMultipart(pseudo_obj, bytes, start, size);
    }else{
        // Normal multipart upload
        wsize = WriteMultipart(pseudo_obj, bytes, start, size);
    }

    return wsize;
}

// [NOTE]
// Both fdent_lock and fdent_data_lock must be locked before calling.
//
ssize_t FdEntity::WriteNoMultipart(PseudoFdInfo* pseudo_obj, const char* bytes, off_t start, size_t size)
{
    S3FS_PRN_DBG("[path=%s][pseudo_fd=%d][physical_fd=%d][offset=%lld][size=%zu]", path.c_str(), (pseudo_obj ? pseudo_obj->GetPseudoFd() : -1), physical_fd, static_cast<long long int>(start), size);

    if(-1 == physical_fd || !pseudo_obj){
        S3FS_PRN_ERR("pseudo_fd(%d) to physical_fd(%d) for path(%s) is not opened or not writable", (pseudo_obj ? pseudo_obj->GetPseudoFd() : -1), physical_fd, path.c_str());
        return -EBADF;
    }

    int result = 0;

    if(pseudo_obj->IsUploading()){
        S3FS_PRN_ERR("Why uploading now, even though ossfs is No Multipart uploading mode.");
        return -EBADF;
    }

    // check disk space
    off_t restsize = pagelist.GetTotalUnloadedPageSize(0, start) + size;
    if(!ReserveDiskSpace(restsize)){
        // no enough disk space
        S3FS_PRN_WARN("Not enough local storage to cache write request: [path=%s][physical_fd=%d][offset=%lld][size=%zu]", path.c_str(), physical_fd, static_cast<long long int>(start), size);
        return -ENOSPC;   // No space left on device
    }

    // Load uninitialized area which starts from 0 to (start + size) before writing.
    if(0 < start){
        result = Load(0, start, AutoLock::ALREADY_LOCKED);
    }

    FdManager::FreeReservedDiskSpace(restsize);
    if(0 != result){
        S3FS_PRN_ERR("failed to load uninitialized area before writing(errno=%d)", result);
        return result;
    }

    // Writing
    ssize_t wsize;
    if(-1 == (wsize = pwrite(physical_fd, bytes, size, start))){
        S3FS_PRN_ERR("pwrite failed. errno(%d)", errno);
        return -errno;
    }
    if(0 < wsize){
        pagelist.SetPageLoadedStatus(start, wsize, PageList::PAGE_LOAD_MODIFIED);
        pseudo_obj->AddUntreated(start, wsize);
    }

    // Load uninitialized area which starts from (start + size) to EOF after writing.
    if(pagelist.Size() > start + static_cast<off_t>(size)){
        result = Load(start + size, pagelist.Size(), AutoLock::ALREADY_LOCKED);
        if(0 != result){
            S3FS_PRN_ERR("failed to load uninitialized area after writing(errno=%d)", result);
            return result;
        }
    }

    return wsize;
}

// [NOTE]
// Both fdent_lock and fdent_data_lock must be locked before calling.
//
ssize_t FdEntity::WriteMultipart(PseudoFdInfo* pseudo_obj, const char* bytes, off_t start, size_t size)
{
    S3FS_PRN_DBG("[path=%s][pseudo_fd=%d][physical_fd=%d][offset=%lld][size=%zu]", path.c_str(), (pseudo_obj ? pseudo_obj->GetPseudoFd() : -1), physical_fd, static_cast<long long int>(start), size);

    if(-1 == physical_fd || !pseudo_obj){
        S3FS_PRN_ERR("pseudo_fd(%d) to physical_fd(%d) for path(%s) is not opened or not writable", (pseudo_obj ? pseudo_obj->GetPseudoFd() : -1), physical_fd, path.c_str());
        return -EBADF;
    }

    int result = 0;

    if(!pseudo_obj->IsUploading()){
        // check disk space
        off_t restsize = pagelist.GetTotalUnloadedPageSize(0, start) + size;
        if(ReserveDiskSpace(restsize)){
            // enough disk space

            // Load uninitialized area which starts from 0 to (start + size) before writing.
            if(0 < start){
                result = Load(0, start, AutoLock::ALREADY_LOCKED);
            }

            FdManager::FreeReservedDiskSpace(restsize);
            if(0 != result){
                S3FS_PRN_ERR("failed to load uninitialized area before writing(errno=%d)", result);
                return result;
            }
        }else{
            // no enough disk space
            if((start + static_cast<off_t>(size)) <= S3fsCurl::GetMultipartSize()){
                S3FS_PRN_WARN("Not enough local storage to cache write request till multipart upload can start: [path=%s][physical_fd=%d][offset=%lld][size=%zu]", path.c_str(), physical_fd, static_cast<long long int>(start), size);
                return -ENOSPC;   // No space left on device
            }
            if(0 != (result = NoCachePreMultipartPost(pseudo_obj))){
                S3FS_PRN_ERR("failed to switch multipart uploading with no cache(errno=%d)", result);
                return result;
            }
            // start multipart uploading
            if(0 != (result = NoCacheLoadAndPost(pseudo_obj, 0, start))){
                S3FS_PRN_ERR("failed to load uninitialized area and multipart uploading it(errno=%d)", result);
                return result;
            }

            pseudo_obj->ClearUntreated();
        }
    }else{
        // already start multipart uploading
    }

    // Writing
    ssize_t wsize;
    if(-1 == (wsize = pwrite(physical_fd, bytes, size, start))){
        S3FS_PRN_ERR("pwrite failed. errno(%d)", errno);
        return -errno;
    }
    if(0 < wsize){
        pagelist.SetPageLoadedStatus(start, wsize, PageList::PAGE_LOAD_MODIFIED);
        pseudo_obj->AddUntreated(start, wsize);
    }

    // Load uninitialized area which starts from (start + size) to EOF after writing.
    if(pagelist.Size() > start + static_cast<off_t>(size)){
        result = Load(start + size, pagelist.Size(), AutoLock::ALREADY_LOCKED);
        if(0 != result){
            S3FS_PRN_ERR("failed to load uninitialized area after writing(errno=%d)", result);
            return result;
        }
    }

    // check multipart uploading
    if(pseudo_obj->IsUploading()){
        // get last untreated part(maximum size is multipart size)
        off_t untreated_start = 0;
        off_t untreated_size  = 0;
        if(pseudo_obj->GetLastUntreated(untreated_start, untreated_size, S3fsCurl::GetMultipartSize())){
            // when multipart max size is reached
            if(0 != (result = NoCacheMultipartPost(pseudo_obj, physical_fd, untreated_start, untreated_size))){
                S3FS_PRN_ERR("failed to multipart post(start=%lld, size=%lld) for file(physical_fd=%d).", static_cast<long long int>(untreated_start), static_cast<long long int>(untreated_size), physical_fd);
                return result;
            }

            // [NOTE]
            // truncate file to zero and set length to part offset + size
            // after this, file length is (offset + size), but file does not use any disk space.
            //
            if(-1 == ftruncate(physical_fd, 0) || -1 == ftruncate(physical_fd, (untreated_start + untreated_size))){
                S3FS_PRN_ERR("failed to truncate file(physical_fd=%d).", physical_fd);
                return -errno;
            }
            pseudo_obj->ClearUntreated(untreated_start, untreated_size);
        }
    }
    return wsize;
}

// [NOTE]
// Both fdent_lock and fdent_data_lock must be locked before calling.
//
ssize_t FdEntity::WriteMixMultipart(PseudoFdInfo* pseudo_obj, const char* bytes, off_t start, size_t size)
{
    S3FS_PRN_DBG("[path=%s][pseudo_fd=%d][physical_fd=%d][offset=%lld][size=%zu]", path.c_str(), (pseudo_obj ? pseudo_obj->GetPseudoFd() : -1), physical_fd, static_cast<long long int>(start), size);

    if(-1 == physical_fd || !pseudo_obj){
        S3FS_PRN_ERR("pseudo_fd(%d) to physical_fd(%d) for path(%s) is not opened or not writable", (pseudo_obj ? pseudo_obj->GetPseudoFd() : -1), physical_fd, path.c_str());
        return -EBADF;
    }

    int result;

    if(!pseudo_obj->IsUploading()){
        // check disk space
        off_t restsize = pagelist.GetTotalUnloadedPageSize(0, start, MIN_MULTIPART_SIZE) + size;
        if(ReserveDiskSpace(restsize)){
            // enough disk space
            FdManager::FreeReservedDiskSpace(restsize);
        }else{
            // no enough disk space
            if((start + static_cast<off_t>(size)) <= S3fsCurl::GetMultipartSize()){
                S3FS_PRN_WARN("Not enough local storage to cache write request till multipart upload can start: [path=%s][physical_fd=%d][offset=%lld][size=%zu]", path.c_str(), physical_fd, static_cast<long long int>(start), size);
                return -ENOSPC;   // No space left on device
            }
            if(0 != (result = NoCachePreMultipartPost(pseudo_obj))){
                S3FS_PRN_ERR("failed to switch multipart uploading with no cache(errno=%d)", result);
                return result;
            }
            // start multipart uploading
            if(0 != (result = NoCacheLoadAndPost(pseudo_obj, 0, start))){
                S3FS_PRN_ERR("failed to load uninitialized area and multipart uploading it(errno=%d)", result);
                return result;
            }

            pseudo_obj->ClearUntreated();
        }
    }else{
        // already start multipart uploading
    }

    // Writing
    ssize_t wsize;
    if(-1 == (wsize = pwrite(physical_fd, bytes, size, start))){
        S3FS_PRN_ERR("pwrite failed. errno(%d)", errno);
        return -errno;
    }
    if(0 < wsize){
        pagelist.SetPageLoadedStatus(start, wsize, PageList::PAGE_LOAD_MODIFIED);
        pseudo_obj->AddUntreated(start, wsize);
    }

    // check multipart uploading
    if(pseudo_obj->IsUploading()){
        // get last untreated part(maximum size is multipart size)
        off_t untreated_start = 0;
        off_t untreated_size  = 0;
        if(pseudo_obj->GetLastUntreated(untreated_start, untreated_size, S3fsCurl::GetMultipartSize())){
            // when multipart max size is reached
            if(0 != (result = NoCacheMultipartPost(pseudo_obj, physical_fd, untreated_start, untreated_size))){
                S3FS_PRN_ERR("failed to multipart post(start=%lld, size=%lld) for file(physical_fd=%d).", static_cast<long long int>(untreated_start), static_cast<long long int>(untreated_size), physical_fd);
                return result;
            }

            // [NOTE]
            // truncate file to zero and set length to part offset + size
            // after this, file length is (offset + size), but file does not use any disk space.
            //
            if(-1 == ftruncate(physical_fd, 0) || -1 == ftruncate(physical_fd, (untreated_start + untreated_size))){
                S3FS_PRN_ERR("failed to truncate file(physical_fd=%d).", physical_fd);
                return -errno;
            }
            pseudo_obj->ClearUntreated(untreated_start, untreated_size);
        }
    }
    return wsize;
}

// [NOTE]
// Returns true if merged to orgmeta.
// If true is returned, the caller can update the header.
// If it is false, do not update the header because multipart upload is in progress.
// In this case, the header is pending internally and is updated after the upload
// is complete(flush file).
//
bool FdEntity::MergeOrgMeta(headers_t& updatemeta)
{
    AutoLock auto_lock(&fdent_lock);

    merge_headers(orgmeta, updatemeta, true);      // overwrite all keys
    // [NOTE]
    // this is special cases, we remove the key which has empty values.
    for(headers_t::iterator hiter = orgmeta.begin(); hiter != orgmeta.end(); ){
        if(hiter->second.empty()){
            orgmeta.erase(hiter++);
        }else{
            ++hiter;
        }
    }
    updatemeta = orgmeta;
    orgmeta.erase("x-oss-copy-source");

    // update ctime/mtime/atime
    struct timespec mtime = get_mtime(updatemeta, false);      // not overcheck
    struct timespec ctime = get_ctime(updatemeta, false);      // not overcheck
    struct timespec atime = get_atime(updatemeta, false);      // not overcheck
    if(0 <= mtime.tv_sec){
        SetMCtime(mtime, (ctime.tv_sec < 0 ? mtime : ctime), true);
    }
    if(0 <= atime.tv_sec){
        SetAtime(atime, true);
    }
    is_meta_pending |= (IsUploading(true) || pagelist.IsModified());

    return is_meta_pending;
}

// global function in s3fs.cpp
int put_headers(const char* path, headers_t& meta, bool is_copy, bool use_st_size = true);

int FdEntity::UploadPendingMeta()
{
    if(!is_meta_pending) {
       return 0;
    }

    headers_t updatemeta = orgmeta;
    updatemeta["x-oss-copy-source"]        = urlEncode(service_path + S3fsCred::GetBucket() + get_realpath(path.c_str()));
    updatemeta["x-oss-metadata-directive"] = "REPLACE";
    // put headers, no need to update mtime to avoid dead lock
    int result = put_headers(path.c_str(), updatemeta, true);
    if(0 != result){
        S3FS_PRN_ERR("failed to put header after flushing file(%s) by(%d).", path.c_str(), result);
    }
    is_meta_pending = false;
    return result;
}

// [NOTE]
// For systems where the fallocate function cannot be detected, use a dummy function.
// ex. OSX
//
#ifndef HAVE_FALLOCATE
static int fallocate(int /*fd*/, int /*mode*/, off_t /*offset*/, off_t /*len*/)
{
    errno = ENOSYS;     // This is a bad idea, but the caller can handle it simply.
    return -1;
}
#endif  // HAVE_FALLOCATE

// [NOTE]
// If HAVE_FALLOCATE is undefined, or versions prior to 2.6.38(fallocate function exists),
// following flags are undefined. Then we need these symbols defined in fallocate, so we
// define them here.
// The definitions are copied from linux/falloc.h, but if HAVE_FALLOCATE is undefined,
// these values can be anything.
//
#ifndef FALLOC_FL_PUNCH_HOLE
#define FALLOC_FL_PUNCH_HOLE     0x02 /* de-allocates range */
#endif
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE      0x01
#endif

// [NOTE]
// This method punches an area(on cache file) that has no data at the time it is called.
// This is called to prevent the cache file from growing.
// However, this method uses the non-portable(Linux specific) system call fallocate().
// Also, depending on the file system, FALLOC_FL_PUNCH_HOLE mode may not work and HOLE
// will not open.(Filesystems for which this method works are ext4, btrfs, xfs, etc.)
// 
bool FdEntity::PunchHole(off_t start, size_t size)
{
    S3FS_PRN_DBG("[path=%s][physical_fd=%d][offset=%lld][size=%zu]", path.c_str(), physical_fd, static_cast<long long int>(start), size);

    if(-1 == physical_fd){
        return false;
    }
    AutoLock auto_lock(&fdent_data_lock);

    // get page list that have no data
    fdpage_list_t   nodata_pages;
    if(!pagelist.GetNoDataPageLists(nodata_pages)){
        S3FS_PRN_ERR("failed to get page list that have no data.");
        return false;
    }
    if(nodata_pages.empty()){
        S3FS_PRN_DBG("there is no page list that have no data, so nothing to do.");
        return true;
    }

    // try to punch hole to file
    for(fdpage_list_t::const_iterator iter = nodata_pages.begin(); iter != nodata_pages.end(); ++iter){
        if(0 != fallocate(physical_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, iter->offset, iter->bytes)){
            if(ENOSYS == errno || EOPNOTSUPP == errno){
                S3FS_PRN_ERR("failed to fallocate for punching hole to file with errno(%d), it maybe the fallocate function is not implemented in this kernel, or the file system does not support FALLOC_FL_PUNCH_HOLE.", errno);
            }else{
                S3FS_PRN_ERR("failed to fallocate for punching hole to file with errno(%d)", errno);
            }
            return false;
        }
        if(!pagelist.SetPageLoadedStatus(iter->offset, iter->bytes, PageList::PAGE_NOT_LOAD_MODIFIED)){
            S3FS_PRN_ERR("succeed to punch HOLEs in the cache file, but failed to update the cache stat.");
            return false;
        }
        S3FS_PRN_DBG("made a hole at [%lld - %lld bytes](into a boundary) of the cache file.", static_cast<long long int>(iter->offset), static_cast<long long int>(iter->bytes));
    }
    return true;
}

// [NOTE]
// Indicate that a new file's is dirty.
// This ensures that both metadata and data are synced during flush.
//
void FdEntity::MarkDirtyNewFile()
{
    pagelist.Init(0, false, true);
    is_meta_pending = true;
}

void FdEntity::CheckAndExitDirectReadIfNeeded()
{
    AutoLock auto_lock(&fdent_lock);
    if(!is_direct_read){
        return;
    }
    
    is_direct_read = false;
    for(fdinfo_map_t::iterator iter = pseudo_fd_map.begin(); iter != pseudo_fd_map.end(); ++iter){
        PseudoFdInfo* ppseudofdinfo = iter->second;
        ppseudofdinfo->ExitDirectRead();
    }
    return;
}

// fdent_data_lock should be locked before calling
// if free disk is less than multipart_size, try to clear all cache for this fd.
void FdEntity::CheckAndFreeDiskCacheIfNeeded()
{
    if(FdManager::IsSafeDiskSpace(NULL, S3fsCurl::GetMultipartSize())){
        return;
    }

    if(!pagelist.IsModified()){
        // try to clear all cache for this fd.
        S3FS_PRN_DBG("try to clear cache for file(%s).", path.c_str());
        pagelist.Init(pagelist.Size(), false, false);
        if(-1 == ftruncate(physical_fd, 0) || -1 == ftruncate(physical_fd, pagelist.Size())){
            S3FS_PRN_WARN("failed to truncate temporary file(physical_fd=%d).", physical_fd);
        }
    }

    return;
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
