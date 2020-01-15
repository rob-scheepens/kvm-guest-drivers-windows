/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Gal Hammer <ghammer@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma warning(push)
// '<anonymous-tag>' : structure was padded due to alignment specifier
#pragma warning(disable: 4324) 
// nonstandard extension used : nameless struct / union
#pragma warning(disable: 4201)
// potentially uninitialized local variable 'Size' used
#pragma warning(disable: 4701)
#include "winfsp/winfsp.h"
#pragma warning(pop)

#include <aclapi.h>
#include <fcntl.h>
#include <initguid.h>
#include <setupapi.h>
#include <stdio.h>
#include <sys/stat.h>

#include "virtfs.h"
#include "fusereq.h"

#define FS_SERVICE_NAME TEXT("VirtIO-FS")
#define ALLOCATION_UNIT 4096
#define OWNER_UID       197609 // 544 // 501
#define OWNER_GID       197121 // 514

#if !defined(O_DIRECTORY)
#define O_DIRECTORY 0x200000
#endif

#define DBG(format, ...) \
    fprintf(stderr, "*** %s: " format "\n", __FUNCTION__, __VA_ARGS__)

#define SafeHeapFree(p) if (p != NULL) { HeapFree(GetProcessHeap(), 0, p); }

typedef struct
{
    FSP_FILE_SYSTEM *FileSystem;
    
    HANDLE  Device;

    UINT32  OwnerUid;
    UINT32  OwnerGid;

} VIRTFS;

typedef struct
{
    PVOID   DirBuffer;
    BOOLEAN IsDirectory;

    uint64_t NodeId;
    uint64_t FileHandle;

} VIRTFS_FILE_CONTEXT, *PVIRTFS_FILE_CONTEXT;

static int64_t GetUniqueIdentifier()
{
    static int64_t uniq = 1;

    return InterlockedIncrement64(&uniq);
}

static void FUSE_HEADER_INIT(struct fuse_in_header *hdr,
                             uint32_t opcode,
                             uint64_t nodeid,
                             uint32_t datalen)
{
    hdr->len = sizeof(*hdr) + datalen;
    hdr->opcode = opcode;
    hdr->unique = GetUniqueIdentifier();
    hdr->nodeid = nodeid;
    hdr->uid = 0;
    hdr->gid = 0;
    hdr->pid = GetCurrentProcessId();
}

static VOID VirtFsDelete(VIRTFS *VirtFs)
{
    if (VirtFs->FileSystem != NULL)
    {
        FspFileSystemDelete(VirtFs->FileSystem);
        VirtFs->FileSystem = NULL;
    }

    if (VirtFs->Device != INVALID_HANDLE_VALUE)
    {
        CloseHandle(VirtFs->Device);
        VirtFs->Device = INVALID_HANDLE_VALUE;
    }

    SafeHeapFree(VirtFs);
}

static NTSTATUS FindDeviceInterface(PHANDLE Device)
{
    WCHAR DevicePath[MAX_PATH];
    HDEVINFO DevInfo;
    SECURITY_ATTRIBUTES SecurityAttributes;
    SP_DEVICE_INTERFACE_DATA DevIfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA DevIfaceDetail = NULL;
    ULONG Length, RequiredLength = 0;
    BOOL Result;

    DevInfo = SetupDiGetClassDevs(&GUID_DEVINTERFACE_VIRT_FS, NULL, NULL,
        (DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));

    if (DevInfo == INVALID_HANDLE_VALUE)
    {
        return FspNtStatusFromWin32(GetLastError());;
    }

    DevIfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    Result = SetupDiEnumDeviceInterfaces(DevInfo, 0,
        &GUID_DEVINTERFACE_VIRT_FS, 0, &DevIfaceData);

    if (Result == FALSE)
    {
        SetupDiDestroyDeviceInfoList(DevInfo);
        return FspNtStatusFromWin32(GetLastError());
    }

    SetupDiGetDeviceInterfaceDetail(DevInfo, &DevIfaceData, NULL, 0,
        &RequiredLength, NULL);

    DevIfaceDetail = LocalAlloc(LMEM_FIXED, RequiredLength);
    if (DevIfaceDetail == NULL)
    {
        SetupDiDestroyDeviceInfoList(DevInfo);
        return FspNtStatusFromWin32(GetLastError());
    }

    DevIfaceDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
    Length = RequiredLength;

    Result = SetupDiGetDeviceInterfaceDetail(DevInfo, &DevIfaceData,
        DevIfaceDetail, Length, &RequiredLength, NULL);

    if (Result == FALSE)
    {
        LocalFree(DevIfaceDetail);
        SetupDiDestroyDeviceInfoList(DevInfo);
        return FspNtStatusFromWin32(GetLastError());
    }

    wcscpy_s(DevicePath, sizeof(DevicePath) / sizeof(WCHAR),
        DevIfaceDetail->DevicePath);

    LocalFree(DevIfaceDetail);
    SetupDiDestroyDeviceInfoList(DevInfo);

    SecurityAttributes.nLength = sizeof(SecurityAttributes);
    SecurityAttributes.lpSecurityDescriptor = NULL;
    SecurityAttributes.bInheritHandle = FALSE;

    *Device = CreateFile(DevicePath, GENERIC_READ | GENERIC_WRITE,
        0, &SecurityAttributes, OPEN_EXISTING, 0L, NULL);

    if (*Device == INVALID_HANDLE_VALUE)
    {
        return FspNtStatusFromWin32(GetLastError());
    }

    return STATUS_SUCCESS;
}

static UINT32 PosixUnixModeToAttributes(uint32_t mode)
{
    UINT32 Attributes;

    switch (mode & S_IFMT)
    {
        case S_IFDIR:
            Attributes = FILE_ATTRIBUTE_DIRECTORY;
            break;

        default:
            Attributes = FILE_ATTRIBUTE_NORMAL;
            break;
    }

    return Attributes;
}

static VOID SetFileInfo(struct fuse_attr *attr,
                        FSP_FSCTL_FILE_INFO *FileInfo)
{
    FileInfo->FileAttributes = PosixUnixModeToAttributes(attr->mode);
    FileInfo->ReparseTag = 0;
    FileInfo->AllocationSize = attr->blocks * attr->blksize;
    FileInfo->FileSize = attr->size;
    FspPosixUnixTimeToFileTime((void *)&attr->ctime, &FileInfo->CreationTime);
    FspPosixUnixTimeToFileTime((void *)&attr->atime,
        &FileInfo->LastAccessTime);
    FspPosixUnixTimeToFileTime((void *)&attr->mtime,
        &FileInfo->LastWriteTime);
    FileInfo->ChangeTime = FileInfo->LastWriteTime;
    FileInfo->IndexNumber = 0;
    FileInfo->HardLinks = 0;
    FileInfo->EaSize = 0;

    DBG("ino=%Iu size=%Iu blocks=%Iu atime=%Iu mtime=%Iu ctime=%Iu "
        "atimensec=%u mtimensec=%u ctimensec=%u mode=%x nlink=%u uid=%u "
        "gid=%u rdev=%u blksize=%u", attr->ino, attr->size, attr->blocks,
        attr->atime, attr->mtime, attr->ctime, attr->atimensec,
        attr->mtimensec, attr->ctimensec, attr->mode, attr->nlink,
        attr->uid, attr->gid, attr->rdev, attr->blksize);
}

