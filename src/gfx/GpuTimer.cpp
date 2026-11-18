#include "GpuTimer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>

namespace skydiag::gpu
{
	namespace
	{
		constexpr int kFrames = 4;       // frames in flight: results are read kFrames - 1 frames later
		constexpr int kMaxMarks = 192;   // pass boundaries in one frame

		struct Mark
		{
			ID3D11Query* query;
			int          name;  // index in g_names, -1 = the end of the frame
		};

		struct Frame
		{
			ID3D11Query* disjoint;
			Mark         marks[kMaxMarks];
			int          markCount;
			bool         open;
		};

		ID3D11Device*        g_device = nullptr;
		ID3D11DeviceContext* g_context = nullptr;
		Frame                g_frames[kFrames];
		int                  g_frame = 0;
		bool                 g_started = false;
		bool                 g_failed = false;

		// Pass names.
		constexpr int kMaxNames = 128;
		char          g_names[kMaxNames][64];
		int           g_nameCount = 0;

		// The name of a render target view, cached by the view pointer.
		struct ViewName
		{
			const void* view;
			int         name;
		};
		ViewName g_viewNames[512];

		// Sums of the last second, render thread only.
		double g_sumMs[kMaxNames];
		int    g_sumCount[kMaxNames];  // passes with this name, all frames
		double g_frameSumMs = 0;
		int    g_frameCount = 0;
		LONGLONG g_windowStart = 0;
		LARGE_INTEGER g_frequency{};

		// Session values (under g_lock).
		std::mutex  g_lock;
		Snapshot    g_snapshot;
		double      g_sessionSum[kMaxNames];
		float       g_sessionMax[kMaxNames];
		int         g_sessionSeconds = 0;
		double      g_sessionFrameSum = 0;
		float       g_sessionFrameMax = 0;

		const char* FormatName(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
				return "R8G8B8A8_UNORM";
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				return "R8G8B8A8_SRGB";
			case DXGI_FORMAT_B8G8R8A8_UNORM:
				return "B8G8R8A8_UNORM";
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				return "R16G16B16A16_FLOAT";
			case DXGI_FORMAT_R11G11B10_FLOAT:
				return "R11G11B10_FLOAT";
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				return "R10G10B10A2_UNORM";
			case DXGI_FORMAT_R16G16_FLOAT:
				return "R16G16_FLOAT";
			case DXGI_FORMAT_R32_FLOAT:
				return "R32_FLOAT";
			case DXGI_FORMAT_R16_FLOAT:
				return "R16_FLOAT";
			case DXGI_FORMAT_R8_UNORM:
				return "R8_UNORM";
			case DXGI_FORMAT_R8G8_UNORM:
				return "R8G8_UNORM";
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
				return "R32G32B32A32_FLOAT";
			default:
				return nullptr;
			}
		}

		int NameIndex(const char* a_name)
		{
			for (int i = 0; i < g_nameCount; ++i) {
				if (std::strcmp(g_names[i], a_name) == 0) {
					return i;
				}
			}
			if (g_nameCount >= kMaxNames) {
				return kMaxNames - 1;
			}
			strncpy_s(g_names[g_nameCount], a_name, _TRUNCATE);
			return g_nameCount++;
		}

