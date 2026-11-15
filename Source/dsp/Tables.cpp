#include "Tables.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace vessel::dsp
{
namespace { juce::File& folderOverride() { static juce::File f; return f; } }

void setTablesFolder (const juce::File& folder) { folderOverride() = folder; }

juce::File tablesFolder()
{
    if (folderOverride() != juce::File()) return folderOverride();
    // inside a bundle the binary is <bundle>/Contents/MacOS/<binary>: the tables are in Contents/Resources
    const auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    const auto bundled = exe.getParentDirectory().getParentDirectory().getChildFile ("Resources").getChildFile ("Wavetables");
    if (bundled.isDirectory()) return bundled;
    const auto sibling = exe.getSiblingFile ("Wavetables");
    if (sibling.isDirectory()) return sibling;
    return bundled;
}

std::unique_ptr<WavetableBank> loadTable (int menuIndex, juce::String* error)
{
    if (menuIndex <= 0 || menuIndex > kNumTables) return nullptr;
    const auto& info = kTables[menuIndex - 1];
    const auto file = tablesFolder().getChildFile (info.file);

    // a missing file (a damaged install) must fall back to the built-in table, not hand the reader a null stream
    auto stream = file.createInputStream();
    if (stream == nullptr)
    {
        if (error != nullptr) *error = "missing " + file.getFullPathName();
        return nullptr;
    }
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (stream.release(), true));
    if (reader == nullptr)
    {
        if (error != nullptr) *error = "cannot read " + file.getFullPathName();
        return nullptr;
    }
    const int frames = (int) std::min<juce::int64> (reader->lengthInSamples / WavetableBank::kSize, info.frames);
    if (frames < 1)
    {
        if (error != nullptr) *error = "no frames in " + file.getFullPathName();
        return nullptr;
    }
    juce::AudioBuffer<float> buf (1, frames * WavetableBank::kSize);
    reader->read (&buf, 0, buf.getNumSamples(), 0, true, false);
    return WavetableBank::fromFrames (buf.getReadPointer (0), frames);
}

const juce::StringArray& tableMenuNames()
{
    static const juce::StringArray names = []
    {
        juce::StringArray n { "Vessel" };
        for (const auto& t : kTables) n.add (juce::String (t.category) + " - " + juce::String::fromUTF8 (t.name));
        return n;
    }();
    return names;
}

//==============================================================================
TableStore::TableStore() : juce::Thread ("VESSEL tables")
{
    startThread (juce::Thread::Priority::low);
}

TableStore::~TableStore()
{
    stopThread (2000);
}

std::shared_ptr<const WavetableBank> TableStore::currentShared() const
{
    const std::lock_guard<std::mutex> g (lock);
    const auto* c = cur.load (std::memory_order_acquire);
    for (const auto& e : cache)
        if (e.bank.get() == c) return e.bank;
    return std::shared_ptr<const WavetableBank> (&WavetableBank::get(), [] (const WavetableBank*) {});
}

void TableStore::loadNow (int menuIndex)
{
    const std::lock_guard<std::mutex> g (lock);
    wanted.store (menuIndex, std::memory_order_relaxed);   // the loader must not chase an older request afterwards
    makeCurrent (menuIndex);
}

void TableStore::makeCurrent (int menuIndex)
{
    // under lock
    if (menuIndex == curIndex.load()) return;

    const WavetableBank* next = nullptr;
    if (menuIndex <= 0 || menuIndex > kNumTables) next = &WavetableBank::get();
    else
    {
        for (auto& e : cache) if (e.index == menuIndex) next = e.bank.get();
        if (next == nullptr)
        {
            juce::String err;
            std::shared_ptr<const WavetableBank> bank = loadTable (menuIndex, &err);
            if (bank == nullptr) { DBG ("VESSEL: " << err); next = &WavetableBank::get(); }
            else { next = bank.get(); cache.push_back ({ menuIndex, std::move (bank), 0.0 }); }
        }
    }

    const double now = juce::Time::getMillisecondCounterHiRes();
    for (auto& e : cache) if (e.bank.get() == cur.load()) e.offSince = now;   // leaving the audio thread's hands
    for (auto& e : cache) if (e.bank.get() == next) e.offSince = 0.0;
    cur.store (next, std::memory_order_seq_cst);
    curIndex.store (menuIndex, std::memory_order_release);
    evict();
}

void TableStore::evict()
{
    // under lock: free the oldest tables beyond the cache size, never the current/pinned one or one released < 2 s ago
    const double now = juce::Time::getMillisecondCounterHiRes();
    const auto* currentBank = cur.load (std::memory_order_seq_cst);
    const auto* pinnedBank  = audioPin.load (std::memory_order_seq_cst);
    while ((int) cache.size() > kCache)
    {
        int victim = -1;
        for (int i = 0; i < (int) cache.size(); ++i)
        {
            const auto& e = cache[(size_t) i];
            if (e.bank.get() == currentBank || e.bank.get() == pinnedBank || e.offSince <= 0.0 || now - e.offSince < 2000.0) continue;
            if (victim < 0 || e.offSince < cache[(size_t) victim].offSince) victim = i;
        }
        if (victim < 0) break;
        cache.erase (cache.begin() + victim);
    }
}

void TableStore::run()
{
    while (! threadShouldExit())
    {
        wait (15);
        const std::lock_guard<std::mutex> g (lock);
        const int w = wanted.load (std::memory_order_relaxed);   // read under the lock: a loadNow cannot slip in between
        if (w != curIndex.load()) makeCurrent (w);
        else evict();
    }
}
} // namespace vessel::dsp
