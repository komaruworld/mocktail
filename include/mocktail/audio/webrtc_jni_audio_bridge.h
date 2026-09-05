#ifndef MOCKTAIL_AUDIO_WEBRTC_JNI_AUDIO_BRIDGE_H_
#define MOCKTAIL_AUDIO_WEBRTC_JNI_AUDIO_BRIDGE_H_

#include "mocktail/status.h"

namespace jnivm {
class VM;
}

namespace mocktail::audio {

// Installs WebRtcAudioManager, WebRtcAudioRecord and WebRtcAudioTrack. The
// manager only advertises the host PCM contract; capture opens on recording
// init, never on manager construction. SDL's audio subsystem must remain alive
// until ShutdownWebRtcJniAudioBridge.
Status InstallWebRtcJniAudioBridge(jnivm::VM* vm);
Status ShutdownWebRtcJniAudioBridge(jnivm::VM* vm);

}  // namespace mocktail::audio

#endif  // MOCKTAIL_AUDIO_WEBRTC_JNI_AUDIO_BRIDGE_H_
