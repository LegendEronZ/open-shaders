// Minimal NVAPI DRS tool for the DLSS-G driver-profile investigation.
// Usage:
//   drs-tool dump                     - print the DLSSG keys on the Skyrim profile
//   drs-tool set <hexId> <hexValue>   - set a DWORD key on the Skyrim profile
//   drs-tool del <hexId>              - remove a key from the Skyrim profile
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using NvU32 = uint32_t;
using NvU16 = uint16_t;
using NvU8 = uint8_t;
using NvAPI_Status = int;
using NvDRSSessionHandle = void*;
using NvDRSProfileHandle = void*;

#pragma pack(push, 8)
struct NVDRS_BINARY_SETTING
{
	NvU32 valueLength;
	NvU8 valueData[4096];
};
struct NVDRS_SETTING
{
	NvU32 version;
	NvU16 settingName[2048];
	NvU32 settingId;
	NvU32 settingType;
	NvU32 settingLocation;
	NvU32 isCurrentPredefined;
	NvU32 isPredefinedValid;
	union
	{
		NvU32 u32PredefinedValue;
		NVDRS_BINARY_SETTING binaryPredefinedValue;
		NvU16 wszPredefinedValue[2048];
	};
	union
	{
		NvU32 u32CurrentValue;
		NVDRS_BINARY_SETTING binaryCurrentValue;
		NvU16 wszCurrentValue[2048];
	};
};
#pragma pack(pop)

namespace
{
	using PQueryInterface = void*(__cdecl*)(NvU32);
	NvAPI_Status(__cdecl* pInitialize)(){};
	NvAPI_Status(__cdecl* pCreateSession)(NvDRSSessionHandle*){};
	NvAPI_Status(__cdecl* pDestroySession)(NvDRSSessionHandle){};
	NvAPI_Status(__cdecl* pLoadSettings)(NvDRSSessionHandle){};
	NvAPI_Status(__cdecl* pSaveSettings)(NvDRSSessionHandle){};
	NvAPI_Status(__cdecl* pFindProfileByName)(NvDRSSessionHandle, NvU16*, NvDRSProfileHandle*){};
	NvAPI_Status(__cdecl* pGetSetting)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32, NVDRS_SETTING*){};
	NvAPI_Status(__cdecl* pSetSetting)(NvDRSSessionHandle, NvDRSProfileHandle, NVDRS_SETTING*){};
	NvAPI_Status(__cdecl* pDeleteProfileSetting)(NvDRSSessionHandle, NvDRSProfileHandle, NvU32){};

	const wchar_t kProfileName[] = L"The Elder Scrolls V: Skyrim Special Edition";
	const NvU32 kKeys[] = { 0x104D6667, 0x10E41DF1, 0x10308298 };
}

int main(int argc, char** argv)
{
	HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
	if (!nvapi) {
		printf("FATAL: nvapi64.dll not found\n");
		return 1;
	}
	auto qi = (PQueryInterface)GetProcAddress(nvapi, "nvapi_QueryInterface");
	if (!qi) {
		printf("FATAL: nvapi_QueryInterface missing\n");
		return 1;
	}
	*(void**)&pInitialize = qi(0x0150E828);
	*(void**)&pCreateSession = qi(0x0694D52E);
	*(void**)&pDestroySession = qi(0xDAD9CFF8);
	*(void**)&pLoadSettings = qi(0x375DBD6B);
	*(void**)&pSaveSettings = qi(0xFCBC7E14);
	*(void**)&pFindProfileByName = qi(0x7E4A9A0B);
	*(void**)&pGetSetting = qi(0x73BF8338);
	*(void**)&pSetSetting = qi(0x577DD202);
	*(void**)&pDeleteProfileSetting = qi(0xE4A26362);
	if (!pInitialize || !pCreateSession || !pLoadSettings || !pFindProfileByName || !pGetSetting || !pSetSetting ||
		!pSaveSettings || !pDeleteProfileSetting) {
		printf("FATAL: missing NVAPI entry points\n");
		return 1;
	}
	if (pInitialize() != 0) {
		printf("FATAL: NvAPI_Initialize failed\n");
		return 1;
	}

	NvDRSSessionHandle session{};
	if (pCreateSession(&session) != 0 || pLoadSettings(session) != 0) {
		printf("FATAL: DRS session/load failed\n");
		return 1;
	}

	NvU16 profileName[2048]{};
	for (size_t i = 0; kProfileName[i]; i++)
		profileName[i] = (NvU16)kProfileName[i];
	NvDRSProfileHandle profile{};
	NvAPI_Status st = pFindProfileByName(session, profileName, &profile);
	if (st != 0) {
		printf("FATAL: FindProfileByName failed (%d)\n", st);
		return 1;
	}
	printf("Profile found: %ls\n", kProfileName);

	const NvU32 settingVer = (NvU32)sizeof(NVDRS_SETTING) | (1u << 16);
	std::string cmd = argc > 1 ? argv[1] : "dump";

	if (cmd == "dump") {
		for (NvU32 id : kKeys) {
			NVDRS_SETTING s{};
			s.version = settingVer;
			NvAPI_Status r = pGetSetting(session, profile, id, &s);
			if (r == 0)
				printf("key %08x: current=%08x predefined=%08x type=%u isCurrentPredefined=%u\n",
					id, s.u32CurrentValue, s.u32PredefinedValue, s.settingType, s.isCurrentPredefined);
			else
				printf("key %08x: GetSetting failed (%d)\n", id, r);
		}
	} else if (cmd == "set" && argc == 4) {
		NvU32 id = (NvU32)strtoul(argv[2], nullptr, 16);
		NvU32 value = (NvU32)strtoul(argv[3], nullptr, 16);
		NVDRS_SETTING s{};
		s.version = settingVer;
		s.settingId = id;
		s.settingType = 0;  // DWORD
		s.u32CurrentValue = value;
		NvAPI_Status r = pSetSetting(session, profile, &s);
		if (r != 0) {
			printf("SetSetting %08x=%08x failed (%d)\n", id, value, r);
			return 1;
		}
		if (pSaveSettings(session) != 0) {
			printf("SaveSettings failed\n");
			return 1;
		}
		printf("OK: key %08x set to %08x and saved\n", id, value);
	} else if (cmd == "del" && argc == 3) {
		NvU32 id = (NvU32)strtoul(argv[2], nullptr, 16);
		NvAPI_Status r = pDeleteProfileSetting(session, profile, id);
		if (r != 0) {
			printf("DeleteProfileSetting %08x failed (%d)\n", id, r);
			return 1;
		}
		if (pSaveSettings(session) != 0) {
			printf("SaveSettings failed\n");
			return 1;
		}
		printf("OK: key %08x deleted (reverts to driver predefined) and saved\n", id);
	} else {
		printf("usage: drs-tool dump | set <hexId> <hexValue> | del <hexId>\n");
	}

	pDestroySession(session);
	return 0;
}
