/* From wine1.2-1.1.42/dlls/ntdll/loader.c  */

/*
 * Loader functions
 *
 * Copyright 1995, 2003 Alexandre Julliard
 * Copyright 2002 Dmitry Timoshkov for CodeWeavers
 * Copyright 2010 g10 Code GmbH
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */



#include <assert.h>

#undef USE_DLMALLOC
#ifdef USE_DLMALLOC
#include "dlmalloc.h"
#endif

#include "wine.h"

/* convert PE image VirtualAddress to Real Address */
static void *get_rva( HMODULE module, DWORD va )
{
  return (void *)((char *)module + va);
}


#define USE_HIMEMCE_MAP
#ifdef USE_HIMEMCE_MAP
/* Support for DLL loading.  */

#include "himemce-map.h"

static int himemce_map_initialized;
static struct himemce_map *himemce_map;
int himemce_mod_loaded[HIMEMCE_MAP_MAX_MODULES];

void
himemce_invoke_dll_mains (DWORD reason, LPVOID reserved)
{
  int i;

  if (! himemce_map)
    return NULL;

  for (i = 0; i < himemce_map->nr_modules; i++)
    if (himemce_mod_loaded[modidx])
      {
	char *ptr = himemce_map->module[i].base;
	IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)ptr;
	IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(ptr + dos->e_lfanew);
	BOOL WINAPI (*dllmain) (HINSTANCE, DWORD, LPVOID);
	BOOL res;

	dllmain = nt->OptionalHeader.AddressOfEntryPoint;
	res = (*dllmain) (ptr, reason, reserved);
	if (reason == DLL_PROCESS_ATTACH && !res)
	  {
	    ERR ("attaching %s failed (ignored)", himemce_map->module[i].name);
	  }
      }
}