		// "1920x1080 R11G11B10_FLOAT", "+2" for more render targets, or "depth only 2048x2048".
		int PassName(UINT a_count, ID3D11RenderTargetView* const* a_views, ID3D11DepthStencilView* a_depth)
		{
			ID3D11View* first = nullptr;
			UINT        targets = 0;
			for (UINT i = 0; i < a_count; ++i) {
				if (a_views && a_views[i]) {
					first = first ? first : a_views[i];
					++targets;
				}
			}
			const bool depthOnly = first == nullptr;
			if (depthOnly) {
				first = a_depth;
			}
			if (!first) {
				return NameIndex("no render target");
			}
			auto& cached = g_viewNames[(reinterpret_cast<std::uintptr_t>(first) >> 4) % std::size(g_viewNames)];
			if (cached.view == first && targets <= 1) {
				return cached.name;
			}
			char           name[64] = "other render target";
			ID3D11Resource* resource = nullptr;
			first->GetResource(&resource);
			if (resource) {
				ID3D11Texture2D* texture = nullptr;
				if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) && texture) {
					D3D11_TEXTURE2D_DESC desc{};
					texture->GetDesc(&desc);
					if (depthOnly) {
						_snprintf_s(name, _TRUNCATE, "depth only %ux%u", desc.Width, desc.Height);
					} else {
						const char* format = FormatName(desc.Format);
						if (format) {
							_snprintf_s(name, _TRUNCATE, "%ux%u %s%s", desc.Width, desc.Height, format, targets > 1 ? " and more" : "");
						} else {
							_snprintf_s(name, _TRUNCATE, "%ux%u format %u%s", desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
								targets > 1 ? " and more" : "");
						}
					}
					texture->Release();
				}
				resource->Release();
			}
			const int index = NameIndex(name);
			if (targets <= 1) {
				cached.view = first;
				cached.name = index;
			}
			return index;
		}

		void AddMark(int a_name)
		{
			Frame& f = g_frames[g_frame];
			if (!f.open || f.markCount >= kMaxMarks) {
				return;
			}
			Mark& m = f.marks[f.markCount];
			if (!m.query) {
				D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP, 0 };
				if (FAILED(g_device->CreateQuery(&desc, &m.query))) {
					return;
				}
			}
			g_context->End(m.query);
			m.name = a_name;
			++f.markCount;
		}

		// ------------------------------------------------------------ hooks

		using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
		using OMSetRenderTargetsAndUAVsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT,
			UINT, ID3D11UnorderedAccessView* const*, const UINT*);
		OMSetRenderTargetsFn        g_originalSet = nullptr;
		OMSetRenderTargetsAndUAVsFn g_originalSetUav = nullptr;

		void STDMETHODCALLTYPE HookedSet(ID3D11DeviceContext* a_context, UINT a_count, ID3D11RenderTargetView* const* a_views, ID3D11DepthStencilView* a_depth)
		{
			if (a_context == g_context && g_started) {
				AddMark(PassName(a_count, a_views, a_depth));
			}
			g_originalSet(a_context, a_count, a_views, a_depth);
		}

		void STDMETHODCALLTYPE HookedSetUav(ID3D11DeviceContext* a_context, UINT a_count, ID3D11RenderTargetView* const* a_views, ID3D11DepthStencilView* a_depth,
			UINT a_uavStart, UINT a_uavCount, ID3D11UnorderedAccessView* const* a_uavs, const UINT* a_counts)
		{
			// D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL: only the UAVs change, not a new pass.
			if (a_context == g_context && g_started && a_count != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
				AddMark(PassName(a_count, a_views, a_depth));
			}
			g_originalSetUav(a_context, a_count, a_views, a_depth, a_uavStart, a_uavCount, a_uavs, a_counts);
		}

		template <class F>
		bool PatchSlot(void** a_table, int a_slot, F a_hook, F& a_original)
		{
			void** slot = &a_table[a_slot];
			DWORD  oldProtect = 0;
			if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
				return false;
			}
			a_original = reinterpret_cast<F>(*slot);
			InterlockedExchangePointer(slot, reinterpret_cast<void*>(a_hook));
			VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
			return true;
		}

		// Reads the results of a frame that the GPU finished. Returns false if
		// they are not ready.
		bool Collect(Frame& a_frame)
		{
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			if (g_context->GetData(a_frame.disjoint, &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
				return false;
			}
			if (disjoint.Disjoint || disjoint.Frequency == 0 || a_frame.markCount < 2) {
				return true;  // not usable, but done
			}
			UINT64 previous = 0;
			UINT64 first = 0;
			UINT64 last = 0;
			for (int i = 0; i < a_frame.markCount; ++i) {
				UINT64 time = 0;
				if (g_context->GetData(a_frame.marks[i].query, &time, sizeof(time), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
					return false;
				}
				if (i == 0) {
					first = time;
				} else {
					// The pass that started at mark i-1 ends at mark i.
					const int name = a_frame.marks[i - 1].name;
					if (name >= 0 && time >= previous) {
						g_sumMs[name] += static_cast<double>(time - previous) * 1000.0 / static_cast<double>(disjoint.Frequency);
						++g_sumCount[name];
					}
				}
				previous = time;
				last = time;
			}
			g_frameSumMs += static_cast<double>(last - first) * 1000.0 / static_cast<double>(disjoint.Frequency);
			++g_frameCount;
			return true;
		}

		void PublishSecond()
		{
			if (g_frameCount == 0) {
				return;
			}
			Snapshot result;
			result.valid = true;
			result.frameMs = static_cast<float>(g_frameSumMs / g_frameCount);
			for (int i = 0; i < g_nameCount; ++i) {
				if (g_sumCount[i] == 0) {
					continue;
				}
				const float ms = static_cast<float>(g_sumMs[i] / g_frameCount);
				const int   max = static_cast<int>(std::size(result.passes));
				int         position = result.passCount;
				while (position > 0 && result.passes[position - 1].ms < ms) {
					--position;
				}
				if (position < max) {
					const int lastIndex = result.passCount < max ? result.passCount : max - 1;
					for (int j = lastIndex; j > position; --j) {
						result.passes[j] = result.passes[j - 1];
					}
					strncpy_s(result.passes[position].name, g_names[i], _TRUNCATE);
					result.passes[position].ms = ms;
					result.passes[position].count = (g_sumCount[i] + g_frameCount / 2) / g_frameCount;
					if (result.passCount < max) {
						++result.passCount;
					}
				}
			}
			{
				const std::scoped_lock lock{ g_lock };
				g_snapshot = result;
				for (int i = 0; i < g_nameCount; ++i) {
					const float ms = g_sumCount[i] ? static_cast<float>(g_sumMs[i] / g_frameCount) : 0.0f;
					g_sessionSum[i] += ms;
					g_sessionMax[i] = (std::max)(g_sessionMax[i], ms);
				}
				g_sessionFrameSum += result.frameMs;
				g_sessionFrameMax = (std::max)(g_sessionFrameMax, result.frameMs);
				++g_sessionSeconds;
			}
			std::fill(std::begin(g_sumMs), std::end(g_sumMs), 0.0);
			std::fill(std::begin(g_sumCount), std::end(g_sumCount), 0);
			g_frameSumMs = 0;
			g_frameCount = 0;
		}
	}

	bool Start(ID3D11Device* a_device, ID3D11DeviceContext* a_context)
	{
		if (g_started || g_failed || !a_device || !a_context) {
			return g_started;
		}
		g_device = a_device;
		g_context = a_context;
		for (auto& f : g_frames) {
			D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			if (FAILED(g_device->CreateQuery(&desc, &f.disjoint))) {
				g_failed = true;
				return false;
			}
		}
		// Positions in the ID3D11DeviceContext function table: OMSetRenderTargets
		// is 33, OMSetRenderTargetsAndUnorderedAccessViews is 34.
		void** table = *reinterpret_cast<void***>(g_context);
		if (!PatchSlot(table, 33, &HookedSet, g_originalSet) || !PatchSlot(table, 34, &HookedSetUav, g_originalSetUav)) {
			g_failed = true;
			return false;
		}
		QueryPerformanceFrequency(&g_frequency);
		g_started = true;
		return true;
	}

	void OnPresent()
	{
		if (!g_started) {
			return;
		}
		// End the frame that the game submitted.
		Frame& current = g_frames[g_frame];
		if (current.open) {
			AddMark(-1);
			g_context->End(current.disjoint);
			current.open = false;
		}
		// The oldest frame: its results should be ready now.
		g_frame = (g_frame + 1) % kFrames;
		Frame& next = g_frames[g_frame];
		if (next.markCount > 0) {
			Collect(next);  // if not ready, the results of that frame are lost
		}
		next.markCount = 0;
		g_context->Begin(next.disjoint);
		next.open = true;
		AddMark(NameIndex("start of the frame"));

		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		if (g_windowStart == 0) {
			g_windowStart = now.QuadPart;
		} else if (now.QuadPart - g_windowStart >= g_frequency.QuadPart) {
			g_windowStart = now.QuadPart;
			PublishSecond();
		}
	}

	void GetSnapshot(Snapshot& a_out)
	{
		const std::scoped_lock lock{ g_lock };
		a_out = g_snapshot;
	}

	int GetSession(SessionPass* a_out, int a_max, float& a_frameAverageMs, float& a_frameMaxMs)
	{
		const std::scoped_lock lock{ g_lock };
		a_frameAverageMs = g_sessionSeconds ? static_cast<float>(g_sessionFrameSum / g_sessionSeconds) : 0.0f;
		a_frameMaxMs = g_sessionFrameMax;
		int count = 0;
		for (int i = 0; i < g_nameCount; ++i) {
			const float average = g_sessionSeconds ? static_cast<float>(g_sessionSum[i] / g_sessionSeconds) : 0.0f;
			int         position = count;
			while (position > 0 && a_out[position - 1].averageMs < average) {
				--position;
			}
			if (position >= a_max) {
				continue;
			}
			const int lastIndex = count < a_max ? count : a_max - 1;
			for (int j = lastIndex; j > position; --j) {
				a_out[j] = a_out[j - 1];
			}
			strncpy_s(a_out[position].name, g_names[i], _TRUNCATE);
			a_out[position].averageMs = average;
			a_out[position].maxMs = g_sessionMax[i];
			if (count < a_max) {
				++count;
			}
		}
		return count;
	}
}
