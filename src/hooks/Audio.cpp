#include <Geode/Geode.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>

#include "../bot/GameSim.hpp"

using namespace geode::prelude;

// While the bot simulates ahead (or rewinds with checkpoints) the game runs level triggers
// that would play sound effects or change the music. Those must not be heard: the real run
// plays them again when it gets there. Everything here is a no-op outside the bot.
namespace {
    bool muted() {
        return ac::simContext().active || ac::audioStateFrozen();
    }
}

class $modify(ACAudioEngine, FMODAudioEngine) {
    void loadAudioState(FMODAudioState& state) {
        if (muted()) return;
        FMODAudioEngine::loadAudioState(state);
    }

    void saveAudioState(FMODAudioState& state) {
        if (muted()) return;
        FMODAudioEngine::saveAudioState(state);
    }

    int playEffect(gd::string path) {
        if (muted()) return 0;
        return FMODAudioEngine::playEffect(path);
    }

    int playEffect(gd::string path, float speed, float unknown, float volume) {
        if (muted()) return 0;
        return FMODAudioEngine::playEffect(path, speed, unknown, volume);
    }

    int playEffectAsync(gd::string path) {
        if (muted()) return 0;
        return FMODAudioEngine::playEffectAsync(path);
    }

    int queuePlayEffect(
        gd::string audioFilename, float speed, float unknown, float volume, float pitch,
        bool fastFourierTransform, bool reverb, int start, int end, int fadeIn, int fadeOut,
        bool loop, int effectID, bool overrideFlag, int uniqueID, float minInterval, int group
    ) {
        if (muted()) return 0;
        return FMODAudioEngine::queuePlayEffect(
            audioFilename, speed, unknown, volume, pitch, fastFourierTransform, reverb, start, end,
            fadeIn, fadeOut, loop, effectID, overrideFlag, uniqueID, minInterval, group
        );
    }

    void queueStartMusic(
        gd::string audioFilename, float pitch, float unknown, float volume, bool loop, int start,
        int end, int fadeIn, int fadeOut, int musicID, bool unk, int channelID, bool noPrepare,
        bool dontReset
    ) {
        if (muted()) return;
        FMODAudioEngine::queueStartMusic(
            audioFilename, pitch, unknown, volume, loop, start, end, fadeIn, fadeOut, musicID, unk,
            channelID, noPrepare, dontReset
        );
    }

    void playMusic(gd::string path, bool shouldLoop, float fadeInTime, int channel) {
        if (muted()) return;
        FMODAudioEngine::playMusic(path, shouldLoop, fadeInTime, channel);
    }

    void startMusic(int start, int end, int fadeIn, int fadeOut, bool loop, int musicID, bool noResume, bool dontReset) {
        if (muted()) return;
        FMODAudioEngine::startMusic(start, end, fadeIn, fadeOut, loop, musicID, noResume, dontReset);
    }

    void fadeMusic(float duration, int channel, float startVolume, float endVolume) {
        if (muted()) return;
        FMODAudioEngine::fadeMusic(duration, channel, startVolume, endVolume);
    }

    void fadeInMusic(float duration, int channel) {
        if (muted()) return;
        FMODAudioEngine::fadeInMusic(duration, channel);
    }

    void fadeOutMusic(float duration, int channel) {
        if (muted()) return;
        FMODAudioEngine::fadeOutMusic(duration, channel);
    }
};