static NTSTATUS VirtFsFuseRequest(HANDLE Device,
                                  LPVOID InBuffer,
                                  DWORD InBufferSize,
                                  LPVOID OutBuffer,
                                  DWORD OutBufferSize)
{
    NTSTATUS Status = STATUS_SUCCESS;
    DWORD BytesReturned = 0;
    BOOL Result;
    struct fuse_in_header *in_hdr = InBuffer;
    struct fuse_out_header *out_hdr = OutBuffer;

    DBG(">>req: %d unique: %Iu len: %u", in_hdr->opcode, in_hdr->unique,
        in_hdr->len);

    Result = DeviceIoControl(Device, IOCTL_VIRTFS_FUSE_REQUEST,
        InBuffer, InBufferSize, OutBuffer, OutBufferSize,
        &BytesReturned, NULL);

    if (Result == FALSE)
    {
        return FspNtStatusFromWin32(GetLastError());
    }

    DBG("<<len: %u error: %d unique: %Iu", out_hdr->len, out_hdr->error,
        out_hdr->unique);

    if (BytesReturned != out_hdr->len)
    {
        DBG("BytesReturned != hdr->len");
    }

    if ((BytesReturned != sizeof(struct fuse_out_header)) &&
        (BytesReturned < OutBufferSize))
    {
        DBG("Bytes Returned: %d Expected: %d", BytesReturned, OutBufferSize);
        // XXX return STATUS_UNSUCCESSFUL;
    }

    if (out_hdr->error < 0)
    {
        switch (out_hdr->error)
        {
            case -EPERM:
                Status = STATUS_ACCESS_DENIED;
                break;
            case -ENOENT:
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                break;
            case -EIO:
                Status = STATUS_IO_DEVICE_ERROR;
                break;
            case -EBADF:
                Status = STATUS_OBJECT_NAME_INVALID;
                break;
            case -EINVAL:
                Status = STATUS_INVALID_PARAMETER;
                break;
            default:
                Status = STATUS_UNSUCCESSFUL;
                break;
        }
    }

    return Status;
}

static NTSTATUS VirtFsCreateFile(VIRTFS *VirtFs,
                                 VIRTFS_FILE_CONTEXT *FileContext,
                                 UINT32 GrantedAccess,
                                 CHAR *FileName,
                                 UINT32 Mode,
                                 FSP_FSCTL_FILE_INFO *FileInfo)
{
    NTSTATUS Status;
    FUSE_CREATE_IN create_in;
    FUSE_CREATE_OUT create_out;

    FUSE_HEADER_INIT(&create_in.hdr, FUSE_CREATE, FUSE_ROOT_ID,
        sizeof(struct fuse_create_in) + lstrlenA(FileName) + 1);

    create_in.hdr.uid = VirtFs->OwnerUid;
    create_in.hdr.gid = VirtFs->OwnerGid;

    lstrcpyA(create_in.name, FileName);
    create_in.create.mode = Mode;
    create_in.create.umask = 0;

    switch (GrantedAccess & (FILE_READ_DATA | FILE_WRITE_DATA))
    {
        case FILE_WRITE_DATA:
            create_in.create.flags = O_WRONLY;
            break;
        case FILE_READ_DATA | FILE_WRITE_DATA:
            create_in.create.flags = O_RDWR;
            break;
        case FILE_READ_DATA:
            __fallthrough;
        default:
            create_in.create.flags = O_RDONLY;
            break;
    }

    if (GrantedAccess & FILE_APPEND_DATA)
    {
        create_in.create.flags |= O_APPEND;
    }

    DBG("create_in.create.flags: 0x%08x", create_in.create.flags);
    DBG("create_in.create.mode: 0x%08x", create_in.create.mode);

    Status = VirtFsFuseRequest(VirtFs->Device, &create_in, create_in.hdr.len,
        &create_out, sizeof(create_out));

    if (NT_SUCCESS(Status))
    {
        FileContext->NodeId = create_out.entry.nodeid;
        FileContext->FileHandle = create_out.open.fh;
        SetFileInfo(&create_out.entry.attr, FileInfo);
    }

    return Status;
}

static NTSTATUS VirtFsCreateDir(VIRTFS *VirtFs,
                                VIRTFS_FILE_CONTEXT *FileContext,
                                CHAR *FileName,
                                UINT32 Mode,
                                FSP_FSCTL_FILE_INFO *FileInfo)
{
    NTSTATUS Status;
    FUSE_MKDIR_IN mkdir_in;
    FUSE_MKDIR_OUT mkdir_out;

    FUSE_HEADER_INIT(&mkdir_in.hdr, FUSE_MKDIR, FUSE_ROOT_ID,
        sizeof(struct fuse_mkdir_in) + lstrlenA(FileName) + 1);

    mkdir_in.hdr.uid = VirtFs->OwnerUid;
    mkdir_in.hdr.gid = VirtFs->OwnerGid;

    lstrcpyA(mkdir_in.name, FileName);
    mkdir_in.mkdir.mode = Mode;
    mkdir_in.mkdir.umask = 0;

    Status = VirtFsFuseRequest(VirtFs->Device, &mkdir_in, mkdir_in.hdr.len,
        &mkdir_out, sizeof(mkdir_out));

    if (NT_SUCCESS(Status))
    {
        FileContext->NodeId = mkdir_out.entry.nodeid;
        SetFileInfo(&mkdir_out.entry.attr, FileInfo);
    }

    return Status;
}

static VOID VirtFsDeleteFile(VIRTFS *VirtFs,
                             VIRTFS_FILE_CONTEXT *FileContext,
                             CHAR *FileName)
{
    FUSE_UNLINK_IN unlink_in;
    FUSE_UNLINK_OUT unlink_out;

    FUSE_HEADER_INIT(&unlink_in.hdr,
        FileContext->IsDirectory ? FUSE_RMDIR : FUSE_UNLINK, FUSE_ROOT_ID,
        lstrlenA(FileName) + 1);

    lstrcpyA(unlink_in.name, FileName);

    (VOID)VirtFsFuseRequest(VirtFs->Device, &unlink_in, unlink_in.hdr.len,
        &unlink_out, sizeof(unlink_out));
}

