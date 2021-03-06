#include "Core.h"
#include "UnCore.h"

#include "UnObject.h"
#include "UnPackage.h"		// for Package->Name

#include "Exporters.h"


// configuration variables
bool GExportScripts      = false;
bool GExportLods         = false;
bool GDontOverwriteFiles = false;


/*-----------------------------------------------------------------------------
	Exporter function management
-----------------------------------------------------------------------------*/

#define MAX_EXPORTERS		20

struct CExporterInfo
{
	const char		*ClassName;
	ExporterFunc_t	Func;
};

static CExporterInfo exporters[MAX_EXPORTERS];
static int numExporters = 0;

void RegisterExporter(const char *ClassName, ExporterFunc_t Func)
{
	guard(RegisterExporter);
	assert(numExporters < MAX_EXPORTERS);
	CExporterInfo &Info = exporters[numExporters];
	Info.ClassName = ClassName;
	Info.Func      = Func;
	numExporters++;
	unguard;
}


// List of already exported objects

#define EXPORTED_LIST_HASH_SIZE		4096

struct ExportedObjectEntry
{
	const UnPackage* Package;
	int				ExportIndex;
	int				HashNext;

	ExportedObjectEntry()
	{}

	ExportedObjectEntry(const UObject* Obj)
	:	Package(Obj->Package)
	,	ExportIndex(Obj->PackageIndex)
	,	HashNext(0)
	{}

	int GetHash() const
	{
		return ( ((size_t)Package >> 3) ^ ExportIndex ^ (ExportIndex << 4) ) & (EXPORTED_LIST_HASH_SIZE - 1);
	}
};

struct ExportContext
{
	const UObject* LastExported;
	TArray<ExportedObjectEntry> Objects;
	int ObjectHash[EXPORTED_LIST_HASH_SIZE];
	unsigned long startTime;
	int NumSkippedObjects;

	ExportContext()
	{
		Reset();
	}

	void Reset()
	{
		LastExported = NULL;
		NumSkippedObjects = 0;
		Objects.Empty(1024);
		memset(ObjectHash, -1, sizeof(ObjectHash));
	}

	bool ItemExists(const UObject* Obj)
	{
		guard(ExportContext::ItemExists);

		ExportedObjectEntry item(Obj);
		int h = item.GetHash();
//		appPrintf("Register: %s/%s/%s (%d) : ", Obj->Package->Name, Obj->GetClassName(), Obj->Name, ProcessedObjects.Num());

		//todo: in general, this is a logic of 'TSet<UObject*>'
		int newIndex = -1;
		const ExportedObjectEntry* expEntry;
		for (newIndex = ObjectHash[h]; newIndex >= 0; newIndex = expEntry->HashNext)
		{
//			appPrintf("-- %d ", newIndex);
			expEntry = &Objects[newIndex];
			if ((expEntry->Package == item.Package) && (expEntry->ExportIndex == item.ExportIndex))
			{
//				appPrintf("-> FOUND\n");
				return true;		// the object already exists
			}
		}
		return false;

		unguard;
	}

	// Return 'false' if object already exists in a list, otherwise adds it and returns 'true'
	bool AddItem(const UObject* Obj)
	{
		guard(ExportContext::AddItem);

		if (ItemExists(Obj))
			return false;

		// not registered yet
		ExportedObjectEntry item(Obj);
		int h = item.GetHash();
		int newIndex = Objects.Add(item);
		Objects[newIndex].HashNext = ObjectHash[h];
		ObjectHash[h] = newIndex;
//		appPrintf("-> none\n");

		return true;

		unguard;
	}
};

static ExportContext ctx;

void BeginExport()
{
	ctx.startTime = appMilliseconds();
}

void EndExport(bool profile)
{
	if (profile)
	{
		assert(ctx.startTime);
		unsigned long elapsedTime = appMilliseconds() - ctx.startTime;
		appPrintf("Exported %d/%d objects in %.1f sec\n", ctx.Objects.Num() - ctx.NumSkippedObjects, ctx.Objects.Num(), elapsedTime / 1000.0f);
	}
	ctx.startTime = 0;

	ctx.Reset();
}

// return 'false' if object already registered
//todo: make a method of 'ctx' as this function is 1) almost empty, 2) not public
static bool RegisterProcessedObject(const UObject* Obj)
{
	if (Obj->Package == NULL || Obj->PackageIndex < 0)
	{
		// this object was generated; always export it to not write a more complex code here
		// Example: UMaterialWithPolyFlags
		return true;
	}

	return ctx.AddItem(Obj);
}