static void
himemce_map_init ()
{
  void *ptr;
  /* Only try once.  */
  if (himemce_map_initialized)
    return;
  himemce_map_initialized = 1;
  himemce_map = himemce_map_open ();
  if (! himemce_map)
    {
      TRACE ("can not open himemce map\n");
      return;
    }
  TRACE ("himemce map found at %p (reserving 0x%x bytes at %p)\n", himemce_map,
	 himemce_map->low_start, himemce_map->low_size);
  ptr = VirtualAlloc(himemce_map->low_start, himemce_map->low_size,
		     MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (! ptr)
    {
      TRACE ("failed to reserve memory: %i\n", GetLastError ());
      himemce_map_close (himemce_map);
      himemce_map = NULL;
      return;
    }

  himemce_set_dllmain_cb (himemce_invoke_dll_mains);
}


# define page_mask  0xfff
# define page_shift 12
# define page_size  0x1000

#define ROUND_SIZE(size)			\
  (((SIZE_T)(size) + page_mask) & ~page_mask)

static SIZE_T
section_size (IMAGE_SECTION_HEADER *sec)
{
  static const SIZE_T sector_align = 0x1ff;
  SIZE_T map_size, file_size, end;
  
  if (!sec->Misc.VirtualSize)
    map_size = ROUND_SIZE( sec->SizeOfRawData );
  else
    map_size = ROUND_SIZE( sec->Misc.VirtualSize );
  
  file_size = (sec->SizeOfRawData + (sec->PointerToRawData & sector_align)
	       + sector_align) & ~sector_align;
  if (file_size > map_size) file_size = map_size;
  end = ROUND_SIZE( file_size );
  if (end > map_size) end = map_size;
  return end;
}


/* Returns the base of the module after loading it, if necessary.
   NULL if not found, -1 if a fatal error occurs.  */
void *
himemce_map_load_dll (const char *name)
{
  struct himemce_module *mod;
  int modidx;
  char *ptr;
  IMAGE_DOS_HEADER *dos;
  IMAGE_NT_HEADERS *nt;
  IMAGE_SECTION_HEADER *sec;
  int sec_cnt;
  int idx;
  const IMAGE_IMPORT_DESCRIPTOR *imports;
  DWORD imports_size;
  
  himemce_map_init ();
  if (! himemce_map)
    return NULL;
  
  mod = himemce_map_find_module (himemce_map, name);
  if (!mod)
    return NULL;
  modidx = mod - himemce_map->module;
  if (himemce_mod_loaded[modidx])
    return mod->base;
  
  /* First map the sections low.  */
  ptr = mod->base;
  dos = (IMAGE_DOS_HEADER *) ptr;
  nt = (IMAGE_NT_HEADERS *) (ptr + dos->e_lfanew);
  sec = (IMAGE_SECTION_HEADER *) ((char*) &nt->OptionalHeader
                                  + nt->FileHeader.SizeOfOptionalHeader);
  sec_cnt = nt->FileHeader.NumberOfSections;
  for (idx = 0; idx < sec_cnt; idx++)
    {
      size_t secsize;
      char *secptr;
      
      if (! sec[idx].PointerToLinenumbers)
	continue;
      secsize = section_size (&sec[idx]);
      secptr = VirtualAlloc ((void *) sec[idx].PointerToLinenumbers,
			     secsize, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      if (! secptr)
	{
	  TRACE ("could not allocate 0x%x bytes of low memory at %p: %i\n",
		 secsize, sec[idx].PointerToLinenumbers, GetLastError ());
	  return (void *) -1;
	}
      memcpy (secptr, ptr + sec[idx].VirtualAddress, secsize);
    }
  
  /* To break circles, we claim that we loaded before recursing.  */
  himemce_mod_loaded[modidx]++;
  imports = MyRtlImageDirectoryEntryToData ((HMODULE) ptr, TRUE,
                                            IMAGE_DIRECTORY_ENTRY_IMPORT,
                                            &imports_size);
  if (imports)
    {
      idx = 0;
      while (imports[idx].Name && imports[idx].FirstThunk)
	{
	  char *iname = ptr + imports[idx].Name;
	  void *ibase;
	  
	  /* Recursion!  */
	  ibase = himemce_map_load_dll (iname);
	  if (ibase == (void *) -1)
	    return (void *) -1;
	  /* Nothing to do if ibase !=0: Successful loading of high DLL.  */
	  if (ibase == 0)
	    {
	      ibase = LoadLibrary (iname);
	      if (!ibase)
		{
		  TRACE ("Could not find %s, dependency of %s\n", iname, name);
		  return (void *) -1;
		}
	    }
	  idx++;
	}
    }
  return ptr;
}


static void *
get_rva_low (char *module, size_t rva)
{
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)module;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(module + dos->e_lfanew);
  IMAGE_SECTION_HEADER *sec;
  int sec_cnt;
  int idx;
  
  sec = (IMAGE_SECTION_HEADER*)((char*)&nt->OptionalHeader
				+ nt->FileHeader.SizeOfalHeader);
  sec_cnt = nt->FileHeader.NumberOfSections;

  for (idx = 0; idx < sec_cnt; idx++)
    {
      if (! sec[idx].PointerToLinenumbers)
        continue;
      if (rva >= sec[idx].VirtualAddress
          && rva < sec[idx].VirtualAddress + section_size (&sec[idx]))
	break;
    }
  if (idx == sec_cnt)
    return (void *)((char *)module + rva);
  
  return (void *)((char *)sec[idx].PointerToLinenumbers
                  + (rva - sec[idx].VirtualAddress));
}


static FARPROC
find_ordinal_export (void *module, const IMAGE_EXPORT_DIRECTORY *exports,
                     DWORD exp_size, DWORD ordinal, LPCWSTR load_path)
{
  FARPROC proc;
  const DWORD *functions = get_rva (module, exports->AddressOfFunctions);
  
  if (ordinal >= exports->NumberOfFunctions)
    {
      TRACE(" ordinal %d out of range!\n", ordinal + exports->Base );
      return NULL;
    }
  if (!functions[ordinal]) return NULL;
  
#if 0
  /* if the address falls into the export dir, it's a forward */
  if (((const char *)proc >= (const char *)exports) &&
      ((const char *)proc < (const char *)exports + exp_size))
    return find_forwarded_export( module, (const char *)proc, load_path );
#endif

  proc = get_rva_low (module, functions[ordinal]);
  return proc;
}

static FARPROC
find_named_export (void *module, const IMAGE_EXPORT_DIRECTORY *exports,
                   DWORD exp_size, const char *name, int hint,
		   LPCWSTR load_path)
{
  const WORD *ordinals = get_rva (module, exports->AddressOfNameOrdinals);
  const DWORD *names = get_rva (module, exports->AddressOfNames);
  int min = 0, max = exports->NumberOfNames - 1;
  
  /* first check the hint */
  if (hint >= 0 && hint <= max)
    {
      char *ename = get_rva( module, names[hint] );
      if (!strcmp( ename, name ))
        return find_ordinal_export( module, exports, exp_size,
				    ordinals[hint], load_path);
    }
  
  /* then do a binary search */
  while (min <= max)
    {
      int res, pos = (min + max) / 2;
      char *ename = get_rva( module, names[pos] );
      if (!(res = strcmp( ename, name )))
        return find_ordinal_export( module, exports, exp_size,
				    ordinals[pos], load_path);
      if (res > 0) max = pos - 1;
      else min = pos + 1;
    }
  return NULL;
}

#endif


PIMAGE_NT_HEADERS MyRtlImageNtHeader(HMODULE hModule)
{
  IMAGE_NT_HEADERS *ret;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)hModule;
  
  ret = NULL;
  if (dos->e_magic == IMAGE_DOS_SIGNATURE)
    {
      ret = (IMAGE_NT_HEADERS *)((char *)dos + dos->e_lfanew);
      if (ret->Signature != IMAGE_NT_SIGNATURE) ret = NULL;
    }
  return ret;
}


