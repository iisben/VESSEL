#pragma once

#include "Parameters.h"
#include <array>
#include <atomic>
#include <juce_core/juce_core.h>

namespace vessel
{
/** Lock-free MIDI CC Learn map shared safely between the audio thread and the message thread.
    - Audio thread: reads/writes only `std::atomic` slots (zero locks, zero allocations, zero I/O).
    - Message thread: arms/cancels learning, clears/binds CCs, and persists mappings to
      `<userApplicationDataDirectory>/VESSEL_II/midi_learn.xml` (cross-platform: macOS, Windows, Linux). */
class MidiLearnMap
{
public:
    static constexpr int kNumCCs = 128;

    MidiLearnMap()
    {
        for (auto& slot : ccToPid)
            slot.store (-1, std::memory_order_relaxed);
    }

    static bool isLearnableCC (int cc) noexcept
    {
        // Exclude sustain pedal (64) and channel mode messages (120..127)
        return cc >= 0 && cc < 120 && cc != 64;
    }

    /** Cross-platform user data directory for VESSEL II (never hardcoded OS paths). */
    static juce::File defaultAppDataDir()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
            .getChildFile ("VESSEL_II");
    }

    void setStorageDirectory (const juce::File& dir)
    {
        storageDir = dir;
    }

    juce::File storageFile() const
    {
        const auto dir = storageDir.getFullPathName().isNotEmpty() ? storageDir : defaultAppDataDir();
        return dir.getChildFile ("midi_learn.xml");
    }

    /** Arm MIDI Learn for `pid` (message thread). Next learnable CC received on audio thread binds it. */
    void beginLearn (int pid) noexcept
    {
        if (pid >= 0 && pid < NUM_PARAMS)
            learningPid.store (pid, std::memory_order_release);
    }

    void cancelLearn() noexcept
    {
        learningPid.store (-1, std::memory_order_release);
    }

    int currentlyLearningPid() const noexcept
    {
        return learningPid.load (std::memory_order_acquire);
    }

    /** Explicitly bind `cc -> pid` (1-to-1: clears any prior CC mapped to `pid`). Lock-free. */
    void bind (int cc, int pid) noexcept
    {
        if (! isLearnableCC (cc) || pid < 0 || pid >= NUM_PARAMS)
            return;
        for (int c = 0; c < kNumCCs; ++c)
            if (ccToPid[(size_t) c].load (std::memory_order_relaxed) == pid)
                ccToPid[(size_t) c].store (-1, std::memory_order_relaxed);
        ccToPid[(size_t) cc].store (pid, std::memory_order_release);
        dirtyForSave.store (true, std::memory_order_release);
        version.fetch_add (1, std::memory_order_release);
    }

    /** Unbind any CC mapped to `pid`. Lock-free. */
    void clearForPid (int pid) noexcept
    {
        bool changed = false;
        for (int c = 0; c < kNumCCs; ++c)
        {
            if (ccToPid[(size_t) c].load (std::memory_order_relaxed) == pid)
            {
                ccToPid[(size_t) c].store (-1, std::memory_order_release);
                changed = true;
            }
        }
        if (learningPid.load (std::memory_order_relaxed) == pid)
            learningPid.store (-1, std::memory_order_release);
        if (changed)
        {
            dirtyForSave.store (true, std::memory_order_release);
            version.fetch_add (1, std::memory_order_release);
        }
    }

    void clearCC (int cc) noexcept
    {
        if (cc < 0 || cc >= kNumCCs)
            return;
        if (ccToPid[(size_t) cc].exchange (-1, std::memory_order_acq_rel) >= 0)
        {
            dirtyForSave.store (true, std::memory_order_release);
            version.fetch_add (1, std::memory_order_release);
        }
    }

    void clearAll() noexcept
    {
        learningPid.store (-1, std::memory_order_release);
        for (auto& slot : ccToPid)
            slot.store (-1, std::memory_order_release);
        dirtyForSave.store (true, std::memory_order_release);
        version.fetch_add (1, std::memory_order_release);
    }

