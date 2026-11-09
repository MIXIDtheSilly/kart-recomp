// HOST_NVN_DUMP=<frame>[,<dir>]: logs every draw of one frame and saves its colour targets (dump.cpp).
#pragma once
#include "vkctx.h"

namespace nvk {

bool dump_active();
bool dump_brief();                         // HOST_NVN_DUMP_NOTARGETS: one line a draw (no per-binding detail)
bool dump_buffers();                       // a dump notes hashes of the uniform and storage buffers (HOST_NVN_DUMP_RING_DETAIL: those without the rest)
void dump_arm(u64 from, u64 to, const char* dir);   // dumps the frames whose present is capture from..to (g_presents)
// HOST_NVN_DUMP_RING=<dir> (with HOST_NVN_CAPTURE_EVERY=1): the brief notes of every frame stay in memory, and a flash in present
// number `num` writes those of the frames around it to <dir>/ring_<num>.txt.
void dump_ring_flash(s64 num);
void dump_sub_begin();                       // decides whether this submission is in the dumped frame
void dump_note(const char* fmt, ...);        // a log line while active
void dump_trace(bool on);                    // dump_note records this draw too (HOST_NVN_TRACE_PS), kept for dump_trace_print
bool dump_tracing();
void dump_trace_print();                     // the traced draws' recent notes (at device loss)
void dump_targets(VkCommandBuffer cb);       // the colour targets as they are now (a rendering scope just ended)
void dump_flush(u64 value);
// Memory noted at record time, read again once the submission has completed: rows that changed in between are noted.
void dump_recheck(u64 addr, const u8* now, size_t n);
// While a dump with targets is active: an occlusion query around each draw (the samples it wrote are noted as
// "samples q<n> <count>" once the submission completes). dump_query_begin returns the query number, or ~0u.
u32 dump_query_begin(VkCommandBuffer cb);
void dump_query_end(VkCommandBuffer cb, u32 q);                  // after the submission: wait for it and write the pictures

}  // namespace nvk