static NTSTATUS VirtFsLookupFileName(HANDLE Device,
                                     PWSTR FileName,
                                     FUSE_LOOKUP_OUT *LookupOut)
{
    NTSTATUS Status;
    FUSE_LOOKUP_IN lookup_in;
    char *filename;

    DBG("\"%S\"", FileName);

    if (lstrcmp(FileName, TEXT("\\")) == 0)
    {
        FileName = TEXT("\\.");
    }

    Status = FspPosixMapWindowsToPosixPath(FileName + 1, &filename);
    if (!NT_SUCCESS(Status))
    {
        FspPosixDeletePath(filename);
        return Status;
    }

    FUSE_HEADER_INIT(&lookup_in.hdr, FUSE_LOOKUP, FUSE_ROOT_ID,
        lstrlenA(filename) + 1);

    lstrcpyA(lookup_in.name, filename);

    Status = VirtFsFuseRequest(Device, &lookup_in, lookup_in.hdr.len,
        LookupOut, sizeof(*LookupOut));

    if (NT_SUCCESS(Status))
    {
        struct fuse_attr *attr = &LookupOut->entry.attr;

        DBG("nodeid=%Iu ino=%Iu size=%Iu blocks=%Iu atime=%Iu mtime=%Iu "
            "ctime=%Iu atimensec=%u mtimensec=%u ctimensec=%u mode=%x "
            "nlink=%u uid=%u gid=%u rdev=%u blksize=%u",
            LookupOut->entry.nodeid, attr->ino, attr->size, attr->blocks,
            attr->atime, attr->mtime, attr->ctime, attr->atimensec,
            attr->mtimensec, attr->ctimensec, attr->mode, attr->nlink,
            attr->uid, attr->gid, attr->rdev, attr->blksize);
    }

    FspPosixDeletePath(filename);

    return Status;
}

static NTSTATUS GetFileInfoInternal(HANDLE Device,
                                    uint64_t nodeid,
                                    uint64_t fh,
                                    FSP_FSCTL_FILE_INFO *FileInfo,
                                    PSECURITY_DESCRIPTOR *SecurityDescriptor)
{
    NTSTATUS Status;
    FUSE_GETATTR_IN getattr_in;
    FUSE_GETATTR_OUT getattr_out;

    DBG("fh: %Iu nodeid: %Iu", fh, nodeid);

    FUSE_HEADER_INIT(&getattr_in.hdr, FUSE_GETATTR, nodeid,
        sizeof(getattr_in.getattr));

    getattr_in.getattr.fh = fh;
    getattr_in.getattr.getattr_flags = 0;
    if (fh != 0)
    {
        getattr_in.getattr.getattr_flags |= FUSE_GETATTR_FH;
    }
        
    Status = VirtFsFuseRequest(Device, &getattr_in, sizeof(getattr_in),
        &getattr_out, sizeof(getattr_out));

    if (NT_SUCCESS(Status))
    {
        struct fuse_attr *attr = &getattr_out.attr.attr;

        if (FileInfo != NULL)
        {
            SetFileInfo(attr, FileInfo);
        }

        if (SecurityDescriptor != NULL)
        {
            Status = FspPosixMapPermissionsToSecurityDescriptor(OWNER_UID,
                OWNER_GID, attr->mode, SecurityDescriptor);
        }
    }

    return Status;
}

static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM *FileSystem,
                              FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    DWORD BytesReturned;
    NTSTATUS Status;
    BOOL Result;
    FUSE_STATFS_IN statfs_in;
    FUSE_STATFS_OUT statfs_out;

    Result = DeviceIoControl(VirtFs->Device, IOCTL_VIRTFS_GET_VOLUME_NAME,
        NULL, 0, VolumeInfo->VolumeLabel, sizeof(VolumeInfo->VolumeLabel),
        &BytesReturned, NULL);

    if (Result == FALSE)
    {
        lstrcpy(VolumeInfo->VolumeLabel, L"VirtFS");
    }

    FUSE_HEADER_INIT(&statfs_in.hdr, FUSE_STATFS, FUSE_ROOT_ID, 0);

    Status = VirtFsFuseRequest(VirtFs->Device, &statfs_in, sizeof(statfs_in),
        &statfs_out, sizeof(statfs_out));

    if (NT_SUCCESS(Status))
    {
        struct fuse_kstatfs *kstatfs = &statfs_out.statfs.st;

        VolumeInfo->TotalSize = kstatfs->bsize * kstatfs->blocks;
        VolumeInfo->FreeSize = kstatfs->bsize * kstatfs->bavail;
        VolumeInfo->VolumeLabelLength = 
            (UINT16)(wcslen(VolumeInfo->VolumeLabel) * sizeof(WCHAR));
    }

    DBG("VolumeLabel: %S", VolumeInfo->VolumeLabel);

    return Status;
}

static NTSTATUS SetVolumeLabel_(FSP_FILE_SYSTEM *FileSystem,
                                PWSTR VolumeLabel,
                                FSP_FSCTL_VOLUME_INFO *VolumeInfo)
{
    UNREFERENCED_PARAMETER(FileSystem);
    UNREFERENCED_PARAMETER(VolumeLabel);
    UNREFERENCED_PARAMETER(VolumeInfo);

    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM *FileSystem,
                                  PWSTR FileName,
                                  PUINT32 PFileAttributes,
                                  PSECURITY_DESCRIPTOR SecurityDescriptor,
                                  SIZE_T *PSecurityDescriptorSize)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    PSECURITY_DESCRIPTOR Security = NULL;
    DWORD SecuritySize;
    NTSTATUS Status;
    FUSE_LOOKUP_OUT lookup_out;

    DBG("\"%S\"", FileName);

    Status = VirtFsLookupFileName(VirtFs->Device, FileName, &lookup_out);
    if (NT_SUCCESS(Status))
    {
        struct fuse_attr *attr = &lookup_out.entry.attr;

        if (lstrcmp(FileName, TEXT("\\")) == 0)
        {
            VirtFs->OwnerUid = attr->uid;
            VirtFs->OwnerGid = attr->gid;
        }

        if (PFileAttributes != NULL)
        {
            *PFileAttributes = PosixUnixModeToAttributes(attr->mode);
        }

        Status = FspPosixMapPermissionsToSecurityDescriptor(OWNER_UID,
            OWNER_GID, attr->mode, &Security);

        if (NT_SUCCESS(Status))
        {
            SecuritySize = GetSecurityDescriptorLength(Security);

            if ((PSecurityDescriptorSize != NULL) &&
                (*PSecurityDescriptorSize < SecuritySize))
            {
                Status = STATUS_BUFFER_OVERFLOW;
            }
            else
            {
                if (SecurityDescriptor != NULL)
                {
                    memcpy(SecurityDescriptor, Security, SecuritySize);
                }
            }
            SafeHeapFree(Security);
        }
        else
        {
            SecuritySize = 0;
        }

        if (PSecurityDescriptorSize != NULL)
        {
            *PSecurityDescriptorSize = SecuritySize;
        }
    }

    return Status;
}

