////////////////////////////////////////////////////////////////////////////////
//  $Id: vld.cpp,v 1.38 2006/03/08 22:42:36 dmouldin Exp $
//
//  Visual Leak Detector (Version 1.9a) - VisualLeakDetector Class Impl.
//  Copyright (c) 2005-2006 Dan Moulding
//
//  This library is free software; you can redistribute it and/or
//  modify it under the terms of the GNU Lesser General Public
//  License as published by the Free Software Foundation; either
//  version 2.1 of the License, or (at your option) any later version.
//
//  This library is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//  Lesser General Public License for more details.
//
//  You should have received a copy of the GNU Lesser General Public
//  License along with this library; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
//
//  See COPYING.txt for the full terms of the GNU Lesser General Public License.
//
////////////////////////////////////////////////////////////////////////////////

#if !defined(_DEBUG) || !defined(_DLL)
#error "Visual Leak Detector must be dynamically linked with the debug C runtime library (compiler option /MDd)"
#endif // _DEBUG

#include <cassert>
#include <cstdio>
#include <windows.h>
#define __out_xcount(x) // Workaround for the specstrings.h bug in the Platform SDK.
#define DBGHELP_TRANSLATE_TCHAR
#include <dbghelp.h>    // Provides symbol handling services.
#define _CRTBLD         // Force dbgint.h and winheap.h to allow us to include them.
#include <dbgint.h>     // Provides access to the CRT heap internals, specifically the memory block header structure.
#include <winheap.h>    // Provides access to more heap internals, specifically the "paragraph" size.
#undef _CRTBLD
#define VLDBUILD        // Declares that we are building Visual Leak Detector.
#include "callstack.h"  // Provides a class for handling call stacks.
#include "map.h"        // Provides a lightweight STL-like map template.
#include "ntapi.h"      // Provides access to NT APIs.
#include "set.h"        // Provides a lightweidht STL-like set template.
#include "utility.h"    // Provides various utility functions.
#include "vldheap.h"    // Provides internal new and delete operators.
#include "vldint.h"     // Provides access to the Visual Leak Detector internals.

#define BLOCKMAPRESERVE     64  // This should strike a balance between memory use and a desire to minimize heap hits.
#define HEAPMAPRESERVE      2   // Usually there won't be more than a few heaps in the process, so this should be small.
#define MAXSYMBOLNAMELENGTH 256 // Maximum symbol name length that we will allow. Longer names will be truncated.
#define MODULESETRESERVE    16  // There are likely to be several modules loaded in the process.

// Imported global variables.
extern vldblockheader_t *vldblocklist;
extern HANDLE            vldheap;
extern CRITICAL_SECTION  vldheaplock;

// Global variables.
HANDLE currentprocess; // Pseudo-handle for the current process.
HANDLE currentthread;  // Pseudo-handle for the current thread.
HANDLE processheap;    // Handle to the process's heap (COM allocations come from here).

// Function pointer types for explicit dynamic linking with functions listed in
// the import patch table.
typedef void* (__cdecl *_malloc_dbg_t) (size_t, int, const char *, int);
typedef void* (__cdecl *_realloc_dbg_t) (void *, size_t, int, const char *, int);
typedef HRESULT (__stdcall *CoGetMalloc_t) (DWORD, LPMALLOC *);
typedef LPVOID (__stdcall *CoTaskMemAlloc_t) (ULONG);
typedef LPVOID (__stdcall *CoTaskMemRealloc_t) (LPVOID, ULONG);
typedef void* (__cdecl *crtnewdbg_t) (unsigned int, int, const char *, int);
typedef void* (__cdecl *malloc_t) (size_t);
typedef void* (__cdecl *mfc42newdbg_t) (unsigned int, const char *, int);
typedef void* (__cdecl *new_t) (unsigned int);
typedef void* (__cdecl *realloc_t) (void *, size_t);

// Global function pointers for explicit dynamic linking with functions listed
// in the import patch table. Using explicit dynamic linking minimizes VLD's
// footprint by loading only modules that are actually used. These pointers will
// be linked to the real functions the first time they are used.
static _malloc_dbg_t      p_malloc_dbg      = NULL;
static _realloc_dbg_t     p_realloc_dbg     = NULL;
static CoGetMalloc_t      pCoGetMalloc      = NULL;
static CoTaskMemAlloc_t   pCoTaskMemAlloc   = NULL;
static CoTaskMemRealloc_t pCoTaskMemRealloc = NULL;
static new_t              pcrtnew           = NULL;
static crtnewdbg_t        pcrtnewdbg        = NULL;
static malloc_t           pmalloc           = NULL;
static new_t              pmfc42new         = NULL;
static mfc42newdbg_t      pmfc42newdbg      = NULL;
static realloc_t          prealloc          = NULL;

// The one and only VisualLeakDetector object instance.
__declspec(dllexport) VisualLeakDetector vld;

// The import patch table: lists the heap-related API imports that VLD patches
// through to replacement functions provided by VLD. Having this table simply
// makes it more convenient to add additional IAT patches.
patchentry_t VisualLeakDetector::m_patchtable [] = {
    // Win32 heap APIs.
    "kernel32.dll", "GetProcAddress",    _GetProcAddress, // Not heap related, but can be used to obtain pointers to heap functions.
    "kernel32.dll", "HeapAlloc",         _RtlAllocateHeap,
    "kernel32.dll", "HeapCreate",        _HeapCreate,
    "kernel32.dll", "HeapDestroy",       _HeapDestroy,
    "kernel32.dll", "HeapFree",          _RtlFreeHeap,
    "kernel32.dll", "HeapReAlloc",       _RtlReAllocateHeap,

    // MFC new operators (exported by ordinal).
    "mfc42d.dll",   (LPCSTR)711,         _mfc42_new,
    "mfc42d.dll",   (LPCSTR)714,         _mfc42_new_dbg,
    // XXX 7.x and 8.x MFC DLL new operators still need to be added to this
    //   table, but I currently don't know their ordinals (they won't
    //   necessarily be the same as they were in MFC 4.2).

    // CRT new operators and heap APIs.
    "msvcrtd.dll",  "??2@YAPAXI@Z",      _crt_new,     // operator new
    "msvcrtd.dll",  "??2@YAPAXIHPBDH@Z", _crt_new_dbg, // debug operator new
    "msvcrtd.dll",  "_malloc_dbg",       __malloc_dbg,
    "msvcrtd.dll",  "_realloc_dbg",      __realloc_dbg,
    "msvcrtd.dll",  "malloc",            _malloc,
    "msvcrtd.dll",  "realloc",           _realloc,

    // NT APIs.
    "ntdll.dll",    "RtlAllocateHeap",   _RtlAllocateHeap,
    "ntdll.dll",    "RtlFreeHeap",       _RtlFreeHeap,
    "ntdll.dll",    "RtlReAllocateHeap", _RtlReAllocateHeap,

    // COM heap APIs.
    "ole32.dll",    "CoGetMalloc",       _CoGetMalloc,
    "ole32.dll",    "CoTaskMemAlloc",    _CoTaskMemAlloc,
    "ole32.dll",    "CoTaskMemRealloc",  _CoTaskMemRealloc
};

