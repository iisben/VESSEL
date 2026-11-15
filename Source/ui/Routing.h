#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>

class VesselProcessor;

namespace vessel::ui
{
/** A modulation source a control can be routed from: one of the three LFOs (four routes each), one of the
    three expression sources (two routes each) or one of the eight macros (six routes each). Routes are ordinary parameters (dest + amount), so a mapping
    made by drag-and-drop or the right-click menu is saved, automatable and undoable like any other edit. */
struct ModSource
{
    enum Kind { Lfo, Expr, Macro };
    Kind kind = Lfo;
    int index = 0;   // LFO 0..2, expression 0 velocity / 1 mod wheel / 2 aftertouch, or macro 0..7

    int slots() const noexcept;
    int destPid (int slot) const noexcept;
    int amtPid (int slot) const noexcept;
    juce::String name() const;
    /** The name shown to the user: a macro's own caption when it has one. */
    juce::String displayName (VesselProcessor&) const;
    juce::String dragId() const;
    static bool fromDragId (const juce::var&, ModSource&);
    bool operator== (const ModSource& o) const noexcept { return kind == o.kind && index == o.index; }
};

namespace routing
{
    std::vector<ModSource> allSources();
    bool isDestination (int pid);
    /** The slot of `src` routed to `target`, or -1. */
    int slotFor (VesselProcessor&, ModSource src, int target);
    /** First slot of `src` whose destination is None, or -1 when every route is in use. */
    int freeSlot (VesselProcessor&, ModSource src);

    enum class Result { Added, Exists, Full, NotADestination, Removed };
    /** Routes src → target in the first free slot at a moderate default depth. */
    Result add (VesselProcessor&, ModSource src, int target, int& slotOut);
    /** Clears every route from src to target. */
    Result remove (VesselProcessor&, ModSource src, int target);
    /** Every (source, slot) currently routed to target. */
    std::vector<std::pair<ModSource, int>> routesTo (VesselProcessor&, int target);
}

/** Implemented by the editor, which is told whenever a control makes or removes a route, so it can bring the
    source's page forward and point at the route. */
struct RouteListener
{
    virtual ~RouteListener() = default;
    virtual void routeChanged (ModSource, int slot, routing::Result, int targetPid) = 0;
    virtual void routesCleared (int targetPid) = 0;
};

/** The drag image for a source: a chlorine diamond and the source's name. */
juce::Image makeDragImage (const juce::String& name);
} // namespace vessel::ui
