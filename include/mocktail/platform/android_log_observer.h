#ifndef MOCKTAIL_PLATFORM_ANDROID_LOG_OBSERVER_H_
#define MOCKTAIL_PLATFORM_ANDROID_LOG_OBSERVER_H_

// Observes the fully formatted payload written through Mocktail's liblog
// adapter. The callback must not retain tag or message; both are borrowed for
// the duration of the call. Passing nullptr removes the current observer.
using MocktailAndroidLogObserver = void (*)(int priority, const char* tag,
                                            const char* message);

extern "C" void mocktail_android_log_set_observer(
    MocktailAndroidLogObserver observer);

#endif  // MOCKTAIL_PLATFORM_ANDROID_LOG_OBSERVER_H_