// Constructor - Initializes private data, loads configuration options, and
//   attaches Visual Leak Detector to all other modules loaded into the current
//   process.
//
VisualLeakDetector::VisualLeakDetector ()
{
    WCHAR   bom       = BOM; // Unicode byte-order mark.
    HMODULE kernel32  = GetModuleHandle(L"kernel32.dll");
    HMODULE ntdll     = GetModuleHandle(L"ntdll.dll");
    LPWSTR  symbolpath;

    // Initialize global variables.
    currentprocess    = GetCurrentProcess();
    currentthread     = GetCurrentThread();
    LdrLoadDll        = (LdrLoadDll_t)GetProcAddress(ntdll, "LdrLoadDll");
    processheap       = GetProcessHeap();
    RtlAllocateHeap   = (RtlAllocateHeap_t)GetProcAddress(ntdll, "RtlAllocateHeap");
    RtlFreeHeap       = (RtlFreeHeap_t)GetProcAddress(ntdll, "RtlFreeHeap");
    RtlReAllocateHeap = (RtlReAllocateHeap_t)GetProcAddress(ntdll, "RtlReAllocateHeap");
    vldheap           = HeapCreate(0x0, 0, 0);
    InitializeCriticalSection(&vldheaplock);

    // Initialize private data.
    m_heapmap         = new HeapMap;
    m_heapmap->reserve(HEAPMAPRESERVE);
    m_imalloc         = NULL;
    m_leaksfound      = 0;
    InitializeCriticalSection(&m_lock);
    m_maxdatadump     = 0xffffffff;
    m_maxtraceframes  = 0xffffffff;
    wcsnset(m_forcedmodulelist, L'\0', MAXMODULELISTLENGTH);
    m_moduleset       = new ModuleSet;
    m_moduleset->reserve(MODULESETRESERVE);
    m_options         = 0x0;
    m_reportfile      = NULL;
    wcsncpy(m_reportfilepath, VLD_DEFAULT_REPORT_FILE_NAME, MAX_PATH);
    m_selftestfile    = __FILE__;
    m_selftestline    = 0;
    m_status          = 0x0;

    // Load configuration options.
    configure();
    if (m_options & VLD_OPT_SELF_TEST) {
        // Self-test mode has been enabled. Intentionally leak a small amount of
        // memory so that memory leak self-checking can be verified.
        if (m_options & VLD_OPT_UNICODE_REPORT) {
            wcsncpy(new WCHAR [21], L"Memory Leak Self-Test", 21); m_selftestline = __LINE__;
        }
        else {
            strncpy(new CHAR [21], "Memory Leak Self-Test", 21); m_selftestline = __LINE__;
        }
    }
    if (m_options & VLD_OPT_START_DISABLED) {
        // Memory leak detection will initially be disabled.
        m_status |= VLD_STATUS_NEVER_ENABLED;
    }
    if (m_options & VLD_OPT_REPORT_TO_FILE) {
        // Reporting to file enabled.
        if (m_options & VLD_OPT_UNICODE_REPORT) {
            // Unicode data encoding has been enabled. Write the byte-order
            // mark before anything else gets written to the file. Open the
            // file for binary writing.
            m_reportfile = _wfopen(m_reportfilepath, L"wb");
            if (m_reportfile != NULL) {
                fwrite(&bom, sizeof(WCHAR), 1, m_reportfile);
            }
            setreportencoding(unicode);
        }
        else {
            // Open the file in text mode for ASCII output.
            m_reportfile = _wfopen(m_reportfilepath, L"w");
            setreportencoding(ascii);
        }
        if (m_reportfile == NULL) {
            report(L"WARNING: Visual Leak Detector: Couldn't open report file for writing: %s\n"
                   L"  The report will be sent to the debugger instead.\n", m_reportfilepath);
        }
        else {
            // Set the "report" function to write to the file.
            setreportfile(m_reportfile, m_options & VLD_OPT_REPORT_TO_DEBUGGER);
        }
    }

    // Initialize the symbol handler. We use it for obtaining source file/line
    // number information and function names for the memory leak report.
    symbolpath = buildsymbolsearchpath();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    if (!SymInitialize(currentprocess, symbolpath, FALSE)) {
        report(L"WARNING: Visual Leak Detector: The symbol handler failed to initialize (error=%lu).\n"
               L"    File and function names will probably not be available in call stacks.\n", GetLastError());
    }
    delete [] symbolpath;

    // Patch into kernel32.dll's calls to LdrLoadDll so that VLD can
    // dynamically attach to new modules loaded during runtime.
    patchimport(kernel32, "ntdll.dll", "LdrLoadDll", _LdrLoadDll);

    // Attach Visual Leak Detector to every module loaded in the process.
    EnumerateLoadedModules64(currentprocess, attachtomodule, NULL);

    report(L"Visual Leak Detector Version " VLDVERSION L" installed.\n");
    if (m_status & VLD_STATUS_FORCE_REPORT_TO_FILE) {
        // The report is being forced to a file. Let the human know why.
        report(L"NOTE: Visual Leak Detector: Unicode-encoded reporting has been enabled, but the\n"
               L"  debugger is the only selected report destination. The debugger cannot display\n"
               L"  Unicode characters, so the report will also be sent to a file. If no file has\n"
               L"  been specified, the default file name is \"" VLD_DEFAULT_REPORT_FILE_NAME L"\".\n");

    }
    reportconfig();
}

// Destructor - Detaches Visual Leak Detector from all modules loaded in the
//   process, frees internally allocated resources, and generates the memory
//   leak report.
//
VisualLeakDetector::~VisualLeakDetector ()
{
    BlockMap::Iterator  blockit;
    BlockMap           *blockmap;
    vldblockheader_t   *header;
    HANDLE              heap;
    HeapMap::Iterator   heapit;
    SIZE_T              internalleaks = 0;
    const char         *leakfile;
    WCHAR               leakfilew [MAX_PATH];
    int                 leakline;

    // Detach Visual Leak Detector from all previously attached modules.
    EnumerateLoadedModules64(currentprocess, detachfrommodule, NULL);

    if (m_status & VLD_STATUS_NEVER_ENABLED) {
        // Visual Leak Detector started with leak detection disabled and
        // it was never enabled at runtime. A lot of good that does.
        report(L"WARNING: Visual Leak Detector: Memory leak detection was never enabled.\n");
    }
    else {
        // Generate a memory leak report for each heap in the process.
        for (heapit = m_heapmap->begin(); heapit != m_heapmap->end(); ++heapit) {
            heap = (*heapit).first;
            reportleaks(heap);
        }

        // Show a summary.
        if (m_leaksfound == 0) {
            report(L"No memory leaks detected.\n");
        }
        else {
            report(L"Visual Leak Detector detected %lu memory leak", m_leaksfound);
            report((m_leaksfound > 1) ? L"s.\n" : L".\n");
        }
    }

    // Free resources used by the symbol handler.
    if (!SymCleanup(currentprocess)) {
        report(L"WARNING: Visual Leak Detector: The symbol handler failed to deallocate resources (error=%lu).\n", GetLastError());
    }

    // Free internally allocated resources.
    for (heapit = m_heapmap->begin(); heapit != m_heapmap->end(); ++heapit) {
        blockmap = &(*heapit).second->blockmap;
        for (blockit = blockmap->begin(); blockit != blockmap->end(); ++blockit) {
            delete (*blockit).second->callstack;
            delete (*blockit).second;
        }
        delete blockmap;
    }
    delete m_heapmap;
    delete m_moduleset;

    // Do a memory leak self-check.
    header = vldblocklist;
    while (header) {
        // Doh! VLD still has an internally allocated block!
        // This won't ever actually happen, right guys?... guys?
        internalleaks++;
        leakfile = header->file;
        leakline = header->line;
        mbstowcs(leakfilew, leakfile, MAX_PATH);
        report(L"ERROR: Visual Leak Detector: Detected a memory leak internal to Visual Leak Detector!!\n");
        report(L"---------- Block %ld at " ADDRESSFORMAT L": %u bytes ----------\n", header->serialnumber, BLOCKDATA(header), header->size);
        report(L"  Call Stack:\n");
        report(L"    %s (%d): Full call stack not available.\n", leakfilew, leakline);
        if (m_maxdatadump != 0) {
            report(L"  Data:\n");
            if (m_options & VLD_OPT_UNICODE_REPORT) {
                dumpmemoryw(BLOCKDATA(header), (m_maxdatadump < header->size) ? m_maxdatadump : header->size);
            }
            else {
                dumpmemorya(BLOCKDATA(header), (m_maxdatadump < header->size) ? m_maxdatadump : header->size);
            }
        }
        report(L"\n");
        header = header->next;
    }
    if (m_options & VLD_OPT_SELF_TEST) {
        if ((internalleaks == 1) && (strcmp(leakfile, m_selftestfile) == 0) && (leakline == m_selftestline)) {
            report(L"Visual Leak Detector passed the memory leak self-test.\n");
        }
        else {
            report(L"ERROR: Visual Leak Detector: Failed the memory leak self-test.\n");
        }
    }
    DeleteCriticalSection(&m_lock);
    HeapDestroy(vldheap);

    report(L"Visual Leak Detector is now exiting.\n");

    if (m_reportfile != NULL) {
        fclose(m_reportfile);
    }
}

