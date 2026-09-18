/* audio.h — 音乐（FDMUS .mid → libADLMIDI，AIL 音量模型 + FD2SAMPLE.wopl
 * 音色库，对齐 fd2re/src/audio.c 的合成器配置）与音效（sfx 包 .wav
 * 8bit unsigned 11025Hz 单声道，重采样混音）。 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ADL_MIDIPlayer;

class Audio {
public:
    bool init();
    void shutdown();

    /* 成功返回 true；err（可选）带回 adl_errorInfo */
    bool music_open(const std::string& mid_path, const std::string& bank_path,
                    std::string* err = nullptr);
    void music_close();
    bool music_at_end();

    /* sfx 包条目（fd2play 内部以序号引用） */
    int wav_add(const std::vector<uint8_t>& pcm_8u_mono, int rate);
    void sfx_play(int id, int loop = 0);   /* loop>0 = 重复次数（0=单次） */
    void sfx_stop_all();

    void lock();
    void unlock();

private:
    struct Voice {
        int id = -1;
        double pos = 0;
        double step = 0;
        int loop_left = 0;
    };
    static void callback(void* userdata, uint8_t* stream, int len);

    uint32_t dev_ = 0;
    int rate_ = 44100;
    ADL_MIDIPlayer* adl_ = nullptr;
    std::vector<std::vector<uint8_t>> wavs_;   /* id → PCM（8bit unsigned） */
    std::vector<double> wav_steps_;
    std::vector<Voice> voices_;
};
