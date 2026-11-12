// The graphics syncpoints the game sees (nx2:syncpt, the syncpoint memory of nx2:gsv, the video decoder's frame
// counter): a value per syncpoint that only goes up, and actions run once a value is reached.
#pragma once
#include <functional>

#include "common.h"

namespace sync {

constexpr u32 Count = 192;

using Handle = u64;   // 0: none (the action ran at once)

u32 value(u32 id);
bool ready(u32 id, u32 value);
void increment(u32 id);
// Runs `f` once syncpoint `id` reaches `value`: at once (on this thread) when it has, else on the thread that
// increments it there.
Handle on_reach(u32 id, u32 value, std::function<void()> f);
// Drops an action that has not run (a handle of one that ran, or 0, is ignored).
void cancel(u32 id, Handle h);
// A syncpoint of the host's own (the video decoder's frame counter), from the top half of the range; released for
// reuse.
u32 allocate();
void release(u32 id);

}  // namespace sync
