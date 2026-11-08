#pragma once
#include "emberfall/game_session.hpp"
#include <fstream>

namespace emberfall::scene {
struct AudioOutputOptions {
    std::filesystem::path wav, report;
    unsigned start_ms=0, duration_ms=16000, dialogue_ms=0;
    bool wait_voice=false;
};
struct ChoiceTrace { std::string script; unsigned word, selected, count; };

// Offline device clock. Pulls exactly the frames elapsed before the next VM input.
class AudioExport {
public:
    AudioExport(const AudioOutputOptions& options,unsigned sample_rate,std::size_t restored_events);
    void advance(nnn::audio::GameAudio& audio,unsigned ms);
    void finish(const GameSession& game,const std::string& entry,const std::vector<ChoiceTrace>& choices);
private:
    AudioOutputOptions options_;
    unsigned rate_,remainder_=0;
    std::size_t restored_events_;
    std::uint64_t elapsed_=0,begin_=0,end_=0,written_=0,nonzero_=0;
    std::ofstream wave_;
};
} // namespace emberfall::scene

