#pragma once

#include <QObject>
#include <QImage>
#include <QTimer>
#include <atomic>
#include <thread>

class FFmpegDecoder;
class VideoRenderer;

struct AVFrame;

class FFmpegPlayer : public QObject
{
    Q_OBJECT

public:
    explicit FFmpegPlayer(QObject* parent = nullptr);
    ~FFmpegPlayer();

    void SetVideoHwnd(HWND hwnd);

    bool OpenFile(const QString& path);
    void Play();
    void Pause();
    void Stop();
    void Close();

    qint64 GetPosition() const;
    qint64 GetDuration() const;
    bool   IsPlaying() const;
    bool   IsPaused() const;

signals:
    void SigLoaded(qint64 duration_ms);
    void SigFrameReady(const QImage& image);
    void SigFinished();
    void SigError(const QString& msg);
    void SigPlayState(const QString& state);

private:
    void DecodeLoop();

    FFmpegDecoder* decoder_{ nullptr };
    VideoRenderer* renderer_{ nullptr };

    HWND hwnd_{ nullptr };

    std::thread         decode_thread_;
    std::atomic<bool>   playing_{ false };
    std::atomic<bool>   paused_{ false };
    std::atomic<qint64> current_pts_ms_{ 0 };
    qint64              duration_ms_{ 0 };
};