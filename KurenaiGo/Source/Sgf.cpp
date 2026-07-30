#include "Sgf.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace KurenaiGo
{
    namespace
    {
        // SGFのテキスト値は']'と'\'をバックスラッシュでエスケープする
        std::string EscapeSgfText(const std::string& text)
        {
            std::string result;
            result.reserve(text.size());
            for (char c : text)
            {
                if (c == ']' || c == '\\')
                {
                    result += '\\';
                }
                result += c;
            }
            return result;
        }

        char SgfCoordChar(int index)
        {
            return static_cast<char>('a' + index);
        }

        // KurenaiGoの内部座標(row=0が盤面下端)をSGFの座標表記(列→行の順、行は盤面上端が0)へ
        // 変換する。kata-analyzeのownership配列で使った変換則(docs/KurenaiGo_Developer.html 8.1節)と同じ
        std::string ToSgfPoint(int row, int col, int boardSize)
        {
            const int sgfRow = (boardSize - 1) - row;
            return std::string(1, SgfCoordChar(col)) + std::string(1, SgfCoordChar(sgfRow));
        }

        void FromSgfPoint(const std::string& point, int boardSize, int& outRow, int& outCol)
        {
            if (point.size() < 2)
            {
                throw std::runtime_error("SGFの着手座標を解釈できませんでした: " + point);
            }
            outCol = point[0] - 'a';
            const int sgfRow = point[1] - 'a';
            outRow = (boardSize - 1) - sgfRow;
        }

        void SkipWhitespace(const std::string& text, size_t& pos)
        {
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])))
            {
                ++pos;
            }
        }
    }

    std::string WriteSgf(const SgfGameRecord& record)
    {
        std::ostringstream out;
        out << "(;GM[1]FF[4]SZ[" << record.BoardSize << "]KM[" << record.Komi << "]"
            << "PB[Human]PW[KataGo]";
        if (!record.Result.empty())
        {
            out << "RE[" << EscapeSgfText(record.Result) << "]";
        }

        for (const SgfMove& move : record.Moves)
        {
            const char colorChar = (move.Color == Stone::Black) ? 'B' : 'W';
            out << ";" << colorChar << "[";
            if (!move.IsPass)
            {
                out << ToSgfPoint(move.Row, move.Col, record.BoardSize);
            }
            out << "]";
        }

        out << ")";
        return out.str();
    }

    SgfGameRecord ReadSgf(const std::string& sgfText)
    {
        // 本実装は分岐(variation)の無い単一手順のみを扱う最小限のSGFパーサ。
        // 自分自身が書き出したSGFを読み戻せることを目的とする(docs/KurenaiGo_Developer.html参照)
        SgfGameRecord record;

        size_t pos = sgfText.find('(');
        if (pos == std::string::npos)
        {
            throw std::runtime_error("SGFの開始'('が見つかりませんでした");
        }
        ++pos;

        for (;;)
        {
            SkipWhitespace(sgfText, pos);
            if (pos >= sgfText.size() || sgfText[pos] == ')')
            {
                break;
            }
            if (sgfText[pos] != ';')
            {
                throw std::runtime_error("SGFのノード区切り';'が見つかりませんでした");
            }
            ++pos;

            SkipWhitespace(sgfText, pos);
            while (pos < sgfText.size() && std::isupper(static_cast<unsigned char>(sgfText[pos])))
            {
                const size_t identStart = pos;
                while (pos < sgfText.size() && std::isupper(static_cast<unsigned char>(sgfText[pos])))
                {
                    ++pos;
                }
                const std::string ident = sgfText.substr(identStart, pos - identStart);

                std::vector<std::string> values;
                SkipWhitespace(sgfText, pos);
                while (pos < sgfText.size() && sgfText[pos] == '[')
                {
                    ++pos;
                    std::string value;
                    while (pos < sgfText.size() && sgfText[pos] != ']')
                    {
                        if (sgfText[pos] == '\\' && pos + 1 < sgfText.size())
                        {
                            ++pos;
                        }
                        value += sgfText[pos];
                        ++pos;
                    }
                    if (pos >= sgfText.size())
                    {
                        throw std::runtime_error("SGFのプロパティ値の終端']'が見つかりませんでした");
                    }
                    ++pos;
                    values.push_back(value);
                    SkipWhitespace(sgfText, pos);
                }

                if (values.empty())
                {
                    throw std::runtime_error("SGFのプロパティ'" + ident + "'に値がありませんでした");
                }

                if (ident == "SZ")
                {
                    record.BoardSize = std::stoi(values[0]);
                }
                else if (ident == "KM")
                {
                    record.Komi = std::stof(values[0]);
                }
                else if (ident == "RE")
                {
                    record.Result = values[0];
                }
                else if (ident == "B" || ident == "W")
                {
                    SgfMove move;
                    move.Color = (ident == "B") ? Stone::Black : Stone::White;
                    if (values[0].empty())
                    {
                        move.IsPass = true;
                    }
                    else
                    {
                        FromSgfPoint(values[0], record.BoardSize, move.Row, move.Col);
                    }
                    record.Moves.push_back(move);
                }
                else if (ident == "AB" || ident == "AW")
                {
                    // 初期配置(詰碁の問題図、17章)。1つのプロパティに複数の座標が並ぶ
                    const Stone color = (ident == "AB") ? Stone::Black : Stone::White;
                    for (const std::string& value : values)
                    {
                        if (value.empty())
                        {
                            continue; // 空の値は座標として意味を持たないため無視する
                        }
                        SgfSetupStone stone;
                        stone.Color = color;
                        FromSgfPoint(value, record.BoardSize, stone.Row, stone.Col);
                        record.SetupStones.push_back(stone);
                    }
                }
                else if (ident == "SQ")
                {
                    // 詰碁(17章)で生死を判定する対象の石を指す四角マーク。1つのプロパティに
                    // 複数の座標が並ぶ
                    for (const std::string& value : values)
                    {
                        if (value.empty())
                        {
                            continue;
                        }
                        int row = -1;
                        int col = -1;
                        FromSgfPoint(value, record.BoardSize, row, col);
                        record.MarkedPoints.push_back({ row, col });
                    }
                }
                else if (ident == "PL")
                {
                    // 手番。SGFでは"B"/"W"(小文字も許容される)
                    if (!values[0].empty())
                    {
                        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(values[0][0])));
                        if (c == 'B')
                        {
                            record.PlayerToMove = Stone::Black;
                        }
                        else if (c == 'W')
                        {
                            record.PlayerToMove = Stone::White;
                        }
                    }
                }
                else if (ident == "GN")
                {
                    record.GameName = values[0];
                }
                else if (ident == "C")
                {
                    record.Comment = values[0];
                }
                // GM/FF/PB/PW等、上記以外のプロパティは読み飛ばす(対局進行には不要)

                SkipWhitespace(sgfText, pos);
            }
        }

        return record;
    }

    void WriteSgfFile(const std::filesystem::path& path, const SgfGameRecord& record)
    {
        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("SGFファイルを作成できませんでした: " + path.string());
        }
        const std::string text = WriteSgf(record);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file)
        {
            throw std::runtime_error("SGFファイルの書き込みに失敗しました: " + path.string());
        }
    }

    SgfGameRecord ReadSgfFile(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            throw std::runtime_error("SGFファイルを開けませんでした: " + path.string());
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        return ReadSgf(buffer.str());
    }
}