static NTSTATUS Create(FSP_FILE_SYSTEM *FileSystem,
                       PWSTR FileName,
                       UINT32 CreateOptions,
                       UINT32 GrantedAccess,
                       UINT32 FileAttributes,
                       PSECURITY_DESCRIPTOR SecurityDescriptor,
                       UINT64 AllocationSize,
                       PVOID *PFileContext,
                       FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext;
    NTSTATUS Status;
    UINT32 Mode = 0;
    char *filename;

    DBG("\"%S\" CreateOptions: 0x%08x GrantedAccess: 0x%08x "
        "FileAttributes: 0x%08x AllocationSize: %Iu", FileName,
        CreateOptions, GrantedAccess, FileAttributes, AllocationSize);

    Status = FspPosixMapWindowsToPosixPath(FileName + 1, &filename);
    if (!NT_SUCCESS(Status))
    {
        FspPosixDeletePath(filename);
        return Status;
    }

    FileContext = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        sizeof(*FileContext));

    if (FileContext == NULL)
    {
        FspPosixDeletePath(filename);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    FileContext->IsDirectory = !!(CreateOptions & FILE_DIRECTORY_FILE);

    if (SecurityDescriptor != NULL)
    {
        UINT32 Uid, Gid;

        Status = FspPosixMapSecurityDescriptorToPermissions(
            SecurityDescriptor, &Uid, &Gid, &Mode);

        if (!NT_SUCCESS(Status))
        {
            Mode = 0400 | 0200; /* S_IRUSR | S_IWUSR */
        }
    }

    if (FileContext->IsDirectory == TRUE)
    {
        Status = VirtFsCreateDir(VirtFs, FileContext, filename, Mode,
            FileInfo);
    }
    else
    {
        Status = VirtFsCreateFile(VirtFs, FileContext, GrantedAccess,
            filename, Mode, FileInfo);
    }

    if (!NT_SUCCESS(Status))
    {
        FspPosixDeletePath(filename);
        SafeHeapFree(FileContext);
        return Status;
    }

    *PFileContext = FileContext;
    FspPosixDeletePath(filename);

    return Status;
}

static NTSTATUS Open(FSP_FILE_SYSTEM *FileSystem,
                     PWSTR FileName,
                     UINT32 CreateOptions,
                     UINT32 GrantedAccess,
                     PVOID *PFileContext,
                     FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext;
    NTSTATUS Status;
    FUSE_LOOKUP_OUT lookup_out;
    FUSE_OPEN_IN open_in;
    FUSE_OPEN_OUT open_out;

    DBG("\"%S\" CreateOptions: 0x%08x GrantedAccess: 0x%08x", FileName,
        CreateOptions, GrantedAccess);

    FileContext = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
        sizeof(*FileContext));

    if (FileContext == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = VirtFsLookupFileName(VirtFs->Device, FileName, &lookup_out);
    if (!NT_SUCCESS(Status))
    {
        SafeHeapFree(FileContext);
        return Status;
    }

    FileContext->IsDirectory = !!(lookup_out.entry.attr.mode & S_IFDIR);

    FUSE_HEADER_INIT(&open_in.hdr,
        (FileContext->IsDirectory == TRUE) ? FUSE_OPENDIR : FUSE_OPEN,
        lookup_out.entry.nodeid, sizeof(open_in.open));

    switch (GrantedAccess & (FILE_READ_DATA | FILE_WRITE_DATA))
    {
        case FILE_WRITE_DATA:
            open_in.open.flags = O_WRONLY;
            break;
        case FILE_READ_DATA | FILE_WRITE_DATA:
            open_in.open.flags = O_RDWR;
            break;
        case FILE_READ_DATA:
            open_in.open.flags = O_RDONLY;
            break;
        default:
            open_in.open.flags = 0;
            break;
    }

    if (FileContext->IsDirectory == TRUE)
    {
        open_in.open.flags |= O_DIRECTORY;
    }

    if (GrantedAccess & FILE_APPEND_DATA)
    {
        open_in.open.flags |= O_APPEND;
    }

    Status = VirtFsFuseRequest(VirtFs->Device, &open_in, sizeof(open_in),
        &open_out, sizeof(open_out));

    if (!NT_SUCCESS(Status))
    {
        SafeHeapFree(FileContext);
        return Status;
    }

    FileContext->NodeId = lookup_out.entry.nodeid;
    FileContext->FileHandle = open_out.open.fh;
    
    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    SetFileInfo(&lookup_out.entry.attr, FileInfo);
    
    *PFileContext = FileContext;

    return Status;
}

static NTSTATUS Overwrite(FSP_FILE_SYSTEM *FileSystem,
                          PVOID FileContext0,
                          UINT32 FileAttributes,
                          BOOLEAN ReplaceFileAttributes,
                          UINT64 AllocationSize,
                          FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    NTSTATUS Status;

    DBG("FileAttributes: 0x%08x ReplaceFileAttributes: %d "
        "AllocationSize: %Iu", FileAttributes, ReplaceFileAttributes,
        AllocationSize);

    if (ReplaceFileAttributes == FALSE)
    {
        Status = GetFileInfoInternal(VirtFs->Device, FileContext->NodeId,
            FileContext->FileHandle, FileInfo, NULL);

        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        FileAttributes |= FileInfo->FileAttributes;
    }

    // XXX Call SetBasicInfo and SetFileSize?

    return GetFileInfoInternal(VirtFs->Device, FileContext->NodeId,
        FileContext->FileHandle, FileInfo, NULL);
}

static VOID Cleanup(FSP_FILE_SYSTEM *FileSystem,
                    PVOID FileContext0,
                    PWSTR FileName,
                    ULONG Flags)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    NTSTATUS Status;
    char *filename;

    DBG("\"%S\" Flags: 0x%02x", FileName, Flags);

    if (FileName == NULL)
    {
        return;
    }

    Status = FspPosixMapWindowsToPosixPath(FileName + 1, &filename);
    if (!NT_SUCCESS(Status))
    {
        FspPosixDeletePath(filename);
        return;
    }

    if (Flags & FspCleanupDelete)
    {
        VirtFsDeleteFile(VirtFs, FileContext, filename);
    }
    else
    {
        if (Flags & FspCleanupSetAllocationSize)
        {

        }
        if (Flags & FspCleanupSetArchiveBit)
        {

        }
        if (Flags & FspCleanupSetLastAccessTime)
        {

        }
        if (Flags & FspCleanupSetLastWriteTime)
        {

        }
        if (Flags & FspCleanupSetChangeTime)
        {

        }
    }

    FspPosixDeletePath(filename);
}

static VOID Close(FSP_FILE_SYSTEM *FileSystem, PVOID FileContext0)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    FUSE_RELEASE_IN release_in;
    FUSE_RELEASE_OUT release_out;

    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    FUSE_HEADER_INIT(&release_in.hdr,
        FileContext->IsDirectory ? FUSE_RELEASEDIR : FUSE_RELEASE,
        FileContext->NodeId, sizeof(release_in.release));

    release_in.release.fh = FileContext->FileHandle;
    release_in.release.flags = 0;
    release_in.release.lock_owner = 0;
    release_in.release.release_flags = 0;

    (VOID)VirtFsFuseRequest(VirtFs->Device, &release_in, sizeof(release_in),
        &release_out, sizeof(release_out));

    FspFileSystemDeleteDirectoryBuffer(&FileContext->DirBuffer);

    SafeHeapFree(FileContext);
}

