#pragma once

// Thin wrapper over Tracy profiler macros.
// When COBRA_ENABLE_TRACY is defined, these expand to Tracy zones/plots.
// When not defined, they compile to nothing (zero overhead).
//
// Usage:
//   COBRA_ZONE()                  — zone named after the enclosing function
//   COBRA_ZONE_N("PassName")      — zone with an explicit name
//   COBRA_ZONE_TEXT(str)           — attach a dynamic string to the current zone
//   COBRA_ZONE_VALUE(val)         — attach a numeric value to the current zone
//   COBRA_FRAME()                 — mark a frame boundary (one per expression)
//   COBRA_PLOT(name, val)         — plot a scalar on the timeline
//   COBRA_MSG(literal)            — log a message literal to the timeline

#ifdef COBRA_ENABLE_TRACY

    #include <tracy/Tracy.hpp>

    #define COBRA_ZONE()          ZoneScoped
    #define COBRA_ZONE_N(name)    ZoneScopedN(name)
    #define COBRA_ZONE_TEXT(str)  ZoneText((str).data(), (str).size())
    #define COBRA_ZONE_VALUE(val) ZoneValue(val)
    #define COBRA_FRAME()         FrameMark
    #define COBRA_PLOT(name, val) TracyPlot(name, val)
    #define COBRA_MSG(literal)    TracyMessageL(literal)

#else

    #define COBRA_ZONE()          ((void) 0)
    #define COBRA_ZONE_N(name)    ((void) 0)
    #define COBRA_ZONE_TEXT(str)  ((void) 0)
    #define COBRA_ZONE_VALUE(val) ((void) 0)
    #define COBRA_FRAME()         ((void) 0)
    #define COBRA_PLOT(name, val) ((void) 0)
    #define COBRA_MSG(literal)    ((void) 0)

#endif
