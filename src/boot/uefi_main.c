typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef unsigned int UINT32;
typedef unsigned long long UINT64;
typedef unsigned long long UINTN;
typedef long long INT64;
typedef void *EFI_HANDLE;
typedef UINT64 EFI_STATUS;
typedef void *EFI_EVENT;
typedef unsigned short CHAR16;

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS 0
#define EFI_LOAD_ERROR 0x8000000000000001ULL
#define EFI_INVALID_PARAMETER 0x8000000000000002ULL
#define EFI_NOT_FOUND 0x8000000000000014ULL
#define EFI_OUT_OF_RESOURCES 0x8000000000000009ULL
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL

#define EFI_ERROR(Status) (((Status) & 0x8000000000000000ULL) != 0)
#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef enum {
  EfiLoaderData = 2
} EFI_MEMORY_TYPE;

typedef struct {
  UINT32 Type;
  UINT32 Pad;
  UINT64 PhysicalStart;
  UINT64 VirtualStart;
  UINT64 NumberOfPages;
  UINT64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

typedef EFI_STATUS(EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR *MemoryMap,
    UINTN *MapKey,
    UINTN *DescriptorSize,
    UINT32 *DescriptorVersion);

typedef enum {
  ByProtocol = 2
} EFI_LOCATE_SEARCH_TYPE;

typedef struct {
  UINT32 Data1;
  UINT16 Data2;
  UINT16 Data3;
  UINT8 Data4[8];
} EFI_GUID;

typedef struct {
  UINT64 Signature;
  UINT32 Revision;
  UINT32 HeaderSize;
  UINT32 CRC32;
  UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef struct SIMPLE_TEXT_OUTPUT_PROTOCOL SIMPLE_TEXT_OUTPUT_PROTOCOL;
typedef struct EFI_BOOT_SERVICES EFI_BOOT_SERVICES;
typedef struct EFI_SYSTEM_TABLE EFI_SYSTEM_TABLE;
typedef struct EFI_LOADED_IMAGE_PROTOCOL EFI_LOADED_IMAGE_PROTOCOL;
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
typedef struct KERNEL_BOOT_INFO KERNEL_BOOT_INFO;

typedef EFI_STATUS(EFIAPI *EFI_TEXT_STRING)(SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
                                            CHAR16 *String);

struct SIMPLE_TEXT_OUTPUT_PROTOCOL {
  void *Reset;
  EFI_TEXT_STRING OutputString;
};

typedef EFI_STATUS(EFIAPI *EFI_HANDLE_PROTOCOL)(EFI_HANDLE Handle,
                                                EFI_GUID *Protocol,
                                                void **Interface);
typedef EFI_STATUS(EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    EFI_LOCATE_SEARCH_TYPE SearchType,
    EFI_GUID *Protocol,
    void *SearchKey,
    UINTN *NoHandles,
    EFI_HANDLE **Buffer);
typedef enum {
  AllocateAnyPages = 0,
  AllocateMaxAddress = 1,
  AllocateAddress = 2
} EFI_ALLOCATE_TYPE;
typedef UINT64 EFI_PHYSICAL_ADDRESS;
typedef EFI_STATUS(EFIAPI *EFI_ALLOCATE_PAGES)(EFI_ALLOCATE_TYPE Type,
                                               EFI_MEMORY_TYPE MemoryType,
                                               UINTN Pages,
                                               EFI_PHYSICAL_ADDRESS *Memory);
typedef EFI_STATUS(EFIAPI *EFI_FREE_PAGES)(EFI_PHYSICAL_ADDRESS Memory,
                                           UINTN Pages);
typedef EFI_STATUS(EFIAPI *EFI_ALLOCATE_POOL)(EFI_MEMORY_TYPE PoolType,
                                              UINTN Size,
                                              void **Buffer);
typedef EFI_STATUS(EFIAPI *EFI_FREE_POOL)(void *Buffer);
typedef void(EFIAPI *EFI_COPY_MEM)(void *Destination, void *Source, UINTN Length);
typedef void(EFIAPI *EFI_SET_MEM)(void *Buffer, UINTN Size, UINT8 Value);

struct EFI_BOOT_SERVICES {
  EFI_TABLE_HEADER Hdr;
  void *RaiseTPL;
  void *RestoreTPL;
  EFI_ALLOCATE_PAGES AllocatePages;
  EFI_FREE_PAGES FreePages;
  EFI_GET_MEMORY_MAP GetMemoryMap;
  EFI_ALLOCATE_POOL AllocatePool;
  EFI_FREE_POOL FreePool;
  void *CreateEvent;
  void *SetTimer;
  void *WaitForEvent;
  void *SignalEvent;
  void *CloseEvent;
  void *CheckEvent;
  void *InstallProtocolInterface;
  void *ReinstallProtocolInterface;
  void *UninstallProtocolInterface;
  EFI_HANDLE_PROTOCOL HandleProtocol;
  void *Reserved;
  void *RegisterProtocolNotify;
  void *LocateHandle;
  void *LocateDevicePath;
  void *InstallConfigurationTable;
  void *LoadImage;
  void *StartImage;
  void *Exit;
  void *UnloadImage;
  void *ExitBootServices;
  void *GetNextMonotonicCount;
  void *Stall;
  void *SetWatchdogTimer;
  void *ConnectController;
  void *DisconnectController;
  void *OpenProtocol;
  void *CloseProtocol;
  void *OpenProtocolInformation;
  void *ProtocolsPerHandle;
  void *LocateHandleBuffer;
  void *LocateProtocol;
  void *InstallMultipleProtocolInterfaces;
  void *UninstallMultipleProtocolInterfaces;
  void *CalculateCrc32;
  EFI_COPY_MEM CopyMem;
  EFI_SET_MEM SetMem;
  void *CreateEventEx;
};

struct EFI_SYSTEM_TABLE {
  EFI_TABLE_HEADER Hdr;
  CHAR16 *FirmwareVendor;
  UINT32 FirmwareRevision;
  EFI_HANDLE ConsoleInHandle;
  void *ConIn;
  EFI_HANDLE ConsoleOutHandle;
  SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
  EFI_HANDLE StandardErrorHandle;
  SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
  void *RuntimeServices;
  EFI_BOOT_SERVICES *BootServices;
  UINTN NumberOfTableEntries;
  void *ConfigurationTable;
};

struct EFI_LOADED_IMAGE_PROTOCOL {
  UINT32 Revision;
  EFI_HANDLE ParentHandle;
  EFI_SYSTEM_TABLE *SystemTable;
  EFI_HANDLE DeviceHandle;
  void *FilePath;
  void *Reserved;
  UINT32 LoadOptionsSize;
  void *LoadOptions;
  void *ImageBase;
  UINT64 ImageSize;
  EFI_MEMORY_TYPE ImageCodeType;
  EFI_MEMORY_TYPE ImageDataType;
  EFI_STATUS(EFIAPI *Unload)(EFI_HANDLE ImageHandle);
};

typedef EFI_STATUS(EFIAPI *EFI_OPEN_VOLUME)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                            EFI_FILE_PROTOCOL **Root);

struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
  UINT64 Revision;
  EFI_OPEN_VOLUME OpenVolume;
};

