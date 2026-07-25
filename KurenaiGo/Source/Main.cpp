// KurenaiGo - 囲碁アプリ。
// KurenaiEngine2D(公開API)で19路盤・石を描画し、KataGo(https://github.com/lightvector/katago)を
// GTP(Go Text Protocol)で動かして人間(黒)とKataGo(白)の対局を行う。
// 座標系はワールド=ピクセル座標(原点は画面左下、Y-up)。
//
// 操作: 交点クリックで着手。着手以外の操作(パス・投了・地合い表示切替・着手ヒント表示切替・
//       棋譜再生・終了)は盤下のボタン行から行う。キーボードでも同じ操作が可能:
//       Pキーでパス / Rキーで投了 / Tキーで地合い表示切替 / Hキーで着手ヒント表示切替 /
//       対局終了後Vキーで棋譜再生 / 再生中は←→キーで手を戻す・進める / Escで終了

#include <Windows.h>

#include <objbase.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "GoBoard.h"
#include "KataGoClient.h"
#include "KurenaiEngine2D.h"
#include "KurenaiTypes.h"
#include "PathUtil.h"
#include "Sgf.h"

using namespace Kurenai;
using namespace KurenaiGo;

namespace
{
    // ソースは/utf-8オプションでコンパイルしているため、ナロー文字列リテラル("...")はUTF-8で
    // エンコードされる。MessageBoxAにそのまま渡すと、ANSI版はシステムのコードページ(日本語環境では
    // Shift-JIS)で解釈してしまい日本語部分が文字化けするため、UTF-16に変換してMessageBoxWへ渡す
    std::wstring Utf8ToWide(const std::string& utf8)
    {
        if (utf8.empty())
        {
            return std::wstring();
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
        return wide;
    }

    void ShowMessageBoxUtf8(const std::string& utf8Text, const std::string& utf8Caption, UINT type)
    {
        MessageBoxW(nullptr, Utf8ToWide(utf8Text).c_str(), Utf8ToWide(utf8Caption).c_str(), type);
    }

    // 盤の目の数(19路盤)
    constexpr int kBoardLines = 19;

    // 19路盤の星(hoshi)の位置。0-indexed(0〜18)で、標準的な4線交点
    constexpr std::array<int, 3> kHoshiIndices = { 3, 9, 15 };

    // 盤の下に確保する操作ボタン行の高さ。ComputeBoardLayoutはこの分を除いた領域に盤を配置する
    constexpr float kButtonBarHeight = 64.0f;
    // 盤の上に確保する損失グラフの帯の高さ(棋譜再生中のみ描画する。対局中は空のまま)
    constexpr float kGraphAreaHeight = 90.0f;

    constexpr uint32_t kWindowWidth = 900;
    constexpr uint32_t kWindowHeight =
        900 + static_cast<uint32_t>(kButtonBarHeight) + static_cast<uint32_t>(kGraphAreaHeight);

    // 盤(木目)が画面短辺に対して占める割合。残りは外周の余白
    constexpr float kBoardExtentRatio = 0.88f;
    // 盤の中で格子線が占める範囲の割合。残りは木目の縁
    constexpr float kGridExtentRatio = 0.90f;

    // 木目盤の色
    constexpr float kBoardColorR = 0.80f;
    constexpr float kBoardColorG = 0.62f;
    constexpr float kBoardColorB = 0.36f;

    // 格子線・星の色(濃い木の色)
    constexpr float kLineColorR = 0.15f;
    constexpr float kLineColorG = 0.09f;
    constexpr float kLineColorB = 0.05f;

    // 背景クリア色
    constexpr float kClearColorR = 0.12f;
    constexpr float kClearColorG = 0.12f;
    constexpr float kClearColorB = 0.14f;

    // 黒石・白石の色(DrawCircleで直接描画する。勝率バーの黒/白領域にも同じ色を使い回す)
    constexpr float kBlackStoneR = 0.05f, kBlackStoneG = 0.05f, kBlackStoneB = 0.05f;
    constexpr float kWhiteStoneR = 0.95f, kWhiteStoneG = 0.95f, kWhiteStoneB = 0.92f;

    // 勝率バー(黒番から見た勝率で2色に分割する横棒)の見た目
    constexpr float kWinrateBarHeight = 18.0f;
    constexpr float kWinrateBarMargin = 6.0f; // 盤の上端からバーまでの隙間

    // 勝率バーの下に表示する数値テキスト(勝率%・目差)の見た目
    constexpr float kWinrateTextFontSize = 15.0f;
    constexpr float kWinrateTextMargin = 4.0f; // バーからテキストまでの隙間
    constexpr float kWinrateTextColorR = 0.90f, kWinrateTextColorG = 0.90f, kWinrateTextColorB = 0.88f;

    // 損失グラフ(棋譜再生中のみ、盤上のkGraphAreaHeight帯に描画)の見た目
    constexpr float kGraphMarginX = 24.0f; // 帯の左右の余白
    constexpr float kGraphMarginTop = 12.0f;
    constexpr float kGraphMarginBottom = 8.0f;
    constexpr float kGraphLineThickness = 2.5f;
    constexpr float kGraphLineColorR = 0.95f, kGraphLineColorG = 0.75f, kGraphLineColorB = 0.25f;
    constexpr float kGraphReferenceLineThickness = 1.0f;
    constexpr float kGraphReferenceColorR = 0.45f, kGraphReferenceColorG = 0.45f, kGraphReferenceColorB = 0.48f;
    constexpr float kGraphMarkerRadius = 4.5f;
    constexpr float kGraphMarkerColorR = 1.0f, kGraphMarkerColorG = 1.0f, kGraphMarkerColorB = 1.0f;

    // 地合い可視化(Tキーでトグル)の見た目。黒地=青系、白地=赤系のオーバーレイ
    constexpr float kTerritoryBlackR = 0.25f, kTerritoryBlackG = 0.45f, kTerritoryBlackB = 0.95f;
    constexpr float kTerritoryWhiteR = 0.95f, kTerritoryWhiteG = 0.35f, kTerritoryWhiteB = 0.25f;
    // これ未満のownershipの絶対値は「地としてほぼ確定していない」とみなし表示しない
    constexpr float kTerritoryMinMagnitude = 0.15f;
    constexpr float kTerritoryMaxAlpha = 0.55f;

    // 着手ヒント(Hキーでトグル)の最大表示数と、順位ごとの色(金・銀・銅)・不透明度
    constexpr int kMaxHintMarkers = 3;
    constexpr float kHintColors[kMaxHintMarkers][3] = {
        { 1.00f, 0.85f, 0.20f },
        { 0.75f, 0.78f, 0.82f },
        { 0.80f, 0.50f, 0.30f },
    };
    constexpr float kHintAlphas[kMaxHintMarkers] = { 0.85f, 0.70f, 0.55f };

    // 盤下の余白に表示するHUDテキストの見た目
    constexpr float kHudFontSize = 18.0f;
    constexpr float kHudColorR = 0.92f, kHudColorG = 0.92f, kHudColorB = 0.90f;

    // 操作ボタン(パス・投了・地合い表示切替・着手ヒント表示切替・棋譜再生・終了)の見た目
    constexpr float kButtonHeight = 40.0f;
    constexpr float kButtonPaddingX = 14.0f;
    constexpr float kButtonMinWidth = 88.0f;
    constexpr float kButtonSpacing = 10.0f;
    constexpr float kButtonFontSize = 16.0f;
    // 通常時・ホバー時・トグルON時・無効時の背景色
    constexpr float kButtonColorR = 0.30f, kButtonColorG = 0.30f, kButtonColorB = 0.34f;
    constexpr float kButtonHoverColorR = 0.42f, kButtonHoverColorG = 0.42f, kButtonHoverColorB = 0.48f;
    constexpr float kButtonActiveColorR = 0.30f, kButtonActiveColorG = 0.55f, kButtonActiveColorB = 0.35f;
    constexpr float kButtonDisabledColorR = 0.18f, kButtonDisabledColorG = 0.18f, kButtonDisabledColorB = 0.20f;
    // 通常時・無効時の文字色
    constexpr float kButtonTextColorR = 0.95f, kButtonTextColorG = 0.95f, kButtonTextColorB = 0.95f;
    constexpr float kButtonDisabledTextColorR = 0.5f, kButtonDisabledTextColorG = 0.5f, kButtonDisabledTextColorB = 0.5f;

    const wchar_t* kWindowTitle = L"KurenaiGo";

    // 現在のウィンドウサイズから盤のレイアウト(中心・格子の一辺・目の間隔)を求める
    struct BoardLayout
    {
        float CenterX = 0.0f;
        float CenterY = 0.0f;
        float BoardExtent = 0.0f;
        float GridExtent = 0.0f;
        float LineSpacing = 0.0f;
    };

    BoardLayout ComputeBoardLayout(uint32_t windowWidth, uint32_t windowHeight)
    {
        // 盤の描画領域はウィンドウ下端の操作ボタン行(kButtonBarHeight)と上端の損失グラフの帯
        // (kGraphAreaHeight)を除いた範囲とする
        const float boardAreaHeight =
            (std::max)(1.0f, static_cast<float>(windowHeight) - kButtonBarHeight - kGraphAreaHeight);
        const float minDimension = (std::min)(static_cast<float>(windowWidth), boardAreaHeight);

        BoardLayout layout;
        layout.CenterX = static_cast<float>(windowWidth) * 0.5f;
        layout.CenterY = kButtonBarHeight + boardAreaHeight * 0.5f;
        layout.BoardExtent = minDimension * kBoardExtentRatio;
        layout.GridExtent = layout.BoardExtent * kGridExtentRatio;
        layout.LineSpacing = layout.GridExtent / static_cast<float>(kBoardLines - 1);
        return layout;
    }

    // 格子線上のインデックス(0〜kBoardLines-1)からワールド座標へ変換する
    float GridIndexToCoordinate(const BoardLayout& layout, float center, int index)
    {
        const float origin = center - layout.GridExtent * 0.5f;
        return origin + layout.LineSpacing * static_cast<float>(index);
    }

    // ワールド座標(worldX, worldY)に最も近い交点を求め、スナップ範囲内であれば
    // outRow/outColに書き込みtrueを返す。盤外・範囲外ならfalse
    bool TryGetHoveredIntersection(const BoardLayout& layout, float worldX, float worldY, int& outRow, int& outCol)
    {
        const auto nearestIndex = [](const BoardLayout& layoutRef, float center, float coord) -> int
        {
            const float origin = center - layoutRef.GridExtent * 0.5f;
            return static_cast<int>(std::lround((coord - origin) / layoutRef.LineSpacing));
        };

        const int col = nearestIndex(layout, layout.CenterX, worldX);
        const int row = nearestIndex(layout, layout.CenterY, worldY);
        if (col < 0 || col >= kBoardLines || row < 0 || row >= kBoardLines)
        {
            return false;
        }

        const float exactX = GridIndexToCoordinate(layout, layout.CenterX, col);
        const float exactY = GridIndexToCoordinate(layout, layout.CenterY, row);
        const float dx = worldX - exactX;
        const float dy = worldY - exactY;
        const float snapRadius = layout.LineSpacing * 0.4f;
        if (dx * dx + dy * dy > snapRadius * snapRadius)
        {
            return false;
        }

        outRow = row;
        outCol = col;
        return true;
    }

    void DrawBoard(KurenaiEngine2D& renderer, TextureHandle whiteTexture, const BoardLayout& layout)
    {
        // 木目の盤面
        renderer.DrawSprite(
            layout.CenterX, layout.CenterY, layout.BoardExtent, layout.BoardExtent, 0.0f,
            whiteTexture, kBoardColorR, kBoardColorG, kBoardColorB, 1.0f);

        const float lineThickness = (std::max)(1.5f, layout.LineSpacing * 0.035f);
        const float halfGrid = layout.GridExtent * 0.5f;

        // 横線(各行ごとに1本、盤の幅いっぱいに伸ばす)
        for (int row = 0; row < kBoardLines; ++row)
        {
            const float y = GridIndexToCoordinate(layout, layout.CenterY, row);
            renderer.DrawLine(
                layout.CenterX - halfGrid, y, layout.CenterX + halfGrid, y, lineThickness,
                kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }

        // 縦線(各列ごとに1本、盤の高さいっぱいに伸ばす)
        for (int col = 0; col < kBoardLines; ++col)
        {
            const float x = GridIndexToCoordinate(layout, layout.CenterX, col);
            renderer.DrawLine(
                x, layout.CenterY - halfGrid, x, layout.CenterY + halfGrid, lineThickness,
                kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }

        // 星(hoshi)。3x3の標準的な交点に小さな点を描く
        const float hoshiRadius = layout.LineSpacing * 0.11f;
        for (int hoshiRow : kHoshiIndices)
        {
            const float y = GridIndexToCoordinate(layout, layout.CenterY, hoshiRow);
            for (int hoshiCol : kHoshiIndices)
            {
                const float x = GridIndexToCoordinate(layout, layout.CenterX, hoshiCol);
                renderer.DrawCircle(x, y, hoshiRadius, kLineColorR, kLineColorG, kLineColorB, 1.0f);
            }
        }
    }

    void DrawStones(KurenaiEngine2D& renderer, const GoBoard& board, const BoardLayout& layout)
    {
        const float stoneRadius = layout.LineSpacing * 0.46f;
        for (int row = 0; row < kBoardLines; ++row)
        {
            for (int col = 0; col < kBoardLines; ++col)
            {
                const Stone stone = board.At(row, col);
                if (stone == Stone::Empty)
                {
                    continue;
                }

                const float x = GridIndexToCoordinate(layout, layout.CenterX, col);
                const float y = GridIndexToCoordinate(layout, layout.CenterY, row);
                if (stone == Stone::Black)
                {
                    renderer.DrawCircle(x, y, stoneRadius, kBlackStoneR, kBlackStoneG, kBlackStoneB, 1.0f);
                }
                else
                {
                    renderer.DrawCircle(x, y, stoneRadius, kWhiteStoneR, kWhiteStoneG, kWhiteStoneB, 1.0f);
                }
            }
        }
    }

    // kata-analyzeのownership配列(row-major、盤面左上=index0)における(row, col)のインデックスを
    // 求める。KurenaiGoの内部座標はrow=0が盤面下端・col=0が左端(GTPと同じ)なので変換が必要。
    // 実機での検証結果(黒石をA19に置いてindex0付近が反応することを確認)はdocs/KurenaiGo.html参照
    int OwnershipIndex(int row, int col, int boardSize)
    {
        const int rowFromTop = (boardSize - 1) - row;
        return rowFromTop * boardSize + col;
    }

    // kata-analyzeの結果は解析対象色(ColorToMove)から見た値のため、常に黒視点に変換して扱う
    float ToBlackWinrate(const KataGoAnalysisResult& analysis)
    {
        return analysis.ColorToMove == Stone::Black ? analysis.WinrateForColorToMove : 1.0f - analysis.WinrateForColorToMove;
    }

    float ToBlackOwnership(const KataGoAnalysisResult& analysis, int ownershipIndex)
    {
        const float value = analysis.Ownership[ownershipIndex];
        return analysis.ColorToMove == Stone::Black ? value : -value;
    }

    float ToBlackScoreLead(const KataGoAnalysisResult& analysis)
    {
        return analysis.ColorToMove == Stone::Black ? analysis.ScoreLeadForColorToMove : -analysis.ScoreLeadForColorToMove;
    }

    // 盤の上マージンに、黒番から見た勝率で2分割した横棒を描く
    void DrawWinrateBar(KurenaiEngine2D& renderer, TextureHandle whiteTexture, const BoardLayout& layout, float blackWinrate)
    {
        const float clampedWinrate = (std::max)(0.0f, (std::min)(1.0f, blackWinrate));
        const float barWidth = layout.GridExtent;
        const float barLeft = layout.CenterX - barWidth * 0.5f;
        const float barY = layout.CenterY + layout.BoardExtent * 0.5f + kWinrateBarMargin + kWinrateBarHeight * 0.5f;

        const float blackWidth = barWidth * clampedWinrate;
        const float whiteWidth = barWidth - blackWidth;

        if (blackWidth > 0.0f)
        {
            renderer.DrawSprite(
                barLeft + blackWidth * 0.5f, barY, blackWidth, kWinrateBarHeight, 0.0f,
                whiteTexture, kBlackStoneR, kBlackStoneG, kBlackStoneB, 1.0f);
        }
        if (whiteWidth > 0.0f)
        {
            renderer.DrawSprite(
                barLeft + blackWidth + whiteWidth * 0.5f, barY, whiteWidth, kWinrateBarHeight, 0.0f,
                whiteTexture, kWhiteStoneR, kWhiteStoneG, kWhiteStoneB, 1.0f);
        }
    }

    // 勝率バーのすぐ下に、黒視点の勝率(%)と目差を数値で表示する
    void DrawWinrateText(KurenaiEngine2D& renderer, const BoardLayout& layout, float blackWinrate, float blackScoreLead)
    {
        const float textX = layout.CenterX - layout.GridExtent * 0.5f;
        const float textY = layout.CenterY + layout.BoardExtent * 0.5f + kWinrateBarMargin + kWinrateBarHeight +
            kWinrateTextMargin;

        std::wostringstream text;
        text << L"黒 " << std::fixed << std::setprecision(1) << (blackWinrate * 100.0f) << L"%"
             << L"   目差 " << (blackScoreLead >= 0.0f ? L"+" : L"") << std::setprecision(1) << blackScoreLead;

        renderer.DrawText(textX, textY, text.str(), kWinrateTextFontSize,
            kWinrateTextColorR, kWinrateTextColorG, kWinrateTextColorB, 1.0f);
    }

    // 棋譜再生中、その局面の解析がまだキャッシュされていない間に表示する代わりの文言
    void DrawWinratePending(KurenaiEngine2D& renderer, const BoardLayout& layout)
    {
        const float textX = layout.CenterX - layout.GridExtent * 0.5f;
        const float textY = layout.CenterY + layout.BoardExtent * 0.5f + kWinrateBarMargin + kWinrateBarHeight +
            kWinrateTextMargin;
        renderer.DrawText(textX, textY, L"解析中...", kWinrateTextFontSize,
            kWinrateTextColorR, kWinrateTextColorG, kWinrateTextColorB, 1.0f);
    }

    // 石のない交点に、地の所有率(黒視点)に応じた色つきオーバーレイを描く(Tキーでトグル)
    void DrawTerritoryOverlay(KurenaiEngine2D& renderer, const GoBoard& board, const BoardLayout& layout,
        TextureHandle whiteTexture, const KataGoAnalysisResult& analysis)
    {
        if (analysis.Ownership.size() != static_cast<size_t>(kBoardLines) * static_cast<size_t>(kBoardLines))
        {
            return;
        }

        const float overlaySize = layout.LineSpacing * 0.82f;
        for (int row = 0; row < kBoardLines; ++row)
        {
            for (int col = 0; col < kBoardLines; ++col)
            {
                if (board.At(row, col) != Stone::Empty)
                {
                    continue;
                }

                const float blackOwnership = ToBlackOwnership(analysis, OwnershipIndex(row, col, kBoardLines));
                const float magnitude = std::fabs(blackOwnership);
                if (magnitude < kTerritoryMinMagnitude)
                {
                    continue;
                }

                const float x = GridIndexToCoordinate(layout, layout.CenterX, col);
                const float y = GridIndexToCoordinate(layout, layout.CenterY, row);
                const float alpha = (std::min)(magnitude, 1.0f) * kTerritoryMaxAlpha;
                if (blackOwnership > 0.0f)
                {
                    renderer.DrawSprite(x, y, overlaySize, overlaySize, 0.0f, whiteTexture,
                        kTerritoryBlackR, kTerritoryBlackG, kTerritoryBlackB, alpha);
                }
                else
                {
                    renderer.DrawSprite(x, y, overlaySize, overlaySize, 0.0f, whiteTexture,
                        kTerritoryWhiteR, kTerritoryWhiteG, kTerritoryWhiteB, alpha);
                }
            }
        }
    }

    // 候補手の上位(最大kMaxHintMarkers件)に順位つきマーカーを描く(Hキーでトグル)
    void DrawMoveHints(KurenaiEngine2D& renderer, const BoardLayout& layout, TextureHandle whiteTexture,
        const KataGoAnalysisResult& analysis)
    {
        std::vector<AnalysisMoveInfo> sortedMoves = analysis.TopMoves;
        std::sort(sortedMoves.begin(), sortedMoves.end(),
            [](const AnalysisMoveInfo& a, const AnalysisMoveInfo& b) { return a.Order < b.Order; });

        const float markerSize = layout.LineSpacing * 0.6f;
        int shown = 0;
        for (const AnalysisMoveInfo& move : sortedMoves)
        {
            if (shown >= kMaxHintMarkers)
            {
                break;
            }
            if (move.Row < 0 || move.Col < 0)
            {
                continue; // passなど盤上の座標を持たない候補は表示しない
            }

            const float x = GridIndexToCoordinate(layout, layout.CenterX, move.Col);
            const float y = GridIndexToCoordinate(layout, layout.CenterY, move.Row);
            renderer.DrawSprite(x, y, markerSize, markerSize, 0.0f, whiteTexture,
                kHintColors[shown][0], kHintColors[shown][1], kHintColors[shown][2], kHintAlphas[shown]);
            ++shown;
        }
    }

    // 対局の進行状態
    enum class TurnState
    {
        EngineStarting, // KataGo起動中(OpenCL初回チューニング等で数十秒かかることがある)
        HumanToMove,     // 黒(人間)の手番
        AIThinking,      // 白(KataGo)がgenmove応答待ち
        WaitingForScore, // 両者パス後、final_score応答待ち
        GameOver,        // 対局終了。Vキーで直前の対局の棋譜再生(Reviewing)へ移れる
        Reviewing,       // 棋譜再生中。矢印キーで手を進め戻しする
    };

    // 盤下のHUDに表示する手番状態・アゲハマ数のテキストを組み立てる。reviewMoveIndex/
    // reviewTotalMoves/reviewResultはturnState==Reviewingの場合のみ使う
    std::wstring BuildStatusText(TurnState turnState, const GoBoard& board,
        int reviewMoveIndex, int reviewTotalMoves, const std::string& reviewResult)
    {
        std::wstring text;
        switch (turnState)
        {
        case TurnState::EngineStarting:  text = L"KataGo起動中..."; break;
        case TurnState::HumanToMove:     text = L"あなたの番です(黒)"; break;
        case TurnState::AIThinking:      text = L"KataGo思考中..."; break;
        case TurnState::WaitingForScore: text = L"終局判定中..."; break;
        case TurnState::GameOver:        text = L"対局終了(Vキーで棋譜再生)"; break;
        case TurnState::Reviewing:
            text = L"棋譜再生中 (手 " + std::to_wstring(reviewMoveIndex) + L"/" +
                std::to_wstring(reviewTotalMoves) + L")";
            if (reviewMoveIndex == reviewTotalMoves && !reviewResult.empty())
            {
                text += L"  結果: " + Utf8ToWide(reviewResult);
            }
            break;
        }
        text += L"   アゲハマ 黒:" + std::to_wstring(board.CapturesBy(Stone::Black)) +
            L" 白:" + std::to_wstring(board.CapturesBy(Stone::White));
        return text;
    }

    // 盤の下マージンにHUDテキストを描画する
    void DrawHud(KurenaiEngine2D& renderer, const BoardLayout& layout, TurnState turnState, const GoBoard& board,
        int reviewMoveIndex, int reviewTotalMoves, const std::string& reviewResult)
    {
        const float hudX = layout.CenterX - layout.GridExtent * 0.5f;
        const float bottomMarginCenterY = (layout.CenterY - layout.BoardExtent * 0.5f) * 0.5f;
        const float hudY = bottomMarginCenterY - kHudFontSize * 0.5f;
        renderer.DrawText(hudX, hudY,
            BuildStatusText(turnState, board, reviewMoveIndex, reviewTotalMoves, reviewResult),
            kHudFontSize, kHudColorR, kHudColorG, kHudColorB, 1.0f);
    }

    // ボタン1個の識別子。着手以外の操作(パス・投了・地合い表示切替・着手ヒント表示切替・
    // 棋譜再生・終了)にそれぞれ対応する
    enum class ButtonId
    {
        ToggleTerritory,
        ToggleHint,
        Pass,
        Resign,
        StartReview,
        ReviewPrev,
        ReviewNext,
        Quit,
    };

    // 1フレーム分のボタン行を組み立てる際の仕様(ラベル・有効/無効・トグルON状態)
    struct ButtonSpec
    {
        ButtonId Id;
        std::wstring Label;
        bool Enabled = true;
        bool Active = false; // トグル系ボタンがON状態かどうか(背景色に反映)
    };

    // ButtonSpecから求めたヒット領域(中心x, y基準)
    struct ButtonRect
    {
        ButtonId Id = ButtonId::Quit;
        float CenterX = 0.0f;
        float CenterY = 0.0f;
        float Width = 0.0f;
        float Height = 0.0f;
        bool Enabled = true;
        bool Active = false;
    };

    // ラベル文字列のおおよその描画幅を見積もる。KurenaiEngine2Dは実測用のAPIを公開していないため、
    // ASCII(半角)はfontSizeの約0.55倍、それ以外(かな漢字などの全角文字)は約1.0倍として概算する
    float EstimateTextWidth(const std::wstring& text, float fontSize)
    {
        float width = 0.0f;
        for (wchar_t ch : text)
        {
            const bool isHalfWidth = ch < 0x100;
            width += fontSize * (isHalfWidth ? 0.55f : 1.0f);
        }
        return width;
    }

    // ボタン仕様のリストを、盤の左端を起点に左詰めで1行に並べたヒット領域のリストへ変換する
    std::vector<ButtonRect> LayoutButtonRow(const std::vector<ButtonSpec>& specs, const BoardLayout& layout)
    {
        std::vector<ButtonRect> rects;
        rects.reserve(specs.size());

        float cursorX = layout.CenterX - layout.GridExtent * 0.5f;
        const float centerY = kButtonBarHeight * 0.5f;
        for (const ButtonSpec& spec : specs)
        {
            const float width = (std::max)(kButtonMinWidth, EstimateTextWidth(spec.Label, kButtonFontSize) + kButtonPaddingX * 2.0f);

            ButtonRect rect;
            rect.Id = spec.Id;
            rect.Width = width;
            rect.Height = kButtonHeight;
            rect.CenterX = cursorX + width * 0.5f;
            rect.CenterY = centerY;
            rect.Enabled = spec.Enabled;
            rect.Active = spec.Active;
            rects.push_back(rect);

            cursorX += width + kButtonSpacing;
        }
        return rects;
    }

    bool IsPointInButton(const ButtonRect& button, float worldX, float worldY)
    {
        return worldX >= button.CenterX - button.Width * 0.5f && worldX <= button.CenterX + button.Width * 0.5f &&
            worldY >= button.CenterY - button.Height * 0.5f && worldY <= button.CenterY + button.Height * 0.5f;
    }

    // ボタン1個の背景と文字を描画する。ホバー/トグルON/無効状態に応じて背景色を変える
    void DrawButton(KurenaiEngine2D& renderer, TextureHandle whiteTexture, const ButtonRect& button,
        const std::wstring& label, bool isHovered)
    {
        float r = kButtonColorR, g = kButtonColorG, b = kButtonColorB;
        if (!button.Enabled)
        {
            r = kButtonDisabledColorR; g = kButtonDisabledColorG; b = kButtonDisabledColorB;
        }
        else if (button.Active)
        {
            r = kButtonActiveColorR; g = kButtonActiveColorG; b = kButtonActiveColorB;
        }
        else if (isHovered)
        {
            r = kButtonHoverColorR; g = kButtonHoverColorG; b = kButtonHoverColorB;
        }

        renderer.DrawSprite(button.CenterX, button.CenterY, button.Width, button.Height, 0.0f, whiteTexture, r, g, b, 1.0f);

        const float textWidth = EstimateTextWidth(label, kButtonFontSize);
        const float textX = button.CenterX - textWidth * 0.5f;
        const float textY = button.CenterY - kButtonFontSize * 0.5f;
        if (button.Enabled)
        {
            renderer.DrawText(textX, textY, label, kButtonFontSize, kButtonTextColorR, kButtonTextColorG, kButtonTextColorB, 1.0f);
        }
        else
        {
            renderer.DrawText(textX, textY, label, kButtonFontSize,
                kButtonDisabledTextColorR, kButtonDisabledTextColorG, kButtonDisabledTextColorB, 1.0f);
        }
    }

    // 現在時刻から"game_YYYYMMDD_HHMMSS.sgf"形式のファイル名を組み立てる
    std::wstring BuildGameFileName(std::chrono::system_clock::time_point when)
    {
        const std::time_t time = std::chrono::system_clock::to_time_t(when);
        std::tm localTime{};
        localtime_s(&localTime, &time);

        std::wostringstream name;
        name << L"game_" << std::put_time(&localTime, L"%Y%m%d_%H%M%S") << L".sgf";
        return name.str();
    }

    // 対局の記録をSGFへ保存する。棋譜保存は対局結果の表示を妨げない補助機能のため、
    // 失敗しても例外は投げずerror.logに記録するのみとする。戻り値は保存先パス
    // (棋譜再生で読み直すために使う)。失敗時は空のパスを返す
    std::filesystem::path SaveGameRecordSafely(const std::filesystem::path& gamesDir,
        const std::vector<SgfMove>& moves, const std::string& result)
    {
        try
        {
            std::filesystem::create_directories(gamesDir);

            SgfGameRecord record;
            record.BoardSize = kBoardLines;
            record.Komi = kKomi;
            record.Result = result;
            record.Moves = moves;

            const std::filesystem::path path = gamesDir / BuildGameFileName(std::chrono::system_clock::now());
            WriteSgfFile(path, record);
            return path;
        }
        catch (const std::exception& e)
        {
            std::ofstream log("error.log", std::ios::app);
            log << "SGFの保存に失敗しました: " << e.what() << std::endl;
            return {};
        }
    }

    // reviewMoveIndex手目までを空盤面から再生し、再生用盤面を作り直す
    GoBoard ReplayMoves(const std::vector<SgfMove>& moves, int upToIndex, int boardSize)
    {
        GoBoard replayBoard(boardSize);
        for (int i = 0; i < upToIndex; ++i)
        {
            const SgfMove& move = moves[static_cast<size_t>(i)];
            if (move.IsPass)
            {
                replayBoard.Pass();
            }
            else
            {
                replayBoard.TryPlay(move.Row, move.Col, move.Color);
            }
        }
        return replayBoard;
    }

    // 損失グラフ(棋譜再生中のみ、盤上部のkGraphAreaHeight帯に黒視点勝率の推移を描く)。
    // hasCached[i]がtrueの手数のみ値が有効(未解析の区間は線が途切れる)。
    // winrateCache/hasCachedのサイズは総手数+1(0手目〜総手数)
    void DrawLossGraph(KurenaiEngine2D& renderer, uint32_t windowWidth, uint32_t windowHeight,
        const std::vector<float>& winrateCache, const std::vector<bool>& hasCached, int currentIndex)
    {
        const int totalMoves = static_cast<int>(hasCached.size()) - 1;
        if (totalMoves <= 0)
        {
            return;
        }

        const float graphLeft = kGraphMarginX;
        const float graphRight = static_cast<float>(windowWidth) - kGraphMarginX;
        const float graphBottom = static_cast<float>(windowHeight) - kGraphAreaHeight + kGraphMarginBottom;
        const float graphTop = static_cast<float>(windowHeight) - kGraphMarginTop;
        const float graphWidth = (std::max)(1.0f, graphRight - graphLeft);
        const float graphHeight = (std::max)(1.0f, graphTop - graphBottom);

        const auto xForIndex = [&](int index) -> float
        {
            return graphLeft + graphWidth * (static_cast<float>(index) / static_cast<float>(totalMoves));
        };
        const auto yForWinrate = [&](float winrate) -> float
        {
            return graphBottom + graphHeight * (std::max)(0.0f, (std::min)(1.0f, winrate));
        };

        // 50%の目安線
        const float midY = yForWinrate(0.5f);
        renderer.DrawLine(graphLeft, midY, graphRight, midY, kGraphReferenceLineThickness,
            kGraphReferenceColorR, kGraphReferenceColorG, kGraphReferenceColorB, 1.0f);

        // キャッシュ済みの区間のみ線分で結ぶ
        for (int i = 0; i < totalMoves; ++i)
        {
            if (!hasCached[i] || !hasCached[i + 1])
            {
                continue;
            }
            renderer.DrawLine(
                xForIndex(i), yForWinrate(winrateCache[i]),
                xForIndex(i + 1), yForWinrate(winrateCache[i + 1]),
                kGraphLineThickness, kGraphLineColorR, kGraphLineColorG, kGraphLineColorB, 1.0f);
        }

        // 現在の手数の位置にマーカーを描く
        if (currentIndex >= 0 && currentIndex <= totalMoves && hasCached[static_cast<size_t>(currentIndex)])
        {
            renderer.DrawCircle(
                xForIndex(currentIndex), yForWinrate(winrateCache[static_cast<size_t>(currentIndex)]),
                kGraphMarkerRadius, kGraphMarkerColorR, kGraphMarkerColorG, kGraphMarkerColorB, 1.0f);
        }
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // DirectXTexのWICテクスチャ読み込みがCOMに依存しているため必須(docs/KurenaiEngine.html 7章参照)
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 0;
    try
    {
        const std::filesystem::path kataGoDir = ResolveAppDataPath(L"KataGo");
        const std::filesystem::path soundsDir = ResolveAppDataPath(L"Assets/Sounds");
        const std::filesystem::path gamesDir = ResolveAppDataPath(L"Games");

        KurenaiEngine2D renderer(kWindowTitle, kWindowWidth, kWindowHeight, GraphicsAPI::DX11);

        const TextureHandle whiteTexture = renderer.CreateSolidColorTexture(255, 255, 255, 255);
        const SoundHandle stonePlaceSound = renderer.LoadSound((soundsDir / L"stone_place.wav").wstring());
        const SoundHandle gameEndSound = renderer.LoadSound((soundsDir / L"game_end.wav").wstring());

        GoBoard board(kBoardLines);
        KataGoClient kataGo;
        TurnState turnState = TurnState::EngineStarting;

        // 対局中の着手・パスの記録。対局終了時にSGFへ保存する
        std::vector<SgfMove> moveHistory;
        // 直近の対局終了時にSGFを保存したパス。GameOver中にVキーを押すとこれを読み込んで再生する
        std::filesystem::path lastSavedGamePath;

        // 棋譜再生(Reviewing)の状態
        SgfGameRecord reviewRecord;
        int reviewMoveIndex = 0;
        GoBoard reviewBoard(kBoardLines);

        // 棋譜再生中の局面ごとの解析結果キャッシュ(黒視点に変換済み)。要素数は総手数+1
        // (0手目〜総手数)。reviewHasCached[i]がtrueの手数のみ有効な値を持つ
        std::vector<float> reviewWinrateCache;
        std::vector<float> reviewScoreLeadCache;
        std::vector<bool> reviewHasCached;
        // 解析要求中のreviewMoveIndex(-1なら要求なし)。KataGoClientは1件ずつしか処理しない
        // 設計のため、前の解析が終わるまで次のreset/replayは送らない(描画ループを止めないため)
        int reviewAnalysisPendingIndex = -1;

        // 解析(kata-analyze)の最新結果。対局中の勝率表示・地合い可視化・着手ヒントが共通で使う
        KataGoAnalysisResult latestAnalysis;
        bool hasAnalysis = false;

        // HumanToMoveへ遷移すると同時に、その局面の解析(黒=人間視点)を要求する
        const auto enterHumanToMove = [&]()
        {
            turnState = TurnState::HumanToMove;
            hasAnalysis = false;
            kataGo.RequestAnalysis(Stone::Black);
        };

        // reviewMoveIndex手目の局面をKataGoに解析させる(未キャッシュかつ現在解析要求中でない場合のみ)。
        // KataGoの盤面をclear_boardしてから0手目〜reviewMoveIndex手目の直前まで再生し直し、
        // その局面の解析を要求する
        const auto triggerReviewAnalysisIfNeeded = [&]()
        {
            if (reviewAnalysisPendingIndex != -1)
            {
                return;
            }
            if (reviewMoveIndex < static_cast<int>(reviewHasCached.size()) && reviewHasCached[static_cast<size_t>(reviewMoveIndex)])
            {
                return;
            }

            kataGo.ResetBoard();
            for (int i = 0; i < reviewMoveIndex; ++i)
            {
                const SgfMove& move = reviewRecord.Moves[static_cast<size_t>(i)];
                if (move.IsPass)
                {
                    kataGo.PlayPass(move.Color);
                }
                else
                {
                    kataGo.PlayMove(move.Color, move.Row, move.Col);
                }
            }

            const Stone colorToMove = (reviewMoveIndex == 0)
                ? Stone::Black
                : Opponent(reviewRecord.Moves[static_cast<size_t>(reviewMoveIndex - 1)].Color);
            kataGo.RequestAnalysis(colorToMove);
            reviewAnalysisPendingIndex = reviewMoveIndex;
        };

        kataGo.StartAsync(
            kataGoDir / L"katago.exe",
            kataGoDir / L"model.bin.gz",
            kataGoDir / L"gtp.cfg",
            kataGoDir / L"katago_stderr.log",
            kBoardLines);

        bool territoryOverlayEnabled = false;
        bool hintOverlayEnabled = false;

        while (!renderer.ShouldClose())
        {
            renderer.PumpEvents();

            if (renderer.WasKeyPressed(VK_ESCAPE))
            {
                renderer.Close();
            }

            const uint32_t width = renderer.GetWidth();
            const uint32_t height = renderer.GetHeight();
            if (width == 0 || height == 0)
            {
                // 最小化中などはスキップ
                continue;
            }

            const BoardLayout layout = ComputeBoardLayout(width, height);

            // マウス位置をワールド座標(原点は画面左下、Y-up)へ変換する。GetClientMousePosition()は
            // Win32標準のクライアント座標(原点は左上、Y-down)を返すため、Yを反転する
            const bool mouseInWindow = renderer.IsMouseOverWindow();
            float mouseWorldX = 0.0f;
            float mouseWorldY = 0.0f;
            if (mouseInWindow)
            {
                const POINT cursor = renderer.GetClientMousePosition();
                mouseWorldX = static_cast<float>(cursor.x);
                mouseWorldY = static_cast<float>(height) - static_cast<float>(cursor.y);
            }

            int hoverRow = -1;
            int hoverCol = -1;
            const bool isHovering = mouseInWindow &&
                TryGetHoveredIntersection(layout, mouseWorldX, mouseWorldY, hoverRow, hoverCol) &&
                board.At(hoverRow, hoverCol) == Stone::Empty;

            const bool clicked = renderer.WasMouseButtonPressed(MouseButton::Left);
            bool passPressed = renderer.WasKeyPressed('P');
            bool resignPressed = renderer.WasKeyPressed('R');
            bool reviewStartPressed = renderer.WasKeyPressed('V');
            bool reviewPrevPressed = renderer.WasKeyPressed(VK_LEFT);
            bool reviewNextPressed = renderer.WasKeyPressed(VK_RIGHT);

            if (renderer.WasKeyPressed('T'))
            {
                territoryOverlayEnabled = !territoryOverlayEnabled;
            }

            if (renderer.WasKeyPressed('H'))
            {
                hintOverlayEnabled = !hintOverlayEnabled;
            }

            // 着手以外の操作(パス・投了・地合い表示切替・着手ヒント表示切替・棋譜再生・終了)を
            // 行うボタン行。表示内容は対局の進行状態(turnState)に応じて変える
            std::vector<ButtonSpec> buttonSpecs;
            buttonSpecs.push_back({ ButtonId::ToggleTerritory, L"地合い表示", true, territoryOverlayEnabled });
            buttonSpecs.push_back({ ButtonId::ToggleHint, L"着手ヒント", true, hintOverlayEnabled });
            if (turnState == TurnState::HumanToMove)
            {
                buttonSpecs.push_back({ ButtonId::Pass, L"パス", true, false });
                buttonSpecs.push_back({ ButtonId::Resign, L"投了", true, false });
            }
            if (turnState == TurnState::GameOver && !lastSavedGamePath.empty())
            {
                buttonSpecs.push_back({ ButtonId::StartReview, L"棋譜再生", true, false });
            }
            if (turnState == TurnState::Reviewing)
            {
                const bool canGoPrev = reviewMoveIndex > 0;
                const bool canGoNext = reviewMoveIndex < static_cast<int>(reviewRecord.Moves.size());
                buttonSpecs.push_back({ ButtonId::ReviewPrev, L"前の手", canGoPrev, false });
                buttonSpecs.push_back({ ButtonId::ReviewNext, L"次の手", canGoNext, false });
            }
            buttonSpecs.push_back({ ButtonId::Quit, L"終了", true, false });

            const std::vector<ButtonRect> buttonRects = LayoutButtonRow(buttonSpecs, layout);

            if (mouseInWindow && clicked)
            {
                for (const ButtonRect& button : buttonRects)
                {
                    if (!button.Enabled || !IsPointInButton(button, mouseWorldX, mouseWorldY))
                    {
                        continue;
                    }
                    switch (button.Id)
                    {
                    case ButtonId::ToggleTerritory: territoryOverlayEnabled = !territoryOverlayEnabled; break;
                    case ButtonId::ToggleHint:      hintOverlayEnabled = !hintOverlayEnabled; break;
                    case ButtonId::Pass:            passPressed = true; break;
                    case ButtonId::Resign:          resignPressed = true; break;
                    case ButtonId::StartReview:     reviewStartPressed = true; break;
                    case ButtonId::ReviewPrev:      reviewPrevPressed = true; break;
                    case ButtonId::ReviewNext:      reviewNextPressed = true; break;
                    case ButtonId::Quit:            renderer.Close(); break;
                    }
                    break; // ボタンは重ならない配置のため、1個ヒットしたら以降は調べない
                }
            }

            // 解析結果のポーリング。要求元(通常の対局中/棋譜再生中)に応じて振り分ける
            KataGoAnalysisResult polledAnalysis;
            if (kataGo.TryGetAnalysisResult(polledAnalysis))
            {
                if (reviewAnalysisPendingIndex != -1)
                {
                    if (!polledAnalysis.Failed &&
                        reviewAnalysisPendingIndex < static_cast<int>(reviewHasCached.size()))
                    {
                        const size_t index = static_cast<size_t>(reviewAnalysisPendingIndex);
                        reviewWinrateCache[index] = ToBlackWinrate(polledAnalysis);
                        reviewScoreLeadCache[index] = ToBlackScoreLead(polledAnalysis);
                        reviewHasCached[index] = true;
                    }
                    reviewAnalysisPendingIndex = -1;
                }
                else
                {
                    latestAnalysis = polledAnalysis;
                    hasAnalysis = !latestAnalysis.Failed;
                }
            }

            switch (turnState)
            {
            case TurnState::EngineStarting:
                if (kataGo.IsStartupComplete())
                {
                    if (kataGo.StartupFailed())
                    {
                        throw std::runtime_error("KataGoの起動に失敗しました: " + kataGo.LastError());
                    }
                    enterHumanToMove();
                }
                break;

            case TurnState::HumanToMove:
                if (resignPressed)
                {
                    renderer.PlaySound(gameEndSound);
                    lastSavedGamePath = SaveGameRecordSafely(gamesDir, moveHistory, "W+R");
                    ShowMessageBoxUtf8("投了しました。KataGoの勝ちです。", "KurenaiGo", MB_OK | MB_ICONINFORMATION);
                    turnState = TurnState::GameOver;
                }
                else if (passPressed)
                {
                    board.Pass();
                    kataGo.PlayPass(Stone::Black);
                    moveHistory.push_back({ Stone::Black, true, -1, -1 });
                    if (board.ConsecutivePasses() >= 2)
                    {
                        kataGo.RequestFinalScore();
                        turnState = TurnState::WaitingForScore;
                    }
                    else
                    {
                        kataGo.RequestGenMove(Stone::White);
                        turnState = TurnState::AIThinking;
                    }
                }
                else if (clicked && isHovering)
                {
                    if (board.TryPlay(hoverRow, hoverCol, Stone::Black))
                    {
                        renderer.PlaySound(stonePlaceSound);
                        kataGo.PlayMove(Stone::Black, hoverRow, hoverCol);
                        moveHistory.push_back({ Stone::Black, false, hoverRow, hoverCol });
                        kataGo.RequestGenMove(Stone::White);
                        turnState = TurnState::AIThinking;
                    }
                    // 非合法手(コウ等)の場合は無視して手番を継続する
                }
                break;

            case TurnState::AIThinking:
            {
                KataGoMoveResult result;
                if (kataGo.TryGetGenMoveResult(result))
                {
                    if (result.Failed)
                    {
                        const std::string message = "KataGoとの通信でエラーが発生しました:\n" + kataGo.LastError();
                        ShowMessageBoxUtf8(message, "KurenaiGo - KataGoエラー", MB_OK | MB_ICONERROR);
                        turnState = TurnState::GameOver;
                    }
                    else if (result.IsResign)
                    {
                        renderer.PlaySound(gameEndSound);
                        lastSavedGamePath = SaveGameRecordSafely(gamesDir, moveHistory, "B+R");
                        ShowMessageBoxUtf8("KataGoが投了しました。あなたの勝ちです。", "KurenaiGo", MB_OK | MB_ICONINFORMATION);
                        turnState = TurnState::GameOver;
                    }
                    else if (result.IsPass)
                    {
                        board.Pass();
                        moveHistory.push_back({ Stone::White, true, -1, -1 });
                        if (board.ConsecutivePasses() >= 2)
                        {
                            kataGo.RequestFinalScore();
                            turnState = TurnState::WaitingForScore;
                        }
                        else
                        {
                            enterHumanToMove();
                        }
                    }
                    else if (!board.TryPlay(result.Row, result.Col, Stone::White))
                    {
                        // KataGoは常に合法手を返す前提のため、ここに来るのは想定外の異常事態
                        ShowMessageBoxUtf8("KataGoの着手を反映できませんでした(想定外の座標)。",
                            "KurenaiGo - エラー", MB_OK | MB_ICONERROR);
                        turnState = TurnState::GameOver;
                    }
                    else
                    {
                        renderer.PlaySound(stonePlaceSound);
                        moveHistory.push_back({ Stone::White, false, result.Row, result.Col });
                        enterHumanToMove();
                    }
                }
                break;
            }

            case TurnState::WaitingForScore:
            {
                std::string score;
                if (kataGo.TryGetFinalScore(score))
                {
                    renderer.PlaySound(gameEndSound);
                    lastSavedGamePath = SaveGameRecordSafely(gamesDir, moveHistory, score);
                    const std::string message = "対局終了\n結果: " + score;
                    ShowMessageBoxUtf8(message, "KurenaiGo - 対局終了", MB_OK | MB_ICONINFORMATION);
                    turnState = TurnState::GameOver;
                }
                break;
            }

            case TurnState::GameOver:
                if (reviewStartPressed && !lastSavedGamePath.empty())
                {
                    try
                    {
                        reviewRecord = ReadSgfFile(lastSavedGamePath);
                        reviewMoveIndex = 0;
                        reviewBoard = GoBoard(kBoardLines);
                        hasAnalysis = false; // 直前の対局の解析結果を棋譜再生画面に持ち越さない
                        reviewWinrateCache.assign(reviewRecord.Moves.size() + 1, 0.5f);
                        reviewScoreLeadCache.assign(reviewRecord.Moves.size() + 1, 0.0f);
                        reviewHasCached.assign(reviewRecord.Moves.size() + 1, false);
                        reviewAnalysisPendingIndex = -1;
                        turnState = TurnState::Reviewing;
                        triggerReviewAnalysisIfNeeded(); // 0手目の解析をすぐに開始する
                    }
                    catch (const std::exception& e)
                    {
                        std::ofstream log("error.log", std::ios::app);
                        log << "棋譜の読み込みに失敗しました: " << e.what() << std::endl;
                    }
                }
                break;

            case TurnState::Reviewing:
            {
                const int totalMoves = static_cast<int>(reviewRecord.Moves.size());
                bool indexChanged = false;
                if (reviewNextPressed && reviewMoveIndex < totalMoves)
                {
                    ++reviewMoveIndex;
                    indexChanged = true;
                }
                else if (reviewPrevPressed && reviewMoveIndex > 0)
                {
                    --reviewMoveIndex;
                    indexChanged = true;
                }
                if (indexChanged)
                {
                    reviewBoard = ReplayMoves(reviewRecord.Moves, reviewMoveIndex, kBoardLines);
                }
                triggerReviewAnalysisIfNeeded();
                break;
            }
            }

            const GoBoard& displayBoard = (turnState == TurnState::Reviewing) ? reviewBoard : board;

            // 表示する勝率・目差の決定(通常の対局中は最新解析、棋譜再生中はその手数のキャッシュ)
            bool haveWinrateToShow = false;
            float displayBlackWinrate = 0.5f;
            float displayBlackScoreLead = 0.0f;
            if (turnState == TurnState::Reviewing)
            {
                if (reviewMoveIndex < static_cast<int>(reviewHasCached.size()) &&
                    reviewHasCached[static_cast<size_t>(reviewMoveIndex)])
                {
                    haveWinrateToShow = true;
                    displayBlackWinrate = reviewWinrateCache[static_cast<size_t>(reviewMoveIndex)];
                    displayBlackScoreLead = reviewScoreLeadCache[static_cast<size_t>(reviewMoveIndex)];
                }
            }
            else if (hasAnalysis)
            {
                haveWinrateToShow = true;
                displayBlackWinrate = ToBlackWinrate(latestAnalysis);
                displayBlackScoreLead = ToBlackScoreLead(latestAnalysis);
            }

            renderer.BeginFrame(kClearColorR, kClearColorG, kClearColorB);
            DrawBoard(renderer, whiteTexture, layout);
            if (hasAnalysis && territoryOverlayEnabled)
            {
                DrawTerritoryOverlay(renderer, displayBoard, layout, whiteTexture, latestAnalysis);
            }
            DrawStones(renderer, displayBoard, layout);
            if (hasAnalysis && hintOverlayEnabled && turnState == TurnState::HumanToMove)
            {
                DrawMoveHints(renderer, layout, whiteTexture, latestAnalysis);
            }
            if (haveWinrateToShow)
            {
                DrawWinrateBar(renderer, whiteTexture, layout, displayBlackWinrate);
                DrawWinrateText(renderer, layout, displayBlackWinrate, displayBlackScoreLead);
            }
            else if (turnState == TurnState::Reviewing)
            {
                DrawWinratePending(renderer, layout);
            }
            if (turnState == TurnState::Reviewing)
            {
                DrawLossGraph(renderer, width, height, reviewWinrateCache, reviewHasCached, reviewMoveIndex);
            }
            DrawHud(renderer, layout, turnState, displayBoard,
                reviewMoveIndex, static_cast<int>(reviewRecord.Moves.size()), reviewRecord.Result);

            // 着手以外の操作ボタンを盤下のボタン行に描画する
            for (size_t i = 0; i < buttonRects.size(); ++i)
            {
                const bool isButtonHovered = mouseInWindow && IsPointInButton(buttonRects[i], mouseWorldX, mouseWorldY);
                DrawButton(renderer, whiteTexture, buttonRects[i], buttonSpecs[i].Label, isButtonHovered);
            }

            // 手番中、カーソルが空点の交点上にあれば半透明のプレビューを表示する
            if (turnState == TurnState::HumanToMove && isHovering)
            {
                const float x = GridIndexToCoordinate(layout, layout.CenterX, hoverCol);
                const float y = GridIndexToCoordinate(layout, layout.CenterY, hoverRow);
                const float previewRadius = layout.LineSpacing * 0.46f;
                renderer.DrawCircle(x, y, previewRadius, 0.2f, 0.2f, 0.2f, 0.35f);
            }

            renderer.EndFrame(true);
        }
    }
    catch (const std::exception& e)
    {
        std::ofstream log("error.log", std::ios::app);
        log << e.what() << std::endl;
        ShowMessageBoxUtf8(e.what(), "KurenaiGo - エラー", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }

    return exitCode;
}
