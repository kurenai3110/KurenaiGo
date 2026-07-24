// KurenaiGo - 囲碁アプリ。
// KurenaiEngine2D(公開API)で19路盤・石を描画し、KataGo(https://github.com/lightvector/katago)を
// GTP(Go Text Protocol)で動かして人間(黒)とKataGo(白)の対局を行う。
// 座標系はワールド=ピクセル座標(原点は画面左下、Y-up)。
//
// 操作: 交点クリックで着手 / Pキーでパス / Rキーで投了 / Escで終了

#include <Windows.h>

#include <objbase.h>

#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

#include "GoBoard.h"
#include "KataGoClient.h"
#include "KurenaiEngine2D.h"
#include "KurenaiTypes.h"
#include "PathUtil.h"

using namespace Kurenai;
using namespace KurenaiGo;

namespace
{
    // 盤の目の数(19路盤)
    constexpr int kBoardLines = 19;

    // 19路盤の星(hoshi)の位置。0-indexed(0〜18)で、標準的な4線交点
    constexpr std::array<int, 3> kHoshiIndices = { 3, 9, 15 };

    constexpr uint32_t kWindowWidth = 900;
    constexpr uint32_t kWindowHeight = 900;

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

    // KurenaiEngineが内部で登録しているウィンドウクラス名(Core/Window.cpp参照)。
    // KurenaiEngineBaseはHWNDを公開していないため、マウス座標変換(ScreenToClient)用に
    // 自ウィンドウのHWNDを取得する手段としてFindWindowWで探す
    const wchar_t* kEngineWindowClassName = L"KurenaiEngineWindowClass";
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
        const float minDimension = static_cast<float>((std::min)(windowWidth, windowHeight));

        BoardLayout layout;
        layout.CenterX = static_cast<float>(windowWidth) * 0.5f;
        layout.CenterY = static_cast<float>(windowHeight) * 0.5f;
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

        // 横線(各行ごとに1本、盤の幅いっぱいに伸ばす)
        for (int row = 0; row < kBoardLines; ++row)
        {
            const float y = GridIndexToCoordinate(layout, layout.CenterY, row);
            renderer.DrawSprite(
                layout.CenterX, y, layout.GridExtent, lineThickness, 0.0f,
                whiteTexture, kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }

        // 縦線(各列ごとに1本、盤の高さいっぱいに伸ばす)
        for (int col = 0; col < kBoardLines; ++col)
        {
            const float x = GridIndexToCoordinate(layout, layout.CenterX, col);
            renderer.DrawSprite(
                x, layout.CenterY, lineThickness, layout.GridExtent, 0.0f,
                whiteTexture, kLineColorR, kLineColorG, kLineColorB, 1.0f);
        }

        // 星(hoshi)。3x3の標準的な交点に小さな点を描く
        const float hoshiSize = layout.LineSpacing * 0.22f;
        for (int hoshiRow : kHoshiIndices)
        {
            const float y = GridIndexToCoordinate(layout, layout.CenterY, hoshiRow);
            for (int hoshiCol : kHoshiIndices)
            {
                const float x = GridIndexToCoordinate(layout, layout.CenterX, hoshiCol);
                renderer.DrawSprite(
                    x, y, hoshiSize, hoshiSize, 0.0f,
                    whiteTexture, kLineColorR, kLineColorG, kLineColorB, 1.0f);
            }
        }
    }

