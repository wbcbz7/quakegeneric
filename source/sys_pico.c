/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// sys_null.h -- null system driver to aid porting efforts

#include <pico/stdlib.h>
#include <hardware/watchdog.h>
#include "quakedef.h"
#include "sys.h"

qboolean isDedicated;

/*
===============================================================================

FILE IO

===============================================================================
*/

#define MAX_HANDLES             10
static FIL* sys_handles[MAX_HANDLES] = { 0 };

int findhandle (void)
{
	int             i;
	
	for (i=1 ; i<MAX_HANDLES ; i++)
		if (!sys_handles[i])
			return i;
	Sys_Error ("out of handles");
	return -1;
}

FIL* Sys_File(int hndl) {
	if (hndl < 0) return NULL;
	return sys_handles[hndl];
}

int Sys_FileOpenRead (char *path, int *hndl)
{
	FIL* f = malloc(sizeof(FIL));
	if (!f) {
		*hndl = -1;
		return -1;
	}
	int i = findhandle ();
	if (f_open(f, path, FA_READ) != FR_OK)
	{
		free(f);
		*hndl = -1;
		return -1;
	}
	sys_handles[i] = f;
	*hndl = i;
	return f_size(f);
}

int Sys_FileOpenWrite (char *path)
{
	FIL* f = malloc(sizeof(FIL));
	if (!f) {
		Sys_Error ("Error opening %s: %s", path, "NOMEM");
		return -1;
	}
	int i = findhandle ();
	if (f_open(f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		free(f);
		Sys_Error ("Error opening %s: %s", path, "f_open");
		return -1;
	}
	sys_handles[i] = f;
	return i;
}

void Sys_FileClose (int handle)
{
	f_close(sys_handles[handle]);
	free(sys_handles[handle]);
	sys_handles[handle] = NULL;
}

void Sys_FileSeek (int handle, int position)
{
	f_lseek(sys_handles[handle], position);
}

int Sys_FileRead (int handle, void *dest, int count)
{
	UINT rb = 0;
	f_read (sys_handles[handle], dest, count, &rb);
	return rb;
}

int Sys_FileWrite (int handle, void *data, int count)
{
	UINT wb = 0;
	f_write (sys_handles[handle], data, count, &wb);
	return wb;
}

typedef union {
	FIL f;
	FILINFO fi;
} FIL_FILINFO;

static FIL_FILINFO tmp_f_struct;
static char buf[256];

int Sys_FileTime (char *path)
{
    if (FR_OK != f_stat(path, &tmp_f_struct.fi)) {
		return -1;
	}
	return ((int)tmp_f_struct.fi.fdate << 16) | tmp_f_struct.fi.ftime;
}

void Sys_mkdir (char *path)
{
	f_mkdir(path);
}


/*
===============================================================================

SYSTEM IO

===============================================================================
*/

void Sys_MakeCodeWriteable (unsigned long startaddr, unsigned long length)
{
}

void Sys_Error (char *error, ...) {
	UINT wb;
	va_list         argptr;
	va_start (argptr,error);
    vsnprintf(buf, 256, error, argptr);
	va_end (argptr);

	// write error message on screen
	tputstr( 0*5, 0, "Sys_Error: ");
	tputstr(11*5, 0, buf);

	// then write to the file
	if (FR_OK == f_open(&tmp_f_struct.f, "quake.log", FA_WRITE | FA_OPEN_ALWAYS | FA_OPEN_APPEND)) {
		f_write(&tmp_f_struct.f, "Sys_Error: ", 11, &wb);  
		f_write(&tmp_f_struct.f, buf, strlen(buf), &wb);
		f_write(&tmp_f_struct.f, "\n", 1, &wb);
		f_close(&tmp_f_struct.f);
	}
	while(true) {
		sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
		// TODO: input
	}
	//Sys_Quit();
}

int Sys_Fscanf(FIL *f, char *fmt, ...)
{
    char line[256];
    UINT rb = 0;
    TCHAR* fr;

    DWORD start = f_tell(f);

    /* читаем строго одну строку */
    fr = f_gets(line, sizeof(line), f);
    if (fr && line[0] == '\0') {
        return EOF;
    }

    /* подготовить va_list */
    va_list ap;
    va_start(ap, fmt);

    int scanned = vsscanf(line, fmt, ap);   /* парсинг */
    va_end(ap);

    /* если вообще ничего не подошло — откатить позицию файла */
    if (scanned <= 0) {
        f_lseek(f, start);
        return scanned;
    }

    return scanned;
}

void Sys_Fprintf (FIL* f, char *fmt, ...) {
	UINT wb;
	va_list argptr;
	va_start (argptr, fmt);
    vsnprintf(buf, 256, fmt, argptr);
	va_end (argptr);
	f_write(f, buf, strlen(buf), &wb);
}

void Sys_Printf (char *fmt, ...)
{	
	UINT wb;
	va_list         argptr;
	if (quietlog) return;
	if (FR_OK != f_open(&tmp_f_struct.f, "quake.log", FA_WRITE | FA_OPEN_ALWAYS | FA_OPEN_APPEND)) return;
	va_start (argptr,fmt);
    vsnprintf(buf, 256, fmt, argptr);
	va_end (argptr);
	f_write(&tmp_f_struct.f, buf, strlen(buf), &wb);
	f_close(&tmp_f_struct.f);
}

void Sys_Quit (void)
{
	f_unlink(".firmware");
    watchdog_enable(1, true);
    while(true) ;
    __unreachable();
}

double Sys_FloatTime (void)
{
	return time_us_64() * 1e-6;   // seconds since the ARM-chip started
}

char *Sys_ConsoleInput (void)
{
	return NULL;
}

void Sys_Sleep (void)
{
}

void Sys_SendKeyEvents (void)
{
	IN_Commands();
}

void Sys_HighFPPrecision (void)
{
}

void Sys_LowFPPrecision (void)
{
}

void _unlink(const char* path) {
	f_unlink(path);
}