static NTSTATUS Read(FSP_FILE_SYSTEM *FileSystem,
                     PVOID FileContext0,
                     PVOID Buffer,
                     UINT64 Offset,
                     ULONG Length,
                     PULONG PBytesTransferred)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    FUSE_READ_IN read_in;
    FUSE_READ_OUT *read_out;
    NTSTATUS Status;

    DBG("Offset: %Iu Length: %u", Offset, Length);
    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    read_out = HeapAlloc(GetProcessHeap(), 0, sizeof(*read_out) + Length);
    if (read_out == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    read_in.read.fh = FileContext->FileHandle;
    read_in.read.offset = Offset;
    read_in.read.size = Length;
    read_in.read.read_flags = 0;
    read_in.read.lock_owner = 0;
    read_in.read.flags = 0;

    FUSE_HEADER_INIT(&read_in.hdr, FUSE_READ, FileContext->NodeId, Length);

    Status = VirtFsFuseRequest(VirtFs->Device, &read_in, sizeof(read_in),
        read_out, sizeof(*read_out) + Length);

    if (!NT_SUCCESS(Status))
    {
        if (PBytesTransferred != NULL)
        {
            *PBytesTransferred = 0;
        }
        SafeHeapFree(read_out);
        return Status;
    }

    if (PBytesTransferred != NULL)
    {
        *PBytesTransferred = read_out->hdr.len -
            sizeof(struct fuse_out_header);

        if (Buffer != NULL)
        {
            CopyMemory(Buffer, read_out->buf, *PBytesTransferred);
        }

        DBG("BytesTransferred: %d", *PBytesTransferred);
    }

    SafeHeapFree(read_out);

    return STATUS_SUCCESS;
}

static NTSTATUS Write(FSP_FILE_SYSTEM *FileSystem,
                      PVOID FileContext0,
                      PVOID Buffer,
                      UINT64 Offset,
                      ULONG Length,
                      BOOLEAN WriteToEndOfFile,
                      BOOLEAN ConstrainedIo,
                      PULONG PBytesTransferred,
                      FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    FUSE_WRITE_IN *write_in;
    FUSE_WRITE_OUT write_out;
    NTSTATUS Status;

    DBG("Buffer: %p Offset: %Iu Length: %u WriteToEndOfFile: %d "
        "ConstrainedIo: %d", Buffer, Offset, Length, WriteToEndOfFile,
        ConstrainedIo);
    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    if (ConstrainedIo)
    {
        Status = GetFileInfoInternal(VirtFs->Device, FileContext->NodeId,
            FileContext->FileHandle, FileInfo, NULL);

        if (!NT_SUCCESS(Status))
        {
            return Status;
        }

        if (Offset >= FileInfo->FileSize)
        {
            return STATUS_SUCCESS;
        }
        
        if ((Offset + Length) > FileInfo->FileSize)
        {
            Length = (ULONG)(FileInfo->FileSize - Offset);
        }
    }

    write_in = HeapAlloc(GetProcessHeap(), 0, sizeof(*write_in) + Length);
    if (write_in == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    FUSE_HEADER_INIT(&write_in->hdr, FUSE_WRITE, FileContext->NodeId,
        sizeof(struct fuse_write_in) + Length);
    
    write_in->write.fh = FileContext->FileHandle;
    write_in->write.offset = Offset;
    write_in->write.size = Length;
    write_in->write.write_flags = 0;
    write_in->write.lock_owner = 0;
    write_in->write.flags = 0;

    if (Buffer != NULL)
    {
        CopyMemory(write_in->buf, Buffer, Length);
    }

    Status = VirtFsFuseRequest(VirtFs->Device, write_in,
        write_in->hdr.len, &write_out, sizeof(write_out));

    SafeHeapFree(write_in);

    if (!NT_SUCCESS(Status))
    {
        if (PBytesTransferred != NULL)
        {
            *PBytesTransferred = 0;
        }
        return Status;
    }

    if (PBytesTransferred != NULL)
    {
        *PBytesTransferred = write_out.write.size;
    }

    return GetFileInfoInternal(VirtFs->Device, FileContext->NodeId,
        FileContext->FileHandle, FileInfo, NULL);
}

NTSTATUS Flush(FSP_FILE_SYSTEM *FileSystem,
               PVOID FileContext0,
               FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    NTSTATUS Status;
    FUSE_FLUSH_IN flush_in;
    FUSE_FLUSH_OUT flush_out;

    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    FUSE_HEADER_INIT(&flush_in.hdr, FUSE_FLUSH, FileContext->NodeId,
        sizeof(flush_in.flush));

    flush_in.flush.fh = FileContext->FileHandle;
    flush_in.flush.unused = 0;
    flush_in.flush.padding = 0;
    flush_in.flush.lock_owner = 0;

    Status = VirtFsFuseRequest(VirtFs->Device, &flush_in, sizeof(flush_in),
        &flush_out, sizeof(flush_out));

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return GetFileInfoInternal(VirtFs->Device, FileContext->NodeId,
        FileContext->FileHandle, FileInfo, NULL);
}

static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM *FileSystem,
                            PVOID FileContext0,
                            FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;

    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    return GetFileInfoInternal(VirtFs->Device, FileContext->NodeId,
        FileContext->FileHandle, FileInfo, NULL);
}

static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM *FileSystem,
                             PVOID FileContext0,
                             UINT32 FileAttributes,
                             UINT64 CreationTime,
                             UINT64 LastAccessTime,
                             UINT64 LastWriteTime,
                             UINT64 ChangeTime,
                             FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    NTSTATUS Status;
    FUSE_SETATTR_IN setattr_in;
    FUSE_SETATTR_OUT setattr_out;

    UNREFERENCED_PARAMETER(FileAttributes);

    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    FUSE_HEADER_INIT(&setattr_in.hdr, FUSE_SETATTR, FileContext->NodeId,
        sizeof(setattr_in.setattr));

    ZeroMemory(&setattr_in.setattr, sizeof(setattr_in.setattr));
    
    setattr_in.setattr.valid = FATTR_FH;
    setattr_in.setattr.fh = FileContext->FileHandle;

    if (LastAccessTime != 0)
    {
        setattr_in.setattr.valid |= FATTR_ATIME;
        FspPosixFileTimeToUnixTime(LastAccessTime,
            (void *)&setattr_in.setattr.atime);
    }
    if (LastWriteTime != 0)
    {
        setattr_in.setattr.valid |= FATTR_MTIME;
        FspPosixFileTimeToUnixTime(LastWriteTime,
            (void *)&setattr_in.setattr.mtime);
    }
    if (ChangeTime != 0)
    {
        setattr_in.setattr.valid |= FATTR_CTIME;
        FspPosixFileTimeToUnixTime(CreationTime,
            (void *)&setattr_in.setattr.ctime);
    }

    Status = VirtFsFuseRequest(VirtFs->Device, &setattr_in,
        sizeof(setattr_in), &setattr_out, sizeof(setattr_out));

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return GetFileInfoInternal(FileSystem, FileContext->NodeId,
        FileContext->FileHandle, FileInfo, NULL);
}

