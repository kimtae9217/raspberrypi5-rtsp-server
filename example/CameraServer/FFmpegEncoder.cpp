#include "FFmpegEncoder.h"
#include <iostream>
#include <thread>
#include "DataCapture.h"
/// C언어로 FFmpeg Library를 사용
extern "C"
{
    #include "libavformat/avformat.h"
    #include "libavcodec/avcodec.h"
    #include "libavutil/avutil.h"
    #include "libavutil/opt.h"
    #include "libswscale/swscale.h"
}

/**
 * @brief FFmpegEncoder 생성자
 * @details AVCodec을 설정하고 ffmpeg library를 이용해 라즈베리파이 카메라모듈과 연결합니다.
 */
FFmpegEncoder::FFmpegEncoder(const std::string& filename, int width, int height, double fps)
    : width(width), height(height) {
    initFFmpeg(filename, fps);
}

/**
 * @brief FFmpegEncoder 소멸자
 * @details FFmpeg에 연결된 카메라모듈을 반환합니다.
 */
FFmpegEncoder::~FFmpegEncoder() {
    releaseFFmpeg();
}

/**
 * @brief FFmpeg library로 카메라를 설정합니다.
 * @details AVCodec을 설정하고 ffmpeg library를 이용해 라즈베리파이 카메라모듈과 연결합니다.
 */
void FFmpegEncoder::initFFmpeg(const std::string& filename, double fps) {
    const struct AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!codec) {
        throw std::runtime_error("Codec not found");
    }

    codec_ctx = avcodec_alloc_context3(codec);
    if (!codec_ctx) {
        throw std::runtime_error("Could not allocate codec context");
    }

    codec_ctx->bit_rate = 4000000; // 4Mbps
    codec_ctx->width = width;
    codec_ctx->height = height;
    codec_ctx->time_base = (AVRational){1, static_cast<int>(fps)};
    codec_ctx->gop_size = 30;
    codec_ctx->max_b_frames = 1;
    codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    // Enable multi-threading
    codec_ctx->thread_count = std::thread::hardware_concurrency();
    codec_ctx->thread_type = FF_THREAD_FRAME;

    // Set encoder options
    av_opt_set(codec_ctx->priv_data, "preset", "ultrafast", 0);
    av_opt_set(codec_ctx->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(codec_ctx, codec, NULL) < 0) {
        throw std::runtime_error("Could not open codec");
    }

    fmt_ctx = avformat_alloc_context();
    if (!fmt_ctx) {
        throw std::runtime_error("Could not allocate format context");
    }

    const AVOutputFormat *output_fmt = av_guess_format(NULL, filename.c_str(), NULL);
    fmt_ctx->oformat = output_fmt;

    if (avio_open(&fmt_ctx->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0) {
        throw std::runtime_error("Could not open output file");
    }

    stream = avformat_new_stream(fmt_ctx, NULL);
    if (!stream) {
        throw std::runtime_error("Could not allocate stream");
    }

    stream->time_base = codec_ctx->time_base;
    stream->codecpar->codec_id = AV_CODEC_ID_H264; // H.264 코덱 설정
    stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO; // 미디어 타입 비디오로 설정(오디오도 있음)
    stream->codecpar->width = codec_ctx->width;
    stream->codecpar->height = codec_ctx->height;
    stream->codecpar->format = AV_PIX_FMT_YUV420P;

    if (avformat_write_header(fmt_ctx, NULL) < 0) {
        throw std::runtime_error("Error writing header");
    }

    packet = av_packet_alloc();
    if (!packet) {
        throw std::runtime_error("Could not allocate AVPacket");
    }

    frame = av_frame_alloc();
    if (!frame) {
        throw std::runtime_error("Could not allocate AVFrame");
    }

    frame->format = codec_ctx->pix_fmt;
    frame->width = codec_ctx->width;
    frame->height = codec_ctx->height;

    if (av_frame_get_buffer(frame, 32) < 0) {
        throw std::runtime_error("Could not allocate frame buffer");
    }
}

