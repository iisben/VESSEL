#pragma once

#include "Wavetable.h"
#include "TableList.h"
#include <atomic>
#include <memory>
#include <mutex>

namespace vessel::dsp
{
/** Imported wavetables (the Echo Sound Works Core Tables, CC0). Menu index 0 is the built-in table; index i > 0 is
    kTables[i - 1], stored as 16-bit FLAC in the plug-in bundle's Resources/Wavetables (the source tree's
    Wavetables folder for the command-line tools). */
juce::File tablesFolder();
/** Command-line tools (no bundle): read the tables from this folder instead. Call before the first load. */
void setTablesFolder (const juce::File&);

/** Decodes and band-limits one table. nullptr for index 0 (the built-in table) or if the file cannot be read. */
std::unique_ptr<WavetableBank> loadTable (int menuIndex, juce::String* error = nullptr);

/** The menu names: "Vessel", then "Category - Name" for every imported table (the wt_table choices). */
const juce::StringArray& tableMenuNames();

//==============================================================================
/** Owns the decoded tables and publishes the selected one to the audio thread.

    The audio thread only ever stores the wanted index (request) and loads the current pointer (current), both
    lock-free. A background thread notices a new request within ~15 ms, decodes the table (FLAC + FFT, ~5-20 ms)
    and swaps the pointer, so the old table plays until the new one is ready. Preset loads, state restores and
    prepareToPlay call loadNow instead, which finishes before returning, so the first note after a preset change
    (or the first sample of an offline bounce) already uses the right table.

    Decoded tables stay cached (up to kCache); one is freed only after it has been off the audio thread's pointer
    for two seconds, long after any block that could still be reading it has finished. */
class TableStore : private juce::Thread
{
public:
    TableStore();
    ~TableStore() override;

    /** Audio thread, lock-free and non-blocking. */
    void request (int menuIndex) noexcept { wanted.store (menuIndex, std::memory_order_relaxed); }
    const WavetableBank* current() const noexcept { return cur.load (std::memory_order_acquire); }
    int currentIndex() const noexcept { return curIndex.load (std::memory_order_acquire); }

    /** Audio thread hazard-pointer pin/unpin across processBlock so evict() cannot free an active bank. */
    const WavetableBank* acquireAudio() noexcept
    {
        const WavetableBank* b = cur.load (std::memory_order_acquire);
        for (;;)
        {
            audioPin.store (b, std::memory_order_seq_cst);
            const WavetableBank* now = cur.load (std::memory_order_seq_cst);
            if (b == now) return b;
            b = now;
        }
    }
    void releaseAudio() noexcept { audioPin.store (nullptr, std::memory_order_release); }

    /** UI / message thread: returns a ref-counted handle safe against background eviction. */
    std::shared_ptr<const WavetableBank> currentShared() const;

    /** Message thread / prepare: makes menuIndex current before returning. */
    void loadNow (int menuIndex);

    static constexpr int kCache = 4;   // a 64-frame table is ~5.8 MB decoded (11 mip levels)

private:
    void run() override;
    void makeCurrent (int menuIndex);
    void evict();

    struct Entry { int index; std::shared_ptr<const WavetableBank> bank; double offSince; };
    mutable std::mutex lock;
    std::vector<Entry> cache;
    std::atomic<const WavetableBank*> cur { &WavetableBank::get() };
    std::atomic<const WavetableBank*> audioPin { nullptr };
    std::atomic<int> wanted { 0 }, curIndex { 0 };
};
} // namespace vessel::dsp
