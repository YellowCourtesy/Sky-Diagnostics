#include "PCH.h"

#include "Papyrus.h"
#include "crash/CrashHandler.h"
#include "crash/Session.h"

namespace skydiag::papyrus
{
	namespace
	{
		using VM = RE::BSScript::Internal::VirtualMachine;

		constexpr int kMaxScripts = 96;
		constexpr int kPeriodSamples = 60;  // 3 s at 20 samples each second

		struct ScriptCount
		{
			char          name[96];
			std::uint32_t running;
			std::uint32_t waiting;
		};

		// Plain data only: the sample function can stop at any point if it
		// reads memory that is no longer valid.
		struct Accumulator
		{
			ScriptCount   scripts[kMaxScripts];
			int           scriptCount;
			ScriptCount   functions[kMaxScripts];  // "Script.Function" of stacks that run
			int           functionCount;
			std::uint64_t running;
			std::uint64_t waiting;
			std::uint32_t maxStacks;
			std::uint32_t samples;
		};

		const VM*         g_vm = nullptr;
		const char*       g_status = "not started";
		std::atomic<long> g_faults{ 0 };

		std::mutex g_snapshotLock;
		Snapshot   g_snapshot;

		// Adds one sample to a list. The name is a_name, or a_name + "." +
		// a_second. Returns false if a text is not a script name.
		bool Count(ScriptCount* a_list, int& a_count, const char* a_name, const char* a_second, bool a_running)
		{
			char name[96];
			int  n = 0;
			for (int part = 0; part < 2; ++part) {
				const char* text = part == 0 ? a_name : a_second;
				if (!text) {
					break;
				}
				if (part == 1) {
					name[n++] = '.';
				}
				const int start = n;
				for (int i = 0; n < 94 && text[i] != '\0'; ++i, ++n) {
					const char c = text[i];
					if (c < 0x20 || c >= 0x7F) {
						return false;  // not a name: the memory is not what it was
					}
					name[n] = c;
				}
				if (n == start) {
					return false;
				}
			}
			name[n] = '\0';

			int found = -1;
			for (int i = 0; i < a_count; ++i) {
				if (std::strcmp(a_list[i].name, name) == 0) {
					found = i;
					break;
				}
			}
			if (found < 0) {
				if (a_count >= kMaxScripts) {
					return false;
				}
				found = a_count++;
				std::memcpy(a_list[found].name, name, sizeof(name));
				a_list[found].running = 0;
				a_list[found].waiting = 0;
			}
			if (a_running) {
				++a_list[found].running;
			} else {
				++a_list[found].waiting;
			}
			return true;
		}

		// ------------------------------------------------ time measurement
		//
		// The virtual machine runs the code of a script function through
		// ScriptFunction::Call. One call runs the code of that function
		// until the function ends, waits, or calls a different function.
		// The time of each call is added to the entry of the function.

		using ScriptFunction = RE::BSScript::Internal::ScriptFunction;
		using CallResult = RE::BSScript::IFunction::CallResult;
		using CallFn = CallResult (*)(ScriptFunction*, const RE::BSTSmartPointer<RE::BSScript::Stack>&, RE::BSScript::ErrorLogger*, VM*, bool);

		constexpr int kTimedSlots = 8192;  // must be a power of 2

		struct TimedSlot
		{
			std::atomic<const void*>   key;    // the function object
			std::atomic<bool>          ready;  // the name is complete
			char                       name[96];
			std::atomic<std::uint64_t> ticks;
			std::atomic<std::uint32_t> calls;
			std::uint64_t              lastTicks;  // sampler thread only
			std::uint32_t              lastCalls;  // sampler thread only
		};

		TimedSlot    g_timed[kTimedSlots];
		CallFn       g_originalCall = nullptr;
		std::int64_t g_ticksPerSecond = 0;

