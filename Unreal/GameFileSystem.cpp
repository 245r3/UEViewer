#include "Core.h"
#include "UnCore.h"
#include "GameFileSystem.h"

#include "UnArchiveObb.h"

// includes for file enumeration
#if _WIN32
#	include <io.h>					// for findfirst() set
#else
#	include <dirent.h>				// for opendir() etc
#	include <sys/stat.h>			// for stat()
#endif


/*-----------------------------------------------------------------------------
	Game file system
-----------------------------------------------------------------------------*/

#define MAX_GAME_FILES			32768		// DC Universe Online has more than 20k upk files
#define MAX_FOREIGN_FILES		32768

static char RootDirectory[256];


static const char *PackageExtensions[] =
{
	"u", "ut2", "utx", "uax", "usx", "ukx",
#if RUNE
	"ums",
#endif
#if BATTLE_TERR
	"bsx", "btx", "bkx",				// older version
	"ebsx", "ebtx", "ebkx", "ebax",		// newer version, with encryption
#endif
#if TRIBES3
	"pkg",
#endif
#if BIOSHOCK
	"bsm",
#endif
#if VANGUARD
	"uea", "uem",
#endif
#if LEAD
	"ass", "umd",
#endif
#if UNREAL3
	"upk", "ut3", "xxx", "umap", "udk", "map",
#endif
#if UNREAL4
	"uasset",
#endif
#if MASSEFF
	"sfm",			// Mass Effect
	"pcc",			// Mass Effect 2
#endif
#if TLR
	"tlr",
#endif
#if LEGENDARY
	"ppk", "pda",	// Legendary: Pandora's Box
#endif
#if R6VEGAS
	"uppc", "rmpc",	// Rainbow 6 Vegas 2
#endif
#if TERA
	"gpk",			// TERA: Exiled Realms of Arborea
#endif
#if APB
	"apb",			// All Points Bulletin
#endif
#if TRIBES4
	"fmap",			// Tribes: Ascend
#endif
	// other games with no special code
	"lm",			// Landmass
	"s8m",			// Section 8 map
	"ccpk",			// Crime Craft character package
};

#if UNREAL3 || UC2
#define HAS_SUPORT_FILES 1
// secondary (non-package) files
static const char *KnownExtensions[] =
{
#	if UNREAL3
	"tfc",			// Texture File Cache
	"bin",			// just in case
#	endif
#	if UC2
	"xpr",			// XBox texture container
#	endif
#	if BIOSHOCK
	"blk", "bdc",	// Bulk Content + Catalog
#	endif
#	if TRIBES4
	"rtc",
#	endif
};
#endif

// By default umodel extracts data to the current directory. Working with a huge amount of files
// could result to get "too much unknown files" error. We can ignore types of files which are
// extracted by umodel to reduce chance to get such error.
static const char *SkipExtensions[] =
{
	"tga", "dds", "bmp", "mat",				// textures, materials
	"psk", "pskx", "psa", "config",			// meshes, animations
	"ogg", "wav", "fsb", "xma", "unk",		// sounds
	"gfx", "fxa",							// 3rd party
	"md5mesh", "md5anim",					// md5 mesh
	"uc", "3d",								// vertex mesh
};

static bool FindExtension(const char *Filename, const char **Extensions, int NumExtensions)
{
	const char *ext = strrchr(Filename, '.');
	if (!ext) return false;
	ext++;
	for (int i = 0; i < NumExtensions; i++)
		if (!stricmp(ext, Extensions[i])) return true;
	return false;
}


static CGameFileInfo *GameFiles[MAX_GAME_FILES];
int GNumGameFiles = 0;
int GNumPackageFiles = 0;
int GNumForeignFiles = 0;


//!! add define USE_VFS = SUPPORT_ANDROID || UNREAL4

static TArray<FVirtualFileSystem*> GFileSystems;