typedef EFI_STATUS(EFIAPI *EFI_FILE_OPEN)(EFI_FILE_PROTOCOL *This,
                                          EFI_FILE_PROTOCOL **NewHandle,
                                          CHAR16 *FileName,
                                          UINT64 OpenMode,
                                          UINT64 Attributes);
typedef EFI_STATUS(EFIAPI *EFI_FILE_CLOSE)(EFI_FILE_PROTOCOL *This);
typedef EFI_STATUS(EFIAPI *EFI_FILE_READ)(EFI_FILE_PROTOCOL *This,
                                          UINTN *BufferSize,
                                          void *Buffer);
typedef EFI_STATUS(EFIAPI *EFI_FILE_GET_INFO)(EFI_FILE_PROTOCOL *This,
                                              EFI_GUID *InformationType,
                                              UINTN *BufferSize,
                                              void *Buffer);

struct EFI_FILE_PROTOCOL {
  UINT64 Revision;
  EFI_FILE_OPEN Open;
  EFI_FILE_CLOSE Close;
  void *Delete;
  EFI_FILE_READ Read;
  void *Write;
  void *GetPosition;
  void *SetPosition;
  EFI_FILE_GET_INFO GetInfo;
  void *SetInfo;
  void *Flush;
};

typedef struct {
  UINT64 Size;
  UINT64 FileSize;
  UINT64 PhysicalSize;
  UINT64 CreateTime[2];
  UINT64 LastAccessTime[2];
  UINT64 ModificationTime[2];
  UINT64 Attribute;
  CHAR16 FileName[1];
} EFI_FILE_INFO;

