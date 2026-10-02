// Process-memory and PE primitives shared by the UE5 CVar scanner, installer,
// and console-object registry. Split out of main_ue5_scan.cpp so each unit
// stays inside the source-size ceiling.
#include "main_ue5_internal.h"

namespace UE5::detail {
namespace {

bool IsReadableProtection(DWORD protection) {
  if (protection & (PAGE_GUARD | PAGE_NOACCESS))
    return false;
  switch (protection & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

bool IsWritableProtection(DWORD protection) {
  if (protection & (PAGE_GUARD | PAGE_NOACCESS))
    return false;
  switch (protection & 0xFF) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

bool IsExecutableProtection(DWORD protection) {
  if (protection & (PAGE_GUARD | PAGE_NOACCESS))
    return false;
  switch (protection & 0xFF) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

template <typename Predicate>
bool IsRangeProtected(const void* pointer, std::size_t size, Predicate predicate) {
  if (!pointer || size == 0)
    return false;
  uintptr_t cursor = reinterpret_cast<uintptr_t>(pointer);
  if (cursor > (std::numeric_limits<uintptr_t>::max)() - size)
    return false;
  const uintptr_t end = cursor + size;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT || !predicate(memory.Protect)) {
      return false;
    }
    const uintptr_t regionBase = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    if (regionBase > (std::numeric_limits<uintptr_t>::max)() - memory.RegionSize)
      return false;
    const uintptr_t regionEnd = regionBase + memory.RegionSize;
    if (regionEnd <= cursor)
      return false;
    cursor = (std::min)(regionEnd, end);
  }
  return true;
}

}  // namespace

bool IsReadableRange(const void* pointer, std::size_t size) {
  return IsRangeProtected(pointer, size, IsReadableProtection);
}

bool IsWritableRange(const void* pointer, std::size_t size) {
  return IsRangeProtected(pointer, size, IsWritableProtection);
}

bool IsExecutableAddress(const void* pointer) {
  MEMORY_BASIC_INFORMATION memory{};
  return pointer && VirtualQuery(pointer, &memory, sizeof(memory)) == sizeof(memory) &&
         memory.State == MEM_COMMIT && IsExecutableProtection(memory.Protect);
}

bool HasCallableVtable(const void* object) {
  void* vtable = nullptr;
  void* firstMethod = nullptr;
  return ReadValue(object, vtable) && ReadValue(vtable, firstMethod) && IsExecutableAddress(firstMethod);
}

const char* ModuleBaseName(HMODULE module, char (&path)[MAX_PATH]) {
  std::memset(path, 0, sizeof(path));
  if (!GetModuleFileNameA(module, path, static_cast<DWORD>(std::size(path))))
    return "<unknown>";
  path[std::size(path) - 1] = '\0';
  const char* backslash = std::strrchr(path, '\\');
  const char* slash = std::strrchr(path, '/');
  const char* base = backslash && slash ? (std::max)(backslash, slash) : (backslash ? backslash : slash);
  return base ? base + 1 : path;
}

const SectionView* FindSection(const ModuleView& image, uintptr_t address, std::size_t bytes,
                               DWORD requiredCharacteristics) {
  for (const SectionView& section : image.sections) {
    if ((section.characteristics & requiredCharacteristics) == requiredCharacteristics &&
        section.Contains(address, bytes)) {
      return &section;
    }
  }
  return nullptr;
}

bool BuildModuleView(HMODULE module, ModuleView& image) {
  MODULEINFO info{};
  if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)) || !info.lpBaseOfDll ||
      info.SizeOfImage < sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS)) {
    return false;
  }

  image.module = module;
  image.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
  image.size = info.SizeOfImage;
  if (image.base > (std::numeric_limits<uintptr_t>::max)() - image.size)
    return false;
  const uintptr_t imageEnd = image.base + image.size;
  if (!IsReadableRange(info.lpBaseOfDll, sizeof(IMAGE_DOS_HEADER)))
    return false;

  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
  if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
      static_cast<std::size_t>(dos->e_lfanew) > image.size - sizeof(IMAGE_NT_HEADERS)) {
    return false;
  }

  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image.base + dos->e_lfanew);
  if (!IsReadableRange(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
      nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
      nt->FileHeader.NumberOfSections == 0 || nt->FileHeader.NumberOfSections > 96) {
    return false;
  }

  const uintptr_t optionalHeaderAddress = reinterpret_cast<uintptr_t>(&nt->OptionalHeader);
  if (optionalHeaderAddress < image.base || optionalHeaderAddress > imageEnd ||
      nt->FileHeader.SizeOfOptionalHeader > imageEnd - optionalHeaderAddress) {
    return false;
  }
  const uintptr_t sectionHeaderAddress = optionalHeaderAddress + nt->FileHeader.SizeOfOptionalHeader;
  const std::size_t sectionHeaderBytes =
      static_cast<std::size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
  if (sectionHeaderAddress < image.base || sectionHeaderAddress > imageEnd ||
      sectionHeaderBytes > imageEnd - sectionHeaderAddress ||
      !IsReadableRange(reinterpret_cast<const void*>(sectionHeaderAddress), sectionHeaderBytes)) {
    return false;
  }
  const auto* sectionHeaders = reinterpret_cast<const IMAGE_SECTION_HEADER*>(sectionHeaderAddress);

  image.sections.reserve(nt->FileHeader.NumberOfSections);
  for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
    const IMAGE_SECTION_HEADER& header = sectionHeaders[index];
    if (header.VirtualAddress >= image.size)
      continue;
    const std::size_t requestedSize = header.Misc.VirtualSize != 0
                                          ? static_cast<std::size_t>(header.Misc.VirtualSize)
                                          : static_cast<std::size_t>(header.SizeOfRawData);
    const std::size_t sectionSize =
        (std::min)(requestedSize, image.size - static_cast<std::size_t>(header.VirtualAddress));
    if (sectionSize == 0)
      continue;
    const uintptr_t sectionBegin = image.base + header.VirtualAddress;
    if (IsReadableRange(reinterpret_cast<const void*>(sectionBegin), sectionSize))
      image.sections.push_back({sectionBegin, sectionSize, static_cast<DWORD>(header.Characteristics)});
  }
  return !image.sections.empty();
}

}  // namespace UE5::detail