static bool RegisterGameFile(const char *FullName, FVirtualFileSystem* parentVfs = NULL)
{
	guard(RegisterGameFile);

//	printf("..file %s\n", FullName);
	// return false when MAX_GAME_FILES
	if (GNumGameFiles >= ARRAY_COUNT(GameFiles))
		return false;

	if (!parentVfs)		// no nested VFSs
	{
		const char* ext = strrchr(FullName, '.');
		if (ext)
		{
			guard(MountVFS);

			ext++;
			FVirtualFileSystem* vfs = NULL;
			FArchive* reader = NULL;

#if SUPPORT_ANDROID
			if (!stricmp(ext, "obb"))
			{
//??			GForcePlatform = PLATFORM_ANDROID;
				reader = new FFileReader(FullName);
				if (!reader) return true;
				vfs = new FObbVFS();
			}
#endif // SUPPORT_ANDROID
			//!! process other VFS types here
			if (vfs)
			{
				assert(reader);
				// read VF directory
				if (!vfs->AttachReader(reader))
				{
					// something goes wrong
					delete vfs;
					delete reader;
					return true;
				}
				// add game files
				int NumVFSFiles = vfs->NumFiles();
				for (int i = 0; i < NumVFSFiles; i++)
				{
					if (!RegisterGameFile(vfs->FileName(i), vfs))
						return false;
				}
				return true;
			}

			unguard;
		}
	}

	bool IsPackage;
	if (FindExtension(FullName, ARRAY_ARG(PackageExtensions)))
	{
		IsPackage = true;
	}
	else
	{
#if HAS_SUPORT_FILES
		if (!FindExtension(FullName, ARRAY_ARG(KnownExtensions)))
#endif
		{
			// perhaps this file was exported by our tool - skip it
			if (FindExtension(FullName, ARRAY_ARG(SkipExtensions)))
				return true;
			// unknown file type
			if (++GNumForeignFiles >= MAX_FOREIGN_FILES)
				appError("Too much unknown files - bad root directory (%s)?", RootDirectory);
			return true;
		}
		IsPackage = false;
	}

	// create entry
	CGameFileInfo *info = new CGameFileInfo;
	GameFiles[GNumGameFiles++] = info;
	info->IsPackage = IsPackage;
	info->FileSystem = parentVfs;
	if (IsPackage) GNumPackageFiles++;

	if (!parentVfs)
	{
		// regular file
		FILE* f = fopen(FullName, "rb");
		if (f)
		{
			fseek(f, 0, SEEK_END);
			info->SizeInKb = (ftell(f) + 512) / 1024;
			fclose(f);
		}
		else
		{
			info->SizeInKb = 0;
		}
		// cut RootDirectory from filename
		const char *s = FullName + strlen(RootDirectory) + 1;
		assert(s[-1] == '/');
		appStrncpyz(info->RelativeName, s, ARRAY_COUNT(info->RelativeName));
	}
	else
	{
		// file in virtual file system
		info->SizeInKb = parentVfs->GetFileSize(FullName);
		appStrncpyz(info->RelativeName, FullName, ARRAY_COUNT(info->RelativeName));
	}

	// find filename
	const char* s = strrchr(info->RelativeName, '/');
	if (s) s++; else s = info->RelativeName;
	info->ShortFilename = s;
	// find extension
	s = strrchr(info->ShortFilename, '.');
	if (s) s++;
	info->Extension = s;
//	printf("..  -> %s (pkg=%d)\n", info->ShortFilename, info->IsPackage);

	return true;

	unguardf("%s", FullName);
}

static bool ScanGameDirectory(const char *dir, bool recurse)
{
	guard(ScanGameDirectory);

	char Path[256];
	bool res = true;
//	printf("Scan %s\n", dir);
#if _WIN32
	appSprintf(ARRAY_ARG(Path), "%s/*.*", dir);
	_finddatai64_t found;
	long hFind = _findfirsti64(Path, &found);
	if (hFind == -1) return true;
	do
	{
		if (found.name[0] == '.') continue;			// "." or ".."
		appSprintf(ARRAY_ARG(Path), "%s/%s", dir, found.name);
		// directory -> recurse
		if (found.attrib & _A_SUBDIR)
		{
			if (recurse)
				res = ScanGameDirectory(Path, recurse);
			else
				res = true;
		}
		else
			res = RegisterGameFile(Path);
	} while (res && _findnexti64(hFind, &found) != -1);
	_findclose(hFind);
#else
	DIR *find = opendir(dir);
	if (!find) return true;
	struct dirent *ent;
	while (/*res &&*/ (ent = readdir(find)))
	{
		if (ent->d_name[0] == '.') continue;			// "." or ".."
		appSprintf(ARRAY_ARG(Path), "%s/%s", dir, ent->d_name);
		// directory -> recurse
		// note: using 'stat64' here because 'stat' ignores large files
		struct stat64 buf;
		if (stat64(Path, &buf) < 0) continue;			// or break?
		if (S_ISDIR(buf.st_mode))
		{
			if (recurse)
				res = ScanGameDirectory(Path, recurse);
			else
				res = true;
		}
		else
			res = RegisterGameFile(Path);
	}
	closedir(find);
#endif
	return res;

	unguard;
}


