/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2005 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers
Copyright (C) 2016 Sascha Willems (Android parts)

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

#include "arch_def.h"
#include "quakedef.h"

#include <sys/types.h>
#include <errno.h>
#include <unistd.h>
#ifdef PLATFORM_OSX
#include <libgen.h>	/* dirname() and basename() */
#endif
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#ifdef DO_USERDIRS
#include <pwd.h>
#endif

#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL/SDL.h>
#endif
#else
#include "SDL.h"
#endif

#include "cpu-features.h"

qboolean		isDedicated;
cvar_t		sys_throttle = { "sys_throttle", "0.02", CVAR_ARCHIVE };

#define	MAX_HANDLES		32	/* johnfitz -- was 10 */
static FILE		*sys_handles[MAX_HANDLES];

/*
if LOAD_FROM_ASSETS is defined:
Use Android's asset manager for loading files stored in the applications apk - Sascha Willems
Writing files using fopen and fwrite works with permission to write to SD card
*/


static int findhandle(void)
{
	int i;

	for (i = 1; i < MAX_HANDLES; i++)
	{
		if (!sys_handles[i])
			return i;
	}
	Sys_Error("out of handles");
	return -1;
}

long Sys_filelength(FILE *f)
{
	long		pos, end;

	pos = ftell(f);
	fseek(f, 0, SEEK_END);
	end = ftell(f);
	fseek(f, pos, SEEK_SET);

	return end;
}

#ifdef LOAD_FROM_ASSETS
AAsset* Sys_FileOpenRead(const char *path)
{
	// Asset path must not start with ./
	char filepath[512];
	int s = 0;
	if ((path[0] == '.') && (path[1] == '/'))
		s = 2;
	memset(filepath, 0, 512);
	memcpy(filepath, path + s, strlen(path) - s);
	Sys_Printf("Opening asset %s", filepath);

	AAsset* asset = AAssetManager_open(android_app->activity->assetManager, filepath, AASSET_MODE_STREAMING);
	if (!asset)
	{
		Sys_Printf("Asset not found");
		return NULL;
	}
	size_t size = AAsset_getLength(asset);

	Sys_Printf("Asset size %d", size);

	return asset;
}

int Sys_FileOpenWrite(const char *path)
{
	FILE	*f;
	int		i;

	i = findhandle();
	f = fopen(path, "wb");

	if (!f)
		Sys_Error("Error opening %s: %s", path, strerror(errno));

	sys_handles[i] = f;
	return i;
}

void Sys_FileClose(AAsset *asset)
{
	AAsset_close(asset);
}

void Sys_FileSeek(AAsset *asset, int position)
{
	AAsset_seek64(asset, position, SEEK_SET);
}

int Sys_FileRead(AAsset *asset, void *dest, int count)
{
	if (!asset)
	 	Sys_Error("asset is null");
	return AAsset_read(asset, dest, count);
}
#else
int Sys_FileOpenRead (const char *path, int *hndl)
{
	FILE	*f;
	int	i, retval;

	i = findhandle ();
	f = fopen(path, "rb");

	if (!f)
	{
		*hndl = -1;
		retval = -1;
	}
	else
	{
		sys_handles[i] = f;
		*hndl = i;
		retval = Sys_filelength(f);
	}

	return retval;
}

int Sys_FileOpenWrite (const char *path)
{
	FILE	*f;
	int		i;

	i = findhandle ();
	f = fopen(path, "wb");

	if (!f)
		Sys_Error ("Error opening %s: %s", path, strerror(errno));

	sys_handles[i] = f;
	return i;
}

void Sys_FileClose (int handle)
{
	fclose (sys_handles[handle]);
	sys_handles[handle] = NULL;
}

void Sys_FileSeek (int handle, int position)
{
	fseek (sys_handles[handle], position, SEEK_SET);
}

int Sys_FileRead (int handle, void *dest, int count)
{
	return fread (dest, 1, count, sys_handles[handle]);
}
#endif

int Sys_FileWrite(int handle, const void *data, int count)
{
	return fwrite(data, 1, count, sys_handles[handle]);
}

int Sys_FileTime(const char *path)
{
	FILE	*f;

	f = fopen(path, "rb");

	if (f)
	{
		fclose(f);
		return 1;
	}

	return -1;
}

static int Sys_NumCPUs(void)
{
	int numcpus = android_getCpuCount();
	return numcpus;
}

void Sys_Init(void)
{
	// Get a path we can write to on external storage
	const char* base_path = android_app->activity->externalDataPath;
	host_parms->userdir = base_path;
	Sys_Printf("userdir %s", host_parms->userdir);
	host_parms->numcpus = Sys_NumCPUs();
	Sys_Printf("Detected %d CPUs.\n", host_parms->numcpus);
}

void Sys_mkdir(const char *path)
{
	int rc = mkdir(path, 0777);
	if (rc != 0 && errno == EEXIST)
	{
		struct stat st;
		if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
			rc = 0;
	}
	if (rc != 0)
	{
		rc = errno;
		Sys_Error("Unable to create directory %s: %s", path, strerror(rc));
	}
}

static const char errortxt1[] = "\nERROR-OUT BEGIN\n\n";
static const char errortxt2[] = "\nQUAKE ERROR: ";

void Sys_Error(const char *error, ...)
{
	va_list		argptr;
	char		text[1024];

	fputs(errortxt1, stderr);

	Host_Shutdown();

	va_start(argptr, error);
	__android_log_vprint(ANDROID_LOG_ERROR, APPTAG, error, argptr);
	va_end(argptr);

	fputs(errortxt2, stderr);
	fputs(text, stderr);
	fputs("\n\n", stderr);
	if (!isDedicated)
		PL_ErrorDialog(text);

	exit(1);
}

void Sys_Printf(const char *fmt, ...)
{
	va_list argptr;

	va_start(argptr, fmt);
	__android_log_vprint(ANDROID_LOG_INFO, APPTAG, fmt, argptr);
	va_end(argptr);
}

void Sys_Quit(void)
{
	Host_Shutdown();

	exit(0);
}

double Sys_DoubleTime(void)
{
	return SDL_GetTicks() / 1000.0;
}

const char *Sys_ConsoleInput(void)
{
	static char	con_text[256];
	static int	textlen;
	char		c;
	fd_set		set;
	struct timeval	timeout;

	FD_ZERO(&set);
	FD_SET(0, &set);	// stdin
	timeout.tv_sec = 0;
	timeout.tv_usec = 0;

	while (select(1, &set, NULL, NULL, &timeout))
	{
		read(0, &c, 1);
		if (c == '\n' || c == '\r')
		{
			con_text[textlen] = '\0';
			textlen = 0;
			return con_text;
		}
		else if (c == 8)
		{
			if (textlen)
			{
				textlen--;
				con_text[textlen] = '\0';
			}
			continue;
		}
		con_text[textlen] = c;
		textlen++;
		if (textlen < (int) sizeof(con_text))
			con_text[textlen] = '\0';
		else
		{
			// buffer is full
			textlen = 0;
			con_text[0] = '\0';
			Sys_Printf("\nConsole input too long!\n");
			break;
		}
	}

	return NULL;
}

void Sys_Sleep(unsigned long msecs)
{
	/*	usleep (msecs * 1000);*/
	SDL_Delay(msecs);
}

void Sys_SendKeyEvents(void)
{
	IN_SendKeyEvents();
}