static NTSTATUS SetFileSize(FSP_FILE_SYSTEM *FileSystem,
                            PVOID FileContext0,
                            UINT64 NewSize,
                            BOOLEAN SetAllocationSize,
                            FSP_FSCTL_FILE_INFO *FileInfo)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    NTSTATUS Status;
    FUSE_SETATTR_IN setattr_in;
    FUSE_SETATTR_OUT setattr_out;

    UNREFERENCED_PARAMETER(SetAllocationSize);

    DBG("NewSize: %Iu SetAllocationSide: %d", NewSize, SetAllocationSize);
    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    FUSE_HEADER_INIT(&setattr_in.hdr, FUSE_SETATTR, FileContext->NodeId,
        sizeof(setattr_in.setattr));

    ZeroMemory(&setattr_in.setattr, sizeof(setattr_in.setattr));
    setattr_in.setattr.valid = FATTR_SIZE;
    setattr_in.setattr.size = NewSize;

    Status = VirtFsFuseRequest(VirtFs->Device, &setattr_in,
        sizeof(setattr_in), &setattr_out, sizeof(setattr_out));

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    return GetFileInfoInternal(FileSystem, FileContext->NodeId,
        FileContext->FileHandle, FileInfo, NULL);
}

static NTSTATUS CanDelete(FSP_FILE_SYSTEM *FileSystem,
                          PVOID FileContext0,
                          PWSTR FileName)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    PVIRTFS_FILE_CONTEXT FileContext = FileContext0;
    FSP_FSCTL_FILE_INFO FileInfo;
    NTSTATUS Status;

    DBG("\"%S\"", FileName);
 
    Status = GetFileInfoInternal(VirtFs->Device, FileContext->NodeId,
        FileContext->FileHandle, &FileInfo, NULL);

    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    if (FileInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        // XXX How to check if a directory is not empty?
        return STATUS_DIRECTORY_NOT_EMPTY;
    }

    return Status;
}

static NTSTATUS Rename(FSP_FILE_SYSTEM *FileSystem,
                       PVOID FileContext0,
                       PWSTR FileName,
                       PWSTR NewFileName,
                       BOOLEAN ReplaceIfExists)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    FUSE_RENAME_IN *rename_in;
    FUSE_RENAME_OUT rename_out;
    NTSTATUS Status;
    char *oldname, *newname;
    int oldname_size, newname_size;

    DBG("\"%S\" -> \"%S\" ReplaceIfExist: %d", FileName, NewFileName,
        ReplaceIfExists);
    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    Status = FspPosixMapWindowsToPosixPath(FileName + 1, &oldname);
    if (!NT_SUCCESS(Status))
    {
        FspPosixDeletePath(oldname);
        return Status;
    }

    Status = FspPosixMapWindowsToPosixPath(NewFileName + 1, &newname);
    if (!NT_SUCCESS(Status))
    {
        FspPosixDeletePath(oldname);
        FspPosixDeletePath(newname);
        return Status;
    }

    oldname_size = lstrlenA(oldname) + 1;
    newname_size = lstrlenA(newname) + 1;

    DBG("old: %s (%d) new: %s (%d)", oldname, oldname_size, newname,
        newname_size);

    rename_in = HeapAlloc(GetProcessHeap(), 0, sizeof(*rename_in) +
        oldname_size + newname_size);

    if (rename_in == NULL)
    {
        FspPosixDeletePath(oldname);
        FspPosixDeletePath(newname);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    // TODO: FUSE_ROOT_ID should be the FileName's directory nodeid.
    FUSE_HEADER_INIT(&rename_in->hdr, FUSE_RENAME, FUSE_ROOT_ID,
        sizeof(rename_in->rename) + oldname_size + newname_size);

    rename_in->rename.newdir = FUSE_ROOT_ID;

    CopyMemory(rename_in->names, oldname, oldname_size);
    CopyMemory(rename_in->names + oldname_size, newname, newname_size);

    FspPosixDeletePath(oldname);
    FspPosixDeletePath(newname);

    if (ReplaceIfExists == TRUE)
    {
        // XXX check Linux's behavior and fix to match.
    }

    return VirtFsFuseRequest(VirtFs->Device, rename_in,
        rename_in->hdr.len, &rename_out, sizeof(rename_out));
}

static NTSTATUS GetSecurity(FSP_FILE_SYSTEM *FileSystem,
                            PVOID FileContext0,
                            PSECURITY_DESCRIPTOR SecurityDescriptor,
                            SIZE_T *PSecurityDescriptorSize)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    PVIRTFS_FILE_CONTEXT FileContext = FileContext0;
    PSECURITY_DESCRIPTOR Security;
    DWORD SecurityLength;
    NTSTATUS Status;

    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    Status = GetFileInfoInternal(VirtFs->Device, FileContext->NodeId,
        FileContext->FileHandle, NULL, &Security);

    if (!NT_SUCCESS(Status))
    {
        *PSecurityDescriptorSize = 0;
        return Status;
    }

    SecurityLength = GetSecurityDescriptorLength(Security);
    if (*PSecurityDescriptorSize < SecurityLength)
    {
        *PSecurityDescriptorSize = SecurityLength;
        SafeHeapFree(Security);
        return STATUS_BUFFER_TOO_SMALL;
    }

    *PSecurityDescriptorSize = SecurityLength;
    if (SecurityDescriptor != NULL)
    {
        CopyMemory(SecurityDescriptor, Security, SecurityLength);
    }

    SafeHeapFree(Security);

    return STATUS_SUCCESS;
}

static NTSTATUS SetSecurity(FSP_FILE_SYSTEM *FileSystem,
                            PVOID FileContext0,
                            SECURITY_INFORMATION SecurityInformation,
                            PSECURITY_DESCRIPTOR ModificationDescriptor)
{
    VIRTFS *VirtFs = FileSystem->UserContext;
    PVIRTFS_FILE_CONTEXT FileContext = FileContext0;
    PSECURITY_DESCRIPTOR FileSecurity, NewSecurityDescriptor;
    UINT32 Uid, Gid, Mode, NewMode;
    NTSTATUS Status;

    DBG("fh: %Iu nodeid: %Iu", FileContext->FileHandle, FileContext->NodeId);

    Status = GetFileInfoInternal(VirtFs->FileSystem, FileContext->NodeId,
        FileContext->FileHandle, NULL, &FileSecurity);

    if (!NT_SUCCESS(Status))
    {
        SafeHeapFree(FileSecurity);
        return Status;
    }

    Status = FspPosixMapSecurityDescriptorToPermissions(FileSecurity, &Uid,
        &Gid, &Mode);

    if (!NT_SUCCESS(Status))
    {
        SafeHeapFree(FileSecurity);
        return Status;
    }

    Status = FspSetSecurityDescriptor(FileSecurity, SecurityInformation,
        ModificationDescriptor, &NewSecurityDescriptor);

    if (!NT_SUCCESS(Status))
    {
        SafeHeapFree(FileSecurity);
        return Status;
    }

    SafeHeapFree(FileSecurity);

    Status = FspPosixMapSecurityDescriptorToPermissions(NewSecurityDescriptor,
        &Uid, &Gid, &NewMode);

    if (!NT_SUCCESS(Status))
    {
        FspDeleteSecurityDescriptor(NewSecurityDescriptor,
            (NTSTATUS(*)())FspSetSecurityDescriptor);
        return Status;
    }

    FspDeleteSecurityDescriptor(NewSecurityDescriptor,
        (NTSTATUS(*)())FspSetSecurityDescriptor);

    if (Mode != NewMode)
    {
        FUSE_SETATTR_IN setattr_in;
        FUSE_SETATTR_OUT setattr_out;

        FUSE_HEADER_INIT(&setattr_in.hdr, FUSE_SETATTR, FileContext->NodeId,
            sizeof(setattr_in.setattr));

        ZeroMemory(&setattr_in.setattr, sizeof(setattr_in.setattr));
        setattr_in.setattr.valid = FATTR_MODE;
        setattr_in.setattr.mode = NewMode;

        Status = VirtFsFuseRequest(VirtFs->Device, &setattr_in,
            sizeof(setattr_in), &setattr_out, sizeof(setattr_out));
    }

    return Status;
}