/* internal representation of 32bit modules. per process. */
typedef struct _wine_modref
{
    LDR_MODULE            ldr;
    int                   nDeps;
    struct _wine_modref **deps;
} WINE_MODREF;

/* FIXME: cmp with himemce-map.h */
#define MAX_MODREFS 64
WINE_MODREF *modrefs[MAX_MODREFS];
int nr_modrefs;


static WINE_MODREF *current_modref;


/* convert from straight ASCII to Unicode without depending on the current codepage */
static void ascii_to_unicode( WCHAR *dst, const char *src, size_t len )
{
  while (len--) *dst++ = (unsigned char)*src++;
}


/***********************************************************************
 *           RtlImageDirectoryEntryToData   (NTDLL.@)
 */
PVOID MyRtlImageDirectoryEntryToData( HMODULE module, BOOL image, WORD dir, ULONG *size )
{
  const IMAGE_NT_HEADERS *nt;
  DWORD addr;

  if ((ULONG_PTR)module & 1)  /* mapped as data file */
    {
      module = (HMODULE)((ULONG_PTR)module & ~1);
      image = FALSE;
    }
  if (!(nt = MyRtlImageNtHeader( module ))) return NULL;
  if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
      const IMAGE_NT_HEADERS64 *nt64 = (const IMAGE_NT_HEADERS64 *)nt;

      if (dir >= nt64->OptionalHeader.NumberOfRvaAndSizes) return NULL;
      if (!(addr = nt64->OptionalHeader.DataDirectory[dir].VirtualAddress)) return NULL;
      *size = nt64->OptionalHeader.DataDirectory[dir].Size;
      if (image || addr < nt64->OptionalHeader.SizeOfHeaders) return (char *)module + addr;
    }
  else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC)
    {
      const IMAGE_NT_HEADERS32 *nt32 = (const IMAGE_NT_HEADERS32 *)nt;

      if (dir >= nt32->OptionalHeader.NumberOfRvaAndSizes) return NULL;
      if (!(addr = nt32->OptionalHeader.DataDirectory[dir].VirtualAddress)) return NULL;
      *size = nt32->OptionalHeader.DataDirectory[dir].Size;
      if (image || addr < nt32->OptionalHeader.SizeOfHeaders) return (char *)module + addr;
    }
  else return NULL;

#if 0
  /* not mapped as image, need to find the section containing the virtual address */
  return RtlImageRvaToVa( nt, module, addr, NULL );
#else
  return NULL;
#endif
}


#define allocate_stub(x,y) (0xdeadbeef)

/*************************************************************************
 *              import_dll
 *
 * Import the dll specified by the given import descriptor.
 * The loader_section must be locked while calling this function.
 */
