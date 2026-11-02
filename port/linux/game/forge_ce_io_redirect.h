/*
 * OpenCE Forge - Custom Edition & Map Share File I/O Redirection
 * Routes custom_maps/ lookups for Forge-built maps to forge_maps/<name>/<name>.map
 * and serves embedded .bmp / .txt sidecars from the 2048-byte map header.
 */

#ifndef FORGE_CE_IO_REDIRECT_H
#define FORGE_CE_IO_REDIRECT_H

struct file_reference *forge_ce_file_reference_create_from_path(
	struct file_reference *reference,
	char const *full_path,
	boolean is_directory);
FILE *forge_ce_fopen(char const *filename, char const *mode);
size_t forge_ce_fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
int forge_ce_fseek(FILE *stream, long offset, int whence);
long forge_ce_ftell(FILE *stream);
int forge_ce_fclose(FILE *stream);
HANDLE WINAPI forge_ce_FindFirstFileA(LPCSTR file_name, LPWIN32_FIND_DATAA data);
BOOL WINAPI forge_ce_FindNextFileA(HANDLE find_file, LPWIN32_FIND_DATAA data);
BOOL WINAPI forge_ce_CloseHandle(HANDLE hObject);

#undef file_reference_create_from_path
#define file_reference_create_from_path forge_ce_file_reference_create_from_path

#undef fopen
#define fopen forge_ce_fopen

#undef FindFirstFileA
#define FindFirstFileA forge_ce_FindFirstFileA

#undef FindNextFileA
#define FindNextFileA forge_ce_FindNextFileA

#undef CloseHandle
#define CloseHandle forge_ce_CloseHandle

#ifdef FORGE_CE_IO_STDIO
#undef fread
#define fread forge_ce_fread
#undef fseek
#define fseek forge_ce_fseek
#undef ftell
#define ftell forge_ce_ftell
#undef fclose
#define fclose forge_ce_fclose
#endif

#endif /* FORGE_CE_IO_REDIRECT_H */
