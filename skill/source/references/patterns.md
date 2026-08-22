# Patterns

Working shapes for the three things that are easy to get subtly wrong: sharing control
state with a realtime thread, owning the buffer you hand back to a host, and keeping a
nested hook from processing twice.

## Contents

- [Seqlock shared state](#seqlock-shared-state)
- [Buffer ownership at a capture hook](#buffer-ownership-at-a-capture-hook)
- [Marker interface to prevent double processing](#marker-interface-to-prevent-double-processing)
- [Making the DSP testable offline](#making-the-dsp-testable-offline)

## Seqlock shared state

One writer (the GUI), one reader (the audio thread), no blocking on either side. The
writer makes the counter odd, writes, then makes it even. A reader that sees an odd count,
or sees the count change across its read, simply retries.

```cpp
template<typename T>
void SeqWrite(volatile uint32_t& seq, T& dst, const T& src)
{
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t s = seq;
    seq = s + 1;                                        // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    dst = src;
    std::atomic_thread_fence(std::memory_order_release);
    seq = s + 2;                                        // even: consistent again
}

// Returns false if it could not read a consistent value within the retry budget.
// The caller keeps its previous copy rather than blocking.
template<typename T>
bool SeqRead(const volatile uint32_t& seq, const T& src, T& dst, int retries = 8)
{
    for (int i = 0; i < retries; ++i)
    {
        const uint32_t s0 = seq;
        if (s0 & 1u) continue;
        std::atomic_thread_fence(std::memory_order_acquire);
        dst = src;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq == s0) return true;
    }
    return false;
}
```

The audio thread caches the last good value, so a GUI stuck mid-write costs nothing:

```cpp
const Control& AudioThreadControl()
{
    Control tmp;
    if (SeqRead(shm->ctlSeq, shm->control, tmp))
        m_cached = tmp;
    return m_cached;     // last known good if the GUI was mid-write
}
```

Put a magic number and a version in the shared block. Both processes create-or-open it,
and whichever gets there first initialises it; an interlocked compare-exchange on the
magic settles the race. If the version does not match, detach and ignore rather than
misinterpreting a different layout.

For telemetry going the other way, a heartbeat that increments every audio block is what
lets the UI distinguish "connected" from "the host is not running" - check whether the
value moved between polls, not whether it is non-zero.

## Buffer ownership at a capture hook

The pointer a capture API returns belongs to the driver. Process a copy and return your
own memory, sized once at init.

```cpp
BYTE* Hook::GetBuffer(BYTE** ppData, UINT32* pFrames, DWORD* pFlags, ...)
{
    HRESULT hr = m_real.GetBuffer(ppData, pFrames, pFlags, ...);
    if (FAILED(hr) || !m_active || !ppData || !pFrames)
        return hr;                                  // untouched on anything unexpected

    BYTE* out = Process(*ppData, *pFrames, *pFlags);
    if (out) *ppData = out;                         // hand the host our copy
    return hr;
}

BYTE* Process(const BYTE* src, unsigned frames, DWORD flags)
{
    // refuse rather than allocate on this thread
    if (frames == 0 || frames > m_maxFrames)
        return nullptr;

    // a silent packet may point at uninitialised memory: synthesise it instead of
    // reading it, but still run it through so the delay line stays continuous
    if ((flags & SILENT) || !src)
        memset(m_out.data(), 0, frames * m_blockAlign);
    else
        memcpy(m_out.data(), src, frames * m_blockAlign);

    for (unsigned ch = 0; ch < m_channels; ++ch)
    {
        Deinterleave(m_out.data() + ch * m_sampleBytes, m_scratch.data(), frames);
        m_dsp[ch].Process(m_scratch.data(), frames);
        Interleave(m_scratch.data(), m_out.data() + ch * m_sampleBytes, frames);
    }
    return m_out.data();
}
```

`m_maxFrames` comes from the client's buffer size at init. Returning `nullptr` for an
oversized packet means the host gets its own untouched buffer, which is always safe.

Reuse the host's own format conversion if it has one rather than writing your own
int16/int24/int32/float paths - it is already correct and already handles the stride.

## Marker interface to prevent double processing

When the layer you hook also wraps your other implementation, ask for identity:

```cpp
// {6F2B1A54-...} private, never registered
extern const GUID IID_MyDspMarker;

// In your own audio client, answer to it:
if (riid == IID_MyDspMarker) { *ppv = this; AddRef(); return S_OK; }

// In the hook, stand down when the thing underneath is already yours:
IUnknown* marker = nullptr;
if (SUCCEEDED(m_real.QueryInterface(IID_MyDspMarker, (void**)&marker)) && marker)
{
    m_skip = true;          // already handled upstream
    marker->Release();
}
```

## Making the DSP testable offline

Split the code so the part that touches the host is thin and the part worth testing has no
dependency on it. A packet processor that takes a format, a frame count and a raw pointer
can be driven entirely from a console program; the wrapper around it that reads shared
memory, registers streams and publishes telemetry cannot.

Then the harness can feed the real code deliberately awkward input: ragged packet sizes
like 160, 441, 128, 1024, 96, 480 rather than a constant block; every bit depth and
channel count the host might negotiate; oversized packets, to confirm they are refused
rather than truncated; formats you do not support, to confirm they decline cleanly instead
of producing garbage.

For synthetic test material, a plucked string is a harmonic stack with per-harmonic decay,
slight inharmonicity and a short noise attack. That is enough to exercise a pitch tracker
honestly - a bare sine is not, because it hides every octave and alignment error.
