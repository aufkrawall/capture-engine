#include "mediaengine_internal.h"

void MediaEngine::AudioLoop() {
    AudioLoopState s;
    if (!AudioLoopInit(s))
        return;
    while (audioRunning) {
        AudioLoopIteration(s);
    }
    AudioLoopTail(s);
}