typedef struct {
  UINT8 Magic[8];
  UINT32 AbiVersion;
  UINT32 HeaderSize;
  UINT64 EntryOffset;
  UINT64 ImageSize;
  UINT64 Flags;
  UINT64 BssSize;   // extra zero-fill bytes the loader must append after ImageSize
} KERNEL_IMAGE_HEADER;

struct KERNEL_BOOT_INFO {
  UINT64 Magic;
  UINT32 Version;
  UINT32 Size;
  UINT64 KernelImageBase;
  UINT64 KernelImageSize;
  EFI_SYSTEM_TABLE *SystemTable;
  EFI_MEMORY_DESCRIPTOR *MemoryMap;
  UINT64 MemoryMapSize;
  UINT64 MemoryMapDescSize;
  UINT64 MemoryMapEntries;
};

typedef void(EFIAPI *KERNEL_ENTRY_POINT)(const KERNEL_BOOT_INFO *BootInfo);

static EFI_GUID gEfiLoadedImageProtocolGuid = {
    0x5B1B31A1U, 0x9562U, 0x11D2U,
    {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID gEfiSimpleFileSystemProtocolGuid = {
    0x964E5B22U, 0x6459U, 0x11D2U,
    {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static EFI_GUID gEfiFileInfoGuid = {
    0x09576E92U, 0x6D3FU, 0x11D2U,
    {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};

static CHAR16 kKernelPath[] = L"\\EFI\\BOOT\\KERNEL.BIN";
static CHAR16 kKernelPathRelative[] = L"EFI\\BOOT\\KERNEL.BIN";
static CHAR16 kKernelPathLocal[] = L"KERNEL.BIN";
static UINT8 kKernelImageMagic[8] = {'S', 'P', 'O', 'S', 'K', 'R', 'N', 'L'};

#define KERNEL_ABI_VERSION 1U
#define KERNEL_BOOT_INFO_MAGIC 0x31544F4F42535053ULL
#define KERNEL_PAGE_SIZE 4096ULL

static UINTN align_up_u64(UINTN value, UINTN alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

static EFI_STATUS write_line(EFI_SYSTEM_TABLE *system_table, CHAR16 *message) {
  if (system_table == 0 || system_table->ConOut == 0 ||
      system_table->ConOut->OutputString == 0) {
    return EFI_INVALID_PARAMETER;
  }

  return system_table->ConOut->OutputString(system_table->ConOut, message);
}

static EFI_STATUS load_kernel_image(EFI_HANDLE image_handle,
                                    EFI_SYSTEM_TABLE *system_table,
                                    void **kernel_buffer,
                                    UINTN *kernel_size) {
  EFI_STATUS status;
  EFI_LOADED_IMAGE_PROTOCOL *loaded_image;
  EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs;
  EFI_FILE_PROTOCOL *root;
  EFI_FILE_PROTOCOL *kernel_file;
  EFI_FILE_INFO *kernel_info;
  EFI_HANDLE *fs_handles;
  UINTN fs_handle_count;
  UINTN i;
  EFI_LOCATE_HANDLE_BUFFER locate_handle_buffer;
  UINTN info_size;
  UINTN read_size;

  if (system_table == 0 || system_table->BootServices == 0 ||
      kernel_buffer == 0 || kernel_size == 0) {
    return EFI_INVALID_PARAMETER;
  }

  *kernel_buffer = 0;
  *kernel_size = 0;
  loaded_image = 0;
  fs = 0;
  root = 0;
  kernel_file = 0;
  kernel_info = 0;
  fs_handles = 0;
  fs_handle_count = 0;
  locate_handle_buffer = 0;

  status = system_table->BootServices->HandleProtocol(
      image_handle, &gEfiLoadedImageProtocolGuid, (void **)&loaded_image);
  if (EFI_ERROR(status) || loaded_image == 0) {
    write_line(system_table, L"Load error: HandleProtocol(LoadedImage) failed.\r\n");
    return EFI_LOAD_ERROR;
  }

  status = system_table->BootServices->HandleProtocol(
      loaded_image->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid,
      (void **)&fs);
  if (!EFI_ERROR(status) && fs != 0) {
    status = fs->OpenVolume(fs, &root);
    if (!EFI_ERROR(status) && root != 0) {
      status = root->Open(root, &kernel_file, kKernelPath, EFI_FILE_MODE_READ, 0);
      if (EFI_ERROR(status) || kernel_file == 0) {
        status = root->Open(root, &kernel_file, kKernelPathRelative, EFI_FILE_MODE_READ, 0);
      }
      if (EFI_ERROR(status) || kernel_file == 0) {
        status = root->Open(root, &kernel_file, kKernelPathLocal, EFI_FILE_MODE_READ, 0);
      }
      if (EFI_ERROR(status) || kernel_file == 0) {
        root->Close(root);
        root = 0;
      }
    }
  }

  if (kernel_file == 0) {
    locate_handle_buffer =
        (EFI_LOCATE_HANDLE_BUFFER)system_table->BootServices->LocateHandleBuffer;
    if (locate_handle_buffer == 0) {
      write_line(system_table, L"Load error: LocateHandleBuffer unavailable.\r\n");
      return EFI_LOAD_ERROR;
    }

    status = locate_handle_buffer(ByProtocol, &gEfiSimpleFileSystemProtocolGuid, 0,
                                  &fs_handle_count, &fs_handles);
    if (EFI_ERROR(status)) {
      write_line(system_table, L"Load error: filesystem handle discovery failed.\r\n");
      return status;
    }
    if (fs_handles == 0 || fs_handle_count == 0) {
      if (fs_handles != 0) {
        system_table->BootServices->FreePool(fs_handles);
      }
      write_line(system_table, L"Load error: no filesystem handles available.\r\n");
      return EFI_NOT_FOUND;
    }

    for (i = 0; i < fs_handle_count; ++i) {
      fs = 0;
      status = system_table->BootServices->HandleProtocol(
          fs_handles[i], &gEfiSimpleFileSystemProtocolGuid, (void **)&fs);
      if (EFI_ERROR(status) || fs == 0) {
        continue;
      }

      status = fs->OpenVolume(fs, &root);
      if (EFI_ERROR(status) || root == 0) {
        continue;
      }

      status = root->Open(root, &kernel_file, kKernelPath, EFI_FILE_MODE_READ, 0);
      if (EFI_ERROR(status) || kernel_file == 0) {
        status = root->Open(root, &kernel_file, kKernelPathRelative, EFI_FILE_MODE_READ, 0);
      }
      if (EFI_ERROR(status) || kernel_file == 0) {
        status = root->Open(root, &kernel_file, kKernelPathLocal, EFI_FILE_MODE_READ, 0);
      }
      if (!EFI_ERROR(status) && kernel_file != 0) {
        break;
      }

      root->Close(root);
      root = 0;
    }

    system_table->BootServices->FreePool(fs_handles);
    fs_handles = 0;
  }

  if (kernel_file == 0 || root == 0) {
    write_line(system_table, L"Load error: KERNEL.BIN not found on any filesystem.\r\n");
    return EFI_NOT_FOUND;
  }

  info_size = 0;
  status = kernel_file->GetInfo(kernel_file, &gEfiFileInfoGuid, &info_size, 0);
  if (status != EFI_BUFFER_TOO_SMALL || info_size == 0) {
    kernel_file->Close(kernel_file);
    root->Close(root);
    return EFI_LOAD_ERROR;
  }

  status = system_table->BootServices->AllocatePool(EfiLoaderData, info_size,
                                                    (void **)&kernel_info);
  if (EFI_ERROR(status) || kernel_info == 0) {
    kernel_file->Close(kernel_file);
    root->Close(root);
    return EFI_OUT_OF_RESOURCES;
  }

  status = kernel_file->GetInfo(kernel_file, &gEfiFileInfoGuid, &info_size,
                                kernel_info);
  if (EFI_ERROR(status)) {
    system_table->BootServices->FreePool(kernel_info);
    kernel_file->Close(kernel_file);
    root->Close(root);
    return EFI_LOAD_ERROR;
  }

  if (kernel_info->FileSize == 0 ||
      kernel_info->FileSize > (UINT64)((UINTN)-1)) {
    system_table->BootServices->FreePool(kernel_info);
    kernel_file->Close(kernel_file);
    root->Close(root);
    return EFI_LOAD_ERROR;
  }

  *kernel_size = (UINTN)kernel_info->FileSize;
  status = system_table->BootServices->AllocatePool(EfiLoaderData, *kernel_size,
                                                    kernel_buffer);
  if (EFI_ERROR(status) || *kernel_buffer == 0) {
    system_table->BootServices->FreePool(kernel_info);
    kernel_file->Close(kernel_file);
    root->Close(root);
    return EFI_OUT_OF_RESOURCES;
  }

  read_size = *kernel_size;
  status = kernel_file->Read(kernel_file, &read_size, *kernel_buffer);
  if (EFI_ERROR(status) || read_size != *kernel_size) {
    system_table->BootServices->FreePool(*kernel_buffer);
    *kernel_buffer = 0;
    *kernel_size = 0;
    system_table->BootServices->FreePool(kernel_info);
    kernel_file->Close(kernel_file);
    root->Close(root);
    return EFI_LOAD_ERROR;
  }

  system_table->BootServices->FreePool(kernel_info);
  kernel_file->Close(kernel_file);
  root->Close(root);
  return EFI_SUCCESS;
}

static int bytes_equal(const UINT8 *left, const UINT8 *right, UINTN size) {
  UINTN i;

  for (i = 0; i < size; ++i) {
    if (left[i] != right[i]) {
      return 0;
    }
  }

  return 1;
}

static EFI_STATUS validate_kernel_image(void *kernel_buffer,
                                        UINTN kernel_size,
                                        KERNEL_ENTRY_POINT *entry_point,
                                        UINTN *bss_size_out) {
  KERNEL_IMAGE_HEADER *header;

  if (kernel_buffer == 0 || entry_point == 0) {
    return EFI_INVALID_PARAMETER;
  }
  if (kernel_size < sizeof(KERNEL_IMAGE_HEADER)) {
    return EFI_LOAD_ERROR;
  }

  header = (KERNEL_IMAGE_HEADER *)kernel_buffer;
  if (!bytes_equal(header->Magic, kKernelImageMagic, sizeof(header->Magic))) {
    return EFI_LOAD_ERROR;
  }
  if (header->AbiVersion != KERNEL_ABI_VERSION) {
    return EFI_LOAD_ERROR;
  }
  if (header->HeaderSize < sizeof(KERNEL_IMAGE_HEADER) ||
      header->HeaderSize > kernel_size) {
    return EFI_LOAD_ERROR;
  }
  if (header->ImageSize != (UINT64)kernel_size ||
      header->ImageSize > (UINT64)((UINTN)-1)) {
    return EFI_LOAD_ERROR;
  }
  if (header->EntryOffset < header->HeaderSize ||
      header->EntryOffset >= header->ImageSize ||
      header->EntryOffset > (UINT64)((UINTN)-1)) {
    return EFI_LOAD_ERROR;
  }

  *entry_point = (KERNEL_ENTRY_POINT)((UINT8 *)kernel_buffer +
                                      (UINTN)header->EntryOffset);
  if (bss_size_out != 0) {
    *bss_size_out = (header->BssSize <= (UINT64)((UINTN)-1))
                    ? (UINTN)header->BssSize : 0;
  }
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *system_table) {
  EFI_STATUS status;
  KERNEL_ENTRY_POINT kernel_entry;
  KERNEL_BOOT_INFO boot_info;
  void *kernel_buffer;
  void *kernel_full;
  UINTN kernel_size;
  UINTN kernel_bss_size;
  UINTN kernel_runtime_size;
  EFI_MEMORY_DESCRIPTOR *mem_map;
  UINTN mem_map_size;
  UINTN mem_map_key;
  UINTN mem_desc_size;
  UINT32 mem_desc_version;

  status = write_line(system_table, L"Secure Personal OS UEFI loader\r\n");
  if (EFI_ERROR(status)) {
    return status;
  }

  status = write_line(system_table, L"Loading \\EFI\\BOOT\\KERNEL.BIN ...\r\n");
  if (EFI_ERROR(status)) {
    return status;
  }

  status = load_kernel_image(image_handle, system_table, &kernel_buffer,
                             &kernel_size);
  if (EFI_ERROR(status)) {
    if (status == EFI_NOT_FOUND) {
      write_line(system_table, L"Kernel load failed: KERNEL.BIN not found.\r\n");
    } else if (status == EFI_OUT_OF_RESOURCES) {
      write_line(system_table, L"Kernel load failed: out of memory.\r\n");
    } else if (status == EFI_INVALID_PARAMETER) {
      write_line(system_table, L"Kernel load failed: invalid parameter.\r\n");
    } else {
      write_line(system_table, L"Kernel load failed: filesystem/read error.\r\n");
    }
    return status;
  }

  kernel_bss_size = 0;
  kernel_runtime_size = kernel_size;
  status = validate_kernel_image(kernel_buffer, kernel_size, &kernel_entry, &kernel_bss_size);
  if (EFI_ERROR(status)) {
    write_line(system_table, L"Kernel header validation failed.\r\n");
    system_table->BootServices->FreePool(kernel_buffer);
    return status;
  }

  // Re-allocate a page-aligned buffer large enough for the binary + BSS,
  // copy the binary, and zero the BSS region so static globals are initialised.
  // AllocatePages guarantees 4 KB alignment — required so that any SSE/aligned
  // data the compiler places in the binary is correctly aligned at runtime.
  kernel_full = 0;
  if (kernel_bss_size > 0) {
    UINTN kernel_total_size = kernel_size + kernel_bss_size;
    UINTN kernel_alloc_size = align_up_u64(kernel_total_size, KERNEL_PAGE_SIZE);
    UINTN kernel_pages = kernel_alloc_size / KERNEL_PAGE_SIZE;
    EFI_PHYSICAL_ADDRESS kernel_full_phys = 0;

    status = system_table->BootServices->AllocatePages(
        AllocateAnyPages, EfiLoaderData, kernel_pages, &kernel_full_phys);
    if (!EFI_ERROR(status) && kernel_full_phys != 0) {
      kernel_full = (void *)(UINTN)kernel_full_phys;
      system_table->BootServices->CopyMem(kernel_full, kernel_buffer, kernel_size);
      system_table->BootServices->SetMem(
          (UINT8 *)kernel_full + kernel_size, kernel_alloc_size - kernel_size, 0);
      system_table->BootServices->FreePool(kernel_buffer);
      kernel_buffer = kernel_full;
      kernel_runtime_size = kernel_alloc_size;
      // Recompute entry point in the new buffer.
      {
        KERNEL_IMAGE_HEADER *hdr = (KERNEL_IMAGE_HEADER *)kernel_buffer;
        kernel_entry = (KERNEL_ENTRY_POINT)(
            (UINT8 *)kernel_buffer + (UINTN)hdr->EntryOffset);
      }
    } else {
      // Fall through without BSS zero — not ideal but non-fatal for PMM.
      kernel_full = 0;
    }
  }

  // Snapshot the UEFI physical memory map so the kernel can manage RAM.
  mem_map_size = 0;
  mem_map_key  = 0;
  mem_desc_size    = 0;
  mem_desc_version = 0;
  mem_map = 0;

  // First call: get required buffer size.
  status = system_table->BootServices->GetMemoryMap(
      &mem_map_size, 0, &mem_map_key, &mem_desc_size, &mem_desc_version);
  if (status == EFI_BUFFER_TOO_SMALL && mem_map_size > 0 && mem_desc_size > 0) {
    // Add slack for the AllocatePool call itself expanding the map.
    mem_map_size += mem_desc_size * 8;
    status = system_table->BootServices->AllocatePool(
        EfiLoaderData, mem_map_size, (void **)&mem_map);
    if (!EFI_ERROR(status) && mem_map != 0) {
      status = system_table->BootServices->GetMemoryMap(
          &mem_map_size, mem_map, &mem_map_key, &mem_desc_size, &mem_desc_version);
      if (EFI_ERROR(status)) {
        system_table->BootServices->FreePool(mem_map);
        mem_map = 0;
        mem_map_size = 0;
      }
    }
  }

  write_line(system_table, L"Kernel image validated.\r\n");
  write_line(system_table, L"Handing off to kernel entrypoint...\r\n");

  boot_info.Magic            = KERNEL_BOOT_INFO_MAGIC;
  boot_info.Version          = KERNEL_ABI_VERSION;
  boot_info.Size             = (UINT32)sizeof(KERNEL_BOOT_INFO);
  boot_info.KernelImageBase  = (UINT64)(UINTN)kernel_buffer;
  boot_info.KernelImageSize  = (UINT64)kernel_runtime_size;
  boot_info.SystemTable      = system_table;
  boot_info.MemoryMap        = mem_map;
  boot_info.MemoryMapSize    = (UINT64)mem_map_size;
  boot_info.MemoryMapDescSize = (UINT64)mem_desc_size;
  boot_info.MemoryMapEntries  = (mem_desc_size > 0)
                                ? (UINT64)(mem_map_size / mem_desc_size) : 0;

  kernel_entry(&boot_info);

  write_line(system_table, L"Kernel returned unexpectedly.\r\n");
  system_table->BootServices->FreePool(kernel_buffer);
  return EFI_LOAD_ERROR;
}