bool IsObjectExported(const UObject* Obj)
{
	return ctx.ItemExists(Obj);
}

//todo: move to ExportContext and reset with ctx.Reset()?
struct UniqueNameList
{
	UniqueNameList()
	{
		Items.Empty(1024);
	}

	struct Item
	{
		FString Name;
		int Count;
	};
	TArray<Item> Items;

	int RegisterName(const char *Name)
	{
		for (int i = 0; i < Items.Num(); i++)
		{
			Item &V = Items[i];
			if (V.Name == Name)
			{
				return ++V.Count;
			}
		}
		Item *N = new (Items) Item;
		N->Name = Name;
		N->Count = 1;
		return 1;
	}
};

bool ExportObject(const UObject *Obj)
{
	guard(ExportObject);

	if (!Obj) return false;
	if (strnicmp(Obj->Name, "Default__", 9) == 0)	// default properties object, nothing to export
		return true;

	static UniqueNameList ExportedNames;

	// For "uncook", different packages may have copies of the same object, which are stored with different quality.
	// For example, Gears3 has anim sets which cooked with different tracks into different maps. To be able to export
	// all versions of the file, we're adding unique numeric suffix for that. UE2 and UE4 doesn't require that.
	bool bAddUniqueSuffix = false;
	if (GUncook && Obj->Package && (Obj->Package->Game >= GAME_UE3) && (Obj->Package->Game < GAME_UE4_BASE))
	{
		bAddUniqueSuffix = true;
	}

	for (int i = 0; i < numExporters; i++)
	{
		const CExporterInfo &Info = exporters[i];
		if (Obj->IsA(Info.ClassName))
		{
			char ExportPath[1024];
			strcpy(ExportPath, GetExportPath(Obj));
			const char* ClassName = Obj->GetClassName();
			// check for duplicate name
			// get name unique index
			char uniqueName[1024];
			appSprintf(ARRAY_ARG(uniqueName), "%s/%s.%s", ExportPath, Obj->Name, ClassName);

			const char* OriginalName = NULL;
			if (bAddUniqueSuffix)
			{
				// Add unique numeric suffix when needed
				int uniqueIdx = ExportedNames.RegisterName(uniqueName);
				if (uniqueIdx >= 2)
				{
					appSprintf(ARRAY_ARG(uniqueName), "%s_%d", Obj->Name, uniqueIdx);
					appPrintf("Duplicate name %s found for class %s, renaming to %s\n", Obj->Name, ClassName, uniqueName);
					// HACK: temporary replace object name with unique one
					OriginalName = Obj->Name;
					const_cast<UObject*>(Obj)->Name = uniqueName;
				}
			}

			// Do the export with saving current "LastExported" value. This will fix an issue when object exporter
			// will call another ExportObject function then continue exporting - without the fix, calling CreateExportArchive()
			// will always fail because code will recognize object as exported for 2nd time.
			const UObject* saveLastExported = ctx.LastExported;
			Info.Func(Obj);
			ctx.LastExported = saveLastExported;

			//?? restore object name
			if (OriginalName) const_cast<UObject*>(Obj)->Name = OriginalName;
			return true;
		}
	}
	return false;

	unguardf("%s'%s'", Obj->GetClassName(), Obj->Name);
}


/*-----------------------------------------------------------------------------
	Export path functions
-----------------------------------------------------------------------------*/

static char BaseExportDir[512];

bool GUncook    = false;
bool GUseGroups = false;

void appSetBaseExportDirectory(const char *Dir)
{
	strcpy(BaseExportDir, Dir);
}