static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM *FileSystem,
                              PVOID FileContext0,
                              PWSTR Pattern,
                              PWSTR Marker,
                              PVOID Buffer,
                              ULONG BufferLength,
                              PULONG PBytesTransferred)
{
#if 1
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    BYTE DirInfoBuf[sizeof(FSP_FSCTL_DIR_INFO) + MAX_PATH * sizeof(WCHAR)];
    FSP_FSCTL_DIR_INFO *DirInfo = (FSP_FSCTL_DIR_INFO *)DirInfoBuf;
    BYTE ReadOutBuf[sizeof(struct fuse_out_header) + ALLOCATION_UNIT];
    struct fuse_direntplus *DirEntryPlus;
    NTSTATUS Status = STATUS_SUCCESS;
    UINT64 Offset = 0;
    UINT32 Remains;
    BOOLEAN Result;
    int FileNameLength;
    FUSE_READ_IN read_in;
    FUSE_READ_OUT *read_out = (FUSE_READ_OUT *)ReadOutBuf;

    DBG("Pattern: %S Marker: %S BufferLength: %u",
        Pattern ? Pattern : TEXT("(null)"), Marker ? Marker : TEXT("(null)"),
        BufferLength);

    Result = FspFileSystemAcquireDirectoryBuffer(&FileContext->DirBuffer,
        Marker == NULL, &Status);
    
    if (Result)
    {
        for (;;)
        {
            FUSE_HEADER_INIT(&read_in.hdr, FUSE_READDIRPLUS, FUSE_ROOT_ID,
                sizeof(read_in.read));

            read_in.read.fh = FileContext->FileHandle;
            read_in.read.offset = Offset;
            read_in.read.size = ALLOCATION_UNIT;
            read_in.read.read_flags = 0;
            read_in.read.lock_owner = 0;
            read_in.read.flags = 0;

            Status = VirtFsFuseRequest(VirtFs->Device, &read_in,
                sizeof(read_in), read_out, sizeof(ReadOutBuf));

            if (!NT_SUCCESS(Status))
            {
                break;
            }

            Remains = read_out->hdr.len - sizeof(struct fuse_out_header);
            if (Remains == 0)
            {
                // A successful request with no data means no more entries.
                break;
            }

            DirEntryPlus = (struct fuse_direntplus *)read_out->buf;

            while (Remains > sizeof(struct fuse_direntplus))
            {
                DBG("ino=%Iu off=%Iu namelen=%u type=%u name=%s",
                    DirEntryPlus->dirent.ino, DirEntryPlus->dirent.off,
                    DirEntryPlus->dirent.namelen, DirEntryPlus->dirent.type,
                    DirEntryPlus->dirent.name);

                ZeroMemory(DirInfoBuf, sizeof(DirInfoBuf));

                // Not using FspPosixMapPosixToWindowsPath so we can do the
                // conversion in-place.
                FileNameLength = MultiByteToWideChar(CP_UTF8, 0,
                    DirEntryPlus->dirent.name, DirEntryPlus->dirent.namelen,
                    DirInfo->FileNameBuf, MAX_PATH);

                DBG("\"%S\" (%d)", DirInfo->FileNameBuf, FileNameLength);

                if (FileNameLength > 0)
                {
                    DirInfo->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) +
                        FileNameLength * sizeof(WCHAR));

                    SetFileInfo(&DirEntryPlus->entry_out.attr,
                        &DirInfo->FileInfo);

                    Result = FspFileSystemFillDirectoryBuffer(
                        &FileContext->DirBuffer, DirInfo, &Status);

                    if (Result == FALSE)
                    {
                        break;
                    }
                }

                Offset = DirEntryPlus->dirent.off;
                Remains -= FUSE_DIRENTPLUS_SIZE(DirEntryPlus);
                DirEntryPlus = (struct fuse_direntplus *)(
                    (PBYTE)DirEntryPlus + FUSE_DIRENTPLUS_SIZE(DirEntryPlus));
            }
        }

        FspFileSystemReleaseDirectoryBuffer(&FileContext->DirBuffer);
    }

    if (NT_SUCCESS(Status))
    {
        FspFileSystemReadDirectoryBuffer(&FileContext->DirBuffer, Marker,
            Buffer, BufferLength, PBytesTransferred);
    }

    return Status;
#endif
#if 0
    BYTE DirInfoBuf[sizeof(FSP_FSCTL_DIR_INFO) + MAX_PATH * sizeof(WCHAR)];
    FSP_FSCTL_DIR_INFO *DirInfo = (FSP_FSCTL_DIR_INFO *)DirInfoBuf;
    BYTE ReadDirBuffer[0x1000];
    VIRTFS *VirtFs = FileSystem->UserContext;
    VIRTFS_FILE_CONTEXT *FileContext = FileContext0;
    NTSTATUS Status;
    BOOL Result;
    FUSE_READ_IN read_in;
    FUSE_READ_OUT *read_out = (FUSE_READ_OUT *)ReadDirBuffer;
    struct fuse_direntplus *dirent;
    int FileNameLength;

    DBG("Pattern: %S Marker: %S BufferLength: %u",
        Pattern ? Pattern : TEXT("(null)"), Marker ? Marker : TEXT("(null)"),
        BufferLength);

    DBG("FileContext Node: %Iu FileHandle: %Iu", FileContext->NodeId,
        FileContext->FileHandle);

    FUSE_HEADER_INIT(&read_in.hdr, FUSE_READDIRPLUS, FUSE_ROOT_ID,
        sizeof(read_in.read));
    
    read_in.read.fh = FileContext->FileHandle;
    read_in.read.offset = 0;
    read_in.read.size = sizeof(ReadDirBuffer);
    read_in.read.read_flags = 0;
    read_in.read.lock_owner = 0;
    read_in.read.flags = 0;

    Status = VirtFsFuseRequest(VirtFs->Device, &read_in, sizeof(read_in),
        read_out, sizeof(ReadDirBuffer));

    if (!NT_SUCCESS(Status))
    {
        *PBytesTransferred = 0;
        return Status;
    }

    DBG("read_out->hdr.len: %u", read_out->hdr.len);
    dirent = (struct fuse_direntplus *)read_out->buf;

    while (read_out->hdr.len > sizeof(struct fuse_direntplus))
    {
        DBG("ino=%Iu off=%Iu namelen=%u type=%u name=%s", dirent->dirent.ino,
            dirent->dirent.off, dirent->dirent.namelen, dirent->dirent.type,
            dirent->dirent.name);

        ZeroMemory(DirInfoBuf, sizeof(DirInfoBuf));

        // Not using FspPosixMapPosixToWindowsPath so we can do the conversion
        // in-place.
        FileNameLength = MultiByteToWideChar(CP_UTF8, 0, dirent->dirent.name,
            dirent->dirent.namelen, DirInfo->FileNameBuf, MAX_PATH);
        
        DBG("\"%S\" (%d)", DirInfo->FileNameBuf, FileNameLength);
        
        if (FileNameLength > 0)
        {
            DirInfo->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) +
                FileNameLength * sizeof(WCHAR));

            SetFileInfo(&dirent->entry_out.attr, &DirInfo->FileInfo);
            
            Result = FspFileSystemAddDirInfo(DirInfo, Buffer, BufferLength,
                PBytesTransferred);

            if (Result == FALSE)
            {
                break;
            }
        }

        read_out->hdr.len -= FUSE_DIRENTPLUS_SIZE(dirent);
        dirent = (struct fuse_direntplus *)((PBYTE)dirent +
            FUSE_DIRENTPLUS_SIZE(dirent));
    }

    FspFileSystemAddDirInfo(NULL, Buffer, BufferLength, PBytesTransferred);

    return STATUS_SUCCESS;