static WINE_MODREF *import_dll( HMODULE module, const IMAGE_IMPORT_DESCRIPTOR *descr, LPCWSTR load_path )
{
  NTSTATUS status = STATUS_SUCCESS;
  //  WINE_MODREF *wmImp;
  HMODULE imp_mod;
#ifdef USE_HIMEMCE_MAP
  void *imp_base = 0;
  const IMAGE_EXPORT_DIRECTORY *exports;
  DWORD exp_size;
#endif
  const IMAGE_THUNK_DATA *import_list;
  IMAGE_THUNK_DATA *thunk_list;
  WCHAR buffer[32];
  const char *name = get_rva( module, descr->Name );
  DWORD len = strlen(name);
#if 0
  PVOID protect_base;
  SIZE_T protect_size = 0;
  DWORD protect_old;
#endif
#ifdef USE_DLMALLOC
  int iscoredll = 0;
#endif

  thunk_list = get_rva( module, (DWORD)descr->FirstThunk );
  if (descr->OriginalFirstThunk)
    import_list = get_rva( module, (DWORD)descr->OriginalFirstThunk );
  else
    import_list = thunk_list;

  while (len && name[len-1] == ' ') len--;  /* remove trailing spaces */

#ifdef USE_DLMALLOC
  if (! _stricmp (name, "coredll.dll"))
    iscoredll = 1;
#endif

#ifdef USE_HIMEMCE_MAP
  imp_base = himemce_map_load_dll (name);
  if (imp_base == (void *) -1)
    status = GetLastError ();
  if (imp_base)
    goto loaded;
#endif

  if (len * sizeof(WCHAR) < sizeof(buffer))
    {
      ascii_to_unicode( buffer, name, len );
      buffer[len] = 0;
      //      status = load_dll( load_path, buffer, 0, &wmImp );
      imp_mod = LoadLibrary (buffer);
	  if (imp_mod == INVALID_HANDLE_VALUE)
	    status = GetLastError ();
  }
  else  /* need to allocate a larger buffer */
    {
      WCHAR *ptr = malloc ((len + 1) * sizeof(WCHAR) );
      if (!ptr) return NULL;
      ascii_to_unicode( ptr, name, len );
      ptr[len] = 0;
      // status = load_dll( load_path, ptr, 0, &wmImp );
	  imp_mod = LoadLibrary (ptr);
	  if (imp_mod == INVALID_HANDLE_VALUE)
	    status = GetLastError ();
	  free (ptr);
    }
#ifdef USE_HIMEMCE_MAP
loaded:
#endif
  if (status)
    {
      if (status == STATUS_DLL_NOT_FOUND)
	TRACE("Library %s (which is needed by %s) not found\n",
	    name, current_modref->ldr.FullDllName);
      else
	TRACE("Loading library %s (which is needed by %s) failed (error %x).\n",
	    name, current_modref->ldr.FullDllName, status);
      return NULL;
    }
  
#if 0
  /* unprotect the import address table since it can be located in
   * readonly section */
  while (import_list[protect_size].u1.Ordinal) protect_size++;
  protect_base = thunk_list;
  protect_size *= sizeof(*thunk_list);
  NtProtectVirtualMemory( NtCurrentProcess(), &protect_base,
			  &protect_size, PAGE_WRITECOPY, &protect_old );
#endif

#ifdef USE_HIMEMCE_MAP
 if (imp_base)
 {
   exports = MyRtlImageDirectoryEntryToData( imp_base, TRUE, IMAGE_DIRECTORY_ENTRY_EXPORT, &exp_size );

  if (!exports)
    {
      /* set all imported function to deadbeef */
      while (import_list->u1.Ordinal)
        {
	  if (IMAGE_SNAP_BY_ORDINAL(import_list->u1.Ordinal))
            {
	      int ordinal = IMAGE_ORDINAL(import_list->u1.Ordinal);
	      TRACE("No implementation for %s.%d", name, ordinal );
	      thunk_list->u1.Function = (PDWORD)(ULONG_PTR)allocate_stub( name, IntToPtr(ordinal) );
            }
	  else
            {
	      IMAGE_IMPORT_BY_NAME *pe_name = get_rva( module, (DWORD)import_list->u1.AddressOfData );
	      TRACE("No implementation for %s.%s", name, pe_name->Name );
	      thunk_list->u1.Function = (PDWORD)(ULONG_PTR)allocate_stub( name, (const char*)pe_name->Name );
            }
	  TRACE(" imported from %s, allocating stub %p\n",
	       current_modref->ldr.FullDllName,
	       (void *)thunk_list->u1.Function );
	  import_list++;
	  thunk_list++;
        }
      goto done;
    }
 }
#endif

  while (import_list->u1.Ordinal)
    {
      if (IMAGE_SNAP_BY_ORDINAL(import_list->u1.Ordinal))
        {
	  int ordinal = IMAGE_ORDINAL(import_list->u1.Ordinal);

#ifdef USE_HIMEMCE_MAP
	  if (imp_base)
		thunk_list->u1.Function = (PDWORD)(ULONG_PTR)find_ordinal_export( imp_base, exports, exp_size,
	                                                              ordinal - exports->Base, load_path );
	  else
#endif

#ifdef USE_DLMALLOC
	  if (iscoredll)
	    {
#define COREDLL_MALLOC 1041
#define COREDLL_CALLOC 1346
#define COREDLL_FREE 1018
#define COREDLL_REALLOC 1054

	      if (ordinal == COREDLL_MALLOC)
		thunk_list->u1.Function = (PWORD) dlmalloc;
	      else if (ordinal == COREDLL_CALLOC)
		thunk_list->u1.Function = (PWORD) dlcalloc;
	      else if (ordinal == COREDLL_FREE)
		thunk_list->u1.Function = (PWORD) dlfree;
	      else if (ordinal == COREDLL_REALLOC)
		thunk_list->u1.Function = (PWORD) dlrealloc;
	      else
		thunk_list->u1.Function = (PWORD)(ULONG_PTR)GetProcAddress (imp_mod, (void *) (ordinal & 0xffff));
	    }
	  else
#endif
	    thunk_list->u1.Function = (PDWORD)(ULONG_PTR)GetProcAddress (imp_mod, (void *) (ordinal & 0xffff));

	  if (!thunk_list->u1.Function)
            {
	      thunk_list->u1.Function = (PDWORD) allocate_stub( name, IntToPtr(ordinal) );
	      TRACE("No implementation for %s.%d imported from %s, setting to %p\n",
		    name, ordinal, current_modref->ldr.FullDllName,
		    (void *)thunk_list->u1.Function );
            }
	  TRACE("--- Ordinal %s.%d = %p\n", name, ordinal, (void *)thunk_list->u1.Function );
        }
      else  /* import by name */
        {
	  IMAGE_IMPORT_BY_NAME *pe_name;
	  const char *symname;
	  pe_name = get_rva( module, (DWORD)import_list->u1.AddressOfData );
	  symname = (const char*)pe_name->Name;

#ifdef USE_HIMEMCE_MAP
	  if (imp_base)
		  thunk_list->u1.Function = (PDWORD)(ULONG_PTR)find_named_export( imp_base, exports, exp_size,
	  								  (const char*)pe_name->Name,
	  								  pe_name->Hint, load_path );
	  else
#endif
#ifdef USE_DLMALLOC
	  if (iscoredll)
	    {
	      if (! strcmp (symname, "malloc"))
		thunk_list->u1.Function = (PWORD) dlmalloc;
	      else if (! strcmp (symname, "calloc"))
		thunk_list->u1.Function = (PWORD) dlcalloc;
	      else if (! strcmp (symname, "free"))
		thunk_list->u1.Function = (PWORD) dlfree;
	      else if (! strcmp (symname, "realloc"))
		thunk_list->u1.Function = (PWORD) dlrealloc;
	      else
		thunk_list->u1.Function = (PDWORD)(ULONG_PTR)GetProcAddressA (imp_mod, symname);
	    }
	  else
#endif
	    thunk_list->u1.Function = (PDWORD)(ULONG_PTR)GetProcAddressA (imp_mod, symname);
	  if (!thunk_list->u1.Function)
            {
	      thunk_list->u1.Function = (PDWORD) allocate_stub (name, symname);
	      TRACE("No implementation for %s.%s imported from %s, setting to %p\n",
		    name, symname, current_modref->ldr.FullDllName,
		    (void *)thunk_list->u1.Function );
            }
	  TRACE("--- %s %s.%d = %p\n",
		symname, name, pe_name->Hint, (void *)thunk_list->u1.Function);
        }
      import_list++;
      thunk_list++;
    }
done:
#if 0
  /* restore old protection of the import address table */
  NtProtectVirtualMemory( NtCurrentProcess(), &protect_base, &protect_size, protect_old, NULL );
  return wmImp;
#endif
  return (void*)1;
}



