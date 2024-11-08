#include "mmap.h"

bool mmap(HANDLE hProcess, PBYTE pSrcData, SIZE_T FileSize, bool ClearHeader, bool ClearNonNeededSections, bool AdjustProtections, bool SEHExceptionSupport, DWORD fdwReason, LPVOID lpReserved) {
	PIMAGE_NT_HEADERS pOldNtHeader = NULL;
	PIMAGE_OPTIONAL_HEADER pOldOptHeader = NULL;
	PIMAGE_FILE_HEADER pOldFileHeader = NULL;
	PBYTE pTargetBase = NULL;

	if (((PIMAGE_DOS_HEADER)pSrcData)->e_magic != 0x5A4D) {
		MM_LOG("Invalid DOS header\n");
		return false;
	}

	pOldNtHeader = (PIMAGE_NT_HEADERS)(pSrcData + ((PIMAGE_DOS_HEADER)pSrcData)->e_lfanew);
	pOldOptHeader = &pOldNtHeader->OptionalHeader;
	pOldFileHeader = &pOldNtHeader->FileHeader;

	if (pOldFileHeader->Machine != CURRENT_ARCH) {
		MM_LOG("Invalid architecture\n");
		return false;
	}

	pTargetBase = (PBYTE)(VirtualAllocEx(hProcess, NULL, pOldOptHeader->SizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if (!pTargetBase) {
		MM_LOG("Can't allocate memory in target process 0x%lu\n", GetLastError());
		return false;
	}

	DWORD oldp = 0;
	VirtualProtectEx(hProcess, pTargetBase, pOldOptHeader->SizeOfImage, PAGE_EXECUTE_READWRITE, &oldp);

	MANUAL_MAPPING_DATA data{ 0 };
	data.pLoadLibraryA = LoadLibraryA;
	data.pGetProcAddress = GetProcAddress;
#ifdef _WIN64
	data.pRtlAddFunctionTable = (f_RtlAddFunctionTable)RtlAddFunctionTable;
#else 
	SEHExceptionSupport = false;
#endif
	data.pbase = pTargetBase;
	data.fdwReasonParam = fdwReason;
	data.reservedParam = lpReserved;
	data.SEHSupport = SEHExceptionSupport;

	PVOID pPeb = Utils::findRemotePEB(hProcess);
    if (!pPeb) {
        MM_LOG("Failed to find remote PEB.\n");
        return false;
    }

    PEB peb = Utils::rpm<PEB>(hProcess, pPeb);
    if (!peb.ImageBaseAddress) {
        MM_LOG("Failed to read remote PEB.\n");
        return false;
    }

#ifdef _WIN64
	PBYTE remoteBase = (PBYTE)peb.ImageBaseAddress;
#else
	// Probably PEB in def.h doesn't work for x86
	// Too lazy to fix, this does the job.
	PBYTE remoteBase = *(PBYTE*)((uintptr_t)&peb + 0x8);
#endif

	printf("\n[+] Remote base address: 0x%p\n", remoteBase);

    IMAGE_DOS_HEADER remoteDosHeaders = Utils::rpm<IMAGE_DOS_HEADER>(hProcess, remoteBase);
    IMAGE_NT_HEADERS remoteNtHeaders = Utils::rpm<IMAGE_NT_HEADERS>(hProcess, (PBYTE)remoteBase + remoteDosHeaders.e_lfanew);

    IMAGE_IMPORT_DESCRIPTOR remoteImportDesc = Utils::rpm<IMAGE_IMPORT_DESCRIPTOR>(hProcess, ((PBYTE)remoteBase + remoteNtHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress));

    int descCount = 0;
    while (remoteImportDesc.Name) {
        IMAGE_THUNK_DATA firstThunk = Utils::rpm<IMAGE_THUNK_DATA>(hProcess, (PBYTE)remoteBase + remoteImportDesc.FirstThunk);
        IMAGE_THUNK_DATA originalFirstThunk = Utils::rpm<IMAGE_THUNK_DATA>(hProcess, (PBYTE)remoteBase + remoteImportDesc.OriginalFirstThunk);

        int thunkCount = 0;
        while (originalFirstThunk.u1.AddressOfData) {

            char szFnName[256];
            Utils::rpmb(hProcess, (PBYTE)remoteBase + originalFirstThunk.u1.AddressOfData + 2, szFnName, sizeof(szFnName));

			int thunkOffset = thunkCount * sizeof(IMAGE_THUNK_DATA);
            if (strcmp(szFnName, FN_TO_HOOK) == 0) {
                data.pHkFnLocAddress = (PVOID*)((uintptr_t)remoteBase + remoteImportDesc.FirstThunk + thunkOffset);
				data.pHkFnAddress = Utils::rpm<PVOID>(hProcess, data.pHkFnLocAddress);

                break;
            }

            thunkCount++;
            firstThunk = Utils::rpm<IMAGE_THUNK_DATA>(hProcess, (PBYTE)remoteBase + remoteImportDesc.FirstThunk + thunkOffset);
            originalFirstThunk = Utils::rpm<IMAGE_THUNK_DATA>(hProcess, (PBYTE)remoteBase + remoteImportDesc.OriginalFirstThunk + thunkOffset);
        }

        descCount++;
        remoteImportDesc = Utils::rpm<IMAGE_IMPORT_DESCRIPTOR>(hProcess, (PBYTE)remoteBase + remoteNtHeaders.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress + descCount * sizeof(IMAGE_IMPORT_DESCRIPTOR));
    }

	printf("\n[+] Hook address: 0x%p\n", data.pHkFnLocAddress);
	printf("[+] *Hook address (%s): 0x%p\n", FN_TO_HOOK, data.pHkFnAddress);

	if (!Utils::wpmb(hProcess, pTargetBase, pSrcData, 0x1000)) {
		MM_LOG("Can't write header 0x%lu\n", GetLastError());
		VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
		return false;
	}

	PIMAGE_SECTION_HEADER pSectionHeader = IMAGE_FIRST_SECTION(pOldNtHeader);
	for (UINT i = 0; i != pOldFileHeader->NumberOfSections; ++i, ++pSectionHeader) {
		if (pSectionHeader->SizeOfRawData) {
			if (!Utils::wpmb(hProcess, pTargetBase + pSectionHeader->VirtualAddress, pSrcData + pSectionHeader->PointerToRawData, pSectionHeader->SizeOfRawData)) {
				MM_LOG("Can't write section %s 0x%lu\n", pSectionHeader->Name, GetLastError());
				VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
				return false;
			}
		}
	}

	PBYTE mappingDataAlloc = (PBYTE)(VirtualAllocEx(hProcess, NULL, sizeof(MANUAL_MAPPING_DATA), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if (!mappingDataAlloc) {
		MM_LOG("Memory allocation failed (ex) 0x%lu\n", GetLastError());
		VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
		return false;
	}

	if (!Utils::wpmb(hProcess, mappingDataAlloc, &data, sizeof(MANUAL_MAPPING_DATA))) {
		MM_LOG("Can't write mapping data 0x%lu\n", GetLastError());
		VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
		VirtualFreeEx(hProcess, mappingDataAlloc, 0, MEM_RELEASE);
		return false;
	}

	PVOID pShellcode = VirtualAllocEx(hProcess, NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!pShellcode) {
		MM_LOG("Memory allocation failed (shellcode) 0x%lu\n", GetLastError());
		VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
		VirtualFreeEx(hProcess, mappingDataAlloc, 0, MEM_RELEASE);
		return false;
	}

	if (!Utils::wpmb(hProcess, pShellcode, (PVOID)Shellcode, 0x1000)) {
		MM_LOG("Can't write shell code 0x%lu\n", GetLastError());
		VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
		VirtualFreeEx(hProcess, mappingDataAlloc, 0, MEM_RELEASE);
		VirtualFreeEx(hProcess, pShellcode, 0, MEM_RELEASE);
		return false;
	}

#ifdef _WIN64
	// pShellCode + 0x9: mov rax, 0xFFFFFFFFDEADEEF
    Utils::wpmb(hProcess, (PVOID)((uintptr_t)pShellcode + 0x9), &mappingDataAlloc, sizeof(PVOID));
#else
	Utils::wpmb(hProcess, (PVOID)((uintptr_t)pShellcode + 0x8), &mappingDataAlloc, sizeof(PVOID));
#endif

	printf("\n[+] Shellcode address: 0x%p\n", pShellcode);
	printf("[+] Mapping data address: 0x%p\n", mappingDataAlloc);

	ULONG oldIatProtect = 0;
	VirtualProtectEx(hProcess, data.pHkFnLocAddress, sizeof(PVOID), PAGE_READWRITE, &oldIatProtect);
	Utils::wpmb(hProcess, data.pHkFnLocAddress, &pShellcode, sizeof(PVOID));

	HINSTANCE hCheck = NULL;
	while (!hCheck) {
		DWORD exitcode = 0;
		GetExitCodeProcess(hProcess, &exitcode);
		if (exitcode != STILL_ACTIVE) {
			MM_LOG("Process exited with code %lu\n", exitcode);
			return false;
		}

		MANUAL_MAPPING_DATA data_checked{ 0 };
		Utils::rpmb(hProcess, mappingDataAlloc, &data_checked, sizeof(data_checked));
		hCheck = data_checked.hMod;

		if (hCheck == (HINSTANCE)0x404040) {
			MM_LOG("Wrong mapping ptr\n");
			VirtualFreeEx(hProcess, pTargetBase, 0, MEM_RELEASE);
			VirtualFreeEx(hProcess, mappingDataAlloc, 0, MEM_RELEASE);
			VirtualFreeEx(hProcess, pShellcode, 0, MEM_RELEASE);
			return false;
		}
		else if (hCheck == (HINSTANCE)0x505050) {
			MM_LOG("WARNING: Exception support failed!\n");
		}

		Sleep(10);
	}

	ULONG ulOldProtect = 0;
	VirtualProtectEx(hProcess, data.pHkFnLocAddress, sizeof(PVOID), oldIatProtect, &ulOldProtect);

	PBYTE emptyBuffer = (PBYTE)malloc(1024 * 1024 * 20);
	if (emptyBuffer == NULL) {
		MM_LOG("Can't allocate memory for clearing\n");
		return false;
	}

	memset(emptyBuffer, 0, 1024 * 1024 * 20);

	if (ClearHeader) {
		if (!Utils::wpmb(hProcess, pTargetBase, emptyBuffer, 0x1000)) {
			MM_LOG("WARNING!: Can't clear HEADER\n");
		}
	}

	if (ClearNonNeededSections) {
		pSectionHeader = IMAGE_FIRST_SECTION(pOldNtHeader);
		for (UINT i = 0; i != pOldFileHeader->NumberOfSections; ++i, ++pSectionHeader) {
			if (pSectionHeader->Misc.VirtualSize) {
				if ((SEHExceptionSupport ? 0 : strcmp((PCHAR)pSectionHeader->Name, ".pdata") == 0) ||
					strcmp((PCHAR)pSectionHeader->Name, ".rsrc") == 0 ||
					strcmp((PCHAR)pSectionHeader->Name, ".reloc") == 0) {
					if (!Utils::wpmb(hProcess, pTargetBase + pSectionHeader->VirtualAddress, emptyBuffer, pSectionHeader->Misc.VirtualSize)) {
						MM_LOG("WARNING: Can't clear section %s\n", (PCHAR)pSectionHeader->Name);
					}
				}
			}
		}
	}

	if (AdjustProtections) {
		pSectionHeader = IMAGE_FIRST_SECTION(pOldNtHeader);
		for (UINT i = 0; i != pOldFileHeader->NumberOfSections; ++i, ++pSectionHeader) {
			if (pSectionHeader->Misc.VirtualSize) {
				DWORD old = 0;
				DWORD newP = PAGE_READONLY;

				if ((pSectionHeader->Characteristics & IMAGE_SCN_MEM_WRITE) > 0)
					newP = PAGE_READWRITE;
				else if ((pSectionHeader->Characteristics & IMAGE_SCN_MEM_EXECUTE) > 0)
					newP = PAGE_EXECUTE_READ;
				if (!VirtualProtectEx(hProcess, pTargetBase + pSectionHeader->VirtualAddress, pSectionHeader->Misc.VirtualSize, newP, &old))
					MM_LOG("WARNING: section %s not set as %lX\n", (PCHAR)pSectionHeader->Name, newP);
			}
		}
		DWORD old = 0;
		VirtualProtectEx(hProcess, pTargetBase, IMAGE_FIRST_SECTION(pOldNtHeader)->VirtualAddress, PAGE_READONLY, &old);
	}

	if (!Utils::wpmb(hProcess, pShellcode, emptyBuffer, 0x1000)) {
		MM_LOG("WARNING: Can't clear shell code\n");
	}
	if (!VirtualFreeEx(hProcess, pShellcode, 0, MEM_RELEASE)) {
		MM_LOG("WARNING: can't release shell code memory\n");
	}
	if (!VirtualFreeEx(hProcess, mappingDataAlloc, 0, MEM_RELEASE)) {
		MM_LOG("WARNING: can't release mapping data memory\n");
	}

	return true;
}

void mmlog(PCSTR fmt, ...) {
	va_list args;
	va_start(args, fmt);

	FILE* fp;
	errno_t err;

	err = fopen_s(&fp, "error.log", "a");
	if (!err) {
        time_t now = time(NULL);
        struct tm local;
        if (localtime_s(&local, &now) == 0) {
            char timestamp[32];
            if (strftime(timestamp, sizeof(timestamp), "[%Y:%m:%d - %H:%M:%S] ", &local)) {
                fprintf(fp, "%s", timestamp);
                vfprintf(fp, fmt, args);
            } else {
                printf("Failed to format time.\n");
            }
        }

		fclose(fp);
	}

	va_end(args);
}

#pragma runtime_checks("", off )
#pragma optimize("", off )
void __stdcall Shellcode() {
#ifdef _WIN64
    PMANUAL_MAPPING_DATA pData = (PMANUAL_MAPPING_DATA)(0xFFFFFFFFDEADEEF);
#else
	PMANUAL_MAPPING_DATA pData = (PMANUAL_MAPPING_DATA)(0xDEADBEEF);
#endif
	*pData->pHkFnLocAddress = pData->pHkFnAddress;

	if (!pData) {
		pData->hMod = (HINSTANCE)0x404040;
		return;
	}

	PBYTE pBase = pData->pbase;
	auto* pOpt = &((PIMAGE_NT_HEADERS)(pBase + ((PIMAGE_DOS_HEADER)pBase)->e_lfanew))->OptionalHeader;

	auto _LoadLibraryA = pData->pLoadLibraryA;
	auto _GetProcAddress = pData->pGetProcAddress;
#ifdef _WIN64
	auto _RtlAddFunctionTable = pData->pRtlAddFunctionTable;
#endif
	auto _DllMain = (f_DLL_ENTRY_POINT)(pBase + pOpt->AddressOfEntryPoint);
	PBYTE LocationDelta = pBase - pOpt->ImageBase;
	if (LocationDelta) {
		if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size) {
			auto* pRelocData = (PIMAGE_BASE_RELOCATION)(pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress);
			const auto* pRelocEnd = (PIMAGE_BASE_RELOCATION)((uintptr_t)(pRelocData) + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size);
			while (pRelocData < pRelocEnd && pRelocData->SizeOfBlock) {
				UINT AmountOfEntries = (pRelocData->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
				PWORD pRelativeInfo = (PWORD)(pRelocData + 1);
				for (UINT i = 0; i != AmountOfEntries; ++i, ++pRelativeInfo) {
					if (RELOC_FLAG(*pRelativeInfo)) {
						uintptr_t* pPatch = (uintptr_t*)(pBase + pRelocData->VirtualAddress + ((*pRelativeInfo) & 0xFFF));
						*pPatch += (uintptr_t)(LocationDelta);
					}
				}
				pRelocData = (PIMAGE_BASE_RELOCATION)((PBYTE)(pRelocData) + pRelocData->SizeOfBlock);
			}
		}
	}
	if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size) {
		auto* pImportDescr = (IMAGE_IMPORT_DESCRIPTOR*)(pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
		while (pImportDescr->Name) {
			PCHAR szMod = (PCHAR)(pBase + pImportDescr->Name);
			HINSTANCE hDll = _LoadLibraryA(szMod);

			uintptr_t* pThunkRef = (uintptr_t*)(pBase + pImportDescr->OriginalFirstThunk);
			uintptr_t* pFuncRef = (uintptr_t*)(pBase + pImportDescr->FirstThunk);

			if (!pThunkRef)
				pThunkRef = pFuncRef;

			for (; *pThunkRef; ++pThunkRef, ++pFuncRef) {
				if (IMAGE_SNAP_BY_ORDINAL(*pThunkRef)) {
					*pFuncRef = (uintptr_t)_GetProcAddress(hDll, (PCHAR)(*pThunkRef & 0xFFFF));
				}
				else {
					auto* pImport = (IMAGE_IMPORT_BY_NAME*)(pBase + (*pThunkRef));
					*pFuncRef = (uintptr_t)_GetProcAddress(hDll, pImport->Name);
				}
			}
			++pImportDescr;
		}
	}
	if (pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].Size) {
		auto* pTLS = (PIMAGE_TLS_DIRECTORY)(pBase + pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress);
		auto* pCallback = (PIMAGE_TLS_CALLBACK*)(pTLS->AddressOfCallBacks);
		for (; pCallback && *pCallback; ++pCallback)
			(*pCallback)(pBase, DLL_PROCESS_ATTACH, NULL);
	}
	bool ExceptionSupportFailed = false;

#ifdef _WIN64

	if (pData->SEHSupport) {
		auto excep = pOpt->DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
		if (excep.Size) {
			if (!_RtlAddFunctionTable(
				(PIMAGE_RUNTIME_FUNCTION_ENTRY)(pBase + excep.VirtualAddress),
				excep.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY), (DWORD64)pBase)) {
				ExceptionSupportFailed = true;
			}
		}
	}
#endif
	_DllMain(pBase, pData->fdwReasonParam, pData->reservedParam);

	if (ExceptionSupportFailed)
		pData->hMod = (HINSTANCE)(0x505050);
	else
		pData->hMod = (HINSTANCE)(pBase);

}