    void DrawStones(KurenaiEngine2D& renderer, const GoBoard& board, const BoardLayout& layout,
        TextureHandle blackStoneTexture, TextureHandle whiteStoneTexture)
    {
        const float stoneSize = layout.LineSpacing * 0.92f;
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
                const TextureHandle texture = (stone == Stone::Black) ? blackStoneTexture : whiteStoneTexture;
                renderer.DrawSprite(x, y, stoneSize, stoneSize, 0.0f, texture, 1.0f, 1.0f, 1.0f, 1.0f);
            }
        }
    }

    // 対局の進行状態
    enum class TurnState
    {
        EngineStarting, // KataGo起動中(OpenCL初回チューニング等で数十秒かかることがある)
        HumanToMove,     // 黒(人間)の手番
        AIThinking,      // 白(KataGo)がgenmove応答待ち
        WaitingForScore, // 両者パス後、final_score応答待ち
        GameOver,
    };
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    // DirectXTexのWICテクスチャ読み込みがCOMに依存しているため必須(docs/KurenaiEngine.html 7章参照)
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode = 0;
    try
    {
        const std::filesystem::path assetsDir = ResolveAppDataPath(L"Assets");
        const std::filesystem::path kataGoDir = ResolveAppDataPath(L"KataGo");

        KurenaiEngine2D renderer(kWindowTitle, kWindowWidth, kWindowHeight, GraphicsAPI::DX11);

        const TextureHandle whiteTexture = renderer.CreateSolidColorTexture(255, 255, 255, 255);
        const TextureHandle blackStoneTexture = renderer.LoadTexture((assetsDir / L"stone_black.png").wstring(), true);
        const TextureHandle whiteStoneTexture = renderer.LoadTexture((assetsDir / L"stone_white.png").wstring(), true);

        // KurenaiEngineBaseはHWNDを公開していないため、固定のウィンドウクラス名+タイトルで
        // 自ウィンドウを検索する(コンストラクタ完了時点でウィンドウは既に生成済み)
        const HWND windowHandle = FindWindowW(kEngineWindowClassName, kWindowTitle);

        GoBoard board(kBoardLines);
        KataGoClient kataGo;
        TurnState turnState = TurnState::EngineStarting;

        kataGo.StartAsync(
            kataGoDir / L"katago.exe",
            kataGoDir / L"model.bin.gz",
            kataGoDir / L"gtp.cfg",
            kataGoDir / L"katago_stderr.log",
            kBoardLines);

        bool prevLButtonDown = false;
        bool prevPassKeyDown = false;
        bool prevResignKeyDown = false;

        while (!renderer.ShouldClose())
        {
            renderer.PumpEvents();

            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
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

            // マウス位置をワールド座標(原点は画面左下、Y-up)へ変換する
            int hoverRow = -1;
            int hoverCol = -1;
            bool isHovering = false;
            if (windowHandle)
            {
                POINT cursor{};
                GetCursorPos(&cursor);
                ScreenToClient(windowHandle, &cursor);
                const float worldX = static_cast<float>(cursor.x);
                const float worldY = static_cast<float>(height) - static_cast<float>(cursor.y);
                isHovering = TryGetHoveredIntersection(layout, worldX, worldY, hoverRow, hoverCol) &&
                    board.At(hoverRow, hoverCol) == Stone::Empty;
            }

            const bool lButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            const bool clicked = lButtonDown && !prevLButtonDown;
            prevLButtonDown = lButtonDown;

            const bool passKeyDown = (GetAsyncKeyState('P') & 0x8000) != 0;
            const bool passPressed = passKeyDown && !prevPassKeyDown;
            prevPassKeyDown = passKeyDown;

            const bool resignKeyDown = (GetAsyncKeyState('R') & 0x8000) != 0;
            const bool resignPressed = resignKeyDown && !prevResignKeyDown;
            prevResignKeyDown = resignKeyDown;

            switch (turnState)
            {
            case TurnState::EngineStarting:
                if (kataGo.IsStartupComplete())
                {
                    if (kataGo.StartupFailed())
                    {
                        throw std::runtime_error("KataGoの起動に失敗しました: " + kataGo.LastError());
                    }
                    turnState = TurnState::HumanToMove;
                }
                break;

            case TurnState::HumanToMove:
                if (resignPressed)
                {
                    MessageBoxA(nullptr, "投了しました。KataGoの勝ちです。", "KurenaiGo", MB_OK | MB_ICONINFORMATION);
                    turnState = TurnState::GameOver;
                }
                else if (passPressed)
                {
                    board.Pass();
                    kataGo.PlayPass(Stone::Black);
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
                        kataGo.PlayMove(Stone::Black, hoverRow, hoverCol);
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
                        MessageBoxA(nullptr, message.c_str(), "KurenaiGo - KataGoエラー", MB_OK | MB_ICONERROR);
                        turnState = TurnState::GameOver;
                    }
                    else if (result.IsResign)
                    {
                        MessageBoxA(nullptr, "KataGoが投了しました。あなたの勝ちです。", "KurenaiGo", MB_OK | MB_ICONINFORMATION);
                        turnState = TurnState::GameOver;
                    }
                    else if (result.IsPass)
                    {
                        board.Pass();
                        if (board.ConsecutivePasses() >= 2)
                        {
                            kataGo.RequestFinalScore();
                            turnState = TurnState::WaitingForScore;
                        }
                        else
                        {
                            turnState = TurnState::HumanToMove;
                        }
                    }
                    else if (!board.TryPlay(result.Row, result.Col, Stone::White))
                    {
                        // KataGoは常に合法手を返す前提のため、ここに来るのは想定外の異常事態
                        MessageBoxA(nullptr, "KataGoの着手を反映できませんでした(想定外の座標)。",
                            "KurenaiGo - エラー", MB_OK | MB_ICONERROR);
                        turnState = TurnState::GameOver;
                    }
                    else
                    {
                        turnState = TurnState::HumanToMove;
                    }
                }
                break;
            }

            case TurnState::WaitingForScore:
            {
                std::string score;
                if (kataGo.TryGetFinalScore(score))
                {
                    const std::string message = "対局終了\n結果: " + score;
                    MessageBoxA(nullptr, message.c_str(), "KurenaiGo - 対局終了", MB_OK | MB_ICONINFORMATION);
                    turnState = TurnState::GameOver;
                }
                break;
            }

            case TurnState::GameOver:
                break;
            }

            renderer.BeginFrame(kClearColorR, kClearColorG, kClearColorB);
            DrawBoard(renderer, whiteTexture, layout);
            DrawStones(renderer, board, layout, blackStoneTexture, whiteStoneTexture);

            // 手番中、カーソルが空点の交点上にあれば半透明のプレビューを表示する
            if (turnState == TurnState::HumanToMove && isHovering)
            {
                const float x = GridIndexToCoordinate(layout, layout.CenterX, hoverCol);
                const float y = GridIndexToCoordinate(layout, layout.CenterY, hoverRow);
                const float previewSize = layout.LineSpacing * 0.92f;
                renderer.DrawSprite(x, y, previewSize, previewSize, 0.0f, whiteStoneTexture, 0.2f, 0.2f, 0.2f, 0.35f);
            }

            renderer.EndFrame(true);
        }
    }
    catch (const std::exception& e)
    {
        std::ofstream log("error.log", std::ios::app);
        log << e.what() << std::endl;
        MessageBoxA(nullptr, e.what(), "KurenaiGo - エラー", MB_OK | MB_ICONERROR);
        exitCode = 1;
    }

    if (SUCCEEDED(comResult))
    {
        CoUninitialize();
    }

    return exitCode;
}
