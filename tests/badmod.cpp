// Test DLL that plays the role of a defective mod.

// This file makes faults on purpose. The compiler warnings about them are
// correct and not useful here.
#pragma warning(disable : 4717)  // recursive on all control paths
#pragma warning(disable : 4297)  // function assumed not to throw an exception but does

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>

struct Widget
{
	virtual ~Widget() = default;
	virtual void Poke(volatile int* a_target) { *a_target = 42; }
	int value{ 7 };
};

// Classes that play the role of game objects. FakeRef has the "form" as
// its second base class, so the form data is not at the start of the object.
struct FakeForm
{
	virtual ~FakeForm() = default;
	unsigned int id{ 0 };
};

struct FakeNode
{
	virtual ~FakeNode() = default;
	int pad{ 0 };
};

struct FakeRef : FakeNode, FakeForm
{
	virtual void Touch(volatile int* a_target) { *a_target = 1; }
};

// A template class, to test readable template names.
template <class T>
struct Holder
{
	virtual ~Holder() = default;
	T* item{ nullptr };
};

namespace
{
	__declspec(noinline) void CrashWithFakeRef(FakeRef* a_ref)
	{
		FakeRef* volatile         keep = a_ref;  // keeps the object pointers on the stack
		Holder<Widget>* volatile  holder = new Holder<Widget>;
		Holder<int>* volatile     plain = new Holder<int>;
		keep->Touch(nullptr);
		holder->item = nullptr;
		plain->item = nullptr;
	}

	__declspec(noinline) void CrashWithObject(Widget* a_widget)
	{
		Widget* volatile keep = a_widget;  // keeps the object pointer on the stack
		keep->Poke(nullptr);
	}

	__declspec(noinline) int Recurse(int a_depth)
	{
		volatile char pad[2048];
		pad[0] = static_cast<char>(a_depth);
		return Recurse(a_depth + 1) + pad[0];
	}
}

extern "C" __declspec(dllexport) void BadModNullWrite()
{
	CrashWithObject(new Widget);
}

extern "C" __declspec(dllexport) void BadModThrow()
{
	throw std::runtime_error("test message 123");
}

extern "C" __declspec(dllexport) int BadModOverflow()
{
	return Recurse(0);
}

extern "C" __declspec(dllexport) void BadModSpin()
{
	volatile unsigned long long counter = 0;
	for (;;) {
		counter = counter + 1;
	}
}

// Uses the processor for the given time, then returns.
extern "C" __declspec(dllexport) unsigned long long BadModSpinFor(unsigned long a_milliseconds)
{
	volatile unsigned long long counter = 0;
	const ULONGLONG             end = GetTickCount64() + a_milliseconds;
	while (GetTickCount64() < end) {
		for (int i = 0; i < 100000; ++i) {
			counter = counter + 1;
		}
	}
	return counter;
}

// Sleeps for the given time, then returns. Plays the role of a mod that
// wraps a wait (for example a frame limiter).
extern "C" __declspec(dllexport) unsigned long BadModSleepFor(unsigned long a_milliseconds)
{
	Sleep(a_milliseconds);
	volatile unsigned long keep = a_milliseconds;  // no tail call: this function must stay on the call stack
	return keep;
}

extern "C" __declspec(dllexport) unsigned long BadModBlock()
{
	const HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	const DWORD  result = WaitForSingleObject(never, INFINITE);
	volatile DWORD keep = result;  // no tail call: this function must stay on the call stack
	return keep;
}

extern "C" __declspec(dllexport) void BadModGameObject()
{
	auto ref = new FakeRef;
	ref->id = 0xBEEF;
	CrashWithFakeRef(ref);
}

// Allocates memory that this DLL keeps, for the test of the memory count by
// DLL. a_useHeap: 1 = HeapAlloc, 0 = malloc.
extern "C" __declspec(dllexport) void* BadModAllocate(unsigned long long a_bytes, int a_useHeap)
{
	void* block = a_useHeap ? HeapAlloc(GetProcessHeap(), 0, static_cast<SIZE_T>(a_bytes)) : std::malloc(static_cast<std::size_t>(a_bytes));
	if (block) {
		std::memset(block, 1, static_cast<std::size_t>(a_bytes));
	}
	return block;
}

extern "C" __declspec(dllexport) void BadModRelease(void* a_block, int a_useHeap)
{
	if (a_useHeap) {
		HeapFree(GetProcessHeap(), 0, a_block);
	} else {
		std::free(a_block);
	}
}
