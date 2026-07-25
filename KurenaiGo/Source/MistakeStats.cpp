#include "MistakeStats.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace KurenaiGo
{
    namespace
    {
        const char* PhaseToString(GamePhase phase)
        {
            switch (phase)
            {
            case GamePhase::Opening: return "Opening";
            case GamePhase::Middle:  return "Middle";
            case GamePhase::Endgame: return "Endgame";
            }
            return "Opening";
        }

        bool PhaseFromString(const std::string& text, GamePhase& outPhase)
        {
            if (text == "Opening") { outPhase = GamePhase::Opening; return true; }
            if (text == "Middle")  { outPhase = GamePhase::Middle; return true; }
            if (text == "Endgame") { outPhase = GamePhase::Endgame; return true; }
            return false;
        }

        const char* QualityToString(MoveQuality quality)
        {
            switch (quality)
            {
            case MoveQuality::Best:       return "Best";
            case MoveQuality::SlightLoss: return "SlightLoss";
            case MoveQuality::Loose:      return "Loose";
            case MoveQuality::Blunder:    return "Blunder";
            }
            return "Best";
        }

        bool QualityFromString(const std::string& text, MoveQuality& outQuality)
        {
            if (text == "Best")       { outQuality = MoveQuality::Best; return true; }
            if (text == "SlightLoss") { outQuality = MoveQuality::SlightLoss; return true; }
            if (text == "Loose")      { outQuality = MoveQuality::Loose; return true; }
            if (text == "Blunder")    { outQuality = MoveQuality::Blunder; return true; }
            return false;
        }
    }

    GamePhase DeterminePhase(int moveIndex, int totalMoves)
    {
        if (totalMoves <= 0)
        {
            return GamePhase::Opening;
        }
        const double fraction = static_cast<double>(moveIndex) / static_cast<double>(totalMoves);
        if (fraction <= 1.0 / 3.0)
        {
            return GamePhase::Opening;
        }
        if (fraction <= 2.0 / 3.0)
        {
            return GamePhase::Middle;
        }
        return GamePhase::Endgame;
    }

    MistakeStatsData LoadMistakeStats(const std::filesystem::path& path)
    {
        MistakeStatsData data;

        std::ifstream file(path);
        if (!file)
        {
            return data;
        }

        std::string line;
        while (std::getline(file, line))
        {
            std::istringstream iss(line);
            std::string sgfFileName;
            int moveIndex = 0;
            std::string phaseText;
            std::string qualityText;
            if (!(iss >> sgfFileName >> moveIndex >> phaseText >> qualityText))
            {
                continue;
            }
            GamePhase phase;
            MoveQuality quality;
            if (!PhaseFromString(phaseText, phase) || !QualityFromString(qualityText, quality))
            {
                continue;
            }
            data.Counts[static_cast<int>(phase)][static_cast<int>(quality)] += 1;
            data.SeenKeys.insert(sgfFileName + ":" + std::to_string(moveIndex));
        }
        return data;
    }

    void AppendMistakeEntry(const std::filesystem::path& path, const std::string& sgfFileName,
        int moveIndex, GamePhase phase, MoveQuality quality)
    {
        std::ofstream file(path, std::ios::app);
        if (!file)
        {
            throw std::runtime_error("苦手分野の統計ファイルを開けませんでした: " + path.string());
        }
        file << sgfFileName << " " << moveIndex << " " << PhaseToString(phase) << " "
            << QualityToString(quality) << "\n";
        if (!file)
        {
            throw std::runtime_error("苦手分野の統計の書き込みに失敗しました: " + path.string());
        }
    }
}