		void Record(const ScriptFunction* a_function, std::uint64_t a_ticks)
		{
			std::size_t index = (reinterpret_cast<std::uintptr_t>(a_function) >> 4) & (kTimedSlots - 1);
			for (int probe = 0; probe < 32; ++probe, index = (index + 1) & (kTimedSlots - 1)) {
				TimedSlot&  slot = g_timed[index];
				const void* key = slot.key.load(std::memory_order_relaxed);
				if (key == nullptr) {
					const void* expected = nullptr;
					if (slot.key.compare_exchange_strong(expected, a_function)) {
						// This thread owns the new entry: write the name.
						const char* script = a_function->objName.c_str();
						const char* function = a_function->name.c_str();
						_snprintf_s(slot.name, sizeof(slot.name), _TRUNCATE, "%s.%s", script ? script : "?", function ? function : "?");
						slot.ready.store(true, std::memory_order_release);
						key = a_function;
					} else {
						key = expected;
					}
				}
				if (key == a_function) {
					slot.ticks.fetch_add(a_ticks, std::memory_order_relaxed);
					slot.calls.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			}
			// The table is full near this place: the call is not counted.
		}

		CallResult HookedCall(ScriptFunction* a_function, const RE::BSTSmartPointer<RE::BSScript::Stack>& a_stack, RE::BSScript::ErrorLogger* a_logger,
			VM* a_vm, bool a_arg4)
		{
			const auto       before = std::chrono::steady_clock::now();
			const CallResult result = g_originalCall(a_function, a_stack, a_logger, a_vm, a_arg4);
			const auto       elapsed = std::chrono::steady_clock::now() - before;
			Record(a_function, static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
			return result;
		}

		// The functions with the most time since the last call of this
		// function. a_seconds: the length of that period.
		void PublishTimed(Snapshot& a_result, float a_seconds)
		{
			if (!g_originalCall || g_ticksPerSecond <= 0 || a_seconds <= 0.0f) {
				return;
			}
			for (auto& slot : g_timed) {
				if (!slot.ready.load(std::memory_order_acquire)) {
					continue;
				}
				const std::uint64_t ticks = slot.ticks.load(std::memory_order_relaxed);
				const std::uint32_t calls = slot.calls.load(std::memory_order_relaxed);
				const std::uint64_t deltaTicks = ticks - slot.lastTicks;
				const std::uint32_t deltaCalls = calls - slot.lastCalls;
				slot.lastTicks = ticks;
				slot.lastCalls = calls;
				if (deltaCalls == 0) {
					continue;
				}
				const float ms = static_cast<float>(static_cast<double>(deltaTicks) * 1000.0 / static_cast<double>(g_ticksPerSecond)) / a_seconds;
				// Insert in the sorted result.
				const int max = static_cast<int>(std::size(a_result.timed));
				int       position = a_result.timedCount;
				while (position > 0 && a_result.timed[position - 1].msPerSecond < ms) {
					--position;
				}
				if (position >= max) {
					continue;
				}
				const int last = a_result.timedCount < max ? a_result.timedCount : max - 1;
				for (int j = last; j > position; --j) {
					a_result.timed[j] = a_result.timed[j - 1];
				}
				auto& out = a_result.timed[position];
				strncpy_s(out.name, slot.name, _TRUNCATE);
				out.msPerSecond = ms;
				out.callsPerSecond = static_cast<float>(deltaCalls) / a_seconds;
				if (a_result.timedCount < max) {
					++a_result.timedCount;
				}
			}
		}

		void InstallTimeMeasurement()
		{
			g_ticksPerSecond = 1'000'000'000;  // the unit of the time values is one nanosecond
			// Entry 0x0F of the function table is Call.
			REL::Relocation<std::uintptr_t> table{ RE::VTABLE_BSScript__Internal__ScriptFunction[0] };
			g_originalCall = reinterpret_cast<CallFn>(table.write_vfunc(0x0F, reinterpret_cast<std::uintptr_t>(&HookedCall)));
			SKSE::log::info("Papyrus function time measurement is on.");
		}

		// Reads the stack list with no lock. The virtual machine can change
		// the list at the same time, so a read can fail: the caller catches
		// that. Nothing is written to game memory.
		void SampleUnguarded(Accumulator& a_acc)
		{
			std::uint32_t total = 0;
			for (const auto& entry : g_vm->allRunningStacks) {
				const RE::BSScript::Stack* stack = entry.second.get();
				if (!stack) {
					continue;
				}
				if (++total > 50000) {
					break;
				}
				const bool running = stack->state.get() == RE::BSScript::Stack::State::kRunning;
				if (running) {
					++a_acc.running;
				} else {
					++a_acc.waiting;
				}
				const RE::BSScript::StackFrame* frame = stack->top;
				if (!frame) {
					continue;
				}
				const RE::BSScript::ObjectTypeInfo* type = frame->owningObjectType.get();
				if (!type) {
					continue;
				}
				const char* name = type->GetName();
				if (!name) {
					continue;
				}
				Count(a_acc.scripts, a_acc.scriptCount, name, nullptr, running);
				// The function at the top of a stack that runs. The name is
				// at the same place in script functions and native
				// functions (the two classes start with the same members).
				if (running) {
					if (const auto function = reinterpret_cast<const RE::BSScript::Internal::ScriptFunction*>(frame->owningFunction.get())) {
						if (const char* functionName = function->name.c_str()) {
							Count(a_acc.functions, a_acc.functionCount, name, functionName, true);
						}
					}
				}
			}
			if (total > a_acc.maxStacks) {
				a_acc.maxStacks = total;
			}
			++a_acc.samples;
		}

		bool SampleGuarded(Accumulator& a_acc)
		{
			__try {
				SampleUnguarded(a_acc);
				return true;
			} __except (1) {
			}
			return false;
		}

		void Publish(const Accumulator& a_acc)
		{
			Snapshot result;
			result.valid = a_acc.samples > 0;
			result.samples = a_acc.samples;
			if (result.valid) {
				const float samples = static_cast<float>(a_acc.samples);
				result.runningStacks = static_cast<float>(a_acc.running) / samples;
				result.waitingStacks = static_cast<float>(a_acc.waiting) / samples;
				result.maxStacks = a_acc.maxStacks;

				// Most running stacks first, then most waiting stacks.
				static ScriptCount sorted[kMaxScripts];
				std::copy_n(a_acc.scripts, a_acc.scriptCount, sorted);
				std::sort(sorted, sorted + a_acc.scriptCount, [](const ScriptCount& a_lhs, const ScriptCount& a_rhs) {
					return a_lhs.running != a_rhs.running ? a_lhs.running > a_rhs.running : a_lhs.waiting > a_rhs.waiting;
				});
				for (int i = 0; i < a_acc.scriptCount && result.count < static_cast<int>(std::size(result.entries)); ++i) {
					auto& out = result.entries[result.count++];
					strncpy_s(out.name, sorted[i].name, _TRUNCATE);
					out.running = static_cast<float>(sorted[i].running) / samples;
					out.waiting = static_cast<float>(sorted[i].waiting) / samples;
				}
				std::copy_n(a_acc.functions, a_acc.functionCount, sorted);
				std::sort(sorted, sorted + a_acc.functionCount, [](const ScriptCount& a_lhs, const ScriptCount& a_rhs) { return a_lhs.running > a_rhs.running; });
				for (int i = 0; i < a_acc.functionCount && result.functionCount < static_cast<int>(std::size(result.functions)); ++i) {
					auto& out = result.functions[result.functionCount++];
					strncpy_s(out.name, sorted[i].name, _TRUNCATE);
					out.running = static_cast<float>(sorted[i].running) / samples;
				}
			}
			PublishTimed(result, static_cast<float>(kPeriodSamples) * 0.05f);
			const std::scoped_lock lock{ g_snapshotLock };
			g_snapshot = result;
		}

		void SamplerLoop()
		{
			static Accumulator acc;
			std::uint32_t      ticks = 0;
			for (;;) {
				std::this_thread::sleep_for(50ms);
				if (!SampleGuarded(acc)) {
					// The list changed while it was read. That is normal
					// from time to time. Many failures mean that the memory
					// layout is not as expected: stop.
					if (++g_faults > 200) {
						g_status = "stopped: too many read failures (the memory layout of this game version is not as expected)";
						session::Event("Papyrus sampler stopped: too many read failures");
						Publish({});
						return;
					}
				}
				if (++ticks >= kPeriodSamples) {
					ticks = 0;
					Publish(acc);
					acc = {};
				}
			}
		}

		// Report section for crash and stall reports. No heap memory.
		void ReportSection(char* a_out, std::size_t a_cap)
		{
			static Snapshot snapshot;
			{
				const std::unique_lock lock{ g_snapshotLock, std::try_to_lock };
				if (!lock.owns_lock()) {
					std::snprintf(a_out, a_cap, "Not available.\n");
					return;
				}
				snapshot = g_snapshot;
			}
			if (!snapshot.valid) {
				std::snprintf(a_out, a_cap, "No data. Sampler state: %s\n", g_status);
				return;
			}
			int used = std::snprintf(a_out, a_cap, "Script stacks (average of the last 3 s): %.1f running, %.1f waiting, maximum %u\n",
				static_cast<double>(snapshot.runningStacks), static_cast<double>(snapshot.waitingStacks), snapshot.maxStacks);
			used += std::snprintf(a_out + used, a_cap - used, "Running  Waiting  Script\n");
			for (int i = 0; i < snapshot.count && used > 0 && static_cast<std::size_t>(used) + 100 < a_cap; ++i) {
				used += std::snprintf(a_out + used, a_cap - used, "%7.1f  %7.1f  %s\n", static_cast<double>(snapshot.entries[i].running),
					static_cast<double>(snapshot.entries[i].waiting), snapshot.entries[i].name);
			}
			if (snapshot.timedCount > 0 && used > 0 && static_cast<std::size_t>(used) + 200 < a_cap) {
				used += std::snprintf(a_out + used, a_cap - used, "Time in script functions (measured, last 3 s):\nms each second  Calls each second  Function\n");
				for (int i = 0; i < snapshot.timedCount && used > 0 && static_cast<std::size_t>(used) + 140 < a_cap; ++i) {
					used += std::snprintf(a_out + used, a_cap - used, "%14.2f  %16.0f  %s\n", static_cast<double>(snapshot.timed[i].msPerSecond),
						static_cast<double>(snapshot.timed[i].callsPerSecond), snapshot.timed[i].name);
				}
			}
			if (snapshot.functionCount > 0 && used > 0 && static_cast<std::size_t>(used) + 200 < a_cap) {
				used += std::snprintf(a_out + used, a_cap - used, "Functions at the top of the stacks that run:\nRunning  Function\n");
				for (int i = 0; i < snapshot.functionCount && used > 0 && static_cast<std::size_t>(used) + 130 < a_cap; ++i) {
					used += std::snprintf(a_out + used, a_cap - used, "%7.2f  %s\n", static_cast<double>(snapshot.functions[i].running),
						snapshot.functions[i].name);
				}
			}
		}
	}

	void Start(bool a_measureTime)
	{
		static std::once_flag once;
		std::call_once(once, [a_measureTime] {
			const auto vm = VM::GetSingleton();
			if (!vm) {
				g_status = "off: the virtual machine was not found";
				SKSE::log::error("Papyrus sampler is {}", g_status);
				return;
			}
			// Make sure that the pointer is the virtual machine object. If
			// the game version has a different memory layout, it is not.
			char className[128] = "";
			if (!crash::ClassName(vm, className, sizeof(className)) || std::strstr(className, "VirtualMachine") == nullptr) {
				g_status = "off: the virtual machine object was not recognized (game version not supported)";
				SKSE::log::error("Papyrus sampler is {} (found '{}')", g_status, className);
				return;
			}
			g_vm = vm;
			g_status = "running";
			crash::RegisterSection("PAPYRUS", &ReportSection);
			if (a_measureTime) {
				InstallTimeMeasurement();
			}
			std::thread(SamplerLoop).detach();
			SKSE::log::info("Papyrus sampler is on.");
		});
	}

	void GetSnapshot(Snapshot& a_out)
	{
		const std::scoped_lock lock{ g_snapshotLock };
		a_out = g_snapshot;
	}

	const char* Status()
	{
		return g_status;
	}
}
