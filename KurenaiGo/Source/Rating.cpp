#include "Rating.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace KurenaiGo
{
    RatingData LoadRating(const std::filesystem::path& path)
    {
        RatingData data;

        std::ifstream file(path);
        if (!file)
        {
            return data;
        }

        int parsedCount = 0;
        double lastRating = data.Rating;
        std::string line;
        while (std::getline(file, line))
        {
            std::istringstream iss(line);
            std::string timestamp;
            double rating = 0.0;
            if (iss >> timestamp >> rating)
            {
                lastRating = rating;
                ++parsedCount;
            }
        }

        if (parsedCount > 0)
        {
            data.Rating = lastRating;
            data.GamesPlayed = parsedCount;
        }
        return data;
    }

    void AppendRatingEntry(const std::filesystem::path& path,
        const std::string& timestamp, double ratingAfter, const std::string& result)
    {
        std::ofstream file(path, std::ios::app);
        if (!file)
        {
            throw std::runtime_error("レーティング履歴ファイルを開けませんでした: " + path.string());
        }
        file << timestamp << " " << ratingAfter << " " << result << "\n";
        if (!file)
        {
            throw std::runtime_error("レーティング履歴の書き込みに失敗しました: " + path.string());
        }
    }

    bool TryParseBlackWinFraction(const std::string& result, double& outScore)
    {
        if (result == "W+R")
        {
            outScore = 0.0;
            return true;
        }
        if (result == "B+R")
        {
            outScore = 1.0;
            return true;
        }
        if (result == "0")
        {
            outScore = 0.5;
            return true;
        }
        if (result.rfind("B+", 0) == 0)
        {
            outScore = 1.0;
            return true;
        }
        if (result.rfind("W+", 0) == 0)
        {
            outScore = 0.0;
            return true;
        }
        return false;
    }

    double ComputeEloDelta(double userRating, double opponentRating, double actualScore, double kFactor)
    {
        const double expected = 1.0 / (1.0 + std::pow(10.0, (opponentRating - userRating) / 400.0));
        return kFactor * (actualScore - expected);
    }

    double InvertEloForRating(double expectedScore, double opponentRating)
    {
        const double clamped = (std::min)(0.99, (std::max)(0.01, expectedScore));
        return opponentRating - 400.0 * std::log10(1.0 / clamped - 1.0);
    }
}