/****************************************************************
 *       fixup_imports
 *
 * Fixup all imports of a given module.
 * The loader_section must be locked while calling this function.
 */
static NTSTATUS fixup_imports( WINE_MODREF *wm, LPCWSTR load_path )
{
  int i, nb_imports;
  const IMAGE_IMPORT_DESCRIPTOR *imports;
  WINE_MODREF *prev;
  DWORD size;
  NTSTATUS status;
  //  ULONG_PTR cookie;

  if (!(wm->ldr.Flags & LDR_DONT_RESOLVE_REFS)) return STATUS_SUCCESS;  /* already done */
  wm->ldr.Flags &= ~LDR_DONT_RESOLVE_REFS;
  
  if (!(imports = MyRtlImageDirectoryEntryToData( wm->ldr.BaseAddress, TRUE,
						  IMAGE_DIRECTORY_ENTRY_IMPORT, &size )))
    return STATUS_SUCCESS;
  
  nb_imports = 0;
  while (imports[nb_imports].Name && imports[nb_imports].FirstThunk) nb_imports++;
  
  if (!nb_imports) return STATUS_SUCCESS;  /* no imports */
  
#if 0
  if (!create_module_activation_context( &wm->ldr ))
    RtlActivateActivationContext( 0, wm->ldr.ActivationContext, &cookie );
#endif
  
#if 0
  /* Allocate module dependency list */
  wm->nDeps = nb_imports;
  wm->deps  = RtlAllocateHeap( GetProcessHeap(), 0, nb_imports*sizeof(WINE_MODREF *) );
#endif

  /* load the imported modules. They are automatically
   * added to the modref list of the process.
   */
  prev = current_modref;
  current_modref = wm;
  status = STATUS_SUCCESS;
  for (i = 0; i < nb_imports; i++)
    {
      //      if (!(wm->deps[i] = import_dll( wm->ldr.BaseAddress, &imports[i], load_path )))
      if (! import_dll( wm->ldr.BaseAddress, &imports[i], load_path ))
	status = STATUS_DLL_NOT_FOUND;
    }
  current_modref = prev;
  //  if (wm->ldr.ActivationContext) RtlDeactivateActivationContext( 0, cookie );

#ifdef USE_HIMEMCE_MAP
  himemce_invoke_dll_mains (DLL_PROCESS_ATTACH, NULL);
#endif

  return status;
}


