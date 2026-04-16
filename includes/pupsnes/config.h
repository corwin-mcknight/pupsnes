#pragma once

// ---------------------------------------------------------------------------
// Build profile configuration
//
// CMake presets define exactly one of:
//   PUPSNES_PROFILE_DEBUG      — dev preset (local development)
//   PUPSNES_PROFILE_CI         — ci preset  (continuous integration)
//   PUPSNES_PROFILE_PRODUCTION — release preset (shipping builds)
//
// Each knob is a constexpr bool.  The compiler eliminates dead branches
// entirely, so disabled knobs have zero runtime cost.
// ---------------------------------------------------------------------------

namespace pupsnes::config {

#if defined(PUPSNES_PROFILE_DEBUG)

inline constexpr bool kLogUnmappedBusAccess = true;
inline constexpr bool kLogSchedulerEvents = true;
inline constexpr bool kLogBusAccess = false;
inline constexpr bool kLogCpuExecution = false;
inline constexpr bool kEnableDeviceAsserts = true;

#elif defined(PUPSNES_PROFILE_CI)

inline constexpr bool kLogUnmappedBusAccess = false;
inline constexpr bool kLogSchedulerEvents = false;
inline constexpr bool kLogBusAccess = false;
inline constexpr bool kLogCpuExecution = false;
inline constexpr bool kEnableDeviceAsserts = true;

#else  // PUPSNES_PROFILE_PRODUCTION (default)

inline constexpr bool kLogUnmappedBusAccess = false;
inline constexpr bool kLogSchedulerEvents = false;
inline constexpr bool kLogBusAccess = false;
inline constexpr bool kLogCpuExecution = false;
inline constexpr bool kEnableDeviceAsserts = false;

#endif

}  // namespace pupsnes::config