#endif
}

static FSP_FILE_SYSTEM_INTERFACE VirtFsInterface =
{
    .GetVolumeInfo = GetVolumeInfo,
    .SetVolumeLabel = SetVolumeLabel_,
    .GetSecurityByName = GetSecurityByName,
    .Create = Create,
    .Open = Open,
    .Overwrite = Overwrite,
    .Cleanup = Cleanup,
    .Close = Close,
    .Read = Read,
    .Write = Write,
    .Flush = Flush,
    .GetFileInfo = GetFileInfo,
    .SetBasicInfo = SetBasicInfo,
    .SetFileSize = SetFileSize,
    .CanDelete = CanDelete,
    .Rename = Rename,
    .GetSecurity = GetSecurity,
    .SetSecurity = SetSecurity,
    .ReadDirectory = ReadDirectory
};

static NTSTATUS SvcStart(FSP_SERVICE *Service, ULONG argc, PWSTR *argv)
{
    VIRTFS *VirtFs;
    FSP_FSCTL_VOLUME_PARAMS VolumeParams;
//    PWSTR MountPoint = L"C:\\Shared Folders\\mytag";
    FILETIME FileTime;
    NTSTATUS Status;
    FUSE_INIT_IN init_in;
    FUSE_INIT_OUT init_out;

    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    // PWSTR* ProfilePath;
    // HRESULT Result;
    // Result = SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, NULL,
    //      &ProfilePath);
    // if (FAILED(Result))
    // {
    // }
    // CoTaskMemFree(ProfilePath);

    //PathMakeSystemFolder();

    PWSTR VolumePrefix = L"\\foo\\bar";
//    PWSTR PassThrough = L"foo";
    PWSTR MountPoint = L"Z:";
//    PWSTR MountPoint = NULL;

    VirtFs = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*VirtFs));
    if (VirtFs == NULL)
    {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Status = FindDeviceInterface(&VirtFs->Device);
    if (!NT_SUCCESS(Status))
    {
        VirtFsDelete(VirtFs);
        return Status;
    }

    FUSE_HEADER_INIT(&init_in.hdr, FUSE_INIT, FUSE_ROOT_ID,
        sizeof(init_in.init));

    init_in.init.major = FUSE_KERNEL_VERSION;
    init_in.init.minor = FUSE_KERNEL_MINOR_VERSION;
    init_in.init.max_readahead = 0;
    init_in.init.flags = FUSE_DO_READDIRPLUS;

    Status = VirtFsFuseRequest(VirtFs->Device, &init_in, sizeof(init_in),
        &init_out, sizeof(init_out));

    if (!NT_SUCCESS(Status))
    {
        VirtFsDelete(VirtFs);
        return Status;
    }

    GetSystemTimeAsFileTime(&FileTime);

    ZeroMemory(&VolumeParams, sizeof(VolumeParams));
    VolumeParams.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
    VolumeParams.SectorSize = ALLOCATION_UNIT;
    VolumeParams.SectorsPerAllocationUnit = 1;
    VolumeParams.VolumeCreationTime = ((PLARGE_INTEGER)&FileTime)->QuadPart;
//    VolumeParams.VolumeSerialNumber = 0;
    VolumeParams.FileInfoTimeout = 1000;
    VolumeParams.CaseSensitiveSearch = 1;
    VolumeParams.CasePreservedNames = 1;
    VolumeParams.UnicodeOnDisk = 1;
    VolumeParams.PersistentAcls = 1;
    VolumeParams.PostCleanupWhenModifiedOnly = 1;
//    VolumeParams.PassQueryDirectoryPattern = 1;
    VolumeParams.FlushAndPurgeOnCleanup = 1;
    VolumeParams.UmFileContextIsUserContext2 = 1;
//    VolumeParams.DirectoryMarkerAsNextOffset = 1;
    if (VolumePrefix != NULL)
    {
        wcscpy_s(VolumeParams.Prefix,
            sizeof(VolumeParams.Prefix) / sizeof(WCHAR), VolumePrefix);
    }
    wcscpy_s(VolumeParams.FileSystemName,
        sizeof(VolumeParams.FileSystemName) / sizeof(WCHAR), FS_SERVICE_NAME);

    Status = FspFileSystemCreate(TEXT(FSP_FSCTL_NET_DEVICE_NAME),
        &VolumeParams, &VirtFsInterface, &VirtFs->FileSystem);

    if (!NT_SUCCESS(Status))
    {
        VirtFsDelete(VirtFs);
        return Status;
    }

    FspDebugLogSetHandle(GetStdHandle(STD_ERROR_HANDLE));
    FspFileSystemSetDebugLog(VirtFs->FileSystem, (UINT32)(-1));
    
    VirtFs->FileSystem->UserContext = Service->UserContext = VirtFs;

    Status = FspFileSystemSetMountPoint(VirtFs->FileSystem, MountPoint);
    if (NT_SUCCESS(Status))
    {
        Status = FspFileSystemStartDispatcher(VirtFs->FileSystem, 0);
    }

    if (!NT_SUCCESS(Status))
    {
        VirtFsDelete(VirtFs);
    }

    return Status;
}

static NTSTATUS SvcStop(FSP_SERVICE *Service)
{
    VIRTFS *VirtFs = Service->UserContext;

    FspFileSystemStopDispatcher(VirtFs->FileSystem);
    VirtFsDelete(VirtFs);

    return STATUS_SUCCESS;
}

static NTSTATUS SvcControl(FSP_SERVICE *Service,
                           ULONG Control,
                           ULONG EventType,
                           PVOID EventData)
{
    UNREFERENCED_PARAMETER(Service);
    UNREFERENCED_PARAMETER(Control);
    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(EventData);

    return STATUS_SUCCESS;
}

int wmain(int argc, wchar_t **argv)
{
    UNREFERENCED_PARAMETER(argc);
    UNREFERENCED_PARAMETER(argv);

    if (!NT_SUCCESS(FspLoad(0)))
    {
        return ERROR_DELAY_LOAD_FAILED;
    }

    return FspServiceRun(FS_SERVICE_NAME, SvcStart, SvcStop, SvcControl);
}