    /** Returns the CC number bound to `pid`, or -1 if unbound. Lock-free. */
    int ccForPid (int pid) const noexcept
    {
        for (int c = 0; c < kNumCCs; ++c)
            if (ccToPid[(size_t) c].load (std::memory_order_relaxed) == pid)
                return c;
        return -1;
    }

    /** Returns the PID bound to `cc`, or -1 if unbound. Lock-free (audio thread). */
    int pidForCC (int cc) const noexcept
    {
        if (cc < 0 || cc >= kNumCCs)
            return -1;
        return ccToPid[(size_t) cc].load (std::memory_order_acquire);
    }

    /** Audio-thread entry point for incoming MIDI CC.
        Captures active learn target if armed, then returns bound PID (or -1). Zero locks/allocs. */
    int processCCAudio (int cc) noexcept
    {
        if (! isLearnableCC (cc))
            return -1;
        int lp = learningPid.load (std::memory_order_acquire);
        if (lp >= 0 && lp < NUM_PARAMS)
        {
            if (learningPid.compare_exchange_strong (lp, -1, std::memory_order_acq_rel))
            {
                bind (cc, lp);
                return lp;
            }
        }
        return ccToPid[(size_t) cc].load (std::memory_order_acquire);
    }

    uint32_t getVersion() const noexcept { return version.load (std::memory_order_acquire); }

    bool consumeDirtyForSave() noexcept
    {
        return dirtyForSave.exchange (false, std::memory_order_acq_rel);
    }

    /** Serialise to ValueTree (`<MIDILEARN version="2">`) for state or disk persistence (message thread). */
    juce::ValueTree toValueTree() const
    {
        juce::ValueTree tree ("MIDILEARN");
        tree.setProperty ("version", 2, nullptr);
        for (int c = 0; c < kNumCCs; ++c)
        {
            const int pid = ccToPid[(size_t) c].load (std::memory_order_acquire);
            if (pid >= 0 && pid < NUM_PARAMS)
            {
                juce::ValueTree m ("Map");
                m.setProperty ("cc", c, nullptr);
                m.setProperty ("id", spec (pid).id, nullptr);
                tree.appendChild (m, nullptr);
            }
        }
        return tree;
    }

    void fromValueTree (const juce::ValueTree& tree, bool markDirty = false)
    {
        if (! tree.isValid() || ! tree.hasType ("MIDILEARN"))
            return;
        for (auto& slot : ccToPid)
            slot.store (-1, std::memory_order_relaxed);
        for (int i = 0; i < tree.getNumChildren(); ++i)
        {
            const auto child = tree.getChild (i);
            if (! child.hasType ("Map"))
                continue;
            const int cc = (int) child.getProperty ("cc", -1);
            const int pid = pidForId (child.getProperty ("id").toString());
            if (isLearnableCC (cc) && pid >= 0 && pid < NUM_PARAMS)
                ccToPid[(size_t) cc].store (pid, std::memory_order_release);
        }
        dirtyForSave.store (markDirty, std::memory_order_release);
        version.fetch_add (1, std::memory_order_release);
    }

    /** Save current map to cross-platform user application data directory (message thread only). */
    bool saveToDisk() const
    {
        const auto file = storageFile();
        if (! file.getParentDirectory().createDirectory())
            return false;
        const auto tree = toValueTree();
        if (auto xml = tree.createXml())
            return xml->writeTo (file);
        return false;
    }

    /** Load persisted map from disk if present (message thread only). */
    bool loadFromDisk()
    {
        const auto file = storageFile();
        if (! file.existsAsFile())
            return false;
        if (auto xml = juce::XmlDocument::parse (file))
        {
            const auto tree = juce::ValueTree::fromXml (*xml);
            if (tree.isValid() && tree.hasType ("MIDILEARN"))
            {
                fromValueTree (tree, false);
                return true;
            }
        }
        return false;
    }

private:
    std::array<std::atomic<int>, kNumCCs> ccToPid{};
    std::atomic<int> learningPid { -1 };
    std::atomic<bool> dirtyForSave { false };
    std::atomic<uint32_t> version { 0 };
    juce::File storageDir;
};
} // namespace vessel