void appSetRootDirectory(const char *dir, bool recurse)
{
	guard(appSetRootDirectory);
	if (dir[0] == 0) dir = ".";	// using dir="" will cause scanning of "/dir1", "/dir2" etc (i.e. drive root)
	appStrncpyz(RootDirectory, dir, ARRAY_COUNT(RootDirectory));
	ScanGameDirectory(RootDirectory, recurse);
	appPrintf("Found %d game files (%d skipped)\n", GNumGameFiles, GNumForeignFiles);
	unguardf("dir=%s", dir);
}


const char *appGetRootDirectory()
{
	return RootDirectory[0] ? RootDirectory : NULL;
}


// UE2 has simple directory hierarchy with directory depth 1
static const char *KnownDirs2[] =
{
	"Animations",
	"Maps",
	"Sounds",
	"StaticMeshes",
	"System",
#if LINEAGE2
	"Systextures",
#endif
#if UC2
	"XboxTextures",
	"XboxAnimations",
#endif
	"Textures"
};

#if UNREAL3
const char *GStartupPackage = "startup_xxx";
#endif


void appSetRootDirectory2(const char *filename)
{
	char buf[256], buf2[256];
	appStrncpyz(buf, filename, ARRAY_COUNT(buf));
	char *s;
	// replace slashes
	for (s = buf; *s; s++)
		if (*s == '\\') *s = '/';
	// cut filename
	s = strrchr(buf, '/');
	*s = 0;
	// make a copy for fallback
	strcpy(buf2, buf);

	FString root;
	int detected = 0;				// weigth; 0 = not detected
	root = buf;

	// analyze path
	const char *pCooked = NULL;
	for (int i = 0; i < 8; i++)
	{
		// find deepest directory name
		s = strrchr(buf, '/');
		if (!s) break;
		*s++ = 0;
		bool found = false;
		if (i == 0)
		{
			for (int j = 0; j < ARRAY_COUNT(KnownDirs2); j++)
				if (!stricmp(KnownDirs2[j], s))
				{
					found = true;
					break;
				}
		}
		if (found)
		{
			if (detected < 1)
			{
				detected = 1;
				root = buf;
			}
		}
		pCooked = appStristr(s, "Cooked");
		if (pCooked || appStristr(s, "Content"))
		{
			s[-1] = '/';			// put it back
			if (detected < 2)
			{
				detected = 2;
				root = buf;
				break;
			}
		}
	}
	appPrintf("Detected game root %s%s", *root, (detected == false) ? " (no recurse)" : "");
	// detect platform
	if (GForcePlatform == PLATFORM_UNKNOWN && pCooked)
	{
		pCooked += 6;	// skip "Cooked" string
		if (!strnicmp(pCooked, "PS3", 3))
			GForcePlatform = PLATFORM_PS3;
		else if (!strnicmp(pCooked, "Xenon", 5))
			GForcePlatform = PLATFORM_XBOX360;
		else if (!strnicmp(pCooked, "IPhone", 6))
			GForcePlatform = PLATFORM_IOS;
		if (GForcePlatform != PLATFORM_UNKNOWN)
		{
			static const char *PlatformNames[] =
			{
				"",
				"PC",
				"XBox360",
				"PS3",
				"iPhone",
			};
			staticAssert(ARRAY_COUNT(PlatformNames) == PLATFORM_COUNT, Verify_PlatformNames);
			appPrintf("; platform %s", PlatformNames[GForcePlatform]);
		}
	}
	// scan root directory
	appPrintf("\n");
	appSetRootDirectory(*root, detected);
}


