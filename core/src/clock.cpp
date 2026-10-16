#include "flash/clock.h"

#include <windows.h>
#include <timeapi.h>
#include <intrin.h>

namespace flash {

static uint64_t clock_freq()
{
	static const uint64_t freq = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return (uint64_t)f.QuadPart;
	}();
	return freq;
}

static uint64_t mul_div64(uint64_t num, uint64_t mul, uint64_t div)
{
	unsigned __int64 high;
	const unsigned __int64 low = _umul128(num, mul, &high);
	unsigned __int64 rem;
	return _udiv128(high, low, div, &rem);
}

uint64_t qpc_to_ns(int64_t qpc)
{
	return mul_div64((uint64_t)qpc, 1000000000ULL, clock_freq());
}

uint64_t now_ns()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return qpc_to_ns(t.QuadPart);
}

void sleep_until_ns(uint64_t t)
{
	thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
							   TIMER_ALL_ACCESS);
	const uint64_t now = now_ns();
	if (t <= now)
		return;

	// En unités de 100 ns, valeur négative = délai relatif
	LARGE_INTEGER due;
	due.QuadPart = -(LONGLONG)((t - now) / 100);
	if (timer && due.QuadPart < 0 && SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
		WaitForSingleObject(timer, INFINITE);
	} else {
		Sleep((DWORD)((t - now) / 1000000));
	}
}

void enable_precise_timers()
{
	timeBeginPeriod(1);
}

} // namespace flash