static BOOL is_dll_native_subsystem( HMODULE module, const IMAGE_NT_HEADERS *nt, LPCWSTR filename )
{
	return FALSE;
}


/*************************************************************************
 *              get_modref
 *
 * Looks for the referenced HMODULE in the current process
 * The loader_section must be locked while calling this function.
 */
static WINE_MODREF *get_modref( HMODULE hmod )
{
  int i;
  for (i = 0; i < nr_modrefs; i++)
    if (modrefs[i]->ldr.BaseAddress == hmod)
      return modrefs[i];
  return NULL;
}



static WINE_MODREF *alloc_module( HMODULE hModule, LPCWSTR filename )
{
    WINE_MODREF *wm;
    const WCHAR *p;
    const IMAGE_NT_HEADERS *nt = MyRtlImageNtHeader(hModule);
#if 0
    PLIST_ENTRY entry, mark;
#endif

    if (!(wm = malloc (sizeof(*wm)))) return NULL;

    wm->nDeps    = 0;
    wm->deps     = NULL;

    wm->ldr.BaseAddress   = hModule;
    wm->ldr.EntryPoint    = NULL;
    wm->ldr.SizeOfImage   = nt->OptionalHeader.SizeOfImage;
    wm->ldr.Flags         = LDR_DONT_RESOLVE_REFS;
    wm->ldr.LoadCount     = 1;
    wm->ldr.TlsIndex      = -1;
    wm->ldr.SectionHandle = NULL;
    wm->ldr.CheckSum      = 0;
    wm->ldr.TimeDateStamp = 0;
    wm->ldr.ActivationContext = 0;

    wcscpy (wm->ldr.FullDllName, filename);
    if ((p = wcsrchr( wm->ldr.FullDllName, L'\\' ))) p++;
    else p = wm->ldr.FullDllName;
    wcscpy (wm->ldr.BaseDllName, p );

    if ((nt->FileHeader.Characteristics & IMAGE_FILE_DLL) && !is_dll_native_subsystem( hModule, nt, p ))
    {
        wm->ldr.Flags |= LDR_IMAGE_IS_DLL;
        if (nt->OptionalHeader.AddressOfEntryPoint)
            wm->ldr.EntryPoint = (char *)hModule + nt->OptionalHeader.AddressOfEntryPoint;
    }

#if 0
    InsertTailList(&NtCurrentTeb()->Peb->LdrData->InLoadOrderModuleList,
                   &wm->ldr.InLoadOrderModuleList);

    /* insert module in MemoryList, sorted in increasing base addresses */
    mark = &NtCurrentTeb()->Peb->LdrData->InMemoryOrderModuleList;
    for (entry = mark->Flink; entry != mark; entry = entry->Flink)
    {
        if (CONTAINING_RECORD(entry, LDR_MODULE, InMemoryOrderModuleList)->BaseAddress > wm->ldr.BaseAddress)
            break;
    }
    entry->Blink->Flink = &wm->ldr.InMemoryOrderModuleList;
    wm->ldr.InMemoryOrderModuleList.Blink = entry->Blink;
    wm->ldr.InMemoryOrderModuleList.Flink = entry;
    entry->Blink = &wm->ldr.InMemoryOrderModuleList;

    /* wait until init is called for inserting into this list */
    wm->ldr.InInitializationOrderModuleList.Flink = NULL;
    wm->ldr.InInitializationOrderModuleList.Blink = NULL;
#endif

    modrefs[nr_modrefs++] = wm;

    return wm;
}