const CGameFileInfo *appFindGameFile(const char *Filename, const char *Ext)
{
	guard(appFindGameFile);

	char buf[256];
	appStrncpyz(buf, Filename, ARRAY_COUNT(buf));

	// replace backslashes
	for (char* s = buf; *s; s++)
	{
		char c = *s;
		if (c == '\\') *s = '/';
	}

	if (Ext)
	{
		// extension is provided
		assert(!strchr(buf, '.'));
	}
	else
	{
		// check for extension in filename
		char *s = strrchr(buf, '.');
		if (s)
		{
			Ext = s + 1;	// remember extension
			*s = 0;			// cut extension
		}
	}

#if UNREAL3
	bool findStartupPackage = (strcmp(Filename, GStartupPackage) == 0);
#endif

	int nameLen = strlen(buf);
	CGameFileInfo *info = NULL;
	for (int i = 0; i < GNumGameFiles; i++)
	{
		CGameFileInfo *info2 = GameFiles[i];
#if UNREAL3
		// check for startup package
		// possible variants:
		// - startup
		if (findStartupPackage)
		{
			if (strnicmp(info2->ShortFilename, "startup", 7) != 0)
				continue;
			if (info2->ShortFilename[7] == '.')
				return info2;							// "startup.upk" (DCUO, may be others too)
			if (strnicmp(info2->ShortFilename+7, "_int.", 5) == 0)
				return info2;							// "startup_int.upk"
			if (info2->ShortFilename[7] == '_')
				info = info2;							// non-int locale, lower priority - use if when other is not detected
			continue;
		}
#endif // UNREAL3
		// verify a filename
		bool found = false;
		if (strnicmp(info2->ShortFilename, buf, nameLen) == 0)
		{
			if (info2->ShortFilename[nameLen] == '.') found = true;
		}
		if (!found)
		{
			if (strnicmp(info2->RelativeName, buf, nameLen) == 0)
			{
				if (info2->RelativeName[nameLen] == '.') found = true;
			}
		}
		if (!found) continue;
		// verify extension
		if (Ext)
		{
			if (stricmp(info2->Extension, Ext) != 0) continue;
		}
		else
		{
			// Ext = NULL => should be any package extension
			if (!info2->IsPackage) continue;
		}
		// file was found
		return info2;
	}
	return info;

	unguardf("name=%s ext=%s", Filename, Ext);
}


const char *appSkipRootDir(const char *Filename)
{
	if (!RootDirectory[0]) return Filename;

	const char *str1 = RootDirectory;
	const char *str2 = Filename;
	while (true)
	{
		char c1 = *str1++;
		char c2 = *str2++;
		// normalize names for easier checking
		if (c1 == '\\') c1 = '/';
		if (c2 == '\\') c2 = '/';
		if (!c1)
		{
			// root directory name is fully scanned
			if (c2 == '/') return str2;
			// else - something like this: root="dirname/name2", file="dirname/name2extrachars"
			return Filename;			// not in root
		}
		if (!c2) return Filename;		// Filename is shorter than RootDirectory
		if (c1 != c2) return Filename;	// different names
	}
}


FArchive *appCreateFileReader(const CGameFileInfo *info)
{
	if (!info->FileSystem)
	{
		// regular file
		char buf[256];
		appSprintf(ARRAY_ARG(buf), "%s/%s", RootDirectory, info->RelativeName);
		return new FFileReader(buf);
	}
	else
	{
		// file from virtual file system
		return info->FileSystem->CreateReader(info->RelativeName);
	}
}


void appEnumGameFilesWorker(bool (*Callback)(const CGameFileInfo*, void*), const char *Ext, void *Param)
{
	for (int i = 0; i < GNumGameFiles; i++)
	{
		const CGameFileInfo *info = GameFiles[i];
		if (!Ext)
		{
			// enumerate packages
			if (!info->IsPackage) continue;
		}
		else
		{
			// check extension
			if (stricmp(info->Extension, Ext) != 0) continue;
		}
		if (!Callback(info, Param)) break;
	}
}
