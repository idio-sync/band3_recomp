#pragma once

// Experimental: logs the game's long frames (game_stall_log_ms, 100 ms by
// default; 0 off), for hitches the log otherwise says nothing about. The game
// thread marks where it is in each frame from DxRnd::Present's hook
// (scene_capture.cpp): a few clock reads and stores a frame. A watcher thread,
// started with the first frame, looks every 10 ms; once a frame has run past
// half the threshold it notes what's cheap to know (the game thread's and the
// emulated GPU's command processor's CPU time, the process's file I/O and page
// faults, what the native renderer's worker is doing) and samples the game
// thread's, the command processor's and the worker's stacks every 50 ms
// until the frame ends, then, if it ran past the threshold, logs one
// warning: the frame's length split at the hook's marks, those numbers over
// the part of it watched, and each thread's distinct stacks with how often
// each was seen. Frames are module+RVA (Windows only; elsewhere just the
// times), resolved against out/build/<preset>/band3.map as crash_trace.cpp's
// are. At most one is logged every two seconds, with the number left out.

namespace band3::stall_watch {

// the game thread's marks in DxRnd::Present's hook: it began, the game's
// Present returned, capture finished the frame, the frame cap's wait ended,
// and the hook returned (a render check's hold included)
void PresentBegin();
void PresentDone();
void FinishDone();
void PaceDone();
void FrameEnd();

// What the native renderer's worker is doing (native_view.cpp, gpu_view.cpp),
// for a stall's context: one store each.
enum class Worker : int {
    kIdle,       // no worker, or between frames
    kWaiting,    // for a capture, a slot or a frame's due time
    kRecording,  // recording and submitting a frame
    kGpuWait,    // waiting for the GPU to finish a frame
};
void SetWorker(Worker w);
// the worker's thread, whose stack a stall samples too: from the worker as it
// starts (native_view.cpp's Renderer::Run)
void SetWorkerThread();

// at shutdown: stops the watcher
void Stop();

}  // namespace band3::stall_watch
