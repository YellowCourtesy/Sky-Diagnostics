#include "SystemInfo.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <dxgi.h>
#include <intrin.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace skydiag::sysinfo
{
	namespace
	{
		std::string g_text;
		bool        g_collected = false;
		bool        g_dxvk = false;

		std::string Utf8(const wchar_t* a_text)
		{
			const int n = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
			if (n <= 1) {
				return {};
			}
			std::string out(static_cast<std::size_t>(n - 1), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), n, nullptr, nullptr);
			return out;
		}

		std::string Format(const char* a_fmt, ...)
		{
			char    buffer[1024];
			va_list args;
			va_start(args, a_fmt);
			_vsnprintf_s(buffer, _TRUNCATE, a_fmt, args);
			va_end(args);
			return buffer;
		}

		std::string CpuName()
		{
			int  info[4] = {};
			char brand[49] = {};
			__cpuid(info, 0x80000000);
			if (static_cast<unsigned>(info[0]) < 0x80000004) {
				return "not known";
			}
			for (int i = 0; i < 3; ++i) {
				__cpuid(info, 0x80000002 + i);
				std::memcpy(brand + i * 16, info, 16);
			}
			std::string name = brand;
			name.erase(0, name.find_first_not_of(' '));
			return name;
		}

		int PhysicalCores()
		{
			DWORD length = 0;
			GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
			if (length == 0) {
				return 0;
			}
			std::vector<unsigned char> buffer(length);
			if (!GetLogicalProcessorInformationEx(RelationProcessorCore, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &length)) {
				return 0;
			}
			int   cores = 0;
			DWORD offset = 0;
			while (offset < length) {
				const auto* entry = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data() + offset);
				if (entry->Relationship == RelationProcessorCore) {
					++cores;
				}
				offset += entry->Size;
			}
			return cores;
		}

		std::string OsText()
		{
			using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOW*);
			const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			std::string   text = "Windows";
			if (auto* get = reinterpret_cast<RtlGetVersion_t>(GetProcAddress(ntdll, "RtlGetVersion"))) {
				OSVERSIONINFOW info{};
				info.dwOSVersionInfoSize = sizeof(info);
				if (get(&info) == 0) {
					text = Format("Windows %lu.%lu build %lu (as Windows reports it)", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
				}
			}
			using WineVersion_t = const char*(__cdecl*)();
			using WineHost_t = void(__cdecl*)(const char**, const char**);
			if (auto* wine = reinterpret_cast<WineVersion_t>(GetProcAddress(ntdll, "wine_get_version"))) {
				text += Format("; Wine %s", wine());
				if (auto* host = reinterpret_cast<WineHost_t>(GetProcAddress(ntdll, "wine_get_host_version"))) {
					const char* system = nullptr;
					const char* release = nullptr;
					host(&system, &release);
					text += Format(" on %s %s", system ? system : "?", release ? release : "?");
				}
			}
			return text;
		}

		// DXVK: the d3d11.dll of the process has DXVK text in it.
		bool FileHasText(const wchar_t* a_path, const char* a_text)
		{
			std::ifstream file(a_path, std::ios::binary);
			if (!file) {
				return false;
			}
			std::string data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			return data.find(a_text) != std::string::npos;
		}

		std::string GpuText()
		{
			std::string text;
			IDXGIFactory1* factory = nullptr;
			if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) {
				return "GPU: not known\n";
			}
			IDXGIAdapter1* adapter = nullptr;
			for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
				DXGI_ADAPTER_DESC1 desc{};
				adapter->GetDesc1(&desc);
				if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
					LARGE_INTEGER driver{};
					const bool    hasDriver = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver));
					text += Format("GPU %u: %s, %llu MB video memory, vendor %04X device %04X", i, Utf8(desc.Description).c_str(),
						static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20), desc.VendorId, desc.DeviceId);
					if (hasDriver) {
						text += Format(", driver %u.%u.%u.%u", HIWORD(driver.HighPart), LOWORD(driver.HighPart), HIWORD(driver.LowPart), LOWORD(driver.LowPart));
					}
					text += "\n";
				}
				adapter->Release();
			}
			factory->Release();
			return text.empty() ? "GPU: not known\n" : text;
		}

		// Wine, Proton and DXVK settings: the environment variables with these
		// names. Variables with folder paths (they can have the user name) are left out.
		std::string WineSettings()
		{
			std::string  text;
			wchar_t*     block = GetEnvironmentStringsW();
			if (!block) {
				return text;
			}
			for (const wchar_t* entry = block; *entry; entry += wcslen(entry) + 1) {
				const std::string line = Utf8(entry);
				const auto        upper = [&](const char* a_prefix) { return _strnicmp(line.c_str(), a_prefix, strlen(a_prefix)) == 0; };
				const bool        wanted = upper("PROTON_") || upper("DXVK_") || upper("VKD3D_") || upper("WINEESYNC") || upper("WINEFSYNC") || upper("WINENTSYNC") ||
				                    upper("WINEDLLOVERRIDES") || upper("WINE_FULLSCREEN") || upper("WINE_LARGE_ADDRESS_AWARE");
				const bool path = line.find(":\\") != std::string::npos || line.find("=/") != std::string::npos;
				if (wanted && !path) {
					text += "  " + line + "\n";
				}
			}
			FreeEnvironmentStringsW(block);
			return text;
		}
	}

	const std::string& Collect()
	{
		if (g_collected) {
			return g_text;
		}
		g_collected = true;
		std::string text;
		text += Format("Processor: %s, %d cores, %lu logical processors\n", CpuName().c_str(), PhysicalCores(),
			static_cast<unsigned long>(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)));
		MEMORYSTATUSEX memory{};
		memory.dwLength = sizeof(memory);
		if (GlobalMemoryStatusEx(&memory)) {
			text += Format("Memory: %llu MB (%.1f GB)\n", static_cast<unsigned long long>(memory.ullTotalPhys >> 20),
				static_cast<double>(memory.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0));
		}
		text += GpuText();
		text += "Operating system: " + OsText() + "\n";

		wchar_t d3d11[MAX_PATH * 2] = L"";
		if (const HMODULE module = GetModuleHandleW(L"d3d11.dll"); module && GetModuleFileNameW(module, d3d11, static_cast<DWORD>(std::size(d3d11)))) {
			g_dxvk = FileHasText(d3d11, "DXVK_");
			text += Format("Direct3D 11: %s\n", g_dxvk ? "DXVK (Direct3D 11 on Vulkan)" : "d3d11.dll of the system or the driver");
		}
		const std::string wine = WineSettings();
		if (!wine.empty()) {
			text += "Wine, Proton and DXVK settings (environment):\n" + wine;
		}
		g_text = text;
		return g_text;
	}

	const char* Text()
	{
		return g_collected ? g_text.c_str() : "";
	}

	bool IsDxvk()
	{
		return g_dxvk;
	}

	std::vector<std::string> SksePluginProblems(const std::wstring& a_skseLogDir)
	{
		std::vector<std::string> result;
		for (const wchar_t* name : { L"skse64.log", L"sksevr.log" }) {
			std::ifstream file(a_skseLogDir + L"\\" + name);
			if (!file) {
				continue;
			}
			std::string line;
			while (std::getline(file, line)) {
				std::string lower = line;
				std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				// Lines about a DLL that did not load: the words differ between SKSE
				// versions, so any line with a DLL name and one of these words counts.
				if (lower.find(".dll") == std::string::npos) {
					continue;
				}
				static const char* const kWords[] = { "couldn't load", "could not load", "failed", "disabled", "incompatible", "not compatible", "unsupported",
					"error", "too old", "address library" };
				for (const char* word : kWords) {
					if (lower.find(word) != std::string::npos) {
						while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
							line.pop_back();
						}
						result.push_back(line);
						break;
					}
				}
			}
			break;
		}
		return result;
	}
}