/**
 * @brief 카메라 모듈에서 프레임을 읽어옵니다.
 * @param inputFrame YUVformat형태의 프레임
 * @param fps 카메라 모듈에 설정된 frame per seconds
 * @details 카메라 모듈에서 YUVformat의 프레임을 읽어와 DataCapture로 프레임을 처리합니다.
 */
void FFmpegEncoder::encode(const cv::Mat& inputFrame, double fps) {

    int y_size = codec_ctx->width * codec_ctx->height;      // Y 채널 크기
    int uv_size = y_size / 4;                              // U 및 V 채널 크기

    frame->data[0] = inputFrame.data;                      // Y 채널
    frame->data[1] = inputFrame.data + y_size;            // U 채널
    frame->data[2] = inputFrame.data + y_size + uv_size;  // V 채널


    // PTS 설정 (프레임 인덱스를 사용하여 PTS 설정)
    // PTS는 각 프레임이 언제 표시되어야하는지를 나타내는 시간정보. 여기서 frame_index는 인코딩중인 프레임 순서 의미
    // PTS 계산 (이전 값보다 항상 증가 보장)
    static int64_t lastPTS = 0;
    int64_t currentPTS = static_cast<int64_t>(frame_index * codec_ctx->time_base.den / fps);

    // `PTS`가 이전 값보다 크도록 보장
    if (currentPTS <= lastPTS) {
        currentPTS = lastPTS + 1;
    }
    frame->pts = currentPTS;
    lastPTS = currentPTS; // PTS 업데이트

    frame_index++;


    // 프레임을 인코더로 보냄
    int ret = avcodec_send_frame(codec_ctx, frame);
    if ( ret < 0) {
        throw std::runtime_error("Error sending frame for encoding");
    }

    // 인코딩된 패킷을 가져오기
    DataCaptureFrame newframe;
    while ( ret >=  0) {
        ret = avcodec_receive_packet(codec_ctx, packet);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
        if (ret < 0) {
            throw std::runtime_error("Error during encoding");
        }

        //인코딩된 패킷을 출력파일에 기록한 후 패킷을 해제함(unref)
        // av_interleaved_write_fraem 은 인코딩된 패킷을 출력파일에 기록하는 함수
        // 인터리빙 방식 : 오디오 및 비디오 스트림 교차저장하는 방식(영상재생시 동시에 재생되게함)
        // fmt_ctx(포맷컨텍스트) 출력파일과 관련된 정보 포함하고있으며, 파일에 패킷을 기록할 대상이 됨
        // pkt : 인코딩된 비디오또는 오디오 데이터 포함하고있음. 이 데이터를 출력파일에 기록하는것
        // av_interleaved_write_frame(fmt_ctx, packet);      // 패킷을 파일에 기록
        // 패킷에 할당된 메모리를 해제. 인코딩된 데이터를 파일에 기록한 후 메모리 해제해야함
        if (packet->size > 0 && packet->data) {
            newframe.dataPtr = (unsigned char*)packet->data;
            newframe.size = packet->size;
            // PTS를 사용하여 초 단위로 변환
            double seconds = packet->pts * av_q2d(stream->time_base);
            std::cout << "Time in seconds: " << seconds << std::endl;
            // 실시간 timestamp 추가
            auto now = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch());
            std::cout << "timestamp :" << duration.count()*100 << std::endl;
            newframe.timestamp = duration.count()*100; // 밀리초 단위 timestamp

            DataCapture::getInstance().pushFrame(newframe);
        }
        av_packet_unref(packet);
    }

}

/**
 * @brief FFmpeg library의 카메라 메모리를 해제합니다.
 * @details FFmpeg에 연결된 카메라모듈을 반환합니다.
 */
void FFmpegEncoder::releaseFFmpeg() {
    if (frame) {
        av_frame_free(&frame);
    }
    if (packet) {
        av_packet_free(&packet);
    }
    if (codec_ctx) {
        avcodec_free_context(&codec_ctx);
    }
    if (fmt_ctx) {
        avio_close(fmt_ctx->pb);
        avformat_free_context(fmt_ctx);
    }
}