static NTSTATUS load_native_dll( LPCWSTR load_path, LPCWSTR name, HANDLE file,
                                 DWORD flags, WINE_MODREF** pwm )
{
  void *module;
  HANDLE mapping;
  LARGE_INTEGER size;
  SIZE_T len = 0;
  WINE_MODREF *wm;
  NTSTATUS status;

  TRACE("Trying native dll %S\n", name);
  
  size.QuadPart = 0;
  status = MyNtCreateSection( &mapping, STANDARD_RIGHTS_REQUIRED | SECTION_QUERY | SECTION_MAP_READ,
			      NULL, &size, PAGE_READONLY, SEC_IMAGE, file );
  if (status != STATUS_SUCCESS) return status;
  
  module = NULL;
  status = MyNtMapViewOfSection( mapping, NtCurrentProcess(),
				 &module, 0, 0, &size, &len, ViewShare, 0, PAGE_READONLY );
  CloseHandle( mapping );
  if (status < 0) return status;
  
  /* create the MODREF */
  
  if (!(wm = alloc_module( module, name ))) return STATUS_NO_MEMORY;
  
  /* fixup imports */
  
  if (!(flags & DONT_RESOLVE_DLL_REFERENCES))
    {
#if 1
      return (STATUS_NOT_IMPLEMENTED);
#else
      if ((status = fixup_imports( wm, load_path )) != STATUS_SUCCESS)
        {
#if 0
	  /* the module has only be inserted in the load & memory order lists */
	  RemoveEntryList(&wm->ldr.InLoadOrderModuleList);
	  RemoveEntryList(&wm->ldr.InMemoryOrderModuleList);
	  
	  /* FIXME: there are several more dangling references
	   * left. Including dlls loaded by this dll before the
	   * failed one. Unrolling is rather difficult with the
	   * current structure and we can leave them lying
	   * around with no problems, so we don't care.
	   * As these might reference our wm, we don't free it.
	   */
#endif
	  return status;
        }
#endif
    }
  
  TRACE( "Loaded %S at %p: native\n", wm->ldr.FullDllName, module );
  
  wm->ldr.LoadCount = 1;
  *pwm = wm;
  return STATUS_SUCCESS;
}


