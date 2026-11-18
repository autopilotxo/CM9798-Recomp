/* dlls.h - copy the DLLs a Windows program needs next to it (see dlls.c) */
#pragma once
/* the DLLs `exe` loads (and those load), found next to it or on the PATH, copied into `to`;
   Windows' own are left out. Returns how many were copied; log gets a line for any not found. */
int copy_program_dlls(const char *exe, const char *to, void (*log)(const char *text));