const char* GetExportPath(const UObject* Obj)
{
	guard(GetExportPath);

	static char buf[1024]; // will be returned outside

	if (!BaseExportDir[0])
		appSetBaseExportDirectory(".");	// to simplify code

#if UNREAL4
	if (Obj->Package && Obj->Package->Game >= GAME_UE4_BASE)
	{
		// Special path for UE4 games - its packages are usually have 1 asset per file, plus
		// package names could be duplicated across directory tree, with use of full package
		// paths to identify packages.
		const char* PackageName = Obj->Package->Filename;
		// Package name could be:
		// a) /(GameName|Engine)/Content/... - when loaded from pak file
		// b) [[GameName/]Content/]... - when not packaged to pak file
		if (PackageName[0] == '/') PackageName++;
		if (!strnicmp(PackageName, "Content/", 8))
		{
			PackageName += 8;
		}
		else
		{
			const char* s = strchr(PackageName, '/');
			if (s && !strnicmp(s+1, "Content/", 8))
			{
				// skip 'Content'
				PackageName = s + 9;
			}
		}
		appSprintf(ARRAY_ARG(buf), "%s/%s", BaseExportDir, PackageName);
		// Check if object's name is the same as uasset name, or if it is the same as uasset with added "_suffix".
		// Suffix may be added by ExportObject (see 'uniqueIdx').
		int len = strlen(Obj->Package->Name);
		if (!strnicmp(Obj->Name, Obj->Package->Name, len) && (Obj->Name[len] == 0 || Obj->Name[len] == '_'))
		{
			// Object's name matches with package name, so don't create a directory for it.
			// Strip package name, leave only path.
			char* s = strrchr(buf, '/');
			if (s) *s = 0;
		}
		else
		{
			// Multiple objects could be placed in this package. Strip only package's extension.
			char* s = strrchr(buf, '.');
			if (s) *s = 0;
		}
		return buf;
	}
#endif // UNREAL4

	const char* PackageName = "None";
	if (Obj->Package)
	{
		PackageName = (GUncook) ? Obj->GetUncookedPackageName() : Obj->Package->Name;
	}

	static char group[512];
	if (GUseGroups)
	{
		// get group name
		// include cooked package name when not uncooking
		Obj->GetFullName(ARRAY_ARG(group), false, !GUncook);
		// replace all '.' with '/'
		for (char* s = group; *s; s++)
			if (*s == '.') *s = '/';
	}
	else
	{
		strcpy(group, Obj->GetClassName());
	}

	appSprintf(ARRAY_ARG(buf), "%s/%s%s%s", BaseExportDir, PackageName,
		(group[0]) ? "/" : "", group);
	return buf;

	unguard;
}


const char* GetExportFileName(const UObject* Obj, const char* fmt, va_list args)
{
	guard(GetExportFileName);

	char fmtBuf[256];
	int len = vsnprintf(ARRAY_ARG(fmtBuf), fmt, args);
	if (len < 0 || len >= sizeof(fmtBuf) - 1) return NULL;

	static char buffer[1024];
	appSprintf(ARRAY_ARG(buffer), "%s/%s", GetExportPath(Obj), fmtBuf);
	return buffer;

	unguard;
}


const char* GetExportFileName(const UObject* Obj, const char* fmt, ...)
{
	va_list	argptr;
	va_start(argptr, fmt);
	const char* filename = GetExportFileName(Obj, fmt, argptr);
	va_end(argptr);

	return filename;
}


bool CheckExportFilePresence(const UObject* Obj, const char* fmt, ...)
{
	va_list	argptr;
	va_start(argptr, fmt);
	const char* filename = GetExportFileName(Obj, fmt, argptr);
	va_end(argptr);

	if (!filename) return false;
	return appFileExists(filename);
}


FArchive* CreateExportArchive(const UObject* Obj, unsigned FileOptions, const char* fmt, ...)
{
	guard(CreateExportArchive);

	bool bNewObject = false;
	if (ctx.LastExported != Obj)
	{
		// Exporting a new object, should perform some actions
		if (!RegisterProcessedObject(Obj))
			return NULL; // already exported
		bNewObject = true;
		ctx.LastExported = Obj;
	}

	va_list	argptr;
	va_start(argptr, fmt);
	const char* filename = GetExportFileName(Obj, fmt, argptr);
	va_end(argptr);

	if (!filename) return NULL;

	if (bNewObject)
	{
		// Check for file overwrite only when "new" object is saved. When saving 2nd part of the object - keep
		// overwrite logic for upper code level. If 1st object part was successfully created, then allow creation
		// of the 2nd part even if "don't overwrite" is enabled, and 2nd file already exists.
		if ((GDontOverwriteFiles && appFileExists(filename)) == false)
		{
			appPrintf("Exporting %s %s to %s\n", Obj->GetClassName(), Obj->Name, filename);
		}
		else
		{
			appPrintf("Export: file already exists %s\n", filename);
			ctx.NumSkippedObjects++;
		}
	}

	appMakeDirectoryForFile(filename);
	FFileWriter *Ar = new FFileWriter(filename, FAO_NoOpenError | FileOptions);
	if (!Ar->IsOpen())
	{
		appPrintf("Error creating file \"%s\" ...\n", filename);
		delete Ar;
		return NULL;
	}

	Ar->ArVer = 128;			// less than UE3 version (required at least for VJointPos structure)

	return Ar;

	unguard;
}