// __malloc_dbg - Calls to _malloc_dbg are patched through to this function.
//   This function is just a wrapper around the real _malloc_dbg that sets
//   appropriate flags to be consulted when the memory is actually allocated by
//   RtlAllocateHeap.
//
//  - size (IN): The size, in bytes, of the memory block to be allocated.
//
//  - type (IN): The CRT "use type" of the block to be allocated.
//
//  - file (IN): The name of the file from which this function is being called.
//
//  - line (IN): The line number, in the above file, at which this function is
//      being called.
//
//  Return Value:
//
//    Returns the value returned by _malloc_dbg.
//
void* VisualLeakDetector::__malloc_dbg (size_t size, int type, const char *file, int line)
{
    void    *block;
    SIZE_T   fp;
    HMODULE  msvcrtd;

    // _malloc_dbg is a CRT function and allocates from the CRT heap.
    vld.m_tls.flags |= VLD_TLS_CRTALLOC;

    if (m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        vld.m_tls.addrfp = fp;
    }

    if (p_malloc_dbg == NULL) {
        // This is the first call to this function. Link to the real
        // _malloc_dbg.
        msvcrtd = GetModuleHandle(L"msvcrtd.dll");
        p_malloc_dbg = (_malloc_dbg_t)GetProcAddress(msvcrtd, "_malloc_dbg");
    }

    // Do the allocation. The block will be mapped by _RtlAllocateHeap.
    block = p_malloc_dbg(size, type, file, line);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// __realloc_dbg - Calls to _realloc_dbg are patched through to this function.
//   This function is just a wrapper around the real _realloc_dbg that sets
//   appropriate flags to be consulted when the memory is actually allocated by
//   RtlAllocateHeap.
//
//  - mem (IN): Pointer to the memory block to be reallocated.
//
//  - size (IN): The size of the memory block to reallocate.
//
//  - type (IN): The CRT "use type" of the block to be reallocated.
//
//  - file (IN): The name of the file from which this function is being called.
//
//  - line (IN): The line number, in the above filel, at which this function is
//      being called.
//
//  Return Value:
//
//    Returns the value returned by _realloc_dbg.
//
void* VisualLeakDetector::__realloc_dbg (void *mem, size_t size, int type, const char *file, int line)
{
    void    *block;
    SIZE_T   fp;
    HMODULE  msvcrtd;

    // _realloc_dbg is a CRT function and allocates from the CRT heap.
    vld.m_tls.flags |= VLD_TLS_CRTALLOC;

    if (vld.m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        vld.m_tls.addrfp = fp;
    }

    if (p_realloc_dbg == NULL) {
        // This is the first call to this function. Link to the real
        // _realloc_dbg.
        msvcrtd = GetModuleHandle(L"msvcrtd.dll");
        p_realloc_dbg = (_realloc_dbg_t)GetProcAddress(msvcrtd, "_realloc_dbg");
    }

    // Do the allocation. The block will be mapped by _RtlReAllocateHeap.
    block = p_realloc_dbg(mem, size, type, file, line);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// _CoGetMalloc - Calls to CoGetMalloc are patched through to this function.
//   This function returns a pointer to Visual Leak Detector's implementation
//   of the IMalloc interface, instead of returning a pointer to the system
//   implementation. This allows VLD's implementation of the IMalloc interface
//   (which is basically a thin wrapper around the system implementation) to be
//   invoked in place of the system implementation.
//
//  - context (IN): Reserved; value must be 1.
//
//  - imalloc (IN): Address of a pointer to receive the address of VLD's
//      implementation of the IMalloc interface.
//
//  Return Value:
//
//    Always returns S_OK.
//
HRESULT VisualLeakDetector::_CoGetMalloc (DWORD context, LPMALLOC *imalloc)
{
    HMODULE ole32;

    *imalloc = (LPMALLOC)&vld;

    if (pCoGetMalloc == NULL) {
        // This is the first call to this function. Link to the real
        // CoGetMalloc and get a pointer to the system implementation of the
        // IMalloc interface.
        ole32 = GetModuleHandle(L"ole32.dll");
        pCoGetMalloc = (CoGetMalloc_t)GetProcAddress(ole32, "CoGetMalloc");
        pCoGetMalloc(1, &vld.m_imalloc);
    }

    return S_OK;
}

// _CoTaskMemAlloc - Calls to CoTaskMemAlloc are patched through to this
//   function. This function is just a wrapper around the real CoTaskMemAlloc
//   that sets appropriate flags to be consulted when the memory is actually
//   allocated by RtlAllocateHeap.
//
//  - size (IN): Size of the memory block to allocate.
//
//  Return Value:
//
//    Returns the value returned from CoTaskMemAlloc.
//
LPVOID VisualLeakDetector::_CoTaskMemAlloc (ULONG size)
{
    LPVOID  block;
    SIZE_T  fp;
    HMODULE ole32;

    if (vld.m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        vld.m_tls.addrfp = fp;
    }

    if (pCoTaskMemAlloc == NULL) {
        // This is the first call to this function. Link to the real
        // CoTaskMemAlloc.
        ole32 = GetModuleHandle(L"ole32.dll");
        pCoTaskMemAlloc = (CoTaskMemAlloc_t)GetProcAddress(ole32, "CoTaskMemAlloc");
    }

    // Do the allocation. The block will be mapped by _RtlAllocateHeap.
    block = pCoTaskMemAlloc(size);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;
    
    return block;
}

// _CoTaskMemRealloc - Calls to CoTaskMemRealloc are patched through to this
//   function. This function is just a wrapper around the real CoTaskMemRealloc
//   that sets appropriate flags to be consulted when the memory is actually
//   allocated by RtlAllocateHeap.
//
//  - mem (IN): Pointer to the memory block to reallocate.
//
//  - size (IN): Size, in bytes, of the block to reallocate.
//
//  Return Value:
//
//    Returns the value returned from CoTaskMemRealloc.
//
LPVOID VisualLeakDetector::_CoTaskMemRealloc (LPVOID mem, ULONG size)
{
    LPVOID  block;
    SIZE_T  fp;
    HMODULE ole32;

    if (vld.m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        vld.m_tls.addrfp = fp;
    }

    if (pCoTaskMemRealloc == NULL) {
        // This is the first call to this function. Link to the real
        // CoTaskMemRealloc.
        ole32 = GetModuleHandle(L"ole32.dll");
        pCoTaskMemRealloc = (CoTaskMemRealloc_t)GetProcAddress(ole32, "CoTaskMemRealloc");
    }

    // Do the allocation. The block will be mapped by _RtlReAllocateHeap.
    block = pCoTaskMemRealloc(mem, size);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// _crt_new - Calls to the CRT's new operator are patched through to this
//   function. This function is just a wrapper around the real CRT new operator
//   that sets appropriate flags to be consulted when the memory is actually
//   allocated by RtlAllocateHeap.
//
//  - size (IN): The size, in bytes, of the memory block to be allocated.
//
//  Return Value:
//
//    Returns the value returned by the CRT new operator.
//
void* VisualLeakDetector::_crt_new (unsigned int size)
{
    void    *block;
    SIZE_T   fp;
    HMODULE  msvcrtd;

    // The new operator is a CRT function and allocates from the CRT heap.
    vld.m_tls.flags |= VLD_TLS_CRTALLOC;

    if (vld.m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        vld.m_tls.addrfp = fp;
    }

    if (pcrtnew == NULL) {
        // This is the first call to this function. Link to the real CRT new
        // operator.
        msvcrtd = GetModuleHandle(L"msvcrtd.dll");
        pcrtnew = (new_t)GetProcAddress(msvcrtd, "??2@YAPAXI@Z");
    }

    // Do tha allocation. The block will be mapped by _RtlAllocateHeap.
    block = pcrtnew(size);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// _crt_new_dbg - Calls to the CRT's debug new operator are patched through to
//   this function. This function is just a wrapper around the real CRT debug
//   new operator that sets appropriate flags to be consulted when the memory is
//   actually allocated by RtlAllocateHeap.
//
//  - size (IN): The size, in bytes, of the memory block to be allocated.
//
//  - type (IN): The CRT "use type" of the block to be allocated.
//
//  - file (IN): The name of the file from which this function is being called.
//
//  - line (IN): The line number, in the above file, at which this function is
//      being called.
//
//  Return Value:
//
//    Returns the value returned by the CRT debug new operator.
//
void* VisualLeakDetector::_crt_new_dbg (unsigned int size, int type, const char *file, int line)
{
    void    *block;
    SIZE_T   fp;
    HMODULE  msvcrtd;

    // The debug new operator is a CRT function and allocates from the CRT heap.
    vld.m_tls.flags |= VLD_TLS_CRTALLOC;

    if (vld.m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        vld.m_tls.addrfp = fp;
    }

    if (pcrtnewdbg == NULL) {
        // This is the first call to this function. Link to the real CRT debug
        // new operator.
        msvcrtd = GetModuleHandle(L"msvcrtd.dll");
        pcrtnewdbg = (crtnewdbg_t)GetProcAddress(msvcrtd, "??2@YAPAXIHPBDH@Z");
    }

    // Do the allocation. The block will be mapped by _RtlAllocateHeap.
    block = pcrtnewdbg(size, type, file, line);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// _GetProcAddress - Calls to GetProcAddress are patched through to this
//   function. If the requested function is a function that has been patched
//   through to one of VLD's handlers, then the address of VLD's handler
//   function is returned instead of the real address. Otherwise, this 
//   function is just a wrapper around the real GetProcAddress.
//
//  - module (IN): Handle (base address) of the module from which to retrieve
//      the address of an exported function.
//
//  - procname (IN): ANSI string containing the name of the exported function
//      whose address is to be retrieved.
//
//  Return Value:
//
//    Returns a pointer to the requested function, or VLD's replacement for
//    the function, if there is a replacement function.
//
FARPROC VisualLeakDetector::_GetProcAddress (HMODULE module, LPCSTR procname)
{
    patchentry_t *entry;
    HMODULE       exportmodule;
    UINT          index;
    UINT          tablesize = sizeof(vld.m_patchtable) / sizeof(patchentry_t);

    // See if there is an entry in the patch table that matches the requested
    // function.
    for (index = 0; index < tablesize; ++index) {
        entry = &vld.m_patchtable[index];
        exportmodule = GetModuleHandleA(entry->exportmodulename);
        if (exportmodule != module) {
            // This patch table entry is for a different module.
            continue;
        }

        // This patch table entry is for the specified module.
        if (strcmp(entry->importname, procname) == 0) {
            // The function name in the patch entry is the same as the requested
            // function name. This means a request for a patched function's
            // address has been made. Return tha address of the replacement
            // function, not the address of the real function.
            return (FARPROC)entry->replacement;
        }
    }

    // The requested function is not a patched function. Just return the real
    // address of the requested function.
    return GetProcAddress(module, procname);
}

// _HeapCreate - Calls to HeapCreate are patched through to this function. This
//   function is just a wrapper around the real HeapCreate that calls VLD's heap
//   creation tracking function after the heap has been created.
//
//  - options (IN): Heap options.
//
//  - initsize (IN): Initial size of the heap.
//
//  - maxsize (IN): Maximum size of the heap.
//
//  Return Value:
//
//    Returns the value returned by HeapCreate.
//
HANDLE VisualLeakDetector::_HeapCreate (DWORD options, SIZE_T initsize, SIZE_T maxsize)
{
    DWORD64            displacement;
    SIZE_T             fp;
    SYMBOL_INFO       *functioninfo;
    HANDLE             heap;
    HeapMap::Iterator  heapit;
    SIZE_T             ra;
    BYTE               symbolbuffer [sizeof(SYMBOL_INFO) + (MAXSYMBOLNAMELENGTH * sizeof(WCHAR)) - 1] = { 0 };
    BOOL               symfound;

    heap = HeapCreate(options, initsize, maxsize);

    // Map the created heap handle to a new block map.
    vld.mapheap(heap);

    // Get the return address within the calling function.
    FRAMEPOINTER(fp);
    ra = *((SIZE_T*)fp + 1);

    // Try to get the name of the function containing the return address.
    EnterCriticalSection(&vld.m_lock);
    functioninfo = (SYMBOL_INFO*)&symbolbuffer;
    functioninfo->SizeOfStruct = sizeof(SYMBOL_INFO);
    functioninfo->MaxNameLen = MAXSYMBOLNAMELENGTH;
    symfound = SymFromAddr(currentprocess, ra, &displacement, functioninfo);
    LeaveCriticalSection(&vld.m_lock);
    if (symfound == TRUE) {
        if (wcscmp(L"_heap_init", functioninfo->Name) == 0) {
            // HeapCreate was called by _heap_init. This is a static CRT heap.
            heapit = vld.m_heapmap->find(heap);
            assert(heapit != vld.m_heapmap->end());
            (*heapit).second->flags |= VLD_HEAP_CRT;
        }
    }

    return heap;
}

// _HeapDestroy - Calls to HeapDestroy are patched through to this function.
//   This function is just a wrapper around the real HeapDestroy that calls
//   VLD's heap destruction tracking function after the heap has been destroyed.
//
//  - heap (IN): Handle to the heap to be destroyed.
//
//  Return Value:
//
//    Returns the valued returned by HeapDestroy.
//
BOOL VisualLeakDetector::_HeapDestroy (HANDLE heap)
{
    // After this heap is destroyed, the heap's address space will be unmapped
    // from the process's address space. So, we'd better generate a leak report
    // for this heap now, while we can still read from the memory blocks
    // allocated to it.
    vld.reportleaks(heap);

    vld.unmapheap(heap);
    return HeapDestroy(heap);
}

// _LdrLoadDll - Calls to LdrLoadDll are patched through to this function. This
//   function invokes the real LdrLoadDll and then re-attaches VLD to all
//   modules loaded in the process after loading of the new DLL is complete.
//   All modules must be re-enumerated because the explicit load of the
//   specified module may result in the implicit load of one or more additional
//   modules which are dependencies of the specified module.
//
//  - searchpath (IN): The path to use for searching for the specified module to
//      be loaded.
//
//  - flags (IN): Pointer to action flags.
//
//  - modulename (IN): Pointer to a unicodestring_t structure specifying the
//      name of the module to be loaded.
//
//  - modulehandle (OUT): Address of a HANDLE to receive the newly loaded
//      module's handle.
//
//  Return Value:
//
//    Returns the value returned by LdrLoadDll.
//
NTSTATUS VisualLeakDetector::_LdrLoadDll (LPWSTR searchpath, PDWORD flags, unicodestring_t *modulename, PHANDLE modulehandle)
{
    NTSTATUS status;

    status = LdrLoadDll(searchpath, flags, modulename, modulehandle);
    
    // Attach to any newly loaded modules.
    EnterCriticalSection(&vld.m_lock);
    EnumerateLoadedModules64(currentprocess, attachtomodule, NULL);
    LeaveCriticalSection(&vld.m_lock);

    return status;
}

// _malloc - Calls to malloc and operator new are patched through to this
//   function. This function is just a wrapper around the real malloc that sets
//   appropriate flags to be consulted when the memory is actually allocated by
//   RtlAllocateHeap.
//
//  - size (IN): The size, in bytes, of the memory block to be allocated.
//
//  Return Value:
//
//    Returns the valued returned from malloc.
//
void* VisualLeakDetector::_malloc (size_t size)
{
    void    *block;
    SIZE_T   fp;
    HMODULE  msvcrtd;

    // malloc is a CRT function and allocates fro the CRT heap.
    vld.m_tls.flags |= VLD_TLS_CRTALLOC;

    if (vld.m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        vld.m_tls.addrfp = fp;
    }

    if (pmalloc == NULL) {
        // This is the first call to this function. Link to the real malloc.
        msvcrtd = GetModuleHandle(L"msvcrtd.dll");
        pmalloc = (malloc_t)GetProcAddress(msvcrtd, "malloc");
    }

    // Do the allocation. The block will be mapped by _RtlAllocateHeap.
    block = pmalloc(size);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// _mfc42_new - Calls to the MFC 4.2 new operator are patched through to this
//   function. This function is just a wrapper around the real MFC 4.2 new
//   operator that sets appropriate flags to be consulted when the memory is
//   actually allocated by RtlAllocateHeap.
//
//  - size (IN): The size, in bytes, of the memory block to be allocated.
//
//  Return Value:
//
//    Returns the value returned by the MFC 4.2 new operator.
//
void* VisualLeakDetector::_mfc42_new (unsigned int size)
{
    void    *block;
    SIZE_T   fp;
    HMODULE  mfc42d;

    // The MFC new operators are CRT-based and allocate from the CRT heap.
    vld.m_tls.flags |= VLD_TLS_CRTALLOC;

    if (vld.m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        vld.m_tls.addrfp = fp;
    }

    if (pmfc42new == NULL) {
        // This is the first call to this function. Link to the real MFC 4.2 new
        // operator.
        mfc42d = GetModuleHandle(L"mfc42d.dll");
        pmfc42new = (new_t)GetProcAddress(mfc42d, (LPCSTR)711);
    }

    // Do the allocation. The block will be mapped by _RtlAllocateHeap.
    block = pmfc42new(size);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// _mfc42_new_dbg - Calls to the MFC 4.2 debug new operator are patched through
//   to this function. This function is just a wrapper around the real MFC 4.2
//   debug new operator that sets appropriate flags to be consulted when the
//   memory is actually allocated by RtlAllocateHeap.
//
//  - size (IN): The size, in bytes, of the memory block to be allocated.
//
//  - file (IN): The name of the file from which this function is being called.
//
//  - line (IN): The line number, in the above file, at which this function is
//      being called.
//
//  Return Value:
//
//    Returns the value returned by the MFC 4.2 debug new operator.
//
void* VisualLeakDetector::_mfc42_new_dbg (unsigned int size, const char *file, int line)
{
    void    *block;
    SIZE_T   fp;
    HMODULE  mfc42d;

    // The MFC new operators are CRT-based and allocate from the CRT heap.
    vld.m_tls.flags |= VLD_TLS_CRTALLOC;

    if (vld.m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        vld.m_tls.addrfp = fp;
    }

    if (pmfc42newdbg == NULL) {
        // This is the first call to this function. Link to the real MFC 4.2
        // debug new operator.
        mfc42d = GetModuleHandle(L"mfc42d.dll");
        pmfc42newdbg = (mfc42newdbg_t)GetProcAddress(mfc42d, (LPCSTR)714);
    }

    // Do the allocation. The block will be mapped by _RtlAllocateHeap.
    block = pmfc42newdbg(size, file, line);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// _realloc - Calls to realloc are patched through to this function. This
//   function is just a wrapper around the real realloc that sets appropriate
//   flags to be consulted when the memory is actually allocated by
//   RtlAllocateHeap.
//
//  - mem (IN): Pointer to the memory block to reallocate.
//
//  - size (IN): Size of the memory block to reallocate.
//
//  Return Value:
//
//    Returns the value returned from realloc.
//
void* VisualLeakDetector::_realloc (void *mem, size_t size)
{
    void    *block;
    SIZE_T   fp;
    HMODULE  msvcrtd;

    // realloc is a CRT function and allocates from the CRT heap.
    vld.m_tls.flags |= VLD_TLS_CRTALLOC;

    if (vld.m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        vld.m_tls.addrfp = fp;
    }

    if (prealloc == NULL) {
        // This is the first call to this function. Link to the real realloc.
        msvcrtd = GetModuleHandle(L"msvcrtd.dll");
        prealloc = (realloc_t)GetProcAddress(msvcrtd, "realloc");
    }

    // Do the allocation. The block will be mapped by _RtlReAllocateHeap.
    block = prealloc(mem, size);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// _RtlAllocateHeap - Calls to RtlAllocateHeap are patched through to this
//   function. This function invokes the real RtlAllocateHeap and then calls
//   VLD's allocation tracking function. Pretty much all memory allocations
//   will eventually result in a call to RtlAllocateHeap, so this is where we
//   finally map the allocated block.
//
//  - heap (IN): Handle to the heap from which to allocate memory.
//
//  - flags (IN): Heap allocation control flags.
//
//  - size (IN): Size, in bytes, of the block to allocate.
//
//  Return Value:
//
//    Returns the return value from RtlAllocateHeap.
//
LPVOID VisualLeakDetector::_RtlAllocateHeap (HANDLE heap, DWORD flags, SIZE_T size)
{
    SIZE_T fp;
    LPVOID block;

    block = RtlAllocateHeap(heap, flags, size);
    if (block != NULL) {
        if (vld.m_tls.addrfp == 0) {
            // This is the first call to enter VLD for the current allocation.
            // Record the current frame pointer.
            FRAMEPOINTER(fp);
            vld.m_tls.addrfp = fp;
        }

        // Map the block to the specified heap.
        vld.mapblock(heap, block, size);
    }

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// _RtlFreeHeap - Calls to RtlFreeHeap are patched through to this function.
//   This function calls VLD's free tracking function and then invokes the real
//   RtlFreeHeap. Pretty much all memory frees will eventually result in a call
//   to RtlFreeHeap, so this is where we finally unmap the freed block.
//
//  - heap (IN): Handle to the heap to which the block being freed belongs.
//
//  - flags (IN): Heap control flags.
//
//  - mem (IN): Pointer to the memory block being freed.
//
//  Return Value:
//
//    Returns the value returned by RtlFreeHeap.
//
BOOL VisualLeakDetector::_RtlFreeHeap (HANDLE heap, DWORD flags, LPVOID mem)
{
    // Unmap the block from the specified heap.
    vld.unmapblock(heap, mem);

    return RtlFreeHeap(heap, flags, mem);
}

// _RtlReAllocateHeap - Calls to RtlReAllocateHeap are patched through to this
//   function. This function invokes the real RtlReAllocateHeap and then calls
//   VLD's reallocation tracking function. All arguments passed to this function
//   are passed on to the real RtlReAllocateHeap without modification. Pretty
//   much all memory re-allocations will eventually result in a call to
//   RtlReAllocateHeap, so this is where we finally remap the reallocated block.
//
//  - heap (IN): Handle to the heap to reallocate memory from.
//
//  - flags (IN): Heap control flags.
//
//  - mem (IN): Pointer to the currently allocated block which is to be
//      reallocated.
//
//  - size (IN): Size, in bytes, of the block to reallocate.
//
//  Return Value:
//
//    Returns the value returned by RtlReAllocateHeap.
//
LPVOID VisualLeakDetector::_RtlReAllocateHeap (HANDLE heap, DWORD flags, LPVOID mem, SIZE_T size)
{
    SIZE_T fp;
    LPVOID newmem;

    newmem = RtlReAllocateHeap(heap, flags, mem, size);
    if (newmem != NULL) {
        if (vld.m_tls.addrfp == 0) {
            // This is the first call to enter VLD for the current allocation.
            // Record the current frame pointer.
            FRAMEPOINTER(fp);
            vld.m_tls.addrfp = fp;
        }
        // Re-map the block to the new heap.
        vld.remapblock(heap, mem, newmem, size);
    }

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return newmem;
}

// AddRef - Calls to IMalloc::AddRef end up here. This function is just a
//   wrapper around the real IMalloc::AddRef implementation.
//
//  Return Value:
//
//    Returns the value returned by the system implementation of
//    IMalloc::AddRef.
//
ULONG VisualLeakDetector::AddRef ()
{
    assert(m_imalloc != NULL);
    return m_imalloc->AddRef();
}

// Alloc - Calls to IMalloc::Alloc end up here. This function is just a wrapper
//   around the real IMalloc::Alloc implementation that sets appropriate flags
//   to be consulted when the memory is actually allocated by RtlAllocateHeap.
//
//  - size (IN): The size of the memory block to allocate.
//
//  Return Value:
//
//    Returns the value returned by the system's IMalloc::Alloc implementation.
//
LPVOID VisualLeakDetector::Alloc (ULONG size)
{
    LPVOID block;
    SIZE_T fp;

    if (m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        m_tls.addrfp = fp;
    }

    // Do the allocation. The block will be mapped by _RtlAllocateHeap.
    assert(m_imalloc != NULL);
    block = m_imalloc->Alloc(size);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;

    return block;
}

// attachtomodule - Callback function for EnumerateLoadedModules64 that attaches
//   Visual Leak Detector to the specified module. This provides a way for VLD
//   to be attached to every module loaded in the process. However, even though
//   it attaches to all modules, not all modules are actually included in leak
//   detection. Only modules that import the global VisualLeakDetector class
//   object, or those that are otherwise explicitly included in leak detection,
//   will be checked for memory leaks.
//
//   Caution: This function is not thread-safe. It calls into the Debug Help
//     Library which is single-threaded. Therefore, calls to this function must
//     be synchronized.
//
//  - modulepath (IN): String containing the name, which may include a path, of
//      the module to attach to.
//
//  - modulebase (IN): Base address of the module.
//
//  - modulesize (IN): Total size of the module.
//
//  - context (IN): User-supplied context (ignored).
//
//  Return Value:
//
//    Always returns TRUE.
//
BOOL VisualLeakDetector::attachtomodule (PCWSTR modulepath, DWORD64 modulebase, ULONG modulesize, PVOID context)
{
    WCHAR               extension [_MAX_EXT];
    WCHAR               filename [_MAX_FNAME];
    IMAGEHLP_MODULE64   moduleimageinfo;
    moduleinfo_t        moduleinfo;
    ModuleSet::Iterator moduleit;
#define MAXMODULENAME (_MAX_FNAME + _MAX_EXT)
    WCHAR               modulename [MAXMODULENAME + 1];
    CHAR                modulepatha [MAX_PATH];
    UINT                tablesize = sizeof(m_patchtable) / sizeof(patchentry_t);

    // Extract just the filename and extension from the module path.
    _wsplitpath(modulepath, NULL, NULL, filename, extension);
    wcsncpy(modulename, filename, MAXMODULENAME);
    wcsncat(modulename, extension, MAXMODULENAME - wcslen(modulename));
    wcslwr(modulename);

    // Find this module in our module set. The module set contains information
    // about all modules loaded in the process that have already been attached.
    // If we find that the module is not already in the module set, we try to
    // load the module's symbols, add the module's information to the module
    // set and then attach to the module.
    moduleinfo.addrlow  = (SIZE_T)modulebase;
    moduleinfo.addrhigh = (SIZE_T)modulebase + modulesize - 1;
    moduleinfo.flags    = 0x0;
    moduleit = vld.m_moduleset->find(moduleinfo);
    if (moduleit != vld.m_moduleset->end()) {
        // This module has already been attached.
        return TRUE;
    }

    // Try to load the module's symbols. This ensures that we have loaded the
    // symbols for every module that has ever been loaded into the process,
    // guaranteeing the symbols' availability when generating the leak report.
    moduleimageinfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
    wcstombs(modulepatha, modulepath, MAX_PATH);
    if ((SymGetModuleInfo64(currentprocess, modulebase, &moduleimageinfo) == TRUE) ||
        ((SymLoadModule64(currentprocess, NULL, modulepatha, NULL, modulebase, modulesize) == modulebase) &&
        (SymGetModuleInfo64(currentprocess, modulebase, &moduleimageinfo) == TRUE))) {
        moduleinfo.flags |= VLD_MODULE_SYMBOLSLOADED;
    }

    if (wcsicmp(L"vld.dll", modulename) == NULL) {
        // What happens when a module goes through it's own portal? Bad things.
        // Like infinite recursion. And ugly bald men wearing dresses. VLD
        // should not, therefore, attach to itself.
        return TRUE;
    }

    if ((findimport((HMODULE)modulebase, "vld.dll", "?vld@@3VVisualLeakDetector@@A") == FALSE) &&
        (wcsstr(vld.m_forcedmodulelist, modulename) == NULL)) {
        // This module does not import VLD. This means that none of the module's
        // sources #included vld.h. Exclude this module from leak detection.
        moduleinfo.flags |= VLD_MODULE_EXCLUDED;
    }
    else if (!(moduleinfo.flags & VLD_MODULE_SYMBOLSLOADED) || (moduleimageinfo.SymType == SymExport)) {
        // This module is going to be included in leak detection, but complete
        // symbols for this module couldn't be loaded. This means that any stack
        // traces through this module may lack information, like line numbers
        // and function names.
        report(L"WARNING: Visual Leak Detector: A module, %s, included in memory leak detection\n"
               L"  does not have any debugging symbols available, or they could not be located.\n"
               L"  Function names and/or line numbers for this module may not be available.\n", modulename);
    }

    // Insert the module's information into the module set.
    vld.m_moduleset->insert(moduleinfo);

    // Attach to the module.
    patchmodule((HMODULE)modulebase, m_patchtable, tablesize);

    return TRUE;
}

// buildsymbolsearchpath - Builds the symbol search path for the symbol handler.
//   This helps the symbol handler find the symbols for the application being
//   debugged.
//
//  Return Value:
//
//    Returns a string containing the search path. The caller is responsible for
//    freeing the string.
//
LPWSTR VisualLeakDetector::buildsymbolsearchpath ()
{
    WCHAR   directory [_MAX_DIR];
    WCHAR   drive [_MAX_DRIVE];
    LPWSTR  env;
    DWORD   envlen;
    size_t  index;
    size_t  length;
    HMODULE module;
    LPWSTR  path = new WCHAR [MAX_PATH];
    size_t  pos = 0;
    WCHAR   system [MAX_PATH];
    WCHAR   windows [MAX_PATH];

    // Oddly, the symbol handler ignores the link to the PDB embedded in the
    // executable image. So, we'll manually add the location of the executable
    // to the search path since that is often where the PDB will be located.
    path[0] = L'\0';
    module = GetModuleHandle(NULL);
    GetModuleFileName(module, path, MAX_PATH);
    _wsplitpath(path, drive, directory, NULL, NULL);
    wcsncpy(path, drive, MAX_PATH);
    strapp(&path, directory);

    // When the symbol handler is given a custom symbol search path, it will no
    // longer search the default directories (working directory, system root,
    // etc). But we'd like it to still search those directories, so we'll add
    // them to our custom search path.
    //
    // Append the working directory.
    strapp(&path, L";.\\");

    // Append the Windows directory.
    if (GetWindowsDirectory(windows, MAX_PATH) != 0) {
        strapp(&path, L";");
        strapp(&path, windows);
    }

    // Append the system directory.
    if (GetSystemDirectory(system, MAX_PATH) != 0) {
        strapp(&path, L";");
        strapp(&path, system);
    }

    // Append %_NT_SYMBOL_PATH%.
    envlen = GetEnvironmentVariable(L"_NT_SYMBOL_PATH", NULL, 0);
    if (envlen != 0) {
        env = new WCHAR [envlen];
        if (GetEnvironmentVariable(L"_NT_SYMBOL_PATH", env, envlen) != 0) {
            strapp(&path, L";");
            strapp(&path, env);
        }
        delete [] env;
    }

    //  Append %_NT_ALT_SYMBOL_PATH%.
    envlen = GetEnvironmentVariable(L"_NT_ALT_SYMBOL_PATH", NULL, 0);
    if (envlen != 0) {
        env = new WCHAR [envlen];
        if (GetEnvironmentVariable(L"_NT_ALT_SYMBOL_PATH", env, envlen) != 0) {
            strapp(&path, L";");
            strapp(&path, env);
        }
        delete [] env;
    }

    // Remove any quotes from the path. The symbol handler doesn't like them.
    pos = 0;
    length = wcslen(path);
    while (pos < length) {
        if (path[pos] == L'\"') {
            for (index = pos; index < length; index++) {
                path[index] = path[index + 1];
            }
        }
        pos++;
    }

    return path;
}

// configure - Configures VLD using values read from the vld.ini file.
//
//  Return Value:
//
//    None.
//
VOID VisualLeakDetector::configure ()
{
#define BSIZE 64
    WCHAR   buffer [BSIZE];
    WCHAR   filename [MAX_PATH];
    WCHAR   inipath [MAX_PATH];

    _wfullpath(inipath, L".\\vld.ini", MAX_PATH);

    // Read the boolean options.
    GetPrivateProfileString(L"Options", L"AggregateDuplicates", L"", buffer, BSIZE, inipath);
    if (strtobool(buffer) == TRUE) {
        m_options |= VLD_OPT_AGGREGATE_DUPLICATES;
    }

    GetPrivateProfileString(L"Options", L"SelfTest", L"", buffer, BSIZE, inipath);
    if (strtobool(buffer) == TRUE) {
        m_options |= VLD_OPT_SELF_TEST;
    }

    GetPrivateProfileString(L"Options", L"StartDisabled", L"", buffer, BSIZE, inipath);
    if (strtobool(buffer) == TRUE) {
        m_options |= VLD_OPT_START_DISABLED;
    }

    GetPrivateProfileString(L"Options", L"TraceInternalFrames", L"", buffer, BSIZE, inipath);
    if (strtobool(buffer) == TRUE) {
        m_options |= VLD_OPT_TRACE_INTERNAL_FRAMES;
    }

    // Read the integer configuration options.
    m_maxdatadump = GetPrivateProfileInt(L"Options", L"MaxDataDump", VLD_DEFAULT_MAX_DATA_DUMP, inipath);
    m_maxtraceframes = GetPrivateProfileInt(L"Options", L"MaxTraceFrames", VLD_DEFAULT_MAX_TRACE_FRAMES, inipath);
    if (m_maxtraceframes < 1) {
        m_maxtraceframes = VLD_DEFAULT_MAX_TRACE_FRAMES;
    }

    // Read the force-include module list.
    GetPrivateProfileString(L"Options", L"ForceIncludeModules", L"", m_forcedmodulelist, MAXMODULELISTLENGTH, inipath);
    wcslwr(m_forcedmodulelist);

    // Read the report destination (debugger, file, or both).
    GetPrivateProfileString(L"Options", L"ReportFile", L"", filename, MAX_PATH, inipath);
    if (wcslen(filename) == 0) {
        wcsncpy(filename, VLD_DEFAULT_REPORT_FILE_NAME, MAX_PATH);
    }
    _wfullpath(m_reportfilepath, filename, MAX_PATH);
    GetPrivateProfileString(L"Options", L"ReportTo", L"", buffer, BSIZE, inipath);
    if (wcsicmp(buffer, L"both") == 0) {
        m_options |= (VLD_OPT_REPORT_TO_DEBUGGER | VLD_OPT_REPORT_TO_FILE);
    }
    else if (wcsicmp(buffer, L"file") == 0) {
        m_options |= VLD_OPT_REPORT_TO_FILE;
    }
    else {
        m_options |= VLD_OPT_REPORT_TO_DEBUGGER;
    }

    // Read the report file encoding (ascii or unicode).
    GetPrivateProfileString(L"Options", L"ReportEncoding", L"", buffer, BSIZE, inipath);
    if (wcsicmp(buffer, L"unicode") == 0) {
        m_options |= VLD_OPT_UNICODE_REPORT;
    }
    if ((m_options & VLD_OPT_UNICODE_REPORT) && !(m_options & VLD_OPT_REPORT_TO_FILE)) {
        // If Unicode report encoding is enabled, then the report needs to be
        // sent to a file because the debugger will not display Unicode
        // characters, it will display question marks in their place instead.
        m_options |= VLD_OPT_REPORT_TO_FILE;
        m_status |= VLD_STATUS_FORCE_REPORT_TO_FILE;
    }

    // Read the stack walking method.
    GetPrivateProfileString(L"Options", L"StackWalkMethod", L"", buffer, BSIZE, inipath);
    if (wcsicmp(buffer, L"safe") == 0) {
        m_options |= VLD_OPT_SAFE_STACK_WALK;
    }
}

// detachfrommodule - Callback function for EnumerateLoadedModules64 that
//   detaches Visual Leak Detector from the specified module. If the specified
//   module has not previously been attached to, then calling this function will
//   not actually result in any changes.
//
//   Caution: This function is not thread-safe. It calls into the Debug Help
//     Library which is single-threaded. Therefore, calls to this function must
//     be synchronized.
//
//  - modulepath (IN): String containing the name, which may inlcude a path, of
//      the module to detach from (ignored).
//
//  - modulebase (IN): Base address of the module.
//
//  - modulesize (IN): Total size of the module (ignored).
//
//  - context (IN): User-supplied context (ignored).
//
//  Return Value:
//
//    Always returns TRUE.
//
BOOL VisualLeakDetector::detachfrommodule (PCWSTR modulepath, DWORD64 modulebase, ULONG modulesize, PVOID context)
{
    UINT tablesize = sizeof(m_patchtable) / sizeof(patchentry_t);

    restoremodule((HMODULE)modulebase, m_patchtable, tablesize);

    return TRUE;
}

// DidAlloc - Calls to IMalloc::DidAlloc will end up here. This function is just
//   a wrapper around the system implementation of IMalloc::DidAlloc.
//
//  - mem (IN): Pointer to a memory block to inquire about.
//
//  Return Value:
//
//    Returns the value returned by the system implementation of
//    IMalloc::DidAlloc.
//
INT VisualLeakDetector::DidAlloc (LPVOID mem)
{
    assert(m_imalloc != NULL);
    return m_imalloc->DidAlloc(mem);
}

// enabled - Determines if memory leak detection is enabled for the current
//   thread.
//
//  Return Value:
//
//    Returns true if Visual Leak Detector is enabled for the current thread.
//    Otherwise, returns false.
//
BOOL VisualLeakDetector::enabled ()
{
    if (!(m_tls.flags & VLD_TLS_DISABLED) && !(m_tls.flags & VLD_TLS_ENABLED)) {
        // The enabled/disabled state for the current thread has not been 
        // initialized yet. Use the default state.
        if (m_options & VLD_OPT_START_DISABLED) {
            m_tls.flags |= VLD_TLS_DISABLED;
        }
        else {
            m_tls.flags |= VLD_TLS_ENABLED;
        }
    }

    return ((m_tls.flags & VLD_TLS_ENABLED) != 0);
}

// eraseduplicates - Erases, from the block maps, blocks that appear to be
//   duplicate leaks of an already identified leak.
//
//  - element (IN): BlockMap Iterator referencing the block of which to search
//      for duplicates.
//
//  Return Value:
//
//    Returns the number of duplicate blocks erased from the block map.
//
SIZE_T VisualLeakDetector::eraseduplicates (const BlockMap::Iterator &element)
{
    BlockMap::Iterator  blockit;
    BlockMap           *blockmap;
    blockinfo_t        *elementinfo;
    SIZE_T              erased = 0;
    HeapMap::Iterator   heapit;
    blockinfo_t        *info;
    BlockMap::Iterator  previt;

    elementinfo = (*element).second;

    // Iteratate through all block maps, looking for blocks with the same size
    // and callstack as the specified element.
    for (heapit = m_heapmap->begin(); heapit != m_heapmap->end(); ++heapit) {
        blockmap = &(*heapit).second->blockmap;
        for (blockit = blockmap->begin(); blockit != blockmap->end(); ++blockit) {
            if (blockit == element) {
                // Don't delete the element of which we are searching for
                // duplicates.
                continue;
            }
            info = (*blockit).second;
            if ((info->size == elementinfo->size) && (*(info->callstack) == *(elementinfo->callstack))) {
                // Found a duplicate. Erase it.
                delete info->callstack;
                delete info;
                previt = blockit - 1;
                blockmap->erase(blockit);
                blockit = previt;
                erased++;
            }
        }
    }

    return erased;
}

// Free - Calls to IMalloc::Free will end up here. This function is just a
//   wrapper around the real IMalloc::Free implementation.
//
//  - mem (IN): Pointer to the memory block to be freed.
//
//  Return Value:
//
//    None.
//
VOID VisualLeakDetector::Free (LPVOID mem)
{
    assert(m_imalloc != NULL);
    m_imalloc->Free(mem);
}

// GetSize - Calls to IMalloc::GetSize will end up here. This function is just a
//   wrapper around the real IMalloc::GetSize implementation.
//
//  - mem (IN): Pointer to the memory block to inquire about.
//
//  Return Value:
//
//    Returns the value returned by the system implementation of
//    IMalloc::GetSize.
//
ULONG VisualLeakDetector::GetSize (LPVOID mem)
{
    assert(m_imalloc != NULL);
    return m_imalloc->GetSize(mem);
}

// HeapMinimize - Calls to IMalloc::HeapMinimize will end up here. This function
//   is just a wrapper around the real IMalloc::HeapMinimize implementation.
//
//  Return Value:
//
//    None.
//
VOID VisualLeakDetector::HeapMinimize ()
{
    assert(m_imalloc != NULL);
    m_imalloc->HeapMinimize();
}

// mapblock - Tracks memory allocations. Information about allocated blocks is
//   collected and then the block is mapped to this information.
//
//  - heap (IN): Handle to the heap from which the block has been allocated.
//
//  - mem (IN): Pointer to the memory block being allocated.
//
//  - size (IN): Size, in bytes, of the memory block being allocated.
//
//  Return Value:
//
//    None.
//
VOID VisualLeakDetector::mapblock (HANDLE heap, LPCVOID mem, SIZE_T size)
{
    blockinfo_t         *blockinfo;
    BlockMap::Iterator   blockit;
    BlockMap            *blockmap;
    HeapMap::Iterator    heapit;
    moduleinfo_t         moduleinfo;
    ModuleSet::Iterator  moduleit;
    SIZE_T               ra;
    static SIZE_T        serialnumber = 0;

    if (m_tls.flags & VLD_TLS_MAPINPROGRESS) {
        // Prevent threads which are already mapping a block from re-entering
        // the mapping function. Otherwise infinite recursion could occur.
        return;
    }
    m_tls.flags |= VLD_TLS_MAPINPROGRESS;
    
    if (!enabled()) {
        // Memory leak detection is disabled. Don't track any allocations.
        m_tls.flags &= ~VLD_TLS_MAPINPROGRESS;
        return;
    }

    // Find the information for the module that initiated this allocation.
    ra = *((SIZE_T*)m_tls.addrfp + 1);
    moduleinfo.addrhigh = ra;
    moduleinfo.addrlow  = ra;
    moduleit = m_moduleset->find(moduleinfo);
    assert(moduleit != m_moduleset->end());
    if ((*moduleit).flags & VLD_MODULE_EXCLUDED) {
        // The module that initiated this allocation is exlcuded from leak
        // detection. Don't track this allocation.
        m_tls.flags &= ~VLD_TLS_MAPINPROGRESS;
        return;
    }

    EnterCriticalSection(&m_lock);

    // Record the block's information.
    blockinfo = new blockinfo_t;
    if (m_options & VLD_OPT_SAFE_STACK_WALK) {
        blockinfo->callstack = new SafeCallStack;
    }
    else {
        blockinfo->callstack = new FastCallStack;
    }
    if (m_options & VLD_OPT_TRACE_INTERNAL_FRAMES) {
        // Passing NULL for the frame pointer argument will force the stack
        // trace to begin at the current frame.
        blockinfo->callstack->getstacktrace(m_maxtraceframes, NULL);
    }
    else {
        // Start the stack trace at the call that first entered VLD's code.
        blockinfo->callstack->getstacktrace(m_maxtraceframes, (SIZE_T*)m_tls.addrfp);
    }
    blockinfo->serialnumber = serialnumber++;
    blockinfo->size = size;

    // Insert the block's information into the block map.
    heapit = m_heapmap->find(heap);
    if (heapit == m_heapmap->end()) {
        // We haven't mapped this heap to a block map yet. Do it now.
        mapheap(heap);
        heapit = m_heapmap->find(heap);
        assert(heapit != m_heapmap->end());
    }
    if (m_tls.flags & VLD_TLS_CRTALLOC) {
        // The heap that this block was allocated from is a CRT heap.
        (*heapit).second->flags |= VLD_HEAP_CRT;
    }
    blockmap = &(*heapit).second->blockmap;
    blockit = blockmap->insert(mem, blockinfo);
    if (blockit == blockmap->end()) {
        // A block with this address has already been allocated. The
        // previously allocated block must have been freed (probably by some
        // mechanism unknown to VLD), or the heap wouldn't have allocated it
        // again. Replace the previously allocated info with the new info.
        blockit = blockmap->find(mem);
        delete (*blockit).second->callstack;
        delete (*blockit).second;
        blockmap->erase(blockit);
        blockmap->insert(mem, blockinfo);
    }

    LeaveCriticalSection(&m_lock);
    m_tls.flags &= ~VLD_TLS_MAPINPROGRESS;
}

// mapheap - Tracks heap creation. Creates a block map for tracking individual
//   allocations from the newly created heap and then maps the heap to this
//   block map.
//
//  - heap (IN): Handle to the newly created heap.
//
//  Return Value:
//
//    None.
//
VOID VisualLeakDetector::mapheap (HANDLE heap)
{
    heapinfo_t        *heapinfo;
    HeapMap::Iterator  heapit;

    // Create a new block map for this heap and insert it into the heap map.
    heapinfo = new heapinfo_t;
    heapinfo->blockmap.reserve(BLOCKMAPRESERVE);
    heapinfo->flags = 0x0;
    heapit = m_heapmap->insert(heap, heapinfo);
    if (heapit == m_heapmap->end()) {
        // Somehow this heap has been created twice without being destroyed,
        // or at least it was destroyed without VLD's knowledge. Unmap the heap
        // from the existing heapinfo, and remap it to the new one.
        report(L"WARNING: Visual Leak Detector detected a duplicate heap (" ADDRESSFORMAT L").\n", heap);
        heapit = m_heapmap->find(heap);
        unmapheap((*heapit).first);
        m_heapmap->insert(heap, heapinfo);
    }
}

// QueryInterface - Calls to IMalloc::QueryInterface will end up here. This
//   function is just a wrapper around the real IMalloc::QueryInterface
//   implementation.
//
//  - iid (IN): COM interface ID to query about.
//
//  - object (IN): Address of a pointer to receive the requested interface
//      pointer.
//
//  Return Value:
//
//    Returns the value returned by the system implementation of
//    IMalloc::QueryInterface.
//
HRESULT VisualLeakDetector::QueryInterface (REFIID iid, LPVOID *object)
{
    assert(m_imalloc != NULL);
    return m_imalloc->QueryInterface(iid, object);
}

// Realloc - Calls to IMalloc::Realloc will end up here. This function is just a
//   wrapper around the real IMalloc::Realloc implementation that sets
//   appropriate flags to be consulted when the memory is actually allocated by
//   RtlAllocateHeap.
//
//  - mem (IN): Pointer to the memory block to reallocate.
//
//  - size (IN): Size, in bytes, of the memory block to reallocate.
//
//  Return Value:
//
//    Returns the value returned by the system implementation of
//    IMalloc::Realloc.
//
LPVOID VisualLeakDetector::Realloc (LPVOID mem, ULONG size)
{
    LPVOID block;
    SIZE_T fp;

    if (m_tls.addrfp == 0) {
        // This is the first call to enter VLD for the current allocation.
        // Record the current frame pointer.
        FRAMEPOINTER(fp);
        m_tls.addrfp = fp;
    }

    // Do the allocation. The block will be mapped by _RtlReAllocateHeap.
    assert(m_imalloc != NULL);
    block = m_imalloc->Realloc(mem, size);

    // Reset thread local flags and variables for the next allocation.
    vld.m_tls.addrfp = 0x0;
    vld.m_tls.flags &= ~VLD_TLS_CRTALLOC;
    
    return block;
}

// Release - Calls to IMalloc::Release will end up here. This function is just
//   a wrapper around the real IMalloc::Release implementation.
//
//  Return Value:
//
//    Returns the value returned by the system implementation of
//    IMalloc::Release.
//
ULONG VisualLeakDetector::Release ()
{
    assert(m_imalloc != NULL);
    return m_imalloc->Release();
}

// remapblock - Tracks reallocations. Unmaps a block from its previously
//   collected information and remaps it to updated information.
//
//  Note:If the block itself remains at the same address, then the block's
//   information can simply be updated rather than having to actually erase and
//   reinsert the block.
//
//  - heap (IN): Handle to the heap from which the memory is being reallocated.
//
//  - mem (IN): Pointer to the memory block being reallocated.
//
//  - newmem (IN): Pointer to the memory block being returned to the caller
//      that requested the reallocation. This pointer may or may not be the same
//      as the original memory block (as pointed to by "mem").
//
//  - size (IN): Size, in bytes, of the new memory block.
//
//  Return Value:
//
//    None.
//
VOID VisualLeakDetector::remapblock (HANDLE heap, LPCVOID mem, LPCVOID newmem, SIZE_T size)
{
    BlockMap::Iterator   blockit;
    BlockMap            *blockmap;
    HeapMap::Iterator    heapit;
    blockinfo_t         *info;
    moduleinfo_t         moduleinfo;
    ModuleSet::Iterator  moduleit;
    SIZE_T               ra;

    if (!enabled()) {
        // Memory leak detection is disabled. Don't track any allocations.
        return;
    }

    // Find the information for the module that initiated this allocation.
    ra = *((SIZE_T*)m_tls.addrfp + 1);
    moduleinfo.addrhigh = ra;
    moduleinfo.addrlow  = ra;
    moduleit = m_moduleset->find(moduleinfo);
    assert(moduleit != m_moduleset->end());
    if ((*moduleit).flags & VLD_MODULE_EXCLUDED) {
        // The module that initiated this allocation is excluded from leak
        // detection. Don't track this allocation.
        return;
    }

    if (newmem != mem) {
        // The block was not reallocated in-place. Instead the old block was
        // freed and a new block allocated to satisfy the new size.
        unmapblock(heap, mem);
        mapblock(heap, newmem, size);
    }
    else {
        // The block was reallocated in-place. Find the existing blockinfo_t
        // entry in the block map and update it with the new callstack and size.
        EnterCriticalSection(&m_lock);
        heapit = m_heapmap->find(heap);
        if (heapit == m_heapmap->end()) {
            // We haven't mapped this heap to a block map yet. Obviously the
            // block has also not been mapped to a blockinfo_t entry yet either,
            // so treat this reallocation as a brand-new allocation (this will
            // also map the heap to a new block map).
            mapblock(heap, newmem, size);
        }
        else {
            // Find the block's blockinfo_t structure so that we can update it.
            blockmap = &(*heapit).second->blockmap;
            blockit = blockmap->find(mem);
            if (blockit == blockmap->end()) {
                // The block hasn't been mapped to a blockinfo_t entry yet.
                // Treat this reallocation as a new allocation.
                mapblock(heap, newmem, size);
            }
            else {
                if (m_tls.flags & VLD_TLS_MAPINPROGRESS) {
                    // Prevent threads which are already mapping a block from
                    // re-entering this part of the mapping function. Otherwise
                    // infinite recursion could occur.
                    LeaveCriticalSection(&m_lock);
                    return;
                }
                m_tls.flags |= VLD_TLS_MAPINPROGRESS;

                // Found the blockinfo_t entry for this block. Update it with
                // a new callstack and new size.
                info = (*blockit).second;
                info->callstack->clear();
                if (m_options & VLD_OPT_TRACE_INTERNAL_FRAMES) {
                    // Passing NULL for the frame pointer argument will force
                    // the stack trace to begin at the current frame.
                    info->callstack->getstacktrace(m_maxtraceframes, NULL);
                }
                else {
                    // Start the stack trace at the call that first entered
                    // VLD's code.
                    info->callstack->getstacktrace(m_maxtraceframes, (SIZE_T*)m_tls.addrfp);
                }
                info->size = size;
                if (m_tls.flags & VLD_TLS_CRTALLOC) {
                    // The heap that this block was allocated from is a CRT heap.
                    (*heapit).second->flags |= VLD_HEAP_CRT;
                }

                m_tls.flags &= ~VLD_TLS_MAPINPROGRESS;
            }
        }
        LeaveCriticalSection(&m_lock);
    }
}

// reportconfig - Generates a brief report summarizing Visual Leak Detector's
//   configuration, as loaded from the vld.ini file.
//
//  Return Value:
//
//    None.
//
VOID VisualLeakDetector::reportconfig ()
{
    if (m_options & VLD_OPT_AGGREGATE_DUPLICATES) {
        report(L"    Aggregating duplicate leaks.\n");
    }
    if (wcslen(m_forcedmodulelist) != 0) {
        report(L"    Forcing inclusion of these modules in leak detection: %s\n", m_forcedmodulelist);
    }
    if (m_maxdatadump != VLD_DEFAULT_MAX_DATA_DUMP) {
        if (m_maxdatadump == 0) {
            report(L"    Suppressing data dumps.\n");
        }
        else {
            report(L"    Limiting data dumps to %lu bytes.\n", m_maxdatadump);
        }
    }
    if (m_maxtraceframes != VLD_DEFAULT_MAX_TRACE_FRAMES) {
        report(L"    Limiting stack traces to %u frames.\n", m_maxtraceframes);
    }
    if (m_options & VLD_OPT_UNICODE_REPORT) {
        report(L"    Generating a Unicode (UTF-16) encoded report.\n");
    }
    if (m_options & VLD_OPT_REPORT_TO_FILE) {
        if (m_options & VLD_OPT_REPORT_TO_DEBUGGER) {
            report(L"    Outputting the report to the debugger and to %s\n", m_reportfilepath);
        }
        else {
            report(L"    Outputting the report to %s\n", m_reportfilepath);
        }
    }
    if (m_options & VLD_OPT_SAFE_STACK_WALK) {
        report(L"    Using the \"safe\" (but slow) stack walking method.\n");
    }
    if (m_options & VLD_OPT_SELF_TEST) {
        report(L"    Perfoming a memory leak self-test.\n");
    }
    if (m_options & VLD_OPT_START_DISABLED) {
        report(L"    Starting with memory leak detection disabled.\n");
    }
    if (m_options & VLD_OPT_TRACE_INTERNAL_FRAMES) {
        report(L"    Including heap and VLD internal frames in stack traces.\n");
    }
}

// reportleaks - Generates a memory leak report for the specified heap.
//
//   Caution: This function is not thread-safe. It calls into the Debug Help
//     Library which is single-threaded. Therefore, calls to this function must
//     be synchronized.
//
//  - heap (IN): Handle to the heap for which to generate a memory leak
//      report.
//
//  Return Value:
//
//    None.
//
VOID VisualLeakDetector::reportleaks (HANDLE heap)
{
    LPCVOID             address;
    LPCVOID             block;
    BlockMap::Iterator  blockit;
    BlockMap           *blockmap;
    _CrtMemBlockHeader *crtheader;
    SIZE_T              duplicates;
    heapinfo_t         *heapinfo;
    HeapMap::Iterator   heapit;
    blockinfo_t        *info;
    SIZE_T              size;

    EnterCriticalSection(&m_lock);

    // Find the heap's information (blockmap, etc).
    heapit = m_heapmap->find(heap);
    if (heapit == m_heapmap->end()) {
        // Nothing is allocated from this heap. No leaks.
        LeaveCriticalSection(&m_lock);
        return;
    }

    heapinfo = (*heapit).second;
    blockmap = &heapinfo->blockmap;
    for (blockit = blockmap->begin(); blockit != blockmap->end(); ++blockit) {
        // Found a block which is still in the BlockMap. We've identified a
        // potential memory leak.
        block = (*blockit).first;
        info = (*blockit).second;
        address = block;
        size = info->size;
        if (heapinfo->flags & VLD_HEAP_CRT) {
            // This block is allocated to a CRT heap, so the block has a CRT
            // memory block header prepended to it.
            crtheader = (_CrtMemBlockHeader*)block;
            if (_BLOCK_TYPE(crtheader->nBlockUse) == _CRT_BLOCK) {
                // This block is marked as being used internally by the CRT.
                // The CRT will free the block after VLD is destroyed.
                continue;
            }
            // The CRT header is more or less transparent to the user, so
            // the information about the contained block will probably be
            // more useful to the user. Accordingly, that's the information
            // we'll include in the report.
            address = pbData(block);
            size = crtheader->nDataSize;
        }
        // It looks like a real memory leak.
        if (m_leaksfound == 0) {
            report(L"WARNING: Visual Leak Detector detected memory leaks!\n");
        }
        m_leaksfound++;
        report(L"---------- Block %ld at " ADDRESSFORMAT L": %u bytes ----------\n", info->serialnumber, address, size);
        if (m_options & VLD_OPT_AGGREGATE_DUPLICATES) {
            // Aggregate all other leaks which are duplicates of this one
            // under this same heading, to cut down on clutter.
            duplicates = eraseduplicates(blockit);
            if (duplicates) {
                report(L"A total of %lu leaks match this size and call stack. Showing only the first one.\n", duplicates + 1);
                m_leaksfound += duplicates;
            }
        }
        // Dump the call stack.
        report(L"  Call Stack:\n");
        info->callstack->dump(m_options & VLD_OPT_TRACE_INTERNAL_FRAMES);
        // Dump the data in the user data section of the memory block.
        if (m_maxdatadump != 0) {
            report(L"  Data:\n");
            if (m_options & VLD_OPT_UNICODE_REPORT) {
                dumpmemoryw(address, (m_maxdatadump < size) ? m_maxdatadump : size);
            }
            else {
                dumpmemorya(address, (m_maxdatadump < size) ? m_maxdatadump : size);
            }
        }
        report(L"\n");
    }

    LeaveCriticalSection(&m_lock);

    return;
}

// unmapblock - Tracks memory blocks that are freed. Unmaps the specified block
//   from the block's information, relinquishing internally allocated resources.
//
//  - heap (IN): Handle to the heap to which this block is being freed.
//
//  - mem (IN): Pointer to the memory block being freed.
//
//  Return Value:
//
//    None.
//
VOID VisualLeakDetector::unmapblock (HANDLE heap, LPCVOID mem)
{
    BlockMap::Iterator  blockit;
    BlockMap           *blockmap;
    HeapMap::Iterator   heapit;
    blockinfo_t        *info;

    EnterCriticalSection(&m_lock);

    // Find this heap's block map.
    heapit = m_heapmap->find(heap);
    if (heapit == m_heapmap->end()) {
        // We don't have a block map for this heap. We must not have monitored
        // this allocation (probably happened before VLD was initialized).
        LeaveCriticalSection(&m_lock);
        return;
    }

    // Find this block in the block map.
    blockmap = &(*heapit).second->blockmap;
    blockit = blockmap->find(mem);
    if (blockit == blockmap->end()) {
        // This block is not in the block map. We must not have monitored this
        // allocation (probably happened before VLD was initialized).
        LeaveCriticalSection(&m_lock);
        return;
    }
    // Free the blockinfo_t structure and erase it from the block map.
    info = (*blockit).second;
    delete info->callstack;
    delete info;
    blockmap->erase(blockit);

    LeaveCriticalSection(&m_lock);
}

// unmapheap - Tracks heap destruction. Unmaps the specified heap from its block
//   map. The block map is cleared and deleted, relinquishing internally
//   allocated resources.
//
//  - heap (IN): Handle to the heap which is being destroyed.
//
//  Return Value:
//
//    None.
//
VOID VisualLeakDetector::unmapheap (HANDLE heap)
{
    BlockMap::Iterator  blockit;
    BlockMap           *blockmap;
    heapinfo_t         *heapinfo;
    HeapMap::Iterator   heapit;

    // Find this heap's block map.
    EnterCriticalSection(&m_lock);
    heapit = m_heapmap->find(heap);
    if (heapit == m_heapmap->end()) {
        // This heap hasn't been mapped. We must not have monitored this heap's
        // creation (probably happened before VLD was initialized).
        LeaveCriticalSection(&m_lock);
        return;
    }

    // Free all of the blockinfo_t structures stored in the block map.
    heapinfo = (*heapit).second;
    blockmap = &heapinfo->blockmap;
    for (blockit = blockmap->begin(); blockit != blockmap->end(); ++blockit) {
        delete (*blockit).second->callstack;
        delete (*blockit).second;
    }
    delete heapinfo;

    // Remove this heap's block map from the heap map.
    m_heapmap->erase(heapit);
    LeaveCriticalSection(&m_lock);
}
