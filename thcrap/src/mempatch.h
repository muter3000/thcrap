/**
  * Touhou Community Reliant Automatic Patcher
  * Main DLL
  *
  * ----
  *
  * Direct memory patching and Import Address Table detouring.
  */

#pragma once

// IsBadReadPointer without the flawed implementation.
// Returns TRUE if [ptr] points to at least [len] bytes of valid memory in the
// address space of the current process.
BOOL VirtualCheckRegion(const void *ptr, const size_t len);
BOOL VirtualCheckCode(const void *ptr);

// Writes [len] bytes from [new] to [ptr] in the address space of the current
// or another process if the current value in [ptr] equals [prev].
int PatchRegion(void *ptr, const void *Prev, const void *New, size_t len);
int PatchRegionEx(HANDLE hProcess, void *ptr, const void *Prev, const void *New, size_t len);

/// DLL function macros
/// -------------------
// For external DLL functions, the form [(dll)_(func)] is used for the individual function pointers.

#define DLL_FUNC(dll, func) \
	dll##_##func
#define DLL_FUNC_TYPE(dll, func) \
	DLL_FUNC(dll, func)##_type

#define DLL_FUNC_DEC(dll, func) \
	extern DLL_FUNC_TYPE(dll, func) DLL_FUNC(dll, func)

#define DLL_FUNC_DEF(dll, func) \
	DLL_FUNC_TYPE(dll, func) DLL_FUNC(dll, func) = NULL

#define DLL_GET_PROC_ADDRESS(handle, dll, func) \
	DLL_FUNC(dll, func) = (DLL_FUNC_TYPE(dll, func))GetProcAddress(handle, #func)

#define DLL_GET_PROC_ADDRESS_REPORT(handle, dll, func) \
	DLL_GET_PROC_ADDRESS(handle, dll, func) \
	if(!DLL_FUNC(dll, func)) { \
		log_mboxf(NULL, MB_ICONEXCLAMATION | MB_OK, \
			"Function <%s> not found!", name); \
	}

#define DLL_SET_IAT_DETOUR(num, dll, old_func, new_func) \
	iat_detour_set(&patch[num], #old_func, DLL_FUNC(dll, old_func), new_func)
/// -------------------

/// Import Address Table patching
/// =============================

// Information about a single function to detour
typedef struct {
	const char *old_func;
	const void *old_ptr;
	const void *new_ptr;
} iat_detour_t;

/// Low-level
/// ---------
// Replaces the function pointer of [pThunk] with [new_ptr]
int func_detour(PIMAGE_THUNK_DATA pThunk, const void *new_ptr);

// Sets up [detour] by name or pointer.
// Returns 1 if the function was found and detoured, 0 if it wasn't.
int func_detour_by_name(HMODULE hMod, PIMAGE_THUNK_DATA pOrigFirstThunk, PIMAGE_THUNK_DATA pImpFirstThunk, const iat_detour_t *detour);
int func_detour_by_ptr(PIMAGE_THUNK_DATA pImpFirstThunk, const iat_detour_t *detour);
/// ---------

/// High-level
/// ----------
// Convenience function to set a single iat_detour_t entry
void iat_detour_set(iat_detour_t* detour, const char *old_func, const void *old_ptr, const void *new_ptr);

// Sets up [detour] using the most appropriate low-level detouring function.
int iat_detour_func(HMODULE hMod, PIMAGE_IMPORT_DESCRIPTOR pImpDesc, const iat_detour_t *detour);

// Detours [detour_count] functions in the [iat_detour] array
int iat_detour_funcs(HMODULE hMod, const char *dll_name, iat_detour_t *iat_detour, const size_t detour_count);
/// ----------

/// Detour chaining
/// ---------------
/**
  * This system allows the support of an arbitrary number of detour hooks
  * for any given API function.
  *
  * Prior to this, detours weren't really reliably extensible by plugins.
  * Since they mostly need to call the original function they hook, they used
  * to imply a certain hierarchy and required knowledge of the entire
  * detour sequence (with the win32_utf8 functions mostly being on the lowest
  * level) in order to work as intended. This could have led to cases where
  * newer detours could have potentially override existing functionality
  * (see https://bitbucket.org/nmlgc/thpatch-bugs/issue/18).
  *
  * It took a while to come up with a proper solution to this problem that
  * wouldn't impose any restrictions on what detour functions are able to do.
  * After all, they may need to call the original function multiple times,
  * or pass locally constructed data as a parameter.
  *
  * After some further thought, it then became clear that, for most reasonable
  * cases where detours only modify function parameters or return values, the
  * order in which they are called doesn't actually matter. Sure, win32_utf8
  * must be at the bottom, but as an integral part of the engine, it can
  * always be treated separately.
  *
  * This realization allows us to build "detour chains": Instead of patching
  * the IAT directly with some function pointer, we first prepare lists of
  * function pointers for every function in every DLL we want to detour. Once
  * we have a module handle, we patch the IAT with the address of the first
  * function in each chain.
  *
  * Each detour function should then call a custom chaining function anywhere
  * it previously called the original function. This will recursively call
  * all subsequent hooks in the chain, with the original function at the end.
  *
  * However, the chaining function has the downside of adding one more
  * instance of unportable, architecture-specific code to the engine. It needs
  * to pass a variable number of parameters to functions with a constant
  * parameter list. Since C doesn't have a way of resolving a va_list back
  * into individual parameters, our only option is to directly build the
  * function call stack using inline assembly.
  */

/**
  * Adds one new detour hook for a number of functions in [dll_name].
  * Expects additional parameters of the form
  *
  *	"exported name", new_func_ptr,
  *	"exported name", new_func_ptr,
  *	...,
  *
  * terminated by a single NULL parameter.
  */
int detour_cache_add(const char *dll_name, ...);

// Applies the cached detours to [hMod].
int iat_detour_apply(HMODULE hMod);

// Calls the next detour hook for [func_name] in [dll_name], passing
// [arg_count] variadic parameters.
// The list index is derived from [caller], which should be a pointer to the
// currently executing *detour* function. This has to be a function that was
// previously added using detour_add(), so be careful if you're using helper
// functions. If [caller] happens to be the last detour for the given
// function, the original function is called.
// [caller] can also be NULL to run the entire chain from the beginning.
size_t detour_next(const char *dll_name, const char *func_name, void *caller, size_t arg_count, ...);

// *Not* a module function because we want to call it manually after
// everything else has been cleaned up.
void detour_exit(void);
/// ---------------

/// =============================