static NTSTATUS find_dll_file( const WCHAR *load_path, const WCHAR *libname,
                               WCHAR *filename, ULONG *size, WINE_MODREF **pwm, HANDLE *handle )
{
  HMODULE hnd = GetModuleHandle (NULL);
  int len;
  
  assert (handle);
  assert (*size == MAX_PATH);
  
  if (libname[0] == L'/' || libname[0] == L'\\')
    {
      wcscpy (filename, libname);
    }
  else
    {
      len = GetModuleFileName (hnd, filename, MAX_PATH);
      filename[len++] = L'\\';
      wcscpy (&filename[len], libname);
    }
  TRACE( "opening %S\n", filename);
  
  if (handle)
    {
      *handle = CreateFile( filename, GENERIC_READ, FILE_SHARE_READ, NULL,
			    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
      TRACE ("find_dll_file: 0x%p (0x%x)\n", *handle, GetFileSize (*handle, NULL));
    }
  return STATUS_SUCCESS;
}


static NTSTATUS load_dll( LPCWSTR load_path, LPCWSTR libname, DWORD flags, WINE_MODREF** pwm )
{
    WCHAR filename[MAX_PATH];
    ULONG size;
    HANDLE handle = 0;
    NTSTATUS nts;

    TRACE( "looking for %S in %S\n", libname, load_path ? load_path : L"default path" );

    *pwm = NULL;
    size = MAX_PATH;
    find_dll_file( load_path, libname, filename, &size, pwm, &handle );
    
    if (!handle)
      nts = STATUS_DLL_NOT_FOUND;
    else
      nts = load_native_dll( load_path, filename, handle, flags, pwm );
    
    if (nts == STATUS_SUCCESS)
      {
        /* Initialize DLL just loaded */
        TRACE("Loaded module %S at %p\n", filename, (*pwm)->ldr.BaseAddress);
        if (handle)
	  CloseHandle( handle );
        return nts;
      }
    
    TRACE("Failed to load module %S; status=%x\n", libname, nts);
    if (handle)
      CloseHandle( handle );
    return nts;
}


NTSTATUS MyLdrLoadDll(LPCWSTR path_name, DWORD flags,
		      LPCWSTR libname, HMODULE* hModule)
{
  WINE_MODREF *wm;
  NTSTATUS nts;

  /* Support for dll path removed.  */
  nts = load_dll( path_name, libname, flags, &wm );

  /* For now.  */
  assert (wm->ldr.Flags & LDR_DONT_RESOLVE_REFS);
#if 0
  if (nts == STATUS_SUCCESS && !(wm->ldr.Flags & LDR_DONT_RESOLVE_REFS))
    {
      nts = process_attach( wm, NULL );
      if (nts != STATUS_SUCCESS)
        {
	  LdrUnloadDll(wm->ldr.BaseAddress);
	  wm = NULL;
        }
    }
#endif
  *hModule = (wm) ? wm->ldr.BaseAddress : NULL;

  return nts;
}



/***********************************************************************
 *           LdrProcessRelocationBlock  (NTDLL.@)
 *
 * Apply relocations to a given page of a mapped PE image.
 */
IMAGE_BASE_RELOCATION * MyLdrProcessRelocationBlock( void *page, UINT count,
						     USHORT *relocs, INT_PTR delta )
{
  while (count--)
    {
      USHORT offset = *relocs & 0xfff;
      int type = *relocs >> 12;
      switch(type)
        {
        case IMAGE_REL_BASED_ABSOLUTE:
	  break;
#if 1
        case IMAGE_REL_BASED_HIGH:
	  *(short *)((char *)page + offset) += HIWORD(delta);
	  break;
        case IMAGE_REL_BASED_LOW:
	  *(short *)((char *)page + offset) += LOWORD(delta);
	  break;
        case IMAGE_REL_BASED_HIGHLOW:
	  *(int *)((char *)page + offset) += delta;
	  break;
#else
        case IMAGE_REL_BASED_DIR64:
	  *(INT_PTR *)((char *)page + offset) += delta;
	  break;
#endif
        default:
	  TRACE("Unknown/unsupported fixup type %x.\n", type);
	  return NULL;
        }
      relocs++;
    }
  return (IMAGE_BASE_RELOCATION *)relocs;  /* return address of next block */
}


void MyLdrInitializeThunk( void *kernel_start, ULONG_PTR unknown2,
			   ULONG_PTR unknown3, ULONG_PTR unknown4 )
{
  static const WCHAR globalflagW[] = {'G','l','o','b','a','l','F','l','a','g',0};
  NTSTATUS status;
  WINE_MODREF *wm;
  LPCWSTR load_path = NULL;
  PEB *peb = current_peb();
  IMAGE_NT_HEADERS *nt = MyRtlImageNtHeader( peb->ImageBaseAddress );
  void (*_kernel_start) (void *ptr) = kernel_start;

#if 0
  if (main_exe_file) NtClose( main_exe_file );  /* at this point the main module is created */
#endif

  /* allocate the modref for the main exe (if not already done) */
  wm = get_modref( peb->ImageBaseAddress );
  assert( wm );
  if (wm->ldr.Flags & LDR_IMAGE_IS_DLL)
    {
      TRACE("%S is a dll, not an executable\n", wm->ldr.FullDllName );
      exit(1);
    }

  //  peb->ProcessParameters->ImagePathName = wm->ldr.FullDllName;
  //  version_init( wm->ldr.FullDllName );

  //  LdrQueryImageFileExecutionOptions( &peb->ProcessParameters->ImagePathName, globalflagW,
  //				     REG_DWORD, &peb->NtGlobalFlag, sizeof(peb->NtGlobalFlag), NULL );

  /* the main exe needs to be the first in the load order list */
  //  RemoveEntryList( &wm->ldr.InLoadOrderModuleList );
  //  InsertHeadList( &peb->LdrData->InLoadOrderModuleList, &wm->ldr.InLoadOrderModuleList );

  //  if ((status = virtual_alloc_thread_stack( NtCurrentTeb(), 0, 0 )) != STATUS_SUCCESS) goto error;
  //  if ((status = server_init_process_done()) != STATUS_SUCCESS) goto error;

  //  actctx_init();
  //  load_path = NtCurrentTeb()->Peb->ProcessParameters->DllPath.Buffer;
  if ((status = fixup_imports( wm, load_path )) != STATUS_SUCCESS) goto error;
  //  if ((status = alloc_process_tls()) != STATUS_SUCCESS) goto error;
  //  if ((status = alloc_thread_tls()) != STATUS_SUCCESS) goto error;
  //  heap_set_debug_flags( GetProcessHeap() );

#if 0
  /* FIXME: This may be interesting at some point.  */
  status = wine_call_on_stack( attach_process_dlls, wm, NtCurrentTeb()->Tib.StackBase );
  if (status != STATUS_SUCCESS) goto error;
#endif

  //  virtual_release_address_space( nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE );
  //  virtual_clear_thread_stack();
  //  wine_switch_to_stack( start_process, kernel_start, NtCurrentTeb()->Tib.StackBase );
  // stack( start_process, kernel_start, NtCurrentTeb()->Tib.StackBase );
  _kernel_start (peb);

 error:
  TRACE( "Main exe initialization for %S failed, status %x\n",
	 wm->ldr.FullDllName, status);
	 //	 peb->ProcessParameters->ImagePathName, status );
  //  NtTerminateProcess( GetCurrentProcess(), status );
  exit (1);
}
